#!/usr/bin/env python3
"""Sonde d'arbre : faire pousser un seul arbre connu par chunk, et lire sa forme.

Même principe que `probe_decoration.py`, tourné vers la géométrie plutôt que
vers la graine. Un datapack vide toutes les listes de features de `plains` et
n'en laisse qu'une, à l'étape 9 (`vegetal_decoration`), index 0 :

    count(1) → in_square → heightmap(OCEAN_FLOOR) → <la feature demandée>

Le monde qui en sort a **un arbre par chunk**, isolé, sur un terrain de plaine.
Les deux premiers tirages de la graine de feature donnent sa colonne, et tout
le reste du flux appartient à l'arbre. Comparer notre modèle au sien n'est donc
plus « 40 % des troncs sont au bon endroit » : c'est une hauteur, un compte de
grappes et une liste d'attaches, chacun juste ou faux.

La graine de feature est celle que `docs/provenance/features.md` établit :

    a, b       = WorldgenRandom(graine du monde).next_long() | 1, deux fois
    décoration = (x_bloc·a + z_bloc·b) ^ graine du monde
    feature    = décoration + index + 10000 × étape

et `WorldgenRandom` est l'hybride : état Xoroshiro128++, bits extraits à la
mode *legacy*.

Sous-commandes :

    pack <dossier> <racine data vanilla> <feature…>  écrit le datapack
    read <monde> <sortie.json>                      extrait les arbres
    fancy <arbres.json> <graine>                    modèle du fancy oak
    dump <arbres.json> <graine> [n]                 imprime n arbres
"""
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import anvil_read

M64 = (1 << 64) - 1


def s64(x):
    x &= M64
    return x - (1 << 64) if x >> 63 else x


def s32(x):
    x &= 0xFFFFFFFF
    return x - (1 << 32) if x >> 31 else x


def rotl(x, k):
    x &= M64
    return ((x << k) | (x >> (64 - k))) & M64


def mix_stafford13(x):
    x &= M64
    x ^= x >> 30
    x = (x * 0xBF58476D1CE4E5B9) & M64
    x ^= x >> 27
    x = (x * 0x94D049BB133111EB) & M64
    x ^= x >> 31
    return x


class Worldgen:
    """`WorldgenRandom` : état Xoroshiro128++, primitives de java.util.Random.

    L'enveloppe ne redéfinit que `next(bits)` ; `nextInt`, `nextFloat` et
    `nextLong` restent ceux du JDK, construits par-dessus. Mesuré à 9712/9712
    marqueurs sur quatre graines — voir docs/provenance/features.md.
    """

    def __init__(self, seed):
        lo = (seed ^ 0x6A09E667F3BCC909) & M64
        hi = (lo + 0x9E3779B97F4A7C15) & M64
        self.lo = mix_stafford13(lo)
        self.hi = mix_stafford13(hi)
        if self.lo == 0 and self.hi == 0:
            self.lo, self.hi = 0x9E3779B97F4A7C15, 0x6A09E667F3BCC909

    def _next_long_raw(self):
        lo, hi = self.lo, self.hi
        n = (rotl((lo + hi) & M64, 17) + lo) & M64
        hi ^= lo
        self.lo = rotl(lo, 49) ^ hi ^ ((hi << 21) & M64)
        self.hi = rotl(hi, 28)
        return n

    def next(self, bits):
        return s32(self._next_long_raw() >> (64 - bits))

    def next_long(self):
        high = self.next(32)
        low = self.next(32)
        return s64((high << 32) + low)

    def next_int(self, bound):
        if bound <= 0:
            return 0
        if bound & (bound - 1) == 0:
            return (bound * self.next(31)) >> 31
        while True:
            bits = self.next(31)
            value = bits % bound
            if s32(bits - value + (bound - 1)) >= 0:
                return value

    def next_float(self):
        return self.next(24) / float(1 << 24)


def feature_seed(level_seed, min_x, min_z, index, step):
    source = Worldgen(level_seed)
    a = source.next_long() | 1
    b = source.next_long() | 1
    deco = s64(s64(min_x * a + min_z * b) ^ level_seed)
    return s64(deco + index + 10000 * step)


# --------------------------------------------------------------- pack


def write_pack(root, vanilla, features, step=9):
    """`features` : des noms de features configurées, ou `@fichier.json` pour
    une définition locale — c'est ce qui permet de faire varier *un* champ d'un
    placer et de lire ce que le jeu en fait, au lieu de le supposer.

    Deux formes de plus, pour les features qui ne sont pas des arbres :

    * `=minecraft:nom` nomme une **placed** feature de vanilla, gardée telle
      quelle avec son propre pipeline (une géode, un lac, un iceberg) ;
    * `%fichier.json` est une placed feature écrite par nous — typiquement
      `count(1) → in_square → height_range → <feature de vanilla>`, pour avoir
      une géode par chunk au lieu d'une sur vingt-quatre.

    `step` est l'étape de décoration où la liste est posée (9 par défaut). Une
    liste vide donne le monde **témoin** : même graine, même preset, aucune
    feature — c'est l'état du terrain avant la feature, bloc pour bloc.
    """
    if isinstance(features, str):
        features = [features]
    features = [f for f in features if f and f != "none"]
    root = pathlib.Path(root)
    vanilla = pathlib.Path(vanilla)
    for sub in ("data/minecraft/worldgen/biome",
                "data/probe/worldgen/configured_feature",
                "data/probe/worldgen/placed_feature",
                "data/probe/worldgen/world_preset"):
        (root / sub).mkdir(parents=True, exist_ok=True)

    (root / "pack.mcmeta").write_text(json.dumps(
        {"pack": {"pack_format": 15, "description": "tree shape probe"}}))

    preset = json.loads(
        (vanilla / "worldgen/world_preset/single_biome_surface.json").read_text())
    (root / "data/probe/worldgen/world_preset/probe.json").write_text(
        json.dumps(preset, indent=2))

    biome = json.loads((vanilla / "worldgen/biome/plains.json").read_text())
    biome["carvers"] = {}
    biome["features"] = [[] for _ in range(11)]
    # `=minecraft:nom` goes into the list under its own name: the biome lists
    # the vanilla placed feature itself, pipeline included, and the probe
    # writes no file for it. Its index is still its position in this list.
    biome["features"][step] = [
        f[1:] if f.startswith("=") else f"probe:tree{i}" for i, f in enumerate(features)]
    biome["spawners"] = {key: [] for key in biome.get("spawners", {})}
    biome["spawn_costs"] = {}
    (root / "data/minecraft/worldgen/biome/plains.json").write_text(
        json.dumps(biome, indent=2))

    # Une feature nommée est celle de vanilla, non copiée : le datapack ne doit
    # rien redéfinir de la forme de l'arbre, sinon la sonde mesure le datapack
    # et non le jeu. Une feature `@fichier` est au contraire délibérément à
    # nous : elle sert à faire varier *un* champ d'un placer et à lire la règle
    # que le jeu en tire.
    for index, feature in enumerate(features):
        target = root / f"data/probe/worldgen/placed_feature/tree{index}.json"
        if feature.startswith("="):
            # A vanilla placed feature, pipeline and all: the biome names it
            # directly and the probe redefines nothing.
            continue
        if feature.startswith("%"):
            target.write_text(pathlib.Path(feature[1:]).read_text())
            continue
        if feature.startswith("@"):
            body = json.loads(pathlib.Path(feature[1:]).read_text())
            name = f"probe:body{index}"
            (root / f"data/probe/worldgen/configured_feature/body{index}.json"
             ).write_text(json.dumps(body, indent=2))
        else:
            name = feature
        (root / f"data/probe/worldgen/placed_feature/tree{index}.json").write_text(
            json.dumps({"feature": name, "placement": [
                {"type": "minecraft:count", "count": 1},
                {"type": "minecraft:in_square"},
                {"type": "minecraft:heightmap", "heightmap": "OCEAN_FLOOR"}]},
                indent=2))
    print("datapack écrit dans", root, "pour", ", ".join(features))


# --------------------------------------------------------------- read

WOOD_SUFFIX = ("_log", "_wood", "_leaves", "_stem", "_hyphae", "_wart_block")
EXTRA = ("minecraft:vine", "minecraft:cocoa", "minecraft:bee_nest",
         "minecraft:mangrove_roots", "minecraft:moss_carpet",
         "minecraft:shroomlight")


def wanted(name):
    return name.endswith(WOOD_SUFFIX) or name in EXTRA


def section_blocks(section):
    states = section.get("block_states")
    if not states:
        return None, None
    palette = states.get("palette", [])
    entries = []
    for entry in palette:
        entries.append((entry["Name"], entry.get("Properties", {})))
    if not any(wanted(name) for name, _ in entries):
        return None, None
    return entries, states.get("data")


def read_world(world, out_path):
    rows = []
    for path in sorted((pathlib.Path(world) / "region").glob("*.mca")):
        for _, _, nbt in anvil_read.chunks(path):
            if nbt.get("Status") != "minecraft:full":
                continue
            cx, cz = nbt["xPos"], nbt["zPos"]
            cells = []
            for section in nbt.get("sections", []):
                entries, data = section_blocks(section)
                if entries is None:
                    continue
                y0 = section["Y"] * 16
                if data is None:
                    continue
                bits = max(4, (len(entries) - 1).bit_length())
                per = 64 // bits
                mask = (1 << bits) - 1
                for i in range(4096):
                    word = data[i // per]
                    index = (word >> ((i % per) * bits)) & mask
                    name, props = entries[index]
                    if not wanted(name):
                        continue
                    cells.append([cx * 16 + i % 16, y0 + i // 256,
                                  cz * 16 + (i % 256) // 16, name,
                                  props.get("axis", "")])
            if cells:
                rows.append({"cx": cx, "cz": cz, "cells": cells})
    pathlib.Path(out_path).write_text(json.dumps(rows))
    print(len(rows), "chunks avec du bois ->", out_path)


# --------------------------------------------------------------- modèle


def tree_shape(height, y):
    if float(y) < float(height) * 0.3:
        return -1.0
    half = height / 2.0
    frm = half - y
    out = (half * half - frm * frm) ** 0.5
    if frm == 0.0:
        out = half
    elif abs(frm) >= half:
        return 0.0
    return out * 0.5


def fancy_model(seed, base, clusters_rule):
    """Rend (hauteur, sommet du tronc, attaches) pour un fancy oak.

    `base` est la position du tronc. Les tirages sont pris dans l'ordre du jeu :
    hauteur (deux tirages), puis deux flottants par grappe.
    """
    random = Worldgen(seed)
    random.next_int(16)  # in_square dx
    random.next_int(16)  # in_square dz
    height_asked = 3 + random.next_int(12) + random.next_int(1)
    # foliage_height et radius sont des constantes du fichier : aucun tirage.
    free = height_asked  # sur une plaine dégagée
    height = free + 2
    trunk_top = int(height * 0.618)
    clusters = clusters_rule(height)
    base_y = base[1] + trunk_top
    coords = [((base[0], base[1] + height - 5, base[2]), base_y)]
    import math
    for y in range(height - 5, -1, -1):
        shape = tree_shape(height, y)
        if shape < 0.0:
            continue
        for _ in range(clusters):
            spread_draw = random.next_float()
            spread = 1.0 * shape * (spread_draw + 0.328)
            angle_draw = random.next_float()
            angle = (angle_draw * 2.0) * math.pi
            dx = spread * math.sin(angle) + 0.5
            dz = spread * math.cos(angle) + 0.5
            start = (base[0] + math.floor(dx), base[1] + y - 1,
                     base[2] + math.floor(dz))
            back_x = base[0] - start[0]
            back_z = base[2] - start[2]
            slope = start[1] - math.sqrt(back_x * back_x + back_z * back_z) * 0.381
            branch_base = base_y if slope > base_y else int(slope)
            coords.append((start, branch_base))
    return height, trunk_top, coords


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    command = sys.argv[1]
    if command == "pack":
        import os
        write_pack(sys.argv[2], sys.argv[3], sys.argv[4:],
                   step=int(os.environ.get("STEP", "9")))
    elif command == "read":
        read_world(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
