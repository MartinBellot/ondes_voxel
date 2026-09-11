#!/usr/bin/env python3
"""Ce que coûte un geste de joueur : casser, poser, marcher — et combien de la
lenteur vient du serveur, combien de la machine.

Le rapport qui a fait naître ce banc : « quand je casse un bloc il y a 2 s de
délai et tout est super lent », sur un monde généré, pendant que huit autres
agents compilaient. Deux causes superposées, et ce script sert à les séparer :

  * **la latence de cassage**, mesurée comme le joueur la vit : l'instant où le
    paquet `Player Action` part, l'instant où le `Block Update` de cette
    position revient. Idem pour la pose (`Use Item On`). L'accusé de réception
    (`Acknowledge Block Change`) est chronométré à part : il part du thread
    réseau *avant* l'écriture, donc l'écart entre les deux est ce que coûte
    l'écriture elle-même (l'éclairage, avant ce banc) ;
  * **le rythme réel du serveur** : le nombre de tours de boucle par seconde,
    lu dans le rapport d'arrêt (`tick profile: … iterations/s`). Ce n'est pas
    le temps du monde envoyé au client — l'horloge avale les ticks manqués
    (piège 22 du briefing) —, c'est combien de fois le monde a réellement
    avancé ;
  * **les phases du tick**, lues dans le même rapport, avec leur part de CPU :
    une phase dont le temps mur dépasse de loin le temps CPU attendait ;
  * **la machine** pendant la mesure : charge (`uptime`), swap
    (`sysctl vm.swapusage`), pages sorties et rentrées (`vm_stat`), CPU et
    mémoire résidente du serveur (`ps`), toutes les deux secondes.

La sonde parle le protocole 763 (`vanilla_miner.Miner`). Elle lit les
heightmaps des paquets `Chunk Data` pour savoir où est le sol, marche en ligne
droite, et à intervalle fixe casse le bloc du dessus d'une colonne à deux blocs
devant elle, puis en pose un. Mode créatif : le cassage est instantané, donc
tout le délai mesuré est celui du serveur, jamais celui des règles de minage.

Usage :
    python3 scripts/bench_play.py --world=seed:12345 --seconds=60 --label=avant
    python3 scripts/bench_play.py --world=lab --preset=macos-release
    python3 scripts/bench_play.py --world=seed:12345 --workers=1 --nice=10

Les mondes vivent sous `.scratch/bench-play/` et sont supprimés après lecture.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import signal
import socket
import statistics
import struct
import subprocess
import sys
import threading
import time
import zlib
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from anvil_read import R, payload as nbt_payload  # noqa: E402
from vanilla_miner import Miner, block_pos, read_varint, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SCRATCH = ROOT / ".scratch" / "bench-play"
GENERATED = ROOT / "data" / "vanilla" / "1.20.1" / "generated" / "reports"

# ── Paquets du protocole 763 ────────────────────────────────────────────────
SB_PLAYER_ACTION = 0x1D
SB_USE_ITEM_ON = 0x31
SB_SET_POSITION = 0x14
SB_SET_CREATIVE_SLOT = 0x2B
SB_SET_HELD_ITEM = 0x28
SB_KEEP_ALIVE = 0x12
SB_CONFIRM_TELEPORT = 0x00

# 0x06, as `kAcknowledgeDig` in src/ov_protocol/include/ov/protocol/play.hpp.
# The first version read 0x05 and saw no accusé at all — while Block Updates,
# sent *after* them, arrived.
CB_ACK_BLOCK_CHANGE = 0x06
CB_BLOCK_UPDATE = 0x0A
CB_DISCONNECT = 0x1A
CB_KEEP_ALIVE = 0x23
CB_CHUNK_DATA = 0x24
CB_SYNC_POSITION = 0x3C

MIN_Y = -64


def decode_position(raw: bytes) -> tuple[int, int, int]:
    packed = struct.unpack_from(">q", raw, 0)[0]
    x = packed >> 38
    y = (packed << 52) % (1 << 64) >> 52
    z = (packed << 26) % (1 << 64) >> 38
    x = x - (1 << 26) if x >= (1 << 25) else x
    y = y - (1 << 12) if y >= (1 << 11) else y
    z = z - (1 << 26) if z >= (1 << 25) else z
    return x, y, z


def motion_blocking(chunk_payload: bytes) -> list[int] | None:
    """Les 256 hauteurs MOTION_BLOCKING d'un paquet Chunk Data, ou None.

    En 763 la racine NBT du paquet porte encore un nom (vide) : type, longueur
    du nom, puis la composée. 9 bits par colonne, sept colonnes par long, sans
    chevauchement ; la valeur est la première case libre, comptée depuis −64.
    """
    reader = R(chunk_payload)
    reader.i = 8  # chunk x, chunk z
    if reader.u1() != 10:
        return None
    reader.s()
    root = nbt_payload(reader, 10)
    longs = root.get("MOTION_BLOCKING")
    if not longs:
        return None
    heights = []
    for index in range(256):
        word = longs[index // 7] & 0xFFFFFFFFFFFFFFFF
        heights.append((word >> ((index % 7) * 9)) & 0x1FF)
    return heights


def state_id(block: str) -> int:
    with open(GENERATED / "blocks.json") as handle:
        report = json.load(handle)
    for state in report[block]["states"]:
        if state.get("default"):
            return state["id"]
    raise KeyError(block)


def item_id(item: str) -> int:
    with open(GENERATED / "registries.json") as handle:
        report = json.load(handle)
    return report["minecraft:item"]["entries"][item]["protocol_id"]


def percentile(values: list[float], q: float) -> float:
    if not values:
        return float("nan")
    ordered = sorted(values)
    return ordered[int(q * (len(ordered) - 1))]


# ── La machine pendant la mesure ────────────────────────────────────────────

class MachineSampler(threading.Thread):
    """Charge, swap, pagination et coût du serveur, toutes les deux secondes."""

    def __init__(self, pid: int) -> None:
        super().__init__(daemon=True)
        self.pid = pid
        self.samples: list[dict] = []
        self.stop = threading.Event()

    @staticmethod
    def _vm_stat() -> dict[str, int]:
        out = subprocess.run(["vm_stat"], capture_output=True, text=True).stdout
        counters = {}
        for line in out.splitlines():
            match = re.match(r'"?([^:"]+)"?:\s+(\d+)', line)
            if match:
                counters[match.group(1).strip()] = int(match.group(2))
        return counters

    def sample(self) -> dict:
        load = subprocess.run(["sysctl", "-n", "vm.loadavg"], capture_output=True,
                              text=True).stdout.split()
        swap = subprocess.run(["sysctl", "-n", "vm.swapusage"], capture_output=True,
                              text=True).stdout
        used = re.search(r"used = ([\d.]+)M", swap)
        ps = subprocess.run(["ps", "-o", "%cpu=,rss=", "-p", str(self.pid)],
                            capture_output=True, text=True).stdout.split()
        vm = self._vm_stat()
        return {
            "t": time.monotonic(),
            "load1": float(load[1]) if len(load) > 1 else float("nan"),
            "swap_used_mb": float(used.group(1)) if used else float("nan"),
            "pageouts": vm.get("Pageouts", 0),
            "swapins": vm.get("Swapins", 0),
            "swapouts": vm.get("Swapouts", 0),
            "server_cpu": float(ps[0]) if len(ps) == 2 else float("nan"),
            "server_rss_mb": int(ps[1]) / 1024 if len(ps) == 2 else float("nan"),
        }

    def run(self) -> None:
        while not self.stop.is_set():
            try:
                self.samples.append(self.sample())
            except (OSError, ValueError, IndexError):
                pass
            self.stop.wait(2.0)

    def summary(self) -> dict:
        s = self.samples
        if len(s) < 2:
            return {}
        page = 16384  # octets par page sur Apple Silicon
        return {
            "load1_min": min(x["load1"] for x in s),
            "load1_median": statistics.median(x["load1"] for x in s),
            "load1_max": max(x["load1"] for x in s),
            "swap_used_mb_start": s[0]["swap_used_mb"],
            "swap_used_mb_end": s[-1]["swap_used_mb"],
            "pageouts_mb": (s[-1]["pageouts"] - s[0]["pageouts"]) * page / 2**20,
            "swapins_mb": (s[-1]["swapins"] - s[0]["swapins"]) * page / 2**20,
            "swapouts_mb": (s[-1]["swapouts"] - s[0]["swapouts"]) * page / 2**20,
            "server_cpu_median": statistics.median(x["server_cpu"] for x in s),
            "server_rss_mb_max": max(x["server_rss_mb"] for x in s),
        }


# ── La sonde ────────────────────────────────────────────────────────────────

class Player(Miner):
    """Un joueur qui marche, casse et pose, et chronomètre chaque réponse."""

    def __init__(self, port: int, name: str) -> None:
        super().__init__(port, name)
        self.heights: dict[tuple[int, int], int] = {}   # colonne -> 1re case libre
        self.pending: dict[tuple[int, int, int], tuple[str, float]] = {}
        self.acks: dict[int, float] = {}
        self.sequence = 0
        self.latency = {"break": [], "place": []}
        self.ack_latency: list[float] = []
        self.lost = {"break": 0, "place": 0}
        self.chunks = 0

    def handle(self, pid: int, p: bytes) -> None:
        now = self.last_arrival
        if pid == CB_KEEP_ALIVE:
            self.send(SB_KEEP_ALIVE, p[:8])
        elif pid == CB_SYNC_POSITION:
            x, y, z = struct.unpack_from(">ddd", p, 0)
            self.pos = (x, y, z)
            tid, _ = read_varint(p, 33)
            self.send(SB_CONFIRM_TELEPORT, varint(tid))
            self.send(SB_SET_POSITION, struct.pack(">ddd", x, y, z) + bytes([1]))
        elif pid == CB_CHUNK_DATA:
            cx, cz = struct.unpack_from(">ii", p, 0)
            heights = motion_blocking(p)
            if heights is not None:
                self.chunks += 1
                for index, value in enumerate(heights):
                    self.heights[(cx * 16 + index % 16, cz * 16 + index // 16)] = MIN_Y + value
        elif pid == CB_ACK_BLOCK_CHANGE:
            sequence, _ = read_varint(p, 0)
            sent = self.acks.pop(sequence, None)
            if sent is not None:
                self.ack_latency.append((now - sent) * 1000)
        elif pid == CB_BLOCK_UPDATE and len(p) >= 8:
            where = decode_position(p)
            waiting = self.pending.pop(where, None)
            if waiting is not None:
                kind, sent = waiting
                self.latency[kind].append((now - sent) * 1000)
        elif pid == CB_DISCONNECT:
            raise EOFError("disconnected by the server")

    def read(self):
        """One frame, and never a byte consumed unless the whole frame is here.

        `Miner.read` takes the length prefix a byte at a time from the socket.
        With the short timeouts this loop needs, a timeout between two of
        those bytes throws the first ones away and the stream is out of step
        for good — every later "packet" is garbage. That is exactly what the
        first two runs of this bench showed: 8 chunks and no Block Update at
        all in 45 s. So the frame is parsed from the buffer, and the buffer is
        only cut once the frame is complete.
        """
        while True:
            length, shift, index = 0, 0, 0
            complete = False
            while index < len(self.buf) and index < 5:
                byte = self.buf[index]
                index += 1
                length |= (byte & 0x7F) << shift
                shift += 7
                if not byte & 0x80:
                    complete = True
                    break
            if complete and len(self.buf) >= index + length:
                data = self.buf[index:index + length]
                self.buf = self.buf[index + length:]
                self.last_arrival = time.monotonic()
                i = 0
                if self.threshold is not None:
                    size, i = read_varint(data, 0)
                    data = data[i:] if size == 0 else zlib.decompress(data[i:])
                    i = 0
                pid, i = read_varint(data, i)
                return pid, data[i:]
            chunk = self.s.recv(65536)  # a timeout here leaves the buffer intact
            if not chunk:
                raise EOFError
            self.buf += chunk

    def drain(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return
            self.s.settimeout(max(0.001, left))
            try:
                pid, p = self.read()
            except (socket.timeout, TimeoutError):
                return
            self.handle(pid, p)

    def move(self, x: float, y: float, z: float) -> None:
        self.pos = (x, y, z)
        self.send(SB_SET_POSITION, struct.pack(">ddd", x, y, z) + bytes([1]))

    def break_block(self, x: int, y: int, z: int) -> None:
        self.sequence += 1
        now = time.monotonic()
        self.pending[(x, y, z)] = ("break", now)
        self.acks[self.sequence] = now
        self.send(SB_PLAYER_ACTION,
                  varint(0) + block_pos(x, y, z) + bytes([1]) + varint(self.sequence))

    def place_on(self, x: int, y: int, z: int) -> None:
        self.sequence += 1
        now = time.monotonic()
        self.pending[(x, y + 1, z)] = ("place", now)
        self.acks[self.sequence] = now
        self.send(SB_USE_ITEM_ON,
                  varint(0) + block_pos(x, y, z) + varint(1) +
                  struct.pack(">fff", 0.5, 1.0, 0.5) + bytes([0]) + varint(self.sequence))

    def expire(self, older_than: float) -> None:
        now = time.monotonic()
        for where, (kind, sent) in list(self.pending.items()):
            if now - sent > older_than:
                del self.pending[where]
                self.lost[kind] += 1


def connect(port: int, name: str, patience: float) -> Player:
    """Réessaie tant que le serveur prépare son spawn (piège 29 du briefing)."""
    deadline = time.monotonic() + patience
    while True:
        try:
            return Player(port, name)
        except (OSError, EOFError, ValueError):
            if time.monotonic() > deadline:
                raise
            time.sleep(1.0)


def play(player: Player, seconds: float, walk: float, edit_rate: float, stone: int) -> None:
    player.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", 36) + bytes([1]) + varint(stone)
                + bytes([64, 0]))
    player.send(SB_SET_HELD_ITEM, struct.pack(">h", 0))

    # Le sol sous les pieds avant de commencer : sans lui, pas de cible.
    settle_until = time.monotonic() + 60
    while player.pos is None or not player.heights:
        player.drain(0.2)
        if time.monotonic() > settle_until:
            raise RuntimeError("no position or no chunk after 60 s")
    x, _, z = player.pos
    player.drain(2.0)

    step = 0.05
    next_edit = time.monotonic()
    edits = 0
    started = time.monotonic()
    while time.monotonic() - started < seconds:
        tick_start = time.monotonic()
        column = (int(x // 1), int(z // 1))
        ground = player.heights.get(column)
        ahead = player.heights.get((int((x + walk * step) // 1), column[1]))
        if walk > 0 and ahead is not None:
            x += walk * step
            ground = ahead
        if ground is not None:
            player.move(x, float(ground), z)

        if tick_start >= next_edit:
            next_edit += 1.0 / edit_rate
            target = (int(x // 1) + 2, int(z // 1) + (edits % 3) - 1)
            top = player.heights.get(target)
            if top is not None and top > MIN_Y:
                if edits % 2 == 0:
                    player.break_block(target[0], top - 1, target[1])
                    player.heights[target] = top - 1
                else:
                    player.place_on(target[0], top - 1, target[1])
                    player.heights[target] = top + 1
            edits += 1

        # Thirty seconds before an edit counts as lost. Ten was the first
        # choice, and the server before its fix answered most edits later than
        # that: a latency that long is a result to report, not a loss.
        player.expire(30.0)
        player.drain(max(0.0, step - (time.monotonic() - tick_start)))
    player.drain(5.0)
    player.expire(0.0)


# ── Le serveur ──────────────────────────────────────────────────────────────

def parse_server_log(text: str) -> dict:
    out: dict = {"phases": [], "network": {}}
    match = re.search(r"tick profile: (\d+) loop iterations, (\d+) clock ticks, ([\d.]+) s: "
                      r"([\d.]+) iterations/s", text)
    if match:
        out["iterations"] = int(match.group(1))
        out["clock_ticks"] = int(match.group(2))
        out["seconds"] = float(match.group(3))
        out["iterations_per_s"] = float(match.group(4))
    match = re.search(r"tick profile: whole tick p50 (\d+) us, p90 (\d+) us, p99 (\d+) us, "
                      r"max (\d+) us, mean (\d+) us, (\d+) over 50 ms, tick thread on cpu "
                      r"(\d+)% of wall", text)
    if match:
        keys = ["p50", "p90", "p99", "max", "mean", "over_50ms", "cpu_pct"]
        out["tick"] = dict(zip(keys, map(int, match.groups())))
    for match in re.finditer(
            r"tick phase ([a-z ]+): (\d+) runs, p50 (\d+) us, p90 (\d+) us, p99 (\d+) us, "
            r"max (\d+) us, mean (\d+) us, total (\d+) ms, cpu (\d+)%, (\d+) over 50 ms, "
            r"heaviest in (\d+) of (\d+) slow ticks", text):
        keys = ["runs", "p50", "p90", "p99", "max", "mean", "total_ms", "cpu_pct", "over_50ms",
                "heaviest_in_slow", "slow_ticks"]
        out["phases"].append({"name": match.group(1), **dict(zip(keys, map(int,
                                                                      match.groups()[1:])))})
    for match in re.finditer(
            r"network ([a-z ]+): (\d+) samples, p50 (\d+) us, p90 (\d+) us, p99 (\d+) us, "
            r"max (\d+) us, mean (\d+) us, (\d+) over 50 ms", text):
        keys = ["samples", "p50", "p90", "p99", "max", "mean", "over_50ms"]
        out["network"][match.group(1)] = dict(zip(keys, map(int, match.groups()[1:])))
    match = re.search(r"process: (\d+) involuntary and (\d+) voluntary context switches, "
                      r"(\d+) major and (\d+) minor page faults", text)
    if match:
        out["process"] = dict(zip(["involuntary", "voluntary", "major_faults", "minor_faults"],
                                  map(int, match.groups())))
    # The line every server has printed since before the profile: what a binary
    # without it (an older `main`) can still be compared on.
    match = re.search(r"tick time over (\d+) ticks: p50 (\d+) us, p90 (\d+) us, p99 (\d+) us, "
                      r"max (\d+) us, (\d+) over 50 ms", text)
    if match:
        keys = ["loop_iterations", "p50", "p90", "p99", "max", "over_50ms"]
        out["legacy_tick"] = dict(zip(keys, map(int, match.groups())))
    match = re.search(r"(\d+) generated on the tick thread", text)
    if match:
        out["synchronous_generations"] = int(match.group(1))
    match = re.search(r"stopped after (\d+) ticks \((\d+) overload events\)", text)
    if match:
        out["clock_ticks"] = out.get("clock_ticks", int(match.group(1)))
        out["overload_events"] = int(match.group(2))
    return out


def prepare_world(kind: str, preset: str, label: str) -> tuple[Path, dict]:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    world = SCRATCH / f"world-{label}"
    if world.exists():
        shutil.rmtree(world)
    env = dict(os.environ)
    if kind.startswith("seed:"):
        world.mkdir(parents=True)
        env["OV_WORLDGEN_SEED"] = kind.split(":", 1)[1]
    elif kind == "lab":
        lab = ROOT / ".scratch" / "lab"
        if not (lab / "region").exists():
            builder = ROOT / "build" / preset / "bin" / "ov_lab"
            subprocess.run([str(builder), f"--out={lab}"], check=True,
                           stdout=subprocess.DEVNULL)
        shutil.copytree(lab, world)
    else:
        raise SystemExit(f"--world must be seed:<n> or lab, not {kind!r}")
    return world, env


def run_once(args: argparse.Namespace, label: str) -> dict:
    binary = ROOT / "build" / args.preset / "bin" / "ov_dedicated"
    world, env = prepare_world(args.world, args.preset, label)
    if args.workers is not None:
        env["OV_WORLDGEN_WORKERS"] = str(args.workers)
    log_path = SCRATCH / f"server-{label}.log"
    command = [str(binary), f"--world={world}", f"--port={args.port}"]
    if args.nice:
        command = ["nice", "-n", str(args.nice)] + command

    burners = [subprocess.Popen(["yes"], stdout=subprocess.DEVNULL) for _ in range(args.burn)]
    result: dict = {"label": label, "world": args.world, "preset": args.preset,
                    "workers": args.workers, "nice": args.nice, "burn": args.burn,
                    "walk": args.walk, "edit_rate": args.edit_rate, "seconds": args.seconds}
    with open(log_path, "w") as log:
        server = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, env=env)
        sampler = MachineSampler(server.pid)
        sampler.start()
        try:
            joined = time.monotonic()
            player = connect(args.port, "BenchPlayer", patience=args.patience)
            result["join_s"] = round(time.monotonic() - joined, 1)
            played = time.monotonic()
            try:
                play(player, args.seconds, args.walk, args.edit_rate, item_id("minecraft:stone"))
            except (EOFError, OSError) as error:
                # A server too slow to answer drops the connection — that is a
                # result, not a crash of the bench: keep what was measured.
                result["disconnected_after_s"] = round(time.monotonic() - played, 1)
                result["disconnect"] = str(error) or type(error).__name__
                player.expire(0.0)
            result["latency_ms"] = {
                kind: {"n": len(v), "p50": percentile(v, 0.5), "p90": percentile(v, 0.9),
                       "p99": percentile(v, 0.99), "max": max(v) if v else float("nan")}
                for kind, v in player.latency.items()}
            a = player.ack_latency
            result["ack_ms"] = {"n": len(a), "p50": percentile(a, 0.5),
                                "p90": percentile(a, 0.9), "max": max(a) if a else float("nan")}
            result["lost"] = player.lost
            result["chunks_received"] = player.chunks
            player.s.close()
        finally:
            sampler.stop.set()
            server.send_signal(signal.SIGINT)
            try:
                server.wait(timeout=120)
            except subprocess.TimeoutExpired:
                server.kill()
            for burner in burners:
                burner.kill()
            sampler.join(timeout=5)
    result["machine"] = sampler.summary()
    result["server"] = parse_server_log(log_path.read_text(errors="replace"))
    if not args.keep:
        shutil.rmtree(world, ignore_errors=True)
    return result


def show(result: dict) -> None:
    lat = result.get("latency_ms", {})
    srv = result.get("server", {})
    mach = result.get("machine", {})
    print(f"\n── {result['label']} — {result['world']}, {result['preset']}, "
          f"workers={result['workers']}, nice={result['nice']}, burn={result['burn']}")
    for kind in ("break", "place"):
        v = lat.get(kind)
        if v:
            print(f"  {kind:5}  n={v['n']:3}  p50 {v['p50']:8.1f} ms  p90 {v['p90']:8.1f} ms  "
                  f"p99 {v['p99']:8.1f} ms  max {v['max']:8.1f} ms  lost {result['lost'][kind]}")
    ack = result.get("ack_ms")
    if ack and ack["n"]:
        print(f"  ack    n={ack['n']:3}  p50 {ack['p50']:8.1f} ms  p90 {ack['p90']:8.1f} ms  "
              f"max {ack['max']:8.1f} ms")
    if "iterations_per_s" in srv:
        tick = srv.get("tick", {})
        print(f"  server {srv['iterations_per_s']:.2f} iterations/s, tick p50 {tick.get('p50')} us "
              f"p99 {tick.get('p99')} us max {tick.get('max')} us, "
              f"{tick.get('over_50ms')} over 50 ms, cpu {tick.get('cpu_pct')}% of wall")
    elif "legacy_tick" in srv:
        tick = srv["legacy_tick"]
        print(f"  server (no profile) {tick['loop_iterations']} loop iterations for "
              f"{srv.get('clock_ticks')} clock ticks, tick p50 {tick['p50']} us p99 {tick['p99']} us "
              f"max {tick['max']} us, {tick['over_50ms']} over 50 ms")
    for phase in srv.get("phases", [])[:8]:
        print(f"    {phase['name']:17} total {phase['total_ms']:7} ms  p99 {phase['p99']:8} us  "
              f"max {phase['max']:8} us  cpu {phase['cpu_pct']:3}%  "
              f"heaviest in {phase['heaviest_in_slow']}/{phase['slow_ticks']} slow")
    for name, h in srv.get("network", {}).items():
        print(f"    network {name:16} p50 {h['p50']:7} us  p99 {h['p99']:8} us  max {h['max']:8} us")
    if "process" in srv:
        p = srv["process"]
        print(f"  process {p['involuntary']} involuntary switches, {p['major_faults']} major faults")
    if mach:
        print(f"  machine load {mach['load1_min']:.1f}–{mach['load1_max']:.1f} "
              f"(median {mach['load1_median']:.1f}), swap {mach['swap_used_mb_start']:.0f}→"
              f"{mach['swap_used_mb_end']:.0f} MB, swapins {mach['swapins_mb']:.0f} MB, "
              f"pageouts {mach['pageouts_mb']:.0f} MB, server cpu {mach['server_cpu_median']:.0f}% "
              f"rss {mach['server_rss_mb_max']:.0f} MB")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--preset", default=os.environ.get("OV_PRESET", "macos-debug"))
    parser.add_argument("--world", default="seed:12345", help="seed:<n> or lab")
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--walk", type=float, default=1.0, help="blocks per second along +x")
    parser.add_argument("--edit-rate", type=float, default=4.0, help="edits per second")
    parser.add_argument("--workers", type=int, default=None, help="OV_WORLDGEN_WORKERS")
    parser.add_argument("--nice", type=int, default=0, help="run the server under nice -n")
    parser.add_argument("--burn", type=int, default=0,
                        help="busy processes started beside the server, for a contention series")
    parser.add_argument("--port", type=int, default=int(os.environ.get("OV_BENCH_PORT", "25617")))
    parser.add_argument("--patience", type=float, default=240.0, help="seconds to wait to join")
    parser.add_argument("--label", default="run")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--keep", action="store_true", help="keep the world afterwards")
    parser.add_argument("--json", default=None, help="append results to this JSON-lines file")
    args = parser.parse_args()

    for index in range(args.repeat):
        label = args.label if args.repeat == 1 else f"{args.label}-{index + 1}"
        result = run_once(args, label)
        show(result)
        if args.json:
            with open(args.json, "a") as handle:
                handle.write(json.dumps(result) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
