#!/usr/bin/env python3
"""Prove the binary registry says exactly what Mojang's report says.

There are three steps between the official server jar and a block id in a chunk:

    server.jar --reports  ->  blocks.json
    datagen.py            ->  normalized/blocks.json   (property order derived)
    ovpack.py             ->  registry.ovpack          (binary, what C++ reads)

Each step is small and each is tested, but "each step is fine" is not the same
claim as "the end matches the beginning". This reads the binary the way the C++
does — independently, from the format spec, not by calling the emitter — and
compares all 24135 states against Mojang's own report.

The ids matter because the vanilla client hard-codes them and never receives
them. A single one wrong shows up as a client seeing a different block, with
nothing in any log to say so.
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET_VERSION = "1.20.1"
DATA = ROOT / "data" / "vanilla" / TARGET_VERSION

PACK = DATA / "registry.ovpack"
REPORT = DATA / "generated" / "reports" / "blocks.json"

HEADER_FORMAT = "<4sIIIIIIIIIIII"
HEADER_SIZE = 64


def read_pack(path: Path) -> dict:
    """Decode the pack from the format description, not from the emitter."""
    blob = path.read_bytes()
    fields = struct.unpack_from(HEADER_FORMAT, blob, 0)
    (magic, version, block_count, state_count, property_count, value_count,
     string_bytes, strings_at, blocks_at, props_at, values_at, states_at, _) = fields

    if magic != b"OVPK":
        sys.exit("error: not an .ovpack")

    strings = blob[strings_at:strings_at + string_bytes]

    def text(offset: int) -> str:
        end = strings.index(b"\0", offset)
        return strings[offset:end].decode("utf-8")

    value_offsets = struct.unpack_from(f"<{value_count}I", blob, values_at)

    properties = []
    for i in range(property_count):
        name_at, value_first, values_len, stride = struct.unpack_from("<IIHH", blob,
                                                                      props_at + i * 12)
        properties.append({
            "name": text(name_at),
            "values": [text(value_offsets[value_first + k]) for k in range(values_len)],
            "stride": stride,
        })

    blocks = []
    for i in range(block_count):
        (name_at, base, count, default, prop_first,
         prop_count, _pad) = struct.unpack_from("<IHHHHHH", blob, blocks_at + i * 16)
        blocks.append({
            "name": text(name_at),
            "base_state": base,
            "state_count": count,
            "default_state": default,
            "properties": properties[prop_first:prop_first + prop_count],
        })

    state_to_block = struct.unpack_from(f"<{state_count}H", blob, states_at)

    return {
        "version": version,
        "blocks": blocks,
        "state_count": state_count,
        "state_to_block": state_to_block,
        "size": len(blob),
    }


def main() -> int:
    for path in (PACK, REPORT):
        if not path.is_file():
            sys.exit(f"error: {path.relative_to(ROOT)} not found.\n"
                     f"  Run tools/ov_datagen/datagen.py, then tools/ov_datagen/ovpack.py")

    pack = read_pack(PACK)
    report = json.loads(REPORT.read_text())

    by_name = {block["name"]: block for block in pack["blocks"]}
    errors: list[str] = []
    checked = 0

    for name, entry in report.items():
        block = by_name.get(name)
        if block is None:
            errors.append(f"{name}: missing from the pack")
            continue

        if len(entry["states"]) != block["state_count"]:
            errors.append(f"{name}: {block['state_count']} states in the pack, "
                          f"{len(entry['states'])} in the report")
            continue

        default = next(s["id"] for s in entry["states"] if s.get("default"))
        if default != block["default_state"]:
            errors.append(f"{name}: default state {block['default_state']} != {default}")

        # The claim that matters: for every state, the property values Mojang
        # reports must be exactly what the mixed-radix arithmetic yields.
        for state in entry["states"]:
            offset = state["id"] - block["base_state"]
            if not 0 <= offset < block["state_count"]:
                errors.append(f"{name}: state {state['id']} outside the pack's range")
                continue

            for prop in block["properties"]:
                index = (offset // prop["stride"]) % len(prop["values"])
                actual = prop["values"][index]
                expected = state.get("properties", {}).get(prop["name"])
                if expected != actual:
                    errors.append(
                        f"{name} state {state['id']}: {prop['name']} is {actual!r} "
                        f"in the pack, {expected!r} in the report")

            if pack["state_to_block"][state["id"]] != pack["blocks"].index(block):
                errors.append(f"{name}: state {state['id']} maps to the wrong block")

            checked += 1

    print(f"\n  pack ............. {PACK.relative_to(ROOT)} ({pack['size']:,} bytes)")
    print(f"  report ........... {len(report)} blocks")
    print(f"  states checked ... {checked:,}")

    if errors:
        print(f"\n\033[0;31m{len(errors)} mismatches against Mojang's report\033[0m\n")
        for line in errors[:20]:
            print(f"    {line}")
        if len(errors) > 20:
            print(f"    ... and {len(errors) - 20} more")
        print()
        return 1

    print("\n\033[0;32mEvery state matches Mojang's own report\033[0m\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
