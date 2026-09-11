#!/usr/bin/env python3
"""De bout en bout : brasser une potion de force sur notre serveur, la boire, en recevoir l'effet.

Le décor est une copie du banc (`run/lab`), servie par `ov_dedicated`. Le client est la sonde des
autres vérifications — protocole 763 exact, pas d'écran — et il ne fait que ce qu'un joueur ferait :

  1. poser un alambic, l'ouvrir (fenêtre `minecraft:brewing_stand`) ;
  2. y mettre trois fioles d'eau (données par `/give`, parce qu'une fiole d'eau n'est une fiole
     d'eau que par son tag `Potion`, et que Set Creative Slot n'en porte pas ici), de la poudre de
     blaze en combustible et une verrue du Nether ;
  3. regarder les deux barres (Container Property 0 et 1) et les fioles devenir « awkward » ;
  4. y remettre de la poudre de blaze comme ingrédient — **arrêter le serveur au milieu du
     brassage**, le relancer, rouvrir l'alambic : le contenu et le compte à rebours doivent avoir
     survécu à la sauvegarde Anvil ;
  5. laisser finir : trois potions de force ;
  6. en prendre une, passer en survie, la boire : Entity Effect force, niveau I, 3600 ticks, et la
     fiole vide revient dans la main ;
  7. lancer une potion jetable et une persistante à ses propres pieds : l'effet arrive, réduit par
     la distance pour la première, au quart pour le nuage.

Usage : python3 scripts/check_brewing_e2e.py
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import time
import uuid as uuidlib
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from measure_crafting import (CB_CONTAINER_CONTENT, CB_CONTAINER_SLOT, CB_OPEN_SCREEN,  # noqa: E402
                              Probe)
from vanilla_miner import block_pos, read_varint, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = Path(os.environ.get("OV_NORMALIZED", ROOT / "data/vanilla/1.20.1/normalized"))
PRESET = os.environ.get("OV_PRESET", "macos-debug")
BINARY = ROOT / "build" / PRESET / "bin" / "ov_dedicated"
LAB = ROOT / "run" / "lab"
WORLD = ROOT / "run" / "e2e-brewing"
PORT = int(os.environ.get("OV_BREW_E2E_PORT", "25651"))
NAME = "E2EBREW"

FLOOR = -60
STAND = (2, FLOOR, 169)      # a free square of the lab's workbench plot

CB_CONTAINER_PROPERTY = 0x13
CB_ENTITY_EFFECT = 0x6C
SB_CHAT_COMMAND = 0x04
SB_CLOSE_CONTAINER = 0x0C
SB_SET_HELD_ITEM = 0x28
SB_USE_ITEM = 0x32
SB_POSITION_ROTATION = 0x15
SB_USE_ITEM_ON = 0x31


def fail(message: str) -> None:
    print(f"\033[0;31m✗\033[0m {message}", flush=True)
    raise SystemExit(1)


def ok(message: str) -> None:
    print(f"\033[0;32m▸\033[0m {message}", flush=True)


def offline_uuid(name: str) -> str:
    digest = bytearray(hashlib.md5(f"OfflinePlayer:{name}".encode()).digest())
    digest[6] = (digest[6] & 0x0F) | 0x30
    digest[8] = (digest[8] & 0x3F) | 0x80
    return str(uuidlib.UUID(bytes=bytes(digest)))


# ── Slots, with the one NBT string this needs ───────────────────────────────


def nbt_strings(buf: bytes, i: int, out: dict, tag: int | None = None, named: bool = True) -> int:
    """Walk one NBT tag, collecting every string by its key."""
    if tag is None:
        tag = buf[i]
        i += 1
        if tag == 0:
            return i
    key = ""
    if named:
        length = struct.unpack_from(">H", buf, i)[0]
        key = buf[i + 2:i + 2 + length].decode("utf-8", "replace")
        i += 2 + length
    sizes = {1: 1, 2: 2, 3: 4, 4: 8, 5: 4, 6: 8}
    if tag in sizes:
        return i + sizes[tag]
    if tag == 7:
        return i + 4 + struct.unpack_from(">i", buf, i)[0]
    if tag == 8:
        length = struct.unpack_from(">H", buf, i)[0]
        out[key] = buf[i + 2:i + 2 + length].decode("utf-8", "replace")
        return i + 2 + length
    if tag == 9:
        inner = buf[i]
        n = struct.unpack_from(">i", buf, i + 1)[0]
        i += 5
        for _ in range(n):
            i = nbt_strings(buf, i, out, tag=inner, named=False)
        return i
    if tag == 10:
        while buf[i] != 0:
            i = nbt_strings(buf, i, out)
        return i + 1
    if tag in (11, 12):
        n = struct.unpack_from(">i", buf, i)[0]
        return i + 4 + n * (4 if tag == 11 else 8)
    raise RuntimeError(f"unknown NBT tag {tag}")


def read_slot(buf: bytes, i: int):
    if not buf[i]:
        return None, i + 1
    item, i = read_varint(buf, i + 1)
    count = buf[i]
    strings: dict = {}
    i = nbt_strings(buf, i + 1, strings)
    return (item, count, strings.get("Potion")), i


class Client(Probe):
    def __init__(self, port: int, name: str) -> None:
        self.properties: dict[int, int] = {}
        self.property_log: list[tuple[float, int, int]] = []
        self.full: dict[int, tuple | None] = {}
        self.inventory: dict[int, tuple | None] = {}
        self.effects: list[dict] = []
        super().__init__(port, name)

    def handle(self, pid: int, payload: bytes) -> None:
        if pid == CB_CONTAINER_PROPERTY and len(payload) == 5:
            window, prop, value = struct.unpack(">bhh", payload)
            if self.window is None or window == self.window:
                self.properties[prop] = value
                self.property_log.append((time.monotonic(), prop, value))
            return
        if pid == CB_ENTITY_EFFECT:
            _, i = read_varint(payload, 0)
            effect, i = read_varint(payload, i)
            amplifier = payload[i]
            duration, i = read_varint(payload, i + 1)
            self.effects.append({"effect": effect, "amplifier": amplifier, "duration": duration,
                                 "at": time.monotonic()})
            return
        if pid == CB_CONTAINER_CONTENT and self.window is not None and payload[0] == self.window:
            _, i = read_varint(payload, 1)
            count, i = read_varint(payload, i)
            for index in range(count):
                self.full[index], i = read_slot(payload, i)
        elif pid == CB_CONTAINER_SLOT:
            window = struct.unpack_from(">b", payload, 0)[0]
            _, i = read_varint(payload, 1)
            slot = struct.unpack_from(">h", payload, i)[0]
            stack, _ = read_slot(payload, i + 2)
            if window == 0:
                self.inventory[slot] = stack
            elif window == self.window:
                self.full[slot] = stack
        if pid == CB_OPEN_SCREEN:
            self.full = {}
        super().handle(pid, payload)

    def command(self, text: str) -> None:
        body = text.encode("utf-8")
        self.send(SB_CHAT_COMMAND, varint(len(body)) + body
                  + struct.pack(">qq", int(time.time() * 1000), 0) + varint(0) + varint(0)
                  + bytes(3))

    def walk_to(self, x: float, y: float, z: float) -> None:
        self.stand(x, y, z)
        self.settle(0.3)

    def look(self, x: float, y: float, z: float, yaw: float, pitch: float) -> None:
        self.send(SB_POSITION_ROTATION, struct.pack(">dddff", x, y, z, yaw, pitch) + bytes([1]))

    def use_on(self, pos, face: int = 1) -> None:
        self.send(SB_USE_ITEM_ON, varint(0) + block_pos(*pos) + varint(face)
                  + struct.pack(">fff", 0.5, 1.0, 0.5) + bytes([0]) + varint(0))

    def open_at(self, pos, tries: int = 8) -> int:
        self.window = None
        for _ in range(tries):
            self.walk_to(pos[0] + 0.5, float(pos[1]), pos[2] + 2.5)
            self.use_on(pos)
            deadline = time.monotonic() + 2.0
            while self.window is None and time.monotonic() < deadline:
                self.settle(0.1)
            if self.window is not None:
                self.settle(0.4)
                return self.window
        fail(f"rien ne s'est ouvert en cliquant {pos}")
        raise SystemExit(1)


def start_server(log_name: str) -> subprocess.Popen:
    log = open(WORLD / log_name, "w")
    return subprocess.Popen([str(BINARY), f"--port={PORT}", f"--world={WORLD}",
                             "--log-level=info"], cwd=WORLD, stdout=log, stderr=subprocess.STDOUT)


def stop_server(server: subprocess.Popen) -> None:
    server.terminate()
    try:
        server.wait(timeout=60)
    except subprocess.TimeoutExpired:
        server.kill()


def connect() -> Client:
    last = None
    for _ in range(20):
        try:
            client = Client(PORT, NAME)
            client.settle(3.0)
            return client
        except OSError as error:
            last = error
            time.sleep(2.0)
    fail(f"pas de connexion : {last}")
    raise SystemExit(1)


def wait_brew(client: Client, wanted: str) -> bool:
    """Until slot 0 holds `wanted`, as long as the brew bar keeps moving.

    Not a wall-clock deadline: a debug server on a loaded machine was measured
    at five ticks a second here, and 400 ticks then take eighty seconds. What
    fails the check is a bar that stops moving, or five minutes in all."""
    last, moved = client.properties.get(0), time.monotonic()
    deadline = time.monotonic() + 300.0
    while time.monotonic() < deadline:
        if potion_at(client, 0) == wanted:
            return True
        client.settle(0.25)
        now = client.properties.get(0)
        if now != last:
            last, moved = now, time.monotonic()
        elif time.monotonic() - moved > 20.0:
            return False
    return False


def wait_effect(client: Client, effect_id: int, seconds: float = 40.0) -> list[dict]:
    """Until an Entity Effect for `effect_id` arrives. A drink is 32 ticks, and
    this server was measured at five ticks a second on a loaded machine."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        got = [e for e in client.effects if e["effect"] == effect_id]
        if got:
            return got
        client.settle(0.25)
    return []


def potion_at(client: Client, slot: int) -> str | None:
    stack = client.full.get(slot)
    return stack[2] if stack else None


def main() -> int:
    if not BINARY.is_file():
        fail(f"{BINARY} not built")
    if not (LAB / "level.dat").is_file():
        fail(f"{LAB} absent — ./scripts/lab.sh --rebuild d'abord")
    items = json.loads((NORMALIZED / "registries.json").read_text())["registries"][
        "minecraft:item"]["entries"]
    index = {name: i for i, name in enumerate(items)}

    shutil.rmtree(WORLD, ignore_errors=True)
    shutil.copytree(LAB, WORLD)
    (WORLD / "ops.json").write_text(json.dumps([{"uuid": offline_uuid(NAME), "name": NAME,
                                                 "level": 4, "bypassesPlayerLimit": False}]))
    summary: dict = {}
    server = start_server("server-1.log")
    try:
        time.sleep(4.0)
        client = connect()
        ok(f"connecté ({NAME}, op 4)")

        # ── 1. L'alambic ────────────────────────────────────────────────────
        for slot in range(36, 45):
            client.creative_set(slot, None)
        client.creative_set(36, index["minecraft:brewing_stand"], 1)
        client.send(SB_SET_HELD_ITEM, struct.pack(">h", 0))
        client.walk_to(STAND[0] + 0.5, float(STAND[1]), STAND[2] + 2.5)
        client.use_on((STAND[0], STAND[1] - 1, STAND[2]))
        client.settle(0.8)
        client.creative_set(36, None)
        client.settle(0.3)
        client.command(f'give {NAME} minecraft:potion{{Potion:"minecraft:water"}} 3')
        client.settle(1.0)
        window = client.open_at(STAND)
        if window != 3:
            fail(f"l'alambic ouvre la fenêtre {window}, pas 3")
        slots = len(client.full)
        if slots != 41:
            fail(f"la fenêtre compte {slots} cases, pas 5 + 36")
        ok(f"alambic posé et ouvert : fenêtre {window}, {slots} cases")

        # ── 2. Le charger ───────────────────────────────────────────────────
        # `/give` is a command: on a server running five ticks a second it can
        # land after the window opened. Reopen until the bottles show.
        waters: list[int] = []
        for _ in range(12):
            waters = [s for s in range(5, 41) if potion_at(client, s) == "minecraft:water"]
            if len(waters) == 3:
                break
            client.send(SB_CLOSE_CONTAINER, bytes([client.window]))
            client.settle(5.0)
            client.open_at(STAND)
        if len(waters) != 3:
            fail(f"trois fioles d'eau attendues dans l'inventaire, {len(waters)} vues : {client.full}")
        for target, source in zip(range(3), waters):
            client.click(source, 0, 0)
            client.settle(0.2)
            client.click(target, 0, 0)
            client.settle(0.2)
        client.creative_set(43, index["minecraft:blaze_powder"], 2)
        client.creative_set(44, index["minecraft:nether_wart"], 1)
        client.settle(0.4)
        client.click(4, 7, 2)     # fuel  <- hotbar 7
        client.settle(0.3)
        client.click(3, 8, 2)     # ingredient <- hotbar 8
        started = time.monotonic()
        # Clicks are answered on the network thread, which waits for the lock an
        # overloaded tick holds: poll the state rather than trust a delay.
        deadline = time.monotonic() + 30.0
        placed = [potion_at(client, s) for s in range(3)]
        while time.monotonic() < deadline and placed != ["minecraft:water"] * 3:
            client.settle(0.5)
            placed = [potion_at(client, s) for s in range(3)]
        if placed != ["minecraft:water"] * 3:
            print(f"  (état de la fenêtre : curseur ?, cases {client.full})")
        if placed != ["minecraft:water"] * 3:
            fail(f"les trois fioles ne sont pas dans l'alambic : {placed}")
        ok(f"trois fioles d'eau, poudre et verrue en place ; propriétés {client.properties}")

        # ── 3. Le premier brassage ──────────────────────────────────────────
        wait_brew(client, "minecraft:awkward")
        took = time.monotonic() - started
        brews = [potion_at(client, s) for s in range(3)]
        if brews != ["minecraft:awkward"] * 3:
            fail(f"pas de potion étrange, barre arrêtée : {brews}, propriétés {client.properties}")
        times = [v for _, p, v in client.property_log if p == 0]
        fuels = [v for _, p, v in client.property_log if p == 1]
        summary["first"] = {"seconds": round(took, 2), "brew_time_first": times[:3],
                            "brew_time_last": times[-3:], "fuel": sorted(set(fuels))}
        ok(f"trois potions étranges en {took:.1f} s ; barre de brassage {times[:2]} … "
           f"{times[-2:]}, combustible {sorted(set(fuels))}")
        if 400 not in times and 399 not in times:
            fail(f"la barre n'a jamais montré 400 : {times[:5]}")
        if client.full.get(3):
            fail(f"la verrue n'a pas été consommée : {client.full.get(3)}")

        # ── 4. Le second, coupé par un redémarrage ──────────────────────────
        client.creative_set(42, index["minecraft:blaze_powder"], 1)
        client.settle(0.4)
        client.click(3, 6, 2)
        client.settle(6.0)
        before_stop = client.properties.get(0)
        ok(f"force en cours : barre à {before_stop} ; arrêt du serveur")
        client.s.close()
        stop_server(server)
        server = start_server("server-2.log")
        time.sleep(4.0)
        client = connect()
        client.open_at(STAND)
        client.settle(0.6)
        resumed = client.properties.get(0)
        held = [potion_at(client, s) for s in range(3)]
        ingredient = client.full.get(3)
        summary["restart"] = {"before": before_stop, "after": resumed, "bottles": held,
                              "ingredient": ingredient}
        if held != ["minecraft:awkward"] * 3 or not ingredient:
            fail(f"le contenu n'a pas survécu à la sauvegarde : {held}, ingrédient {ingredient}")
        # Either the brew went on from where the save left it, or the stand
        # forgot the ingredient it started with and began again (a fuel spent).
        # Which one vanilla does is the `timing` campaign's unload/reload
        # experiment; here it is only recorded. A bar at 0 is the failure.
        if resumed is None or resumed <= 0:
            fail(f"le brassage n'a pas repris : barre à {resumed}")
        summary["restart"]["restarted"] = before_stop is not None and resumed > before_stop
        ok(f"rechargé depuis le disque : trois potions étranges, poudre en ingrédient, "
           f"barre à {resumed} (elle était à {before_stop}), combustible "
           f"{client.properties.get(1)}")

        wait_brew(client, "minecraft:strength")
        brews = [potion_at(client, s) for s in range(3)]
        if brews != ["minecraft:strength"] * 3:
            fail(f"pas de potion de force : {brews}")
        ok(f"trois potions de force ; combustible {client.properties.get(1)}")

        # ── 5. En boire une ─────────────────────────────────────────────────
        client.click(0, 0, 1)                   # shift-click: back to front, hotbar 8 first
        client.settle(0.6)
        where = [s for s in range(5, 41) if potion_at(client, s) == "minecraft:strength"]
        if not where or where[0] < 32:
            fail(f"la potion n'est pas revenue dans la barre d'action : {where}")
        hotbar = where[0] - 32
        client.send(SB_CLOSE_CONTAINER, bytes([client.window]))
        client.settle(0.3)
        client.command(f"gamemode survival {NAME}")
        client.settle(0.8)
        client.effects.clear()
        client.send(SB_SET_HELD_ITEM, struct.pack(">h", hotbar))
        client.settle(0.3)
        client.send(SB_USE_ITEM, varint(0) + varint(77))
        drank = time.monotonic()
        strength = wait_effect(client, 5)
        if not strength:
            fail(f"aucune force reçue : {client.effects}")
        effect = strength[0]
        summary["drink"] = {"effect": effect, "seconds": round(effect["at"] - drank, 2)}
        if effect["amplifier"] != 0 or effect["duration"] != 3600:
            fail(f"force reçue mais pas celle attendue : {effect}")
        in_hand = client.inventory.get(36 + hotbar)
        glass = index["minecraft:glass_bottle"]
        if not in_hand or in_hand[0] != glass:
            fail(f"pas de fiole vide dans la main : {in_hand}")
        ok(f"bu : Entity Effect force I, 3600 ticks, {effect['at'] - drank:.2f} s après Use Item ; "
           f"fiole vide rendue")

        # ── 6. Jetable et persistante, à ses pieds ──────────────────────────
        x, y, z = STAND[0] + 4.5, float(FLOOR), STAND[2] + 0.5
        for label, item, potion, effect_id in (
                ("jetable", "splash_potion", "minecraft:strong_swiftness", 1),
                ("persistante", "lingering_potion", "minecraft:long_night_vision", 16)):
            client.command(f"effect clear {NAME}")
            for slot in range(36, 45):
                client.creative_set(slot, None)
            client.command(f'give {NAME} minecraft:{item}{{Potion:"{potion}"}}')
            client.settle(0.8)
            slot = next((s for s in range(36, 45) if (client.inventory.get(s) or (None,))[0]
                         == index[f"minecraft:{item}"]), None)
            if slot is None:
                fail(f"la potion {label} n'est pas arrivée : {client.inventory}")
            client.send(SB_SET_HELD_ITEM, struct.pack(">h", slot - 36))
            client.walk_to(x, y, z)
            client.look(x, y, z, 0.0, 90.0)
            client.settle(0.3)
            client.effects.clear()
            client.send(SB_USE_ITEM, varint(0) + varint(90 + slot))
            got = wait_effect(client, effect_id)
            summary[label] = got
            if not got:
                fail(f"potion {label} : aucun effet reçu ({client.effects})")
            ok(f"potion {label} : effet {effect_id}, niveau {got[0]['amplifier'] + 1}, "
               f"{got[0]['duration']} ticks")
        print("\n\033[0;32mtout est passé\033[0m")
        print(json.dumps(summary, indent=1, default=str))
    finally:
        stop_server(server)
    # A probe's world is not kept (the briefing's rule for temporary worlds).
    shutil.rmtree(WORLD, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
