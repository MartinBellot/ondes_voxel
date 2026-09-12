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

The four species the plan brings next — the wiki's numbers are hypotheses:

  enderman  a survival probe teleported to face an enderman's eyes (the
            enderman in a pit, so it cannot wander out of the gaze) against one
            looking away: its `AngryAt`; then water poured on it.
  spider    its hits on a survival probe at noon and at midnight.
  slime     contact hits by size (0, 1, 3), and each size's jump rhythm with
            nobody to chase.
  witch     what a survival probe at 9 and at 2.5 blocks is given and loses,
            and what the witch drinks.

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

# The first run gave every cell through `ActiveEffects` in the summon NBT, and
# all four walked at the control's 0.11417: an effect read from NBT does not
# bring its modifier — vanilla keeps those in `Attributes`. So the effects are
# given by `effect give` now, and the NBT way is kept as a cell of its own.
SPEED_CELLS = [("none", None), ("speed1", ("speed", 0)), ("speed2", ("speed", 1)),
               ("slow1", ("slowness", 0)), ("speed1_nbt", "nbt")]
SPEED_NBT = "ActiveEffects:[{Id:1,Amplifier:0b,Duration:-1,ShowParticles:0b}]"


def campaign_speed(server, rec: Recorder) -> dict:
    mm2.reset(server)
    field = mm2.Field(server)
    groups: dict[str, list[str]] = {}
    for c, (label, how) in enumerate(SPEED_CELLS):
        for k in range(5):
            name = field.summon("zombie", -30.5 + k * 12.0, -30.5 + c * 16.0,
                                SPEED_NBT if how == "nbt" else "")
            groups.setdefault(label, []).append(name)
    time.sleep(1.0)
    commands = []
    for label, how in SPEED_CELLS:
        if isinstance(how, tuple):
            effect, amplifier = how
            commands += [f"effect give @e[name={n},limit=1] minecraft:{effect} infinite "
                         f"{amplifier} true" for n in groups[label]]
    server.batch(commands, timeout=120)
    # The replies themselves, so that a None below can be read for its cause.
    raw_attribute = server.batch(
        [f"attribute @e[name={groups[label][0]},limit=1] minecraft:generic.movement_speed get"
         for label, _ in SPEED_CELLS], timeout=60)
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
    return {"table": table, "raw_attribute": raw_attribute, "raw": field.dump()}


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


def hits_of(server, summon_nbt: str, wanted: int = 4, limit: float = 25.0,
            kind: str = "zombie", dx: float = 2.0, setup: list[str] | None = None,
            after: list[str] | None = None) -> list[dict]:
    fresh(server, "normal", "survival")
    server.batch([f"clear {PROBE}"] + (setup or []))
    heal(server)
    server.batch([f"summon minecraft:{kind} {0.5 + dx:.2f} {Y} 0.5 "
                  "{PersistenceRequired:1b,Silent:1b,Tags:[\"ovm\"]," + summon_nbt + "}"])
    if after:
        server.batch(after)
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
    # By `effect give`, not summon NBT: the first run gave them in NBT and all
    # four cells hit for the control's 3.0 — the modifier does not come with an
    # effect read from NBT (see SPEED_CELLS).
    out = {}
    for label, effect in (("strength1", "strength infinite 0"),
                          ("strength2", "strength infinite 1"),
                          ("weakness1", "weakness infinite 0"),
                          ("control", None)):
        print(f"   strength {label}", flush=True)
        after = [f"effect give @e[tag=ovm] minecraft:{effect} true"] if effect else None
        out[label] = hits_of(server, "", limit=14.0 if label == "weakness1" else 25.0,
                             after=after)
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
        # The first run hit no cow at all (health 10.0) and the zombie: where
        # the arrow and the mob ended up tells a miss from a refusal.
        mob_pos = value(server.batch(["data get entity @e[tag=ova,limit=1] Pos"]))
        arrow_pos = value(server.batch([f"data get entity @e[type=minecraft:{entity},limit=1] "
                                        "Pos"]))
        out[label] = {"effects": effects_of(raw), "health": hp, "mob_pos": mob_pos,
                      "arrow_pos": arrow_pos}
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
    """Vanilla reads back the world our server rewrote (check_hostile_e2e.py anvil).

    Every tagged mob, wherever it stands: our server ignores NoAI (mobs-3.md
    § 6), so the zoo walks during the e2e run — the first version looked for
    each mob within two blocks of where it was summoned and found none.
    """
    time.sleep(3.0)
    lines = server.batch(["execute as @e[tag=ovzoo] run data get entity @s"], timeout=60)
    mobs = []
    for line in lines:
        m = DATA.search(line)
        if not m:
            continue
        raw = m.group(1)
        kind = re.search(r'id: "minecraft:([a-z_]+)"', raw)
        section = raw.split("ActiveEffects:", 1)[1][:800] if "ActiveEffects:" in raw else ""
        mobs.append({"type": kind.group(1) if kind else None,
                     "effects": effects_of(section) if section else []})
    return {"mobs": mobs}


# ── The four species the plan brings next ───────────────────────────────────
#
# The wiki's numbers (enderman, slime, spider, witch pages) are the hypotheses;
# these campaigns are what decides. Each keeps its raw readings.

def gametime_of(lines: list[str]) -> int | None:
    for line in lines:
        m = GAMETIME.search(line)
        if m:
            return int(m.group(1))
    return None


def campaign_enderman(server, rec: Recorder) -> dict:
    out: dict = {}
    # The enderman in a pit two blocks deep: it cannot jump out (2.9 tall,
    # jump 1.25) and so cannot wander out of the gaze. Its eyes at Y - 2 + 2.55,
    # the probe's at Y + 1.62, eight blocks apart: the line clears the rim.
    pit = [f"fill 8 {Y - 2} 0 8 {Y - 1} 0 minecraft:air"]
    dy = (Y - 2 + 2.55) - (Y + 1.62)
    pitch = -math.degrees(math.atan2(dy, 8.0))
    for label, yaw in (("stare", -90.0), ("away", 90.0)):
        fresh(server, "normal", "survival")
        server.batch([f"clear {PROBE}", "time set midnight"] + pit)
        server.batch([f"summon minecraft:enderman 8.5 {Y - 2} 0.5 "
                      "{PersistenceRequired:1b,Silent:1b,Tags:[\"ove\"]}"])
        time.sleep(1.0)
        # A teleport with a rotation sets where the player looks, server-side.
        server.batch([f"tp {PROBE} 0.5 {Y} 0.5 {yaw} {pitch:.3f}"])
        rows = []
        for _ in range(10):
            time.sleep(0.5)
            lines = server.batch(["time query gametime",
                                  "data get entity @e[tag=ove,limit=1] AngryAt"])
            rows.append({"tick": gametime_of(lines), "angry_at": value(lines)})
        out[label] = {"pitch": pitch, "rows": rows,
                      "pos": value(server.batch(["data get entity @e[tag=ove,limit=1] Pos"]))}
    # Water on it, at midnight, in the open: health and where it went.
    fresh(server)
    server.batch(["time set midnight", f"summon minecraft:enderman 8.5 {Y} 0.5 "
                  "{PersistenceRequired:1b,Silent:1b,Tags:[\"ove\"]}"])
    time.sleep(1.0)
    server.batch([f"setblock 8 {Y} 0 minecraft:water"])
    trace = []
    begin = time.monotonic()
    while time.monotonic() - begin < 10.0:
        tick = gametime_of(server.batch(["time query gametime"]))
        hp = value(server.batch(["data get entity @e[tag=ove,limit=1] Health"]))
        pos = value(server.batch(["data get entity @e[tag=ove,limit=1] Pos"]))
        trace.append({"tick": tick, "health": hp, "pos": pos})
    out["water"] = trace
    return out


# ── enderman, second pass: what the implementation needs ────────────────────

def entity_of_type(packets, name: str) -> int | None:
    for _, pid, p in packets:
        if pid == SPAWN_ENTITY:
            eid, i = read_varint(p, 0)
            kind, _ = read_varint(p, i + 16)
            if kind == tid(name):
                return eid
    return None


def metadata_hex(packets, eid: int | None) -> list[str]:
    out = []
    for _, pid, p in packets:
        if pid == METADATA and eid is not None:
            first, _ = read_varint(p, 0)
            if first == eid:
                out.append(p.hex())
    return out


def pit(d: int) -> str:
    """A 1×1 hole two deep at (d, 0): an enderman in it cannot walk out."""
    return f"fill {d} {Y - 2} 0 {d} {Y - 1} 0 minecraft:air"


def ender_in_pit(server, d: int, extra: str = "") -> None:
    server.batch([pit(d), f"summon minecraft:enderman {d + 0.5} {Y - 2} 0.5 "
                  "{PersistenceRequired:1b,Silent:1b,Tags:[\"ove\"]" + extra + "}"])


def stare_pitch(d: float, offset: float = 0.0) -> float:
    """The pitch that looks from the probe's eyes to those of an enderman in the
    pit `d` blocks east — minus `offset` degrees, looking higher."""
    dy = (Y - 2 + 2.55) - (Y + 1.62)
    return -math.degrees(math.atan2(dy, d)) - offset


def angered(server) -> bool:
    return value(server.batch(["data get entity @e[tag=ove,limit=1] AngryAt"])) is not None


def position(server, selector: str) -> list[float] | None:
    raw = value(server.batch([f"data get entity {selector} Pos"]))
    if raw is None:
        return None
    return [float(v.strip().rstrip("d")) for v in raw.strip("[]").split(",")]


def campaign_enderman2(server, rec: Recorder) -> dict:
    out: dict = {}
    # (a) The metadata: carrying a block from the spawn, then stared at.
    fresh(server, "normal", "survival")
    server.batch([f"clear {PROBE}", "time set midnight", "gamerule mobGriefing true"])
    rec.take()
    ender_in_pit(server, 8, ",carriedBlockState:{Name:\"minecraft:grass_block\","
                            "Properties:{snowy:\"false\"}}")
    time.sleep(1.5)
    got = rec.take()
    eid = entity_of_type(got, "enderman")
    out["spawn_metadata"] = metadata_hex(got, eid)
    server.batch([f"tp {PROBE} 0.5 {Y} 0.5 -90 {stare_pitch(8):.3f}"])
    time.sleep(2.5)
    out["stared_metadata"] = metadata_hex(rec.take(), eid)
    out["carried_nbt"] = value(server.batch(["data get entity @e[tag=ove,limit=1] "
                                             "carriedBlockState"]))
    out["anger_nbt"] = value(server.batch(["data get entity @e[tag=ove,limit=1] AngerTime"]))

    # (b) The stare's cone: looking above the eyes by `offset` degrees.
    cells = []
    for d in (8, 16):
        for offset in (0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 7.0):
            fresh(server, "normal", "survival")
            server.batch(["time set midnight"])
            ender_in_pit(server, d)
            time.sleep(0.8)
            server.batch([f"tp {PROBE} 0.5 {Y} 0.5 -90 {stare_pitch(d, offset):.3f}"])
            time.sleep(1.5)
            cells.append({"d": d, "offset": offset, "angry": angered(server)})
    out["cone"] = cells

    # (c) A carved pumpkin on the head.
    fresh(server, "normal", "survival")
    server.batch(["time set midnight",
                  f"item replace entity {PROBE} armor.head with minecraft:carved_pumpkin"])
    ender_in_pit(server, 8)
    time.sleep(0.8)
    server.batch([f"tp {PROBE} 0.5 {Y} 0.5 -90 {stare_pitch(8):.3f}"])
    time.sleep(2.5)
    out["pumpkin_angry"] = angered(server)
    server.batch([f"item replace entity {PROBE} armor.head with minecraft:air"])

    # (d) Rain, in the open.
    fresh(server)
    server.batch(["time set midnight", "weather rain",
                  f"summon minecraft:enderman 8.5 {Y} 0.5 "
                  "{PersistenceRequired:1b,Silent:1b,Tags:[\"ove\"]}"])
    rain = []
    begin = time.monotonic()
    while time.monotonic() - begin < 12.0:
        tick = gametime_of(server.batch(["time query gametime"]))
        hp = value(server.batch(["data get entity @e[tag=ove,limit=1] Health"]))
        rain.append({"tick": tick, "health": hp, "pos": position(server, "@e[tag=ove,limit=1]")})
    out["rain"] = rain
    server.batch(["weather clear"])

    # (e) Where a hurt enderman lands: sixteen hits, the position around each.
    fresh(server)
    server.batch(["time set midnight", f"summon minecraft:enderman 0.5 {Y} 20.5 "
                  "{PersistenceRequired:1b,Silent:1b,Tags:[\"ove\"]}"])
    time.sleep(1.0)
    hops = []
    for _ in range(16):
        before = position(server, "@e[tag=ove,limit=1]")
        server.batch(["damage @e[tag=ove,limit=1] 1 minecraft:generic",
                      "effect give @e[tag=ove] minecraft:instant_health 1 3 true"])
        time.sleep(0.8)
        hops.append({"before": before, "after": position(server, "@e[tag=ove,limit=1]")})
    out["hurt_hops"] = hops

    # (f) Picking up and putting down: eight endermen on the grass, left alone.
    fresh(server)
    server.batch(["time set midnight", "gamerule mobGriefing true"])
    for k in range(8):
        server.batch([f"summon minecraft:enderman {20.5 + (k % 4) * 10} {Y} {20.5 + (k // 4) * 10} "
                      "{PersistenceRequired:1b,Silent:1b,Tags:[\"ovc\",\"c" + str(k) + "\"]}"])
    carry = []
    begin = time.monotonic()
    while time.monotonic() - begin < 80.0:
        tick = gametime_of(server.batch(["time query gametime"]))
        row = {"tick": tick}
        for k in range(8):
            row[k] = value(server.batch([f"data get entity @e[tag=c{k},limit=1] "
                                         "carriedBlockState.Name"]))
        carry.append(row)
        time.sleep(1.0)
    out["carry"] = carry
    return out


def carry_trace(server, count: int, seconds: float) -> list[dict]:
    rows = []
    begin = time.monotonic()
    while time.monotonic() - begin < seconds:
        row = {"tick": gametime_of(server.batch(["time query gametime"]))}
        for k in range(count):
            row[k] = value(server.batch([f"data get entity @e[tag=c{k},limit=1] "
                                         "carriedBlockState.Name"]))
        rows.append(row)
        time.sleep(1.0)
    return rows


def campaign_enderman3(server, rec: Recorder) -> dict:
    """Carrying, second attempt. `enderman2` put eight endermen on bare flat
    grass and none took anything in 80 s: the only holdable block was the one
    under their feet. Here the holdable blocks are at feet level."""
    out: dict = {}
    # Pickup: a layer of dandelions (#enderman_holdable, and walkable).
    fresh(server)
    server.batch(["time set midnight", "gamerule mobGriefing true",
                  f"fill 10 {Y} 10 60 {Y} 40 minecraft:dandelion"], timeout=60)
    for k in range(8):
        server.batch([f"summon minecraft:enderman {20.5 + (k % 4) * 10} {Y} {20.5 + (k // 4) * 10} "
                      "{PersistenceRequired:1b,Silent:1b,Tags:[\"ovc\",\"c" + str(k) + "\"]}"])
    out["pickup"] = carry_trace(server, 8, 60.0)
    server.batch([f"fill 10 {Y} 10 60 {Y} 40 minecraft:air"], timeout=60)

    # Placement: eight endermen carrying dirt, on the flat grass.
    fresh(server)
    server.batch(["time set midnight", "gamerule mobGriefing true"])
    for k in range(8):
        server.batch([f"summon minecraft:enderman {20.5 + (k % 4) * 10} {Y} {20.5 + (k // 4) * 10} "
                      "{PersistenceRequired:1b,Silent:1b,Tags:[\"ovc\",\"c" + str(k) + "\"],"
                      "carriedBlockState:{Name:\"minecraft:dirt\"}}"])
    out["place"] = carry_trace(server, 8, 100.0)
    # Where the dirt went: every dirt block standing at feet level or above.
    found = []
    for x in range(0, 80, 2):
        lines = server.batch([f"execute if block {x} {Y} {z} minecraft:dirt run say D {x} {z}"
                              for z in range(0, 70)], timeout=60)
        found += [line.split("D ", 1)[1] for line in lines if "D " in line]
    out["placed_at"] = found
    return out


def campaign_spider(server, rec: Recorder) -> dict:
    out = {}
    for when in ("noon", "midnight"):
        print(f"   spider {when}", flush=True)
        out[when] = hits_of(server, "", wanted=3, limit=14.0, kind="spider", dx=3.0,
                            setup=[f"time set {when}"])
    return out


def campaign_slime(server, rec: Recorder) -> dict:
    out: dict = {"contact": {}, "rhythm": None}
    for size in (0, 1, 3):
        print(f"   slime contact size {size}", flush=True)
        out["contact"][size] = hits_of(server, f"Size:{size}", wanted=4, limit=12.0,
                                       kind="slime", dx=1.5, setup=["time set midnight"])
    # The rhythm, with nobody to chase: the probe is in creative far away.
    mm2.reset(server)
    field = mm2.Field(server)
    for i, size in enumerate((0, 1, 3)):
        for k in range(3):
            field.summon("slime", -20.5 + k * 12.0, -20.5 + i * 16.0, f"Size:{size}")
    time.sleep(1.0)
    field.run(40.0)
    out["rhythm"] = field.dump()
    return out


def rhythm_field(server, kind: str, sizes: tuple[int, ...], per: int, seconds: float,
                 probe_mode: str = "creative") -> list[dict]:
    mm2.reset(server, probe_mode)
    field = mm2.Field(server)
    for i, size in enumerate(sizes):
        for k in range(per):
            field.summon(kind, -20.5 + k * 12.0, -20.5 + i * 16.0, f"Size:{size}")
    time.sleep(1.0)
    field.run(seconds)
    return field.dump()


def campaign_slime2(server, rec: Recorder) -> dict:
    """The slime's size on the wire, the magma cube's contact and rhythm, and a
    slime's rhythm while it chases."""
    out: dict = {}
    # (a) The size on the wire: one slime of each size, their spawn metadata.
    fresh(server)
    rec.take()
    for size, x in ((0, 4.5), (1, 8.5), (3, 14.5)):
        server.batch([f"summon minecraft:slime {x} {Y} 0.5 "
                      "{NoAI:1b,PersistenceRequired:1b,Silent:1b,Size:" + str(size) + "}"])
    time.sleep(1.5)
    got = rec.take()
    spawns = []
    for _, pid, p in got:
        if pid == SPAWN_ENTITY:
            eid, i = read_varint(p, 0)
            kind, _ = read_varint(p, i + 16)
            if kind == tid("slime"):
                spawns.append(eid)
    out["size_metadata"] = {eid: metadata_hex(got, eid) for eid in spawns}

    # (b) The magma cube's contact, size by size, on Normal.
    out["magma_contact"] = {}
    for size in (0, 1, 3):
        print(f"   magma contact size {size}", flush=True)
        out["magma_contact"][size] = hits_of(server, f"Size:{size}", wanted=4, limit=12.0,
                                             kind="magma_cube", dx=1.5,
                                             setup=["time set midnight"])
    # (c) The magma cube's rhythm, nobody to chase.
    out["magma_rhythm"] = rhythm_field(server, "magma_cube", (0, 1, 3), 3, 40.0)

    # (d) A slime's rhythm while it chases a survival probe out of reach.
    mm2.reset(server, "survival")
    server.batch(["difficulty normal", f"tp {PROBE} 0.5 {Y} 0.5",
                  f"effect give {PROBE} minecraft:resistance 999999 4 true",
                  f"effect give {PROBE} minecraft:regeneration 999999 4 true"])
    field = mm2.Field(server)
    for k in range(4):
        field.summon("slime", 12.5 + k * 3.0, 12.5, "Size:1")
    time.sleep(0.5)
    field.run(30.0)
    out["chase_rhythm"] = field.dump()
    return out


def campaign_witch(server, rec: Recorder) -> dict:
    out = {}
    for label, d in (("far", 9.0), ("near", 2.5)):
        print(f"   witch {label}", flush=True)
        fresh(server, "normal", "survival")
        server.batch([f"clear {PROBE}", "time set midnight"])
        heal(server)
        server.batch([f"summon minecraft:witch {0.5 + d:.2f} {Y} 0.5 "
                      "{PersistenceRequired:1b,Silent:1b,Tags:[\"ovw\"]}"])
        rows = []
        begin = time.monotonic()
        while time.monotonic() - begin < 24.0:
            tick = gametime_of(server.batch(["time query gametime"]))
            mine = effects_of(value(server.batch([f"data get entity {PROBE} ActiveEffects"])))
            hp = value(server.batch([f"data get entity {PROBE} Health"]))
            hers = effects_of(value(server.batch(["data get entity @e[tag=ovw,limit=1] "
                                                  "ActiveEffects"])))
            rows.append({"tick": tick, "probe_effects": mine, "health": hp,
                         "witch_effects": hers})
            if hp is not None and float(hp.rstrip("f")) < 8.0:
                # Healing only: clearing the effects would change her choice.
                server.batch([f"effect give {PROBE} minecraft:instant_health 1 5 true"])
        out[label] = rows
    return out


CAMPAIGNS = {"packets": campaign_packets, "speed": campaign_speed, "strength": campaign_strength,
             "splash": campaign_splash, "arrow": campaign_arrow, "anvil": campaign_anvil,
             "enderman": campaign_enderman, "enderman2": campaign_enderman2,
             "enderman3": campaign_enderman3,
             "spider": campaign_spider, "slime": campaign_slime, "slime2": campaign_slime2,
             "witch": campaign_witch, "anvil_back": campaign_anvil_back}


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
            try:
                result[name] = CAMPAIGNS[name](server, rec)
            except Exception as error:  # one campaign's mistake must not cost the others
                result[name] = {"error": repr(error)}
                print(f"   {name} failed: {error!r}", flush=True)
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
