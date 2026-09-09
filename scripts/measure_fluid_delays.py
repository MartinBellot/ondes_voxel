#!/usr/bin/env python3
"""Le délai exact d'un tick de fluide, lu dans la sauvegarde.

Pas de chronomètre : vanilla écrit dans le chunk le **délai restant** (`t`) de
chaque tick programmé. Une source posée puis sauvée dans la foulée n'a pas eu
le temps de s'écouler, donc son `t` est le délai complet — un entier exact,
pas une estimation.
"""
import glob
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fluid_lab import run, setup, LAB
import anvil_read as nbt

BASE = 80 * 48
NB = 8 * 48


def ticks_near(region_glob, x0, x1, dim):
    found = []
    for p in sorted(glob.glob(region_glob)):
        for _cx, _cz, c in nbt.chunks(p):
            for entry in (c.get("fluid_ticks") or []):
                if x0 <= entry["x"] <= x1:
                    found.append((dim, entry["i"], entry["t"], entry["p"],
                                  entry["x"], entry["y"], entry["z"]))
    return found


def main():
    setup()
    run(f"forceload add {BASE-16} -16 {BASE+16} 16",
        f"forceload add {BASE+NB-16} -16 {BASE+NB+16} 16",
        f"execute in minecraft:the_nether run forceload add {BASE-16} -16 {BASE+16} 16")
    time.sleep(2.5)
    run(f"fill {BASE-6} -60 -6 {BASE+6} -55 6 minecraft:air",
        f"fill {BASE-6} -61 -6 {BASE+6} -61 6 minecraft:stone",
        f"fill {BASE+NB-6} -60 -6 {BASE+NB+6} -55 6 minecraft:air",
        f"fill {BASE+NB-6} -61 -6 {BASE+NB+6} -61 6 minecraft:stone",
        f"execute in minecraft:the_nether run fill {BASE-6} 32 -6 {BASE+6} 37 6 minecraft:air",
        f"execute in minecraft:the_nether run fill {BASE-6} 31 -6 {BASE+6} 31 6"
        " minecraft:netherrack")
    time.sleep(2.0)

    # Les trois sources et la sauvegarde dans la même écriture : le serveur les
    # traite d'affilée, donc les ticks programmés sont encore intacts.
    run(f"setblock {BASE} -60 0 minecraft:water",
        f"setblock {BASE+NB} -60 0 minecraft:lava",
        f"execute in minecraft:the_nether run setblock {BASE} 32 0 minecraft:lava",
        "save-all flush")
    time.sleep(4.0)

    rows = ticks_near(os.path.join(LAB, "world/region/*.mca"), BASE - 8, BASE + NB + 8,
                      "overworld")
    rows += ticks_near(os.path.join(LAB, "world/DIM-1/region/*.mca"), BASE - 8, BASE + 8,
                       "nether")
    if not rows:
        print("aucun tick en attente — la sauvegarde est arrivée trop tard")
        return 1
    for r in sorted(rows):
        print(f"  {r[0]:9s} {r[1]:26s} t={r[2]:3d} p={r[3]}  ({r[4]},{r[5]},{r[6]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
