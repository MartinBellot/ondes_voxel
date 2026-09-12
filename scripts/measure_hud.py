#!/usr/bin/env python3
"""Le HUD : le vrai client 1.20.1 et le nôtre, sur le même serveur vanilla.

Les deux clients jouent `scripts/hud_scenes.txt` — les mêmes commandes, dans
le même ordre — contre un serveur vanilla 1.20.1 **neuf** (monde plat, survie,
opérateur) et reçoivent donc les mêmes octets : Set Health, les métadonnées de
l'air et du gel, Entity Effect, Boss Bar, Player Info Update… Tout écart entre
les deux captures est un écart de *notre* HUD.

    measure_hud.py vanilla   le vrai client (PrismLauncher, Faithful 32x), piloté
                             par scripts/hud_oracle.java
    measure_hud.py ours      notre client, `ov_voxel --hud-script`
    measure_hud.py ... --hardcore --scenes scripts/hud_scenes_hardcore.txt
                             le même serveur en hardcore

Un serveur java et un client : à passer dans la voie java
(`ovlane.sh java python3 scripts/measure_hud.py vanilla`), moins de 10 min.

Rien n'est lu ni traduit du code de Mojang (CLAUDE.md § 1). Rien n'est commité :
tout va sous data/vanilla/1.20.1/generated/hud/ (gitignoré).

Puis : scripts/compare_hud.py <vanilla> <ours>.
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
import uuid

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import measure_creative_screen as creative  # noqa: E402  (classpath, mappings, options)

CACHE = os.path.join(ROOT, "data", "vanilla", "1.20.1", "generated", "hud")
NAME = "OvOracle"
BIN = os.path.join(ROOT, "build", os.environ.get("OV_PRESET", "macos-debug"), "bin")


def offline_uuid(name):
    digest = bytearray(hashlib.md5(("OfflinePlayer:" + name).encode("utf-8")).digest())
    digest[6] = (digest[6] & 0x0F) | 0x30
    digest[8] = (digest[8] & 0x3F) | 0x80
    return str(uuid.UUID(bytes=bytes(digest)))


def port_open(port):
    with socket.socket() as s:
        s.settimeout(0.5)
        return s.connect_ex(("127.0.0.1", port)) == 0


def start_server(port, hardcore):
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
            "online-mode=false", "gamemode=survival", "force-gamemode=true",
            "hardcore=%s" % ("true" if hardcore else "false"),
            "level-type=minecraft\\:flat", "generate-structures=false",
            "view-distance=3", "simulation-distance=3", "spawn-protection=0",
            # spawn-animals=false also refuses a /summon'd horse (the first run's
            # mount scene: "No entity was found"); doMobSpawning false in the
            # scenes is what keeps the world empty.
            "spawn-monsters=false", "spawn-animals=true", "spawn-npcs=false",
            "difficulty=normal", "server-port=%d" % port, "max-players=2", ""]))
    jar = os.path.join(ROOT, "tools", "vanilla", "server.jar")
    log = open(os.path.join(server_dir, "server.log"), "w")
    proc = subprocess.Popen([os.path.join(creative.JAVA_HOME, "bin", "java"), "-Xmx800M", "-jar",
                             jar, "nogui"], cwd=server_dir, stdout=log, stderr=subprocess.STDOUT,
                            stdin=subprocess.PIPE)
    for _ in range(240):
        if port_open(port):
            return proc
        time.sleep(0.5)
    proc.kill()
    raise SystemExit("le serveur vanilla n'a pas ouvert le port %d" % port)


def stop_server(server):
    try:
        server.communicate(b"stop\n", timeout=60)
    except Exception:  # noqa: BLE001
        server.kill()
    shutil.rmtree(os.path.join(CACHE, "server", "world"), ignore_errors=True)


def run_vanilla(args, out):
    mappings = creative.fetch_mappings()
    cp, natives = creative.classpath_and_natives()
    classes = os.path.join(CACHE, "classes")
    staged = os.path.join(CACHE, "src", "ov")
    os.makedirs(classes, exist_ok=True)
    os.makedirs(staged, exist_ok=True)
    source = os.path.join(staged, "HudOracle.java")
    shutil.copyfile(os.path.join(ROOT, "scripts", "hud_oracle.java"), source)
    subprocess.run([os.path.join(creative.JAVA_HOME, "bin", "javac"), "-nowarn", "-d", classes,
                    source], check=True)

    game_dir = os.path.join(CACHE, "client-faithful")
    shutil.rmtree(os.path.join(game_dir, "screenshots"), ignore_errors=True)
    creative.write_options(game_dir, "faithful")
    with open(os.path.join(game_dir, "options.txt"), "a") as f:
        f.write('renderClouds:"false"\n')
    command = [
        os.path.join(creative.JAVA_HOME, "bin", "java"), "-XstartOnFirstThread", "-Xmx1500M",
        "-Djava.library.path=" + natives, "-Dorg.lwjgl.librarypath=" + natives,
        "-cp", os.pathsep.join([classes] + cp), "ov.HudOracle", mappings, out,
        os.path.abspath(args.scenes),
        "--username", NAME, "--version", "1.20.1", "--gameDir", game_dir,
        "--assetsDir", os.path.join(creative.PRISM, "assets"), "--assetIndex", "5",
        "--uuid", offline_uuid(NAME).replace("-", ""), "--accessToken", "0",
        "--userType", "legacy", "--versionType", "release",
        "--width", "1280", "--height", "720",
        "--quickPlayMultiplayer", "127.0.0.1:%d" % args.port,
    ]
    log_path = os.path.join(out, "client.log")
    with open(log_path, "w") as log:
        proc = subprocess.run(command, cwd=game_dir, stdout=log, stderr=subprocess.STDOUT,
                              timeout=args.timeout)
    print("client vanilla : code %d, journal %s" % (proc.returncode, log_path))
    shots = os.path.join(game_dir, "screenshots")
    if os.path.isdir(shots):
        dest = os.path.join(out, "screenshots")
        shutil.rmtree(dest, ignore_errors=True)
        shutil.move(shots, dest)


def run_ours(args, out):
    shots = os.path.join(out, "screenshots")
    shutil.rmtree(shots, ignore_errors=True)
    os.makedirs(shots)
    command = [
        os.path.join(BIN, "ov_voxel"), "--connect=127.0.0.1:%d" % args.port,
        "--username=" + NAME, "--width=1280", "--height=720", "--gui-scale=3",
        "--radius=2", "--no-sound", "--no-vsync",
        "--hud-script=" + os.path.abspath(args.scenes), "--hud-shots=" + shots,
    ] + args.extra
    log_path = os.path.join(out, "client.log")
    try:
        with open(log_path, "w") as log:
            proc = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                                  timeout=args.timeout)
        print("notre client : code %d, journal %s" % (proc.returncode, log_path))
    finally:
        # 11 MB a PPM, two a scene: kept as PNG (lossless), the PPM deleted.
        for name in sorted(os.listdir(shots)):
            if name.endswith(".ppm"):
                path = os.path.join(shots, name)
                ppm_to_png(path, path[:-4] + ".png")
                os.remove(path)


def ppm_to_png(src, dst):
    """A binary PPM, rewritten as an RGB PNG with the standard library."""
    import struct
    import zlib
    data = open(src, "rb").read()
    parts = data.split(b"\n", 3)
    width, height = map(int, parts[1].split())
    body = parts[3]
    stride = width * 3
    raw = b"".join(b"\x00" + body[y * stride:(y + 1) * stride] for y in range(height))

    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload
                + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    with open(dst, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("which", choices=("vanilla", "ours"))
    parser.add_argument("--scenes", default=os.path.join(ROOT, "scripts", "hud_scenes.txt"))
    parser.add_argument("--hardcore", action="store_true")
    parser.add_argument("--port", type=int, default=25621)
    parser.add_argument("--timeout", type=int, default=540)
    parser.add_argument("--tag", default="")
    parser.add_argument("extra", nargs="*", help="ours: options passed on to ov_voxel")
    args = parser.parse_args()

    label = args.which + ("-hardcore" if args.hardcore else "") + (("-" + args.tag) if args.tag else "")
    out = os.path.join(CACHE, label)
    os.makedirs(out, exist_ok=True)
    server = start_server(args.port, args.hardcore)
    try:
        if args.which == "vanilla":
            run_vanilla(args, out)
        else:
            run_ours(args, out)
    except subprocess.TimeoutExpired:
        print("délai dépassé (%d s)" % args.timeout)
    finally:
        stop_server(server)
    print("sortie : %s" % os.path.relpath(out, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
