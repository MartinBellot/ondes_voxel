#!/usr/bin/env python3
"""Le coût d'un tick, sur le banc, avec un vrai client connecté.

Le scénario de `docs/provenance/branchement.md` § 4, rendu rejouable : le monde
`ov_lab` recopié, une sonde qui se connecte et ne fait rien, et la ligne
d'histogramme que le serveur imprime en s'arrêtant.

**Piège 22 du briefing.** `TickClock::advance()` avale les ticks manqués, donc
un tick raté ne produit qu'un seul échantillon : le `max` bat le p99 et les
mauvais échantillons sont rares même quand le serveur est inutilisable. Ce
script rapporte donc aussi l'**écart entre les tours de boucle et les ticks
d'horloge** — c'est lui qui dit combien de travail a réellement quitté le
thread, et il ne ment pas.

Usage :
    python3 scripts/bench_tick.py [--ticks=900] [--label=après] [--repeat=3]
"""
from __future__ import annotations

import os
import re
import shutil
import socket
import statistics
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ROOT = Path(__file__).resolve().parent.parent
PRESET = os.environ.get("OV_PRESET", "macos-release")
BINARY = ROOT / "build" / PRESET / "bin" / "ov_dedicated"
LAB_BUILDER = ROOT / "build" / PRESET / "bin" / "ov_lab"
LAB = Path(os.environ.get("OV_LAB_WORLD", ROOT / ".scratch" / "lab"))
WORLD = ROOT / "run" / "bench-tick"
PORT = int(os.environ.get("OV_BENCH_PORT", "25581"))

HISTOGRAM = re.compile(
    r"tick time over (\d+) ticks: p50 (\d+) us, p90 (\d+) us, p99 (\d+) us, "
    r"max (\d+) us, (\d+) over 50 ms")
STOPPED = re.compile(r"stopped after (\d+) ticks \((\d+) overload events\)")
PHASE = re.compile(
    r"(natural spawning|spawn chunk list rebuild) over (\d+) runs: p50 (\d+) us, "
    r"p90 (\d+) us, p99 (\d+) us, max (\d+) us, mean (\d+) us")


def run_once(ticks: int, extra: list[str]) -> dict:
    if WORLD.exists():
        shutil.rmtree(WORLD)
    shutil.copytree(LAB, WORLD)

    log_path = ROOT / "run" / "bench-tick.log"
    with open(log_path, "w") as log:
        server = subprocess.Popen(
            [str(BINARY), f"--world={WORLD}", f"--port={PORT}", f"--ticks={ticks}"] + extra,
            stdout=log, stderr=subprocess.STDOUT)

        for _ in range(300):
            time.sleep(0.1)
            try:
                with socket.create_connection(("127.0.0.1", PORT), timeout=0.2):
                    break
            except OSError:
                if server.poll() is not None:
                    raise RuntimeError("le serveur s'est arrêté avant d'écouter")
        else:
            raise RuntimeError("le serveur n'a jamais écouté")

        # Un client, connecté et immobile. Sans lui l'apparition ne fait rien du
        # tout — c'est la règle du jeu, et c'est ce qui a fait lire zéro partout
        # à une campagne entière (piège 11 du briefing).
        from vanilla_miner import Miner  # noqa: E402
        probe = Miner(PORT, "OndeBench")
        try:
            while server.poll() is None:
                probe.pump(timeout=0.5)
        except Exception:
            pass
        server.wait(timeout=120)

    out: dict = {}
    for line in open(log_path):
        found = HISTOGRAM.search(line)
        if found:
            out.update(samples=int(found.group(1)), p50=int(found.group(2)),
                       p90=int(found.group(3)), p99=int(found.group(4)),
                       max=int(found.group(5)), over=int(found.group(6)))
        found = STOPPED.search(line)
        if found:
            out.update(clock_ticks=int(found.group(1)), overloads=int(found.group(2)))
        found = PHASE.search(line)
        if found:
            out.setdefault("phases", {})[found.group(1)] = {
                "runs": int(found.group(2)), "p50": int(found.group(3)),
                "p90": int(found.group(4)), "p99": int(found.group(5)),
                "max": int(found.group(6)), "mean": int(found.group(7))}
    return out


def main() -> int:
    ticks = 900
    repeat = 1
    label = PRESET
    extra: list[str] = []
    for arg in sys.argv[1:]:
        if arg.startswith("--ticks="):
            ticks = int(arg.split("=", 1)[1])
        elif arg.startswith("--repeat="):
            repeat = int(arg.split("=", 1)[1])
        elif arg.startswith("--label="):
            label = arg.split("=", 1)[1]
        else:
            extra.append(arg)

    if not BINARY.exists():
        print(f"pas de binaire à {BINARY}")
        return 2
    if not LAB.exists():
        subprocess.run([str(LAB_BUILDER), f"--out={LAB}"], check=True)

    runs = [run_once(ticks, extra) for _ in range(repeat)]
    print(f"\n── {label} ({repeat} run{'s' if repeat > 1 else ''}, --ticks={ticks}) ────")
    print(f"  {'p50':>8} {'p90':>8} {'p99':>9} {'max':>9} {'>50ms':>7} "
          f"{'tours':>7} {'ticks':>7} {'perdus':>7}")
    for run in runs:
        lost = run.get("clock_ticks", 0) - run.get("samples", 0)
        print(f"  {run.get('p50', -1):>8} {run.get('p90', -1):>8} {run.get('p99', -1):>9} "
              f"{run.get('max', -1):>9} {run.get('over', -1):>7} "
              f"{run.get('samples', -1):>7} {run.get('clock_ticks', -1):>7} {lost:>7}")
    for run in runs:
        for name, phase in run.get("phases", {}).items():
            print(f"  {name:26} p50 {phase['p50']:>6} p90 {phase['p90']:>6} "
                  f"p99 {phase['p99']:>7} max {phase['max']:>7} moy {phase['mean']:>6} us "
                  f"({phase['runs']} fois)")
    if repeat > 1:
        for key in ("p50", "p90", "p99", "max", "over"):
            values = [r.get(key, 0) for r in runs]
            print(f"  médiane {key}: {statistics.median(values)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
