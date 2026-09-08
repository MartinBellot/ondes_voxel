#!/usr/bin/env python3
"""Vérifie la durée de cassage de chaque bloc contre un vrai serveur 1.20.1.

La dureté n'est ni dans les rapports du data generator ni dans les datapacks :
c'est du code Java. PrismarineJS/minecraft-data (MIT) la publie, et c'est de là
que vient la table candidate — mais sur ce projet cette source s'est déjà
révélée fausse une fois, pour l'émission lumineuse, donnée par bloc alors
qu'elle est par état. Elle est donc prise comme hypothèse et confrontée au jeu,
bloc par bloc.

Le protocole exploite un détail du serveur : il ne casse pas le bloc tout seul.
Il attend que le client dise « j'ai fini », puis vérifie. Dire « fini » tout de
suite passe sous le seuil de 0,7 qui laisserait passer l'affirmation, et le
serveur bascule alors sur sa propre horloge : il casse le bloc au tick exact où
vanilla le casserait. C'est ce tick que l'on compte — le client n'a jamais eu
besoin de connaître la durée.

Deux conditions, choisies par bloc pour aller vite :
  * à main nue, où la vitesse vaut exactement 1 et le compte de ticks lit la
    dureté sans intervalle ;
  * avec l'outil correct en netherite quand la première dépasse vingt secondes,
    ce que fait l'obsidienne avec ses deux cent cinquante.

Usage : python3 scripts/measure_hardness.py <fifo> <sortie.json> [sondes]
"""
import json, math, os, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vanilla_miner import Miner
from measure_motion import SUBSTRATE

NORMALIZED = os.environ.get("OV_NORMALIZED", "data/vanilla/1.20.1/normalized")
PORT = 25565

TOOL_FOR_TAG = {
    "minecraft:mineable/pickaxe": "minecraft:netherite_pickaxe",
    "minecraft:mineable/axe": "minecraft:netherite_axe",
    "minecraft:mineable/shovel": "minecraft:netherite_shovel",
    "minecraft:mineable/hoe": "minecraft:netherite_hoe",
}

# Les vitesses vanilla par palier, à confirmer par la mesure plutôt qu'à croire.
NETHERITE_SPEED = 9.0


class Fifo:
    """La console du serveur, partagée par toutes les sondes."""

    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()

    def run(self, *lines):
        with self.lock, open(self.path, "w") as f:
            f.write("".join(line + "\n" for line in lines))


def tools_of(tags):
    """Quel outil rend chaque bloc rapide, d'après les tags du datapack."""
    out = {}
    for tag, tool in TOOL_FOR_TAG.items():
        for block in tags.get(tag, []):
            out[block] = tool
    return out


class Probe:
    """Une sonde : un joueur en survie, sa colonne d'essai, et rien d'autre."""

    def __init__(self, index, fifo, substrate):
        self.name = f"Hard{index}"
        self.fifo = fifo
        self.substrate = substrate
        # Près du spawn : à quelques centaines de blocs, un joueur téléporté ne
        # casse plus rien, et le silence se lit à tort comme « incassable ».
        self.x = index * 16
        self.block = (self.x + 2, -60, 0)
        self.miner = Miner(PORT, self.name)
        self.miner.pump(timeout=3.0)
        fifo.run(
            f"gamemode survival {self.name}",
            f"effect give {self.name} minecraft:saturation 999999 4 true",
            f"effect give {self.name} minecraft:resistance 999999 4 true",
            f"tp {self.name} {self.x + 0.5} -60.0 0.5",
        )
        time.sleep(1.0)
        self.miner.pump(timeout=1.0)

    def stand(self):
        self.miner.stand(self.x + 0.5, -60.0, 0.5)

    def place(self, block):
        x, y, z = self.block
        below = self.substrate.get(block, "minecraft:grass_block")
        self.fifo.run(
            f"setblock {x} {y - 1} {z} {below} replace",
            f"setblock {x + 1} {y} {z} minecraft:stone replace",
            f"setblock {x - 1} {y} {z} minecraft:stone replace",
            f"setblock {x} {y} {z + 1} minecraft:stone replace",
            f"setblock {x} {y} {z - 1} minecraft:stone replace",
            f"setblock {x} {y} {z} {block} replace",
        )

    def measure(self, block, tool, timeout):
        self.fifo.run(f"clear {self.name}", *( [f"give {self.name} {tool}"] if tool else []))
        self.place(block)
        time.sleep(0.55)
        self.miner.pump(timeout=0.35)
        self.stand()
        time.sleep(0.1)
        ticks = self.miner.mine_timed(*self.block, timeout=timeout)
        if ticks is None:
            # Un bloc qui n'a pas cédé laisse le serveur en attente de
            # destruction différée sur *ce* bloc, et il refusera alors tous les
            # suivants. Le faire disparaître libère l'état ; sans ça, un seul
            # bloc incassable fausse tout ce qui vient après.
            x, y, z = self.block
            self.fifo.run(f"setblock {x} {y} {z} minecraft:air replace")
            time.sleep(0.35)
            self.miner.pump(timeout=0.25)
        return ticks



# Vitesses mesurées, pas supposées : chacune est le seul entier compatible avec
# le nombre de ticks relevé sur de la pierre, dont la dureté vaut 1,5.
TIER_SPEED = {"wooden": 2.0, "stone": 4.0, "iron": 6.0, "diamond": 8.0, "netherite": 9.0}


def predict(hardness, requires_tool, speed, correct):
    """Le nombre de ticks que vanilla met à casser un bloc.

    `ceil(diviseur x dureté / vitesse)`, où le diviseur vaut 30 quand l'outil
    tenu permet la récolte et 100 sinon. Les deux valeurs ont été relevées sur
    le serveur, de même que le fait que la **famille** de l'outil compte et pas
    seulement son palier : une pelle en netherite sur de la pierre donne 150
    ticks, soit le diviseur 100, alors qu'une pioche en bois en donne 23.
    """
    divisor = 30 if (correct or not requires_tool) else 100
    return math.ceil(divisor * hardness / speed - 1e-9)


def check_block(probe, block, candidate, tool_of):
    """Confronte la table candidate au serveur, sur la condition la moins chère.

    À main nue la vitesse vaut exactement 1, donc le nombre de ticks lit la
    dureté sans intervalle — mais l'obsidienne y demande deux cent cinquante
    secondes. Au-delà de vingt secondes on repasse donc à l'outil correct, plus
    grossier mais toujours une prédiction exacte à vérifier.
    """
    entry = dict(candidate)
    correct = tool_of.get(block)
    hardness = candidate["hardness"]

    if hardness < 0:
        entry["expected"] = None
        entry["measured"] = probe.measure(block, correct, timeout=18.0)
        entry["ok"] = entry["measured"] is None
        return entry

    by_hand = predict(hardness, candidate["requires_tool"], 1.0, False)
    if by_hand <= 400 or correct is None:
        tool, expected = None, by_hand
        budget = 0.05 * (by_hand + 60)
    else:
        tool = correct
        expected = predict(hardness, candidate["requires_tool"], TIER_SPEED["netherite"], True)
        budget = 0.05 * (expected + 60)

    measured = probe.measure(block, tool, timeout=min(340.0, budget))
    entry["tool"] = tool
    entry["expected"] = expected
    entry["measured"] = measured
    # Un bloc cassé à l'instant même ne passe pas par l'horloge du serveur, et
    # le compte tombe à zéro ou à un selon la milliseconde. Au-delà, l'égalité
    # est stricte.
    entry["ok"] = measured is not None and (
        measured == expected or (expected <= 1 and measured <= 1))
    return entry


def main():
    fifo = Fifo(sys.argv[1])
    out_path = sys.argv[2]
    probe_count = int(sys.argv[3]) if len(sys.argv) > 3 else 6

    with open(f"{NORMALIZED}/tags.json") as f:
        tags = json.load(f)["tags"]["minecraft:block"]
    with open(f"{NORMALIZED}/hardness.json") as f:
        candidate = json.load(f)["blocks"]
    with open(f"{NORMALIZED}/motion.json") as f:
        # Les blocs dont on sait déjà qu'ils tiennent en place : un bloc qui ne
        # tient pas ne peut pas être miné, et son silence se lirait à tort comme
        # « incassable ».
        placeable = list(json.load(f)["blocks"])
    # Le même tableau de supports que la mesure des heightmaps : c'est là qu'on
    # a établi, bloc par bloc, ce sur quoi chacun accepte de tenir.
    substrate = SUBSTRATE

    tool_of = tools_of(tags)

    fifo.run("gamerule randomTickSpeed 0", "gamerule doDaylightCycle false",
             "gamerule doMobSpawning false", "gamerule doWeatherCycle false",
             "gamerule doTileDrops false", "difficulty peaceful", "time set noon",
             f"forceload add -8 -8 {probe_count * 16 + 8} 8")
    time.sleep(4.0)

    results = {}
    lock = threading.Lock()
    queue = [b for b in placeable if b in candidate]
    cursor = [0]

    def worker(index):
        # Chaque sonde se connecte dans son propre fil. Montées l'une après
        # l'autre, les premières restent muettes le temps des suivantes et le
        # serveur les expulse pour keep-alive non répondu — trente secondes
        # suffisent, et six sondes les dépassent.
        probe = Probe(index, fifo, substrate)
        while True:
            with lock:
                if cursor[0] >= len(queue):
                    return
                block = queue[cursor[0]]
                cursor[0] += 1
            try:
                entry = check_block(probe, block, candidate[block], tool_of)
            except (OSError, EOFError):
                # Une sonde perdue ne doit pas emporter sa part du travail :
                # on la remplace et on remet le bloc dans la file.
                probe = Probe(index, fifo, substrate)
                with lock:
                    queue.append(block)
                continue
            with lock:
                results[block] = entry
                done = len(results)
            if done % 20 == 0:
                print(f"  {done}/{len(queue)}", flush=True)

    threads = [threading.Thread(target=worker, args=(i,), daemon=True)
               for i in range(probe_count)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    agree = sum(1 for v in results.values() if v["ok"])
    with open(out_path, "w") as f:
        json.dump({"checked": len(results), "agree": agree, "results": results}, f, indent=1)
    print(f"écrit {out_path} : {agree}/{len(results)} blocs conformes")
    for name, v in sorted(results.items()):
        if not v["ok"]:
            print(f"  écart {name}: dureté {v['hardness']} outil_requis={v['requires_tool']}"
                  f" attendu {v['expected']} mesuré {v['measured']}")


if __name__ == "__main__":
    main()
