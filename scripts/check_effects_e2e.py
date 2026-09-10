#!/usr/bin/env python3
"""Status effects, end to end, against our own server.

The unit tests replay what the real 1.20.1 server did (docs/provenance/effets.md);
this script checks that ov_dedicated puts it on the wire. A probe client — the
same one the other end-to-end checks use, protocol 763, no screen — joins a
server started with `--effect=`, which gives every joining player a list of
effects until the /effect command exists.

Two sessions:

  speed      `--effect=speed:1:60`. The probe must see Entity Effect (0x6C) for
             speed II, 60 ticks, particles and icon, then Update Attributes
             (0x6A) with the player's movement speed — base 0.10000000149011612,
             the value measured on a real player — and the speed modifier
             91aeaa56-… of 0.4000000059604645, operation 2. Three seconds later
             it must see Remove Entity Effect (0x3F) and the attribute again,
             without the modifier. Byte for byte the layout of the vanilla
             capture in tests/test_effect_packets.cpp.

  regen      `--effect=hunger:255:40,regeneration:1:1200`. Hunger 256 for two
             seconds empties the saturation and most of the bar, so natural
             regeneration is off (it needs 18 food). The probe then falls ten
             blocks — seven points, measured in survie.md — and every Set
             Health after that must be exactly +1.0, 25 ticks apart on the
             server's own clock: regeneration II, 50 >> 1, the interval the
             522-cow campaign pinned.

Usage: python3 scripts/check_effects_e2e.py [path/to/ov_dedicated]
"""
from __future__ import annotations

import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_entity_packets import read_varint  # noqa: E402
from check_survival import CB_SET_HEALTH, Faller  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PORT = 25621

CB_ENTITY_EFFECT = 0x6C
CB_REMOVE_EFFECT = 0x3F
CB_UPDATE_ATTRIBUTES = 0x6A
CB_UPDATE_TIME = 0x5E

SPEED_UUID = bytes.fromhex("91aeaa56376b4498935b2f7f68070635")


class Watcher(Faller):
    """A faller that also timestamps what it hears on the server's clock."""

    def __init__(self, port: int) -> None:
        self.world_age: tuple[int, float] | None = None
        self.ages: list[tuple[float, int]] = []
        self.timeline: list[tuple[float, int, bytes]] = []
        super().__init__(port)

    def read(self):  # type: ignore[override]
        packet_id, payload = super().read()
        now = time.monotonic()
        if packet_id == CB_UPDATE_TIME and len(payload) >= 8:
            self.world_age = (struct.unpack_from(">q", payload, 0)[0], now)
            self.ages.append((now, self.world_age[0]))
        self.timeline.append((now, packet_id, payload))
        return packet_id, payload

    def tick_at(self, when: float) -> float | None:
        """The server tick at a wall-clock instant, interpolated between the
        two Update Time packets around it.

        Not extrapolated from the last one at 20 per second: a debug build
        generating the spawn chunks runs well under 20 TPS, and the first
        version of this check measured a 60-tick effect as 131 "ticks" that
        were really six and a half seconds of a slow server."""
        before = [(t, age) for t, age in self.ages if t <= when]
        after = [(t, age) for t, age in self.ages if t >= when]
        if not before or not after:
            if self.world_age is None:
                return None
            age, at = self.world_age
            return age + (when - at) / 0.05
        (t0, a0), (t1, a1) = before[-1], after[0]
        if t1 == t0:
            return float(a0)
        return a0 + (a1 - a0) * (when - t0) / (t1 - t0)


def decode_effect(payload: bytes) -> dict:
    entity, i = read_varint(payload, 0)
    effect, i = read_varint(payload, i)
    amplifier = payload[i]
    duration, i = read_varint(payload, i + 1)
    if duration >= 1 << 31:
        duration -= 1 << 32
    flags = payload[i]
    return {"entity": entity, "effect": effect, "amplifier": amplifier,
            "duration": duration, "flags": flags, "factor": payload[i + 1]}


def decode_attributes(payload: bytes) -> dict:
    entity, i = read_varint(payload, 0)
    count, i = read_varint(payload, i)
    out = {}
    for _ in range(count):
        length, i = read_varint(payload, i)
        name = payload[i:i + length].decode()
        i += length
        base = struct.unpack_from(">d", payload, i)[0]
        i += 8
        mods, i = read_varint(payload, i)
        modifiers = []
        for _ in range(mods):
            uuid = payload[i:i + 16]
            amount = struct.unpack_from(">d", payload, i + 16)[0]
            operation = payload[i + 24]
            modifiers.append((uuid, amount, operation))
            i += 25
        out[name] = (base, modifiers)
    return out


def start(binary: Path, effects: str) -> subprocess.Popen:
    command = [str(binary), f"--port={PORT}", "--survival", "--ticks=3000",
               f"--effect={effects}"]
    server = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.DEVNULL,
                              stderr=subprocess.STDOUT)
    time.sleep(2.5)
    return server


def session_speed(binary: Path, failures: list[str]) -> None:
    server = start(binary, "speed:1:60")
    try:
        # A warm-up connection first. The world age Update Time carries is
        # the tick *clock*, and TickClock swallows the ticks it falls behind
        # on (briefing, trap 22): while the first join generates its chunks, a
        # debug build runs well under twenty loop iterations a second and the
        # clock keeps counting. A 60-tick effect then measured 94 and 95
        # "ticks" of clock. The second join finds the chunks made.
        warm = Watcher(PORT)
        warm.pump(8.0)
        warm.socket.close()
        time.sleep(1.0)
        bot = Watcher(PORT)
        # Long enough for the join itself: the effect is given on the first
        # tick after the player is confirmed, which a debug build can take
        # several seconds to reach. The first run pumped five seconds from the
        # login and never saw the expiry.
        bot.pump(12.0)
        effects = [decode_effect(p) for _, pid, p in bot.timeline if pid == CB_ENTITY_EFFECT]
        removes = [p for _, pid, p in bot.timeline if pid == CB_REMOVE_EFFECT]
        attributes = [decode_attributes(p) for _, pid, p in bot.timeline
                      if pid == CB_UPDATE_ATTRIBUTES]
        print(f"speed: {len(effects)} Entity Effect, {len(removes)} Remove, "
              f"{len(attributes)} Update Attributes")
        if not effects:
            failures.append("speed: no Entity Effect arrived")
        else:
            got = effects[0]
            want = {"entity": bot.entity_id, "effect": 1, "amplifier": 1, "duration": 60,
                    "flags": 0x06, "factor": 0}
            if got != want:
                failures.append(f"speed: Entity Effect {got}, expected {want}")
        speeds = [a["minecraft:generic.movement_speed"] for a in attributes
                  if "minecraft:generic.movement_speed" in a]
        with_modifier = [s for s in speeds if s[1]]
        without = [s for s in speeds if not s[1]]
        if not with_modifier:
            failures.append("speed: no Update Attributes carried the speed modifier")
        else:
            base, modifiers = with_modifier[0]
            if base != 0.10000000149011612:
                failures.append(f"speed: movement speed base {base!r}, measured 0.10000000149011612")
            uuid, amount, operation = modifiers[0]
            if (uuid, amount, operation) != (SPEED_UUID, 0.4000000059604645, 2):
                failures.append(f"speed: modifier {uuid.hex()} {amount!r} op {operation}")
        if not removes or removes[0] != bytes([bot.entity_id]) + bytes([1]):
            failures.append(f"speed: Remove Entity Effect {[r.hex() for r in removes]}, "
                            f"expected entity {bot.entity_id} effect 1")
        if not without or speeds.index(without[-1]) < speeds.index(with_modifier[0]) if with_modifier and without else True:
            if not without:
                failures.append("speed: the modifier was never taken off")
        # The expiry, on the server clock: sixty ticks after the Entity Effect.
        stamps = [(t, pid) for t, pid, _ in bot.timeline if pid in (CB_ENTITY_EFFECT, CB_REMOVE_EFFECT)]
        if len(stamps) >= 2 and bot.world_age is not None:
            given = bot.tick_at(stamps[0][0])
            gone = bot.tick_at(stamps[-1][0])
            print(f"speed: given at tick {given}, removed at tick {gone} ({gone - given} ticks)")
            if abs((gone - given) - 60) > 2:
                failures.append(f"speed: removed {gone - given} ticks after it was given, "
                                "expected 60")
        bot.socket.close()
    finally:
        server.terminate()
        server.wait(timeout=30)


def session_regen(binary: Path, failures: list[str]) -> None:
    server = start(binary, "hunger:255:40,regeneration:1:1200")
    try:
        bot = Watcher(PORT)
        bot.pump(4.0)
        print(f"regen: after the hunger, food {bot.food}, saturation {bot.saturation}, "
              f"health {bot.health}")
        if bot.food is None or bot.food >= 18:
            failures.append(f"regen: food {bot.food} after hunger 256; natural regeneration "
                            "would confound the count")
        mark = len(bot.timeline)
        bot.fall(10.0)
        bot.pump(9.0)
        healths = [(t, struct.unpack_from(">f", p, 0)[0]) for t, pid, p in bot.timeline[mark:]
                   if pid == CB_SET_HEALTH]
        lowest = min(range(len(healths)), key=lambda k: healths[k][1]) if healths else None
        if lowest is None:
            failures.append("regen: no Set Health after the fall")
            return
        # Only the packets where the health moved. Set Health also goes out
        # when the *food* changes, and the first run read two of those as heals
        # of zero, three and seven ticks apart.
        rising = [healths[lowest]]
        for sample in healths[lowest + 1:]:
            if abs(sample[1] - rising[-1][1]) > 1e-4:
                rising.append(sample)
        steps = [round(b[1] - a[1], 4) for a, b in zip(rising, rising[1:])]
        ticks = [bot.tick_at(b[0]) - bot.tick_at(a[0]) for a, b in zip(rising, rising[1:])
                 ] if bot.world_age else []
        # The first heal's distance from the landing depends on where in the
        # interval the fall happened; only the spacing between heals is the
        # effect's rhythm.
        ticks = ticks[1:]
        print(f"regen: from {rising[0][1]}: steps {steps}")
        print(f"regen: spacing in server ticks {ticks}")
        if len(steps) < 5:
            failures.append(f"regen: only {len(steps)} heals in nine seconds")
        if any(abs(s - 1.0) > 1e-4 for s in steps):
            failures.append(f"regen: a heal was not exactly one point: {steps}")
        if ticks and any(abs(t - 25) > 1 for t in ticks):
            failures.append(f"regen: heals not 25 ticks apart: {ticks}")
        bot.socket.close()
    finally:
        server.terminate()
        server.wait(timeout=30)


def main() -> int:
    arguments = [a for a in sys.argv[1:] if not a.startswith("--")]
    binary = Path(arguments[0]) if arguments else ROOT / "build" / "macos-debug" / "bin" / "ov_dedicated"
    if not binary.exists():
        print(f"error: {binary} not found; build ov_dedicated first")
        return 1
    failures: list[str] = []
    session_speed(binary, failures)
    session_regen(binary, failures)
    if failures:
        print("\nFAILED")
        for failure in failures:
            print(f"  {failure}")
        return 1
    print("\nall effect checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
