#!/usr/bin/env python3
"""Notre table d'instruments contre celle que le vrai serveur a écrite.

`scripts/measure_redstone.py noteblock` a posé un note block au-dessus de
chacun des 987 blocs du jeu et relu l'état de bloc du note block dans la
sauvegarde. Ce script compare ce fichier à la table compilée dans
`src/ov_gameplay/src/redstone.cpp`, bloc par bloc, et donne un chiffre.

Il compare les *listes*, pas le binaire : la table du C++ est une suite de
`kInstrument_<nom>` de littéraux, et c'est exactement ce qu'on veut vérifier —
qu'aucun nom n'a été perdu, dupliqué ni mal orthographié entre la mesure et le
code. Un bloc absent des deux côtés est `harp` par défaut, des deux côtés.

Usage : python3 scripts/check_noteblock.py
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MEASURED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized" / "redstone_noteblock.json"
SOURCE = ROOT / "src" / "ov_gameplay" / "src" / "redstone.cpp"

TABLE = re.compile(
    r"constexpr std::array<std::string_view, (\d+)> kInstrument_(\w+)\{(.*?)\n\};",
    re.DOTALL)


def compiled_table() -> dict[str, str]:
    text = SOURCE.read_text()
    out: dict[str, str] = {}
    for declared, instrument, body in TABLE.findall(text):
        names = re.findall(r'"([a-z_0-9]+)"', body)
        if len(names) != int(declared):
            print(f"  {instrument}: la taille déclarée est {declared} pour "
                  f"{len(names)} noms")
            sys.exit(1)
        for name in names:
            full = f"minecraft:{name}"
            if full in out:
                print(f"  {full} apparaît deux fois : {out[full]} et {instrument}")
                sys.exit(1)
            out[full] = instrument
    return out


def main() -> int:
    if not MEASURED.exists():
        print(f"mesure absente : {MEASURED}")
        print("lancer : python3 scripts/measure_redstone.py noteblock")
        return 2

    measured = json.load(MEASURED.open())["instrument"]
    ours = compiled_table()

    agree = 0
    wrong: list[tuple[str, str, str]] = []
    for block, expected in measured.items():
        got = ours.get(block, "harp")
        if got == expected:
            agree += 1
        else:
            wrong.append((block, expected, got))

    # Un nom de notre table que la mesure ne connaît pas : une faute de frappe,
    # ou un bloc d'une autre version. Les deux méritent d'être dits.
    unknown = sorted(set(ours) - set(measured))

    print(f"instruments : {agree}/{len(measured)} blocs d'accord")
    for block, expected, got in wrong[:20]:
        print(f"  {block}: mesuré {expected}, table {got}")
    if len(wrong) > 20:
        print(f"  … et {len(wrong) - 20} autres")
    for block in unknown:
        print(f"  {block}: dans la table, absent de la mesure")

    return 0 if not wrong and not unknown else 1


if __name__ == "__main__":
    raise SystemExit(main())
