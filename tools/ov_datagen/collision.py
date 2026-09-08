"""Normalise les formes de collision, et en déduit les faces pleines.

Les formes viennent de PrismarineJS/minecraft-data (MIT), qui les publie par
état. Comme pour la dureté, cette source est prise pour **hypothèse** et non
pour vérité : elle s'est déjà trompée une fois sur ce projet.

Ici la vérification est indirecte et forte. Une face est « pleine » quand la
tranche de la forme sur cette face couvre le carré entier, et c'est ce prédicat
qui décide si une clôture s'accroche. Or ce dernier a été mesuré sur un vrai
serveur 1.20.1, face par face, sur 23 358 faces
(`scripts/measure_sturdy.py`) — et la règle reconstruite à partir de ces formes
les reproduit **toutes**. Une erreur dans les formes se verrait là.

Toutes les coordonnées sont des multiples de 1/32, ce qui les fait tenir sur un
octet chacune ; l'émetteur le vérifie plutôt que de le supposer.
"""
from __future__ import annotations

import json

UNITS = 32


def load(path: str) -> dict:
    with open(path) as f:
        return json.load(f)


def normalize(document: dict, blocks: list[dict]) -> dict:
    """Une table de formes, et un index de forme par état."""
    shapes = {int(key): value for key, value in document["shapes"].items()}
    by_block = document["blocks"]

    # Ré-indexer : les identifiants de la source sont épars, et un tableau plat
    # se lit avec une multiplication au lieu d'une recherche.
    order: dict[int, int] = {}
    table: list[list[list[int]]] = []
    for key in sorted(shapes):
        boxes = []
        for box in shapes[key]:
            scaled = []
            for value in box:
                units = value * UNITS
                if abs(units - round(units)) > 1e-9:
                    raise ValueError(f"la coordonnée {value} n'est pas un multiple de 1/{UNITS}")
                scaled.append(int(round(units)))
            boxes.append(scaled)
        order[key] = len(table)
        table.append(boxes)

    # Indexé par identifiant d'état, pas par ordre de parcours : `blocks.json`
    # est trié par nom et les identifiants ne le sont pas. Remplir séquentiellement
    # donne un tableau plausible et entièrement décalé.
    total = max(block["base_state"] + block["state_count"] for block in blocks)
    per_state: list[int] = [0] * total
    missing: list[str] = []
    for block in blocks:
        name = block["name"].split(":", 1)[1]
        entry = by_block.get(name)
        if entry is None:
            missing.append(block["name"])
            entry = 0
        for offset in range(block["state_count"]):
            index = entry[offset] if isinstance(entry, list) else entry
            per_state[block["base_state"] + offset] = order.get(index, order.get(0, 0))

    return {"shapes": table, "states": per_state, "missing": missing}


def face_is_full(boxes: list[list[int]], axis: int, at_max: bool) -> bool:
    """La tranche de la forme sur cette face couvre-t-elle le carré entier ?

    Sur une grille de 1/32 : une case compte quand une boîte la recouvre
    entièrement, ce qui est la question posée et non une approximation — les
    coordonnées sont toutes sur cette grille.
    """
    others = [i for i in range(3) if i != axis]
    covered = [[False] * UNITS for _ in range(UNITS)]
    for box in boxes:
        low, high = box[:3], box[3:]
        if at_max and high[axis] < UNITS:
            continue
        if not at_max and low[axis] > 0:
            continue
        # Une boîte peut déborder du cube — un piston étendu, une shulker box
        # ouverte — et ce qui déborde ne couvre rien de plus à l'intérieur.
        a0, a1 = max(0, low[others[0]]), min(UNITS, high[others[0]])
        b0, b1 = max(0, low[others[1]]), min(UNITS, high[others[1]])
        for i in range(a0, a1):
            for j in range(b0, b1):
                covered[i][j] = True
    return all(all(row) for row in covered)


# L'ordre des faces, partagé avec ov_registry : -Y, +Y, -Z, +Z, -X, +X, comme
# les faces d'un paquet de pose.
FACES = ((1, False), (1, True), (2, False), (2, True), (0, False), (0, True))


def sturdy_bits(boxes: list[list[int]]) -> int:
    bits = 0
    for index, (axis, at_max) in enumerate(FACES):
        if face_is_full(boxes, axis, at_max):
            bits |= 1 << index
    return bits
