#!/usr/bin/env python3
"""Decode the sound packets of a capture written by capture_sound_packets.py.

Identified by content, not by id alone — the layouts below were pinned by the
`layout.*` gestures, whose every field was chosen from the console:

  0x62 Sound Effect   varint sound (registry id + 1, or 0 then an identifier
                      and an optional fixed range), varint category, three
                      i32 positions in eighths of a block, f32 volume,
                      f32 pitch, i64 seed.
  0x61 Entity Sound   the same sound and category, varint entity id, f32
                      volume, f32 pitch, i64 seed (archived layout; see the
                      provenance file for whether a capture confirmed it).
  0x63 Stop Sound     u8 flags (1: source follows, 2: sound follows), then
                      varint source and/or identifier.
  0x25 World Event    i32 event, position (packed long), i32 data, bool global.

Usage: python3 scripts/decode_sound_packets.py [capture.json] [--json out.json]
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
CATEGORIES = ["master", "music", "record", "weather", "block", "hostile", "neutral",
              "player", "ambient", "voice"]


def varint(buf: bytes, i: int) -> tuple[int, int]:
    value, shift = 0, 0
    while True:
        byte = buf[i]
        i += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, i
        shift += 7


def string(buf: bytes, i: int) -> tuple[str, int]:
    length, i = varint(buf, i)
    return buf[i:i + length].decode(), i + length


def sound(buf: bytes, i: int, names: list[str]) -> tuple[str, int]:
    sid, i = varint(buf, i)
    if sid != 0:
        return names[sid - 1], i
    name, i = string(buf, i)
    has_range = buf[i]
    i += 1
    if has_range:
        i += 4
    return name + " (inline)", i


def decode(packet_id: int, payload: bytes, names: list[str]) -> dict | None:
    if packet_id == 0x62:
        name, i = sound(payload, 0, names)
        category, i = varint(payload, i)
        x, y, z = struct.unpack_from(">iii", payload, i)
        volume, pitch = struct.unpack_from(">ff", payload, i + 12)
        return {"kind": "sound", "sound": name, "category": CATEGORIES[category],
                "pos": [x / 8, y / 8, z / 8], "volume": round(volume, 6),
                "pitch": round(pitch, 6)}
    if packet_id == 0x61:
        name, i = sound(payload, 0, names)
        category, i = varint(payload, i)
        entity, i = varint(payload, i)
        volume, pitch = struct.unpack_from(">ff", payload, i)
        return {"kind": "entity_sound", "sound": name, "category": CATEGORIES[category],
                "entity": entity, "volume": round(volume, 6), "pitch": round(pitch, 6)}
    if packet_id == 0x63:
        flags = payload[0]
        i = 1
        out: dict = {"kind": "stop", "flags": flags}
        if flags & 1:
            source, i = varint(payload, i)
            out["source"] = CATEGORIES[source]
        if flags & 2:
            out["sound"], i = string(payload, i)
        return out
    if packet_id == 0x25:
        event = struct.unpack_from(">i", payload, 0)[0]
        packed = struct.unpack_from(">q", payload, 4)[0]

        def signed(value: int, bits: int) -> int:
            # Python integers are unbounded, so a shift pair does not
            # sign-extend the way it would in a 64-bit register.
            return value - (1 << bits) if value >= 1 << (bits - 1) else value

        x = packed >> 38
        y = signed(packed & 0xFFF, 12)
        z = signed((packed >> 12) & 0x3FFFFFF, 26)
        data = struct.unpack_from(">i", payload, 12)[0]
        return {"kind": "world_event", "event": event, "pos": [x, y, z], "data": data,
                "global": bool(payload[16])}
    return None


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    capture = Path(args[0]) if args else NORMALIZED / "sound_packets_vanilla.json"
    names = json.loads((NORMALIZED / "registries.json").read_text())[
        "registries"]["minecraft:sound_event"]["entries"]
    document = json.loads(capture.read_text())
    table: dict = {}
    for label, gesture in document["gestures"].items():
        row = {}
        for who in ("actor", "ear"):
            row[who] = [d for pid, hx in gesture[who]
                        if (d := decode(pid, bytes.fromhex(hx), names)) is not None]
        table[label] = row
        print(f"== {label}")
        for who in ("actor", "ear"):
            for d in row[who]:
                rest = {k: v for k, v in d.items() if k != "kind"}
                print(f"   {who:5s} {d['kind']:12s} {rest}")
    if "--json" in sys.argv:
        out = Path(sys.argv[sys.argv.index("--json") + 1])
        out.write_text(json.dumps(table, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
