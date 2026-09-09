#!/usr/bin/env python3
"""Pilote le serveur vanilla 1.20.1 par sa console et relit la sauvegarde.

Aucun client : les scénarios de fluides se posent entièrement avec `setblock`
et `fill`, ce qui évite la portée de six blocs du clic et rend le scénario
reproductible tick pour tick.
"""
import os
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Le bac à sable du serveur vanilla. Gitignoré comme tout ce qui sort du jeu :
# le dépôt garde la méthode, jamais les octets de Mojang.
LAB = os.environ.get("OV_FLUID_LAB", os.path.join(REPO, "run", "fluidlab"))
FIFO = os.path.join(LAB, "console.fifo")
LOG = os.path.join(LAB, "server.log")
WORLD = os.path.join(LAB, "world", "region")
NETHER = os.path.join(LAB, "world", "DIM-1", "region")
INSPECT = os.path.join(REPO, "build/macos-debug/bin/ov_inspect")
PACK = os.path.join(REPO, "data/vanilla/1.20.1/registry.ovpack")


def run(*lines):
    with open(FIFO, "w") as fifo:
        fifo.write("".join(line + "\n" for line in lines))


def save(settle=4.0):
    time.sleep(settle)
    run("save-all flush")
    time.sleep(3.0)


def states(positions, world=WORLD):
    """Rend l'état complet de chaque position, dans l'ordre."""
    text = "\n".join(f"{x} {y} {z}" for x, y, z in positions)
    out = subprocess.run([INSPECT, "state", world, f"--pack={PACK}"],
                         input=text, capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit(f"ov_inspect a échoué : {out.stderr}")
    return out.stdout.splitlines()


def setup():
    run("gamerule randomTickSpeed 0", "gamerule doDaylightCycle false",
        "gamerule doFireTick false", "gamerule doMobSpawning false",
        "difficulty peaceful", "time set noon")
    time.sleep(0.5)


def boot_instructions():
    """Comment lancer l'oracle. Le serveur n'est pas démarré d'ici : il tourne
    en tâche de fond pendant toute une campagne de mesures, et le tuer entre
    deux scénarios coûterait plus que la campagne."""
    return f"""Préparer le bac à sable une fois :

    mkdir -p {LAB} && cd {LAB}
    cp {REPO}/tools/vanilla/server.jar .
    echo eula=true > eula.txt
    printf 'level-type=minecraft\\:flat\\nonline-mode=false\\nserver-port=25577\\n'
        'max-tick-time=-1\\nallow-nether=true\\nspawn-monsters=false\\n' > server.properties
    mkfifo console.fifo

Puis le laisser tourner, la fifo tenue ouverte pour qu'il ne voie jamais EOF :

    (sleep 100000 > console.fifo &) ; java -Xmx1G -jar server.jar nogui < console.fifo &
"""


if __name__ == "__main__":
    print(boot_instructions())
