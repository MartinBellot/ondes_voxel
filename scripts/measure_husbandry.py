#!/usr/bin/env python3
"""Ask a real 1.20.1 server how its farm animals grow, breed and are worked.

Every number the husbandry code carries comes out of one of these campaigns,
run against `tools/vanilla/server.jar` with a probe client connected — the
probe is the only way to *interact* with an entity (Interact, 0x10), and the
only way to see what the wire says about it (Spawn Entity, Set Entity
Metadata, Entity Event, Spawn Experience Orb).

  meta      Which metadata index says "baby", "sheared", "colour", "saddled".
            One NBT field at a time against a baseline of the same species,
            as the zombie table in ov/protocol/entity.hpp was built.
  box       A baby's hitbox and eye height (bisection, as measure_entities.py),
            and its movement_speed attribute.
  growth    How fast Age climbs, with and without NoAI, and what the wire says
            when a baby becomes an adult.
  feed      What feeding does: to a baby (how much of the remaining time), to
            an adult (InLove), to an adult already in love, to one cooling down.
  food      Which items each species takes. 1.20.1 has no `#*_food` item tags
            for these animals, so the list is measured, item by item.
  love      Two cows fed by the probe: the Entity Event status, InLove, the time
            to a birth, the calf's Age, the parents' Age after, the XP orb and
            the `animals_bred` statistic.
  radius    Two cows in love in glass lanes, at a range of gaps. Kept because
            it FAILED: no pair in a lane bred at any gap (docs/provenance/
            elevage.md § 1.3). `pair` found the set-ups that do breed, and
            `reach` measured the radius on open grass.
  pair      Five set-ups at four blocks, to find out why `radius` bred nothing.
  reach     The partner radius: pairs on open grass, 6 to 10 blocks apart.
  xp        Sixty pairs bred at once: the orb value distribution, parent Age.
  inherit   256 sheep pairs, every ordered pair of colours: the lamb's colour.
  shear     Sheep sheared one by one: wool count distribution, Sheared, the
            shears' wear; a baby, an already-sheared sheep, a coloured one.
  regrow    Sheared sheep on grass, alone in pens: how long until they eat,
            and what the grass becomes.
  eggs      EggLayTime of freshly summoned chickens; what it becomes after an
            egg; whether a chick lays.
  hatch     Thrown eggs landing: how many chickens, and at what Age.
  tempt     The probe holding food: from how far does an animal come, how
            fast, and where does it stop? Adult and baby.
  work      Milk (bucket on cow, calf, a stack of two buckets), saddle on a
            pig and a piglet, dye on a sheep (and the same dye again).

Usage: python3 scripts/measure_husbandry.py [campaign ...]

Writes data/vanilla/1.20.1/normalized/husbandry.json (gitignored), merging into
what is already there so campaigns can be run one at a time.
"""
from __future__ import annotations

import json
import math
import re
import shutil
import socket
import struct
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_entity_packets import Probe, read_varint, varint  # noqa: E402
from measure_mobs import FlatServer  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
OUT = NORMALIZED / "husbandry.json"
RUN = ROOT / "run" / "husbandry-oracle"
PORT = 25611

Y = -60  # the superflat's standing height; grass is at -61

DATA = re.compile(r"has the following entity data: (.*)$")
GAMETIME = re.compile(r"The time is (\d+)")
TEST = re.compile(r"Test (passed|failed)")
SCORE = re.compile(r"has (-?\d+) \[")
POS = re.compile(r"\[([-0-9.Ee]+)d?, ([-0-9.Ee]+)d?, ([-0-9.Ee]+)d?\]")

COLOURS = ["white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
           "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"]

TYPE_IDS = {"cow": 18, "sheep": 82, "pig": 72, "chicken": 15, "item": 54,
            "experience_orb": 34, "rabbit": 79}


def uuid_of(n: int) -> str:
    """The selector string for a UUID summoned as [I;0x4F56,0,0,n]."""
    return f"00004f56-0000-0000-0000-{n:012x}"


def uuid_nbt(n: int) -> str:
    return f"UUID:[I;{0x4F56},0,0,{n}]"


# ── Metadata ────────────────────────────────────────────────────────────────

def parse_metadata(payload: bytes, i: int) -> list[tuple[int, int, object]]:
    """(index, type, value) until 0xFF. Only the types a farm animal sends."""
    out = []
    while i < len(payload):
        index = payload[i]
        i += 1
        if index == 0xFF:
            break
        kind, i = read_varint(payload, i)
        if kind == 0:
            value = struct.unpack_from(">b", payload, i)[0]; i += 1
        elif kind in (1, 12, 14, 15, 20, 21, 22, 24, 25):
            value, i = read_varint(payload, i)
        elif kind == 2:
            value, i = read_varint(payload, i)
        elif kind == 3:
            value = struct.unpack_from(">f", payload, i)[0]; i += 4
        elif kind == 8:
            value = payload[i] != 0; i += 1
        elif kind == 7:
            present = payload[i]; i += 1
            if present:
                item, i = read_varint(payload, i)
                count = payload[i]; i += 1
                if payload[i] != 0:
                    return out + [(index, kind, ("nbt", item, count))]
                i += 1
                value = (item, count)
            else:
                value = None
        elif kind == 13:
            present = payload[i]; i += 1
            value = payload[i:i + 16].hex() if present else None
            i += 16 if present else 0
        elif kind == 19:
            value, i = read_varint(payload, i)
        elif kind == 6:
            present = payload[i]; i += 1
            if present:
                n, i = read_varint(payload, i)
                value = payload[i:i + n].decode(); i += n
            else:
                value = None
        else:
            return out + [(index, kind, "undecoded")]
        out.append((index, kind, value))
    return out


class Hand(Probe):
    """A probe that keeps reading on a thread and can act on entities.

    Sends go through a lock: the reader answers keep-alives and teleports from
    its own thread while the campaign sends Interact from the main one.
    """

    def __init__(self, port: int, name: str = "ovhand") -> None:
        self.lock = threading.Lock()
        super().__init__(port, name)
        self.state_lock = threading.Lock()
        self.spawns: dict[int, dict] = {}        # eid -> {type, uuid_lsb, t}
        self.by_lsb: dict[int, int] = {}         # uuid lsb -> eid (our summons)
        self.metadata: list[tuple[float, int, list]] = []
        self.events: list[tuple[float, int, int]] = []
        self.orbs: list[tuple[float, int, int, tuple]] = []
        self.clock: tuple[int, float] | None = None
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def send(self, packet_id: int, payload: bytes) -> None:
        with self.lock:
            super().send(packet_id, payload)

    def _run(self) -> None:
        self.socket.settimeout(0.5)
        while not self._stop.is_set():
            try:
                pid, p = self.read()
            except (socket.timeout, TimeoutError):
                continue
            except Exception:
                return
            now = time.monotonic()
            if pid == 0x23:
                self.send(0x12, p[:8])
            elif pid == 0x3C:
                x, y, z = struct.unpack_from(">ddd", p, 0)
                self.position = (x, y, z)
                tid, _ = read_varint(p, 33)
                self.send(0x00, varint(tid))
                self.send(0x14, struct.pack(">ddd", x, y, z) + bytes([1]))
            elif pid == 0x5E and len(p) >= 8:
                self.clock = (struct.unpack_from(">q", p, 0)[0], now)
            elif pid == 0x01:
                eid, i = read_varint(p, 0)
                msb, lsb = struct.unpack_from(">QQ", p, i)
                i += 16
                etype, i = read_varint(p, i)
                x, y, z = struct.unpack_from(">ddd", p, i)
                with self.state_lock:
                    self.spawns[eid] = {"type": etype, "lsb": lsb, "msb": msb,
                                        "t": now, "pos": (x, y, z)}
                    if msb == (0x4F56 << 32):
                        self.by_lsb[lsb] = eid
            elif pid == 0x02:
                eid, i = read_varint(p, 0)
                x, y, z = struct.unpack_from(">ddd", p, i)
                value = struct.unpack_from(">h", p, i + 24)[0]
                with self.state_lock:
                    self.orbs.append((now, eid, value, (x, y, z)))
            elif pid == 0x52:
                eid, i = read_varint(p, 0)
                try:
                    fields = parse_metadata(p, i)
                except Exception:
                    fields = [("unparsed", p[i:].hex())]
                with self.state_lock:
                    self.metadata.append((now, eid, fields))
            elif pid == 0x1C and len(p) >= 5:
                eid = struct.unpack_from(">i", p, 0)[0]
                status = struct.unpack_from(">b", p, 4)[0]
                with self.state_lock:
                    self.events.append((now, eid, status))

    def eid_for(self, n: int, timeout: float = 5.0) -> int | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self.state_lock:
                if n in self.by_lsb:
                    return self.by_lsb[n]
            time.sleep(0.02)
        return None

    def interact(self, eid: int, sneaking: bool = False) -> None:
        self.send(0x10, varint(eid) + varint(0) + varint(0) + bytes([1 if sneaking else 0]))
        self.send(0x2F, varint(0))

    def metadata_of(self, eid: int) -> list[tuple[float, list]]:
        with self.state_lock:
            return [(t, f) for t, e, f in self.metadata if e == eid]

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2.0)
        try:
            self.socket.close()
        except Exception:
            pass


# ── Console helpers ─────────────────────────────────────────────────────────

class Rig:
    def __init__(self, server, hand: Hand) -> None:
        self.server = server
        self.hand = hand
        self.next_uuid = 1

    def uuid(self) -> int:
        self.next_uuid += 1
        return self.next_uuid

    def data(self, target: str, path: str = "") -> str | None:
        lines = self.server.batch([f"data get entity {target} {path}".rstrip()], timeout=60)
        for line in lines:
            m = DATA.search(line)
            if m:
                return m.group(1)
        return None

    def datas(self, queries: list[tuple[str, str]]) -> list[str | None]:
        """Many `data get` at once; a missing answer is None, in order."""
        commands = []
        for i, (target, path) in enumerate(queries):
            commands.append(f"say ovq{i}")
            commands.append(f"data get entity {target} {path}".rstrip())
        lines = self.server.batch(commands, timeout=300)
        out: list[str | None] = [None] * len(queries)
        current = -1
        for line in lines:
            m = re.search(r"\[Server\] ovq(\d+)$", line)
            if m:
                current = int(m.group(1))
                continue
            d = DATA.search(line)
            if d and current >= 0:
                out[current] = d.group(1)
        return out

    def gametime(self) -> int:
        lines = self.server.batch(["time query gametime"])
        for line in lines:
            m = GAMETIME.search(line)
            if m:
                return int(m.group(1))
        raise RuntimeError("no gametime")

    def count(self, selector: str) -> int:
        lines = self.server.batch([
            "scoreboard players set ovc ovcount 0",
            f"execute store result score ovc ovcount if entity {selector}",
            "scoreboard players get ovc ovcount"])
        for line in lines:
            m = SCORE.search(line)
            if m:
                return int(m.group(1))
        raise RuntimeError(f"no count for {selector}: {lines[-3:]}")

    def hold(self, item: str, count: int = 1) -> None:
        self.server.batch([f"item replace entity ovhand weapon.mainhand with {item} {count}"])

    def held(self) -> tuple[str, int] | None:
        raw = self.data("ovhand", "SelectedItem")
        if raw is None:
            return None
        item = re.search(r'id: "([^"]+)"', raw)
        count = re.search(r"Count: (\d+)b", raw)
        return (item.group(1) if item else "?", int(count.group(1)) if count else 0)

    def summon_near(self, kind: str, nbt: str, at: tuple[float, float, float] | None = None
                    ) -> tuple[int, int]:
        """Summon one entity by the probe; return (uuid number, wire id)."""
        n = self.uuid()
        x, y, z = at or (1.5, Y, 0.5)
        extra = f",{nbt}" if nbt else ""
        self.server.batch([f"summon minecraft:{kind} {x} {y} {z} {{{uuid_nbt(n)}{extra}}}"])
        eid = self.hand.eid_for(n)
        if eid is None:
            raise RuntimeError(f"the probe never saw {kind} {n}")
        return n, eid

    def kill(self, n: int) -> None:
        self.server.batch([f"kill {uuid_of(n)}"])

    def pens(self, x0: int, z0: int, cols: int, rows: int, w: int, d: int,
             sx: int, sz: int, block: str = "minecraft:glass") -> list[tuple[int, int]]:
        """A grid of pens, walls two high; returns each interior's min corner."""
        corners = []
        commands = []
        for r in range(rows):
            for c in range(cols):
                x = x0 + c * sx
                z = z0 + r * sz
                commands.append(f"fill {x - 1} {Y} {z - 1} {x + w} {Y + 1} {z + d} {block}")
                commands.append(f"fill {x} {Y} {z} {x + w - 1} {Y + 1} {z + d - 1} minecraft:air")
                corners.append((x, z))
        for start in range(0, len(commands), 200):
            self.server.batch(commands[start:start + 200])
        return corners

    def clear(self, x0: int, z0: int, x1: int, z1: int) -> None:
        """Air above the grass, in strips small enough for /fill."""
        commands = []
        for x in range(x0, x1 + 1, 32):
            commands.append(f"fill {x} {Y} {z0} {min(x + 31, x1)} {Y + 3} {z1} minecraft:air")
        self.server.batch(commands)


def nbt_int(raw: str | None) -> int | None:
    if raw is None:
        return None
    m = re.match(r"(-?\d+)[bsL]?$", raw.strip())
    return int(m.group(1)) if m else None


def pos_of(raw: str | None) -> tuple[float, float, float] | None:
    if raw is None:
        return None
    m = POS.search(raw)
    return tuple(float(g) for g in m.groups()) if m else None  # type: ignore


# ── Campaigns ───────────────────────────────────────────────────────────────

def campaign_meta(rig: Rig) -> dict:
    """One NBT field at a time, against a baseline of the same species."""
    cases = [
        ("cow", "NoAI:1b,Silent:1b"),
        ("cow", "NoAI:1b,Silent:1b,Age:-24000"),
        ("sheep", "NoAI:1b,Silent:1b,Color:0b"),
        ("sheep", "NoAI:1b,Silent:1b,Color:14b"),
        ("sheep", "NoAI:1b,Silent:1b,Color:0b,Sheared:1b"),
        ("sheep", "NoAI:1b,Silent:1b,Color:0b,Age:-24000"),
        ("pig", "NoAI:1b,Silent:1b"),
        ("pig", "NoAI:1b,Silent:1b,Saddle:1b"),
        ("pig", "NoAI:1b,Silent:1b,Age:-24000"),
        ("chicken", "NoAI:1b,Silent:1b"),
        ("chicken", "NoAI:1b,Silent:1b,Age:-24000"),
        ("cow", "NoAI:1b,Silent:1b,InLove:600"),
    ]
    out = []
    for kind, nbt in cases:
        n, eid = rig.summon_near(kind, nbt, (3.5, Y, 3.5))
        time.sleep(0.6)
        fields = [f for _, f in rig.hand.metadata_of(eid)]
        out.append({"type": kind, "nbt": nbt,
                    "fields": [[list(map(str, fl)) for fl in f] for f in fields]})
        print(f"  {kind:8} {nbt:45} {fields}")
        rig.kill(n)
    return {"cases": out}


def campaign_box(rig: Rig) -> dict:
    from measure_entities import bisect_edges
    out = {}
    kinds = ["cow", "sheep", "pig", "chicken"]
    probes = []
    placed = {}
    for i, kind in enumerate(kinds):
        for baby, age in ((False, 0), (True, -24000)):
            x, z = 40.5 + 8 * i, 40.5 + (8 if baby else 0)
            tag = f"ovbox{i}{int(baby)}"
            rig.server.batch([f'summon minecraft:{kind} {x} {Y + 1} {z} '
                              f'{{NoAI:1b,NoGravity:1b,Silent:1b,Age:{age},Tags:["{tag}"]}}'])
            placed[tag] = (kind, baby, x, z)
            sel = f"@e[tag={tag},dx=0,dy=0,dz=0,limit=1]"
            probes.append((sel, x, Y + 1, z, 0, 0.0, 8.0))
            probes.append((sel, x, Y + 1, z, 1, 0.0, 8.0))
    time.sleep(1.0)
    edges = bisect_edges(rig.server, probes)
    offset = -1.4901161193847656e-08  # entities.json probe_offset
    rig.server.batch(["kill @e[type=minecraft:marker]"])
    tags = list(placed)
    rig.server.batch([f'execute as @e[tag={t},limit=1] at @s anchored eyes positioned ^ ^ ^ '
                      f'run summon minecraft:marker ~ ~ ~ {{Tags:["{t}eye"]}}' for t in tags])
    eyes = rig.datas([(f"@e[tag={t}eye,limit=1]", "Pos") for t in tags])
    speeds = []
    lines = rig.server.batch([f"attribute @e[tag={t},limit=1] minecraft:generic.movement_speed get"
                              for t in tags])
    for line in lines:
        m = re.search(r"is ([-0-9.Ee]+)$", line)
        if m:
            speeds.append(float(m.group(1)))
    for k, t in enumerate(tags):
        kind, baby, x, z = placed[t]
        eye = pos_of(eyes[k])
        out.setdefault(kind, {})["baby" if baby else "adult"] = {
            "width": round((edges[2 * k] - offset) * 2.0, 6),
            "height": round(edges[2 * k + 1] - offset, 6),
            "eye_height": round(eye[1] - (Y + 1), 6) if eye else None,
            "movement_speed": speeds[k] if k < len(speeds) else None,
        }
        print(f"  {kind:8} {'baby ' if baby else 'adult'} {out[kind]['baby' if baby else 'adult']}")
    rig.server.batch(["kill @e[type=minecraft:marker]"] +
                     [f"kill @e[tag={t}]" for t in tags])
    return out


def campaign_growth(rig: Rig) -> dict:
    """Age per tick, with AI and without, and the wire at adulthood."""
    corners = rig.pens(-40, 20, 8, 1, 1, 1, 3, 3)
    ids = []
    for k, (x, z) in enumerate(corners):
        noai = k >= 4
        n, eid = rig.summon_near("cow", f"Age:-200{',NoAI:1b' if noai else ''},Silent:1b",
                                 (x + 0.5, Y, z + 0.5))
        ids.append((n, eid, noai))
    samples = []
    for _ in range(3):
        queries = [(uuid_of(n), "Age") for n, _, _ in ids]
        t0 = rig.gametime()
        ages = rig.datas(queries)
        t1 = rig.gametime()
        samples.append({"t": [t0, t1], "ages": [nbt_int(a) for a in ages]})
        time.sleep(2.0)
    time.sleep(10.0)  # past zero
    final = [nbt_int(a) for a in rig.datas([(uuid_of(n), "Age") for n, _, _ in ids])]
    adult_meta = {}
    for n, eid, noai in ids:
        adult_meta[str(eid)] = [[str(x) for x in f] for _, f in rig.hand.metadata_of(eid)]
    for n, _, _ in ids:
        rig.kill(n)
    print(f"  samples {samples}")
    print(f"  final {final}")
    return {"noai": [noai for _, _, noai in ids], "samples": samples, "final": final,
            "metadata": adult_meta}


def feed(rig: Rig, kind: str, nbt: str, item: str, count: int = 2,
         read: tuple[str, ...] = ("Age", "InLove")) -> dict:
    n, eid = rig.summon_near(kind, nbt)
    rig.hold(item, count)
    time.sleep(0.2)
    before = rig.datas([(uuid_of(n), p) for p in read])
    rig.hand.interact(eid)
    time.sleep(0.4)
    after = rig.datas([(uuid_of(n), p) for p in read])
    held = rig.held()
    rig.kill(n)
    return {"type": kind, "nbt": nbt, "item": item,
            "before": dict(zip(read, before)), "after": dict(zip(read, after)),
            "held_after": held, "consumed": held is None or held[1] < count, "eid": eid}


def campaign_feed(rig: Rig) -> dict:
    out = []
    for age in (-24000, -23999, -20000, -12000, -6000, -2001, -2000, -1999, -401, -400, -399,
                -200, -100, -40, -39, -21, -20, -19, -10, -1):
        r = feed(rig, "cow", f"NoAI:1b,Silent:1b,Age:{age}", "minecraft:wheat")
        out.append(r)
        print(f"  baby {age:6} -> {r['after']['Age']}  consumed {r['consumed']}")
    for label, nbt in (("adult", "Age:0"), ("in love", "Age:0,InLove:300"),
                       ("cooling", "Age:3000"), ("cooling 1", "Age:1")):
        r = feed(rig, "cow", f"NoAI:1b,Silent:1b,{nbt}", "minecraft:wheat")
        r["label"] = label
        out.append(r)
        print(f"  {label:9} {r['before']} -> {r['after']}  consumed {r['consumed']}")
    return {"cases": out}


FOOD_CANDIDATES = [
    "minecraft:wheat", "minecraft:wheat_seeds", "minecraft:melon_seeds",
    "minecraft:pumpkin_seeds", "minecraft:beetroot_seeds", "minecraft:torchflower_seeds",
    "minecraft:pitcher_pod", "minecraft:carrot", "minecraft:potato", "minecraft:beetroot",
    "minecraft:golden_carrot", "minecraft:apple", "minecraft:dandelion", "minecraft:hay_block",
    "minecraft:sugar", "minecraft:bread", "minecraft:sweet_berries", "minecraft:poisonous_potato",
]


def campaign_food(rig: Rig) -> dict:
    out = {}
    for kind in ("cow", "sheep", "pig", "chicken", "rabbit"):
        taken = []
        for item in FOOD_CANDIDATES:
            r = feed(rig, kind, "NoAI:1b,Silent:1b,Age:0", item, read=("InLove",))
            if nbt_int(r["after"]["InLove"]) and r["consumed"]:
                taken.append(item)
        out[kind] = taken
        print(f"  {kind:8} {taken}")
    return out


def campaign_love(rig: Rig) -> dict:
    """The probe feeds two cows penned beside it. Five times."""
    rig.server.batch(["scoreboard objectives add ovbred minecraft.custom:minecraft.animals_bred"])
    trials = []
    for trial in range(5):
        rig.clear(-3, -3, 6, 6)
        rig.pens(1, 0, 1, 1, 2, 2, 1, 1)
        a, ea = rig.summon_near("cow", "Silent:1b,Age:0", (1.5, Y, 0.5))
        b, eb = rig.summon_near("cow", "Silent:1b,Age:0", (2.5, Y, 1.5))
        rig.hold("minecraft:wheat", 8)
        with rig.hand.state_lock:
            ev0 = len(rig.hand.events)
            orb0 = len(rig.hand.orbs)
        rig.hand.interact(ea)
        rig.hand.interact(eb)
        t_fed = rig.gametime()
        love = rig.datas([(uuid_of(a), "InLove"), (uuid_of(b), "InLove")])
        birth = None
        for _ in range(200):
            c = rig.count("@e[type=minecraft:cow,x=0,y=-61,z=-1,dx=4,dy=3,dz=4,nbt={Age:-24000}]")
            c2 = rig.count("@e[type=minecraft:cow,x=0,y=-61,z=-1,dx=4,dy=3,dz=4]")
            if c2 >= 3:
                birth = rig.gametime()
                break
        calf = rig.datas([("@e[type=minecraft:cow,x=0,y=-61,z=-1,dx=4,dy=3,dz=4,"
                           "nbt=!{UUID:[I;20310,0,0," + str(a) + "]},"
                           "nbt=!{UUID:[I;20310,0,0," + str(b) + "]},limit=1]", "Age"),
                          (uuid_of(a), "Age"), (uuid_of(b), "Age"),
                          (uuid_of(a), "InLove"), (uuid_of(b), "InLove")])
        stat = rig.server.batch(["scoreboard players get ovhand ovbred"])
        time.sleep(0.5)
        with rig.hand.state_lock:
            events = [(e, s) for _, e, s in rig.hand.events[ev0:] if e in (ea, eb)]
            orbs = [v for _, _, v, _ in rig.hand.orbs[orb0:]]
        r = {"fed_at": t_fed, "love_after_feed": love, "birth_at": birth,
             "calf_age": calf[0], "parent_age": calf[1:3], "parent_love": calf[3:5],
             "events": events, "orbs": orbs,
             "stat": [m.group(1) for line in stat for m in [SCORE.search(line)] if m],
             "held": rig.held()}
        trials.append(r)
        print(f"  trial {trial}: {r}")
        rig.server.batch(["kill @e[type=minecraft:cow]", "kill @e[type=minecraft:experience_orb]"])
    return {"trials": trials}


def campaign_radius(rig: Rig) -> dict:
    """Two cows in love in a three-block-wide lane, `gap` apart along x.

    The first version used a one-block corridor, and no cow in love moved at
    all, not even at four blocks: a 0.9-wide mob does not path along a lane
    exactly one block wide. Three wide, and the pair stands on the middle row.
    """
    gaps = [6.0, 7.5, 8.0, 8.5, 8.8, 9.0, 9.5, 10.0, 12.0]
    reps = 3
    rig.clear(-70, -70, -10, 70)
    rows = []
    x0, z0 = -64, -64
    commands = []
    k = 0
    for gi, gap in enumerate(gaps):
        for rep in range(reps + 1):  # the last one is the control: not in love
            z = z0 + k * 5
            commands.append(f"fill {x0 - 1} {Y} {z - 2} {x0 + 16} {Y + 1} {z + 2} minecraft:glass")
            commands.append(f"fill {x0} {Y} {z - 1} {x0 + 15} {Y + 1} {z + 1} minecraft:air")
            rows.append({"gap": gap, "control": rep == reps, "z": z})
            k += 1
    for s in range(0, len(commands), 100):
        rig.server.batch(commands[s:s + 100])
    ids = []
    commands = []
    for row in rows:
        love = "" if row["control"] else ",InLove:600"
        a, b = rig.uuid(), rig.uuid()
        xa = x0 + 2.0
        xb = xa + row["gap"]
        commands.append(f"summon minecraft:cow {xa} {Y} {row['z'] + 0.5} "
                        f"{{{uuid_nbt(a)},Silent:1b,Rotation:[0f,0f]{love}}}")
        commands.append(f"summon minecraft:cow {xb} {Y} {row['z'] + 0.5} "
                        f"{{{uuid_nbt(b)},Silent:1b,Rotation:[0f,0f]{love}}}")
        ids.append((a, b))
    rig.server.batch(commands)
    t0 = rig.gametime()
    first = rig.datas([(uuid_of(n), "Pos") for ab in ids for n in ab])
    time.sleep(3.0)
    t1 = rig.gametime()
    second = rig.datas([(uuid_of(n), "Pos") for ab in ids for n in ab])
    time.sleep(27.0)
    t2 = rig.gametime()
    babies = []
    for row in rows:
        babies.append(rig.count(f"@e[type=minecraft:cow,x={x0},y=-61,z={row['z'] - 1},"
                                f"dx=16,dy=3,dz=2]") - 2)
    out = []
    for k, row in enumerate(rows):
        pa0, pb0 = pos_of(first[2 * k]), pos_of(first[2 * k + 1])
        pa1, pb1 = pos_of(second[2 * k]), pos_of(second[2 * k + 1])
        closing = None
        if pa0 and pb0 and pa1 and pb1:
            closing = (pb0[0] - pa0[0]) - (pb1[0] - pa1[0])
        out.append({**row, "closing": closing, "babies_after_30s": babies[k]})
    print(f"  ticks {t1 - t0} / {t2 - t0}")
    for gap in gaps:
        sel = [r for r in out if r["gap"] == gap]
        print(f"  gap {gap:5}: love closing {[round(r['closing'] or 0, 2) for r in sel if not r['control']]}"
              f" babies {[r['babies_after_30s'] for r in sel if not r['control']]}"
              f" | control {[round(r['closing'] or 0, 2) for r in sel if r['control']]}"
              f" {[r['babies_after_30s'] for r in sel if r['control']]}")
    rig.server.batch(["kill @e[type=minecraft:cow]", "kill @e[type=minecraft:experience_orb]"])
    return {"window": [t1 - t0, t2 - t0], "rows": out}


def campaign_reach(rig: Rig) -> dict:
    """The radius question again, on the set-up `pair` proved breeds.

    Open grass, no walls, each pair on a row of its own fourteen blocks from
    the next, all within sixty blocks of the probe. `radius` (lanes, further
    out) bred nothing at any gap, and `pair` bred on every set-up at four
    blocks — so the lanes were the fault, not the love.
    """
    gaps = [6.0, 7.0, 7.5, 8.0, 8.5, 9.0, 9.5, 10.0]
    reps = 2
    rig.clear(-50, -60, 30, 60)
    rows = []
    commands = []
    k = 0
    for gap in gaps:
        for rep in range(reps):
            x = -45 if k % 2 == 0 else 5
            z = -56 + (k // 2) * 14
            a, b = rig.uuid(), rig.uuid()
            commands.append(f"summon minecraft:cow {x + 0.5} {Y} {z + 0.5} "
                            f"{{{uuid_nbt(a)},Silent:1b,InLove:600}}")
            commands.append(f"summon minecraft:cow {x + 0.5 + gap} {Y} {z + 0.5} "
                            f"{{{uuid_nbt(b)},Silent:1b,InLove:600}}")
            rows.append({"gap": gap, "x": x, "z": z, "ids": (a, b)})
            k += 1
    rig.server.batch(commands)
    first = rig.datas([(uuid_of(n), "Pos") for r in rows for n in r["ids"]])
    time.sleep(4.0)
    second = rig.datas([(uuid_of(n), "Pos") for r in rows for n in r["ids"]])
    time.sleep(16.0)
    out = []
    for i, r in enumerate(rows):
        pa0, pb0 = pos_of(first[2 * i]), pos_of(first[2 * i + 1])
        pa1, pb1 = pos_of(second[2 * i]), pos_of(second[2 * i + 1])
        closing = ((pb0[0] - pa0[0]) - (pb1[0] - pa1[0])
                   if pa0 and pb0 and pa1 and pb1 else None)
        calves = rig.count(f"@e[type=minecraft:cow,x={r['x'] - 4},y=-61,z={r['z'] - 6},"
                           f"dx=20,dy=3,dz=12,nbt={{Age:0}}]")
        adults = rig.count(f"@e[type=minecraft:cow,x={r['x'] - 4},y=-61,z={r['z'] - 6},"
                           f"dx=20,dy=3,dz=12]")
        out.append({"gap": r["gap"], "closing_80": closing, "cows_in_box": adults,
                    "age0_in_box": calves})
    for gap in gaps:
        sel = [o for o in out if o["gap"] == gap]
        print(f"  gap {gap:5}: closing {[round(o['closing_80'] or 0, 2) for o in sel]}"
              f" cows in box {[o['cows_in_box'] for o in sel]}")
    rig.server.batch(["kill @e[type=minecraft:cow]", "kill @e[type=minecraft:experience_orb]"])
    return {"rows": out}


def campaign_pair(rig: Rig) -> dict:
    """Why did the radius lanes breed nothing? Five set-ups, four blocks apart.

    A open grass, InLove from NBT · B three-wide glass lane, InLove from NBT ·
    C open grass, fed by the probe · D the xp campaign's 2x1 pen, adjacent ·
    E open grass, InLove from NBT and nothing else in the NBT.
    """
    rig.clear(-40, 60, 40, 100)
    setups = []
    commands = []
    base = {"A": (-30, 70), "B": (-10, 70), "C": (10, 70), "D": (-30, 85), "E": (-10, 85)}
    for label, (x, z) in base.items():
        a, b = rig.uuid(), rig.uuid()
        gap = 1.0 if label == "D" else 4.0
        if label == "B":
            commands.append(f"fill {x - 1} {Y} {z - 2} {x + 8} {Y + 1} {z + 2} minecraft:glass")
            commands.append(f"fill {x} {Y} {z - 1} {x + 7} {Y + 1} {z + 1} minecraft:air")
        if label == "D":
            commands.append(f"fill {x - 1} {Y} {z - 1} {x + 2} {Y + 1} {z + 1} minecraft:glass")
            commands.append(f"fill {x} {Y} {z} {x + 1} {Y + 1} {z} minecraft:air")
        love = "" if label == "C" else ",InLove:600"
        extra = "" if label == "E" else ",Silent:1b"
        commands.append(f"summon minecraft:cow {x + 0.5} {Y} {z + 0.5} {{{uuid_nbt(a)}{extra}{love}}}")
        commands.append(f"summon minecraft:cow {x + 0.5 + gap} {Y} {z + 0.5} {{{uuid_nbt(b)}{extra}{love}}}")
        setups.append((label, a, b, x, z))
    rig.server.batch(commands)
    # C: the probe walks over and feeds both.
    _, a, b, x, z = setups[2]
    rig.server.batch([f"tp ovhand {x + 2.5} {Y} {z + 2.5}"])
    time.sleep(1.0)
    rig.hold("minecraft:wheat", 4)
    for n in (a, b):
        eid = rig.hand.eid_for(n)
        if eid is not None:
            rig.hand.interact(eid)
    first = rig.datas([(uuid_of(n), "Pos") for s in setups for n in s[1:3]])
    loves = rig.datas([(uuid_of(n), "InLove") for s in setups for n in s[1:3]])
    time.sleep(4.0)
    second = rig.datas([(uuid_of(n), "Pos") for s in setups for n in s[1:3]])
    time.sleep(16.0)
    out = {}
    for k, (label, a, b, x, z) in enumerate(setups):
        pa0, pb0 = pos_of(first[2 * k]), pos_of(first[2 * k + 1])
        pa1, pb1 = pos_of(second[2 * k]), pos_of(second[2 * k + 1])
        closing = (pb0[0] - pa0[0]) - (pb1[0] - pa1[0]) if pa0 and pb0 and pa1 and pb1 else None
        calves = rig.count(f"@e[type=minecraft:cow,x={x - 2},y=-61,z={z - 3},dx=12,dy=3,dz=6]") - 2
        out[label] = {"closing_80_ticks": closing, "calves_20s": calves,
                      "love": loves[2 * k:2 * k + 2]}
        print(f"  {label}: {out[label]}")
    rig.server.batch(["kill @e[type=minecraft:cow]", "kill @e[type=minecraft:experience_orb]",
                      "tp ovhand -0.5 -60 0.5"])
    return out


def campaign_xp(rig: Rig) -> dict:
    rig.clear(10, -70, 60, -10)
    corners = rig.pens(12, -66, 10, 6, 2, 1, 4, 3)
    ids = []
    commands = []
    for x, z in corners:
        a, b = rig.uuid(), rig.uuid()
        commands.append(f"summon minecraft:cow {x + 0.5} {Y} {z + 0.5} {{{uuid_nbt(a)},Silent:1b,InLove:600}}")
        commands.append(f"summon minecraft:cow {x + 1.5} {Y} {z + 0.5} {{{uuid_nbt(b)},Silent:1b,InLove:600}}")
        ids.append((a, b))
    with rig.hand.state_lock:
        orb0 = len(rig.hand.orbs)
    rig.server.batch(commands)
    t0 = rig.gametime()
    time.sleep(15.0)
    t1 = rig.gametime()
    parent_ages = [nbt_int(v) for v in rig.datas([(uuid_of(n), "Age") for ab in ids for n in ab])]
    total = rig.count("@e[type=minecraft:cow]")
    babies = rig.count("@e[type=minecraft:cow,nbt={Age:-24000}]") if False else total - 2 * len(ids)
    baby_ages_raw = rig.server.batch(["execute as @e[type=minecraft:cow] if data entity @s {Age:0} run say x"])
    with rig.hand.state_lock:
        orbs = [v for _, _, v, _ in rig.hand.orbs[orb0:]]
    hist = {v: orbs.count(v) for v in range(0, 10)}
    print(f"  {len(ids)} pairs, {babies} calves in {t1 - t0} ticks, orbs {len(orbs)}: {hist}")
    print(f"  parent ages {sorted(set(parent_ages), key=lambda v: (v is None, v))}")
    rig.server.batch(["kill @e[type=minecraft:cow]", "kill @e[type=minecraft:experience_orb]"])
    return {"pairs": len(ids), "calves": babies, "ticks": t1 - t0, "orbs": orbs,
            "parent_ages": parent_ages}


def campaign_inherit(rig: Rig) -> dict:
    rig.clear(-70, 50, 10, 110)
    corners = rig.pens(-66, 52, 16, 16, 2, 1, 4, 3)
    commands = []
    pairs = []
    k = 0
    for ca in range(16):
        for cb in range(16):
            x, z = corners[k]
            k += 1
            commands.append(f"summon minecraft:sheep {x + 0.5} {Y} {z + 0.5} "
                            f"{{Silent:1b,InLove:600,Color:{ca}b,Tags:[\"ovpar\"]}}")
            commands.append(f"summon minecraft:sheep {x + 1.5} {Y} {z + 0.5} "
                            f"{{Silent:1b,InLove:600,Color:{cb}b,Tags:[\"ovpar\"]}}")
            pairs.append((ca, cb, x, z))
    for s in range(0, len(commands), 128):
        rig.server.batch(commands[s:s + 128])
    time.sleep(20.0)
    queries = [(f"@e[type=minecraft:sheep,tag=!ovpar,x={x},y=-61,z={z},dx=1,dy=3,dz=0,limit=1]",
                "Color") for _, _, x, z in pairs]
    colours = rig.datas(queries)
    out = []
    for (ca, cb, _, _), c in zip(pairs, colours):
        out.append([ca, cb, nbt_int(c)])
    missing = sum(1 for r in out if r[2] is None)
    print(f"  {len(out)} pairs, {missing} without a lamb")
    rig.server.batch(["kill @e[type=minecraft:sheep]", "kill @e[type=minecraft:experience_orb]"])
    return {"pairs": out}


def campaign_shear(rig: Rig) -> dict:
    wool_ids = {}
    reg = json.load(open(ROOT / "data/vanilla/1.20.1/generated/reports/registries.json"))
    items = {v["protocol_id"]: k for k, v in reg["minecraft:item"]["entries"].items()}
    counts = []
    rig.hold("minecraft:shears", 1)
    for trial in range(150):
        if trial % 100 == 0:
            rig.hold("minecraft:shears", 1)
        n, eid = rig.summon_near("sheep", "NoAI:1b,Silent:1b,Color:0b")
        with rig.hand.state_lock:
            s0 = set(rig.hand.spawns)
        rig.hand.interact(eid)
        time.sleep(0.35)
        with rig.hand.state_lock:
            new = [e for e in rig.hand.spawns if e not in s0 and rig.hand.spawns[e]["type"] == 54]
        wool = 0
        names = set()
        for e in new:
            # The latest stack an item carries, once: a dropped item's metadata
            # arrives twice, and summing every packet counted each wool double.
            latest = None
            for _, fields in rig.hand.metadata_of(e):
                for index, kind, value in fields:
                    if index == 8 and isinstance(value, tuple):
                        latest = value
            if latest is not None:
                wool += latest[1]
                names.add(items.get(latest[0]))
        counts.append(wool)
        rig.server.batch([f"kill {uuid_of(n)}", "kill @e[type=minecraft:item]"])
        if trial == 0:
            print(f"  first: {wool} x {names}, sheep meta {rig.hand.metadata_of(eid)[-1:]}")
    shears = rig.data("ovhand", "SelectedItem")
    hist = {v: counts.count(v) for v in range(0, 5)}
    print(f"  {len(counts)} shorn: {hist}; shears after 50 since last reset: {shears}")
    extra = {}
    for label, nbt in (("baby", "Age:-24000"), ("sheared", "Sheared:1b"), ("red", "Color:14b")):
        rig.hold("minecraft:shears", 1)
        n, eid = rig.summon_near("sheep", f"NoAI:1b,Silent:1b,{nbt}")
        with rig.hand.state_lock:
            s0 = set(rig.hand.spawns)
        rig.hand.interact(eid)
        time.sleep(0.4)
        with rig.hand.state_lock:
            new = [e for e in rig.hand.spawns if e not in s0 and rig.hand.spawns[e]["type"] == 54]
        got = []
        for e in new:
            for _, fields in rig.hand.metadata_of(e):
                for index, kind, value in fields:
                    if index == 8 and isinstance(value, tuple):
                        got.append((items.get(value[0]), value[1]))
        extra[label] = {"drops": got, "sheared": rig.data(uuid_of(n), "Sheared"),
                        "held": rig.held()}
        print(f"  {label}: {extra[label]}")
        rig.server.batch([f"kill {uuid_of(n)}", "kill @e[type=minecraft:item]"])
    return {"counts": counts, "shears_after": shears, "cases": extra}


def campaign_regrow(rig: Rig) -> dict:
    rig.clear(60, 20, 120, 60)
    corners = rig.pens(62, 22, 10, 5, 1, 1, 3, 3)
    ids = []
    commands = []
    for k, (x, z) in enumerate(corners):
        n = rig.uuid()
        # The last ten are lambs: they eat more often, and eating is said to
        # age them. Their Age is read at the end.
        age = -24000 if k >= 40 else 0
        commands.append(f"summon minecraft:sheep {x + 0.5} {Y} {z + 0.5} "
                        f"{{{uuid_nbt(n)},Silent:1b,Color:0b,Sheared:1b,Age:{age}}}")
        ids.append((n, x, z))
    with rig.hand.state_lock:
        ev0 = len(rig.hand.events)
    rig.server.batch(commands)
    t0 = rig.gametime()
    series = []
    for _ in range(25):
        time.sleep(5.0)
        t = rig.gametime()
        sheared = rig.datas([(uuid_of(n), "Sheared") for n, _, _ in ids])
        lines = rig.server.batch([f"execute if block {x} {Y - 1} {z} minecraft:grass_block"
                                  for _, x, z in ids])
        grass = [m.group(1) == "passed" for line in lines for m in [TEST.search(line)] if m]
        series.append({"t": t - t0, "sheared": [nbt_int(s) for s in sheared], "grass": grass})
        print(f"  t={t - t0:5}: unshorn {sum(1 for s in sheared if nbt_int(s) == 0)}/{len(ids)}"
              f"  grass eaten {sum(1 for g in grass if not g)}")
    below = rig.server.batch([f"execute if block {x} {Y - 1} {z} minecraft:dirt" for _, x, z in ids])
    dirt = sum(1 for line in below for m in [TEST.search(line)] if m and m.group(1) == "passed")
    t_end = rig.gametime()
    lamb_ages = [nbt_int(v) for v in rig.datas([(uuid_of(n), "Age") for n, _, _ in ids[40:]])]
    eids = {rig.hand.by_lsb.get(n): k for k, (n, _, _) in enumerate(ids)}
    with rig.hand.state_lock:
        events = [(round(t, 2), eids[e], s) for t, e, s in rig.hand.events[ev0:] if e in eids]
    print(f"  lamb ages at +{t_end - t0}: {lamb_ages}")
    print(f"  entity events: {sorted({s for _, _, s in events})}, {len(events)} in all")
    rig.server.batch(["kill @e[type=minecraft:sheep]"])
    return {"series": series, "dirt_at_end": dirt, "n": len(ids), "lambs_from": 40,
            "lamb_ages": lamb_ages, "elapsed": t_end - t0, "events": events}


def campaign_eggs(rig: Rig) -> dict:
    rig.clear(-70, -130, 10, -70)
    commands = []
    ids = []
    for k in range(200):
        n = rig.uuid()
        commands.append(f"summon minecraft:chicken {-64 + (k % 20) * 3 + 0.5} {Y} {-124 + (k // 20) * 3 + 0.5} "
                        f"{{{uuid_nbt(n)},NoAI:1b,Silent:1b}}")
        ids.append(n)
    rig.server.batch(commands)
    initial = [nbt_int(v) for v in rig.datas([(uuid_of(n), "EggLayTime") for n in ids])]
    rig.server.batch(["kill @e[type=minecraft:chicken]"])
    # Chickens about to lay: with and without AI, adult and chick.
    corners = rig.pens(-64, -124, 10, 6, 1, 1, 3, 3)
    lay = []
    commands = []
    for k, (x, z) in enumerate(corners):
        n = rig.uuid()
        kind = ["ai", "noai", "chick"][k % 3]
        extra = {"ai": "", "noai": ",NoAI:1b", "chick": ",Age:-24000"}[kind]
        commands.append(f"summon minecraft:chicken {x + 0.5} {Y} {z + 0.5} "
                        f"{{{uuid_nbt(n)},Silent:1b,EggLayTime:20{extra}}}")
        lay.append((n, kind, x, z))
    rig.server.batch(commands)
    t0 = rig.gametime()
    time.sleep(3.0)
    t1 = rig.gametime()
    after = [nbt_int(v) for v in rig.datas([(uuid_of(n), "EggLayTime") for n, _, _, _ in lay])]
    eggs = []
    for n, kind, x, z in lay:
        eggs.append(rig.count(f"@e[type=minecraft:item,x={x},y=-61,z={z},dx=0,dy=3,dz=0,"
                              f"nbt={{Item:{{id:\"minecraft:egg\"}}}}]"))
    rig.server.batch(["kill @e[type=minecraft:chicken]", "kill @e[type=minecraft:item]"])
    res = {"initial": initial, "elapsed": t1 - t0,
           "after": [{"kind": k, "egg_lay_time": a, "eggs": e}
                     for (_, k, _, _), a, e in zip(lay, after, eggs)]}
    vals = [v for v in initial if v is not None]
    print(f"  initial EggLayTime: n={len(vals)} min {min(vals)} max {max(vals)} "
          f"mean {sum(vals) / len(vals):.0f}")
    for kind in ("ai", "noai", "chick"):
        rows = [r for r in res["after"] if r["kind"] == kind]
        print(f"  {kind:6}: eggs {[r['eggs'] for r in rows]} time {[r['egg_lay_time'] for r in rows]}")
    return res


def campaign_hatch(rig: Rig) -> dict:
    rig.clear(20, 70, 90, 120)
    rig.server.batch(["kill @e[type=minecraft:chicken]"])
    commands = []
    for k in range(400):
        x = 22 + (k % 20) * 3 + 0.5
        z = 72 + (k // 20) * 2 + 0.5
        commands.append(f"summon minecraft:egg {x} {Y + 3} {z} {{Motion:[0.0d,-0.5d,0.0d]}}")
    for s in range(0, len(commands), 100):
        rig.server.batch(commands[s:s + 100])
    time.sleep(3.0)
    total = rig.count("@e[type=minecraft:chicken]")
    ages = []
    lines = rig.server.batch(["execute as @e[type=minecraft:chicken] run data get entity @s Age"])
    for line in lines:
        m = DATA.search(line)
        if m:
            ages.append(nbt_int(m.group(1)))
    rig.server.batch(["kill @e[type=minecraft:chicken]"])
    print(f"  400 eggs -> {total} chickens, ages {sorted(set(ages))}")
    return {"eggs": 400, "chickens": total, "ages": ages}


def campaign_tempt(rig: Rig) -> dict:
    """The probe at (0.5, Y, 0.5) holding food; one animal at a time."""
    rig.clear(-24, -24, 24, 24)
    rig.server.batch(["tp ovhand 0.5 -60 0.5 0 0"])
    time.sleep(1.0)
    out = []
    plan = []
    for d in (4, 6, 8, 9, 10, 11, 12, 14):
        for rep in range(2):
            plan.append(("cow", "minecraft:wheat", d, 0, rep))
    plan.append(("cow", "minecraft:air", 6, 0, 0))
    plan.append(("cow", "minecraft:air", 6, 0, 1))
    for kind, food in (("cow", "minecraft:wheat"), ("sheep", "minecraft:wheat"),
                       ("pig", "minecraft:carrot"), ("chicken", "minecraft:wheat_seeds")):
        plan.append((kind, food, 9, 0, 0))
        plan.append((kind, food, 9, -24000, 0))
    for kind, food, d, age, rep in plan:
        if food == "minecraft:air":
            rig.server.batch(["item replace entity ovhand weapon.mainhand with minecraft:air"])
        else:
            rig.hold(food, 1)
        angle = (rep * 90 + d * 37) % 360
        x = 0.5 + d * math.cos(math.radians(angle))
        z = 0.5 + d * math.sin(math.radians(angle))
        n = rig.uuid()
        rig.server.batch([f"summon minecraft:{kind} {x:.3f} {Y} {z:.3f} "
                          f"{{{uuid_nbt(n)},Silent:1b,Age:{age}}}"])
        trace = []
        start = time.monotonic()
        while time.monotonic() - start < 7.0:
            lines = rig.server.batch(["time query gametime", f"data get entity {uuid_of(n)} Pos"])
            t = next((int(m.group(1)) for l in lines for m in [GAMETIME.search(l)] if m), None)
            p = next((pos_of(m.group(1)) for l in lines for m in [DATA.search(l)] if m), None)
            if t is not None and p is not None:
                trace.append([t, p[0], p[1], p[2]])
        rig.kill(n)
        dist = [math.hypot(p[1] - 0.5, p[3] - 0.5) for p in trace]
        steps = []
        for a, b in zip(trace, trace[1:]):
            dt = b[0] - a[0]
            if dt > 0:
                steps.append(math.hypot(b[1] - a[1], b[3] - a[3]) / dt)
        moving = sorted(s for s in steps if s > 0.02)
        median = moving[len(moving) // 2] if moving else 0.0
        row = {"type": kind, "food": food, "gap": d, "age": age, "start": dist[0] if dist else None,
               "end": dist[-1] if dist else None, "median_speed": median, "trace": trace}
        out.append(row)
        print(f"  {kind:7} {food:22} gap {d:4} age {age:6}: {row['start']:.2f} -> "
              f"{row['end']:.2f}, median moving speed {median:.4f}")
    return {"runs": out}


def campaign_work(rig: Rig) -> dict:
    out = {}
    out["milk"] = feed(rig, "cow", "NoAI:1b,Silent:1b", "minecraft:bucket", 1, read=("Age",))
    out["milk_calf"] = feed(rig, "cow", "NoAI:1b,Silent:1b,Age:-24000", "minecraft:bucket", 1,
                            read=("Age",))
    out["milk_two"] = feed(rig, "cow", "NoAI:1b,Silent:1b", "minecraft:bucket", 2, read=("Age",))
    out["milk_two_inventory"] = rig.data("ovhand", "Inventory")
    rig.server.batch(["clear ovhand"])
    out["saddle"] = feed(rig, "pig", "NoAI:1b,Silent:1b", "minecraft:saddle", 1, read=("Saddle",))
    out["saddle_piglet"] = feed(rig, "pig", "NoAI:1b,Silent:1b,Age:-24000", "minecraft:saddle", 1,
                                read=("Saddle",))
    out["saddle_meta"] = [[str(x) for x in f] for _, f in rig.hand.metadata_of(out["saddle"]["eid"])]
    out["dye"] = feed(rig, "sheep", "NoAI:1b,Silent:1b,Color:0b", "minecraft:red_dye", 2,
                      read=("Color",))
    out["dye_same"] = feed(rig, "sheep", "NoAI:1b,Silent:1b,Color:14b", "minecraft:red_dye", 2,
                           read=("Color",))
    out["dye_lamb"] = feed(rig, "sheep", "NoAI:1b,Silent:1b,Color:0b,Age:-24000",
                           "minecraft:blue_dye", 2, read=("Color",))
    out["dye_sheared"] = feed(rig, "sheep", "NoAI:1b,Silent:1b,Color:0b,Sheared:1b",
                              "minecraft:blue_dye", 2, read=("Color",))
    for k, v in out.items():
        print(f"  {k}: {v}")
    return out


CAMPAIGNS = {
    "meta": campaign_meta, "box": campaign_box, "growth": campaign_growth,
    "feed": campaign_feed, "food": campaign_food, "love": campaign_love,
    "radius": campaign_radius, "pair": campaign_pair, "reach": campaign_reach, "xp": campaign_xp, "inherit": campaign_inherit,
    "shear": campaign_shear, "regrow": campaign_regrow, "eggs": campaign_eggs,
    "hatch": campaign_hatch, "tempt": campaign_tempt, "work": campaign_work,
}


def main(argv: list[str]) -> int:
    names = argv or list(CAMPAIGNS)
    results = json.loads(OUT.read_text()) if OUT.exists() else {}
    if RUN.exists():
        shutil.rmtree(RUN)
    server = FlatServer(RUN, port=PORT)
    hand = None
    try:
        server.batch(["gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                      "gamerule doWeatherCycle false", "gamerule sendCommandFeedback true",
                      "gamerule doInsomnia false", "gamerule doPatrolSpawning false",
                      "gamerule doTraderSpawning false", "difficulty peaceful",
                      "time set noon", "setworldspawn 0 -60 0",
                      "scoreboard objectives add ovcount dummy",
                      "forceload add -128 -144 127 127"])
        time.sleep(5.0)
        hand = Hand(PORT, "ovhand")
        time.sleep(2.0)
        rig = Rig(server, hand)
        server.batch(["tp ovhand -0.5 -60 0.5 -90 0", "gamemode survival ovhand",
                      "effect give ovhand minecraft:resistance infinite 5 true",
                      "kill @e[type=!minecraft:player]"])
        time.sleep(1.0)
        for name in names:
            print(f"── {name}", flush=True)
            started = time.monotonic()
            results[name] = CAMPAIGNS[name](rig)
            results[name]["_seconds"] = round(time.monotonic() - started, 1)
            OUT.write_text(json.dumps(results, indent=1))
    finally:
        if hand is not None:
            hand.close()
        server.stop()
        shutil.rmtree(RUN, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
