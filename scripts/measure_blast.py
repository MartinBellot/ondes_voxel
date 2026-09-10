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
CELL_DX = 20   # >= row length + the reach of the next charge backwards
CELL_DZ = 12   # >= twice the reach, so no charge touches its neighbour's row


def cell_of(index: int) -> tuple[int, int]:
    return (index % COLUMNS) * CELL_DX, (index // COLUMNS) * CELL_DZ


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


def measure_resistance(out: Path, trials: int) -> None:
    names, dry = bench_names()
    y = BENCH_Y
    check_y(y, y - 1, y - 2)
    rows = (len(names) + COLUMNS - 1) // COLUMNS
    width = COLUMNS * CELL_DX + ROW + 8
    depth = rows * CELL_DZ + 8

    server, world = start("resistance")
    result: dict = {"trials": trials, "row": ROW, "power": 4}
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
                cx, cz = cell_of(index)
                floor = floor_of.get(name)
                if floor is not None:
                    commands.append(f"fill {cx} {y - 1} {cz} {cx + ROW} {y - 1} {cz} {floor}")
                commands.append(
                    f"fill {cx + 1} {y} {cz} {cx + ROW} {y} {cz} {placed(name, dry)}")
            return commands

        def read_rows(subset: list[str]) -> dict[str, list[str]]:
            cells, order = [], []
            for index, name in enumerate(names):
                if name not in subset:
                    continue
                cx, cz = cell_of(index)
                order.append(name)
                cells += [(cx + k, y, cz) for k in range(1, ROW + 1)]
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
                cx, cz = cell_of(index)
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


# ── scenario: damage ────────────────────────────────────────────────────────
#
# ⚠ `Invulnerable:1b` would make this unreadable — an invulnerable entity takes
#   nothing and reads exactly like an entity out of range. The targets here are
#   ordinary and simply have enough health to survive: a zombie with a large
#   `Health` and the matching `generic.max_health`.

DAMAGE_DISTANCES = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
DAMAGE_HEALTH = 1000.0


def measure_damage(out: Path, trials: int) -> None:
    y = BENCH_Y
    check_y(y, y - 1)
    server, world = start("damage")
    result: dict = {"trials": trials, "health": DAMAGE_HEALTH, "power": 4,
                    "distances": DAMAGE_DISTANCES}
    try:
        forceload(server, -48, -48, 48, 48)
        fill(server, -40, y - 1, -40, 40, y - 1, 40, "minecraft:barrier")
        fill(server, -40, y, -40, 40, y + 6, 40, "minecraft:air")
        samples = {d: [] for d in DAMAGE_DISTANCES}
        knock = {d: [] for d in DAMAGE_DISTANCES}
        for trial in range(trials):
            server.batch(["kill @e[type=minecraft:zombie]"])
            summons = []
            for d in DAMAGE_DISTANCES:
                summons.append(
                    f"summon minecraft:zombie {d}.0 {y}.0 0.0 "
                    "{Tags:[\"blast\"],NoAI:1b,PersistenceRequired:1b,Silent:1b,"
                    f"Health:{DAMAGE_HEALTH}f,"
                    "Attributes:[{Name:\"generic.max_health\",Base:"
                    f"{DAMAGE_HEALTH}"
                    "}],CustomName:'{\"text\":\"d" + str(d) + "\"}'}")
            server.batch(summons)
            wait_ticks(server, 4)
            server.batch([f"summon minecraft:tnt 0.5 {y}.0 0.5 "
                          "{Fuse:0,NoGravity:1b,Motion:[0.0,0.0,0.0]}"])
            wait_ticks(server, 3)
            for d in DAMAGE_DISTANCES:
                lines = server.batch([
                    "data get entity @e[type=minecraft:zombie,limit=1,sort=nearest,"
                    f"x={d}.0,y={y}.0,z=0.0,distance=..1.5] Health"])
                for line in lines:
                    m = re.search(r"following entity data: ([0-9.]+)", line)
                    if m:
                        samples[d].append(round(DAMAGE_HEALTH - float(m.group(1)), 4))
            print(f"  trial {trial + 1}/{trials}: "
                  + " ".join(f"{d}:{samples[d][-1] if samples[d] else '-'}"
                             for d in DAMAGE_DISTANCES))
            server.batch(["kill @e[type=minecraft:zombie]"])
        result["damage"] = {str(d): samples[d] for d in DAMAGE_DISTANCES}
        result["knockback"] = {str(d): knock[d] for d in DAMAGE_DISTANCES}
    finally:
        server.stop()
    with open(out, "w") as f:
        json.dump(result, f, indent=1)


# ── scenario: drops ─────────────────────────────────────────────────────────


def measure_drops(out: Path, trials: int) -> None:
    y = BENCH_Y
    check_y(y - 4)
    server, world = start("drops")
    result: dict = {"trials": trials}
    try:
        forceload(server, -48, -48, 48, 48)
        server.batch(["gamerule doTileDrops true"])
        broken, dropped = [], []
        for trial in range(trials):
            fill(server, -6, y - 4, -6, 6, y + 4, 6, "minecraft:stone")
            server.batch(["kill @e[type=minecraft:item]"])
            server.batch([f"summon minecraft:tnt 0.5 {y}.0 0.5 "
                          "{Fuse:0,NoGravity:1b,Motion:[0.0,0.0,0.0]}"])
            wait_ticks(server, 20)
            lines = server.batch(["execute store result score #n blast run "
                                  "data get entity @e[type=minecraft:item,limit=1] Item.Count",
                                  "scoreboard objectives add blast dummy"])
            # Counting items is done the blunt way: kill them and read how many
            # the console says it removed.
            killed = server.batch(["kill @e[type=minecraft:item]"])
            count = 0
            for line in killed:
                m = re.search(r"Killed (\d+) entities", line)
                if m:
                    count = int(m.group(1))
                if "Killed " in line and "entities" not in line:
                    count = 1
            save(server)
            cells = [(dx, y + dy, dz) for dy in range(-4, 5)
                     for dz in range(-6, 7) for dx in range(-6, 7)]
            states = read_states(world, cells)
            gone = sum(1 for s in states if name_of(s) != "minecraft:stone")
            broken.append(gone - 1)   # the charge's own cell was never stone
            dropped.append(count)
            print(f"  trial {trial + 1}/{trials}: broke {gone - 1}, dropped {count}")
        result["broken"] = broken
        result["dropped"] = dropped
        total_b, total_d = sum(broken), sum(dropped)
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


SCENARIOS = {
    "resistance": measure_resistance,
    "crater": measure_crater,
    "damage": measure_damage,
    "drops": measure_drops,
    "sources": measure_sources,
}

DEFAULT_TRIALS = {"resistance": 24, "crater": 30, "damage": 12, "drops": 20, "sources": 20}


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] not in SCENARIOS:
        print(__doc__)
        return 2
    name = sys.argv[1]
    trials = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_TRIALS[name]
    NORMALIZED.mkdir(parents=True, exist_ok=True)
    print(f"── {name} ({trials} trials) ──")
    SCENARIOS[name](NORMALIZED / f"blast_{name}.json", trials)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
