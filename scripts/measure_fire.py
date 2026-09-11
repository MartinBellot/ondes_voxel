#!/usr/bin/env python3
"""Ask a real 1.20.1 server how fire behaves: delays, odds, lifetimes, burns.

Nothing here reads a number off the console at console speed. Every campaign
installs a **datapack** whose `tick` function watches the rig once per game
tick and writes `time query gametime` into a scoreboard or a storage list the
moment something changes. The console only builds the rig, starts it in one
function call (one tick), and reads the scores back at the end. So a delay of
30 ticks reads 30, not "about 1.5 seconds".

Campaigns (each starts its own server on port 25631, in run/fire-oracle/<name>/,
and deletes the world when it is done):

  blocks   One world, three rigs, 4000 ticks, clear weather, difficulty normal.
           * life  64 fires on stone and 64 on netherrack, 4 apart, nothing
                   flammable anywhere near. The first 16 of each group log every
                   age change (gametime, age) and the death (-1); the rest only
                   the death. Netherrack must never die; stone dies when the age
                   passes 3. The intervals between age changes on netherrack are
                   the fire's own tick delay, a geometric number of times.
           * burn  A fire on netherrack with one block B to its east, B walled in
                   by stone on its five other faces. No air cell touches B, so
                   nothing spreads: the only thing that can happen to B is the
                   fire's own burn-out roll against it. Ten kinds of B, 16 each.
                   Logged: the tick B stopped being B, and whether it became fire
                   or air.
           * ignite  A fire on netherrack, an air cell C east of it, a block F
                   east of C walled in by stone. F is outside the fire's reach,
                   so F never burns first; C is the one air cell in reach with a
                   flammable neighbour. Logged: the tick C became fire. Ten kinds
                   of F, 16 each.
           Item entities are counted at the end: burnt blocks must drop nothing.

  rain     64 fires on stone and 16 on netherrack under rain; four cows with
           Fire:200 in the open and four under a stone roof, their Fire and
           Health logged every tick.

  lava     Five geometries of 16 enclosed lava sources, a player connected, and
           randomTickSpeed 200 for a measured number of ticks. The tick function
           removes every fire in each geometry the tick after it appears and
           counts it: the lava's random tick is the only thing that makes fire
           there, and the fire never lives long enough to do anything else.

  entity   Cows (max health 200) in 1x1 glass pens: burning in the open, in fire,
           in soul fire, in lava, on a campfire, on a soul campfire, burning in
           water, burning with fire resistance — Fire and Health every tick.
           Zombies at noon and at a darker hour: the tick each one first
           catches fire. A probe player standing in fire. A campfire given raw
           beef by the probe's right click: the ticks until the steak drops.

Usage: python3 scripts/measure_fire.py <blocks|rain|lava|entity> ...
Run through the machine's shared lock:
    lockf /tmp/ov-vanilla.lock python3 scripts/measure_fire.py blocks
Writes data/vanilla/1.20.1/normalized/fire_<campaign>.json.
"""
from __future__ import annotations

import json
import math
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_entity_packets import Probe, varint  # noqa: E402
from measure_entities import Server  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
RUN = ROOT / "run" / "fire-oracle"
PORT = int(os.environ.get("OV_FIRE_PORT", "25631"))

GY = -61  # the superflat's grass: the support layer
FY = -60  # the layer fires, cows and rigs stand in

TIME = re.compile(r"The time is (\d+)")
SCORE = re.compile(r"(#\w+) has (-?\d+) \[")
STORAGE = re.compile(r"has the following contents: (.*)$")


class FireServer(Server):
    EXTRA_PROPERTIES = "difficulty=normal\n"
    HEAP = "-Xmx1G"


def write_pack(world: Path, functions: dict[str, list[str]]) -> None:
    base = world / "datapacks" / "ovfire"
    fn = base / "data" / "ovfire" / "functions"
    fn.mkdir(parents=True, exist_ok=True)
    (base / "pack.mcmeta").write_text(json.dumps(
        {"pack": {"pack_format": 15, "description": "ondes voxel fire oracle"}}))
    tags = base / "data" / "minecraft" / "tags" / "functions"
    tags.mkdir(parents=True, exist_ok=True)
    (tags / "tick.json").write_text(json.dumps({"values": ["ovfire:tick"]}))
    functions = dict(functions)
    functions["tick"] = ["execute if score #armed ov matches 1 run function ovfire:body"]
    functions.setdefault("ping", ["say ovfire-ready"])
    for name, lines in functions.items():
        (fn / f"{name}.mcfunction").write_text("\n".join(lines) + "\n")


def start(name: str, functions: dict[str, list[str]]) -> FireServer:
    directory = RUN / name
    if directory.exists():
        shutil.rmtree(directory)
    write_pack(directory / "world", functions)
    server = FireServer(directory, PORT)
    # A pack found in world/datapacks at creation is normally enabled; said
    # rather than assumed, and the ping proves the functions were loaded.
    server.batch(['datapack enable "file/ovfire"'])
    lines = server.batch(["function ovfire:ping"])
    if not any("ovfire-ready" in line for line in lines):
        server.stop()
        raise SystemExit(f"the datapack did not load: {lines[-5:]}")
    server.batch(["scoreboard objectives add ov dummy",
                  "gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                  "gamerule doWeatherCycle false", "weather clear 1000000",
                  "gamerule randomTickSpeed 0", "time set noon",
                  "gamerule doFireTick false",
                  "forceload add -16 -16 111 143"])
    return server


def gametime(server: Server) -> int:
    for line in server.batch(["time query gametime"]):
        m = TIME.search(line)
        if m:
            return int(m.group(1))
    raise SystemExit("no gametime")


def run_function(server: Server, name: str, lines: list[str]) -> None:
    """A function executes in one tick; the console would spread it over many."""
    del lines  # written into the pack before the start
    server.batch([f"function ovfire:{name}"])


def wait_until(server: Server, target: int) -> None:
    while True:
        now = gametime(server)
        if now >= target:
            return
        time.sleep(min(5.0, max(0.2, (target - now) / 20.0)))


def scores(server: Server, names: list[str]) -> dict[str, int]:
    out: dict[str, int] = {}
    for i in range(0, len(names), 64):
        for line in server.batch([f"scoreboard players get {n} ov" for n in names[i:i + 64]]):
            m = SCORE.search(line)
            if m:
                out[m.group(1)] = int(m.group(2))
    return out


def storage_list(server: Server, path: str) -> list[int]:
    for line in server.batch([f"data get storage ovfire:log {path}"]):
        m = STORAGE.search(line)
        if m:
            return [int(v) for v in re.findall(r"-?\d+", m.group(1))]
    return []


def write(name: str, document: dict) -> Path:
    NORMALIZED.mkdir(parents=True, exist_ok=True)
    path = NORMALIZED / f"fire_{name}.json"
    path.write_text(json.dumps(document, indent=1))
    print(f"wrote {path}")
    return path


def finish(server: Server, name: str) -> None:
    server.stop()
    shutil.rmtree(RUN / name, ignore_errors=True)


# ── Watchers, as mcfunction lines ───────────────────────────────────────────

def age_logger(i: int, x: int, y: int, z: int) -> tuple[list[str], list[str]]:
    """Per tick: the fire's age into #a (-1 when it is not fire); on a change,
    append (gametime, age) to storage list c<i>."""
    body = [f"scoreboard players set #a ov -1"]
    body += [f"execute if block {x} {y} {z} minecraft:fire[age={k}] run scoreboard players set #a ov {k}"
             for k in range(16)]
    body.append(f"execute unless score #a ov = #p{i} ov run function ovfire:log{i}")
    log = [f"scoreboard players operation #p{i} ov = #a ov",
           "execute store result storage ovfire:log t int 1 run time query gametime",
           f"data modify storage ovfire:log c{i} append from storage ovfire:log t",
           "execute store result storage ovfire:log t int 1 run scoreboard players get #a ov",
           f"data modify storage ovfire:log c{i} append from storage ovfire:log t"]
    return body, log


def first_time(name: str, condition: str) -> str:
    """Record the first tick `condition` holds into score `name` (0 until then)."""
    return (f"execute if score {name} ov matches 0 {condition} "
            f"store result score {name} ov run time query gametime")


def entity_logger(tag: str) -> list[str]:
    """Per tick: append (gametime, Fire, Health x 100) to storage list e_<tag>."""
    sel = f"@e[tag={tag},limit=1]"
    return ["execute store result storage ovfire:log t int 1 run time query gametime",
            f"data modify storage ovfire:log e_{tag} append from storage ovfire:log t",
            f"execute store result storage ovfire:log t int 1 run data get entity {sel} Fire",
            f"data modify storage ovfire:log e_{tag} append from storage ovfire:log t",
            f"execute store result storage ovfire:log t int 1 run data get entity {sel} Health 100",
            f"data modify storage ovfire:log e_{tag} append from storage ovfire:log t"]


def triples(values: list[int]) -> list[list[int]]:
    return [values[i:i + 3] for i in range(0, len(values) - 2, 3)]


# ── blocks: life, burn, ignite ──────────────────────────────────────────────

BURN_KINDS = ["oak_planks", "oak_log", "white_wool", "oak_leaves", "bookshelf", "hay_block",
              "coal_block", "dried_kelp_block", "oak_fence", "stone"]
IGNITE_KINDS = ["oak_planks", "oak_log", "white_wool", "oak_leaves", "bookshelf", "hay_block",
                "coal_block", "target", "dried_kelp_block", "crafting_table"]
REPLICAS = 16


def placed(kind: str) -> str:
    return "minecraft:oak_leaves[persistent=true]" if kind == "oak_leaves" else f"minecraft:{kind}"


def life_cells(z0: int) -> list[tuple[int, int]]:
    return [(4 * (k % 16), z0 + 4 * (k // 16)) for k in range(64)]


def measure_blocks(ticks: int) -> None:
    build: list[str] = []
    start_fn: list[str] = []
    body: list[str] = []
    extra: dict[str, list[str]] = {}
    names: list[str] = []
    loggers: list[tuple[str, int, int, int]] = []

    # life
    groups = {"stone": life_cells(0), "netherrack": life_cells(20)}
    life: dict[str, list[dict]] = {}
    index = 0
    for support, cells in groups.items():
        life[support] = []
        for k, (x, z) in enumerate(cells):
            build.append(f"setblock {x} {GY} {z} minecraft:{support}")
            start_fn.append(f"setblock {x} {FY} {z} minecraft:fire")
            if k < 16:
                lines, log = age_logger(index, x, FY, z)
                body += lines
                extra[f"log{index}"] = log
                start_fn.append(f"scoreboard players set #p{index} ov 0")
                life[support].append({"cell": [x, z], "log": index})
                index += 1
            else:
                name = f"#d{support[0]}{k}"
                names.append(name)
                start_fn.append(f"scoreboard players set {name} ov 0")
                body.append(first_time(name, f"unless block {x} {FY} {z} minecraft:fire"))
                life[support].append({"cell": [x, z], "death": name})

    # burn
    burn: dict[str, list[dict]] = {}
    for g, kind in enumerate(BURN_KINDS):
        burn[kind] = []
        for r in range(REPLICAS):
            x, z = 4 * r, 40 + 4 * g
            build += [f"setblock {x} {GY} {z} minecraft:netherrack",
                      f"setblock {x + 1} {GY} {z} minecraft:stone",
                      f"setblock {x + 2} {FY} {z} minecraft:stone",
                      f"setblock {x + 1} {FY + 1} {z} minecraft:stone",
                      f"setblock {x + 1} {FY} {z - 1} minecraft:stone",
                      f"setblock {x + 1} {FY} {z + 1} minecraft:stone",
                      f"setblock {x + 1} {FY} {z} {placed(kind)}"]
            start_fn.append(f"setblock {x} {FY} {z} minecraft:fire")
            gone, fate = f"#bg{g}_{r}", f"#bf{g}_{r}"
            names += [gone, fate]
            start_fn += [f"scoreboard players set {gone} ov 0", f"scoreboard players set {fate} ov 0"]
            body.append(first_time(gone, f"unless block {x + 1} {FY} {z} minecraft:{kind}"))
            body.append(f"execute if score {gone} ov matches 1.. if score {fate} ov matches 0 "
                        f"if block {x + 1} {FY} {z} minecraft:fire run scoreboard players set {fate} ov 1")
            body.append(f"execute if score {gone} ov matches 1.. if score {fate} ov matches 0 "
                        f"if block {x + 1} {FY} {z} minecraft:air run scoreboard players set {fate} ov 2")
            burn[kind].append({"gone": gone, "fate": fate})

    # ignite
    ignite: dict[str, list[dict]] = {}
    for g, kind in enumerate(IGNITE_KINDS):
        ignite[kind] = []
        for r in range(REPLICAS):
            x, z = 5 * r, 84 + 4 * g
            build += [f"setblock {x} {GY} {z} minecraft:netherrack",
                      f"setblock {x + 2} {GY} {z} minecraft:stone",
                      f"setblock {x + 3} {FY} {z} minecraft:stone",
                      f"setblock {x + 2} {FY + 1} {z} minecraft:stone",
                      f"setblock {x + 2} {FY} {z - 1} minecraft:stone",
                      f"setblock {x + 2} {FY} {z + 1} minecraft:stone",
                      f"setblock {x + 2} {FY} {z} {placed(kind)}"]
            start_fn.append(f"setblock {x} {FY} {z} minecraft:fire")
            lit = f"#ig{g}_{r}"
            names.append(lit)
            start_fn.append(f"scoreboard players set {lit} ov 0")
            body.append(first_time(lit, f"if block {x + 1} {FY} {z} minecraft:fire"))
            ignite[kind].append({"lit": lit})

    start_fn += ["execute store result score #t0 ov run time query gametime",
                 "gamerule doFireTick true", "scoreboard players set #armed ov 1"]
    names.append("#t0")
    server = start("blocks", {"build": build, "start": start_fn, "body": body, **extra})
    try:
        server.batch(["function ovfire:build"])
        time.sleep(2.0)
        server.batch(["function ovfire:start"])
        t_start = gametime(server)
        wait_until(server, t_start + ticks)
        server.batch(["gamerule doFireTick false", "scoreboard players set #armed ov 0",
                      "execute store result score #items ov if entity @e[type=item]"])
        names.append("#items")
        values = scores(server, names)
        logs = {i: storage_list(server, f"c{i}") for i in range(index)}
        t_end = gametime(server)
    finally:
        finish(server, "blocks")

    t0 = values["#t0"]
    doc = {"t0": t0, "ticks": t_end - t0, "items": values.get("#items"),
           "life": {}, "burn": {}, "ignite": {}}
    for support, cells in life.items():
        doc["life"][support] = []
        for cell in cells:
            if "log" in cell:
                raw = logs[cell["log"]]
                pairs = [[raw[i] - t0, raw[i + 1]] for i in range(0, len(raw) - 1, 2)]
                death = next((t for t, a in pairs if a == -1), None)
                doc["life"][support].append({"cell": cell["cell"], "ages": pairs, "death": death})
            else:
                v = values.get(cell["death"], 0)
                doc["life"][support].append({"cell": cell["cell"], "death": v - t0 if v else None})
    for kind, cells in burn.items():
        doc["burn"][kind] = [{"gone": (values[c["gone"]] - t0) if values.get(c["gone"]) else None,
                              "fate": {0: None, 1: "fire", 2: "air"}[values.get(c["fate"], 0)]}
                             for c in cells]
    for kind, cells in ignite.items():
        doc["ignite"][kind] = [(values[c["lit"]] - t0) if values.get(c["lit"]) else None
                               for c in cells]
    write("blocks", doc)
    summarise_blocks(doc)


def summarise_blocks(doc: dict) -> None:
    print(f"ticks {doc['ticks']}, items on the ground {doc['items']}")
    for support, cells in doc["life"].items():
        deaths = [c["death"] for c in cells if c["death"] is not None]
        print(f"life on {support}: {len(deaths)}/{len(cells)} died, "
              f"mean {sum(deaths) / max(1, len(deaths)):.0f} ticks")
        gaps = []
        for c in cells:
            ages = c.get("ages") or []
            for (t1, a1), (t2, a2) in zip(ages, ages[1:]):
                if a1 >= 0 and a2 >= 0:
                    gaps.append(t2 - t1)
        if gaps:
            short = [g for g in gaps if g < 60]
            print(f"  {len(gaps)} gaps, min {min(gaps)}, below 60: {len(short)} "
                  f"histogram {sorted({g: short.count(g) for g in set(short)}.items())}")
    for kind, cells in doc["burn"].items():
        gone = [c["gone"] for c in cells if c["gone"] is not None]
        fire = sum(1 for c in cells if c["fate"] == "fire")
        print(f"burn {kind:18s} {len(gone):2d}/{len(cells)} gone, mean {sum(gone) / max(1, len(gone)):6.0f}, "
              f"became fire {fire}")
    for kind, cells in doc["ignite"].items():
        lit = [c for c in cells if c is not None]
        print(f"ignite {kind:18s} {len(lit):2d}/{len(cells)} lit, mean {sum(lit) / max(1, len(lit)):6.0f}")


# ── confirm: the two questions `blocks` left open, interleaved ──────────────
#
# The `blocks` run put each kind in a row of its own, and two things came out
# that sixteen samples a row could not settle:
#
#   * oak logs and coal blocks — both ignite odds 5 — differed from each other
#     at p = 0.0019 while their pool fitted the table at p = 0.99;
#   * wool, dried kelp and leaves (burn odds 60) burnt at 0.30 per fire tick
#     against the table's 0.20, and the bookshelf (20) at about 0.12.
#
# Block and row were confounded in both. Here the kinds alternate position by
# position, 32 of each, so any effect of place falls on all of them alike.

IGNITE5_KINDS = ["oak_planks", "oak_log", "coal_block"]
BURN_CONFIRM_KINDS = ["oak_planks", "white_wool", "oak_leaves", "dried_kelp_block", "bookshelf"]


def measure_ignite5(ticks: int, replicas: int = 32) -> None:
    build: list[str] = []
    start_fn: list[str] = []
    body: list[str] = []
    names: list[str] = []
    cells: dict[str, list[str]] = {k: [] for k in IGNITE5_KINDS}
    burn_cells: dict[str, list[tuple[str, str]]] = {k: [] for k in BURN_CONFIRM_KINDS}
    for n in range(replicas * len(BURN_CONFIRM_KINDS)):
        kind = BURN_CONFIRM_KINDS[n % len(BURN_CONFIRM_KINDS)]
        x, z = 4 * (n % 20), 40 + 4 * (n // 20)
        build += [f"setblock {x} {GY} {z} minecraft:netherrack",
                  f"setblock {x + 1} {GY} {z} minecraft:stone",
                  f"setblock {x + 2} {FY} {z} minecraft:stone",
                  f"setblock {x + 1} {FY + 1} {z} minecraft:stone",
                  f"setblock {x + 1} {FY} {z - 1} minecraft:stone",
                  f"setblock {x + 1} {FY} {z + 1} minecraft:stone",
                  f"setblock {x + 1} {FY} {z} {placed(kind)}"]
        start_fn.append(f"setblock {x} {FY} {z} minecraft:fire")
        gone, fate = f"#cg{n}", f"#cf{n}"
        names += [gone, fate]
        start_fn += [f"scoreboard players set {gone} ov 0", f"scoreboard players set {fate} ov 0"]
        body.append(first_time(gone, f"unless block {x + 1} {FY} {z} minecraft:{kind}"))
        body.append(f"execute if score {gone} ov matches 1.. if score {fate} ov matches 0 "
                    f"if block {x + 1} {FY} {z} minecraft:fire run scoreboard players set {fate} ov 1")
        body.append(f"execute if score {gone} ov matches 1.. if score {fate} ov matches 0 "
                    f"if block {x + 1} {FY} {z} minecraft:air run scoreboard players set {fate} ov 2")
        burn_cells[kind].append((gone, fate))
    for n in range(replicas * len(IGNITE5_KINDS)):
        kind = IGNITE5_KINDS[n % len(IGNITE5_KINDS)]
        x, z = 5 * (n % 16), 4 * (n // 16)
        build += [f"setblock {x} {GY} {z} minecraft:netherrack",
                  f"setblock {x + 2} {GY} {z} minecraft:stone",
                  f"setblock {x + 3} {FY} {z} minecraft:stone",
                  f"setblock {x + 2} {FY + 1} {z} minecraft:stone",
                  f"setblock {x + 2} {FY} {z - 1} minecraft:stone",
                  f"setblock {x + 2} {FY} {z + 1} minecraft:stone",
                  f"setblock {x + 2} {FY} {z} minecraft:{kind}"]
        start_fn.append(f"setblock {x} {FY} {z} minecraft:fire")
        lit = f"#i{n}"
        names.append(lit)
        start_fn.append(f"scoreboard players set {lit} ov 0")
        body.append(first_time(lit, f"if block {x + 1} {FY} {z} minecraft:fire"))
        cells[kind].append(lit)
    start_fn += ["execute store result score #t0 ov run time query gametime",
                 "gamerule doFireTick true", "scoreboard players set #armed ov 1"]
    names.append("#t0")
    server = start("confirm", {"build": build, "start": start_fn, "body": body})
    try:
        server.batch(["function ovfire:build"])
        time.sleep(2.0)
        server.batch(["function ovfire:start"])
        t_start = gametime(server)
        wait_until(server, t_start + ticks)
        server.batch(["gamerule doFireTick false", "scoreboard players set #armed ov 0",
                      "execute store result score #items ov if entity @e[type=item]"])
        names.append("#items")
        values = scores(server, names)
        t_end = gametime(server)
    finally:
        finish(server, "confirm")
    t0 = values["#t0"]
    doc = {"t0": t0, "ticks": t_end - t0, "items": values.get("#items"),
           "ignite": {k: [(values[c] - t0) if values.get(c) else None for c in v]
                      for k, v in cells.items()},
           "burn": {k: [{"gone": (values[g] - t0) if values.get(g) else None,
                         "fate": {0: None, 1: "fire", 2: "air"}[values.get(f, 0)]}
                        for g, f in v]
                    for k, v in burn_cells.items()}}
    write("confirm", doc)
    for kind, times in doc["ignite"].items():
        lit = [t for t in times if t is not None]
        print(f"confirm ignite {kind:16s} {len(lit):2d}/{len(times)} lit, "
              f"mean {sum(lit) / max(1, len(lit)):6.0f}")
    for kind, cells_ in doc["burn"].items():
        gone = [c["gone"] for c in cells_ if c["gone"] is not None]
        print(f"confirm burn   {kind:16s} {len(gone):2d}/{len(cells_)} gone, "
              f"mean {sum(gone) / max(1, len(gone)):6.0f}")


# ── rain ────────────────────────────────────────────────────────────────────

def measure_rain(ticks: int) -> None:
    build: list[str] = []
    start_fn: list[str] = []
    body: list[str] = []
    extra: dict[str, list[str]] = {}
    names: list[str] = []
    cells_doc: dict[str, list[dict]] = {"stone": [], "netherrack": []}
    index = 0
    for k, (x, z) in enumerate(life_cells(0)):
        build.append(f"setblock {x} {GY} {z} minecraft:stone")
        start_fn.append(f"setblock {x} {FY} {z} minecraft:fire")
        if k < 16:
            lines, log = age_logger(index, x, FY, z)
            body += lines
            extra[f"log{index}"] = log
            start_fn.append(f"scoreboard players set #p{index} ov 0")
            cells_doc["stone"].append({"log": index})
            index += 1
        else:
            name = f"#ds{k}"
            names.append(name)
            start_fn.append(f"scoreboard players set {name} ov 0")
            body.append(first_time(name, f"unless block {x} {FY} {z} minecraft:fire"))
            cells_doc["stone"].append({"death": name})
    for k in range(16):
        x, z = 4 * k, 20
        build.append(f"setblock {x} {GY} {z} minecraft:netherrack")
        start_fn.append(f"setblock {x} {FY} {z} minecraft:fire")
        lines, log = age_logger(index, x, FY, z)
        body += lines
        extra[f"log{index}"] = log
        start_fn.append(f"scoreboard players set #p{index} ov 0")
        cells_doc["netherrack"].append({"log": index})
        index += 1
    cows = []
    for k in range(8):
        x, z = 4 * k, 30
        roofed = k >= 4
        build += pen(x, z)
        if roofed:
            build.append(f"fill {x - 1} {FY + 3} {z - 1} {x + 1} {FY + 3} {z + 1} minecraft:stone")
        tag = f"r{k}"
        start_fn.append(summon_cow(x, FY, z, tag, "Fire:200s"))
        body += entity_logger(tag)
        cows.append({"tag": tag, "roofed": roofed})
    start_fn += ["execute store result score #t0 ov run time query gametime",
                 "gamerule doFireTick true", "scoreboard players set #armed ov 1"]
    names.append("#t0")
    server = start("rain", {"build": build, "start": start_fn, "body": body, **extra})
    try:
        server.batch(["function ovfire:build", "weather rain 1000000"])
        time.sleep(5.0)  # the rain level climbs 0.01 a tick; raining is > 0.2
        server.batch(["function ovfire:start"])
        t_start = gametime(server)
        wait_until(server, t_start + ticks)
        server.batch(["gamerule doFireTick false", "scoreboard players set #armed ov 0"])
        values = scores(server, names)
        logs = {i: storage_list(server, f"c{i}") for i in range(index)}
        entity_logs = {c["tag"]: storage_list(server, f"e_{c['tag']}") for c in cows}
    finally:
        finish(server, "rain")
    t0 = values["#t0"]
    doc = {"t0": t0, "life": {}, "cows": []}
    for support, cells in cells_doc.items():
        doc["life"][support] = []
        for cell in cells:
            if "log" in cell:
                raw = logs[cell["log"]]
                pairs = [[raw[i] - t0, raw[i + 1]] for i in range(0, len(raw) - 1, 2)]
                death = next((t for t, a in pairs if a == -1), None)
                doc["life"][support].append({"ages": pairs, "death": death})
            else:
                v = values.get(cell["death"], 0)
                doc["life"][support].append({"death": v - t0 if v else None})
    for c in cows:
        doc["cows"].append({"roofed": c["roofed"],
                            "series": [[t - t0, f, h] for t, f, h in triples(entity_logs[c["tag"]])][:200]})
    write("rain", doc)
    for support, cells in doc["life"].items():
        deaths = [c["death"] for c in cells if c["death"] is not None]
        print(f"rain life on {support}: {len(deaths)}/{len(cells)} died, "
              f"mean {sum(deaths) / max(1, len(deaths)):.0f}")
    for c in doc["cows"]:
        print(f"cow roofed={c['roofed']}: {c['series'][:6]}")


# ── lava ────────────────────────────────────────────────────────────────────

LAVA_GEOMETRIES = ["planks_roof_2", "planks_roof_3", "planks_ring", "crafting_roof_2",
                   "stone_roof_2"]


def measure_lava(ticks: int, speed: int) -> None:
    build: list[str] = []
    body: list[str] = []
    names = []
    for g, geometry in enumerate(LAVA_GEOMETRIES):
        z = 6 * g + 2
        for r in range(REPLICAS):
            x = 6 * r + 2
            ring = "oak_planks" if geometry == "planks_ring" else "stone"
            build.append(f"fill {x - 1} {FY} {z - 1} {x + 1} {FY} {z + 1} minecraft:{ring}")
            build.append(f"setblock {x} {FY} {z} minecraft:lava")
            roof = {"planks_roof_2": ("oak_planks", 2), "planks_roof_3": ("oak_planks", 3),
                    "crafting_roof_2": ("crafting_table", 2),
                    "stone_roof_2": ("stone", 2)}.get(geometry)
            if roof:
                build.append(f"fill {x - 1} {FY + roof[1]} {z - 1} {x + 1} {FY + roof[1]} {z + 1} "
                             f"minecraft:{roof[0]}")
        name = f"#lava{g}"
        names.append(name)
        body += [f"execute store result score #n ov run fill 0 {FY} {z - 2} 97 {FY + 2} {z + 2} "
                 f"minecraft:air replace minecraft:fire",
                 f"scoreboard players operation {name} ov += #n ov"]
    start_fn = [f"scoreboard players set {n} ov 0" for n in names]
    start_fn += ["gamerule doFireTick true", "scoreboard players set #armed ov 1"]
    server = start("lava", {"build": build, "start": start_fn, "body": body})
    keeper = None
    try:
        keeper = subprocess.Popen([sys.executable, str(ROOT / "scripts" / "keepalive_client.py"),
                                   str(PORT), "ovlava"])
        time.sleep(4.0)
        server.batch(["gamemode creative ovlava", "tp ovlava 48 -60 40", "function ovfire:build"])
        time.sleep(3.0)
        server.batch(["function ovfire:start"])
        time.sleep(1.0)
        t0 = gametime(server)
        server.batch([f"gamerule randomTickSpeed {speed}"])
        time.sleep(ticks / 20.0)
        lines = server.batch(["gamerule randomTickSpeed 0", "time query gametime"])
        t1 = next(int(TIME.search(l).group(1)) for l in lines if TIME.search(l))
        time.sleep(1.0)
        server.batch(["scoreboard players set #armed ov 0"])
        values = scores(server, names)
        players = server.batch(["list"])
    finally:
        if keeper:
            keeper.kill()
        finish(server, "lava")
    measured = t1 - t0 - 1
    doc = {"speed": speed, "ticks": measured, "players": players[-1:],
           "fires": {g: values.get(f"#lava{i}", 0) for i, g in enumerate(LAVA_GEOMETRIES)}}
    expected_ticks = measured * speed / 4096.0 * REPLICAS
    doc["random_ticks_per_geometry"] = expected_ticks
    write("lava", doc)
    for g in LAVA_GEOMETRIES:
        n = doc["fires"][g]
        print(f"lava {g:16s} {n:5d} fires, {n / expected_ticks:.4f} per random tick")


# ── entity ──────────────────────────────────────────────────────────────────

def pen(x: int, z: int) -> list[str]:
    return [f"fill {x - 1} {FY} {z - 1} {x + 1} {FY + 1} {z + 1} minecraft:glass",
            f"fill {x} {FY} {z} {x} {FY + 1} {z} minecraft:air"]


def summon_cow(x: int, y: float, z: int, tag: str, extra: str = "") -> str:
    nbt = (f'Tags:["{tag}"],PersistenceRequired:1b,'
           f'Attributes:[{{Name:"generic.max_health",Base:200d}}],Health:200f')
    if extra:
        nbt += "," + extra
    return f"summon minecraft:cow {x + 0.5} {y} {z + 0.5} {{{nbt}}}"


COW_GROUPS = ["burning", "fire", "soul_fire", "lava", "campfire", "soul_campfire", "water",
              "fire_resistance"]


def sky_darken(day_time: int) -> int:
    fraction = day_time / 24000.0 - 0.25
    d0 = fraction - math.floor(fraction)
    d1 = 0.5 - math.cos(d0 * math.pi) / 2.0
    tod = (d0 * 2.0 + d1) / 3.0
    raw = 1.0 - (math.cos(tod * 2.0 * math.pi) * 2.0 + 0.5)
    return int(min(1.0, max(0.0, raw)) * 11.0)


def darker_hour(target: int) -> int:
    for t in range(6000, 13000, 10):
        if sky_darken(t) == target:
            return t
    raise SystemExit("no hour")


class Walker(Probe):
    """A probe that keeps telling the server where it stands, every tick, the
    way an idle client does — and answers its keep-alives on a thread."""

    def __init__(self, port: int, name: str) -> None:
        super().__init__(port, name=name)
        self.lock = threading.Lock()
        self.alive = True
        self.sequence = 0
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()

    def _loop(self) -> None:
        last = 0.0
        while self.alive:
            try:
                with self.lock:
                    self.pump(0.02)
                    if self.position and time.monotonic() - last > 0.05:
                        x, y, z = self.position
                        self.send(0x14, struct.pack(">ddd", x, y, z) + bytes([1]))
                        last = time.monotonic()
                self.captured.clear()
            except (EOFError, OSError):
                return
            time.sleep(0.005)

    def act(self, packet_id: int, payload: bytes) -> None:
        with self.lock:
            self.send(packet_id, payload)

    def close(self) -> None:
        self.alive = False
        try:
            self.socket.close()
        except OSError:
            pass


def packed(x: int, y: int, z: int) -> bytes:
    return struct.pack(">Q", ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF))


def measure_entity(ticks: int) -> None:
    build: list[str] = []
    start_fn: list[str] = []
    body: list[str] = []
    cows = []
    for g, group in enumerate(COW_GROUPS):
        for r in range(4):
            x, z = 4 * r + 2, 4 * g + 2
            build += pen(x, z)
            tag = f"c{g}_{r}"
            y: float = FY
            extra = ""
            if group == "burning":
                extra = "Fire:200s"
            elif group in ("fire", "soul_fire"):
                build.append(f"setblock {x} {GY} {z} minecraft:"
                             f"{'netherrack' if group == 'fire' else 'soul_soil'}")
                start_fn.append(f"setblock {x} {FY} {z} minecraft:{group}")
            elif group == "lava":
                start_fn.append(f"setblock {x} {FY} {z} minecraft:lava")
            elif group in ("campfire", "soul_campfire"):
                build.append(f"setblock {x} {FY} {z} minecraft:{group}")
                y = FY + 0.4375
            elif group == "water":
                build.append(f"setblock {x} {FY} {z} minecraft:water")
                extra = "Fire:200s"
            elif group == "fire_resistance":
                # 1.20.1's key: ActiveEffects with the numeric id (12 is
                # fire_resistance); `active_effects` is 1.20.2's.
                extra = "Fire:200s,ActiveEffects:[{Id:12b,Amplifier:0b,Duration:100000}]"
            start_fn.insert(0, summon_cow(x, y, z, tag, extra))
            body += entity_logger(tag)
            cows.append({"group": group, "tag": tag})
    # the player's pen: fire on netherrack
    px, pz = 30, 2
    build += pen(px, pz)
    build.append(f"setblock {px} {GY} {pz} minecraft:netherrack")
    # a campfire the probe feeds
    cx, cz = 30, 10
    build += [f"setblock {cx} {FY} {cz} minecraft:campfire"]
    body += [first_time("#cf_in", f"if data block {cx} {FY} {cz} Items[0]"),
             first_time("#cf_out", f"positioned {cx + 0.5} {FY + 0.5} {cz + 0.5} "
                                   f"if entity @e[type=item,distance=..3]")]
    start_fn += ["scoreboard players set #cf_in ov 0", "scoreboard players set #cf_out ov 0",
                 "execute store result score #t0 ov run time query gametime",
                 "gamerule doFireTick true", "scoreboard players set #armed ov 1"]
    # zombies: their own function, run twice
    zombie_fn: list[str] = ["kill @e[type=zombie]"]
    zombie_names = []
    for k in range(32):
        x, z = 4 * (k % 16) + 2, 40 + 4 * (k // 16)
        build += pen(x, z)
        name = f"#z{k}"
        zombie_names.append(name)
        zombie_fn += [f"scoreboard players set {name} ov 0",
                      f'summon minecraft:zombie {x + 0.5} {FY} {z + 0.5} '
                      f'{{Tags:["z{k}"],PersistenceRequired:1b}}']
        body += [f"execute store result score #f ov run data get entity @e[tag=z{k},limit=1] Fire",
                 f"execute if score {name} ov matches 0 if score #f ov matches 1.. "
                 f"store result score {name} ov run time query gametime"]
    zombie_fn.append("execute store result score #tz ov run time query gametime")

    server = start("entity", {"build": build, "start": start_fn, "body": body, "zombies": zombie_fn})
    walker = None
    doc: dict = {"cows": [], "zombies": []}
    try:
        walker = Walker(PORT, "ovfire")
        time.sleep(3.0)
        server.batch(["function ovfire:build", "gamemode survival ovfire",
                      f"tp ovfire {px + 0.5} {FY} {pz + 5.5}",
                      "give ovfire minecraft:beef 4"])
        time.sleep(2.0)
        server.batch(["function ovfire:start"])
        t0 = gametime(server)
        # the probe steps into fire, and feeds the campfire from next to it
        server.batch([f"setblock {px} {FY} {pz} minecraft:fire",
                      f"tp ovfire {px + 0.5} {FY} {pz + 0.5}"])
        tp_time = gametime(server)
        body_player = []  # logged from the console below, coarsely
        samples = []
        for _ in range(60):
            for line in server.batch(["data get entity ovfire Fire", "data get entity ovfire Health",
                                      "time query gametime"]):
                samples.append(line)
            time.sleep(0.1)
        server.batch([f"tp ovfire {cx + 0.5} {FY} {cz + 2.5}", "effect give ovfire instant_health 1 5"])
        time.sleep(1.0)
        walker.act(0x28, struct.pack(">h", 0))
        walker.act(0x31, varint(0) + packed(cx, FY, cz) + varint(1) +
                   struct.pack(">fff", 0.5, 0.5, 0.5) + bytes([0]) + varint(1))
        wait_until(server, t0 + ticks)
        # zombies, noon then a darker hour
        dark = darker_hour(2)
        for hour in (6000, dark):
            server.batch([f"time set {hour}", "function ovfire:zombies"])
            tz = scores(server, ["#tz"])["#tz"]
            wait_until(server, tz + 600)
            zs = scores(server, zombie_names)
            doc["zombies"].append({"time": hour, "darken": sky_darken(hour),
                                   "ignited": [(zs[n] - tz) if zs.get(n) else None
                                               for n in zombie_names]})
        server.batch(["scoreboard players set #armed ov 0"])
        values = scores(server, ["#t0", "#cf_in", "#cf_out"])
        logs = {c["tag"]: storage_list(server, f"e_{c['tag']}") for c in cows}
        items = server.batch(["data get entity @e[type=item,limit=1] Item"])
    finally:
        if walker:
            walker.close()
        finish(server, "entity")
    t0 = values["#t0"]
    for c in cows:
        doc["cows"].append({"group": c["group"],
                            "series": [[t - t0, f, h] for t, f, h in triples(logs[c["tag"]])][:400]})
    doc["player_samples"] = samples
    doc["player_tp_time"] = tp_time - t0
    doc["campfire"] = {"in": values.get("#cf_in"), "out": values.get("#cf_out"),
                       "item": items[-1:]}
    write("entity", doc)
    for c in doc["cows"][::4]:
        s = c["series"]
        print(f"{c['group']:16s} {s[:3]} ... {s[40:43]}")
    for z in doc["zombies"]:
        lit = [v for v in z["ignited"] if v is not None]
        print(f"zombies at {z['time']} (darken {z['darken']}): {len(lit)}/32 lit, "
              f"mean {sum(lit) / max(1, len(lit)):.1f}")
    print("campfire", doc["campfire"])


# ── analyse: the documented rules, simulated, against what vanilla did ──────
#
# A reference model of the wiki's rules for these simple rigs only, used to
# tell apart the variants the documentation leaves open — never as the
# implementation (that is src/ov_gameplay/src/fire.cpp, compared to the same
# numbers by test_fire.cpp). Every comparison carries a wrong variant as a
# control, which must score clearly worse (briefing, trap 14).

import random as _random  # noqa: E402


def ks(a: list[float], b: list[float]) -> float:
    """Two-sample Kolmogorov-Smirnov distance."""
    # Ticks are integers and ties are everywhere: both sides advance through
    # a tied value together, or a sample scores a distance against itself.
    a, b = sorted(a), sorted(b)
    i = j = 0
    d = 0.0
    while i < len(a) and j < len(b):
        value = min(a[i], b[j])
        while i < len(a) and a[i] == value:
            i += 1
        while j < len(b) and b[j] == value:
            j += 1
        d = max(d, abs(i / len(a) - j / len(b)))
    return d


def ks_p(d: float, n: int, m: int) -> float:
    """Asymptotic p-value of a two-sample KS distance."""
    en = math.sqrt(n * m / (n + m))
    lam = (en + 0.12 + 0.11 / en) * d
    s = 0.0
    for k in range(1, 101):
        s += 2 * (-1) ** (k - 1) * math.exp(-2 * k * k * lam * lam)
    return max(0.0, min(1.0, s))


def sim_stone_life(rng: _random.Random, spread: int = 10, new_age: bool = False,
                   rain: bool = False) -> int:
    """A fire on stone, nothing flammable: ticks until it goes out."""
    t = 30 + rng.randrange(spread)
    age = 0
    while True:
        if rain and rng.random() < 0.2 + age * 0.03:
            return t
        grown = min(15, age + rng.randrange(3) // 2)
        test = grown if new_age else age
        age = grown
        if test > 3:
            return t
        t += 30 + rng.randrange(spread)


def sim_burn(rng: _random.Random, burn: int, limit: int) -> int | None:
    """The `burn` rig: B's one roll per fire tick, 300."""
    t = 30 + rng.randrange(10)
    while t <= limit:
        if rng.randrange(300) < burn:
            return t
        t += 30 + rng.randrange(10)
    return None


def sim_ignite(rng: _random.Random, ignite: int, limit: int, difficulty: int = 2) -> int | None:
    """The `ignite` rig: one empty cell in reach, 100, from a fire on netherrack."""
    t = 30 + rng.randrange(10)
    age = 0
    while t <= limit:
        age_then = age
        age = min(15, age + rng.randrange(3) // 2)
        chance = (ignite + 40 + difficulty * 7) // (age_then + 30)
        if chance > 0 and rng.randrange(100) <= chance:
            return t
        t += 30 + rng.randrange(10)
    return None


WIKI_ODDS = {"oak_planks": (5, 20), "oak_log": (5, 5), "white_wool": (30, 60),
             "oak_leaves": (30, 60), "bookshelf": (30, 20), "hay_block": (60, 20),
             "coal_block": (5, 5), "dried_kelp_block": (30, 60), "oak_fence": (5, 20),
             "target": (15, 20), "stone": (0, 0), "crafting_table": (0, 0)}


def analyse_blocks(doc: dict) -> dict:
    rng = _random.Random(1)
    out: dict = {}
    limit = doc["ticks"]
    stone = [c["death"] for c in doc["life"]["stone"] if c["death"] is not None]
    model = {"age at tick start": [sim_stone_life(rng) for _ in range(20000)],
             "age after aging (control)": [sim_stone_life(rng, new_age=True) for _ in range(20000)],
             "delay 30..40 (control)": [sim_stone_life(rng, spread=11) for _ in range(20000)]}
    out["stone_life"] = {k: {"ks": round(ks(stone, v), 4), "p": ks_p(ks(stone, v), len(stone), len(v))}
                         for k, v in model.items()}
    out["stone_life_mean"] = sum(stone) / max(1, len(stone))
    seen4 = [max(a for _, a in c["ages"]) for c in doc["life"]["stone"] if c.get("ages")]
    out["stone_max_age_seen"] = sorted(seen4)
    gaps = []
    for c in doc["life"]["netherrack"]:
        ages = c.get("ages") or []
        gaps += [t2 - t1 for (t1, a1), (t2, a2) in zip(ages, ages[1:]) if a1 >= 0 and a2 >= 0]
    short = [g for g in gaps if g < 60]
    out["gaps_below_60"] = {g: short.count(g) for g in sorted(set(short))}
    out["gaps_min"] = min(gaps) if gaps else None
    out["gap_fraction_single"] = len(short) / max(1, len(gaps))  # 1/3 if +1 w.p. 1/3
    out["burn"] = {}
    for kind, cells in doc["burn"].items():
        seen = [c["gone"] if c["gone"] is not None else limit + 1 for c in cells]
        wiki = WIKI_ODDS.get(kind, (0, 0))[1]
        row = {"gone": sum(1 for c in cells if c["gone"] is not None), "of": len(cells),
               "fire": sum(1 for c in cells if c["fate"] == "fire")}
        for label, odds in (("wiki", wiki), ("half", wiki // 2), ("double", wiki * 2)):
            if odds <= 0:
                continue
            sim = [sim_burn(rng, odds, limit) or limit + 1 for _ in range(5000)]
            d = ks(seen, sim)
            row[f"{label}({odds})"] = {"ks": round(d, 3), "p": round(ks_p(d, len(seen), len(sim)), 4)}
        out["burn"][kind] = row
    out["ignite"] = {}
    for kind, cells in doc["ignite"].items():
        seen = [t if t is not None else limit + 1 for t in cells]
        wiki = WIKI_ODDS.get(kind, (0, 0))[0]
        row = {"lit": sum(1 for t in cells if t is not None), "of": len(cells)}
        for label, odds in (("wiki", wiki), ("zero", 0), ("60", 60)):
            if label != "wiki" and odds == wiki:
                continue
            if odds <= 0:
                row[f"{label}(0)"] = "never lights"
                continue
            sim = [sim_ignite(rng, odds, limit) or limit + 1 for _ in range(5000)]
            d = ks(seen, sim)
            row[f"{label}({odds})"] = {"ks": round(d, 3), "p": round(ks_p(d, len(seen), len(sim)), 4)}
        out["ignite"][kind] = row
    return out


def main() -> int:
    if len(sys.argv) >= 2 and sys.argv[1] == "analyse":
        doc = json.loads((NORMALIZED / "fire_blocks.json").read_text())
        print(json.dumps(analyse_blocks(doc), indent=1))
        return 0
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    what = sys.argv[1]
    if what == "blocks":
        measure_blocks(int(sys.argv[2]) if len(sys.argv) > 2 else 4000)
    elif what == "confirm":
        measure_ignite5(int(sys.argv[2]) if len(sys.argv) > 2 else 4000)
    elif what == "rain":
        measure_rain(int(sys.argv[2]) if len(sys.argv) > 2 else 1500)
    elif what == "lava":
        measure_lava(int(sys.argv[2]) if len(sys.argv) > 2 else 1200,
                     int(sys.argv[3]) if len(sys.argv) > 3 else 200)
    elif what == "entity":
        measure_entity(int(sys.argv[2]) if len(sys.argv) > 2 else 900)
    elif what == "summary":
        summarise_blocks(json.loads((NORMALIZED / "fire_blocks.json").read_text()))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
