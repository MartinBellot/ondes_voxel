#!/usr/bin/env python3
"""Le rendu des entités : les modèles du vrai client, et les captures côte à côte.

Sous-commandes :

    models      lance le vrai client 1.20.1 de l'utilisateur (sans serveur),
                attend que ses ressources soient chargées, et écrit chaque
                modèle d'entité tel que le jeu le cuit — parties, pivots,
                rotations, polygones et coordonnées de texture — dans
                data/vanilla/1.20.1/entity_java_models.json (gitignoré).
                À lancer SOUS LE VERROU COMMUN :
                    lockf /tmp/ov-vanilla.lock python3 scripts/measure_entity_render.py models

Le programme Java (scripts/entity_model_oracle.java) ne lit aucun code
décompilé : les mappings officiels ne servent qu'à *nommer* les classes et les
champs, et chaque nombre écrit est produit par le jeu pendant qu'il tourne —
la même technique que les oracles du rendu, de l'inventaire créatif et du
chat. La sortie est une donnée Mojang, régénérée localement, jamais commitée.
"""

import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure_creative_screen as creative  # noqa: E402  (classpath, mappings)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Everything this script writes that is not the dump stays in the worktree's
# own scratch directory: run/ may be shared with other checkouts.
WORK = os.path.join(ROOT, ".scratch", "entity-render")
DUMP = os.path.join(ROOT, "data", "vanilla", "1.20.1", "entity_java_models.json")
WIDTH, HEIGHT = 854, 480


def write_options(game_dir):
    os.makedirs(game_dir, exist_ok=True)
    link = os.path.join(game_dir, "resourcepacks")
    if not os.path.exists(link):
        os.symlink(os.path.join(ROOT, "ressourcepacks"), link)
    names = [n for n in os.listdir(link) if n.lower().startswith("faithful")]
    if not names:
        raise SystemExit("aucun pack Faithful dans ressourcepacks/")
    # The same settings as the render parity captures, so that a capture taken
    # here compares with ours: fancy, smooth lighting, brightness 0.5, the
    # Faithful pack, no view bobbing.
    with open(os.path.join(game_dir, "options.txt"), "w") as f:
        f.write("\n".join([
            "version:3465", "guiScale:2", "pauseOnLostFocus:false", "onboardAccessibility:false",
            "narrator:0", "tutorialStep:none", "joinedFirstServer:true", "skipMultiplayerWarning:true",
            "renderDistance:6", "simulationDistance:5", "soundCategory_master:0.0",
            "lang:en_us", "fov:0.0", "gamma:0.5", "graphicsMode:1", "ao:true", "biomeBlendRadius:2",
            "mipmapLevels:4", "renderClouds:\"false\"", "bobView:false", "fovEffectScale:0.0",
            "screenEffectScale:1.0", "particles:2", "entityShadows:false", "maxFps:60",
            "enableVsync:true", "prioritizeChunkUpdates:2",
            "resourcePacks:[\"vanilla\",\"file/%s\"]" % names[0], "incompatibleResourcePacks:[]", "",
        ]))


def compile_oracle(source_name, class_name):
    classes = os.path.join(WORK, "classes")
    staged = os.path.join(WORK, "src", "ov")
    os.makedirs(classes, exist_ok=True)
    os.makedirs(staged, exist_ok=True)
    source = os.path.join(staged, class_name + ".java")
    shutil.copyfile(os.path.join(ROOT, "scripts", source_name), source)
    cp, _ = creative.classpath_and_natives()
    subprocess.run([os.path.join(creative.JAVA_HOME, "bin", "javac"), "-nowarn", "-d", classes,
                    "-cp", os.pathsep.join(cp), source], check=True)
    return classes


def client_command(classes, main_class, program_args, extra_mc_args=()):
    mappings = creative.fetch_mappings()
    cp, natives = creative.classpath_and_natives()
    game_dir = os.path.join(WORK, "client")
    write_options(game_dir)
    return [
        os.path.join(creative.JAVA_HOME, "bin", "java"), "-XstartOnFirstThread", "-Xmx1500M",
        "-Djava.library.path=" + natives, "-Dorg.lwjgl.librarypath=" + natives,
        "-cp", os.pathsep.join([classes] + cp), main_class, mappings, *program_args,
        "--username", "OvOracle", "--version", "1.20.1", "--gameDir", game_dir,
        "--assetsDir", os.path.join(creative.PRISM, "assets"), "--assetIndex", "5",
        "--uuid", "0f0e0d0c0b0a09080706050403020100", "--accessToken", "0",
        "--userType", "legacy", "--versionType", "release",
        "--width", str(WIDTH), "--height", str(HEIGHT), *extra_mc_args,
    ], game_dir


def run_models(timeout):
    classes = compile_oracle("entity_model_oracle.java", "EntityModelOracle")
    command, game_dir = client_command(classes, "ov.EntityModelOracle", [DUMP])
    log_path = os.path.join(WORK, "models-client.log")
    os.makedirs(WORK, exist_ok=True)
    tmp = DUMP + ".partial"
    command[command.index(DUMP)] = tmp
    try:
        with open(log_path, "w") as log:
            proc = subprocess.run(command, cwd=game_dir, stdout=log, stderr=subprocess.STDOUT,
                                  timeout=timeout)
        code = proc.returncode
    except subprocess.TimeoutExpired:
        code = -1
        print("client vanilla : délai dépassé (%d s) — voir %s" % (timeout, log_path))
    if code == 0 and os.path.exists(tmp):
        os.replace(tmp, DUMP)
        print("modèles : %s (%d o)" % (os.path.relpath(DUMP, ROOT), os.path.getsize(DUMP)))
    else:
        print("client vanilla : code %s, journal %s" % (code, os.path.relpath(log_path, ROOT)))
        sys.exit(1)


# ── The side-by-side scenes ─────────────────────────────────────────────────
#
# One vanilla server (the real jar) on a superflat world in .scratch/, the
# scenes of scripts/entity_scenes.py summoned on it. The real client visits each
# scene (a capture before and after the summon, then the vertices the game
# emits for the entity); then our client visits the same scenes on the same
# server (a capture with its entities and one without, and the vertices it
# submits). Everything under one hold of the common lock:
#
#     lockf /tmp/ov-vanilla.lock python3 scripts/measure_entity_render.py scenes

import socket  # noqa: E402
import time  # noqa: E402

import entity_scenes  # noqa: E402

PORT = 25781
SERVER_DIR = os.path.join(WORK, "server")
VANILLA_OUT = os.path.join(WORK, "vanilla")
OURS_OUT = os.path.join(WORK, "ours")
VIEW_DISTANCE = 5


def offline_uuid(name):
    import hashlib
    import uuid
    digest = bytearray(hashlib.md5(("OfflinePlayer:" + name).encode()).digest())
    digest[6] = (digest[6] & 0x0F) | 0x30
    digest[8] = (digest[8] & 0x3F) | 0x80
    return str(uuid.UUID(bytes=bytes(digest)))


def prepare_server():
    shutil.rmtree(SERVER_DIR, ignore_errors=True)
    os.makedirs(SERVER_DIR)
    with open(os.path.join(SERVER_DIR, "eula.txt"), "w") as f:
        f.write("eula=true\n")
    with open(os.path.join(SERVER_DIR, "server.properties"), "w") as f:
        f.write("\n".join([
            "online-mode=false", "server-port=%d" % PORT, "level-type=minecraft\\:flat",
            "allow-flight=true",
            "generate-structures=false", "spawn-protection=0", "difficulty=easy",
            # Not spawn-animals=false nor spawn-npcs=false: a vanilla server
            # does not only stop spawning them, it discards every animal and
            # villager that exists — the summoned ones included. The second
            # session lost its cows, sheep, cats, horses and villagers to that.
            # doMobSpawning false already keeps the world empty.
            "view-distance=%d" % VIEW_DISTANCE, "simulation-distance=4", "spawn-monsters=false",
            "max-players=4", "motd=ov-entities",
            "level-name=world", "sync-chunk-writes=false", "",
        ]))
    import json
    with open(os.path.join(SERVER_DIR, "ops.json"), "w") as f:
        json.dump([{"uuid": offline_uuid(n), "name": n, "level": 4, "bypassesPlayerLimit": False}
                   for n in ("OvOracle", "OvOurs")], f)


def port_open(port):
    with socket.socket() as s:
        s.settimeout(0.5)
        return s.connect_ex(("127.0.0.1", port)) == 0


class VanillaServer:
    def __init__(self):
        self.log_path = os.path.join(WORK, "server.log")
        self.log = open(self.log_path, "w")
        jar = os.path.join(ROOT, "tools", "vanilla", "server.jar")
        self.proc = subprocess.Popen([os.path.join(creative.JAVA_HOME, "bin", "java"), "-Xmx1G",
                                      "-jar", jar, "nogui"], cwd=SERVER_DIR, stdin=subprocess.PIPE,
                                     stdout=self.log, stderr=subprocess.STDOUT)
        for _ in range(600):
            if "Done (" in open(self.log_path).read():
                return
            if self.proc.poll() is not None:
                raise SystemExit("le serveur vanilla s'est arrêté — voir " + self.log_path)
            time.sleep(0.5)
        raise SystemExit("le serveur vanilla n'a pas démarré")

    def console(self, command):
        self.proc.stdin.write((command + "\n").encode())
        self.proc.stdin.flush()
        time.sleep(0.05)

    def stop(self):
        try:
            self.proc.communicate(b"stop\n", timeout=90)
        except Exception:  # noqa: BLE001
            self.proc.kill()


def setup_world(server):
    scenes = entity_scenes.SCENES
    # 13000, not noon. Glass passes full sky light and the game counts a mob
    # under sky light 15 as under the sky: in the first session every zombie,
    # skeleton and stray burned in its capture despite the roof, and would have
    # died before our client arrived. Undead stop burning at 12542 (the wiki's
    # daylight cycle); at 13000 they stand unlit, and both clients light them
    # from the same lightmap, which ours reproduces texel for texel.
    for command in ("gamerule doDaylightCycle false", "gamerule doMobSpawning false",
                    "gamerule doWeatherCycle false", "gamerule randomTickSpeed 0",
                    "gamerule doFireTick false", "gamerule mobGriefing false", "time set 13000",
                    "weather clear", "difficulty easy"):
        server.console(command)
    last_x = int(scenes[-1].x) + 16
    # Every cell's chunks stay loaded with nobody near, and a glass roof keeps
    # the sun off what burns in it: /fill refuses more than 32768 blocks, so
    # the roof goes down in strips. The forced chunks load asynchronously, and
    # a /fill into a chunk that is not loaded yet fails ("That position is not
    # loaded") — the first session lost its roof, and every zombie burned in
    # the capture, to exactly that. So: force, wait, fill, and count.
    strips = list(range(-16, last_x, 256))
    for x0 in strips:
        server.console("forceload add %d -32 %d 32" % (x0, min(x0 + 255, last_x)))
    time.sleep(8)
    before = open(server.log_path).read().count("Successfully filled")
    for x0 in strips:
        server.console("fill %d -48 -12 %d -48 20 minecraft:glass" % (x0, min(x0 + 255, last_x)))
    time.sleep(3)
    filled = open(server.log_path).read().count("Successfully filled") - before
    print("toit de verre : %d bandes sur %d" % (filled, len(strips)))
    if filled != len(strips):
        raise SystemExit("le toit de verre n'est pas posé en entier — voir " + server.log_path)


def write_directives(path):
    lines = ["facts start"]
    for scene in entity_scenes.SCENES:
        x, y, z, yaw, pitch = scene.camera
        lines.append("shot %s_empty %.2f %.2f %.2f %.1f %.2f" % (scene.name, x, y, z, yaw, pitch))
        lines.append("cmd " + scene.summon)
        lines.append("wait 1500")
        lines.append("shot %s %.2f %.2f %.2f %.1f %.2f" % (scene.name, x, y, z, yaw, pitch))
        lines.append("dump %s %.2f %.2f %.2f 3" % (scene.name, scene.x, entity_scenes.GROUND, scene.z))
        if scene.name == "ender_dragon":
            lines.append("flap 60")
    # Riding: the player is put in a minecart, and the heights of the player,
    # the camera and the cart are printed.
    ride = entity_scenes.SPACING * (len(entity_scenes.SCENES) + 1) + 0.5
    lines += ["cmd tp @s %.1f -60 3.5 180 20" % ride,
              "cmd summon minecraft:minecart %.1f -60 0.5" % ride,
              "wait 1500",
              "cmd ride @s mount @e[type=minecraft:minecart,limit=1,sort=nearest]",
              "wait 1500", "facts riding_minecart",
              "cmd ride @s dismount",
              "cmd summon minecraft:boat %.1f -60 6.5" % ride,
              "wait 1500",
              "cmd ride @s mount @e[type=minecraft:boat,limit=1,sort=nearest]",
              "wait 1500", "facts riding_boat",
              "cmd ride @s dismount"]
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def run_vanilla_scenes(timeout):
    classes = compile_oracle("entity_render_oracle.java", "EntityRenderOracle")
    directives = os.path.join(WORK, "directives.txt")
    write_directives(directives)
    shutil.rmtree(VANILLA_OUT, ignore_errors=True)
    os.makedirs(VANILLA_OUT)
    command, game_dir = client_command(classes, "ov.EntityRenderOracle", [VANILLA_OUT, directives],
                                       ["--quickPlayMultiplayer", "127.0.0.1:%d" % PORT])
    shutil.rmtree(os.path.join(game_dir, "screenshots"), ignore_errors=True)
    log_path = os.path.join(WORK, "vanilla-client.log")
    with open(log_path, "w") as log:
        try:
            code = subprocess.run(command, cwd=game_dir, stdout=log, stderr=subprocess.STDOUT,
                                  timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            code = "délai"
    shots = os.path.join(game_dir, "screenshots")
    count = len(os.listdir(shots)) if os.path.isdir(shots) else 0
    print("client vanilla : code %s, %d captures, journal %s" % (code, count,
                                                               os.path.relpath(log_path, ROOT)))
    if os.path.isdir(shots):
        for name in os.listdir(shots):
            shutil.copyfile(os.path.join(shots, name), os.path.join(VANILLA_OUT, name))


def run_ours_scenes(server, binary, only):
    os.makedirs(OURS_OUT, exist_ok=True)
    settle_chunks = (2 * VIEW_DISTANCE + 1) ** 2
    for scene in entity_scenes.SCENES:
        if only and scene.name not in only:
            continue
        if scene.ephemeral:
            # What expires on its own is summoned again just before our visit.
            server.console("kill @e[x=%.1f,y=-60,z=%.1f,distance=..3,type=!minecraft:player]"
                           % (scene.x, scene.z))
            server.console(scene.summon)
            time.sleep(1.0)
        x, y, z, yaw, pitch = scene.camera
        for variant in ("", "_empty"):
            ppm = os.path.join(OURS_OUT, scene.name + variant + ".ppm")
            command = [binary, "--connect=127.0.0.1:%d" % PORT, "--username=OvOurs",
                       "--width=854", "--height=480", "--radius=%d" % VIEW_DISTANCE, "--no-hud",
                       "--no-sound", "--stand-at=%.2f,%.2f,%.2f,%.1f,%.2f" % (x, y, z, yaw, pitch),
                       "--chat=/tp @s %.2f %.2f %.2f %.1f %.2f" % (x, y, z, yaw, pitch),
                       "--chat-at=60", "--frame-ms=16", "--frames=100000", "--settle-shot",
                       "--settle-chunks=%d" % settle_chunks, "--screenshot=" + ppm]
            if variant:
                command.append("--no-entities")
            else:
                command.append("--entity-dump=" + os.path.join(OURS_OUT, scene.name + ".json"))
            log = os.path.join(OURS_OUT, scene.name + variant + ".log")
            with open(log, "w") as f:
                try:
                    code = subprocess.run(command, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT,
                                          timeout=240).returncode
                except subprocess.TimeoutExpired:
                    code = "délai"
            print("notre client %-22s : %s%s" % (scene.name + variant, code,
                                                  "" if os.path.exists(ppm) else " — PAS DE CAPTURE"))


def run_scenes(timeout, binary, only, vanilla, ours):
    """`vanilla` runs the real client on a fresh world (every scene summoned by
    it); `ours` then visits the same world — in the same session, or in a later
    one on the world the first left behind (the mobs are NoAI and persistent,
    what expires is summoned again)."""
    if vanilla or not os.path.isdir(os.path.join(SERVER_DIR, "world")):
        prepare_server()
    server = VanillaServer()
    try:
        setup_world(server)
        if vanilla:
            run_vanilla_scenes(timeout)
        if ours:
            run_ours_scenes(server, binary, only)
    finally:
        server.stop()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["models", "compile", "scenes"])
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--binary", default=os.path.join(ROOT, "build", "macos-debug", "bin",
                                                         "ov_voxel"))
    parser.add_argument("--only", default="")
    parser.add_argument("--ours-only", action="store_true",
                        help="ne pas relancer le client vanilla (ses captures sont déjà là)")
    parser.add_argument("--vanilla-only", action="store_true",
                        help="seulement le vrai client : captures, sommets, états")
    args = parser.parse_args()
    if args.command == "compile":
        compile_oracle("entity_model_oracle.java", "EntityModelOracle")
        compile_oracle("entity_render_oracle.java", "EntityRenderOracle")
        print("compilé")
    elif args.command == "models":
        run_models(args.timeout)
    elif args.command == "scenes":
        run_scenes(max(args.timeout, 1800), os.path.abspath(args.binary),
                   [s for s in args.only.split(",") if s], not args.ours_only,
                   not args.vanilla_only)


if __name__ == "__main__":
    main()
