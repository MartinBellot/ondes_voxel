#!/usr/bin/env python3
"""Demander à un vrai serveur 1.20.1 comment un conteneur admet et rend un objet.

Trois questions que rien dans les rapports du data generator ne répond, parce
que l'accès par face est du code Java :

  furnace-faces  Un entonnoir posé **au-dessus**, **à côté** et **en dessous**
                 d'un four, un objet dans chacun, et on lit dans quelle case du
                 four il atterrit — et, pour celui du dessous, ce qu'il en
                 ressort. C'est la table d'accès par face, mesurée plutôt que
                 récitée.

  into-container Un distributeur et un dropper face à un coffre, la même pile
                 dans les deux, déclenchés. L'objet finit-il dans le coffre ou
                 par terre ? Le dropper est le témoin : il met dans le coffre,
                 c'est connu. La question porte sur le distributeur.

  shulker        Un entonnoir tente de pousser une shulker box dans une shulker
                 box. Passe ou ne passe pas.

⚠ Deux pièges généraux du dépôt s'appliquent ici :

  * `/setblock` ne prévient que six voisins (docs/provenance/redstone.md §1) :
    le levier se pose **en dernier** et **adjacent** ;
  * un entonnoir attend huit ticks entre deux transferts, donc une sonde qui
    regarde tout de suite ne voit rien. On laisse quarante ticks.

Usage :
    python3 scripts/measure_containers.py <scénario> [sortie.json]
    python3 scripts/measure_containers.py all

Écrit data/vanilla/1.20.1/normalized/containers_<scénario>.json.
"""
from __future__ import annotations

import json
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from measure_entities import Server  # noqa: E402

NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
RUN = ROOT / "run" / "containers-oracle"
PORT = 25613

# Superflat : socle à -64, herbe à -61. Le premier étage libre est -60.
Y = -60

# `data get block` répond « x a la valeur suivante : … ». Ce qui est utile est
# tout ce qui suit les deux-points.
VALUE = re.compile(r"has the following block data: (.*)$")
NOTHING = re.compile(r"(No block data|not a block entity|Found no elements)")


def block_data(server: Server, positions: list[tuple[int, int, int]]) -> list[str | None]:
    """`data get block` pour chaque position, en un seul aller-retour."""
    commands = [f"data get block {x} {y} {z}" for x, y, z in positions]
    answers: list[str | None] = []
    for line in server.batch(commands):
        found = VALUE.search(line)
        if found:
            answers.append(found.group(1))
        elif NOTHING.search(line):
            answers.append(None)
    return answers


def slot_of(data: str | None, item: str) -> int | None:
    """Dans quelle case de ce conteneur se trouve cet objet ?"""
    if not data:
        return None
    # Les entrées sont de la forme {Slot: 0b, id: "minecraft:x", Count: 1b}.
    for match in re.finditer(r"\{Slot:\s*(\d+)b,\s*id:\s*\"([^\"]+)\",\s*Count:\s*(\d+)b\}", data):
        if match.group(2) == item:
            return int(match.group(1))
    for match in re.finditer(r"\{Count:\s*(\d+)b,\s*Slot:\s*(\d+)b,\s*id:\s*\"([^\"]+)\"\}", data):
        if match.group(3) == item:
            return int(match.group(2))
    return None


def count_of(data: str | None, item: str) -> int:
    if not data:
        return 0
    total = 0
    for match in re.finditer(r"id:\s*\"([^\"]+)\",\s*Count:\s*(\d+)b", data):
        if match.group(1) == item:
            total += int(match.group(2))
    for match in re.finditer(r"Count:\s*(\d+)b,\s*Slot:\s*\d+b,\s*id:\s*\"([^\"]+)\"", data):
        if match.group(2) == item:
            total += int(match.group(1))
    return total


def clear_area(server: Server, x0: int, z0: int, x1: int, z1: int) -> None:
    server.batch([f"fill {x0} {Y - 1} {z0} {x1} {Y + 6} {z1} air",
                  f"fill {x0} {Y - 1} {z0} {x1} {Y - 1} {z1} stone",
                  "kill @e[type=item]"])


# ── furnace-faces ───────────────────────────────────────────────────────────

def furnace_faces(server: Server) -> dict:
    """Trois entonnoirs, trois faces, un objet chacun : où tombe-t-il ?

    Le montage, par cellule : un four, et **un** entonnoir tourné vers lui. Une
    cellule pour le dessus, une pour le côté, une pour le dessous. L'objet est
    mis dans l'entonnoir avec `item replace block`, et on lit le four.
    """
    cells = {
        # (x du four, face, position de l'entonnoir, orientation)
        "up":   (0,  (0, Y + 1, 0),  "down"),
        "side": (8,  (7, Y, 0),      "east"),
        "down": (16, (16, Y - 1, 0), "up"),
    }
    clear_area(server, -4, -4, 24, 8)

    commands = []
    for label, (fx, hopper, facing) in cells.items():
        commands.append(f"setblock {fx} {Y} 0 furnace[facing=north,lit=false]")
        commands.append(f"setblock {hopper[0]} {hopper[1]} {hopper[2]} "
                        f"hopper[facing={facing},enabled=true]")
    server.batch(commands)

    # Le combustible d'un four est du charbon, son entrée est du minerai : deux
    # objets différents pour que la case d'arrivée soit lisible sans ambiguïté.
    # Le même objet dans les trois entonnoirs, une campagne par objet.
    results: dict[str, dict] = {}
    for item in ("minecraft:coal", "minecraft:iron_ore"):
        commands = []
        for label, (fx, hopper, facing) in cells.items():
            commands.append(f"data merge block {fx} {Y} 0 {{Items:[]}}")
            commands.append(f"item replace block {hopper[0]} {hopper[1]} {hopper[2]} "
                            f"container.0 with {item} 1")
        server.batch(commands)
        # Huit ticks de recharge, plus de la marge : quarante ticks.
        time.sleep(3.0)
        answers = block_data(server, [(fx, Y, 0) for fx, _, _ in cells.values()])
        per_face = {}
        for label, data in zip(cells.keys(), answers):
            per_face[label] = slot_of(data, item)
        results[item] = per_face
        print(f"  {item:22s} " + "  ".join(f"{f}→{s}" for f, s in per_face.items()))

    # Et la sortie : un four dont la case 2 tient trois lingots et la case 1 du
    # charbon, un entonnoir en dessous. L'entonnoir est lu **lui-même** — pas un
    # coffre plus loin : un entonnoir tourné dans une direction où il n'y a rien
    # garde ce qu'il a tiré, et lire un coffre qu'il n'alimente pas rapporte
    # zéro pour un entonnoir qui a parfaitement fonctionné.
    clear_area(server, -4, -4, 24, 8)
    server.batch([
        f"setblock 0 {Y} 0 furnace[facing=north,lit=false]",
        f"setblock 0 {Y - 1} 0 hopper[facing=north,enabled=true]",
        f"item replace block 0 {Y} 0 container.2 with minecraft:iron_ingot 3",
        f"item replace block 0 {Y} 0 container.1 with minecraft:coal 2",
    ])
    time.sleep(5.0)
    below = block_data(server, [(0, Y - 1, 0), (0, Y, 0)])
    pulled = count_of(below[0], "minecraft:iron_ingot")
    fuel_pulled = count_of(below[0], "minecraft:coal")
    fuel_left = count_of(below[1], "minecraft:coal")
    print(f"  sortie par le dessous : {pulled} lingot(s) dans l'entonnoir, "
          f"{fuel_pulled} charbon(s) tiré(s), {fuel_left} charbon(s) restant(s) "
          f"dans la case combustible")

    return {
        "insert": results,
        "extract_below": {"iron_ingot_pulled": pulled, "coal_pulled": fuel_pulled,
                          "coal_left_in_fuel": fuel_left},
    }


# ── into-container ──────────────────────────────────────────────────────────

def into_container(server: Server) -> dict:
    """Un distributeur et un dropper face à un coffre. Qui remplit le coffre ?"""
    clear_area(server, -4, -4, 24, 8)
    # Deux cellules, vingt-quatre blocs d'écart : un objet éjecté vole, et une
    # cellule polluée par sa voisine se lit exactement comme une machine qui n'a
    # rien fait. C'est le même écart que la campagne du distributeur (§13).
    rigs = {"dispenser": 0, "dropper": 24}
    commands = []
    for kind, x in rigs.items():
        commands.append(f"setblock {x} {Y} 0 {kind}[facing=east,triggered=false]")
        commands.append(f"setblock {x + 1} {Y} 0 chest[facing=north,type=single]")
        commands.append(f"item replace block {x} {Y} 0 container.0 "
                        f"with minecraft:cobblestone 5")
    server.batch(commands)
    time.sleep(1.0)
    # Le levier **en dernier** et **adjacent** : `/setblock` ne prévient que six
    # voisins, et un déclencheur à deux blocs ne déclenche rien.
    server.batch([f"setblock {x} {Y + 1} 0 lever[face=floor,facing=north,powered=true]"
                  for x in rigs.values()])
    time.sleep(3.0)

    out = {}
    for kind, x in rigs.items():
        chest, machine = block_data(server, [(x + 1, Y, 0), (x, Y, 0)])
        in_chest = count_of(chest, "minecraft:cobblestone")
        left = count_of(machine, "minecraft:cobblestone")
        ground = server.batch([f"execute positioned {x + 1}.5 {Y} 0.5 "
                               f"run data get entity @e[type=item,limit=1,distance=..3] Item"])
        on_floor = any("cobblestone" in line for line in ground)
        out[kind] = {"in_chest": in_chest, "left_in_machine": left, "on_floor": on_floor}
        print(f"  {kind:10s} coffre {in_chest}, machine {left}, "
              f"au sol {'oui' if on_floor else 'non'}")
    return out


# ── shulker ─────────────────────────────────────────────────────────────────

def shulker(server: Server) -> dict:
    """Une shulker box entre-t-elle dans une shulker box ?"""
    clear_area(server, -4, -4, 24, 8)
    server.batch([
        f"setblock 0 {Y} 0 shulker_box[facing=up]",
        f"setblock 0 {Y + 1} 0 hopper[facing=down,enabled=true]",
        f"item replace block 0 {Y + 1} 0 container.0 with minecraft:red_shulker_box 1",
        f"item replace block 0 {Y + 1} 0 container.1 with minecraft:cobblestone 1",
    ])
    time.sleep(4.0)
    box, hopper = block_data(server, [(0, Y, 0), (0, Y + 1, 0)])
    result = {
        "shulker_in_box": count_of(box, "minecraft:red_shulker_box"),
        "cobblestone_in_box": count_of(box, "minecraft:cobblestone"),
        "shulker_left_in_hopper": count_of(hopper, "minecraft:red_shulker_box"),
    }
    print(f"  shulker dans la boîte : {result['shulker_in_box']}, "
          f"pavé dans la boîte : {result['cobblestone_in_box']}, "
          f"shulker resté dans l'entonnoir : {result['shulker_left_in_hopper']}")
    return result


SCENARIOS = {
    "furnace-faces": furnace_faces,
    "into-container": into_container,
    "shulker": shulker,
}


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] not in ({"all"} | set(SCENARIOS)):
        print(__doc__)
        return 2
    wanted = list(SCENARIOS) if sys.argv[1] == "all" else [sys.argv[1]]

    NORMALIZED.mkdir(parents=True, exist_ok=True)
    server = Server(RUN, PORT)
    try:
        server.batch([
            "gamerule doMobSpawning false",
            "gamerule randomTickSpeed 0",
            "gamerule doDaylightCycle false",
            "gamerule sendCommandFeedback true",
            "time set noon",
            # Sans un joueur, un serveur vanilla ne fait apparaître rien et lit
            # zéro partout — mais les entonnoirs, eux, tournent : ils sont des
            # block entities dans un chunk chargé par le spawn.
            "forceload add -32 -32 64 64",
        ])
        for name in wanted:
            print(f"\n── {name} ──")
            result = SCENARIOS[name](server)
            path = NORMALIZED / f"containers_{name.replace('-', '_')}.json"
            path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
            print(f"  → {path.relative_to(ROOT)}")
    finally:
        server.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
