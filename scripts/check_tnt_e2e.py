#!/usr/bin/env python3
"""Drive our own server with a probe client: light TNT, drop sand, read what comes back.

The unit tests prove the rules against the numbers the vanilla server gave. This
proves the server carries them out, on a socket, the way a client sees it — and,
for the crater, that the whole path from a player's flint and steel to a hole in
a save gives the same hole vanilla gives.

Scenarios (each starts and stops its own ov_dedicated on port 25621):

  probe    A superflat world. The probe places a TNT block and lights it; then
           places a stone with sand on top and breaks the stone. What it must
           see: the primed TNT spawned (type, velocity), its fuse counting down
           in index 8, the Explosion packet (0x1D) with its records, the Block
           Updates emptying the crater; then the falling block spawned with
           sand's state in `data`, and the Block Update that puts it down.

  lab      A copy of run/lab, served untouched for ten seconds. Nothing may
           fall: vanilla does not re-evaluate a loaded world, and neither may we.

  crater   A copy of run/tnt-oracle/crater-seed — the sixteen boxes of dirt the
           vanilla bench built, each with a TNT block at its centre, before
           vanilla primed them. The probe lights each one with flint and steel,
           the server is stopped (it saves), and the boxes are read back from the
           region files with the same reader the vanilla bench used. Compared
           to scripts/measure_tnt_gravity.py crater cell by cell: union,
           intersection, mean frequency difference — and a witness, our crater
           moved one block along x, which must score clearly worse.

Usage: python3 scripts/check_tnt_e2e.py <probe|lab|crater> [path/to/ov_dedicated]
"""
from __future__ import annotations

import json
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_entity_packets import Probe, read_varint, varint  # noqa: E402
from measure_redstone import name_of, read_states  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
RUN = ROOT / "run" / "tnt-e2e"
PORT = 25621
BINARY = ROOT / "build" / "macos-debug" / "bin" / "ov_dedicated"

with open(NORMALIZED / "registries.json") as f:
    REGISTRIES = json.load(f)["registries"]
ITEMS = REGISTRIES["minecraft:item"]["entries"]
ENTITY_TYPES = REGISTRIES["minecraft:entity_type"]["entries"]
TNT_TYPE = ENTITY_TYPES.index("minecraft:tnt")
FALLING_TYPE = ENTITY_TYPES.index("minecraft:falling_block")


def item(name: str) -> int:
    return ITEMS.index(name)


def packed(x: int, y: int, z: int) -> bytes:
    value = ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)
    return struct.pack(">Q", value)


def unpacked(data: bytes, i: int) -> tuple[tuple[int, int, int], int]:
    value = struct.unpack_from(">q", data, i)[0]
    x = value >> 38
    y = value & 0xFFF
    if y >= 0x800:
        y -= 0x1000
    z = (value >> 12) & 0x3FFFFFF
    if z >= 0x2000000:
        z -= 0x4000000
    return (x, y, z), i + 8


class Hand:
    """What the probe needs to act, not just to watch: a creative hand."""

    def __init__(self, probe: Probe) -> None:
        self.probe = probe
        self.sequence = 0

    def hold(self, name: str) -> None:
        # Set Creative Mode Slot into hotbar slot 0 (window slot 36), then
        # select it. Slot data: present, item id, count, no NBT.
        self.probe.send(0x2B, struct.pack(">h", 36) + bytes([1]) + varint(item(name)) +
                        bytes([1, 0]))
        self.probe.send(0x28, struct.pack(">h", 0))

    def use_on(self, x: int, y: int, z: int, face: int = 1) -> None:
        self.sequence += 1
        self.probe.send(0x31, varint(0) + packed(x, y, z) + varint(face) +
                        struct.pack(">fff", 0.5, 1.0, 0.5) + bytes([0]) + varint(self.sequence))

    def break_block(self, x: int, y: int, z: int) -> None:
        self.sequence += 1
        self.probe.send(0x1D, varint(0) + packed(x, y, z) + bytes([1]) + varint(self.sequence))

    def stand(self, x: float, y: float, z: float) -> None:
        self.probe.send(0x14, struct.pack(">ddd", x, y, z) + bytes([1]))


def start_server(world: Path, ticks: int, extra: list[str] | None = None) -> subprocess.Popen:
    if not BINARY.exists():
        raise SystemExit(f"{BINARY} not found; build ov_dedicated first")
    log = open(RUN / f"{world.name}.log", "w")
    process = subprocess.Popen([str(BINARY), f"--port={PORT}", f"--world={world}",
                                f"--ticks={ticks}", "--log-level=debug", *(extra or [])],
                               cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    time.sleep(2.5)
    return process


def stop_server(process: subprocess.Popen) -> None:
    process.terminate()
    try:
        process.wait(timeout=60)
    except subprocess.TimeoutExpired:
        process.kill()


def parse_spawn(payload: bytes) -> dict:
    entity_id, i = read_varint(payload, 0)
    i += 16
    type_id, i = read_varint(payload, i)
    x, y, z = struct.unpack_from(">ddd", payload, i)
    i += 24 + 3
    data, i = read_varint(payload, i)
    vx, vy, vz = struct.unpack_from(">hhh", payload, i)
    return {"id": entity_id, "type": type_id, "pos": (x, y, z), "data": data,
            "velocity": (vx / 8000.0, vy / 8000.0, vz / 8000.0)}


def parse_fuse(payload: bytes) -> tuple[int, int | None]:
    entity_id, i = read_varint(payload, 0)
    while i < len(payload) and payload[i] != 0xFF:
        index = payload[i]
        kind, j = read_varint(payload, i + 1)
        if index == 8 and kind == 1:
            value, _ = read_varint(payload, j)
            return entity_id, value
        return entity_id, None
    return entity_id, None


def parse_explosion(payload: bytes) -> dict:
    x, y, z = struct.unpack_from(">ddd", payload, 0)
    radius = struct.unpack_from(">f", payload, 24)[0]
    count, i = read_varint(payload, 28)
    records = [struct.unpack_from(">bbb", payload, i + 3 * k) for k in range(count)]
    i += 3 * count
    motion = struct.unpack_from(">fff", payload, i)
    return {"centre": (x, y, z), "radius": radius, "count": count, "records": records,
            "motion": motion, "trailing": len(payload) - i - 12}


def block_updates(packets: list) -> list[tuple[tuple[int, int, int], int]]:
    out = []
    for pid, payload in packets:
        if pid == 0x0A:
            pos, i = unpacked(payload, 0)
            state, _ = read_varint(payload, i)
            out.append((pos, state))
    return out


# ── probe ───────────────────────────────────────────────────────────────────


def check_probe() -> int:
    world = RUN / "probe"
    shutil.rmtree(world, ignore_errors=True)
    world.mkdir(parents=True)
    server = start_server(world, 1400)
    failures: list[str] = []
    report: dict = {}
    try:
        probe = Probe(PORT, name="ovtnt")
        hand = Hand(probe)
        probe.pump(3.0)
        probe.drain()

        # A TNT block on the grass, lit by flint and steel.
        hand.hold("minecraft:tnt")
        hand.use_on(3, -61, 3, face=1)
        probe.pump(0.5)
        hand.hold("minecraft:flint_and_steel")
        hand.use_on(3, -60, 3, face=1)
        started = time.monotonic()
        timeline: list[tuple[float, int, bytes]] = []
        while time.monotonic() - started < 7.0:
            probe.pump(0.05)
            for pid, payload in probe.drain():
                timeline.append((time.monotonic() - started, pid, payload))
        spawns = [(t, parse_spawn(p)) for t, pid, p in timeline if pid == 0x01]
        tnt = [(t, s) for t, s in spawns if s["type"] == TNT_TYPE]
        if len(tnt) != 1:
            failures.append(f"expected one primed TNT spawned, saw {len(tnt)}")
        else:
            t_spawn, spawn = tnt[0]
            fuses = [parse_fuse(p) for t, pid, p in timeline if pid == 0x52]
            fuses = [v for eid, v in fuses if eid == spawn["id"] and v is not None]
            explosions = [(t, parse_explosion(p)) for t, pid, p in timeline if pid == 0x1D]
            removed = [t for t, pid, p in timeline if pid == 0x3E]
            updates = [(pos, state) for pos, state in block_updates(
                [(pid, p) for t, pid, p in timeline if t > t_spawn + 3.0])]
            horizontal = (spawn["velocity"][0] ** 2 + spawn["velocity"][2] ** 2) ** 0.5
            report["tnt"] = {
                "spawn": spawn, "fuse_updates": len(fuses),
                "first_fuses": fuses[:3], "last_fuses": fuses[-3:],
                "explosions": [{"t": round(t - t_spawn, 3), **{k: v for k, v in e.items()
                                                               if k != "records"}}
                               for t, e in explosions],
                "air_updates_after": sum(1 for _, s in updates if s == 0),
            }
            if abs(spawn["velocity"][1] - 0.2) > 1.0 / 8000.0:
                failures.append(f"primed TNT sent vy={spawn['velocity'][1]}, measured 0.2")
            if abs(horizontal - 0.02) > 2.0 / 8000.0:
                failures.append(f"primed TNT sent |h|={horizontal:.5f}, measured 0.02")
            if fuses[:2] != [79, 78] or fuses[-1] != 1 or len(fuses) != 79:
                failures.append(f"fuse sequence {fuses[:3]} … {fuses[-3:]} ({len(fuses)}), "
                                "measured 79, 78 … 1")
            if len(explosions) != 1:
                failures.append(f"expected one Explosion packet, saw {len(explosions)}")
            else:
                t_boom, boom = explosions[0]
                if boom["radius"] != 4.0 or boom["trailing"] != 0:
                    failures.append(f"explosion radius {boom['radius']}, trailing {boom['trailing']}")
                if abs(boom["centre"][1] - (-60.0 + 0.98 * 0.0625)) > 1e-6:
                    failures.append(f"explosion centre y {boom['centre'][1]}")
                # 80 ticks is four seconds; the probe's clock is a wall clock
                # and the server's tick can be skipped under contention, so the
                # window is generous and the exact count is the fuse above.
                if not 3.5 < t_boom - t_spawn < 5.5:
                    failures.append(f"exploded {t_boom - t_spawn:.2f} s after spawning")
                if report["tnt"]["air_updates_after"] == 0:
                    failures.append("no Block Update emptied the crater")
            if not removed:
                failures.append("the primed TNT was never removed")

        # Sand on a stone, the stone broken.
        probe.drain()
        hand.hold("minecraft:stone")
        hand.use_on(8, -61, 8, face=1)          # stone at (8, -60, 8)
        probe.pump(0.3)
        hand.use_on(8, -60, 8, face=1)          # stone at (8, -59, 8)
        probe.pump(0.3)
        hand.hold("minecraft:sand")
        hand.use_on(8, -59, 8, face=1)          # sand at (8, -58, 8)
        probe.pump(0.5)
        probe.drain()
        hand.break_block(8, -60, 8)
        probe.pump(0.2)
        hand.break_block(8, -59, 8)
        probe.pump(3.0)
        packets = probe.drain()
        falls = [parse_spawn(p) for pid, p in packets if pid == 0x01]
        falls = [s for s in falls if s["type"] == FALLING_TYPE]
        updates = block_updates(packets)
        sand_state = 112
        landed = [pos for pos, state in updates if state == sand_state]
        report["sand"] = {"spawns": falls, "landed": landed}
        if len(falls) != 1 or falls[0]["data"] != sand_state:
            failures.append(f"expected one falling block carrying state {sand_state}, saw {falls}")
        if (8, -60, 8) not in landed:
            failures.append(f"sand did not land at (8, -60, 8): updates {landed}")
    finally:
        stop_server(server)
        shutil.rmtree(world, ignore_errors=True)
    print(json.dumps(report, indent=1, default=str))
    return report_failures(failures, "the TNT went off and the sand came down")


# ── lab ─────────────────────────────────────────────────────────────────────


def check_lab() -> int:
    source = ROOT / "run" / "lab"
    world = RUN / "lab"
    shutil.rmtree(world, ignore_errors=True)
    shutil.copytree(source, world)
    server = start_server(world, 400)
    failures: list[str] = []
    try:
        probe = Probe(PORT, name="ovlab")
        probe.pump(10.0)
        packets = probe.drain()
        spawns = [parse_spawn(p) for pid, p in packets if pid == 0x01]
        falling = [s for s in spawns if s["type"] == FALLING_TYPE]
        tnt = [s for s in spawns if s["type"] == TNT_TYPE]
        updates = block_updates(packets)
        print(f"lab: {len(spawns)} entity spawns, {len(falling)} falling blocks, "
              f"{len(tnt)} primed TNT, {len(updates)} block updates in ten seconds")
        if falling or tnt:
            failures.append(f"the lab moved on load: {falling + tnt}")
    finally:
        stop_server(server)
        shutil.rmtree(world, ignore_errors=True)
    return report_failures(failures, "the lab loaded and nothing fell")


# ── crater ──────────────────────────────────────────────────────────────────


def compare(ours: dict[str, int], theirs: dict[str, int], trials: int) -> dict:
    cells = set(ours) | set(theirs)
    union_ours = set(ours)
    union_theirs = set(theirs)
    core_ours = {c for c, n in ours.items() if n == trials}
    core_theirs = {c for c, n in theirs.items() if n == trials}
    diff = sum(abs(ours.get(c, 0) - theirs.get(c, 0)) for c in cells) / trials
    return {
        "union": {"ours": len(union_ours), "theirs": len(union_theirs),
                  "agree": len(union_ours & union_theirs),
                  "ours_only": len(union_ours - union_theirs),
                  "theirs_only": len(union_theirs - union_ours)},
        "intersection": {"ours": len(core_ours), "theirs": len(core_theirs),
                         "agree": len(core_ours & core_theirs),
                         "ours_only": len(core_ours - core_theirs),
                         "theirs_only": len(core_theirs - core_ours)},
        "mean_frequency_difference": round(diff / max(1, len(cells)), 4),
        "cells": len(cells),
    }


def check_crater() -> int:
    with open(NORMALIZED / "tnt_crater.json") as f:
        vanilla = json.load(f)
    seed = ROOT / "run" / "tnt-oracle" / "crater-seed"
    world = RUN / "crater"
    shutil.rmtree(world, ignore_errors=True)
    shutil.copytree(seed, world)
    boxes = vanilla["boxes"]
    y, half, up, trials = vanilla["y"], vanilla["half"], vanilla["up"], vanilla["trials"]
    server = start_server(world, 20 * (12 + 7 * len(boxes)))
    try:
        probe = Probe(PORT, name="ovcrater")
        hand = Hand(probe)
        probe.pump(3.0)
        hand.hold("minecraft:flint_and_steel")
        for cx in boxes:
            # Stand over the box so its chunks are streamed and resident before
            # the charge goes off: an explosion reads only resident chunks,
            # and a box half in a chunk nobody loaded would read as half air.
            hand.stand(cx + 0.5, float(y + up + 1), 0.5)
            probe.pump(1.5)
            hand.use_on(cx, y, 0, face=1)
            probe.pump(5.0)
            probe.drain()
            print(f"  box at x={cx} lit")
    finally:
        stop_server(server)
    ours: dict[str, int] = {}
    for cx in boxes:
        cells = [(cx + dx, y + dy, dz)
                 for dy in range(-up, up + 1)
                 for dz in range(-half, half + 1)
                 for dx in range(-half, half + 1)]
        states = read_states(world / "region", cells)
        for (bx, by, bz), state in zip(cells, states):
            if name_of(state) != "minecraft:dirt":
                key = f"{bx - cx},{by - y},{bz}"
                ours[key] = ours.get(key, 0) + 1
    shutil.rmtree(world, ignore_errors=True)

    theirs = vanilla["counts"]
    result = compare(ours, theirs, trials)
    # The witness: the same crater, one block along x. A comparison that
    # scores it as well as the real one is measuring nothing.
    shifted = {}
    for key, n in ours.items():
        dx, dy, dz = (int(v) for v in key.split(","))
        shifted[f"{dx + 1},{dy},{dz}"] = n
    result["witness_shifted_one_block"] = compare(shifted, theirs, trials)
    result["ours_raw"] = ours
    out = NORMALIZED / "tnt_e2e_crater.json"
    with open(out, "w") as f:
        json.dump(result, f, indent=1, sort_keys=True)
    printable = {k: v for k, v in result.items() if k != "ours_raw"}
    print(json.dumps(printable, indent=1))
    failures = []
    union = result["union"]
    witness = result["witness_shifted_one_block"]["union"]
    if union["agree"] < 0.9 * max(union["ours"], union["theirs"]):
        failures.append(f"union agreement {union['agree']} of {union['ours']}/{union['theirs']}")
    if witness["agree"] >= union["agree"]:
        failures.append("the shifted witness scores as well as the real crater")
    return report_failures(failures, "our crater is vanilla's crater")


# ── records ─────────────────────────────────────────────────────────────────


def check_records() -> int:
    """The same 32 charges as measure_tnt_gravity.py records, lit by our probe."""
    from measure_tnt_gravity import RECORD_OFFSETS  # noqa: E402
    with open(NORMALIZED / "tnt_records.json") as f:
        vanilla = json.load(f)
    world = RUN / "records"
    shutil.rmtree(world, ignore_errors=True)
    world.mkdir(parents=True)
    server = start_server(world, 20 * 60)
    counts: list[int] = []
    try:
        probe = Probe(PORT, name="ovrecords")
        hand = Hand(probe)
        probe.pump(3.0)
        px, py, pz = probe.position or (0.5, -60.0, 0.5)
        bx, by, bz = int(px // 1), int(py // 1), int(pz // 1)
        hand.hold("minecraft:tnt")
        for dx, dz in RECORD_OFFSETS:
            hand.use_on(bx + dx, by - 1, bz + dz, face=1)
            probe.pump(0.05)
        probe.pump(1.0)
        hand.hold("minecraft:flint_and_steel")
        for dx, dz in RECORD_OFFSETS:
            hand.use_on(bx + dx, by, bz + dz, face=1)
            probe.pump(0.05)
        probe.pump(7.0)
        for pid, payload in probe.drain():
            if pid == 0x1D:
                counts.append(parse_explosion(payload)["count"])
    finally:
        stop_server(server)
        shutil.rmtree(world, ignore_errors=True)
    failures = []
    if len(counts) != len(RECORD_OFFSETS):
        failures.append(f"{len(counts)} explosions for {len(RECORD_OFFSETS)} charges")
    if counts:
        mean = sum(counts) / len(counts)
        sd = (sum((v - mean) ** 2 for v in counts) / max(1, len(counts) - 1)) ** 0.5
        theirs, their_sd = vanilla.get("mean"), vanilla.get("sd")
        print(f"records: ours mean {mean:.1f} sd {sd:.1f} over {len(counts)}, "
              f"vanilla mean {theirs:.1f} sd {their_sd:.1f} over {len(vanilla['packets'])}")
        with open(NORMALIZED / "tnt_e2e_records.json", "w") as f:
            json.dump({"counts": counts, "mean": mean, "sd": sd,
                       "vanilla_mean": theirs, "vanilla_sd": their_sd}, f, indent=1)
        # Two standard errors of the difference: a record list short of the
        # air cells would miss by hundreds, not by this.
        standard_error = ((sd ** 2) / len(counts) +
                          (their_sd ** 2) / max(1, len(vanilla["packets"]))) ** 0.5
        if abs(mean - theirs) > max(3.0 * standard_error, 0.02 * theirs):
            failures.append(f"record count {mean:.1f} against vanilla {theirs:.1f}")
    return report_failures(failures, "our explosion packets carry vanilla's records")


def report_failures(failures: list[str], success: str) -> int:
    if failures:
        print()
        for line in failures:
            print(f"FAIL {line}")
        return 1
    print(f"\n{success}")
    return 0


def main() -> int:
    global BINARY
    scenarios = {"probe": check_probe, "lab": check_lab, "crater": check_crater,
                 "records": check_records}
    if len(sys.argv) < 2 or sys.argv[1] not in scenarios:
        print(__doc__)
        return 2
    if len(sys.argv) > 2:
        BINARY = Path(sys.argv[2])
    RUN.mkdir(parents=True, exist_ok=True)
    return scenarios[sys.argv[1]]()


if __name__ == "__main__":
    sys.exit(main())
