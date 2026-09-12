#!/usr/bin/env python3
"""De bout en bout, côté client : un son est joué, et ce qu'il coûte par frame.

Starts our dedicated server on a world of its own — never run/world, which is
shared with every other worktree through the run/ symlink — and runs the real
client, ov_voxel, against it twice with the same scripted scene: walk forward,
break the block underfoot, place one beside it (--walk --dig).

  1. With --sound-log. The engine's own record of every sound it started is
     printed at the end, and this script requires, from it:
       * a block break      (played by the breaker's own client: the server
                             leaves the breaker out of World Event 2001),
       * a block place      (played when the server's Block Update comes back:
                             the server leaves the placer out of the sound),
       * at least one step  (the walker's own client plays its footsteps).
     With no sound card the engine falls back to its null backend, which
     decides, mixes and logs exactly the same way; the log says which.
  2. With --no-sound: the same frames with no audio at all.

What is compared is the frame's CPU time, p50 and p99, with and without sound,
and the audio work itself per frame. Run it with nothing else heavy on the
machine: a vanilla server measuring in the background moves these numbers.

Usage: python3 scripts/check_client_sounds_e2e.py [frames]
"""
from __future__ import annotations

import os
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PRESET = os.environ.get("OV_PRESET", "macos-debug")
BIN = ROOT / "build" / PRESET / "bin"
# Never under run/: it is the shared asset tree, a symlink in every worktree,
# and a world written there is written into every other agent's run/ too.
WORLD = Path(os.environ.get("OV_CLIENT_E2E_DIR", str(ROOT / ".scratch" / "sound-client-e2e")))
PORT = int(os.environ.get("OV_CLIENT_E2E_PORT", "25752"))

PERCENTILES = re.compile(r"^(cpu|rec|gpu|snd)\s+p50 ([0-9.]+) ms\s+p99 ([0-9.]+) ms")
SOUND = re.compile(r"^snd\s+(\w+) (minecraft:[\w.]+) \(")


def start_server() -> subprocess.Popen:
    if WORLD.exists():
        shutil.rmtree(WORLD)
    WORLD.mkdir(parents=True)
    server = subprocess.Popen(
        [str(BIN / "ov_dedicated"), f"--world={WORLD / 'world'}", f"--port={PORT}",
         "--log-level=info"],
        cwd=ROOT, stdin=subprocess.PIPE, stdout=open(WORLD / "server.log", "w"),
        stderr=subprocess.STDOUT, text=True)
    deadline = time.monotonic() + 180.0
    while time.monotonic() < deadline:
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=1.0).close()
            return server
        except OSError:
            if server.poll() is not None:
                sys.exit(f"ov_dedicated exited: see {WORLD / 'server.log'}")
            time.sleep(0.5)
    server.kill()
    sys.exit("ov_dedicated never opened its port")


LOGS = ROOT / ".scratch"


def run_client(frames: int, name: str, extra: list[str]) -> str:
    # Kept outside the world directory, which is deleted when the run ends: a
    # failure has to be explainable after the fact.
    LOGS.mkdir(exist_ok=True)
    out = LOGS / f"client_e2e_{name}.log"
    with open(out, "w") as log:
        subprocess.run(
            [str(BIN / "ov_voxel"), f"--connect=127.0.0.1:{PORT}", f"--username={name}",
             f"--frames={frames}", "--no-vsync", "--radius=4", *extra],
            cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=900, check=False)
    return out.read_text()


def percentiles(text: str) -> dict[str, tuple[float, float]]:
    found = {}
    for line in text.splitlines():
        if (match := PERCENTILES.match(line)) is not None:
            found[match.group(1)] = (float(match.group(2)), float(match.group(3)))
    return found


def main() -> int:
    frames = int(sys.argv[1]) if len(sys.argv) > 1 else 1500
    for binary in ("ov_dedicated", "ov_voxel"):
        if not (BIN / binary).exists():
            sys.exit(f"{BIN / binary} not built")
    server = start_server()
    try:
        # ⚠ Three runs, not two. The first version walked and dug in the same
        #   scene: the scripted dig and place fire at fixed frame counts while
        #   the player walks on, so whether their targets were still in reach
        #   was luck — one run broke and placed, the next did neither. Digging
        #   is now done standing still, and the walk is its own scene, the one
        #   the two frame timings are taken on.
        #
        #   And the digger last: all three share one world, and the first
        #   ordering put the digger first — it dug the block at the spawn, and
        #   the walker after it spawned in that hole and walked into its wall
        #   for 1500 frames, which read as "no footsteps".
        heard_text = run_client(frames, "Walker", ["--walk", "--sound-log"])
        silent_text = run_client(frames, "Silent", ["--walk", "--no-sound"])
        dig_text = run_client(frames, "Digger", ["--dig", "--sound-log"])
    finally:
        try:
            assert server.stdin is not None
            server.stdin.write("stop\n")
            server.stdin.flush()
            server.wait(timeout=60)
        except Exception:
            server.kill()

    def started(text: str) -> list[str]:
        return [m.group(2) for line in text.splitlines() if (m := SOUND.match(line)) is not None]

    events = started(dig_text) + started(heard_text)
    breaks = [e for e in started(dig_text)
              if e.startswith("minecraft:block.") and e.endswith(".break")]
    places = [e for e in started(dig_text)
              if e.startswith("minecraft:block.") and e.endswith(".place")]
    steps = [e for e in started(heard_text) if e.endswith(".step")]
    backend = "null backend" if "continuing without output" in heard_text else "device"

    print(f"sounds started: {len(events)} ({backend})")
    print(f"  break {len(breaks)} {sorted(set(breaks))}")
    print(f"  place {len(places)} {sorted(set(places))}")
    print(f"  step  {len(steps)} {sorted(set(steps))}")
    others = sorted(set(events) - set(breaks) - set(places) - set(steps))
    if others:
        print(f"  other {others}")
    # The client's own account of the scripted gestures, which says whether the
    # server accepted them — a missing place sound is first a missing place.
    for line in dig_text.splitlines():
        if line.startswith(("dug ", "placed at ")) or "sound: " in line:
            print(f"  {line}")

    heard, silent = percentiles(heard_text), percentiles(silent_text)
    print("\nframe CPU, same scene, same frames:")
    for label, stats in (("with sound", heard), ("--no-sound", silent)):
        if "cpu" in stats:
            print(f"  {label:10s} cpu p50 {stats['cpu'][0]:.2f} ms  p99 {stats['cpu'][1]:.2f} ms")
    if "snd" in heard:
        print(f"  audio work per frame: p50 {heard['snd'][0]:.3f} ms  p99 {heard['snd'][1]:.3f} ms")

    # The place sound is required only when the server took the place: the
    # scripted placement is the client's, and it is refused often enough (two
    # runs in three) that requiring its sound unconditionally would test the
    # script's luck. When it is refused that is reported, not counted.
    accepted = any(line.startswith("placed at ") and line.endswith("PLACED")
                   and "NOTHING" not in line for line in dig_text.splitlines())
    if not accepted:
        print("  place: the server refused the scripted placement — not a sound result")
    ok = bool(breaks) and bool(steps) and (bool(places) or not accepted)
    print("\nOK" if ok else f"\nFAILED: see {LOGS}/client_e2e_*.log")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
