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
REGISTRIES_REPORT = DATA / "generated" / "reports" / "registries.json"

HEADER_FORMAT = "<4sIIIIIIIIIIIIIIIIIIII"
HEADER_SIZE = 128


def read_pack(path: Path) -> dict:
    """Decode the pack from the format description, not from the emitter."""
    blob = path.read_bytes()
    fields = struct.unpack_from(HEADER_FORMAT, blob, 0)
    (magic, version, block_count, state_count, property_count, value_count,
     string_bytes, strings_at, blocks_at, props_at, values_at, states_at,
     registry_count, entry_count, registries_at, entries_at,
     tag_count, member_count, tags_at, members_at, _) = fields

    if magic != b"OVPK":
        sys.exit("error: not an .ovpack")

    strings = blob[strings_at:strings_at + string_bytes]

    def text(offset: int) -> str:
        end = strings.index(b"\0", offset)
        return strings[offset:end].decode("utf-8")

    # Every registry whose ids the vanilla client hard-codes.
    entry_offsets = struct.unpack_from(f"<{entry_count}I", blob, entries_at)
    registries = {}
    for i in range(registry_count):
        name_at, entry_first, entries_len, first_id = struct.unpack_from(
            "<IIII", blob, registries_at + i * 16)
        registries[text(name_at)] = {
            "entries": [text(entry_offsets[entry_first + j]) for j in range(entries_len)],
            "first_id": first_id,
        }

    # Tags, flattened at build time. Members are ids, not names.
    members = struct.unpack_from(f"<{member_count}i", blob, members_at)
    registry_names = sorted(registries)
    tags = {}
    for i in range(tag_count):
        name_at, registry_index, member_first, members_len = struct.unpack_from(
            "<IIII", blob, tags_at + i * 16)
        tags.setdefault(registry_names[registry_index], {})[text(name_at)] = list(
            members[member_first:member_first + members_len])

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
        "registries": registries,
        "tags": tags,
    }


def main() -> int:
    for path in (PACK, REPORT, REGISTRIES_REPORT):
        if not path.is_file():
            sys.exit(f"error: {path.relative_to(ROOT)} not found.\n"
                     f"  Run tools/ov_datagen/datagen.py, then tools/ov_datagen/ovpack.py")

    pack = read_pack(PACK)
    report = json.loads(REPORT.read_text())

    # ── Every hard-coded registry, entry by entry, order included ───────────
    #
    # This is risk R1 from the plan. The vanilla client knows these ids before
    # it connects and is never told them, so a single entry out of order means
    # it renders the wrong entity and reports nothing.
    #
    # The six registries the server actually sends are listed as excluded, but
    # measured today the exclusion never fires: registries.json contains
    # exactly the 66 hard-coded registries and none of the dynamic ones, which
    # live in the datapack instead. The list stays as a guard — if a future
    # report starts including them, pinning their ids would break the first
    # datapack that adds a biome, and that must fail here rather than in play.
    DYNAMIC = {
        "minecraft:dimension_type", "minecraft:worldgen/biome", "minecraft:chat_type",
        "minecraft:damage_type", "minecraft:trim_material", "minecraft:trim_pattern",
    }
    registries_report = json.loads(REGISTRIES_REPORT.read_text())
    registry_errors: list[str] = []
    ids_checked = 0

    for name, entry in registries_report.items():
        if name in DYNAMIC:
            if name in pack["registries"]:
                registry_errors.append(f"{name}: dynamic registry must not be pinned")
            continue

        ours = pack["registries"].get(name)
        if ours is None:
            registry_errors.append(f"{name}: missing from the pack")
            continue

        # Mojang gives {name: {protocol_id: n}}; the id is the position, so
        # sorting by it reconstructs the order that defines them.
        expected = [k for k, _ in sorted(entry["entries"].items(),
                                         key=lambda kv: kv[1]["protocol_id"])]
        first_id = min(v["protocol_id"] for v in entry["entries"].values())

        if first_id != ours["first_id"]:
            registry_errors.append(
                f"{name}: first id {ours['first_id']} in the pack, {first_id} in the report")

        if len(expected) != len(ours["entries"]):
            registry_errors.append(
                f"{name}: {len(ours['entries'])} entries in the pack, {len(expected)} in the report")
            continue

        for index, (mine, theirs) in enumerate(zip(ours["entries"], expected)):
            ids_checked += 1
            if mine != theirs:
                registry_errors.append(
                    f"{name}[{first_id + index}]: pack says {mine}, report says {theirs}")

    missing = set(pack["registries"]) - set(registries_report)
    for name in sorted(missing):
        registry_errors.append(f"{name}: in the pack but not in the report")

    if registry_errors:
        print("\033[0;31mregistry id mismatches\033[0m")
        for line in registry_errors[:20]:
            print(f"  {line}")
        if len(registry_errors) > 20:
            print(f"  ... and {len(registry_errors) - 20} more")
        return 1

    print(f"\033[0;32m▸\033[0m {len(pack['registries'])} registries, "
          f"{ids_checked} ids identical to Mojang's report")

    # ── Tags, resolved again from the raw files ─────────────────────────────
    #
    # Deliberately a second implementation of the '#' resolution, not a call
    # into the emitter's. An emitter and a reader that share a resolver agree
    # with each other whatever it does; only an independent walk of the same
    # source files can say the flattening is right.
    tag_root = DATA / "generated" / "data"
    directory_to_registry = {
        "blocks": "minecraft:block", "items": "minecraft:item",
        "entity_types": "minecraft:entity_type", "fluids": "minecraft:fluid",
        "game_events": "minecraft:game_event",
    }
    dynamic = {"damage_type", "worldgen/biome", "worldgen/structure",
               "worldgen/world_preset", "worldgen/flat_level_generator_preset"}

    sources: dict[str, dict[str, list]] = {}
    files_seen = 0
    for path in sorted(tag_root.rglob("tags/**/*.json")):
        rel = path.relative_to(tag_root)
        parts = rel.parts[2:-1] + (rel.parts[-1][:-len(".json")],)
        files_seen += 1
        for cut in range(len(parts) - 1, 0, -1):
            candidate = "/".join(parts[:cut])
            registry = directory_to_registry.get(candidate, f"minecraft:{candidate}")
            if candidate in dynamic or registry in pack["registries"]:
                if candidate not in dynamic:
                    sources.setdefault(registry, {})[
                        f"{rel.parts[0]}:" + "/".join(parts[cut:])] = json.loads(
                            path.read_text())["values"]
                break

    tag_errors: list[str] = []
    members_checked = 0

    for registry, group in sources.items():
        entries = pack["registries"][registry]["entries"]
        first_id = pack["registries"][registry]["first_id"]
        position = {name: first_id + i for i, name in enumerate(entries)}

        def flatten(tag, seen):
            if tag in seen:
                sys.exit(f"error: tag cycle at {tag}")
            out = set()
            for value in group[tag]:
                required = True
                if isinstance(value, dict):
                    required, value = value.get("required", True), value["id"]
                if value.startswith("#"):
                    out |= flatten(value[1:], seen | {tag})
                elif value in position:
                    out.add(position[value])
                elif required:
                    tag_errors.append(f"{tag}: requires missing {value}")
            return out

        for tag in sorted(group):
            expected = sorted(flatten(tag, frozenset()))
            actual = pack["tags"].get(registry, {}).get(tag)
            if actual is None:
                tag_errors.append(f"{tag}: missing from the pack ({registry})")
                continue
            members_checked += len(expected)
            if actual != expected:
                tag_errors.append(
                    f"{tag}: {len(actual)} members in the pack, {len(expected)} resolved")

    packed_tags = sum(len(g) for g in pack["tags"].values())
    source_tags = sum(len(g) for g in sources.values())
    if packed_tags != source_tags:
        tag_errors.append(f"{packed_tags} tags in the pack, {source_tags} in the source files")

    if tag_errors:
        print("\033[0;31mtag mismatches\033[0m")
        for line in tag_errors[:20]:
            print(f"  {line}")
        return 1

    print(f"\033[0;32m▸\033[0m {packed_tags} tags re-resolved independently, "
          f"{members_checked} members identical "
          f"({files_seen - source_tags} files skipped: dynamic registries)")

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
