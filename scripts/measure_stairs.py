#!/usr/bin/env python3
"""Relève la forme que prend un escalier selon son voisin.

Un escalier a cinq formes — droite, deux coins rentrants, deux coins sortants —
et il en change tout seul quand un escalier voisin apparaît. La règle est du
code Java ; elle est ici mesurée exhaustivement, ce qui est possible parce que
l'espace est petit et fermé : quatre orientations, deux moitiés, quatre
directions de voisin, et de nouveau quatre orientations et deux moitiés, soit
512 cas.

Le montage : on pose l'escalier du centre, puis le voisin. C'est la pose du
voisin qui déclenche le recalcul, et on relit le centre.

Le troisième bloc compte aussi en vanilla — un escalier de l'autre côté empêche
le coin de se former — et cette passe ne le couvre pas. C'est dit plutôt que
supposé.

Usage : python3 scripts/measure_stairs.py <fifo> <monde> <sortie.json>
"""
import json
import os
import subprocess
import sys
import time

INSPECT = "build/macos-debug/bin/ov_inspect"
FACINGS = ("north", "south", "west", "east")
HALVES = ("top", "bottom")
STEP = {"north": (0, -1), "south": (0, 1), "west": (-1, 0), "east": (1, 0)}
STRIDE = 4
COLUMNS = 24
Y = -60


def main() -> int:
    fifo_path, world_dir, out_path = sys.argv[1:4]

    def run(*lines):
        with open(fifo_path, "w") as fifo:
            fifo.write("".join(line + "\n" for line in lines))

    cases = [(facing, half, side, other_facing, other_half)
             for facing in FACINGS for half in HALVES
             for side in FACINGS for other_facing in FACINGS for other_half in HALVES]

    width = COLUMNS * STRIDE
    depth = ((len(cases) - 1) // COLUMNS + 1) * STRIDE
    run("gamerule randomTickSpeed 0", "difficulty peaceful",
        f"forceload add -8 -8 {width + 8} {depth + 8}")
    time.sleep(4.0)
    for z0 in range(-4, depth + 8, 16):
        run(f"fill -4 {Y - 1} {z0} {width + 4} {Y - 1} {min(z0 + 15, depth + 8)} minecraft:stone",
            f"fill -4 {Y} {z0} {width + 4} {Y + 2} {min(z0 + 15, depth + 8)} minecraft:air")
        time.sleep(0.08)
    time.sleep(2.0)

    batch = []
    for index, (facing, half, side, other_facing, other_half) in enumerate(cases):
        x = (index % COLUMNS) * STRIDE
        z = (index // COLUMNS) * STRIDE
        dx, dz = STEP[side]
        # Le centre d'abord, le voisin ensuite : c'est le second qui déclenche
        # le recalcul du premier.
        batch.append(f"setblock {x} {Y} {z} minecraft:oak_stairs"
                     f"[facing={facing},half={half},shape=straight,waterlogged=false] replace")
        batch.append(f"setblock {x + dx} {Y} {z + dz} minecraft:oak_stairs"
                     f"[facing={other_facing},half={other_half},shape=straight,"
                     f"waterlogged=false] replace")
        if len(batch) >= 400:
            run(*batch)
            batch.clear()
            time.sleep(0.2)
    if batch:
        run(*batch)
    time.sleep(1.5)
    run("save-all flush")
    time.sleep(10.0)

    queries = "\n".join(f"{(i % COLUMNS) * STRIDE} {Y} {(i // COLUMNS) * STRIDE}"
                        for i in range(len(cases)))
    output = subprocess.run([INSPECT, "state", world_dir], input=queries,
                            capture_output=True, text=True, check=True).stdout.splitlines()

    results = {}
    unreadable = 0
    for case, line in zip(cases, output):
        parts = line.split(maxsplit=3)
        if len(parts) < 4 or not parts[3].startswith("minecraft:oak_stairs["):
            unreadable += 1
            continue
        properties = dict(pair.split("=")
                          for pair in parts[3][len("minecraft:oak_stairs["):-1].split(","))
        results["/".join(case)] = properties["shape"]

    with open(out_path, "w") as f:
        json.dump({"$comment": "Mesure : forme prise par un escalier selon un voisin escalier. "
                               "Cle : facing/half/cote/facing du voisin/half du voisin.",
                   "measured": len(results), "unreadable": unreadable, "shapes": results}, f,
                  indent=0)
    print(f"{len(results)}/{len(cases)} cas relevés, {unreadable} illisibles")
    from collections import Counter
    print(Counter(results.values()).most_common())
    return 0


if __name__ == "__main__":
    sys.exit(main())
