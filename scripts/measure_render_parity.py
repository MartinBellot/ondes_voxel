#!/usr/bin/env python3
"""Le rendu du monde : le vrai client 1.20.1 et le nôtre, scène par scène.

Les deux clients rejoignent **le même serveur** — le nôtre, `ov_dedicated`,
sur une copie de quelques régions du monde de référence — et reçoivent donc
exactement les mêmes octets : les mêmes blocs, les mêmes biomes, la même
lumière. Tout écart entre les deux captures est un écart de *rendu*.

1. `ov_dedicated` démarre sur `run/render-parity/world` (copie de régions de
   `run/reference-1234567890`, gitignoré), en créatif.
2. Le client vanilla de l'utilisateur (PrismLauncher, Faithful 32x) rejoint ce
   serveur, piloté par `scripts/render_parity_oracle.java` : pour chaque scène
   une heure, une position, une orientation, puis une capture par le chemin de
   capture du jeu et les nombres de la frame (couleurs du ciel et du
   brouillard, lightmap 16×16…). Sous le verrou commun `/tmp/ov-vanilla.lock`.
3. Notre client (`ov_voxel --connect`) fait les mêmes scènes, une exécution
   par scène, et écrit ses captures en PPM.
4. `scripts/compare_render_parity.py` compare les paires.

Rien n'est commité : tout va sous `run/render-parity/` (gitignoré) et
`data/vanilla/1.20.1/generated/render-parity/`.

Usage:
    scripts/measure_render_parity.py world              # prépare le monde
    scripts/measure_render_parity.py vanilla            # captures du vrai client
    scripts/measure_render_parity.py ours [--tag=after] # nos captures
    scripts/measure_render_parity.py all
"""

import argparse
import os
import shutil
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure_creative_screen as creative  # noqa: E402  (classpath, mappings)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "run", "render-parity")
CACHE = os.path.join(ROOT, "data", "vanilla", "1.20.1", "generated", "render-parity")
REFERENCE = os.path.join(ROOT, "run", "reference-1234567890", "world")
SCENES = os.path.join(ROOT, "scripts", "render_parity_scenes.txt")
BUILD = os.path.join(ROOT, "build", "macos-debug", "bin")

# Three corners of the reference world, 23 MB of regions: the spawn (jungle,
# beach, lukewarm ocean, river), plains with a river at x = 48000, and desert
# against badlands at x = 15000.
REGIONS = ["r.0.0", "r.-1.0", "r.0.-1", "r.-1.-1", "r.93.0", "r.93.-1", "r.94.0", "r.94.-1",
           "r.29.50", "r.29.51"]

PORT = 25671
WIDTH, HEIGHT = 854, 480
RENDER_DISTANCE = 8
# The server streams a square of 17 x 17 chunks around a player: a capture
# waits for all of it.
SETTLE_CHUNKS = (2 * RENDER_DISTANCE + 1) ** 2
# Any ov_dedicated will do — the server only has to feed both clients the same
# bytes. A debug one cannot keep up with a 17 x 17 square (it logs "can't keep
# up" and trickles chunks for minutes); --server points at a release build.
SERVER = os.path.join(BUILD, "ov_dedicated")


def port_open(port):
    with socket.socket() as s:
        s.settimeout(0.5)
        return s.connect_ex(("127.0.0.1", port)) == 0


def prepare_world(name):
    world = os.path.join(OUT, name)
    shutil.rmtree(world, ignore_errors=True)
    os.makedirs(os.path.join(world, "region"))
    for region in REGIONS:
        shutil.copyfile(os.path.join(REFERENCE, "region", region + ".mca"),
                        os.path.join(world, "region", region + ".mca"))
    shutil.copyfile(os.path.join(REFERENCE, "level.dat"), os.path.join(world, "level.dat"))
    print("monde : %s (%d régions)" % (os.path.relpath(world, ROOT), len(REGIONS)))


def start_server(name, port):
    """Our server, on a copy: the scenes set the time, and /time writes level.dat.

    One world and one port per client: the vanilla run waits for the common
    lock and may start while our client is capturing, and two servers on one
    world directory would write over each other. Both copies come from the
    same regions, and the structures the scenes need are built by whichever
    client runs first on each.
    """
    if port_open(port):
        raise SystemExit("le port %d est déjà pris" % port)
    if not os.path.isdir(os.path.join(OUT, name)):
        prepare_world(name)
    log = open(os.path.join(OUT, name + ".log"), "w")
    proc = subprocess.Popen([SERVER, "--world=" + os.path.join(OUT, name),
                             "--port=%d" % port],
                            cwd=OUT, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.PIPE)
    for _ in range(600):
        if port_open(port):
            return proc
        if proc.poll() is not None:
            raise SystemExit("ov_dedicated s'est arrêté — voir %s/%s.log" % (OUT, name))
        time.sleep(0.5)
    proc.kill()
    raise SystemExit("ov_dedicated n'a pas ouvert le port %d" % port)


def stop(proc):
    if proc is None:
        return
    try:
        proc.communicate(b"stop\n", timeout=60)
    except Exception:  # noqa: BLE001
        proc.kill()


def ops(names):
    """Operators beside the server, where it looks: the offline UUID of each name."""
    import hashlib
    import json
    import uuid
    entries = []
    for name in names:
        digest = bytearray(hashlib.md5(("OfflinePlayer:" + name).encode()).digest())
        digest[6] = (digest[6] & 0x0F) | 0x30
        digest[8] = (digest[8] & 0x3F) | 0x80
        entries.append({"uuid": str(uuid.UUID(bytes=bytes(digest))), "name": name, "level": 4,
                        "bypassesPlayerLimit": False})
    with open(os.path.join(OUT, "ops.json"), "w") as f:
        json.dump(entries, f)


def write_options(game_dir):
    os.makedirs(game_dir, exist_ok=True)
    link = os.path.join(game_dir, "resourcepacks")
    if not os.path.exists(link):
        os.symlink(os.path.join(ROOT, "ressourcepacks"), link)
    names = [n for n in os.listdir(link) if n.lower().startswith("faithful")]
    if not names:
        raise SystemExit("aucun pack Faithful dans ressourcepacks/")
    # The user's own settings (PrismLauncher instance) wherever they bear on
    # the picture — fancy, smooth lighting, blend 2, mipmaps 4, brightness 0.5,
    # clouds on — and three that only remove motion: no view bobbing, no FOV
    # change for flying, no pause when the window loses the focus.
    with open(os.path.join(game_dir, "options.txt"), "w") as f:
        f.write("\n".join([
            "version:3465", "guiScale:2", "pauseOnLostFocus:false", "onboardAccessibility:false",
            "narrator:0", "tutorialStep:none", "joinedFirstServer:true", "skipMultiplayerWarning:true",
            "renderDistance:%d" % RENDER_DISTANCE, "simulationDistance:5", "soundCategory_master:0.0",
            "lang:en_us", "fov:0.0", "gamma:0.5", "graphicsMode:1", "ao:true", "biomeBlendRadius:2",
            "mipmapLevels:4", "renderClouds:\"true\"", "bobView:false", "fovEffectScale:0.0",
            "screenEffectScale:1.0", "particles:0", "entityShadows:true", "maxFps:60",
            "enableVsync:true", "prioritizeChunkUpdates:2",
            "resourcePacks:[\"vanilla\",\"file/%s\"]" % names[0], "incompatibleResourcePacks:[]", "",
        ]))


def run_vanilla(timeout):
    mappings = creative.fetch_mappings()
    cp, natives = creative.classpath_and_natives()
    classes = os.path.join(CACHE, "classes")
    staged = os.path.join(CACHE, "src", "ov")
    os.makedirs(classes, exist_ok=True)
    os.makedirs(staged, exist_ok=True)
    source = os.path.join(staged, "RenderParityOracle.java")
    shutil.copyfile(os.path.join(ROOT, "scripts", "render_parity_oracle.java"), source)
    subprocess.run([os.path.join(creative.JAVA_HOME, "bin", "javac"), "-nowarn", "-d", classes, source],
                   check=True)
    game_dir = os.path.join(CACHE, "client")
    out_dir = os.path.join(game_dir, "oracle")
    shutil.rmtree(os.path.join(game_dir, "screenshots"), ignore_errors=True)
    write_options(game_dir)
    command = [
        os.path.join(creative.JAVA_HOME, "bin", "java"), "-XstartOnFirstThread", "-Xmx1500M",
        "-Djava.library.path=" + natives, "-Dorg.lwjgl.librarypath=" + natives,
        "-cp", os.pathsep.join([classes] + cp), "ov.RenderParityOracle", mappings, out_dir, SCENES,
        "--username", "OvOracle", "--version", "1.20.1", "--gameDir", game_dir,
        "--assetsDir", os.path.join(creative.PRISM, "assets"), "--assetIndex", "5",
        "--uuid", "0f0e0d0c0b0a09080706050403020100", "--accessToken", "0",
        "--userType", "legacy", "--versionType", "release",
        "--width", str(WIDTH), "--height", str(HEIGHT),
        "--quickPlayMultiplayer", "127.0.0.1:%d" % PORT,
    ]
    log_path = os.path.join(game_dir, "client.log")
    try:
        with open(log_path, "w") as log:
            proc = subprocess.run(command, cwd=game_dir, stdout=log, stderr=subprocess.STDOUT,
                                  timeout=timeout)
        print("client vanilla : code %d, journal %s" % (proc.returncode, os.path.relpath(log_path, ROOT)))
    except subprocess.TimeoutExpired:
        print("client vanilla : délai dépassé (%d s) — voir %s" % (timeout, log_path))
    print("faits : %s" % os.path.relpath(os.path.join(out_dir, "facts.txt"), ROOT))


def scenes():
    out = []
    with open(SCENES) as f:
        for line in f:
            p = line.split()
            if p and p[0] == "scene":
                out.append(p[1:])
    return out


def setup_commands():
    """The scenes file's commands, minus the teleport the oracle needs to reach
    the chunks: our client already stands there when it sends them."""
    out = []
    with open(SCENES) as f:
        for line in f:
            p = line.split()
            if p and p[0] == "cmd" and p[1] != "tp":
                out.append("/" + line.strip()[4:].strip())
    return out


def run_ours(tag, only, extra, binary):
    shots = os.path.join(OUT, "ours-" + tag)
    os.makedirs(shots, exist_ok=True)
    first = True
    for name, x, y, z, yaw, pitch, t in scenes():
        if only and name not in only:
            continue
        ppm = os.path.join(shots, name + ".ppm")
        log = os.path.join(shots, name + ".log")
        # The first run of a session builds the scenes' structures, the way
        # the oracle does, so that either client can go first on a fresh
        # world. /fill is idempotent; running it twice changes nothing.
        chat = (setup_commands() if first else []) + ["/time set %s" % t]
        first = False
        # The eye is where vanilla's is: --stand-at takes the feet, the client
        # adds 1.62. The chat lines set the time the scene is taken at; the
        # capture waits for every section in range to be meshed (see
        # --settle-shot in apps/ov_voxel).
        command = [binary, "--connect=127.0.0.1:%d" % (PORT + 1),
                   "--username=OvOurs", "--width=%d" % WIDTH, "--height=%d" % HEIGHT,
                   "--radius=%d" % RENDER_DISTANCE, "--no-hud", "--no-sound",
                   # The vanilla scenes hold no entity (regions copied without
                   # their entities, no spawning); ours must not draw the body
                   # a dropped earlier connection left standing on the spot.
                   "--no-entities",
                   "--stand-at=%s,%s,%s,%s,%s" % (x, y, z, yaw, pitch),
                   "--chat-at=120", "--frame-ms=16",
                   "--frames=100000", "--settle-shot", "--settle-chunks=%d" % SETTLE_CHUNKS,
                   "--screenshot=" + ppm] + \
                  ["--chat=" + c for c in chat] + extra
        # Retried, because the server sometimes closes a connection mid-scene
        # ("the server closed the connection") and the run then ends with no
        # capture: three tries, and a missing file is reported, not hidden.
        code = None
        for attempt in range(3):
            if os.path.exists(ppm):
                os.remove(ppm)
            with open(log, "w") as f:
                try:
                    code = subprocess.run(command, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT,
                                          timeout=600).returncode
                except subprocess.TimeoutExpired:
                    code = "délai"
            # A capture taken short of the full square (the client logs "FEWER
            # than asked") is a scene that was cut off, not a scene: retried.
            with open(log) as f:
                short = "FEWER than asked" in f.read()
            # A capture at another size than the window asked for (the OS
            # gave the window another size) cannot be compared pixel for pixel.
            if os.path.exists(ppm):
                with open(ppm, "rb") as f:
                    head = f.read(32).split()
                if head[1:3] != [b"%d" % (WIDTH * 2), b"%d" % (HEIGHT * 2)]:
                    print("notre client %-14s : capture %sx%s, pas %dx%d" %
                          (name, head[1].decode(), head[2].decode(), WIDTH * 2, HEIGHT * 2))
                    short = True
            if short and os.path.exists(ppm):
                os.remove(ppm)
            if os.path.exists(ppm):
                break
            print("notre client %-14s : pas de capture%s (essai %d)"
                  % (name, " complète" if short else "", attempt + 1))
        print("notre client %-14s : %s -> %s%s" % (name, code, os.path.relpath(ppm, ROOT),
                                                  "" if os.path.exists(ppm) else " MANQUANTE"))


def run_perf(binaries, rounds, radius):
    """The M5 criterion, p99 of the frame at 12 chunks with no vsync, for each
    binary in turn — A B A B, so that a machine shared with other work drifts
    the same way for both. Offline viewer on the copied world, fixed camera
    over the plains, fixed noon: the same frames for every run."""
    import re
    world = os.path.join(OUT, "world-ours")
    if not os.path.isdir(world):
        prepare_world("world-ours")
    table = {b: [] for b in binaries}
    for r in range(rounds):
        for binary in binaries:
            # --at is in chunks: 48046, 26 is chunk (3002, 1).
            command = [binary, "--world=" + world, "--at=3002,1", "--radius=%d" % radius,
                       "--camera=48046.5,69.62,26.5,-90,15", "--time=6000", "--no-daylight-cycle",
                       "--no-vsync", "--no-sound", "--frames=900", "--width=%d" % WIDTH,
                       "--height=%d" % HEIGHT]
            out = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=1800)
            text = out.stdout + out.stderr
            cpu = re.search(r"cpu\s+p50 ([\d.]+) ms\s+p99 ([\d.]+) ms\s+max ([\d.]+) ms", text)
            gpu = re.search(r"gpu\s+p50 ([\d.]+) ms\s+p99 ([\d.]+) ms\s+max ([\d.]+) ms", text)
            row = (cpu.groups() if cpu else ("?",) * 3) + (gpu.groups() if gpu else ("?",) * 3)
            table[binary].append(row)
            print("tour %d %-40s cpu p50 %s p99 %s max %s | gpu p50 %s p99 %s max %s"
                  % ((r + 1, os.path.relpath(binary, ROOT)) + row))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("what", choices=("world", "vanilla", "ours", "all", "perf"))
    parser.add_argument("--perf-binaries", default="",
                        help="perf : les ov_voxel à comparer, séparés par des virgules")
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--radius", type=int, default=12)
    parser.add_argument("--tag", default="before")
    parser.add_argument("--only", default="")
    parser.add_argument("--timeout", type=int, default=1500)
    parser.add_argument("--extra", default="", help="arguments en plus pour ov_voxel, séparés par ;")
    parser.add_argument("--binary", default=os.path.join(BUILD, "ov_voxel"),
                        help="l'ov_voxel à mesurer (une copie du binaire « avant », par exemple)")
    parser.add_argument("--server", default="", help="l'ov_dedicated à lancer (une build release)")
    parser.add_argument("--scenes", default="",
                        help="un autre fichier de directives (par ex. render_parity_offsets.txt)")
    args = parser.parse_args()
    global SERVER, SCENES
    if args.server:
        SERVER = os.path.abspath(args.server)
    if args.scenes:
        SCENES = os.path.abspath(args.scenes)
    os.makedirs(OUT, exist_ok=True)
    if args.what == "perf":
        run_perf([os.path.abspath(b) for b in args.perf_binaries.split(",") if b], args.rounds,
                 args.radius)
        return 0
    if args.what == "world":
        prepare_world("world-vanilla")
        prepare_world("world-ours")
        return 0
    ops(["OvOracle", "OvOurs"])
    if args.what in ("vanilla", "all"):
        server = start_server("world-vanilla", PORT)
        try:
            run_vanilla(args.timeout)
        finally:
            stop(server)
    if args.what in ("ours", "all"):
        server = start_server("world-ours", PORT + 1)
        try:
            run_ours(args.tag, [s for s in args.only.split(",") if s],
                     [e for e in args.extra.split(";") if e], os.path.abspath(args.binary))
        finally:
            stop(server)
    return 0


if __name__ == "__main__":
    sys.exit(main())
