#!/usr/bin/env python3
"""Sonde de la graine de décoration : lire le flux du RNG, pas ses effets.

Le problème que cet outil résout : la parité des minerais mesure la graine de
décoration à travers un proxy bruité. Une veine de minerai est un nuage de
blocs, elle chevauche ses voisines, et « ce minerai est-il au bon endroit »
répond oui une fois sur dix par hasard. Balayer 130 000 décalages dans ce
bruit-là ne peut rien trancher.

Ici, le jeu lui-même écrit le flux de tirages sur le disque. Un datapack
remplace la liste de features de `plains` par quatre marqueurs, le preset de
monde force `plains` partout, et chaque marqueur est un bloc unique posé par
`simple_block` derrière `count(1) → in_square → height_range(uniforme sur seize
blocs)`. Sa position est donc exactement trois `nextInt(16)` :

    dx = nextInt(16)   (in_square, x d'abord)
    dz = nextInt(16)   (in_square, z ensuite)
    dy = nextInt(16)   (height_range uniforme sur une bande de seize)

Douze bits exacts par marqueur, quarante-huit par chunk. Une hypothèse sur la
graine n'est plus un pourcentage : elle est juste ou fausse, et elle est fausse
au premier chunk.

Les marqueurs, leur étape et leur index :

    m0  gold_block     étape 6 (underground_ores)  index 0  bande -60..-45
    m1  diamond_block  étape 6                     index 1  bande -40..-25
    m2  emerald_block  étape 6                     index 2  bande -20..-5
    m3  iron_block     étape 8 (fluid_springs)     index 0  bande   0.. 15

Trois marqueurs à des index consécutifs contraignent le terme `+ index` ; le
quatrième, à une autre étape, contraint le terme `10000 × étape`.

Sous-commandes :

    pack   <dossier> <racine data vanilla>   écrit le datapack
    read   <monde> <sortie.json>             extrait les marqueurs
    check  <marqueurs.json> <graine>         balaie les variantes de formule
"""
import collections
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import anvil_read

MARKERS = {
    "minecraft:gold_block": "m0",
    "minecraft:diamond_block": "m1",
    "minecraft:emerald_block": "m2",
    "minecraft:iron_block": "m3",
}
BANDS = {"m0": -60, "m1": -40, "m2": -20, "m3": 0}
SLOT = {"m0": (6, 0), "m1": (6, 1), "m2": (6, 2), "m3": (8, 0)}

M64 = (1 << 64) - 1
M48 = (1 << 48) - 1


def s64(x):
    x &= M64
    return x - (1 << 64) if x >> 63 else x


def s32(x):
    x &= 0xFFFFFFFF
    return x - (1 << 32) if x >> 31 else x


def rotl(x, k):
    x &= M64
    return ((x << k) | (x >> (64 - k))) & M64


class Legacy:
    """java.util.Random : LCG tronqué à 48 bits, spécifié par le JDK."""

    def __init__(self, seed):
        self.seed = (seed ^ 0x5DEECE66D) & M48

    def next(self, bits):
        self.seed = (self.seed * 0x5DEECE66D + 0xB) & M48
        return s32(self.seed >> (48 - bits))

    def next_int(self, bound):
        if bound & (bound - 1) == 0:
            return (bound * self.next(31)) >> 31
        while True:
            bits = self.next(31)
            val = bits % bound
            if s32(bits - val + (bound - 1)) >= 0:
                return val

    def next_long(self):
        hi = self.next(32)
        lo = self.next(32)
        return s64((hi << 32) + lo)


def mix_stafford13(x):
    x &= M64
    x ^= x >> 30
    x = (x * 0xBF58476D1CE4E5B9) & M64
    x ^= x >> 27
    x = (x * 0x94D049BB133111EB) & M64
    x ^= x >> 31
    return x


class Xoroshiro:
    """Xoroshiro128++ tel que le jeu l'ensemence (ratio d'argent, mix de
    Stafford sur chaque moitié)."""

    def __init__(self, seed):
        lo = (seed ^ 0x6A09E667F3BCC909) & M64
        hi = (lo + 0x9E3779B97F4A7C15) & M64
        self.lo = mix_stafford13(lo)
        self.hi = mix_stafford13(hi)
        if self.lo == 0 and self.hi == 0:
            self.lo, self.hi = 0x9E3779B97F4A7C15, 0x6A09E667F3BCC909

    def next_long(self):
        lo, hi = self.lo, self.hi
        n = (rotl((lo + hi) & M64, 17) + lo) & M64
        hi ^= lo
        self.lo = rotl(lo, 49) ^ hi ^ ((hi << 21) & M64)
        self.hi = rotl(hi, 28)
        return s64(n)

    def next_int(self, bound):
        value = self.next_long() & 0xFFFFFFFF
        product = value * bound
        low = product & 0xFFFFFFFF
        if low < bound:
            limit = ((1 << 32) - bound) % bound
            while low < limit:
                value = self.next_long() & 0xFFFFFFFF
                product = value * bound
                low = product & 0xFFFFFFFF
        return product >> 32


FAMILIES = {"legacy": Legacy, "xoroshiro": Xoroshiro}


# --------------------------------------------------------------- pack


def write_pack(root, vanilla):
    root = pathlib.Path(root)
    vanilla = pathlib.Path(vanilla)
    for sub in ("data/minecraft/worldgen/biome",
                "data/probe/worldgen/configured_feature",
                "data/probe/worldgen/placed_feature",
                "data/probe/worldgen/world_preset"):
        (root / sub).mkdir(parents=True, exist_ok=True)

    (root / "pack.mcmeta").write_text(json.dumps(
        {"pack": {"pack_format": 15, "description": "decoration seed probe"}}))

    # Une copie de minecraft:single_biome_surface sous notre propre identifiant.
    # Le serveur dédié traite le sien à part (il relit le biome dans
    # generator-settings) ; un identifiant privé passe par le chemin ordinaire.
    preset = json.loads(
        (vanilla / "worldgen/world_preset/single_biome_surface.json").read_text())
    (root / "data/probe/worldgen/world_preset/probe.json").write_text(
        json.dumps(preset, indent=2))

    biome = json.loads((vanilla / "worldgen/biome/plains.json").read_text())
    biome["carvers"] = {}
    biome["features"] = [[] for _ in range(11)]
    biome["features"][6] = ["probe:m0", "probe:m1", "probe:m2"]
    biome["features"][8] = ["probe:m3"]
    biome["spawners"] = {key: [] for key in biome.get("spawners", {})}
    biome["spawn_costs"] = {}
    (root / "data/minecraft/worldgen/biome/plains.json").write_text(
        json.dumps(biome, indent=2))

    blocks = {"m0": "minecraft:gold_block", "m1": "minecraft:diamond_block",
              "m2": "minecraft:emerald_block", "m3": "minecraft:iron_block"}
    for name, block in blocks.items():
        (root / f"data/probe/worldgen/configured_feature/{name}.json").write_text(
            json.dumps({"type": "minecraft:simple_block",
                        "config": {"to_place": {
                            "type": "minecraft:simple_state_provider",
                            "state": {"Name": block}}}}, indent=2))
        low = BANDS[name]
        (root / f"data/probe/worldgen/placed_feature/{name}.json").write_text(
            json.dumps({"feature": f"probe:{name}", "placement": [
                {"type": "minecraft:count", "count": 1},
                {"type": "minecraft:in_square"},
                {"type": "minecraft:height_range", "height": {
                    "type": "minecraft:uniform",
                    "min_inclusive": {"absolute": low},
                    "max_inclusive": {"absolute": low + 15}}}]}, indent=2))
    print("datapack écrit dans", root)


# --------------------------------------------------------------- read


def section_markers(section):
    states = section.get("block_states")
    if not states:
        return None
    palette = states.get("palette", [])
    names = [entry.get("Name") for entry in palette]
    if not any(name in MARKERS for name in names):
        return None
    data = states.get("data")
    if data is None:
        return None
    bits = max(4, (len(palette) - 1).bit_length())
    per_long = 64 // bits
    mask = (1 << bits) - 1
    found = []
    for index in range(4096):
        word = data[index // per_long] & M64
        value = (word >> (index % per_long * bits)) & mask
        if value < len(names) and names[value] in MARKERS:
            found.append((index, names[value]))
    return found


def read_world(world, out_path):
    rows = []
    for path in sorted(pathlib.Path(world, "region").glob("*.mca")):
        rx, rz = (int(part) for part in path.stem.split(".")[1:3])
        for lx, lz, nbt in anvil_read.chunks(path):
            if nbt.get("Status") != "minecraft:full":
                continue
            cx, cz = rx * 32 + lx, rz * 32 + lz
            found = {}
            for section in nbt.get("sections", []):
                hits = section_markers(section)
                if not hits:
                    continue
                base_y = section["Y"] * 16
                for index, name in hits:
                    found.setdefault(MARKERS[name], []).append(
                        [cx * 16 + index % 16,
                         base_y + index // 256,
                         cz * 16 + index % 256 // 16])
            rows.append({"cx": cx, "cz": cz, "markers": found})
    pathlib.Path(out_path).write_text(json.dumps(rows))
    whole = sum(1 for row in rows if len(row["markers"]) == 4)
    print(f"{len(rows)} chunks complets, {whole} avec les quatre marqueurs "
          f"-> {out_path}")


def draws(rows):
    """Les trois tirages de chaque marqueur, par chunk."""
    out = []
    for row in rows:
        entry = {"cx": row["cx"], "cz": row["cz"]}
        for name, places in row["markers"].items():
            if len(places) != 1:
                continue
            x, y, z = places[0]
            entry[name] = (x - row["cx"] * 16, z - row["cz"] * 16,
                           y - BANDS[name])
        out.append(entry)
    return out


# --------------------------------------------------------------- check


def decoration_seed(level_seed, min_x, min_z, family, shape="xor", odd=True):
    source = family(level_seed)
    a = source.next_long()
    b = source.next_long()
    if odd:
        a |= 1
        b |= 1
    mixed = s64(min_x * a + min_z * b)
    if shape == "xor":
        return s64(mixed ^ level_seed)
    if shape == "plus":
        return s64(mixed + level_seed)
    if shape == "bare":
        return mixed
    raise ValueError(shape)


def predict(deco, index, step, family):
    source = family(s64(deco + index + 10000 * step))
    return (source.next_int(16), source.next_int(16), source.next_int(16))


def check(rows, level_seed):
    observed = draws(rows)
    for deco_name, deco_family in FAMILIES.items():
        for feat_name, feat_family in FAMILIES.items():
            for shape in ("xor", "plus", "bare"):
                for odd in (True, False):
                    for chunk_coords in (False, True):
                        hits = collections.Counter()
                        total = 0
                        for row in observed:
                            scale = 1 if chunk_coords else 16
                            deco = decoration_seed(
                                level_seed, row["cx"] * scale, row["cz"] * scale,
                                deco_family, shape, odd)
                            for name, (step, index) in SLOT.items():
                                if name not in row:
                                    continue
                                total += 1
                                if predict(deco, index, step,
                                           feat_family) == row[name]:
                                    hits[name] += 1
                        got = sum(hits.values())
                        flag = "  <-- " if got * 64 > total else ""
                        print(f"deco={deco_name:9} feat={feat_name:9} "
                              f"shape={shape:5} odd={int(odd)} "
                              f"chunk_coords={int(chunk_coords)}: "
                              f"{got}/{total}{flag}")
    print("\nUn accord correct vaut le total ; tout le reste est du hasard "
          "à 1/4096 par marqueur.")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    command = sys.argv[1]
    if command == "pack":
        write_pack(sys.argv[2], sys.argv[3])
    elif command == "read":
        read_world(sys.argv[2], sys.argv[3])
    elif command == "check":
        rows = json.loads(pathlib.Path(sys.argv[2]).read_text())
        check(rows, int(sys.argv[3]))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
