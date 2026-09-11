#!/usr/bin/env python3
"""Ask a real 1.20.1 server what rails and minecarts do.

Scenarios (each starts and stops its own server on OV_RAILS_PORT, default
25623, in run/rails-oracle/<scenario>/, and deletes its world on the way out):

  shapes     The exhaustive placement bench. A rail is set in the middle of
             four neighbour cells, each of which holds nothing, a rail at the
             same level, a rail one block up, or a rail one block down: 4^4 =
             256 arrangements. Six variants: the centre is a rail or a powered
             rail, set with shape north_south or east_west, next to plain rails;
             and a plain rail centre next to powered rails. 1536 cells. The
             state of the centre and of each neighbour is read off the save.

  busy       The same centre, next to neighbours that are already part of a
             line (connected at both ends) or half connected. Says whether a
             full neighbour is taken.

  power      Lines of powered rails and activator rails, flat and on a slope,
             lit by a redstone block set beside one end or the middle. Which
             rails are powered, read off the save, then again once the source
             is gone.

  carts      Minecart trajectories on fourteen lanes: straight fast and slow,
             down a slope, over powered rails, braking on unpowered ones, round
             a curve, with a passenger, off the rails, launched from a block,
             uphill, a furnace minecart, two carts colliding. Every sample is
             labelled by the `Fuse` of a witness TNT read in the same tick.
             The track's block states are read off the save and stored with the
             trajectories, so a replay builds the exact same track.

  detector   A cart resting on a detector rail: the rail's `powered`, the lamp
             beside it, and how many ticks it takes to switch off once the cart
             is gone. A comparator behind a detector rail reading a chest
             minecart with 0..N stacks.

  capture    A probe client joins and records the packets: Spawn Entity for
             every minecart type, the metadata index of each field (one NBT
             field at a time), Set Passengers (found by content), and what the
             server does with a Player Input packet from a rider.

Usage:
    python3 scripts/measure_rails.py <scenario>

Writes data/vanilla/1.20.1/normalized/rails_<scenario>.json (gitignored).
Run under the machine-wide lock:
    lockf /tmp/ov-vanilla.lock python3 scripts/measure_rails.py <scenario>
"""
from __future__ import annotations

import json
import math
import os
import re
import shutil
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from anvil_read import chunks  # noqa: E402
from capture_entity_packets import Probe, read_varint, varint  # noqa: E402
from measure_entities import Server  # noqa: E402
from measure_redstone import fill, forceload, freeze, save  # noqa: E402

NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
RUN = ROOT / "run" / "rails-oracle"
PORT = int(os.environ.get("OV_RAILS_PORT", "25623"))

Y = -50  # bench level: well above the superflat floor (-61), far from the build limit

FUSE = re.compile(r"\bFuse: (-?\d+)s")
MOTION = re.compile(r"Motion: \[([-0-9.Ee]+)d, ([-0-9.Ee]+)d, ([-0-9.Ee]+)d\]")
POS = re.compile(r"Pos: \[([-0-9.Ee]+)d, ([-0-9.Ee]+)d, ([-0-9.Ee]+)d\]")
ROTATION = re.compile(r"Rotation: \[([-0-9.Ee]+)f, ([-0-9.Ee]+)f\]")
UUID = re.compile(r"UUID: \[I; (-?\d+), (-?\d+), (-?\d+), (-?\d+)\]")
ON_GROUND = re.compile(r"OnGround: ([01])b")
GAMETIME = re.compile(r"time is (\d+)")

DIRS = {"north": (0, -1), "south": (0, 1), "west": (-1, 0), "east": (1, 0)}
DIR_ORDER = ["north", "south", "west", "east"]
KINDS = ["none", "flat", "up", "down"]


# ── the world ────────────────────────────────────────────────────────────────

def start(name: str) -> Server:
    directory = RUN / name
    if directory.exists():
        shutil.rmtree(directory)
    server = Server(directory, port=PORT)
    freeze(server)
    return server


def finish(server: Server, name: str) -> None:
    server.stop()
    shutil.rmtree(RUN / name, ignore_errors=True)


def world_dir(name: str) -> Path:
    return RUN / name / "world"


def unsigned(value: int) -> int:
    return value & 0xFFFFFFFFFFFFFFFF


def state_text(entry: dict) -> str:
    props = entry.get("Properties") or {}
    if not props:
        return entry["Name"]
    return entry["Name"] + "[" + ",".join(f"{k}={props[k]}" for k in sorted(props)) + "]"


def read_blocks(world: Path, positions: list[tuple[int, int, int]]) -> list[str]:
    """Block states off the save, one per position, as name[k=v,...] sorted."""
    by_region: dict[tuple[int, int], list[int]] = {}
    for i, (x, _, z) in enumerate(positions):
        by_region.setdefault((x >> 9, z >> 9), []).append(i)
    out = ["?"] * len(positions)
    for (rx, rz), indices in by_region.items():
        path = world / "region" / f"r.{rx}.{rz}.mca"
        loaded: dict[tuple[int, int], dict] = {}
        for cx, cz, tag in chunks(path):
            loaded[(rx * 32 + cx, rz * 32 + cz)] = tag
        for i in indices:
            x, y, z = positions[i]
            tag = loaded.get((x >> 4, z >> 4))
            if tag is None:
                out[i] = "missing-chunk"
                continue
            section = next((s for s in tag.get("sections", []) if s.get("Y") == y >> 4), None)
            if section is None:
                out[i] = "minecraft:air"
                continue
            states = section["block_states"]
            palette = states["palette"]
            if len(palette) == 1:
                out[i] = state_text(palette[0])
                continue
            bits = max(4, (len(palette) - 1).bit_length())
            per_long = 64 // bits
            index = ((y & 15) << 8) | ((z & 15) << 4) | (x & 15)
            word = unsigned(states["data"][index // per_long])
            value = (word >> ((index % per_long) * bits)) & ((1 << bits) - 1)
            out[i] = state_text(palette[value])
    return out


def run_commands(server: Server, commands: list[str], chunk: int = 400) -> None:
    for i in range(0, len(commands), chunk):
        server.batch(commands[i:i + chunk], timeout=600.0)


def write(name: str, data: dict) -> Path:
    NORMALIZED.mkdir(parents=True, exist_ok=True)
    path = NORMALIZED / f"rails_{name}.json"
    path.write_text(json.dumps(data, indent=1, sort_keys=True))
    print(f"wrote {path}")
    return path


# ── shapes ───────────────────────────────────────────────────────────────────

SPACING = 6
COLUMNS = 40

SHAPE_VARIANTS = [
    ("rail", "north_south", "rail"),
    ("rail", "east_west", "rail"),
    ("powered_rail", "north_south", "rail"),
    ("powered_rail", "east_west", "rail"),
    ("rail", "north_south", "powered_rail"),
    ("rail", "east_west", "powered_rail"),
]


def neighbour_cell(ox: int, oz: int, direction: str, kind: str) -> tuple[int, int, int] | None:
    dx, dz = DIRS[direction]
    if kind == "flat":
        return (ox + dx, Y, oz + dz)
    if kind == "up":
        return (ox + dx, Y + 1, oz + dz)
    if kind == "down":
        return (ox + dx, Y - 1, oz + dz)
    return None


def measure_shapes() -> int:
    name = "shapes"
    server = start(name)
    try:
        cells = []
        for v, (centre, initial, neighbour) in enumerate(SHAPE_VARIANTS):
            for config in range(256):
                cells.append((v, config))
        rows = (len(cells) + COLUMNS - 1) // COLUMNS
        x1, z1 = COLUMNS * SPACING + 4, rows * SPACING + 4
        forceload(server, -8, -8, x1, z1)
        fill(server, -4, Y - 2, -4, x1, Y - 1, z1, "minecraft:stone")
        fill(server, -4, Y, -4, x1, Y + 3, z1, "minecraft:air")

        neighbours: list[str] = []
        centres: list[str] = []
        layout = []
        for index, (v, config) in enumerate(cells):
            centre, initial, neighbour = SHAPE_VARIANTS[v]
            ox = (index % COLUMNS) * SPACING
            oz = (index // COLUMNS) * SPACING
            kinds = {d: KINDS[(config >> (2 * i)) & 3] for i, d in enumerate(DIR_ORDER)}
            for d in DIR_ORDER:
                kind = kinds[d]
                cell = neighbour_cell(ox, oz, d, kind)
                if cell is None:
                    continue
                if kind == "up":
                    neighbours.append(f"setblock {cell[0]} {Y} {cell[2]} minecraft:stone")
                neighbours.append(f"setblock {cell[0]} {cell[1]} {cell[2]} minecraft:{neighbour}")
            centres.append(f"setblock {ox} {Y} {oz} minecraft:{centre}[shape={initial}]")
            layout.append((ox, oz, kinds))

        run_commands(server, neighbours)
        run_commands(server, centres)
        time.sleep(1.0)
        save(server)

        positions: list[tuple[int, int, int]] = []
        for ox, oz, kinds in layout:
            positions.append((ox, Y, oz))
            for d in DIR_ORDER:
                cell = neighbour_cell(ox, oz, d, kinds[d])
                positions.append(cell if cell else (ox + DIRS[d][0], Y, oz + DIRS[d][1]))
        states = read_blocks(world_dir(name), positions)

        results = []
        for i, ((v, config), (ox, oz, kinds)) in enumerate(zip(cells, layout)):
            centre, initial, neighbour = SHAPE_VARIANTS[v]
            row = states[i * 5:(i + 1) * 5]
            results.append({
                "centre": centre, "initial": initial, "neighbour": neighbour,
                "kinds": kinds, "centre_after": row[0],
                "neighbours_after": {d: row[1 + j] for j, d in enumerate(DIR_ORDER)},
            })
        write(name, {"y": Y, "cells": results})
        refused = sum(1 for r in results if not r["centre_after"].startswith("minecraft:" + r["centre"]))
        print(f"{len(results)} cells, {refused} whose centre is not the rail that was set")
    finally:
        finish(server, name)
    return 0


# ── busy neighbours ──────────────────────────────────────────────────────────

def measure_busy() -> int:
    """Centre at P; the east neighbour is part of a north-south line of three
    (full), or has one rail to its north (half). Other sides vary: none or a
    free flat rail. Answers: does a full neighbour refuse the connection, and
    does a half neighbour bend towards the new rail."""
    name = "busy"
    server = start(name)
    try:
        forceload(server, -8, -8, 200, 60)
        fill(server, -4, Y - 1, -4, 200, Y - 1, 60, "minecraft:stone")
        cases = []
        cmds_before: list[str] = []
        cmds_centre: list[str] = []
        index = 0
        for east in ("full", "half", "half_south", "free"):
            for west in ("none", "free"):
                for north in ("none", "free"):
                    for centre in ("rail", "powered_rail"):
                        ox, oz = (index % 20) * 8, (index // 20) * 8
                        index += 1
                        placed = []
                        if east in ("full",):
                            placed += [(ox + 1, oz - 1), (ox + 1, oz), (ox + 1, oz + 1)]
                        elif east == "half":
                            placed += [(ox + 1, oz - 1), (ox + 1, oz)]
                        elif east == "half_south":
                            placed += [(ox + 1, oz + 1), (ox + 1, oz)]
                        else:
                            placed += [(ox + 1, oz)]
                        if west == "free":
                            placed.append((ox - 1, oz))
                        if north == "free":
                            placed.append((ox, oz - 1))
                        for x, z in placed:
                            cmds_before.append(f"setblock {x} {Y} {z} minecraft:rail")
                        cmds_centre.append(f"setblock {ox} {Y} {oz} minecraft:{centre}")
                        cases.append({"east": east, "west": west, "north": north,
                                      "centre": centre, "origin": [ox, oz]})
        run_commands(server, cmds_before)
        run_commands(server, cmds_centre)
        time.sleep(1.0)
        save(server)
        positions = []
        for case in cases:
            ox, oz = case["origin"]
            for dx in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    positions.append((ox + dx, Y, oz + dz))
        states = read_blocks(world_dir(name), positions)
        for i, case in enumerate(cases):
            grid = states[i * 9:(i + 1) * 9]
            case["after"] = {f"{dx},{dz}": grid[j] for j, (dx, dz) in
                             enumerate((a, b) for a in (-1, 0, 1) for b in (-1, 0, 1))}
        write(name, {"y": Y, "cases": cases})
    finally:
        finish(server, name)
    return 0


# ── power ────────────────────────────────────────────────────────────────────

def measure_power() -> int:
    name = "power"
    server = start(name)
    try:
        forceload(server, -8, -8, 80, 120)
        fill(server, -4, Y - 1, -4, 80, Y - 1, 120, "minecraft:stone")
        lines = []
        build: list[str] = []
        sources: list[str] = []
        lane = 0
        for block in ("powered_rail", "activator_rail"):
            for layout in ("flat_end", "flat_middle", "slope_bottom", "slope_top", "flat_two"):
                z = lane * 6
                lane += 1
                length = 24
                cells = []
                for i in range(length):
                    if layout.startswith("slope"):
                        y = Y + min(i, 12)
                        for sy in range(Y, y):
                            build.append(f"setblock {i} {sy} {z} minecraft:stone")
                    else:
                        y = Y
                    cells.append((i, y, z))
                for x, y, zz in cells:
                    build.append(f"setblock {x} {y} {zz} minecraft:{block}")
                if layout == "flat_end":
                    src = [(0, Y, z - 1)]
                elif layout == "flat_middle":
                    src = [(12, Y, z - 1)]
                elif layout == "slope_bottom":
                    src = [(0, Y, z - 1)]
                elif layout == "slope_top":
                    src = [(23, Y + 12, z - 1)]
                else:  # two sources 10 apart: does the stretch between stay lit
                    src = [(0, Y, z - 1), (20, Y, z - 1)]
                for sx, sy, sz in src:
                    if sy > Y:
                        for yy in range(Y, sy):
                            build.append(f"setblock {sx} {yy} {sz} minecraft:stone")
                    sources.append(f"setblock {sx} {sy} {sz} minecraft:redstone_block")
                lines.append({"block": block, "layout": layout, "cells": cells, "sources": src})
        run_commands(server, build)
        run_commands(server, sources)
        time.sleep(1.5)
        save(server)
        positions = [c for line in lines for c in line["cells"]]
        lit = read_blocks(world_dir(name), positions)
        # And off again.
        run_commands(server, [f"setblock {x} {y} {z} minecraft:stone"
                              for line in lines for (x, y, z) in line["sources"]])
        time.sleep(1.5)
        save(server)
        unlit = read_blocks(world_dir(name), positions)
        i = 0
        for line in lines:
            n = len(line["cells"])
            line["lit"] = lit[i:i + n]
            line["unlit"] = unlit[i:i + n]
            i += n
            powered = [k for k, s in enumerate(line["lit"]) if "powered=true" in s]
            print(f'{line["block"]:15} {line["layout"]:13} powered {len(powered):2}: {powered}')
        write(name, {"y": Y, "lines": lines})
    finally:
        finish(server, name)
    return 0


# ── carts ────────────────────────────────────────────────────────────────────

def lane_z(k: int) -> int:
    return 8 * k


def lanes() -> list[dict]:
    """Each lane: blocks to set (in order), and the carts to summon."""
    out = []

    def straight(k, n=100):
        return [(x, Y, lane_z(k), "minecraft:rail") for x in range(n)]

    out.append({"name": "straight_fast", "blocks": straight(0),
                "carts": [("minecart", 2.5, Y, lane_z(0) + 0.5, (1.0, 0.0, 0.0), "")]})
    out.append({"name": "straight_slow", "blocks": straight(1),
                "carts": [("minecart", 2.5, Y, lane_z(1) + 0.5, (0.1, 0.0, 0.0), "")]})
    slope = [(0, Y + 8, lane_z(2), "minecraft:rail"), (1, Y + 8, lane_z(2), "minecraft:rail")]
    for i in range(8):
        slope.append((2 + i, Y + 7 - i, lane_z(2), "minecraft:rail"))
    slope += [(x, Y, lane_z(2), "minecraft:rail") for x in range(10, 100)]
    out.append({"name": "slope", "blocks": slope, "supports": True,
                "carts": [("minecart", 2.5, Y + 7, lane_z(2) + 0.5, (0.0, 0.0, 0.0), "")]})
    powered = []
    for x in range(100):
        if 5 <= x < 25:
            powered.append((x, Y - 1, lane_z(3), "minecraft:redstone_block"))
            powered.append((x, Y, lane_z(3), "minecraft:powered_rail"))
        else:
            powered.append((x, Y, lane_z(3), "minecraft:rail"))
    # The first run started this cart on a plain rail at 0.02, and it stopped
    # half a block short of the first powered one. It starts on one now.
    out.append({"name": "powered", "blocks": powered,
                "carts": [("minecart", 5.5, Y, lane_z(3) + 0.5, (0.02, 0.0, 0.0), "")]})
    brake = [(x, Y, lane_z(4), "minecraft:powered_rail" if 20 <= x < 26 else "minecraft:rail")
             for x in range(100)]
    out.append({"name": "brake", "blocks": brake,
                "carts": [("minecart", 15.5, Y, lane_z(4) + 0.5, (0.4, 0.0, 0.0), "")]})
    out.append({"name": "passenger", "blocks": straight(6),
                "carts": [("minecart", 2.5, Y, lane_z(6) + 0.5, (0.3, 0.0, 0.0),
                           'Passengers:[{id:"minecraft:armor_stand",Tags:["ovp"]}]')]})
    out.append({"name": "ground", "blocks": [],
                "carts": [("minecart", 2.5, Y, lane_z(7) + 0.5, (0.3, 0.0, 0.0), "")]})
    launch = [(0, Y, lane_z(8), "minecraft:stone"), (1, Y - 1, lane_z(8), "minecraft:redstone_block"),
              (1, Y, lane_z(8), "minecraft:powered_rail")]
    launch += [(x, Y, lane_z(8), "minecraft:rail") for x in range(2, 100)]
    out.append({"name": "launch", "blocks": launch,
                "carts": [("minecart", 1.5, Y, lane_z(8) + 0.5, (0.0, 0.0, 0.0), "")]})
    uphill = [(x, Y, lane_z(9), "minecraft:rail") for x in range(10)]
    for i in range(8):
        uphill.append((10 + i, Y + 1 + i, lane_z(9), "minecraft:rail"))
    uphill += [(x, Y + 8, lane_z(9), "minecraft:rail") for x in range(18, 60)]
    out.append({"name": "uphill", "blocks": uphill, "supports": True,
                "carts": [("minecart", 7.5, Y, lane_z(9) + 0.5, (0.4, 0.0, 0.0), "")]})
    out.append({"name": "furnace", "blocks": straight(10),
                "carts": [("furnace_minecart", 2.5, Y, lane_z(10) + 0.5, (0.0, 0.0, 0.0),
                           "Fuel:3600s,PushX:1.0d,PushZ:0.0d")]})
    out.append({"name": "collide", "blocks": straight(11),
                "carts": [("minecart", 2.5, Y, lane_z(11) + 0.5, (0.3, 0.0, 0.0), ""),
                          ("minecart", 6.5, Y, lane_z(11) + 0.5, (0.0, 0.0, 0.0), "")]})
    # A cart dropped from three blocks onto bare stone: gravity and the air drag.
    out.append({"name": "air", "blocks": [],
                "carts": [("minecart", 2.5, Y + 3, lane_z(13) + 0.5, (0.1, 0.0, 0.0), "")]})
    out.append({"name": "slow_passenger", "blocks": straight(12),
                "carts": [("minecart", 2.5, Y, lane_z(12) + 0.5, (0.1, 0.0, 0.0),
                           'Passengers:[{id:"minecraft:armor_stand",Tags:["ovp"]}]')]})
    # A curve: east along z=Z, a corner at x=20, then south.
    zc = 200
    curve = [(x, Y, zc, "minecraft:rail") for x in range(20)]
    curve += [(20, Y, z, "minecraft:rail") for z in range(zc + 1, zc + 60)]
    curve.append((20, Y, zc, "minecraft:rail"))  # last: it bends towards both
    out.append({"name": "curve", "blocks": curve,
                "carts": [("minecart", 15.5, Y, zc + 0.5, (0.3, 0.0, 0.0), "")]})
    zc2 = 280
    curve_fast = [(x, Y, zc2, "minecraft:rail") for x in range(20)]
    curve_fast += [(20, Y, z, "minecraft:rail") for z in range(zc2 + 1, zc2 + 60)]
    curve_fast.append((20, Y, zc2, "minecraft:rail"))
    out.append({"name": "curve_fast", "blocks": curve_fast,
                "carts": [("minecart", 2.5, Y, zc2 + 0.5, (1.0, 0.0, 0.0), "")]})
    return out


def parse_entity(line: str) -> dict | None:
    if "has the following entity data" not in line:
        return None
    out: dict = {"who": line.split(" has the following")[0]}
    m = FUSE.search(line)
    if m:
        out["fuse"] = int(m.group(1))
    for key, pattern in (("motion", MOTION), ("pos", POS)):
        m = pattern.search(line)
        if m:
            out[key] = [float(m.group(i)) for i in (1, 2, 3)]
    m = ROTATION.search(line)
    if m:
        out["rotation"] = [float(m.group(1)), float(m.group(2))]
    m = UUID.search(line)
    if m:
        out["uuid"] = [int(m.group(i)) for i in (1, 2, 3, 4)]
    m = ON_GROUND.search(line)
    if m:
        out["on_ground"] = m.group(1) == "1"
    return out


WITNESS_FUSE = 30000


def measure_carts() -> int:
    name = "carts"
    server = start(name)
    probe = None
    try:
        forceload(server, -16, -16, 130, 360)
        fill(server, -8, Y - 1, -8, 130, Y - 1, 360, "minecraft:stone")
        fill(server, -8, Y, -8, 130, Y + 10, 360, "minecraft:air")
        defs = lanes()
        build: list[str] = []
        for lane in defs:
            if lane.get("supports"):
                for x, y, z, _ in lane["blocks"]:
                    if y > Y:
                        build.append(f"fill {x} {Y} {z} {x} {y - 1} {z} minecraft:stone")
            for x, y, z, block in lane["blocks"]:
                build.append(f"setblock {x} {y} {z} {block}")
        run_commands(server, build)
        time.sleep(1.0)
        save(server)
        for lane in defs:
            cells = [(x, y, z) for x, y, z, b in lane["blocks"] if b.endswith("rail")]
            lane["track"] = [[x, y, z, s] for (x, y, z), s in
                             zip(cells, read_blocks(world_dir(name), cells))]
        # A player nearby keeps the level honest (pitfall 11).
        probe = Probe(PORT)
        probe.pump(2.0)
        server.batch(["tp ovprobe 60 -60 -30"])
        probe.pump(1.0)

        summons = [f"summon tnt 120 {Y + 2} -10 "
                   f"{{Tags:[\"ovw\"],Fuse:{WITNESS_FUSE}s,NoGravity:1b}}"]
        for lane in defs:
            for kind, x, y, z, (mx, my, mz), extra in lane["carts"]:
                nbt = f'Tags:["ovc"],Motion:[{mx}d,{my}d,{mz}d]'
                if extra:
                    nbt += "," + extra
                summons.append(f"summon {kind} {x} {y} {z} {{{nbt}}}")
        server.send(*summons)
        query = "execute as @e[tag=ovw] run data get entity @s"
        cart_query = "execute as @e[tag=ovc] run data get entity @s"
        raw_lines: list[str] = []
        deadline = time.monotonic() + 16.0
        while time.monotonic() < deadline:
            server.send(query, cart_query, query)
            time.sleep(0.02)
            probe.pump(0.005)
            while not server.lines.empty():
                raw_lines.append(server.lines.get())
        time.sleep(1.0)
        while not server.lines.empty():
            raw_lines.append(server.lines.get())

        samples = []
        pending: list[dict] = []
        last_fuse = None
        for line in raw_lines:
            entity = parse_entity(line)
            if entity is None:
                continue
            if "fuse" in entity and entity["who"] == "Primed TNT":
                if last_fuse is not None and entity["fuse"] == last_fuse and pending:
                    tick = WITNESS_FUSE - last_fuse
                    for cart in pending:
                        cart["tick"] = tick
                        samples.append(cart)
                pending = []
                last_fuse = entity["fuse"]
            else:
                pending.append(entity)
        write(name, {"y": Y, "witness_fuse": WITNESS_FUSE,
                     "lanes": [{k: v for k, v in lane.items() if k != "blocks"} for lane in defs],
                     "samples": samples})
        ticks = sorted({s["tick"] for s in samples})
        print(f"{len(samples)} samples over {len(ticks)} distinct ticks "
              f"({ticks[:3]} … {ticks[-3:] if ticks else []})")
    finally:
        if probe is not None:
            probe.socket.close()
        finish(server, name)
    return 0


# ── detector ─────────────────────────────────────────────────────────────────

def gametime(server: Server) -> int:
    for line in server.batch(["time query gametime"]):
        m = GAMETIME.search(line)
        if m:
            return int(m.group(1))
    raise RuntimeError("no gametime")


def measure_detector() -> int:
    name = "detector"
    server = start(name)
    probe = None
    try:
        forceload(server, -8, -8, 64, 64)
        fill(server, -4, Y - 1, -4, 64, Y - 1, 64, "minecraft:stone")
        probe = Probe(PORT)
        probe.pump(2.0)
        server.batch(["tp ovprobe 30 -60 30"])
        out: dict = {}
        # 1. Cart resting on a detector rail; lamp beside; then the cart is killed.
        server.batch([f"setblock {x} {Y} 0 minecraft:rail" for x in range(0, 3)])
        server.batch([f"setblock 1 {Y} 0 minecraft:detector_rail",
                      f"setblock 1 {Y} 1 minecraft:redstone_lamp"])
        server.batch([f'summon minecart 1.5 {Y} 0.5 {{Tags:["det"]}}'])
        t0 = gametime(server)
        polls = []
        for _ in range(30):
            lines = server.batch([f"execute if block 1 {Y} 0 minecraft:detector_rail[powered=true]",
                                  f"execute if block 1 {Y} 1 minecraft:redstone_lamp[lit=true]",
                                  "time query gametime"])
            polls.append({"rail": any("Test passed" in l for l in lines[:1]),
                          "lines": lines})
        rail_on = []
        for _ in range(30):
            lines = server.batch([f"execute if block 1 {Y} 0 minecraft:detector_rail[powered=true]"])
            g = gametime(server)
            rail_on.append((g - t0, any("passed" in l for l in lines)))
        server.batch(["kill @e[tag=det]"])
        t_kill = gametime(server)
        off = []
        for _ in range(60):
            lines = server.batch([f"execute if block 1 {Y} 0 minecraft:detector_rail[powered=true]",
                                  "time query gametime"])
            g = next(int(m.group(1)) for l in lines if (m := GAMETIME.search(l)))
            off.append((g - t_kill, any("passed" in l for l in lines[:-1])))
        out["on_after_summon"] = rail_on
        out["after_kill"] = off
        # 2. Comparator reading a chest minecart on a detector rail.
        readings = []
        for i, stacks in enumerate((0, 1, 2, 5, 13, 26, 27)):
            z = 6 + 4 * i
            server.batch([f"setblock 0 {Y} {z} minecraft:rail", f"setblock 2 {Y} {z} minecraft:rail",
                          f"setblock 1 {Y} {z} minecraft:detector_rail",
                          f"setblock 1 {Y} {z + 1} minecraft:comparator[facing=north]"])
            items = ",".join(f'{{Slot:{s}b,id:"minecraft:stone",Count:64b}}' for s in range(stacks))
            server.batch([f'summon chest_minecart 1.5 {Y} {z + 0.5} {{Items:[{items}]}}'])
        time.sleep(2.0)
        for i, stacks in enumerate((0, 1, 2, 5, 13, 26, 27)):
            z = 6 + 4 * i
            lines = server.batch([f"data get block 1 {Y} {z + 1} OutputSignal"])
            value = next((int(m.group(1)) for l in lines
                          if (m := re.search(r"following block data: (-?\d+)", l))), None)
            readings.append({"stacks": stacks, "signal": value})
            print(f"chest minecart with {stacks:2} stacks: comparator {value}")
        out["comparator"] = readings
        write(name, out)
    finally:
        if probe is not None:
            probe.socket.close()
        finish(server, name)
    return 0


# ── capture ──────────────────────────────────────────────────────────────────

def entity_packets(captured: list[tuple[int, bytes]], entity_id: int) -> list[tuple[int, str]]:
    prefix = varint(entity_id)
    return [(pid, payload.hex()) for pid, payload in captured if payload.startswith(prefix)]


def spawns(captured: list[tuple[int, bytes]]) -> list[dict]:
    out = []
    for pid, payload in captured:
        if pid != 0x01:
            continue
        eid, i = read_varint(payload, 0)
        i += 16
        etype, i = read_varint(payload, i)
        x, y, z = struct.unpack_from(">ddd", payload, i)
        i += 24
        pitch, yaw, head = payload[i], payload[i + 1], payload[i + 2]
        i += 3
        data, i = read_varint(payload, i)
        vx, vy, vz = struct.unpack_from(">hhh", payload, i)
        out.append({"id": eid, "type": etype, "pos": [x, y, z], "pitch": pitch, "yaw": yaw,
                    "head": head, "data": data, "velocity": [vx, vy, vz]})
    return out


def measure_capture() -> int:
    name = "capture"
    server = start(name)
    probe = None
    try:
        forceload(server, -32, -32, 32, 32)
        probe = Probe(PORT)
        probe.pump(3.0)
        first = probe.drain()
        player_id = next(struct.unpack_from(">i", p, 0)[0] for pid, p in first if pid == 0x28)
        server.batch(["tp ovprobe 0.5 -60 0.5", "gamemode creative ovprobe"])
        probe.pump(1.0)
        probe.drain()
        out: dict = {"player_id": player_id, "cases": []}
        cases = [
            ("minecart", ""), ("chest_minecart", ""), ("furnace_minecart", ""),
            ("tnt_minecart", ""), ("hopper_minecart", ""), ("spawner_minecart", ""),
            ("command_block_minecart", ""),
            ("minecart", 'CustomDisplayTile:1b,DisplayState:{Name:"minecraft:stone"}'),
            ("minecart", 'CustomDisplayTile:1b,DisplayState:{Name:"minecraft:stone"},DisplayOffset:3'),
            ("minecart", "DisplayOffset:3"),
            ("furnace_minecart", "Fuel:100s"),
            ("command_block_minecart", 'Command:"say hi"'),
            ("hopper_minecart", "Enabled:0b"),
            ("tnt_minecart", "TNTFuse:40"),
            ("minecart", "Motion:[0.3d,0.0d,0.0d]"),
            ("minecart", "Rotation:[45.0f,0.0f]"),
        ]
        for i, (kind, extra) in enumerate(cases):
            x, z = 4 + (i % 8) * 3, 4 + (i // 8) * 3
            tag = f"cap{i}"
            nbt = f'Tags:["{tag}"]' + ("," + extra if extra else "")
            server.batch([f"setblock {x} -61 {z} minecraft:rail"])
            server.batch([f"summon {kind} {x + 0.5} -60 {z + 0.5} {{{nbt}}}"])
            probe.pump(1.5)
            captured = probe.drain()
            spawned = spawns(captured)
            mine = [s for s in spawned if abs(s["pos"][0] - (x + 0.5)) < 1.0
                    and abs(s["pos"][2] - (z + 0.5)) < 1.0]
            case = {"kind": kind, "extra": extra, "spawn": mine}
            if mine:
                case["packets"] = entity_packets(captured, mine[0]["id"])
            out["cases"].append(case)
            print(kind, extra, "->", mine[0]["type"] if mine else None,
                  [(hex(p), h[:40]) for p, h in case.get("packets", [])][:6])

        # Hurt one: the shaking fields.
        server.batch([f"setblock 4 -61 20 minecraft:rail"])
        server.batch(['summon minecart 4.5 -60 20.5 {Tags:["hurt"]}'])
        probe.pump(1.0)
        captured = probe.drain()
        hurt_spawn = [s for s in spawns(captured) if abs(s["pos"][2] - 20.5) < 1]
        server.batch(["damage @e[tag=hurt,limit=1] 1 minecraft:player_attack by ovprobe"])
        probe.pump(1.0)
        captured = probe.drain()
        if hurt_spawn:
            out["hurt"] = entity_packets(captured, hurt_spawn[0]["id"])
            print("hurt", out["hurt"][:6])

        # Ride: Set Passengers by content, then Player Input.
        server.batch(["setblock 0 -61 30 minecraft:rail"] +
                     [f"setblock {x} -61 30 minecraft:rail" for x in range(1, 30)])
        server.batch(['summon minecart 2.5 -60 30.5 {Tags:["ride"]}'])
        probe.pump(1.0)
        captured = probe.drain()
        ride_spawn = [s for s in spawns(captured) if abs(s["pos"][2] - 30.5) < 1]
        server.batch(["ride ovprobe mount @e[tag=ride,limit=1]"])
        probe.pump(1.0)
        captured = probe.drain()
        if ride_spawn:
            cart = ride_spawn[0]["id"]
            wanted = varint(cart) + varint(1) + varint(player_id)
            out["set_passengers"] = [(pid, p.hex()) for pid, p in captured if p == wanted]
            print("set passengers", out["set_passengers"])
            out["after_ride"] = [(pid, p.hex()[:60]) for pid, p in captured][:40]
        # Face east and push forward for two seconds.
        probe.send(0x16, struct.pack(">ff", -90.0, 0.0) + bytes([0]))
        before = server.batch(["execute as @e[tag=ride] run data get entity @s Motion"])
        for _ in range(40):
            probe.send(0x1F, struct.pack(">ff", 0.0, 0.98) + bytes([0]))
            probe.pump(0.05)
        after = server.batch(["execute as @e[tag=ride] run data get entity @s Motion",
                              "execute as @e[tag=ride] run data get entity @s Pos"])
        out["input_before"] = before
        out["input_after"] = after
        print("forward input:", before, after)
        probe.send(0x1F, struct.pack(">ff", 0.0, 0.0) + bytes([0]))
        probe.pump(1.0)
        probe.drain()
        # Dismount: flag 0x02.
        probe.send(0x1F, struct.pack(">ff", 0.0, 0.0) + bytes([2]))
        probe.pump(1.0)
        captured = probe.drain()
        if ride_spawn:
            cart = ride_spawn[0]["id"]
            out["dismount"] = [(pid, p.hex()) for pid, p in captured
                               if p.startswith(varint(cart) + varint(0))]
            print("dismount", out["dismount"])
        write(name, out)
    finally:
        if probe is not None:
            probe.socket.close()
        finish(server, name)
    return 0


SCENARIOS = {
    "shapes": measure_shapes,
    "busy": measure_busy,
    "power": measure_power,
    "carts": measure_carts,
    "detector": measure_detector,
    "capture": measure_capture,
}


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] not in SCENARIOS:
        print(__doc__)
        return 2
    return SCENARIOS[sys.argv[1]]()


if __name__ == "__main__":
    raise SystemExit(main())
