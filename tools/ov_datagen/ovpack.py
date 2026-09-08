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
    registries  one record per registry: name, entry range, first id
    entries     one string offset per entry, in the order Mojang lists them
    tags        one record per tag, sorted by (registry, name) for searching
    members     one numeric id per tag member, already flattened and sorted
    blocks      one record per block, in registry order
    flags       one byte per block: measured properties the reports do not carry
    stacks      one byte per item: its maximum stack size, also measured
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
FORMAT_VERSION = 5

HEADER_SIZE = 128


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


def build(blocks_doc: dict, registries_doc: dict, tags_doc: dict,
          opacity_doc: dict, stacks_doc: dict) -> bytes:
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

    # ── Every other registry ────────────────────────────────────────────────
    #
    # These carry the ids the vanilla client hard-codes and never receives, so
    # they are not ours to choose. Order is Mojang's order, and `first_id` is
    # zero everywhere except minecraft:mob_effect.
    registry_records: list[tuple[int, int, int, int]] = []
    entry_offsets: list[int] = []

    for name in sorted(registries_doc["registries"]):
        registry = registries_doc["registries"][name]
        entry_first = len(entry_offsets)
        for entry in registry["entries"]:
            entry_offsets.append(strings.intern(entry))
        registry_records.append(
            (strings.intern(name), entry_first, len(registry["entries"]),
             registry["first_id"])
        )

    # ── Tags, already flattened by the normalizer ───────────────────────────
    #
    # Sorted by (registry index, tag name) so the reader can binary-search, and
    # so the output is byte-stable. Members are ids rather than names: the whole
    # point of resolving at build time is that the game never looks a name up.
    registry_index = {name: i for i, name in enumerate(sorted(registries_doc["registries"]))}

    tag_records: list[tuple[int, int, int, int]] = []
    member_ids: list[int] = []

    flat = []
    for registry_name, group in tags_doc["tags"].items():
        first_id = registries_doc["registries"][registry_name]["first_id"]
        entries = registries_doc["registries"][registry_name]["entries"]
        position = {entry: i for i, entry in enumerate(entries)}
        for tag_name, members in group.items():
            flat.append((registry_index[registry_name], tag_name,
                         [first_id + position[m] for m in members]))

    for index, tag_name, ids in sorted(flat, key=lambda row: (row[0], row[1])):
        member_first = len(member_ids)
        member_ids.extend(ids)
        tag_records.append((strings.intern(tag_name), index, member_first, len(ids)))

    # ── Measured block flags ────────────────────────────────────────────────
    #
    # Nothing in Mojang's reports says whether a block stops light: in vanilla
    # that is Java code. These come from measuring the game — see
    # docs/PROVENANCE.md — and a block that was never measured is left opaque,
    # which errs towards a dark room rather than a world with no shadows.
    opacity = opacity_doc["opacity"]
    flag_bytes = bytes(opacity.get(block["name"], 2) for block in blocks)

    # Maximum stack size per item, in the item registry's own order so the
    # numeric id indexes it directly. Nothing in the reports carries this
    # either. An item that was never measured gets 64, the majority answer;
    # `air` is the only one, and it is never stacked.
    max_stack = stacks_doc["max_stack"]
    item_entries = registries_doc["registries"]["minecraft:item"]["entries"]
    stack_bytes = bytes(min(255, max_stack.get(name, 64)) for name in item_entries)

    string_blob = strings.blob()

    # ── Assemble ────────────────────────────────────────────────────────────
    body = bytearray()

    strings_offset = HEADER_SIZE
    body += string_blob
    align8(body)

    flags_offset = HEADER_SIZE + len(body)
    body += flag_bytes
    align8(body)

    stacks_offset = HEADER_SIZE + len(body)
    body += stack_bytes
    align8(body)

    registries_offset = HEADER_SIZE + len(body)
    for name_offset, entry_first, entry_count, first_id in registry_records:
        # u32 name, u32 entry_first, u32 entry_count, u32 first_id — 16 bytes.
        body += struct.pack("<IIII", name_offset, entry_first, entry_count, first_id)
    align8(body)

    entries_offset = HEADER_SIZE + len(body)
    for offset in entry_offsets:
        body += struct.pack("<I", offset)
    align8(body)

    tags_offset = HEADER_SIZE + len(body)
    for name_offset, index, member_first, member_count in tag_records:
        # u32 name, u32 registry index, u32 member_first, u32 member_count.
        body += struct.pack("<IIII", name_offset, index, member_first, member_count)
    align8(body)

    members_offset = HEADER_SIZE + len(body)
    for member in member_ids:
        body += struct.pack("<i", member)
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
        "<4sIIIIIIIIIIIIIIIIIIIIIII",
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
        len(registry_records),
        len(entry_offsets),
        registries_offset,
        entries_offset,
        len(tag_records),
        len(member_ids),
        tags_offset,
        members_offset,
        flags_offset,
        stacks_offset,
        len(item_entries),
        0,  # reserved
    )
    assert len(header) <= HEADER_SIZE
    header += b"\0" * (HEADER_SIZE - len(header))

    return bytes(header) + bytes(body)


def main() -> int:
    blocks_path = NORMALIZED / "blocks.json"
    if not blocks_path.is_file():
        sys.exit(f"error: {blocks_path} not found. Run tools/ov_datagen/datagen.py first.")

    for required in ("registries.json", "tags.json"):
        if not (NORMALIZED / required).is_file():
            sys.exit(f"error: {NORMALIZED / required} not found. "
                     f"Run tools/ov_datagen/datagen.py first.")

    registries_path = NORMALIZED / "registries.json"
    if not registries_path.is_file():
        sys.exit(f"error: {registries_path} not found. Run tools/ov_datagen/datagen.py first.")

    with open(blocks_path) as f:
        blocks_doc = json.load(f)
    with open(registries_path) as f:
        registries_doc = json.load(f)

    with open(NORMALIZED / "tags.json") as f:
        tags_doc = json.load(f)
    with open(NORMALIZED / "light_opacity.json") as f:
        opacity_doc = json.load(f)
    with open(NORMALIZED / "stack_sizes.json") as f:
        stacks_doc = json.load(f)

    payload = build(blocks_doc, registries_doc, tags_doc, opacity_doc, stacks_doc)
    tag_records_count = [t for g in tags_doc["tags"].values() for t in g]
    member_count_total = sum(len(v) for g in tags_doc["tags"].values() for v in g.values())
    OUTPUT.write_bytes(payload)

    print(f"\033[0;32m▸\033[0m {OUTPUT.relative_to(ROOT)}")
    print(f"    blocks ......... {blocks_doc['block_count']}")
    print(f"    states ......... {blocks_doc['state_count']}")
    print(f"    registries ..... {len(registries_doc['registries'])}")
    print(f"    tags ........... {len(tag_records_count)} ({member_count_total} members)")
    print(f"    light opacity .. {opacity_doc['measured']} blocks measured")
    print(f"    stack sizes .... {stacks_doc['measured']} items measured")
    print(f"    size ........... {len(payload):,} bytes")

    # Byte-stability is the property the manifest depends on. Checking it here
    # costs nothing and catches a non-deterministic dict order immediately.
    if build(blocks_doc, registries_doc, tags_doc, opacity_doc, stacks_doc) != payload:
        sys.exit("error: emitter is not deterministic")
    print("    deterministic .. yes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
