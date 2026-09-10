#!/usr/bin/env python3
"""Ask a real 1.20.1 server how arrows, snowballs, eggs, pearls and tridents fly.

`docs/provenance/mobs.md` fitted the mob motion model and found that an arrow
does not follow it (residual 2.6e-03 against 1.6e-06). This script measures the
model an arrow *does* follow, and everything a projectile does around it.

Nothing is fitted to a trajectory as a whole. A witness — a primed TNT with a
long fuse and no gravity, tagged like the projectiles and read **in the same
command** — labels every sample with the tick it was taken on, so consecutive
samples give one-tick transitions (v_n -> v_{n+1}, p_n -> p_{n+1}) directly.
Gravity, drag and their order fall out of a linear regression per axis; the
birth tick of a projectile never enters the calculation.

Scenarios (each starts and stops its own server on OV_PROJ_PORT, default
25631, in run/projectile-oracle/<scenario>/, and deletes its world on leaving):

  flight     Arrow, snowball, egg, ender pearl, trident, experience bottle in
             the air; arrow, snowball and trident in a pool of still water.
  ground     An arrow that lands: where it rests, `inGround`, `life`, and the
             tick it disappears.
  damage     Arrows summoned with a known Motion one tenth of a block from a
             cow's (inflated) box, so the hit happens on the first tick at a
             known speed; the cow's Health is read back. Crits, a non-default
             `damage`, a trident, a snowball on a blaze and on a cow.
  bow        A probe player in survival draws a bow for t ticks and lets go:
             the arrow's Motion, crit, damage and pickup as the server wrote
             them, the Spawn Entity packet (type, data, velocity), the arrows
             left in the inventory and the bow's Damage. Then Infinity,
             creative, range at pitch 0, and the pickup.
  crossbow   The same probe with a crossbow: the charge threshold, the stored
             projectile, the shot's speed and flags.
  eggs       Eggs broken one per glass cell; chickens counted per cell.
  pearl      The probe throws an ender pearl in survival: where it ends up and
             what it cost.
  skeleton   A skeleton shooting at the probe, on easy, normal and hard: the
             interval between arrows and their speed and spread.
  metadata   One NBT field at a time on a summoned arrow / trident / snowball,
             against a baseline, and which metadata index moved.

Usage:
    python3 scripts/measure_projectiles.py <scenario> [<scenario>...]

Writes data/vanilla/1.20.1/normalized/projectile_<scenario>.json (gitignored).
"""
from __future__ import annotations

import json
import math
import os
import re
import shutil
import statistics
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_entity_packets import Probe, read_varint, varint  # noqa: E402
from measure_entities import Server  # noqa: E402
from measure_redstone import fill, forceload, freeze  # noqa: E402

NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
RUN = ROOT / "run" / "projectile-oracle"
PORT = int(os.environ.get("OV_PROJ_PORT", "25631"))

with open(NORMALIZED / "registries.json") as _f:
    _REGISTRIES = json.load(_f)["registries"]
ENTITY_TYPES: list[str] = _REGISTRIES["minecraft:entity_type"]["entries"]
ITEMS: list[str] = _REGISTRIES["minecraft:item"]["entries"]

FLOOR_Y = -60          # the superflat's walking surface
EYE = 1.62

FUSE = re.compile(r"\bFuse: (-?\d+)s")
MOTION = re.compile(r"Motion: \[([-0-9.Ee]+)d, ([-0-9.Ee]+)d, ([-0-9.Ee]+)d\]")
POS = re.compile(r"Pos: \[([-0-9.Ee]+)d, ([-0-9.Ee]+)d, ([-0-9.Ee]+)d\]")
TAGS = re.compile(r"Tags: \[([^\]]*)\]")
IN_GROUND = re.compile(r"inGround: ([01])b")
LIFE = re.compile(r"\blife: (-?\d+)s")
HEALTH = re.compile(r"Health: ([-0-9.Ee]+)f")
GAMETIME = re.compile(r"time is (\d+)")
# The name has spaces in it for some types ("Primed TNT", "Thrown Ender Pearl").
ENTITY_LINE = re.compile(r"^(.*?) has the following entity data: (.*)$")


class SmallServer(Server):
    """A gigabyte of heap: four agents share eight."""

    HEAP = "-Xmx1G"


def start(name: str) -> Server:
    directory = RUN / name
    if directory.exists():
        shutil.rmtree(directory)
    server = SmallServer(directory, port=PORT)
    freeze(server)
    server.batch(["gamerule sendCommandFeedback true", "gamerule doMobLoot false",
                  "gamerule mobGriefing false"])
    return server


def finish(server: Server, name: str) -> None:
    server.stop()
    shutil.rmtree(RUN / name, ignore_errors=True)


def write(name: str, document: dict) -> Path:
    out = NORMALIZED / f"projectile_{name}.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w") as f:
        json.dump(document, f, indent=1, sort_keys=True)
    print(f"wrote {out}")
    return out


def parse_entity(line: str) -> dict | None:
    match = ENTITY_LINE.search(line)
    if match is None:
        return None
    data = match.group(2)
    out: dict = {"raw": data}
    tags = TAGS.search(data)
    out["tags"] = [t.strip().strip('"') for t in tags.group(1).split(",")] if tags else []
    for key, pattern in (("motion", MOTION), ("pos", POS)):
        found = pattern.search(data)
        if found:
            out[key] = [float(found.group(i)) for i in (1, 2, 3)]
    for key, pattern in (("fuse", FUSE), ("life", LIFE)):
        found = pattern.search(data)
        if found:
            out[key] = int(found.group(1))
    found = IN_GROUND.search(data)
    if found:
        out["in_ground"] = found.group(1) == "1"
    found = HEALTH.search(data)
    if found:
        out["health"] = float(found.group(1))
    return out


def poll(server: Server, selector: str) -> list[dict]:
    lines = server.batch([f"execute as {selector} run data get entity @s"])
    return [e for e in (parse_entity(line) for line in lines) if e is not None]


def gametime(server: Server) -> int:
    for line in server.batch(["time query gametime"]):
        match = GAMETIME.search(line)
        if match:
            return int(match.group(1))
    raise RuntimeError("no gametime")


def wait_ticks(server: Server, count: int) -> None:
    begin = gametime(server)
    deadline = time.monotonic() + 120.0
    while time.monotonic() < deadline:
        if gametime(server) - begin >= count:
            return
        time.sleep(0.05)
    raise TimeoutError("the clock stopped")


def tag_of(entity: dict, prefix: str) -> str | None:
    for tag in entity["tags"]:
        if tag.startswith(prefix):
            return tag
    return None


def nbt_vec(v: tuple[float, float, float]) -> str:
    return "[" + ",".join(f"{c!r}d" for c in v) + "]"


# ── the probe, with hands ───────────────────────────────────────────────────

SB_CONFIRM_TELEPORT = 0x00
SB_KEEP_ALIVE = 0x12
SB_POSITION = 0x14
SB_POSITION_ROTATION = 0x15
SB_PLAYER_ACTION = 0x1D
SB_SET_HELD_ITEM = 0x28
SB_USE_ITEM = 0x32

CB_SPAWN_ENTITY = 0x01
CB_LOGIN = 0x28
CB_SYNC_POSITION = 0x3C
CB_KEEP_ALIVE = 0x23
CB_TAKE_ITEM = 0x67


class Archer(Probe):
    """A client that stands, looks, draws and lets go."""

    def __init__(self, port: int, name: str = "ovarcher") -> None:
        super().__init__(port, name=name)
        self.entity_id = 0
        self.sequence = 1
        self.stamped: list[tuple[float, int, bytes]] = []

    def pump(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.socket.settimeout(max(0.001, deadline - time.monotonic()))
            try:
                packet_id, payload = self.read()
            except (TimeoutError, OSError):
                return
            except EOFError:
                return
            self.captured.append((packet_id, payload))
            self.stamped.append((time.monotonic(), packet_id, payload))
            if packet_id == CB_KEEP_ALIVE:
                self.send(SB_KEEP_ALIVE, payload[:8])
            elif packet_id == CB_SYNC_POSITION:
                x, y, z = struct.unpack_from(">ddd", payload, 0)
                self.position = (x, y, z)
                teleport_id, _ = read_varint(payload, 33)
                self.send(SB_CONFIRM_TELEPORT, varint(teleport_id))
                self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([1]))
            elif packet_id == CB_LOGIN:
                self.entity_id = struct.unpack_from(">i", payload, 0)[0]

    def look(self, x: float, y: float, z: float, yaw: float, pitch: float) -> None:
        self.position = (x, y, z)
        self.send(SB_POSITION_ROTATION,
                  struct.pack(">dddff", x, y, z, yaw, pitch) + bytes([1]))

    def hold(self, slot: int) -> None:
        self.send(SB_SET_HELD_ITEM, struct.pack(">h", slot))

    def use(self) -> None:
        self.sequence += 1
        self.send(SB_USE_ITEM, varint(0) + varint(self.sequence))

    def release(self) -> None:
        # Player Action status 5, "release use item": position zero, face down.
        self.sequence += 1
        self.send(SB_PLAYER_ACTION, varint(5) + struct.pack(">Q", 0) + bytes([0])
                  + varint(self.sequence))


def spawn_fields(payload: bytes) -> dict:
    entity_id, i = read_varint(payload, 0)
    i += 16
    type_id, i = read_varint(payload, i)
    x, y, z = struct.unpack_from(">ddd", payload, i)
    i += 24
    pitch, yaw, head = payload[i], payload[i + 1], payload[i + 2]
    i += 3
    data, i = read_varint(payload, i)
    vx, vy, vz = struct.unpack_from(">hhh", payload, i)
    return {"entity_id": entity_id, "type": ENTITY_TYPES[type_id] if type_id < len(ENTITY_TYPES)
            else type_id, "pos": [x, y, z], "data": data, "angles": [pitch, yaw, head],
            "velocity_raw": [vx, vy, vz]}


def metadata_fields(payload: bytes) -> tuple[int, list[dict]]:
    entity_id, i = read_varint(payload, 0)
    fields = []
    while i < len(payload):
        index = payload[i]
        i += 1
        if index == 0xFF:
            break
        kind, i = read_varint(payload, i)
        if kind == 0:
            value = payload[i]
            i += 1
        elif kind in (1, 14, 20):
            value, i = read_varint(payload, i)
        elif kind == 3:
            value = struct.unpack_from(">f", payload, i)[0]
            i += 4
        elif kind == 8:
            value = bool(payload[i])
            i += 1
        elif kind == 13:
            present = payload[i]
            i += 1
            value = payload[i:i + 16].hex() if present else None
            i += 16 if present else 0
        elif kind == 7:
            present = payload[i]
            i += 1
            if present:
                item_id, i = read_varint(payload, i)
                count = payload[i]
                i += 1
                value = {"item": ITEMS[item_id] if item_id < len(ITEMS) else item_id,
                         "count": count, "rest": payload[i:].hex()}
                fields.append({"index": index, "type": kind, "value": value})
                break
            value = None
        else:
            fields.append({"index": index, "type": kind, "value": None,
                           "rest": payload[i:].hex()})
            break
        fields.append({"index": index, "type": kind, "value": value})
    return entity_id, fields


# ── scenario: flight ────────────────────────────────────────────────────────

AIR_Y = 150.0
V0 = (0.6, 0.4, -0.3)
AIR_SHOTS = {
    "arrow": "minecraft:arrow",
    "snowball": "minecraft:snowball",
    "egg": "minecraft:egg",
    "pearl": "minecraft:ender_pearl",
    "trident": "minecraft:trident",
    "xpbottle": "minecraft:experience_bottle",
    "potion": "minecraft:potion",
}
WATER_SHOTS = {
    "warrow": ("minecraft:arrow", (0.6, 0.0, 0.1), -35.5, 105.5),
    "wsnowball": ("minecraft:snowball", (0.6, 0.0, 0.1), -31.5, 105.5),
    "wtrident": ("minecraft:trident", (0.6, 0.0, 0.1), -39.5, 109.0),
}


def fit_transitions(samples: dict[int, dict]) -> dict:
    """Consecutive ages only. Returns per-axis regressions and both orders."""
    ages = sorted(samples)
    pairs = [(samples[a], samples[a + 1]) for a in ages if a + 1 in samples]
    pairs = [(p, q) for p, q in pairs
             if not p.get("in_ground") and not q.get("in_ground")
             and "motion" in p and "motion" in q]
    out: dict = {"pairs": len(pairs)}
    if len(pairs) < 3:
        return out
    ratios = {axis: [q["motion"][axis] / p["motion"][axis] for p, q in pairs
                     if abs(p["motion"][axis]) > 1e-9] for axis in (0, 2)}
    for axis, name in ((0, "x"), (2, "z")):
        if ratios[axis]:
            out[f"drag_{name}"] = statistics.fmean(ratios[axis])
            out[f"drag_{name}_spread"] = max(ratios[axis]) - min(ratios[axis])
    # v'_y = a·v_y + b: least squares.
    xs = [p["motion"][1] for p, _ in pairs]
    ys = [q["motion"][1] for _, q in pairs]
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx > 0:
        a = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
        b = my - a * mx
        out["drag_y"] = a
        out["intercept_y"] = b
        out["residual_y"] = max(abs(y - (a * x + b)) for x, y in zip(xs, ys))
        # gravity after drag: b = -g; before drag: b = -a·g.
        out["g_if_after_drag"] = -b
        out["g_if_before_drag"] = -b / a if a else None
    # Which velocity moves the position: the one before the tick's drag, or after?
    before = max(abs(q["pos"][k] - p["pos"][k] - p["motion"][k])
                 for p, q in pairs for k in range(3))
    after = max(abs(q["pos"][k] - p["pos"][k] - q["motion"][k])
                for p, q in pairs for k in range(3))
    out["move_with_old_velocity_residual"] = before
    out["move_with_new_velocity_residual"] = after
    out["transitions"] = [{"v": p["motion"], "v_next": q["motion"], "p": p["pos"],
                           "p_next": q["pos"]} for p, q in pairs[:12]]
    return out


def measure_flight(_trials: int) -> None:
    server = start("flight")
    document: dict = {"v0": V0}
    try:
        forceload(server, -16, -64, 80, 96)
        # A still pool in the sky, walled in glass so nothing flows out.
        fill(server, -2, 99, -42, 44, 111, -27, "minecraft:glass")
        fill(server, -1, 100, -41, 43, 110, -28, "minecraft:water")
        probe = Archer(PORT)
        probe.pump(2.0)
        server.batch(["tp ovarcher 0 -60 60", "gamemode spectator ovarcher"])
        probe.pump(1.0)
        commands = ["kill @e[type=!player]",
                    'summon tnt 0.5 200 60.5 {Fuse:30000s,NoGravity:1b,Tags:["m","clock"]}']
        for k, (name, kind) in enumerate(AIR_SHOTS.items()):
            z = 8.0 * k + 0.5
            extra = ',Item:{id:"minecraft:splash_potion",Count:1b}' if name == "potion" else ""
            commands.append(f'summon {kind} 0.5 {AIR_Y} {z} '
                            f'{{Motion:{nbt_vec(V0)},Tags:["m","s_{name}"]{extra}}}')
        for name, (kind, v, z, y) in WATER_SHOTS.items():
            commands.append(f'summon {kind} 0.5 {y} {z} {{Motion:{nbt_vec(v)},Tags:["m","s_{name}"]}}')
        server.batch(commands)
        samples: dict[str, dict[int, dict]] = {}
        started = time.monotonic()
        while time.monotonic() - started < 6.0:
            entities = poll(server, "@e[tag=m]")
            probe.pump(0.005)
            clock = next((e for e in entities if "clock" in e["tags"]), None)
            if clock is None or "fuse" not in clock:
                if entities:
                    print("no clock among", [e["raw"][:80] for e in entities[:2]])
                continue
            age = 30000 - clock["fuse"]
            for e in entities:
                name = tag_of(e, "s_")
                if name:
                    samples.setdefault(name[2:], {})[age] = e
        for name, by_age in samples.items():
            fit = fit_transitions(by_age)
            fit["samples"] = len(by_age)
            fit["trace"] = [{"age": a, "pos": s.get("pos"), "motion": s.get("motion"),
                             "in_ground": s.get("in_ground")} for a, s in sorted(by_age.items())]
            document[name] = fit
            print(f"{name:10s} samples={len(by_age):3d} pairs={fit['pairs']:3d} "
                  f"drag_x={fit.get('drag_x')!r} drag_y={fit.get('drag_y')!r} "
                  f"b={fit.get('intercept_y')!r} res_y={fit.get('residual_y')!r} "
                  f"old_v={fit.get('move_with_old_velocity_residual')!r} "
                  f"new_v={fit.get('move_with_new_velocity_residual')!r}")
        spawns = [spawn_fields(p) for pid, p in probe.drain() if pid == CB_SPAWN_ENTITY]
        document["spawn_packets"] = spawns
        for s in spawns:
            print("spawn", s)
    finally:
        finish(server, "flight")
    write("flight", document)


# ── scenario: ground ────────────────────────────────────────────────────────

def measure_ground(_trials: int) -> None:
    server = start("ground")
    document: dict = {}
    try:
        forceload(server, -16, -16, 32, 32)
        fill(server, 10, FLOOR_Y, -2, 10, FLOOR_Y + 4, 6, "minecraft:stone")
        probe = Archer(PORT)
        probe.pump(2.0)
        server.batch(["tp ovarcher 0 -60 -10", "gamemode spectator ovarcher"])
        server.batch([
            "kill @e[type=!player]",
            'summon tnt 0.5 -40 20.5 {Fuse:30000s,NoGravity:1b,Tags:["m","clock"]}',
            # Straight down onto the grass, and flat into a stone wall.
            'summon arrow 2.5 -55 0.5 {Motion:[0.0d,-1.3d,0.0d],Tags:["m","s_floor"]}',
            'summon arrow 5.5 -57.5 4.5 {Motion:[1.7d,0.0d,0.0d],Tags:["m","s_wall"]}',
            'summon snowball 5.5 -57.5 2.5 {Motion:[1.7d,0.0d,0.0d],Tags:["m","s_snow"]}',
            'summon trident 5.5 -57.5 0.5 {Motion:[1.7d,0.0d,0.0d],Tags:["m","s_trident"]}',
        ])
        traces: dict[str, list[dict]] = {}
        gone: dict[str, int] = {}
        last_seen: dict[str, int] = {}
        started = time.monotonic()
        while time.monotonic() - started < 75.0:
            entities = poll(server, "@e[tag=m]")
            clock = next((e for e in entities if "clock" in e["tags"]), None)
            if clock is None:
                continue
            age = 30000 - clock["fuse"]
            seen = set()
            for e in entities:
                name = tag_of(e, "s_")
                if not name:
                    continue
                seen.add(name)
                last_seen[name] = age
                row = {"age": age, "pos": e.get("pos"), "motion": e.get("motion"),
                       "in_ground": e.get("in_ground"), "life": e.get("life")}
                trace = traces.setdefault(name, [])
                if len(trace) < 40 or trace[-1]["life"] != row["life"] and row["life"] and row["life"] % 100 == 0:
                    trace.append(row)
                traces[name][-1:] = traces[name][-1:]  # keep list
                document.setdefault("last", {})[name] = row
            for name in list(last_seen):
                if name not in seen and name not in gone:
                    gone[name] = age
                    print(f"{name} gone at age {age}, last {document['last'][name]}")
            probe.pump(0.01)
            if len(gone) >= 3 and time.monotonic() - started > 65.0:
                break
            if time.monotonic() - started > 3.0:
                time.sleep(0.2)
        document["traces"] = traces
        document["gone_at_age"] = gone
        spawns = [spawn_fields(p) for pid, p in probe.drain() if pid == CB_SPAWN_ENTITY]
        document["spawn_packets"] = spawns
    finally:
        finish(server, "ground")
    write("ground", document)


# ── scenario: damage ────────────────────────────────────────────────────────

COW = 'NoAI:1b,Silent:1b,PersistenceRequired:1b,Attributes:[{Name:"generic.max_health",Base:1024d}],Health:1024f'


def measure_damage(trials: int) -> None:
    """Hits on the first tick, at a known speed, on a fresh cow each time.

    The cow stands at x = 10.5 (box 10.05 .. 10.95); the arrow starts at x =
    9.65, a tenth of a block short of the box grown by 0.3, level with the
    cow's middle. Whatever its speed it reaches the cow in the first tick.
    """
    server = start("damage")
    server.batch(["difficulty normal"])
    document: dict = {"cases": []}
    try:
        forceload(server, -16, -16, 48, 400)
        probe = Archer(PORT)
        probe.pump(2.0)
        server.batch(["tp ovarcher 0 -60 -10", "gamemode spectator ovarcher"])
        cases: list[tuple[str, str, float, str, str]] = []
        for v in (0.3, 0.7, 1.0, 1.3, 1.9, 2.6, 3.0):
            cases.append((f"arrow_v{v}", "arrow", v, "", "cow"))
        cases.append(("arrow_v1.3_dmg3.5", "arrow", 1.3, "damage:3.5d,", "cow"))
        cases.append(("arrow_v0.7_dmg0.5", "arrow", 0.7, "damage:0.5d,", "cow"))
        for k in range(max(trials, 24)):
            cases.append((f"crit_{k}", "arrow", 3.0, "crit:1b,", "cow"))
        cases.append(("trident_v2.5", "trident", 2.5, "", "cow"))
        cases.append(("trident_v0.5", "trident", 0.5, "", "cow"))
        cases.append(("snowball_cow", "snowball", 1.5, "", "cow"))
        cases.append(("snowball_blaze", "snowball", 1.5, "", "blaze"))
        cases.append(("egg_cow", "egg", 1.5, "", "cow"))
        cases.append(("arrow_blaze", "arrow", 1.9, "", "blaze"))
        for k, (name, kind, v, extra, target) in enumerate(cases):
            z = 4.0 * k + 0.5
            if target == "cow":
                mid = FLOOR_Y + 0.7
            else:
                mid = FLOOR_Y + 0.9
            server.batch([
                f'summon {target} 10.5 {FLOOR_Y} {z} {{{COW},NoGravity:1b,Tags:["t{k}"]}}',
            ])
            server.batch([f'summon {kind} 9.65 {mid} {z} '
                          f'{{{extra}Motion:[{v!r}d,0.0d,0.0d],NoGravity:1b,Tags:["p{k}"]}}'])
        wait_ticks(server, 10)
        for k, (name, kind, v, extra, target) in enumerate(cases):
            rows = poll(server, f"@e[tag=t{k}]")
            health = rows[0].get("health") if rows else None
            motion = rows[0].get("motion") if rows else None
            proj = poll(server, f"@e[tag=p{k}]")
            row = {"name": name, "kind": kind, "speed": v, "extra": extra, "target": target,
                   "health": health, "lost": None if health is None else 1024.0 - health,
                   "target_motion": motion,
                   "projectile_left": [{"pos": p.get("pos"), "motion": p.get("motion"),
                                        "in_ground": p.get("in_ground")} for p in proj]}
            document["cases"].append(row)
            print(f"{name:22s} lost={row['lost']} motion={motion} proj_left={len(proj)}")
        crits = [c["lost"] for c in document["cases"] if c["name"].startswith("crit_")]
        document["crit_values"] = sorted(crits)
        print("crit values:", sorted(crits))
    finally:
        finish(server, "damage")
    write("damage", document)


# ── scenario: bow ───────────────────────────────────────────────────────────

def arrows_in_inventory(server: Server, name: str) -> tuple[int, int | None]:
    lines = server.batch([f"data get entity {name} Inventory"])
    text = " ".join(lines)
    count = 0
    for match in re.finditer(r'id: "minecraft:arrow"[^}]*?Count: (\d+)b', text):
        count += int(match.group(1))
    for match in re.finditer(r'Count: (\d+)b, id: "minecraft:arrow"', text):
        count += int(match.group(1))
    damage = None
    found = re.search(r'id: "minecraft:(?:bow|crossbow)", tag: \{[^}]*?Damage: (\d+)', text)
    if found:
        damage = int(found.group(1))
    found = re.search(r'Damage: (\d+)[^}]*\}, id: "minecraft:(?:bow|crossbow)"', text)
    if found:
        damage = int(found.group(1))
    return count, damage


def one_shot(server: Server, probe: Archer, hold_ticks: float) -> dict:
    """Draw for `hold_ticks` (commanded in wall time), let go, read the arrow."""
    server.batch(["kill @e[type=arrow]"])
    probe.pump(0.2)
    probe.drain()
    t0 = gametime(server)
    probe.use()
    probe.pump(hold_ticks * 0.05)
    probe.release()
    probe.pump(0.15)
    t1 = gametime(server)
    arrows = poll(server, "@e[type=arrow,limit=1,sort=nearest]")
    packets = probe.drain()
    spawns = [spawn_fields(p) for pid, p in packets if pid == CB_SPAWN_ENTITY]
    metas = [metadata_fields(p) for pid, p in packets if pid == 0x52]
    arrow = arrows[0] if arrows else None
    row = {"commanded_ticks": hold_ticks, "gametime_span": t1 - t0}
    if arrow:
        raw = arrow["raw"]
        row["motion"] = arrow.get("motion")
        row["pos"] = arrow.get("pos")
        row["speed"] = math.sqrt(sum(c * c for c in arrow["motion"])) if arrow.get("motion") else None
        for key, pattern in (("crit", r"\bcrit: ([01])b"), ("pickup", r"\bpickup: (\d)b"),
                             ("damage", r"\bdamage: ([-0-9.Ee]+)d"),
                             ("crossbow", r"ShotFromCrossbow: ([01])b"),
                             ("pierce", r"PierceLevel: (\d+)b")):
            found = re.search(pattern, raw)
            row[key] = float(found.group(1)) if found else None
    arrow_spawns = [s for s in spawns if s["type"] == "minecraft:arrow"]
    row["spawn"] = arrow_spawns[0] if arrow_spawns else None
    if row["spawn"]:
        eid = row["spawn"]["entity_id"]
        row["metadata"] = [f for e, f in metas if e == eid]
    return row


def setup_archer(server: Server, name: str = "ovarcher") -> Archer:
    probe = Archer(PORT, name=name)
    probe.pump(2.0)
    server.batch([f"tp {name} 0.5 {FLOOR_Y} 0.5 -90 0", f"gamemode survival {name}",
                  f"clear {name}", f"effect give {name} minecraft:resistance 100000 4 true",
                  f"effect give {name} minecraft:saturation 100000 4 true"])
    probe.pump(1.0)
    probe.look(0.5, FLOOR_Y, 0.5, -90.0, 0.0)
    probe.pump(0.5)
    return probe


def measure_bow(trials: int) -> None:
    server = start("bow")
    server.batch(["difficulty normal"])
    document: dict = {}
    try:
        forceload(server, -32, -32, 96, 32)
        probe = setup_archer(server)
        server.batch(["give ovarcher minecraft:bow", "give ovarcher minecraft:arrow 64"])
        probe.hold(0)
        probe.pump(0.5)
        before = arrows_in_inventory(server, "ovarcher")
        document["inventory_before"] = before
        shots = []
        for hold in [2, 3, 4, 5, 6, 8, 10, 12, 15, 18, 20, 22, 25, 30]:
            for _ in range(max(1, trials // 8)):
                row = one_shot(server, probe, hold)
                row["inventory_after"] = arrows_in_inventory(server, "ovarcher")
                shots.append(row)
                print(f"hold={hold:3} speed={row.get('speed')!r} crit={row.get('crit')} "
                      f"dmg={row.get('damage')} pickup={row.get('pickup')} "
                      f"inv={row['inventory_after']} spawn={row.get('spawn')}")
        document["shots"] = shots
        # Dispersion: full charge, many times.
        spread = []
        for _ in range(max(trials, 40)):
            server.batch(["give ovarcher minecraft:arrow 1"])
            row = one_shot(server, probe, 24)
            if row.get("motion"):
                spread.append(row["motion"])
        document["full_charge_motions"] = spread
        # Range at pitch 0: a full charge, read where it lands.
        server.batch(["kill @e[type=arrow]", "give ovarcher minecraft:arrow 8"])
        probe.use()
        probe.pump(1.3)
        probe.release()
        time.sleep(4.0)
        probe.pump(0.1)
        landed = poll(server, "@e[type=arrow]")
        document["range_pitch0"] = [{"pos": a.get("pos"), "in_ground": a.get("in_ground"),
                                     "life": a.get("life")} for a in landed]
        print("range", document["range_pitch0"])
        # Pickup: walk onto it.
        if landed and landed[0].get("pos"):
            x, _, z = landed[0]["pos"]
            count0 = arrows_in_inventory(server, "ovarcher")[0]
            probe.drain()
            server.batch([f"tp ovarcher {x} {FLOOR_Y} {z}"])
            probe.pump(1.5)
            count1 = arrows_in_inventory(server, "ovarcher")[0]
            takes = []
            for pid, p in probe.drain():
                if pid == CB_TAKE_ITEM:
                    a, i = read_varint(p, 0)
                    b, i = read_varint(p, i)
                    c, _ = read_varint(p, i)
                    takes.append([a, b, c])
            document["pickup"] = {"before": count0, "after": count1, "take_packets": takes,
                                  "remaining": len(poll(server, "@e[type=arrow]"))}
            print("pickup", document["pickup"])
        # Infinity.
        server.batch(["clear ovarcher",
                      'give ovarcher minecraft:bow{Enchantments:[{id:"minecraft:infinity",lvl:1s}]}',
                      "give ovarcher minecraft:arrow 5"])
        server.batch(["tp ovarcher 0.5 -60 0.5 -90 0"])
        probe.pump(0.5)
        probe.look(0.5, FLOOR_Y, 0.5, -90.0, 0.0)
        row = one_shot(server, probe, 24)
        row["inventory_after"] = arrows_in_inventory(server, "ovarcher")
        document["infinity"] = row
        print("infinity", row.get("pickup"), row["inventory_after"])
        # Creative.
        server.batch(["gamemode creative ovarcher", "clear ovarcher", "give ovarcher minecraft:bow",
                      "give ovarcher minecraft:arrow 5"])
        probe.pump(0.5)
        row = one_shot(server, probe, 24)
        row["inventory_after"] = arrows_in_inventory(server, "ovarcher")
        document["creative"] = row
        print("creative", row.get("pickup"), row["inventory_after"])
        # Creative, and no arrows at all.
        server.batch(["clear ovarcher", "give ovarcher minecraft:bow"])
        probe.pump(0.5)
        row = one_shot(server, probe, 24)
        document["creative_no_arrows"] = row
        print("creative without arrows", row.get("speed"), row.get("pickup"))
        # Survival, no arrows.
        server.batch(["gamemode survival ovarcher", "clear ovarcher", "give ovarcher minecraft:bow"])
        probe.pump(0.5)
        row = one_shot(server, probe, 24)
        document["survival_no_arrows"] = row
        print("survival without arrows", row.get("speed"))
        # Charged shots at a cow, 2.5 blocks ahead, its middle 0.3 below the eye.
        server.batch(["gamemode survival ovarcher", "clear ovarcher", "give ovarcher minecraft:bow",
                      "give ovarcher minecraft:arrow 64"])
        probe.pump(0.3)
        hits = []
        for hold in (5, 10, 15, 20, 25):
            for k in range(3):
                server.batch(["kill @e[type=cow]",
                              f'summon cow 3.0 {FLOOR_Y + EYE - 0.3 - 0.7} 0.5 '
                              f'{{{COW},NoGravity:1b,Tags:["target"]}}'])
                probe.pump(0.3)
                row = one_shot(server, probe, hold)
                time.sleep(0.5)
                cow = poll(server, "@e[tag=target]")
                row["cow_lost"] = 1024.0 - cow[0]["health"] if cow and "health" in cow[0] else None
                hits.append(row)
                print(f"hit hold={hold} speed={row.get('speed')} lost={row['cow_lost']}")
        document["hits"] = hits
    finally:
        finish(server, "bow")
    write("bow", document)


# ── scenario: crossbow ──────────────────────────────────────────────────────

def crossbow_state(server: Server) -> tuple[bool, str]:
    lines = server.batch(["data get entity ovarcher SelectedItem"])
    text = " ".join(lines)
    return "Charged: 1b" in text, text


def measure_crossbow(trials: int) -> None:
    server = start("crossbow")
    document: dict = {"charge": []}
    try:
        forceload(server, -32, -32, 96, 32)
        probe = setup_archer(server)
        for hold in [20, 22, 23, 24, 25, 26, 27, 28, 30, 35]:
            for _ in range(max(2, trials // 8)):
                server.batch(["clear ovarcher", "give ovarcher minecraft:crossbow",
                              "give ovarcher minecraft:arrow 8"])
                probe.hold(0)
                probe.pump(0.3)
                t0 = gametime(server)
                probe.use()
                probe.pump(hold * 0.05)
                probe.release()
                probe.pump(0.2)
                t1 = gametime(server)
                charged, text = crossbow_state(server)
                count, _ = arrows_in_inventory(server, "ovarcher")
                document["charge"].append({"hold": hold, "span": t1 - t0, "charged": charged,
                                           "arrows_left": count})
                print(f"crossbow hold={hold} span={t1 - t0} charged={charged} arrows={count}")
        document["charged_item"] = crossbow_state(server)[1]
        # Shoot what is loaded.
        server.batch(["clear ovarcher", "give ovarcher minecraft:crossbow",
                      "give ovarcher minecraft:arrow 8"])
        probe.pump(0.3)
        probe.use()
        probe.pump(1.6)
        probe.release()
        probe.pump(0.3)
        document["loaded"] = crossbow_state(server)[1]
        shots = []
        for _ in range(max(4, trials // 4)):
            server.batch(["kill @e[type=arrow]"])
            probe.drain()
            probe.use()      # fires
            probe.pump(0.2)
            arrows = poll(server, "@e[type=arrow]")
            packets = probe.drain()
            for a in arrows:
                raw = a["raw"]
                row = {"motion": a.get("motion"),
                       "speed": math.sqrt(sum(c * c for c in a["motion"])) if a.get("motion") else None,
                       "crit": re.search(r"\bcrit: ([01])b", raw).group(1) if re.search(r"\bcrit: ([01])b", raw) else None,
                       "crossbow": "ShotFromCrossbow: 1b" in raw,
                       "pickup": re.search(r"\bpickup: (\d)b", raw).group(1) if re.search(r"\bpickup: (\d)b", raw) else None,
                       "damage": re.search(r"\bdamage: ([-0-9.Ee]+)d", raw).group(1) if re.search(r"\bdamage: ([-0-9.Ee]+)d", raw) else None,
                       "pierce": re.search(r"PierceLevel: (\d+)b", raw).group(1) if re.search(r"PierceLevel: (\d+)b", raw) else None,
                       "pos": a.get("pos")}
                shots.append(row)
                print("crossbow shot", row)
            document.setdefault("shot_spawns", []).extend(
                spawn_fields(p) for pid, p in packets if pid == CB_SPAWN_ENTITY)
            document.setdefault("shot_meta", []).extend(
                metadata_fields(p) for pid, p in packets if pid == 0x52)
            document["after_shot"] = crossbow_state(server)[1]
            # Load again for the next.
            probe.use()
            probe.pump(1.6)
            probe.release()
            probe.pump(0.3)
        document["shots"] = shots
        document["durability"] = arrows_in_inventory(server, "ovarcher")
    finally:
        finish(server, "crossbow")
    write("crossbow", document)


# ── scenario: eggs ──────────────────────────────────────────────────────────

def measure_eggs(trials: int) -> None:
    """One egg per glass cell; chickens counted per cell from their Pos."""
    server = start("eggs")
    server.batch(["difficulty normal"])
    side = 20
    rounds = max(trials, 10)
    per_cell: list[int] = []
    try:
        forceload(server, -8, -8, 2 * side + 8, 2 * side + 8)
        fill(server, 0, FLOOR_Y, 0, 2 * side, FLOOR_Y + 2, 2 * side, "minecraft:glass")
        for i in range(side):
            server.batch([f"fill {2 * i + 1} {FLOOR_Y} 1 {2 * i + 1} {FLOOR_Y + 2} {2 * side - 1} "
                          "minecraft:air"])
        for j in range(side):
            server.batch([f"fill 1 {FLOOR_Y} {2 * j + 2} {2 * side - 1} {FLOOR_Y + 2} {2 * j + 2} "
                          "minecraft:glass"])
        # Rows of cells now: x odd columns, separated in z by glass every other block.
        probe = Archer(PORT)
        probe.pump(2.0)
        server.batch([f"tp ovarcher {side} -40 {side}", "gamemode spectator ovarcher"])
        cells = [(2 * i + 1, 2 * j + 1) for i in range(side) for j in range(side)]
        for r in range(rounds):
            server.batch(["kill @e[type=chicken]", "kill @e[type=egg]"])
            server.batch([f"summon egg {x + 0.5} {FLOOR_Y + 1.5} {z + 0.5} {{Motion:[0.0d,-0.6d,0.0d]}}"
                          for x, z in cells])
            wait_ticks(server, 10)
            counts = {c: 0 for c in cells}
            for e in poll(server, "@e[type=chicken]"):
                if "pos" in e:
                    key = (math.floor(e["pos"][0]), math.floor(e["pos"][2]))
                    if key in counts:
                        counts[key] += 1
            per_cell.extend(counts.values())
            hist = {n: per_cell.count(n) for n in sorted(set(per_cell))}
            print(f"round {r + 1}/{rounds}: eggs={len(per_cell)} histogram={hist}")
    finally:
        finish(server, "eggs")
    hist = {str(n): per_cell.count(n) for n in sorted(set(per_cell))}
    write("eggs", {"eggs": len(per_cell), "histogram": hist})


# ── scenario: pearl ─────────────────────────────────────────────────────────

def measure_pearl(trials: int) -> None:
    server = start("pearl")
    # Without this the first version read 2.5, 4.2 and 4.8 points lost: a fed
    # player heals a point every half-second, and the landing was read seconds
    # after it happened.
    server.batch(["difficulty normal", "gamerule naturalRegeneration false"])
    document: dict = {"throws": []}
    try:
        forceload(server, -32, -32, 96, 32)
        probe = Archer(PORT)
        probe.pump(2.0)
        server.batch([f"tp ovarcher 0.5 {FLOOR_Y} 0.5 -90 0", "gamemode survival ovarcher",
                      "clear ovarcher"])
        probe.pump(1.0)
        for k in range(max(3, trials // 8)):
            server.batch(["effect clear ovarcher", "clear ovarcher",
                          "give ovarcher minecraft:ender_pearl 1",
                          "effect give ovarcher minecraft:instant_health 1 10 true",
                          f"tp ovarcher 0.5 {FLOOR_Y} 0.5 -90 -20"])
            probe.pump(1.0)
            probe.look(0.5, FLOOR_Y, 0.5, -90.0, -20.0)
            probe.pump(0.3)
            hp0 = poll(server, "@a[name=ovarcher]")[0].get("health")
            probe.use()
            probe.pump(4.0)
            after = poll(server, "@a[name=ovarcher]")[0]
            mites = len(poll(server, "@e[type=endermite]"))
            row = {"health_before": hp0, "health_after": after.get("health"),
                   "pos_after": after.get("pos"), "endermites": mites}
            document["throws"].append(row)
            print("pearl", row)
    finally:
        finish(server, "pearl")
    write("pearl", document)


# ── scenario: skeleton ──────────────────────────────────────────────────────

def measure_skeleton(trials: int) -> None:
    server = start("skeleton")
    document: dict = {}
    try:
        forceload(server, -32, -32, 64, 32)
        probe = Archer(PORT)
        probe.pump(2.0)
        server.batch([f"tp ovarcher 0.5 {FLOOR_Y} 0.5 -90 0", "gamemode survival ovarcher",
                      "effect give ovarcher minecraft:resistance 100000 4 true",
                      "effect give ovarcher minecraft:regeneration 100000 4 true",
                      "effect give ovarcher minecraft:saturation 100000 4 true",
                      "time set midnight"])
        # A glass cage, so the skeleton cannot walk up, and stays at 10 blocks.
        fill(server, 9, FLOOR_Y, -1, 13, FLOOR_Y + 3, 3, "minecraft:glass")
        fill(server, 10, FLOOR_Y, 0, 12, FLOOR_Y + 2, 2, "minecraft:air")
        fill(server, 9, FLOOR_Y, 0, 9, FLOOR_Y + 2, 2, "minecraft:air")  # open towards the probe
        fill(server, 9, FLOOR_Y + 3, -1, 13, FLOOR_Y + 3, 3, "minecraft:glass")
        for difficulty in ("easy", "normal", "hard"):
            server.batch([f"difficulty {difficulty}", "kill @e[type=!player]"])
            server.batch([f'summon skeleton 11.5 {FLOOR_Y} 1.5 '
                          '{PersistenceRequired:1b,HandItems:[{id:"minecraft:bow",Count:1b},{}]}'])
            probe.pump(0.5)
            probe.drain()
            probe.stamped.clear()
            t0 = gametime(server)
            probe.pump(20.0)
            t1 = gametime(server)
            arrows = []
            for when, pid, payload in probe.stamped:
                if pid == CB_SPAWN_ENTITY:
                    s = spawn_fields(payload)
                    if s["type"] == "minecraft:arrow":
                        s["t"] = when
                        arrows.append(s)
            gaps = [round((b["t"] - a["t"]) * 20.0, 1) for a, b in zip(arrows, arrows[1:])]
            speeds = [math.sqrt(sum((c / 8000.0) ** 2 for c in s["velocity_raw"])) for s in arrows]
            document[difficulty] = {"arrows": arrows, "gaps_ticks_wall": gaps, "speeds": speeds,
                                    "gametime_span": t1 - t0}
            print(f"{difficulty}: {len(arrows)} arrows in {t1 - t0} ticks, gaps {gaps}, "
                  f"speeds {[round(s, 3) for s in speeds]}")
    finally:
        finish(server, "skeleton")
    write("skeleton", document)


# ── scenario: metadata ──────────────────────────────────────────────────────

def measure_metadata(_trials: int) -> None:
    server = start("metadata")
    document: dict = {}
    try:
        forceload(server, -16, -16, 32, 32)
        probe = Archer(PORT)
        probe.pump(2.0)
        server.batch(["tp ovarcher 0.5 -60 0.5", "gamemode spectator ovarcher"])
        probe.pump(1.0)
        variants = [
            ("arrow_base", "arrow", "NoGravity:1b"),
            ("arrow_crit", "arrow", "NoGravity:1b,crit:1b"),
            ("arrow_pierce", "arrow", "NoGravity:1b,PierceLevel:3b"),
            ("arrow_color", "arrow", "NoGravity:1b,Color:16711680"),
            ("arrow_potion", "arrow", 'NoGravity:1b,Potion:"minecraft:poison"'),
            ("arrow_crossbow", "arrow", "NoGravity:1b,ShotFromCrossbow:1b"),
            ("arrow_noclip", "arrow", "NoGravity:1b,inGround:1b"),
            ("spectral", "spectral_arrow", "NoGravity:1b"),
            ("trident_base", "trident", "NoGravity:1b"),
            ("trident_loyal", "trident",
             'NoGravity:1b,Trident:{id:"minecraft:trident",Count:1b,tag:{Enchantments:[{id:"minecraft:loyalty",lvl:3s}]}}'),
            ("trident_foil", "trident",
             'NoGravity:1b,Trident:{id:"minecraft:trident",Count:1b,tag:{Enchantments:[{id:"minecraft:sharpness",lvl:1s}]}}'),
            ("snowball", "snowball", "NoGravity:1b"),
            ("egg", "egg", "NoGravity:1b"),
            ("pearl", "ender_pearl", "NoGravity:1b"),
            ("xpbottle", "experience_bottle", "NoGravity:1b"),
        ]
        for name, kind, nbt in variants:
            probe.drain()
            server.batch([f"summon {kind} 3.5 -58 0.5 {{{nbt}}}"])
            probe.pump(0.6)
            packets = probe.drain()
            spawns = [spawn_fields(p) for pid, p in packets if pid == CB_SPAWN_ENTITY]
            ids = {s["entity_id"] for s in spawns}
            metas = [f for pid, p in packets if pid == 0x52
                     for e, f in [metadata_fields(p)] if e in ids]
            document[name] = {"spawn": spawns, "metadata": metas}
            print(name, spawns[:1], metas[:2])
            server.batch([f"kill @e[type={kind}]"])
            probe.pump(0.3)
    finally:
        finish(server, "metadata")
    write("metadata", document)


SCENARIOS = {
    "flight": measure_flight,
    "ground": measure_ground,
    "damage": measure_damage,
    "bow": measure_bow,
    "crossbow": measure_crossbow,
    "eggs": measure_eggs,
    "pearl": measure_pearl,
    "skeleton": measure_skeleton,
    "metadata": measure_metadata,
}


def main() -> int:
    args = sys.argv[1:]
    if not args or any(a not in SCENARIOS and not a.isdigit() for a in args):
        print(__doc__)
        return 2
    trials = next((int(a) for a in args if a.isdigit()), 16)
    for name in (a for a in args if not a.isdigit()):
        SCENARIOS[name](trials)
    return 0


if __name__ == "__main__":
    sys.exit(main())
