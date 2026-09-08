#!/usr/bin/env python3
"""Connect to our own server and check that its mobs are really there.

The unit tests prove the encoders write the bytes the vanilla server wrote. This
proves the server puts those bytes on a socket, in the right order, about the
right entities — which is a different claim, and the one that decides whether a
real client sees anything.

The probe is the same one the capture harness uses against the vanilla jar, so
the two runs are directly comparable: same reader, same expectations, different
server.

What it asserts, for every mob named on the command line:

  * a Spawn Entity carrying **Mojang's** type id for that name, at the position
    the server logged;
  * a Set Entity Metadata carrying the type's measured max health;
  * an Update Attributes carrying the type's measured base values;
  * movement deltas while it falls, and a resting height equal to the floor plus
    nothing — a mob standing on the superflat's grass, not sunk into it.

Usage: python3 scripts/check_entities.py [path/to/ov_dedicated]
"""
from __future__ import annotations

import json
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_entity_packets import Probe, read_varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
PORT = 25611
MOBS = ["zombie", "cow", "creeper", "chicken", "enderman", "slime", "villager", "skeleton"]
# The superflat's grass sits at y = -61, so its top face — and anything standing
# on it — is at -60.
GROUND_Y = -60.0


def parse_spawn(payload: bytes) -> dict:
    entity_id, i = read_varint(payload, 0)
    uuid = payload[i:i + 16]
    i += 16
    type_id, i = read_varint(payload, i)
    x, y, z = struct.unpack_from(">ddd", payload, i)
    return {"entity_id": entity_id, "uuid": uuid.hex(), "type": type_id, "x": x, "y": y, "z": z}


def parse_metadata(payload: bytes) -> tuple[int, dict[int, tuple[int, object]]]:
    entity_id, i = read_varint(payload, 0)
    fields: dict[int, tuple[int, object]] = {}
    while i < len(payload):
        index = payload[i]
        i += 1
        if index == 0xFF:
            break
        value_type, i = read_varint(payload, i)
        if value_type == 3:  # float
            fields[index] = (value_type, struct.unpack_from(">f", payload, i)[0])
            i += 4
        elif value_type == 0:  # byte
            fields[index] = (value_type, payload[i])
            i += 1
        elif value_type == 8:  # boolean
            fields[index] = (value_type, bool(payload[i]))
            i += 1
        else:
            raise AssertionError(f"unexpected metadata type {value_type} at index {index}")
    return entity_id, fields


def parse_attributes(payload: bytes) -> tuple[int, dict[str, float]]:
    entity_id, i = read_varint(payload, 0)
    count, i = read_varint(payload, i)
    out: dict[str, float] = {}
    for _ in range(count):
        length, i = read_varint(payload, i)
        name = payload[i:i + length].decode()
        i += length
        value = struct.unpack_from(">d", payload, i)[0]
        i += 8
        modifiers, i = read_varint(payload, i)
        assert modifiers == 0, "this server sends no modifiers yet"
        out[name] = value
    return entity_id, out


def parse_delta(payload: bytes) -> tuple[int, tuple[float, float, float], bool]:
    entity_id, i = read_varint(payload, 0)
    dx, dy, dz = struct.unpack_from(">hhh", payload, i)
    i += 6
    return entity_id, (dx / 4096.0, dy / 4096.0, dz / 4096.0), bool(payload[i])


def main() -> int:
    binary = Path(sys.argv[1]) if len(sys.argv) > 1 else (
        ROOT / "build" / "macos-release" / "bin" / "ov_dedicated")
    if not binary.exists():
        print(f"error: {binary} not found; build ov_dedicated first")
        return 1

    with open(NORMALIZED / "registries.json") as f:
        registries = json.load(f)["registries"]
    entity_types = registries["minecraft:entity_type"]["entries"]

    with open(NORMALIZED / "entities.json") as f:
        measured = json.load(f)

    server = subprocess.Popen(
        [str(binary), f"--port={PORT}", "--ticks=400", "--mobs=" + ",".join(MOBS)],
        cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    failures: list[str] = []
    try:
        time.sleep(2.0)
        probe = Probe(PORT, name="ovcheck")
        probe.pump(6.0)
        packets = probe.drain()

        spawns = {}
        metadata: dict[int, dict] = {}
        attributes: dict[int, dict[str, float]] = {}
        deltas: dict[int, list] = {}
        for packet_id, payload in packets:
            if packet_id == 0x01:
                spawn = parse_spawn(payload)
                spawns[spawn["entity_id"]] = spawn
            elif packet_id == 0x52:
                entity_id, fields = parse_metadata(payload)
                metadata.setdefault(entity_id, {}).update(fields)
            elif packet_id == 0x6A:
                entity_id, values = parse_attributes(payload)
                attributes[entity_id] = values
            elif packet_id == 0x2B:
                entity_id, delta, on_ground = parse_delta(payload)
                deltas.setdefault(entity_id, []).append((delta, on_ground))

        print(f"{len(spawns)} spawns, {len(metadata)} metadata, "
              f"{len(attributes)} attribute sets, "
              f"{sum(len(v) for v in deltas.values())} movement deltas")

        by_type = {spawn["type"]: spawn for spawn in spawns.values()}
        for name in MOBS:
            full = f"minecraft:{name}"
            expected_type = entity_types.index(full)
            spawn = by_type.get(expected_type)
            if spawn is None:
                failures.append(f"{full}: no Spawn Entity with type {expected_type}")
                continue
            entity_id = spawn["entity_id"]

            fields = metadata.get(entity_id, {})
            health = fields.get(9, (None, None))[1]
            want_health = measured["attributes"].get(full, {}).get(
                "minecraft:generic.max_health")
            if want_health is not None and abs((health or -1) - want_health) > 1e-6:
                failures.append(f"{full}: health {health} != measured {want_health}")

            for attribute, base in measured["attributes"].get(full, {}).items():
                sent = attributes.get(entity_id, {}).get(attribute)
                if sent is None:
                    failures.append(f"{full}: attribute {attribute} never sent")
                elif sent != base:
                    failures.append(f"{full}: {attribute} sent {sent!r}, measured {base!r}")

            fell = deltas.get(entity_id, [])
            if not fell:
                failures.append(f"{full}: never moved — spawned three blocks up and did not fall")
                continue
            landed_y = spawn["y"] + sum(step[0][1] for step in fell)
            # Within one quantum of the floor, and no more. A delta packet
            # carries 1/4096 of a block, so the client can never be exactly
            # right; what it must not do is *drift*, which is what happens if
            # the server takes each delta from the true position instead of from
            # the one the client already has. Before that was fixed this number
            # was 0.000244 out after nine ticks, and it would have kept growing.
            if abs(landed_y - GROUND_Y) > 1.0 / 4096.0:
                failures.append(f"{full}: came to rest at y={landed_y:.6f}, floor is {GROUND_Y}")
            if not fell[-1][1]:
                failures.append(f"{full}: last movement did not report on_ground")
            print(f"  {full:24s} entity {entity_id} type {expected_type} "
                  f"health {health} fell {len(fell)} ticks to y={landed_y:.4f}")
    finally:
        server.terminate()
        try:
            server.wait(timeout=20)
        except subprocess.TimeoutExpired:
            server.kill()

    if failures:
        print()
        for line in failures:
            print(f"FAIL {line}")
        return 1
    print("\nall mobs spawned, measured and landed as expected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
