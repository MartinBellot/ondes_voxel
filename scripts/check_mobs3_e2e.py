#!/usr/bin/env python3
"""De bout en bout, sur **notre** serveur : les mobs hostiles qui frappent, le
despawn, le villageois zombifié puis guéri, et un monde vanilla relu.

La sonde est celle de `check_husbandry_e2e.py` (protocole 763), jugée sur le fil
seulement ; les commandes passent par la console d'`ov_dedicated`.

  melee     un zombie, un husk, une araignée venimeuse à côté de la sonde en
            survie : chaque `Set Health` (0x57) qui baisse est un coup — dégâts,
            intervalle en ticks (`Update Time`), `Entity Effect` (0x6C) reçu.
  despawn   des zombies à 16 et 100 blocs ; puis la sonde téléportée à 300 blocs :
            les `Remove Entities` comptés.
  villager  un villageois et un zombie dans un enclos, en difficile : le
            `Spawn Entity` du villageois zombie et son indice 20 ; puis une potion
            jetable de Faiblesse, une pomme d'or, l'indice 19, et le villageois
            guéri qui réapparaît.
  anvil     notre serveur sur une copie du monde vanilla de `measure_mobs3.py
            anvil` : chaque type du zoo reçu sur le fil ; `save-all`, et le monde
            réécrit gardé pour `measure_mobs3.py anvil_back`.

Usage : python3 scripts/check_mobs3_e2e.py [melee] [despawn] [villager] [anvil]
Écrit .scratch/mobs3_e2e.json (non suivi).
"""
from __future__ import annotations

import json
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_husbandry_e2e import Probe as HusbandryProbe  # noqa: E402
from check_interaction_e2e import items_by_id, item_id  # noqa: E402
from vanilla_miner import read_varint, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PRESET = os.environ.get("OV_PRESET", "macos-debug")
BINARY = ROOT / "build" / PRESET / "bin" / "ov_dedicated"
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
OUT = ROOT / ".scratch" / "mobs3_e2e.json"
VANILLA_ZOO = ROOT / ".scratch" / "mobs3-anvil-vanilla"
OURS_ZOO = ROOT / ".scratch" / "mobs3-anvil-ours"
PORT = int(os.environ.get("OV_E2E_PORT", "25657"))
NAME = "OndeMobs3"

SET_HEALTH, ENTITY_EFFECT, REMOVE_ENTITIES, LOGIN = 0x57, 0x6C, 0x3E, 0x28
UPDATE_TIME = 0x5E


def entity_names() -> list[str]:
    reg = json.loads((NORMALIZED / "registries.json").read_text())
    return reg["registries"]["minecraft:entity_type"]["entries"]


TYPES = entity_names()


def tid(name: str) -> int:
    return TYPES.index(f"minecraft:{name}")


class Probe(HusbandryProbe):
    def __init__(self, port: int, name: str) -> None:
        super().__init__(port, name)
        self.health: list[tuple[int | None, float]] = []
        self.effects: list[tuple[int, int, int]] = []  # (entity, effect, duration)
        self.removed: list[tuple[float, int]] = []
        self.me = 0
        self.age: int | None = None

    def handle(self, pid: int, p: bytes) -> None:
        if pid == SET_HEALTH:
            self.health.append((self.age, struct.unpack_from(">f", p, 0)[0]))
        elif pid == ENTITY_EFFECT:
            eid, i = read_varint(p, 0)
            effect, i = read_varint(p, i)
            i += 1  # amplifier
            duration, _ = read_varint(p, i)
            self.effects.append((eid, effect, duration))
        elif pid == REMOVE_ENTITIES:
            count, i = read_varint(p, 0)
            for _ in range(count):
                eid, i = read_varint(p, i)
                self.removed.append((time.monotonic(), eid))
        elif pid == LOGIN:
            self.me = struct.unpack_from(">i", p, 0)[0]
        elif pid == UPDATE_TIME and len(p) >= 8:
            self.age = struct.unpack_from(">q", p, 0)[0]
        super().handle(pid, p)

    def hold(self, slot: int) -> None:
        self.send(0x28, struct.pack(">h", slot))

    def look(self, yaw: float, pitch: float) -> None:
        self.send(0x16, struct.pack(">ff", yaw, pitch) + bytes([1]))

    def use_item(self, sequence: int) -> None:
        self.send(0x32, varint(0) + varint(sequence))

    def creative_nbt(self, slot: int, item: int, nbt: bytes) -> None:
        self.send(0x2B, struct.pack(">h", slot) + bytes([1]) + varint(item) + bytes([1]) + nbt)


def potion_nbt(potion: str) -> bytes:
    key, value = b"Potion", potion.encode()
    return (b"\x0a\x00\x00" + b"\x08" + struct.pack(">H", len(key)) + key +
            struct.pack(">H", len(value)) + value + b"\x00")


class Ours:
    def __init__(self, world: Path, fresh: bool = True) -> None:
        if fresh:
            shutil.rmtree(world, ignore_errors=True)
            world.mkdir(parents=True)
        self.log = open(world.parent / f"{world.name}.log", "w")
        self.process = subprocess.Popen(
            [str(BINARY), f"--world={world}", f"--port={PORT}", "--log-level=info"],
            stdin=subprocess.PIPE, stdout=self.log, stderr=subprocess.STDOUT, text=True)
        self.world = world

    def console(self, *lines: str) -> None:
        assert self.process.stdin is not None
        for line in lines:
            self.process.stdin.write(line + "\n")
        self.process.stdin.flush()

    def stop(self, keep: bool = False) -> None:
        try:
            self.console("stop")
            self.process.wait(timeout=90)
        except Exception:
            self.process.kill()
        if not keep:
            shutil.rmtree(self.world, ignore_errors=True)


def connect() -> Probe:
    for _ in range(40):
        try:
            return Probe(PORT, NAME)
        except (OSError, EOFError):
            time.sleep(4.0)
    raise RuntimeError("never joined")


def settle_until(probe: Probe, seconds: float, done) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline and not done():
        probe.settle(0.25)


def spawned_of(probe: Probe, kind: str, after: set[int]) -> list[int]:
    return [e for e, t in probe.types.items() if t == tid(kind) and e not in after]


# ── melee ───────────────────────────────────────────────────────────────────

def melee_one(server: Ours, probe: Probe, mob: str, difficulty: str, armour: str | None,
              items: dict) -> dict:
    server.console("kill @e[type=!minecraft:player]", f"difficulty {difficulty}",
                   f"gamemode creative {NAME}")
    probe.settle(0.5)
    for slot in (5, 6, 7, 8):
        probe.creative_set(slot, 0, 0)
    if armour:
        for slot, piece in zip((5, 6, 7, 8), ("helmet", "chestplate", "leggings", "boots")):
            probe.creative_set(slot, item_id(items, f"minecraft:{armour}_{piece}"))
    probe.settle(0.5)
    server.console(f"gamemode survival {NAME}", f"effect clear {NAME}",
                   f"effect give {NAME} minecraft:instant_health 1 5")
    probe.settle(1.0)
    x, y, z = probe.pos
    probe.health.clear()
    probe.effects.clear()
    server.console(f"summon minecraft:{mob} {x + 2.0:.2f} {y:.2f} {z:.2f}")
    hits: list[dict] = []
    last = None
    begin = time.monotonic()
    while len(hits) < 5 and time.monotonic() - begin < 40.0:
        probe.settle(0.25)
        probe.stand(x, y, z)
        for age, hp in probe.health:
            if last is not None and hp < last - 1e-4:
                hits.append({"age": age, "lost": round(last - hp, 5)})
            last = hp
        probe.health.clear()
        if last is not None and last < 9.0:
            server.console(f"effect give {NAME} minecraft:instant_health 1 5")
    effects = [(e, d) for eid, e, d in probe.effects if eid == probe.me]
    return {"mob": mob, "difficulty": difficulty, "armour": armour, "hits": hits,
            "effects": effects}


def check_melee() -> list[dict]:
    items = items_by_id()
    server = Ours(ROOT / ".scratch" / "e2e-mobs3-melee")
    out = []
    try:
        probe = connect()
        probe.settle(3.0)
        server.console("gamerule naturalRegeneration false", "time set 18000",
                       "gamerule doMobSpawning false")
        for difficulty in ("easy", "normal", "hard"):
            for armour in (None, "iron", "diamond"):
                print(f"   zombie {difficulty} {armour}", flush=True)
                out.append(melee_one(server, probe, "zombie", difficulty, armour, items))
            for mob in ("husk", "cave_spider"):
                print(f"   {mob} {difficulty}", flush=True)
                out.append(melee_one(server, probe, mob, difficulty, None, items))
    finally:
        server.stop()
    return out


# ── despawn ─────────────────────────────────────────────────────────────────

def check_despawn() -> dict:
    server = Ours(ROOT / ".scratch" / "e2e-mobs3-despawn")
    try:
        probe = connect()
        probe.settle(3.0)
        server.console("gamerule doMobSpawning false", "time set 18000", "difficulty normal",
                       f"gamemode creative {NAME}")
        probe.settle(1.0)
        x, y, z = probe.pos
        before = set(probe.types)
        for i in range(4):
            server.console(f"summon minecraft:zombie {x + 16:.1f} {y:.1f} {z + i * 2:.1f}")
        probe.settle(2.0)
        near = spawned_of(probe, "zombie", before)
        before = set(probe.types)
        for i in range(8):
            server.console(f"summon minecraft:zombie {x + 100:.1f} {y + 2:.1f} {z + i * 2:.1f}")
        probe.settle(2.0)
        middle = spawned_of(probe, "zombie", before)
        start = time.monotonic()
        series = []
        while time.monotonic() - start < 150.0:
            probe.settle(5.0)
            probe.stand(x, y, z)
            gone = {e for _, e in probe.removed}
            series.append({"t": round(time.monotonic() - start, 1),
                           "near_left": sum(e not in gone for e in near),
                           "middle_left": sum(e not in gone for e in middle)})
        server.console(f"tp {NAME} {x + 300:.1f} {y + 20:.1f} {z:.1f}")
        probe.settle(3.0)
        gone = {e for _, e in probe.removed}
        return {"near": near, "middle": middle, "series": series,
                "near_after_leaving": sum(e not in gone for e in near)}
    finally:
        server.stop()


# ── villager ────────────────────────────────────────────────────────────────

def check_villager() -> dict:
    items = items_by_id()
    server = Ours(ROOT / ".scratch" / "e2e-mobs3-villager")
    out: dict = {}
    try:
        probe = connect()
        probe.settle(3.0)
        server.console("gamerule doMobSpawning false", "time set 18000", "difficulty hard",
                       f"gamemode creative {NAME}")
        probe.settle(1.0)
        x, y, z = (int(c) for c in probe.pos)
        px, pz = x + 8, z
        server.console(f"fill {px - 2} {y} {pz - 2} {px + 2} {y + 1} {pz + 2} minecraft:glass",
                       f"fill {px - 1} {y} {pz - 1} {px + 1} {y + 1} {pz + 1} minecraft:air")
        probe.settle(1.0)
        before = set(probe.types)
        server.console(f"summon minecraft:villager {px + 0.5} {y} {pz + 0.5}",
                       f"summon minecraft:zombie {px + 0.5} {y} {pz + 0.5}")
        settle_until(probe, 90.0, lambda: spawned_of(probe, "zombie_villager", before))
        risen = spawned_of(probe, "zombie_villager", before)
        out["risen"] = risen
        if not risen:
            return out
        zv = risen[0]
        probe.settle(1.0)
        out["index20"] = repr(probe.field(zv, 20))
        server.console("kill @e[type=minecraft:zombie]")
        # Weakness, thrown from the pen's wall, straight down.
        probe.creative_nbt(37, item_id(items, "minecraft:splash_potion"),
                           potion_nbt("minecraft:weakness"))
        probe.creative_set(36, item_id(items, "minecraft:golden_apple"), 4)
        probe.settle(0.5)
        probe.stand(px + 2.5, y + 2, pz + 0.5)
        probe.look(0.0, 90.0)
        probe.hold(1)
        probe.settle(0.3)
        probe.use_item(1)
        probe.settle(2.0)
        server.console(f"gamemode survival {NAME}")
        probe.hold(0)
        probe.settle(0.5)
        probe.interact(zv)
        settle_until(probe, 5.0, lambda: probe.field(zv, 19) is not None)
        out["index19"] = repr(probe.field(zv, 19))
        started = probe.age
        before = set(probe.types)
        settle_until(probe, 360.0, lambda: spawned_of(probe, "villager", before))
        cured = spawned_of(probe, "villager", before)
        out["cured"] = cured
        out["cure_ticks"] = (probe.age - started) if (cured and started and probe.age) else None
        if cured:
            probe.settle(1.0)
            out["cured_index18"] = repr(probe.field(cured[0], 18))
    finally:
        server.stop()
    return out


# ── anvil ───────────────────────────────────────────────────────────────────

ZOO = ["zombie", "zombie", "husk", "drowned", "skeleton", "stray", "creeper", "spider",
       "cave_spider", "witch", "enderman", "slime", "cow", "cow", "pig", "sheep", "chicken",
       "villager", "zombie_villager", "wolf", "cat", "rabbit", "fox", "horse"]


def check_anvil() -> dict:
    if not VANILLA_ZOO.exists():
        return {"error": f"no {VANILLA_ZOO}: run measure_mobs3.py anvil"}
    shutil.rmtree(OURS_ZOO, ignore_errors=True)
    shutil.copytree(VANILLA_ZOO, OURS_ZOO)
    server = Ours(OURS_ZOO, fresh=False)
    out: dict = {}
    try:
        probe = connect()
        probe.settle(20.0)
        seen: dict[str, int] = {}
        ids: dict[str, list[int]] = {}
        for eid, t in probe.types.items():
            name = TYPES[t].removeprefix("minecraft:")
            seen[name] = seen.get(name, 0) + 1
            ids.setdefault(name, []).append(eid)
        expected: dict[str, int] = {}
        for name in ZOO:
            expected[name] = expected.get(name, 0) + 1
        out["seen"] = seen
        out["missing"] = {k: v - seen.get(k, 0) for k, v in expected.items() if seen.get(k, 0) < v}
        if "villager" in ids:
            out["villager_index18"] = repr(probe.field(ids["villager"][0], 18))
        if "zombie_villager" in ids:
            out["zombie_villager_index20"] = repr(probe.field(ids["zombie_villager"][0], 20))
        if "slime" in ids:
            out["slime_index16"] = repr(probe.field(ids["slime"][0], 16))
        if "sheep" in ids:
            out["sheep_index17"] = repr(probe.field(ids["sheep"][0], 17))
        server.console("save-all")
        probe.settle(3.0)
    finally:
        server.stop(keep=True)
    out["entities_files"] = sorted(p.name for p in (OURS_ZOO / "entities").glob("*.mca"))
    return out


CHECKS = {"melee": check_melee, "despawn": check_despawn, "villager": check_villager,
          "anvil": check_anvil}


def main(argv: list[str]) -> int:
    wanted = argv or list(CHECKS)
    result = json.loads(OUT.read_text()) if OUT.exists() else {}
    for name in wanted:
        print(f"== {name}", flush=True)
        result[name] = CHECKS[name]()
        OUT.write_text(json.dumps(result, indent=1))
        print(json.dumps(result[name], indent=1)[:3000], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
