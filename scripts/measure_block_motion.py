#!/usr/bin/env python3
"""How blocks move the things on them and in them, traced tick by tick on a real 1.20.1 server.

Ice, slime, honey, soul sand, ladders, cobwebs, powder snow, bubble columns and
flowing water all change how an entity moves. None of it is in Mojang's reports:
it is Java code, and this project does not read Java code. The running game
answers anyway.

The trick that makes the answer exact is the recorder. A datapack's tick
function runs once per game tick, before the entities move, and appends every
tagged entity's `Pos`, `Motion` and `OnGround` to a list in command storage.
The console then prints that list — doubles printed by the game at full
precision, one row per entity per tick, with the game time. No sampling, no
guessing which tick a reading belongs to: this is the complete trajectory.

Pushing is done the same way. An entity tagged `pushx` has `Motion[0]` set to
0.1 at the start of every tick, which is what a mob walking into a wall does to
itself and what climbing by collision needs.

Every scenario is laid out at once, a few blocks apart, and all run in the same
hundred ticks. Armor stands stand for living entities (they have no AI, so
nothing but physics moves them); dropped items for everything that is not
alive.

Usage:
    scripts/measure_block_motion.py run   # the vanilla server (java lane), writes the trace
    scripts/measure_block_motion.py fit   # reads the trace and prints the constants

Output goes to .scratch/block-motion/ (gitignored): the trace is the game's
output and is never committed.
"""
from __future__ import annotations

import json
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / ".scratch" / "block-motion"
TRACE = RUN / "trace.json"
PORT = 25603

# Superflat: bedrock at -64, dirt, grass at -61. Things stand at -60.
FLOOR = -61
TICKS = 110

STAND = "minecraft:armor_stand"
ITEM = "minecraft:item"
COMMON = "Invulnerable:1b,Silent:1b,PersistenceRequired:1b"
ITEM_NBT = 'Item:{id:"minecraft:stone",Count:1b},Age:-32768s,PickupDelay:32767s'


def summon(kind: str, tag: str, x: float, y: float, z: float,
           motion: tuple[float, float, float] = (0.0, 0.0, 0.0), push: bool = False) -> str:
    tags = f'"rec","{tag}"' + (',"pushx"' if push else "")
    extra = ITEM_NBT if kind == ITEM else "NoBasePlate:1b"
    m = ",".join(f"{v}d" for v in motion)
    return f"summon {kind} {x} {y} {z} {{{COMMON},{extra},Tags:[{tags}],Motion:[{m}]}}"


def scenarios() -> tuple[list[str], list[str], dict[str, dict]]:
    """(build commands, summon commands, what each tag is)."""
    build: list[str] = []
    spawn: list[str] = []
    meta: dict[str, dict] = {}

    def add(kind: str, tag: str, x: float, y: float, z: float, **kw) -> None:
        spawn.append(summon(kind, tag, x, y, z, kw.get("motion", (0.0, 0.0, 0.0)),
                            kw.get("push", False)))
        meta[tag] = {"kind": kind, "x": x, "y": y, "z": z, **{k: v for k, v in kw.items()}}

    # ── A: sliding on a floor. Launched along +x at 0.8 and left alone. ──────
    floors = ["stone", "ice", "packed_ice", "blue_ice", "slime_block", "honey_block",
              "soul_sand", "soul_soil", "grass_block"]
    for i, floor in enumerate(floors):
        for j, kind in enumerate((STAND, ITEM)):
            z = i * 6 + j * 3
            build.append(f"fill 0 {FLOOR} {z} 60 {FLOOR} {z} minecraft:{floor}")
            add(kind, f"slide_{floor}_{'stand' if kind == STAND else 'item'}", 1.5, FLOOR + 1,
                z + 0.5, motion=(0.8, 0.0, 0.0), floor=floor)

    # ── B: landing. Dropped from 11 blocks up. ──────────────────────────────
    zb = 60
    for i, floor in enumerate(["slime_block", "honey_block", "stone", "hay_block"]):
        for j, kind in enumerate((STAND, ITEM)):
            x = 2 + i * 6 + j * 3
            build.append(f"setblock {x} {FLOOR} {zb} minecraft:{floor}")
            add(kind, f"land_{floor}_{'stand' if kind == STAND else 'item'}", x + 0.5, -50.0,
                zb + 0.5, floor=floor)

    # ── C: climbing. A stone wall at x+1, the climbable in column x. ────────
    zc = 70
    climbables = {
        "ladder": "minecraft:ladder[facing=west]",
        "vine": "minecraft:vine[east=true]",
        "twisting": "minecraft:twisting_vines_plant",
        "weeping": "minecraft:weeping_vines_plant",
        "cave": "minecraft:cave_vines_plant",
        "scaffolding": "minecraft:scaffolding[distance=0,bottom=false]",
        "none": "minecraft:air",
    }
    for i, (name, state) in enumerate(climbables.items()):
        for j, mode in enumerate(("fall", "climb", "fallitem")):
            x = 2 + i * 4
            z = zc + j * 3
            build.append(f"fill {x + 1} {FLOOR + 1} {z} {x + 1} -36 {z} minecraft:stone")
            build.append(f"fill {x} {FLOOR + 1} {z} {x} -38 {z} {state}")
            build.append(f"setblock {x} -37 {z} minecraft:stone")
            if mode == "fall":
                add(STAND, f"climb_{name}_fall", x + 0.5, -42.0, z + 0.5, block=name)
            elif mode == "climb":
                # Pressed against the wall from the start, pushed into it every tick.
                add(STAND, f"climb_{name}_up", x + 0.5, FLOOR + 1, z + 0.5, push=True, block=name)
            else:
                add(ITEM, f"climb_{name}_item", x + 0.5, -42.0, z + 0.5, block=name)

    # ── D: stuck in a block. A column to fall through, a row to slide through.
    zd = 95
    stuck = {"cobweb": "minecraft:cobweb", "berry": "minecraft:sweet_berry_bush[age=1]",
             "powder": "minecraft:powder_snow", "air": "minecraft:air"}
    for i, (name, state) in enumerate(stuck.items()):
        x = 2 + i * 8
        # Column: berries cannot stack, so theirs is one block on the floor.
        top = FLOOR + 1 if name == "berry" else FLOOR + 8
        for j, kind in enumerate((STAND, ITEM)):
            z = zd + j * 3
            build.append(f"fill {x} {FLOOR + 1} {z} {x} {top} {z} {state}")
            add(kind, f"stuck_{name}_drop_{'stand' if kind == STAND else 'item'}", x + 0.5, -45.0,
                z + 0.5, block=name)
        # Row: launched through five blocks of it along +x.
        for j, kind in enumerate((STAND, ITEM)):
            z = zd + 6 + j * 3
            build.append(f"fill {x + 2} {FLOOR + 1} {z} {x + 6} {FLOOR + 1} {z} {state}")
            add(kind, f"stuck_{name}_row_{'stand' if kind == STAND else 'item'}", x + 0.5,
                FLOOR + 1, z + 0.5, motion=(0.6, 0.0, 0.0), block=name)
        # Spawned inside, with no fall at all.
        z = zd + 12
        build.append(f"fill {x} {FLOOR + 1} {z} {x} {FLOOR + 8} {z} {state}")
        add(STAND, f"stuck_{name}_inside_stand", x + 0.5, FLOOR + 5, z + 0.5, block=name)

    # ── E: bubble columns in a glass tube, and a plain water control. ────────
    ze = 115
    for i, (name, bottom) in enumerate((("up", "soul_sand"), ("down", "magma_block"),
                                        ("still", "stone"))):
        for j, kind in enumerate((STAND, ITEM)):
            x = 2 + i * 8 + j * 4
            z = ze
            build.append(f"fill {x - 1} {FLOOR} {z - 1} {x + 1} {FLOOR + 12} {z + 1} minecraft:glass")
            build.append(f"setblock {x} {FLOOR} {z} minecraft:{bottom}")
            build.append(f"fill {x} {FLOOR + 1} {z} {x} {FLOOR + 10} {z} minecraft:water")
            build.append(f"fill {x} {FLOOR + 11} {z} {x} {FLOOR + 12} {z} minecraft:air")
            y = FLOOR + 3 if name != "down" else FLOOR + 9
            add(kind, f"bubble_{name}_{'stand' if kind == STAND else 'item'}", x + 0.5, y,
                z + 0.5, column=name, top=FLOOR + 10)

    # ── F: a current. A one-wide channel with a source at its west end. ─────
    zf = 125
    for i, fluid in enumerate(("water", "lava")):
        for j, kind in enumerate((STAND, ITEM)):
            z = zf + (i * 2 + j) * 4
            build.append(f"fill 0 {FLOOR + 1} {z - 1} 14 {FLOOR + 2} {z - 1} minecraft:glass")
            build.append(f"fill 0 {FLOOR + 1} {z + 1} 14 {FLOOR + 2} {z + 1} minecraft:glass")
            build.append(f"fill 0 {FLOOR} {z} 14 {FLOOR} {z} minecraft:stone")
            build.append(f"setblock 0 {FLOOR + 1} {z} minecraft:{fluid}")
            if kind == ITEM and fluid == "lava":
                continue  # burns at once
            add(kind, f"current_{fluid}_{'stand' if kind == STAND else 'item'}", 2.5,
                FLOOR + 1, z + 0.5, fluid=fluid, channel_z=z)
    return build, spawn, meta


DATAPACK = {
    "pack.mcmeta": '{"pack":{"pack_format":15,"description":"ov block motion trace"}}',
    "data/minecraft/tags/functions/tick.json": '{"values":["ovtrace:tick"]}',
    "data/ovtrace/functions/tick.mcfunction": "\n".join([
        "execute as @e[tag=rec] run function ovtrace:row",
        "execute as @e[tag=pushx] run data modify entity @s Motion[0] set value 0.1d",
    ]) + "\n",
    "data/ovtrace/functions/row.mcfunction": "\n".join([
        "data modify storage ovtrace:t row set value {}",
        "data modify storage ovtrace:t row.id set from entity @s Tags",
        "data modify storage ovtrace:t row.p set from entity @s Pos",
        "data modify storage ovtrace:t row.m set from entity @s Motion",
        "data modify storage ovtrace:t row.g set from entity @s OnGround",
        "execute store result storage ovtrace:t row.t int 1 run time query gametime",
        "data modify storage ovtrace:t rows append from storage ovtrace:t row",
    ]) + "\n",
}

ROW = re.compile(r"\{(?:[^{}]|\[[^\]]*\])*\}")
FIELD_ID = re.compile(r'id: \[([^\]]*)\]')
FIELD_P = re.compile(r"p: \[([^\]]*)\]")
FIELD_M = re.compile(r"m: \[([^\]]*)\]")
FIELD_G = re.compile(r"g: (\d)b")
FIELD_T = re.compile(r"t: (-?\d+)")


def parse_rows(line: str) -> list[dict]:
    rows = []
    body = line.split("following contents: ", 1)[-1]
    for match in ROW.finditer(body):
        text = match.group(0)
        try:
            rows.append({
                "id": [t.strip().strip('"') for t in FIELD_ID.search(text).group(1).split(",")
                       if t.strip().strip('"') not in ("rec", "pushx")][0],
                "p": [float(v.strip().rstrip("d")) for v in FIELD_P.search(text).group(1).split(",")],
                "m": [float(v.strip().rstrip("d")) for v in FIELD_M.search(text).group(1).split(",")],
                "g": int(FIELD_G.search(text).group(1)),
                "t": int(FIELD_T.search(text).group(1)),
            })
        except AttributeError:
            continue
    return rows


def run() -> int:
    from measure_entities import Server  # noqa: E402  (the vanilla console driver)

    RUN.mkdir(parents=True, exist_ok=True)
    world = RUN / "world"
    if world.exists():
        import shutil
        shutil.rmtree(world)
    for rel, text in DATAPACK.items():
        path = world / "datapacks" / "ovtrace" / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    build, spawn, meta = scenarios()
    server = Server(RUN, port=PORT)
    rows: list[dict] = []
    try:
        server.batch(["gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                      "gamerule doWeatherCycle false", "gamerule randomTickSpeed 0",
                      "gamerule doFireTick false", "gamerule sendCommandFeedback true",
                      "gamerule doEntityDrops false", "time set noon",
                      "forceload add -16 -16 80 180"])
        time.sleep(4.0)
        server.batch(build)
        # Water has to finish flowing down the channels and the bubble columns
        # have to form, both on scheduled ticks.
        time.sleep(6.0)
        server.batch(["data remove storage ovtrace:t rows"] + spawn)
        deadline = time.monotonic() + TICKS / 20.0 + 1.0
        while time.monotonic() < deadline:
            time.sleep(1.0)
            for line in server.batch(["data get storage ovtrace:t rows",
                                      "data remove storage ovtrace:t rows"]):
                if "following contents" in line:
                    rows += parse_rows(line)
    finally:
        server.stop()
    TRACE.write_text(json.dumps({"meta": meta, "rows": rows}))
    tags = {r["id"] for r in rows}
    print(f"{len(rows)} rows, {len(tags)}/{len(meta)} entities traced -> {TRACE}")
    missing = sorted(set(meta) - tags)
    if missing:
        print("never traced:", ", ".join(missing))
    return 0


def load() -> tuple[dict, dict[str, list[dict]]]:
    data = json.loads(TRACE.read_text())
    per: dict[str, list[dict]] = {}
    for row in data["rows"]:
        per.setdefault(row["id"], []).append(row)
    for rows in per.values():
        rows.sort(key=lambda r: r["t"])
    return data["meta"], per


def show(tag: str, rows: list[dict], limit: int = 40) -> None:
    print(f"── {tag}")
    for r in rows[:limit]:
        p, m = r["p"], r["m"]
        print(f"  t={r['t']:6d} g={r['g']} p=({p[0]:.6f},{p[1]:.6f},{p[2]:.6f}) "
              f"m=({m[0]!r},{m[1]!r},{m[2]!r})")


def f32(value: float) -> float:
    """The double a Java float widens to — how the game's constants really read."""
    import struct
    return struct.unpack("f", struct.pack("f", value))[0]


def distinct(values: list[float], digits: int = 12) -> list[float]:
    return sorted({round(v, digits) for v in values})


def ground_ratios(rows: list[dict]) -> list[float]:
    """Motion.x(t+1) / Motion.x(t) over ticks spent on the ground, moving."""
    out = []
    for a, b in zip(rows, rows[1:]):
        if a["g"] and b["g"] and abs(a["m"][0]) > 1e-3 and abs(b["m"][0]) > 1e-3:
            out.append(b["m"][0] / a["m"][0])
    return out


def fit() -> int:
    meta, per = load()
    if len(sys.argv) > 2:
        for tag in sys.argv[2:]:
            for key in sorted(per):
                if key.startswith(tag):
                    show(key, per[key])
        return 0

    # ── Friction: the ratio of successive ground velocities. For a living
    # thing it is friction × 0.91 (× the speed factor after the move); for an
    # item friction × 0.98. Compared against the float and the double product,
    # so the trace says which one the game computes.
    print("== friction ==")
    for tag in sorted(t for t in per if t.startswith("slide_")):
        ratios = ground_ratios(per[tag])
        if not ratios:
            print(f"  {tag:28s} no ground ticks")
            continue
        living = tag.endswith("_stand")
        base = 0.91 if living else 0.98
        kept = distinct(ratios)
        friction = [r / base for r in kept]
        print(f"  {tag:28s} {len(ratios):3d} ticks  ratio {kept[:3]}  "
              f"friction {[round(f, 9) for f in friction[:3]]}")
        for name, value in (("0.6F", 0.6), ("0.98F", 0.98), ("0.989F", 0.989), ("0.8F", 0.8)):
            for kind, product in (("float", f32(f32(value) * f32(base))),
                                  ("double", f32(value) * base)):
                if any(abs(r - product) < 1e-15 for r in kept):
                    print(f"      = {name} x {base} in {kind}, exactly")

    # ── Everything else is read as per-tick velocities: the ladder clamp and
    # climb, the cobweb hold, the bubble-column steps, the current's push.
    print("== vertical motion (first 30 ticks: dy, Motion.y) ==")
    for prefix in ("climb_", "stuck_", "bubble_", "land_"):
        for tag in sorted(t for t in per if t.startswith(prefix)):
            rows = per[tag]
            dys = [round(b["p"][1] - a["p"][1], 10) for a, b in zip(rows, rows[1:])][:30]
            mys = [round(r["m"][1], 10) for r in rows[:30]]
            print(f"  {tag}\n    dy {dys}\n    my {mys}")
    print("== currents (Motion.x per tick, and its change) ==")
    for tag in sorted(t for t in per if t.startswith("current_")):
        rows = per[tag]
        mx = [r["m"][0] for r in rows[:30]]
        print(f"  {tag}\n    mx {[round(v, 10) for v in mx]}\n"
              f"    dmx {[round(b - a, 10) for a, b in zip(mx, mx[1:])]}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in ("run", "fit"):
        print(__doc__)
        sys.exit(2)
    sys.exit(run() if sys.argv[1] == "run" else fit())
