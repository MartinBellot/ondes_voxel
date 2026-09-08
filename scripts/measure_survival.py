#!/usr/bin/env python3
"""Ask a real 1.20.1 server what it costs to stay alive.

Health, hunger and experience are the part of Minecraft with the most numbers
and the fewest of them written down anywhere Mojang publishes. Nutrition and
saturation live in Java code. So do the level costs, the exhaustion per action,
the fall formula and the length of the invulnerability window. None of that is
in the data generator's reports, and none of it is guessed here.

Every campaign below drives the real server and reads its answers back:

  fall          A cow with a two-hundred-point health bar is dropped from
                y = floor + h for h = 1..30 and its Health is read afterwards.
                Damage is the difference. The floor height is *measured first*
                by dropping one from a single block and seeing where it comes to
                rest, so an off-by-one in the superflat's layers cannot shift
                the whole table by one block.

  invuln        The window after a hit during which a second one is ignored.
                Console commands are not tick-accurate — two `damage` lines in
                one batch may land in the same tick or in two — so this campaign
                uses a **datapack** written by this script: a function runs its
                commands in one tick, and `schedule` places the second hit an
                exact number of ticks later. The pair is fired for gaps of 0..14
                ticks and the health lost says where the window ends.

                The "a stronger hit still lands" rule is measured the same way:
                4 then 9 in one tick, and 9 then 4 in one tick, are different
                functions and give different answers.

  xp            The cost of every level from 0 to 40. `experience set <n> levels`
                followed by `experience add 1 points` leaves XpP at exactly one
                over the level's cost, and XpP is a float with room to spare —
                1/249 survives the round trip. The cumulative total each cost
                table predicts is then fed back as raw points and the resulting
                XpLevel is checked, which is an independent test of the whole
                curve rather than of one entry.

  food         Nutrition and saturation of every candidate item. The player is
                pinned at foodLevel 10 with saturation 0 and full health (so
                natural regeneration cannot move either number underneath the
                measurement), handed one item, and made to use it. Nutrition is
                the food gained; saturation is what appeared, valid as long as
                it did not hit its own ceiling of the new food level — which is
                checked and reported per item rather than assumed.

                The candidate set is every item that is NOT also a block, which
                is a filter over the registries and not a guess: no edible item
                in 1.20.1 is a block item. That takes 1319 items down to ~330.

  exhaustion    What each action costs. The bot sprints a measured distance,
                breaks a measured number of blocks, and takes measured damage,
                with foodExhaustionLevel and foodSaturationLevel read on both
                sides. Exhaustion wraps at 4.0 into one point of saturation, so
                the total is reconstructed as 4·(saturation spent) + what is
                left rather than read off the counter directly.

  starve        Where starvation stops, per difficulty. The bot is set to
                foodLevel 0 at Health 12 and left alone; easy, normal and hard
                come to rest in different places.

  regen         How fast health comes back at food 18 with no saturation, and
                with saturation.

  oxygen        A cow is put underwater and its Air and Health are sampled.

  death_xp      The bot is set to level L and killed; the Value of every
                experience orb the death produced is summed.

  mining_xp     The bot breaks ore in survival and the orbs are counted, thirty
                times per ore, so the answer is a distribution and not one draw.

Usage: python3 scripts/measure_survival.py [--only fall,invuln,...] [out.json]

Writes data/vanilla/1.20.1/normalized/survival.json. Partial results are written
after every campaign, so a run that dies late still leaves what it learned.
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_entity_packets import Probe, read_varint, varint  # noqa: E402
from measure_entities import Server  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
GENERATED = ROOT / "data" / "vanilla" / "1.20.1" / "generated"
RUN = ROOT / "run" / "survival-oracle"
PORT = 25607
BOT = "ovsurvive"

DATA = re.compile(r"following entity data: (.*)$")
ORB_VALUE = re.compile(r"following entity data: (\d+)s?$")

# Serverbound ids this probe needs beyond the ones the capture harness already
# uses. Not taken on trust: every one of them is *verified by its effect* —
# sprinting that produces no exhaustion, or a use that eats nothing, means the
# id is wrong and the campaign says so instead of reporting zeros.
SB_CLIENT_COMMAND = 0x07
SB_PLAYER_ACTION = 0x1D
SB_PLAYER_COMMAND = 0x1E
SB_SET_HELD_ITEM = 0x28
SB_USE_ITEM = 0x32
SB_POSITION = 0x14

# Clientbound ids are NOT written here. Every one this project has ever taken
# from a summary was wrong, so the `packets` campaign derives them by changing
# one thing at a time on a real server and seeing which id carries the change.
# Everything downstream reads this dict, which is empty until it has been
# filled — a campaign that runs without it reports nothing rather than
# misreading a packet that happens to be the right length.
CB: dict[str, int | None] = {
    "set_health": None,
    "set_experience": None,
    "combat_death": None,
    "damage_event": None,
    "hurt_animation": None,
    "respawn": None,
}


# The bot, once it exists, so that campaigns which do not use it still keep it
# alive. This is not tidiness: a vanilla server drops a client that has not
# answered a keep-alive for thirty seconds, and the fall campaign spends two
# minutes dropping cows without touching the socket. The first full run died at
# the start of the campaign *after* that one, with an EOF and no explanation.
_BOT: "Bot | None" = None


def nap(seconds: float) -> None:
    """Wait, answering the bot's housekeeping if there is a bot."""
    if _BOT is None:
        time.sleep(seconds)
    else:
        _BOT.pump(seconds)


class Bot(Probe):
    """A probe that can also move, sprint, dig, eat and respawn."""

    def __init__(self, port: int, name: str = BOT) -> None:
        super().__init__(port, name=name)
        self.entity_id = 0
        self.health: float | None = None
        self.food: int | None = None
        self.saturation: float | None = None
        self.death_message: str | None = None
        self.dead = False
        self.xp_bar: float | None = None
        self.xp_level: int | None = None
        self.xp_total: int | None = None

    def pump(self, seconds: float) -> None:
        """Same as the base pump, plus the survival packets this needs."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.socket.settimeout(max(0.01, deadline - time.monotonic()))
            try:
                packet_id, payload = self.read()
            except (TimeoutError, OSError):
                return
            self.captured.append((packet_id, payload))
            if packet_id == 0x23:  # keep alive
                self.send(0x12, payload[:8])
            elif packet_id == 0x3C:  # synchronize position
                x, y, z = struct.unpack_from(">ddd", payload, 0)
                self.position = (x, y, z)
                teleport_id, _ = read_varint(payload, 33)
                self.send(0x00, varint(teleport_id))
                self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([1]))
            elif packet_id == 0x28:  # login (play)
                self.entity_id = struct.unpack_from(">i", payload, 0)[0]
            elif packet_id == CB["set_health"]:
                self.health, i = struct.unpack_from(">f", payload, 0)[0], 4
                food, i = read_varint(payload, i)
                self.food = food
                self.saturation = struct.unpack_from(">f", payload, i)[0]
                if self.health <= 0.0:
                    self.dead = True
            elif packet_id == CB["set_experience"]:
                self.xp_bar = struct.unpack_from(">f", payload, 0)[0]
                self.xp_level, i = read_varint(payload, 4)
                self.xp_total, _ = read_varint(payload, i)
            elif packet_id == CB["combat_death"]:
                _, i = read_varint(payload, 0)
                i += 4  # killer entity id, an int and not a varint
                length, i = read_varint(payload, i)
                self.death_message = payload[i:i + length].decode("utf-8", "replace")
                self.dead = True

    def move_to(self, x: float, y: float, z: float, on_ground: bool = True) -> None:
        self.position = (x, y, z)
        self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([1 if on_ground else 0]))

    def sprint(self, on: bool) -> None:
        # Player Command: entity id, action (3 start sprinting, 4 stop), horse
        # jump boost. Action ids are the protocol's own and are checked by the
        # exhaustion they produce.
        self.send(SB_PLAYER_COMMAND, varint(self.entity_id) + varint(3 if on else 4) + varint(0))

    def hold(self, slot: int) -> None:
        self.send(SB_SET_HELD_ITEM, struct.pack(">h", slot))

    def use_item(self, sequence: int) -> None:
        self.send(SB_USE_ITEM, varint(0) + varint(sequence))

    def dig(self, x: int, y: int, z: int, status: int, sequence: int) -> None:
        packed = ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)
        self.send(SB_PLAYER_ACTION,
                  varint(status) + struct.pack(">q", packed) + bytes([1]) + varint(sequence))

    def respawn(self) -> None:
        self.send(SB_CLIENT_COMMAND, varint(0))
        self.dead = False


def scalar(lines: list[str]) -> str | None:
    for line in lines:
        match = DATA.search(line)
        if match:
            return match.group(1).strip()
    return None


def number(lines: list[str]) -> float | None:
    raw = scalar(lines)
    if raw is None:
        return None
    return float(raw.rstrip("bsfdL").rstrip("bsfd"))


# ── Driving a player without being able to write its NBT ────────────────────
#
# `/data merge entity <player>` is refused by the vanilla server — "cannot
# modify player data" — so none of the obvious setup works. Reading is allowed,
# which is what the campaigns below rely on, and the state a measurement needs
# is reached through effects and damage instead:
#
#   food and saturation to full   minecraft:saturation, amplifier 9. One tick of
#                                 it feeds ten points at a saturation modifier
#                                 of 1.0, so both counters land on their ceiling
#                                 of twenty.
#   food down to a chosen level   minecraft:hunger, amplifier 255. It charges
#                                 exhaustion rather than food, which is exactly
#                                 what is wanted: saturation drains first and
#                                 the player arrives at the target with a
#                                 saturation of zero, which is the baseline
#                                 every food measurement needs.
#   health to full                minecraft:instant_health, amplifier 5 — 128
#                                 points of healing, so the ceiling is reached
#                                 whatever the player had.
#   health to a chosen value      full, then one `damage` of the difference with
#                                 minecraft:generic, whose own exhaustion is
#                                 0.0 in the data generator's output and so
#                                 cannot disturb a hunger measurement.


def read_player(server: Server, name: str = BOT) -> dict:
    """Health, food, saturation and exhaustion, in one round trip each."""
    out = {}
    for field, path in (("health", "Health"), ("food", "foodLevel"),
                        ("saturation", "foodSaturationLevel"),
                        ("exhaustion", "foodExhaustionLevel"),
                        ("air", "Air"), ("level", "XpLevel"), ("bar", "XpP"),
                        ("total", "XpTotal")):
        out[field] = number(server.batch([f"data get entity {name} {path}"]))
    return out


def fill_food(server: Server, bot: Bot) -> None:
    server.batch([f"effect give {BOT} minecraft:saturation 1 9 true"])
    bot.pump(0.4)
    server.batch([f"effect clear {BOT}"])
    bot.pump(0.2)


def heal(server: Server, bot: Bot) -> None:
    server.batch([f"effect give {BOT} minecraft:instant_health 1 5 true"])
    bot.pump(0.4)


def set_health(server: Server, bot: Bot, target: float) -> None:
    heal(server, bot)
    if target < 20.0:
        server.batch([f"damage {BOT} {20.0 - target} minecraft:generic"])
        bot.pump(0.4)


def drain_food(server: Server, bot: Bot, target: int, timeout: float = 30.0) -> float | None:
    """Bring foodLevel down to `target`, leaving saturation at zero."""
    food = number(server.batch([f"data get entity {BOT} foodLevel"]))
    if food is None:
        return None
    if food <= target:
        server.batch([f"effect clear {BOT}"])
        return food
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        # Two speeds. Amplifier 255 charges 1.28 exhaustion a tick, which is
        # about a third of a food point per tick and overshoots a target by two
        # or three; amplifier 15 charges 0.08 and lands on it. Starting slow
        # would take a minute, so the coarse one runs until the last few points.
        gap = food - target
        if gap > 4:
            server.batch([f"effect give {BOT} minecraft:hunger 2 255 true"])
            bot.pump(0.30)
        else:
            server.batch([f"effect give {BOT} minecraft:hunger 2 15 true"])
            bot.pump(0.20)
        food = number(server.batch([f"data get entity {BOT} foodLevel"]))
        if food is None or food <= target:
            break
    server.batch([f"effect clear {BOT}"])
    bot.pump(0.2)
    return food


def install_datapack(directory: Path, functions: dict[str, list[str]]) -> None:
    """Write a datapack of our own into the world, before it is ever loaded.

    A function is the only way to be sure of what happens in one tick: console
    commands are read between ticks and a batch of them can straddle two. This
    matters exactly once — for the invulnerability window — and getting it wrong
    there means measuring the console's timing rather than the game's.
    """
    pack = directory / "world" / "datapacks" / "ovprobe"
    (pack / "data" / "ovprobe" / "functions").mkdir(parents=True, exist_ok=True)
    (pack / "pack.mcmeta").write_text(json.dumps(
        {"pack": {"pack_format": 15, "description": "Ondes VOXEL measurement probe"}}))
    for name, body in functions.items():
        (pack / "data" / "ovprobe" / "functions" / f"{name}.mcfunction").write_text(
            "\n".join(body) + "\n")


# ── Campaigns ───────────────────────────────────────────────────────────────


def campaign_packets(server: Server, bot: Bot) -> dict:
    """Which id carries which survival packet, derived rather than recalled.

    One thing is changed at a time and the traffic that follows is recorded. The
    packet is then identified by a *payload* that could not be anything else —
    a float holding exactly the health that was just set, a varint holding the
    damage type that was just named — never by the id being the one a table said
    it would be. The id is the answer, so it cannot also be the question.

    Two of these need a differential rather than a single capture. Damage Event
    and Hurt Animation are both "an entity id then four bytes", so telling them
    apart by shape is impossible; hitting the player with two *different* damage
    types separates them, because only one of the two packets changes.
    """
    server.batch([f"gamemode survival {BOT}", "difficulty normal",
                  "gamerule naturalRegeneration false", "gamerule doImmediateRespawn false",
                  f"experience set {BOT} 0 levels", f"experience set {BOT} 0 points"])
    fill_food(server, bot)
    heal(server, bot)
    bot.pump(1.0)
    bot.drain()

    found: dict = {}

    # Health. Brought to a value nothing else on the wire happens to hold, by
    # taking exactly 6.5 points of a damage type whose own exhaustion is zero.
    for target, taken in ((13.5, 6.5), (11.25, 8.75)):
        heal(server, bot)
        bot.pump(0.8)
        bot.drain()
        server.batch([f"damage {BOT} {taken} minecraft:generic"])
        bot.pump(1.8)
        want = struct.pack(">f", target)
        for packet_id, payload in bot.drain():
            if len(payload) < 9 or payload[:4] != want:
                continue
            food, i = read_varint(payload, 4)
            if len(payload) != i + 4 or not 0 <= food <= 20:
                continue
            saturation = struct.unpack_from(">f", payload, i)[0]
            if not 0.0 <= saturation <= 20.0:
                continue
            CB["set_health"] = packet_id
            found["set_health"] = {
                "id": packet_id, "hex": payload.hex(),
                "health": target, "food": food, "saturation": saturation,
                "fields": "float health, varint food, float saturation"}
            break
        if CB["set_health"] is not None:
            break

    # Experience: float bar, varint level, varint total.
    server.batch([f"experience set {BOT} 7 levels", f"experience add {BOT} 1 points"])
    bot.pump(1.5)
    for packet_id, payload in bot.drain():
        if len(payload) < 6:
            continue
        bar = struct.unpack_from(">f", payload, 0)[0]
        level, i = read_varint(payload, 4)
        if level == 7 and 0.0 < bar < 1.0:
            total, j = read_varint(payload, i)
            if j == len(payload):
                CB["set_experience"] = packet_id
                found["set_experience"] = {"id": packet_id, "hex": payload.hex(),
                                           "bar": bar, "level": level, "total": total,
                                           "fields": "float bar, varint level, varint total"}
                break

    # Two hits of different types. Everything that is the same in both windows
    # is not about the damage type; the packet that differs is Damage Event.
    windows = {}
    for damage_type in ("minecraft:cactus", "minecraft:lightning_bolt"):
        heal(server, bot)
        bot.pump(1.0)
        bot.drain()
        server.batch([f"damage {BOT} 3 {damage_type}"])
        bot.pump(1.5)
        windows[damage_type] = [(pid, payload) for pid, payload in bot.drain()]
    found["hit"] = {name: [{"id": pid, "hex": payload.hex()} for pid, payload in window]
                    for name, window in windows.items()}
    a, b = windows["minecraft:cactus"], windows["minecraft:lightning_bolt"]
    by_id_a = {pid: payload for pid, payload in a}
    by_id_b = {pid: payload for pid, payload in b}
    for packet_id, payload in a:
        entity, i = read_varint(payload, 0)
        if entity != bot.entity_id or packet_id not in by_id_b:
            continue
        other = by_id_b[packet_id]
        if len(payload) - i != 4 or len(other) != len(payload):
            continue
        if payload != other:
            kind_a, _ = read_varint(payload, i)
            kind_b, _ = read_varint(other, i)
            CB["damage_event"] = packet_id
            found["damage_event"] = {
                "id": packet_id, "hex": payload.hex(),
                "cactus_type_id": kind_a, "lightning_type_id": kind_b,
                "fields": "varint entity, varint damage type, varint cause+1, "
                          "varint direct+1, bool has position"}
        else:
            CB["hurt_animation"] = packet_id
            found["hurt_animation"] = {"id": packet_id, "hex": payload.hex(),
                                       "fields": "varint entity, float yaw"}
    if CB["hurt_animation"] is None:
        found["hurt_animation"] = ("no packet of that shape was constant across two damage "
                                   "types: 1.20.1 sends Damage Event and nothing else")

    # Death, then the respawn the client has to ask for.
    bot.death_message = None
    server.batch([f"damage {BOT} 100 minecraft:generic_kill"])
    bot.pump(2.5)
    death_window = bot.drain()
    found["death"] = [{"id": pid, "hex": payload.hex()} for pid, payload in death_window]
    for packet_id, payload in death_window:
        if len(payload) < 6:
            continue
        entity, i = read_varint(payload, 0)
        if entity != bot.entity_id:
            continue
        length, j = read_varint(payload, i)
        if j + length != len(payload) or length == 0:
            continue
        message = payload[j:j + length]
        if b"translate" in message:
            CB["combat_death"] = packet_id
            found["combat_death"] = {
                "id": packet_id, "hex": payload.hex(),
                "message": message.decode("utf-8", "replace"),
                # Measured, and it is one field short of what the archived page
                # describes: after the player id comes the chat component and
                # nothing else. There is no killer entity id on the wire in
                # 1.20.1. Writing one would push the string out by four bytes
                # and the client would disconnect on a length it cannot read.
                "fields": "varint player id, string chat component"}
            break

    # The orbs a death produces, and the packet that carries them. Identified by
    # shape — an id, three doubles that are where the player was standing, and a
    # short — which nothing else in the window matches.
    for packet_id, payload in death_window:
        if len(payload) != 27:
            continue
        _, i = read_varint(payload, 0)
        if i != 1 and i != 2:
            continue
        try:
            x, y, z = struct.unpack_from(">ddd", payload, i)
        except struct.error:
            continue
        count = struct.unpack_from(">h", payload, i + 24)[0]
        if 0 < count <= 32767 and abs(y) < 400.0:
            found["spawn_experience_orb"] = {
                "id": packet_id, "hex": payload.hex(),
                "x": x, "y": y, "z": z, "count": count,
                "fields": "varint entity, double x, double y, double z, short count"}
            break

    bot.respawn()
    bot.pump(2.5)
    after = bot.drain()
    found["after_respawn"] = [{"id": pid, "hex": payload.hex()[:200]} for pid, payload in after]
    # Respawn is the one packet in that window that starts with two identical
    # dimension strings.
    for packet_id, payload in after:
        length, i = read_varint(payload, 0)
        first = payload[i:i + length]
        if not first.startswith(b"minecraft:"):
            continue
        second_length, j = read_varint(payload, i + length)
        if payload[j:j + second_length] == first:
            CB["respawn"] = packet_id
            found["respawn"] = {"id": packet_id, "hex": payload.hex(),
                                "dimension": first.decode()}
            break
    bot.dead = False

    print("  " + ", ".join(f"{name}=0x{value:02X}" for name, value in CB.items()
                           if value is not None))
    missing = [name for name, value in CB.items() if value is None]
    if missing:
        print(f"  NOT FOUND: {', '.join(missing)}")
    found["ids"] = dict(CB)
    server.batch(["gamerule naturalRegeneration true"])
    return found


def campaign_fall(server: Server) -> dict:
    """Fall damage for thirty heights, against a measured floor."""
    server.batch(["gamerule doMobSpawning false", "difficulty normal", "time set noon",
                  "forceload add -32 -32 32 32", "gamerule sendCommandFeedback true"])
    nap(2.0)

    def drop(height: float, blocks: float) -> float | None:
        server.batch(['kill @e[tag=faller]'])
        server.batch([f'summon minecraft:cow 0.5 {height} 0.5 '
                      '{Silent:1b,PersistenceRequired:1b,NoAI:0b,Tags:["faller"]}',
                      'attribute @e[tag=faller,limit=1] minecraft:generic.max_health base set 200',
                      'data merge entity @e[tag=faller,limit=1] {Health:200.0f}'])
        # Roughly how long the fall takes, so the polling below starts near the
        # landing rather than a second before it. `blocks` and not `height`:
        # the world's floor is at y = -60 and an absolute coordinate there is
        # negative, which is how the first run of this asked to sleep for minus
        # three seconds.
        nap(0.3 + blocks / 20.0)
        for _ in range(40):
            lines = server.batch(['data get entity @e[tag=faller,limit=1] OnGround'])
            if scalar(lines) in ("1b", "true"):
                break
            nap(0.25)
        else:
            return None
        nap(0.4)
        health = number(server.batch(['data get entity @e[tag=faller,limit=1] Health']))
        return None if health is None else 200.0 - health

    # Where the floor actually is. Dropped from a round number well above it and
    # asked where it stopped, rather than trusting a layer count.
    server.batch(['kill @e[tag=faller]'])
    server.batch(['summon minecraft:cow 0.5 20 0.5 '
                  '{Silent:1b,Invulnerable:1b,PersistenceRequired:1b,Tags:["faller"]}'])
    nap(4.0)
    resting = None
    for _ in range(20):
        lines = server.batch(['data get entity @e[tag=faller,limit=1] Pos[1]'])
        candidate = number(lines)
        if candidate is not None and candidate == resting:
            break
        resting = candidate
        nap(0.4)
    if resting is None:
        raise RuntimeError("the floor never answered; no cow came to rest")
    floor = round(resting, 4)
    print(f"  floor measured at y={floor}")

    table = {}
    for h in range(1, 31):
        damage = drop(floor + float(h), float(h))
        table[h] = damage
        print(f"  fall {h:2d} blocks -> {damage}")
    server.batch(['kill @e[tag=faller]'])
    return {"floor_y": floor, "damage_by_height": table}


def campaign_invuln(server: Server) -> dict:
    """The invulnerability window, to the tick."""
    server.batch(["difficulty normal", "gamerule sendCommandFeedback true",
                  "gamerule doMobSpawning false", "reload"])
    nap(2.0)

    def fresh() -> None:
        server.batch(['kill @e[tag=inv]'])
        server.batch(['summon minecraft:cow 0.5 -55 0.5 '
                      '{Silent:1b,NoAI:1b,NoGravity:1b,PersistenceRequired:1b,Tags:["inv"]}',
                      'attribute @e[tag=inv,limit=1] minecraft:generic.max_health base set 200',
                      'data merge entity @e[tag=inv,limit=1] {Health:200.0f}'])
        nap(0.5)

    def lost() -> float | None:
        health = number(server.batch(['data get entity @e[tag=inv,limit=1] Health']))
        return None if health is None else round(200.0 - health, 4)

    gaps = {}
    for gap in range(0, 15):
        fresh()
        server.batch([f"function ovprobe:gap{gap}"])
        nap(1.5 + gap * 0.05)
        gaps[gap] = lost()
        print(f"  two 4-point hits {gap} ticks apart -> {gaps[gap]} lost")

    ordering = {}
    for name, label in (("stronger", "4 then 9, same tick"), ("weaker", "9 then 4, same tick")):
        fresh()
        server.batch([f"function ovprobe:{name}"])
        nap(1.5)
        ordering[name] = lost()
        print(f"  {label} -> {ordering[name]} lost")

    server.batch(['kill @e[tag=inv]'])
    return {"pair_gap_ticks": gaps, "ordering": ordering}


def campaign_xp(server: Server, bot: Bot) -> dict:
    """The cost of every level from 0 to 40, then the curve checked as a whole."""
    costs = {}
    for level in range(0, 41):
        server.batch([f"experience set {BOT} 0 points",
                      f"experience set {BOT} {level} levels",
                      f"experience add {BOT} 1 points"])
        bot.pump(0.25)
        progress = number(server.batch([f"data get entity {BOT} XpP"]))
        if progress is None or progress <= 0.0:
            costs[level] = None
            continue
        costs[level] = round(1.0 / progress)
        print(f"  level {level:2d} -> {level + 1} costs {costs[level]} points")

    # The whole curve, checked in one go: feed the cumulative total the table
    # predicts and see which level the game says that is.
    checks = {}
    cumulative = 0
    for level in range(0, 41):
        if costs.get(level) is None:
            break
        cumulative += costs[level]
        server.batch([f"experience set {BOT} 0 levels", f"experience set {BOT} 0 points",
                      f"experience add {BOT} {cumulative} points"])
        bot.pump(0.25)
        got = number(server.batch([f"data get entity {BOT} XpLevel"]))
        bar = number(server.batch([f"data get entity {BOT} XpP"]))
        checks[level + 1] = {"points": cumulative, "level": None if got is None else int(got),
                             "bar": bar}
    server.batch([f"experience set {BOT} 0 levels", f"experience set {BOT} 0 points"])
    return {"cost_of_level": costs, "cumulative_check": checks}


def candidate_items() -> list[str]:
    with open(NORMALIZED / "registries.json") as f:
        registries = json.load(f)["registries"]
    items = registries["minecraft:item"]["entries"]
    blocks = set(registries["minecraft:block"]["entries"])
    return [name for name in items if name not in blocks]


def campaign_food(server: Server, bot: Bot, items: list[str]) -> dict:
    """Nutrition and saturation, one item at a time.

    The baseline is foodLevel 10 and saturation 0. Ten is not arbitrary: the
    largest nutrition in the game is ten, so nothing eaten from there can hit
    the ceiling of twenty and be recorded short. Saturation has a ceiling of its
    own — the *new* food level — and that one is reachable, so it is checked per
    item rather than assumed away.

    Natural regeneration is off for the whole campaign. With it on, an item that
    happens to hurt the player (a splash potion of harming, say) starts a
    regeneration that spends saturation, and the next item measured comes out
    wrong for a reason nothing in its own numbers would show.
    """
    server.batch([f"gamemode survival {BOT}", "difficulty normal",
                  "gamerule naturalRegeneration false"])
    out: dict[str, dict] = {}
    sequence = 1
    drain_food(server, bot, 10)
    for index, item in enumerate(items):
        if index % 25 == 0:
            heal(server, bot)
            print(f"  ... {index}/{len(items)}")
        before = read_player(server)
        if before["food"] is None or before["food"] > 10 or (before["saturation"] or 0) > 0.0:
            drain_food(server, bot, 10)
            before = read_player(server)
        server.batch([f"clear {BOT}", f"give {BOT} {item} 1"])
        bot.pump(0.3)
        bot.hold(0)
        bot.use_item(sequence)
        sequence += 1
        bot.pump(2.3)
        after = read_player(server)
        if before["food"] is None or after["food"] is None:
            continue
        nutrition = int(after["food"]) - int(before["food"])
        saturation = round((after["saturation"] or 0.0) - (before["saturation"] or 0.0), 4)
        if nutrition == 0 and saturation == 0.0:
            continue
        clamped = abs((after["saturation"] or 0.0) - (after["food"] or 0.0)) < 1e-4
        out[item] = {"nutrition": nutrition, "saturation": saturation,
                     "saturation_clamped": clamped,
                     "from_food": int(before["food"])}
        print(f"  {item}: +{nutrition} food, +{saturation} saturation"
              f"{' (CLAMPED)' if clamped else ''}")
    server.batch([f"clear {BOT}", "gamerule naturalRegeneration true"])
    print(f"  {len(out)} of {len(items)} candidates changed the player's food")
    return out


def campaign_exhaustion(server: Server, bot: Bot) -> dict:
    """What each action costs in exhaustion."""
    server.batch([f"gamemode survival {BOT}", "difficulty normal",
                  "gamerule naturalRegeneration false"])
    bot.pump(0.5)

    def reset() -> None:
        # Food and saturation to their ceiling, health to its own, and the
        # exhaustion counter wherever the last wrap left it — which is why the
        # measurement below is a *difference* rather than a reading.
        heal(server, bot)
        fill_food(server, bot)
        bot.pump(0.3)

    baseline: dict = {}

    def mark() -> None:
        baseline.clear()
        baseline.update(read_player(server))

    def spent() -> float | None:
        now = read_player(server)
        for field in ("exhaustion", "saturation", "food"):
            if now[field] is None or baseline.get(field) is None:
                return None
        # Each 4.0 of exhaustion wraps into one point of saturation, and once
        # saturation is gone, one point of food. Reconstructing the total from
        # what was consumed is the only way to see past the counter's own
        # ceiling of four.
        wrapped = (baseline["saturation"] - now["saturation"]) + (baseline["food"] - now["food"])
        return round(4.0 * wrapped + (now["exhaustion"] - baseline["exhaustion"]), 5)

    out: dict = {}

    # Sprinting. The bot walks a straight line in ticks of a realistic length so
    # the server's own movement checks stay quiet.
    if bot.position is None:
        raise RuntimeError("the bot was never told where it is")
    x0, y0, z0 = bot.position
    server.batch([f"forceload add {int(x0) - 48} {int(z0) - 48} {int(x0) + 48} {int(z0) + 48}"])
    for label, sprinting, step, steps in (("sprint", True, 0.28, 200),
                                          ("walk", False, 0.13, 200)):
        server.batch([f"tp {BOT} {x0} {y0} {z0}"])
        bot.pump(0.6)
        reset()
        mark()
        bot.sprint(sprinting)
        bot.move_to(x0, y0, z0)
        for i in range(1, steps + 1):
            bot.move_to(x0 + step * i, y0, z0)
            bot.pump(0.05)
        bot.sprint(False)
        bot.pump(0.6)
        distance = step * steps
        total = spent()
        out[label] = {"blocks": distance, "exhaustion": total,
                      "per_block": None if total is None else round(total / distance, 6)}
        print(f"  {label} {distance:.2f} blocks -> {total} exhaustion")

    # Breaking blocks. Survival, a diamond pickaxe, and stone put back each time
    # so every break is the same job.
    server.batch([f"tp {BOT} {x0} {y0} {z0}", f"clear {BOT}",
                  f"give {BOT} minecraft:diamond_pickaxe 1"])
    bot.pump(0.6)
    reset()
    mark()
    bot.hold(0)
    bx, by, bz = int(x0) + 2, int(y0), int(z0)
    breaks = 20
    sequence = 5000
    for i in range(breaks):
        server.batch([f"setblock {bx} {by} {bz} minecraft:stone replace"])
        bot.dig(bx, by, bz, 0, sequence)
        sequence += 1
        bot.pump(0.9)
        bot.dig(bx, by, bz, 2, sequence)
        sequence += 1
        bot.pump(0.35)
    total = spent()
    out["break_block"] = {"blocks": breaks, "exhaustion": total,
                          "per_block": None if total is None else round(total / breaks, 6)}
    print(f"  {breaks} blocks broken -> {total} exhaustion")

    # Taking damage. The damage type's own exhaustion figure is in the data
    # generator's output; this checks that it is what actually gets charged.
    for damage_type in ("minecraft:cactus", "minecraft:generic", "minecraft:fall",
                        "minecraft:starve", "minecraft:player_attack"):
        reset()
        mark()
        server.batch([f"damage {BOT} 1 {damage_type}"])
        bot.pump(0.6)
        out.setdefault("damage", {})[damage_type] = spent()
        print(f"  one point of {damage_type} -> {out['damage'][damage_type]} exhaustion")

    # A jump, expressed the only way a client can express one: position packets
    # that go up and come back down.
    server.batch([f"tp {BOT} {x0} {y0} {z0}"])
    bot.pump(0.6)
    reset()
    mark()
    for _ in range(10):
        for dy, ground in ((0.42, False), (0.75, False), (0.98, False), (1.1, False),
                           (0.98, False), (0.75, False), (0.42, False), (0.0, True)):
            bot.move_to(x0, y0 + dy, z0, ground)
            bot.pump(0.05)
    out["jump"] = {"jumps": 10, "exhaustion": spent()}
    print(f"  ten jumps -> {out['jump']['exhaustion']} exhaustion")

    server.batch([f"clear {BOT}", "gamerule naturalRegeneration true"])
    return out


def campaign_starve(server: Server, bot: Bot) -> dict:
    """Where starvation stops, per difficulty."""
    out = {}
    for difficulty in ("peaceful", "easy", "normal", "hard"):
        server.batch([f"difficulty {difficulty}", f"gamemode survival {BOT}",
                      "gamerule naturalRegeneration true"])
        set_health(server, bot, 12.0)
        drain_food(server, bot, 0, timeout=60.0)
        bot.pump(0.5)
        samples = []
        for _ in range(24):
            bot.pump(2.0)
            now = read_player(server)
            health, food = now["health"], now["food"]
            samples.append((health, food))
            if bot.dead:
                break
        final = samples[-1] if samples else (None, None)
        out[difficulty] = {"health": final[0], "food": final[1], "died": bot.dead,
                           "trace": samples}
        print(f"  {difficulty}: settled at health {final[0]}, food {final[1]}, "
              f"died={bot.dead}")
        if bot.dead:
            bot.respawn()
            bot.pump(1.5)
    server.batch(["difficulty normal"])
    return out


def campaign_regen(server: Server, bot: Bot) -> dict:
    """How fast health comes back, at each food level that allows it."""
    server.batch(["difficulty normal", f"gamemode survival {BOT}",
                  "gamerule naturalRegeneration true"])
    out = {}
    for label, food in (("food20_sat20", 20), ("food18_sat0", 18), ("food17_sat0", 17),
                        ("food6_sat0", 6)):
        fill_food(server, bot)
        if food < 20:
            drain_food(server, bot, food)
        set_health(server, bot, 10.0)
        before = read_player(server)
        started = time.monotonic()
        trace = []
        for _ in range(16):
            bot.pump(1.0)
            now = read_player(server)
            trace.append((round(time.monotonic() - started, 2), now["health"], now["food"],
                          now["saturation"]))
        gained = None
        if trace and trace[-1][1] is not None and before["health"] is not None:
            gained = round(trace[-1][1] - before["health"], 3)
        out[label] = {"start": before, "seconds": trace[-1][0] if trace else None,
                      "gained": gained, "trace": trace}
        print(f"  {label} (food {before['food']}, saturation {before['saturation']}): "
              f"+{gained} health in {out[label]['seconds']}s")
    return out


def campaign_oxygen(server: Server) -> dict:
    """Air underwater, and what happens when it runs out."""
    server.batch(["difficulty normal", "kill @e[tag=diver]",
                  "fill 8 -60 8 12 -56 12 minecraft:water replace"])
    nap(1.0)
    server.batch(['summon minecraft:cow 10.5 -58 10.5 '
                  '{Silent:1b,NoAI:1b,NoGravity:1b,PersistenceRequired:1b,Tags:["diver"]}',
                  'attribute @e[tag=diver,limit=1] minecraft:generic.max_health base set 200',
                  'data merge entity @e[tag=diver,limit=1] {Health:200.0f}'])
    nap(0.6)
    trace = []
    start = time.monotonic()
    for _ in range(70):
        air = number(server.batch(['data get entity @e[tag=diver,limit=1] Air']))
        health = number(server.batch(['data get entity @e[tag=diver,limit=1] Health']))
        trace.append((round(time.monotonic() - start, 2), air, health))
        if health is not None and health < 180.0:
            break
        nap(0.4)
    server.batch(["kill @e[tag=diver]"])
    print(f"  {len(trace)} samples, first air {trace[0][1] if trace else None}, "
          f"last {trace[-1][1] if trace else None} at health {trace[-1][2] if trace else None}")
    return {"trace": trace}


def campaign_death_xp(server: Server, bot: Bot) -> dict:
    """What a player drops when they die, per level."""
    server.batch(["difficulty normal", f"gamemode survival {BOT}",
                  "gamerule keepInventory false", "gamerule doImmediateRespawn false"])
    out = {}
    for level in list(range(0, 21)) + [25, 30, 40, 60, 100]:
        server.batch(["kill @e[type=minecraft:experience_orb]",
                      f"experience set {BOT} 0 points",
                      f"experience set {BOT} {level} levels"])
        heal(server, bot)
        bot.pump(0.4)
        server.batch([f"damage {BOT} 100 minecraft:generic_kill"])
        bot.pump(1.5)
        lines = server.batch(['execute as @e[type=minecraft:experience_orb] '
                              'run data get entity @s Value'])
        total = 0
        found = 0
        for line in lines:
            match = ORB_VALUE.search(line)
            if match:
                total += int(match.group(1))
                found += 1
        out[level] = {"orbs": found, "xp": total, "death_message": bot.death_message}
        print(f"  level {level}: {found} orbs worth {total}")
        bot.respawn()
        bot.pump(1.2)
    server.batch(["kill @e[type=minecraft:experience_orb]",
                  f"experience set {BOT} 0 levels", f"experience set {BOT} 0 points"])
    return out


def campaign_mining_xp(server: Server, bot: Bot) -> dict:
    """The experience a broken ore is worth, thirty draws at a time."""
    server.batch([f"gamemode survival {BOT}", f"clear {BOT}",
                  f"give {BOT} minecraft:diamond_pickaxe 1"])
    bot.pump(0.5)
    bot.hold(0)
    if bot.position is None:
        raise RuntimeError("the bot was never told where it is")
    x0, y0, z0 = bot.position
    bx, by, bz = int(x0) + 2, int(y0), int(z0)
    out = {}
    sequence = 20000
    for ore in ("coal_ore", "iron_ore", "copper_ore", "gold_ore", "diamond_ore",
                "emerald_ore", "lapis_ore", "redstone_ore", "nether_quartz_ore",
                "nether_gold_ore", "deepslate_coal_ore", "deepslate_diamond_ore",
                "ancient_debris", "spawner", "stone"):
        draws = []
        for _ in range(30):
            server.batch(["kill @e[type=minecraft:experience_orb]",
                          f"setblock {bx} {by} {bz} minecraft:{ore} replace"])
            bot.dig(bx, by, bz, 0, sequence)
            sequence += 1
            bot.pump(1.1)
            bot.dig(bx, by, bz, 2, sequence)
            sequence += 1
            bot.pump(0.45)
            lines = server.batch(['execute as @e[type=minecraft:experience_orb] '
                                  'run data get entity @s Value'])
            total = sum(int(m.group(1)) for line in lines
                        for m in [ORB_VALUE.search(line)] if m)
            draws.append(total)
        out[ore] = {"draws": draws, "min": min(draws), "max": max(draws),
                    "mean": round(sum(draws) / len(draws), 4)}
        print(f"  {ore}: {out[ore]['min']}..{out[ore]['max']} "
              f"(mean {out[ore]['mean']})")
    server.batch([f"setblock {bx} {by} {bz} minecraft:air replace", f"clear {BOT}"])
    return out


ALL = ["packets", "fall", "invuln", "xp", "food", "exhaustion", "starve", "regen",
       "oxygen", "death_xp", "mining_xp"]

NEEDS_BOT = {"packets", "xp", "food", "exhaustion", "starve", "regen", "death_xp",
             "mining_xp"}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", default=",".join(ALL))
    parser.add_argument("--skip-done", action="store_true",
                        help="leave campaigns already present in the output file alone")
    parser.add_argument("out", nargs="?", default=str(NORMALIZED / "survival.json"))
    args = parser.parse_args()
    wanted = [name for name in args.only.split(",") if name]
    for name in wanted:
        if name not in ALL:
            print(f"unknown campaign {name!r}; known: {', '.join(ALL)}")
            return 2

    out_path = Path(args.out)
    # A campaign that already ran is kept. Each one takes minutes and several
    # need a player logged in, so losing eight of them because the ninth threw
    # would make the whole rig unusable — and re-running a campaign to get the
    # same answer is not evidence, it is just time.
    document: dict = {}
    if out_path.exists():
        try:
            with open(out_path) as f:
                document = json.load(f)
            print(f"resuming; already have {[k for k in document if not k.startswith('$')]}")
        except (OSError, ValueError):
            document = {}
    document["$comment"] = ("Mesure contre un vrai serveur 1.20.1. Voir "
                            "docs/provenance/survie.md et scripts/measure_survival.py.")

    # The invulnerability functions, written before the world exists so the
    # datapack is there the first time it is loaded.
    functions = {
        "hit_a": ["damage @e[tag=inv,limit=1] 4 minecraft:generic"],
        "hit_b": ["damage @e[tag=inv,limit=1] 4 minecraft:generic"],
        "stronger": ["damage @e[tag=inv,limit=1] 4 minecraft:generic",
                     "damage @e[tag=inv,limit=1] 9 minecraft:generic"],
        "weaker": ["damage @e[tag=inv,limit=1] 9 minecraft:generic",
                   "damage @e[tag=inv,limit=1] 4 minecraft:generic"],
        "gap0": ["function ovprobe:hit_a", "function ovprobe:hit_b"],
    }
    for gap in range(1, 15):
        functions[f"gap{gap}"] = ["function ovprobe:hit_a",
                                  f"schedule function ovprobe:hit_b {gap}t"]
    RUN.mkdir(parents=True, exist_ok=True)
    install_datapack(RUN, functions)

    server = Server(RUN, port=PORT)
    bot: Bot | None = None
    try:
        server.batch(["gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                      "gamerule sendCommandFeedback true", "gamerule doFireTick false",
                      "gamerule randomTickSpeed 0", "difficulty normal", "time set noon"])
        listed = server.batch(["datapack list"])
        if not any("ovprobe" in line for line in listed):
            raise RuntimeError("the probe datapack was not loaded; "
                               f"server said: {listed[-4:]}")

        if any(name in NEEDS_BOT for name in wanted):
            bot = Bot(PORT)
            globals()["_BOT"] = bot
            bot.pump(3.0)
            if bot.position is None:
                raise RuntimeError("the bot was never told where it is")
            px, py, pz = bot.position
            server.batch([f"forceload add {int(px) - 64} {int(pz) - 64} "
                          f"{int(px) + 64} {int(pz) + 64}",
                          f"gamemode survival {BOT}"])
            bot.pump(1.0)
            print(f"bot at {px:.2f} {py:.2f} {pz:.2f}, entity {bot.entity_id}")

        for name in wanted:
            if args.skip_done and name in document:
                print(f"\n── {name}: already measured, kept ──")
                continue
            print(f"\n── {name} ──")
            started = time.monotonic()
            if name == "fall":
                document["fall"] = campaign_fall(server)
            elif name == "invuln":
                document["invuln"] = campaign_invuln(server)
            elif name == "oxygen":
                document["oxygen"] = campaign_oxygen(server)
            else:
                assert bot is not None
                if name == "packets":
                    document["packets"] = campaign_packets(server, bot)
                elif name == "xp":
                    document["xp"] = campaign_xp(server, bot)
                elif name == "food":
                    document["food"] = campaign_food(server, bot, candidate_items())
                elif name == "exhaustion":
                    document["exhaustion"] = campaign_exhaustion(server, bot)
                elif name == "starve":
                    document["starve"] = campaign_starve(server, bot)
                elif name == "regen":
                    document["regen"] = campaign_regen(server, bot)
                elif name == "death_xp":
                    document["death_xp"] = campaign_death_xp(server, bot)
                elif name == "mining_xp":
                    document["mining_xp"] = campaign_mining_xp(server, bot)
            print(f"── {name} took {time.monotonic() - started:.0f}s ──")
            out_path.parent.mkdir(parents=True, exist_ok=True)
            with open(out_path, "w") as f:
                json.dump(document, f, indent=1, sort_keys=True)
    finally:
        server.stop()
    print(f"\nwrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
