#!/usr/bin/env python3
"""Combien de lumière chaque bloc émet.

Rien dans les rapports officiels ne le dit : c'est du code Java. Mais le jeu
écrit la lumière qu'il calcule dans chaque chunk qu'il sauvegarde, et la valeur
dans la case du bloc **est** son émission.

Le montage : une salle sans ciel, creusée sous terre, une cellule par bloc,
espacées de dix-sept — une source de quinze à seize cases ne contribue plus
rien, donc rien ne se mélange. Une tentative précédente les avait mises trop
près et la moitié des relevés portait sur la somme de deux sources.

L'émission est **par état** et pas par bloc : un minerai de redstone allumé,
une bougie selon leur nombre, un ancre de résurrection selon sa charge. Une
seconde passe (`--states`) donne une cellule à chaque état des blocs concernés,
ce qui ne fait que 479 états pour la soixantaine de familles qui varient.

Usage : python3 scripts/measure_light_emission.py <fifo> <monde> <sortie.json> [--states]
"""
import json
import os
import subprocess
import sys
import time

NORMALIZED = "data/vanilla/1.20.1/normalized"
INSPECT = "build/macos-debug/bin/ov_inspect"
SPACING = 17
COLUMNS = 32
Y = -40

# Les blocs qui ne restent pas où on les met, ou qui réécrivent leurs voisins.
DYNAMIC = {"minecraft:water", "minecraft:lava", "minecraft:fire", "minecraft:soul_fire",
           "minecraft:bubble_column", "minecraft:moving_piston", "minecraft:nether_portal",
           "minecraft:end_portal", "minecraft:end_gateway"}


def state_text(block: dict, offset: int) -> str:
    if not block["properties"]:
        return block["name"]
    parts = []
    for prop in block["properties"]:
        index = (offset // prop["stride"]) % len(prop["values"])
        parts.append(f"{prop['name']}={prop['values'][index]}")
    return f"{block['name']}[{','.join(parts)}]"


# Les propriétés qui peuvent allumer un bloc. Un bloc qui n'en porte aucune a la
# même émission dans tous ses états, et une seule cellule suffit.
SWITCHES = ("lit", "candles", "berries", "charges", "pickles", "level", "bloom")


def main() -> int:
    fifo_path, world_dir, out_path = sys.argv[1:4]
    per_state = "--states" in sys.argv[4:]

    with open(f"{NORMALIZED}/registries.json") as f:
        blocks = json.load(f)["registries"]["minecraft:block"]["entries"]

    if per_state:
        with open(f"{NORMALIZED}/blocks.json") as f:
            described = json.load(f)["blocks"]
        targets = []
        for block in described:
            if block["name"] in DYNAMIC:
                continue
            if not any(p["name"] in SWITCHES for p in block["properties"]):
                continue
            for offset in range(block["state_count"]):
                targets.append(state_text(block, offset))
    else:
        targets = [b for b in blocks if b not in DYNAMIC]

    def run(*lines):
        with open(fifo_path, "w") as fifo:
            fifo.write("".join(line + "\n" for line in lines))

    width = COLUMNS * SPACING
    depth = ((len(targets) - 1) // COLUMNS + 1) * SPACING
    run("gamerule randomTickSpeed 0", "gamerule doFireTick false", "difficulty peaceful")
    # forceload plafonne à 256 chunks par commande et fill à 32768 blocs. Les
    # dépasser échoue en silence, et le relevé rend alors une salle à moitié
    # creusée dont la moitié des cases n'existe pas.
    for z0 in range(-24, depth + 24, 128):
        for x0 in range(-24, width + 24, 128):
            run(f"forceload add {x0} {z0} {min(x0 + 127, width + 24)} "
                f"{min(z0 + 127, depth + 24)}")
            time.sleep(0.05)
    time.sleep(10.0)

    # Une salle pleine, puis creusée : sous terre et couverte, la lumière du
    # ciel n'entre pas, donc ce qu'on lit est uniquement ce que le bloc émet.
    # La coque déborde de vingt blocs de tous les côtés. Avec quatre, la
    # lumière du ciel entrait par la tranche et se propageait quinze cases vers
    # l'intérieur : les cellules de bordure lisaient la somme d'une émission et
    # d'un reste de jour, et cinquante-quatre blocs sortaient faux — dont la
    # torche des âmes, qui éclaire à dix et rendait zéro.
    for z0 in range(-20, depth + 20, 8):
        end = min(z0 + 7, depth + 20)
        for x0 in range(-20, width + 20, 256):
            x1 = min(x0 + 255, width + 20)
            run(f"fill {x0} {Y - 2} {z0} {x1} {Y + 4} {end} minecraft:stone",
                f"fill {x0} {Y} {z0} {x1} {Y + 2} {end} minecraft:air")
            time.sleep(0.1)
    time.sleep(6.0)

    batch = []
    for index, name in enumerate(targets):
        x = (index % COLUMNS) * SPACING
        z = (index // COLUMNS) * SPACING
        batch.append(f"setblock {x} {Y} {z} {name} replace")
        if len(batch) >= 400:
            run(*batch)
            batch.clear()
            time.sleep(0.2)
    if batch:
        run(*batch)
    time.sleep(3.0)
    run("save-all flush")
    time.sleep(45.0)

    # La case du bloc lui-même : la lumière qu'il émet y est écrite telle quelle.
    queries = "\n".join(f"{(i % COLUMNS) * SPACING} {Y} {(i // COLUMNS) * SPACING}"
                        for i in range(len(targets)))
    output = subprocess.run([INSPECT, "light", world_dir], input=queries,
                            capture_output=True, text=True, check=True).stdout.splitlines()

    emission = {}
    wrong_block = []
    for name, line in zip(targets, output):
        parts = line.split()
        if len(parts) < 6:
            continue
        found, block_light, sky = parts[3], parts[4], parts[5]
        # En mode par état on ne relit que le nom : le jeu ajuste souvent les
        # propriétés qu'on lui a demandées, et la lumière lue reste celle de ce
        # qui est effectivement là.
        if found != name.split("[")[0]:
            wrong_block.append(name)
            continue
        if sky not in ("0", "-1"):
            # Du ciel ici veut dire que la salle fuit, et la lecture ne porte
            # plus sur la seule émission.
            wrong_block.append(name)
            continue
        emission[name] = int(block_light)

    with open(out_path, "w") as f:
        json.dump({"$comment": "Mesure : lumiere lue dans la case du bloc, dans une salle sans "
                               "ciel, cellules espacees de 17. Voir docs/PROVENANCE.md.",
                   "measured": len(emission), "unusable": len(wrong_block),
                   "unusable_names": wrong_block,
                   "emission": emission}, f, indent=1)
    print(f"{len(emission)} blocs relevés, {len(wrong_block)} inutilisables")
    from collections import Counter
    print(sorted(Counter(emission.values()).items()))
    print("sources :", sorted((n.replace("minecraft:", ""), v)
                              for n, v in emission.items() if v > 0)[:20])
    return 0


if __name__ == "__main__":
    sys.exit(main())
