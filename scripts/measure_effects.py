#!/usr/bin/env python3
"""Ask a real 1.20.1 server what status effects and attribute modifiers do.

Nothing about effects is in the data generator's reports. The registry gives
thirty-three names and their (1-based) ids, and that is all: the interval at
which regeneration heals, the modifier speed adds, the colour of a potion
swirl and the rule that lets a strong effect hide a weak one are Java code.
So every number this project keeps about them was asked of the game, here.

Campaigns
---------

  bounds       The clamp of every attribute. `attribute ... base set` accepts
               any double; `attribute ... get` answers the clamped value. Each
               attribute is set to 1e9 and to -1e9 on an entity that owns it.

  ops          The three modifier operations and their order, on a zombie's
               movement speed, with modifiers added by `attribute ... modifier
               add` and the total read back after each one. Compared bit for
               bit: the console prints doubles with Java's Double.toString,
               which round-trips.

  modifiers    Every effect at amplifiers 0, 1 and 4, on a zombie and on the
               bot, with the whole `Attributes` list read back as SNBT — so the
               modifiers an effect carries (UUID, name, amount, operation) are
               read, not recalled — and `attribute ... get` for each attribute
               that carries one.

  immunity     `effect give` of all 33 effects to a zombie, a spider and a cow.
               The console says "Applied" or "Unable to apply".

  periodic     Regeneration, poison and wither on cows. Each cow is summoned
               with the effect in its NBT at an exact duration D (in ticks, not
               the seconds `effect give` allows), and left until the effect has
               expired. The health gained or lost over the whole duration is a
               count that does not depend on console timing at all, and the
               function D -> count pins the interval and the boundary.

  instant      Instant health and instant damage, amplifiers 0..5, on cows and
               on zombies (undead are inverted).

  resistance   A cow per resistance amplifier 0..5, hit for 10 and for 7 with
               minecraft:generic, and for 10 with a type in #bypasses_resistance.

  absorption   AbsorptionAmount per amplifier, what a hit takes from it, and
               what is left when the effect is cleared.

  health_boost The max-health modifier, and the clamp of Health when it goes.

  replace      The replacement rules and the hidden effect: pairs of `effect
               give` in one batch, the ActiveEffects list read back, then read
               again once the stronger one has expired.

  packets      Entity Effect, Remove Entity Effect and Update Attributes as the
               bot receives them, identified by payload rather than by id.

  metadata     The living-entity metadata an effect changes, one effect at a
               time on a zombie, read from Set Entity Metadata.

  nbt_file     A real playerdata file, written by the server with effects on
               the bot — hidden, infinite, darkness and hidden particles — and
               read back with types.

  saturation   The saturation effect per amplifier from a known baseline, and
               the hunger effect's exhaustion per tick.

  food         What each effect-bearing food applies, and how often.

  death        Whether effects survive a death.

  breaking     Ticks to break under haste, conduit power and mining fatigue,
               with a second client that mines on the server's own clock (the
               method of scripts/measure_hardness.py).

  infinite     The rate of an infinite regeneration, which cannot count down.

Usage: python3 scripts/measure_effects.py [--only a,b,...] [out.json]
Writes data/vanilla/1.20.1/normalized/effects.json after every campaign.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import math
import re
import struct
import sys
import time
import uuid as uuidlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import measure_survival as ms  # noqa: E402
from capture_entity_packets import read_varint, varint  # noqa: E402
from measure_entities import Server  # noqa: E402
from vanilla_miner import Miner  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
GENERATED = ROOT / "data" / "vanilla" / "1.20.1" / "generated"
RUN = ROOT / "run" / "effects-oracle"
PORT = 25611
BOT = "oveffects"
MINER = "ovminer"

# measure_survival's helpers (drain_food, heal, read_player) address the player
# through that module's own global. Pointing it at this bot is the whole
# integration.
ms.BOT = BOT

DATA = re.compile(r"following entity data: (.*)$")
VALUE = re.compile(r" is (-?[0-9][0-9.E\-]*)$")
SET_TO = re.compile(r" set to (-?[0-9][0-9.E\-]*)$")

REGISTRIES = json.loads((GENERATED / "reports" / "registries.json").read_text())
EFFECTS = [name for name, _ in sorted(REGISTRIES["minecraft:mob_effect"]["entries"].items(),
                                      key=lambda kv: kv[1]["protocol_id"])]
EFFECT_ID = {name: e["protocol_id"]
             for name, e in REGISTRIES["minecraft:mob_effect"]["entries"].items()}
ATTRIBUTES = [name for name, _ in sorted(REGISTRIES["minecraft:attribute"]["entries"].items(),
                                         key=lambda kv: kv[1]["protocol_id"])]
ENTITY_ID = {name: e["protocol_id"]
             for name, e in REGISTRIES["minecraft:entity_type"]["entries"].items()}

CB_ENTITY_EFFECT_GUESS = 0x6C   # only used to *label*; identification is by payload
CB_SET_METADATA = 0x52
CB_SPAWN_ENTITY = 0x01


# ── SNBT, as the console prints it ──────────────────────────────────────────


def parse_snbt(text: str):
    """Just enough SNBT for `data get entity` output. Numbers keep their type
    as a suffix-derived tag so a double is never confused with a float."""
    pos = 0

    def ws() -> None:
        nonlocal pos
        while pos < len(text) and text[pos] in " \t\n":
            pos += 1

    def bare(token: str):
        if token in ("true", "false"):
            return token == "true"
        suffix = token[-1]
        if suffix in "bB":
            return {"t": "byte", "v": int(token[:-1])}
        if suffix in "sS":
            return {"t": "short", "v": int(token[:-1])}
        if suffix in "lL":
            return {"t": "long", "v": int(token[:-1])}
        if suffix in "fF":
            return {"t": "float", "v": float(token[:-1])}
        if suffix in "dD":
            return {"t": "double", "v": float(token[:-1])}
        if "." in token or "E" in token or "e" in token:
            return {"t": "double", "v": float(token)}
        try:
            return {"t": "int", "v": int(token)}
        except ValueError:
            return token

    def key() -> str:
        nonlocal pos
        ws()
        if text[pos] in "\"'":
            return value()
        start = pos
        while text[pos] not in ":":
            pos += 1
        return text[start:pos].strip()

    def value():
        nonlocal pos
        ws()
        c = text[pos]
        if c == "{":
            pos += 1
            out = {}
            ws()
            if text[pos] == "}":
                pos += 1
                return out
            while True:
                k = key()
                ws()
                assert text[pos] == ":", text[pos:pos + 20]
                pos += 1
                out[k] = value()
                ws()
                if text[pos] == ",":
                    pos += 1
                    continue
                assert text[pos] == "}", text[pos:pos + 20]
                pos += 1
                return out
        if c == "[":
            pos += 1
            ws()
            typed = None
            if text[pos] in "BIL" and text[pos + 1] == ";":
                typed = text[pos]
                pos += 2
            out = []
            ws()
            if text[pos] == "]":
                pos += 1
                return {"t": f"{typed}array", "v": out} if typed else out
            while True:
                item = value()
                out.append(item["v"] if typed and isinstance(item, dict) else item)
                ws()
                if text[pos] == ",":
                    pos += 1
                    continue
                assert text[pos] == "]", text[pos:pos + 20]
                pos += 1
                return {"t": f"{typed}array", "v": out} if typed else out
        if c in "\"'":
            quote = c
            pos += 1
            chars = []
            while text[pos] != quote:
                if text[pos] == "\\":
                    pos += 1
                chars.append(text[pos])
                pos += 1
            pos += 1
            return "".join(chars)
        start = pos
        while pos < len(text) and text[pos] not in ",]} ":
            pos += 1
        return bare(text[start:pos])

    return value()


def plain(node):
    """Strip the type tags, for printing."""
    if isinstance(node, dict) and set(node) == {"t", "v"}:
        return node["v"]
    if isinstance(node, dict):
        return {k: plain(v) for k, v in node.items()}
    if isinstance(node, list):
        return [plain(v) for v in node]
    return node


def uuid_of(int_array: list[int]) -> str:
    raw = b"".join(struct.pack(">i", part) for part in int_array)
    return str(uuidlib.UUID(bytes=raw))


def data_get(server: Server, target: str, path: str):
    lines = server.batch([f"data get entity {target} {path}"])
    for line in lines:
        match = DATA.search(line)
        if match:
            return parse_snbt(match.group(1).strip())
    return None


def number_of(lines: list[str], pattern=VALUE) -> float | None:
    for line in lines:
        match = pattern.search(line)
        if match:
            return float(match.group(1))
    return None


def offline_uuid(name: str) -> str:
    digest = bytearray(hashlib.md5(f"OfflinePlayer:{name}".encode()).digest())
    digest[6] = (digest[6] & 0x0F) | 0x30
    digest[8] = (digest[8] & 0x3F) | 0x80
    return str(uuidlib.UUID(bytes=bytes(digest)))


# ── Campaigns ───────────────────────────────────────────────────────────────


class Rig:
    """Where things are put. Everything stays within a few chunks of the bot,
    which keeps it inside simulation distance as well as force-loaded."""

    def __init__(self, x: float, y: float, z: float) -> None:
        self.x, self.y, self.z = x, y, z
        self.next_tag = 0

    def tag(self, prefix: str) -> str:
        self.next_tag += 1
        return f"{prefix}{self.next_tag}"


def summon(kind: str, x: float, y: float, z: float, tag: str, extra: str = "") -> str:
    body = f"NoAI:1b,Silent:1b,PersistenceRequired:1b,Tags:[\"{tag}\",\"ovfx\"]"
    if extra:
        body += "," + extra
    return f"summon {kind} {x:.1f} {y:.1f} {z:.1f} {{{body}}}"


def sel(tag: str) -> str:
    return f"@e[tag={tag},limit=1]"


def campaign_bounds(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    hosts = {"zombie": rig.tag("bz"), "horse": rig.tag("bh"), "parrot": rig.tag("bp")}
    server.batch([summon("minecraft:zombie", rig.x + 3, rig.y, rig.z - 6, hosts["zombie"]),
                  summon("minecraft:horse", rig.x + 6, rig.y, rig.z - 6, hosts["horse"]),
                  summon("minecraft:parrot", rig.x + 9, rig.y, rig.z - 6, hosts["parrot"])])
    bot.pump(0.5)
    targets = [("zombie", sel(hosts["zombie"])), ("horse", sel(hosts["horse"])),
               ("parrot", sel(hosts["parrot"])), ("player", BOT)]
    out = {}
    for attribute in ATTRIBUTES:
        for host, target in targets:
            base_lines = server.batch([f"attribute {target} {attribute} base get"])
            base = number_of(base_lines)
            if base is None:
                continue
            high_set = server.batch([f"attribute {target} {attribute} base set 1000000000"])
            high = number_of(server.batch([f"attribute {target} {attribute} get"]))
            high_base = number_of(server.batch([f"attribute {target} {attribute} base get"]))
            server.batch([f"attribute {target} {attribute} base set -1000000000"])
            low = number_of(server.batch([f"attribute {target} {attribute} get"]))
            server.batch([f"attribute {target} {attribute} base set {base!r}"])
            out[attribute] = {"host": host, "default_base": base, "max": high, "min": low,
                              "stored_base_after_high": high_base,
                              "set_reply": high_set[-1:] if high_set else []}
            print(f"  {attribute}: [{low}, {high}] (base {base} on {host})")
            break
        else:
            out[attribute] = {"host": None}
            print(f"  {attribute}: no host among {[h for h, _ in targets]}")
        bot.pump(0.05)
    heal_bot(server, bot)
    server.batch(["kill @e[tag=ovfx]"])
    return out


def campaign_player_bases(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    """The player's own attribute bases, read before anything has touched them.

    A player cannot be summoned, so the entity sweep of measure_entities.py
    never saw one; these are the only measurement of them. Run first."""
    out = {}
    for attribute in ATTRIBUTES:
        base = number_of(server.batch([f"attribute {BOT} {attribute} base get"]))
        value = number_of(server.batch([f"attribute {BOT} {attribute} get"]))
        if base is not None:
            out[attribute] = {"base": base, "value": value}
            print(f"  {attribute}: base {base!r}, value {value!r}")
    return out


def heal_bot(server: Server, bot: ms.Bot) -> None:
    # No `base set` here, ever: an earlier version reset the movement speed to
    # 0.1 in this helper, and the 0.1 then read off an Update Attributes packet
    # looked like a measurement of the player's default. It was our own write.
    server.batch([f"effect clear {BOT}",
                  f"effect give {BOT} minecraft:instant_health 1 5 true"])
    bot.pump(0.3)
    server.batch([f"effect clear {BOT}"])


def campaign_ops(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    tag = rig.tag("op")
    server.batch([summon("minecraft:zombie", rig.x + 3, rig.y, rig.z - 9, tag)])
    bot.pump(0.4)
    target = sel(tag)
    attr = "minecraft:generic.movement_speed"
    steps = [
        ("00000000-0000-0000-0000-00000000a001", "add", 0.1),
        ("00000000-0000-0000-0000-00000000a002", "multiply_base", 0.5),
        ("00000000-0000-0000-0000-00000000a003", "multiply", 0.25),
        ("00000000-0000-0000-0000-00000000a004", "multiply", -0.5),
        ("00000000-0000-0000-0000-00000000a005", "add", 0.07),
        ("00000000-0000-0000-0000-00000000a006", "multiply_base", -0.3),
    ]
    base = number_of(server.batch([f"attribute {target} {attr} base get"]))
    trace = []
    for uid, operation, amount in steps:
        reply = server.batch([f"attribute {target} {attr} modifier add {uid} ovprobe {amount} {operation}"])
        total = number_of(server.batch([f"attribute {target} {attr} get"]))
        value = number_of(server.batch([f"attribute {target} {attr} modifier value get {uid}"]))
        trace.append({"uuid": uid, "operation": operation, "amount": amount,
                      "modifier_value": value, "total": total, "reply": reply[-1:]})
        print(f"  +{operation} {amount}: total {total!r}")
    listing = data_get(server, target, "Attributes")
    # Clamps reached through modifiers rather than through the base.
    clamps = {}
    for attribute, amount in (("minecraft:generic.max_health", 5000.0),
                              ("minecraft:generic.knockback_resistance", 5.0),
                              ("minecraft:generic.armor", 100.0)):
        server.batch([f"attribute {target} {attribute} modifier add "
                      f"00000000-0000-0000-0000-00000000b001 ovclamp {amount} add"])
        clamps[attribute] = number_of(server.batch([f"attribute {target} {attribute} get"]))
        server.batch([f"attribute {target} {attribute} modifier remove "
                      f"00000000-0000-0000-0000-00000000b001"])
    # Removing a modifier that is not there, and adding one twice.
    again = server.batch([f"attribute {target} {attr} modifier add {steps[0][0]} ovprobe 0.3 add"])
    server.batch(["kill @e[tag=ovfx]"])
    return {"attribute": attr, "base": base, "trace": trace, "clamps": clamps,
            "duplicate_add_reply": again[-1:], "listing": plain(listing)}


def modifiers_of(listing) -> list[dict]:
    out = []
    for entry in listing or []:
        for modifier in entry.get("Modifiers", []):
            out.append({"attribute": entry.get("Name"),
                        "base": plain(entry.get("Base")),
                        "name": modifier.get("Name"),
                        "uuid": uuid_of(plain(modifier["UUID"])),
                        "amount": plain(modifier.get("Amount")),
                        "operation": plain(modifier.get("Operation"))})
    return out


def campaign_modifiers(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    tag = rig.tag("mz")
    server.batch([summon("minecraft:zombie", rig.x + 3, rig.y, rig.z - 12, tag,
                         'Attributes:[{Name:"minecraft:generic.max_health",Base:1000d}],Health:1000f')])
    bot.pump(0.4)
    out = {"zombie": {}, "player": {}}
    for who, target in (("zombie", sel(tag)), ("player", BOT)):
        for effect in EFFECTS:
            if who == "player" and effect in ("minecraft:instant_damage", "minecraft:instant_health"):
                continue
            per_amp = {}
            for amp in (0, 1, 4):
                server.batch([f"effect give {target} {effect} 30 {amp} true"])
                listing = data_get(server, target, "Attributes")
                mods = [m for m in modifiers_of(listing) if m["name"] != "ovprobe"]
                totals = {}
                for m in mods:
                    totals[m["attribute"]] = number_of(
                        server.batch([f"attribute {target} {m['attribute']} get"]))
                server.batch([f"effect clear {target}"])
                per_amp[str(amp)] = {"modifiers": mods, "totals": totals}
                if who == "player":
                    bot.pump(0.05)
            out[who][effect] = per_amp
            if any(per_amp[a]["modifiers"] for a in per_amp):
                first = per_amp["0"]["modifiers"]
                print(f"  {who} {effect}: " + ", ".join(
                    f"{m['attribute']} {m['amount']!r} op{m['operation']}" for m in first))
            if who == "player":
                heal_bot(server, bot)
    server.batch(["kill @e[tag=ovfx]"])
    return out


def campaign_immunity(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    out = {}
    for index, kind in enumerate(("minecraft:zombie", "minecraft:spider", "minecraft:cow",
                                  "minecraft:skeleton")):
        tag = rig.tag("im")
        server.batch([summon(kind, rig.x + 3 + 4 * index, rig.y, rig.z - 16, tag,
                             'Attributes:[{Name:"minecraft:generic.max_health",Base:1000d}],'
                             'Health:1000f')])
        bot.pump(0.3)
        refused = []
        for effect in EFFECTS:
            if effect in ("minecraft:instant_damage", "minecraft:instant_health"):
                continue
            reply = server.batch([f"effect give {sel(tag)} {effect} 10 0 true"])
            text = " ".join(reply)
            if "Unable" in text or "immune" in text:
                refused.append(effect)
            server.batch([f"effect clear {sel(tag)}"])
        out[kind] = refused
        print(f"  {kind}: refuses {refused}")
    server.batch(["kill @e[tag=ovfx]"])
    return out


PERIODIC_DURATIONS = [1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 19, 20, 21, 24, 25, 26,
                      39, 40, 41, 49, 50, 51, 99, 100, 101, 150, 151]


def campaign_periodic(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    """Cows summoned with an exact-duration effect in their NBT; the whole
    effect is counted once it has expired."""
    start_health = 500.0
    cows = []
    commands = []
    row = 0
    for effect in ("minecraft:regeneration", "minecraft:poison", "minecraft:wither"):
        for amp in range(6):
            for column, duration in enumerate(PERIODIC_DURATIONS):
                tag = rig.tag("pc")
                x = rig.x + 4 + column * 2
                z = rig.z + 6 + row * 2
                nbt = ('Attributes:[{Name:"minecraft:generic.max_health",Base:1000d}],'
                       f'Health:{start_health}f,ActiveEffects:[{{Id:{EFFECT_ID[effect]},'
                       f'Amplifier:{amp}b,Duration:{duration},ShowParticles:0b}}]')
                commands.append(summon("minecraft:cow", x, rig.y, z, tag, nbt))
                cows.append({"tag": tag, "effect": effect, "amplifier": amp,
                             "duration": duration})
            row += 1
    # Controls: no effect at all, and poison at the one-health floor.
    control = rig.tag("pc")
    commands.append(summon("minecraft:cow", rig.x + 2, rig.y, rig.z + 6, control,
                           'Attributes:[{Name:"minecraft:generic.max_health",Base:1000d}],'
                           f'Health:{start_health}f'))
    floor_tags = []
    for amp in (0, 3, 5):
        tag = rig.tag("pf")
        floor_tags.append((tag, amp))
        commands.append(summon("minecraft:cow", rig.x + 2, rig.y, rig.z + 10 + 2 * amp, tag,
                               'Attributes:[{Name:"minecraft:generic.max_health",Base:1000d}],'
                               f'Health:3f,ActiveEffects:[{{Id:19,Amplifier:{amp}b,'
                               'Duration:300,ShowParticles:0b}]'))
    wither_floor = rig.tag("wf")
    commands.append(summon("minecraft:cow", rig.x + 2, rig.y, rig.z + 24, wither_floor,
                           'Attributes:[{Name:"minecraft:generic.max_health",Base:1000d}],'
                           'Health:3f,ActiveEffects:[{Id:20,Amplifier:3b,'
                           'Duration:300,ShowParticles:0b}]'))
    for i in range(0, len(commands), 40):
        server.batch(commands[i:i + 40])
        bot.pump(0.05)
    print(f"  {len(cows)} cows summoned; waiting for every effect to expire")
    bot.pump(max(PERIODIC_DURATIONS) / 20.0 + 3.0)
    bot.pump(12.0)  # the 300-tick floor cows
    results = []
    for cow in cows:
        health = ms.number(server.batch([f"data get entity {sel(cow['tag'])} Health"]))
        cow = dict(cow)
        cow["health"] = health
        cow["delta"] = None if health is None else round(health - start_health, 4)
        results.append(cow)
    control_health = ms.number(server.batch([f"data get entity {sel(control)} Health"]))
    floors = {}
    for tag, amp in floor_tags:
        floors[f"poison{amp}"] = ms.number(server.batch([f"data get entity {sel(tag)} Health"]))
    floors["wither3"] = ms.number(server.batch([f"data get entity {sel(wither_floor)} Health"]))
    server.batch(["kill @e[tag=ovfx]"])
    bot.pump(0.5)
    print(f"  control {control_health}, floors {floors}")
    return {"start_health": start_health, "control": control_health, "floors": floors,
            "cows": results}


def campaign_instant(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    out = []
    commands = []
    subjects = []
    for index, kind in enumerate(("minecraft:cow", "minecraft:zombie")):
        for effect in ("minecraft:instant_health", "minecraft:instant_damage"):
            for amp in range(6):
                tag = rig.tag("in")
                commands.append(summon(kind, rig.x - 6 - amp * 2, rig.y,
                                       rig.z + 6 + index * 8 + (4 if "damage" in effect else 0),
                                       tag,
                                       'Attributes:[{Name:"minecraft:generic.max_health",'
                                       'Base:1000d}],Health:500f'))
                subjects.append((tag, kind, effect, amp))
    server.batch(commands)
    bot.pump(0.5)
    server.batch([f"effect give {sel(tag)} {effect} 1 {amp}" for tag, _, effect, amp in subjects])
    bot.pump(1.0)
    for tag, kind, effect, amp in subjects:
        health = ms.number(server.batch([f"data get entity {sel(tag)} Health"]))
        out.append({"entity": kind, "effect": effect, "amplifier": amp,
                    "delta": None if health is None else round(health - 500.0, 4)})
        print(f"  {kind} {effect} {amp}: {out[-1]['delta']}")
    server.batch(["kill @e[tag=ovfx]"])
    return {"results": out}


def campaign_resistance(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    out = []
    subjects = []
    commands = []
    for amp in range(6):
        for hit, kind in ((10.0, "minecraft:generic"), (7.0, "minecraft:generic"),
                          (3.0, "minecraft:generic"), (10.0, "minecraft:out_of_world"),
                          (10.0, "minecraft:magic"), (10.0, "minecraft:starve")):
            tag = rig.tag("rs")
            commands.append(summon("minecraft:cow", rig.x - 6 - len(subjects) % 12 * 2, rig.y,
                                   rig.z - 6 - len(subjects) // 12 * 2, tag,
                                   'Attributes:[{Name:"minecraft:generic.max_health",'
                                   'Base:1000d}],Health:500f'))
            subjects.append((tag, amp, hit, kind))
    server.batch(commands)
    bot.pump(0.5)
    server.batch([f"effect give {sel(tag)} minecraft:resistance 60 {amp}"
                  for tag, amp, _, _ in subjects])
    bot.pump(0.3)
    server.batch([f"damage {sel(tag)} {hit} {kind}" for tag, _, hit, kind in subjects])
    bot.pump(0.5)
    for tag, amp, hit, kind in subjects:
        health = ms.number(server.batch([f"data get entity {sel(tag)} Health"]))
        lost = None if health is None else round(500.0 - health, 5)
        out.append({"amplifier": amp, "hit": hit, "type": kind, "lost": lost})
        print(f"  resistance {amp}, {kind} {hit}: lost {lost}")
    server.batch(["kill @e[tag=ovfx]"])
    return {"results": out}


def campaign_absorption(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    out = []
    for amp in range(4):
        tag = rig.tag("ab")
        server.batch([summon("minecraft:cow", rig.x - 8, rig.y, rig.z - 20 - amp * 2, tag,
                             'Attributes:[{Name:"minecraft:generic.max_health",Base:100d}],'
                             'Health:50f')])
        bot.pump(0.3)
        server.batch([f"effect give {sel(tag)} minecraft:absorption 60 {amp}"])
        bot.pump(0.3)
        given = ms.number(server.batch([f"data get entity {sel(tag)} AbsorptionAmount"]))
        server.batch([f"damage {sel(tag)} 3 minecraft:generic"])
        bot.pump(0.3)
        after_hit = ms.number(server.batch([f"data get entity {sel(tag)} AbsorptionAmount"]))
        health_after_hit = ms.number(server.batch([f"data get entity {sel(tag)} Health"]))
        # A second, larger hit that goes through the whole pool, a second
        # later so the invulnerability window has closed.
        bot.pump(1.2)
        server.batch([f"damage {sel(tag)} 30 minecraft:generic"])
        bot.pump(0.3)
        after_big = ms.number(server.batch([f"data get entity {sel(tag)} AbsorptionAmount"]))
        health_after_big = ms.number(server.batch([f"data get entity {sel(tag)} Health"]))
        # Re-applied at a lower amplifier over a pool, then cleared.
        server.batch([f"effect give {sel(tag)} minecraft:absorption 60 {amp}"])
        bot.pump(0.3)
        refill = ms.number(server.batch([f"data get entity {sel(tag)} AbsorptionAmount"]))
        server.batch([f"effect clear {sel(tag)} minecraft:absorption"])
        bot.pump(0.3)
        cleared = ms.number(server.batch([f"data get entity {sel(tag)} AbsorptionAmount"]))
        row = {"amplifier": amp, "given": given, "after_hit_3": after_hit,
               "health_after_hit_3": health_after_hit, "after_hit_30": after_big,
               "health_after_hit_30": health_after_big, "refill": refill,
               "after_clear": cleared}
        out.append(row)
        print(f"  absorption {amp}: {row}")
    server.batch(["kill @e[tag=ovfx]"])
    return {"results": out}


def campaign_health_boost(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    tag = rig.tag("hb")
    server.batch([summon("minecraft:cow", rig.x - 12, rig.y, rig.z - 20, tag)])
    bot.pump(0.3)
    start = ms.number(server.batch([f"data get entity {sel(tag)} Health"]))
    server.batch([f"effect give {sel(tag)} minecraft:health_boost 60 1"])
    bot.pump(0.3)
    boosted_max = number_of(server.batch(
        [f"attribute {sel(tag)} minecraft:generic.max_health get"]))
    health_on_boost = ms.number(server.batch([f"data get entity {sel(tag)} Health"]))
    server.batch([f"effect give {sel(tag)} minecraft:instant_health 1 3"])
    bot.pump(0.3)
    healed = ms.number(server.batch([f"data get entity {sel(tag)} Health"]))
    server.batch([f"effect clear {sel(tag)} minecraft:health_boost"])
    bot.pump(0.3)
    after = ms.number(server.batch([f"data get entity {sel(tag)} Health"]))
    after_max = number_of(server.batch(
        [f"attribute {sel(tag)} minecraft:generic.max_health get"]))
    server.batch(["kill @e[tag=ovfx]"])
    out = {"start": start, "boosted_max": boosted_max, "health_on_boost": health_on_boost,
           "healed": healed, "after_clear": after, "max_after_clear": after_max}
    print(f"  {out}")
    return out


REPLACE_CASES = {
    "stronger_shorter": [("speed", 30, 0), ("speed", 5, 2)],
    "weaker_longer": [("speed", 5, 2), ("speed", 30, 0)],
    "stronger_longer": [("speed", 5, 0), ("speed", 30, 2)],
    "weaker_shorter": [("speed", 30, 2), ("speed", 5, 0)],
    "equal_longer": [("speed", 5, 1), ("speed", 30, 1)],
    "equal_shorter": [("speed", 30, 1), ("speed", 5, 1)],
    "infinite_under_stronger": [("speed", "infinite", 0), ("speed", 5, 2)],
    "finite_under_infinite": [("speed", "infinite", 2), ("speed", 30, 0)],
    "chain": [("speed", 30, 0), ("speed", 10, 1), ("speed", 5, 2)],
    "hidden_particles": [("speed", 30, 0, "true")],
    "equal_infinite_over_finite": [("speed", 30, 1), ("speed", "infinite", 1)],
}


def campaign_replace(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    out = {}
    tags = {}
    commands = []
    for index, name in enumerate(REPLACE_CASES):
        tag = rig.tag("rp")
        tags[name] = tag
        commands.append(summon("minecraft:zombie", rig.x + 4 + index * 2, rig.y, rig.z - 24, tag))
    server.batch(commands)
    bot.pump(0.4)
    gives = []
    for name, steps in REPLACE_CASES.items():
        for step in steps:
            effect, duration, amp = step[:3]
            hide = step[3] if len(step) > 3 else "false"
            gives.append(f"effect give {sel(tags[name])} minecraft:{effect} {duration} {amp} {hide}")
    replies = server.batch(gives)
    for name in REPLACE_CASES:
        out[name] = {"steps": [list(s) for s in REPLACE_CASES[name]],
                     "at_once": plain(data_get(server, sel(tags[name]), "ActiveEffects"))}
    bot.pump(6.0)
    for name in REPLACE_CASES:
        out[name]["after_6s"] = plain(data_get(server, sel(tags[name]), "ActiveEffects"))
        print(f"  {name}: {out[name]['at_once']} -> {out[name]['after_6s']}")
    bot.pump(6.0)
    out["chain"]["after_12s"] = plain(data_get(server, sel(tags["chain"]), "ActiveEffects"))
    server.batch(["kill @e[tag=ovfx]"])
    return {"cases": out, "replies": replies}


def packets_with(captured, needle: bytes):
    return [(pid, payload) for pid, payload in captured if payload.startswith(needle)]


def campaign_packets(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    heal_bot(server, bot)
    bot.pump(0.5)
    bot.drain()
    eid = varint(bot.entity_id)
    out = {"entity_id": bot.entity_id, "steps": []}
    steps = [
        ("speed 30 1", f"effect give {BOT} minecraft:speed 30 1", EFFECT_ID["minecraft:speed"]),
        ("speed 30 1 again", f"effect give {BOT} minecraft:speed 30 1", EFFECT_ID["minecraft:speed"]),
        ("haste infinite 0 hidden", f"effect give {BOT} minecraft:haste infinite 0 true",
         EFFECT_ID["minecraft:haste"]),
        ("darkness 10 0", f"effect give {BOT} minecraft:darkness 10 0",
         EFFECT_ID["minecraft:darkness"]),
        ("clear speed", f"effect clear {BOT} minecraft:speed", EFFECT_ID["minecraft:speed"]),
        ("strength 30 2", f"effect give {BOT} minecraft:strength 30 2",
         EFFECT_ID["minecraft:strength"]),
        ("clear all", f"effect clear {BOT}", None),
        ("jump 2s", f"effect give {BOT} minecraft:jump_boost 2 0", EFFECT_ID["minecraft:jump_boost"]),
    ]
    for label, command, effect_id in steps:
        server.batch([command])
        bot.pump(3.0 if label == "jump 2s" else 0.8)
        captured = bot.drain()
        mine = []
        for pid, payload in captured:
            if not payload.startswith(eid):
                continue
            if pid in (0x2B, 0x2C, 0x2D, 0x42, 0x68, 0x23):
                continue
            mine.append({"id": pid, "hex": payload.hex()})
        # Identified by payload: entity id, then the effect id we just named.
        matched = []
        if effect_id is not None:
            key = eid + varint(effect_id)
            matched = [{"id": pid, "hex": payload.hex()} for pid, payload in captured
                       if payload.startswith(key)]
        out["steps"].append({"label": label, "command": command, "effect_id": effect_id,
                             "packets_about_bot": mine, "starting_with_effect": matched})
        print(f"  {label}: " + ", ".join(f"0x{p['id']:02X}({len(p['hex']) // 2})" for p in mine))
    heal_bot(server, bot)
    return out


METADATA_TYPES = {0: "byte", 1: "varint", 2: "varlong", 3: "float", 8: "boolean"}


def parse_metadata(payload: bytes):
    eid, i = read_varint(payload, 0)
    fields = []
    while i < len(payload):
        index = payload[i]
        i += 1
        if index == 0xFF:
            break
        kind, i = read_varint(payload, i)
        if kind == 0:
            value = struct.unpack_from(">b", payload, i)[0]
            i += 1
        elif kind in (1, 2):
            value, i = read_varint(payload, i)
            if value >= 1 << 31:
                value -= 1 << 32
        elif kind == 3:
            value = struct.unpack_from(">f", payload, i)[0]
            i += 4
        elif kind == 8:
            value = bool(payload[i])
            i += 1
        else:
            fields.append({"index": index, "type": kind, "raw": payload[i:].hex()})
            break
        fields.append({"index": index, "type": kind, "value": value})
    return eid, fields


def campaign_metadata(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    # A cow, at midnight, with a thousand health. The first version used a
    # zombie at noon: it burned (index 9 moved on every read), refused
    # regeneration and poison as undead, and died after the poison entry, so
    # every colour after it came back empty.
    server.batch(["time set midnight"])
    zombie_type = ENTITY_ID["minecraft:cow"]
    hardy = 'Attributes:[{Name:"minecraft:generic.max_health",Base:1000d}],Health:1000f'

    def spawn(tag: str, extra: str = "") -> tuple[int | None, list]:
        bot.drain()
        server.batch([summon("minecraft:cow", rig.x + 3, rig.y, rig.z + 3, tag,
                             hardy + ("," + extra if extra else ""))])
        bot.pump(0.8)
        eid = None
        metadata = []
        for pid, payload in bot.drain():
            if pid == CB_SPAWN_ENTITY and eid is None:
                entity, i = read_varint(payload, 0)
                kind, _ = read_varint(payload, i + 16)
                if kind == zombie_type:
                    eid = entity
            elif pid == CB_SET_METADATA and eid is not None:
                owner, fields = parse_metadata(payload)
                if owner == eid:
                    metadata.append(fields)
        return eid, metadata

    out = {"single": {}, "spawned_with": {}}
    tag = rig.tag("md")
    eid, baseline = spawn(tag)
    out["baseline"] = baseline
    for effect in EFFECTS:
        if effect in ("minecraft:instant_damage", "minecraft:instant_health"):
            continue
        bot.drain()
        server.batch([f"effect give {sel(tag)} {effect} 30 0"])
        bot.pump(0.5)
        given = [parse_metadata(p)[1] for pid, p in bot.drain()
                 if pid == CB_SET_METADATA and parse_metadata(p)[0] == eid]
        server.batch([f"effect clear {sel(tag)}"])
        bot.pump(0.5)
        cleared = [parse_metadata(p)[1] for pid, p in bot.drain()
                   if pid == CB_SET_METADATA and parse_metadata(p)[0] == eid]
        out["single"][effect] = {"given": given, "cleared": cleared}
        print(f"  {effect}: {given}")
    combos = {"speed0+strength0": ["minecraft:speed 30 0", "minecraft:strength 30 0"],
              "speed0+haste2": ["minecraft:speed 30 0", "minecraft:haste 30 2"],
              "speed3": ["minecraft:speed 30 3"],
              "speed0 hidden": ["minecraft:speed 30 0 true"],
              "speed0 hidden+strength0": ["minecraft:speed 30 0 true", "minecraft:strength 30 0"]}
    out["combos"] = {}
    for label, gives in combos.items():
        bot.drain()
        server.batch([f"effect give {sel(tag)} {g}" for g in gives])
        bot.pump(0.5)
        out["combos"][label] = [parse_metadata(p)[1] for pid, p in bot.drain()
                                if pid == CB_SET_METADATA and parse_metadata(p)[0] == eid]
        server.batch([f"effect clear {sel(tag)}"])
        bot.pump(0.4)
        print(f"  {label}: {out['combos'][label]}")
    server.batch(["kill @e[tag=ovfx]"])
    bot.pump(0.4)
    for label, nbt in (("ambient", "ActiveEffects:[{Id:1,Amplifier:0b,Duration:600,Ambient:1b,"
                                   "ShowParticles:1b}]"),
                       ("not_ambient", "ActiveEffects:[{Id:1,Amplifier:0b,Duration:600,Ambient:0b,"
                                       "ShowParticles:1b}]"),
                       ("no_particles", "ActiveEffects:[{Id:1,Amplifier:0b,Duration:600,"
                                        "Ambient:0b,ShowParticles:0b}]")):
        spawn_tag = rig.tag("ms")
        _, fields = spawn(spawn_tag, nbt)
        bot.pump(0.3)
        out["spawned_with"][label] = fields + [parse_metadata(p)[1] for pid, p in bot.drain()
                                               if pid == CB_SET_METADATA]
        server.batch(["kill @e[tag=ovfx]"])
        bot.pump(0.4)
        print(f"  spawned {label}: {out['spawned_with'][label]}")
    return out


def read_typed_nbt(data: bytes):
    """A typed NBT reader: every value comes back as (type name, value)."""
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    pos = 0
    names = {1: "byte", 2: "short", 3: "int", 4: "long", 5: "float", 6: "double",
             7: "byte_array", 8: "string", 9: "list", 10: "compound", 11: "int_array",
             12: "long_array"}

    def take(n: int) -> bytes:
        nonlocal pos
        out = data[pos:pos + n]
        pos += n
        return out

    def string() -> str:
        (length,) = struct.unpack(">H", take(2))
        return take(length).decode("utf-8", "replace")

    def payload(kind: int):
        if kind == 1:
            return struct.unpack(">b", take(1))[0]
        if kind == 2:
            return struct.unpack(">h", take(2))[0]
        if kind == 3:
            return struct.unpack(">i", take(4))[0]
        if kind == 4:
            return struct.unpack(">q", take(8))[0]
        if kind == 5:
            return struct.unpack(">f", take(4))[0]
        if kind == 6:
            return struct.unpack(">d", take(8))[0]
        if kind == 7:
            (n,) = struct.unpack(">i", take(4))
            return list(take(n))
        if kind == 8:
            return string()
        if kind == 9:
            element = take(1)[0]
            (n,) = struct.unpack(">i", take(4))
            return {"element": names.get(element, element),
                    "items": [payload(element) for _ in range(n)]}
        if kind == 10:
            out = {}
            while True:
                child = take(1)[0]
                if child == 0:
                    return out
                name = string()
                out[name] = [names[child], payload(child)]
        if kind == 11:
            (n,) = struct.unpack(">i", take(4))
            return [struct.unpack(">i", take(4))[0] for _ in range(n)]
        if kind == 12:
            (n,) = struct.unpack(">i", take(4))
            return [struct.unpack(">q", take(8))[0] for _ in range(n)]
        raise ValueError(kind)

    root = take(1)[0]
    string()
    return payload(root)


def campaign_nbt_file(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    heal_bot(server, bot)
    server.batch([f"effect give {BOT} minecraft:speed 300 0",
                  f"effect give {BOT} minecraft:speed 60 2",
                  f"effect give {BOT} minecraft:darkness 30 0",
                  f"effect give {BOT} minecraft:regeneration infinite 0 true",
                  f"effect give {BOT} minecraft:luck 120 4"])
    bot.pump(1.0)
    listed = plain(data_get(server, BOT, "ActiveEffects"))
    server.batch(["save-all flush"])
    bot.pump(2.0)
    path = RUN / "world" / "playerdata" / f"{offline_uuid(BOT)}.dat"
    if not path.exists():
        return {"error": f"{path} not written", "console": listed}
    root = read_typed_nbt(path.read_bytes())
    effects = root.get("ActiveEffects")
    attributes = root.get("Attributes")
    heal_bot(server, bot)
    print(f"  ActiveEffects: {json.dumps(effects)[:400]}")
    return {"file": path.name, "active_effects": effects, "attributes": attributes,
            "console": listed, "top_level_keys": sorted(root)}


def campaign_saturation(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    server.batch([f"gamemode survival {BOT}", "gamerule naturalRegeneration false"])
    out = {"saturation": [], "hunger": []}
    for amp, seconds in ((0, 1), (1, 1), (3, 1), (0, 3), (9, 1)):
        ms.drain_food(server, bot, 6)
        before = ms.read_player(server, BOT)
        server.batch([f"effect give {BOT} minecraft:saturation {seconds} {amp} true"])
        bot.pump(seconds + 0.6)
        after = ms.read_player(server, BOT)
        server.batch([f"effect clear {BOT}"])
        row = {"amplifier": amp, "seconds": seconds, "before": before, "after": after}
        out["saturation"].append(row)
        print(f"  saturation {amp} {seconds}s: food {before['food']}->{after['food']}, "
              f"sat {before['saturation']}->{after['saturation']}")
    for amp, seconds in ((0, 5), (1, 5), (4, 5), (9, 2)):
        ms.fill_food(server, bot)
        bot.pump(0.5)
        before = ms.read_player(server, BOT)
        server.batch([f"effect give {BOT} minecraft:hunger {seconds} {amp} true"])
        bot.pump(seconds + 1.0)
        after = ms.read_player(server, BOT)
        server.batch([f"effect clear {BOT}"])
        spent = None
        if None not in (before["exhaustion"], after["exhaustion"], before["saturation"],
                        after["saturation"]):
            spent = (after["exhaustion"] - before["exhaustion"]
                     + 4.0 * (before["saturation"] - after["saturation"])
                     + 4.0 * (before["food"] - after["food"]))
        row = {"amplifier": amp, "seconds": seconds, "before": before, "after": after,
               "exhaustion_spent": spent}
        out["hunger"].append(row)
        print(f"  hunger {amp} {seconds}s: exhaustion spent {spent}")
    server.batch(["gamerule naturalRegeneration true"])
    return out


FOODS = [("minecraft:golden_apple", 1), ("minecraft:enchanted_golden_apple", 1),
         ("minecraft:pufferfish", 1), ("minecraft:spider_eye", 1),
         ("minecraft:suspicious_stew", 1), ("minecraft:rotten_flesh", 24),
         ("minecraft:chicken", 24), ("minecraft:poisonous_potato", 24)]


def eat(server: Server, bot: ms.Bot, item: str, sequence: int, wait: float = 2.4) -> None:
    server.batch([f"clear {BOT}", f"give {BOT} {item} 1"])
    bot.pump(0.3)
    bot.hold(0)
    bot.use_item(sequence)
    bot.pump(wait)


def campaign_food(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    server.batch([f"gamemode survival {BOT}", "gamerule naturalRegeneration false"])
    out = {}
    sequence = 100
    for item, times in FOODS:
        rows = []
        for _ in range(times):
            if bot.lost:
                break
            heal_bot(server, bot)
            player = ms.read_player(server, BOT)
            if player["food"] is None or player["food"] > 12:
                ms.drain_food(server, bot, 10)
            server.batch([f"effect clear {BOT}"])
            bot.pump(0.2)
            eat(server, bot, item, sequence)
            sequence += 1
            effects = plain(data_get(server, BOT, "ActiveEffects")) or []
            food_now = ms.number(server.batch([f"data get entity {BOT} foodLevel"]))
            rows.append({"effects": effects, "food_after": food_now})
        out[item] = rows
        summary = {}
        for row in rows:
            for e in row["effects"]:
                key = (e.get("Id"), e.get("Amplifier"))
                summary[str(key)] = summary.get(str(key), 0) + 1
        print(f"  {item} x{len(rows)}: {summary}; first {rows[0]['effects'] if rows else None}")
    # Honey removes poison and nothing else; milk removes everything.
    for item, label in (("minecraft:honey_bottle", "honey"), ("minecraft:milk_bucket", "milk")):
        heal_bot(server, bot)
        ms.drain_food(server, bot, 10)
        server.batch([f"effect give {BOT} minecraft:poison 60 0",
                      f"effect give {BOT} minecraft:speed 60 0",
                      f"effect give {BOT} minecraft:regeneration 60 0"])
        eat(server, bot, item, sequence, wait=2.8)
        sequence += 1
        out[label] = plain(data_get(server, BOT, "ActiveEffects")) or []
        print(f"  {label}: left {out[label]}")
    server.batch([f"effect clear {BOT}", f"clear {BOT}", "gamerule naturalRegeneration true"])
    heal_bot(server, bot)
    return out


def campaign_death(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    heal_bot(server, bot)
    server.batch(["gamerule doImmediateRespawn false",
                  f"effect give {BOT} minecraft:speed infinite 1",
                  f"effect give {BOT} minecraft:strength 60 0"])
    bot.pump(0.5)
    before = plain(data_get(server, BOT, "ActiveEffects"))
    bot.drain()
    server.batch([f"kill {BOT}"])
    bot.pump(1.0)
    dying = [{"id": pid, "hex": p.hex()} for pid, p in bot.drain()
             if p.startswith(varint(bot.entity_id)) and pid not in (0x2B, 0x2C, 0x2D, 0x42)]
    bot.respawn()
    bot.pump(2.0)
    after = plain(data_get(server, BOT, "ActiveEffects"))
    respawn_packets = [{"id": pid, "len": len(p)} for pid, p in bot.drain()
                       if pid in (0x6C, 0x3F, 0x6A, 0x41)]
    print(f"  before {before}; after respawn {after}")
    return {"before": before, "after_respawn": after, "at_death": dying,
            "after_respawn_packets": respawn_packets}


BREAK_CASES = [
    # block, tool, effects [(name, amp)], label
    ("minecraft:stone", None, [], "stone hand"),
    ("minecraft:stone", None, [("haste", 0)], "stone hand haste0"),
    ("minecraft:stone", None, [("haste", 1)], "stone hand haste1"),
    ("minecraft:stone", None, [("haste", 2)], "stone hand haste2"),
    ("minecraft:stone", None, [("haste", 4)], "stone hand haste4"),
    ("minecraft:stone", None, [("conduit_power", 0)], "stone hand conduit0"),
    ("minecraft:stone", None, [("conduit_power", 1), ("haste", 0)], "stone hand conduit1+haste0"),
    ("minecraft:stone", "minecraft:wooden_pickaxe", [("haste", 1)], "stone wood haste1"),
    ("minecraft:obsidian", "minecraft:netherite_pickaxe", [("haste", 1)], "obsidian netherite haste1"),
    ("minecraft:dirt", None, [], "dirt hand"),
    ("minecraft:dirt", None, [("mining_fatigue", 0)], "dirt hand fatigue0"),
    ("minecraft:dirt", None, [("mining_fatigue", 1)], "dirt hand fatigue1"),
    ("minecraft:dirt", "minecraft:netherite_shovel", [("mining_fatigue", 2)], "dirt netherite fatigue2"),
    ("minecraft:dirt", 'minecraft:netherite_shovel{Enchantments:[{id:"minecraft:efficiency",lvl:5s}]}',
     [("mining_fatigue", 3)], "dirt netherite eff5 fatigue3"),
    ("minecraft:dirt", None, [("haste", 1), ("mining_fatigue", 0)], "dirt hand haste1+fatigue0"),
    ("minecraft:oak_planks", None, [("haste", 2)], "planks hand haste2"),
]


def campaign_breaking(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    miner = Miner(PORT, MINER)
    miner.pump(timeout=3.0)
    x, y, z = int(rig.x) - 20, -60, int(rig.z) - 20
    server.batch([f"gamemode survival {MINER}", f"tp {MINER} {x + 0.5} {y}.0 {z + 0.5}",
                  "gamerule doTileDrops false"])
    time.sleep(1.0)
    miner.pump(timeout=1.0)
    block = (x + 2, y, z)
    out = []
    for block_name, tool, effects, label in BREAK_CASES:
        results = []
        for attempt in range(2):
            server.batch([f"effect clear {MINER}",
                          f"effect give {MINER} minecraft:saturation 1000 4 true",
                          f"effect give {MINER} minecraft:resistance 1000 4 true",
                          *[f"effect give {MINER} minecraft:{name} 1000 {amp} true"
                            for name, amp in effects],
                          f"clear {MINER}", *([f"give {MINER} {tool}"] if tool else []),
                          f"setblock {block[0]} {y - 1} {z} minecraft:grass_block replace",
                          f"setblock {block[0] + 1} {y} {z} minecraft:stone replace",
                          f"setblock {block[0]} {y} {z + 1} minecraft:stone replace",
                          f"setblock {block[0]} {y} {z - 1} minecraft:stone replace",
                          f"setblock {block[0]} {y} {z} {block_name} replace"])
            time.sleep(0.55)
            miner.pump(timeout=0.35)
            miner.stand(x + 0.5, float(y), z + 0.5)
            time.sleep(0.1)
            ticks = miner.mine_timed(*block, timeout=120.0)
            if ticks is None:
                server.batch([f"setblock {block[0]} {y} {z} minecraft:air replace"])
                time.sleep(0.35)
                miner.pump(timeout=0.25)
            results.append(ticks)
        out.append({"label": label, "block": block_name, "tool": tool,
                    "effects": effects, "ticks": results})
        print(f"  {label}: {results}")
        ms.nap(0.1)
    server.batch([f"effect clear {MINER}"])
    try:
        miner.s.close()
    except OSError:
        pass
    return {"cases": out}


def campaign_infinite(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    tags = [rig.tag("if") for _ in range(4)]
    server.batch([summon("minecraft:cow", rig.x - 14 - 2 * i, rig.y, rig.z - 24, t,
                         'Attributes:[{Name:"minecraft:generic.max_health",Base:1000d}],'
                         'Health:500f') for i, t in enumerate(tags)])
    bot.pump(0.4)
    server.batch([f"effect give {sel(t)} minecraft:regeneration infinite 0 true" for t in tags])
    started = time.monotonic()
    bot.pump(20.0)
    server.batch([f"effect clear {sel(t)}" for t in tags])
    elapsed = time.monotonic() - started
    healed = [ms.number(server.batch([f"data get entity {sel(t)} Health"])) for t in tags]
    server.batch(["kill @e[tag=ovfx]"])
    out = {"elapsed_seconds": round(elapsed, 3), "healed": [h - 500.0 if h else None for h in healed]}
    print(f"  {out}")
    return out


def campaign_order(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    """In which order the modifiers of one operation are applied.

    Addition and multiplication of doubles are not associative, so the order
    is visible in the last bit. The amounts are chosen so that it is: base +
    0.1 + 0.07 and base + 0.07 + 0.1 differ by one ulp on a zombie's speed. The
    UUIDs are chosen so that their Java hash buckets (…a009 in bucket 9, …a002
    in bucket 2) run against the order they are inserted in — so the answer
    tells insertion order from hash order."""
    attr = "minecraft:generic.movement_speed"
    cases = {
        "add hi-then-lo": [("00000000-0000-0000-0000-00000000a009", 0.1, "add"),
                           ("00000000-0000-0000-0000-00000000a002", 0.07, "add")],
        "add lo-then-hi": [("00000000-0000-0000-0000-00000000a002", 0.07, "add"),
                           ("00000000-0000-0000-0000-00000000a009", 0.1, "add")],
        "mul hi-then-lo": [("00000000-0000-0000-0000-00000000b009", 0.1, "multiply"),
                           ("00000000-0000-0000-0000-00000000b002", 0.07, "multiply")],
        "mul lo-then-hi": [("00000000-0000-0000-0000-00000000b002", 0.07, "multiply"),
                           ("00000000-0000-0000-0000-00000000b009", 0.1, "multiply")],
        # Same amounts, same bucket order, opposite amounts per bucket.
        "add hi07-then-lo10": [("00000000-0000-0000-0000-00000000a009", 0.07, "add"),
                               ("00000000-0000-0000-0000-00000000a002", 0.1, "add")],
    }
    out = {}
    for label, steps in cases.items():
        tag = rig.tag("or")
        server.batch([summon("minecraft:zombie", rig.x - 3, rig.y, rig.z - 30, tag)])
        bot.pump(0.3)
        for uid, amount, operation in steps:
            server.batch([f"attribute {sel(tag)} {attr} modifier add {uid} ovorder {amount} {operation}"])
        total = number_of(server.batch([f"attribute {sel(tag)} {attr} get"]))
        out[label] = {"steps": steps, "total": total}
        print(f"  {label}: {total!r}")
        server.batch(["kill @e[tag=ovfx]"])
    return out


def campaign_player_meta(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    heal_bot(server, bot)
    bot.pump(0.4)
    bot.drain()
    eid = bot.entity_id
    out = {}
    for label, command in (("absorption 1", f"effect give {BOT} minecraft:absorption 30 1 true"),
                           ("invisibility", f"effect give {BOT} minecraft:invisibility 30 0 true"),
                           ("glowing", f"effect give {BOT} minecraft:glowing 30 0 true"),
                           ("health_boost 1", f"effect give {BOT} minecraft:health_boost 30 1 true"),
                           ("clear", f"effect clear {BOT}")):
        server.batch([command])
        bot.pump(0.6)
        packets = []
        for pid, payload in bot.drain():
            if pid == CB_SET_METADATA and parse_metadata(payload)[0] == eid:
                packets.append({"metadata": parse_metadata(payload)[1]})
            elif pid in (0x57, 0x6A) and (pid == 0x57 or payload.startswith(varint(eid))):
                packets.append({"id": pid, "hex": payload.hex()})
        out[label] = packets
        print(f"  {label}: {packets}")
    heal_bot(server, bot)
    return out


def campaign_oxygen(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    x0, z0 = int(rig.x) + 12, int(rig.z) - 36
    y0 = int(rig.y)
    server.batch([f"fill {x0 - 1} {y0} {z0 - 1} {x0 + 7} {y0 + 3} {z0 + 1} minecraft:glass hollow",
                  f"fill {x0} {y0} {z0} {x0 + 6} {y0 + 2} {z0} minecraft:water"])
    bot.pump(0.5)
    tags = {}
    for index, effect in enumerate((None, "minecraft:water_breathing", "minecraft:conduit_power",
                                    "minecraft:dolphins_grace")):
        tag = rig.tag("ox")
        tags[str(effect)] = tag
        server.batch([summon("minecraft:cow", x0 + 0.5 + index * 2, y0, z0 + 0.5, tag,
                             "NoGravity:1b")])
        if effect:
            server.batch([f"effect give {sel(tag)} {effect} 60 0 true"])
    bot.pump(5.0)
    out = {}
    for effect, tag in tags.items():
        out[effect] = ms.number(server.batch([f"data get entity {sel(tag)} Air"]))
    server.batch(["kill @e[tag=ovfx]",
                  f"fill {x0 - 1} {y0} {z0 - 1} {x0 + 7} {y0 + 3} {z0 + 1} minecraft:air"])
    print(f"  {out}")
    return out


def campaign_fall(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    out = []
    subjects = []
    for index, (effect, amp) in enumerate(((None, 0), ("minecraft:jump_boost", 0),
                                            ("minecraft:jump_boost", 1),
                                            ("minecraft:jump_boost", 4),
                                            ("minecraft:slow_falling", 0))):
        for column, height in enumerate((6, 10, 20)):
            tag = rig.tag("fl")
            x = rig.x - 30 - index * 3
            z = rig.z + column * 3
            effects = ""
            if effect:
                effects = (f',ActiveEffects:[{{Id:{EFFECT_ID[effect]},Amplifier:{amp}b,'
                           'Duration:2000,ShowParticles:0b}]')
            server.batch([summon("minecraft:cow", x, rig.y + height, z, tag,
                                 'Attributes:[{Name:"minecraft:generic.max_health",Base:200d}],'
                                 f'Health:200f{effects}').replace("NoAI:1b,", "")])
            subjects.append((tag, effect, amp, height))
    bot.pump(6.0)
    for tag, effect, amp, height in subjects:
        health = ms.number(server.batch([f"data get entity {sel(tag)} Health"]))
        out.append({"effect": effect, "amplifier": amp, "height": height,
                    "damage": None if health is None else round(200.0 - health, 4)})
        print(f"  {effect} {amp} from {height}: {out[-1]['damage']}")
    server.batch(["kill @e[tag=ovfx]"])
    return {"results": out}


GAMETIME = re.compile(r"The time is (\d+)")


def gametime(server: Server) -> int | None:
    """The server's own clock. A wall-clock wait measures the machine, and on a
    machine with three JVMs and a compiler it measured 11 ticks in six seconds
    once — which is how the first `replace` run never saw a promotion."""
    for line in server.batch(["time query gametime"]):
        match = GAMETIME.search(line)
        if match:
            return int(match.group(1))
    return None


def wait_ticks(server: Server, bot: ms.Bot, ticks: int, limit: float = 120.0) -> int | None:
    start = gametime(server)
    if start is None:
        return None
    deadline = time.monotonic() + limit
    while time.monotonic() < deadline:
        bot.pump(0.1)
        now = gametime(server)
        if now is not None and now - start >= ticks:
            return now - start
    return None


def campaign_promote(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    """The hidden effect coming back, timed on the server's clock."""
    cases = {
        "stronger_shorter": [("speed", 5, 2), ("speed", 30, 0)],
        "chain": [("speed", 30, 0), ("speed", 10, 1), ("speed", 5, 2)],
        "infinite_under": [("speed", "infinite", 0), ("speed", 5, 2)],
    }
    tags = {}
    for index, name in enumerate(cases):
        tags[name] = rig.tag("pr")
        server.batch([summon("minecraft:cow", rig.x + 4 + index * 2, rig.y, rig.z - 40, tags[name])])
    bot.pump(0.3)
    gives = []
    for name, steps in cases.items():
        # Stronger effect given second where the case is named that way; the
        # order inside the list is the order of the commands.
        for effect, duration, amp in (steps if name != "stronger_shorter" else steps[::-1]):
            gives.append(f"effect give {sel(tags[name])} minecraft:{effect} {duration} {amp}")
    start = gametime(server)
    server.batch(gives)
    out = {"start": start, "cases": {}}
    for name in cases:
        out["cases"][name] = {"t0": plain(data_get(server, sel(tags[name]), "ActiveEffects"))}
    for label, ticks in (("t110", 110), ("t210", 210)):
        elapsed = wait_ticks(server, bot, ticks - (gametime(server) - start))
        now = gametime(server)
        for name in cases:
            out["cases"][name][label] = {
                "gametime_since_give": None if now is None or start is None else now - start,
                "effects": plain(data_get(server, sel(tags[name]), "ActiveEffects"))}
        print(f"  {label}: " + "; ".join(f"{n}: {out['cases'][n][label]}" for n in cases))
    server.batch(["kill @e[tag=ovfx]"])
    return out


def campaign_window(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    """Absorption against a second hit, with the gap held on the server clock."""
    out = {}
    for label, absorption, gap in (("plain gap30", None, 30), ("plain gap5", None, 5),
                                   ("absorb1 gap30", 1, 30), ("absorb1 gap5", 1, 5)):
        tag = rig.tag("wi")
        server.batch([summon("minecraft:cow", rig.x - 8, rig.y, rig.z - 40, tag,
                             'Attributes:[{Name:"minecraft:generic.max_health",Base:100d}],'
                             'Health:50f')])
        bot.pump(0.2)
        if absorption is not None:
            server.batch([f"effect give {sel(tag)} minecraft:absorption 60 {absorption}"])
            wait_ticks(server, bot, 3)
        server.batch([f"damage {sel(tag)} 3 minecraft:generic"])
        waited = wait_ticks(server, bot, gap)
        server.batch([f"damage {sel(tag)} 30 minecraft:generic"])
        wait_ticks(server, bot, 2)
        out[label] = {"gap_ticks": waited,
                      "health": ms.number(server.batch([f"data get entity {sel(tag)} Health"])),
                      "absorption": ms.number(server.batch(
                          [f"data get entity {sel(tag)} AbsorptionAmount"]))}
        print(f"  {label}: {out[label]}")
        server.batch(["kill @e[tag=ovfx]"])
    return out


def campaign_instant_night(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    """The undead half of `instant`, at midnight: at noon a zombie burns, and
    the first run's zombies came back one point off at four amplifiers."""
    server.batch(["time set midnight"])
    subjects = []
    commands = []
    for effect in ("minecraft:instant_health", "minecraft:instant_damage"):
        for amp in range(6):
            tag = rig.tag("iz")
            commands.append(summon("minecraft:zombie", rig.x - 6 - amp * 2, rig.y,
                                   rig.z + 40 + (4 if "damage" in effect else 0), tag,
                                   'Attributes:[{Name:"minecraft:generic.max_health",'
                                   'Base:1000d}],Health:500f'))
            subjects.append((tag, effect, amp))
    server.batch(commands)
    wait_ticks(server, bot, 10)
    server.batch([f"effect give {sel(tag)} {effect} 1 {amp}" for tag, effect, amp in subjects])
    wait_ticks(server, bot, 10)
    out = []
    for tag, effect, amp in subjects:
        health = ms.number(server.batch([f"data get entity {sel(tag)} Health"]))
        out.append({"effect": effect, "amplifier": amp,
                    "delta": None if health is None else round(health - 500.0, 4)})
        print(f"  zombie {effect} {amp}: {out[-1]['delta']}")
    server.batch(["kill @e[tag=ovfx]", "time set noon"])
    return {"results": out}


def campaign_boost_refresh(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    """Re-applying Health Boost to a cow that is full on it: does the health
    survive, or is it clamped to the unboosted maximum in between?"""
    out = {}
    for label, second in (("same amp longer", "minecraft:health_boost 120 1"),
                          ("higher amp", "minecraft:health_boost 120 2"),
                          ("same amp shorter", "minecraft:health_boost 10 1")):
        tag = rig.tag("br")
        server.batch([summon("minecraft:cow", rig.x - 12, rig.y, rig.z - 44, tag)])
        wait_ticks(server, bot, 2)
        server.batch([f"effect give {sel(tag)} minecraft:health_boost 60 1"])
        wait_ticks(server, bot, 2)
        server.batch([f"effect give {sel(tag)} minecraft:instant_health 1 3"])
        wait_ticks(server, bot, 2)
        full = ms.number(server.batch([f"data get entity {sel(tag)} Health"]))
        server.batch([f"effect give {sel(tag)} {second}"])
        wait_ticks(server, bot, 2)
        after = ms.number(server.batch([f"data get entity {sel(tag)} Health"]))
        maximum = number_of(server.batch(
            [f"attribute {sel(tag)} minecraft:generic.max_health get"]))
        out[label] = {"full": full, "after_refresh": after, "max_after": maximum}
        print(f"  {label}: {out[label]}")
        server.batch(["kill @e[tag=ovfx]"])
    return out


def campaign_saturation2(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    """Saturation over several seconds, to pin how often it feeds."""
    server.batch([f"gamemode survival {BOT}", "gamerule naturalRegeneration false"])
    out = []
    for amp, seconds in ((0, 2), (0, 5), (1, 3), (2, 2)):
        ms.drain_food(server, bot, 2)
        before = ms.read_player(server, BOT)
        start = gametime(server)
        server.batch([f"effect give {BOT} minecraft:saturation {seconds} {amp} true"])
        wait_ticks(server, bot, seconds * 20 + 10)
        after = ms.read_player(server, BOT)
        server.batch([f"effect clear {BOT}"])
        out.append({"amplifier": amp, "seconds": seconds, "before": before, "after": after,
                    "ticks": None if start is None else gametime(server) - start})
        print(f"  saturation {amp} {seconds}s: food {before['food']}->{after['food']}, "
              f"sat {before['saturation']}->{after['saturation']}")
    server.batch(["gamerule naturalRegeneration true"])
    return {"results": out}


def campaign_flags(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    """What a second give does to the particle and icon bits when it changes
    nothing else — equal amplifier, shorter duration."""
    cases = {"visible then hidden-shorter": ["minecraft:speed 30 0", "minecraft:speed 10 0 true"],
             "hidden then visible-shorter": ["minecraft:speed 30 0 true", "minecraft:speed 10 0"],
             "visible then hidden-weaker-longer": ["minecraft:speed 10 1",
                                                   "minecraft:speed 30 0 true"]}
    out = {}
    for label, gives in cases.items():
        tag = rig.tag("fg")
        server.batch([summon("minecraft:cow", rig.x + 14, rig.y, rig.z - 44, tag)])
        bot.pump(0.2)
        replies = server.batch([f"effect give {sel(tag)} {g}" for g in gives])
        out[label] = {"effects": plain(data_get(server, sel(tag), "ActiveEffects")),
                      "replies": replies}
        print(f"  {label}: {out[label]}")
        server.batch(["kill @e[tag=ovfx]"])
    return out


def campaign_food_odds(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    """More eats of the three chancy foods, to narrow their intervals."""
    global FOODS
    saved = FOODS
    FOODS = [("minecraft:chicken", 60), ("minecraft:poisonous_potato", 60),
             ("minecraft:rotten_flesh", 36)]
    try:
        return campaign_food(server, bot, rig)
    finally:
        FOODS = saved


def campaign_weights(server: Server, bot: ms.Bot, rig: Rig) -> dict:
    """One effect alone at amplifiers 2, 4 and 6: the swirl colour of a single
    effect is its own colour only if the mixing arithmetic is exact at that
    weight. Two float orderings fit every mixture measured before this, and
    they disagree here."""
    server.batch(["time set midnight"])
    cow_type = ENTITY_ID["minecraft:cow"]
    tag = rig.tag("wt")
    bot.drain()
    server.batch([summon("minecraft:cow", rig.x + 3, rig.y, rig.z + 3, tag,
                         'Attributes:[{Name:"minecraft:generic.max_health",Base:1000d}],'
                         'Health:1000f')])
    bot.pump(0.8)
    eid = None
    for pid, payload in bot.drain():
        if pid == CB_SPAWN_ENTITY and eid is None:
            entity, i = read_varint(payload, 0)
            kind, _ = read_varint(payload, i + 16)
            if kind == cow_type:
                eid = entity
    out = {}
    for effect in ("minecraft:speed", "minecraft:nausea", "minecraft:invisibility",
                   "minecraft:night_vision", "minecraft:poison", "minecraft:bad_omen",
                   "minecraft:slow_falling", "minecraft:levitation", "minecraft:haste"):
        for amp in (2, 4, 6):
            bot.drain()
            server.batch([f"effect give {sel(tag)} {effect} 30 {amp}"])
            bot.pump(0.5)
            colours = [f["value"] for pid, p in bot.drain()
                       if pid == CB_SET_METADATA and parse_metadata(p)[0] == eid
                       for f in parse_metadata(p)[1] if f.get("index") == 10]
            server.batch([f"effect clear {sel(tag)}"])
            bot.pump(0.4)
            out[f"{effect} {amp}"] = colours
            print(f"  {effect} amp {amp}: {[hex(c) for c in colours]}")
    server.batch(["kill @e[tag=ovfx]", "time set noon"])
    return out


ALL = {
    "player_bases": campaign_player_bases,
    "weights": campaign_weights,
    "flags": campaign_flags, "food_odds": campaign_food_odds,
    "boost_refresh": campaign_boost_refresh, "saturation2": campaign_saturation2,
    "promote": campaign_promote, "window": campaign_window,
    "instant_night": campaign_instant_night,
    "order": campaign_order, "player_meta": campaign_player_meta, "oxygen": campaign_oxygen,
    "fall": campaign_fall,
    "bounds": campaign_bounds, "ops": campaign_ops, "modifiers": campaign_modifiers,
    "immunity": campaign_immunity, "periodic": campaign_periodic, "instant": campaign_instant,
    "resistance": campaign_resistance, "absorption": campaign_absorption,
    "health_boost": campaign_health_boost, "replace": campaign_replace,
    "packets": campaign_packets, "metadata": campaign_metadata, "nbt_file": campaign_nbt_file,
    "saturation": campaign_saturation, "food": campaign_food, "death": campaign_death,
    "infinite": campaign_infinite, "breaking": campaign_breaking,
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", default=",".join(ALL))
    parser.add_argument("out", nargs="?", default=str(NORMALIZED / "effects.json"))
    args = parser.parse_args()
    wanted = [name for name in args.only.split(",") if name]
    for name in wanted:
        if name not in ALL:
            print(f"unknown campaign {name!r}; known: {', '.join(ALL)}")
            return 2
    out_path = Path(args.out)
    document: dict = {}
    if out_path.exists():
        try:
            document = json.loads(out_path.read_text())
        except ValueError:
            document = {}
    document["$comment"] = ("Mesure contre un vrai serveur 1.20.1. Voir "
                            "docs/provenance/effets.md et scripts/measure_effects.py.")

    RUN.mkdir(parents=True, exist_ok=True)
    server = Server(RUN, port=PORT)
    try:
        server.batch(["gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                      "gamerule sendCommandFeedback true", "gamerule doFireTick false",
                      "gamerule randomTickSpeed 0", "gamerule doWeatherCycle false",
                      "difficulty normal", "time set noon"])
        bot = ms.Bot(PORT, name=BOT)
        ms._BOT = bot
        bot.pump(3.0)
        if bot.position is None:
            raise RuntimeError("the bot was never told where it is")
        px, py, pz = bot.position
        server.batch([f"forceload add {int(px) - 80} {int(pz) - 80} {int(px) + 80} {int(pz) + 80}",
                      f"gamemode survival {BOT}"])
        bot.pump(2.0)
        print(f"bot at {px:.2f} {py:.2f} {pz:.2f}, entity {bot.entity_id}")
        rig = Rig(px, py, pz)
        for name in wanted:
            print(f"\n── {name} ──", flush=True)
            # A bot dropped in silence poisons every campaign after it — the
            # first full run lost it before `metadata` and three campaigns
            # recorded nothing. Checked and replaced before each one, with the
            # server's stated reason kept.
            bot.pump(0.2)
            if bot.lost:
                reasons = [p[:200].decode("utf-8", "replace") for pid, p in bot.captured
                           if pid == 0x1A]
                document.setdefault("$bot_lost", []).append({"before": name, "reason": reasons})
                print(f"  bot was lost ({reasons}); reconnecting", flush=True)
                try:
                    bot.socket.close()
                except OSError:
                    pass
                bot = ms.Bot(PORT, name=BOT)
                ms._BOT = bot
                bot.pump(2.0)
                server.batch([f"gamemode survival {BOT}"])
            started = time.monotonic()
            try:
                document[name] = ALL[name](server, bot, rig)
            except Exception as error:  # noqa: BLE001 — a campaign's failure is data
                document[name] = {"error": repr(error)}
                print(f"  FAILED: {error!r}")
                if bot.lost:
                    bot = ms.Bot(PORT, name=BOT)
                    ms._BOT = bot
                    bot.pump(2.0)
            bot.drain()
            print(f"  ({time.monotonic() - started:.0f} s)")
            out_path.parent.mkdir(parents=True, exist_ok=True)
            out_path.write_text(json.dumps(document, indent=1, sort_keys=True))
    finally:
        server.stop()
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
