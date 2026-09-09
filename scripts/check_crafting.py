#!/usr/bin/env python3
"""Traduit l'oracle de fabrication en la table plate que lit le test de parité.

`scripts/measure_crafting.py` interroge un vrai serveur 1.20.1 et écrit un JSON
détaillé. Le test C++ n'a pas d'analyseur JSON — simdjson n'arrive qu'avec les
datapacks — et n'en a pas besoin : ce qu'il compare tient en une ligne par
grille.

    <résultat> <compte> <c0> <c1> … <c8>

où chaque case est un nom d'objet ou `-`, et où le résultat est `-` quand le
jeu n'a rien mis dans la case de sortie. Ces lignes-là sont la moitié
intéressante de la mesure : une implémentation trop généreuse ne se voit que
sur les grilles qui ne doivent rien donner.

Usage : python3 scripts/check_crafting.py [entrée.json] [sortie.txt]
"""
from __future__ import annotations

import json
import os
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = Path(os.environ.get("OV_NORMALIZED", ROOT / "data/vanilla/1.20.1/normalized"))


def main() -> int:
    source = Path(sys.argv[1]) if len(sys.argv) > 1 else NORMALIZED / "crafting.json"
    target = Path(sys.argv[2]) if len(sys.argv) > 2 else NORMALIZED / "crafting_grids.txt"
    if not source.is_file():
        sys.exit(f"error: {source} not found. Run scripts/measure_crafting.py first.")

    doc = json.loads(source.read_text(encoding="utf-8"))
    kinds: Counter[str] = Counter()
    lines = []
    for case in doc["cases"]:
        kinds[case["kind"]] += 1
        result = case["result"]
        cells = " ".join(name or "-" for name in case["grid"])
        if result is None:
            lines.append(f"- 0 {cells}")
        else:
            lines.append(f"{result['item']} {result['count']} {cells}")

    header = [
        "# Ce qu'un vrai serveur 1.20.1 met dans la case de sortie pour chaque grille.",
        "# Une ligne par grille : résultat, quantité, puis les neuf cases en lignes.",
        "# '-' est une case vide, et un résultat '-' veut dire que le jeu n'a rien fait.",
        f"# {len(lines)} grilles, dont " + ", ".join(f"{n} {k}" for k, n in kinds.most_common()),
    ]
    target.write_text("\n".join(header + lines) + "\n", encoding="utf-8")

    with_result = sum(1 for c in doc["cases"] if c["result"])
    print(f"{len(lines)} grilles : {with_result} avec un résultat, "
          f"{len(lines) - with_result} sans")
    for kind, count in kinds.most_common():
        print(f"    {kind:12s} {count}")
    print(f"restes mesurés : {doc.get('crafting_remainder', {})}")
    print(f"écrit {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
