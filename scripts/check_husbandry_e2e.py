#!/usr/bin/env python3
"""De bout en bout : nourrir deux vaches, voir naître un veau, le voir grandir
sous la nourriture ; tondre un mouton ; traire une vache — sur **notre** serveur.

Le client est une sonde protocole 763 (celle de `vanilla_miner.py`) : il ne voit
que ce que le serveur lui envoie, exactement comme un vrai client. Chaque étape
est jugée sur le fil :

  1. deux `Interact` avec du blé → deux `Entity Event` statut 18 (les cœurs) ;
  2. un `Spawn Entity` de vache dont la métadonnée porte l'indice 16 à vrai
     (bébé), et un `Spawn Experience Orb` ;
  3. le veau nourri jusqu'à ce que la nourriture ne serve plus (un dixième du
     restant, en secondes entières), puis la métadonnée 16 qui repasse à faux ;
  4. des cisailles sur le mouton → une à trois piles de laine blanche, et
     l'indice 17 qui passe à 16 (tondu) ;
  5. un seau sur une vache adulte → un `Set Container Slot` de seau de lait.

Le serveur est lancé en créatif (le mode par défaut) : `Set Creative Slot` est
la seule façon pour la sonde de se donner du blé, des cisailles et un seau, et
le créatif ne consomme rien — ce qui ne change aucune des cinq preuves.

Usage : python3 scripts/check_husbandry_e2e.py
"""
from __future__ import annotations

import os
import shutil
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vanilla_miner import Miner, read_varint, varint  # noqa: E402
from measure_husbandry import parse_metadata  # noqa: E402
from check_interaction_e2e import items_by_id, item_id  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PRESET = os.environ.get("OV_PRESET", "macos-debug")
BINARY = ROOT / "build" / PRESET / "bin" / "ov_dedicated"
WORLD = ROOT / ".scratch" / "e2e-husbandry"
PORT = int(os.environ.get("OV_E2E_PORT", "25583"))

COW, SHEEP, ITEM = 18, 82, 54


class Probe(Miner):
    def __init__(self, port: int, name: str) -> None:
        super().__init__(port, name)
        self.types: dict[int, int] = {}
        self.where: dict[int, list[float]] = {}
        self.meta: dict[int, list[tuple[float, list]]] = {}
        self.events: list[tuple[float, int, int]] = []
        self.orbs: list[int] = []
        self.spawned_at: dict[int, float] = {}
        self.slots: list[tuple[int, int]] = []

    def handle(self, pid: int, p: bytes) -> None:
        now = time.monotonic()
        if pid == 0x01:
            eid, i = read_varint(p, 0)
            i += 16
            etype, i = read_varint(p, i)
            x, y, z = struct.unpack_from(">ddd", p, i)
            self.types[eid] = etype
            self.where[eid] = [x, y, z]
            self.spawned_at[eid] = now
        elif pid == 0x02:
            self.orbs.append(struct.unpack_from(">h", p, len(p) - 2)[0])
        elif pid in (0x2B, 0x2C):
            eid, i = read_varint(p, 0)
            dx, dy, dz = struct.unpack_from(">hhh", p, i)
            if eid in self.where:
                w = self.where[eid]
                self.where[eid] = [w[0] + dx / 4096, w[1] + dy / 4096, w[2] + dz / 4096]
        elif pid == 0x68:
            eid, i = read_varint(p, 0)
            self.where[eid] = list(struct.unpack_from(">ddd", p, i))
        elif pid == 0x52:
            eid, i = read_varint(p, 0)
            try:
                fields = parse_metadata(p, i)
            except Exception:
                fields = []
            self.meta.setdefault(eid, []).append((now, fields))
        elif pid == 0x1C and len(p) >= 5:
            self.events.append((now, struct.unpack_from(">i", p, 0)[0],
                                struct.unpack_from(">b", p, 4)[0]))
        elif pid == 0x14:
            _, i = 0, 1
            _, i = read_varint(p, i)
            slot = struct.unpack_from(">h", p, i)[0]
            i += 2
            if p[i]:
                item, _ = read_varint(p, i + 1)
                self.slots.append((slot, item))

    def settle(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.pump(until=lambda pid, p: self.handle(pid, p), timeout=0.05)

    def interact(self, eid: int) -> None:
        self.send(0x10, varint(eid) + varint(0) + varint(0) + bytes([0]))

    def creative_set(self, slot: int, item: int, count: int = 1) -> None:
        self.send(0x2B, struct.pack(">h", slot) + bytes([1]) + varint(item) + bytes([count, 0]))

    def near(self, eid: int) -> None:
        x, y, z = self.where[eid]
        self.stand(x + 1.0, y, z)

    def field(self, eid: int, index: int):
        value = None
        for _, fields in self.meta.get(eid, []):
            for i, _, v in fields:
                if i == index:
                    value = v
        return value


def connect() -> Probe:
    for _ in range(20):
        try:
            return Probe(PORT, "OndeFarmer")
        except (OSError, EOFError):
            time.sleep(4.0)
    raise RuntimeError("never joined")


def main() -> int:
    items = items_by_id()
    wheat = item_id(items, "minecraft:wheat")
    shears = item_id(items, "minecraft:shears")
    bucket = item_id(items, "minecraft:bucket")
    milk = item_id(items, "minecraft:milk_bucket")
    white_wool = item_id(items, "minecraft:white_wool")

    shutil.rmtree(WORLD, ignore_errors=True)
    WORLD.mkdir(parents=True)
    log = open(ROOT / ".scratch" / "e2e-husbandry.log", "w")
    server = subprocess.Popen([str(BINARY), f"--world={WORLD}", f"--port={PORT}",
                               "--log-level=debug", "--mobs=cow,cow,sheep"],
                              stdout=log, stderr=subprocess.STDOUT)
    results: dict[str, str] = {}
    try:
        time.sleep(3.0)
        probe = connect()
        probe.settle(8.0)
        cows = [e for e, t in probe.types.items() if t == COW]
        sheep = [e for e, t in probe.types.items() if t == SHEEP]
        print(f"vaches {cows}, moutons {sheep}")
        if len(cows) < 2 or not sheep:
            print("les mobs de --mobs ne sont pas arrivés")
            return 1

        # 1. Les cœurs.
        probe.creative_set(36, wheat, 64)
        probe.send(0x28, struct.pack(">h", 0))
        probe.settle(0.5)
        for cow in cows[:2]:
            probe.near(cow)
            probe.settle(0.3)
            probe.interact(cow)
            probe.settle(0.3)
        hearts = sorted({e for _, e, s in probe.events if s == 18})
        results["cœurs (statut 18)"] = f"{len(hearts)}/2"
        fed_at = time.monotonic()

        # 2. Le veau.
        calf = None
        deadline = time.monotonic() + 15.0
        while time.monotonic() < deadline and calf is None:
            probe.settle(0.2)
            for e, t in probe.types.items():
                if t == COW and e not in cows and probe.field(e, 16) is True:
                    calf = e
        if calf is None:
            results["veau"] = "non"
        else:
            delay = probe.spawned_at[calf] - fed_at
            results["veau"] = (f"oui, {delay:.1f} s après le repas, métadonnée 16 = vrai, "
                               f"orbe(s) {probe.orbs}")

            # 3. Grandir sous la nourriture.
            for _ in range(46):
                probe.near(calf)
                probe.interact(calf)
                probe.settle(0.12)
            grown = None
            deadline = time.monotonic() + 40.0
            while time.monotonic() < deadline:
                probe.settle(0.25)
                if probe.field(calf, 16) is False:
                    grown = time.monotonic() - fed_at
                    break
            results["veau adulte"] = (f"oui, {grown:.1f} s après le premier repas "
                                      "(1 200 s sans nourriture)" if grown else "non")

        # 4. Tondre.
        probe.creative_set(36, shears, 1)
        probe.settle(0.5)
        before = set(probe.types)
        probe.near(sheep[0])
        probe.settle(0.3)
        probe.interact(sheep[0])
        probe.settle(1.0)
        drops = [e for e, t in probe.types.items() if t == ITEM and e not in before]
        wool = sum(1 for e in drops if (probe.field(e, 8) or (None,))[0] == white_wool)
        results["tonte"] = (f"{wool} laine(s) blanche(s), indice 17 = {probe.field(sheep[0], 17)}")

        # 5. Traire.
        probe.creative_set(36, bucket, 1)
        probe.settle(0.5)
        probe.slots.clear()
        probe.near(cows[0])
        probe.settle(0.3)
        probe.interact(cows[0])
        probe.settle(1.0)
        results["traite"] = "oui" if any(item == milk for _, item in probe.slots) else "non"
    finally:
        server.terminate()
        server.wait(timeout=30)
        shutil.rmtree(WORLD, ignore_errors=True)

    print()
    print("── résultat ─────────────────────────────────────────────")
    for key, value in results.items():
        print(f"  {key:20} {value}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
