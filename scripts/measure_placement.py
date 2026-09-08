#!/usr/bin/env python3
"""Relève l'état qu'un bloc prend quand un joueur le pose.

`setblock` place exactement l'état qu'on lui donne : il ne dit rien des
conventions. Ce qu'une dalle, un escalier ou une clôture décident au moment
d'être posés vit dans du code Java, et ne se lit qu'en posant vraiment — avec
un clic, une face, et un point de contact.

Le scénario est joué sur un vrai serveur 1.20.1, puis le monde est sauvegardé
et relu avec `ov-inspect state`, qui rend le nom **et toutes les propriétés**.

Usage : python3 scripts/measure_placement.py <fifo> <log> <monde> <sortie.json>
"""
import json
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vanilla_miner import Miner, block_pos, varint

PORT = 25566
INSPECT = "build/macos-debug/bin/ov_inspect"

# Chaque scénario pose une suite de blocs et relit une suite de positions.
# `clic` : (bloc tenu, x, y, z de la face cliquée, face, curseur x/y/z)
# Les faces sont numérotées -Y, +Y, -Z, +Z, -X, +X.
SCENARIOS = [
    ("dalle posée au sol", [("minecraft:oak_slab", 0, -61, 0, 1, 0.5, 1.0, 0.5)], [(0, -60, 0)]),
    ("dalle doublée", [("minecraft:oak_slab", 0, -61, 0, 1, 0.5, 1.0, 0.5),
                       ("minecraft:oak_slab", 0, -60, 0, 1, 0.5, 0.5, 0.5)], [(0, -60, 0)]),
    ("dalle sous un surplomb", [("minecraft:stone", 0, -61, 0, 1, 0.5, 1.0, 0.5),
                                ("minecraft:stone", 0, -60, 0, 1, 0.5, 1.0, 0.5),
                                ("minecraft:oak_slab", 0, -59, 0, 0, 0.5, 0.0, 0.5)],
     [(0, -60, 0)]),
    ("escalier seul", [("minecraft:oak_stairs", 0, -61, 0, 1, 0.5, 1.0, 0.5)], [(0, -60, 0)]),
    ("escalier en angle", [("minecraft:oak_stairs", 0, -61, 0, 1, 0.5, 1.0, 0.5),
                           ("minecraft:oak_stairs", 1, -61, 0, 1, 0.5, 1.0, 0.5)],
     [(0, -60, 0), (1, -60, 0)]),
    ("clôture seule", [("minecraft:oak_fence", 0, -61, 0, 1, 0.5, 1.0, 0.5)], [(0, -60, 0)]),
    ("deux clôtures", [("minecraft:oak_fence", 0, -61, 0, 1, 0.5, 1.0, 0.5),
                       ("minecraft:oak_fence", 1, -61, 0, 1, 0.5, 1.0, 0.5)],
     [(0, -60, 0), (1, -60, 0)]),
    ("clôture contre un bloc plein", [("minecraft:oak_fence", 0, -61, 0, 1, 0.5, 1.0, 0.5),
                                      ("minecraft:stone", 1, -61, 0, 1, 0.5, 1.0, 0.5)],
     [(0, -60, 0)]),
    ("deux vitres", [("minecraft:glass_pane", 0, -61, 0, 1, 0.5, 1.0, 0.5),
                     ("minecraft:glass_pane", 1, -61, 0, 1, 0.5, 1.0, 0.5)],
     [(0, -60, 0), (1, -60, 0)]),
    ("deux murets", [("minecraft:cobblestone_wall", 0, -61, 0, 1, 0.5, 1.0, 0.5),
                     ("minecraft:cobblestone_wall", 1, -61, 0, 1, 0.5, 1.0, 0.5)],
     [(0, -60, 0), (1, -60, 0)]),
    ("porte", [("minecraft:oak_door", 0, -61, 0, 1, 0.5, 1.0, 0.5)],
     [(0, -60, 0), (0, -59, 0)]),
    ("lit", [("minecraft:red_bed", 0, -61, 0, 1, 0.5, 1.0, 0.5)],
     [(0, -60, 0), (0, -60, -1), (0, -60, 1)]),
    ("torche murale", [("minecraft:stone", 0, -61, 0, 1, 0.5, 1.0, 0.5),
                       ("minecraft:torch", 0, -60, 0, 3, 0.5, 0.5, 1.0)], [(0, -60, 1)]),
    ("échelle", [("minecraft:stone", 0, -61, 0, 1, 0.5, 1.0, 0.5),
                 ("minecraft:ladder", 0, -60, 0, 3, 0.5, 0.5, 1.0)], [(0, -60, 1)]),
]


def main() -> int:
    fifo_path, log_path, world_dir, out_path = sys.argv[1:5]

    def run(*lines):
        with open(fifo_path, "w") as fifo:
            fifo.write("".join(line + "\n" for line in lines))

    with open("data/vanilla/1.20.1/normalized/registries.json") as f:
        items = json.load(f)["registries"]["minecraft:item"]["entries"]

    name = "Placer"
    miner = Miner(PORT, name)
    miner.pump(timeout=3.0)
    run(f"gamemode creative {name}", f"tp {name} 2.5 -60.0 2.5",
        "gamerule randomTickSpeed 0", "difficulty peaceful", "forceload add -16 -16 16 16")
    time.sleep(1.5)
    miner.pump(timeout=1.0)

    def hold(item):
        payload = struct.pack(">h", 36) + bytes([1]) + varint(items.index(item)) + \
            struct.pack(">b", 1) + bytes([0])
        miner.send(0x2B, payload)
        time.sleep(0.12)

    sequence = [1000]
    def click(x, y, z, face, cx, cy, cz):
        sequence[0] += 1
        miner.send(0x31, varint(0) + block_pos(x, y, z) + varint(face) +
                   struct.pack(">fff", cx, cy, cz) + bytes([0]) + varint(sequence[0]))
        time.sleep(0.18)

    # Chaque scénario a sa propre bande de terrain : joués au même endroit, ils
    # s'écraseraient l'un l'autre et une seule relecture ne verrait que le
    # dernier.
    queries = []
    for index, (label, steps, reads) in enumerate(SCENARIOS):
        shift = index * 8
        run(f"fill {shift - 2} -60 -2 {shift + 3} -57 3 minecraft:air replace",
            f"fill {shift - 2} -61 -2 {shift + 3} -61 3 minecraft:grass_block replace")
        time.sleep(0.35)
        miner.pump(timeout=0.3)
        # Assez près : au-delà de six blocs le serveur refuse le clic, et le
        # scénario rend un monde vide sans le dire.
        run(f"tp {name} {shift + 2.5} -60.0 2.5")
        time.sleep(0.35)
        miner.pump(timeout=0.35)
        miner.stand(shift + 2.5, -60.0, 2.5)
        for item, x, y, z, face, cx, cy, cz in steps:
            hold(item)
            click(x + shift, y, z, face, cx, cy, cz)
        queries.append((label, [(x + shift, y, z) for x, y, z in reads]))

    time.sleep(0.5)
    run("save-all flush")
    time.sleep(3.0)

    lines = "\n".join(f"{x} {y} {z}" for _label, reads in queries for x, y, z in reads)
    output = subprocess.run([INSPECT, "state", world_dir], input=lines,
                            capture_output=True, text=True, check=True).stdout.splitlines()

    results = {}
    index = 0
    for label, reads in queries:
        results[label] = [output[index + i] for i in range(len(reads))]
        index += len(reads)
    with open(out_path, "w") as f:
        json.dump(results, f, indent=1)
    for label, rows in results.items():
        print(f"{label}:")
        for row in rows:
            print(f"    {row}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
