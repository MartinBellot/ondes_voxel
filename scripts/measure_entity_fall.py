#!/usr/bin/env python3
"""What a mob's fall is made of, read out of the game's own Motion tag.

The player's gravity and drag were fitted from a real client's position reports
(docs/PROVENANCE.md). Assuming a mob falls the same way would be a guess, and a
plausible one — which is the worst kind. So it is measured.

`data get entity <mob> Motion` prints the velocity vector as exact doubles. A
mob dropped from a great height with no AI is a pure fall, and its vertical
velocity follows

    v(0) = 0,   v(n+1) = (v(n) - g) * d

so v(n) = -g·d·(1 - dⁿ) / (1 - d). Sampling Motion many times during one long
fall gives a set of velocities whose tick numbers are unknown; a candidate
(g, d) predicts a curve, every sample is matched to its nearest point on it, and
the pair that minimises the residual is the answer. Two unknowns against forty
samples is overdetermined, so a wrong pair cannot fit.

Usage: python3 scripts/measure_entity_fall.py
"""
from __future__ import annotations

import math
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from measure_entities import Server  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / "run" / "entity-fall"

MOTION = re.compile(r"following entity data: \[([-0-9.dEe]+), ([-0-9.dEe]+), ([-0-9.dEe]+)\]")


def speed_at(g: float, d: float, n: int) -> float:
    """The closed form of v(n+1) = (v(n) - g)·d with v(0) = 0."""
    return -g * d * (1.0 - d ** n) / (1.0 - d)


def residual(samples: list[float], g: float, d: float) -> float:
    """Mean squared distance from each sample to the nearest point on the curve.

    The curve is strictly monotone, so the nearest point is found by inverting
    it rather than by scanning: n = log(1 + v(1-d)/(g·d)) / log(d), rounded to
    the tick it must have been. Scanning six hundred candidate ticks per sample
    is the obvious way to write this and is a thousand times slower.
    """
    total = 0.0
    for v in samples:
        arg = 1.0 + v * (1.0 - d) / (g * d)
        if arg <= 0.0:
            # This (g, d) cannot reach the observed speed at all: its terminal
            # velocity is slower than something that was actually seen.
            return float("inf")
        n = max(1, round(math.log(arg) / math.log(d)))
        total += (v - speed_at(g, d, n)) ** 2
    return total / max(1, len(samples))


def main() -> int:
    server = Server(RUN, port=25603)
    try:
        server.batch(["gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                      "gamerule sendCommandFeedback true", "difficulty normal",
                      "time set midnight", "forceload add -32 -32 32 32"])
        time.sleep(3.0)

        samples: dict[str, list[float]] = {}
        for name in ("minecraft:zombie", "minecraft:cow", "minecraft:armor_stand",
                     "minecraft:arrow", "minecraft:item"):
            extra = ',Item:{id:"minecraft:stone",Count:1b}' if name == "minecraft:item" else ""
            # Dropped from y = 300 in a superflat whose floor is at -60, so the
            # fall lasts about eleven seconds — long enough for the velocity to
            # get most of the way to its limit.
            # No NoAI here. On a Mob it does not merely switch the brain off, it
            # switches the *physics* off: a zombie summoned with NoAI:1b at
            # y = 300 stays at y = 300 forever with Motion flat zero. The first
            # run of this script measured nothing at all for every living mob
            # and reported "0 samples" — which is why the count is printed
            # rather than assumed.
            server.batch(['kill @e[tag=faller]',
                          f'summon {name} 0.5 300 0.5 '
                          f'{{Silent:1b,Invulnerable:1b,PersistenceRequired:1b,'
                          f'Tags:["faller"]{extra}}}'])
            observed: list[float] = []
            for _ in range(40):
                lines = server.batch(['data get entity @e[tag=faller,limit=1] Motion'])
                for line in lines:
                    match = MOTION.search(line)
                    if match:
                        observed.append(float(match.group(2).rstrip("d")))
                        break
                time.sleep(0.12)
            samples[name] = [v for v in observed if v < -0.0001]
            print(f"{name}: {len(samples[name])} samples, "
                  f"fastest {min(samples[name], default=0.0):.6f}")

        print()
        for name, observed in samples.items():
            if len(observed) < 5:
                print(f"{name}: too few samples to fit")
                continue
            best = None
            # A coarse sweep, then a fine one around the winner. The range is
            # wide on purpose: the point is to find out whether a mob falls like
            # a player, not to confirm that it does.
            for g in [x / 1000.0 for x in range(20, 161)]:
                for d in [0.90 + y / 1000.0 for y in range(0, 100)]:
                    r = residual(observed, g, d)
                    if best is None or r < best[0]:
                        best = (r, g, d)
            assert best is not None
            _, g0, d0 = best
            for g in [g0 + x / 100000.0 for x in range(-100, 101)]:
                for d in [d0 + y / 100000.0 for y in range(-100, 101)]:
                    r = residual(observed, g, d)
                    if r < best[0]:
                        best = (r, g, d)
            r, g, d = best
            terminal = -g * d / (1.0 - d)
            print(f"{name:24s} gravity {g:.5f}  drag {d:.5f}  "
                  f"rms {r ** 0.5:.2e}  terminal {terminal:.4f}")
    finally:
        server.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
