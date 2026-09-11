#!/usr/bin/env python3
"""De bout en bout, sur **notre** serveur : un villageois et un pupitre, un
bibliothécaire, un livre enchanté acheté contre des émeraudes, un niveau gagné.

La sonde est un client protocole 763 sans écran (celui de
`check_interaction_e2e.py`) ; tout est jugé sur le fil, comme le verrait un vrai
client :

  1. `--mobs=villager` pose un villageois sans métier ; la sonde pose un pupitre
     à trois blocs (Use Item On, en créatif) ;
  2. la métadonnée 18 (VillagerData) du villageois passe à la profession 9
     (bibliothécaire) — l'indice et les octets ont été relevés sur le vrai
     serveur ;
  3. un Interact ouvre l'écran : Open Screen (menu 18, `merchant`), puis Merchant
     Offers (0x2A), décodé champ par champ avec le décodeur écrit contre la
     capture du vrai serveur (`measure_villagers.decode_offers`) ;
  4. dès qu'une offre de livre enchanté paraît : Select Trade (0x26) sur elle, un
     clic sur la case résultat ; le livre arrive dans le curseur avec ses
     `StoredEnchantments`, les émeraudes du prix quittent l'inventaire, un orbe
     apparaît ;
  5. tant qu'il n'y a pas de livre, et encore après l'achat, la sonde échange
     tout ce qui est ouvert (majuscule-clic sur le résultat : des échanges en
     série, comme vanilla) ; l'écran fermé, la métadonnée 18 annonce le niveau
     suivant (le vrai serveur : 40 ticks après la fermeture).

Le tirage des offres est aléatoire : le livre peut n'apparaître qu'au niveau 3
ou 4 ; la sonde monte jusqu'à le trouver.

Usage : python3 scripts/check_villagers_e2e.py
"""
from __future__ import annotations

import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_interaction_e2e import Probe as BaseProbe  # noqa: E402
from check_interaction_e2e import item_id, items_by_id  # noqa: E402
from measure_villagers import decode_offers, read_slot  # noqa: E402
from vanilla_miner import read_varint, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PRESET = os.environ.get("OV_PRESET", "macos-debug")
BINARY = ROOT / "build" / PRESET / "bin" / "ov_dedicated"
WORLD = ROOT / ".scratch" / "e2e-villagers"
PORT = int(os.environ.get("OV_E2E_PORT", "25587"))

VILLAGER = 108
LIBRARIAN = 9
FLOOR = -61          # the superflat's top block; standing height is -60
LECTERN_AT = (3, FLOOR + 1, 3)


class Probe(BaseProbe):
    def __init__(self, port: int, name: str) -> None:
        super().__init__(port, name)
        self.villager_data: dict[int, list[tuple[float, tuple[int, int, int]]]] = {}
        self.window: int | None = None
        self.menu: int | None = None
        self.title = ""
        self.offers: dict | None = None
        self.state_id = 0
        self.contents: dict[int, list] = {}
        self.carried = None
        self.orbs: list[int] = []

    def handle(self, pid: int, payload: bytes) -> None:
        super().handle(pid, payload)
        now = time.monotonic()
        if pid in (0x2B, 0x2C):
            eid, i = read_varint(payload, 0)
            dx, dy, dz = struct.unpack_from(">hhh", payload, i)
            if eid in self.entity_pos:
                x, y, z = self.entity_pos[eid]
                self.entity_pos[eid] = (x + dx / 4096, y + dy / 4096, z + dz / 4096)
        elif pid == 0x68:
            eid, i = read_varint(payload, 0)
            self.entity_pos[eid] = struct.unpack_from(">ddd", payload, i)
        elif pid == 0x52:
            eid, i = read_varint(payload, 0)
            # Index 18 of type 18 (VillagerData) wherever it sits: walk the
            # fields this server sends a villager, stop at anything else.
            while i < len(payload):
                index = payload[i]
                i += 1
                if index == 0xFF:
                    break
                kind, i = read_varint(payload, i)
                if kind == 18:
                    t, i = read_varint(payload, i)
                    p, i = read_varint(payload, i)
                    level, i = read_varint(payload, i)
                    self.villager_data.setdefault(eid, []).append((now, (t, p, level)))
                elif kind in (1, 20):          # varint, pose
                    _, i = read_varint(payload, i)
                elif kind == 3:                # float
                    i += 4
                elif kind in (0, 8):           # byte, boolean
                    i += 1
                elif kind == 11:               # optional block pos
                    i += 1 + (8 if payload[i] else 0)
                else:
                    break
        elif pid == 0x30:
            self.window, i = read_varint(payload, 0)
            self.menu, i = read_varint(payload, i)
            n, i = read_varint(payload, i)
            self.title = payload[i:i + n].decode()
        elif pid == 0x2A:
            self.offers = decode_offers(payload)
        elif pid == 0x12:
            window = payload[0]
            self.state_id, i = read_varint(payload, 1)
            n, i = read_varint(payload, i)
            slots = []
            for _ in range(n):
                stack, i = read_slot(payload, i)
                slots.append(stack)
            self.contents[window] = slots
            self.carried, _ = read_slot(payload, i)
        elif pid == 0x02:
            self.orbs.append(struct.unpack_from(">h", payload, len(payload) - 2)[0])

    def data_of(self, eid: int):
        seen = self.villager_data.get(eid)
        return seen[-1][1] if seen else None

    def interact(self, eid: int) -> None:
        self.send(0x10, varint(eid) + varint(0) + varint(0) + bytes([0]))

    def select(self, index: int) -> None:
        self.send(0x26, varint(index))

    def click(self, slot: int, button: int = 0, mode: int = 0) -> None:
        self.send(0x0B, bytes([self.window or 0]) + varint(self.state_id)
                  + struct.pack(">hb", slot, button) + varint(mode) + varint(0) + b"\x00")

    def close_screen(self) -> None:
        self.send(0x0C, bytes([self.window or 0]))
        self.window = None


def connect() -> Probe:
    for _ in range(30):
        try:
            return Probe(PORT, "OndeLibraire")
        except (OSError, EOFError):
            time.sleep(3.0)
    raise RuntimeError("never joined")


def count_of(slots: list, item: int) -> int:
    return sum(s["count"] for s in slots if s and s["item"] == item)


def main() -> int:
    items = items_by_id()
    ids = {name: item_id(items, f"minecraft:{name}")
           for name in ("emerald", "book", "paper", "lectern", "enchanted_book", "ink_sac",
                        "writable_book")}
    shutil.rmtree(WORLD, ignore_errors=True)
    WORLD.mkdir(parents=True)
    log = open(ROOT / ".scratch" / "e2e-villagers.log", "w")
    server = subprocess.Popen([str(BINARY), f"--world={WORLD}", f"--port={PORT}",
                               "--log-level=debug", "--mobs=villager"],
                              stdout=log, stderr=subprocess.STDOUT)
    results: dict[str, str] = {}
    bought_at_level = None
    level_seen = 1
    ok = False
    try:
        time.sleep(3.0)
        probe = connect()
        probe.settle(8.0)
        villagers = [e for e, t in probe.entities.items() if t == VILLAGER]
        if not villagers:
            results["villageois"] = "le villageois de --mobs n'est pas arrivé"
            return 1
        eid = villagers[0]
        results["villageois à l'apparition"] = f"VillagerData {probe.data_of(eid)}"

        # 1-2. The lectern, and the profession.
        probe.creative_set(36, ids["lectern"], 1)
        probe.hold(0)
        probe.settle(0.5)
        probe.stand(1.5, FLOOR + 1, 1.5)
        probe.use_on(LECTERN_AT[0], FLOOR, LECTERN_AT[2], face=1)
        placed = time.monotonic()
        employed = None
        while time.monotonic() - placed < 40.0 and employed is None:
            probe.settle(0.25)
            data = probe.data_of(eid)
            if data and data[1] == LIBRARIAN:
                employed = time.monotonic() - placed
        results["bibliothécaire"] = (f"oui, {employed:.1f} s après le pupitre, "
                                     f"VillagerData {probe.data_of(eid)}" if employed
                                     else f"non (VillagerData {probe.data_of(eid)})")
        if employed is None:
            return 1

        # Supplies: the main inventory and the hotbar, all of it.
        stock = ([("book", 64)] * 8 + [("paper", 64)] * 6 + [("ink_sac", 64)] * 3
                 + [("writable_book", 16)] * 2 + [("emerald", 64)] * 8)
        for slot, (name, n) in zip(range(9, 36), stock):
            probe.creative_set(slot, ids[name], n)
        for slot in range(36, 45):
            probe.creative_set(slot, ids["emerald"], 64)
        probe.hold(0)
        probe.settle(0.8)

        def stand_near() -> None:
            x, y, z = probe.entity_pos.get(eid, (0.5, FLOOR + 1, 3.5))
            probe.stand(x + 1.2, FLOOR + 1, z)
            probe.settle(0.3)

        def open_screen() -> dict | None:
            for _ in range(3):
                probe.offers = None
                stand_near()
                probe.interact(eid)
                deadline = time.monotonic() + 3.0
                while time.monotonic() < deadline and probe.offers is None:
                    probe.settle(0.1)
                if probe.offers is not None:
                    return probe.offers
            return None

        def emeralds() -> int:
            return count_of(probe.contents.get(0, [])[9:45], ids["emerald"])

        for attempt in range(10):
            offers = open_screen()
            if offers is None:
                results[f"écran, tour {attempt}"] = "non ouvert"
                break
            if attempt == 0:
                results["écran"] = (f"fenêtre {probe.window}, menu {probe.menu}, "
                                    f"titre …{probe.title[-58:]}")
            listed = [(t["a"]["count"] if t["a"] else 0,
                       items.get(t["a"]["item"], "?").removeprefix("minecraft:") if t["a"] else "-",
                       items.get(t["result"]["item"], "?").removeprefix("minecraft:")
                       if t["result"] else "-")
                      for t in offers["trades"]]
            results[f"offres, niveau {offers['level']} (xp {offers['experience']})"] = str(listed)
            book = next((k for k, t in enumerate(offers["trades"])
                         if t["result"] and t["result"]["item"] == ids["enchanted_book"]
                         and not t["disabled"]), None)
            if bought_at_level is None and book is not None:
                probe.close_screen()
                probe.settle(0.6)
                before = emeralds()
                offers = open_screen()
                probe.select(book)
                probe.settle(0.4)
                probe.click(2)
                probe.settle(0.4)
                carried = probe.carried
                probe.close_screen()
                probe.settle(0.8)
                paid = before - emeralds()
                got = carried and carried["item"] == ids["enchanted_book"]
                nbt = bytes.fromhex(carried["nbt_hex"]) if got else b""
                name = nbt[nbt.find(b"minecraft:"):].split(b"\x00")[0].decode(errors="replace") \
                    if b"minecraft:" in nbt else "?"
                price = offers["trades"][book]["a"]["count"] if offers else "?"
                results["livre acheté"] = (f"{name}, prix affiché {price} émeraudes, payé {paid}, "
                                           f"orbes jusque-là {probe.orbs}" if got
                                           else f"non (curseur {carried})")
                if got:
                    bought_at_level = offers["level"] if offers else level_seen
                continue
            # Climb: every open offer, shift-clicked — repeated trades, as vanilla.
            for k, t in enumerate(offers["trades"]):
                if t["disabled"]:
                    continue
                probe.select(k)
                probe.settle(0.25)
                probe.click(2, mode=1)
                probe.settle(0.25)
            probe.close_screen()
            deadline = time.monotonic() + 6.0
            while time.monotonic() < deadline:
                probe.settle(0.25)
                data = probe.data_of(eid)
                if data and data[2] > level_seen:
                    results[f"niveau {data[2]}"] = (f"oui, métadonnée 18 = {data}, "
                                                    f"derniers orbes {probe.orbs[-3:]}")
                    level_seen = data[2]
                    break
            if bought_at_level is not None and level_seen > bought_at_level:
                ok = True
                break
        if bought_at_level is None:
            results.setdefault("livre acheté", "aucune offre de livre vue")
    finally:
        server.terminate()
        server.wait(timeout=30)
        shutil.rmtree(WORLD, ignore_errors=True)
        # Printed on every way out: a run that stops early says where.
        print()
        print("── résultat ─────────────────────────────────────────────")
        for key, value in results.items():
            print(f"  {key:30} {value}")
        print(f"  {'verdict':30} {'livre acheté, puis un niveau' if ok else 'incomplet'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
