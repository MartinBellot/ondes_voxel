#!/usr/bin/env python3
"""Ask a real 1.20.1 server how brewing and potions work.

Nothing about brewing is in the data generator's reports: there is no
`brewing` recipe type in 1.20.1, the mixes are Java code, and so are the brew
time, the fuel, the potions' effects and what a thrown potion does at a
distance. The wiki's « Brewing » article is the documentary source of the
recipe table this project implements; every campaign below checks it, or
measures what the wiki does not say, against the real server.

Campaigns
---------

  recipes     **Exhaustive.** Every (container, potion) bottle — 3 × 43 — is put
              in a brewing stand with every candidate ingredient — the 17 the
              wiki names plus 4 controls that are not ingredients — three
              bottles per stand, 903 stands, fuel 20. After 440 ticks on the
              server's clock every stand is read back. A bottle that changed is
              a recipe; a bottle that did not is the absence of one. Nothing is
              assumed about which pairs are interesting.

  timing      One stand watched tick by tick (`BrewTime`, `Fuel` and the
              server's `gametime` in one batch): the brew time, when the fuel is
              spent, what refuelling does, what removing the ingredient does,
              the `has_bottle_N` block state, and dragon's breath's remainder.

  faces       A hopper into each face of a stand, four different items, and
              one under a stand pulling: the sided-access table, read as with
              the furnace (`measure_containers.py furnace-faces`).

  window      The bot opens a stand and watches the Container Property packets
              while it brews: which property is the brew time, which the fuel.

  drink       The bot drinks each of the 43 potions. Effects are read off the
              Entity Effect packets (exact durations, no console timing);
              instant ones off Set Health. What is left in the hand after.

  drinktime   How many ticks a drink takes, on the server's clock, calibrated
              against a console `effect give` read the same way.

  splash      A splash potion broken at a measured distance from the bot, which
              reads its effect off Entity Effect: the intensity law, the radius,
              the rounding and the 20-tick floor; instant healing by distance.

  lingering   The area effect cloud a lingering potion leaves: its NBT at
              birth, its shrinking, what it applies to the bot and how often.

  arrow       Tipped and spectral arrows shot into the bot: the effect's
              duration against the potion's.

  stew        Suspicious stew with effects in its NBT, eaten by the bot.

Usage: python3 scripts/measure_brewing.py [--only a,b,...] [out.json]
Writes data/vanilla/1.20.1/normalized/brewing.json after every campaign.
Run through the shared lock: lockf /tmp/ov-vanilla.lock python3 scripts/measure_brewing.py
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import measure_survival as ms  # noqa: E402
from capture_entity_packets import read_varint, varint  # noqa: E402
from measure_effects import parse_metadata, parse_snbt, plain  # noqa: E402
from measure_entities import Server  # noqa: E402
from vanilla_miner import block_pos  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
GENERATED = ROOT / "data" / "vanilla" / "1.20.1" / "generated"
RUN = ROOT / "run" / "brewing-oracle"
PORT = 25641
BOT = "ovbrew"
ms.BOT = BOT
# Measured in docs/provenance/survie.md.
ms.CB["set_health"] = 0x57
Server.HEAP = "-Xmx1G"   # nine agents, one machine of 8 GB

CB_ENTITY_EFFECT = 0x6C       # confirmed by payload in effets.md
CB_SPAWN_ENTITY = 0x01
CB_SET_METADATA = 0x52
CB_OPEN_SCREEN = 0x30
SB_USE_ITEM_ON = 0x31

REGISTRIES = json.loads((GENERATED / "reports" / "registries.json").read_text())
POTIONS = [name for name, _ in sorted(REGISTRIES["minecraft:potion"]["entries"].items(),
                                      key=lambda kv: kv[1]["protocol_id"])]
EFFECT_NAME = {e["protocol_id"]: name
               for name, e in REGISTRIES["minecraft:mob_effect"]["entries"].items()}
CONTAINERS = ["minecraft:potion", "minecraft:splash_potion", "minecraft:lingering_potion"]
INGREDIENTS = [
    "minecraft:nether_wart", "minecraft:redstone", "minecraft:glowstone_dust",
    "minecraft:fermented_spider_eye", "minecraft:gunpowder", "minecraft:dragon_breath",
    "minecraft:sugar", "minecraft:rabbit_foot", "minecraft:glistering_melon_slice",
    "minecraft:spider_eye", "minecraft:pufferfish", "minecraft:magma_cream",
    "minecraft:golden_carrot", "minecraft:blaze_powder", "minecraft:ghast_tear",
    "minecraft:turtle_helmet", "minecraft:phantom_membrane",
]
# Not ingredients, by the wiki. A stand must not start on any of them.
CONTROLS = ["minecraft:carrot", "minecraft:cod", "minecraft:golden_apple",
            "minecraft:gold_nugget"]

BLOCK_DATA = re.compile(r"has the following block data: (.*)$")
ENTITY_DATA = re.compile(r"has the following entity data: (.*)$")
GAMETIME = re.compile(r"The time is (\d+)")
Y = -60   # the superflat's first free layer


# ── Helpers ─────────────────────────────────────────────────────────────────


def gametime_of(lines: list[str]) -> int | None:
    for line in lines:
        match = GAMETIME.search(line)
        if match:
            return int(match.group(1))
    return None


def gametime(server: Server) -> int | None:
    return gametime_of(server.batch(["time query gametime"]))


def wait_ticks(server: Server, bot: ms.Bot, ticks: int, limit: float = 180.0) -> int | None:
    """Wait on the server's own clock, not the wall's (effets.md, piège 3)."""
    start = gametime(server)
    if start is None:
        return None
    deadline = time.monotonic() + limit
    while time.monotonic() < deadline:
        bot.pump(0.25)
        now = gametime(server)
        if now is not None and now - start >= ticks:
            return now - start
    return None


def bottle(container: str, potion: str, slot: int) -> str:
    return (f'{{Slot:{slot}b,id:"{container}",Count:1b,tag:{{Potion:"{potion}"}}}}')


def stand_nbt(bottles: list[str], ingredient: str | None, count: int = 1,
              fuel: int = 20, powder: int = 0) -> str:
    items = list(bottles)
    if ingredient:
        items.append(f'{{Slot:3b,id:"{ingredient}",Count:{count}b}}')
    if powder:
        items.append(f'{{Slot:4b,id:"minecraft:blaze_powder",Count:{powder}b}}')
    return f"brewing_stand{{Items:[{','.join(items)}],Fuel:{fuel}b}}"


def read_blocks(server: Server, positions: list[tuple[int, int, int]]) -> list[dict | None]:
    """`data get block` for every position, in one round trip, parsed."""
    answers: list[dict | None] = []
    lines = server.batch([f"data get block {x} {y} {z}" for x, y, z in positions])
    for line in lines:
        match = BLOCK_DATA.search(line)
        if match:
            answers.append(plain(parse_snbt(match.group(1).strip())))
        elif "not a block entity" in line or "No block" in line:
            answers.append(None)
    return answers


def items_by_slot(data: dict | None) -> dict[int, dict]:
    if not data:
        return {}
    return {entry["Slot"]: entry for entry in data.get("Items", [])}


def describe(entry: dict | None) -> list | None:
    """(item, potion, count) of one slot, None when empty."""
    if not entry:
        return None
    return [entry["id"], (entry.get("tag") or {}).get("Potion"), entry["Count"]]


def entity_effects(bot: ms.Bot) -> list[dict]:
    """Every Entity Effect addressed to the bot since the last drain."""
    out = []
    for pid, payload in bot.drain():
        if pid != CB_ENTITY_EFFECT:
            continue
        entity, i = read_varint(payload, 0)
        if entity != bot.entity_id:
            continue
        effect, i = read_varint(payload, i)
        amplifier = payload[i]
        i += 1
        duration, i = read_varint(payload, i)
        if duration >= 1 << 31:
            duration -= 1 << 32
        flags = payload[i]
        out.append({"effect": EFFECT_NAME.get(effect, effect), "amplifier": amplifier,
                    "duration": duration, "flags": flags})
    return out


def reset_bot(server: Server, bot: ms.Bot, health: float | None = None) -> None:
    """Full health, no effects, empty hands — then, optionally, hurt to `health`."""
    server.batch([f"effect clear {BOT}", f"clear {BOT}",
                  f"effect give {BOT} minecraft:instant_health 1 5 true"])
    bot.pump(0.4)
    server.batch([f"effect clear {BOT}"])
    if health is not None and health < 20.0:
        # Past the invulnerability window of any earlier hit.
        bot.pump(0.7)
        server.batch([f"damage {BOT} {20.0 - health} minecraft:generic"])
    bot.pump(0.4)
    bot.drain()


# ── recipes ─────────────────────────────────────────────────────────────────


def campaign_recipes(server: Server, bot: ms.Bot, origin: tuple[int, int]) -> dict:
    x0, z0 = origin
    bottles = [(c, p) for c in CONTAINERS for p in POTIONS]
    stands = []   # (position, ingredient, [(container, potion)] * 3)
    columns = 31
    for ingredient in INGREDIENTS + CONTROLS:
        for first in range(0, len(bottles), 3):
            index = len(stands)
            pos = (x0 + (index % columns) * 2, Y, z0 + (index // columns) * 2)
            stands.append((pos, ingredient, bottles[first:first + 3]))
    rows = (len(stands) + columns - 1) // columns
    server.batch([f"forceload add {x0 - 2} {z0 - 2} {x0 + columns * 2 + 2} {z0 + rows * 2 + 2}"])
    server.batch([f"fill {x0 - 1} {Y} {z0 - 1} {x0 + columns * 2} {Y + 1} {z0 + rows * 2} air"])
    commands = []
    for pos, ingredient, group in stands:
        nbt = stand_nbt([bottle(c, p, s) for s, (c, p) in enumerate(group)], ingredient)
        commands.append(f"setblock {pos[0]} {pos[1]} {pos[2]} {nbt}")
    for start in range(0, len(commands), 150):
        server.batch(commands[start:start + 150])
    placed = gametime(server)
    print(f"  {len(stands)} stands, {len(bottles) * len(INGREDIENTS + CONTROLS)} bottles, "
          f"placed at gametime {placed}", flush=True)
    waited = wait_ticks(server, bot, 440)
    print(f"  waited {waited} ticks", flush=True)

    results = []
    for start in range(0, len(stands), 100):
        chunk = stands[start:start + 100]
        answers = read_blocks(server, [pos for pos, _, _ in chunk])
        for (pos, ingredient, group), data in zip(chunk, answers):
            slots = items_by_slot(data)
            for slot, (container, potion) in enumerate(group):
                out = describe(slots.get(slot))
                results.append({"container": container, "potion": potion,
                                "ingredient": ingredient,
                                "out": out[:2] if out else None,
                                "changed": out is None or out[0] != container or out[1] != potion})
            if data is not None and group:
                results[-1]["stand"] = {"ingredient_left": describe(slots.get(3)),
                                        "fuel": data.get("Fuel"),
                                        "brew_time": data.get("BrewTime")}
    changed = [r for r in results if r["changed"]]
    print(f"  {len(changed)} bottles changed out of {len(results)}")
    per_ingredient: dict[str, int] = {}
    for r in changed:
        per_ingredient[r["ingredient"]] = per_ingredient.get(r["ingredient"], 0) + 1
    print(f"  per ingredient: {per_ingredient}")
    server.batch([f"fill {x0 - 1} {Y} {z0 - 1} {x0 + columns * 2} {Y + 1} {z0 + rows * 2} air",
                  "kill @e[type=item]"])
    return {"placed_at": placed, "waited": waited, "results": results}


# ── timing ──────────────────────────────────────────────────────────────────


def campaign_timing(server: Server, bot: ms.Bot, origin: tuple[int, int]) -> dict:
    x0, z0 = origin
    server.batch([f"fill {x0 - 2} {Y} {z0 - 2} {x0 + 20} {Y + 2} {z0 + 6} air",
                  "kill @e[type=item]"])
    water = [bottle("minecraft:potion", "minecraft:water", s) for s in range(3)]
    a, b, c, d, e = [(x0 + 3 * k, Y, z0) for k in range(5)]
    f, g, h = [(x0 + 8 * k, Y, z0 + 4) for k in range(3)]
    placing = [
        f"setblock {a[0]} {a[1]} {a[2]} {stand_nbt(water, 'minecraft:nether_wart', fuel=20)}",
        f"setblock {b[0]} {b[1]} {b[2]} {stand_nbt(water, 'minecraft:nether_wart', fuel=0, powder=1)}",
        f"setblock {c[0]} {c[1]} {c[2]} {stand_nbt(water, 'minecraft:nether_wart', fuel=0)}",
        f"setblock {d[0]} {d[1]} {d[2]} {stand_nbt(water, 'minecraft:nether_wart', fuel=1, powder=1)}",
        f"setblock {e[0]} {e[1]} {e[2]} {stand_nbt(water, 'minecraft:nether_wart', fuel=20)}",
        # Bottles in slots 0 and 2 only: the block state.
        f"setblock {f[0]} {f[1]} {f[2]} "
        + stand_nbt([bottle("minecraft:potion", "minecraft:water", 0),
                     bottle("minecraft:potion", "minecraft:water", 2)], None, fuel=0),
        # Dragon's breath, one and two.
        f"setblock {g[0]} {g[1]} {g[2]} "
        + stand_nbt([bottle("minecraft:splash_potion", "minecraft:water", s) for s in range(3)],
                    "minecraft:dragon_breath", count=1),
        f"setblock {h[0]} {h[1]} {h[2]} "
        + stand_nbt([bottle("minecraft:splash_potion", "minecraft:water", s) for s in range(3)],
                    "minecraft:dragon_breath", count=2),
        "time query gametime",
    ]
    placed = gametime_of(server.batch(placing))
    samples = []
    removed_at = None
    deadline = time.monotonic() + 40.0
    while time.monotonic() < deadline:
        lines = server.batch(["time query gametime"]
                             + [f"data get block {p[0]} {p[1]} {p[2]}" for p in (a, b, c, d, e)])
        now = gametime_of(lines)
        datas = [plain(parse_snbt(m.group(1).strip())) for line in lines
                 if (m := BLOCK_DATA.search(line))]
        if now is None or len(datas) != 5:
            continue
        samples.append({"t": now - placed,
                        "a": [datas[0].get("BrewTime"), datas[0].get("Fuel"),
                              describe(items_by_slot(datas[0]).get(0))],
                        "b": [datas[1].get("BrewTime"), datas[1].get("Fuel"),
                              describe(items_by_slot(datas[1]).get(4))],
                        "c": [datas[2].get("BrewTime"), datas[2].get("Fuel")],
                        "d": [datas[3].get("BrewTime"), datas[3].get("Fuel"),
                              describe(items_by_slot(datas[3]).get(4))],
                        "e": [datas[4].get("BrewTime"), datas[4].get("Fuel"),
                              describe(items_by_slot(datas[4]).get(3))]})
        # Take E's ingredient away a hundred ticks in.
        if removed_at is None and now - placed >= 100:
            removed_at = gametime_of(server.batch(
                [f"item replace block {e[0]} {e[1]} {e[2]} container.3 with minecraft:air",
                 "time query gametime"])) - placed
        bot.pump(0.15)
        if now - placed > 470:
            break
    # When did A finish, and with what?
    finished = next((s["t"] for s in samples if s["a"][2] and s["a"][2][1] == "minecraft:awkward"),
                    None)
    first = samples[0] if samples else None
    print(f"  first sample t={first['t'] if first else None}: {first}")
    print(f"  A finished (awkward seen) at t={finished}; E ingredient removed at t={removed_at}")

    # D: a second brew, to see the refuel from the powder once the fuel is out.
    server.batch([f"item replace block {d[0]} {d[1]} {d[2]} container.{s} with "
                  f'minecraft:potion{{Potion:"minecraft:water"}}' for s in range(3)]
                 + [f"item replace block {d[0]} {d[1]} {d[2]} container.3 with "
                    f"minecraft:nether_wart"])
    bot.pump(1.0)
    d_second = read_blocks(server, [d])[0]
    print(f"  D second brew: Fuel {d_second.get('Fuel') if d_second else None}, "
          f"powder {describe(items_by_slot(d_second).get(4))}")

    # A: three consecutive brews on one powder, the fuel read at each start.
    fuel_runs = []
    for _ in range(3):
        server.batch([f"item replace block {a[0]} {a[1]} {a[2]} container.{s} with "
                      f'minecraft:potion{{Potion:"minecraft:water"}}' for s in range(3)]
                     + [f"item replace block {a[0]} {a[1]} {a[2]} container.3 with "
                        f"minecraft:nether_wart"])
        bot.pump(0.6)
        start = read_blocks(server, [a])[0]
        wait_ticks(server, bot, 410)
        fuel_runs.append({"fuel_at_start": start.get("Fuel") if start else None,
                          "brew_time_at_start": start.get("BrewTime") if start else None})
    print(f"  consecutive brews on A: {fuel_runs}")

    # A stand whose chunk is unloaded mid-brew and loaded again: does the brew
    # survive, or does the stand forget the ingredient it started with? Far
    # from the bot and from every force-loaded square, so the chunk can go.
    far = (x0 + 480, Y, z0)
    server.batch([f"forceload add {far[0]} {far[2]}",
                  f"setblock {far[0]} {far[1]} {far[2]} {stand_nbt(water, 'minecraft:nether_wart')}"])
    wait_ticks(server, bot, 100)
    before_unload = read_blocks(server, [far])[0]
    server.batch([f"forceload remove {far[0]} {far[2]}"])
    unloaded = False
    for _ in range(60):
        bot.pump(0.5)
        lines = server.batch([f"execute if loaded {far[0]} {far[1]} {far[2]}"])
        if not any("Test passed" in line for line in lines):
            unloaded = True
            break
    wait_ticks(server, bot, 20)
    server.batch([f"forceload add {far[0]} {far[2]}"])
    wait_ticks(server, bot, 3)
    after_reload = read_blocks(server, [far])[0]
    reload = {"unloaded": unloaded,
              "before": [before_unload.get("BrewTime"), before_unload.get("Fuel")]
              if before_unload else None,
              "after": [after_reload.get("BrewTime"), after_reload.get("Fuel")]
              if after_reload else None}
    server.batch([f"setblock {far[0]} {far[1]} {far[2]} air",
                  f"forceload remove {far[0]} {far[2]}"])
    print(f"  unload/reload mid-brew: {reload}")

    states = {}
    for label, pos in (("f", f),):
        for pattern in ("has_bottle_0=true,has_bottle_1=false,has_bottle_2=true",
                        "has_bottle_0=false,has_bottle_1=false,has_bottle_2=false"):
            lines = server.batch([f"execute if block {pos[0]} {pos[1]} {pos[2]} "
                                  f"minecraft:brewing_stand[{pattern}]"])
            states[pattern] = any("Test passed" in line for line in lines)
    print(f"  block state of F: {states}")
    g_data, h_data = read_blocks(server, [g, h])
    bottles_dropped = server.batch([f"execute positioned {h[0]}.5 {h[1]} {h[2]}.5 run "
                                    f"data get entity @e[type=item,limit=1,distance=..3] Item"])
    dragon = {"one": {"ingredient_slot": describe(items_by_slot(g_data).get(3)),
                      "outputs": [describe(items_by_slot(g_data).get(s)) for s in range(3)]},
              "two": {"ingredient_slot": describe(items_by_slot(h_data).get(3)),
                      "dropped": [line for line in bottles_dropped if "entity data" in line]}}
    print(f"  dragon's breath: {dragon}")
    server.batch([f"fill {x0 - 2} {Y} {z0 - 2} {x0 + 20} {Y + 2} {z0 + 6} air",
                  "kill @e[type=item]"])
    return {"samples": samples, "removed_at": removed_at, "finished": finished,
            "d_second": {"fuel": d_second.get("Fuel") if d_second else None,
                         "powder": describe(items_by_slot(d_second).get(4))},
            "consecutive": fuel_runs, "block_state": states, "dragon_breath": dragon}


# ── faces ───────────────────────────────────────────────────────────────────


def campaign_faces(server: Server, bot: ms.Bot, origin: tuple[int, int]) -> dict:
    x0, z0 = origin
    area = f"{x0 - 2} {Y - 1} {z0 - 2} {x0 + 30} {Y + 3} {z0 + 8}"
    cells = {
        "up":   ((x0, Y, z0),      (x0, Y + 1, z0),      "down"),
        "side": ((x0 + 8, Y, z0),  (x0 + 7, Y, z0),      "east"),
    }
    items = [('minecraft:potion{Potion:"minecraft:water"}', "minecraft:potion"),
             ("minecraft:blaze_powder", "minecraft:blaze_powder"),
             ("minecraft:nether_wart", "minecraft:nether_wart"),
             ("minecraft:glass_bottle", "minecraft:glass_bottle"),
             ("minecraft:redstone", "minecraft:redstone")]
    insert = {}
    for given, item in items:
        server.batch([f"fill {area} air", f"fill {x0 - 2} {Y - 1} {z0 - 2} {x0 + 30} {Y - 1} "
                      f"{z0 + 8} stone", "kill @e[type=item]"])
        commands = []
        for stand, hopper, facing in cells.values():
            commands.append(f"setblock {stand[0]} {stand[1]} {stand[2]} brewing_stand")
            commands.append(f"setblock {hopper[0]} {hopper[1]} {hopper[2]} "
                            f"hopper[facing={facing},enabled=true]")
            commands.append(f"item replace block {hopper[0]} {hopper[1]} {hopper[2]} "
                            f"container.0 with {given} 1")
        server.batch(commands)
        wait_ticks(server, bot, 40)
        answers = read_blocks(server, [c[0] for c in cells.values()])
        insert[item] = {}
        for label, data in zip(cells, answers):
            slots = items_by_slot(data)
            landed = [s for s, e in slots.items() if e["id"] == item]
            insert[item][label] = {"slots": landed, "fuel": data.get("Fuel") if data else None}
        print(f"  {item:28s} {insert[item]}")

    # Out through the bottom.
    server.batch([f"fill {area} air", f"fill {x0 - 2} {Y - 1} {z0 - 2} {x0 + 30} {Y - 1} "
                  f"{z0 + 8} stone", "kill @e[type=item]"])
    below = {}
    rigs = {
        "potions_wart_powder": [bottle("minecraft:potion", "minecraft:awkward", 0),
                                '{Slot:3b,id:"minecraft:nether_wart",Count:1b}',
                                '{Slot:4b,id:"minecraft:blaze_powder",Count:1b}'],
        "glass_bottle_in_ingredient": ['{Slot:3b,id:"minecraft:glass_bottle",Count:1b}'],
        "glass_bottle_in_bottle_slot": ['{Slot:1b,id:"minecraft:glass_bottle",Count:1b}'],
    }
    positions = {}
    for k, (label, items_nbt) in enumerate(rigs.items()):
        pos = (x0 + 6 * k, Y + 1, z0 + 4)
        positions[label] = pos
        server.batch([f"setblock {pos[0]} {pos[1] - 1} {pos[2]} hopper[facing=north,enabled=true]",
                      f"setblock {pos[0]} {pos[1]} {pos[2]} "
                      f"brewing_stand{{Items:[{','.join(items_nbt)}],Fuel:5b}}"])
    wait_ticks(server, bot, 60)
    for label, pos in positions.items():
        hopper, stand = read_blocks(server, [(pos[0], pos[1] - 1, pos[2]), pos])
        below[label] = {"hopper": [describe(e) for e in items_by_slot(hopper).values()],
                        "stand": {s: describe(e) for s, e in items_by_slot(stand).items()}}
        print(f"  below, {label}: {below[label]}")
    server.batch([f"fill {area} air", "kill @e[type=item]"])
    return {"insert": insert, "extract_below": below}


# ── window ──────────────────────────────────────────────────────────────────


def use_on(bot: ms.Bot, pos: tuple[int, int, int], sequence: int) -> None:
    bot.send(SB_USE_ITEM_ON, varint(0) + block_pos(*pos) + varint(1)
             + struct.pack(">fff", 0.5, 1.0, 0.5) + bytes([0]) + varint(sequence))


def campaign_window(server: Server, bot: ms.Bot, origin: tuple[int, int]) -> dict:
    x0, z0 = origin
    pos = (x0, Y, z0)
    water = [bottle("minecraft:potion", "minecraft:water", s) for s in range(3)]
    server.batch([f"fill {x0 - 2} {Y} {z0 - 2} {x0 + 2} {Y + 2} {z0 + 3} air",
                  f"setblock {pos[0]} {pos[1]} {pos[2]} "
                  + stand_nbt(water, "minecraft:nether_wart", fuel=0, powder=2),
                  f"tp {BOT} {x0 + 0.5} {Y} {z0 + 2.5} 180 30"])
    bot.pump(1.0)
    bot.drain()
    use_on(bot, pos, 900)
    bot.pump(1.0)
    captured = bot.drain()
    window = None
    menu = None
    for pid, payload in captured:
        if pid == CB_OPEN_SCREEN:
            window, i = read_varint(payload, 0)
            menu, _ = read_varint(payload, i)
    properties = []
    for pid, payload in captured:
        if len(payload) == 5 and window is not None and payload[0] == window:
            _, prop, value = struct.unpack(">bhh", payload)
            properties.append({"id": pid, "t": 0.0, "property": prop, "value": value,
                               "opening": True})
    content_len = None
    started = time.monotonic()
    while time.monotonic() - started < 24.0:
        bot.pump(0.5)
        for pid, payload in bot.drain():
            if len(payload) == 5 and window is not None and payload[0] == window:
                _, prop, value = struct.unpack(">bhh", payload)
                properties.append({"id": pid, "t": round(time.monotonic() - started, 2),
                                   "property": prop, "value": value})
    for pid, payload in captured:
        if pid == 0x12 and window is not None and payload and payload[0] == window:
            _, i = read_varint(payload, 1)
            count, _ = read_varint(payload, i)
            content_len = count
    distinct = {}
    for row in properties:
        distinct.setdefault(row["property"], []).append(row["value"])
    print(f"  window {window}, menu {menu}, {content_len} slots; properties seen: "
          f"{ {k: (v[:3], v[-3:], len(v)) for k, v in distinct.items()} }")
    server.batch([f"setblock {pos[0]} {pos[1]} {pos[2]} air", "kill @e[type=item]"])
    return {"window": window, "menu": menu, "content_slots": content_len,
            "properties": properties}


# ── drink ───────────────────────────────────────────────────────────────────


def drink(server: Server, bot: ms.Bot, item_nbt: str, sequence: int, wait: float = 2.6) -> None:
    server.batch([f"item replace entity {BOT} hotbar.0 with {item_nbt}"])
    bot.pump(0.3)
    bot.hold(0)
    bot.drain()
    bot.use_item(sequence)
    bot.pump(wait)


def campaign_drink(server: Server, bot: ms.Bot, origin: tuple[int, int]) -> dict:
    server.batch([f"gamemode survival {BOT}", "gamerule naturalRegeneration false",
                  "time set noon"])
    out = {}
    sequence = 1000
    for potion in POTIONS:
        instant = potion.endswith("healing") or potion.endswith("harming")
        start_health = 6.0 if potion.endswith("healing") else 20.0
        reset_bot(server, bot, start_health)
        before = bot.health
        drink(server, bot, f'minecraft:potion{{Potion:"{potion}"}}', sequence)
        sequence += 1
        effects = entity_effects(bot)
        hand = ms.scalar(server.batch([f"data get entity {BOT} SelectedItem"]))
        out[potion] = {"effects": effects, "health_before": before, "health_after": bot.health,
                       "hand_after": hand}
        print(f"  {potion:34s} {[(e['effect'], e['amplifier'], e['duration']) for e in effects]}"
              + (f" health {before} -> {bot.health}" if instant else "")
              + f" hand {hand}", flush=True)
    # Creative: is the bottle kept?
    server.batch([f"gamemode creative {BOT}"])
    reset_bot(server, bot)
    drink(server, bot, 'minecraft:potion{Potion:"minecraft:swiftness"}', sequence)
    out["$creative_hand"] = ms.scalar(server.batch([f"data get entity {BOT} SelectedItem"]))
    out["$creative_effects"] = entity_effects(bot)
    print(f"  creative: hand {out['$creative_hand']}, effects {out['$creative_effects']}")
    server.batch([f"gamemode survival {BOT}", "gamerule naturalRegeneration true"])
    return out


def campaign_drinktime(server: Server, bot: ms.Bot, origin: tuple[int, int]) -> dict:
    """Ticks between Use Item and the effect, on the server's clock.

    G_use is a `time query gametime` sent right after the Use Item packet: both
    are handled between the same two ticks unless a tick falls between them.
    The tick the effect landed is recovered from its remaining duration read
    with the gametime in one batch, and the phase of that recovery is
    calibrated by a console `effect give` treated exactly the same way."""
    server.batch([f"gamemode survival {BOT}"])
    rows = []
    for trial in range(8):
        reset_bot(server, bot)
        # Control: console give at a known tick.
        given = gametime_of(server.batch([f"effect give {BOT} minecraft:haste 100 0",
                                          "time query gametime"]))
        bot.pump(1.0)
        lines = server.batch(["time query gametime",
                              f"data get entity {BOT} ActiveEffects[0].Duration"])
        read_c = gametime_of(lines)
        remaining_c = ms.number(lines)
        server.batch([f"effect clear {BOT}"])
        offset = (read_c - (2000 - remaining_c)) - given if None not in (read_c, remaining_c,
                                                                           given) else None
        # The drink.
        server.batch([f"item replace entity {BOT} hotbar.0 with "
                      f'minecraft:potion{{Potion:"minecraft:long_swiftness"}}'])
        bot.pump(0.3)
        bot.hold(0)
        bot.drain()
        bot.use_item(2000 + trial)
        used = gametime(server)
        bot.pump(2.4)
        effects = entity_effects(bot)
        lines = server.batch(["time query gametime",
                              f"data get entity {BOT} ActiveEffects[0].Duration"])
        read_d = gametime_of(lines)
        remaining_d = ms.number(lines)
        duration = effects[0]["duration"] if effects else None
        applied = (read_d - (duration - remaining_d) - offset
                   if None not in (read_d, remaining_d, duration, offset) else None)
        rows.append({"used": used, "applied": applied, "offset": offset,
                     "ticks": applied - used if None not in (applied, used) else None})
        print(f"  trial {trial}: offset {offset}, drink ticks {rows[-1]['ticks']}", flush=True)

    # The control: the same instrument on a food whose use time survie.md
    # established another way. A golden apple is always edible and gives
    # absorption for 2400 ticks; if it reads the same as the potion, the two
    # share one use time and any gap to 32 is this instrument's phase.
    food = []
    for trial in range(6):
        reset_bot(server, bot)
        server.batch([f"item replace entity {BOT} hotbar.0 with minecraft:golden_apple"])
        bot.pump(0.3)
        bot.hold(0)
        bot.drain()
        bot.use_item(2100 + trial)
        used = gametime(server)
        bot.pump(2.4)
        effects = [e for e in entity_effects(bot) if e["effect"] == "minecraft:absorption"]
        lines = server.batch(["time query gametime",
                              f"data get entity {BOT} ActiveEffects[{{Id:22}}].Duration"])
        read_f = gametime_of(lines)
        remaining_f = ms.number(lines)
        duration = effects[0]["duration"] if effects else None
        applied = (read_f - (duration - remaining_f)
                   if None not in (read_f, remaining_f, duration) else None)
        food.append({"used": used, "applied": applied,
                     "ticks": applied - used if None not in (applied, used) else None})
        print(f"  golden apple {trial}: eat ticks {food[-1]['ticks']}", flush=True)
    return {"trials": rows, "golden_apple": food}


# ── splash ──────────────────────────────────────────────────────────────────


def summon_potion(item: str, tag: str, x: float, y: float, z: float, vy: float) -> str:
    return (f"summon minecraft:potion {x:.4f} {y:.4f} {z:.4f} "
            f"{{Item:{{id:\"{item}\",Count:1b,tag:{tag}}},Motion:[0.0d,{vy}d,0.0d]}}")


SPLASH_DISTANCES = [1.0, 1.5, 2.0, 2.4, 2.5, 3.0, 3.1, 3.2, 3.5, 3.9, 3.99, 4.05, 4.5]


def campaign_splash(server: Server, bot: ms.Bot, origin: tuple[int, int]) -> dict:
    x0, z0 = origin
    bx, bz = x0 + 0.5, z0 + 0.5
    server.batch([f"fill {x0 - 6} {Y} {z0 - 6} {x0 + 6} {Y + 4} {z0 + 6} air",
                  f"gamemode survival {BOT}", "gamerule naturalRegeneration false"])
    speed = '{Potion:"minecraft:awkward",CustomPotionEffects:[{Id:1,Amplifier:0b,Duration:1000}]}'
    short = '{Potion:"minecraft:awkward",CustomPotionEffects:[{Id:1,Amplifier:0b,Duration:100}]}'
    heal = '{Potion:"minecraft:awkward",CustomPotionEffects:[{Id:6,Amplifier:1b,Duration:1}]}'
    out: dict = {"speed_1000": {}, "speed_100": {}, "heal_amp1": {}}

    def one(label: str, tag: str, r: float, health: float | None = None) -> None:
        reset_bot(server, bot, health)
        server.batch([f"tp {BOT} {bx} {Y} {bz}"])
        bot.pump(0.4)
        bot.drain()
        before = bot.health
        if r == 0.0:
            # A direct hit: through the bot's box from above.
            server.batch([summon_potion("minecraft:splash_potion", tag, bx, Y + 2.0, bz, -0.5)])
        else:
            server.batch([summon_potion("minecraft:splash_potion", tag, bx + r, Y + 0.01, bz,
                                        -0.2)])
        bot.pump(0.8)
        effects = entity_effects(bot)
        out[label][str(r)] = {"effects": effects, "health_before": before,
                              "health_after": bot.health}
        print(f"  {label} r={r}: {[(e['effect'], e['duration']) for e in effects]}"
              f" health {before} -> {bot.health}", flush=True)

    for r in [0.0] + SPLASH_DISTANCES:
        one("speed_1000", speed, r)
    for r in (0.0, 2.0, 2.9, 3.0, 3.1, 3.2):
        one("speed_100", short, r)
    for r in (0.0, 1.0, 1.5, 2.0, 2.5, 3.0):
        one("heal_amp1", heal, r, health=2.0)
    server.batch(["gamerule naturalRegeneration true", "kill @e[type=potion]"])
    return out


# ── lingering ───────────────────────────────────────────────────────────────


def cloud_data(server: Server) -> tuple[int | None, dict | None]:
    lines = server.batch(["time query gametime",
                          # No trailing space: with one, Brigadier waits for a
                          # path, rejects the command, and every read is empty —
                          # which is how the first run lost the cloud's NBT.
                          "data get entity @e[type=area_effect_cloud,limit=1]"])
    data = None
    for line in lines:
        match = ENTITY_DATA.search(line)
        if match:
            data = plain(parse_snbt(match.group(1).strip()))
    return gametime_of(lines), data


def campaign_lingering(server: Server, bot: ms.Bot, origin: tuple[int, int]) -> dict:
    x0, z0 = origin
    cx, cz = x0 + 0.5, z0 + 0.5
    server.batch([f"fill {x0 - 8} {Y} {z0 - 8} {x0 + 8} {Y + 4} {z0 + 8} air",
                  "kill @e[type=area_effect_cloud]", f"gamemode survival {BOT}",
                  "gamerule naturalRegeneration false"])
    out = {}
    cloud_type = REGISTRIES["minecraft:entity_type"]["entries"]["minecraft:area_effect_cloud"][
        "protocol_id"]
    for label, tag, health in (("long_swiftness", '{Potion:"minecraft:long_swiftness"}', None),
                               ("strong_healing", '{Potion:"minecraft:strong_healing"}', 2.0),
                               ("awkward", '{Potion:"minecraft:awkward"}', None),
                               # Instant damage's colour: an effect never visible
                               # on an entity, but a potion shows it.
                               ("harming", '{Potion:"minecraft:harming"}', None)):
        reset_bot(server, bot, health)
        server.batch([f"tp {BOT} {cx + 7} {Y} {cz}"])
        bot.pump(0.4)
        bot.drain()
        server.batch([summon_potion("minecraft:lingering_potion", tag, cx, Y + 0.01, cz, -0.2)])
        bot.pump(0.5)
        spawn = [p for pid, p in bot.captured if pid == CB_SPAWN_ENTITY]
        cloud_id = None
        for payload in spawn:
            eid, i = read_varint(payload, 0)
            kind, _ = read_varint(payload, i + 16)
            if kind == cloud_type:
                cloud_id = eid
        metadata = [parse_metadata(p)[1] for pid, p in bot.captured
                    if pid == CB_SET_METADATA and cloud_id is not None
                    and parse_metadata(p)[0] == cloud_id]
        born_t, born = cloud_data(server)
        bot.pump(1.0)
        later_t, later = cloud_data(server)
        bot.drain()
        # Into the cloud.
        server.batch([f"tp {BOT} {cx} {Y} {cz}"])
        entered = gametime(server)
        before = bot.health
        healths = []
        effects = []
        stamps = []
        started = time.monotonic()
        while time.monotonic() - started < 3.0:
            bot.pump(0.1)
            got = entity_effects(bot)
            if got:
                effects.extend(got)
                stamps.append(gametime(server))
            if bot.health is not None and (not healths or healths[-1] != bot.health):
                healths.append(bot.health)
        after_t, after = cloud_data(server)
        server.batch([f"tp {BOT} {cx + 7} {Y} {cz}"])
        metadata += [parse_metadata(p)[1] for pid, p in bot.captured
                     if pid == CB_SET_METADATA and cloud_id is not None
                     and parse_metadata(p)[0] == cloud_id]
        out[label] = {"cloud_id": cloud_id, "metadata": metadata, "born_t": born_t,
                      "born": born, "later_t": later_t, "later": later, "entered": entered,
                      "effects": effects, "effect_gametimes": stamps,
                      "health_before": before, "healths": healths,
                      "after_t": after_t, "after": after}
        keep = ("Radius", "RadiusOnUse", "RadiusPerTick", "Duration", "DurationOnUse",
                "WaitTime", "ReapplicationDelay", "Age", "Potion", "Color", "Particle")
        print(f"  {label}: born {born_t} { {k: (born or {}).get(k) for k in keep} }")
        print(f"    later {later_t} Radius {(later or {}).get('Radius')} Age "
              f"{(later or {}).get('Age')}; after {after_t} Radius {(after or {}).get('Radius')}")
        print(f"    effects {[(e['effect'], e['amplifier'], e['duration']) for e in effects]} "
              f"at {stamps}; health {before} -> {healths}; metadata {metadata}", flush=True)
        server.batch(["kill @e[type=area_effect_cloud]"])
        bot.pump(0.5)
    server.batch(["gamerule naturalRegeneration true"])
    return out


# ── arrow ───────────────────────────────────────────────────────────────────


def campaign_arrow(server: Server, bot: ms.Bot, origin: tuple[int, int]) -> dict:
    x0, z0 = origin
    bx, bz = x0 + 0.5, z0 + 0.5
    server.batch([f"fill {x0 - 6} {Y} {z0 - 6} {x0 + 6} {Y + 4} {z0 + 6} air",
                  f"gamemode survival {BOT}", "gamerule naturalRegeneration false"])
    cases = [("arrow", 'Potion:"minecraft:swiftness"'),
             ("arrow", 'Potion:"minecraft:long_swiftness"'),
             ("arrow", 'Potion:"minecraft:poison"'),
             ("arrow", 'Potion:"minecraft:strong_poison"'),
             ("arrow", 'Potion:"minecraft:turtle_master"'),
             ("arrow", 'Potion:"minecraft:awkward",CustomPotionEffects:[{Id:1,Amplifier:0b,'
                       'Duration:5}]'),
             ("arrow", 'Potion:"minecraft:harming"'),
             ("arrow", ""),
             ("spectral_arrow", ""),
             ("spectral_arrow", "Duration:300")]
    out = []
    for kind, extra in cases:
        reset_bot(server, bot)
        server.batch([f"tp {BOT} {bx} {Y} {bz}"])
        bot.pump(0.4)
        bot.drain()
        before = bot.health
        comma = "," if extra else ""
        server.batch([f"summon minecraft:{kind} {bx} {Y + 1.0} {bz - 1.2} "
                      f"{{{extra}{comma}Motion:[0.0d,0.0d,1.0d],NoGravity:1b,damage:0.5d}}"])
        bot.pump(1.0)
        effects = entity_effects(bot)
        out.append({"kind": kind, "nbt": extra, "effects": effects,
                    "health_before": before, "health_after": bot.health})
        print(f"  {kind} {{{extra}}}: {[(e['effect'], e['amplifier'], e['duration']) for e in effects]}"
              f" health {before} -> {bot.health}", flush=True)
        server.batch(["kill @e[type=arrow]", "kill @e[type=spectral_arrow]"])
    server.batch(["gamerule naturalRegeneration true"])
    return {"cases": out}


# ── stew ────────────────────────────────────────────────────────────────────


def campaign_stew(server: Server, bot: ms.Bot, origin: tuple[int, int]) -> dict:
    server.batch([f"gamemode survival {BOT}"])
    cases = {
        "night_vision_100": '{Effects:[{EffectId:16,EffectDuration:100}]}',
        "two_effects": '{Effects:[{EffectId:16,EffectDuration:100},{EffectId:19,EffectDuration:60}]}',
        "no_duration": '{Effects:[{EffectId:16}]}',
        "empty": "{}",
        "later_format": '{effects:[{id:"minecraft:night_vision",duration:100}]}',
    }
    out = {}
    sequence = 3000
    for label, tag in cases.items():
        reset_bot(server, bot)
        ms.drain_food(server, bot, 10)
        server.batch([f"effect clear {BOT}"])
        bot.pump(0.2)
        drink(server, bot, f"minecraft:suspicious_stew{tag}", sequence)
        sequence += 1
        effects = entity_effects(bot)
        hand = ms.scalar(server.batch([f"data get entity {BOT} SelectedItem"]))
        out[label] = {"effects": effects, "hand_after": hand}
        print(f"  {label}: {[(e['effect'], e['amplifier'], e['duration']) for e in effects]} "
              f"hand {hand}", flush=True)
    return out


ALL = {
    "recipes": campaign_recipes, "timing": campaign_timing, "faces": campaign_faces,
    "window": campaign_window, "drink": campaign_drink, "drinktime": campaign_drinktime,
    "splash": campaign_splash, "lingering": campaign_lingering, "arrow": campaign_arrow,
    "stew": campaign_stew,
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", default=",".join(ALL))
    parser.add_argument("out", nargs="?", default=str(NORMALIZED / "brewing.json"))
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
                            "docs/provenance/alchimie.md et scripts/measure_brewing.py.")

    shutil.rmtree(RUN, ignore_errors=True)
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
        server.batch([f"forceload add {int(px) - 64} {int(pz) - 64} {int(px) + 64} {int(pz) + 64}",
                      f"gamemode survival {BOT}"])
        bot.pump(2.0)
        print(f"bot at {px:.2f} {py:.2f} {pz:.2f}, entity {bot.entity_id}")
        # Stands and rigs far enough from the bot that nothing it does touches
        # them, close enough to stay force-loaded.
        origins = {"recipes": (int(px) - 30, int(pz) + 8), "timing": (int(px) + 8, int(pz) - 20),
                   "faces": (int(px) + 8, int(pz) - 40), "window": (int(px) - 20, int(pz) - 20)}
        for name in wanted:
            print(f"\n── {name} ──", flush=True)
            bot.pump(0.2)
            if bot.lost:
                bot = ms.Bot(PORT, name=BOT)
                ms._BOT = bot
                bot.pump(2.0)
                server.batch([f"gamemode survival {BOT}"])
            origin = origins.get(name, (int(px), int(pz)))
            started = time.monotonic()
            try:
                document[name] = ALL[name](server, bot, origin)
            except Exception as error:  # noqa: BLE001 — a campaign's failure is data
                import traceback
                traceback.print_exc()
                document[name] = {"error": repr(error)}
                if bot.lost:
                    bot = ms.Bot(PORT, name=BOT)
                    ms._BOT = bot
                    bot.pump(2.0)
            bot.drain()
            print(f"  ({time.monotonic() - started:.0f} s)", flush=True)
            out_path.parent.mkdir(parents=True, exist_ok=True)
            out_path.write_text(json.dumps(document, indent=1, sort_keys=True))
    finally:
        server.stop()
        shutil.rmtree(RUN, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
