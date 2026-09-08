#!/usr/bin/env python3
"""À quels blocs une clôture s'accroche-t-elle, état par état et face par face.

C'est le prédicat qui manque pour les connexions : une clôture, une vitre ou
des barreaux se rattachent au voisin s'il leur présente une face pleine. Rien
dans les rapports officiels ne le dit — c'est la forme de collision, donc du
code Java.

Il ne peut pas être relevé par bloc. Une dalle basse n'offre pas de face pleine
à l'est, la même dalle en `type=double` si ; un escalier dépend de son
orientation. Une table par bloc serait fausse exactement là où ça se verrait.

Le montage : une clôture au centre d'une cellule, le **même état** sur ses
quatre côtés. Les quatre booléens de la clôture donnent alors, d'un coup, les
faces nord, sud, est et ouest de cet état. Les voisins sont relus eux aussi :
un bloc qui n'a pas tenu — une torche sans mur — doit être écarté et non
enregistré comme « n'accroche pas ».

Usage : python3 scripts/measure_sturdy.py <fifo> <monde> <sortie.json> [début] [fin]
"""
import json
import os
import subprocess
import sys
import time

NORMALIZED = "data/vanilla/1.20.1/normalized"
INSPECT = "build/macos-debug/bin/ov_inspect"

# Les blocs qui ne restent pas où on les met, ou qui réécrivent leurs voisins.
DYNAMIC = {"minecraft:water", "minecraft:lava", "minecraft:fire", "minecraft:soul_fire",
           "minecraft:bubble_column", "minecraft:moving_piston", "minecraft:nether_portal",
           "minecraft:end_portal", "minecraft:end_gateway",
           "minecraft:air", "minecraft:cave_air", "minecraft:void_air"}

# La cellule : la clôture au centre, les quatre voisins autour.
STRIDE = 4
COLUMNS = 96
Y = -60


def state_text(block: dict, offset: int) -> str:
    """`minecraft:oak_stairs[facing=north,half=top,...]` pour un état donné."""
    if not block["properties"]:
        return block["name"]
    parts = []
    for prop in block["properties"]:
        index = (offset // prop["stride"]) % len(prop["values"])
        parts.append(f"{prop['name']}={prop['values'][index]}")
    return f"{block['name']}[{','.join(parts)}]"


def main() -> int:
    fifo_path, world_dir, out_path = sys.argv[1:4]
    first = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    last = int(sys.argv[5]) if len(sys.argv) > 5 else 1 << 30

    with open(f"{NORMALIZED}/blocks.json") as f:
        blocks = json.load(f)["blocks"]

    cells = []
    for block in blocks:
        if block["name"] in DYNAMIC:
            continue
        for offset in range(block["state_count"]):
            cells.append((block["base_state"] + offset, state_text(block, offset)))
    cells = cells[first:last]
    print(f"{len(cells)} états à relever", flush=True)

    def run(*lines):
        with open(fifo_path, "w") as fifo:
            fifo.write("".join(line + "\n" for line in lines))

    def place(index):
        x = (index % COLUMNS) * STRIDE
        z = (index // COLUMNS) * STRIDE
        return x, z

    # Le terrain d'abord, en une passe : chaque cellule a besoin d'un sol.
    width = min(len(cells), COLUMNS) * STRIDE
    depth = ((len(cells) - 1) // COLUMNS + 1) * STRIDE
    # Sans cela, les feuillages se décomposent et les pousses grandissent
    # pendant le relevé, et le même état répond deux fois différemment.
    run("gamerule randomTickSpeed 0", "gamerule doFireTick false",
        "gamerule doDaylightCycle false", "difficulty peaceful")
    time.sleep(1.0)
    for z0 in range(-4, depth + 4, 64):
        run(f"forceload add -4 {z0} {width + 4} {min(z0 + 63, depth + 4)}")
    time.sleep(6.0)
    for z0 in range(-4, depth + 4, 16):
        run(f"fill -4 {Y - 1} {z0} {width + 4} {Y - 1} {min(z0 + 15, depth + 4)} minecraft:stone",
            f"fill -4 {Y} {z0} {width + 4} {Y + 2} {min(z0 + 15, depth + 4)} minecraft:air")
        time.sleep(0.08)
    time.sleep(3.0)

    batch = []
    for index, (_state, text) in enumerate(cells):
        x, z = place(index)
        batch.append(f"setblock {x} {Y} {z} minecraft:oak_fence replace")
        for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            batch.append(f"setblock {x + dx} {Y} {z + dz} {text} replace")
        if len(batch) >= 500:
            run(*batch)
            batch.clear()
            time.sleep(0.25)
            if index % 2000 < 5:
                print(f"  {index}/{len(cells)}", flush=True)
    if batch:
        run(*batch)
    time.sleep(2.0)
    run("save-all flush")
    time.sleep(20.0)

    queries = []
    for index in range(len(cells)):
        x, z = place(index)
        queries.append(f"{x} {Y} {z}")
        for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            queries.append(f"{x + dx} {Y} {z + dz}")
    output = subprocess.run([INSPECT, "state", world_dir], input="\n".join(queries),
                            capture_output=True, text=True, check=True).stdout.splitlines()

    # Chaque état qu'on a su écrire, retrouvé depuis son texte : le jeu ajuste
    # souvent le voisin qu'on vient de poser — une clôture se connecte, un
    # escalier change de forme, un feuillage recalcule sa distance — et la
    # réponse porte alors sur l'état **observé**, pas sur celui demandé.
    by_text = {}
    for block in blocks:
        for offset in range(block["state_count"]):
            by_text[state_text(block, offset)] = block["base_state"] + offset

    sides_by_state: dict[int, dict[str, bool]] = {}
    contradictions = 0
    unknown = 0

    for index in range(len(cells)):
        rows = output[index * 5:(index + 1) * 5]
        if len(rows) < 5:
            break
        parts = rows[0].split(maxsplit=3)
        fence = parts[3] if len(parts) > 3 else "-"
        if not fence.startswith("minecraft:oak_fence["):
            continue
        properties = dict(pair.split("=")
                          for pair in fence[len("minecraft:oak_fence["):-1].split(","))

        # Le voisin à l'est touche la clôture par sa face **ouest**.
        for row_index, (fence_side, neighbour_face) in enumerate(
                (("east", "west"), ("west", "east"), ("south", "north"), ("north", "south")), 1):
            columns = rows[row_index].split(maxsplit=3)
            if len(columns) < 4:
                continue
            observed = columns[3]
            state_id = by_text.get(observed)
            if state_id is None:
                unknown += 1
                continue
            answer = properties.get(fence_side) == "true"
            entry = sides_by_state.setdefault(state_id, {})
            if neighbour_face in entry and entry[neighbour_face] != answer:
                contradictions += 1
            entry[neighbour_face] = answer

    result = {str(state): sides for state, sides in sorted(sides_by_state.items())}
    complete = sum(1 for sides in sides_by_state.values() if len(sides) == 4)
    refused = len(cells) - len(result)
    print(f"{len(result)} états touchés, {complete} avec leurs quatre faces, "
          f"{contradictions} contradictions, {unknown} états inconnus")

    with open(out_path, "w") as f:
        json.dump({"$comment": "Mesure : une cloture au centre, le meme etat sur les quatre cotes ; "
                               "les booleens de la cloture donnent les faces de l'etat observe. "
                               "Voir docs/PROVENANCE.md.",
                   "measured": len(result), "complete": complete,
                   "contradictions": contradictions, "states": result}, f)
    return 0


if __name__ == "__main__":
    sys.exit(main())
