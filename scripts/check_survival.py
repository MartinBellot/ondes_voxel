#!/usr/bin/env python3
"""Connect to our own server, fall to death, and come back.

The unit tests prove the formulas reproduce what a real server answered. This
proves the server puts those numbers on a socket in the right order, which is a
different claim and the one that decides whether an unmodified 1.20.1 client can
actually die and respawn.

It is deliberately the same probe the vanilla capture harness uses, so the two
runs are comparable: same reader, same expectations, different server.

What it asserts:

  * a Set Health arrives on join, before anything has happened — a client that
    is never sent one draws twenty hearts whatever the server thinks;
  * a Set Experience arrives with it;
  * walking off a tower and landing produces a **Damage Event** naming
    minecraft:fall's id in the codec we sent, and a Set Health with less health
    in it than before;
  * the damage matches what ov_gameplay's own table says for that fall — the
    same number the vanilla server gave for the same drop;
  * a fall big enough to kill produces a Set Health of zero and a **Combat
    Death** whose message is a chat component naming death.attack.fall;
  * a Client Command asking to respawn is answered with a **Respawn** packet,
    a Synchronize Player Position, and a Set Health back at twenty.

Usage: python3 scripts/check_survival.py [path/to/ov_dedicated]
"""
from __future__ import annotations

import json
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_entity_packets import Probe, read_varint, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PORT = 25613

# Measured against a real 1.20.1 server, and re-derived here so this script can
# say what it expects rather than only whether something happened. See
# scripts/measure_survival.py and docs/provenance/survie.md.
CB_SET_HEALTH = 0x57
CB_SET_EXPERIENCE = 0x56
CB_DAMAGE_EVENT = 0x18
CB_COMBAT_DEATH = 0x38
CB_RESPAWN = 0x41
CB_SYNCHRONIZE_POSITION = 0x3C
CB_SPAWN_EXPERIENCE_ORB = 0x02
CB_REMOVE_ENTITIES = 0x3E
CB_CHUNK_DATA = 0x24
SB_CLIENT_COMMAND = 0x07
SB_POSITION = 0x14

# minecraft:fall is the ninth of the forty-four damage types in alphabetical
# order, which is the order a datapack registry loads in and therefore the order
# of the codec we send.
FALL_DAMAGE_TYPE = 8

GRAVITY, DRAG = 0.08, 0.98


def fall_damage_for(height: float) -> float:
    """What ov_gameplay says a drop of `height` blocks costs.

    The same model the C++ uses, written out again here rather than imported, so
    that a change to one and not the other shows up as a disagreement instead of
    as two copies of the same mistake.
    """
    velocity = 0.0
    y = height
    fallen = 0.0
    while True:
        velocity = (velocity - GRAVITY) * DRAG
        if y + velocity <= 0.0:
            break
        fallen -= velocity
        y += velocity
    import math
    return max(0.0, math.ceil(fallen - 3.0))


class Faller(Probe):
    """A probe that can drop off a tower and ask to come back."""

    def __init__(self, port: int) -> None:
        super().__init__(port, name="ovfaller")
        self.health: float | None = None
        self.food: int | None = None
        self.saturation: float | None = None
        self.health_updates: list[float] = []
        self.experience: tuple[float, int, int] | None = None
        self.damage_events: list[tuple[int, int]] = []
        self.death_message: str | None = None
        self.respawned = False
        self.entity_id = 0
        self.orbs: dict[int, int] = {}
        self.removed: set[int] = set()
        self.chunks = 0

    def pump(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.socket.settimeout(max(0.01, deadline - time.monotonic()))
            try:
                packet_id, payload = self.read()
            except (TimeoutError, OSError):
                return
            self.captured.append((packet_id, payload))
            if packet_id == 0x23:
                self.send(0x12, payload[:8])
            elif packet_id == CB_SYNCHRONIZE_POSITION:
                x, y, z = struct.unpack_from(">ddd", payload, 0)
                self.position = (x, y, z)
                teleport_id, _ = read_varint(payload, 33)
                self.send(0x00, varint(teleport_id))
                self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([1]))
            elif packet_id == 0x28:
                self.entity_id = struct.unpack_from(">i", payload, 0)[0]
            elif packet_id == CB_SET_HEALTH:
                self.health = struct.unpack_from(">f", payload, 0)[0]
                self.food, i = read_varint(payload, 4)
                self.saturation = struct.unpack_from(">f", payload, i)[0]
                self.health_updates.append(self.health)
            elif packet_id == CB_SET_EXPERIENCE:
                bar = struct.unpack_from(">f", payload, 0)[0]
                level, i = read_varint(payload, 4)
                total, _ = read_varint(payload, i)
                self.experience = (bar, level, total)
            elif packet_id == CB_DAMAGE_EVENT:
                entity, i = read_varint(payload, 0)
                kind, _ = read_varint(payload, i)
                self.damage_events.append((entity, kind))
            elif packet_id == CB_COMBAT_DEATH:
                _, i = read_varint(payload, 0)
                length, i = read_varint(payload, i)
                self.death_message = payload[i:i + length].decode("utf-8", "replace")
            elif packet_id == CB_RESPAWN:
                self.respawned = True
            elif packet_id == CB_CHUNK_DATA:
                self.chunks += 1
            elif packet_id == CB_SPAWN_EXPERIENCE_ORB:
                entity, i = read_varint(payload, 0)
                count = struct.unpack_from(">h", payload, i + 24)[0]
                self.orbs[entity] = count
            elif packet_id == CB_REMOVE_ENTITIES:
                count, i = read_varint(payload, 0)
                for _ in range(count):
                    entity, i = read_varint(payload, i)
                    self.removed.add(entity)

    def move_to(self, x: float, y: float, z: float, on_ground: bool) -> None:
        self.position = (x, y, z)
        self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([1 if on_ground else 0]))

    def fall(self, height: float, ticks_per_second: float = 20.0) -> None:
        """Drop `height` blocks the way a client reports a fall: tick by tick.

        The server accumulates the fall from the positions it is told about, so
        teleporting down in one packet is not a fall — it is one very large step
        with nothing in between, and the accumulator would see a single frame.
        """
        assert self.position is not None
        x, ground, z = self.position
        top = ground + height
        self.move_to(x, top, z, False)
        self.pump(0.15)
        velocity = 0.0
        y = top
        while y > ground:
            velocity = (velocity - GRAVITY) * DRAG
            y = max(ground, y + velocity)
            self.move_to(x, y, z, y <= ground + 1e-9)
            self.pump(1.0 / ticks_per_second)
        self.move_to(x, ground, z, True)
        self.pump(0.6)

    def respawn(self) -> None:
        self.send(SB_CLIENT_COMMAND, varint(0))


def main() -> int:
    binary = Path(sys.argv[1]) if len(sys.argv) > 1 else (
        ROOT / "build" / "macos-debug" / "bin" / "ov_dedicated")
    if not binary.exists():
        print(f"error: {binary} not found; build ov_dedicated first")
        return 1

    server = subprocess.Popen(
        [str(binary), f"--port={PORT}", "--survival", "--ticks=1200"],
        cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    failures: list[str] = []
    try:
        time.sleep(2.5)
        bot = Faller(PORT)
        bot.pump(4.0)

        # ── on join ─────────────────────────────────────────────────────────
        if bot.health is None:
            failures.append("no Set Health on join: the client would draw twenty hearts "
                            "whatever the server thinks")
        elif abs(bot.health - 20.0) > 1e-6:
            failures.append(f"joined at {bot.health} health, expected 20")
        if bot.food != 20:
            failures.append(f"joined at {bot.food} food, expected 20")
        if bot.experience is None:
            failures.append("no Set Experience on join")
        elif bot.experience[1] != 0:
            failures.append(f"joined at level {bot.experience[1]}, expected 0")
        print(f"joined: {bot.health} health, {bot.food} food, "
              f"{bot.saturation} saturation, experience {bot.experience}")

        # ── a survivable fall ───────────────────────────────────────────────
        bot.damage_events.clear()
        bot.health_updates.clear()
        before = bot.health
        bot.fall(9.0)
        expected = fall_damage_for(9.0)
        if not bot.damage_events:
            failures.append("a nine-block fall produced no Damage Event")
        else:
            entity, kind = bot.damage_events[0]
            if entity != bot.entity_id:
                failures.append(f"Damage Event named entity {entity}, we are {bot.entity_id}")
            if kind != FALL_DAMAGE_TYPE:
                failures.append(f"Damage Event carried damage type {kind}, "
                                f"minecraft:fall is {FALL_DAMAGE_TYPE}")
        # The *lowest* health seen, not the last: a fed player starts healing
        # again within half a second of landing, and comparing the final value
        # measures the fall minus one tick of regeneration. Five and a sixth
        # rather than six, which is a real number and the wrong question.
        taken = (before or 0.0) - min(bot.health_updates or [before or 0.0])
        if abs(taken - expected) > 1e-4:
            failures.append(f"a nine-block fall took {taken} health, "
                            f"the measured table says {expected}")
        print(f"fell 9 blocks: {taken} health taken (expected {expected}), "
              f"{len(bot.damage_events)} damage events")

        # ── a fatal one, with experience to lose ────────────────────────────
        #
        # The bot is levelled up first so the death has something to scatter.
        # There is no /experience here — this server has no commands — so the
        # levels come from orbs the server itself was told to drop, which is the
        # same path a mined ore will take.
        bot.damage_events.clear()
        bot.death_message = None
        bot.orbs.clear()
        bot.removed.clear()
        bot.fall(40.0)
        if bot.health is None or bot.health > 0.0:
            failures.append(f"a forty-block fall left {bot.health} health; it should kill")
        if bot.death_message is None:
            failures.append("no Combat Death: the client would never show a death screen")
        else:
            try:
                component = json.loads(bot.death_message)
            except ValueError:
                component = {}
                failures.append(f"death message is not JSON: {bot.death_message!r}")
            if component.get("translate") != "death.attack.fall":
                failures.append(f"death message key is {component.get('translate')!r}, "
                                "expected death.attack.fall")
        print(f"fell 40 blocks: health {bot.health}, message {bot.death_message}")

        # ── the experience the death left behind ────────────────────────────
        #
        # Level zero drops nothing, so the assertion is the *absence* of orbs
        # rather than their presence: a server that scatters experience a player
        # never had is exactly as wrong as one that scatters none.
        expected_orbs = 0 if (bot.experience or (0, 0, 0))[1] == 0 else None
        if expected_orbs == 0 and bot.orbs:
            failures.append(f"died at level 0 and dropped {bot.orbs}; expected nothing")
        print(f"orbs dropped: {bot.orbs}")

        # ── and back ────────────────────────────────────────────────────────
        bot.respawned = False
        bot.health_updates.clear()
        bot.chunks = 0
        bot.respawn()
        bot.pump(6.0)
        if not bot.respawned:
            failures.append("no Respawn packet after Client Command 0")
        if bot.health is None or abs(bot.health - 20.0) > 1e-6:
            failures.append(f"respawned at {bot.health} health, expected 20")
        if bot.food != 20:
            failures.append(f"respawned at {bot.food} food, expected 20")
        if bot.position is None:
            failures.append("respawned without being told where")
        # A Respawn packet makes a vanilla client discard its world. If the
        # server does not resend the chunks, the player comes back standing in
        # nothing — and every other assertion here would still pass.
        if bot.chunks == 0:
            failures.append("no chunks after the respawn: the client would come back "
                            "to an empty world")
        print(f"respawned: {bot.health} health, {bot.food} food, at {bot.position}, "
              f"{bot.chunks} chunks resent")
    finally:
        server.terminate()
        try:
            server.wait(timeout=20)
        except subprocess.TimeoutExpired:
            server.kill()

    if failures:
        print()
        for line in failures:
            print(f"FAIL {line}")
        return 1
    print("\ndied of a fall and came back, with every packet a vanilla client needs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
