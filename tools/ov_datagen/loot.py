"""Compile les tables de butin des blocs en tableaux plats.

Les tables sont, elles, vraiment data-driven en vanilla : elles vivent dans le
datapack et on les régénère localement. Ce qui est du code, c'est
l'**interpréteur** — et c'est lui qu'on écrit, pas un DSL de plus.

Le JSON est aplati ici, à la compilation, pour la même raison que les tags :
résoudre un arbre de conditions à chaque bloc cassé mettrait un parcours de
graphe dans le chemin du joueur. Le C++ ne voit que des tableaux d'entiers.

Le vocabulaire réellement utilisé par les 925 tables de blocs de 1.20.1 est
petit et fermé — quatre prédicats `match_tool`, trois formes de `set_count`,
trois formules d'`apply_bonus`. Tout ce qui n'y est pas est refusé bruyamment
plutôt que compilé en silence : une table à moitié comprise donne des objets
qui n'existent pas.
"""
from __future__ import annotations

import json
import struct
from pathlib import Path

# ── Genres, partagés avec src/ov_gameplay/src/loot.cpp ──────────────────────

COND_ALWAYS = 0        # survives_explosion, hors explosion
COND_SILK_TOUCH = 1
COND_TOOL_IS = 2       # l'outil tenu est l'un de ces objets
COND_STATE = 3         # une propriété d'état vaut une valeur
COND_TABLE_BONUS = 4
COND_INVERTED = 5
COND_ANY_OF = 6
COND_RANDOM_CHANCE = 7
COND_BROKEN_BY_ENTITY = 8
COND_NEIGHBOUR = 9     # le bloc juste au-dessus ou juste en dessous
COND_UNSUPPORTED = 10  # évalué faux, et compté

FUNC_COUNT_CONSTANT = 0
FUNC_COUNT_UNIFORM = 1
FUNC_COUNT_BINOMIAL = 2
FUNC_EXPLOSION_DECAY = 3
FUNC_ORE_DROPS = 4
FUNC_UNIFORM_BONUS = 5
FUNC_BINOMIAL_BONUS = 6
FUNC_LIMIT_COUNT = 7
FUNC_UNSUPPORTED = 8

ENTRY_ITEM = 0
ENTRY_ALTERNATIVES = 1
ENTRY_UNSUPPORTED = 2


class Unsupported(Exception):
    pass


class LootCompiler:
    """Aplatit les tables. Les listes produites sont écrites telles quelles."""

    def __init__(self, item_index: dict[str, int], intern):
        self.item_index = item_index
        self.intern = intern
        self.tables: list[tuple[int, int, int, int]] = []
        self.pools: list[tuple[float, int, int, int, int]] = []
        self.entries: list[tuple[int, int, int, int, int, int, int, int]] = []
        self.conds: list[tuple[int, int, float, int, int]] = []
        self.funcs: list[tuple[int, int, float, float, int, int, int]] = []
        self.floats: list[float] = []
        self.ints: list[int] = []
        self.unsupported: dict[str, int] = {}

    # ── conditions ──────────────────────────────────────────────────────────

    def _note(self, what: str) -> None:
        self.unsupported[what] = self.unsupported.get(what, 0) + 1

    def compile_conditions(self, raw) -> tuple[int, int]:
        """Réserve la place d'abord : une condition peut en contenir d'autres,
        et les indices doivent rester stables pendant qu'on descend."""
        raw = raw or []
        first = len(self.conds)
        self.conds.extend([(COND_UNSUPPORTED, 0, 0.0, 0, 0)] * len(raw))
        for i, c in enumerate(raw):
            self.conds[first + i] = self.compile_condition(c)
        return first, len(raw)

    def compile_condition(self, c: dict) -> tuple[int, int, float, int, int]:
        kind = c.get("condition")
        if kind == "minecraft:survives_explosion":
            return (COND_ALWAYS, 0, 0.0, 0, 0)
        if kind == "minecraft:match_tool":
            return self._match_tool(c.get("predicate", {}))
        if kind == "minecraft:block_state_property":
            props = sorted(c.get("properties", {}).items())
            if len(props) != 1:
                # Aucune table de 1.20.1 n'en demande deux ; en accepter une
                # deuxième en silence testerait la mauvaise.
                raise Unsupported(f"block_state_property à {len(props)} propriétés")
            name, value = props[0]
            return (COND_STATE, 0, 0.0, self.intern(name), self.intern(str(value)))
        if kind == "minecraft:table_bonus":
            first = len(self.floats)
            self.floats.extend(float(x) for x in c["chances"])
            return (COND_TABLE_BONUS, len(c["chances"]), 0.0, first, 0)
        if kind == "minecraft:inverted":
            inner = len(self.conds)
            self.conds.append((COND_UNSUPPORTED, 0, 0.0, 0, 0))
            self.conds[inner] = self.compile_condition(c["term"])
            return (COND_INVERTED, 0, 0.0, inner, 0)
        if kind == "minecraft:any_of":
            first, count = self.compile_conditions(c.get("terms", []))
            return (COND_ANY_OF, count, 0.0, first, 0)
        if kind == "minecraft:random_chance":
            return (COND_RANDOM_CHANCE, 0, float(c["chance"]), 0, 0)
        if kind == "minecraft:location_check":
            return self._location_check(c)
        if kind == "minecraft:entity_properties":
            if c.get("predicate"):
                raise Unsupported("entity_properties avec un prédicat")
            # Prédicat vide : « il y a bien une entité ». Vrai quand un joueur
            # casse le bloc, ce qui est le seul cas qu'on évalue.
            return (COND_BROKEN_BY_ENTITY, 0, 0.0, 0, 0)
        self._note(kind or "?")
        return (COND_UNSUPPORTED, 0, 0.0, 0, 0)

    def _location_check(self, c: dict) -> tuple[int, int, float, int, int]:
        """« Le bloc juste au-dessus (ou juste en dessous) est X dans l'état Y. »

        Les quatre seuls usages des tables de blocs sont de cette forme, et
        servent aux deux plantes à deux blocs : casser la moitié basse ne donne
        de graines que si la moitié haute est bien là. Toute autre forme est
        refusée — supposer que le reste se comporte pareil est exactement ce
        qu'on ne fait pas ici.
        """
        offset_y = c.get("offsetY")
        if offset_y not in (1, -1) or c.get("offsetX") or c.get("offsetZ"):
            raise Unsupported(f"location_check décalé de {c.get('offsetX')},"
                              f"{offset_y},{c.get('offsetZ')}")
        block = c.get("predicate", {}).get("block", {})
        names = block.get("blocks") or []
        state = block.get("state") or {}
        if len(names) != 1 or len(state) != 1:
            raise Unsupported(f"location_check sur {names} / {state}")
        (prop, value), = state.items()
        first = len(self.ints)
        self.ints.extend([self.intern(names[0]), self.intern(prop), self.intern(str(value))])
        return (COND_NEIGHBOUR, 3, 0.0, first, offset_y & 0xFFFFFFFF)

    def _match_tool(self, predicate: dict) -> tuple[int, int, float, int, int]:
        enchants = predicate.get("enchantments")
        if enchants:
            if len(enchants) == 1 and enchants[0].get("enchantment") == "minecraft:silk_touch":
                return (COND_SILK_TOUCH, 0, 0.0, 0, 0)
            raise Unsupported(f"match_tool sur {enchants}")
        items = predicate.get("items")
        if items:
            first = len(self.ints)
            self.ints.extend(self.item_index[name] for name in items)
            return (COND_TOOL_IS, len(items), 0.0, first, 0)
        if "tag" in predicate:
            # Le tag est développé ici : le C++ ne compare que des ids.
            names = self.tag_items(predicate["tag"])
            first = len(self.ints)
            self.ints.extend(self.item_index[n] for n in names)
            return (COND_TOOL_IS, len(names), 0.0, first, 0)
        raise Unsupported(f"match_tool {predicate}")

    # ── fonctions ───────────────────────────────────────────────────────────

    def compile_functions(self, raw) -> tuple[int, int]:
        raw = raw or []
        first = len(self.funcs)
        self.funcs.extend([(FUNC_UNSUPPORTED, 0, 0.0, 0.0, 0, 0, 0)] * len(raw))
        for i, f in enumerate(raw):
            self.funcs[first + i] = self.compile_function(f)
        return first, len(raw)

    def compile_function(self, f: dict) -> tuple[int, int, float, float, int, int, int]:
        cond_first, cond_count = self.compile_conditions(f.get("conditions"))
        kind = f.get("function")

        def out(k, a=0.0, b=0.0, aux=0, count=0):
            return (k, count, a, b, aux, cond_first, cond_count)

        if kind == "minecraft:set_count":
            count = f["count"]
            if isinstance(count, (int, float)):
                return out(FUNC_COUNT_CONSTANT, float(count))
            t = count.get("type")
            if t == "minecraft:uniform":
                return out(FUNC_COUNT_UNIFORM, float(count["min"]), float(count["max"]))
            if t == "minecraft:binomial":
                return out(FUNC_COUNT_BINOMIAL, float(count["n"]), float(count["p"]))
            if t == "minecraft:constant":
                return out(FUNC_COUNT_CONSTANT, float(count["value"]))
            raise Unsupported(f"set_count {t}")
        if kind == "minecraft:explosion_decay":
            return out(FUNC_EXPLOSION_DECAY)
        if kind == "minecraft:apply_bonus":
            if f.get("enchantment") != "minecraft:fortune":
                raise Unsupported(f"apply_bonus sur {f.get('enchantment')}")
            formula = f["formula"]
            params = f.get("parameters", {})
            if formula == "minecraft:ore_drops":
                return out(FUNC_ORE_DROPS)
            if formula == "minecraft:uniform_bonus_count":
                return out(FUNC_UNIFORM_BONUS, float(params.get("bonusMultiplier", 1)))
            if formula == "minecraft:binomial_with_bonus_count":
                return out(FUNC_BINOMIAL_BONUS, float(params["extra"]),
                           float(params["probability"]))
            raise Unsupported(f"apply_bonus {formula}")
        if kind == "minecraft:limit_count":
            limit = f["limit"]
            return out(FUNC_LIMIT_COUNT, float(limit.get("min", -1e9)),
                       float(limit.get("max", 1e9)))
        self._note(kind or "?")
        return out(FUNC_UNSUPPORTED)

    # ── entrées ─────────────────────────────────────────────────────────────

    def compile_entries(self, raw) -> tuple[int, int]:
        raw = raw or []
        first = len(self.entries)
        self.entries.extend([(ENTRY_UNSUPPORTED, 0, 0, 0, 0, 0, 0, 0)] * len(raw))
        for i, e in enumerate(raw):
            self.entries[first + i] = self.compile_entry(e)
        return first, len(raw)

    def compile_entry(self, e: dict) -> tuple[int, int, int, int, int, int, int, int]:
        cond_first, cond_count = self.compile_conditions(e.get("conditions"))
        func_first, func_count = self.compile_functions(e.get("functions"))
        kind = e.get("type")
        if kind == "minecraft:item":
            item = self.item_index[e["name"]]
            return (ENTRY_ITEM, item, cond_first, cond_count, func_first, func_count, 0, 0)
        if kind == "minecraft:alternatives":
            child_first, child_count = self.compile_entries(e.get("children"))
            return (ENTRY_ALTERNATIVES, 0, cond_first, cond_count, func_first, func_count,
                    child_first, child_count)
        self._note(kind or "?")
        return (ENTRY_UNSUPPORTED, 0, cond_first, cond_count, func_first, func_count, 0, 0)

    # ── tables ──────────────────────────────────────────────────────────────

    def compile_table(self, doc: dict) -> tuple[int, int, int, int]:
        func_first, func_count = self.compile_functions(doc.get("functions"))
        pool_first = len(self.pools)
        raw_pools = doc.get("pools", [])
        self.pools.extend([(0.0, 0, 0, 0, 0, 0, 0)] * len(raw_pools))
        for i, p in enumerate(raw_pools):
            rolls = p.get("rolls", 1.0)
            if isinstance(rolls, dict):
                raise Unsupported(f"rolls {rolls}")
            cond_first, cond_count = self.compile_conditions(p.get("conditions"))
            pool_func_first, pool_func_count = self.compile_functions(p.get("functions"))
            entry_first, entry_count = self.compile_entries(p.get("entries"))
            self.pools[pool_first + i] = (float(rolls), entry_first, entry_count,
                                          cond_first, cond_count,
                                          pool_func_first, pool_func_count)
        return (pool_first, len(raw_pools), func_first, func_count)


def compile_block_tables(loot_dir: Path, blocks: list[dict], item_index: dict[str, int],
                         tag_items, intern, table_of: dict[str, str]) -> dict:
    """Une table par bloc, dans l'ordre du pack.

    `table_of` dit quelle table chaque bloc emploie, et ce n'est pas toujours
    celle qui porte son nom : une torche murale renvoie à `blocks/torch`, un
    panneau mural à `blocks/oak_sign`, et `wall_torch.json` n'existe pas. Cette
    correspondance est mesurée (scripts/measure_loot_tables.py) plutôt que
    devinée depuis le nom de fichier — la devinette tient pour neuf cent vingt
    blocs sur mille trois, ce qui est juste assez pour ne pas se voir.

    Un bloc renvoyé sur `minecraft:empty` n'a pas de butin du tout, et cela se
    distingue d'une table présente et sans résultat.
    """
    compiler = LootCompiler(item_index, intern)
    compiler.tag_items = tag_items
    records: list[tuple[int, int, int, int, int]] = []
    refused: dict[str, str] = {}
    present = 0

    compiled: dict[str, tuple[int, int, int, int]] = {}

    for block in blocks:
        table_id = table_of.get(block["name"], "minecraft:empty")
        if table_id == "minecraft:empty" or not table_id.startswith("minecraft:blocks/"):
            records.append((0, 0, 0, 0, 0))
            continue
        name = table_id.split("/", 1)[1]
        if name in compiled:
            # Plusieurs blocs partagent une table ; elle n'est aplatie qu'une
            # fois et les deux la désignent.
            pool_first, pool_count, func_first, func_count = compiled[name]
            records.append((1, pool_first, pool_count, func_first, func_count))
            present += 1
            continue
        path = loot_dir / f"{name}.json"
        if not path.is_file():
            records.append((0, 0, 0, 0, 0))
            continue
        with open(path) as f:
            doc = json.load(f)
        try:
            pool_first, pool_count, func_first, func_count = compiler.compile_table(doc)
        except Unsupported as error:
            refused[block["name"]] = str(error)
            records.append((0, 0, 0, 0, 0))
            continue
        compiled[name] = (pool_first, pool_count, func_first, func_count)
        records.append((1, pool_first, pool_count, func_first, func_count))
        present += 1

    return {
        "records": records,
        "pools": compiler.pools,
        "entries": compiler.entries,
        "conds": compiler.conds,
        "funcs": compiler.funcs,
        "floats": compiler.floats,
        "ints": compiler.ints,
        "present": present,
        "refused": refused,
        "unsupported": compiler.unsupported,
    }


def pack(sections: dict) -> dict[str, bytes]:
    """Les octets de chaque section, dans la disposition que lit ov_gameplay."""
    out = {}
    out["tables"] = b"".join(struct.pack("<IIIII", *r) for r in sections["records"])
    out["pools"] = b"".join(struct.pack("<fIIIIII", *p) for p in sections["pools"])
    out["entries"] = b"".join(struct.pack("<IIIIIIII", *e) for e in sections["entries"])
    out["conds"] = b"".join(struct.pack("<IIfII", *c) for c in sections["conds"])
    out["funcs"] = b"".join(struct.pack("<IIffIII", *f) for f in sections["funcs"])
    out["floats"] = b"".join(struct.pack("<f", x) for x in sections["floats"])
    out["ints"] = b"".join(struct.pack("<I", x) for x in sections["ints"])
    return out
