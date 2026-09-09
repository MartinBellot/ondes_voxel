#!/usr/bin/env python3
"""Ask a real 1.20.1 server what it costs to hit something.

Melee is the part of Minecraft with the most numbers that Mojang publishes
nowhere. Weapon damage, the attack-speed attribute, the shape of the 1.9 attack
gauge, the critical multiplier, the sweep, the knockback impulse, the tool
damage per action and every durability in the game are Java constants. None of
them are in the data generator's reports, and none of them are guessed here.

Every campaign drives the real server and reads its answers back:

  attrs       `/attribute <player> minecraft:generic.attack_damage get` with an
              item held. The item's own attribute modifiers are part of the
              player's attribute map, so the command answers with the modifier
              applied — which makes the whole weapon table one console command
              per item instead of a damage measurement per item. The same for
              generic.attack_speed, whose reciprocal is the cooldown.

              Cross-checked by the `weapons` campaign, which actually hits
              something with each one: an attribute that says 8 and a hit that
              takes 8 is the same number reached two ways.

  gauge       Damage as a function of the delay since the previous attack. The
              bot attacks a *priming* target, waits, then attacks a *fresh*
              one — fresh because a second hit inside the twenty-tick
              invulnerability window lands only the difference, and measuring
              the gauge through that window measures the window instead.

              The delay is commanded in milliseconds and lands on whichever
              server tick it lands on, so the answer is read the other way
              round: the damage is quantised, each distinct value is one tick,
              and the staircase is the result. Three repeats per delay say
              whether the staircase is stable.

  crit        The multiplier for a hit made while falling. The bot is teleported
              up and sends descending positions with on_ground false until the
              server has credited it some fall distance; the attack that follows
              is compared with the same attack made standing.

  sweep       What a fully charged sword does to a *second* mob standing next to
              the first. The sweep box is around the attacker, not the victim,
              so both targets are put within a block of the bot — a pair three
              blocks apart, which every other campaign uses, never sweeps.

  knockback   The Set Entity Velocity packet the server sends for the victim,
              in its own units of 1/8000 block per tick. Measured with and
              without sprinting, and with Knockback I and II. The victim's
              knockback resistance is left at zero here and pinned at one
              everywhere else.

  durability  How many uses a tool has, and what each action costs it. The tool
              is handed over with `Damage:N` already on it and made to do one
              action; the largest N that survives is the answer. One tool is
              then actually worn out from pristine, action by action, as a check
              that the bisection means what it says.

  entity_loot Several hundred draws of every entity loot table, through the
              server's own `/loot give <player> kill <entity>` — the same oracle
              scripts/check_loot.py uses for blocks, and the same format.

  mob_xp      The experience a kill drops, summed over every orb, thirty kills
              per type. Killed *by the bot*, because a mob that dies any other
              way drops none.

  use         What using an item on a block does: the bucket pair, flint and
              steel, the hoe, shears, bone meal, and the bare-hand interactions
              — door, trapdoor, gate, button, lever, chest, furnace. Read back
              as the block state that resulted.

Usage: python3 scripts/measure_combat.py [--only gauge,weapons,...] [out.json]

Writes data/vanilla/1.20.1/normalized/combat.json. Partial results are written
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
RUN = ROOT / "run" / "combat-oracle"
PORT = 25611
BOT = "ovfight"

# Serverbound ids beyond the ones the capture harness already uses. Every one is
# verified by its effect rather than taken on trust: an Interact that removes no
# health means the id is wrong, and the campaign says so instead of reporting a
# table of zeros.
SB_CONFIRM_TELEPORT = 0x00
SB_INTERACT = 0x10
SB_KEEP_ALIVE = 0x12
SB_POSITION = 0x14
SB_PLAYER_ACTION = 0x1D
SB_PLAYER_COMMAND = 0x1E
SB_SET_HELD_ITEM = 0x28
SB_SET_CREATIVE_SLOT = 0x2B
SB_SWING_ARM = 0x2F
SB_USE_ITEM_ON = 0x31
SB_USE_ITEM = 0x32

# Clientbound. These three are already established in this repo — see
# src/ov_protocol/include/ov/protocol/entity.hpp, where they were captured off a
# real server rather than read from a summary — and each is checked again here
# by the effect that produces it.
CB_SPAWN_ENTITY = 0x01
CB_LOGIN = 0x28
CB_SYNC_POSITION = 0x3C
CB_KEEP_ALIVE = 0x23
CB_ENTITY_VELOCITY = 0x54

DATA = re.compile(r"following entity data: (.*)$")
ATTRIBUTE = re.compile(r"Value of attribute [A-Za-z ]+ for entity [^ ]+ is ([0-9.eE+-]+)")
ITEM_RE = re.compile(r'id: "(minecraft:[a-z_]+)", Count: (\d+)b')

# The floor of the superflat, and where the rig stands on it.
FLOOR_Y = -60
BOT_X, BOT_Z = 0.5, 0.5
# Three blocks either side: inside the six-block interaction range, and well
# outside the sweep box, which is the attacker's own bounding box grown by one.
LEFT_X, RIGHT_X = -3.5, 3.5

TARGET_HEALTH = 1024.0


def target_nbt(tag: str, resist: float = 1.0) -> str:
    """A sheep that can take a thousand hits and stays where it is put.

    Never `Invulnerable:1b`: an invulnerable entity cannot be targeted at all,
    and an attack packet aimed at one is dropped by the server without a word.
    Staying alive is done with health and, where the campaign is not about
    knockback, with knockback resistance.
    """
    return (f'{{NoAI:1b,Silent:1b,PersistenceRequired:1b,NoGravity:1b,Tags:["{tag}"],'
            f'Attributes:[{{Name:"generic.max_health",Base:{TARGET_HEALTH}}},'
            f'{{Name:"generic.knockback_resistance",Base:{resist}}}],'
            f'Health:{TARGET_HEALTH}f}}')


class Fighter(Probe):
    """A client that joins, stands still, and hits things on command."""

    def __init__(self, port: int, name: str = BOT) -> None:
        super().__init__(port, name=name)
        self.entity_id = 0
        self.lost = False
        self.velocities: list[tuple[int, int, int, int]] = []
        self.sequence = 1

    def pump(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.socket.settimeout(max(0.001, deadline - time.monotonic()))
            try:
                packet_id, payload = self.read()
            except (TimeoutError, OSError):
                return
            except EOFError:
                self.lost = True
                return
            self.captured.append((packet_id, payload))
            if packet_id == CB_KEEP_ALIVE:
                self.send(SB_KEEP_ALIVE, payload[:8])
            elif packet_id == CB_SYNC_POSITION:
                x, y, z = struct.unpack_from(">ddd", payload, 0)
                self.position = (x, y, z)
                teleport_id, _ = read_varint(payload, 33)
                self.send(SB_CONFIRM_TELEPORT, varint(teleport_id))
                self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([1]))
            elif packet_id == CB_LOGIN:
                self.entity_id = struct.unpack_from(">i", payload, 0)[0]
            elif packet_id == CB_ENTITY_VELOCITY:
                who, i = read_varint(payload, 0)
                vx, vy, vz = struct.unpack_from(">hhh", payload, i)
                self.velocities.append((who, vx, vy, vz))

    def next_sequence(self) -> int:
        self.sequence += 1
        return self.sequence

    # ── the verbs ───────────────────────────────────────────────────────────

    def attack(self, entity_id: int, sneaking: bool = False) -> None:
        """Interact, type 1. The swing is sent too: vanilla's own client sends
        both, and the swing is what resets the arm animation."""
        self.send(SB_INTERACT,
                  varint(entity_id) + varint(1) + bytes([1 if sneaking else 0]))
        self.send(SB_SWING_ARM, varint(0))

    def interact(self, entity_id: int, sneaking: bool = False) -> None:
        self.send(SB_INTERACT,
                  varint(entity_id) + varint(0) + varint(0) + bytes([1 if sneaking else 0]))

    def sprint(self, on: bool) -> None:
        self.send(SB_PLAYER_COMMAND, varint(self.entity_id) + varint(3 if on else 4) + varint(0))

    def hold(self, slot: int) -> None:
        self.send(SB_SET_HELD_ITEM, struct.pack(">h", slot))

    def creative_slot(self, slot: int, item_id: int | None, count: int = 1) -> None:
        if item_id is None:
            self.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", slot) + bytes([0]))
        else:
            self.send(SB_SET_CREATIVE_SLOT,
                      struct.pack(">h", slot) + bytes([1]) + varint(item_id)
                      + bytes([count]) + bytes([0]))

    def move_to(self, x: float, y: float, z: float, on_ground: bool = True) -> None:
        self.position = (x, y, z)
        self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([1 if on_ground else 0]))

    def use_item(self) -> None:
        self.send(SB_USE_ITEM, varint(0) + varint(self.next_sequence()))

    def use_item_on(self, x: int, y: int, z: int, face: int = 1,
                    cursor: tuple[float, float, float] = (0.5, 1.0, 0.5)) -> None:
        packed = ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)
        self.send(SB_USE_ITEM_ON,
                  varint(0) + struct.pack(">Q", packed & 0xFFFFFFFFFFFFFFFF) + varint(face)
                  + struct.pack(">fff", *cursor) + bytes([0]) + varint(self.next_sequence()))

    def dig(self, x: int, y: int, z: int, status: int) -> None:
        # Packed as **unsigned**: the three fields are two's-complement bit
        # patterns and the assembled word routinely has its top bit set, which
        # struct's signed 'q' refuses outright.
        packed = ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)
        self.send(SB_PLAYER_ACTION,
                  varint(status) + struct.pack(">Q", packed & 0xFFFFFFFFFFFFFFFF)
                  + bytes([1]) + varint(self.next_sequence()))


# ── reading the server's answers ────────────────────────────────────────────

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
    try:
        return float(raw.rstrip("bsfdL").rstrip("bsfd"))
    except ValueError:
        return None


def attribute_value(lines: list[str]) -> float | None:
    for line in lines:
        match = ATTRIBUTE.search(line)
        if match:
            return float(match.group(1))
    return None


# ── campaigns ───────────────────────────────────────────────────────────────

WEAPONS = [
    "minecraft:wooden_sword", "minecraft:stone_sword", "minecraft:iron_sword",
    "minecraft:golden_sword", "minecraft:diamond_sword", "minecraft:netherite_sword",
    "minecraft:wooden_axe", "minecraft:stone_axe", "minecraft:iron_axe",
    "minecraft:golden_axe", "minecraft:diamond_axe", "minecraft:netherite_axe",
    "minecraft:wooden_pickaxe", "minecraft:stone_pickaxe", "minecraft:iron_pickaxe",
    "minecraft:golden_pickaxe", "minecraft:diamond_pickaxe", "minecraft:netherite_pickaxe",
    "minecraft:wooden_shovel", "minecraft:stone_shovel", "minecraft:iron_shovel",
    "minecraft:golden_shovel", "minecraft:diamond_shovel", "minecraft:netherite_shovel",
    "minecraft:wooden_hoe", "minecraft:stone_hoe", "minecraft:iron_hoe",
    "minecraft:golden_hoe", "minecraft:diamond_hoe", "minecraft:netherite_hoe",
    "minecraft:trident", "minecraft:shears", "minecraft:stick", "minecraft:air",
]


def campaign_attrs(server: Server) -> dict:
    """Attack damage and attack speed, per item, from the attribute map.

    `/attribute get` reports the attribute *after* the held item's modifiers
    have been folded in, which is why the whole weapon table is two console
    commands per item. It is not a shortcut around measuring: the `weapons`
    campaign hits something with each one and the two must agree.
    """
    out: dict[str, dict] = {}
    for item in WEAPONS:
        commands = [f"clear {BOT}"]
        if item != "minecraft:air":
            commands.append(f"give {BOT} {item} 1")
        commands.append(f"item replace entity {BOT} weapon.mainhand with "
                        + ("minecraft:air" if item == "minecraft:air" else f"{item} 1"))
        server.batch(commands)
        # A tick has to pass between putting the item in the hand and asking.
        # An item's attribute modifiers are folded into the map by the living
        # entity's own tick, not by the command that placed it, so a single
        # batch answers with the *previous* item's numbers — a table that is
        # entirely self-consistent, entirely plausible, and shifted by one.
        time.sleep(0.25)
        lines = server.batch([
            f"attribute {BOT} minecraft:generic.attack_damage get",
            f"attribute {BOT} minecraft:generic.attack_speed get",
        ])
        values = [float(m.group(1)) for line in lines for m in [ATTRIBUTE.search(line)] if m]
        if len(values) != 2:
            out[item] = {"error": f"expected two attributes, got {values}", "lines": lines[-4:]}
            continue
        damage, speed = values
        out[item] = {"attack_damage": damage, "attack_speed": speed,
                     "cooldown_ticks": (20.0 / speed) if speed else None}
        print(f"  {item:34s} damage {damage:6.2f}  speed {speed:6.3f}"
              f"  cooldown {20.0 / speed:6.2f} ticks")
    return out


def setup_rig(server: Server, fighter: Fighter) -> None:
    server.batch([
        "gamerule doMobSpawning false", "gamerule doDaylightCycle false",
        "gamerule doImmediateRespawn true", "gamerule sendCommandFeedback true",
        "gamerule mobGriefing false", "gamerule doMobLoot true",
        "gamerule naturalRegeneration false", "gamerule doWeatherCycle false",
        "difficulty normal", "time set noon", "weather clear",
        f"forceload add -32 -32 32 32",
        f"gamemode creative {BOT}",
        f"tp {BOT} {BOT_X} {FLOOR_Y} {BOT_Z} 0 0",
        f"fill -8 {FLOOR_Y - 1} -8 8 {FLOOR_Y - 1} 8 minecraft:stone",
        f"fill -8 {FLOOR_Y} -8 8 {FLOOR_Y + 3} 8 minecraft:air",
        # A world directory that has been used before is full of the animals
        # the superflat spawned on its first tick, and they are indistinguishable
        # from a measurement target once the packets are flying.
        "kill @e[type=!minecraft:player]",
    ])
    fighter.pump(1.0)
    fighter.move_to(BOT_X, float(FLOOR_Y), BOT_Z)
    fighter.pump(0.5)


def summon_pair(server: Server, resist: float = 1.0) -> None:
    server.batch([
        "kill @e[type=minecraft:sheep]",
        f"summon minecraft:sheep {LEFT_X} {FLOOR_Y} {BOT_Z} {target_nbt('ovprime', resist)}",
        f"summon minecraft:sheep {RIGHT_X} {FLOOR_Y} {BOT_Z} {target_nbt('ovmark', resist)}",
    ])


def campaign_weapons(server: Server, fighter: Fighter) -> dict:
    """A fully charged hit with each weapon, on a fresh unarmoured target."""
    out: dict[str, dict] = {}
    for item in WEAPONS:
        server.batch([f"item replace entity {BOT} weapon.mainhand with "
                      + ("minecraft:air" if item == "minecraft:air" else f"{item} 1")])
        summon_pair(server)
        mark = spawn_ids(fighter, server)["ovmark"]
        fighter.pump(0.2)
        # A second and a half is thirty ticks: past every cooldown in the game.
        time.sleep(1.6)
        fighter.attack(mark)
        fighter.pump(0.4)
        health = number(server.batch(["data get entity @e[tag=ovmark,limit=1] Health"]))
        dealt = None if health is None else round(TARGET_HEALTH - health, 4)
        out[item] = {"damage": dealt}
        print(f"  {item:34s} charged hit {dealt}")
    return out


def id_at(fighter: Fighter, server: Server, tag: str, x: float, y: float, z: float) -> int | None:
    """The network id of a tagged entity, by teleporting it and watching where.

    An entity id appears in no NBT field, so it is read off the wire: the server
    is told to teleport the entity, and the Entity Teleport packet that follows
    names it. What the packet is matched on is the **destination**, not merely
    "some entity that is not the bot" — a superflat world that has been used
    before is full of animals, and the first teleport packet in the window
    belongs to one of them about half the time. Matching the id that way put
    every attack of the first run into an empty patch of air and reported a
    diamond sword that did no damage at all.
    """
    fighter.pump(0.05)
    fighter.captured.clear()
    server.batch([f"tp @e[tag={tag},limit=1] {x:.6f} {y:.6f} {z:.6f}"])
    fighter.pump(0.4)
    for packet_id, payload in fighter.captured:
        if packet_id != 0x68:  # Entity Teleport
            continue
        who, i = read_varint(payload, 0)
        px, py, pz = struct.unpack_from(">ddd", payload, i)
        if who != fighter.entity_id and abs(px - x) < 1e-6 and abs(pz - z) < 1e-6 \
                and abs(py - y) < 1e-6:
            fighter.captured.clear()
            return who
    fighter.captured.clear()
    return None


def spawn_ids(fighter: Fighter, server: Server) -> dict[str, int]:
    """Network ids for the two targets the damage campaigns use."""
    out: dict[str, int] = {}
    for tag, x in (("ovprime", LEFT_X), ("ovmark", RIGHT_X)):
        who = id_at(fighter, server, tag, x, float(FLOOR_Y), BOT_Z)
        if who is not None:
            out[tag] = who
    return out


def campaign_gauge(server: Server, fighter: Fighter, item: str, delays_ms: list[int],
                   repeats: int) -> dict:
    """Damage against the delay since the previous attack."""
    server.batch([f"item replace entity {BOT} weapon.mainhand with {item} 1"])
    samples: list[dict] = []
    for delay in delays_ms:
        for _ in range(repeats):
            summon_pair(server)
            ids = spawn_ids(fighter, server)
            if "ovprime" not in ids or "ovmark" not in ids:
                continue
            fighter.pump(0.05)
            time.sleep(1.6)  # every cooldown is spent before the priming hit
            fighter.attack(ids["ovprime"])
            # Sending mid-tick rather than on its edge: a packet that lands on
            # the boundary is one tick early half the time, and the staircase
            # comes out smeared instead of flat.
            time.sleep(delay / 1000.0 + 0.025)
            fighter.attack(ids["ovmark"])
            fighter.pump(0.35)
            health = number(server.batch(["data get entity @e[tag=ovmark,limit=1] Health"]))
            if health is None:
                continue
            samples.append({"delay_ms": delay, "damage": round(TARGET_HEALTH - health, 4)})
        seen = sorted({s["damage"] for s in samples if s["delay_ms"] == delay})
        print(f"  delay {delay:5d} ms -> {seen}")
    return {"item": item, "samples": samples}


def campaign_crit(server: Server, fighter: Fighter, item: str) -> dict:
    """The falling-hit multiplier, against the same hit made standing.

    Two things sank the first version of this and both are worth naming. The
    bot was dropped from twelve blocks up, which put it **out of interaction
    range** of a target on the floor — six blocks, measured against the target's
    box — so the attack was dropped by the server and the campaign reported a
    critical that did no damage at all. And nothing checked that the server had
    actually credited any fall distance, so a run in which the descent was not
    believed would have read as "criticals do not exist".

    So the drop is now three blocks, not twelve, and `FallDistance` is read off
    the bot and reported with the result. A measurement whose precondition is
    not checked is not a measurement.
    """
    server.batch([f"item replace entity {BOT} weapon.mainhand with {item} 1"])
    out: dict[str, object] = {}

    for label, falling in (("ground", False), ("falling", True)):
        summon_pair(server)
        ids = spawn_ids(fighter, server)
        if "ovmark" not in ids:
            continue
        fighter.pump(0.05)
        fighter.sprint(False)
        if falling:
            server.batch([f"tp {BOT} {BOT_X} {FLOOR_Y + 3} {BOT_Z} 0 0"])
            fighter.pump(0.5)
            # The server counts fall distance from the vertical movement it is
            # told about, so the descent goes down step by step with on_ground
            # false. One jump to the bottom is a single move and credits none.
            y = float(FLOOR_Y + 3)
            for _ in range(10):
                y -= 0.15
                fighter.move_to(BOT_X, y, BOT_Z, on_ground=False)
                fighter.pump(0.05)
            out["fall_distance"] = number(
                server.batch([f"data get entity {BOT} FallDistance"]))
            time.sleep(1.2)
            # Keep claiming to be in the air right up to the swing: one packet
            # with on_ground true resets the fall and the hit is ordinary.
            fighter.move_to(BOT_X, y, BOT_Z, on_ground=False)
            fighter.pump(0.05)
            fighter.attack(ids["ovmark"])
            fighter.pump(0.4)
        else:
            server.batch([f"tp {BOT} {BOT_X} {FLOOR_Y} {BOT_Z} 0 0"])
            fighter.pump(0.5)
            fighter.move_to(BOT_X, float(FLOOR_Y), BOT_Z)
            time.sleep(1.6)
            fighter.attack(ids["ovmark"])
            fighter.pump(0.4)
        health = number(server.batch(["data get entity @e[tag=ovmark,limit=1] Health"]))
        out[label] = None if health is None else round(TARGET_HEALTH - health, 4)
        print(f"  {label:8s} {out[label]}", flush=True)

    server.batch([f"tp {BOT} {BOT_X} {FLOOR_Y} {BOT_Z} 0 0"])
    fighter.pump(0.5)
    fighter.move_to(BOT_X, float(FLOOR_Y), BOT_Z)
    ground, air = out.get("ground"), out.get("falling")
    if isinstance(ground, float) and isinstance(air, float) and ground:
        out["ratio"] = round(air / ground, 6)
    return out


def gametime(server: Server) -> int | None:
    """The server's own tick counter, which is the only clock a rig can trust."""
    for line in server.batch(["time query gametime"]):
        match = re.search(r"time is (\d+)", line)
        if match:
            return int(match.group(1))
    return None


def campaign_sweep(server: Server, fighter: Fighter) -> dict:
    """What a charged sword does to the mob standing beside the one it hit.

    Both targets go within a block of the bot: the sweep box is the attacker's
    own bounding box grown by one in x and z, so the three-block spacing every
    other campaign uses never sweeps anything — which is exactly why it is used
    everywhere else.

    The uncharged case needs care that the first two attempts did not have. A
    `sleep(0.05)` before the swing does not make it uncharged: the console round
    trips that set the rig up already took a second and the gauge was full
    before the timer started. And swinging twice in a row does not measure it
    either, because the *first* swing sweeps and its damage is still on the
    second target when the second swing's is read. So: swing once, reset the
    second target, swing again, and report the **tick gap** the server itself
    saw between the two — a gap below the twelve and a half ticks of a sword's
    cooldown is what makes the second swing uncharged, and it is measured rather
    than hoped for.
    """
    out: dict[str, dict] = {}
    for item in ("minecraft:diamond_sword", "minecraft:diamond_axe",
                 "minecraft:wooden_sword", "minecraft:netherite_sword"):
        for charged in (True, False):
            server.batch([
                f"item replace entity {BOT} weapon.mainhand with {item} 1",
                "kill @e[type=minecraft:sheep]",
                f"summon minecraft:sheep {BOT_X + 0.9} {FLOOR_Y} {BOT_Z} "
                + target_nbt("ovprime"),
                f"summon minecraft:sheep {BOT_X - 0.9} {FLOOR_Y} {BOT_Z} "
                + target_nbt("ovmark"),
            ])
            fighter.pump(0.05)
            ids = {}
            for tag, x in (("ovprime", BOT_X + 0.9), ("ovmark", BOT_X - 0.9)):
                who = id_at(fighter, server, tag, x, float(FLOOR_Y), BOT_Z)
                if who is not None:
                    ids[tag] = who
            if "ovprime" not in ids:
                continue
            fighter.sprint(False)
            time.sleep(1.6)
            gap = None
            if not charged:
                fighter.attack(ids["ovprime"])
                fighter.pump(0.15)
                first = gametime(server)
                server.batch([f"data merge entity @e[tag=ovmark,limit=1] "
                              f"{{Health:{TARGET_HEALTH}f}}"])
                second = gametime(server)
                gap = None if first is None or second is None else second - first
            fighter.attack(ids["ovprime"])
            fighter.pump(0.4)
            lines = server.batch(["data get entity @e[tag=ovprime,limit=1] Health",
                                  "data get entity @e[tag=ovmark,limit=1] Health"])
            healths = [float(m.group(1).rstrip("f"))
                       for line in lines for m in [DATA.search(line)]
                       if m and m.group(1).rstrip().endswith("f")]
            if len(healths) != 2:
                continue
            key = f"{item}/{'charged' if charged else 'uncharged'}"
            out[key] = {"direct": round(TARGET_HEALTH - healths[0], 4),
                        "swept": round(TARGET_HEALTH - healths[1], 4),
                        "tick_gap": gap}
            print(f"  {key:44s} direct {out[key]['direct']}  swept {out[key]['swept']}"
                  f"  gap {gap}", flush=True)
    return out


def campaign_knockback(server: Server, fighter: Fighter) -> dict:
    """The velocity the server hands the victim, in its own 1/8000ths."""
    out: dict[str, dict] = {}
    cases = [
        ("bare", "minecraft:air", False),
        ("sword", "minecraft:diamond_sword", False),
        ("sword_sprint", "minecraft:diamond_sword", True),
        ("bare_sprint", "minecraft:air", True),
        ("knockback1",
         'minecraft:diamond_sword{Enchantments:[{id:"minecraft:knockback",lvl:1s}]}', False),
        ("knockback2",
         'minecraft:diamond_sword{Enchantments:[{id:"minecraft:knockback",lvl:2s}]}', False),
    ]
    for label, item, sprinting in cases:
        server.batch([f"item replace entity {BOT} weapon.mainhand with {item} 1"
                      if item != "minecraft:air" else
                      f"item replace entity {BOT} weapon.mainhand with minecraft:air"])
        # Knockback resistance zero: this is the one campaign that wants the
        # target to move.
        server.batch([
            "kill @e[type=minecraft:sheep]",
            f"summon minecraft:sheep {RIGHT_X} {FLOOR_Y} {BOT_Z} "
            + target_nbt("ovmark", resist=0.0),
        ])
        fighter.pump(0.1)
        mark = id_at(fighter, server, "ovmark", RIGHT_X, float(FLOOR_Y), BOT_Z)
        if mark is None:
            continue
        fighter.sprint(sprinting)
        # The server only believes a sprint it has seen a movement under, so the
        # bot walks a little before it swings.
        for step in range(6):
            fighter.move_to(BOT_X + step * 0.1, float(FLOOR_Y), BOT_Z)
            fighter.pump(0.05)
        time.sleep(1.4)
        fighter.velocities.clear()
        fighter.attack(mark)
        fighter.pump(0.5)
        hits = [v for v in fighter.velocities if v[0] == mark]
        out[label] = {"packets": hits,
                      "blocks_per_tick": [[round(c / 8000.0, 6) for c in v[1:]] for v in hits]}
        print(f"  {label:14s} {out[label]['blocks_per_tick']}")
        fighter.sprint(False)
        fighter.move_to(BOT_X, float(FLOOR_Y), BOT_Z)
        fighter.pump(0.2)
    return out


TOOLS = [
    "minecraft:wooden_pickaxe", "minecraft:stone_pickaxe", "minecraft:iron_pickaxe",
    "minecraft:golden_pickaxe", "minecraft:diamond_pickaxe", "minecraft:netherite_pickaxe",
    "minecraft:wooden_sword", "minecraft:stone_sword", "minecraft:iron_sword",
    "minecraft:golden_sword", "minecraft:diamond_sword", "minecraft:netherite_sword",
    "minecraft:wooden_shovel", "minecraft:wooden_axe", "minecraft:wooden_hoe",
    "minecraft:shears", "minecraft:flint_and_steel", "minecraft:fishing_rod",
    "minecraft:bow", "minecraft:trident", "minecraft:shield", "minecraft:elytra",
    "minecraft:turtle_helmet", "minecraft:leather_helmet", "minecraft:chainmail_helmet",
    "minecraft:iron_helmet", "minecraft:golden_helmet", "minecraft:diamond_helmet",
    "minecraft:netherite_helmet", "minecraft:carrot_on_a_stick",
    "minecraft:warped_fungus_on_a_stick",
]


def read_damage(server: Server) -> tuple[bool, int | None]:
    """(still there, damage value) for whatever is in the bot's main hand."""
    lines = server.batch([f"data get entity {BOT} SelectedItem"])
    raw = scalar(lines)
    if raw is None:
        return (False, None)
    match = re.search(r"Damage: (\d+)", raw)
    return (True, int(match.group(1)) if match else 0)


def survival_break(server: Server, fighter: Fighter, x: int, y: int, z: int,
                   settle: float) -> None:
    """Break one block, letting the server's own clock finish it."""
    server.batch([f"setblock {x} {y} {z} minecraft:stone replace"])
    fighter.pump(0.1)
    fighter.dig(x, y, z, 0)
    fighter.pump(settle)
    fighter.dig(x, y, z, 2)
    fighter.pump(0.3)


#: The block each tool family can actually take off in a couple of seconds.
#:
#: Not decoration. The first run of this campaign broke *stone* with every tool,
#: including a wooden shovel and a wooden sword — neither of which gets through
#: stone in the two seconds the rig waited. The dig never finished, the tool
#: took no damage, and the bisection that followed measured nothing at all while
#: reporting a wooden shovel with 2926 points of durability. A number that large
#: is the tell; a number that was merely wrong would not have been.
DURABILITY_BLOCK = {
    "pickaxe": "minecraft:stone",
    "shovel": "minecraft:dirt",
    "axe": "minecraft:oak_planks",
    "hoe": "minecraft:hay_block",
    "sword": "minecraft:dirt",
    # Une toile, pas des feuilles. Des cisailles sur des feuilles, c'est un
    # cassage **instantané** (15 de vitesse pour 0,2 de dureté), et le chemin
    # instantané de vanilla n'est pas celui que prennent les autres outils : la
    # bissection est partie jusqu'en haut et a annoncé 4089 points. La toile
    # d'araignée a une dureté de 4 et demande huit ticks aux mêmes cisailles.
    "shears": "minecraft:cobweb",
}

DURABILITY_TOOLS = [
    f"minecraft:{material}_{family}"
    for family in ("pickaxe", "shovel", "axe", "hoe", "sword")
    for material in ("wooden", "stone", "iron", "golden", "diamond", "netherite")
] + ["minecraft:shears"]


def family_of(tool: str) -> str:
    for family in DURABILITY_BLOCK:
        if tool.endswith(family):
            return family
    raise KeyError(tool)


def campaign_durability(server: Server, fighter: Fighter) -> dict:
    """How many uses a tool has, and what one action costs it.

    The maximum is bisected rather than counted: an item handed over with
    `Damage:N` and made to do one action either survives or does not, and the
    largest N that survives is the answer in thirteen round trips instead of two
    thousand.

    What that largest N *means* is settled by arithmetic and then checked
    against a tool actually worn out from new. An item at `Damage:N` that takes
    one more point is destroyed when `N + 1` reaches the maximum, so the largest
    surviving N is `max - 2` and the maximum is `N + 2`. That is +2 and not +1,
    and the difference is one use in every tool in the game.
    """
    server.batch([f"gamemode survival {BOT}",
                  f"effect give {BOT} minecraft:resistance 100000 4 true",
                  f"effect give {BOT} minecraft:saturation 100000 4 true"])
    # **No Haste.** With Haste II a golden shovel takes dirt off in a single
    # tick, and vanilla's instant-break path is not the path every other tool in
    # this campaign takes. Two of the first run's rows came back from it — a
    # golden shovel with 1185 points and a golden hoe with 1503 — while every
    # tool slow enough to need a second tick was exact. A measurement whose
    # subjects do not all run the same code is not one measurement.
    fighter.pump(0.3)
    out: dict[str, dict] = {}

    bx, by, bz = 1, FLOOR_Y, 0

    def one_break(tool: str, damage: int) -> tuple[bool, int | None]:
        """Break one block and read the tool back, or say the break did not happen.

        The check that the block is actually gone is the whole of this. A
        bisection assumes its answers are monotone, and a single break that
        silently did not happen reads as "the tool survived" — which pushes the
        boundary up and stays there. That is exactly what produced an iron axe
        with 258 points of durability and a golden shovel with 1185, in a table
        where every other row was right. So a break that left the block standing
        is retried, and a break that never happens is reported rather than
        counted.
        """
        block = DURABILITY_BLOCK[family_of(tool)]
        for _ in range(4):
            server.batch([f"clear {BOT}", f"give {BOT} {tool}{{Damage:{damage}}} 1",
                          f"setblock {bx} {by} {bz} {block} replace"])
            fighter.hold(0)
            fighter.pump(0.3)
            fighter.dig(bx, by, bz, 0)
            fighter.pump(1.6)
            fighter.dig(bx, by, bz, 2)
            fighter.pump(0.3)
            gone = any("ovair" in line for line in server.batch(
                [f"execute if block {bx} {by} {bz} minecraft:air run say ovair"]))
            if gone:
                return read_damage(server)
        return (False, -1)

    for tool in DURABILITY_TOOLS:
        alive, damage_after = one_break(tool, 0)
        per_action = damage_after if alive and damage_after and damage_after > 0 else None
        if not per_action:
            # A cost of zero after a break that was supposed to cost something
            # means the block never came off. Reported rather than bisected: the
            # bisection would answer, and the answer would be noise.
            out[tool] = {"damage_per_break": per_action, "error": "the block did not break"}
            print(f"  {tool:32s} the block did not break", flush=True)
            continue
        lo, hi = 0, 4096  # lo survives one point, hi does not
        for _ in range(13):
            if hi - lo <= 1:
                break
            mid = (lo + hi) // 2
            still, _ = one_break(tool, mid)
            if still:
                lo = mid
            else:
                hi = mid
        # The boundary is confirmed rather than trusted: `lo` must survive and
        # `lo + 1` must not, three times each. A bisection is only as monotone
        # as its worst answer.
        confirmed = all(one_break(tool, lo)[0] for _ in range(3)) and \
            not any(one_break(tool, lo + 1)[0] for _ in range(3))
        # An item at `Damage: lo` takes `per_action` more points and survives;
        # one at `lo + 1` does not. So the maximum is `lo + per_action + 1` —
        # and the `per_action` in it is not decoration: a sword wears by two
        # per block and its largest survivor is three below its maximum, not
        # two. Reading `lo + 2` for everything gave 58 for a wooden sword and
        # 59 for a wooden pickaxe, which are the same tool tier.
        maximum = lo + per_action + 1
        out[tool] = {"damage_per_break": per_action, "last_surviving_damage": lo,
                     "max_damage": maximum, "confirmed": confirmed}
        print(f"  {tool:32s} per break {per_action}  max damage {maximum}"
              f"  {'confirmed' if confirmed else 'NOT CONFIRMED'}", flush=True)

    # The cross-check the bisection needs: one tool worn out from new, action by
    # action, until it is gone. A golden pickaxe is the cheapest such proof at
    # thirty-two uses.
    server.batch([f"clear {BOT}", f"give {BOT} minecraft:golden_pickaxe 1"])
    fighter.hold(0)
    fighter.pump(0.2)
    uses = 0
    for _ in range(80):
        server.batch([f"setblock {bx} {by} {bz} minecraft:stone replace"])
        fighter.pump(0.1)
        fighter.dig(bx, by, bz, 0)
        fighter.pump(1.0)
        fighter.dig(bx, by, bz, 2)
        fighter.pump(0.3)
        alive, _ = read_damage(server)
        uses += 1
        if not alive:
            break
    out["wear_out_check"] = {"item": "minecraft:golden_pickaxe", "uses_until_gone": uses}
    print(f"  worn out from new: golden pickaxe lasted {uses} breaks", flush=True)

    server.batch([f"gamemode creative {BOT}"])
    fighter.pump(0.3)
    return out


def campaign_durability_actions(server: Server, fighter: Fighter) -> dict:
    """What each *kind* of action costs a tool, in damage points."""
    server.batch([f"gamemode survival {BOT}",
                  f"effect give {BOT} minecraft:resistance 100000 4 true"])
    fighter.pump(0.3)
    out: dict[str, int | None] = {}

    def with_tool(spec: str) -> None:
        server.batch([f"clear {BOT}", f"give {BOT} {spec}{{Damage:0}} 1"])
        fighter.hold(0)
        fighter.pump(0.2)

    # Breaking a block the tool is for, and one it is not.
    with_tool("minecraft:diamond_pickaxe")
    survival_break(server, fighter, 1, FLOOR_Y, 0, 1.0)
    out["pickaxe_breaks_stone"] = read_damage(server)[1]

    with_tool("minecraft:diamond_sword")
    server.batch([f"setblock 1 {FLOOR_Y} 0 minecraft:dirt replace"])
    fighter.pump(0.1)
    fighter.dig(1, FLOOR_Y, 0, 0)
    fighter.pump(2.0)
    fighter.dig(1, FLOOR_Y, 0, 2)
    fighter.pump(0.3)
    out["sword_breaks_dirt"] = read_damage(server)[1]

    # Breaking something instant — a torch — costs nothing at all.
    with_tool("minecraft:diamond_pickaxe")
    server.batch([f"setblock 1 {FLOOR_Y} 0 minecraft:torch replace"])
    fighter.pump(0.1)
    fighter.dig(1, FLOOR_Y, 0, 0)
    fighter.pump(0.3)
    fighter.dig(1, FLOOR_Y, 0, 2)
    fighter.pump(0.3)
    out["pickaxe_breaks_torch"] = read_damage(server)[1]

    # Hitting a mob.
    for tool in ("minecraft:diamond_sword", "minecraft:diamond_pickaxe",
                 "minecraft:diamond_axe", "minecraft:diamond_shovel",
                 "minecraft:diamond_hoe", "minecraft:shears"):
        with_tool(tool)
        summon_pair(server)
        ids = spawn_ids(fighter, server)
        if "ovmark" not in ids:
            continue
        time.sleep(1.2)
        fighter.attack(ids["ovmark"])
        fighter.pump(0.4)
        out[f"attack/{tool}"] = read_damage(server)[1]

    # Tilling, shearing, lighting.
    with_tool("minecraft:diamond_hoe")
    server.batch([f"setblock 1 {FLOOR_Y} 0 minecraft:dirt replace",
                  f"setblock 1 {FLOOR_Y + 1} 0 minecraft:air replace"])
    fighter.pump(0.2)
    fighter.use_item_on(1, FLOOR_Y, 0, face=1)
    fighter.pump(0.4)
    out["hoe_tills_dirt"] = read_damage(server)[1]
    out["hoe_tills_dirt_result"] = scalar(
        server.batch([f"data get block 1 {FLOOR_Y} 0"]))

    with_tool("minecraft:flint_and_steel")
    server.batch([f"setblock 1 {FLOOR_Y} 0 minecraft:stone replace",
                  f"setblock 1 {FLOOR_Y + 1} 0 minecraft:air replace"])
    fighter.pump(0.2)
    fighter.use_item_on(1, FLOOR_Y, 0, face=1)
    fighter.pump(0.4)
    out["flint_and_steel_lights"] = read_damage(server)[1]
    out["flint_and_steel_result"] = scalar(
        server.batch([f"data get block 1 {FLOOR_Y + 1} 0"]))

    with_tool("minecraft:shears")
    server.batch([f"setblock 1 {FLOOR_Y + 1} 0 minecraft:oak_leaves[persistent=true] replace"])
    fighter.pump(0.2)
    fighter.dig(1, FLOOR_Y + 1, 0, 0)
    fighter.pump(0.5)
    fighter.dig(1, FLOOR_Y + 1, 0, 2)
    fighter.pump(0.3)
    out["shears_break_leaves"] = read_damage(server)[1]

    server.batch([f"gamemode creative {BOT}"])
    fighter.pump(0.3)
    print("  " + json.dumps(out))
    return out


def entity_tables() -> list[str]:
    directory = GENERATED / "data" / "minecraft" / "loot_tables" / "entities"
    return sorted(p.stem for p in directory.glob("*.json"))


def campaign_entity_loot(server: Server, fighter: Fighter, samples: int) -> dict:
    """Draw every entity table many times, through the server's own `/loot`.

    `/loot give <player> kill <entity>` runs the entity's table with no killer
    and no looting, which is a *context*, not a limitation: the same context is
    handed to our own interpreter, and a rare drop that this can never produce
    is measured by the `looting` campaign instead.
    """
    names = entity_tables()
    out: dict[str, dict] = {}
    for index, name in enumerate(names):
        entity = f"minecraft:{name}"
        totals: dict[str, int] = {}
        ok = True
        # Twenty-four draws at a time: an inventory holds thirty-six stacks and
        # a generous table fills it, after which the count stops on a number
        # that is entirely plausible and entirely wrong.
        for start in range(0, samples, 24):
            batch = min(24, samples - start)
            commands = ["kill @e[tag=ovloot]", f"clear {BOT}"]
            commands.append(f'summon {entity} {RIGHT_X} {FLOOR_Y} {BOT_Z} '
                            f'{{NoAI:1b,Silent:1b,PersistenceRequired:1b,NoGravity:1b,'
                            f'Tags:["ovloot"]}}')
            commands += [f"loot give {BOT} kill @e[tag=ovloot,limit=1]"] * batch
            commands.append(f"data get entity {BOT} Inventory")
            lines = server.batch(commands)
            if not any("Inventory" in line or "entity data" in line for line in lines):
                ok = False
                break
            raw = scalar(lines) or ""
            for item, amount in ITEM_RE.findall(raw):
                totals[item] = totals.get(item, 0) + int(amount)
        out[entity] = {"draws": samples, "items": totals, "ok": ok}
        if (index + 1) % 10 == 0:
            print(f"  {index + 1}/{len(names)}", flush=True)
    server.batch(["kill @e[tag=ovloot]", f"clear {BOT}"])
    return out


def campaign_looting(server: Server, fighter: Fighter, samples: int) -> dict:
    """Rare drops, the ones that need a player and a Looting sword.

    A mob that dies to anything but a player drops none of them, and `/loot`
    cannot supply a killer — so these are real kills, with a real sword.
    """
    out: dict[str, dict] = {}
    targets = ["minecraft:zombie", "minecraft:skeleton", "minecraft:creeper",
               "minecraft:spider", "minecraft:cow", "minecraft:sheep",
               "minecraft:enderman", "minecraft:blaze", "minecraft:witch"]
    for level in (0, 3):
        spec = ("minecraft:diamond_sword" if level == 0 else
                f'minecraft:diamond_sword{{Enchantments:[{{id:"minecraft:looting",lvl:{level}s}}]}}')
        server.batch([f"item replace entity {BOT} weapon.mainhand with {spec} 1"])
        for entity in targets:
            totals: dict[str, int] = {}
            for start in range(0, samples, 8):
                batch = min(8, samples - start)
                server.batch([f"clear {BOT}", "kill @e[tag=ovkill]"])
                for _ in range(batch):
                    server.batch([
                        f'summon {entity} {RIGHT_X} {FLOOR_Y} {BOT_Z} '
                        f'{{NoAI:1b,Silent:1b,PersistenceRequired:1b,NoGravity:1b,'
                        f'Tags:["ovkill"],Attributes:[{{Name:"generic.knockback_resistance",'
                        f'Base:1.0}}],Health:1.0f}}'])
                    ids = id_at(fighter, server, "ovkill", RIGHT_X, float(FLOOR_Y), BOT_Z)
                    if ids is None:
                        continue
                    time.sleep(0.7)
                    fighter.attack(ids)
                    fighter.pump(0.6)
                # The drops are on the floor; walk over them.
                server.batch([f"execute at {BOT} run tp @e[type=minecraft:item,distance=..12] "
                              f"{BOT_X} {FLOOR_Y} {BOT_Z}"])
                fighter.pump(1.5)
                raw = scalar(server.batch([f"data get entity {BOT} Inventory"])) or ""
                for item, amount in ITEM_RE.findall(raw):
                    if item == "minecraft:diamond_sword":
                        continue
                    totals[item] = totals.get(item, 0) + int(amount)
            out[f"looting{level}/{entity}"] = {"kills": samples, "items": totals}
            print(f"  looting{level} {entity:24s} {totals}")
    return out


def campaign_mob_xp(server: Server, fighter: Fighter, kills: int) -> dict:
    """The experience a kill drops, summed over every orb it produced."""
    out: dict[str, dict] = {}
    targets = ["minecraft:zombie", "minecraft:skeleton", "minecraft:creeper",
               "minecraft:spider", "minecraft:cow", "minecraft:sheep", "minecraft:pig",
               "minecraft:chicken", "minecraft:enderman", "minecraft:blaze",
               "minecraft:witch", "minecraft:slime", "minecraft:magma_cube",
               "minecraft:villager", "minecraft:zombie_villager", "minecraft:guardian",
               "minecraft:wither_skeleton", "minecraft:piglin", "minecraft:hoglin",
               "minecraft:squid", "minecraft:bat", "minecraft:rabbit", "minecraft:wolf"]
    server.batch([f"item replace entity {BOT} weapon.mainhand with minecraft:diamond_sword 1"])
    for entity in targets:
        values: list[int] = []
        for _ in range(kills):
            server.batch(["kill @e[type=minecraft:experience_orb]", "kill @e[tag=ovkill]"])
            extra = ",Size:1" if entity in ("minecraft:slime", "minecraft:magma_cube") else ""
            server.batch([
                f'summon {entity} {RIGHT_X} {FLOOR_Y} {BOT_Z} '
                f'{{NoAI:1b,Silent:1b,PersistenceRequired:1b,NoGravity:1b,Tags:["ovkill"],'
                f'Attributes:[{{Name:"generic.knockback_resistance",Base:1.0}}],'
                f'Health:1.0f{extra}}}'])
            who = id_at(fighter, server, "ovkill", RIGHT_X, float(FLOOR_Y), BOT_Z)
            if who is None:
                continue
            time.sleep(0.7)
            fighter.attack(who)
            fighter.pump(0.6)
            lines = server.batch(["execute as @e[type=minecraft:experience_orb] run "
                                  "data get entity @s Value"])
            total = 0
            for line in lines:
                match = DATA.search(line)
                if match:
                    total += int(match.group(1).strip().rstrip("s"))
            values.append(total)
        out[entity] = {"kills": len(values), "values": values,
                       "distinct": sorted(set(values))}
        print(f"  {entity:32s} {sorted(set(values))}")
    server.batch(["kill @e[type=minecraft:experience_orb]"])
    return out


USE_CASES = [
    # (label, item held, block placed at the target, face, what to read back)
    ("bucket_water_place", "minecraft:water_bucket", "minecraft:stone", 1, "above"),
    ("bucket_empty_on_water", "minecraft:bucket", "minecraft:water", 1, "self"),
    ("bucket_lava_place", "minecraft:lava_bucket", "minecraft:stone", 1, "above"),
    ("bucket_empty_on_lava", "minecraft:bucket", "minecraft:lava", 1, "self"),
    ("flint_and_steel_on_stone", "minecraft:flint_and_steel", "minecraft:stone", 1, "above"),
    ("flint_and_steel_on_tnt", "minecraft:flint_and_steel", "minecraft:tnt", 1, "self"),
    ("hoe_on_dirt", "minecraft:diamond_hoe", "minecraft:dirt", 1, "self"),
    ("hoe_on_grass_block", "minecraft:diamond_hoe", "minecraft:grass_block", 1, "self"),
    ("hoe_on_coarse_dirt", "minecraft:diamond_hoe", "minecraft:coarse_dirt", 1, "self"),
    ("hoe_on_rooted_dirt", "minecraft:diamond_hoe", "minecraft:rooted_dirt", 1, "self"),
    ("hoe_on_dirt_path", "minecraft:diamond_hoe", "minecraft:dirt_path", 1, "self"),
    ("shovel_on_grass_block", "minecraft:diamond_shovel", "minecraft:grass_block", 1, "self"),
    ("shovel_on_campfire", "minecraft:diamond_shovel", "minecraft:campfire", 1, "self"),
    ("axe_on_oak_log", "minecraft:diamond_axe", "minecraft:oak_log", 1, "self"),
    ("axe_on_stripped_oak_log", "minecraft:diamond_axe", "minecraft:stripped_oak_log", 1, "self"),
    ("axe_on_copper_block", "minecraft:diamond_axe", "minecraft:oxidized_copper", 1, "self"),
    ("axe_on_waxed_copper", "minecraft:diamond_axe", "minecraft:waxed_copper_block", 1, "self"),
    ("bonemeal_on_sapling", "minecraft:bone_meal", "minecraft:oak_sapling", 1, "self"),
    ("bonemeal_on_wheat", "minecraft:bone_meal", "minecraft:wheat[age=0]", 1, "self"),
    ("hand_on_oak_door", None, "minecraft:oak_door", 1, "self"),
    ("hand_on_iron_door", None, "minecraft:iron_door", 1, "self"),
    ("hand_on_oak_trapdoor", None, "minecraft:oak_trapdoor", 1, "self"),
    ("hand_on_iron_trapdoor", None, "minecraft:iron_trapdoor", 1, "self"),
    ("hand_on_oak_fence_gate", None, "minecraft:oak_fence_gate", 1, "self"),
    ("hand_on_lever", None, "minecraft:lever[face=floor]", 1, "self"),
    ("hand_on_stone_button", None, "minecraft:stone_button[face=floor]", 1, "self"),
    ("hand_on_oak_button", None, "minecraft:oak_button[face=floor]", 1, "self"),
    ("hand_on_note_block", None, "minecraft:note_block", 1, "self"),
    ("hand_on_repeater", None, "minecraft:repeater[delay=1]", 1, "self"),
    ("hand_on_comparator", None, "minecraft:comparator", 1, "self"),
    ("hand_on_daylight_detector", None, "minecraft:daylight_detector", 1, "self"),
    ("hand_on_cake", None, "minecraft:cake", 1, "self"),
    ("hand_on_lectern", None, "minecraft:lectern", 1, "self"),
    ("hand_on_candle", None, "minecraft:candle[lit=true]", 1, "self"),
    ("hand_on_tnt", None, "minecraft:tnt", 1, "self"),
    ("hand_on_redstone_ore", None, "minecraft:redstone_ore", 1, "self"),
    ("hand_on_dragon_egg", None, "minecraft:dragon_egg", 1, "self"),
    ("hand_on_bell", None, "minecraft:bell", 1, "self"),
]


def state_decoder() -> dict[int, str]:
    """State id -> "minecraft:x[a=b,...]", from the normalized block report.

    Built rather than guessed. The alternative — `data get block` — answers only
    for blocks that have a block *entity*, so a farmland or a lit candle comes
    back as "no block entity" and every reading in the campaign is null. The
    Block Update packet carries the state id itself, which is the thing we
    actually want to know.
    """
    with open(f"{NORMALIZED}/blocks.json") as handle:
        document = json.load(handle)
    out: dict[int, str] = {}
    for block in document["blocks"]:
        base = block["base_state"]
        properties = block.get("properties") or []
        for offset in range(block["state_count"]):
            parts = []
            for prop in properties:
                index = (offset // prop["stride"]) % len(prop["values"])
                parts.append(f"{prop['name']}={prop['values'][index]}")
            name = block["name"]
            out[base + offset] = name + ("[" + ",".join(parts) + "]" if parts else "")
    return out


def campaign_use(server: Server, fighter: Fighter) -> dict:
    """What using a thing on a block does, read off the Block Update packets.

    The rig places the block, empties the two blocks above it, puts the item in
    the bot's hand, and sends one Use Item On. Everything the server changed
    then arrives as Block Update, which names the position and the resulting
    **state id** — so the answer is the state the game produced, not a guess
    checked against a list of candidates.
    """
    decode = state_decoder()
    out: dict[str, dict] = {}
    x, y, z = 1, FLOOR_Y, 0
    for label, item, block, face, _where in USE_CASES:
        server.batch([
            f"setblock {x} {y + 1} {z} minecraft:air replace",
            f"setblock {x} {y + 2} {z} minecraft:air replace",
            f"setblock {x} {y} {z} {block} replace",
            "kill @e[type=minecraft:tnt]",
            "kill @e[type=minecraft:item]",
            f"item replace entity {BOT} weapon.mainhand with "
            + ("minecraft:air" if item is None else f"{item} 1"),
        ])
        fighter.pump(0.3)
        fighter.captured.clear()
        fighter.use_item_on(x, y, z, face=face)
        fighter.pump(0.6)

        changes: dict[str, str] = {}
        for packet_id, payload in fighter.captured:
            if packet_id != 0x0A:  # Block Update
                continue
            packed = struct.unpack_from(">q", payload, 0)[0]
            bx = packed >> 38
            by = (packed << 52 & 0xFFFFFFFFFFFFFFFF) >> 52
            bz = (packed << 26 & 0xFFFFFFFFFFFFFFFF) >> 38
            bx = bx - (1 << 26) if bx >= (1 << 25) else bx
            by = by - (1 << 12) if by >= (1 << 11) else by
            bz = bz - (1 << 26) if bz >= (1 << 25) else bz
            state, _ = read_varint(payload, 8)
            changes[f"{bx},{by},{bz}"] = decode.get(state, f"state {state}")
        held = scalar(server.batch([f"data get entity {BOT} SelectedItem"]))
        spawned = scalar(server.batch(["execute if entity @e[type=minecraft:tnt] "
                                       "run data get entity @e[type=minecraft:tnt,limit=1] Fuse"]))
        out[label] = {"item": item, "block": block, "changes": changes,
                      "held_after": held, "primed_tnt": spawned}
        summary = ", ".join(f"{k} -> {v}" for k, v in changes.items()) or "no block change"
        print(f"  {label:32s} {summary[:70]:70s} held {str(held)[:50]}", flush=True)
    return out


CAMPAIGNS = ["attrs", "weapons", "gauge", "crit", "sweep", "knockback",
             "durability", "actions", "use", "mob_xp", "entity_loot", "looting"]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", default="")
    parser.add_argument("--samples", type=int, default=256)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("out", nargs="?", default=str(NORMALIZED / "combat.json"))
    args = parser.parse_args()
    wanted = [c for c in (args.only.split(",") if args.only else CAMPAIGNS) if c]
    for name in wanted:
        if name not in CAMPAIGNS:
            print(f"unknown campaign {name!r}; known: {', '.join(CAMPAIGNS)}")
            return 2

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    document: dict = {}
    if out_path.is_file():
        document = json.loads(out_path.read_text())
    document.setdefault("$comment",
                        "Mesures de combat relevees sur un vrai serveur 1.20.1. "
                        "Voir docs/provenance/combat.md.")

    server = Server(RUN, port=PORT)
    fighter: Fighter | None = None
    try:
        fighter = Fighter(PORT)
        fighter.pump(3.0)
        if fighter.position is None:
            raise RuntimeError("the bot was never told where it is")
        setup_rig(server, fighter)
        print(f"bot entity id {fighter.entity_id} at {fighter.position}")

        def save() -> None:
            out_path.write_text(json.dumps(document, indent=1, sort_keys=True))

        for name in wanted:
            print(f"\n── {name} ──", flush=True)
            started = time.monotonic()
            if name == "attrs":
                document["attrs"] = campaign_attrs(server)
            elif name == "weapons":
                document["weapons"] = campaign_weapons(server, fighter)
            elif name == "gauge":
                # Twenty-five milliseconds is half a tick, so the grid crosses
                # every tick boundary twice and the staircase's risers are
                # located rather than assumed.
                document["gauge"] = campaign_gauge(
                    server, fighter, "minecraft:diamond_sword",
                    list(range(0, 800, 25)), args.repeats)
                document["gauge_axe"] = campaign_gauge(
                    server, fighter, "minecraft:diamond_axe",
                    list(range(0, 1200, 50)), args.repeats)
            elif name == "crit":
                document["crit"] = campaign_crit(server, fighter, "minecraft:diamond_sword")
            elif name == "sweep":
                document["sweep"] = campaign_sweep(server, fighter)
            elif name == "knockback":
                document["knockback"] = campaign_knockback(server, fighter)
            elif name == "durability":
                document["durability"] = campaign_durability(server, fighter)
            elif name == "actions":
                document["durability_actions"] = campaign_durability_actions(server, fighter)
            elif name == "use":
                document["use"] = campaign_use(server, fighter)
            elif name == "mob_xp":
                document["mob_xp"] = campaign_mob_xp(server, fighter, 8)
            elif name == "entity_loot":
                document["entity_loot"] = campaign_entity_loot(server, fighter, args.samples)
            elif name == "looting":
                document["looting"] = campaign_looting(server, fighter, 32)
            save()
            print(f"   {name} took {time.monotonic() - started:.0f} s", flush=True)
        save()
    finally:
        if fighter is not None:
            try:
                fighter.socket.close()
            except OSError:
                pass
        server.stop()
    print(f"\nwritten to {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
