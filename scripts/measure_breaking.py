#!/usr/bin/env python3
"""Casser un bloc : le vrai client 1.20.1 et le nôtre, mesurés de la même façon.

Trois mesures, chacune contre l'oracle qui la porte :

  vanilla  Un vrai serveur 1.20.1 (tools/vanilla/server.jar) et, devant lui, un
           proxy qui enregistre chaque paquet. Le vrai client (l'instance
           PrismLauncher, pilotée par scripts/breaking_oracle.java) rejoint par
           le proxy et casse des colonnes de blocs, outil par outil : chaque
           Player Action et chaque Swing Arm est horodaté. Puis, sur le même
           serveur et par le même proxy, notre client (ov_voxel --mine) casse
           les mêmes colonnes : les deux calendriers se lisent de la même façon.
           L'oracle pose aussi les fissures (captures) et lit les particules.
  ours     Notre client contre notre serveur (ov_dedicated), les mêmes poses de
           fissures (ov_voxel --crack), capturées : aucun verrou nécessaire.
  compare  Les captures : pour chaque client, l'image avec fissure divisée par
           l'image sans, sur les mêmes pixels. L'éclairage et la résolution
           s'annulent ; restent la couverture et l'assombrissement.

Rien n'est commité : tout va sous data/vanilla/1.20.1/generated/breaking/
(gitignoré), ou sous --cache.

Un seul JVM à la fois sur la machine partagée :

    lockf /tmp/ov-vanilla.lock python3 scripts/measure_breaking.py vanilla
    python3 scripts/measure_breaking.py ours
    python3 scripts/measure_breaking.py compare
"""

import argparse
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import measure_creative_screen as base  # noqa: E402
from vanilla_miner import read_varint  # noqa: E402

DEFAULT_CACHE = os.path.join(ROOT, "data", "vanilla", "1.20.1", "generated", "breaking")
SERVER_PORT = int(os.environ.get("OV_BREAK_SERVER_PORT", "25646"))
PROXY_PORT = int(os.environ.get("OV_BREAK_PROXY_PORT", "25647"))
OURS_PORT = int(os.environ.get("OV_BREAK_OURS_PORT", "25648"))
VOXEL = os.environ.get("OV_VOXEL") or os.path.join(
    ROOT, "build", os.environ.get("OV_PRESET", "macos-debug"), "bin", "ov_voxel")
DEDICATED = os.path.join(ROOT, "build", os.environ.get("OV_PRESET", "macos-debug"), "bin",
                         "ov_dedicated")

TICK = 0.05

# Serverbound, protocol 763.
SB_CHAT_COMMAND = 0x04
SB_PLAYER_ACTION = 0x1D
SB_SET_HELD_ITEM = 0x28
SB_SWING_ARM = 0x2F
# Clientbound.
CB_ACK = 0x06
CB_UPDATE_TIME = 0x5E
CB_BLOCK_UPDATE = 0x0A


# ── The proxy ────────────────────────────────────────────────────────────────


def unpack_position(packed):
    """(x, z, y) of a packed position. Python's integers do not wrap at 64
    bits, so the C idiom of shifting left then right does not restore the sign:
    each field is sign-extended by hand."""
    packed &= (1 << 64) - 1
    x = (packed >> 38) & 0x3FFFFFF
    z = (packed >> 12) & 0x3FFFFFF
    y = packed & 0xFFF
    x = x - (1 << 26) if x >= 1 << 25 else x
    z = z - (1 << 26) if z >= 1 << 25 else z
    y = y - (1 << 12) if y >= 1 << 11 else y
    return x, z, y


def read_string(payload, i):
    n, i = read_varint(payload, i)
    return payload[i:i + n].decode("utf-8", "replace"), i + n


class Link:
    """One client's connection: two pumps, and a frame parser on each."""

    def __init__(self, number, client, server, records, lock):
        self.number = number
        self.client = client
        self.server = server
        self.records = records
        self.lock = lock
        self.state = "handshake"
        self.threshold = None
        self.name = "?"

    def parse(self, direction, frame):
        body = frame
        if self.threshold is not None:
            size, i = read_varint(body, 0)
            if size == 0:
                body = body[i:]
            elif direction == "s2c" and size > 256:
                return  # a big clientbound packet: chunks, never what is measured
            else:
                body = zlib.decompress(body[i:])
        packet_id, i = read_varint(body, 0)
        payload = body[i:]
        now = time.monotonic()
        if direction == "c2s" and self.state != "play":
            # Before play too: a packet sent between Login Success and the
            # proxy's own switch of state is still a packet the server reads.
            with self.lock:
                self.records.append({"kind": "c2s", "state": self.state, "id": packet_id,
                                     "size": len(payload), "head": payload[:16].hex(),
                                     "t": now, "link": self.number, "player": self.name})
        if self.state == "handshake" and direction == "c2s" and packet_id == 0x00:
            _, j = read_varint(payload, 0)           # protocol
            _, j = read_string(payload, j)           # address
            j += 2                                   # port
            nxt, _ = read_varint(payload, j)
            self.state = "login" if nxt == 2 else "status"
            return
        if self.state == "login":
            if direction == "c2s" and packet_id == 0x00:
                self.name, _ = read_string(payload, 0)
            elif direction == "s2c" and packet_id == 0x03:
                self.threshold, _ = read_varint(payload, 0)
            elif direction == "s2c" and packet_id == 0x02:
                self.state = "play"
            return
        if self.state != "play":
            return
        record = None
        if direction == "c2s":
            # Every serverbound packet, id and size: what a server refuses is
            # found here (a vanilla server drops a client for one malformed
            # packet, and says only "Index 8 out of bounds for length 3").
            with self.lock:
                self.records.append({"kind": "c2s", "id": packet_id, "size": len(payload),
                                     "head": payload[:16].hex(), "t": now,
                                     "link": self.number, "player": self.name})
        if direction == "c2s" and packet_id == SB_PLAYER_ACTION:
            status, j = read_varint(payload, 0)
            packed = struct.unpack_from(">q", payload, j)[0]
            x, z, y = unpack_position(packed)
            face = payload[j + 8]
            sequence, _ = read_varint(payload, j + 9)
            record = {"kind": "action", "status": status, "pos": [x, y, z], "face": face,
                      "sequence": sequence}
        elif direction == "c2s" and packet_id == SB_SWING_ARM:
            record = {"kind": "swing"}
        elif direction == "c2s" and packet_id == SB_CHAT_COMMAND:
            text, _ = read_string(payload, 0)
            record = {"kind": "command", "text": text}
        elif direction == "c2s" and packet_id == SB_SET_HELD_ITEM:
            record = {"kind": "held", "slot": struct.unpack_from(">h", payload, 0)[0]}
        elif direction == "s2c" and packet_id == CB_UPDATE_TIME:
            record = {"kind": "time", "age": struct.unpack_from(">q", payload, 0)[0]}
        elif direction == "s2c" and packet_id == CB_ACK:
            record = {"kind": "ack", "sequence": read_varint(payload, 0)[0]}
        elif direction == "s2c" and packet_id == CB_BLOCK_UPDATE:
            packed = struct.unpack_from(">q", payload, 0)[0]
            x, z, y = unpack_position(packed)
            record = {"kind": "block", "pos": [x, y, z], "state": read_varint(payload, 8)[0]}
        if record is not None:
            record.update({"t": now, "link": self.number, "player": self.name})
            with self.lock:
                self.records.append(record)

    def pump(self, source, sink, direction):
        buffer = b""
        try:
            while True:
                data = source.recv(65536)
                if not data:
                    break
                # Clientbound bytes are parsed *before* they are passed on: the
                # client answers Login Success at once, and a proxy that still
                # believed it was in login would drop that answer unrecorded.
                if direction == "c2s":
                    sink.sendall(data)
                buffer += data
                while True:
                    length, shift, i = 0, 0, 0
                    complete = False
                    while i < len(buffer) and i < 5:
                        byte = buffer[i]
                        length |= (byte & 0x7F) << shift
                        shift += 7
                        i += 1
                        if not byte & 0x80:
                            complete = True
                            break
                    if not complete or len(buffer) < i + length:
                        break
                    frame, buffer = buffer[i:i + length], buffer[i + length:]
                    try:
                        self.parse(direction, frame)
                    except Exception as exc:  # noqa: BLE001 — a parse error must not cut the game
                        print("  proxy: %s frame not read (%s)" % (direction, exc), flush=True)
                if direction == "s2c":
                    sink.sendall(data)
        except OSError:
            pass
        finally:
            if buffer and direction == "c2s":
                # What never made a whole frame: a malformed length is here.
                with self.lock:
                    self.records.append({"kind": "c2s-leftover", "size": len(buffer),
                                         "head": buffer[:48].hex(), "t": time.monotonic(),
                                         "link": self.number, "player": self.name,
                                         "state": self.state, "threshold": self.threshold})
            for s in (source, sink):
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

    def start(self):
        threading.Thread(target=self.pump, args=(self.client, self.server, "c2s"), daemon=True).start()
        threading.Thread(target=self.pump, args=(self.server, self.client, "s2c"), daemon=True).start()


class Proxy(threading.Thread):
    def __init__(self, listen_port, target_port):
        super().__init__(daemon=True)
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", listen_port))
        self.listener.listen(4)
        self.target_port = target_port
        self.records = []
        self.lock = threading.Lock()
        self.links = 0

    def run(self):
        while True:
            try:
                client, _ = self.listener.accept()
            except OSError:
                return
            server = socket.create_connection(("127.0.0.1", self.target_port))
            for s in (client, server):
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.links += 1
            Link(self.links, client, server, self.records, self.lock).start()


# ── The vanilla server ───────────────────────────────────────────────────────


class Server:
    def __init__(self, directory, port):
        shutil.rmtree(os.path.join(directory, "world"), ignore_errors=True)
        os.makedirs(directory, exist_ok=True)
        with open(os.path.join(directory, "eula.txt"), "w") as f:
            f.write("eula=true\n")
        with open(os.path.join(directory, "server.properties"), "w") as f:
            f.write("\n".join([
                "online-mode=false", "gamemode=survival", "level-type=minecraft\\:flat",
                "generate-structures=false", "view-distance=3", "simulation-distance=3",
                "spawn-protection=0", "spawn-monsters=false", "spawn-animals=false",
                "spawn-npcs=false", "difficulty=peaceful", "server-port=%d" % port,
                "max-players=4", ""]))
        self.log_path = os.path.join(directory, "server.log")
        self.log = open(self.log_path, "w")
        jar = os.path.join(ROOT, "tools", "vanilla", "server.jar")
        self.proc = subprocess.Popen([os.path.join(base.JAVA_HOME, "bin", "java"), "-Xmx900M",
                                      "-jar", jar, "nogui"], cwd=directory, stdout=self.log,
                                     stderr=subprocess.STDOUT, stdin=subprocess.PIPE)
        for _ in range(240):
            if base.port_open(port):
                return
            time.sleep(0.5)
        self.proc.kill()
        raise SystemExit("le serveur vanilla n'a pas ouvert le port %d" % port)

    def command(self, text):
        self.proc.stdin.write((text + "\n").encode())
        self.proc.stdin.flush()
        time.sleep(0.15)

    def log_text(self):
        with open(self.log_path, encoding="utf-8", errors="replace") as f:
            return f.read()

    def stop(self):
        try:
            self.proc.communicate(b"stop\n", timeout=60)
        except Exception:  # noqa: BLE001
            self.proc.kill()


def op_when_joined(server, name, stop):
    """Make `name` an operator as soon as the log says it joined."""
    while not stop.is_set():
        if "%s joined the game" % name in server.log_text():
            server.command("op %s" % name)
            return
        time.sleep(0.5)


# ── Our client, on the same server ───────────────────────────────────────────

# (label, block, tool, effect, count, ticks held)
OURS_DIGS = [
    # Three stones by hand: 151 + 6 + 152 + 6 + 152 ticks, and some to spare.
    ("stone.hand", "minecraft:stone", None, None, 3, 480),
    ("dirt.hand", "minecraft:dirt", None, None, 3, 70),
    ("stone.wooden_pickaxe", "minecraft:stone", "minecraft:wooden_pickaxe", None, 3, 90),
    ("planks.wooden_axe", "minecraft:oak_planks", "minecraft:wooden_axe", None, 3, 110),
    ("stone.wooden_pickaxe.haste2", "minecraft:stone", "minecraft:wooden_pickaxe",
     "minecraft:haste 1000 1", 3, 70),
]


def run_ours_dig(server, label, block, tool, effect, count, ticks, logs, name="OvVoxel"):
    """One ov_voxel run: posed in front of the column, the attack held."""
    log_path = os.path.join(logs, "ours-%s.log" % label)
    # Joins are counted, not looked for: the log keeps the earlier runs' lines,
    # and finding one of those sent a run's commands before its client was in.
    joins_before = server.log_text().count("%s joined" % name)
    command = [VOXEL, "--connect=127.0.0.1:%d" % PROXY_PORT, "--username=%s" % name,
               "--stand-at=0.5,-60,0.5,0,0", "--mine=600,%d" % ticks, "--no-hud",
               "--no-vsync"]
    with open(log_path, "w") as log:
        proc = subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    # Set the column once the client is in.
    # "X joined the game" on the real server, "X joined at (...)" on ours.
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline and \
            server.log_text().count("%s joined" % name) <= joins_before:
        time.sleep(0.5)
    time.sleep(3.0)  # the spawn teleport and the pose, before the column appears
    server.command("gamemode survival %s" % name)
    server.command("effect clear %s" % name)
    if effect:
        server.command("effect give %s %s true" % (name, effect))
    server.command("clear %s" % name)
    if tool:
        # /give after /clear lands in the first hotbar slot, the held one, on
        # both servers; ours does not parse `item replace ... hotbar.0`.
        server.command("give %s %s" % (name, tool))
    server.command("fill 0 -59 1 0 -59 9 minecraft:air")
    for i in range(count):
        server.command("setblock 0 -59 %d %s" % (2 + i, block))
    # Until every block of the column is claimed, or two minutes.
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline and proc.poll() is None:
        with open(log_path, encoding="utf-8", errors="replace") as f:
            if f.read().count("finish (") >= count:
                time.sleep(1.0)
                break
        time.sleep(0.5)
    proc.terminate()
    try:
        proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()
    server.command("kick %s" % name)
    time.sleep(2.0)


# ── The analysis ─────────────────────────────────────────────────────────────


def segments(records):
    """(label, player, records) for every ovmark pair, and one per ours-run link."""
    out = []
    by_link = {}
    for r in records:
        by_link.setdefault(r["link"], []).append(r)
    for link, rs in sorted(by_link.items()):
        label = None
        current = []
        marked = False
        for r in rs:
            if r["kind"] == "command" and r["text"].startswith("say ovmark "):
                marked = True
                mark = r["text"][len("say ovmark "):]
                if mark.endswith(".start"):
                    label, current = mark[:-6], []
                elif mark.endswith(".end") and label:
                    out.append((label, rs[0]["player"], current))
                    label = None
            elif label is not None:
                current.append(r)
        if not marked:
            out.append((rs[0]["player"], rs[0]["player"], rs))
    return out


def schedule(rs):
    """Digs, gaps and swings of one segment, in ticks.

    Two clocks. The wall clock (`tick`) is what the proxy sees, and on a loaded
    machine a late client runs several ticks in one frame and bunches its
    packets. The swing clock (`swing_ticks`) does not care: while the button is
    held the client sends exactly one Swing Arm a tick (measured, 1.00 to 1.02
    a tick), so the swings sent after a Start and up to its Finish count the
    ticks between them. Both are reported; the swing clock is the one to read.
    """
    events = [r for r in rs if r["kind"] in ("action", "swing")]
    swings = [r["t"] for r in events if r["kind"] == "swing"]
    digs, gaps, starts, swing_gaps, swing_periods = [], [], [], [], []
    pending = {}
    last_end = None
    swings_since_end = None
    swings_since_start = None
    for r in events:
        if r["kind"] == "swing":
            for key in pending:
                pending[key][1] += 1
            if swings_since_end is not None:
                swings_since_end += 1
            if swings_since_start is not None:
                swings_since_start += 1
            continue
        key = tuple(r["pos"])
        if r["status"] == 0:
            starts.append(r["t"])
            if swings_since_start is not None:
                swing_periods.append(swings_since_start)
            swings_since_start = 0
            if last_end is not None:
                gaps.append(round((r["t"] - last_end) / TICK))
                swing_gaps.append(swings_since_end)
            pending[key] = [r["t"], 0]
        elif r["status"] in (1, 2) and key in pending:
            t0, swung = pending.pop(key)
            # The Start's own tick is the first of the dig, as in
            # Breaking's "on the Nth tick" line: the swings after it count
            # the ticks that follow, the Finish's included.
            digs.append({"pos": list(key), "status": r["status"],
                         "tick": round((r["t"] - t0) / TICK) + 1, "swing_ticks": swung + 1})
            last_end = r["t"]
            swings_since_end = 0
    periods = [round((b - a) / TICK) for a, b in zip(starts, starts[1:])]
    swing_rate = None
    if len(swings) > 1:
        swing_rate = round(len(swings) / ((swings[-1] - swings[0]) / TICK + 1), 3)
    return {"digs": digs, "gaps_after_finish": gaps, "swing_gaps_after_finish": swing_gaps,
            "start_periods": periods, "swing_start_periods": swing_periods,
            "swings": len(swings), "swings_per_tick": swing_rate,
            "sequences": [r["sequence"] for r in events if r["kind"] == "action"]}


def read_particles(facts):
    rows, samples = [], []
    fields = None
    with open(facts, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line.startswith("particle-fields "):
                fields = line.split(" ", 1)[1].split(",")
            elif line.startswith("P,") and fields:
                parts = line.split(",")
                row = {"kind": parts[1], "trial": int(parts[2])}
                for name, value in zip(fields, parts[3:]):
                    row[name] = value == "true" if value in ("true", "false") else float(value)
                rows.append(row)
            elif line.startswith("S,"):
                parts = line.split(",", 4)
                samples.append({"trial": int(parts[1]), "ms": int(parts[2]), "alive": int(parts[3])})
    return rows, samples


def summarise_particles(rows, samples):
    import statistics
    out = {}
    for kind in sorted({r["kind"] for r in rows}):
        rs = [r for r in rows if r["kind"] == kind]
        per_trial = {}
        for r in rs:
            per_trial.setdefault(r["trial"], 0)
            per_trial[r["trial"]] += 1
        speed_h = [(r["xd"] ** 2 + r["zd"] ** 2) ** 0.5 for r in rs]
        out[kind] = {
            "count_per_call": sorted(set(per_trial.values())),
            "lifetime": [min(r["lifetime"] for r in rs), statistics.mean(r["lifetime"] for r in rs),
                         max(r["lifetime"] for r in rs)],
            "quad_size": [min(r["quadSize"] for r in rs), max(r["quadSize"] for r in rs)],
            "yd": [min(r["yd"] for r in rs), statistics.mean(r["yd"] for r in rs),
                   max(r["yd"] for r in rs)],
            "horizontal_speed": [min(speed_h), statistics.mean(speed_h), max(speed_h)],
            "gravity": sorted({r["gravity"] for r in rs}),
            "friction": sorted({r["friction"] for r in rs}),
            "colour": sorted({(r["rCol"], r["gCol"], r["bCol"]) for r in rs}),
            "bb_width": sorted({r["bbWidth"] for r in rs}),
        }
    gone = {}
    for s in samples:
        if s["alive"] == 0:
            gone.setdefault(s["trial"], s["ms"])
    out["destroy_all_gone_ms"] = sorted(gone.values())
    return out


def analyse(records):
    result = {"segments": []}
    for label, player, rs in segments(records):
        s = schedule(rs)
        s.update({"label": label, "player": player})
        result["segments"].append(s)
    return result


def report_segments(result):
    for s in result["segments"]:
        print("  %-28s %-9s digs(wall) %s  digs(swing) %s  gaps(swing) %s  periods(swing) %s  "
              "swings/tick %s" % (
                  s["label"], s["player"], [d["tick"] for d in s["digs"]],
                  [d["swing_ticks"] for d in s["digs"]], s["swing_gaps_after_finish"],
                  s["swing_start_periods"], s["swings_per_tick"]))


def cmd_analyse(args):
    """Re-read a saved capture: the analysis can change without a new run."""
    with open(os.path.join(args.cache, "breaking_records.json")) as f:
        records = json.load(f)
    result = analyse(records)
    report_segments(result)
    for link in sorted({r["link"] for r in records}):
        rs = [r for r in records if r["link"] == link and r["kind"] == "c2s"]
        if rs and rs[0]["player"] != "OvOracle":
            print("  link %d (%s), serverbound packets:" % (link, rs[0]["player"]))
            for r in rs[:60]:
                print("    0x%02X size %3d  %s" % (r["id"], r["size"], r["head"]))


def cmd_probe(args):
    """Our client alone on the real server, through the proxy: what it sends."""
    cache = args.cache
    os.makedirs(cache, exist_ok=True)
    server = Server(os.path.join(cache, "server"), SERVER_PORT)
    proxy = Proxy(PROXY_PORT, SERVER_PORT)
    proxy.start()
    log_path = os.path.join(cache, "probe-client.log")
    try:
        with open(log_path, "w") as log:
            port = SERVER_PORT if args.direct else PROXY_PORT
            proc = subprocess.Popen([VOXEL, "--connect=127.0.0.1:%d" % port,
                                     "--username=OvProbe", "--no-hud", "--frames=0"],
                                    cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        time.sleep(args.seconds)
        proc.terminate()
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()
    finally:
        server.stop()
    with proxy.lock:
        records = list(proxy.records)
    with open(os.path.join(cache, "probe_records.json"), "w") as f:
        json.dump(records, f)
    for r in records:
        if r["kind"] == "c2s":
            print("  0x%02X size %3d  %s" % (r["id"], r["size"], r["head"]))
    for line in server.log_text().splitlines():
        if "OvProbe" in line:
            print("  server: " + line)


def cmd_vanilla(args):
    cache = args.cache
    os.makedirs(cache, exist_ok=True)
    # The mappings and natives, into this cache rather than the creative one.
    base.CACHE = cache
    shared = os.path.join(ROOT, "data", "vanilla", "1.20.1", "generated", "creative-screen",
                          "client-mappings.txt")
    if os.path.exists(shared) and not os.path.exists(os.path.join(cache, "client-mappings.txt")):
        shutil.copyfile(shared, os.path.join(cache, "client-mappings.txt"))
    mappings = base.fetch_mappings()
    cp, natives = base.classpath_and_natives()

    classes = os.path.join(cache, "classes")
    staged = os.path.join(cache, "src", "ov")
    os.makedirs(classes, exist_ok=True)
    os.makedirs(staged, exist_ok=True)
    source = os.path.join(staged, "BreakingOracle.java")
    shutil.copyfile(os.path.join(ROOT, "scripts", "breaking_oracle.java"), source)
    subprocess.run([os.path.join(base.JAVA_HOME, "bin", "javac"), "-nowarn", "-d", classes, source],
                   check=True)

    game_dir = os.path.join(cache, "client")
    out_dir = os.path.join(game_dir, "oracle")
    shutil.rmtree(os.path.join(game_dir, "screenshots"), ignore_errors=True)
    base.write_options(game_dir, "faithful")

    server = Server(os.path.join(cache, "server"), SERVER_PORT)
    proxy = Proxy(PROXY_PORT, SERVER_PORT)
    proxy.start()
    stop = threading.Event()
    threading.Thread(target=op_when_joined, args=(server, "OvOracle", stop), daemon=True).start()
    command = [
        os.path.join(base.JAVA_HOME, "bin", "java"), "-XstartOnFirstThread", "-Xmx1500M",
        "-Dov.only=" + args.only,
        "-Djava.library.path=" + natives, "-Dorg.lwjgl.librarypath=" + natives,
        "-cp", os.pathsep.join([classes] + cp), "ov.BreakingOracle", mappings, out_dir,
        "--username", "OvOracle", "--version", "1.20.1", "--gameDir", game_dir,
        "--assetsDir", os.path.join(base.PRISM, "assets"), "--assetIndex", "5",
        "--uuid", "0f0e0d0c0b0a09080706050403020100", "--accessToken", "0",
        "--userType", "legacy", "--versionType", "release",
        "--width", "1280", "--height", "720",
        "--quickPlayMultiplayer", "127.0.0.1:%d" % PROXY_PORT,
    ]
    try:
        with open(os.path.join(game_dir, "client.log"), "w") as log:
            proc = subprocess.run(command, cwd=game_dir, stdout=log, stderr=subprocess.STDOUT,
                                  timeout=args.timeout)
        print("client vanilla : code %d" % proc.returncode)
        stop.set()
        server.command("kick OvOracle")
        time.sleep(2)
        if args.ours and os.path.exists(VOXEL):
            logs = os.path.join(cache, "ours-logs")
            os.makedirs(logs, exist_ok=True)
            for label, block, tool, effect, count, ticks in OURS_DIGS:
                print("notre client : %s" % label)
                run_ours_dig(server, label, block, tool, effect, count, ticks, logs)
    except subprocess.TimeoutExpired:
        print("client vanilla : délai dépassé (%d s)" % args.timeout)
    finally:
        stop.set()
        server.stop()

    with proxy.lock:
        records = list(proxy.records)
    with open(os.path.join(cache, "breaking_records.json"), "w") as f:
        json.dump(records, f)
    result = analyse(records)
    report_segments(result)
    facts = os.path.join(out_dir, "facts.txt")
    if os.path.exists(facts):
        rows, samples = read_particles(facts)
        if rows:
            result["particles"] = summarise_particles(rows, samples)
            print(json.dumps(result["particles"], indent=1))
    with open(os.path.join(cache, "breaking_vanilla.json"), "w") as f:
        json.dump(result, f, indent=1)
    print("écrit %s" % os.path.join(cache, "breaking_vanilla.json"))


# ── Our digs, against our own server ─────────────────────────────────────────


class OursServer:
    """ov_dedicated, driven through its console, with the log kept to read."""

    def __init__(self, directory, port):
        # A port someone else already listens on looks exactly like ours
        # coming up: the client joins a stranger's server and is thrown out.
        # Several agents run servers on this machine; refuse, and say so.
        if base.port_open(port):
            raise SystemExit("le port %d est déjà pris : OV_BREAK_OURS_PORT=<libre>" % port)
        shutil.rmtree(directory, ignore_errors=True)
        os.makedirs(directory, exist_ok=True)
        self.log_path = os.path.join(directory, "server.log")
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen([DEDICATED, "--world=%s" % os.path.join(directory, "world"),
                                      "--port=%d" % port, "--log-level=info"], cwd=ROOT,
                                     stdin=subprocess.PIPE, stdout=self.log,
                                     stderr=subprocess.STDOUT, text=True)
        for _ in range(360):
            if self.proc.poll() is not None:
                raise SystemExit("ov_dedicated s'est arrêté avant d'ouvrir le port %d : %s"
                                 % (port, self.log_path))
            if base.port_open(port):
                return
            time.sleep(0.5)
        self.proc.kill()
        raise SystemExit("ov_dedicated n'a pas ouvert le port %d" % port)

    def command(self, text):
        self.proc.stdin.write(text + "\n")
        self.proc.stdin.flush()
        time.sleep(0.15)

    def log_text(self):
        with open(self.log_path, encoding="utf-8", errors="replace") as f:
            return f.read()

    def stop(self):
        try:
            self.proc.communicate("stop\n", timeout=60)
        except Exception:  # noqa: BLE001
            self.proc.kill()


def cmd_ours_dig(args):
    """Our client's timed digs, on our server, through the recording proxy.

    The client's schedule does not depend on which server confirms the block:
    it claims on its own count. What the proxy reads — Player Actions and one
    Swing Arm a tick — is compared with the real client's on the swing clock.
    """
    cache = args.cache
    os.makedirs(cache, exist_ok=True)
    proxy = Proxy(PROXY_PORT, OURS_PORT)
    proxy.start()
    logs = os.path.join(cache, "ours-logs")
    os.makedirs(logs, exist_ok=True)
    chosen = [d for d in OURS_DIGS if not args.digs or d[0] in args.digs.split(",")]
    # A fresh server a scenario. On one server shared by every run, a client
    # reconnecting under the same name stopped receiving Block Updates: it
    # kept digging a block the server had already broken, and saw the next
    # scenario's column as the last one left it (docs/provenance/cassage-bloc.md).
    for label, block, tool, effect, count, ticks in chosen:
        print("notre client : %s" % label, flush=True)
        server = OursServer(os.path.join(cache, "ours-dig-server-" + label), OURS_PORT)
        try:
            run_ours_dig(server, label, block, tool, effect, count, ticks, logs)
        finally:
            server.stop()
    with proxy.lock:
        records = list(proxy.records)
    # One ov_voxel run per link, in the order run: name the segments after them.
    links = sorted({r["link"] for r in records})
    for link, (label, *_rest) in zip(links, chosen):
        for r in records:
            if r["link"] == link:
                r["player"] = "ours:" + label
    with open(os.path.join(cache, "breaking_records_ours.json"), "w") as f:
        json.dump(records, f)
    result = analyse(records)
    report_segments(result)
    with open(os.path.join(cache, "breaking_ours.json"), "w") as f:
        json.dump(result, f, indent=1)


# ── Our cracks, against our own server ───────────────────────────────────────

CRACK_BLOCKS = [("stone", "minecraft:stone"), ("planks", "minecraft:oak_planks"),
                ("glass", "minecraft:glass"), ("slab", "minecraft:stone_slab[type=bottom]")]


def cmd_ours(args):
    shots = os.path.join(args.cache, "ours-shots")
    os.makedirs(shots, exist_ok=True)
    world = os.path.join(args.cache, "ours-world")
    shutil.rmtree(world, ignore_errors=True)
    server = subprocess.Popen([DEDICATED, "--world=%s" % world, "--port=%d" % OURS_PORT,
                               "--log-level=info"], cwd=ROOT, stdin=subprocess.PIPE,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, text=True)
    try:
        for _ in range(360):
            if base.port_open(OURS_PORT):
                break
            time.sleep(0.5)
        # Noon, and it stays noon: a capture pair taken ten minutes apart
        # otherwise differs by the whole sky, and so did the first run's slab.
        server.stdin.write("gamerule doDaylightCycle false\ntime set noon\n")
        server.stdin.flush()
        for name, block in CRACK_BLOCKS:
            server.stdin.write("fill -3 -60 -3 3 -57 8 minecraft:air\n")
            server.stdin.write("setblock 0 -60 2 %s\n" % block)
            server.stdin.flush()
            time.sleep(1.0)
            for stage in (-1, 0, 4, 9):
                out = os.path.join(shots, "crack-%s-%s.ppm" % (name, "none" if stage < 0 else stage))
                command = [VOXEL, "--connect=127.0.0.1:%d" % OURS_PORT, "--username=OvShots",
                           "--stand-at=0.5,-60,0.5,0,30", "--no-hud", "--frames=2400",
                           "--screenshot=%s" % out]
                if stage >= 0:
                    command.append("--crack=%d" % stage)
                with open(out + ".log", "w") as log:
                    subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                                   timeout=300)
                print("  %s" % os.path.relpath(out, ROOT))
    finally:
        try:
            server.communicate("stop\n", timeout=60)
        except Exception:  # noqa: BLE001
            server.kill()


# ── Comparing the captures ───────────────────────────────────────────────────


def load_image(path):
    """(width, height, [luminance 0..1]) of a PNG (8-bit RGB/RGBA) or a binary PPM."""
    data = open(path, "rb").read()
    if data[:2] == b"P6":
        parts = data.split(maxsplit=4)
        w, h, pixels = int(parts[1]), int(parts[2]), parts[4]
        bpp = 3
    else:
        i, idat = 8, b""
        while i < len(data):
            n = struct.unpack(">I", data[i:i + 4])[0]
            kind, chunk = data[i + 4:i + 8], data[i + 8:i + 8 + n]
            if kind == b"IHDR":
                w, h, depth, colour = struct.unpack(">IIBB", chunk[:10])
            elif kind == b"IDAT":
                idat += chunk
            i += 12 + n
        bpp = {2: 3, 6: 4}[colour]
        raw = zlib.decompress(idat)
        stride = w * bpp
        rows, prev, p = [], bytearray(stride), 0
        for _ in range(h):
            f, line = raw[p], bytearray(raw[p + 1:p + 1 + stride])
            p += 1 + stride
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                b, c = prev[x], (prev[x - bpp] if x >= bpp else 0)
                if f == 1:
                    line[x] = (line[x] + a) & 255
                elif f == 2:
                    line[x] = (line[x] + b) & 255
                elif f == 3:
                    line[x] = (line[x] + (a + b) // 2) & 255
                elif f == 4:
                    pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                    line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
            rows.append(bytes(line))
            prev = line
        pixels = b"".join(rows)
    lum = []
    for k in range(0, w * h * bpp, bpp):
        r, g, b = pixels[k], pixels[k + 1], pixels[k + 2]
        lum.append((0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0)
    return w, h, lum


# Inside the block's north face in both clients' captures (2560 x 1440, the
# same pose): away from the dropped stacks the real client's frames carry,
# which spin between two captures and would count as crack.
FACE = (1000, 620, 1560, 1000)


def crack_effect(none_path, stage_path):
    """Fraction of the face the crack darkens and brightens, and by how much."""
    w, h, a = load_image(none_path)
    w2, h2, b = load_image(stage_path)
    if (w, h) != (w2, h2):
        return None
    x0, x1 = int(FACE[0] * w / 2560), int(FACE[2] * w / 2560)
    y0, y1 = int(FACE[1] * h / 1440), int(FACE[3] * h / 1440)
    region_a = [a[y * w + x] for y in range(y0, y1) for x in range(x0, x1)]
    region_b = [b[y * w + x] for y in range(y0, y1) for x in range(x0, x1)]
    a, b = region_a, region_b
    w, h = x1 - x0, y1 - y0
    dark, bright, dark_ratio, bright_ratio = 0, 0, [], []
    for x, y in zip(a, b):
        if x < 0.02:
            continue
        r = y / x
        if r < 0.9:
            dark += 1
            dark_ratio.append(r)
        elif r > 1.1:
            bright += 1
            bright_ratio.append(r)
    n = w * h
    mean = (lambda v: round(sum(v) / len(v), 3) if v else None)
    return {"darkened_frac": round(dark / n, 5), "brightened_frac": round(bright / n, 5),
            "dark_ratio": mean(dark_ratio), "bright_ratio": mean(bright_ratio),
            "size": [w, h]}


def cmd_compare(args):
    vanilla_dir = os.path.join(args.cache, "client", "screenshots")
    ours_dir = os.path.join(args.cache, "ours-shots")
    result = {}
    for name, _ in CRACK_BLOCKS:
        for stage in (0, 4, 9):
            row = {}
            v_none = os.path.join(vanilla_dir, "crack-%s-none.png" % name)
            v_stage = os.path.join(vanilla_dir, "crack-%s-%d.png" % (name, stage))
            o_none = os.path.join(ours_dir, "crack-%s-none.ppm" % name)
            o_stage = os.path.join(ours_dir, "crack-%s-%d.ppm" % (name, stage))
            if os.path.exists(v_none) and os.path.exists(v_stage):
                row["vanilla"] = crack_effect(v_none, v_stage)
            if os.path.exists(o_none) and os.path.exists(o_stage):
                row["ours"] = crack_effect(o_none, o_stage)
            result["%s-%d" % (name, stage)] = row
            print("  %-10s %s" % ("%s-%d" % (name, stage), json.dumps(row)))
    with open(os.path.join(args.cache, "breaking_cracks.json"), "w") as f:
        json.dump(result, f, indent=1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("step", choices=("vanilla", "ours", "ours-dig", "compare", "analyse",
                                         "probe"))
    parser.add_argument("--seconds", type=float, default=45.0, help="probe : durée")
    parser.add_argument("--direct", action="store_true", help="probe : sans le proxy")
    parser.add_argument("--digs", default="", help="ours-dig : scénarios, séparés par des virgules")
    parser.add_argument("--cache", default=DEFAULT_CACHE)
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--only", default="", help="dig, crack, particle (vanilla)")
    parser.add_argument("--no-ours", dest="ours", action="store_false",
                        help="vanilla : ne pas faire casser notre client après le vrai")
    args = parser.parse_args()
    args.cache = os.path.abspath(args.cache)
    {"vanilla": cmd_vanilla, "ours": cmd_ours, "compare": cmd_compare,
     "analyse": cmd_analyse, "probe": cmd_probe, "ours-dig": cmd_ours_dig}[args.step](args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
