#!/usr/bin/env python3
"""Validation hors échantillon : un labyrinthe tiré au sort.

Les scénarios des passes 1 et 2 ont servi à **écrire** la règle ; les rejouer
ne prouve donc pas grand-chose de plus que la fidélité de la transcription. Ici
la géométrie est tirée d'un générateur pseudo-aléatoire à graine fixe, sans
qu'aucun cas n'ait été choisi pour être facile : murs épars, trous à des
distances variées, une marche. Le même terrain est rejoué à l'identique dans
`test_fluid.cpp`.

Sortie : un fichier C++ prêt à coller, et le relevé du serveur.
"""
import json
import os
import random
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fluid_lab import run, save, states, setup, LAB

BASE = 200 * 48
HALF = 9
GROUND = -61
AIR = -60

MAZES = []


def build(seed, wall_count, hole_count):
    rng = random.Random(seed)
    cells = [(x, z) for x in range(-HALF, HALF + 1) for z in range(-HALF, HALF + 1)
             if (x, z) != (0, 0)]
    rng.shuffle(cells)
    walls = cells[:wall_count]
    holes = cells[wall_count:wall_count + hole_count]
    return walls, holes


for index, (seed, walls_n, holes_n) in enumerate(
        [(1, 40, 2), (2, 55, 1), (3, 25, 4), (4, 70, 3)]):
    MAZES.append((f"maze_{seed}", seed) + build(seed, walls_n, holes_n))


def main(read_only=False):
    if read_only:
        return report()
    setup()
    for i in range(len(MAZES)):
        ox = BASE + i * 48
        run(f"forceload add {ox-16} -16 {ox+16} 16")
    time.sleep(3.0)

    for i, (name, _seed, walls, holes) in enumerate(MAZES):
        ox = BASE + i * 48
        run(f"fill {ox-HALF-1} {AIR} {-HALF-1} {ox+HALF+1} {AIR+5} {HALF+1} minecraft:air",
            f"fill {ox-HALF-1} {GROUND} {-HALF-1} {ox+HALF+1} {GROUND} {HALF+1} minecraft:stone")
        time.sleep(0.4)
        for x, z in walls:
            run(f"setblock {ox+x} {AIR} {z} minecraft:stone")
        for x, z in holes:
            run(f"setblock {ox+x} {GROUND} {z} minecraft:air")
        time.sleep(0.4)
        run(f"setblock {ox} {AIR} 0 minecraft:water")
        print("posé", name, flush=True)

    print("stabilisation 60 s …", flush=True)
    time.sleep(60.0)
    return report()


def report():
    save(settle=2.0)
    out = {}
    cpp = []
    for i, (name, seed, walls, holes) in enumerate(MAZES):
        ox = BASE + i * 48
        reads = [(ox + x, AIR, z) for z in range(-HALF, HALF + 1)
                 for x in range(-HALF, HALF + 1)]
        rows = states(reads)
        out[name] = {"walls": walls, "holes": holes, "states": rows}

        # Rendu en carte, comme les tests l'attendent.
        grid = []
        k = 0
        for _z in range(-HALF, HALF + 1):
            line = ""
            for _x in range(-HALF, HALF + 1):
                text = rows[k].split(" ", 3)[3]
                k += 1
                if text.startswith("minecraft:water"):
                    level = int(text.split("level=")[1].rstrip("]"))
                    line += "0123456789abcdef"[level]
                elif text == "minecraft:air":
                    line += "."
                else:
                    line += "#"
            grid.append(line)
        out[name]["map"] = grid

        cpp.append(f'    // {name}: seed {seed}, {len(walls)} murs, {len(holes)} trous')
        cpp.append(f'    static constexpr std::array<std::pair<i32, i32>, {len(walls)}> '
                   f'k{name.title().replace("_", "")}Walls{{{{')
        cpp.append("        " + ", ".join(f"{{{x}, {z}}}" for x, z in walls))
        cpp.append("    }};")
        cpp.append(f'    static constexpr std::array<std::pair<i32, i32>, {len(holes)}> '
                   f'k{name.title().replace("_", "")}Holes{{{{')
        cpp.append("        " + ", ".join(f"{{{x}, {z}}}" for x, z in holes))
        cpp.append("    }};")
        cpp.append(f'    static const std::vector<std::string> k{name.title().replace("_","")}Want{{')
        for row in grid:
            cpp.append(f'        "{row}",')
        cpp.append("    };")
        cpp.append("")

    with open(os.path.join(LAB, "maze_oracle.json"), "w") as f:
        json.dump(out, f, indent=1)
    with open(os.path.join(LAB, "maze_oracle.inc"), "w") as f:
        f.write("\n".join(cpp))
    for name in out:
        print("===", name)
        for row in out[name]["map"]:
            print("  ", row)
    return 0


if __name__ == "__main__":
    sys.exit(main(read_only="--read" in sys.argv))
