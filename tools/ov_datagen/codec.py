#!/usr/bin/env python3
"""Emit the registry codec the client is sent during login.

Six registries in 1.20.1 are *not* hard-coded in the client: dimension_type,
worldgen/biome, chat_type, damage_type, trim_material and trim_pattern. The
server sends them as one NBT compound in the Login (play) packet, and a client
that cannot decode it disconnects before the world appears.

Why this is a build-time tool rather than runtime code: the source is JSON and
the destination is NBT, and **the mapping is not mechanical**. JSON has one
number type; NBT has six, and the client's decoder is strict about which it
gets. `temperature` must be a float and `coordinate_scale` a double; every
boolean is a byte; `fog_color` is an int. Converting by inspecting the JSON
value — "it has no decimal point, emit an int" — produces a codec that is
plausible, decodes to the wrong types, and drops the connection with no useful
error. So every field is declared below, with its type, and anything not
declared is left out.

Leaving a field out is safe: the client's codecs ignore fields they do not know
and default the optional ones. Sending a field with the wrong type is not.

Output is a single NBT blob, written to data/vanilla/<version>/registry_codec.nbt
and gitignored like everything else derived from Mojang's data. The server reads
it and copies it into the packet verbatim — it never needs to understand it.
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
TARGET_VERSION = "1.20.1"
DATA = ROOT / "data" / "vanilla" / TARGET_VERSION
SOURCE = DATA / "generated" / "data" / "minecraft"
OUTPUT = DATA / "registry_codec.nbt"

# ── NBT writing ─────────────────────────────────────────────────────────────

TAG_END, TAG_BYTE, TAG_SHORT, TAG_INT, TAG_LONG = 0, 1, 2, 3, 4
TAG_FLOAT, TAG_DOUBLE, TAG_BYTE_ARRAY, TAG_STRING = 5, 6, 7, 8
TAG_LIST, TAG_COMPOUND, TAG_INT_ARRAY, TAG_LONG_ARRAY = 9, 10, 11, 12


def _name(text: str) -> bytes:
    raw = text.encode("utf-8")
    return struct.pack(">H", len(raw)) + raw


class Node:
    """A typed NBT value. The type is chosen here, never inferred."""

    def __init__(self, tag: int, payload: bytes):
        self.tag = tag
        self.payload = payload


def b(value) -> Node:      # noqa: N802 - byte, and booleans are bytes
    return Node(TAG_BYTE, struct.pack(">b", 1 if value is True else 0 if value is False else int(value)))


def i(value) -> Node:
    return Node(TAG_INT, struct.pack(">i", int(value)))


def l(value) -> Node:      # noqa: E743 - long
    return Node(TAG_LONG, struct.pack(">q", int(value)))


def f(value) -> Node:
    return Node(TAG_FLOAT, struct.pack(">f", float(value)))


def d(value) -> Node:
    return Node(TAG_DOUBLE, struct.pack(">d", float(value)))


def s(value) -> Node:
    return Node(TAG_STRING, _name(str(value)))


def compound(entries: dict[str, Node]) -> Node:
    body = bytearray()
    for key, node in entries.items():
        if node is None:
            continue
        body += bytes([node.tag]) + _name(key) + node.payload
    body.append(TAG_END)
    return Node(TAG_COMPOUND, bytes(body))


def lst(nodes: list[Node]) -> Node:
    # An empty list carries element type TAG_End, which is what vanilla writes
    # and what the client expects; using any other type for an empty list is a
    # decode error rather than an empty result.
    element = nodes[0].tag if nodes else TAG_END
    body = bytes([element]) + struct.pack(">i", len(nodes))
    for node in nodes:
        if node.tag != element:
            sys.exit("error: heterogeneous NBT list")
        body += node.payload
    return Node(TAG_LIST, body)


def document(root: Node) -> bytes:
    """A named root compound with an empty name, as 1.20.1 sends it."""
    return bytes([root.tag]) + _name("") + root.payload


# ── The schema ──────────────────────────────────────────────────────────────
#
# Every field the client reads, and the NBT type it must arrive as. Fields the
# source has and this does not are dropped on purpose.


def optional(source: dict, key: str, convert) -> Node | None:
    return convert(source[key]) if key in source else None


def dimension_type(source: dict) -> Node:
    spawn = source.get("monster_spawn_light_level")
    if isinstance(spawn, dict):
        # A value provider, not a plain number. Both forms are legal and the
        # overworld uses this one.
        spawn_node = compound({
            "type": s(spawn["type"]),
            "value": compound({
                "max_inclusive": i(spawn["value"]["max_inclusive"]),
                "min_inclusive": i(spawn["value"]["min_inclusive"]),
            }),
        })
    else:
        spawn_node = i(spawn if spawn is not None else 0)

    return compound({
        "ambient_light": f(source["ambient_light"]),
        "bed_works": b(source["bed_works"]),
        "coordinate_scale": d(source["coordinate_scale"]),
        "effects": s(source["effects"]),
        "fixed_time": optional(source, "fixed_time", l),
        "has_ceiling": b(source["has_ceiling"]),
        "has_raids": b(source["has_raids"]),
        "has_skylight": b(source["has_skylight"]),
        "height": i(source["height"]),
        "infiniburn": s(source["infiniburn"]),
        "logical_height": i(source["logical_height"]),
        "min_y": i(source["min_y"]),
        "monster_spawn_block_light_limit": i(source["monster_spawn_block_light_limit"]),
        "monster_spawn_light_level": spawn_node,
        "natural": b(source["natural"]),
        "piglin_safe": b(source["piglin_safe"]),
        "respawn_anchor_works": b(source["respawn_anchor_works"]),
        "ultrawarm": b(source["ultrawarm"]),
    })


def sound_event(value) -> Node:
    # A sound is either a plain id or {sound_id, range}. Both appear.
    if isinstance(value, dict):
        return compound({
            "sound_id": s(value["sound_id"]),
            "range": optional(value, "range", f),
        })
    return s(value)


def biome(source: dict) -> Node:
    effects = source["effects"]

    particle = None
    if "particle" in effects:
        options = effects["particle"]["options"]
        particle = compound({
            "options": compound({"type": s(options["type"])}),
            "probability": f(effects["particle"]["probability"]),
        })

    mood = None
    if "mood_sound" in effects:
        mood_source = effects["mood_sound"]
        mood = compound({
            "sound": sound_event(mood_source["sound"]),
            "tick_delay": i(mood_source["tick_delay"]),
            "block_search_extent": i(mood_source["block_search_extent"]),
            "offset": d(mood_source["offset"]),
        })

    additions = None
    if "additions_sound" in effects:
        additions_source = effects["additions_sound"]
        additions = compound({
            "sound": sound_event(additions_source["sound"]),
            "tick_chance": d(additions_source["tick_chance"]),
        })

    music = None
    if "music" in effects:
        music_source = effects["music"]
        music = compound({
            "sound": sound_event(music_source["sound"]),
            "min_delay": i(music_source["min_delay"]),
            "max_delay": i(music_source["max_delay"]),
            "replace_current_music": b(music_source["replace_current_music"]),
        })

    return compound({
        # 1.19.4 replaced the "precipitation" string with this boolean. Sending
        # the old field instead is accepted and then ignored, and it rains
        # nowhere.
        "has_precipitation": b(source["has_precipitation"]),
        "temperature": f(source["temperature"]),
        "temperature_modifier": optional(source, "temperature_modifier", s),
        "downfall": f(source["downfall"]),
        "effects": compound({
            "fog_color": i(effects["fog_color"]),
            "water_color": i(effects["water_color"]),
            "water_fog_color": i(effects["water_fog_color"]),
            "sky_color": i(effects["sky_color"]),
            "foliage_color": optional(effects, "foliage_color", i),
            "grass_color": optional(effects, "grass_color", i),
            "grass_color_modifier": optional(effects, "grass_color_modifier", s),
            "particle": particle,
            "ambient_sound": optional(effects, "ambient_sound", sound_event),
            "mood_sound": mood,
            "additions_sound": additions,
            "music": music,
        }),
    })


def text_component(value) -> Node:
    """A chat component, as the trim registries carry for their descriptions."""
    if isinstance(value, str):
        return s(value)
    return compound({
        "translate": optional(value, "translate", s),
        "text": optional(value, "text", s),
        "color": optional(value, "color", s),
    })


def chat_decoration(source: dict) -> Node:
    return compound({
        "translation_key": s(source["translation_key"]),
        "parameters": lst([s(parameter) for parameter in source["parameters"]]),
        "style": compound({}) if "style" not in source else compound({
            "color": optional(source["style"], "color", s),
            "italic": optional(source["style"], "italic", b),
        }),
    })


def chat_type(source: dict) -> Node:
    return compound({
        "chat": chat_decoration(source["chat"]),
        "narration": chat_decoration(source["narration"]),
    })


def damage_type(source: dict) -> Node:
    return compound({
        "message_id": s(source["message_id"]),
        "scaling": s(source["scaling"]),
        "exhaustion": f(source["exhaustion"]),
        "effects": optional(source, "effects", s),
        "death_message_type": optional(source, "death_message_type", s),
    })


def trim_material(source: dict) -> Node:
    overrides = None
    if "override_armor_materials" in source:
        overrides = compound({
            key: s(value) for key, value in source["override_armor_materials"].items()
        })
    ingredient = source["ingredient"]
    return compound({
        "asset_name": s(source["asset_name"]),
        "ingredient": s(ingredient if isinstance(ingredient, str) else ingredient["item"]),
        "item_model_index": f(source["item_model_index"]),
        "override_armor_materials": overrides,
        "description": text_component(source["description"]),
    })


def trim_pattern(source: dict) -> Node:
    template = source["template_item"]
    return compound({
        "asset_id": s(source["asset_id"]),
        "template_item": s(template if isinstance(template, str) else template["item"]),
        "description": text_component(source["description"]),
        "decal": b(source.get("decal", False)),
    })


REGISTRIES = [
    ("minecraft:dimension_type", "dimension_type", dimension_type),
    ("minecraft:worldgen/biome", "worldgen/biome", biome),
    ("minecraft:chat_type", "chat_type", chat_type),
    ("minecraft:damage_type", "damage_type", damage_type),
    ("minecraft:trim_material", "trim_material", trim_material),
    ("minecraft:trim_pattern", "trim_pattern", trim_pattern),
]


def build() -> tuple[bytes, dict[str, int]]:
    registries: dict[str, Node] = {}
    counts: dict[str, int] = {}

    for registry_name, directory, convert in REGISTRIES:
        folder = SOURCE / directory
        if not folder.is_dir():
            sys.exit(f"error: {folder} not found. Run tools/ov_datagen/datagen.py first.")

        entries = []
        # Sorted, so ids are stable across runs. These ids are ours to choose,
        # but they must not change between the codec and the chunks that use
        # them, and a run-to-run reshuffle would do exactly that.
        for index, path in enumerate(sorted(folder.glob("*.json"))):
            source = json.loads(path.read_text(encoding="utf-8"))
            entries.append(compound({
                "name": s(f"minecraft:{path.stem}"),
                "id": i(index),
                "element": convert(source),
            }))

        registries[registry_name] = compound({
            "type": s(registry_name),
            "value": lst(entries),
        })
        counts[registry_name] = len(entries)

    return document(compound(registries)), counts


def main() -> int:
    payload, counts = build()
    OUTPUT.write_bytes(payload)

    print(f"\033[0;32m▸\033[0m {OUTPUT.relative_to(ROOT)}")
    for name, count in counts.items():
        print(f"    {name:32} {count}")
    print(f"    size ........... {len(payload):,} bytes")

    if build()[0] != payload:
        sys.exit("error: emitter is not deterministic")
    print("    deterministic .. yes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
