"""Biome effects: the colours the client paints the world with.

Biomes are a *dynamic* registry — they are not in registries.json and the
client never hardcodes them, it is told about them in Registry Data. So they
belong in the pack with the rest of the data rather than in a table in the
renderer, and the fields here are exactly the ones vanilla sends.

Two of the four colours are not stored at all in vanilla and are computed from
the biome's climate against a colormap texture in the resource pack. That
computation lives on the client side, where the texture is; what travels is
the temperature and the downfall it needs, plus the overrides for the biomes
that ignore the colormap.
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

# The modifier applied on top of the colormap sample, per biome.
GRASS_MODIFIERS = {None: 0, "dark_forest": 1, "swamp": 2}
TEMPERATURE_MODIFIERS = {None: 0, "frozen": 1}

# The climate is f64 and not f32, and that is a measured requirement rather
# than caution. The colormap is indexed by `(int)((1 - temperature) * 255)`,
# a truncation, and a temperature of 0.6 lands within a millionth of the
# boundary between two columns: in double it gives 102, in float 101. Birch
# forest is the biome where it shows, and its published grass colour agrees
# with the double. Twelve of twelve published colours come back exactly in
# double, eleven of twelve in float.
RECORD = "<ddIIIIIiiBBBx"
assert struct.calcsize(RECORD) == 48


def collect(biome_dir: Path) -> list[dict]:
    """Every biome, sorted by name so the C++ side can binary-search."""
    biomes = []
    for path in sorted(biome_dir.glob("*.json")):
        doc = json.loads(path.read_text())
        effects = doc["effects"]

        modifier = effects.get("grass_color_modifier")
        if modifier not in GRASS_MODIFIERS:
            raise SystemExit(f"error: {path.name} has grass_color_modifier "
                             f"{modifier!r}, which nothing here implements")
        temperature_modifier = doc.get("temperature_modifier")
        if temperature_modifier not in TEMPERATURE_MODIFIERS:
            raise SystemExit(f"error: {path.name} has temperature_modifier "
                             f"{temperature_modifier!r}, which nothing here implements")

        biomes.append({
            "name": f"minecraft:{path.stem}",
            "temperature": float(doc["temperature"]),
            "downfall": float(doc["downfall"]),
            "water_color": int(effects["water_color"]),
            "water_fog_color": int(effects["water_fog_color"]),
            "fog_color": int(effects["fog_color"]),
            "sky_color": int(effects["sky_color"]),
            # -1 rather than 0: black is a colour a biome could legitimately
            # override to, and "no override" has to be distinguishable from it.
            "grass_color": int(effects.get("grass_color", -1)),
            "foliage_color": int(effects.get("foliage_color", -1)),
            "grass_modifier": GRASS_MODIFIERS[modifier],
            "temperature_modifier": TEMPERATURE_MODIFIERS[temperature_modifier],
            "has_precipitation": 1 if doc.get("has_precipitation", True) else 0,
        })
    return biomes


def pack(biomes: list[dict], intern) -> bytes:
    body = b""
    for biome in biomes:
        body += struct.pack(
            RECORD,
            biome["temperature"],
            biome["downfall"],
            intern(biome["name"]),
            biome["water_color"],
            biome["water_fog_color"],
            biome["fog_color"],
            biome["sky_color"],
            biome["grass_color"],
            biome["foliage_color"],
            biome["grass_modifier"],
            biome["temperature_modifier"],
            biome["has_precipitation"],
        )
    return body
