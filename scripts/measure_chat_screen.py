#!/usr/bin/env python3
"""Le chat du vrai client 1.20.1 : ses nombres et ses captures.

Même méthode que scripts/measure_creative_screen.py, dont ce script réutilise
le classpath, les mappings et les options : l'instance vanilla 1.20.1 de
l'utilisateur (PrismLauncher) rejoint un serveur vanilla **en opérateur**, et
`scripts/chat_screen_oracle.java` la pilote par des événements GLFW injectés
dans les gestionnaires clavier et souris du jeu. Après chaque geste, le jeu
donne ses nombres (champ de saisie, taille du chat, lignes et leur tick
d'arrivée, suggestions, minuteries des titres) et prend une capture.

Rien n'est lu ni traduit du code de Mojang : les mappings officiels ne servent
qu'à *nommer* (CLAUDE.md § 1). Rien n'est commité : tout va sous
data/vanilla/1.20.1/generated/chat-screen/, gitignoré.

Usage:
    scripts/measure_chat_screen.py                 # serveur vanilla lancé ici
    scripts/measure_chat_screen.py --pack vanilla  # sans Faithful 32x
"""

import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import time
import uuid

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "data", "vanilla", "1.20.1", "generated", "chat-screen")
NAME = "OvOracle"


def creative():
    spec = importlib.util.spec_from_file_location(
        "measure_creative_screen", os.path.join(ROOT, "scripts", "measure_creative_screen.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def offline_uuid(name):
    """L'UUID qu'un serveur hors-ligne dérive d'un nom : MD5, version 3."""
    digest = bytearray(hashlib.md5(("OfflinePlayer:" + name).encode("utf-8")).digest())
    digest[6] = (digest[6] & 0x0F) | 0x30
    digest[8] = (digest[8] & 0x3F) | 0x80
    return str(uuid.UUID(bytes=bytes(digest)))


def start_server(base, port):
    server_dir = os.path.join(CACHE, "server")
    shutil.rmtree(os.path.join(server_dir, "world"), ignore_errors=True)
    os.makedirs(server_dir, exist_ok=True)
    with open(os.path.join(server_dir, "eula.txt"), "w") as f:
        f.write("eula=true\n")
    with open(os.path.join(server_dir, "ops.json"), "w") as f:
        json.dump([{"uuid": offline_uuid(NAME), "name": NAME, "level": 4,
                    "bypassesPlayerLimit": False}], f)
    with open(os.path.join(server_dir, "server.properties"), "w") as f:
        f.write("\n".join([
            "online-mode=false", "gamemode=creative", "force-gamemode=true",
            "level-type=minecraft\\:flat", "generate-structures=false",
            "view-distance=3", "simulation-distance=3", "spawn-protection=0",
            "spawn-monsters=false", "spawn-animals=false", "spawn-npcs=false",
            "difficulty=peaceful", "server-port=%d" % port, "max-players=2", ""]))
    jar = os.path.join(ROOT, "tools", "vanilla", "server.jar")
    log = open(os.path.join(server_dir, "server.log"), "w")
    proc = subprocess.Popen([os.path.join(base.JAVA_HOME, "bin", "java"), "-Xmx900M", "-jar", jar,
                             "nogui"], cwd=server_dir, stdout=log, stderr=subprocess.STDOUT,
                            stdin=subprocess.PIPE)
    for _ in range(240):
        if base.port_open(port):
            return proc
        time.sleep(0.5)
    proc.kill()
    raise SystemExit("le serveur vanilla n'a pas ouvert le port %d" % port)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pack", choices=("faithful", "vanilla"), default="faithful")
    parser.add_argument("--port", type=int, default=25643)
    parser.add_argument("--timeout", type=int, default=420)
    args = parser.parse_args()

    base = creative()
    mappings = base.fetch_mappings()
    cp, natives = base.classpath_and_natives()

    classes = os.path.join(CACHE, "classes")
    staged = os.path.join(CACHE, "src", "ov")
    os.makedirs(classes, exist_ok=True)
    os.makedirs(staged, exist_ok=True)
    source = os.path.join(staged, "ChatScreenOracle.java")
    shutil.copyfile(os.path.join(ROOT, "scripts", "chat_screen_oracle.java"), source)
    subprocess.run([os.path.join(base.JAVA_HOME, "bin", "javac"), "-nowarn", "-d", classes, source],
                   check=True)

    game_dir = os.path.join(CACHE, "client-" + args.pack)
    out_dir = os.path.join(game_dir, "oracle")
    shutil.rmtree(os.path.join(game_dir, "screenshots"), ignore_errors=True)
    base.write_options(game_dir, args.pack)
    # Pas de nuages : l'oracle regarde le ciel à la verticale, et un ciel uni
    # est ce qui rend chaque pixel d'interface mesurable.
    with open(os.path.join(game_dir, "options.txt"), "a") as f:
        f.write('renderClouds:"false"\n')

    server = start_server(base, args.port)
    command = [
        os.path.join(base.JAVA_HOME, "bin", "java"), "-XstartOnFirstThread", "-Xmx1500M",
        "-Djava.library.path=" + natives, "-Dorg.lwjgl.librarypath=" + natives,
        "-cp", os.pathsep.join([classes] + cp), "ov.ChatScreenOracle", mappings, out_dir,
        "--username", NAME, "--version", "1.20.1", "--gameDir", game_dir,
        "--assetsDir", os.path.join(base.PRISM, "assets"), "--assetIndex", "5",
        "--uuid", offline_uuid(NAME).replace("-", ""), "--accessToken", "0",
        "--userType", "legacy", "--versionType", "release",
        "--width", "1280", "--height", "720",
        "--quickPlayMultiplayer", "127.0.0.1:%d" % args.port,
    ]
    log_path = os.path.join(game_dir, "client.log")
    try:
        with open(log_path, "w") as log:
            proc = subprocess.run(command, cwd=game_dir, stdout=log, stderr=subprocess.STDOUT,
                                  timeout=args.timeout)
        print("client : code %d, journal %s" % (proc.returncode, os.path.relpath(log_path, ROOT)))
    except subprocess.TimeoutExpired:
        print("client : délai dépassé (%d s) — voir %s" % (args.timeout, log_path))
    finally:
        try:
            server.communicate(b"stop\n", timeout=60)
        except Exception:  # noqa: BLE001
            server.kill()

    facts = os.path.join(out_dir, "facts.txt")
    if os.path.exists(facts):
        print("faits : %s" % os.path.relpath(facts, ROOT))
    shots = os.path.join(game_dir, "screenshots")
    if os.path.isdir(shots):
        print("captures : %s (%d)" % (os.path.relpath(shots, ROOT), len(os.listdir(shots))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
