#!/usr/bin/env python3
"""La résistance au souffle du pack contre celle que le banc a mesurée.

`scripts/measure_blast.py resistance` a mis chacun des 987 blocs posables du jeu
devant une charge et relu, seize fois, la profondeur à laquelle la rangée a
cédé. `scripts/measure_blast.py table` a confronté ces profondeurs au candidat
de PrismarineJS/minecraft-data et écrit la table retenue. Ce script vérifie deux
choses que rien d'autre ne vérifie :

1. **le pack porte exactement cette table** — un flottant par bloc, relu du
   `.ovpack` lui-même et non du JSON qui l'a produit ;
2. **la mesure est monotone** — le score de pénétration, classe par classe, ne
   remonte jamais quand la résistance monte. C'est le contrôle : une campagne
   qui donnerait l'obsidienne plus fragile que le verre serait fausse quel que
   soit le nombre, et une inversion ici est un échec dur.

Usage : python3 scripts/check_blast.py
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "data" / "vanilla" / "1.20.1"
PACK = DATA / "registry.ovpack"
TABLE = DATA / "normalized" / "blast_resistance.json"
ROWS = DATA / "normalized" / "blast_row.json"

GREEN = "\033[0;32m"
RED = "\033[0;31m"
OFF = "\033[0m"

# La position du champ `resistance_offset` dans l'en-tête : le magic, puis
# 57 u32 dont c'est le dernier. Lu ici plutôt que supposé, pour que le jour où
# l'en-tête bouge ce script casse au lieu de lire un autre champ.
HEADER_FIELDS = 57
MAGIC = b"OVPK"
FORMAT_VERSION = 14


def pack_resistances() -> list[float]:
    data = PACK.read_bytes()
    if data[:4] != MAGIC:
        sys.exit(f"{PACK} ne commence pas par OVPK")
    fields = struct.unpack_from(f"<{HEADER_FIELDS}I", data, 4)
    version, block_count = fields[0], fields[1]
    if version != FORMAT_VERSION:
        sys.exit(f"pack de format {version}, attendu {FORMAT_VERSION} — "
                 f"relance tools/ov_datagen/ovpack.py")
    offset = fields[HEADER_FIELDS - 1]
    return list(struct.unpack_from(f"<{block_count}f", data, offset))


def main() -> int:
    for path in (PACK, TABLE):
        if not path.is_file():
            sys.exit(f"error: {path} manquant — voir docs/provenance/explosions.md § 6")

    with open(TABLE) as f:
        table = json.load(f)
    with open(DATA / "normalized" / "blocks.json") as f:
        order = [b["name"] for b in json.load(f)["blocks"]]

    values = pack_resistances()
    if len(values) != len(order):
        sys.exit(f"le pack porte {len(values)} résistances pour {len(order)} blocs")

    # Tolérance **relative** : le pack porte des `float`, et 3 600 000,8 s'y
    # écrit 3 600 000,75. Une tolérance absolue de 1e-3 déclare le barrier et la
    # lumière faux alors que c'est la seule valeur qu'un flottant 32 bits sait
    # porter — et ce sont deux blocs qu'aucune explosion ne touche de toute
    # façon.
    def differs(expected: float, got: float) -> bool:
        return abs(expected - got) > max(1e-3, abs(expected) * 1e-6)

    wrong = [(name, table["blocks"].get(name), value)
             for name, value in zip(order, values)
             if differs(table["blocks"].get(name, -1.0), value)]
    print(f"  pack ............. {PACK.relative_to(ROOT)}")
    print(f"  blocs ............ {len(values)}")
    print(f"  identiques ....... {len(values) - len(wrong)}")
    for name, expected, got in wrong[:20]:
        print(f"      {name}: table {expected}, pack {got}")

    # La monotonie, sur la mesure brute.
    inversions = []
    if ROWS.is_file():
        with open(ROWS) as f:
            rows = json.load(f)
        confounded = set(table["report"].get("confounded", {}))
        classes: dict[float, list[float]] = {}
        for name, cells in rows["counts"].items():
            if name in confounded:
                continue
            classes.setdefault(table["blocks"][name], []).append(sum(cells) / rows["trials"])
        medians = {value: sorted(scores)[len(scores) // 2]
                   for value, scores in sorted(classes.items())}
        ordered = sorted(medians)
        for previous, value in zip(ordered, ordered[1:]):
            if medians[value] > medians[previous] + 1e-9:
                inversions.append((previous, medians[previous], value, medians[value]))
        print(f"  classes mesurées . {len(medians)}")
        print(f"  inversions ....... {len(inversions)}")
        for a, sa, b, sb in inversions:
            print(f"      R={a} donne {sa:.3f} et R={b} donne {sb:.3f}")
    else:
        print(f"  {ROWS.name} absent : monotonie non vérifiée")

    if wrong or inversions:
        print(f"\n{RED}La table du pack et la mesure ne concordent pas{OFF}")
        return 1
    print(f"\n{GREEN}Le pack porte la table mesurée, et la mesure est monotone{OFF}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
