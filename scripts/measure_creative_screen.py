#!/usr/bin/env python3
"""L'écran créatif du vrai client 1.20.1 : ses nombres et ses captures.

La géométrie de l'inventaire créatif est dans le *client*, et le client peut
se lancer. Ce script démarre l'instance vanilla 1.20.1 de l'utilisateur
(PrismLauncher), la fait rejoindre un serveur vanilla en créatif, et la pilote
avec `scripts/creative_screen_oracle.java` : des événements GLFW synthétiques
injectés dans les gestionnaires clavier et souris du jeu lui-même. Après chaque
geste, le jeu donne ses nombres (panneau, onglets, cases, champ de recherche)
et prend une capture par son propre chemin de capture.

Rien n'est lu ni traduit du code de Mojang : les mappings officiels ne servent
qu'à *nommer* (CLAUDE.md § 1), comme pour scripts/creative_tabs_oracle.java.
Rien n'est commité : tout va sous data/vanilla/1.20.1/generated/creative-screen/,
gitignoré.

Usage:
    scripts/measure_creative_screen.py                 # serveur vanilla lancé ici
    scripts/measure_creative_screen.py --port 25641    # serveur déjà lancé
    scripts/measure_creative_screen.py --pack vanilla  # sans Faithful 32x
"""

import argparse
import hashlib
import json
import os
import shutil
import socket
import subprocess
import sys
import time
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRISM = os.path.expanduser("~/Library/Application Support/PrismLauncher")
LIBS = os.path.join(PRISM, "libraries")
CACHE = os.path.join(ROOT, "data", "vanilla", "1.20.1", "generated", "creative-screen")

# Les mappings client officiels 1.20.1, du manifeste de version de Mojang.
MAPPINGS_URL = ("https://piston-data.mojang.com/v1/objects/"
                "6c48521eed01fe2e8ecdadbd5ae348415f3c47da/client.txt")
MAPPINGS_SHA1 = "6c48521eed01fe2e8ecdadbd5ae348415f3c47da"

JAVA_HOME = os.environ.get("OV_JAVA_HOME", "/opt/homebrew/opt/openjdk@17")


def sha1_of(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fetch_mappings():
    path = os.path.join(CACHE, "client-mappings.txt")
    if os.path.exists(path) and sha1_of(path) == MAPPINGS_SHA1:
        return path
    os.makedirs(CACHE, exist_ok=True)
    try:
        with urllib.request.urlopen(MAPPINGS_URL, timeout=120) as r, open(path, "wb") as f:
            shutil.copyfileobj(r, f)
    except Exception as exc:  # noqa: BLE001
        print("  urllib : %s — nouvel essai avec curl" % exc)
        subprocess.run(["curl", "-sSfL", "-o", path, MAPPINGS_URL], check=True)
    if sha1_of(path) != MAPPINGS_SHA1:
        raise SystemExit("mappings client : mauvais SHA-1")
    return path


def allowed(lib):
    rules = lib.get("rules")
    if not rules:
        return True
    ok = False
    for r in rules:
        name = r.get("os", {}).get("name")
        if name is None or name in ("osx", "osx-arm64"):
            ok = r["action"] == "allow"
    return ok


def jar_path(name):
    parts = name.split(":")
    group, art, ver = parts[0], parts[1], parts[2]
    cls = ("-" + parts[3]) if len(parts) > 3 else ""
    return os.path.join(LIBS, *group.split("."), art, ver, "%s-%s%s.jar" % (art, ver, cls))


def classpath_and_natives():
    """Le classpath de l'instance, depuis les métadonnées de Prism."""
    cp = []
    for meta in ("net.minecraft/1.20.1.json", "org.lwjgl3/3.3.1.json"):
        with open(os.path.join(PRISM, "meta", meta)) as f:
            data = json.load(f)
        for lib in data.get("libraries", []):
            if not allowed(lib):
                continue
            names = [lib["name"]]
            natives = lib.get("natives", {})
            nat = natives.get("osx-arm64") or natives.get("osx")
            if nat:
                names.append(lib["name"] + ":" + nat.replace("${arch}", "arm64"))
            for n in names:
                p = jar_path(n)
                if os.path.exists(p):
                    cp.append(p)
    client = os.path.join(LIBS, "com", "mojang", "minecraft", "1.20.1",
                          "minecraft-1.20.1-client.jar")
    if not os.path.exists(client):
        raise SystemExit("client 1.20.1 introuvable : %s" % client)
    cp.append(client)
    natives_dir = os.path.join(CACHE, "natives")
    os.makedirs(natives_dir, exist_ok=True)
    for p in cp:
        if "natives-macos-arm64" in os.path.basename(p):
            with zipfile.ZipFile(p) as z:
                for n in z.namelist():
                    if n.endswith(".dylib"):
                        with z.open(n) as s, open(os.path.join(natives_dir,
                                                               os.path.basename(n)), "wb") as o:
                            o.write(s.read())
    return cp, natives_dir


def write_options(game_dir, pack):
    os.makedirs(game_dir, exist_ok=True)
    packs = '["vanilla"]'
    if pack == "faithful":
        link = os.path.join(game_dir, "resourcepacks")
        if not os.path.exists(link):
            os.symlink(os.path.join(ROOT, "ressourcepacks"), link)
        names = [n for n in os.listdir(link) if n.lower().startswith("faithful")]
        if not names:
            raise SystemExit("aucun pack Faithful dans ressourcepacks/")
        packs = '["vanilla","file/%s"]' % names[0]
    # L'échelle 3, comme nos captures ; aucune pause quand la fenêtre perd le
    # focus (elle le perd : elle est lancée depuis un terminal) ; aucun écran
    # d'accueil d'accessibilité, qui recouvrirait tout au premier lancement.
    with open(os.path.join(game_dir, "options.txt"), "w") as f:
        f.write("\n".join([
            "version:3465",
            "guiScale:3",
            "pauseOnLostFocus:false",
            "onboardAccessibility:false",
            "narrator:0",
            "tutorialStep:none",
            "joinedFirstServer:true",
            "skipMultiplayerWarning:true",
            "renderDistance:2",
            "simulationDistance:5",
            "soundCategory_master:0.0",
            "lang:en_us",
            "resourcePacks:%s" % packs,
            "incompatibleResourcePacks:[]",
            "",
        ]))


def port_open(port):
    with socket.socket() as s:
        s.settimeout(0.5)
        return s.connect_ex(("127.0.0.1", port)) == 0


def start_server(port):
    server_dir = os.path.join(CACHE, "server")
    # A fresh world every run. The server keeps a player's inventory, and a
    # second run that starts with the first run's stacks measures the first
    # run, not the gestures — which is what the second run of this script did.
    shutil.rmtree(os.path.join(server_dir, "world"), ignore_errors=True)
    os.makedirs(server_dir, exist_ok=True)
    with open(os.path.join(server_dir, "eula.txt"), "w") as f:
        f.write("eula=true\n")
    with open(os.path.join(server_dir, "server.properties"), "w") as f:
        f.write("\n".join([
            "online-mode=false", "gamemode=creative", "force-gamemode=true",
            "level-type=minecraft\\:flat", "generate-structures=false",
            "view-distance=3", "simulation-distance=3", "spawn-protection=0",
            "spawn-monsters=false", "spawn-animals=false", "spawn-npcs=false",
            "difficulty=peaceful", "server-port=%d" % port, "max-players=2", ""]))
    jar = os.path.join(ROOT, "tools", "vanilla", "server.jar")
    log = open(os.path.join(server_dir, "server.log"), "w")
    proc = subprocess.Popen([os.path.join(JAVA_HOME, "bin", "java"), "-Xmx900M", "-jar", jar,
                             "nogui"], cwd=server_dir, stdout=log, stderr=subprocess.STDOUT,
                            stdin=subprocess.PIPE)
    for _ in range(240):
        if port_open(port):
            return proc
        time.sleep(0.5)
    proc.kill()
    raise SystemExit("le serveur vanilla n'a pas ouvert le port %d" % port)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=0,
                        help="un serveur vanilla créatif déjà lancé sur ce port")
    parser.add_argument("--pack", choices=("faithful", "vanilla"), default="faithful")
    parser.add_argument("--timeout", type=int, default=420)
    args = parser.parse_args()

    mappings = fetch_mappings()
    cp, natives = classpath_and_natives()

    classes = os.path.join(CACHE, "classes")
    staged = os.path.join(CACHE, "src", "ov")
    os.makedirs(classes, exist_ok=True)
    os.makedirs(staged, exist_ok=True)
    source = os.path.join(staged, "CreativeScreenOracle.java")
    shutil.copyfile(os.path.join(ROOT, "scripts", "creative_screen_oracle.java"), source)
    subprocess.run([os.path.join(JAVA_HOME, "bin", "javac"), "-nowarn", "-d", classes, source],
                   check=True)

    game_dir = os.path.join(CACHE, "client-" + args.pack)
    out_dir = os.path.join(game_dir, "oracle")
    shutil.rmtree(os.path.join(game_dir, "screenshots"), ignore_errors=True)
    write_options(game_dir, args.pack)

    server = None
    port = args.port
    if port == 0:
        port = 25641
        server = start_server(port)

    command = [
        os.path.join(JAVA_HOME, "bin", "java"), "-XstartOnFirstThread", "-Xmx1500M",
        "-Djava.library.path=" + natives, "-Dorg.lwjgl.librarypath=" + natives,
        "-cp", os.pathsep.join([classes] + cp), "ov.CreativeScreenOracle", mappings, out_dir,
        "--username", "OvOracle", "--version", "1.20.1", "--gameDir", game_dir,
        "--assetsDir", os.path.join(PRISM, "assets"), "--assetIndex", "5",
        "--uuid", "0f0e0d0c0b0a09080706050403020100", "--accessToken", "0",
        "--userType", "legacy", "--versionType", "release",
        "--width", "1280", "--height", "720",
        "--quickPlayMultiplayer", "127.0.0.1:%d" % port,
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
        if server is not None:
            try:
                server.communicate(b"stop\n", timeout=60)
            except Exception:  # noqa: BLE001
                server.kill()

    facts = os.path.join(out_dir, "facts.txt")
    if os.path.exists(facts):
        print("faits : %s" % os.path.relpath(facts, ROOT))
    # Installed where the client reads it. Its hash is printed, not checked:
    # the saved-hotbar hints carry the key labels of *this* keyboard layout,
    # so two machines write two files.
    dump = os.path.join(out_dir, "creative_items.json")
    if os.path.exists(dump):
        target = os.path.join(ROOT, "data", "vanilla", "1.20.1", "creative_items.json")
        shutil.copyfile(dump, target)
        with open(target, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        print("installé : %s (sha256 %s)" % (os.path.relpath(target, ROOT), digest))
    shots = os.path.join(game_dir, "screenshots")
    if os.path.isdir(shots):
        print("captures : %s (%d)" % (os.path.relpath(shots, ROOT), len(os.listdir(shots))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
