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
    biomes      one record per biome, sorted by name; the client's colours
    entities    one record per entity type, in registry order: hitbox, eye
                height and the span of attributes it owns — all measured
    entity_attrs  one (attribute index, base value) pair per owned attribute

Determinism matters as much as compactness: the same input has to produce the
same bytes, or the manifest that proves a regenerated dataset is unchanged
proves nothing.
"""

from __future__ import annotations

import json

import biomes as biomes_module
import collision
import loot
import recipes as recipes_module
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
FORMAT_VERSION = 13

_loot_report = ""
_recipe_report = ""
_fuel_report = ""

# L'ordre des tables de combustion dans le pack. Les trois fours ne lisent pas
# forcément la même durée pour le même objet, et le rapport entre eux est une
# mesure — pas une division supposée. Cet ordre est celui que lit ov_registry.
FUEL_KINDS = ["minecraft:furnace", "minecraft:blast_furnace", "minecraft:smoker"]

# Le pack a dépassé 128 octets d'en-tête en gagnant les tables de butin.
HEADER_SIZE = 256


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
          opacity_doc: dict, stacks_doc: dict, motion_doc: dict,
          hardness_doc: dict, loot_dir, loot_map_doc: dict,
          collision_doc: dict, emission_doc: dict, biome_list: list,
          entities_doc: dict, recipe_dir, fuel_doc: dict,
          remainder_doc: dict) -> bytes:
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
    motion = motion_doc["blocks"]
    hardness = hardness_doc["blocks"]

    def flags_for(block: dict) -> int:
        name = block["name"]
        m = motion.get(name)
        # Bits 0-1 light opacity, then one bit each for the three questions the
        # heightmaps ask. Bit 5 says the block was measured at all: a block that
        # was not must not be silently read as "does not block movement", which
        # is what a zero would mean.
        value = opacity.get(name, 2) & 0b11
        if m is None:
            return value
        return (value
                | (0b000100 if m["motion"] else 0)
                | (0b001000 if m["leaves"] else 0)
                | (0b010000 if m["air"] else 0)
                | 0b100000
                | (0b1000000 if hardness.get(name, {}).get("requires_tool") else 0))

    flag_bytes = bytes(flags_for(block) for block in blocks)

    # Whether a *state* holds a fluid. Per state, not per block, because
    # `waterlogged` is a property: scaffolding[waterlogged=true] raises
    # MOTION_BLOCKING and scaffolding[waterlogged=false] does not — measured,
    # both of them. Six blocks are wet with no such property to set.
    fluid_bits = bytearray((state_count + 7) // 8)
    for block in blocks:
        name = block["name"]
        intrinsic = motion.get(name, {}).get("fluid", False)
        values = None
        for prop in block.get("properties", []):
            if prop["name"] == "waterlogged":
                values, stride = prop["values"], prop["stride"]
                break
        for offset in range(block["state_count"]):
            wet = intrinsic
            if values is not None:
                wet = wet or values[(offset // stride) % len(values)] == "true"
            if wet:
                state = block["base_state"] + offset
                fluid_bits[state >> 3] |= 1 << (state & 7)

    # Maximum stack size per item, in the item registry's own order so the
    # numeric id indexes it directly. Nothing in the reports carries this
    # either. An item that was never measured gets 64, the majority answer;
    # `air` is the only one, and it is never stacked.
    max_stack = stacks_doc["max_stack"]
    item_entries = registries_doc["registries"]["minecraft:item"]["entries"]
    stack_bytes = bytes(min(255, max_stack.get(name, 64)) for name in item_entries)

    # ── Tables de butin ─────────────────────────────────────────────────────
    #
    # Aplaties à la compilation, comme les tags : descendre un arbre de
    # conditions à chaque bloc cassé mettrait un parcours de graphe dans le
    # chemin du joueur.
    item_index = {name: i for i, name in enumerate(item_entries)}
    item_tags = tags_doc["tags"].get("minecraft:item", {})
    loot_sections = loot.compile_block_tables(loot_dir, blocks, item_index,
                                              lambda tag: item_tags[tag], strings.intern,
                                              loot_map_doc["tables"])
    loot_bytes = loot.pack(loot_sections)

    # ── Recettes ────────────────────────────────────────────────────────────
    #
    # Aplaties pour la même raison : un ingrédient qui dit `#minecraft:planks`
    # est développé ici en liste triée d'ids, pour que l'appariement ne fasse
    # jamais qu'une recherche dichotomique par case de grille.
    recipe_sections = recipes_module.compile_recipes(recipe_dir, item_index, item_tags,
                                                     strings.intern)
    recipe_bytes = recipes_module.pack(recipe_sections)

    # ── Combustibles et restes de fabrication ───────────────────────────────
    #
    # Ni les uns ni les autres ne sont dans les données : `getBurnDuration` et
    # `craftingRemainingItem` sont du code Java. Ils viennent de mesures faites
    # sur un vrai serveur 1.20.1 (scripts/measure_fuel.py, scripts/measure_crafting.py),
    # et un objet non mesuré vaut « pas un combustible » / « pas de reste » —
    # ce qui est ici la vérité observée, pas un défaut : les deux mesures
    # couvrent tout le registre des objets.
    burn = fuel_doc["burn_ticks"]
    fuel_bytes = b""
    for kind in FUEL_KINDS:
        if kind not in burn:
            sys.exit(f"error: fuel.json ne couvre pas {kind}")
        table = burn[kind]
        fuel_bytes += b"".join(struct.pack("<H", min(65535, table.get(name, 0)))
                               for name in item_entries)

    remainder_of = dict(fuel_doc.get("fuel_remainder", {}))
    for name, left in remainder_doc.get("crafting_remainder", {}).items():
        if name in remainder_of and remainder_of[name] != left:
            sys.exit(f"error: {name} rend {remainder_of[name]} comme combustible et "
                     f"{left} en fabrication — les deux ne peuvent pas être vrais")
        remainder_of[name] = left
    remainder_bytes = b"".join(
        struct.pack("<i", item_index.get(remainder_of[name], -1) if name in remainder_of else -1)
        for name in item_entries)

    global _recipe_report
    _recipe_report = (f"{len(recipe_sections['records'])} / {recipe_sections['seen']} chargées, "
                      f"{len(recipe_sections['declaration_only'])} déclarées sans appariement, "
                      f"{len(recipe_sections['refused'])} refusées")
    global _fuel_report
    _fuel_report = (f"{sum(1 for name in item_entries if burn[FUEL_KINDS[0]].get(name))} "
                    f"combustibles, {len(remainder_of)} objets à reste")

    global _loot_report
    _loot_report = (f"{loot_sections['present']} blocks, {len(loot_sections['pools'])} pools, "
                    f"{len(loot_sections['entries'])} entries")

    # ── Formes de collision ─────────────────────────────────────────────────
    #
    # En unités de 1/32, ce qui les fait tenir sur un octet par coordonnée. Le
    # masque des faces pleines est calculé ici plutôt qu'à l'exécution : c'est
    # une grille de 32x32 par face, et la question se pose à chaque bloc posé.
    shape_boxes = bytearray()
    shape_records = []
    for boxes in collision_doc["shapes"]:
        first = len(shape_boxes) // 6
        for box in boxes:
            # Signé : une boîte déborde parfois du cube — une tête de piston va
            # de -8/32 à 48/32 — et un octet non signé la replierait en silence.
            shape_boxes += struct.pack("<6b", *box)
        shape_records.append((first, len(boxes), collision.sturdy_bits(boxes)))
    state_shapes = b"".join(struct.pack("<H", index) for index in collision_doc["states"])

    # Un octet par état. Un demi-octet suffirait — les valeurs vont de 0 à 15 —
    # mais la table entière fait 24 ko et rien n'a encore mesuré la différence.
    emission_bytes = bytes(min(15, v) for v in emission_doc["per_state"])

    # Interned before the blob is frozen, not when the section is written: the
    # string table is closed at this line, and a name interned after it lands
    # at an offset past the end of the blob — a lookup that silently returns
    # nothing rather than failing.
    biome_bytes = biomes_module.pack(biome_list, strings.intern)

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

    fluid_offset = HEADER_SIZE + len(body)
    body += bytes(fluid_bits)
    align8(body)

    # Dureté par bloc, en float 32. -1 veut dire incassable, ce que le serveur
    # dit lui-même en ne cassant jamais le bloc — ce n'est pas une valeur par
    # défaut mais une mesure.
    loot_offsets = {}
    for key in ("tables", "pools", "entries", "conds", "funcs", "floats", "ints"):
        loot_offsets[key] = HEADER_SIZE + len(body)
        body += loot_bytes[key]
        align8(body)

    boxes_offset = HEADER_SIZE + len(body)
    body += bytes(shape_boxes)
    align8(body)

    shapes_offset = HEADER_SIZE + len(body)
    for first, count, sturdy in shape_records:
        body += struct.pack("<IHH", first, count, sturdy)
    align8(body)

    state_shapes_offset = HEADER_SIZE + len(body)
    body += state_shapes
    align8(body)

    emission_offset = HEADER_SIZE + len(body)
    body += emission_bytes
    align8(body)

    hardness_offset = HEADER_SIZE + len(body)
    for block in blocks:
        body += struct.pack("<f", float(hardness.get(block["name"], {}).get("hardness", -1.0)))
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

    biomes_offset = HEADER_SIZE + len(body)
    body += biome_bytes
    body += b"\0" * (-len(body) % 8)

    states_offset = HEADER_SIZE + len(body)
    for block_index in state_to_block:
        body += struct.pack("<H", block_index)
    align8(body)

    # ── Entity types ────────────────────────────────────────────────────────
    #
    # Hitbox, eye height and attribute base values, none of which appear in any
    # Mojang report: they are Java code, and they were measured on a running
    # 1.20.1 server instead (scripts/measure_entities.py). A type that could not
    # be measured gets `measured = 0` rather than a zero-sized box, because a
    # mob with no hitbox is a mob that nothing can ever hit.
    entity_entries = registries_doc["registries"]["minecraft:entity_type"]["entries"]
    attribute_entries = registries_doc["registries"]["minecraft:attribute"]["entries"]
    attribute_index = {name: i for i, name in enumerate(attribute_entries)}
    hitboxes = entities_doc["hitbox"]
    eyes = entities_doc["eye_height"]
    attribute_values = entities_doc["attributes"]

    entity_records = []
    entity_attributes = []
    for name in entity_entries:
        box = hitboxes.get(name)
        first = len(entity_attributes)
        for attribute, value in sorted(attribute_values.get(name, {}).items()):
            entity_attributes.append((attribute_index[attribute], value))
        entity_records.append((
            float(box["width"]) if box else 0.0,
            float(box["height"]) if box else 0.0,
            float(eyes[name]) if name in eyes else 0.0,
            first,
            len(entity_attributes) - first,
            # Bit 0: the hitbox was measured. Bit 1: the eye height was.
            (1 if box else 0) | (2 if name in eyes else 0),
        ))

    entities_offset = HEADER_SIZE + len(body)
    for width, height, eye, first, count, measured in entity_records:
        # f32 width, f32 height, f32 eye, u16 attr_first, u8 attr_count,
        # u8 measured — 16 bytes, naturally aligned.
        body += struct.pack("<fffHBB", width, height, eye, first, count, measured)
    align8(body)

    entity_attrs_offset = HEADER_SIZE + len(body)
    for index, value in entity_attributes:
        # u8 attribute index, 7 bytes of padding, f64 base value. The padding is
        # explicit so the double stays naturally aligned on every target.
        body += struct.pack("<B7xd", index, value)
    align8(body)

    recipes_offset = HEADER_SIZE + len(body)
    body += recipe_bytes["recipes"]
    align8(body)

    recipe_ings_offset = HEADER_SIZE + len(body)
    body += recipe_bytes["ingredients"]
    align8(body)

    recipe_choices_offset = HEADER_SIZE + len(body)
    body += recipe_bytes["choices"]
    align8(body)

    fuel_offset = HEADER_SIZE + len(body)
    body += fuel_bytes
    align8(body)

    remainder_offset = HEADER_SIZE + len(body)
    body += remainder_bytes
    align8(body)

    header = struct.pack(
        "<4s" + "I" * 57,
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
        fluid_offset,
        hardness_offset,
        loot_offsets["tables"],
        loot_offsets["pools"],
        loot_offsets["entries"],
        loot_offsets["conds"],
        loot_offsets["funcs"],
        loot_offsets["floats"],
        loot_offsets["ints"],
        len(loot_sections["pools"]),
        len(loot_sections["entries"]),
        len(loot_sections["conds"]),
        len(loot_sections["funcs"]),
        len(loot_sections["floats"]),
        len(loot_sections["ints"]),
        boxes_offset,
        shapes_offset,
        state_shapes_offset,
        len(shape_boxes) // 6,
        len(shape_records),
        emission_offset,
        biomes_offset,
        len(biome_list),
        entities_offset,
        entity_attrs_offset,
        len(entity_records),
        recipes_offset,
        recipe_ings_offset,
        recipe_choices_offset,
        len(recipe_sections["records"]),
        len(recipe_sections["ingredients"]),
        len(recipe_sections["choices"]),
        fuel_offset,
        remainder_offset,
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
    with open(NORMALIZED / "motion.json") as f:
        motion_doc = json.load(f)
    with open(NORMALIZED / "hardness.json") as f:
        hardness_doc = json.load(f)
    with open(NORMALIZED / "loot_tables.json") as f:
        loot_map_doc = json.load(f)
    with open(NORMALIZED / "collision_shapes.json") as f:
        collision_doc = json.load(f)
    with open(NORMALIZED / "light_emission.json") as f:
        emission_doc = json.load(f)
    entities_path = NORMALIZED / "entities.json"
    if not entities_path.is_file():
        sys.exit(f"error: {entities_path} not found. "
                 f"Run scripts/measure_entities.py first.")
    with open(entities_path) as f:
        entities_doc = json.load(f)
    loot_dir = (NORMALIZED.parent / "generated" / "data" / "minecraft" / "loot_tables" / "blocks")
    if not loot_dir.is_dir():
        sys.exit(f"error: {loot_dir} not found. Run tools/ov_datagen/datagen.py first.")

    biome_dir = (NORMALIZED.parent / "generated" / "data" / "minecraft" / "worldgen" / "biome")
    if not biome_dir.is_dir():
        sys.exit(f"error: {biome_dir} not found. Run tools/ov_datagen/datagen.py first.")
    biome_list = biomes_module.collect(biome_dir)

    recipe_dir = NORMALIZED.parent / "generated" / "data" / "minecraft" / "recipes"
    if not recipe_dir.is_dir():
        sys.exit(f"error: {recipe_dir} not found. Run tools/ov_datagen/datagen.py first.")

    fuel_path = NORMALIZED / "fuel.json"
    if not fuel_path.is_file():
        sys.exit(f"error: {fuel_path} not found. Run scripts/measure_fuel.py first.")
    with open(fuel_path) as f:
        fuel_doc = json.load(f)

    remainder_path = NORMALIZED / "crafting.json"
    if not remainder_path.is_file():
        sys.exit(f"error: {remainder_path} not found. "
                 f"Run scripts/measure_crafting.py first.")
    with open(remainder_path) as f:
        remainder_doc = json.load(f)

    payload = build(blocks_doc, registries_doc, tags_doc, opacity_doc, stacks_doc,
                    motion_doc, hardness_doc, loot_dir, loot_map_doc, collision_doc,
                    emission_doc, biome_list, entities_doc, recipe_dir, fuel_doc,
                    remainder_doc)
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
    print(f"    motion flags ... {motion_doc['measured']} blocks measured")
    print(f"    hardness ....... {hardness_doc['count']} blocks")
    print(f"    biomes ......... {len(biome_list)}")
    print(f"    loot tables .... {_loot_report}")
    print(f"    collision ...... {len(collision_doc['shapes'])} shapes, "
          f"{sum(len(s) for s in collision_doc['shapes'])} boxes")
    print(f"    light .......... {emission_doc['covered']} states measured")
    print(f"    entities ....... {entities_doc['present']} of {entities_doc['types']} "
          f"types measured, {sum(len(v) for v in entities_doc['attributes'].values())} "
          f"attribute values")
    print(f"    recipes ........ {_recipe_report}")
    print(f"    fuel ........... {_fuel_report}")
    print(f"    size ........... {len(payload):,} bytes")

    # Byte-stability is the property the manifest depends on. Checking it here
    # costs nothing and catches a non-deterministic dict order immediately.
    if build(blocks_doc, registries_doc, tags_doc, opacity_doc, stacks_doc,
             motion_doc, hardness_doc, loot_dir, loot_map_doc, collision_doc,
             emission_doc, biome_list, entities_doc, recipe_dir, fuel_doc,
             remainder_doc) != payload:
        sys.exit("error: emitter is not deterministic")
    print("    deterministic .. yes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
