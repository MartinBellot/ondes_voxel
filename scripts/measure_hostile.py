#!/usr/bin/env python3
"""Status effects on mobs, and the hostile and neutral mobs still missing their
behaviour. The oracle is the real 1.20.1 server (tools/vanilla/server.jar),
driven through its console, with a recording probe client connected.

Campaigns (run all by default, or name some):

  packets   what a client watching a cow is told when the cow is given an
            effect and when it loses it: which packet ids carry the cow's
            entity id, and the metadata indices among them.
  speed     zombies strolling under Speed I, Speed II and Slowness I (hidden,
            infinite), and a control field with nothing: the cruise speed, to
            be put against the walk law with the attribute's modifier.
  strength  a zombie under Strength I, then under Weakness I, next to a
            survival probe on Normal: what each hit takes.
  splash    a splash potion of Poison broken over a row of NoAI cows at known
            distances, then Harming over zombies and Healing over zombies: the
            duration each cow got, the health each zombie gained or lost.
  arrow     tipped arrows (Poison, Strong Poison) and a spectral arrow falling
            onto NoAI cows: the effect each cow carries.
  anvil     cows and zombies saved by vanilla with effects on them, the world
            kept under .scratch/hostile-anvil-vanilla/ for the round trip.

Raw readings go to .scratch/hostile.json (not tracked). The results are in
docs/provenance/mobs-4.md.

    ovlane.sh java python3 scripts/measure_hostile.py [campaign ...]
"""
from __future__ import annotations

import json
import math
import re
import shutil
import struct
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_entity_packets import Probe, read_varint  # noqa: E402
from measure_mobs import FlatServer  # noqa: E402
import measure_mobs2 as mm2  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / ".scratch" / "hostile-oracle"
OUT = ROOT / ".scratch" / "hostile.json"
ANVIL_COPY = ROOT / ".scratch" / "hostile-anvil-vanilla"
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
PORT = 25713
Y = -60
PROBE = "ovprobe"

GAMETIME = re.compile(r"The time is (\d+)")
DATA = re.compile(r"has the following entity data: (.+)$")
EFFECT = re.compile(r"\{[^{}]*\}")
EFFECT_ID = re.compile(r"\bId: (-?\d+)")
EFFECT_DURATION = re.compile(r"\bDuration: (-?\d+)")
EFFECT_AMPLIFIER = re.compile(r"\bAmplifier: (-?\d+)b")

SPAWN_ENTITY, METADATA, ATTRIBUTES, ENTITY_EFFECT, REMOVE_EFFECT = 0x01, 0x52, 0x6A, 0x6C, 0x3F
TYPES = json.loads((NORMALIZED / "registries.json").read_text())["registries"][
    "minecraft:entity_type"]["entries"]


def tid(name: str) -> int:
    return TYPES.index(f"minecraft:{name}")


class Oracle(FlatServer):
    EXTRA_PROPERTIES = FlatServer.EXTRA_PROPERTIES + "difficulty=normal\n"
    HEAP = "-Xmx1200M"


class Recorder:
    """A probe that stays connected and keeps every packet, with a clock."""

    def __init__(self, port: int, name: str = PROBE) -> None:
        self.probe = Probe(port, name)
        self.lock = threading.Lock()
        self.packets: list[tuple[float, int, bytes]] = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                self.probe.pump(0.3)
            except Exception:
                return
            got = self.probe.drain()
            now = time.monotonic()
            with self.lock:
                self.packets.extend((now, pid, p) for pid, p in got)

    def take(self) -> list[tuple[float, int, bytes]]:
        with self.lock:
            out, self.packets = self.packets, []
        return out

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2.0)
        try:
            self.probe.socket.close()
        except Exception:
            pass


def value(lines: list[str]) -> str | None:
    for line in lines:
        m = DATA.search(line)
        if m:
            return m.group(1).strip()
    return None


def effects_of(raw: str | None) -> list[dict]:
    out = []
    for compound in EFFECT.findall(raw or ""):
        i = EFFECT_ID.search(compound)
        d = EFFECT_DURATION.search(compound)
        a = EFFECT_AMPLIFIER.search(compound)
        if i and d:
            out.append({"id": int(i.group(1)), "duration": int(d.group(1)),
                        "amplifier": int(a.group(1)) if a else 0})
    return out


def setup(server) -> None:
    server.batch(["gamerule doMobSpawning false", "gamerule naturalRegeneration false",
                  "gamerule doDaylightCycle false", "gamerule doWeatherCycle false",
                  "gamerule doImmediateRespawn true", "gamerule showDeathMessages false",
                  "scoreboard objectives add ovcount dummy",
                  "time set midnight", "weather clear"], timeout=60)


def fresh(server, difficulty: str = "normal", mode: str = "creative") -> None:
    server.batch(["kill @e[type=!minecraft:player]", f"difficulty {difficulty}",
                  f"gamemode {mode} {PROBE}", f"tp {PROBE} 0.5 {Y} 0.5 -90 0",
                  f"effect clear {PROBE}"], timeout=60)
    time.sleep(0.5)


def metadata_indices(payload: bytes) -> dict:
    """The (index -> value) of an Entity Metadata payload, for the simple types."""
    _, i = read_varint(payload, 0)
    out = {}
    while i < len(payload):
        index = payload[i]
        i += 1
        if index == 0xFF:
            break
        kind, i = read_varint(payload, i)
        if kind == 0:      # byte
            out[index] = payload[i]
            i += 1
        elif kind == 1:    # varint
            out[index], i = read_varint(payload, i)
        elif kind == 3:    # float
            out[index] = struct.unpack_from(">f", payload, i)[0]
            i += 4
        elif kind == 8:    # boolean
            out[index] = bool(payload[i])
            i += 1
        else:
            out[index] = f"type{kind}"
            break
    return out


def about(packets, eid: int) -> list[dict]:
    """The packets whose payload starts with this entity id."""
    out = []
    for t, pid, p in packets:
        if pid not in (METADATA, ATTRIBUTES, ENTITY_EFFECT, REMOVE_EFFECT):
            continue
        try:
            first, _ = read_varint(p, 0)
        except Exception:
            continue
        if first != eid:
            continue
        row = {"id": hex(pid), "hex": p.hex()[:120]}
        if pid == METADATA:
            row["fields"] = metadata_indices(p)
        out.append(row)
    return out


# ── packets ─────────────────────────────────────────────────────────────────

def campaign_packets(server, rec: Recorder) -> dict:
    fresh(server)
    rec.take()
    server.batch([f"summon minecraft:cow 3.5 {Y} 0.5 {{NoAI:1b,PersistenceRequired:1b,Silent:1b,"
                  "Tags:[\"ovc\"]}"])
    time.sleep(1.5)
    cow = None
    for _, pid, p in rec.take():
        if pid == SPAWN_ENTITY:
            eid, i = read_varint(p, 0)
            kind, _ = read_varint(p, i + 16)
            if kind == tid("cow"):
                cow = eid
    out: dict = {"cow": cow}
    if cow is None:
        return out
    steps = [
        ("give_speed", "effect give @e[tag=ovc] minecraft:speed 30 1"),
        ("give_poison_hidden", "effect give @e[tag=ovc] minecraft:poison 30 0 true"),
        ("clear_speed", "effect clear @e[tag=ovc] minecraft:speed"),
        ("clear_all", "effect clear @e[tag=ovc]"),
        ("give_invisibility", "effect give @e[tag=ovc] minecraft:invisibility 30 0"),
        ("give_glowing", "effect give @e[tag=ovc] minecraft:glowing 30 0"),
        ("clear_all_2", "effect clear @e[tag=ovc]"),
    ]
    for label, command in steps:
        server.batch([command])
        time.sleep(1.5)
        out[label] = about(rec.take(), cow)
    # A second client joining now: what it is told about a cow with an effect.
    server.batch(["effect give @e[tag=ovc] minecraft:slowness 60 2"])
    time.sleep(1.0)
    rec.take()
    late = Recorder(PORT, "ovlate")
    time.sleep(4.0)
    joined = late.take()
    late.close()
    late_cow = None
    for _, pid, p in joined:
        if pid == SPAWN_ENTITY:
            eid, i = read_varint(p, 0)
            kind, _ = read_varint(p, i + 16)
            if kind == tid("cow"):
                late_cow = eid
    out["late_join"] = about(joined, late_cow) if late_cow is not None else []
    return out


# ── speed ───────────────────────────────────────────────────────────────────

SPEED_CELLS = [("none", ""),
               ("speed1", "ActiveEffects:[{Id:1,Amplifier:0b,Duration:-1,ShowParticles:0b}]"),
               ("speed2", "ActiveEffects:[{Id:1,Amplifier:1b,Duration:-1,ShowParticles:0b}]"),
               ("slow1", "ActiveEffects:[{Id:2,Amplifier:0b,Duration:-1,ShowParticles:0b}]")]


def campaign_speed(server, rec: Recorder) -> dict:
    mm2.reset(server)
    field = mm2.Field(server)
    groups: dict[str, list[str]] = {}
    for c, (label, extra) in enumerate(SPEED_CELLS):
        for k in range(5):
            name = field.summon("zombie", -30.5 + k * 12.0, -30.5 + c * 16.0, extra)
            groups.setdefault(label, []).append(name)
    field.read_attributes()
    time.sleep(1.0)
    field.run(110.0)
    table = {}
    for label, names in groups.items():
        steps = []
        for n in names:
            steps += mm2.steps_of(field.mobs[n]["trace"])
        table[label] = {"plateau": mm2.plateau(steps),
                        "attribute": [field.mobs[n]["attribute"] for n in names]}
    return {"table": table, "raw": field.dump()}


# ── strength ────────────────────────────────────────────────────────────────

def health(server) -> tuple[int | None, float | None]:
    lines = server.batch(["time query gametime", f"data get entity {PROBE} Health"], timeout=30)
    tick = None
    for line in lines:
        m = GAMETIME.search(line)
        if m:
            tick = int(m.group(1))
    raw = value(lines)
    return tick, (float(raw.rstrip("f")) if raw is not None else None)


def heal(server) -> None:
    server.batch([f"effect clear {PROBE}",
                  f"effect give {PROBE} minecraft:instant_health 1 5 true"], timeout=30)
    time.sleep(0.3)
    server.batch([f"effect clear {PROBE}"], timeout=30)


def hits_of(server, summon_nbt: str, wanted: int = 4, limit: float = 25.0) -> list[dict]:
    fresh(server, "normal", "survival")
    server.batch([f"clear {PROBE}"])
    heal(server)
    server.batch([f"summon minecraft:zombie 2.0 {Y} 0.5 "
                  "{PersistenceRequired:1b,Silent:1b,Tags:[\"ovm\"]," + summon_nbt + "}"])
    _, last = health(server)
    hits = []
    swings = 0
    begin = time.monotonic()
    while len(hits) < wanted and time.monotonic() - begin < limit:
        tick, hp = health(server)
        if tick is None or hp is None:
            continue
        if last is not None and hp < last - 1e-4:
            hits.append({"tick": tick, "lost": round(last - hp, 5)})
        last = hp
        if hp < 9.0:
            heal(server)
            _, last = health(server)
    swings = len(hits)
    server.batch(["kill @e[tag=ovm]"])
    return hits if swings else []


def campaign_strength(server, rec: Recorder) -> dict:
    out = {}
    for label, nbt in (("strength1", "ActiveEffects:[{Id:5,Amplifier:0b,Duration:-1}]"),
                       ("strength2", "ActiveEffects:[{Id:5,Amplifier:1b,Duration:-1}]"),
                       ("weakness1", "ActiveEffects:[{Id:18,Amplifier:0b,Duration:-1}]"),
                       ("control", "")):
        print(f"   strength {label}", flush=True)
        out[label] = hits_of(server, nbt, limit=14.0 if label == "weakness1" else 25.0)
    return out


# ── splash ──────────────────────────────────────────────────────────────────

ROW = [0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 3.9, 4.4]


def break_potion(server, potion: str, x: float, z: float) -> None:
    # Dropped from 1.5 blocks straight down onto the grass: it breaks on the
    # block below at (x, -60, z), which the distances are measured from.
    server.batch([f"summon minecraft:potion {x} {Y + 1.5} {z} {{Item:{{id:\"minecraft:splash_potion\","
                  f"Count:1b,tag:{{Potion:\"minecraft:{potion}\"}}}},Motion:[0.0,-0.5,0.0]}}"])


def campaign_splash(server, rec: Recorder) -> dict:
    out: dict = {}
    # Cows in a row along +x from the impact, one per distance, each alone in
    # its own z lane so no body shields another.
    fresh(server)
    for i, d in enumerate(ROW):
        server.batch([f"summon minecraft:cow {0.5 + d:.2f} {Y} {0.5 + (i % 2) * 0.0:.2f} "
                      f"{{NoAI:1b,PersistenceRequired:1b,Silent:1b,Tags:[\"ovs\",\"d{i}\"]}}"])
    time.sleep(1.0)
    break_potion(server, "poison", 0.5, 0.5)
    time.sleep(0.6)
    rows = []
    for i, d in enumerate(ROW):
        lines = server.batch([f"data get entity @e[tag=d{i},limit=1] ActiveEffects",
                              f"data get entity @e[tag=d{i},limit=1] Pos"])
        rows.append({"d": d, "effects": effects_of(value(lines[:1])),
                     "pos": value(lines[1:])})
    out["poison_cows"] = rows
    # The undead way round: zombies at midnight, Harming heals, Healing hurts.
    for potion in ("harming", "healing", "strong_harming"):
        fresh(server)
        for i, d in enumerate(ROW):
            server.batch([f"summon minecraft:zombie {0.5 + d:.2f} {Y} 0.5 "
                          f"{{NoAI:1b,PersistenceRequired:1b,Silent:1b,Health:10.0f,"
                          f"Tags:[\"ovs\",\"d{i}\"]}}"])
        time.sleep(1.0)
        break_potion(server, potion, 0.5, 0.5)
        time.sleep(0.8)
        got = []
        for i, d in enumerate(ROW):
            lines = server.batch([f"data get entity @e[tag=d{i},limit=1] Health"])
            raw = value(lines)
            got.append({"d": d, "health": float(raw.rstrip("f")) if raw else None})
        out[f"{potion}_zombies"] = got
    # And a cow under Harming: hurt, the ordinary way.
    fresh(server)
    for i, d in enumerate(ROW):
        server.batch([f"summon minecraft:cow {0.5 + d:.2f} {Y} 0.5 "
                      f"{{NoAI:1b,PersistenceRequired:1b,Silent:1b,Health:10.0f,"
                      f"Attributes:[{{Name:\"minecraft:generic.max_health\",Base:40.0d}}],"
                      f"Tags:[\"ovs\",\"d{i}\"]}}"])
    time.sleep(1.0)
    break_potion(server, "harming", 0.5, 0.5)
    time.sleep(0.8)
    got = []
    for i, d in enumerate(ROW):
        raw = value(server.batch([f"data get entity @e[tag=d{i},limit=1] Health"]))
        got.append({"d": d, "health": float(raw.rstrip("f")) if raw else None})
    out["harming_cows"] = got
    return out


# ── arrow ───────────────────────────────────────────────────────────────────

def campaign_arrow(server, rec: Recorder) -> dict:
    out = {}
    # (label, entity, extra NBT, mob): the arrow falls from four blocks onto
    # the mob's head at 1.2 blocks a tick.
    for label, entity, extra, mob in (
            ("poison", "arrow", "Potion:\"minecraft:poison\",", "cow"),
            ("strong_poison", "arrow", "Potion:\"minecraft:strong_poison\",", "cow"),
            ("long_slowness", "arrow", "Potion:\"minecraft:long_slowness\",", "cow"),
            ("spectral", "spectral_arrow", "", "cow"),
            ("zombie_poison", "arrow", "Potion:\"minecraft:poison\",", "zombie")):
        fresh(server)
        server.batch([f"summon minecraft:{mob} 0.5 {Y} 0.5 {{NoAI:1b,PersistenceRequired:1b,"
                      "Silent:1b,Health:10.0f,Tags:[\"ova\"]}"])
        time.sleep(0.5)
        server.batch([f"summon minecraft:{entity} 0.5 {Y + 4} 0.5 "
                      f"{{{extra}Motion:[0.0,-1.2,0.0],pickup:0b}}"])
        time.sleep(1.2)
        raw = value(server.batch(["data get entity @e[tag=ova,limit=1] ActiveEffects"]))
        hp = value(server.batch(["data get entity @e[tag=ova,limit=1] Health"]))
        out[label] = {"effects": effects_of(raw), "health": hp}
    return out


# ── anvil ───────────────────────────────────────────────────────────────────

ZOO = [
    ("cow", "ActiveEffects:[{Id:10,Amplifier:1b,Duration:40000}]"),
    ("cow", "ActiveEffects:[{Id:1,Amplifier:2b,Duration:30000,HiddenEffect:{Id:1,Amplifier:0b,"
            "Duration:60000}}]"),
    ("zombie", "ActiveEffects:[{Id:5,Amplifier:0b,Duration:-1},{Id:12,Amplifier:0b,"
               "Duration:50000}]"),
    ("spider", "ActiveEffects:[{Id:14,Amplifier:0b,Duration:45000,ShowParticles:0b}]"),
]


def campaign_anvil(server, rec: Recorder) -> dict:
    fresh(server)
    for i, (mob, extra) in enumerate(ZOO):
        server.batch([f"summon minecraft:{mob} {4.5 + i * 3} {Y} 4.5 "
                      "{NoAI:1b,PersistenceRequired:1b,Silent:1b,Tags:[\"ovzoo\"]," + extra + "}"])
    time.sleep(2.0)
    dump = {}
    for i, (mob, _) in enumerate(ZOO):
        dump[f"{i}:{mob}"] = value(server.batch(
            [f"data get entity @e[tag=ovzoo,limit=1,sort=nearest,x={4.5 + i * 3},y={Y},z=4.5]"]))
    server.batch(["save-all flush"], timeout=120)
    time.sleep(2.0)
    if ANVIL_COPY.exists():
        shutil.rmtree(ANVIL_COPY)
    shutil.copytree(RUN / "world", ANVIL_COPY)
    return {"zoo": [m for m, _ in ZOO], "data_get": dump}


OURS_ANVIL = ROOT / ".scratch" / "hostile-anvil-ours"


def campaign_anvil_back(server, rec: Recorder) -> dict:
    """Vanilla reads back the world our server rewrote (check_hostile_e2e.py anvil)."""
    time.sleep(3.0)
    dump = {}
    for i, (mob, _) in enumerate(ZOO):
        dump[f"{i}:{mob}"] = value(server.batch(
            [f"data get entity @e[tag=ovzoo,limit=1,sort=nearest,x={4.5 + i * 3},y={Y},z=4.5,"
             f"distance=..2] ActiveEffects"]))
    return {"data_get": dump}


CAMPAIGNS = {"packets": campaign_packets, "speed": campaign_speed, "strength": campaign_strength,
             "splash": campaign_splash, "arrow": campaign_arrow, "anvil": campaign_anvil,
             "anvil_back": campaign_anvil_back}


def main(argv: list[str]) -> int:
    wanted = argv or [c for c in CAMPAIGNS if c != "anvil_back"]
    result = json.loads(OUT.read_text()) if OUT.exists() else {}
    if RUN.exists():
        shutil.rmtree(RUN)
    if wanted == ["anvil_back"]:
        if not OURS_ANVIL.exists():
            print(f"no {OURS_ANVIL}: run check_hostile_e2e.py anvil first")
            return 1
        RUN.mkdir(parents=True)
        shutil.copytree(OURS_ANVIL, RUN / "world")
        (RUN / "world" / "session.lock").unlink(missing_ok=True)
    server = Oracle(RUN, PORT)
    rec = None
    try:
        setup(server)
        rec = Recorder(PORT, PROBE)
        time.sleep(3.0)
        for name in wanted:
            print(f"== {name}", flush=True)
            started = time.monotonic()
            result[name] = CAMPAIGNS[name](server, rec)
            print(f"   {name}: {time.monotonic() - started:.0f} s", flush=True)
            OUT.write_text(json.dumps(result, indent=1, default=str))
    finally:
        if rec is not None:
            rec.close()
        server.stop()
    shutil.rmtree(RUN, ignore_errors=True)
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
