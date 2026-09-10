#!/usr/bin/env python3
"""Relève l'agriculture sur un vrai serveur 1.20.1 : hydratation, feuilles,
croissance des cultures, poudre d'os.

Le serveur est piloté par sa console (fifo) et la sauvegarde relue par
`ov_inspect state`, comme `measure_fluids.py`. Une différence de taille : les
random ticks n'ont lieu **que** dans un chunk à moins de 128 blocs d'un joueur
qui n'est pas spectateur. Un serveur sans joueur ne fait rien pousser, et lit
« rien n'a bougé » partout — exactement ce que donnerait une vitesse nulle. La
campagne garde donc un client connecté (`keepalive_client.py`) planté au
spawn, et tout est construit autour de lui.

Le temps est compté en ticks du jeu, pas en secondes : la vitesse est posée à
K puis remise à 0 **par la console**, et la console exécute ses commandes au
début d'un tick, dans l'ordre. `time query gametime` avant et après donne le
nombre exact de ticks pendant lesquels la vitesse valait K.

Usage :
    python3 scripts/measure_agriculture.py hydration
    python3 scripts/measure_agriculture.py leaves
    python3 scripts/measure_agriculture.py growth  K T
    python3 scripts/measure_agriculture.py bonemeal
Chaque campagne écrit son JSON dans run/agri-oracle/<nom>.json.
"""
import json
import os
import re
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LAB = os.environ.get("OV_AGRI_LAB", os.path.join(REPO, "run", "agri-oracle"))
FIFO = os.path.join(LAB, "console.fifo")
LOG = os.path.join(LAB, "server.log")
WORLD = os.path.join(LAB, "world", "region")
INSPECT = os.path.join(REPO, "build/macos-debug/bin/ov_inspect")
PACK = os.path.join(REPO, "data/vanilla/1.20.1/registry.ovpack")

GROUND = -61     # sommet de l'herbe d'un monde plat par défaut
FARM = -61       # la couche de terre labourée remplace l'herbe


def run(*lines):
    with open(FIFO, "w") as fifo:
        fifo.write("".join(line + "\n" for line in lines))


def log_size():
    return os.path.getsize(LOG)


def wait_log(pattern, since, timeout=30.0):
    """La première ligne du journal postérieure à `since` qui correspond."""
    deadline = time.time() + timeout
    rx = re.compile(pattern)
    while time.time() < deadline:
        with open(LOG, "rb") as f:
            f.seek(since)
            for line in f.read().decode(errors="replace").splitlines():
                m = rx.search(line)
                if m:
                    return m
        time.sleep(0.2)
    raise SystemExit(f"le journal n'a jamais montré {pattern!r}")


def gametime():
    since = log_size()
    run("time query gametime")
    return int(wait_log(r"The time is (\d+)", since).group(1))


def save():
    since = log_size()
    run("save-all flush")
    wait_log(r"Saved the game", since, timeout=60)


def states(positions):
    text = "\n".join(f"{x} {y} {z}" for x, y, z in positions)
    out = subprocess.run([INSPECT, "state", WORLD, f"--pack={PACK}"],
                         input=text, capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit(f"ov_inspect a échoué : {out.stderr}")
    # Une ligne par position : « x y z état », ou « x y z - » hors chunk.
    return [line.split(" ", 3)[3] for line in out.stdout.splitlines()]


def prop(state, name):
    m = re.search(rf"[\[,]{name}=([^,\]]+)", state)
    return m.group(1) if m else None


def setup():
    run("gamerule randomTickSpeed 0", "gamerule doDaylightCycle false",
        "gamerule doWeatherCycle false", "weather clear 1000000",
        "gamerule doFireTick false", "gamerule doMobSpawning false",
        "gamerule doTileDrops false", "difficulty peaceful", "time set noon",
        "forceload add -64 -64 63 63")
    time.sleep(1.0)


def clear(x0, z0, x1, z1):
    """Remet une zone à plat : herbe au niveau du sol, air au-dessus.

    Par bandes de 16 en z. `/fill` refuse **en silence** au-delà de 32 768
    blocs — un carré de 121 x 121 sur quatre couches en fait 58 564 — et la
    zone gardait alors tout ce que les campagnes précédentes y avaient posé :
    c'est ce qu'a montré la première mesure de poudre sur l'herbe, pleine de
    distributeurs et d'eau d'une autre campagne."""
    cmds = []
    for zs in range(z0, z1 + 1, 16):
        ze = min(z1, zs + 15)
        cmds += [f"fill {x0} {GROUND+1} {zs} {x1} {GROUND+4} {ze} minecraft:air",
                 f"fill {x0} {GROUND} {zs} {x1} {GROUND} {ze} minecraft:grass_block",
                 f"fill {x0} {GROUND-1} {zs} {x1} {GROUND-1} {ze} minecraft:dirt"]
    return cmds


def batched(cmds, pause=0.05, every=40):
    for i in range(0, len(cmds), every):
        run(*cmds[i:i + every])
        time.sleep(pause)


def random_ticks_for(ticks, speed):
    """Pose la vitesse pendant `ticks` ticks mesurés, puis la remet à zéro."""
    t0 = gametime()
    run(f"gamerule randomTickSpeed {speed}")
    # Le compte exact est relu, pas supposé : on dort à peu près, puis on lit.
    time.sleep(ticks / 20.0)
    since = log_size()
    run("gamerule randomTickSpeed 0", "time query gametime")
    t1 = int(wait_log(r"The time is (\d+)", since).group(1))
    # La commande de vitesse et la requête partent dans le même tick ; la
    # première requête était dans un tick antérieur à la pose de K.
    return t1 - t0 - 1


# ── Hydratation ─────────────────────────────────────────────────────────────
# Quatre parcelles de 13 x 13 de terre labourée (moisture=0), l'eau au centre à
# dy = -1, 0, +1, +2 de la couche labourée. Au premier random tick, une cellule
# qui voit l'eau passe à 7, une qui ne la voit pas et n'a pas de culture
# redevient de la terre. La carte est lue après assez de ticks pour que chaque
# cellule en ait reçu plusieurs.

HYD_DY = (-1, 0, 1, 2)


def hydration():
    setup()
    plots = []
    cmds = []
    for i, dy in enumerate(HYD_DY):
        ox, oz = -40 + i * 24, -40
        plots.append((dy, ox, oz))
        cmds += clear(ox - 8, oz - 8, ox + 8, oz + 8)
        cmds.append(f"fill {ox-6} {FARM} {oz-6} {ox+6} {FARM} {oz+6} minecraft:farmland")
        wy = FARM + dy
        if dy == -1:
            cmds.append(f"setblock {ox} {wy} {oz} minecraft:water")
        elif dy == 0:
            cmds.append(f"setblock {ox} {wy} {oz} minecraft:water")
        else:
            # Une colonne de verre sous l'eau, et quatre verres autour d'elle :
            # l'eau ne coule pas, et les cinq cellules couvertes sont exclues.
            for yy in range(FARM, wy):
                cmds.append(f"setblock {ox} {yy} {oz} minecraft:glass")
            for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                for yy in range(FARM + 1, wy + 1):
                    cmds.append(f"setblock {ox+dx} {yy} {oz+dz} minecraft:glass")
            cmds.append(f"setblock {ox} {wy} {oz} minecraft:water")
    batched(cmds)
    time.sleep(2.0)
    ticks = random_ticks_for(400, 100)
    save()
    out = {"speed": 100, "ticks": ticks, "plots": {}}
    for dy, ox, oz in plots:
        pos = [(ox + dx, FARM, oz + dz) for dz in range(-6, 7) for dx in range(-6, 7)]
        got = states(pos)
        rows = []
        for dz in range(-6, 7):
            row = ""
            for dx in range(-6, 7):
                s = got[(dz + 6) * 13 + (dx + 6)]
                if s.startswith("minecraft:farmland"):
                    row += prop(s, "moisture")
                elif s.startswith("minecraft:dirt"):
                    row += "d"
                elif s.startswith("minecraft:water"):
                    row += "w"
                elif s.startswith("minecraft:grass_block"):
                    # Redevenue terre, puis reprise par l'herbe voisine : la
                    # vitesse 100 fait aussi s'étendre l'herbe.
                    row += "g"
                else:
                    row += "x"
            rows.append(row)
        out["plots"][str(dy)] = rows
    return out


# ── Feuilles ────────────────────────────────────────────────────────────────
# Des feuilles posées **une par une** par `setblock`, du tronc vers l'extérieur :
# `setblock` recalcule la forme du bloc posé à partir de ses voisins, donc chaque
# feuille reçoit la distance de celle qui la précède. Les distances sont lues
# une première fois random ticks coupés (ce que la propagation a donné), puis
# une seconde après les random ticks (ce qui est tombé).

LEAF_Y = GROUND + 2


def leaves():
    setup()
    cmds = []
    rigs = {}

    def line(name, ox, oz, source, kinds, length=10, persistent="false"):
        cmds.extend(clear(ox - 2, oz - 2, ox + length + 2, oz + 2))
        cmds.append(f"setblock {ox} {LEAF_Y} {oz} {source}")
        cells = []
        for i in range(1, length + 1):
            kind = kinds[(i - 1) % len(kinds)]
            cmds.append(f"setblock {ox+i} {LEAF_Y} {oz} minecraft:{kind}[persistent={persistent}]")
            cells.append((ox + i, LEAF_Y, oz))
        rigs[name] = cells

    oz = -60
    line("oak_log", -60, oz, "minecraft:oak_log", ["oak_leaves"]); oz += 6
    line("mixed_types", -60, oz, "minecraft:oak_log",
         ["oak_leaves", "birch_leaves", "spruce_leaves", "jungle_leaves", "acacia_leaves",
          "dark_oak_leaves", "azalea_leaves", "mangrove_leaves", "cherry_leaves",
          "flowering_azalea_leaves"]); oz += 6
    line("stripped_log", -60, oz, "minecraft:stripped_oak_log", ["oak_leaves"]); oz += 6
    line("oak_wood", -60, oz, "minecraft:oak_wood", ["oak_leaves"]); oz += 6
    line("crimson_stem", -60, oz, "minecraft:crimson_stem", ["oak_leaves"]); oz += 6
    line("mangrove_roots", -60, oz, "minecraft:mangrove_roots", ["oak_leaves"]); oz += 6
    line("oak_planks", -60, oz, "minecraft:oak_planks", ["oak_leaves"]); oz += 6
    line("persistent_free", -60, oz, "minecraft:stone", ["oak_leaves"], length=4,
         persistent="true"); oz += 6
    # Une diagonale seule : la feuille touche le tronc par un coin et rien d'autre.
    cmds.extend(clear(-60 - 2, oz - 2, -60 + 4, oz + 4))
    cmds.append(f"setblock -60 {LEAF_Y} {oz} minecraft:oak_log")
    cmds.append(f"setblock -59 {LEAF_Y} {oz+1} minecraft:oak_leaves[persistent=false]")
    cmds.append(f"setblock -59 {LEAF_Y+1} {oz+1} minecraft:oak_leaves[persistent=false]")
    rigs["diagonal"] = [(-59, LEAF_Y, oz + 1), (-59, LEAF_Y + 1, oz + 1)]
    oz += 6

    # Une nappe de 17 x 17 x 1 autour d'un tronc : chemin libre, la distance est
    # donc Manhattan, et la frontière 6/7 est un losange exact.
    cx, cz = -20, -40
    cmds.extend(clear(cx - 10, cz - 10, cx + 10, cz + 10))
    cmds.append(f"setblock {cx} {LEAF_Y} {cz} minecraft:oak_log")
    sheet = []
    for r in range(1, 9):
        for dz in range(-r, r + 1):
            for dx in range(-r, r + 1):
                if max(abs(dx), abs(dz)) != r:
                    continue
                cmds.append(f"setblock {cx+dx} {LEAF_Y} {cz+dz} minecraft:oak_leaves[persistent=false]")
                sheet.append((cx + dx, LEAF_Y, cz + dz))
    rigs["sheet"] = sheet

    batched(cmds, pause=0.1)
    time.sleep(3.0)
    save()
    before = {name: states(cells) for name, cells in rigs.items()}
    ticks = random_ticks_for(600, 200)
    time.sleep(1.0)
    save()
    after = {name: states(cells) for name, cells in rigs.items()}
    out = {"speed": 200, "ticks": ticks, "rigs": {}}
    for name, cells in rigs.items():
        out["rigs"][name] = [
            {"pos": list(c), "before": b, "after": a}
            for c, b, a in zip(cells, before[name], after[name])
        ]
    return out


# ── Croissance ──────────────────────────────────────────────────────────────
# Tuiles de 9 x 9 de terre labourée, l'eau au centre dans la couche labourée.
#   A : blé épars, cultures aux décalages impairs (16 par tuile) — aucun blé
#       parmi les huit voisins, voisins humides : points 9,25 ou 10 → p = 1/3.
#   C : blé dense, toute la tuile — diagonales identiques, points / 2 → p = 1/6.
#   B : betterave éparse, même disposition que A.
#   R : blé en rangées sur terre **sèche**, rangs alternés blé / carottes —
#       voisins du même blé seulement est-ouest : points 4 → p = 1/7.
# Les tuiles sèches sont à plus de 4 blocs de toute eau.

SPARSE_CROP = {"A": "wheat", "B": "beetroots", "T": "torchflower_crop", "P": "pitcher_crop",
               "M": "melon_stem"}


def growth(speed, ticks_wanted):
    setup()
    cmds = []
    cells = {"A": [], "C": [], "B": [], "R": [], "T": [], "P": [], "M": [],
             "N": [], "S": [], "O": [], "U": [], "K": []}

    def tile(kind, ox, oz):
        cmds.append(f"fill {ox-4} {FARM} {oz-4} {ox+4} {FARM} {oz+4} minecraft:farmland[moisture=7]")
        cmds.append(f"fill {ox-4} {FARM+1} {oz-4} {ox+4} {FARM+3} {oz+4} minecraft:air")
        cmds.append(f"setblock {ox} {FARM} {oz} minecraft:water")
        for dz in range(-4, 5):
            for dx in range(-4, 5):
                if dx == 0 and dz == 0:
                    continue
                if kind != "C":
                    if dx % 2 == 0 or dz % 2 == 0 or abs(dx) > 3 or abs(dz) > 3:
                        continue
                crop = SPARSE_CROP.get(kind, "wheat")
                cmds.append(f"setblock {ox+dx} {FARM+1} {oz+dz} minecraft:{crop}")
                cells[kind].append((ox + dx, FARM + 1, oz + dz))

    def rows():
        """Les plantes qui ne vivent pas sur la terre labourée, une par deux blocs."""
        for x in range(-58, 59, 2):
            # verrue du Nether sur sable des âmes
            cmds.append(f"setblock {x} {FARM} 32 minecraft:soul_sand")
            cmds.append(f"setblock {x} {FARM+1} 32 minecraft:nether_wart")
            cells["N"].append((x, FARM + 1, 32))
            # baies sucrées sur l'herbe
            cmds.append(f"setblock {x} {FARM+1} 36 minecraft:sweet_berry_bush")
            cells["S"].append((x, FARM + 1, 36))
            # cacao sur le flanc nord d'une bûche d'acajou, tourné vers elle
            cmds.append(f"setblock {x} {FARM+1} 40 minecraft:jungle_log")
            cmds.append(f"setblock {x} {FARM+1} 39 minecraft:cocoa[facing=south]")
            cells["O"].append((x, FARM + 1, 39))
            # canne à sucre sur du sable, l'eau dans le sol juste au sud
            cmds.append(f"setblock {x} {FARM} 44 minecraft:sand")
            cmds.append(f"setblock {x} {FARM} 45 minecraft:water")
            cmds.append(f"setblock {x} {FARM+1} 44 minecraft:sugar_cane")
            cells["U"].append((x, FARM + 1, 44))
            # cactus sur du sable, rien autour
            cmds.append(f"setblock {x} {FARM} 50 minecraft:sand")
            cmds.append(f"setblock {x} {FARM+1} 50 minecraft:cactus")
            cells["K"].append((x, FARM + 1, 50))

    def dry(ox, oz, w=12):
        cmds.append(f"fill {ox} {FARM} {oz} {ox+w-1} {FARM} {oz+w-1} minecraft:farmland[moisture=0]")
        cmds.append(f"fill {ox} {FARM+1} {oz} {ox+w-1} {FARM+3} {oz+w-1} minecraft:air")
        for dz in range(w):
            for dx in range(w):
                crop = "wheat" if dz % 2 == 0 else "carrots"
                cmds.append(f"setblock {ox+dx} {FARM+1} {oz+dz} minecraft:{crop}")
                # Seul l'intérieur est compté : un bord a des voisins en terre
                # nue qui redeviennent de l'herbe, ce qui change ses points.
                if crop == "wheat" and 0 < dx < w - 1 and 0 < dz < w - 1:
                    cells["R"].append((ox + dx, FARM + 1, oz + dz))

    cmds += clear(-60, -60, 60, 60)
    kinds = ["A", "C", "B"]
    for j in range(4):
        for i in range(9):
            tile(kinds[i % 3], -58 + i * 9 + 4, -58 + j * 9 + 4)
    # Une cinquième rangée de tuiles humides : torchflower, pitcher, tige.
    for i in range(9):
        tile(["T", "P", "M"][i % 3], -58 + i * 9 + 4, -58 + 4 * 9 + 4)
    for j in range(2):
        for i in range(4):
            dry(-58 + i * 14, 0 + j * 14)
    rows()
    batched(cmds, pause=0.1)
    time.sleep(3.0)
    ticks = random_ticks_for(ticks_wanted, speed)
    time.sleep(1.0)
    save()
    out = {"speed": speed, "ticks": ticks, "ages": {}, "states": {}}
    for kind, cs in cells.items():
        got = states(cs)
        out["ages"][kind] = [int(prop(s, "age")) if prop(s, "age") is not None else -1
                             for s in got]
        # L'état complet aussi : un pitcher a une moitié haute, une tige
        # devient `attached_*`, un torchflower mûr n'a plus d'âge du tout.
        out["states"][kind] = got
    # Les plantes qui poussent en hauteur : la colonne entière, trois blocs
    # au-dessus du premier, et les blocs posés à côté des tiges.
    for kind in ("U", "K", "P"):
        col = [(x, y + dy, z) for (x, y, z) in cells[kind] for dy in (1, 2, 3)]
        out["states"][kind + "_above"] = states(col)
    side = [(x + dx, y, z + dz) for (x, y, z) in cells["M"]
            for dx, dz in ((0, -1), (1, 0), (0, 1), (-1, 0))]
    out["states"]["M_side"] = states(side)
    return out


# ── Fonte ───────────────────────────────────────────────────────────────────
# La glace et la neige fondent à la lumière **de bloc**, et le seuil est lu
# dans la lumière que le jeu a lui-même écrite dans la cellule, pas déduit de la
# source : un bloc `light[level=N]` invisible et sans collision est posé contre
# chaque échantillon. Huit gréements, N = 8..15, espacés de 20 blocs pour que la
# lumière de l'un n'atteigne pas l'autre. Autour de chaque source : de la glace
# posée sur de la pierre, de la glace posée au-dessus du vide, et une couche de
# neige.

MELT_Y = GROUND + 3


def melt():
    setup()
    cmds = clear(-80, -12, 80, 12)
    samples = []
    for k, level in enumerate(range(8, 16)):
        ox = -70 + k * 20
        cmds.append(f"fill {ox-3} {GROUND+1} -3 {ox+3} {GROUND+6} 3 minecraft:air")
        cmds.append(f"setblock {ox} {MELT_Y} 0 minecraft:light[level={level}]")
        # glace sur pierre, à l'est
        cmds.append(f"setblock {ox+1} {MELT_Y-1} 0 minecraft:stone")
        cmds.append(f"setblock {ox+1} {MELT_Y} 0 minecraft:ice")
        samples.append(("ice_on_stone", level, (ox + 1, MELT_Y, 0)))
        # glace au-dessus du vide, à l'ouest
        cmds.append(f"setblock {ox-1} {MELT_Y} 0 minecraft:ice")
        samples.append(("ice_on_air", level, (ox - 1, MELT_Y, 0)))
        # neige, au nord, posée sur de la pierre
        cmds.append(f"setblock {ox} {MELT_Y-1} -1 minecraft:stone")
        cmds.append(f"setblock {ox} {MELT_Y} -1 minecraft:snow")
        samples.append(("snow", level, (ox, MELT_Y, -1)))
        # glace et neige à deux blocs, au sud : un cran de lumière plus bas
        cmds.append(f"setblock {ox} {MELT_Y-1} 2 minecraft:stone")
        cmds.append(f"setblock {ox} {MELT_Y} 2 minecraft:ice")
        samples.append(("ice_two_away", level, (ox, MELT_Y, 2)))
    batched(cmds, pause=0.1)
    time.sleep(3.0)
    save()
    positions = [p for _, _, p in samples]
    # La lumière de bloc stockée dans la cellule, avant toute fonte.
    light = subprocess.run([INSPECT, "light", WORLD],
                           input="\n".join(f"{x} {y} {z}" for x, y, z in positions),
                           capture_output=True, text=True).stdout.splitlines()
    before = states(positions)
    ticks = random_ticks_for(600, 200)
    time.sleep(1.0)
    save()
    after = states(positions)
    below = states([(x, y - 1, z) for x, y, z in positions])
    return {"speed": 200, "ticks": ticks,
            "samples": [{"kind": k, "source": lv, "pos": list(p), "light": li,
                         "before": b, "after": a, "below_after": bl}
                        for (k, lv, p), li, b, a, bl
                        in zip(samples, light, before, after, below)]}


# ── Poudre d'os ─────────────────────────────────────────────────────────────
# Un distributeur par plante, une poudre chacun, déclenché par un bloc de
# redstone posé **après** le distributeur (piège 9 du briefing : `setblock`
# notifie ses six voisins, le capteur doit donc être posé en dernier). Random
# ticks coupés : seul le distributeur fait bouger la plante.

def bonemeal():
    setup()
    cmds = clear(-60, -60, 60, 60)
    targets = {"wheat": [], "beetroots": [], "oak_sapling": [], "melon_stem": [],
               "sweet_berry_bush": [], "carrots_age6": [], "beetroots_b": [],
               "melon_stem_age5": []}
    rows = list(targets)
    for r, kind in enumerate(rows):
        z = -58 + r * 4
        for i in range(100):
            x = -58 + i
            if x > 58:
                break
            # plante en (x, z), distributeur en (x, z+1) tourné vers le nord
            if kind == "oak_sapling" or kind == "sweet_berry_bush":
                cmds.append(f"setblock {x} {GROUND+1} {z} minecraft:{kind}")
            else:
                cmds.append(f"setblock {x} {GROUND} {z} minecraft:farmland[moisture=7]")
                block = {"carrots_age6": "carrots[age=6]", "beetroots_b": "beetroots",
                         "melon_stem_age5": "melon_stem[age=5]"}.get(kind, kind)
                cmds.append(f"setblock {x} {GROUND+1} {z} minecraft:{block}")
            cmds.append(
                f"setblock {x} {GROUND+1} {z+1} minecraft:dispenser[facing=north]"
                "{Items:[{Slot:0b,id:\"minecraft:bone_meal\",Count:1b}]}")
            targets[kind].append((x, GROUND + 1, z))
    # De l'eau à portée de toutes les terres labourées : une rangée en z-2
    # derrière chaque ligne suffirait, mais les random ticks sont coupés et la
    # terre ne sèche qu'au random tick. Rien à faire.
    batched(cmds, pause=0.1)
    time.sleep(3.0)
    fire = []
    for kind, cs in targets.items():
        for (x, y, z) in cs:
            fire.append(f"setblock {x} {y} {z+2} minecraft:redstone_block")
    batched(fire, pause=0.1)
    time.sleep(3.0)
    save()
    out = {"results": {}}
    for kind, cs in targets.items():
        out["results"][kind] = states(cs)
    # Une tige menée à 7 par la poudre pose-t-elle son fruit sur-le-champ ?
    # Les quatre côtés de chaque tige d'âge 5 : le distributeur occupe le sud.
    out["results"]["melon_stem_age5_sides"] = states(
        [(x + dx, y, z + dz) for (x, y, z) in targets["melon_stem_age5"]
         for dx, dz in ((0, -1), (1, 0), (-1, 0))])
    return out


# ── La betterave, en grand ──────────────────────────────────────────────────
# Deux campagnes de 100 donnaient 207 betteraves avancées d'un cran sur 298 :
# 0,695, entre le 3/4 du wiki (z = -2,2) et 2/3 (z = +1,0). 1500 de plus
# séparent les deux hypothèses de six écarts-types.

def beetmeal():
    setup()
    cmds = clear(-60, -60, 60, 60)
    cells = []
    for r in range(15):
        z = -58 + r * 4
        for i in range(100):
            x = -58 + i
            cmds.append(f"setblock {x} {GROUND} {z} minecraft:farmland[moisture=7]")
            cmds.append(f"setblock {x} {GROUND+1} {z} minecraft:beetroots")
            cmds.append(
                f"setblock {x} {GROUND+1} {z+1} minecraft:dispenser[facing=north]"
                "{Items:[{Slot:0b,id:\"minecraft:bone_meal\",Count:1b}]}")
            cells.append((x, GROUND + 1, z))
    batched(cmds, pause=0.1)
    time.sleep(3.0)
    batched([f"setblock {x} {y} {z+2} minecraft:redstone_block" for x, y, z in cells], pause=0.1)
    time.sleep(3.0)
    save()
    got = states(cells)
    # Un distributeur qui n'a pas tiré laisse aussi l'âge 0, et son état
    # (`triggered=true`) ne dit pas s'il a tiré. Son inventaire, si : la
    # console le rend, une ligne par distributeur. Sans ce tri, 1 % de ratés
    # — c'est ce que montraient les rangées de blé, où l'âge 0 est impossible
    # après une poudre — se lit comme des betteraves qui n'ont pas poussé.
    full = []
    for (x, y, z) in cells:
        since = log_size()
        run(f"data get block {x} {y} {z+1}")
        m = wait_log(r"has the following block data: (.*)$", since)
        full.append("bone_meal" in m.group(1))
    return {"beetroots": got, "unfired": full}


# ── Poudre d'os sur l'herbe ─────────────────────────────────────────────────
# Un bloc d'herbe tous les 16 blocs, le distributeur **enfoncé dans le sol**
# juste au sud et tourné vers le nord : la cible est le bloc d'herbe lui-même,
# avec de l'air au-dessus — la condition pour que la poudre y prenne. Après une
# poudre, on relève tout ce qui a poussé dans un carré de 15 autour.

def grassmeal():
    setup()
    cmds = clear(-60, -60, 60, 60)
    centres = []
    for gz in range(-56, 57, 16):
        for gx in range(-56, 57, 16):
            cmds.append(f"setblock {gx} {GROUND} {gz+1} minecraft:dispenser[facing=north]"
                        "{Items:[{Slot:0b,id:\"minecraft:bone_meal\",Count:1b}]}")
            centres.append((gx, GROUND, gz))
    batched(cmds, pause=0.1)
    time.sleep(3.0)
    batched([f"setblock {x} {y} {z+2} minecraft:redstone_block" for x, y, z in centres],
            pause=0.1)
    time.sleep(3.0)
    save()
    out = {"centres": centres, "patches": []}
    for (x, y, z) in centres:
        cells = [(x + dx, y + 1, z + dz) for dz in range(-7, 8) for dx in range(-7, 8)]
        got = states(cells)
        found = [{"dx": c[0] - x, "dz": c[2] - z, "state": s}
                 for c, s in zip(cells, got) if s != "minecraft:air"]
        out["patches"].append(found)
    return out


# ── Analyse ─────────────────────────────────────────────────────────────────
# Le modèle : chaque tick, K positions tirées par section avec remise, donc un
# bloc reçoit Binomiale(K·T, 1/4096) random ticks, et chacun le fait pousser
# avec la probabilité p tant qu'il n'est pas mûr. L'âge final est donc
# min(max, Binomiale(K·T, p/4096)). On compare l'histogramme observé à ce
# modèle par un χ² (cases regroupées jusqu'à un effectif attendu ≥ 5), pour le
# p attendu **et** pour un p faux — le témoin qui prouve que la mesure
# distingue quelque chose.

from math import comb, exp, lgamma, log


def binom_capped(n, q, cap):
    pmf = []
    for k in range(cap):
        pmf.append(exp(lgamma(n + 1) - lgamma(k + 1) - lgamma(n - k + 1)
                       + k * log(q) + (n - k) * log(1 - q)))
    pmf.append(max(0.0, 1.0 - sum(pmf)))
    return pmf


def chi2_sf(x, df):
    """P(χ²_df ≥ x), par la fonction gamma incomplète régularisée (série)."""
    if x <= 0:
        return 1.0
    a, z = df / 2.0, x / 2.0
    if z < a + 1:
        term = total = 1.0 / a
        n = 1
        while term > total * 1e-12:
            term *= z / (a + n)
            total += term
            n += 1
        return max(0.0, 1.0 - total * exp(-z + a * log(z) - lgamma(a)))
    # fraction continue de Lentz pour la queue
    b = z + 1 - a
    c = 1e300
    d = 1 / b
    h = d
    for i in range(1, 300):
        an = -i * (i - a)
        b += 2
        d = an * d + b
        d = 1e-300 if abs(d) < 1e-300 else d
        c = b + an / c
        c = 1e-300 if abs(c) < 1e-300 else c
        d = 1 / d
        h *= d * c
    return exp(-z + a * log(z) - lgamma(a)) * h


def chi2(observed, expected_p):
    total = sum(observed)
    exp_counts = [p * total for p in expected_p]
    # regroupe les cases jusqu'à un attendu ≥ 5, de gauche à droite
    groups_o, groups_e, o_acc, e_acc = [], [], 0, 0.0
    for o, e in zip(observed, exp_counts):
        o_acc += o
        e_acc += e
        if e_acc >= 5:
            groups_o.append(o_acc); groups_e.append(e_acc); o_acc, e_acc = 0, 0.0
    if e_acc > 0 or o_acc > 0:
        if groups_e:
            groups_o[-1] += o_acc; groups_e[-1] += e_acc
        else:
            groups_o.append(o_acc); groups_e.append(e_acc)
    stat = sum((o - e) ** 2 / e for o, e in zip(groups_o, groups_e) if e > 0)
    df = max(1, len(groups_o) - 1)
    return stat, df, chi2_sf(stat, df)


# groupe → (âge maximal, p attendu, p témoin, ce qui les distingue)
MODELS = {
    "A": (7, 1 / 3, 1 / 6, "blé épars humide : 1/3 ; témoin 1/6 (comme s'il était dense)"),
    "C": (7, 1 / 6, 1 / 3, "blé dense humide : 1/6 ; témoin 1/3 (sans la division par deux)"),
    "R": (7, 1 / 7, 1 / 13, "blé en rangs, sec : 1/7 ; témoin 1/13 (voisins ignorés)"),
    "B": (3, 2 / 9, 1 / 3, "betterave : 2/3 × 1/3 ; témoin 1/3 (sans le tirage 1 sur 3)"),
    # Supposé d'abord comme la betterave (2/9) : rejeté, p = 0,008. La mesure
    # dit la règle ordinaire des cultures, sans tirage 1 sur 3.
    "T": (2, 1 / 3, 2 / 9, "torchflower : 1/3 (âge 2 = la fleur) ; témoin 2/9 (la betterave)"),
    "P": (4, 1 / 3, 2 / 9, "pitcher : 1/3 ; témoin 2/9"),
    "N": (3, 1 / 10, 1 / 5, "verrue : 1/10 ; témoin 1/5"),
    "S": (3, 1 / 5, 1 / 10, "baies : 1/5 ; témoin 1/10"),
    "O": (2, 1 / 5, 1 / 10, "cacao : 1/5 ; témoin 1/10"),
}


def ages_of(data, kind):
    ages = list(data["ages"][kind])
    if kind == "T":
        # Un torchflower mûr n'est plus une culture : c'est la fleur, âge 2.
        ages = [2 if s.startswith("minecraft:torchflower") and not
                s.startswith("minecraft:torchflower_crop") else a
                for s, a in zip(data["states"][kind], ages)]
    return ages


def analyse(paths):
    runs = [json.load(open(p)) for p in paths]
    print("runs:", ", ".join(f"K={r['speed']} T={r['ticks']}" for r in runs))
    results = {}
    for kind, (cap, p, witness, label) in MODELS.items():
        observed = [0] * (cap + 1)
        exp_right = [0.0] * (cap + 1)
        exp_wrong = [0.0] * (cap + 1)
        dropped = 0
        for r in runs:
            n = r["speed"] * r["ticks"]
            ages = [a for a in ages_of(r, kind)]
            valid = [a for a in ages if 0 <= a <= cap]
            dropped += len(ages) - len(valid)
            for a in valid:
                observed[a] += 1
            for k, v in enumerate(binom_capped(n, p / 4096, cap)):
                exp_right[k] += v * len(valid)
            for k, v in enumerate(binom_capped(n, witness / 4096, cap)):
                exp_wrong[k] += v * len(valid)
        total = sum(observed)
        right = chi2(observed, [e / total for e in exp_right])
        wrong = chi2(observed, [e / total for e in exp_wrong])
        results[kind] = {"observed": observed, "expected": [round(e, 1) for e in exp_right],
                         "chi2": right, "witness_chi2": wrong, "dropped": dropped}
        print(f"{kind}  {label}")
        print(f"   observé  {observed}   (hors culture : {dropped})")
        print(f"   attendu  {[round(e, 1) for e in exp_right]}")
        print(f"   χ²={right[0]:.2f} ddl={right[1]} p={right[2]:.3f}   "
              f"témoin χ²={wrong[0]:.1f} p={wrong[2]:.2g}")
    return results


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "hydration"
    if what == "analyse":
        analyse(sys.argv[2:])
        raise SystemExit(0)
    if what == "hydration":
        result = hydration()
    elif what == "leaves":
        result = leaves()
    elif what == "growth":
        result = growth(int(sys.argv[2]), int(sys.argv[3]))
    elif what == "bonemeal":
        result = bonemeal()
    elif what == "melt":
        result = melt()
    elif what == "grassmeal":
        result = grassmeal()
    elif what == "beetmeal":
        result = beetmeal()
    else:
        raise SystemExit(__doc__)
    suffix = "-".join(sys.argv[1:])
    path = os.path.join(LAB, f"{suffix}.json")
    with open(path, "w") as f:
        json.dump(result, f, indent=1)
    print(path)
