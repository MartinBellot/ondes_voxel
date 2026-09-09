#!/usr/bin/env python3
"""Relève l'écoulement des fluides sur un vrai serveur 1.20.1.

Chaque scénario a sa propre bande de terrain le long de X, assez large pour que
l'eau d'un scénario n'atteigne jamais le suivant (l'eau porte à 7, la lave à 3 ;
40 blocs de pas suffisent largement).

Tout est posé par `setblock` / `fill` : pas de client, donc pas de portée de six
blocs, et un scénario qui se rejoue à l'identique.
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fluid_lab import run, save, states, setup, LAB

STRIDE = 48          # pas entre deux scénarios, en blocs
GROUND = -61         # sommet du sol de pierre
AIR = -60            # première couche d'air


def plate(ox, half=10, y=GROUND):
    """Une dalle de pierre pleine, et l'air au-dessus."""
    return [f"fill {ox-half} {y+1} {-half} {ox+half} {y+6} {half} minecraft:air",
            f"fill {ox-half} {y} {-half} {ox+half} {y} {half} minecraft:stone",
            f"fill {ox-half} {y-1} {-half} {ox+half} {y-1} {half} minecraft:stone"]


def grid(ox, half, y=AIR):
    return [(ox + dx, y, dz) for dz in range(-half, half + 1)
            for dx in range(-half, half + 1)]


SCENARIOS = []


def scenario(name, build, reads):
    SCENARIOS.append((name, build, reads))


# ── 1. La flaque : une source d'eau lâchée sur un sol plat ──────────────────
ox = 0 * STRIDE
scenario("flaque_eau_sol_plat",
         plate(ox) + [f"setblock {ox} {AIR} 0 minecraft:water"],
         grid(ox, 9))

# ── 2. Le trou droit devant, à distance 4 ───────────────────────────────────
# Le sol est percé en (+4, 0). L'eau doit filer vers le trou au lieu de faire
# un disque.
ox = 1 * STRIDE
scenario("trou_a_l_est_distance_4",
         plate(ox) + [f"setblock {ox+4} {GROUND} 0 minecraft:air",
                      f"setblock {ox} {AIR} 0 minecraft:water"],
         grid(ox, 8))

# ── 3. Le trou derrière un obstacle ─────────────────────────────────────────
# Le trou est en (+2, 0) : deux blocs plein est. Un mur de deux blocs en x=+1,
# z=-1 et z=0, barre la route directe. Le seul chemin fait quatre pas en
# passant par le sud — dans la limite de 5. Le test est décisif : si la
# recherche existe, le **premier pas de l'eau est vers le sud**, à l'opposé de
# la direction du trou en ligne droite.
ox = 2 * STRIDE
scenario("trou_derriere_un_mur",
         plate(ox) + [f"fill {ox+1} {AIR} -1 {ox+1} {AIR} 0 minecraft:stone",
                      f"setblock {ox+2} {GROUND} 0 minecraft:air",
                      f"setblock {ox} {AIR} 0 minecraft:water"],
         grid(ox, 6))

# ── 4. Deux trous à égale distance ──────────────────────────────────────────
ox = 3 * STRIDE
scenario("deux_trous_equidistants",
         plate(ox) + [f"setblock {ox+3} {GROUND} 0 minecraft:air",
                      f"setblock {ox-3} {GROUND} 0 minecraft:air",
                      f"setblock {ox} {AIR} 0 minecraft:water"],
         grid(ox, 8))

# ── 5. Le trou hors de portée (distance 6) ──────────────────────────────────
# Le rayon de recherche est de 5 : à 6, l'eau ne doit plus le voir et doit
# refaire un disque.
ox = 4 * STRIDE
scenario("trou_hors_de_portee_6",
         plate(ox) + [f"setblock {ox+6} {GROUND} 0 minecraft:air",
                      f"setblock {ox} {AIR} 0 minecraft:water"],
         grid(ox, 8))

# ── 6. L'eau infinie : deux sources en ligne, un creux entre elles ──────────
ox = 5 * STRIDE
scenario("eau_infinie_deux_sources_alignees",
         plate(ox) + [f"fill {ox-1} {AIR} 0 {ox+1} {AIR} 0 minecraft:air",
                      f"setblock {ox-1} {AIR} 0 minecraft:water",
                      f"setblock {ox+1} {AIR} 0 minecraft:water"],
         [(ox + dx, AIR, dz) for dz in (-1, 0, 1) for dx in (-2, -1, 0, 1, 2)])

# ── 7. L'eau infinie refusée : deux sources en diagonale ────────────────────
ox = 6 * STRIDE
scenario("eau_infinie_refusee_diagonale",
         plate(ox) + [f"setblock {ox-1} {AIR} -1 minecraft:water",
                      f"setblock {ox+1} {AIR} 1 minecraft:water"],
         [(ox + dx, AIR, dz) for dz in (-1, 0, 1) for dx in (-1, 0, 1)])

# ── 8. La lave sur un sol plat (overworld) ──────────────────────────────────
ox = 7 * STRIDE
scenario("flaque_lave_sol_plat",
         plate(ox) + [f"setblock {ox} {AIR} 0 minecraft:lava"],
         grid(ox, 6))

# ── 9. La lave n'est pas infinie ────────────────────────────────────────────
ox = 8 * STRIDE
scenario("lave_pas_infinie",
         plate(ox) + [f"fill {ox-1} {AIR} 0 {ox+1} {AIR} 0 minecraft:air",
                      f"setblock {ox-1} {AIR} 0 minecraft:lava",
                      f"setblock {ox+1} {AIR} 0 minecraft:lava"],
         [(ox + dx, AIR, 0) for dx in (-2, -1, 0, 1, 2)])

# ── 10. La chute : une source au bord d'une corniche ────────────────────────
# Sol à GROUND, mais un puits de 4 de profondeur en (+3,0) : l'eau y tombe et
# se répand au fond.
ox = 9 * STRIDE
scenario("chute_dans_un_puits",
         plate(ox) + [f"fill {ox+1} {GROUND-4} -3 {ox+7} {GROUND} 3 minecraft:air",
                      f"fill {ox+1} {GROUND-5} -3 {ox+7} {GROUND-5} 3 minecraft:stone",
                      f"setblock {ox} {AIR} 0 minecraft:water"],
         [(ox + dx, y, dz) for y in (AIR, GROUND, GROUND-1, GROUND-2, GROUND-3, GROUND-4)
          for dz in range(-3, 4) for dx in range(-1, 8)])

# ── 11-17. Les mélanges lave / eau, un cas de géométrie par scénario ────────
def mix(name, ox, cmds, reads):
    scenario(name, plate(ox, half=6) + cmds, reads)


ox = 10 * STRIDE
mix("melange_eau_coule_sur_source_de_lave", ox,
    # Source de lave au sol ; source d'eau posée 3 blocs à l'est, elle coule
    # jusqu'à elle par le côté.
    [f"setblock {ox} {AIR} 0 minecraft:lava",
     f"setblock {ox+3} {AIR} 0 minecraft:water"],
    [(ox + dx, AIR, 0) for dx in range(-2, 5)])

ox = 11 * STRIDE
mix("melange_eau_au_dessus_source_de_lave", ox,
    # Source d'eau juste au-dessus d'une source de lave.
    [f"setblock {ox} {AIR} 0 minecraft:lava",
     f"setblock {ox} {AIR+1} 0 minecraft:water"],
    [(ox, AIR, 0), (ox, AIR + 1, 0)])

ox = 12 * STRIDE
mix("melange_eau_coule_sur_lave_coulante", ox,
    # Une coulée de lave (la source est en x-3) rencontrée par une coulée d'eau
    # venant de l'est.
    [f"setblock {ox-3} {AIR} 0 minecraft:lava",
     f"setblock {ox+3} {AIR} 0 minecraft:water"],
    [(ox + dx, AIR, 0) for dx in range(-4, 5)])

ox = 13 * STRIDE
mix("melange_lave_coule_sur_source_d_eau", ox,
    # L'inverse : la lave arrive sur une source d'eau immobile.
    [f"setblock {ox} {AIR} 0 minecraft:water",
     f"setblock {ox+3} {AIR} 0 minecraft:lava"],
    [(ox + dx, AIR, 0) for dx in range(-2, 5)])

ox = 14 * STRIDE
mix("melange_lave_tombe_dans_l_eau", ox,
    # La lave tombe d'une corniche dans une source d'eau au sol.
    [f"setblock {ox} {AIR} 0 minecraft:water",
     f"setblock {ox} {AIR+3} 0 minecraft:lava"],
    [(ox, y, 0) for y in (AIR, AIR + 1, AIR + 2, AIR + 3)])

ox = 15 * STRIDE
mix("melange_lave_source_eau_dessous", ox,
    # Source de lave posée directement sur une source d'eau.
    [f"setblock {ox} {AIR} 0 minecraft:water",
     f"setblock {ox} {AIR+1} 0 minecraft:lava"],
    [(ox, AIR, 0), (ox, AIR + 1, 0)])

ox = 16 * STRIDE
mix("melange_lave_coulante_eau_a_cote", ox,
    # Une coulée de lave qui longe une source d'eau posée sur le côté (en z).
    [f"setblock {ox-3} {AIR} 0 minecraft:lava",
     f"setblock {ox} {AIR} 2 minecraft:water"],
    [(ox + dx, AIR, dz) for dz in (0, 1, 2) for dx in range(-3, 3)])

# ── 18. Le waterlogging : une dalle gorgée d'eau alimente-t-elle ses voisins ?
ox = 17 * STRIDE
scenario("waterlogging_dalle_source",
         plate(ox, half=6) + [
             f"setblock {ox} {AIR} 0 minecraft:oak_slab[type=bottom,waterlogged=true]"],
         [(ox + dx, AIR, dz) for dz in (-1, 0, 1) for dx in (-2, -1, 0, 1, 2)])

# ── 19. Le waterlogging : l'eau qui coule entre-t-elle dans une dalle ? ─────
ox = 18 * STRIDE
scenario("waterlogging_eau_coulante_sur_dalle",
         plate(ox, half=6) + [
             f"setblock {ox+2} {AIR} 0 minecraft:oak_slab[type=bottom,waterlogged=false]",
             f"setblock {ox} {AIR} 0 minecraft:water"],
         [(ox + dx, AIR, 0) for dx in range(-2, 5)])

# ── 20. L'éponge ────────────────────────────────────────────────────────────
ox = 19 * STRIDE
scenario("eponge_dans_une_flaque",
         plate(ox, half=8) + [f"setblock {ox} {AIR} 0 minecraft:water"],
         [])   # rempli en deux temps ci-dessous


# ── 21. La lave dans le Nether : elle porte plus loin qu'en surface ─────────
# Jouée dans DIM-1 via `execute in`, sur une dalle construite pour l'occasion
# à y=32 (le Nether va de 0 à 255).
NETHER_Y = 32
NETHER_SCENARIOS = []
nox = 0
NETHER_SCENARIOS.append((
    "nether_flaque_lave",
    [f"execute in minecraft:the_nether run fill {nox-10} {NETHER_Y} -10 {nox+10} {NETHER_Y+5} 10"
     " minecraft:air",
     f"execute in minecraft:the_nether run fill {nox-10} {NETHER_Y-1} -10 {nox+10} {NETHER_Y-1} 10"
     " minecraft:netherrack",
     f"execute in minecraft:the_nether run setblock {nox} {NETHER_Y} 0 minecraft:lava"],
    [(nox + dx, NETHER_Y, dz) for dz in range(-6, 7) for dx in range(-6, 7)]))


def main():
    setup()
    # `forceload add` a une limite de 256 chunks par commande et échoue en
    # **silence** au-delà : un seul rectangle couvrant les vingt bandes en
    # demande 544, et les scénarios au-delà de la distance de vue ne tickent
    # jamais — la sauvegarde rend alors de l'air partout sans rien signaler.
    # Une commande par bande, 3x3 chunks chacune.
    for index in range(len(SCENARIOS)):
        ox = index * STRIDE
        run(f"forceload add {ox-16} -16 {ox+16} 16")
    time.sleep(3.0)

    run("execute in minecraft:the_nether run forceload add -16 -16 16 16")
    time.sleep(1.5)

    for name, build, _reads in SCENARIOS + NETHER_SCENARIOS:
        run(*build)
        time.sleep(0.6)
        print(f"  posé : {name}", flush=True)

    # L'eau met 5 ticks par pas et la lave 30 : 90 secondes couvrent largement
    # une coulée de 7 d'eau et de 3 de lave, mélanges compris.
    print("stabilisation 90 s …", flush=True)
    time.sleep(90.0)

    # L'éponge après coup, pour qu'elle absorbe une flaque déjà formée.
    ox = 19 * STRIDE
    run(f"setblock {ox+2} {AIR} 0 minecraft:sponge")
    time.sleep(5.0)

    save(settle=2.0)

    results = {}
    for name, _build, reads in SCENARIOS:
        if name == "eponge_dans_une_flaque":
            reads = grid(ox, 8)
        if not reads:
            continue
        rows = states(reads)
        results[name] = rows
        print(f"{name}: {len(rows)} positions", flush=True)

    from fluid_lab import NETHER
    for name, _build, reads in NETHER_SCENARIOS:
        rows = states(reads, world=NETHER)
        results[name] = rows
        print(f"{name}: {len(rows)} positions", flush=True)

    out = os.path.join(LAB, "fluid_oracle.json")
    with open(out, "w") as f:
        json.dump(results, f, indent=1)
    print("écrit", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
