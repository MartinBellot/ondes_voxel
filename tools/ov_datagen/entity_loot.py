#!/usr/bin/env python3
"""Compile les tables de butin des **entités** en tableaux plats.

Les tables d'entité vivent dans le même datapack que celles des blocs et ont le
même format, mais pas le même vocabulaire : là où une table de bloc parle
d'outil, de Silk Touch et de Fortune, une table d'entité parle de qui a tué, de
Looting, du feu et — pour la boule de magma — de la variante de grenouille qui
l'a mangée. Six prédicats, cinq fonctions, quatre genres d'entrée : petit et
fermé, comme celui des blocs, et refusé bruyamment dès qu'on en sort.

Le résultat est écrit dans un fichier **à part**,
`data/vanilla/1.20.1/entity_loot.ovpack`, et non dans `registry.ovpack`. C'est
délibéré : le pack des registres est partagé par tout le dépôt et par toutes les
branches en cours, et y ajouter une section obligerait chacune à le régénérer
au même instant. Un fichier séparé se charge quand on en a besoin et ne casse
personne quand il manque.

Usage : python3 tools/ov_datagen/entity_loot.py [racine] [sortie]
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

MAGIC = b"OVEL"
VERSION = 1

# ── Genres, partagés avec src/ov_gameplay/src/loot.cpp ──────────────────────

COND_KILLED_BY_PLAYER = 0
COND_RANDOM_CHANCE = 1          # value
COND_RANDOM_CHANCE_LOOTING = 2  # value = chance, value2 = multiplicateur
COND_INVERTED = 3               # aux = index de la condition
COND_THIS_ON_FIRE = 4
COND_KILLER_IN_TAG = 5          # aux = offset du nom de tag
COND_SLIME_SIZE_EQUALS = 6      # count = taille
COND_SLIME_SIZE_AT_LEAST = 7    # count = taille
COND_SOURCE_ENTITY_IS = 8       # aux = offset du nom de type
COND_SOURCE_FROG_VARIANT = 9    # aux = offset de la variante
COND_SOURCE_IS_LIGHTNING = 10
COND_UNSUPPORTED = 11

FUNC_SET_COUNT_CONSTANT = 0     # a = valeur
FUNC_SET_COUNT_UNIFORM = 1      # a = min, b = max
FUNC_LOOTING_ENCHANT = 2        # a = min, b = max par niveau, aux = limite (0 = aucune)
FUNC_FURNACE_SMELT = 3
FUNC_SET_POTION = 4             # aux = offset du nom
FUNC_UNSUPPORTED = 5

ENTRY_ITEM = 0                  # item = id protocole
ENTRY_EMPTY = 1
ENTRY_TAG_EXPAND = 2            # aux = offset du nom de tag
ENTRY_LOOT_TABLE = 3            # aux = offset du nom de table
ENTRY_UNSUPPORTED = 4


class Unsupported(Exception):
    pass


class Strings:
    """Pool de chaînes internées. L'offset est l'adresse dans le pool."""

    def __init__(self) -> None:
        self.blob = bytearray()
        self.offsets: dict[str, int] = {}

    def intern(self, text: str) -> int:
        if text in self.offsets:
            return self.offsets[text]
        offset = len(self.blob)
        encoded = text.encode("utf-8")
        self.blob += struct.pack("<H", len(encoded)) + encoded
        self.offsets[text] = offset
        return offset


class EntityLootCompiler:
    def __init__(self, item_index: dict[str, int], strings: Strings) -> None:
        self.item_index = item_index
        self.strings = strings
        self.tables: list[tuple[int, int, int]] = []
        self.pools: list[tuple[float, float, int, int, int, int, int, int]] = []
        self.entries: list[tuple[int, int, int, int, int, int, int, int, int, int]] = []
        self.conds: list[tuple[int, int, float, float, int, int]] = []
        self.funcs: list[tuple[int, float, float, int, int, int]] = []
        self.unsupported: dict[str, int] = {}

    def _note(self, what: str) -> None:
        self.unsupported[what] = self.unsupported.get(what, 0) + 1

    # ── conditions ──────────────────────────────────────────────────────────

    def compile_conditions(self, raw) -> tuple[int, int]:
        """Réserve la place d'abord : une condition peut en contenir une autre,
        et les indices doivent rester stables pendant qu'on descend."""
        raw = raw or []
        first = len(self.conds)
        self.conds.extend([(COND_UNSUPPORTED, 0, 0.0, 0.0, 0, 0)] * len(raw))
        for index, condition in enumerate(raw):
            self.conds[first + index] = self.compile_condition(condition)
        return (first, len(raw))

    def compile_condition(self, raw) -> tuple[int, int, float, float, int, int]:
        kind = raw["condition"]

        if kind == "minecraft:killed_by_player":
            return (COND_KILLED_BY_PLAYER, 0, 0.0, 0.0, 0, 0)

        if kind == "minecraft:random_chance":
            return (COND_RANDOM_CHANCE, 0, float(raw["chance"]), 0.0, 0, 0)

        if kind == "minecraft:random_chance_with_looting":
            return (COND_RANDOM_CHANCE_LOOTING, 0, float(raw["chance"]),
                    float(raw["looting_multiplier"]), 0, 0)

        if kind == "minecraft:inverted":
            inner = self.compile_condition(raw["term"])
            self.conds.append(inner)
            return (COND_INVERTED, 0, 0.0, 0.0, len(self.conds) - 1, 0)

        if kind == "minecraft:entity_properties":
            return self.compile_entity_predicate(raw.get("entity"), raw.get("predicate") or {})

        if kind == "minecraft:damage_source_properties":
            return self.compile_damage_predicate(raw.get("predicate") or {})

        self._note(f"condition {kind}")
        return (COND_UNSUPPORTED, 0, 0.0, 0.0, 0, 0)

    def compile_entity_predicate(self, who, predicate):
        flags = predicate.get("flags") or {}
        if who == "this" and flags.get("is_on_fire") is True and len(predicate) == 1:
            return (COND_THIS_ON_FIRE, 0, 0.0, 0.0, 0, 0)

        if who == "killer" and set(predicate) == {"type"} and predicate["type"].startswith("#"):
            return (COND_KILLER_IN_TAG, 0, 0.0, 0.0,
                    self.strings.intern(predicate["type"][1:]), 0)

        specific = predicate.get("type_specific") or {}
        if who == "this" and specific.get("type") == "slime" and set(predicate) == {"type_specific"}:
            size = specific.get("size")
            if isinstance(size, (int, float)):
                return (COND_SLIME_SIZE_EQUALS, int(size), 0.0, 0.0, 0, 0)
            if isinstance(size, dict) and set(size) == {"min"}:
                return (COND_SLIME_SIZE_AT_LEAST, int(size["min"]), 0.0, 0.0, 0, 0)

        self._note(f"entity_properties {json.dumps(predicate, sort_keys=True)[:80]}")
        return (COND_UNSUPPORTED, 0, 0.0, 0.0, 0, 0)

    def compile_damage_predicate(self, predicate):
        tags = predicate.get("tags") or []
        if len(tags) == 1 and tags[0].get("id") == "minecraft:is_lightning" \
                and tags[0].get("expected") is True:
            return (COND_SOURCE_IS_LIGHTNING, 0, 0.0, 0.0, 0, 0)

        source = predicate.get("source_entity") or {}
        if set(predicate) == {"source_entity"} and "type" in source:
            specific = source.get("type_specific") or {}
            if specific.get("type") == "frog" and "variant" in specific:
                return (COND_SOURCE_FROG_VARIANT, 0, 0.0, 0.0,
                        self.strings.intern(specific["variant"]), 0)
            if set(source) == {"type"}:
                return (COND_SOURCE_ENTITY_IS, 0, 0.0, 0.0,
                        self.strings.intern(source["type"]), 0)

        self._note(f"damage_source_properties {json.dumps(predicate, sort_keys=True)[:80]}")
        return (COND_UNSUPPORTED, 0, 0.0, 0.0, 0, 0)

    # ── fonctions ───────────────────────────────────────────────────────────

    def compile_functions(self, raw) -> tuple[int, int]:
        raw = raw or []
        first = len(self.funcs)
        self.funcs.extend([(FUNC_UNSUPPORTED, 0.0, 0.0, 0, 0, 0)] * len(raw))
        for index, function in enumerate(raw):
            kind, a, b, aux = self.compile_function(function)
            cfirst, ccount = self.compile_conditions(function.get("conditions"))
            self.funcs[first + index] = (kind, a, b, aux, cfirst, ccount)
        return (first, len(raw))

    def compile_function(self, raw) -> tuple[int, float, float, int]:
        kind = raw["function"]

        if kind == "minecraft:set_count":
            if raw.get("add"):
                self._note("set_count add=true")
                return (FUNC_UNSUPPORTED, 0.0, 0.0, 0)
            count = raw["count"]
            if isinstance(count, (int, float)):
                return (FUNC_SET_COUNT_CONSTANT, float(count), 0.0, 0)
            if count.get("type") == "minecraft:uniform":
                return (FUNC_SET_COUNT_UNIFORM, float(count["min"]), float(count["max"]), 0)
            self._note(f"set_count {count.get('type')}")
            return (FUNC_UNSUPPORTED, 0.0, 0.0, 0)

        if kind == "minecraft:looting_enchant":
            count = raw["count"]
            limit = int(raw.get("limit", 0))
            if isinstance(count, (int, float)):
                return (FUNC_LOOTING_ENCHANT, float(count), float(count), limit)
            if count.get("type") == "minecraft:uniform":
                return (FUNC_LOOTING_ENCHANT, float(count["min"]), float(count["max"]), limit)
            self._note(f"looting_enchant {count.get('type')}")
            return (FUNC_UNSUPPORTED, 0.0, 0.0, 0)

        if kind == "minecraft:furnace_smelt":
            return (FUNC_FURNACE_SMELT, 0.0, 0.0, 0)

        if kind == "minecraft:set_potion":
            return (FUNC_SET_POTION, 0.0, 0.0, self.strings.intern(raw["id"]))

        self._note(f"function {kind}")
        return (FUNC_UNSUPPORTED, 0.0, 0.0, 0)

    # ── entrées ─────────────────────────────────────────────────────────────

    def compile_entries(self, raw) -> tuple[int, int]:
        raw = raw or []
        first = len(self.entries)
        self.entries.extend([(ENTRY_UNSUPPORTED, 0, 1, 0, 0, 0, 0, 0, 0, 0)] * len(raw))
        for index, entry in enumerate(raw):
            self.entries[first + index] = self.compile_entry(entry)
        return (first, len(raw))

    def compile_entry(self, raw):
        kind = raw["type"]
        weight = int(raw.get("weight", 1))
        cfirst, ccount = self.compile_conditions(raw.get("conditions"))
        ffirst, fcount = self.compile_functions(raw.get("functions"))
        child_first, child_count = 0, 0
        aux = 0
        item = 0

        if kind == "minecraft:item":
            name = raw["name"]
            if name not in self.item_index:
                raise Unsupported(f"objet inconnu {name}")
            item = self.item_index[name]
            code = ENTRY_ITEM
        elif kind == "minecraft:empty":
            code = ENTRY_EMPTY
        elif kind == "minecraft:tag":
            code = ENTRY_TAG_EXPAND
            aux = self.strings.intern(raw["name"])
        elif kind == "minecraft:loot_table":
            code = ENTRY_LOOT_TABLE
            aux = self.strings.intern(raw["name"])
        elif kind in ("minecraft:alternatives", "minecraft:group", "minecraft:sequence"):
            child_first, child_count = self.compile_entries(raw.get("children"))
            code = ENTRY_UNSUPPORTED
            self._note(f"entry {kind}")
        else:
            code = ENTRY_UNSUPPORTED
            self._note(f"entry {kind}")

        return (code, item, weight, aux, cfirst, ccount, ffirst, fcount,
                child_first, child_count)

    # ── tables ──────────────────────────────────────────────────────────────

    def compile_table(self, name: str, document: dict) -> None:
        pool_first = len(self.pools)
        pools = document.get("pools") or []
        for pool in pools:
            rolls = pool.get("rolls", 1.0)
            if isinstance(rolls, (int, float)):
                rolls_min = rolls_max = float(rolls)
            elif rolls.get("type") == "minecraft:uniform":
                rolls_min, rolls_max = float(rolls["min"]), float(rolls["max"])
            else:
                raise Unsupported(f"rolls {rolls}")
            entry_first, entry_count = self.compile_entries(pool.get("entries"))
            cfirst, ccount = self.compile_conditions(pool.get("conditions"))
            ffirst, fcount = self.compile_functions(pool.get("functions"))
            self.pools.append((rolls_min, rolls_max, entry_first, entry_count,
                               cfirst, ccount, ffirst, fcount))
        self.tables.append((self.strings.intern(name), pool_first, len(pools)))


def pack(compiler: EntityLootCompiler, strings: Strings) -> bytes:
    body = b"".join(struct.pack("<III", *t) for t in compiler.tables)
    body += b"".join(struct.pack("<ffIIIIII", *p) for p in compiler.pools)
    body += b"".join(struct.pack("<IIIIIIIIII", *e) for e in compiler.entries)
    body += b"".join(struct.pack("<IIffII", *c) for c in compiler.conds)
    body += b"".join(struct.pack("<IffIII", *f) for f in compiler.funcs)
    body += bytes(strings.blob)
    header = MAGIC + struct.pack("<IIIIIII", VERSION, len(compiler.tables), len(compiler.pools),
                                 len(compiler.entries), len(compiler.conds),
                                 len(compiler.funcs), len(strings.blob))
    return header + body


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
    base = root / "data" / "vanilla" / "1.20.1"
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else base / "entity_loot.ovpack"

    with open(base / "normalized" / "registries.json") as handle:
        registries = json.load(handle)["registries"]
    item_index = {name: index
                  for index, name in enumerate(registries["minecraft:item"]["entries"])}

    strings = Strings()
    compiler = EntityLootCompiler(item_index, strings)
    directory = base / "generated" / "data" / "minecraft" / "loot_tables" / "entities"
    # `**` et non `*` : les seize tables de laine vivent dans `entities/sheep/`
    # et une entité en a besoin. Le nom compilé est le chemin relatif, donc
    # `minecraft:sheep/white`, ce qui est exactement le nom que le jeu leur
    # donne — et le seul par lequel `/loot` accepte de les tirer.
    paths = sorted(directory.glob("**/*.json"))
    for path in paths:
        with open(path) as handle:
            document = json.load(handle)
        relative = path.relative_to(directory).with_suffix("").as_posix()
        compiler.compile_table(f"minecraft:{relative}", document)
    names = paths

    blob = pack(compiler, strings)
    out.write_bytes(blob)
    print(f"{len(names)} tables, {len(compiler.pools)} pools, {len(compiler.entries)} entrees, "
          f"{len(compiler.conds)} conditions, {len(compiler.funcs)} fonctions "
          f"-> {out} ({len(blob)} octets)")
    if compiler.unsupported:
        print("non compilé :")
        for what, count in sorted(compiler.unsupported.items()):
            print(f"  {count:4d}  {what}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
