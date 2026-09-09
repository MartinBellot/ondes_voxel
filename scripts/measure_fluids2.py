#!/usr/bin/env python3
"""Deuxième passe : la chute, le rayon exact de recherche, et la cobblestone.

Trois choses que la première passe n'a pas pu établir.

* La chute : le premier `fill` descendait sous le plancher du monde (-65) et
  échouait en bloc, donc le puits n'a jamais été creusé. Ici les plateformes
  sont **construites en l'air**, pas creusées.
* Le rayon : la passe 1 borne la recherche entre 4 (elle voit) et 6 (elle ne
  voit pas). Il faut le cas 5.
* La cobblestone : dans la passe 1 l'eau (5 ticks/pas) arrivait toujours avant
  que la lave (30 ticks/pas) ait eu le temps de couler, si bien que seule la
  **source** de lave se convertissait, en obsidienne. Pour voir une coulée de
  lave rencontrer l'eau il faut deux temps : laisser la lave s'étaler d'abord,
  poser l'eau ensuite.
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fluid_lab import run, save, states, setup, LAB

STRIDE = 48
BASE = 30 * STRIDE       # loin des bandes de la passe 1

TOP = -50                # plateforme haute
BOT = -56                # plateforme basse

results = {}


def forceloads(count):
    for i in range(count):
        ox = BASE + i * STRIDE
        run(f"forceload add {ox-16} -16 {ox+16} 16")


def main():
    setup()
    forceloads(8)
    time.sleep(3.0)

    # ── A. La chute dans un puits ──────────────────────────────────────────
    a = BASE + 0 * STRIDE
    run(f"fill {a-8} {BOT} -8 {a+8} {TOP+4} 8 minecraft:air",
        f"fill {a-8} {BOT} -8 {a+8} {BOT} 8 minecraft:stone",
        f"fill {a-8} {TOP} -8 {a+8} {TOP} 8 minecraft:stone",
        f"setblock {a+2} {TOP} 0 minecraft:air",
        f"setblock {a} {TOP+1} 0 minecraft:water")

    # ── B. Le trou à distance 5, plein est ─────────────────────────────────
    b = BASE + 1 * STRIDE
    run(f"fill {b-9} {-60} -9 {b+9} {-55} 9 minecraft:air",
        f"fill {b-9} {-61} -9 {b+9} {-61} 9 minecraft:stone",
        f"setblock {b+5} {-61} 0 minecraft:air",
        f"setblock {b} {-60} 0 minecraft:water")

    # ── C. Le trou à distance 5 en diagonale (3 est, 2 sud) ────────────────
    c = BASE + 2 * STRIDE
    run(f"fill {c-9} {-60} -9 {c+9} {-55} 9 minecraft:air",
        f"fill {c-9} {-61} -9 {c+9} {-61} 9 minecraft:stone",
        f"setblock {c+3} {-61} 2 minecraft:air",
        f"setblock {c} {-60} 0 minecraft:water")

    # ── D/E/F. La lave d'abord, l'eau ensuite ──────────────────────────────
    # D : l'eau arrive **par le côté** sur une coulée de lave installée.
    d = BASE + 3 * STRIDE
    run(f"fill {d-9} {-60} -9 {d+9} {-55} 9 minecraft:air",
        f"fill {d-9} {-61} -9 {d+9} {-61} 9 minecraft:stone",
        f"setblock {d} {-60} 0 minecraft:lava")
    # E : l'eau tombe **par-dessus** une coulée de lave installée.
    e = BASE + 4 * STRIDE
    run(f"fill {e-9} {-60} -9 {e+9} {-54} 9 minecraft:air",
        f"fill {e-9} {-61} -9 {e+9} {-61} 9 minecraft:stone",
        f"setblock {e} {-60} 0 minecraft:lava")
    # F : une coulée de lave qui **tombe** dans de l'eau posée après.
    f = BASE + 5 * STRIDE
    run(f"fill {f-9} {-60} -9 {f+9} {-52} 9 minecraft:air",
        f"fill {f-9} {-61} -9 {f+9} {-61} 9 minecraft:stone",
        f"fill {f-9} {-55} -9 {f+9} {-55} 9 minecraft:stone",
        f"setblock {f} {-55} 0 minecraft:air",
        f"setblock {f-2} {-54} 0 minecraft:lava")

    # ── G. L'éponge ────────────────────────────────────────────────────────
    g = BASE + 6 * STRIDE
    run(f"fill {g-9} {-60} -9 {g+9} {-55} 9 minecraft:air",
        f"fill {g-9} {-61} -9 {g+9} {-61} 9 minecraft:stone",
        f"setblock {g} {-60} 0 minecraft:water")

    print("phase 1 : 90 s pour que la lave s'installe …", flush=True)
    time.sleep(90.0)

    # Photo de la lave AVANT que l'eau n'arrive, pour savoir ce qu'elle
    # rencontre exactement.
    save(settle=1.0)
    before_d = states([(d + dx, -60, 0) for dx in range(-4, 5)])
    before_f = states([(f - 2, y, 0) for y in (-54, -55, -56, -57, -58, -59, -60)])
    results["lave_avant_eau_laterale"] = before_d
    results["lave_avant_eau_chute"] = before_f

    # Phase 2 : l'eau.
    run(f"setblock {d+4} {-60} 0 minecraft:water")           # par le côté
    run(f"setblock {e+2} {-56} 0 minecraft:water")           # au-dessus, tombe
    run(f"setblock {f} {-54} 0 minecraft:water")             # dans la chute
    run(f"setblock {g+2} {-60} 0 minecraft:sponge")          # l'éponge
    print("phase 2 : 60 s …", flush=True)
    time.sleep(60.0)
    save(settle=2.0)

    results["chute_puits"] = states(
        [(a + dx, y, dz) for y in range(TOP + 1, BOT - 1, -1)
         for dz in range(-4, 5) for dx in range(-2, 7)])
    results["trou_distance_5_est"] = states(
        [(b + dx, -60, dz) for dz in range(-7, 8) for dx in range(-7, 8)])
    results["trou_distance_5_diagonale"] = states(
        [(c + dx, -60, dz) for dz in range(-7, 8) for dx in range(-7, 8)])
    results["lave_installee_puis_eau_laterale"] = states(
        [(d + dx, -60, dz) for dz in range(-4, 5) for dx in range(-6, 7)])
    results["lave_installee_puis_eau_dessus"] = states(
        [(e + dx, y, dz) for y in (-60, -59, -58, -57, -56)
         for dz in range(-3, 4) for dx in range(-4, 5)])
    results["lave_chute_puis_eau"] = states(
        [(f + dx, y, dz) for y in (-54, -55, -56, -57, -58, -59, -60)
         for dz in range(-3, 4) for dx in range(-4, 5)])
    results["eponge"] = states(
        [(g + dx, -60, dz) for dz in range(-9, 10) for dx in range(-9, 10)])
    results["eponge_bloc"] = states([(g + 2, -60, 0)])

    out = os.path.join(LAB, "fluid_oracle2.json")
    with open(out, "w") as fh:
        json.dump(results, fh, indent=1)
    for k, v in results.items():
        print(f"{k}: {len(v)}")
    print("écrit", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
