#!/usr/bin/env python3
"""Les menus du vrai client 1.20.1 : leurs widgets, leurs clés, leurs captures.

Même méthode que scripts/measure_creative_screen.py et
scripts/measure_chat_screen.py, dont ce script réutilise le classpath, les
mappings et les options : l'instance vanilla 1.20.1 de l'utilisateur
(PrismLauncher) démarre sur son écran titre — sans serveur : le monde est créé
par le client lui-même, serveur intégré compris — et
`scripts/screens_oracle.java` la pilote par des événements GLFW injectés dans
ses propres gestionnaires clavier et souris.

Chaque écran atteint est décrit widget par widget (classe, position, taille,
texte, clé de traduction et arguments) et capturé. Le client écrit aussi deux
options.txt : l'un tel qu'au démarrage, l'autre après avoir reçu des valeurs
connues (champ de vision 90, distance 8, sensibilité 0,75, vsync coupée,
60 i/s, musique 0,25, blocs 0,5).

Rien n'est lu ni traduit du code de Mojang : les mappings officiels ne servent
qu'à *nommer* (CLAUDE.md § 1). Rien n'est commité : tout va sous
data/vanilla/1.20.1/generated/screens/, gitignoré.

Un seul JVM à la fois sur la machine partagée :

    lockf /tmp/ov-vanilla.lock python3 scripts/measure_screens.py
"""

import argparse
import importlib.util
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "data", "vanilla", "1.20.1", "generated", "screens")
NAME = "OvOracle"


def creative():
    spec = importlib.util.spec_from_file_location(
        "measure_creative_screen", os.path.join(ROOT, "scripts", "measure_creative_screen.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pack", choices=("faithful", "vanilla"), default="faithful")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--cache", default=CACHE)
    args = parser.parse_args()

    base = creative()
    mappings = base.fetch_mappings()
    cp, natives = base.classpath_and_natives()

    classes = os.path.join(args.cache, "classes")
    staged = os.path.join(args.cache, "src", "ov")
    os.makedirs(classes, exist_ok=True)
    os.makedirs(staged, exist_ok=True)
    source = os.path.join(staged, "ScreensOracle.java")
    shutil.copyfile(os.path.join(ROOT, "scripts", "screens_oracle.java"), source)
    subprocess.run([os.path.join(base.JAVA_HOME, "bin", "javac"), "-nowarn", "-d", classes, source],
                   check=True)

    game_dir = os.path.join(args.cache, "client-" + args.pack)
    out_dir = os.path.join(game_dir, "oracle")
    # A fresh game directory each run: no world (Singleplayer must lead to
    # Create World the first time), no screenshots from an earlier run.
    for sub in ("screenshots", "saves", "oracle"):
        shutil.rmtree(os.path.join(game_dir, sub), ignore_errors=True)
    base.write_options(game_dir, args.pack)
    with open(os.path.join(game_dir, "options.txt"), "a") as f:
        # No Realms notification over the title screen's buttons.
        f.write("realmsNotifications:false\n")

    command = [
        os.path.join(base.JAVA_HOME, "bin", "java"), "-XstartOnFirstThread", "-Xmx1800M",
        "-Djava.library.path=" + natives, "-Dorg.lwjgl.librarypath=" + natives,
        "-cp", os.pathsep.join([classes] + cp), "ov.ScreensOracle", mappings, out_dir,
        "--username", NAME, "--version", "1.20.1", "--gameDir", game_dir,
        "--assetsDir", os.path.join(base.PRISM, "assets"), "--assetIndex", "5",
        "--uuid", "0f0e0d0c0b0a09080706050403020100", "--accessToken", "0",
        "--userType", "legacy", "--versionType", "release",
        "--width", "1280", "--height", "720",
    ]
    log_path = os.path.join(game_dir, "client.log")
    try:
        with open(log_path, "w") as log:
            proc = subprocess.run(command, cwd=game_dir, stdout=log, stderr=subprocess.STDOUT,
                                  timeout=args.timeout)
        print("client : code %d, journal %s" % (proc.returncode, log_path))
    except subprocess.TimeoutExpired:
        print("client : délai dépassé (%d s) — voir %s" % (args.timeout, log_path))

    # The world the oracle made is not needed once measured: it is ~20 MB of
    # spawn chunks on a disk shared by several agents.
    shutil.rmtree(os.path.join(game_dir, "saves"), ignore_errors=True)
    facts = os.path.join(out_dir, "facts.txt")
    if os.path.exists(facts):
        print("faits : %s" % facts)
    shots = os.path.join(game_dir, "screenshots")
    if os.path.isdir(shots):
        print("captures : %s (%d)" % (shots, len(os.listdir(shots))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
