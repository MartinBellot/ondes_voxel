#!/usr/bin/env python3
"""Le chargement du terrain, mesuré comme le joueur le vit.

Le rapport qui a fait naître ce banc : « l'exploration est injouable, le client
affiche Loading terrain en permanence, les chunks autour du joueur ne sont pas
chargés ». Ce script mesure, sur une graine et un trajet fixes :

  * **l'entrée** : quand le serveur déclare le spawn prêt (son journal), quand la
    sonde entre, quand le carré de rayon 4 puis tout le carré de vue autour du
    spawn est arrivé ;
  * **le téléport** : la sonde se déplace de 1000 blocs d'un coup (un seul paquet
    de position — le serveur ne valide pas la vitesse) et chronomètre le
    remplissage des rayons 2, 4 et 8 autour de sa nouvelle position ;
  * **la course** : pour chaque vitesse (5,6 m/s au sprint, 11 m/s en vol créatif,
    33 m/s à l'élytre), la sonde vole en ligne droite à y = 200 et compte deux
    fois par seconde les chunks **manquants** dans les rayons 2, 4 et 8 autour
    d'elle — ce qu'un client verrait comme du vide. Chaque course commence après
    un téléport vers un terrain neuf et l'attente du carré complet ;
  * **le serveur** : son rapport d'arrêt — tick p50/p99, « can't keep up »,
    blocs générés, générations sur le thread de tick — et la charge de la
    machine (`uptime` avant et après, échantillonnage pendant).

Les chunks sont comptés par leurs paquets `Chunk Data` (0x24) et retirés par
`Unload Chunk` (0x1E) : c'est exactement ce que le client a, pas ce que le
serveur croit avoir envoyé.

Usage :
    python3 scripts/bench_stream.py --preset=macos-release --label=avant
    python3 scripts/bench_stream.py --binary=.scratch/before/ov_dedicated --label=avant
    python3 scripts/bench_stream.py --env OV_WORLDGEN_WORKERS=6 --speeds=11

Le monde vit sous `.scratch/bench-stream/` et est supprimé après lecture.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_play import MachineSampler, Player, parse_server_log, percentile  # noqa: E402
from vanilla_miner import read_varint, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SCRATCH = ROOT / ".scratch" / "bench-stream"

CB_UNLOAD_CHUNK = 0x1E
CB_DISCONNECT = 0x1A
CB_KEEP_ALIVE = 0x23
CB_CHUNK_DATA = 0x24
CB_SYNC_POSITION = 0x3C
SB_KEEP_ALIVE = 0x12
SB_CONFIRM_TELEPORT = 0x00
SB_SET_POSITION = 0x14


class Streamer(Player):
    """A probe that only keeps the set of chunks it holds, and when each came."""

    def __init__(self, port: int, name: str) -> None:
        super().__init__(port, name)
        self.have: set[tuple[int, int]] = set()
        self.arrivals = 0
        self.first_chunk_at: float | None = None

    def handle(self, pid: int, p: bytes) -> None:
        if pid == CB_CHUNK_DATA:
            cx, cz = struct.unpack_from(">ii", p, 0)
            self.have.add((cx, cz))
            self.arrivals += 1
            if self.first_chunk_at is None:
                self.first_chunk_at = self.last_arrival
        elif pid == CB_UNLOAD_CHUNK and len(p) >= 8:
            cx, cz = struct.unpack_from(">ii", p, 0)
            self.have.discard((cx, cz))
        elif pid == CB_KEEP_ALIVE:
            self.send(SB_KEEP_ALIVE, p[:8])
        elif pid == CB_SYNC_POSITION:
            x, y, z = struct.unpack_from(">ddd", p, 0)
            self.pos = (x, y, z)
            tid, _ = read_varint(p, 33)
            self.send(SB_CONFIRM_TELEPORT, varint(tid))
            self.send(SB_SET_POSITION, struct.pack(">ddd", x, y, z) + bytes([1]))
        elif pid == CB_DISCONNECT:
            raise EOFError("disconnected by the server")

    def missing(self, x: float, z: float, radius: int) -> int:
        cx, cz = int(x // 16), int(z // 16)
        return sum(1 for dz in range(-radius, radius + 1) for dx in range(-radius, radius + 1)
                   if (cx + dx, cz + dz) not in self.have)


def connect(port: int, patience: float) -> Streamer:
    deadline = time.monotonic() + patience
    while True:
        try:
            return Streamer(port, "StreamProbe")
        except (OSError, EOFError, ValueError):
            if time.monotonic() > deadline:
                raise
            time.sleep(0.25)


def hold(probe: Streamer, x: float, y: float, z: float, radii: list[int],
         timeout: float) -> dict[int, float | None]:
    """Stand still at (x, z) and time how long each radius takes to fill."""
    start = time.monotonic()
    filled: dict[int, float | None] = {r: None for r in radii}
    while time.monotonic() - start < timeout:
        probe.move(x, y, z)
        probe.drain(0.05)
        now = time.monotonic() - start
        for r in radii:
            if filled[r] is None and probe.missing(x, z, r) == 0:
                filled[r] = round(now, 2)
        if all(v is not None for v in filled.values()):
            break
    return filled


def run(probe: Streamer, x: float, y: float, z: float, speed: float, seconds: float,
        radii: list[int]) -> tuple[dict, float]:
    """Fly along +z at `speed` and sample the holes around the probe."""
    step = 0.05
    samples: dict[int, list[int]] = {r: [] for r in radii}
    own_missing = 0
    count = 0
    start = time.monotonic()
    next_sample = start
    while time.monotonic() - start < seconds:
        tick = time.monotonic()
        z += speed * step
        probe.move(x, y, z)
        if tick >= next_sample:
            next_sample += 0.5
            count += 1
            if probe.missing(x, z, 0):
                own_missing += 1
            for r in radii:
                samples[r].append(probe.missing(x, z, r))
        probe.drain(max(0.0, step - (time.monotonic() - tick)))
    out = {"speed": speed, "seconds": seconds, "samples": count,
           "own_chunk_missing_pct": round(100.0 * own_missing / max(1, count), 1)}
    for r in radii:
        v = samples[r]
        area = (2 * r + 1) ** 2
        out[f"r{r}"] = {"mean": round(sum(v) / max(1, len(v)), 1), "p90": percentile(v, 0.9),
                        "max": max(v) if v else None, "of": area,
                        "complete_pct": round(100.0 * sum(1 for m in v if m == 0) / max(1, len(v)),
                                              1)}
    return out, z


def uptime() -> str:
    return subprocess.run(["uptime"], capture_output=True, text=True).stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--preset", default=os.environ.get("OV_PRESET", "macos-release"))
    parser.add_argument("--binary", default=None, help="ov_dedicated to run")
    parser.add_argument("--seed", default="12345")
    parser.add_argument("--port", type=int, default=int(os.environ.get("OV_BENCH_PORT", "25619")))
    parser.add_argument("--speeds", default="5.6,11,33", help="m/s, comma-separated")
    parser.add_argument("--run-seconds", type=float, default=30.0)
    parser.add_argument("--radii", default="2,4,8")
    parser.add_argument("--fill-timeout", type=float, default=120.0)
    parser.add_argument("--patience", type=float, default=600.0, help="seconds to wait to join")
    parser.add_argument("--env", action="append", default=[], help="KEY=VALUE for the server")
    parser.add_argument("--label", default="run")
    parser.add_argument("--json", default=None, help="append the result to this JSON-lines file")
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    radii = [int(r) for r in args.radii.split(",")]
    speeds = [float(s) for s in args.speeds.split(",") if s]
    binary = Path(args.binary) if args.binary else ROOT / "build" / args.preset / "bin" / "ov_dedicated"

    SCRATCH.mkdir(parents=True, exist_ok=True)
    world = SCRATCH / f"world-{args.label}"
    shutil.rmtree(world, ignore_errors=True)
    world.mkdir(parents=True)
    env = dict(os.environ)
    env["OV_WORLDGEN_SEED"] = args.seed
    for pair in args.env:
        key, _, value = pair.partition("=")
        env[key] = value
    log_path = SCRATCH / f"server-{args.label}.log"

    result: dict = {"label": args.label, "binary": str(binary), "seed": args.seed,
                    "env": args.env, "uptime_before": uptime()}
    started = time.monotonic()
    with open(log_path, "w") as log:
        server = subprocess.Popen([str(binary), f"--world={world}", f"--port={args.port}"],
                                  stdout=log, stderr=subprocess.STDOUT, env=env)
        sampler = MachineSampler(server.pid)
        sampler.start()
        try:
            probe = connect(args.port, args.patience)
            joined = time.monotonic()
            result["join_s"] = round(joined - started, 2)
            while probe.pos is None:
                probe.drain(0.1)
            x, _, z = probe.pos
            y = 200.0
            fill = hold(probe, x, y, z, radii, args.fill_timeout)
            result["spawn_fill_s"] = fill
            result["first_chunk_after_join_s"] = (round(probe.first_chunk_at - joined, 3)
                                                  if probe.first_chunk_at else None)
            result["teleports"] = []
            result["runs"] = []
            for speed in speeds:
                x += 1000.0
                t0 = time.monotonic()
                received0 = probe.arrivals
                fill = hold(probe, x, y, z, radii, args.fill_timeout)
                took = time.monotonic() - t0
                result["teleports"].append({"fill_s": fill,
                                            "chunks_per_s": round((probe.arrivals - received0)
                                                                  / max(0.001, took), 1)})
                outcome, z = run(probe, x, y, z, speed, args.run_seconds, radii)
                result["runs"].append(outcome)
            result["chunks_received"] = probe.arrivals
            probe.s.close()
        except (OSError, EOFError, ValueError) as error:
            result["error"] = str(error) or type(error).__name__
        finally:
            sampler.stop.set()
            server.send_signal(signal.SIGINT)
            try:
                server.wait(timeout=120)
            except subprocess.TimeoutExpired:
                server.kill()
            sampler.join(timeout=5)
    text = log_path.read_text(errors="replace")
    result["uptime_after"] = uptime()
    result["machine"] = sampler.summary()
    result["server"] = parse_server_log(text)
    result["server"]["cant_keep_up"] = text.count("can't keep up")
    result["server"]["tree_bucket_errors"] = text.count("collided in one hash bucket")
    match = re.search(r"Spawn area ready in ([\d.]+) s", text)
    result["server"]["spawn_ready_s"] = float(match.group(1)) if match else None
    match = re.search(r"chunk source: (\d+) blocks generated \((\d+) chunks\)", text)
    if match:
        result["server"]["blocks_generated"] = int(match.group(1))
        result["server"]["chunks_generated"] = int(match.group(2))
    if not args.keep:
        shutil.rmtree(world, ignore_errors=True)

    show(result)
    if args.json:
        with open(args.json, "a") as handle:
            handle.write(json.dumps(result) + "\n")
    return 0


def show(result: dict) -> None:
    srv = result.get("server", {})
    print(f"\n── {result['label']} — {result['binary']} {' '.join(result['env'])}")
    print(f"  before: {result['uptime_before']}")
    if "error" in result:
        print(f"  ERROR: {result['error']}")
    print(f"  spawn ready {srv.get('spawn_ready_s')} s (server), probe in at {result.get('join_s')} s, "
          f"first chunk +{result.get('first_chunk_after_join_s')} s, "
          f"spawn square filled {result.get('spawn_fill_s')}")
    for index, tp in enumerate(result.get("teleports", [])):
        print(f"  teleport {index + 1}: filled {tp['fill_s']}  ({tp['chunks_per_s']} chunks/s received)")
    for run_ in result.get("runs", []):
        cells = "  ".join(f"r{r[1:]}: mean {v['mean']}/{v['of']} max {v['max']} "
                          f"complete {v['complete_pct']}%"
                          for r, v in run_.items() if r.startswith("r") and isinstance(v, dict))
        print(f"  {run_['speed']:5} m/s: own chunk missing {run_['own_chunk_missing_pct']}%  {cells}")
    tick = srv.get("tick", {})
    # Executed loop iterations against clock ticks: once the tick loop stops
    # swallowing late ticks, lag shows as ticks dropped, not as a jumping clock.
    print(f"  server: {srv.get('iterations')} loop iterations for {srv.get('clock_ticks')} clock "
          f"ticks ({srv.get('iterations_per_s')}/s), {srv.get('overload_events')} overload events")
    print(f"  server: tick p50 {tick.get('p50')} us p99 {tick.get('p99')} us max {tick.get('max')} us, "
          f"can't keep up {srv.get('cant_keep_up')}, generated {srv.get('chunks_generated')} chunks "
          f"in {srv.get('blocks_generated')} blocks, sync gen {srv.get('synchronous_generations')}, "
          f"tree-bucket errors {srv.get('tree_bucket_errors')}")
    mach = result.get("machine", {})
    if mach:
        print(f"  machine load {mach['load1_min']:.1f}–{mach['load1_max']:.1f} "
              f"(median {mach['load1_median']:.1f}), server cpu {mach['server_cpu_median']:.0f}% "
              f"rss {mach['server_rss_mb_max']:.0f} MB")
    print(f"  after:  {result['uptime_after']}")


if __name__ == "__main__":
    sys.exit(main())
