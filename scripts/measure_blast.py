#!/usr/bin/env python3
"""Ask a real 1.20.1 server what an explosion does.

Blast resistance is in no data generator report — it is Java code, one number
per block. PrismarineJS/minecraft-data (MIT) publishes a candidate table, which
is where the numbers below start; this script is what turns a candidate into a
measurement, the same way scripts/measure_hardness.py did for hardness.

Five scenarios:

  resistance  The **row bench**. One charge per block, and a straight row of
              nine copies of that block leading away from it along +X. The row
              is radial, so a ray can only reach the k-th cell by going through
              the k-1 before it: what comes back is a pure penetration depth,
              and penetration depth is a monotone function of blast resistance
              and nothing else.

              A ray's energy is `power * (0.7 .. 1.3)`, drawn per ray, so a
              single shot is a coin toss near the edge. The bench therefore
              fires the same layout K times and records, per cell, **how often**
              it broke. That frequency is a continuous observable, and it
              separates resistances a single shot cannot.

  crater      The shape, cell by cell. A charge at the centre of a solid box,
              K times, rebuilt each time. What comes out is a per-cell
              destruction frequency, plus the two sharp shapes: the **union**
              over K shots (every cell any shot took) and the **intersection**
              (the cells every shot took). Those two are what our side has to
              reproduce; a single vanilla shot is not reproducible by anything,
              ours included, because the game rolls its rays.

  damage      Mobs at increasing distance from one charge, health read before
              and after. The damage formula, and the exposure that scales it.

  drops       Whether TNT drops what it breaks, and at what rate.

  sources     The powers: TNT, creeper, charged creeper, and the fuses.

Usage:
    python3 scripts/measure_blast.py <scenario> [trials]

Writes data/vanilla/1.20.1/normalized/blast_<scenario>.json.
"""
from __future__ import annotations

import json
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from measure_entities import Server  # noqa: E402
from measure_motion import SUBSTRATE  # noqa: E402
from measure_redstone import (  # noqa: E402
    DYNAMIC, FLOODS, blocks_report, fill, forceload, freeze, gametime, name_of,
    properties_of, read_states, save,
)

NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
RUN = ROOT / "run" / "blast-oracle"
PORT = 25613

# Well clear of the superflat floor at -61 and of the build limit. A ray reaches
# at most 4/3 * power blocks, so eight of clearance is more than enough and
# nothing here ever touches the ground the world came with.
#
# ⚠ The trap this scenario family is named for: a `/fill` whose corner falls
#   below y=-64 fails **as a whole** and says so only on a console line nobody
#   reads. Every cell then reads "not the material", which is exactly what total
#   destruction looks like. Every y in this file is checked against the floor.
BENCH_Y = -40
WORLD_BOTTOM = -64


def check_y(*ys: int) -> None:
    for y in ys:
        if y <= WORLD_BOTTOM:
            raise RuntimeError(f"y={y} is at or below the world bottom {WORLD_BOTTOM}; "
                               f"a /fill reaching it fails silently and reads as destruction")


def start(name: str) -> tuple[Server, Path]:
    directory = RUN / name
    server = Server(directory, port=PORT)
    freeze(server)
    # ⚠ `freeze` sets `difficulty peaceful`, which deletes every hostile mob the
    #   moment it spawns — a creeper summoned already ignited included. The
    #   first run of the sources scenario read "the creeper made no crater",
    #   which is indistinguishable from a creeper that does not break blocks.
    server.batch(["difficulty normal"])
    # doTileDrops off keeps a thousand craters from filling the world with item
    # entities; the drops scenario turns it back on for itself.
    return server, directory / "world" / "region"


def wait_ticks(server: Server, count: int) -> None:
    """Wait for the server's own clock, not ours.

    A thousand explosions in one tick makes that tick take seconds. A wall-clock
    sleep then reads a world that has not finished exploding, which looks like
    blocks that survived.
    """
    start_tick = gametime(server)
    deadline = time.monotonic() + 600.0
    while time.monotonic() < deadline:
        if gametime(server) - start_tick >= count:
            return
        time.sleep(0.2)
    raise TimeoutError("the server never advanced its clock")


def run_batched(server: Server, commands: list[str], chunk: int = 400) -> None:
    for i in range(0, len(commands), chunk):
        server.batch(commands[i:i + chunk], timeout=900.0)


# ── scenario: resistance ────────────────────────────────────────────────────
#
# Cell layout, one per candidate block, seen from the side:
#
#     y            [charge]  B B B B B B B B B      <- the row, nine long
#     y-1          floor     floor …                <- barrier, or the substrate
#     y-2          barrier   barrier …              <- holds sand-like floors up
#
# The charge is a primed TNT summoned with `NoGravity` and zero motion, so its
# centre is a fixed point rather than wherever an entity happened to fall to.
#
# Why nine and not six: the deepest a power-4 ray reaches through air is about
# 8.4 blocks, so cells 7..9 can only break for a block of resistance near zero.
# A row that reads "all nine gone" is therefore a **witness**, not a
# measurement: it means the block was not there — a flower that popped off its
# floor reads exactly like that, and would otherwise be recorded as the most
# fragile block in the game.

ROW = 9
COLUMNS = 46
CELL_DZ = 12   # >= twice the reach, so no charge touches its neighbour's row

# The gap between the charge and the first cell of the row.
#
# With no gap the bench saturates: every block from 2.5 up to 9 breaks its first
# cell every time and nothing beyond it, ever, so 340 blocks share one reading.
# Four blocks of air in front eats about three of the four units of ray energy
# and puts the whole 2.5..9 band back where a cell breaks *sometimes* — and a
# frequency is a measurement where a certainty is not.
GAPS = {"resistance": 0, "resistance_gap": 4}


def cell_dx(gap: int) -> int:
    # Row length, plus the gap, plus how far the next charge reaches backwards.
    return gap + ROW + 11


def cell_of(index: int, gap: int) -> tuple[int, int]:
    return (index % COLUMNS) * cell_dx(gap), (index // COLUMNS) * CELL_DZ


def bench_names() -> tuple[list[str], set[str]]:
    report = blocks_report()
    names = sorted(n for n in report if n not in DYNAMIC and n not in FLOODS)
    dry = {n for n, b in report.items() if "waterlogged" in b.get("properties", {})}
    return names, dry


def placed(name: str, dry: set[str]) -> str:
    # A waterlogged block answers with water's resistance, not its own: the
    # game takes the larger of the two. Everything that can hold water is placed
    # dry, for the same reason every other campaign in this repo does it.
    return f"{name}[waterlogged=false]" if name in dry else name


def measure_resistance(out: Path, trials: int, gap: int = 0) -> None:
    names, dry = bench_names()
    y = BENCH_Y
    check_y(y, y - 1, y - 2)
    rows = (len(names) + COLUMNS - 1) // COLUMNS
    width = COLUMNS * cell_dx(gap) + gap + ROW + 8
    depth = rows * CELL_DZ + 8

    server, world = start("resistance" if gap == 0 else "resistance_gap")
    result: dict = {"trials": trials, "row": ROW, "power": 4, "gap": gap}
    try:
        forceload(server, -8, -8, width, depth)
        fill(server, -8, y - 2, -8, width, y - 2, depth, "minecraft:barrier")
        fill(server, -8, y - 1, -8, width, y - 1, depth, "minecraft:barrier")
        fill(server, -8, y, -8, width, y + 6, depth, "minecraft:air")

        # Blocks that need something specific under them get it; everything else
        # stands on barrier, which no explosion in the game can remove.
        floors = {n: SUBSTRATE[n] for n in names if n in SUBSTRATE}

        def build(subset: list[str], floor_of: dict[str, str]) -> list[str]:
            commands = []
            for index, name in enumerate(names):
                if name not in subset:
                    continue
                cx, cz = cell_of(index, gap)
                floor = floor_of.get(name)
                if floor is not None:
                    commands.append(
                        f"fill {cx} {y - 1} {cz} {cx + gap + ROW} {y - 1} {cz} {floor}")
                commands.append(f"fill {cx + gap + 1} {y} {cz} {cx + gap + ROW} {y} {cz} "
                                f"{placed(name, dry)}")
            return commands

        def read_rows(subset: list[str]) -> dict[str, list[str]]:
            cells, order = [], []
            for index, name in enumerate(names):
                if name not in subset:
                    continue
                cx, cz = cell_of(index, gap)
                order.append(name)
                cells += [(cx + gap + k, y, cz) for k in range(1, ROW + 1)]
            states = read_states(world, cells)
            return {n: [name_of(s) for s in states[i * ROW:(i + 1) * ROW]]
                    for i, n in enumerate(order)}

        # ── pass 1: does the block stand there at all? ──────────────────────
        run_batched(server, build(names, floors))
        save(server)
        standing = read_rows(names)
        refused = [n for n in names if any(s != n for s in standing[n])]
        print(f"  standing on the first floor: {len(names) - len(refused)}/{len(names)}")

        # ── pass 2: dirt for whatever refused barrier ───────────────────────
        if refused:
            retry = {n: "minecraft:dirt" for n in refused}
            run_batched(server, build(refused, retry))
            save(server)
            second = read_rows(refused)
            still = [n for n in refused if any(s != n for s in second[n])]
            for n in refused:
                if n not in still:
                    floors[n] = "minecraft:dirt"
            refused = still
            print(f"  standing after the dirt retry: {len(names) - len(refused)}/{len(names)}")

        measured = [n for n in names if n not in refused]
        counts = {n: [0] * ROW for n in measured}
        result["floors"] = {n: floors[n] for n in measured if n in floors}

        for trial in range(trials):
            run_batched(server, build(measured, floors))
            summons = []
            for index, name in enumerate(names):
                if name not in counts:
                    continue
                cx, cz = cell_of(index, gap)
                summons.append(f"summon minecraft:tnt {cx + 0.5} {y}.0 {cz + 0.5} "
                               "{Fuse:0,NoGravity:1b,Motion:[0.0,0.0,0.0]}")
            run_batched(server, summons)
            wait_ticks(server, 6)
            server.batch(["kill @e[type=minecraft:tnt]", "kill @e[type=minecraft:item]"])
            save(server)
            after = read_rows(measured)
            for name in measured:
                for k, state in enumerate(after[name]):
                    if state != name:
                        counts[name][k] += 1
            print(f"  trial {trial + 1}/{trials}")

        result["counts"] = counts
        result["refused"] = sorted(refused)
    finally:
        server.stop()

    with open(out, "w") as f:
        json.dump(result, f, indent=1)
    print(f"  measured {len(result.get('counts', {}))} blocks, "
          f"refused {len(result.get('refused', []))}")


# ── scenario: crater ────────────────────────────────────────────────────────

CRATER_MATERIALS = ["minecraft:glass", "minecraft:dirt", "minecraft:sand",
                    "minecraft:oak_planks", "minecraft:stone", "minecraft:end_stone",
                    "minecraft:obsidian"]
CRATER_HALF = 8
CRATER_UP = 6
CRATER_SPACING = 40


def measure_crater(out: Path, trials: int) -> None:
    y = BENCH_Y
    check_y(y - CRATER_UP - 1)
    server, world = start("crater")
    result: dict = {"trials": trials, "half": CRATER_HALF, "up": CRATER_UP,
                    "power": 4, "centre": [0.5, float(y) + 0.06125, 0.5]}
    try:
        counts = {m: {} for m in CRATER_MATERIALS}
        for index, material in enumerate(CRATER_MATERIALS):
            cx = index * CRATER_SPACING
            forceload(server, cx - 24, -24, cx + 24, 24)
        for trial in range(trials):
            for index, material in enumerate(CRATER_MATERIALS):
                cx = index * CRATER_SPACING
                fill(server, cx - CRATER_HALF - 1, y - CRATER_UP - 1, -CRATER_HALF - 1,
                     cx + CRATER_HALF + 1, y + CRATER_UP + 1, CRATER_HALF + 1, material)
            summons = [f"summon minecraft:tnt {index * CRATER_SPACING + 0.5} {y}.0 0.5 "
                       "{Fuse:0,NoGravity:1b,Motion:[0.0,0.0,0.0]}"
                       for index in range(len(CRATER_MATERIALS))]
            server.batch(summons)
            wait_ticks(server, 6)
            server.batch(["kill @e[type=minecraft:tnt]", "kill @e[type=minecraft:item]",
                          "kill @e[type=minecraft:falling_block]"])
            save(server)
            for index, material in enumerate(CRATER_MATERIALS):
                cx = index * CRATER_SPACING
                cells = [(cx + dx, y + dy, dz)
                         for dy in range(-CRATER_UP, CRATER_UP + 1)
                         for dz in range(-CRATER_HALF, CRATER_HALF + 1)
                         for dx in range(-CRATER_HALF, CRATER_HALF + 1)]
                states = read_states(world, cells)
                for (bx, by, bz), state in zip(cells, states):
                    if name_of(state) != material:
                        key = f"{bx - cx},{by - y},{bz}"
                        counts[material][key] = counts[material].get(key, 0) + 1
            print(f"  trial {trial + 1}/{trials}")
        result["counts"] = counts
        for material in CRATER_MATERIALS:
            always = sum(1 for v in counts[material].values() if v == trials)
            ever = len(counts[material])
            print(f"  {material:24s} union {ever:5d}  intersection {always:5d}")
    finally:
        server.stop()
    with open(out, "w") as f:
        json.dump(result, f, indent=1)
    # The same thing again as flat text, because the C++ parity test reads it
    # and ov_gameplay has no JSON parser — the crafting parity table is written
    # the same way for the same reason.
    with open(NORMALIZED / "blast_crater.txt", "w") as f:
        cx, cy, cz = result["centre"]
        f.write(f"# trials {trials} half {CRATER_HALF} up {CRATER_UP} power 4 "
                f"centre {cx} {cy} {cz}\n")
        for material in CRATER_MATERIALS:
            for cell, count in sorted(result["counts"][material].items(),
                                      key=lambda kv: tuple(int(v) for v in kv[0].split(","))):
                dx, dy, dz = cell.split(",")
                f.write(f"{material} {dx} {dy} {dz} {count}\n")


# ── scenario: damage ────────────────────────────────────────────────────────
#
# One charge, one zombie per distance, health read before and after.
#
# ⚠ Three traps, all of them paid by other campaigns in this repo before this
#   one existed:
#     * `Invulnerable:1b` makes an entity impossible to read — it takes nothing
#       and looks exactly like an entity out of range. The targets here are
#       ordinary and simply carry a thousand health.
#     * `freeze()` sets `difficulty peaceful`, which deletes a zombie the tick
#       it appears. `start()` puts it back to normal.
#     * It is noon in a frozen world, and a zombie in daylight burns. The clock
#       goes to midnight for this scenario, or the damage read back is a fire.
#
#   `NoAI` is what keeps the target where it was put: a mob with no AI does not
#   walk, so the distance in the table is the distance the game measured.

DAMAGE_DISTANCES = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
DAMAGE_HEALTH = 1000.0


def measure_damage(out: Path, trials: int) -> None:
    y = BENCH_Y
    check_y(y, y - 1)
    server, world = start("damage")
    result: dict = {"trials": trials, "health": DAMAGE_HEALTH, "power": 4,
                    "distances": DAMAGE_DISTANCES}
    try:
        server.batch(["time set midnight", "gamerule doFireTick false"])
        forceload(server, -48, -48, 48, 48)
        fill(server, -40, y - 1, -40, 40, y - 1, 40, "minecraft:barrier")
        fill(server, -40, y, -40, 40, y + 6, 40, "minecraft:air")
        damage = {d: [] for d in DAMAGE_DISTANCES}
        motion = {d: [] for d in DAMAGE_DISTANCES}
        for trial in range(trials):
            server.batch(["kill @e[type=minecraft:zombie]"])
            summons = []
            for d in DAMAGE_DISTANCES:
                summons.append(
                    f"summon minecraft:zombie {d}.0 {y}.0 0.0 "
                    f'{{Tags:["d{d}"],NoAI:1b,Silent:1b,IsBaby:0b,'
                    f"PersistenceRequired:1b,Health:{DAMAGE_HEALTH}f,"
                    f'Attributes:[{{Name:"generic.max_health",Base:{DAMAGE_HEALTH}}}]}}')
            server.batch(summons)
            wait_ticks(server, 4)
            server.batch([f"summon minecraft:tnt 0.0 {y}.0 0.0 "
                          "{Fuse:0,NoGravity:1b,Motion:[0.0,0.0,0.0]}"])
            wait_ticks(server, 2)
            for d in DAMAGE_DISTANCES:
                selector = f"@e[type=minecraft:zombie,tag=d{d},limit=1]"
                for line in server.batch([f"data get entity {selector} Health"]):
                    match = re.search(r"following entity data: ([0-9.]+)", line)
                    if match:
                        damage[d].append(round(DAMAGE_HEALTH - float(match.group(1)), 4))
                for line in server.batch([f"data get entity {selector} Motion"]):
                    match = re.search(r"following entity data: \[(.*)\]", line)
                    if match:
                        motion[d].append([float(v.strip().rstrip("d"))
                                          for v in match.group(1).split(",")])
            print(f"  trial {trial + 1}/{trials}: "
                  + " ".join(f"{d}:{damage[d][-1] if damage[d] else '-'}"
                             for d in DAMAGE_DISTANCES))
        result["damage"] = {str(d): damage[d] for d in DAMAGE_DISTANCES}
        result["motion"] = {str(d): motion[d] for d in DAMAGE_DISTANCES}
        for d in DAMAGE_DISTANCES:
            if damage[d]:
                print(f"  {d:3d} blocs : degats {sorted(set(damage[d]))} "
                      f"recul {motion[d][0] if motion[d] else '-'}")
    finally:
        server.stop()
    with open(out, "w") as f:
        json.dump(result, f, indent=1)


# ── scenario: drops ─────────────────────────────────────────────────────────
#
# What share of what a charge breaks it gives back.
#
# Counting item **entities** would undercount: dropped stone merges into stacks
# of up to 64 within a tick, and "Killed 7 entities" is then seven stacks and
# not seven stones. The count goes through a scoreboard, summing `Item.Count`
# over every item entity — which is the number the yield is a ratio of.


def measure_drops(out: Path, trials: int) -> None:
    y = BENCH_Y
    check_y(y - 5)
    server, world = start("drops")
    result: dict = {"trials": trials}
    try:
        forceload(server, -48, -48, 48, 48)
        server.batch(["gamerule doTileDrops true",
                      "scoreboard objectives add blast dummy"])
        broken, dropped = [], []
        for trial in range(trials):
            server.batch(["kill @e[type=minecraft:item]"])
            fill(server, -8, y - 5, -8, 8, y + 5, 8, "minecraft:stone")
            server.batch([f"summon minecraft:tnt 0.5 {y}.0 0.5 "
                          "{Fuse:0,NoGravity:1b,Motion:[0.0,0.0,0.0]}"])
            wait_ticks(server, 20)
            count = None
            lines = server.batch([
                "execute as @e[type=minecraft:item] store result score @s blast "
                "run data get entity @s Item.Count",
                "scoreboard players set #total blast 0",
                "execute as @e[type=minecraft:item] run "
                "scoreboard players operation #total blast += @s blast",
                "scoreboard players get #total blast"])
            for line in lines:
                match = re.search(r"\[#total\] has (\d+) ", line)
                if match:
                    count = int(match.group(1))
            server.batch(["kill @e[type=minecraft:item]"])
            save(server)
            cells = [(dx, y + dy, dz) for dy in range(-5, 6)
                     for dz in range(-8, 9) for dx in range(-8, 9)]
            states = read_states(world, cells)
            gone = sum(1 for state in states if name_of(state) != "minecraft:stone")
            broken.append(gone)
            dropped.append(count)
            print(f"  trial {trial + 1}/{trials}: broke {gone}, dropped {count}")
        result["broken"] = broken
        result["dropped"] = dropped
        pairs = [(b, d) for b, d in zip(broken, dropped) if d is not None]
        total_b = sum(b for b, _ in pairs)
        total_d = sum(d for _, d in pairs)
        result["yield"] = round(total_d / total_b, 4) if total_b else None
        print(f"  {total_d}/{total_b} = {result['yield']}")
    finally:
        server.stop()
    with open(out, "w") as f:
        json.dump(result, f, indent=1)


# ── scenario: sources ───────────────────────────────────────────────────────


def measure_sources(out: Path, trials: int) -> None:
    """The powers, read off the craters, and the fuses, read off the entities."""
    y = BENCH_Y
    check_y(y - CRATER_UP - 1)
    server, world = start("sources")
    result: dict = {}
    sources = [
        ("tnt", "summon minecraft:tnt {X}.5 {Y}.0 0.5 "
                "{Fuse:0,NoGravity:1b,Motion:[0.0,0.0,0.0]}"),
        ("creeper", "summon minecraft:creeper {X}.5 {Y}.0 0.5 "
                    "{NoGravity:1b,ignited:1b,Fuse:1s,NoAI:1b,Silent:1b}"),
        ("charged_creeper", "summon minecraft:creeper {X}.5 {Y}.0 0.5 "
                            "{NoGravity:1b,ignited:1b,Fuse:1s,powered:1b,NoAI:1b,"
                            "Silent:1b}"),
    ]
    try:
        for index, _ in enumerate(sources):
            cx = index * CRATER_SPACING
            forceload(server, cx - 24, -24, cx + 24, 24)
        counts = {name: {} for name, _ in sources}
        for trial in range(trials):
            for index, (name, template) in enumerate(sources):
                cx = index * CRATER_SPACING
                fill(server, cx - CRATER_HALF - 1, y - CRATER_UP - 1, -CRATER_HALF - 1,
                     cx + CRATER_HALF + 1, y + CRATER_UP + 1, CRATER_HALF + 1,
                     "minecraft:dirt")
            server.batch([t.replace("{X}", str(index * CRATER_SPACING)).replace("{Y}", str(y))
                          for index, (_, t) in enumerate(sources)])
            wait_ticks(server, 8)
            server.batch(["kill @e[type=minecraft:tnt]", "kill @e[type=minecraft:creeper]",
                          "kill @e[type=minecraft:item]",
                          "kill @e[type=minecraft:falling_block]"])
            save(server)
            for index, (name, _) in enumerate(sources):
                cx = index * CRATER_SPACING
                cells = [(cx + dx, y + dy, dz)
                         for dy in range(-CRATER_UP, CRATER_UP + 1)
                         for dz in range(-CRATER_HALF, CRATER_HALF + 1)
                         for dx in range(-CRATER_HALF, CRATER_HALF + 1)]
                states = read_states(world, cells)
                for (bx, by, bz), state in zip(cells, states):
                    if name_of(state) != "minecraft:dirt":
                        key = f"{bx - cx},{by - y},{bz}"
                        counts[name][key] = counts[name].get(key, 0) + 1
            print(f"  trial {trial + 1}/{trials}")
        result["counts"] = counts
        for name, _ in sources:
            ever = len(counts[name])
            always = sum(1 for v in counts[name].values() if v == trials)
            print(f"  {name:18s} union {ever:5d}  intersection {always:5d}")

        # The creeper's own countdown, read off the entity while it burns.
        fuses = []
        for attempt in range(10):
            server.batch(["kill @e[type=minecraft:creeper]",
                          f"summon minecraft:creeper 200.5 {y}.0 0.5 "
                          "{NoGravity:1b,ignited:1b,NoAI:1b}"])
            time.sleep(0.02 * attempt)
            for line in server.batch(["data get entity "
                                      "@e[type=minecraft:creeper,limit=1] Fuse"]):
                m = re.search(r"following entity data: (\d+)", line)
                if m:
                    fuses.append(int(m.group(1)))
        server.batch(["kill @e[type=minecraft:creeper]"])
        result["creeper_fuse"] = {"samples": fuses, "ticks": max(fuses) if fuses else None}
        print(f"  creeper fuse {result['creeper_fuse']['ticks']} (samples {fuses})")
    finally:
        server.stop()
    with open(out, "w") as f:
        json.dump(result, f, indent=1)


def measure_resistance_gap(out: Path, trials: int) -> None:
    measure_resistance(out, trials, gap=GAPS["resistance_gap"])




# ── scenario: table ─────────────────────────────────────────────────────────
#
# The bench gives a **penetration score** per block: the sum, over the nine
# cells of the row, of how often that cell broke. It is monotone in blast
# resistance and in nothing else. What it does not give is a number in the
# game's own units — so the number comes from PrismarineJS/minecraft-data (MIT)
# and the bench is what confronts it, exactly as scripts/measure_hardness.py
# does for hardness.
#
# What the confrontation can catch, and does:
#   * a block whose score sits outside its own resistance class;
#   * a class ordered the wrong way round against another;
#   * a block the candidate table does not mention at all.
# What it cannot catch is a value inside a band the bench does not separate,
# and those bands are written into the output rather than glossed over.

CANDIDATE_URL = ("https://raw.githubusercontent.com/PrismarineJS/minecraft-data/"
                 "master/data/pc/1.20/blocks.json")

# Blocks whose row reads something other than their resistance, each with the
# mechanic that does it. Named rather than dropped quietly: a bench that cannot
# reach a block has to say which block and why.
CONFOUNDED = {
    "minecraft:tnt":
        "chain-detonates — the row is lit by the charge and blows itself up",
}
for _coral in ("tube", "brain", "bubble", "fire", "horn"):
    for _suffix in ("", "_fan", "_wall_fan", "_block"):
        CONFOUNDED[f"minecraft:{_coral}_coral{_suffix}"] = (
            "living coral dies out of water — the row turns into its dead "
            "version and reads as destroyed")


def fetch_candidate() -> dict:
    """The candidate table, cached next to the measurements."""
    cached = NORMALIZED / "blast_candidate.json"
    if not cached.is_file():
        import urllib.request
        with urllib.request.urlopen(CANDIDATE_URL, timeout=60) as response:
            cached.write_bytes(response.read())
    with open(cached) as f:
        return {f"minecraft:{b['name']}": float(b["resistance"]) for b in json.load(f)}


def scores(doc: dict) -> dict[str, float]:
    trials = doc["trials"]
    return {name: sum(cells) / trials for name, cells in doc["counts"].items()}


def measure_table(out: Path, trials: int) -> None:
    candidate = fetch_candidate()
    benches = []
    for label, filename in (("row", "blast_row.json"), ("gap", "blast_row_gap.json")):
        path = NORMALIZED / filename
        if path.is_file():
            with open(path) as f:
                benches.append((label, json.load(f)))
    if not benches:
        raise RuntimeError("no bench data — run measure_blast.py resistance first")

    per_bench = {label: scores(doc) for label, doc in benches}
    measured_on = sorted(set().union(*(set(s) for s in per_bench.values())))
    refused = sorted(set(candidate) - set(measured_on) - DYNAMIC - FLOODS)

    # Group by the candidate value and look at the spread inside each group.
    report: dict = {"benches": {label: doc["trials"] for label, doc in benches},
                    "groups": {}, "deviations": [], "unseparated": {}}
    ok, flagged = 0, []
    for label, score in per_bench.items():
        classes: dict[float, list[str]] = {}
        for name, value in score.items():
            classes.setdefault(candidate[name], []).append(name)
        group_stats = {}
        for value, members in sorted(classes.items()):
            clean = [score[n] for n in members if n not in CONFOUNDED]
            if not clean:
                continue
            clean.sort()
            median = clean[len(clean) // 2]
            group_stats[value] = {"n": len(clean), "median": round(median, 4),
                                  "min": round(clean[0], 4), "max": round(clean[-1], 4)}
            for n in members:
                if n in CONFOUNDED:
                    continue
                if abs(score[n] - median) > 0.4:
                    flagged.append([label, n, value, round(score[n], 4), round(median, 4)])
                else:
                    ok += 1
        report["groups"][label] = {str(k): v for k, v in group_stats.items()}
        # Which candidate values this bench cannot tell apart.
        #
        # Compared on the class **median**, not on the min and max: the extremes
        # are one block each and a single confounded block would merge two
        # classes that the bulk of their members separate cleanly. Two classes
        # count as separated when their medians differ by more than 0.1 of a
        # cell — three times the spread a clean class shows over 16 shots.
        values = sorted(group_stats)
        bands, current = [], [values[0]]
        for previous, value in zip(values, values[1:]):
            if abs(group_stats[previous]["median"] - group_stats[value]["median"]) <= 0.1:
                current.append(value)
            else:
                bands.append(current)
                current = [value]
        bands.append(current)
        report["unseparated"][label] = [b for b in bands if len(b) > 1]
        report.setdefault("separated", {})[label] = sum(1 for b in bands if len(b) == 1)

    report["deviations"] = flagged
    report["confounded"] = {k: v for k, v in sorted(CONFOUNDED.items()) if k in candidate}
    report["refused"] = refused
    doc = {
        "$comment": "Resistance a l'explosion. Candidat repris de "
                    "PrismarineJS/minecraft-data (MIT), pc/1.20, puis confronte bloc par bloc "
                    "a un vrai serveur 1.20.1 par scripts/measure_blast.py : un banc de neuf "
                    "cellules du meme bloc en ligne droite devant une charge, K fois, dont la "
                    "profondeur de penetration est monotone en resistance. Voir "
                    "docs/provenance/explosions.md.",
        "version": "1.20.1",
        "source": "PrismarineJS/minecraft-data pc/1.20 blocks.json",
        "count": len(candidate),
        "measured": len(measured_on),
        "confirmed": ok,
        "blocks": {name: candidate[name] for name in sorted(candidate)},
        "report": report,
    }
    with open(out, "w") as f:
        json.dump(doc, f, indent=1)
    print(f"  candidate values ... {len(set(candidate.values()))} distinct over "
          f"{len(candidate)} blocks")
    print(f"  measured on a bench  {len(measured_on)}")
    print(f"  inside their class   {ok}")
    print(f"  deviations ......... {len(flagged)}")
    for row_ in flagged:
        print(f"      {row_[0]:4s} {row_[1]:45s} R={row_[2]} S={row_[3]} (class {row_[4]})")
    print(f"  confounded ......... {len(report['confounded'])}")
    print(f"  never stood up ..... {len(refused)}")
    for label, bands in report["unseparated"].items():
        print(f"  {label}: bands the bench does not separate: {bands}")


SCENARIOS = {
    "resistance": measure_resistance,
    "table": measure_table,
    "resistance_gap": measure_resistance_gap,
    "crater": measure_crater,
    "damage": measure_damage,
    "drops": measure_drops,
    "sources": measure_sources,
}

OUTPUT_NAME = {"resistance": "blast_row.json",
               "resistance_gap": "blast_row_gap.json",
               "table": "blast_resistance.json"}

DEFAULT_TRIALS = {"resistance": 24, "resistance_gap": 24, "crater": 30,
                  "damage": 12, "drops": 20, "sources": 20, "table": 0}


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] not in SCENARIOS:
        print(__doc__)
        return 2
    name = sys.argv[1]
    trials = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_TRIALS[name]
    NORMALIZED.mkdir(parents=True, exist_ok=True)
    print(f"── {name} ({trials} trials) ──")
    SCENARIOS[name](NORMALIZED / OUTPUT_NAME.get(name, f"blast_{name}.json"), trials)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
