#!/usr/bin/env python3
"""La lumière après un geste, écrite par le vrai serveur 1.20.1.

Le moteur de lumière incrémental prétend réparer la lumière autour d'un geste
exactement comme un recalcul complet. Contre vanilla, la question est plus
simple : après un bloc posé ou cassé, les cases dont le jeu a changé la lumière
ont-elles la même valeur chez nous ?

Le montage, sans client : une copie de `run/saves/New World` (lue, jamais
écrite) sous `.scratch/light-lab/`, le vrai serveur piloté par sa console.

  1. Des scènes construites en l'air et sous terre par `fill` et `setblock` :
     une boîte creuse à cheval sur une frontière de chunk avec une torche, une
     salle souterraine éclairée à la pierre lumineuse, un bloc de pierre à
     percer, une canopée de feuilles, un bassin d'eau entre quatre vitres.
  2. `save-all flush`, copie des régions → `before/`.
  3. Les gestes : toits ouverts, torche déplacée, pierre lumineuse cassée,
     lanterne posée, puits percé jusqu'au ciel, canopée recouverte, bassin
     couvert.
  4. `save-all flush`, copie → `after/`, arrêt du serveur.

La comparaison elle-même est en C++ : `test_ov_world "[.light-vanilla-edits]"`
relit `before/` et `after/`, éclaire `before/` avec notre moteur, retrouve les
gestes en comparant les blocs des deux sauvegardes, les applique un par un en
incrémental, et compte les cases dont vanilla a changé la lumière.

Usage (voie java) : python3 scripts/measure_light_edits.py
"""
import os
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(REPO, "run", "saves", "New World")
LAB = os.path.join(REPO, ".scratch", "light-lab")
JAR = os.path.join(REPO, "tools", "vanilla", "server.jar")
PORT = int(os.environ.get("OV_LIGHT_LAB_PORT", "25602"))

BUILD = [
    "gamerule randomTickSpeed 0", "gamerule doDaylightCycle false",
    "gamerule doFireTick false", "gamerule doMobSpawning false",
    "gamerule doWeatherCycle false", "difficulty peaceful", "time set noon",
    "forceload add 0 0 63 63",
    # Une boîte creuse à cheval sur x = 16, une torche dedans.
    "fill 10 150 4 21 156 11 minecraft:stone hollow",
    "setblock 12 151 7 minecraft:torch",
    # Une salle sous terre, noire, et une pierre lumineuse.
    "fill 30 -40 30 45 -34 45 minecraft:stone hollow",
    "setblock 33 -39 33 minecraft:glowstone",
    # Un bloc de pierre plein, à percer.
    "fill 3 140 28 7 160 32 minecraft:stone",
    # Une canopée de feuilles au-dessus d'un sol.
    "fill 18 140 28 28 140 38 minecraft:stone",
    "fill 20 150 30 26 150 36 minecraft:oak_leaves[persistent=true]",
    # Un bassin d'eau entre quatre vitres, sur un fond de verre.
    "fill 40 139 5 46 146 11 minecraft:glass hollow",
    "fill 41 140 6 45 145 10 minecraft:water",
    "fill 41 146 6 45 146 10 minecraft:air",
]

EDITS = [
    "setblock 12 156 7 minecraft:air",
    "fill 17 156 5 20 156 10 minecraft:air",
    "setblock 12 151 7 minecraft:air",
    "setblock 19 151 8 minecraft:torch",
    "setblock 33 -39 33 minecraft:air",
    "setblock 40 -39 40 minecraft:sea_lantern",
    "fill 5 140 30 5 160 30 minecraft:air",
    "fill 20 152 30 26 152 36 minecraft:stone",
    "setblock 23 150 33 minecraft:air",
    "fill 41 147 6 45 147 10 minecraft:stone",
]


def snapshot(name: str) -> None:
    target = os.path.join(LAB, name)
    if os.path.exists(target):
        shutil.rmtree(target)
    shutil.copytree(os.path.join(LAB, "world", "region"), os.path.join(target, "region"))


def main() -> int:
    if not os.path.exists(JAR):
        print(f"pas de serveur vanilla à {JAR}")
        return 2
    if os.path.exists(LAB):
        shutil.rmtree(LAB)
    os.makedirs(os.path.join(LAB, "world"))
    for part in ("level.dat", "region"):
        source = os.path.join(SOURCE, part)
        target = os.path.join(LAB, "world", part)
        (shutil.copytree if os.path.isdir(source) else shutil.copy2)(source, target)
    with open(os.path.join(LAB, "eula.txt"), "w") as f:
        f.write("eula=true\n")
    with open(os.path.join(LAB, "server.properties"), "w") as f:
        f.write(f"online-mode=false\nserver-port={PORT}\nlevel-name=world\n"
                "max-tick-time=-1\nspawn-protection=0\nspawn-monsters=false\n"
                "sync-chunk-writes=true\n")

    log = open(os.path.join(LAB, "server.log"), "w")
    server = subprocess.Popen(["java", "-Xmx1G", "-jar", JAR, "nogui"], cwd=LAB,
                              stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT,
                              text=True)

    def run(*lines: str, settle: float = 0.3) -> None:
        for line in lines:
            server.stdin.write(line + "\n")
            server.stdin.flush()
            time.sleep(0.05)
        time.sleep(settle)

    def wait_for(text: str, timeout: float) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            with open(os.path.join(LAB, "server.log")) as f:
                if text in f.read():
                    return True
            if server.poll() is not None:
                return False
            time.sleep(0.5)
        return False

    try:
        if not wait_for("Done (", 240):
            print("le serveur n'a jamais fini de démarrer")
            return 1
        run(*BUILD, settle=8.0)
        run("save-all flush", settle=6.0)
        snapshot("before")
        run(*EDITS, settle=8.0)
        run("save-all flush", settle=6.0)
        snapshot("after")
        run("stop", settle=0.0)
        server.wait(timeout=120)
    finally:
        if server.poll() is None:
            server.kill()
        log.close()
    # Le serveur a déballé ses bibliothèques ici : on ne garde que les
    # sauvegardes, pour ne pas laisser 50 Mo dans .scratch.
    for leftover in ("libraries", "versions", "world", "logs"):
        shutil.rmtree(os.path.join(LAB, leftover), ignore_errors=True)
    print(f"instantanés dans {LAB}/before et {LAB}/after")
    return 0


if __name__ == "__main__":
    sys.exit(main())
