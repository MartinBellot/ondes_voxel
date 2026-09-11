#!/usr/bin/env python3
"""Drive our own server with a probe client: a plank forest burns, a player burns.

The unit tests prove the rules against the numbers the vanilla server gave
(scripts/measure_fire.py). This proves the server carries them out, on a
socket, the way a client sees it. The world is built through the server's own
console (stdin), exactly as the vanilla bench built its rigs.

Scenarios (each starts and stops its own ov_dedicated on port 25633, in a
fresh superflat world under run/fire-e2e/):

  forest   A 7x7x3 block of oak planks, one fire set on top. For a minute the
           probe records every Block Update: how many cells became fire, how
           many planks were eaten, when the first spread came, and that no
           item entity was ever spawned (burnt blocks drop nothing).

  player   The probe, in survival, stands in a fire: Set Health must fall, its
           own shared-flags byte must gain 0x01. The fire is removed: the
           health keeps falling once a second, then the flag clears. Then Fire
           Resistance, and the fire again: the flag is set and the health holds.

  zombie   A zombie summoned at noon in the open must catch fire (flag 0x01)
           and take on_fire damage.

Usage: python3 scripts/check_fire_e2e.py <forest|player|zombie|all> [ov_dedicated]
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
from capture_entity_packets import Probe, read_varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
GENERATED = ROOT / "data" / "vanilla" / "1.20.1" / "generated"
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
RUN = ROOT / "run" / "fire-e2e"
PORT = 25633
BINARY = ROOT / "build" / "macos-debug" / "bin" / "ov_dedicated"

CB_LOGIN = 0x28
CB_SPAWN_ENTITY = 0x01
CB_BLOCK_UPDATE = 0x0A
CB_DAMAGE_EVENT = 0x18
CB_METADATA = 0x52
CB_SET_HEALTH = 0x57

with open(GENERATED / "reports" / "blocks.json") as f:
    BLOCKS = json.load(f)
with open(NORMALIZED / "registries.json") as f:
    REGISTRIES = json.load(f)["registries"]
ENTITY_TYPES = REGISTRIES["minecraft:entity_type"]["entries"]
ITEM_TYPE = ENTITY_TYPES.index("minecraft:item")
ZOMBIE_TYPE = ENTITY_TYPES.index("minecraft:zombie")


def state_ids(name: str) -> set[int]:
    return {s["id"] for s in BLOCKS[name]["states"]}


FIRE = state_ids("minecraft:fire")
PLANKS = state_ids("minecraft:oak_planks")
AIR = 0


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


def shared_flags(payload: bytes) -> tuple[int, int | None]:
    entity, i = read_varint(payload, 0)
    while i < len(payload) and payload[i] != 0xFF:
        index = payload[i]
        kind, j = read_varint(payload, i + 1)
        if index == 0 and kind == 0:
            return entity, payload[j]
        return entity, None
    return entity, None


class Server:
    def __init__(self, name: str, ticks: int, survival: bool) -> None:
        if not BINARY.exists():
            raise SystemExit(f"{BINARY} not found; build ov_dedicated first")
        self.world = RUN / name
        shutil.rmtree(self.world, ignore_errors=True)
        self.world.mkdir(parents=True)
        self.log = open(RUN / f"{name}.log", "w")
        args = [str(BINARY), f"--port={PORT}", f"--world={self.world}", f"--ticks={ticks}"]
        if survival:
            args.append("--survival")
        self.process = subprocess.Popen(args, cwd=ROOT, stdin=subprocess.PIPE,
                                        stdout=self.log, stderr=subprocess.STDOUT, text=True)
        time.sleep(2.5)

    def command(self, *lines: str) -> None:
        assert self.process.stdin is not None
        for line in lines:
            self.process.stdin.write(line + "\n")
        self.process.stdin.flush()

    def stop(self) -> None:
        self.process.terminate()
        try:
            self.process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            self.process.kill()
        shutil.rmtree(self.world, ignore_errors=True)


def join(name: str) -> tuple[Probe, int]:
    """Our Debug server can take more than its own 20 s to send the first
    chunk: try again rather than measure a server with nobody on it."""
    for attempt in range(6):
        try:
            probe = Probe(PORT, name=name)
            probe.pump(4.0)
            for pid, payload in probe.captured:
                if pid == CB_LOGIN:
                    return probe, struct.unpack_from(">i", payload, 0)[0]
            return probe, -1
        except (OSError, EOFError):
            time.sleep(5.0)
    raise SystemExit("could not join our server")


CB_UPDATE_TIME = 0x5E


def record(probe: Probe, ticks: int, timeout: float = 600.0) -> list[tuple[float, int, bytes]]:
    """Everything the probe is sent while the server runs `ticks` game ticks.

    Counted in the server's own ticks — the world age of Update Time, sent
    every twenty — never in seconds: a Debug server on a busy machine was
    measured at 2.4 ticks a second, and a five-second window then holds eleven
    ticks, less than a player's twenty-tick grace in fire. The times in the
    result are game ticks since the start, to the resolution of one packet."""
    out = []
    start_age: int | None = None
    age = 0
    started = time.monotonic()
    while time.monotonic() - started < timeout:
        probe.pump(0.05)
        for pid, payload in probe.drain():
            if pid == CB_UPDATE_TIME:
                age = struct.unpack_from(">q", payload, 0)[0]
                if start_age is None:
                    start_age = age
            elapsed = 0 if start_age is None else age - start_age
            out.append((float(elapsed), pid, payload))
        if start_age is not None and age - start_age >= ticks:
            return out
    raise SystemExit(f"the server did not run {ticks} ticks in {timeout:.0f} s")


def report_failures(failures: list[str], ok: str) -> int:
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1
    print(f"OK: {ok}")
    return 0


# ── forest ──────────────────────────────────────────────────────────────────

def check_forest() -> int:
    server = Server("forest", 3000, survival=False)
    failures: list[str] = []
    report: dict = {}
    try:
        probe, _ = join("ovforest")
        server.command("gamerule doFireTick true", "time set noon", "weather clear",
                       "fill 4 -60 4 10 -58 10 minecraft:oak_planks")
        probe.pump(2.0)
        probe.drain()
        server.command("setblock 7 -57 7 minecraft:fire")
        timeline = record(probe, 1200)
        fires: dict[tuple, float] = {}
        eaten: dict[tuple, float] = {}
        for t, pid, payload in timeline:
            if pid != CB_BLOCK_UPDATE:
                continue
            pos, i = unpacked(payload, 0)
            state, _ = read_varint(payload, i)
            inside = 4 <= pos[0] <= 10 and 4 <= pos[2] <= 10 and -60 <= pos[1] <= -58
            if state in FIRE:
                fires.setdefault(pos, t)
                if inside:
                    eaten.setdefault(pos, t)
            elif state == AIR and inside:
                eaten.setdefault(pos, t)
        items = sum(1 for _, pid, p in timeline
                    if pid == CB_SPAWN_ENTITY and read_varint(p, read_varint(p, 0)[1] + 16)[0] ==
                    ITEM_TYPE)
        spread = sorted(t for pos, t in fires.items() if pos != (7, -57, 7))
        report = {"fire_cells": len(fires), "planks_eaten": len(eaten),
                  "first_spread_ticks": round(spread[0]) if spread else None,
                  "items_spawned": items}
        if len(fires) < 5:
            failures.append(f"fire reached only {len(fires)} cells in a minute")
        if not eaten:
            failures.append("no plank was eaten")
        if items:
            failures.append(f"{items} item entities spawned: burnt blocks drop nothing")
    finally:
        server.stop()
    print(json.dumps(report, indent=1))
    return report_failures(failures, "the planks burnt, and dropped nothing")


# ── player ──────────────────────────────────────────────────────────────────

def health_series(timeline) -> list[tuple[float, float]]:
    return [(round(t, 2), struct.unpack_from(">f", p, 0)[0])
            for t, pid, p in timeline if pid == CB_SET_HEALTH]


def own_flags(timeline, entity: int) -> list[tuple[float, int]]:
    out = []
    for t, pid, p in timeline:
        if pid == CB_METADATA:
            who, flags = shared_flags(p)
            if who == entity and flags is not None:
                out.append((round(t, 2), flags))
    return out


def check_player() -> int:
    server = Server("player", 4000, survival=True)
    failures: list[str] = []
    report: dict = {}
    try:
        probe, entity = join("ovfire")
        probe.pump(1.0)
        if probe.position is None:
            raise SystemExit("the probe was never placed")
        x, y, z = (int(v // 1) for v in probe.position)
        server.command("gamerule doFireTick false", "time set noon")
        probe.drain()
        server.command(f"setblock {x} {y} {z} minecraft:fire")
        burning = record(probe, 100)
        server.command(f"setblock {x} {y} {z} minecraft:air")
        after = record(probe, 200)
        server.command("effect give ovfire minecraft:fire_resistance 60 0 true",
                       "effect give ovfire minecraft:instant_health 1 3 true")
        probe.pump(1.0)
        probe.drain()
        server.command(f"setblock {x} {y} {z} minecraft:fire")
        resisted = record(probe, 80)
        server.command(f"setblock {x} {y} {z} minecraft:air")
        report = {"entity": entity, "at": [x, y, z],
                  "health_in_fire": health_series(burning),
                  "flags_in_fire": own_flags(burning, entity),
                  "health_after": health_series(after),
                  "flags_after": own_flags(after, entity),
                  "health_resisted": health_series(resisted),
                  "flags_resisted": own_flags(resisted, entity)}
        hp = report["health_in_fire"]
        if len(hp) < 3 or hp[-1][1] >= hp[0][1]:
            failures.append(f"health did not fall in the fire: {hp}")
        if not any(f & 0x01 for _, f in report["flags_in_fire"]):
            failures.append("the burning bit 0x01 never reached the client")
        falls_after = [h for h in report["health_after"]]
        if len(falls_after) < 2:
            failures.append(f"no on_fire damage after leaving the fire: {falls_after}")
        if not any((f & 0x01) == 0 for _, f in report["flags_after"]):
            failures.append("the burning bit never cleared")
        drops = [h for _, h in report["health_resisted"]]
        if len(drops) > 1 and min(drops) < max(drops):
            failures.append(f"fire resistance let damage through: {drops}")
    finally:
        server.stop()
    print(json.dumps(report, indent=1))
    return report_failures(failures, "the player burnt, and fire resistance held")


# ── zombie ──────────────────────────────────────────────────────────────────

def check_zombie() -> int:
    server = Server("zombie", 3000, survival=False)
    failures: list[str] = []
    report: dict = {}
    try:
        probe, _ = join("ovzombie")
        server.command("time set noon", "weather clear", "gamerule doMobSpawning false")
        probe.pump(1.0)
        probe.drain()
        server.command("summon minecraft:zombie 6 -60 6")
        timeline = record(probe, 400)
        zombie = None
        for t, pid, p in timeline:
            if pid == CB_SPAWN_ENTITY:
                eid, i = read_varint(p, 0)
                kind, _ = read_varint(p, i + 16)
                if kind == ZOMBIE_TYPE:
                    zombie = (eid, t)
        if zombie is None:
            failures.append("no zombie was spawned")
        else:
            eid, t0 = zombie
            lit = [t for t, f in own_flags(timeline, eid) if f & 0x01]
            hurts = [t for t, pid, p in timeline
                     if pid == CB_DAMAGE_EVENT and read_varint(p, 0)[0] == eid]
            report = {"zombie": eid, "lit_after_ticks": round(lit[0] - t0) if lit else None,
                      "damage_events": len(hurts)}
            if not lit:
                failures.append("the zombie never caught fire at noon")
            if not hurts:
                failures.append("the burning zombie was never hurt")
    finally:
        server.stop()
    print(json.dumps(report, indent=1))
    return report_failures(failures, "the zombie burnt at noon")


def main() -> int:
    global BINARY
    args = [a for a in sys.argv[1:]]
    if len(args) > 1:
        BINARY = Path(args[1])
    RUN.mkdir(parents=True, exist_ok=True)
    which = args[0] if args else "all"
    checks = {"forest": check_forest, "player": check_player, "zombie": check_zombie}
    if which == "all":
        return max(check() for check in checks.values())
    if which not in checks:
        print(__doc__)
        return 2
    return checks[which]()


if __name__ == "__main__":
    raise SystemExit(main())
