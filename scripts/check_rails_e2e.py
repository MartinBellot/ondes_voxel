#!/usr/bin/env python3
"""Drive our own server with a probe client: lay rails, ride a cart, press a detector.

The unit tests hold the rules against the numbers the real server gave
(scripts/measure_rails.py). This proves our server carries them out on a
socket, the way a client sees them:

  1. a line of rails laid on the superflat grass comes back east-west, and a
     rail set beside the line's end bends it into a curve;
  2. a detector rail in the line and a redstone lamp beside it;
  3. a minecart item used on a rail spawns a cart (type 64, measured);
  4. a right-click rides it: Set Passengers 0x59 names the probe;
  5. Player Input forward, facing east, rolls it along the line: its position
     packets move east;
  6. the detector rail turns powered under it and the lamp lights;
  7. Player Input 0x02 gets off: Set Passengers with nobody;
  8. the server is stopped (it saves) and started again: the cart comes back
     from entities/, where it was left;
  9. a hit from a creative player takes it away: Remove Entities.

Usage: python3 scripts/check_rails_e2e.py [path/to/ov_dedicated]
Starts ov_dedicated on port 25625 in run/rails-e2e/, which it deletes after.
"""
from __future__ import annotations

import json
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_entity_packets import Probe, read_varint, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / "run" / "rails-e2e"
PORT = 25625
BINARY = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "macos-debug" / "bin" / "ov_dedicated"
GENERATED = ROOT / "data" / "vanilla" / "1.20.1" / "generated" / "reports"

with open(GENERATED / "registries.json") as f:
    REGISTRIES = json.load(f)
ITEMS = {name: entry["protocol_id"]
         for name, entry in REGISTRIES["minecraft:item"]["entries"].items()}
ENTITY_TYPES = {name: entry["protocol_id"]
                for name, entry in REGISTRIES["minecraft:entity_type"]["entries"].items()}
with open(GENERATED / "blocks.json") as f:
    BLOCKS = json.load(f)
STATE_NAMES: dict[int, str] = {}
for block_name, block in BLOCKS.items():
    for state in block["states"]:
        props = state.get("properties", {})
        text = block_name + ("[" + ",".join(f"{k}={props[k]}" for k in sorted(props)) + "]"
                             if props else "")
        STATE_NAMES[state["id"]] = text

SURFACE = -61  # the superflat grass
Y = SURFACE + 1
Z = 4


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
    def __init__(self, probe: Probe) -> None:
        self.probe = probe
        self.sequence = 0

    def hold(self, name: str) -> None:
        self.probe.send(0x2B, struct.pack(">h", 36) + bytes([1]) + varint(ITEMS[name]) + bytes([1, 0]))
        self.probe.send(0x28, struct.pack(">h", 0))

    def use_on(self, x: int, y: int, z: int, face: int = 1) -> None:
        self.sequence += 1
        self.probe.send(0x31, varint(0) + packed(x, y, z) + varint(face) +
                        struct.pack(">fff", 0.5, 1.0, 0.5) + bytes([0]) + varint(self.sequence))

    def face(self, yaw: float) -> None:
        self.probe.send(0x16, struct.pack(">ff", yaw, 0.0) + bytes([1]))

    def interact(self, entity: int) -> None:
        self.probe.send(0x10, varint(entity) + varint(0) + varint(0) + bytes([0]))

    def attack(self, entity: int) -> None:
        self.probe.send(0x10, varint(entity) + varint(1) + bytes([0]))

    def input(self, forward: float, flags: int = 0) -> None:
        self.probe.send(0x1F, struct.pack(">ff", 0.0, forward) + bytes([flags]))


def start_server(world: Path) -> subprocess.Popen:
    if not BINARY.exists():
        raise SystemExit(f"{BINARY} not found; build ov_dedicated first")
    log = open(RUN / "server.log", "a")
    process = subprocess.Popen([str(BINARY), f"--port={PORT}", f"--world={world}",
                                "--log-level=debug"],
                               cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    time.sleep(3.0)
    return process


def stop_server(process: subprocess.Popen) -> None:
    process.terminate()
    try:
        process.wait(timeout=60)
    except subprocess.TimeoutExpired:
        process.kill()


def connect() -> Probe:
    last = None
    for _ in range(30):
        try:
            return Probe(PORT)
        except OSError as error:  # the spawn chunks take a while in Debug (pitfall 29)
            last = error
            time.sleep(2.0)
    raise SystemExit(f"could not connect: {last}")


def block_updates(packets: list) -> dict[tuple[int, int, int], str]:
    out: dict[tuple[int, int, int], str] = {}
    for pid, payload in packets:
        if pid == 0x0A:
            pos, i = unpacked(payload, 0)
            state, _ = read_varint(payload, i)
            out[pos] = STATE_NAMES.get(state, str(state))
    return out


def spawns(packets: list, type_id: int) -> list[dict]:
    out = []
    for pid, payload in packets:
        if pid != 0x01:
            continue
        entity_id, i = read_varint(payload, 0)
        i += 16
        kind, i = read_varint(payload, i)
        if kind != type_id:
            continue
        x, y, z = struct.unpack_from(">ddd", payload, i)
        out.append({"id": entity_id, "pos": (x, y, z)})
    return out


def moves(packets: list, entity: int) -> float:
    """Sum of the x deltas of the entity's position packets, in blocks."""
    total = 0.0
    prefix = varint(entity)
    for pid, payload in packets:
        if pid in (0x2B, 0x2C) and payload.startswith(prefix):
            dx = struct.unpack_from(">h", payload, len(prefix))[0]
            total += dx / 4096.0
        elif pid == 0x68 and payload.startswith(prefix):
            pass
    return total


def main() -> int:
    if RUN.exists():
        shutil.rmtree(RUN)
    RUN.mkdir(parents=True)
    world = RUN / "world"
    failures: list[str] = []
    report: dict = {}

    server = start_server(world)
    try:
        probe = connect()
        hand = Hand(probe)
        probe.pump(4.0)
        probe.drain()

        # 1-2. The line, x = 0..14 on the grass; a detector at x = 8 and a lamp
        # beside it; then a rail beside the east end to bend it.
        hand.hold("minecraft:rail")
        for x in range(0, 15):
            if x == 8:
                continue
            hand.use_on(x, SURFACE, Z)
            probe.pump(0.15)
        hand.hold("minecraft:detector_rail")
        hand.use_on(8, SURFACE, Z)
        probe.pump(0.3)
        hand.hold("minecraft:redstone_lamp")
        hand.use_on(8, SURFACE, Z + 1)
        probe.pump(0.3)
        hand.hold("minecraft:rail")
        hand.use_on(14, SURFACE, Z + 1)
        probe.pump(1.0)
        seen = block_updates(probe.drain())
        line = [seen.get((x, Y, Z), "") for x in range(0, 15)]
        report["line"] = line
        straight = sum(1 for s in line[1:13] if "shape=east_west" in s)
        report["east_west"] = straight
        if straight < 11:
            failures.append(f"only {straight}/12 middle rails came back east_west: {line}")
        if "shape=north_west" not in line[14] and "shape=south_west" not in line[14]:
            failures.append(f"the end rail did not bend towards the new one: {line[14]}")

        # 3. A cart on the rail at x = 2.
        hand.hold("minecraft:minecart")
        hand.use_on(2, Y, Z)
        probe.pump(1.0)
        packets = probe.drain()
        carts = spawns(packets, ENTITY_TYPES["minecraft:minecart"])
        report["spawned"] = carts
        if not carts:
            failures.append("no minecart spawned")
            return report_and_exit(failures, report)
        cart = carts[0]["id"]

        # 4. Ride.
        hand.hold("minecraft:stick")
        hand.interact(cart)
        probe.pump(1.0)
        packets = probe.drain()
        riders = [p for pid, p in packets if pid == 0x59]
        report["set_passengers"] = [p.hex() for p in riders]
        if not riders or not riders[0].startswith(varint(cart) + varint(1)):
            failures.append(f"no Set Passengers naming the probe: {report['set_passengers']}")

        # 5-6. Face east and push for four seconds.
        hand.face(-90.0)
        moved = 0.0
        # Every state each cell went through: the detector switches off again
        # once the cart has passed, and the lamp after it, so the last update
        # alone says nothing (the first run of this check read exactly that).
        history: dict = {}
        deadline = time.monotonic() + 6.0
        while time.monotonic() < deadline:
            hand.input(0.98)
            probe.pump(0.05)
            batch = probe.drain()
            moved += moves(batch, cart)
            for pos, state in block_updates(batch).items():
                history.setdefault(pos, []).append(state)
        report["moved_east"] = moved
        if moved < 3.0:
            failures.append(f"the cart moved only {moved:.3f} blocks east")
        detector = history.get((8, Y, Z), [])
        lamp = history.get((8, Y, Z + 1), [])
        report["detector"] = detector
        report["lamp"] = lamp
        if not any("powered=true" in s for s in detector):
            failures.append(f"the detector rail was never powered: {detector!r}")
        if not any("lit=true" in s for s in lamp):
            failures.append(f"the lamp never lit: {lamp!r}")

        # 7. Off.
        hand.input(0.0, 0x02)
        probe.pump(1.0)
        packets = probe.drain()
        off = [p for pid, p in packets if pid == 0x59 and p == varint(cart) + varint(0)]
        report["dismount"] = len(off)
        if not off:
            failures.append("no Set Passengers emptying the cart")
        probe.socket.close()
    finally:
        stop_server(server)

    # 8. Saved and read back.
    entities = list((world / "entities").glob("r.*.mca"))
    report["entity_regions"] = [p.name for p in entities]
    if not entities:
        failures.append("no entities/ region written")
    if os.environ.get("OV_RAILS_KEEP"):
        # For `measure_rails.py readback`: vanilla reads the world as our
        # server saved it, with the cart still in it — taken before step 9
        # removes it, and outside RUN so the cleanup does not delete it.
        kept = ROOT / "run" / "rails-e2e-world"
        shutil.rmtree(kept, ignore_errors=True)
        shutil.copytree(world, kept)
        print(f"kept the saved world in {kept}")
    server = start_server(world)
    try:
        probe = connect()
        hand = Hand(probe)
        probe.pump(5.0)
        packets = probe.drain()
        carts = spawns(packets, ENTITY_TYPES["minecraft:minecart"])
        report["after_restart"] = carts
        if not carts:
            failures.append("the cart did not come back after a restart")
        else:
            # 9. A creative hit takes it away.
            hand.attack(carts[0]["id"])
            probe.pump(1.0)
            gone = [p for pid, p in probe.drain() if pid == 0x3E]
            report["removed"] = len(gone)
            if not gone:
                failures.append("a creative hit did not remove the cart")
        probe.socket.close()
    finally:
        stop_server(server)
    return report_and_exit(failures, report)


def report_and_exit(failures: list[str], report: dict) -> int:
    print(json.dumps(report, indent=1, default=str))
    shutil.rmtree(RUN, ignore_errors=True)
    if failures:
        for failure in failures:
            print("FAIL:", failure)
        return 1
    print("rails e2e: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
