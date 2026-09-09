"""Compile les recettes du datapack vanilla en tableaux plats.

Les recettes sont vraiment data-driven en 1.20.1 : elles vivent dans le
datapack, on les régénère localement, et rien de ce qu'elles contiennent n'est
écrit à la main ici. Ce qui est du code, c'est **l'appariement** — et c'est lui
qu'on écrit, dans ov_gameplay, pas un second format de recette.

Comme pour les tags et les tables de butin, l'aplatissement a lieu à la
compilation : un ingrédient qui dit `#minecraft:planks` est développé ici en
liste triée d'ids d'objets, pour que la partie ne fasse jamais qu'une recherche
dichotomique. Résoudre un tag à chaque case de la grille mettrait une recherche
de chaîne dans le chemin d'une fabrication.

Le vocabulaire de 1.20.1 est petit et fermé :

  * neuf genres data-driven — `crafting_shaped`, `crafting_shapeless`,
    `smelting`, `blasting`, `smoking`, `campfire_cooking`, `stonecutting`,
    `smithing_transform`, `smithing_trim` ;
  * quatorze recettes **spéciales**, qui n'ont aucune donnée : leur fichier ne
    contient qu'un type et une catégorie, parce qu'en vanilla ce sont des
    classes Java. Elles sont conservées comme *déclarations* — le client les
    reçoit et son livre de recettes s'en sert — mais marquées comme non
    appariables, nommées et comptées. Les traiter comme des recettes vides
    ferait fabriquer n'importe quoi avec une grille vide.

Un genre inconnu est refusé bruyamment plutôt que compilé en silence.
"""
from __future__ import annotations

import json
import struct
from pathlib import Path

# ── Genres, partagés avec src/ov_registry/include/ov/registry/recipe_data.hpp ─

KIND_SHAPED = 0
KIND_SHAPELESS = 1
KIND_SMELTING = 2
KIND_BLASTING = 3
KIND_SMOKING = 4
KIND_CAMPFIRE = 5
KIND_STONECUTTING = 6
KIND_SMITHING_TRANSFORM = 7
KIND_SMITHING_TRIM = 8
KIND_SPECIAL = 9

KIND_OF_TYPE = {
    "minecraft:crafting_shaped": KIND_SHAPED,
    "minecraft:crafting_shapeless": KIND_SHAPELESS,
    "minecraft:smelting": KIND_SMELTING,
    "minecraft:blasting": KIND_BLASTING,
    "minecraft:smoking": KIND_SMOKING,
    "minecraft:campfire_cooking": KIND_CAMPFIRE,
    "minecraft:stonecutting": KIND_STONECUTTING,
    "minecraft:smithing_transform": KIND_SMITHING_TRANSFORM,
    "minecraft:smithing_trim": KIND_SMITHING_TRIM,
}

# L'ordre de ces deux énumérations est un **ordre de protocole** : le client
# reçoit la catégorie comme un VarInt dans Update Recipes et range la recette
# dans l'onglet correspondant de son livre. Il n'est donc pas nôtre.
# CraftingBookCategory puis CookingBookCategory, dans l'ordre déclaré par le
# jeu — vérifié contre l'archive figée du protocole 763.
CRAFTING_CATEGORY = {"building": 0, "redstone": 1, "equipment": 2, "misc": 3}
COOKING_CATEGORY = {"food": 0, "blocks": 1, "misc": 2}

FLAG_SHOW_NOTIFICATION = 1 << 0
# La recette n'est pas appariable par ce serveur : elle existe pour être
# déclarée au client, et l'appariement doit la sauter.
FLAG_DECLARATION_ONLY = 1 << 1


class Unsupported(Exception):
    pass


def trim(pattern: list[str]) -> list[str]:
    """Retire les lignes et colonnes entièrement vides d'un motif.

    Le jeu le fait au chargement, et un seul motif de 1.20.1 en a besoin : la
    longue-vue s'écrit `[" # ", " X ", " X "]`, trois cases de large pour une
    colonne utile. Sans cette coupe, le motif exige une empreinte de trois de
    large et la longue-vue ne se fabrique jamais — un seul objet manquant, dans
    un jeu qui a l'air de marcher.
    """
    width = max(len(row) for row in pattern)
    rows = [row.ljust(width) for row in pattern]
    while rows and all(c == " " for c in rows[0]):
        rows.pop(0)
    while rows and all(c == " " for c in rows[-1]):
        rows.pop()
    while rows and all(row[0] == " " for row in rows):
        rows = [row[1:] for row in rows]
    while rows and rows[0] and all(row[-1] == " " for row in rows):
        rows = [row[:-1] for row in rows]
    if not rows or not rows[0]:
        raise Unsupported("motif entièrement vide")
    return rows


class RecipeCompiler:
    """Aplatit les recettes. Les listes produites sont écrites telles quelles."""

    def __init__(self, item_index: dict[str, int], item_tags: dict[str, list[str]], intern):
        self.item_index = item_index
        self.item_tags = item_tags
        self.intern = intern
        self.records: list[tuple] = []
        self.ingredients: list[tuple[int, int]] = []
        self.choices: list[int] = []
        self.refused: dict[str, str] = {}
        self.declaration_only: list[str] = []
        self.trimmed: list[str] = []
        # Les listes de choix reviennent sans arrêt — `#minecraft:planks` est
        # dans des dizaines de recettes. Les partager divise la section par cinq
        # et rend l'appariement plus amical pour le cache.
        self._pool: dict[tuple[int, ...], int] = {}

    # ── ingrédients ─────────────────────────────────────────────────────────

    def choice_ids(self, ing) -> list[int]:
        """Les objets qu'un ingrédient accepte, en ids, triés et sans doublon."""
        if isinstance(ing, list):
            ids: set[int] = set()
            for one in ing:
                ids.update(self.choice_ids(one))
            return sorted(ids)
        if not isinstance(ing, dict):
            raise Unsupported(f"ingrédient de forme {type(ing).__name__}")
        if "item" in ing and "tag" in ing:
            raise Unsupported("ingrédient à la fois item et tag")
        if "item" in ing:
            name = ing["item"]
            if name not in self.item_index:
                raise Unsupported(f"objet inconnu {name}")
            return [self.item_index[name]]
        if "tag" in ing:
            tag = ing["tag"]
            if not tag.startswith("minecraft:"):
                tag = "minecraft:" + tag
            if tag not in self.item_tags:
                raise Unsupported(f"tag d'objets inconnu {tag}")
            # Les membres sont déjà des noms résolus par le normalisateur.
            return sorted({self.item_index[n] for n in self.item_tags[tag]
                           if n in self.item_index})
        raise Unsupported(f"ingrédient sans item ni tag : {sorted(ing)}")

    def add_ingredient(self, ing) -> None:
        ids = tuple(self.choice_ids(ing))
        if not ids:
            # Un ingrédient que rien ne satisfait rendrait la recette
            # impossible sans le dire. En 1.20.1 aucun n'est dans ce cas.
            raise Unsupported("ingrédient qu'aucun objet ne satisfait")
        first = self._pool.get(ids)
        if first is None:
            first = len(self.choices)
            self.choices.extend(ids)
            self._pool[ids] = first
        self.ingredients.append((first, len(ids)))

    def add_empty_ingredient(self) -> None:
        """Une case vide d'un motif façonné. Zéro choix, ce qui se lit « rien »."""
        self.ingredients.append((0, 0))

    # ── résultat ────────────────────────────────────────────────────────────

    def result_of(self, doc: dict) -> tuple[int, int]:
        result = doc.get("result")
        if isinstance(result, str):
            # Les recettes de cuisson et de découpe donnent le nom nu ; la
            # quantité vit alors dans `count`, absent partout sauf en découpe.
            name, count = result, int(doc.get("count", 1))
        elif isinstance(result, dict):
            name, count = result["item"], int(result.get("count", 1))
        else:
            raise Unsupported("résultat sans objet")
        if name not in self.item_index:
            raise Unsupported(f"résultat inconnu {name}")
        return self.item_index[name], count

    # ── une recette ─────────────────────────────────────────────────────────

    def compile(self, recipe_id: str, doc: dict) -> None:
        kind = KIND_OF_TYPE.get(doc["type"])
        if kind is None:
            if doc["type"].startswith("minecraft:crafting_"):
                # Une recette spéciale : du code Java, pas des données. On la
                # déclare sans l'apparier, et on la nomme.
                self.declaration_only.append(recipe_id)
                self._emit(recipe_id, KIND_SPECIAL, doc, -1, 0, 0, 0, 0, 0,
                           FLAG_DECLARATION_ONLY, special_type=doc["type"])
                return
            raise Unsupported(f"genre inconnu {doc['type']}")

        first = len(self.ingredients)
        width = height = 0

        if kind == KIND_SHAPED:
            key = doc["key"]
            pattern = trim(doc["pattern"])
            if pattern != [row for row in doc["pattern"]]:
                self.trimmed.append(recipe_id)
            height = len(pattern)
            width = max(len(row) for row in pattern)
            if not 1 <= width <= 3 or not 1 <= height <= 3:
                raise Unsupported(f"motif {width}x{height}")
            for row in pattern:
                # Une ligne peut être plus courte que la plus large : elle est
                # complétée par du vide, pas repliée.
                padded = row.ljust(width)
                for symbol in padded:
                    if symbol == " ":
                        self.add_empty_ingredient()
                    elif symbol in key:
                        self.add_ingredient(key[symbol])
                    else:
                        raise Unsupported(f"symbole {symbol!r} absent de la clé")
        elif kind == KIND_SHAPELESS:
            for ing in doc["ingredients"]:
                self.add_ingredient(ing)
        elif kind in (KIND_SMELTING, KIND_BLASTING, KIND_SMOKING, KIND_CAMPFIRE,
                      KIND_STONECUTTING):
            self.add_ingredient(doc["ingredient"])
        elif kind == KIND_SMITHING_TRANSFORM:
            for field in ("template", "base", "addition"):
                self.add_ingredient(doc[field])
        elif kind == KIND_SMITHING_TRIM:
            for field in ("template", "base", "addition"):
                self.add_ingredient(doc[field])

        count = len(self.ingredients) - first

        if kind == KIND_SMITHING_TRIM:
            # Le résultat est la pièce d'armure d'entrée, ornée : il n'existe
            # pas comme objet distinct. -1 dit « pas d'objet », ce qui n'est pas
            # la même chose que zéro, qui serait `minecraft:air`.
            result_item, result_count = -1, 0
        else:
            result_item, result_count = self.result_of(doc)

        experience = float(doc.get("experience", 0.0))
        cook_time = int(doc.get("cookingtime", 0))
        flags = FLAG_SHOW_NOTIFICATION if doc.get("show_notification") else 0
        if kind == KIND_SMITHING_TRIM:
            flags |= FLAG_DECLARATION_ONLY
            self.declaration_only.append(recipe_id)

        self._emit(recipe_id, kind, doc, result_item, result_count, first, count,
                   width, height, flags, experience=experience, cook_time=cook_time)

    def _emit(self, recipe_id: str, kind: int, doc: dict, result_item: int,
              result_count: int, first: int, count: int, width: int, height: int,
              flags: int, experience: float = 0.0, cook_time: int = 0,
              special_type: str | None = None) -> None:
        category_names = (COOKING_CATEGORY
                          if kind in (KIND_SMELTING, KIND_BLASTING, KIND_SMOKING,
                                      KIND_CAMPFIRE)
                          else CRAFTING_CATEGORY)
        raw_category = doc.get("category", "misc")
        if raw_category not in category_names:
            raise Unsupported(f"catégorie inconnue {raw_category}")
        self.records.append((
            self.intern(recipe_id),
            self.intern(doc.get("group", "")),
            self.intern(special_type or doc["type"]),
            result_item,
            result_count,
            first,
            count,
            experience,
            cook_time,
            kind,
            width,
            height,
            category_names[raw_category],
            flags,
        ))


def compile_recipes(recipe_dir: Path, item_index: dict[str, int],
                    item_tags: dict[str, list[str]], intern) -> dict:
    """Toutes les recettes du dossier, triées par identifiant.

    Le tri est ce qui rend la sortie reproductible d'une machine à l'autre : le
    parcours d'un dossier ne l'est pas.
    """
    compiler = RecipeCompiler(item_index, item_tags, intern)
    paths = sorted(recipe_dir.glob("*.json"))
    for path in paths:
        recipe_id = "minecraft:" + path.stem
        with open(path) as handle:
            doc = json.load(handle)
        try:
            compiler.compile(recipe_id, doc)
        except (Unsupported, KeyError) as error:
            compiler.refused[recipe_id] = str(error)

    return {
        "records": compiler.records,
        "ingredients": compiler.ingredients,
        "choices": compiler.choices,
        "refused": compiler.refused,
        "declaration_only": compiler.declaration_only,
        "trimmed": compiler.trimmed,
        "seen": len(paths),
    }


def pack(sections: dict) -> dict[str, bytes]:
    """Les octets de chaque section, dans la disposition que lit ov_registry."""
    out = {}
    # u32 id, u32 group, u32 type_name, i32 result_item, i32 result_count,
    # u32 ingredient_first, u32 ingredient_count, f32 experience,
    # u32 cook_time, u8 kind, u8 width, u8 height, u8 category, u32 flags.
    out["recipes"] = b"".join(struct.pack("<IIIiiIIfIBBBBI", *r) for r in sections["records"])
    out["ingredients"] = b"".join(struct.pack("<II", *i) for i in sections["ingredients"])
    out["choices"] = b"".join(struct.pack("<i", c) for c in sections["choices"])
    return out
