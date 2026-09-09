#!/usr/bin/env python3
"""Ask a real 1.20.1 server how redstone behaves.

None of this is in Mojang's generated reports — redstone is Java code from end
to end. What can be measured is the *result*: build a circuit with `/setblock`,
trigger it, let the server settle, save, and read the block states back out of
the region files with `ov_inspect state`.

Seven scenarios, each answering one question the implementation cannot guess:

  conductors  Which blocks relay strong power? A lever on top of the candidate
              strongly powers it; a lone wire beside it reads 15 if and only if
              the candidate conducts. The same grid run **without** the lever
              says which blocks are power sources in their own right, so one
              pass produces both tables and neither can be confused for the
              other.

  wire        Sixteen wire blocks in a row from a lever. The `power` of each.

  repeater    A repeater locked by a second one on its side, read as the
              `locked` property; and one repeater per delay, whose output is
              read after the circuit has settled.

  comparator  Both modes against every combination of back and side input that
              a repeater chain can produce, and a chest filled item by item.

  torch       A torch wired back into the block it stands on: the burn-out
              oscillator. Left running, then read.

  piston      A line of blocks in front of a piston, from 1 to 14 long. Which
              lengths move and which refuse.

  qc          Quasi-connectivity, on its own: a redstone block diagonally above
              a piston, touching nothing the piston touches.

  ticks       The `block_ticks` list a chunk carries on disk, captured by
              triggering a slow repeater and saving in the same console batch.

Usage:
    python3 scripts/measure_redstone.py <scenario> [output.json]
    python3 scripts/measure_redstone.py all

Writes data/vanilla/1.20.1/normalized/redstone_<scenario>.json.
"""
from __future__ import annotations

import json
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from measure_entities import Server  # noqa: E402

NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
GENERATED = ROOT / "data" / "vanilla" / "1.20.1" / "generated" / "reports"
RUN = ROOT / "run" / "redstone-oracle"
INSPECT = ROOT / "build" / "macos-debug" / "bin" / "ov_inspect"
PORT = 25611

# Superflat: bedrock at -64, dirt at -63 and -62, grass at -61. The first free
# layer is -60, and everything here is built on that floor.
FLOOR_Y = -61
Y = -60

# The blocks the game replaces or destroys the moment they are placed. Placing
# them measures the game's reaction, not the block. The same set the light and
# sturdiness measurements skip, for the same reason.
DYNAMIC = {
    "minecraft:water", "minecraft:lava", "minecraft:fire", "minecraft:soul_fire",
    "minecraft:bubble_column", "minecraft:moving_piston", "minecraft:nether_portal",
    "minecraft:end_portal", "minecraft:end_gateway",
    "minecraft:air", "minecraft:cave_air", "minecraft:void_air",
}


def blocks_report() -> dict:
    with open(GENERATED / "blocks.json") as f:
        return json.load(f)


def freeze(server: Server) -> None:
    """Stop everything that would change the world behind the measurement."""
    server.batch([
        "gamerule randomTickSpeed 0",
        "gamerule doFireTick false",
        "gamerule doDaylightCycle false",
        "gamerule doMobSpawning false",
        "gamerule doTileDrops false",
        "gamerule commandBlockOutput false",
        "gamerule sendCommandFeedback false",
        "difficulty peaceful",
        "time set noon",
        "weather clear 1000000",
    ])


def forceload(server: Server, x0: int, z0: int, x1: int, z1: int) -> None:
    """Hold a rectangle of chunks. `forceload add` caps at 256 chunks and fails
    silently past it, so this walks the area in 128-block tiles."""
    for x in range(x0, x1 + 1, 128):
        for z in range(z0, z1 + 1, 128):
            server.batch([f"forceload add {x} {z} {min(x + 127, x1)} {min(z + 127, z1)}"],
                         timeout=300.0)
    time.sleep(3.0)


def fill(server: Server, x0: int, y0: int, z0: int, x1: int, y1: int, z1: int,
         block: str) -> None:
    """`/fill` caps at 32768 blocks. Sliced along z so a wide plate still works."""
    span = (x1 - x0 + 1) * (y1 - y0 + 1)
    depth = max(1, 32000 // max(1, span))
    for z in range(z0, z1 + 1, depth):
        server.batch([f"fill {x0} {y0} {z} {x1} {y1} {min(z + depth - 1, z1)} {block}"],
                     timeout=300.0)


def read_states(world: Path, positions: list[tuple[int, int, int]]) -> list[str]:
    """Block states as the save holds them, one per position, in order."""
    queries = "".join(f"{x} {y} {z}\n" for x, y, z in positions)
    out = subprocess.run([str(INSPECT), "state", str(world)], input=queries,
                         capture_output=True, text=True, check=True).stdout.splitlines()
    if len(out) != len(positions):
        raise RuntimeError(f"asked {len(positions)} states, got {len(out)}")
    return [line.split(maxsplit=3)[3] if len(line.split(maxsplit=3)) > 3 else "-"
            for line in out]


def properties_of(text: str) -> dict[str, str]:
    if "[" not in text:
        return {}
    return dict(pair.split("=") for pair in text[text.index("[") + 1:-1].split(","))


def name_of(text: str) -> str:
    return text.split("[")[0]


def save(server: Server) -> None:
    server.batch(["save-all flush"], timeout=600.0)
    time.sleep(6.0)


def start(name: str) -> tuple[Server, Path]:
    directory = RUN / name
    server = Server(directory, port=PORT)
    freeze(server)
    return server, directory / "world" / "region"


# ── scenario: conductors and sources ────────────────────────────────────────
#
# Cell layout, one per block, twice (with a lever and without):
#
#     y=-59            lever[face=floor,powered=true]     .
#     y=-60   candidate                                  wire
#     y=-61   grass floor                                grass floor
#
# The lever strongly powers only the block directly beneath it. The wire, one
# block to the +X side of the candidate, therefore reads 15 exactly when the
# candidate relays that strong power — which is what "redstone conductor" means.
# The lever-less half of the grid catches a candidate that powers the wire by
# itself, so a torch is not mistaken for a conductor.
#
# Two things cost a whole run before they were understood, and both are general:
#
#   * **The wire has to be placed last.** `setblock` notifies six neighbours and
#     stops there. A lever put down after the wire is two blocks away from it —
#     diagonally — so the wire never recomputes and reads 0 for every block in
#     the game. Placing the wire last makes it work its own strength out on
#     placement. The first run said stone does not conduct redstone.
#
#   * **Four blocks carry water without saying so.** Kelp, kelp plant, seagrass
#     and tall seagrass have no `waterlogged` property and are full of water
#     anyway; one of them floods a hundred cells in every direction. Everything
#     with the property is placed `waterlogged=false` for the same reason.

CELL_DX = 4
CELL_DZ = 3
COLUMNS = 46

# Water with no `waterlogged` property to turn off. Placing one floods the
# plate; the first run lost 500 cells to a single kelp.
FLOODS = {"minecraft:kelp", "minecraft:kelp_plant", "minecraft:seagrass",
          "minecraft:tall_seagrass"}


def conductor_cells(names: list[str]) -> list[tuple[str, bool, int, int]]:
    cells = []
    for index, name in enumerate(names):
        for half, lever in ((0, True), (1, False)):
            n = index * 2 + half
            cells.append((name, lever, (n % COLUMNS) * CELL_DX, (n // COLUMNS) * CELL_DZ))
    return cells


def measure_conductors(out: Path) -> None:
    report = blocks_report()
    names = sorted(n for n in report if n not in DYNAMIC and n not in FLOODS)
    dry = {n for n, b in report.items() if "waterlogged" in b.get("properties", {})}
    cells = conductor_cells(names)
    width = COLUMNS * CELL_DX + 4
    depth = (len(cells) // COLUMNS + 2) * CELL_DZ

    def placed(name: str) -> str:
        return f"{name}[waterlogged=false]" if name in dry else name

    server, world = start("conductors")
    try:
        forceload(server, -16, -16, width + 16, depth + 16)
        fill(server, -8, FLOOR_Y, -8, width + 8, FLOOR_Y, depth + 8, "minecraft:stone")
        fill(server, -8, Y, -8, width + 8, Y + 2, depth + 8, "minecraft:air")

        # Three passes, in this order and no other: candidates, then levers,
        # then wires. See the note above.
        passes = [
            [f"setblock {x} {Y} {z} {placed(name)} replace" for name, _, x, z in cells],
            [f"setblock {x} {Y + 1} {z} "
             "minecraft:lever[face=floor,facing=north,powered=true] replace"
             for name, lever, x, z in cells if lever],
            [f"setblock {x + 1} {Y} {z} minecraft:redstone_wire replace"
             for name, _, x, z in cells],
        ]
        for commands in passes:
            for i in range(0, len(commands), 400):
                server.batch(commands[i:i + 400], timeout=300.0)
                time.sleep(0.15)

        # Let every neighbour update settle before the snapshot.
        time.sleep(20.0)
        save(server)
    finally:
        server.stop()

    wanted = []
    for _, _, x, z in cells:
        wanted += [(x, Y, z), (x + 1, Y, z), (x, Y + 1, z)]
    states = read_states(world, wanted)

    conducts, sources, no_lever, gone = {}, {}, [], []
    for index, (name, lever, x, z) in enumerate(cells):
        candidate, wire, above = states[index * 3:index * 3 + 3]
        if name_of(candidate) != name or name_of(wire) != "minecraft:redstone_wire":
            gone.append(name)
            continue
        power = int(properties_of(wire).get("power", "0"))
        if lever:
            if name_of(above) != "minecraft:lever":
                no_lever.append(name)
                continue
            conducts[name] = power
        else:
            sources[name] = power

    both = {n: (conducts[n], sources[n]) for n in conducts if n in sources}
    conductors = sorted(n for n, (w, wo) in both.items() if w == 15 and wo == 0)
    with open(out, "w") as f:
        json.dump({
            "$comment": "Mesure : levier sur le bloc candidat, fil a cote, pose en dernier. "
                        "Le fil lit 15 exactement quand le candidat relaie la puissance forte. "
                        "La moitie sans levier isole les blocs qui alimentent d'eux-memes. "
                        "Voir docs/provenance/redstone.md.",
            "candidates": len(names),
            "measured": len(both),
            "cannot_hold_lever": sorted(set(no_lever)),
            "did_not_survive": sorted(set(gone)),
            "conductors": conductors,
            "self_sources": {n: v for n, v in sorted(sources.items()) if v > 0},
            "raw": {n: list(v) for n, v in sorted(both.items())},
        }, f, indent=1)
    print(f"conductors: {len(names)} blocs candidats, {len(both)} mesures des deux cotes, "
          f"{len(conductors)} conducteurs, "
          f"{sum(1 for v in sources.values() if v > 0)} sources, "
          f"{len(set(no_lever))} sans levier, {len(set(gone))} disparus")


def measure_conductor_gaps(out: Path) -> None:
    """The blocks the dense grid could not answer for, given room to fail alone.

    Fifteen full cubes came back unreadable from the packed run. Spacing them
    sixteen apart says which of them are genuinely unmeasurable — coral dies out
    of water, a chorus flower has nothing to stand on, a piston with a lever on
    its head extends and eats the probe wire — and which were only collateral
    damage from a neighbour.
    """
    names = ["minecraft:brain_coral_block", "minecraft:bubble_coral_block",
             "minecraft:chorus_flower", "minecraft:fire_coral_block",
             "minecraft:horn_coral_block", "minecraft:piston", "minecraft:sticky_piston",
             "minecraft:stripped_crimson_stem", "minecraft:stripped_dark_oak_log",
             "minecraft:stripped_dark_oak_wood", "minecraft:tinted_glass", "minecraft:tnt",
             "minecraft:tube_coral_block", "minecraft:warped_hyphae",
             "minecraft:waxed_oxidized_copper"]
    cells = [(name, lever, i * 16, 0 if lever else 16)
             for i, name in enumerate(names) for lever in (True, False)]

    server, world = start("conductor-gaps")
    try:
        forceload(server, -32, -32, len(names) * 16 + 32, 48)
        fill(server, -16, FLOOR_Y, -16, len(names) * 16 + 16, FLOOR_Y, 32, "minecraft:stone")
        fill(server, -16, Y, -16, len(names) * 16 + 16, Y + 3, 32, "minecraft:air")
        server.batch([f"setblock {x} {Y} {z} {n} replace" for n, _, x, z in cells])
        server.batch([f"setblock {x} {Y + 1} {z} "
                      "minecraft:lever[face=floor,facing=north,powered=true] replace"
                      for n, lever, x, z in cells if lever])
        server.batch([f"setblock {x + 1} {Y} {z} minecraft:redstone_wire replace"
                      for n, _, x, z in cells])
        time.sleep(8.0)
        save(server)
    finally:
        server.stop()

    rows = {}
    for name, lever, x, z in cells:
        rows[f"{name}/{'lever' if lever else 'plain'}"] = read_states(
            world, [(x, Y, z), (x + 1, Y, z), (x, Y + 1, z)])
    with open(out, "w") as f:
        json.dump({"$comment": "Mesure : les quinze cubes pleins que la grille dense n'a pas "
                               "su lire, espaces de seize.",
                   "rows": rows}, f, indent=1)
    for k, v in rows.items():
        print("gap", k, v)


def measure_conductor_probe(out: Path) -> None:
    """The blocks a lever cannot be stood on, powered from the side instead.

    A piston with a lever on its head fires and eats its own probe; TNT with one
    explodes. So the strong power comes from a repeater pointing into the
    candidate instead — a repeater strongly powers the block in front of it, and
    it can stand a block away from the candidate rather than on top of it.

        redstone_block   repeater[facing=west]   candidate   wire
             x-2                  x-1                x        x+1

    Stone and glass are measured again the same way, as the control that says
    the probe agrees with the lever.
    """
    names = ["minecraft:piston", "minecraft:sticky_piston", "minecraft:tnt",
             "minecraft:chorus_flower", "minecraft:redstone_block", "minecraft:observer",
             "minecraft:stone", "minecraft:glass", "minecraft:slime_block",
             "minecraft:honey_block", "minecraft:target", "minecraft:redstone_lamp"]
    cells = [(name, i * 16, 0) for i, name in enumerate(names)]

    server, world = start("conductor-probe")
    try:
        forceload(server, -32, -32, len(names) * 16 + 32, 32)
        fill(server, -16, FLOOR_Y, -16, len(names) * 16 + 16, FLOOR_Y, 16, "minecraft:stone")
        fill(server, -16, Y, -16, len(names) * 16 + 16, Y + 3, 16, "minecraft:air")
        server.batch([f"setblock {x} {Y} {z} {n} replace" for n, x, z in cells])
        server.batch([f"setblock {x - 1} {Y} {z} "
                      "minecraft:repeater[facing=west,delay=1,powered=false,locked=false] "
                      "replace" for n, x, z in cells])
        server.batch([f"setblock {x - 2} {Y} {z} minecraft:redstone_block replace"
                      for n, x, z in cells])
        server.batch([f"setblock {x + 1} {Y} {z} minecraft:redstone_wire replace"
                      for n, x, z in cells])
        time.sleep(8.0)
        save(server)
    finally:
        server.stop()

    rows = {}
    for name, x, z in cells:
        rows[name] = read_states(world, [(x - 1, Y, z), (x, Y, z), (x + 1, Y, z)])
    with open(out, "w") as f:
        json.dump({"$comment": "Mesure : puissance forte livree par un repeteur au lieu d'un "
                               "levier, pour les blocs qu'un levier ferait reagir.",
                   "rows": rows}, f, indent=1)
    for k, v in rows.items():
        print("probe", k, v)


def measure_push(out: Path) -> None:
    """What a piston does to every block in the game.

    One cell per block:

        y=-59   redstone_block    .            .
        y=-60   piston(east)   candidate      air
                    x             x+1         x+2

    The redstone block and the candidate go down first and the piston last, so
    it checks its own power on placement. Reading the three cells back says
    which of the four answers the block gave:

        piston extended, candidate at x+2      it moved
        piston extended, x+2 empty             it broke
        piston not extended                    it refused, and stopped the push
        candidate not there at all             unmeasurable, and named

    A push reaction is per block and nothing in Mojang's reports carries it.
    """
    report = blocks_report()
    names = sorted(n for n in report if n not in DYNAMIC and n not in FLOODS)
    dry = {n for n, b in report.items() if "waterlogged" in b.get("properties", {})}
    columns = 36
    dx, dz = 5, 3
    cells = [(n, (i % columns) * dx, (i // columns) * dz) for i, n in enumerate(names)]
    width = columns * dx + 8
    depth = (len(cells) // columns + 2) * dz

    server, world = start("push")
    try:
        forceload(server, -16, -16, width + 16, depth + 16)
        fill(server, -8, FLOOR_Y, -8, width + 8, FLOOR_Y, depth + 8, "minecraft:stone")
        fill(server, -8, Y, -8, width + 8, Y + 2, depth + 8, "minecraft:air")

        def placed(name: str) -> str:
            return f"{name}[waterlogged=false]" if name in dry else name

        passes = [
            [f"setblock {x} {Y + 1} {z} minecraft:redstone_block replace" for _, x, z in cells],
            [f"setblock {x + 1} {Y} {z} {placed(n)} replace" for n, x, z in cells],
            [f"setblock {x} {Y} {z} minecraft:piston[facing=east,extended=false] replace"
             for _, x, z in cells],
        ]
        for commands in passes:
            for i in range(0, len(commands), 400):
                server.batch(commands[i:i + 400], timeout=300.0)
                time.sleep(0.15)
        time.sleep(20.0)
        save(server)
    finally:
        server.stop()

    wanted = []
    for _, x, z in cells:
        wanted += [(x, Y, z), (x + 1, Y, z), (x + 2, Y, z)]
    states = read_states(world, wanted)

    verdict, unusable = {}, []
    for index, (name, x, z) in enumerate(cells):
        piston, middle, far = states[index * 3:index * 3 + 3]
        if name_of(piston) != "minecraft:piston":
            unusable.append(name)
            continue
        extended = properties_of(piston).get("extended") == "true"
        if not extended:
            verdict[name] = "block" if name_of(middle) == name else "unknown"
            if verdict[name] == "unknown":
                unusable.append(name)
                del verdict[name]
        elif name_of(far) == name:
            verdict[name] = "normal"
        elif name_of(far) in ("minecraft:air", "minecraft:cave_air"):
            verdict[name] = "destroy"
        else:
            unusable.append(name)

    counts = {}
    for v in verdict.values():
        counts[v] = counts.get(v, 0) + 1
    with open(out, "w") as f:
        json.dump({"$comment": "Mesure : un piston par bloc, alimente par un bloc de redstone "
                               "pose au-dessus. Le bloc bouge, casse, ou refuse.",
                   "candidates": len(names), "measured": len(verdict), "counts": counts,
                   "unusable": sorted(set(unusable)),
                   "reactions": dict(sorted(verdict.items()))}, f, indent=1)
    print("push:", counts, len(set(unusable)), "inutilisables")


# ── scenario: a wire of sixteen ─────────────────────────────────────────────

def measure_wire(out: Path) -> None:
    server, world = start("wire")
    try:
        forceload(server, -16, -16, 64, 64)
        fill(server, -8, FLOOR_Y, -8, 48, FLOOR_Y, 24, "minecraft:stone")
        fill(server, -8, Y, -8, 48, Y + 3, 24, "minecraft:air")

        commands = ["setblock 0 -60 0 minecraft:redstone_block replace"]
        commands += [f"setblock {x} {Y} 0 minecraft:redstone_wire replace"
                     for x in range(1, 20)]
        # A second run, this time climbing a staircase: wire is meant to reach
        # up and over a block, and the shape properties are what say so.
        for i in range(6):
            commands.append(f"setblock {2 * i} {Y + i} 8 minecraft:stone replace")
            commands.append(f"setblock {2 * i + 1} {Y + i} 8 minecraft:stone replace")
            commands.append(f"setblock {2 * i} {Y + i + 1} 8 minecraft:redstone_wire replace")
            commands.append(f"setblock {2 * i + 1} {Y + i + 1} 8 minecraft:redstone_wire replace")
        commands.append("setblock 0 -59 8 minecraft:redstone_block replace")
        server.batch(commands, timeout=300.0)
        time.sleep(4.0)
        save(server)
    finally:
        server.stop()

    line = read_states(world, [(x, Y, 0) for x in range(1, 20)])
    stair = read_states(world, [(2 * i + j, Y + i + 1, 8) for i in range(6) for j in (0, 1)])
    with open(out, "w") as f:
        json.dump({
            "$comment": "Mesure : bloc de redstone puis dix-neuf fils en ligne, et un "
                        "escalier de six marches. Voir docs/provenance/redstone.md.",
            "line": [{"x": x, "state": s} for x, s in zip(range(1, 20), line)],
            "staircase": stair,
        }, f, indent=1)
    print("wire:", [properties_of(s).get("power") for s in line])


# ── scenario: repeaters ─────────────────────────────────────────────────────

def measure_repeater(out: Path) -> None:
    server, world = start("repeater")
    try:
        forceload(server, -16, -16, 64, 64)
        fill(server, -8, FLOOR_Y, -8, 48, FLOOR_Y, 40, "minecraft:stone")
        fill(server, -8, Y, -8, 48, Y + 3, 40, "minecraft:air")

        commands = []
        # One row per delay: source, repeater, output wire.
        for d in range(1, 5):
            z = d * 4
            commands += [
                f"setblock 0 {Y} {z} minecraft:redstone_block replace",
                f"setblock 1 {Y} {z} minecraft:repeater[facing=west,delay={d},powered=false,"
                "locked=false] replace",
                f"setblock 2 {Y} {z} minecraft:redstone_wire replace",
            ]
        # A locked repeater: R faces west and is fed from the west; L sits on
        # R's north side pointing south into it, and is itself powered.
        z = 24
        commands += [
            f"setblock 0 {Y} {z} minecraft:redstone_block replace",
            f"setblock 1 {Y} {z} minecraft:repeater[facing=west,delay=1,powered=false,"
            "locked=false] replace",
            f"setblock 2 {Y} {z} minecraft:redstone_wire replace",
            f"setblock 1 {Y} {z - 1} minecraft:repeater[facing=north,delay=1,powered=false,"
            "locked=false] replace",
            f"setblock 1 {Y} {z - 2} minecraft:redstone_block replace",
        ]
        # The same shape with the side repeater unpowered, as the control.
        z = 32
        commands += [
            f"setblock 0 {Y} {z} minecraft:redstone_block replace",
            f"setblock 1 {Y} {z} minecraft:repeater[facing=west,delay=1,powered=false,"
            "locked=false] replace",
            f"setblock 2 {Y} {z} minecraft:redstone_wire replace",
            f"setblock 1 {Y} {z - 1} minecraft:repeater[facing=north,delay=1,powered=false,"
            "locked=false] replace",
        ]
        server.batch(commands, timeout=300.0)
        time.sleep(5.0)
        save(server)
    finally:
        server.stop()

    rows = {}
    for d in range(1, 5):
        z = d * 4
        rows[f"delay{d}"] = read_states(world, [(1, Y, z), (2, Y, z)])
    rows["locked"] = read_states(world, [(1, Y, 24), (2, Y, 24), (1, Y, 23)])
    rows["unlocked"] = read_states(world, [(1, Y, 32), (2, Y, 32), (1, Y, 31)])
    with open(out, "w") as f:
        json.dump({"$comment": "Mesure : un repeteur par delai, plus un repeteur verrouille "
                               "par un repeteur lateral alimente et son temoin non alimente.",
                   "rows": rows}, f, indent=1)
    for k, v in rows.items():
        print("repeater", k, v)


# ── scenario: comparators ───────────────────────────────────────────────────

def measure_comparator(out: Path) -> None:
    """Both modes, against every pair of inputs a wire can deliver.

    One cell, with the comparator facing east so its input is on the east side
    and its answer comes out to the west:

        z-18 .. z-1   side chain, running north from the comparator's flank
        z             output wire | comparator | back chain, running east
                          X-1          X            X+1 ..

    A wire of length `16 - level` between a redstone block and the comparator
    delivers exactly `level`, since each block costs one.

    The first attempt laid the cells two blocks apart and measured nothing but
    itself: a side chain sixteen long walks straight through the next four rows,
    and every output read 13 to 15. The cells are twenty-four apart now, which
    is one more than the widest cell.
    """
    levels = [0, 3, 6, 9, 12, 15]
    pairs = [(b, s, m) for b in levels for s in levels for m in ("compare", "subtract")]
    span = 24
    columns = 9
    cells = [(b, s, m, (i % columns) * span + 4, (i // columns) * span + 24)
             for i, (b, s, m) in enumerate(pairs)]
    width = columns * span + 32
    depth = (len(cells) // columns + 2) * span + 32

    server, world = start("comparator")
    try:
        forceload(server, -16, -16, width + 16, depth + 16)
        fill(server, -8, FLOOR_Y, -8, width, FLOOR_Y, depth, "minecraft:stone")
        fill(server, -8, Y, -8, width, Y + 2, depth, "minecraft:air")

        commands = []
        for back, side, mode, x, z in cells:
            commands.append(f"setblock {x} {Y} {z} "
                            f"minecraft:comparator[facing=east,mode={mode},powered=false] "
                            "replace")
            if back > 0:
                k = 16 - back
                for i in range(1, k + 1):
                    commands.append(f"setblock {x + i} {Y} {z} minecraft:redstone_wire replace")
                commands.append(f"setblock {x + k + 1} {Y} {z} minecraft:redstone_block replace")
            if side > 0:
                m = 16 - side
                for i in range(1, m + 1):
                    commands.append(f"setblock {x} {Y} {z - i} minecraft:redstone_wire replace")
                commands.append(f"setblock {x} {Y} {z - m - 1} minecraft:redstone_block replace")
            # The output wire last, so it works its own strength out on placement.
            commands.append(f"setblock {x - 1} {Y} {z} minecraft:redstone_wire replace")
        for i in range(0, len(commands), 400):
            server.batch(commands[i:i + 400], timeout=300.0)
            time.sleep(0.15)
        time.sleep(15.0)
        save(server)
    finally:
        server.stop()

    result = []
    for back, side, mode, x, z in cells:
        st = read_states(world, [(x - 1, Y, z), (x, Y, z), (x + 1, Y, z), (x, Y, z - 1)])
        result.append({"back": back, "side": side, "mode": mode,
                       "output": int(properties_of(st[0]).get("power", "-1")),
                       "back_wire": int(properties_of(st[2]).get("power", "-1"))
                       if name_of(st[2]) == "minecraft:redstone_wire" else 0,
                       "side_wire": int(properties_of(st[3]).get("power", "-1"))
                       if name_of(st[3]) == "minecraft:redstone_wire" else 0,
                       "comparator": st[1]})

    def expected(back: int, side: int, mode: str) -> int:
        return max(0, back - side) if mode == "subtract" else (0 if side > back else back)

    agree = sum(1 for r in result
                if r["output"] == expected(r["back_wire"], r["side_wire"], r["mode"]))
    calibrated = sum(1 for r in result
                     if r["back_wire"] == r["back"] and r["side_wire"] == r["side"])
    with open(out, "w") as f:
        json.dump({"$comment": "Mesure : comparateur alimente par deux fils de longueur "
                               "calibree, dans les deux modes. Les niveaux d'entree sont relus "
                               "et non supposes.",
                   "cells": len(result), "calibrated": calibrated, "agree": agree,
                   "pairs": result}, f, indent=1)
    print(f"comparator: {len(result)} cellules, {calibrated} calibrees, {agree} conformes")
    for r in result:
        if r["output"] != expected(r["back_wire"], r["side_wire"], r["mode"]):
            print("  ecart", r["back_wire"], r["side_wire"], r["mode"], "->", r["output"])


def measure_container(out: Path) -> None:
    """What a comparator reads off a chest, slot by slot.

    A single chest is twenty-seven slots. Filling `n` of them with a full stack
    sweeps the fill fraction across its whole range, which one slot with `n`
    items cannot do — a lone stack is a fortieth of a chest and reads 1 all the
    way up.
    """
    server, world = start("container")
    try:
        forceload(server, -16, -16, 32, 32 + 28 * 6)
        fill(server, -8, FLOOR_Y, -8, 16, FLOOR_Y, 28 * 6 + 8, "minecraft:stone")
        fill(server, -8, Y, -8, 16, Y + 2, 28 * 6 + 8, "minecraft:air")

        commands = []
        for n in range(0, 28):
            z = n * 6
            commands += [
                f"setblock 0 {Y} {z} minecraft:chest[facing=north,type=single,"
                "waterlogged=false] replace",
                f"setblock 1 {Y} {z} minecraft:comparator[facing=west,mode=compare,"
                "powered=false] replace",
                f"setblock 2 {Y} {z} minecraft:redstone_wire replace",
            ]
        server.batch(commands, timeout=300.0)
        time.sleep(2.0)
        commands = []
        for n in range(0, 28):
            z = n * 6
            for slot in range(n):
                commands.append(f"item replace block 0 {Y} {z} container.{slot} "
                                "with minecraft:stone 64")
        for i in range(0, len(commands), 300):
            server.batch(commands[i:i + 300], timeout=300.0)
        time.sleep(8.0)
        save(server)
    finally:
        server.stop()

    rows = []
    for n in range(0, 28):
        z = n * 6
        st = read_states(world, [(2, Y, z), (1, Y, z)])
        rows.append({"full_slots": n,
                     "output": int(properties_of(st[0]).get("power", "-1")),
                     "comparator": st[1]})

    def expected(n: int) -> int:
        import math
        return min(15, int(math.floor(n / 27.0 * 14.0)) + (1 if n else 0))

    agree = sum(1 for r in rows if r["output"] == expected(r["full_slots"]))
    with open(out, "w") as f:
        json.dump({"$comment": "Mesure : un coffre simple, n emplacements sur vingt-sept "
                               "remplis d'une pile complete, lu par un comparateur.",
                   "agree": agree, "rows": rows}, f, indent=1)
    print(f"container: {agree}/{len(rows)} conformes a floor(14*n/27)+1")
    print("  ", [(r["full_slots"], r["output"]) for r in rows])


def probe_lit(server: Server, x: int, y: int, z: int) -> bool:
    """Read a torch without saving the world.

    `execute if block` prints "Test passed" or "Test failed" on the console, so
    the answer arrives in the same second it is asked for. Which matters here:
    a burnt-out torch relights a hundred and sixty ticks later, and a save that
    takes eight seconds to come back has already missed it.
    """
    lines = server.batch(
        [f"execute if block {x} {y} {z} minecraft:redstone_torch[lit=true]"], timeout=60.0)
    return any("Test passed" in line for line in lines)


def measure_torch(out: Path) -> None:
    """The burn-out: a torch made to change too often gives up.

    Each cell is

        y=-59       torch       poke block
        y=-60   support(stone)      .
        y=-61      lever[ceiling]   .
                    x=0            x=1

    The lever hangs under the support and strongly powers it, which puts the
    torch out; flipping it back lets the torch return. Two ticks each way, so a
    full on-off cycle is four ticks and eight of them fit inside the sixty-tick
    window with room to spare.

    Three traps, and only the first is about redstone:

      * Driving the support through a **repeater** cannot burn a torch out: the
        repeater's two ticks plus the torch's two make an eight-tick cycle, and
        eight of those need sixty-four ticks — four ticks past the window. A
        torch behind a repeater is safe however fast you drive it.

      * `setblock` notifies six neighbours and stops. The lever is two blocks
        from the torch, so the torch never hears the change on its own. Every
        toggle therefore also flips a block **beside** the torch, whose own ring
        does include it.

      * A burnt-out torch comes back after a hundred and sixty ticks — eight
        seconds. Reading it out of a save takes longer than that, so the world
        on disk shows a lit torch and says nothing happened. The reading has to
        be live.
    """
    rows = {
        # A toggle every three ticks; the circuit settles in two.
        "fastest": (0, 48, 0.12),
        "fast": (8, 32, 0.2),
        # A toggle every thirty ticks: no eight of them ever share a window.
        "slow": (16, 8, 1.5),
    }
    server, _ = start("torch")
    result: dict[str, object] = {}
    try:
        server.batch(["gamerule sendCommandFeedback true"])
        forceload(server, -16, -16, 64, 80)
        fill(server, -8, FLOOR_Y, -8, 48, FLOOR_Y, 56, "minecraft:stone")
        fill(server, -8, Y, -8, 48, Y + 4, 56, "minecraft:air")

        setup = []
        for _, (z, _, _) in rows.items():
            setup += [
                f"setblock 0 {Y} {z} minecraft:stone replace",
                f"setblock 0 {Y + 1} {z} minecraft:redstone_torch[lit=true] replace",
            ]
        setup += [
            f"setblock 0 {Y} 32 minecraft:stone replace",
            f"setblock 1 {Y} 32 minecraft:repeater[facing=east,delay=1,powered=false,"
            "locked=false] replace",
            f"setblock 0 {Y + 1} 32 minecraft:redstone_torch[lit=true] replace",
            f"setblock 0 {Y} 40 minecraft:stone replace",
            f"setblock 0 {Y + 1} 40 minecraft:redstone_torch[lit=true] replace",
        ]
        server.batch(setup, timeout=300.0)
        time.sleep(2.0)

        # The lever alone, once, to prove the circuit works at all before any
        # conclusion is drawn from it failing to.
        z = rows["fastest"][0]
        server.batch([f"setblock 0 {Y - 1} {z} "
                      "minecraft:lever[face=ceiling,facing=north,powered=true] replace",
                      f"setblock 1 {Y + 1} {z} minecraft:stone replace"])
        time.sleep(0.6)
        result["one_toggle_off"] = not probe_lit(server, 0, Y + 1, z)
        server.batch([f"setblock 0 {Y - 1} {z} "
                      "minecraft:lever[face=ceiling,facing=north,powered=false] replace",
                      f"setblock 1 {Y + 1} {z} minecraft:air replace"])
        time.sleep(0.6)
        result["one_toggle_back"] = probe_lit(server, 0, Y + 1, z)

        for name, (z, count, gap) in rows.items():
            for i in range(count):
                on = i % 2 == 0
                lever = ("minecraft:lever[face=ceiling,facing=north,"
                         f"powered={'true' if on else 'false'}]")
                poke = "minecraft:stone" if on else "minecraft:air"
                server.send(f"setblock 0 {Y - 1} {z} {lever} replace",
                            f"setblock 1 {Y + 1} {z} {poke} replace")
                time.sleep(gap)
            # Leave the lever at rest. A torch that still works comes straight
            # back on; a burnt-out one stays dark for a hundred and sixty ticks.
            server.batch([f"setblock 0 {Y - 1} {z} "
                          "minecraft:lever[face=ceiling,facing=north,powered=false] replace",
                          f"setblock 1 {Y + 1} {z} minecraft:air replace"])
            time.sleep(0.7)
            dark_now = not probe_lit(server, 0, Y + 1, z)
            time.sleep(9.0)
            lit_later = probe_lit(server, 0, Y + 1, z)
            result[name] = {"dark_with_the_lever_at_rest": dark_now,
                            "lit_nine_seconds_later": lit_later}

        server.batch([f"setblock 2 {Y} 32 minecraft:redstone_block replace"], timeout=120.0)
        time.sleep(1.0)
        result["held_off"] = not probe_lit(server, 0, Y + 1, 32)
        result["untouched"] = probe_lit(server, 0, Y + 1, 40)
    finally:
        server.stop()

    with open(out, "w") as f:
        json.dump({"$comment": "Mesure : torche basculee par un levier, lue en direct par "
                               "execute if block. Le burn-out se voit a la torche qui reste "
                               "eteinte alors que le levier est revenu au repos, et qui se "
                               "rallume neuf secondes plus tard.",
                   "rows": result}, f, indent=1)
    for k, v in result.items():
        print("torch", k, v)


# ── scenario: pistons ───────────────────────────────────────────────────────

def measure_piston(out: Path) -> None:
    lengths = list(range(0, 15))
    server, world = start("piston")
    try:
        forceload(server, -32, -32, 96, 96)
        fill(server, -16, FLOOR_Y, -16, 64, FLOOR_Y, 80, "minecraft:stone")
        fill(server, -16, Y, -16, 64, Y + 3, 80, "minecraft:air")

        commands = []
        for n in lengths:
            z = n * 4
            commands.append(
                f"setblock 0 {Y} {z} minecraft:piston[facing=east,extended=false] replace")
            for i in range(1, n + 1):
                commands.append(f"setblock {i} {Y} {z} minecraft:iron_block replace")
        server.batch(commands, timeout=300.0)
        time.sleep(2.0)
        # Power them all at once, on top of the piston: a lever on a block over
        # the piston is not adjacent to it, so this also exercises the piston's
        # own power check rather than a neighbour's.
        commands = []
        for n in lengths:
            z = n * 4
            commands.append(f"setblock 0 {Y + 1} {z} minecraft:redstone_block replace")
        server.batch(commands, timeout=300.0)
        time.sleep(6.0)
        save(server)
    finally:
        server.stop()

    result = []
    for n in lengths:
        z = n * 4
        row = read_states(world, [(x, Y, z) for x in range(0, 18)])
        result.append({"length": n, "row": row})
    with open(out, "w") as f:
        json.dump({"$comment": "Mesure : piston vers l'est avec n blocs de fer devant, "
                               "alimente par un bloc de redstone pose dessus.",
                   "rows": result}, f, indent=1)
    for r in result:
        print("piston", r["length"], [name_of(s).replace("minecraft:", "") for s in r["row"][:5]])


# ── scenario: quasi-connectivity ────────────────────────────────────────────

def measure_qc(out: Path) -> None:
    """Quasi-connectivity, isolated from everything that could explain it away.

    A piston reads the block **above** itself as if it were its own: a source
    that touches only that space still fires it. Two things have to be arranged
    before the measurement means anything.

    The source must touch the space above the piston and nothing the piston
    itself touches, so a redstone block goes diagonally — one up and one north.
    It shares no face with the piston.

    And the piston must be placed **after** the source. `setblock` notifies six
    neighbours and no further, so a source dropped two blocks away never reaches
    a piston already standing there; placing the piston last makes it check on
    its own. That is not an artefact of the measurement — it is why
    quasi-connectivity is called a block update detector — and both orders are
    recorded below.
    """
    server, world = start("qc")
    layout = {
        # A — the source first, then the piston. Diagonally above the piston,
        # so it shares no face with it.
        "qc_diagonal_above": (0, [
            "setblock 0 {y1} -1 minecraft:redstone_block replace",
            "setblock 0 {y} 0 minecraft:piston[facing=east,extended=false] replace",
        ]),
        # B — control: one higher again, touching neither the piston nor the
        # space above it.
        "control_two_above": (8, [
            "setblock 0 {y2} 7 minecraft:redstone_block replace",
            "setblock 0 {y} 8 minecraft:piston[facing=east,extended=false] replace",
        ]),
        # C — the piston first, the source second. Same geometry as A.
        "qc_no_update": (16, [
            "setblock 0 {y} 16 minecraft:piston[facing=east,extended=false] replace",
            "setblock 0 {y1} 15 minecraft:redstone_block replace",
        ]),
        # D — C again, then a block update delivered beside the piston.
        "qc_then_update": (24, [
            "setblock 0 {y} 24 minecraft:piston[facing=east,extended=false] replace",
            "setblock 0 {y1} 23 minecraft:redstone_block replace",
            "setblock 0 {y} 25 minecraft:stone replace",
            "setblock 0 {y} 25 minecraft:air replace",
        ]),
        # E — an ordinary adjacency, for scale: directly above the piston.
        "adjacent_above": (32, [
            "setblock 0 {y1} 32 minecraft:redstone_block replace",
            "setblock 0 {y} 32 minecraft:piston[facing=east,extended=false] replace",
        ]),
        # F — does the dispenser share it?
        "dispenser_qc": (40, [
            "setblock 0 {y1} 39 minecraft:redstone_block replace",
            "setblock 0 {y} 40 minecraft:dispenser[facing=east,triggered=false] replace",
        ]),
        # G — a lamp, an ordinary consumer, which should not have it.
        "lamp_qc": (44, [
            "setblock 0 {y1} 43 minecraft:redstone_block replace",
            "setblock 0 {y} 44 minecraft:redstone_lamp[lit=false] replace",
        ]),
        # H — the piston facing up with the source above. `facing` is excluded
        # from the piston's own ring; this says whether it is excluded from the
        # quasi ring too.
        "facing_up_source_above": (48, [
            "setblock 0 {y2} 48 minecraft:redstone_block replace",
            "setblock 0 {y} 48 minecraft:piston[facing=up,extended=false] replace",
        ]),
        # I — the sticky piston, to confirm both share the rule.
        "sticky_qc": (52, [
            "setblock 0 {y1} 51 minecraft:redstone_block replace",
            "setblock 0 {y} 52 minecraft:sticky_piston[facing=east,extended=false] replace",
        ]),
        # J — the dropper, which fires on a signal like the dispenser.
        "dropper_qc": (56, [
            "setblock 0 {y1} 55 minecraft:redstone_block replace",
            "setblock 0 {y} 56 minecraft:dropper[facing=east,triggered=false] replace",
        ]),
    }
    try:
        forceload(server, -32, -32, 64, 96)
        fill(server, -16, FLOOR_Y, -16, 48, FLOOR_Y, 80, "minecraft:stone")
        fill(server, -16, Y, -16, 48, Y + 4, 80, "minecraft:air")
        for name, (_, commands) in layout.items():
            server.batch([c.format(y=Y, y1=Y + 1, y2=Y + 2) for c in commands], timeout=120.0)
        time.sleep(6.0)
        save(server)
    finally:
        server.stop()

    rows = {}
    for name, (z, _) in layout.items():
        rows[name] = read_states(world, [(0, Y, z), (1, Y, z), (0, Y + 1, z)])
    with open(out, "w") as f:
        json.dump({"$comment": "Mesure : la quasi-connectivite, isolee. Un bloc de redstone en "
                               "diagonale au-dessus du piston, qui ne partage aucune face avec "
                               "lui. L'ordre de pose compte : setblock ne previent que six "
                               "voisins, donc le piston est pose en dernier.",
                   "rows": rows}, f, indent=1)
    for k, v in rows.items():
        print("qc", k, v)



# ── scenario: the block_ticks list on disk ──────────────────────────────────
#
# A minimal Anvil reader lives here rather than in ov_inspect: this is the one
# thing the C++ side cannot be used to check, since checking it is the point.

def region_chunk(path: Path, cx: int, cz: int) -> bytes | None:
    data = path.read_bytes()
    index = ((cx & 31) + (cz & 31) * 32) * 4
    offset = int.from_bytes(data[index:index + 3], "big")
    count = data[index + 3]
    if offset == 0 or count == 0:
        return None
    start = offset * 4096
    length = int.from_bytes(data[start:start + 4], "big")
    scheme = data[start + 4]
    payload = data[start + 5:start + 4 + length]
    if scheme == 1:
        return zlib.decompress(payload, 31)
    if scheme == 2:
        return zlib.decompress(payload)
    if scheme == 3:
        return payload
    raise RuntimeError(f"compression scheme {scheme} not handled")


def nbt_read(data: bytes) -> dict:
    """Just enough NBT to find one list. Refuses what it does not know."""
    pos = [0]

    def u1() -> int:
        v = data[pos[0]]
        pos[0] += 1
        return v

    def take(n: int) -> bytes:
        v = data[pos[0]:pos[0] + n]
        pos[0] += n
        return v

    def name() -> str:
        n = struct.unpack(">H", take(2))[0]
        return take(n).decode("utf-8")

    def payload(tag: int):
        if tag == 0:
            return None
        if tag == 1:
            return struct.unpack(">b", take(1))[0]
        if tag == 2:
            return struct.unpack(">h", take(2))[0]
        if tag == 3:
            return struct.unpack(">i", take(4))[0]
        if tag == 4:
            return struct.unpack(">q", take(8))[0]
        if tag == 5:
            return struct.unpack(">f", take(4))[0]
        if tag == 6:
            return struct.unpack(">d", take(8))[0]
        if tag == 7:
            return take(struct.unpack(">i", take(4))[0])
        if tag == 8:
            return name()
        if tag == 9:
            element = u1()
            count = struct.unpack(">i", take(4))[0]
            return [payload(element) for _ in range(count)]
        if tag == 10:
            body = {}
            while True:
                child = u1()
                if child == 0:
                    return body
                # The name has to come out of the stream before the payload.
                # `body[name()] = payload(child)` reads them in the other order,
                # because Python evaluates the right-hand side first — which
                # decodes the payload as a name and fails several tags later,
                # somewhere that looks nothing like the mistake.
                child_name = name()
                body[child_name] = payload(child)
        if tag == 11:
            count = struct.unpack(">i", take(4))[0]
            return list(struct.unpack(f">{count}i", take(4 * count)))
        if tag == 12:
            count = struct.unpack(">i", take(4))[0]
            return list(struct.unpack(f">{count}q", take(8 * count)))
        raise RuntimeError(f"tag id {tag} not handled")

    root = u1()
    if root != 10:
        raise RuntimeError(f"root tag is {root}, not a compound")
    name()
    return payload(10)


def measure_ticks(out: Path) -> None:
    """The `block_ticks` list a chunk carries on disk.

    Catching one is a race: a repeater's tick comes due eight ticks after it is
    triggered, and a `save-all flush` sent in the same breath still lands after
    it. Two devices with a long fuse make the race winnable:

      * **Frosted ice** schedules its own melt sixty to a hundred and twenty
        ticks out the moment it is placed — three to six seconds of window, and
        no redstone involved, so the entry is guaranteed to be there.
      * **A redstone torch held off by a lever** waits two ticks, which is short,
        but placing it and saving is cheap enough to retry until it is caught.

    Reading the same chunk twice, seconds apart, is what says whether `t` is an
    absolute tick or a countdown: an absolute one would grow with the clock.
    """
    server, world = start("ticks")
    snapshots = []
    try:
        forceload(server, -16, -16, 32, 32)
        fill(server, -8, FLOOR_Y, -8, 24, FLOOR_Y, 24, "minecraft:stone")
        fill(server, -8, Y, -8, 24, Y + 3, 24, "minecraft:air")

        for attempt in range(12):
            z = 2 + attempt
            server.batch([
                f"setblock 2 {Y} {z} minecraft:frosted_ice[age=0] replace",
                f"setblock 5 {Y} {z} minecraft:repeater[facing=west,delay=4,powered=false,"
                "locked=false] replace",
                f"setblock 8 {Y} {z} minecraft:comparator[facing=west,mode=compare,"
                "powered=false] replace",
                f"setblock 11 {Y} {z} minecraft:stone replace",
                f"setblock 11 {Y + 1} {z} minecraft:redstone_torch[lit=true] replace",
            ], timeout=120.0)
            # Trigger everything and save without waiting for anything.
            server.send(
                f"setblock 4 {Y} {z} minecraft:redstone_block replace",
                f"setblock 7 {Y} {z} minecraft:redstone_block replace",
                f"setblock 11 {Y - 1} {z} "
                "minecraft:lever[face=ceiling,facing=north,powered=true] replace",
                "save-all flush")
            server.batch([], timeout=600.0)
            chunk = region_chunk(world / "r.0.0.mca", 0, 0)
            entries = nbt_read(chunk).get("block_ticks", []) if chunk else []
            snapshots.append({"attempt": attempt, "entries": entries})
            names = {e.get("i") for e in entries}
            if any(n != "minecraft:frosted_ice" for n in names):
                break
            time.sleep(0.4)

        # A second read of a quiet chunk, four seconds later: the frosted ice is
        # still pending, and whether its `t` has fallen is the whole question.
        time.sleep(4.0)
        server.batch(["save-all flush"], timeout=600.0)
        chunk = region_chunk(world / "r.0.0.mca", 0, 0)
        later = nbt_read(chunk).get("block_ticks", []) if chunk else []
    finally:
        server.stop()

    caught = [e for s in snapshots for e in s["entries"]]
    with open(out, "w") as f:
        json.dump({"$comment": "Mesure : glace fondante (mecanisme long) et repeteur "
                               "(mecanisme court, retente), sauvegardes en vol. La seconde "
                               "lecture, quatre secondes plus tard, dit si t est un delai ou "
                               "un tick absolu.",
                   "snapshots": snapshots,
                   "four_seconds_later": later,
                   "distinct_blocks": sorted({e.get("i") for e in caught})}, f, indent=1)
    print("block_ticks caught:", json.dumps(caught[:8]))
    print("four seconds later:", json.dumps(later[:4]))


SCENARIOS = {
    "conductors": measure_conductors,
    "conductor_gaps": measure_conductor_gaps,
    "conductor_probe": measure_conductor_probe,
    "push": measure_push,
    "wire": measure_wire,
    "repeater": measure_repeater,
    "comparator": measure_comparator,
    "container": measure_container,
    "torch": measure_torch,
    "piston": measure_piston,
    "qc": measure_qc,
    "ticks": measure_ticks,
}


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] not in ({"all"} | set(SCENARIOS)):
        print(__doc__)
        return 2
    NORMALIZED.mkdir(parents=True, exist_ok=True)
    names = list(SCENARIOS) if sys.argv[1] == "all" else [sys.argv[1]]
    for name in names:
        out = Path(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[1] != "all" \
            else NORMALIZED / f"redstone_{name}.json"
        print(f"── {name} ──")
        SCENARIOS[name](out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
