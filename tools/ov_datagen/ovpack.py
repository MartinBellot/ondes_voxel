#!/usr/bin/env python3
"""Emit the binary registry cache the C++ side reads.

Why a binary format rather than reading the JSON directly: the plan's risk R5.
Parsing 24135 block states from JSON at every launch costs seconds in a release
build and tens of seconds in a debug one, and a forty-second start-up makes a
project unusable within a few months. This produces a single blob that is
mmap-able, position-independent and free of pointers, so loading it is a mmap
and a header check.

It also means ov_registry needs no JSON parser yet. simdjson arrives when
datapacks do, and by then the shape of what it has to read will be known.

Layout, all little-endian, every section 8-byte aligned:

    header      magic "OVPK", format version, counts, section offsets
    strings     NUL-separated; every name is an offset into this blob
    blocks      one record per block, in registry order
    properties  one record per property, grouped by block
    values      one string offset per property value
    state_index one block index per block state, for O(1) reverse lookup

Determinism matters as much as compactness: the same input has to produce the
same bytes, or the manifest that proves a regenerated dataset is unchanged
proves nothing.
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
TARGET_VERSION = "1.20.1"

NORMALIZED = ROOT / "data" / "vanilla" / TARGET_VERSION / "normalized"
OUTPUT = ROOT / "data" / "vanilla" / TARGET_VERSION / "registry.ovpack"

MAGIC = b"OVPK"

# Bumped by hand whenever the layout changes, so a stale cache is detected
# rather than misread. A mismatched cache read as if it were current is far
# worse than no cache at all.
FORMAT_VERSION = 1

HEADER_SIZE = 64


class StringTable:
    """Interned strings, emitted in insertion order for reproducibility."""

    def __init__(self) -> None:
        self._offsets: dict[str, int] = {}
        self._blob = bytearray()

    def intern(self, text: str) -> int:
        if text in self._offsets:
            return self._offsets[text]
        offset = len(self._blob)
        self._offsets[text] = offset
        self._blob += text.encode("utf-8") + b"\0"
        return offset

    def blob(self) -> bytes:
        return bytes(self._blob)


def align8(data: bytearray) -> None:
    while len(data) % 8 != 0:
        data.append(0)


def build(blocks_doc: dict) -> bytes:
    blocks = blocks_doc["blocks"]
    state_count = blocks_doc["state_count"]

    strings = StringTable()

    # Interning in a fixed traversal order is what makes the output byte-stable
    # across runs and across Python versions.
    block_records: list[tuple[int, int, int, int, int, int]] = []
    prop_records: list[tuple[int, int, int, int]] = []
    value_offsets: list[int] = []
    state_to_block = [0] * state_count

    for block_index, block in enumerate(blocks):
        name_offset = strings.intern(block["name"])
        prop_first = len(prop_records)

        for prop in block["properties"]:
            prop_name = strings.intern(prop["name"])
            value_first = len(value_offsets)
            for value in prop["values"]:
                value_offsets.append(strings.intern(value))
            prop_records.append(
                (prop_name, value_first, len(prop["values"]), prop["stride"])
            )

        base = block["base_state"]
        count = block["state_count"]
        for state in range(base, base + count):
            state_to_block[state] = block_index

        block_records.append(
            (name_offset, base, count, block["default_state"], prop_first,
             len(block["properties"]))
        )

    string_blob = strings.blob()

    # ── Assemble ────────────────────────────────────────────────────────────
    body = bytearray()

    strings_offset = HEADER_SIZE
    body += string_blob
    align8(body)

    blocks_offset = HEADER_SIZE + len(body)
    for name_offset, base, count, default, prop_first, prop_count in block_records:
        # u32 name, u16 base, u16 count, u16 default, u16 prop_first,
        # u16 prop_count, u16 padding — 16 bytes, naturally aligned.
        body += struct.pack("<IHHHHHH", name_offset, base, count, default, prop_first,
                            prop_count, 0)
    align8(body)

    props_offset = HEADER_SIZE + len(body)
    for name_offset, value_first, value_count, stride in prop_records:
        # u32 name, u32 value_first, u16 value_count, u16 stride — 12 bytes.
        body += struct.pack("<IIHH", name_offset, value_first, value_count, stride)
    align8(body)

    values_offset = HEADER_SIZE + len(body)
    for offset in value_offsets:
        body += struct.pack("<I", offset)
    align8(body)

    states_offset = HEADER_SIZE + len(body)
    for block_index in state_to_block:
        body += struct.pack("<H", block_index)
    align8(body)

    header = struct.pack(
        "<4sIIIIIIIIIIII",
        MAGIC,
        FORMAT_VERSION,
        len(block_records),
        state_count,
        len(prop_records),
        len(value_offsets),
        len(string_blob),
        strings_offset,
        blocks_offset,
        props_offset,
        values_offset,
        states_offset,
        0,  # reserved
    )
    assert len(header) <= HEADER_SIZE
    header += b"\0" * (HEADER_SIZE - len(header))

    return bytes(header) + bytes(body)


def main() -> int:
    blocks_path = NORMALIZED / "blocks.json"
    if not blocks_path.is_file():
        sys.exit(f"error: {blocks_path} not found. Run tools/ov_datagen/datagen.py first.")

    with open(blocks_path) as f:
        blocks_doc = json.load(f)

    payload = build(blocks_doc)
    OUTPUT.write_bytes(payload)

    print(f"\033[0;32m▸\033[0m {OUTPUT.relative_to(ROOT)}")
    print(f"    blocks ......... {blocks_doc['block_count']}")
    print(f"    states ......... {blocks_doc['state_count']}")
    print(f"    size ........... {len(payload):,} bytes")

    # Byte-stability is the property the manifest depends on. Checking it here
    # costs nothing and catches a non-deterministic dict order immediately.
    if build(blocks_doc) != payload:
        sys.exit("error: emitter is not deterministic")
    print("    deterministic .. yes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
