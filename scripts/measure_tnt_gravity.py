#!/usr/bin/env python3
"""Ask a real 1.20.1 server what primed TNT, creepers and falling blocks do.

`docs/provenance/explosions.md` measured the explosion itself — the rays, the
resistance of 987 blocks, the crater, the damage. What it did not measure is
everything around it: the entity that carries the charge, the law of a chain
reaction, the blocks that fall, and the packets a client is sent about all of
it. That is this script.

Scenarios (each starts and stops its own server, on OV_TNT_PORT, default 25619,
in run/tnt-oracle/<scenario>/):

  capture       A probe client joins and records the packets: Spawn Entity for a
                primed TNT and a falling block (the `data` field), the metadata
                index of the fuse, of a falling block's start position and of a
                creeper's three flags — one NBT field at a time against a
                baseline — and the Explosion packet, found by its payload rather
                than trusted to an id.

  tnt_motion    Twenty TNT blocks primed by redstone, ten in the air and ten on
                the ground, their whole NBT read many times while they burn. The
                entity's own `Fuse` is the tick clock: every sample says exactly
                how many ticks of motion it has had, however late the console
                answered.

  chain         A charge in a ring of TNT blocks. The ring comes back as primed
                entities whose fuses are read in one command; a witness TNT
                summoned beside the charge says how many ticks passed.

  sand          Columns of sand losing their support, a block set in mid-air,
                sand onto a torch, sand into a pool, and concrete powder into a
                pool and beside water on each of its six faces.

  crater        Sixteen boxes of dirt, each with a TNT block at its centre,
                primed the way a player's redstone would. Before priming, the
                world is copied to run/tnt-oracle/crater-seed so that
                scripts/check_tnt_e2e.py can ignite the same sixteen boxes on our
                server and compare the craters cell by cell.

  creeper       The yield of a creeper's explosion: blocks broken against items
                dropped, the same bench as explosions.md § 5 used for TNT.

Usage:
    python3 scripts/measure_tnt_gravity.py <scenario> [trials]

Writes data/vanilla/1.20.1/normalized/tnt_<scenario>.json (gitignored).
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
from capture_entity_packets import Probe, read_varint  # noqa: E402
from measure_entities import Server  # noqa: E402
from measure_redstone import fill, forceload, freeze, gametime, name_of, read_states, save  # noqa: E402

NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
RUN = ROOT / "run" / "tnt-oracle"
PORT = int(os.environ.get("OV_TNT_PORT", "25619"))

# Well above the superflat floor (-61) and well below the build limit. See the
# `/fill` trap in explosions.md: every y below is checked against the floor.
BENCH_Y = -40
WORLD_BOTTOM = -64

FUSE = re.compile(r"\bFuse: (-?\d+)s")
TIME = re.compile(r"\bTime: (-?\d+)[,}]")
MOTION = re.compile(r"Motion: \[([-0-9.Ee]+)d, ([-0-9.Ee]+)d, ([-0-9.Ee]+)d\]")
POS = re.compile(r"Pos: \[([-0-9.Ee]+)d, ([-0-9.Ee]+)d, ([-0-9.Ee]+)d\]")
ON_GROUND = re.compile(r"OnGround: ([01])b")
BLOCK_NAME = re.compile(r'BlockState: \{(?:Properties: \{[^}]*\}, )?Name: "([^"]+)"')
GAMETIME = re.compile(r"time is (\d+)")


def check_y(*ys: int) -> None:
    for y in ys:
        if y <= WORLD_BOTTOM:
            raise RuntimeError(f"y={y} is at or below the world bottom; a fill there fails "
                               "silently and reads as destruction")


def start(name: str) -> Server:
    directory = RUN / name
    if directory.exists():
        shutil.rmtree(directory)
    server = Server(directory, port=PORT)
    freeze(server)
    # `freeze` sets peaceful, which deletes a creeper the tick it appears.
    server.batch(["difficulty normal", "gamerule sendCommandFeedback true"])
    return server


def finish(server: Server, name: str) -> None:
    """Stop, and throw the world away: the disk is shared by four agents."""
    server.stop()
    shutil.rmtree(RUN / name, ignore_errors=True)


def parse_entity(line: str) -> dict | None:
    if "following entity data" not in line:
        return None
    out: dict = {}
    for key, pattern in (("fuse", FUSE), ("time", TIME)):
        match = pattern.search(line)
        if match:
            out[key] = int(match.group(1))
    for key, pattern in (("motion", MOTION), ("pos", POS)):
        match = pattern.search(line)
        if match:
            out[key] = [float(match.group(i)) for i in (1, 2, 3)]
    match = ON_GROUND.search(line)
    if match:
        out["on_ground"] = match.group(1) == "1"
    match = BLOCK_NAME.search(line)
    if match:
        out["block"] = match.group(1)
    return out


def poll(server: Server, selector: str) -> tuple[int | None, list[dict]]:
    """Every entity the selector finds, read in one command, and the time.

    One `execute as … run data get entity @s` reads every entity in the same
    tick, which is what makes their fuses comparable to one another.
    """
    lines = server.batch([f"execute as {selector} run data get entity @s",
                          "time query gametime"])
    now = None
    entities = []
    for line in lines:
        match = GAMETIME.search(line)
        if match:
            now = int(match.group(1))
            continue
        parsed = parse_entity(line)
        if parsed is not None:
            entities.append(parsed)
    return now, entities


def count_items(server: Server) -> int | None:
    """The number of items on the ground, summed over `Item.Count`.

    Stacks merge within a tick, so counting entities undercounts. Same method as
    explosions.md § 5.
    """
    lines = server.batch([
        "scoreboard objectives add tntcount dummy",
        "execute as @e[type=minecraft:item] store result score @s tntcount "
        "run data get entity @s Item.Count",
        "scoreboard players set #total tntcount 0",
        "execute as @e[type=minecraft:item] run "
        "scoreboard players operation #total tntcount += @s tntcount",
        "scoreboard players get #total tntcount"])
    for line in lines:
        match = re.search(r"has (\d+) \[tntcount\]", line)
        if match:
            return int(match.group(1))
    return None


def wait_ticks(server: Server, count: int) -> None:
    """The server's clock, never ours: a tick full of explosions takes seconds."""
    start_tick = gametime(server)
    deadline = time.monotonic() + 600.0
    while time.monotonic() < deadline:
        if gametime(server) - start_tick >= count:
            return
        time.sleep(0.1)
    raise TimeoutError("the server never advanced its clock")


def write(name: str, document: dict) -> Path:
    out = NORMALIZED / f"tnt_{name}.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w") as f:
        json.dump(document, f, indent=1, sort_keys=True)
    print(f"wrote {out}")
    return out


# ── the motion model the measurements are compared against ─────────────────
#
# From the wiki's Entity article ("Motion of entities"): primed TNT and falling
# blocks accelerate downward by 0.04 blocks/tick², and their velocity is
# multiplied by 0.98 on every axis each tick, drag applied after the move. On
# the ground the horizontal components are multiplied by a further 0.7 and the
# vertical one by -0.5. Every number is then checked against the Motion tag.

def tnt_model(v0: tuple[float, float, float], ticks: int, ground: bool,
              gravity: float = 0.04, drag: float = 0.98) -> list[tuple[float, float, float]]:
    vx, vy, vz = v0
    out = [(vx, vy, vz)]
    for _ in range(ticks):
        vy -= gravity
        # The move itself is not modelled here: on the ground the collision
        # eats the downward part of it and sets on_ground, in the air nothing
        # is in the way.
        vx, vy, vz = vx * drag, vy * drag, vz * drag
        if ground:
            vx, vy, vz = vx * 0.7, vy * -0.5, vz * 0.7
        out.append((vx, vy, vz))
    return out


# ── scenario: capture ────────────────────────────────────────────────────────


def metadata_fields(payload: bytes) -> tuple[int, list[dict]]:
    """Set Entity Metadata, decoded as far as the value types we can size."""
    entity_id, i = read_varint(payload, 0)
    fields = []
    while i < len(payload):
        index = payload[i]
        i += 1
        if index == 0xFF:
            break
        kind, i = read_varint(payload, i)
        if kind == 0:      # byte
            value = payload[i]
            i += 1
        elif kind in (1, 14, 20):  # varint, block state, pose
            value, i = read_varint(payload, i)
        elif kind == 3:    # float
            value = struct.unpack_from(">f", payload, i)[0]
            i += 4
        elif kind == 8:    # boolean
            value = bool(payload[i])
            i += 1
        elif kind == 10:   # block position, packed into a long
            packed = struct.unpack_from(">q", payload, i)[0]
            i += 8
            x = packed >> 38
            y = (packed << 52) >> 52
            z = (packed << 26) >> 38
            value = [x, y, z]
        else:
            fields.append({"index": index, "type": kind, "value": None,
                           "rest": payload[i:].hex()})
            break
        fields.append({"index": index, "type": kind, "value": value})
    return entity_id, fields


def spawn_fields(payload: bytes) -> dict:
    entity_id, i = read_varint(payload, 0)
    i += 16
    type_id, i = read_varint(payload, i)
    x, y, z = struct.unpack_from(">ddd", payload, i)
    i += 24
    i += 3  # pitch, yaw, head yaw
    data, i = read_varint(payload, i)
    vx, vy, vz = struct.unpack_from(">hhh", payload, i)
    return {"entity_id": entity_id, "type": type_id, "pos": [x, y, z], "data": data,
            "velocity": [vx / 8000.0, vy / 8000.0, vz / 8000.0], "velocity_raw": [vx, vy, vz]}


def find_explosions(packets: list[tuple[int, bytes]], centre: tuple[float, float, float]) -> list:
    """Every packet whose payload opens with the charge's centre as three doubles.

    The id is found, not assumed: entity.hpp records that the archived wiki
    disagreed with a real server on every packet id this project checked.
    """
    found = []
    for pid, payload in packets:
        if len(payload) < 28:
            continue
        x, y, z = struct.unpack_from(">ddd", payload, 0)
        if all(abs(a - b) < 1e-3 for a, b in zip((x, y, z), centre)):
            radius = struct.unpack_from(">f", payload, 24)[0]
            count, i = read_varint(payload, 28)
            records = [struct.unpack_from(">bbb", payload, i + 3 * k) for k in range(count)]
            i += 3 * count
            tail = payload[i:]
            motion = struct.unpack_from(">fff", tail, 0) if len(tail) >= 12 else None
            found.append({"id": pid, "centre": [x, y, z], "radius": radius, "count": count,
                          "records": records[:8], "motion": motion,
                          "trailing_bytes": len(tail), "hex": payload[:64].hex()})
    return found


def measure_capture(trials: int) -> None:
    del trials
    server = start("capture")
    document: dict = {}
    try:
        server.batch(["gamerule doMobSpawning false", "time set midnight"])
        probe = Probe(PORT, name="ovprobe")
        probe.pump(3.0)
        if probe.position is None:
            raise RuntimeError("the probe was never told where it is")
        px, py, pz = probe.position
        bx, by, bz = math.floor(px), math.floor(py), math.floor(pz)
        server.batch([f"forceload add {bx - 64} {bz - 64} {bx + 64} {bz + 64}",
                      "gamemode creative ovprobe"])
        probe.pump(2.0)
        probe.drain()

        def watch(commands: list[str], seconds: float) -> list[tuple[int, bytes]]:
            server.batch(commands)
            probe.pump(seconds)
            return probe.drain()

        # ── primed TNT: the fuse's index, one field at a time ───────────────
        document["tnt_metadata"] = {}
        for label, nbt in (("baseline", "{NoGravity:1b}"),
                           ("Fuse37", "{NoGravity:1b,Fuse:37s}")):
            packets = watch(["kill @e[type=minecraft:tnt]",
                             f"summon minecraft:tnt {bx + 4.5} {by + 2} {bz + 0.5} {nbt}"], 1.0)
            spawns = [spawn_fields(p) for pid, p in packets if pid == 0x01]
            metas = [metadata_fields(p) for pid, p in packets if pid == 0x52]
            document["tnt_metadata"][label] = {"spawn": spawns, "metadata": metas}
            print(f"  tnt {label}: spawn {spawns} metadata {metas}")
        server.batch(["kill @e[type=minecraft:tnt]"])

        # ── a TNT primed by redstone: the initial velocity, as sent ─────────
        packets = watch([f"setblock {bx + 6} {by + 3} {bz} minecraft:tnt",
                         f"setblock {bx + 7} {by + 3} {bz} minecraft:redstone_block",
                         f"setblock {bx + 7} {by + 3} {bz} minecraft:air"], 1.0)
        document["tnt_primed_by_redstone"] = {
            "spawn": [spawn_fields(p) for pid, p in packets if pid == 0x01],
            "metadata": [metadata_fields(p) for pid, p in packets if pid == 0x52]}
        print(f"  redstone-primed tnt: {document['tnt_primed_by_redstone']}")
        server.batch(["kill @e[type=minecraft:tnt]"])
        probe.pump(0.5)
        probe.drain()

        # ── falling block: the data field and the start position ────────────
        document["falling_block"] = {}
        for label, commands in (
                ("summoned_red_sand", [f'summon minecraft:falling_block {bx + 4.5} {by + 6} '
                                       f'{bz + 0.5} {{BlockState:{{Name:"minecraft:red_sand"}},'
                                       f'NoGravity:1b,Time:1}}']),
                ("set_sand", [f"setblock {bx + 9} {by + 6} {bz} minecraft:sand"])):
            packets = watch(commands, 1.5)
            document["falling_block"][label] = {
                "spawn": [spawn_fields(p) for pid, p in packets if pid == 0x01],
                "metadata": [metadata_fields(p) for pid, p in packets if pid == 0x52]}
            print(f"  falling block {label}: {document['falling_block'][label]}")
        server.batch(["kill @e[type=minecraft:falling_block]"])

        # ── the Explosion packet ────────────────────────────────────────────
        centre = (bx + 3.5, float(by) + 0.06125, bz + 0.5)
        packets = watch([f"summon minecraft:tnt {centre[0]} {by} {centre[2]} "
                         "{Fuse:0,NoGravity:1b,Motion:[0.0,0.0,0.0]}"], 1.5)
        document["explosion"] = {"centre": centre, "found": find_explosions(packets, centre),
                                 "ids_seen": sorted({pid for pid, _ in packets})}
        print(f"  explosion: {document['explosion']['found']}")
        print(f"  ids seen: {[hex(i) for i in document['explosion']['ids_seen']]}")

        # ── creeper: three flags, one at a time ─────────────────────────────
        document["creeper_metadata"] = {}
        for label, extra in (("baseline", ""), ("powered", ",powered:1b"),
                             ("ignited", ",ignited:1b,Fuse:32000s")):
            packets = watch(["kill @e[type=minecraft:creeper]",
                             f"summon minecraft:creeper {bx + 8.5} {by} {bz + 8.5} "
                             f"{{NoAI:1b,Silent:1b,PersistenceRequired:1b{extra}}}"], 1.0)
            metas = [metadata_fields(p) for pid, p in packets if pid == 0x52]
            document["creeper_metadata"][label] = metas
            print(f"  creeper {label}: {metas}")
        server.batch(["kill @e[type=minecraft:creeper]"])

        # ── creeper: swelling at a survival player ──────────────────────────
        # Survival, or the creeper will not target it; Resistance V so the
        # probe survives the blast and keeps recording.
        server.batch(["gamemode survival ovprobe",
                      "effect give ovprobe minecraft:resistance 1000 4 true"])
        probe.pump(1.0)
        probe.drain()
        server.batch([f"summon minecraft:creeper {px + 2.0} {py} {pz} "
                      "{PersistenceRequired:1b,Silent:1b}"])
        started = time.monotonic()
        timeline = []
        packets_all: list[tuple[int, bytes]] = []
        while time.monotonic() - started < 6.0:
            probe.pump(0.1)
            for pid, payload in probe.drain():
                packets_all.append((pid, payload))
                if pid == 0x52:
                    entity_id, fields = metadata_fields(payload)
                    timeline.append({"t": round(time.monotonic() - started, 3),
                                     "entity": entity_id, "fields": fields})
        document["creeper_swell"] = {"metadata": timeline}
        explosions = []
        for pid, payload in packets_all:
            if len(payload) >= 28:
                x, y, z = struct.unpack_from(">ddd", payload, 0)
                if abs(x - px) < 4.0 and abs(z - pz) < 4.0 and abs(y - py) < 4.0 and \
                        0 < len(payload) < 4096:
                    radius = struct.unpack_from(">f", payload, 24)[0]
                    if 0.5 < radius < 10.0:
                        explosions.append({"id": pid, "centre": [x, y, z], "radius": radius})
        document["creeper_swell"]["explosions"] = explosions
        print(f"  creeper swell timeline: {timeline}")
        print(f"  creeper explosion: {explosions}")
    finally:
        finish(server, "capture")
    write("capture", document)


# ── scenario: tnt_motion ────────────────────────────────────────────────────


def measure_tnt_motion(trials: int) -> None:
    server = start("tnt_motion")
    samples: list[dict] = []
    columns = 10
    air_y = 100
    ground_y = -60           # standing on the superflat's grass at -61
    try:
        forceload(server, -16, -16, 96, 48)
        for trial in range(trials):
            server.batch(["kill @e[type=minecraft:tnt]"])
            # A fresh floor: the ground charges dig into it.
            fill(server, -8, -63, 16, 88, -61, 32, "minecraft:stone")
            fill(server, -8, -60, 16, 88, -50, 32, "minecraft:air")
            commands = []
            for c in range(columns):
                ax, az = c * 8, 0
                gx, gz = c * 8, 24
                for (x, y, z) in ((ax, air_y, az), (gx, ground_y, gz)):
                    commands += [f"setblock {x} {y} {z} minecraft:tnt",
                                 f"setblock {x + 1} {y} {z} minecraft:redstone_block",
                                 f"setblock {x + 1} {y} {z} minecraft:air"]
            server.batch(commands)
            deadline = time.monotonic() + 6.0
            while time.monotonic() < deadline:
                now, entities = poll(server, "@e[type=minecraft:tnt]")
                if not entities and time.monotonic() > deadline - 4.0:
                    break
                for e in entities:
                    if "fuse" not in e or "pos" not in e:
                        continue
                    column = round((e["pos"][0] - 0.5) / 8.0)
                    kind = "air" if e["pos"][2] < 12 else "ground"
                    samples.append({"trial": trial, "kind": kind, "column": column,
                                    "gametime": now, **e})
            print(f"  trial {trial + 1}/{trials}: {len(samples)} samples so far")
            wait_ticks(server, 20)
    finally:
        finish(server, "tnt_motion")

    document = {"samples": samples, "air_y": air_y, "ground_y": ground_y}
    # ── what the samples say ────────────────────────────────────────────────
    max_fuse = max((s["fuse"] for s in samples), default=None)
    document["max_fuse"] = max_fuse
    air = [s for s in samples if s["kind"] == "air"]
    ground = [s for s in samples if s["kind"] == "ground"]
    worst_vy = 0.0
    worst_h = 0.0
    worst_y = 0.0
    for s in air:
        n = 80 - s["fuse"]
        vx, vy, vz = s["motion"]
        h = math.hypot(vx, vz)
        model = tnt_model((0.0, 0.2, 0.0), n, False)
        worst_vy = max(worst_vy, abs(vy - model[n][1]))
        worst_h = max(worst_h, abs(h - 0.02 * 0.98 ** n))
        # Position: the spawn is the block's bottom centre, so y(n) is the sum
        # of the first n velocities *before* drag of each tick.
        y = float(air_y)
        v = 0.2
        for _ in range(n):
            v -= 0.04
            y += v
            v *= 0.98
        worst_y = max(worst_y, abs(s["pos"][1] - y))
    document["air"] = {"samples": len(air), "worst_vy_error": worst_vy,
                       "worst_horizontal_error": worst_h, "worst_y_error": worst_y}
    by_n: dict[int, list] = {}
    for s in ground:
        by_n.setdefault(80 - s["fuse"], []).append(
            [round(math.hypot(s["motion"][0], s["motion"][2]), 8), s["motion"][1],
             s["pos"][1], s.get("on_ground")])
    document["ground_by_tick"] = {str(k): v[:3] for k, v in sorted(by_n.items())}
    print(json.dumps({k: document[k] for k in ("max_fuse", "air")}, indent=1))
    for k in sorted(by_n)[:14]:
        print(f"  ground n={k:2d}: {by_n[k][:2]}")
    write("tnt_motion", document)


# ── scenario: chain ─────────────────────────────────────────────────────────


def measure_chain(trials: int) -> None:
    server = start("chain")
    y = BENCH_Y
    check_y(y - 2)
    ring = [(dx, dy, dz) for dx in range(-2, 3) for dz in range(-2, 3) for dy in (-1, 0, 1)
            if max(abs(dx), abs(dz)) == 2]
    records = []
    try:
        forceload(server, -32, -32, 128, 32)
        for trial in range(trials):
            server.batch(["kill @e[type=minecraft:tnt]"])
            fill(server, -4, y - 3, -4, 4, y + 3, 4, "minecraft:air")
            server.batch([f"setblock {dx} {y + dy} {dz} minecraft:tnt" for dx, dy, dz in ring])
            # The witness first, the charge second, in one batch: the console
            # runs them in the same tick, so the witness's own fuse counts the
            # ticks since the charge was summoned.
            server.batch(["summon minecraft:tnt 100.5 -40 0.5 "
                          "{Fuse:400s,NoGravity:1b,Motion:[0.0,0.0,0.0],Tags:[\"witness\"]}",
                          f"summon minecraft:tnt 0.5 {y} 0.5 "
                          "{Fuse:1s,NoGravity:1b,Motion:[0.0,0.0,0.0]}"])
            now, entities = poll(server, "@e[type=minecraft:tnt]")
            witness = [e for e in entities if e.get("pos", [0])[0] > 50]
            chained = [e for e in entities if e.get("pos", [999])[0] < 50]
            if len(witness) != 1:
                print(f"  trial {trial + 1}: no witness, skipped")
                continue
            elapsed = 400 - witness[0]["fuse"]
            records.append({"elapsed": elapsed, "gametime": now,
                            "fuses": [e["fuse"] for e in chained],
                            "motions": [e.get("motion") for e in chained],
                            "count": len(chained), "ring": len(ring)})
            print(f"  trial {trial + 1}/{trials}: {len(chained)} of {len(ring)} primed, "
                  f"elapsed {elapsed}, fuses {sorted(e['fuse'] for e in chained)}")
            server.batch(["kill @e[type=minecraft:tnt]"])
    finally:
        finish(server, "chain")
    # The charge explodes on the first entity tick after the summon. A ring TNT
    # primed then has had `elapsed - 1` ticks of its own if it is not ticked in
    # the tick it is created, `elapsed` if it is.
    initial = [f + r["elapsed"] - 1 for r in records for f in r["fuses"]]
    histogram = {}
    for f in initial:
        histogram[f] = histogram.get(f, 0) + 1
    document = {"records": records, "initial_fuse_if_not_ticked_at_birth": sorted(initial),
                "histogram": dict(sorted(histogram.items())),
                "min": min(initial, default=None), "max": max(initial, default=None),
                "distinct": len(histogram)}
    print(f"  initial fuses {document['min']}..{document['max']}, "
          f"{document['distinct']} distinct values over {len(initial)} samples")
    write("chain", document)


# ── scenario: sand ──────────────────────────────────────────────────────────


def measure_sand(trials: int) -> None:
    del trials
    server = start("sand")
    document: dict = {}
    try:
        forceload(server, -16, -16, 96, 48)
        server.batch(["gamerule doEntityDrops true"])
        fill(server, -8, -60, -8, 88, -30, 8, "minecraft:air")

        # (a) four columns of six, each held by one stone at y = -51.
        column_x = [0, 4, 8, 12]
        commands = []
        for x in column_x:
            commands.append(f"setblock {x} -51 0 minecraft:stone")
            for k in range(6):
                commands.append(f"setblock {x} {-50 + k} 0 minecraft:sand")
        server.batch(commands)
        wait_ticks(server, 10)
        removal = server.batch([f"setblock {x} -51 0 minecraft:air" for x in column_x] +
                               ["time query gametime"])
        t0 = next(int(m.group(1)) for line in removal for m in [GAMETIME.search(line)] if m)
        samples = []
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            now, entities = poll(server, "@e[type=minecraft:falling_block]")
            for e in entities:
                samples.append({"gametime": now, **e})
        wait_ticks(server, 20)
        save(server)
        world = RUN / "sand" / "world" / "region"
        cells = [(x, yy, 0) for x in column_x for yy in range(-61, -44)]
        states = read_states(world, cells)
        final = {f"{x},{yy}": name_of(s) for (x, yy, _), s in zip(cells, states)
                 if name_of(s) != "minecraft:air"}
        document["columns"] = {"removed_at": t0, "samples": samples, "final": final}
        births = {}
        for s in samples:
            if "time" in s and s["gametime"] is not None:
                key = (round(s["pos"][0] - 0.5), )
                births.setdefault(s["gametime"] - s["time"], set()).add(key)
        document["columns"]["birth_ticks_after_removal"] = sorted(
            {b - t0 for b in births})
        print(f"  columns: births after removal {document['columns']['birth_ticks_after_removal']}")
        print(f"  columns: final {final}")

        # (b) one gravel block set in mid-air: the delay before it falls.
        lines = server.batch(["setblock 20 -45 0 minecraft:gravel", "time query gametime"])
        placed = next(int(m.group(1)) for line in lines for m in [GAMETIME.search(line)] if m)
        seen = []
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            now, entities = poll(server, "@e[type=minecraft:falling_block,x=20,dx=1,y=-80,dy=60,z=-1,dz=2]")
            for e in entities:
                seen.append({"gametime": now, **e})
        document["placement"] = {"placed_at": placed, "samples": seen,
                                 "births": sorted({s["gametime"] - s["time"] - placed
                                                   for s in seen if "time" in s})}
        print(f"  placement: born {document['placement']['births']} ticks after the setblock")

        # (c) sand onto a torch, (d) sand into a pool, (e) powder into a pool.
        #
        # ⚠ The first version of this bench filled the glass up to y = -56 and
        #   the water only to -57, which left a glass **lid** on both pools:
        #   the sand and the powder landed on it and read "does not enter
        #   water". The walls now stop level with the water.
        server.batch(["kill @e[type=minecraft:item]",
                      "setblock 24 -60 0 minecraft:torch"])
        for px in (28, 32):
            fill(server, px - 1, -61, -1, px + 1, -57, 1, "minecraft:glass")
            fill(server, px, -60, 0, px, -57, 0, "minecraft:water")
        wait_ticks(server, 10)
        server.batch(["setblock 24 -50 0 minecraft:sand",
                      "setblock 28 -45 0 minecraft:sand",
                      "setblock 32 -45 0 minecraft:red_concrete_powder"])
        # Where the two pool blocks are, tick by tick, while they fall: the
        # difference between "stops at the surface" and "sinks to the floor" is
        # only visible in the trajectory.
        pool_samples = []
        deadline = time.monotonic() + 4.0
        while time.monotonic() < deadline:
            now, entities = poll(server, "@e[type=minecraft:falling_block,x=26,dx=8,"
                                         "y=-62,dy=20,z=-1,dz=2]")
            for e in entities:
                pool_samples.append({"gametime": now, **e})
        wait_ticks(server, 40)
        items = count_items(server)
        save(server)
        cells = [(24, yy, 0) for yy in range(-61, -49)] + \
                [(28, yy, 0) for yy in range(-61, -44)] + \
                [(32, yy, 0) for yy in range(-61, -44)]
        states = read_states(world, cells)
        document["torch_pool"] = {
            "items_on_ground": items,
            "samples": pool_samples,
            "cells": {f"{x},{yy}": s for (x, yy, _), s in zip(cells, states)
                      if name_of(s) not in ("minecraft:air", "minecraft:glass")}}
        print(f"  torch and pools: {document['torch_pool']['cells']}, "
              f"items {items}, {len(pool_samples)} trajectory samples")

        # (e') the "down" face again, with a floor under the water so that the
        # water cannot flow away underneath — the first run let it, and the
        # powder then fell through a cell that was about to change.
        server.batch(["setblock 80 -52 0 minecraft:stone",
                      "setblock 80 -50 0 minecraft:white_concrete_powder",
                      "setblock 79 -50 0 minecraft:stone",
                      "setblock 81 -50 0 minecraft:stone",
                      "setblock 80 -50 -1 minecraft:stone",
                      "setblock 80 -50 1 minecraft:stone"])
        wait_ticks(server, 5)
        server.batch(["setblock 80 -51 0 minecraft:water"])
        down_samples = []
        deadline = time.monotonic() + 1.5
        while time.monotonic() < deadline:
            now, entities = poll(server, "@e[type=minecraft:falling_block,x=79,dx=2,"
                                         "y=-60,dy=20,z=-1,dz=2]")
            for e in entities:
                down_samples.append({"gametime": now, **e})
        wait_ticks(server, 20)
        save(server)
        cells = [(80, yy, 0) for yy in range(-53, -48)]
        states = read_states(world, cells)
        document["powder_down_floored"] = {
            "samples": down_samples,
            "cells": {f"80,{yy}": s for (_, yy, _), s in zip(cells, states)}}
        print(f"  powder over floored water: {document['powder_down_floored']['cells']}")

        # (f) concrete powder beside water, one face at a time. The water is
        # placed **last**, so the powder is told about it by the water's own
        # neighbour update — the "sensor last" rule of redstone.md § 1.
        faces = {"up": (0, 1, 0), "down": (0, -1, 0), "north": (0, 0, -1),
                 "south": (0, 0, 1), "west": (-1, 0, 0), "east": (1, 0, 0)}
        base_x = 44
        placed_cells = {}
        commands = []
        for index, (face, (dx, dy, dz)) in enumerate(faces.items()):
            x = base_x + index * 4
            # Stone under the powder except where the water goes under it.
            if face != "down":
                commands.append(f"setblock {x} -51 0 minecraft:stone")
            else:
                commands.append(f"setblock {x} -53 0 minecraft:stone")
            commands.append(f"setblock {x} -50 0 minecraft:white_concrete_powder")
            placed_cells[face] = (x, dx, dy, dz)
        # A control with no water at all.
        commands.append(f"setblock {base_x + 28} -51 0 minecraft:stone")
        commands.append(f"setblock {base_x + 28} -50 0 minecraft:white_concrete_powder")
        server.batch(commands)
        wait_ticks(server, 5)
        server.batch([f"setblock {x + dx} {-50 + dy} {dz} minecraft:water"
                      for face, (x, dx, dy, dz) in placed_cells.items()])
        wait_ticks(server, 20)
        save(server)
        cells = []
        for face, (x, dx, dy, dz) in placed_cells.items():
            cells += [(x, -50, 0), (x, -51, 0), (x, -52, 0)]
        cells.append((base_x + 28, -50, 0))
        states = read_states(world, cells)
        document["powder_faces"] = {f"{x},{yy}": s for (x, yy, _), s in zip(cells, states)}
        document["powder_faces_layout"] = {face: v[0] for face, v in placed_cells.items()}
        print(f"  powder faces: {document['powder_faces']}")
    finally:
        finish(server, "sand")
    write("sand", document)


# ── scenario: crater ────────────────────────────────────────────────────────

CRATER_HALF = 8
CRATER_UP = 6
CRATER_SPACING = 40
SEED = RUN / "crater-seed"


def crater_boxes(trials: int) -> list[int]:
    return [index * CRATER_SPACING for index in range(trials)]


def measure_crater(trials: int) -> None:
    y = BENCH_Y
    check_y(y - CRATER_UP - 1)
    server = start("crater")
    world_dir = RUN / "crater" / "world"
    boxes = crater_boxes(trials)
    try:
        forceload(server, -24, -24, boxes[-1] + 24, 24)
        for cx in boxes:
            fill(server, cx - CRATER_HALF, y - CRATER_UP, -CRATER_HALF,
                 cx + CRATER_HALF, y + CRATER_UP, CRATER_HALF, "minecraft:dirt")
        server.batch([f"setblock {cx} {y} 0 minecraft:tnt" for cx in boxes])
        save(server)
        # The seed our server will be handed: the same sixteen boxes, unprimed.
        if SEED.exists():
            shutil.rmtree(SEED)
        SEED.mkdir(parents=True)
        shutil.copytree(world_dir / "region", SEED / "region")
        shutil.copy(world_dir / "level.dat", SEED / "level.dat")
        # Primed the way a lever would: a redstone block beside the TNT, then
        # dirt back in its place in the same tick, so the box is whole again
        # before the entity has moved.
        commands = []
        for cx in boxes:
            commands += [f"setblock {cx} {y + 1} 0 minecraft:redstone_block",
                         f"setblock {cx} {y + 1} 0 minecraft:dirt"]
        server.batch(commands)
        wait_ticks(server, 100)
        server.batch(["kill @e[type=minecraft:item]", "kill @e[type=minecraft:tnt]"])
        save(server)
        counts: dict[str, int] = {}
        for cx in boxes:
            cells = [(cx + dx, y + dy, dz)
                     for dy in range(-CRATER_UP, CRATER_UP + 1)
                     for dz in range(-CRATER_HALF, CRATER_HALF + 1)
                     for dx in range(-CRATER_HALF, CRATER_HALF + 1)]
            states = read_states(world_dir / "region", cells)
            for (bx, by, bz), state in zip(cells, states):
                if name_of(state) != "minecraft:dirt":
                    key = f"{bx - cx},{by - y},{bz}"
                    counts[key] = counts.get(key, 0) + 1
    finally:
        finish(server, "crater")
    union = len(counts)
    intersection = sum(1 for v in counts.values() if v == trials)
    print(f"  union {union}, intersection {intersection} over {trials} shots")
    write("crater", {"trials": trials, "half": CRATER_HALF, "up": CRATER_UP, "y": y,
                     "boxes": boxes, "counts": counts, "union": union,
                     "intersection": intersection})


# ── scenario: creeper ───────────────────────────────────────────────────────


def measure_creeper(trials: int) -> None:
    y = BENCH_Y
    check_y(y - 5)
    half, up = 8, 5
    total = (2 * half + 1) ** 2 * (2 * up + 1)
    server = start("creeper")
    broken, dropped = [], []
    try:
        forceload(server, -48, -48, 48, 48)
        server.batch(["gamerule doTileDrops true", "gamerule mobGriefing true"])
        for trial in range(trials):
            server.batch(["kill @e[type=minecraft:item]"])
            fill(server, -half, y - up, -half, half, y + up, half, "minecraft:dirt")
            server.batch([f"summon minecraft:creeper 0.5 {y}.0 0.5 "
                          "{NoGravity:1b,ignited:1b,Fuse:1s,NoAI:1b,Silent:1b}"])
            wait_ticks(server, 20)
            count = count_items(server)
            server.batch(["kill @e[type=minecraft:item]", "kill @e[type=minecraft:creeper]"])
            left = None
            for line in server.batch([f"fill {-half} {y - up} {-half} {half} {y + up} {half} "
                                      "minecraft:air replace minecraft:dirt"]):
                match = re.search(r"filled (\d+) block", line)
                if match:
                    left = int(match.group(1))
            gone = None if left is None else total - left
            broken.append(gone)
            dropped.append(count)
            print(f"  trial {trial + 1}/{trials}: broke {gone}, dropped {count}")
    finally:
        finish(server, "creeper")
    pairs = [(b, d) for b, d in zip(broken, dropped) if b is not None and d is not None]
    total_b = sum(b for b, _ in pairs)
    total_d = sum(d for _, d in pairs)
    document = {"broken": broken, "dropped": dropped,
                "yield": round(total_d / total_b, 4) if total_b else None}
    print(f"  {total_d}/{total_b} = {document['yield']}")
    write("creeper", document)


def measure_powder(trials: int) -> None:
    """Concrete powder with water **under** it, which the `sand` bench left open.

    Five faces of six harden a powder block in place; with water below it the
    block falls instead, and the first two runs ended with the concrete one cell
    below the water (water free to flow) and with no concrete at all (water on a
    floor). Four rigs side by side, each read tick by tick and counted for items:

      A  powder over one water cell over stone, the water walled in;
      B  the same with air under the water;
      C  powder over one cell of air over walled water over stone;
      D  sand in rig A's place, as the control.
    """
    del trials
    server = start("powder")
    document: dict = {}
    try:
        forceload(server, -16, -16, 64, 16)
        server.batch(["gamerule doEntityDrops true", "kill @e[type=minecraft:item]"])
        fill(server, -4, -60, -4, 40, -40, 4, "minecraft:air")
        rigs = {"A": (0, "minecraft:white_concrete_powder", True, 0),
                "B": (8, "minecraft:white_concrete_powder", False, 0),
                "C": (16, "minecraft:white_concrete_powder", True, 1),
                "D": (24, "minecraft:sand", True, 0)}
        y = -45
        setup, triggers = [], []
        for name, (x, block, floored, gap) in rigs.items():
            water_y = y - 1 - gap
            # Walls around the water cell and the powder cell, so nothing flows
            # sideways and the only open face of the water is the one tested.
            for wy in range(water_y - 1, y + 1):
                for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    setup.append(f"setblock {x + dx} {wy} {dz} minecraft:glass")
            if floored:
                setup.append(f"setblock {x} {water_y - 1} 0 minecraft:stone")
            setup.append(f"setblock {x} {y} 0 {block}")
            # The block starts supported by a stone where the water will be,
            # and the water replaces that stone last: the sensor-last rule.
            setup.append(f"setblock {x} {water_y} 0 minecraft:stone")
            if gap:
                setup.append(f"setblock {x} {y - 1} 0 minecraft:stone")
                triggers.append(f"setblock {x} {y - 1} 0 minecraft:air")
            triggers.append(f"setblock {x} {water_y} 0 minecraft:water")
        server.batch(setup)
        wait_ticks(server, 5)
        # Water first, gaps opened after it, all in one tick.
        ordered = [t for t in triggers if "water" in t] + [t for t in triggers if "air" in t]
        lines = server.batch(ordered + ["time query gametime"])
        t0 = next(int(m.group(1)) for line in lines for m in [GAMETIME.search(line)] if m)
        samples = []
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            now, entities = poll(server, "@e[type=minecraft:falling_block]")
            for e in entities:
                samples.append({"gametime": now, "since": now - t0, **e})
        wait_ticks(server, 20)
        items = []
        for line in server.batch(["execute as @e[type=minecraft:item] run data get entity @s"]):
            if "following entity data" in line:
                pos = POS.search(line)
                item = re.search(r'id: "([^"]+)"', line)
                items.append({"pos": [float(pos.group(i)) for i in (1, 2, 3)] if pos else None,
                              "item": item.group(1) if item else None})
        save(server)
        world = RUN / "powder" / "world" / "region"
        cells = [(x, yy, 0) for (x, _, _, _) in rigs.values() for yy in range(y - 4, y + 1)]
        states = read_states(world, cells)
        document = {
            "t0": t0, "samples": samples, "items": items,
            "rigs": {name: {"x": x, "block": block, "floored": floored, "gap": gap}
                     for name, (x, block, floored, gap) in rigs.items()},
            "cells": {f"{x},{yy}": s for (x, yy, _), s in zip(cells, states)}}
        for name, (x, _, _, _) in rigs.items():
            column = {yy: document["cells"][f"{x},{yy}"] for yy in range(y - 4, y + 1)}
            trail = sorted({(s["time"], round(s["pos"][1], 4)) for s in samples
                            if abs(s["pos"][0] - (x + 0.5)) < 0.01 and "time" in s})
            print(f"  rig {name}: {column}\n         trail {trail[:6]}")
        print(f"  items: {items}")
    finally:
        finish(server, "powder")
    write("powder", document)


#: Where the `records` bench puts its charges, relative to the probe: sixteen
#: blocks apart so no crater reaches another, all within the 64 blocks inside
#: which a player is sent the packet. Shared with scripts/check_tnt_e2e.py.
RECORD_OFFSETS = [(dx, dz) for dx in (-40, -24, -8, 8, 24, 40) for dz in (-40, -24, -8, 8, 24, 40)
                  if max(abs(dx), abs(dz)) >= 16]


def measure_records(trials: int) -> None:
    """How many records an Explosion packet carries, on clean ground.

    The capture's one packet (827 records) went off beside an earlier crater,
    so its count cannot be compared with anything. This is the clean version:
    32 TNT blocks on untouched grass, primed by redstone — the same path a
    player's flint and steel takes — and every packet the probe receives.
    """
    del trials
    server = start("records")
    document: dict = {}
    try:
        server.batch(["gamerule doMobSpawning false"])
        probe = Probe(PORT, name="ovprobe")
        probe.pump(3.0)
        if probe.position is None:
            raise RuntimeError("the probe was never told where it is")
        px, py, pz = probe.position
        bx, by, bz = math.floor(px), math.floor(py), math.floor(pz)
        server.batch([f"forceload add {bx - 64} {bz - 64} {bx + 64} {bz + 64}",
                      "gamemode creative ovprobe"])
        probe.pump(2.0)
        probe.drain()
        commands = []
        for dx, dz in RECORD_OFFSETS:
            x, z = bx + dx, bz + dz
            commands += [f"setblock {x} {by} {z} minecraft:tnt",
                         f"setblock {x + 1} {by} {z} minecraft:redstone_block",
                         f"setblock {x + 1} {by} {z} minecraft:air"]
        server.batch(commands)
        probe.pump(7.0)
        packets = probe.drain()
        counts = []
        for pid, payload in packets:
            if pid != 0x1D or len(payload) < 28:
                continue
            count, _ = read_varint(payload, 28)
            x, y, z = struct.unpack_from(">ddd", payload, 0)
            counts.append({"centre": [x - bx, y - by, z - bz], "count": count})
        document = {"offsets": RECORD_OFFSETS, "ground_y_offset": 0, "packets": counts}
        values = [c["count"] for c in counts]
        if values:
            mean = sum(values) / len(values)
            sd = (sum((v - mean) ** 2 for v in values) / max(1, len(values) - 1)) ** 0.5
            document["mean"] = mean
            document["sd"] = sd
            print(f"  {len(values)} explosions, records mean {mean:.1f} sd {sd:.1f}, "
                  f"range {min(values)}..{max(values)}")
    finally:
        finish(server, "records")
    write("records", document)


SCENARIOS = {
    "records": (measure_records, 1),
    "powder": (measure_powder, 1),
    "capture": (measure_capture, 1),
    "tnt_motion": (measure_tnt_motion, 5),
    "chain": (measure_chain, 8),
    "sand": (measure_sand, 1),
    "crater": (measure_crater, 16),
    "creeper": (measure_creeper, 12),
}


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] not in SCENARIOS:
        print(f"usage: {sys.argv[0]} <{'|'.join(SCENARIOS)}> [trials]")
        return 2
    function, default = SCENARIOS[sys.argv[1]]
    trials = int(sys.argv[2]) if len(sys.argv) > 2 else default
    function(trials)
    return 0


if __name__ == "__main__":
    sys.exit(main())
