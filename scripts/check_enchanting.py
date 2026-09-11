#!/usr/bin/env python3
"""Lire la mesure de measure_enchanting.py et en tirer les chiffres.

Deux usages :

  * écrire `normalized/enchanting_anvil_panel.txt`, la table plate que le test
    `test_ov_server` [anvil][parity] rejoue — une ligne par combinaison, avec le
    coût et la pile de sortie de vanilla sous une forme canonique que le test
    C++ reproduit à l'identique ;
  * imprimer ce que les campagnes statistiques ont mesuré : la dégradation de
    l'enclume, la distribution de l'XP de la meule, Solidité, Raccommodage,
    Châtiment et compagnie, l'occultation des étagères, et le maximum de
    durabilité que chaque combinaison d'enclume « max_* » révèle.

Usage : python3 scripts/check_enchanting.py
"""
from __future__ import annotations

import json
import math
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = Path(os.environ.get("OV_NORMALIZED", ROOT / "data/vanilla/1.20.1/normalized"))


def canonical(value) -> str:
    """The exact string test_enchant_session.cpp builds from an nbt::Tag."""
    if isinstance(value, dict):
        return "{" + "".join(f"{k}:{canonical(value[k])}," for k in sorted(value)) + "}"
    if isinstance(value, list):
        return "[" + "".join(f"{canonical(v)}," for v in value) + "]"
    if isinstance(value, str):
        return f'"{value}"'
    if isinstance(value, float):
        return f"{value:g}"
    return str(int(value))


def output_of(stack) -> str:
    if not stack:
        return "-"
    return f"{stack['id']} {stack['count']} {canonical(stack['tag']) if stack['tag'] else '{}'}"


def binomial_z(k: int, n: int, p: float) -> float:
    if not n:
        return float("nan")
    variance = n * p * (1 - p)
    if variance == 0:
        # p is 0 or 1: the only possible count is n·p, exactly.
        return 0.0 if k == round(n * p) else float("inf")
    return (k - n * p) / math.sqrt(variance)


def main() -> int:
    path = NORMALIZED / "enchanting.json"
    if not path.is_file():
        print(f"{path} absent — lance scripts/measure_enchanting.py d'abord")
        return 1
    data = json.loads(path.read_text())

    anvil = data.get("anvil", {})
    panel = anvil.get("panel", {})
    if panel:
        rows = []
        for name, cell in sorted(panel.items()):
            rename = "\x01" if cell["rename"] is None else cell["rename"]
            rows.append("\t".join([name, cell["left"], cell["right"] or "-", rename,
                                   str(cell["cost"]), output_of(cell["output"])]))
        out = NORMALIZED / "enchanting_anvil_panel.txt"
        out.write_text("\n".join(rows) + "\n")
        print(f"enclume : {len(rows)} combinaisons -> {out}")
        print("  maximum de durabilité révélé par deux pièces au même Damage :")
        for name, cell in sorted(panel.items()):
            if not name.startswith("max_") or not cell["output"]:
                continue
            d = int(cell["left"].split("Damage:")[1].split("}")[0])
            got = cell["output"]["tag"].get("Damage", 0) if cell["output"]["tag"] else 0
            # got = max(0, max - (2*(max - d) + max*12//100)) → chercher max.
            found = [m for m in range(1, 3000)
                     if max(0, m - (2 * (m - d) + m * 12 // 100)) == got and m > d]
            print(f"    {name[4:]:28s} Damage {d:4d} -> {got:4d}   max ∈ {found[:3]}")
    if "degradation" in anvil:
        deg = anvil["degradation"]
        uses = sum(deg["uses"].values())
        falls = sum(deg["transitions"].values())
        print(f"  dégradation : {falls} sur {uses} usages = {falls / max(uses, 1):.3f} "
              f"(z = {binomial_z(falls, uses, 0.12):+.2f} contre 0,12)")
    for name, taken in sorted(anvil.get("taken", {}).items()):
        print(f"  pris {name}: coût {taken['cost']}, niveaux 50 -> {taken['levels_after']}")

    grind = data.get("grindstone", {})
    for name, cell in sorted(grind.items()):
        if not isinstance(cell, dict) or "xp" not in cell:
            continue
        xp = cell["xp"]
        span = f"{min(xp)}..{max(xp)}" if xp else "-"
        print(f"meule {name:28s} sortie {output_of(cell['output'])[:70]:70s} xp {span} (n={len(xp)})")

    unb = data.get("unbreaking", {})
    for level, cell in sorted(unb.get("tool", {}).items()):
        n, dmg = cell["uses"], cell["damage"] or 0
        p = 1 / (int(level) + 1)
        print(f"solidité outil {level}: {int(dmg)}/{n} = {dmg / max(n, 1):.3f} "
              f"attendu {p:.3f} (z = {binomial_z(int(dmg), n, p):+.2f})")
    for level, cell in sorted(unb.get("armour", {}).items()):
        n, dmg = cell["hits"], cell["damage"] or 0
        p = 0.6 + 0.4 / (int(level) + 1)
        print(f"solidité armure {level}: {int(dmg)}/{n} = {dmg / max(n, 1):.3f} "
              f"attendu {p:.3f} (z = {binomial_z(int(dmg), n, p):+.2f})")

    for orb in data.get("mending", {}).get("orbs", []):
        repaired = 200 - (orb["damage_after"] or 0)
        print(f"raccommodage orbe {orb['value']:3d}: réparé {repaired:5.0f}, xp restante {orb['xp']} "
              f"(attendu {min(orb['value'] * 2, 200)} et {orb['value'] - min(orb['value'] * 2, 200) // 2})")

    for name, cell in sorted(data.get("weapons", {}).items()):
        if "health_before" in cell and cell["health_before"] is not None:
            print(f"arme {name:26s} dégâts {cell['health_before'] - cell['health_after']:.3f}")

    obstruction = data.get("obstruction", {})
    if "cells" in obstruction:
        counted = sum(1 for c in obstruction["cells"] if c["verdict"] == "counted")
        blocked = sum(1 for c in obstruction["cells"] if c["verdict"] == "blocked")
        other = len(obstruction["cells"]) - counted - blocked
        print(f"étagères : {counted} comptées, {blocked} bloquées, {other} ambiguës "
              f"sur {len(obstruction['cells'])}")
        by_blocker: dict[str, set[str]] = {}
        for c in obstruction["cells"]:
            key = f"{c['blocker']}@{'même' if c['at'][1] == c['offset'][1] else 'autre'}"
            by_blocker.setdefault(key, set()).add(c["verdict"])
        for key, verdicts in sorted(by_blocker.items()):
            print(f"    {key:24s} {sorted(verdicts)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
