#!/usr/bin/env python3
"""Ask a real 1.20.1 server how the Nether's mobs move, trade, shoot and spawn.

── nether-2 ── The oracle for docs/provenance/nether-2.md § 3. Campaigns:

  stroll   Five persistent mobs of one species at a time on a superflat of bare
           grass, sampled for sixty seconds; the cruise speed is the top plateau
           of the per-tick steps, as in measure_mobs2.py, turned into a goal
           modifier through the walk law. Piglins, brutes and hoglins carry
           `IsImmuneToZombification:1b` — in the overworld they would otherwise
           become zombified after 300 ticks, in the middle of the measurement.
           Striders are measured twice: on grass (cold, they shiver) and on a
           lava pool (warm).
  barter   Ten piglins, one gold ingot summoned at each one's feet per round;
           every item the round leaves is read and removed from the console
           (`data get entity @e[type=item,limit=1,sort=arbitrary] Item`, then
           `kill` of the same entity in the same batch). The distribution over
           the table's entries, and the delay between the ingot and the loot.
  fire     A blaze eight blocks from the probe (survival, Resistance V, Fire
           Resistance), then a ghast twenty blocks away: every Spawn Entity of a
           `small_fireball` / `fireball` the probe receives, for a minute each —
           the cadence of the volleys.
  biomes   A world generated at the reference seed. For each Nether biome:
           `locate`, the probe spread onto the floor there, everything killed,
           then every Spawn Entity for 120 s, each spawn's biome tested one by
           one (`execute in the_nether if biome`).

Usage: python3 scripts/measure_nether_mobs.py [stroll|barter|fire|biomes] ...
Writes .scratch/nether_mobs.json (the worktree's scratch; nothing outside it).
Run the whole thing under the machine's JVM lock:
    lockf /tmp/ov-vanilla.lock python3 scripts/measure_nether_mobs.py stroll barter fire
"""
from __future__ import annotations

import json
import math
import re
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from measure_husbandry import Hand  # noqa: E402
from measure_mobs import FlatServer  # noqa: E402
from measure_mobs2 import LAW, Field, grid, plateau, steps_of  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / ".scratch" / "nether_mobs.json"
RUN = ROOT / ".scratch" / "nether-mobs-oracle"
PORT = 25653
WORLD_PORT = 25657
Y = -60

GAMETIME = re.compile(r"The time is (\d+)")
ITEM = re.compile(r'id: "minecraft:([a-z_]+)"')
COUNT = re.compile(r"Count: (\d+)b")
NO_ENTITY = "No entity was found"
LOCATED = re.compile(r"is at \[(-?\d+), [^,]+, (-?\d+)\]")
TEST = re.compile(r"Test (passed|failed)")

# Entity type ids (normalized/registries.json, minecraft:entity_type).
SMALL_FIREBALL = 89
FIREBALL = 57
ITEM_TYPE = 54

IMMUNE = "IsImmuneToZombification:1b"
SPECIES = [
    ("piglin", IMMUNE), ("piglin_brute", IMMUNE), ("hoglin", IMMUNE), ("zoglin", ""),
    ("zombified_piglin", ""), ("wither_skeleton", ""), ("strider", ""),
    ("magma_cube", "Size:1"), ("blaze", ""), ("ghast", ""),
]
NETHER_BIOMES = ["nether_wastes", "crimson_forest", "warped_forest", "soul_sand_valley",
                 "basalt_deltas"]


def reset(server, mode: str = "creative") -> None:
    server.batch(["kill @e[type=!minecraft:player]",
                  f"gamemode {mode} ovhand",
                  "tp ovhand -60 -60 0.5",
                  "effect give ovhand minecraft:resistance 999999 4 true",
                  "effect give ovhand minecraft:fire_resistance 999999 0 true",
                  "effect give ovhand minecraft:regeneration 999999 4 true",
                  "time set midnight"], timeout=120)
    time.sleep(1.0)


# ── stroll ──────────────────────────────────────────────────────────────────

def stroll_one(server, kind: str, extra: str, lava: bool) -> list[dict]:
    reset(server)
    if lava:
        server.batch(["fill -30 -61 -30 30 -61 30 minecraft:lava"], timeout=60)
    field = Field(server)
    spacing = 18.0 if kind == "ghast" else 9.0
    y = Y + (8 if kind in ("ghast", "blaze") else 0)
    for name, x, z in grid([kind], 5, spacing):
        field.summon(name, x, z, extra, y=y)
    field.read_attributes()
    time.sleep(1.0)
    field.run(60.0)
    if lava:
        server.batch(["fill -30 -61 -30 30 -61 30 minecraft:grass_block"], timeout=60)
    return field.dump()


def campaign_stroll(server, hand) -> dict:
    out = {}
    for kind, extra in SPECIES:
        print(f"   stroll {kind}", flush=True)
        out[kind] = stroll_one(server, kind, extra, False)
    print("   stroll strider on lava", flush=True)
    out["strider@lava"] = stroll_one(server, "strider", "", True)
    return out


# ── barter ──────────────────────────────────────────────────────────────────

def read_items(server) -> list[tuple[str, int]]:
    """Every item entity in the world, read and removed one at a time."""
    items: list[tuple[str, int]] = []
    for _ in range(400):
        lines = server.batch(["data get entity @e[type=minecraft:item,limit=1,sort=arbitrary] Item",
                              "kill @e[type=minecraft:item,limit=1,sort=arbitrary]"])
        text = " ".join(lines)
        if NO_ENTITY in text:
            break
        m = ITEM.search(text)
        c = COUNT.search(text)
        if m:
            items.append((m.group(1), int(c.group(1)) if c else 1))
    return items


def campaign_barter(server, hand) -> dict:
    reset(server)
    # A piglin picks up nothing with mobGriefing off, and a summoned one (NBT
    # given: `finalizeSpawn` skipped, trap 32) is not allowed to pick up loot at
    # all unless told. The first run of this campaign read zero drops for both
    # reasons at once.
    server.batch(["gamerule mobGriefing true"])
    # A pen of glass, so the piglins stay by their ingots.
    pens = []
    commands = []
    for i in range(10):
        x, z = (i % 5) * 6, (i // 5) * 6 + 10
        commands.append(f"fill {x - 1} -60 {z - 1} {x + 1} -58 {z + 1} minecraft:glass hollow")
        commands.append(f"fill {x} -60 {z} {x} -59 {z} minecraft:air")
        commands.append(f"summon minecraft:piglin {x + 0.5} -60 {z + 0.5} "
                        f"{{{IMMUNE},CanPickUpLoot:1b,PersistenceRequired:1b,Silent:1b,"
                        f"Tags:[\"trader\"]}}")
        pens.append((x, z))
    server.batch(commands, timeout=120)
    time.sleep(1.0)
    drops: list[tuple[str, int]] = []
    delays: list[float] = []
    rounds = 24
    for r in range(rounds):
        read_items(server)
        with hand.state_lock:
            hand.spawns.clear()
        start = time.monotonic()
        server.batch([f"summon minecraft:item {x + 0.5} -59.9 {z + 0.5} "
                      "{Item:{id:\"minecraft:gold_ingot\",Count:1b},PickupDelay:0}"
                      for x, z in pens], timeout=60)
        time.sleep(11.0)
        with hand.state_lock:
            loot_times = sorted(s["t"] - start for s in hand.spawns.values()
                                if s["type"] == ITEM_TYPE and s["t"] - start > 1.0)
        delays.extend(loot_times)
        got = read_items(server)
        got = [g for g in got if g[0] != "gold_ingot"]
        drops.extend(got)
        print(f"   barter round {r + 1}/{rounds}: {len(got)} drops", flush=True)
    server.batch(["kill @e[type=!minecraft:player]", "gamerule mobGriefing false"])
    return {"ingots": rounds * len(pens), "drops": drops, "delays": delays}


# ── fire ────────────────────────────────────────────────────────────────────

def shots(server, hand, kind: str, distance: float, seconds: float) -> dict:
    reset(server, "survival")
    server.batch(["tp ovhand 0.5 -60 0.5"])
    y = Y + (6 if kind == "ghast" else 0)
    server.batch([f"summon minecraft:{kind} {distance + 0.5} {y} 0.5 "
                  "{PersistenceRequired:1b,Silent:1b}"])
    with hand.state_lock:
        hand.spawns.clear()
    start = time.monotonic()
    while time.monotonic() - start < seconds:
        server.send("tp ovhand 0.5 -60 0.5", "effect give ovhand minecraft:instant_health 1 4 true")
        time.sleep(1.0)
    with hand.state_lock:
        fired = sorted((s["t"] - start, s["type"]) for s in hand.spawns.values()
                       if s["type"] in (SMALL_FIREBALL, FIREBALL))
    server.batch(["kill @e[type=!minecraft:player]"])
    return {"times": [t for t, _ in fired], "types": [k for _, k in fired]}


def campaign_fire(server, hand) -> dict:
    out = {}
    print("   fire blaze", flush=True)
    out["blaze"] = shots(server, hand, "blaze", 8.0, 60.0)
    print(f"      {len(out['blaze']['times'])} small fireballs", flush=True)
    print("   fire ghast", flush=True)
    out["ghast"] = shots(server, hand, "ghast", 20.0, 60.0)
    print(f"      {len(out['ghast']['times'])} fireballs", flush=True)
    return out


# ── biomes ──────────────────────────────────────────────────────────────────

class WorldServer(FlatServer):
    EXTRA_PROPERTIES = FlatServer.EXTRA_PROPERTIES + (
        "level-type=minecraft\\:normal\n"
        "level-seed=1234567890\n"
        "view-distance=8\n"
        "simulation-distance=8\n"
        "allow-nether=true\n"
    )


def tests(server, commands: list[str]) -> list[bool | None]:
    batch = []
    for i, command in enumerate(commands):
        batch.append(f"say ovt{i}")
        batch.append(command)
    out: list[bool | None] = [None] * len(commands)
    current = -1
    for line in server.batch(batch, timeout=600):
        m = re.search(r"\[Server\] ovt(\d+)$", line)
        if m:
            current = int(m.group(1))
            continue
        v = TEST.search(line)
        if v and current >= 0:
            out[current] = v.group(1) == "passed"
    return out


def campaign_biomes(server, hand, names: list[str]) -> dict:
    out: dict = {}
    server.batch(["gamemode creative ovbiome", "gamerule doMobSpawning false"], timeout=60)
    for biome in NETHER_BIOMES:
        lines = server.batch([f"execute in minecraft:the_nether run locate biome "
                              f"minecraft:{biome}"], timeout=600)
        found = next((LOCATED.search(l) for l in lines if LOCATED.search(l)), None)
        if found is None:
            out[biome] = {"error": "not located", "lines": lines[-3:]}
            continue
        cx, cz = int(found.group(1)), int(found.group(2))
        server.batch([f"execute in minecraft:the_nether run spreadplayers {cx} {cz} 0 8 under "
                      f"100 false ovbiome"], timeout=600)
        time.sleep(20.0)
        server.batch(["execute in minecraft:the_nether run kill @e[type=!minecraft:player]"],
                     timeout=60)
        time.sleep(1.0)
        with hand.state_lock:
            hand.spawns.clear()
        server.batch(["gamerule doMobSpawning true"], timeout=60)
        time.sleep(120.0)
        server.batch(["gamerule doMobSpawning false"], timeout=60)
        with hand.state_lock:
            natural = [(names[s["type"]], s["pos"]) for s in hand.spawns.values()]
        natural = [(n, p) for n, p in natural
                   if n not in ("minecraft:item", "minecraft:experience_orb", "minecraft:player",
                                "minecraft:small_fireball", "minecraft:fireball")]
        inside = tests(server, [f"execute in minecraft:the_nether if biome "
                                f"{int(math.floor(p[0]))} {int(math.floor(p[1]))} "
                                f"{int(math.floor(p[2]))} minecraft:{biome}"
                                for _, p in natural])
        counts: dict[str, int] = {}
        for (n, _), ok in zip(natural, inside):
            if ok:
                counts[n] = counts.get(n, 0) + 1
        out[biome] = {"centre": [cx, cz], "natural_in_biome": counts,
                      "natural_total": len(natural),
                      "spawns": [[n, list(p)] for n, p in natural]}
        print(f"   {biome}: in biome {counts} (of {len(natural)})", flush=True)
        server.batch(["execute in minecraft:the_nether run kill @e[type=!minecraft:player]"],
                     timeout=60)
    return out


# ── analysis ────────────────────────────────────────────────────────────────

def analyze(result: dict) -> dict:
    table: dict = {}
    for kind, group in result.get("stroll", {}).items():
        steps: list[float] = []
        steps3: list[float] = []
        attr = None
        for mob in group:
            steps.extend(steps_of(mob["trace"]))
            for a, b in zip(mob["trace"], mob["trace"][1:]):
                dt = b[0] - a[0]
                if 0 < dt <= 4:
                    steps3.append(math.dist(a[1:], b[1:]) / dt)
            attr = mob.get("attribute") or attr
        row = plateau(steps)
        row["attribute"] = attr
        row["3d"] = plateau(steps3).get("cruise")
        if "cruise" in row and attr:
            row["implied_modifier"] = round(math.sqrt(row["cruise"] / LAW) / attr, 4)
        table[kind] = row
    barter = result.get("barter")
    if barter:
        hist: dict[str, list[int]] = {}
        for name, count in barter["drops"]:
            hist.setdefault(name, []).append(count)
        table["barter"] = {name: {"n": len(v), "min": min(v), "max": max(v),
                                  "mean": sum(v) / len(v)} for name, v in sorted(hist.items())}
        delays = barter.get("delays") or []
        if delays:
            table["barter_delay_s"] = {"min": min(delays), "median": sorted(delays)[len(delays) // 2]}
    fire = result.get("fire")
    if fire:
        for kind, rec in fire.items():
            t = rec["times"]
            gaps = [round(b - a, 2) for a, b in zip(t, t[1:])]
            table[f"fire_{kind}"] = {"shots": len(t), "gaps": gaps}
    return table


def run_flat(wanted: list[str], result: dict) -> None:
    if RUN.exists():
        shutil.rmtree(RUN)
    server = FlatServer(RUN, port=PORT)
    hand = None
    try:
        server.batch(["gamerule doDaylightCycle false", "gamerule doWeatherCycle false",
                      "gamerule doMobSpawning false", "gamerule mobGriefing false",
                      "gamerule randomTickSpeed 0", "difficulty hard",
                      "gamerule doImmediateRespawn true"], timeout=120)
        hand = Hand(PORT, "ovhand")
        time.sleep(2.0)
        server.batch(["forceload add -64 -64 64 64"], timeout=120)
        for name in wanted:
            print(f"── campaign {name}", flush=True)
            result[name] = {"stroll": campaign_stroll, "barter": campaign_barter,
                            "fire": campaign_fire}[name](server, hand)
            OUT.write_text(json.dumps(result) + "\n")
    finally:
        if hand is not None:
            hand.close()
        server.stop()
        shutil.rmtree(RUN, ignore_errors=True)


def run_world(result: dict) -> None:
    run = RUN.with_name("nether-mobs-world")
    if run.exists():
        shutil.rmtree(run)
    server = WorldServer(run, port=WORLD_PORT)
    hand = None
    names = json.loads((ROOT / "data" / "vanilla" / "1.20.1" / "normalized" /
                        "registries.json").read_text())["registries"]["minecraft:entity_type"][
        "entries"]
    try:
        server.batch(["gamerule doWeatherCycle false", "gamerule doMobSpawning false",
                      "gamerule randomTickSpeed 0", "difficulty hard"], timeout=120)
        hand = Hand(WORLD_PORT, "ovbiome")
        time.sleep(2.0)
        print("── campaign biomes", flush=True)
        result["biomes"] = campaign_biomes(server, hand, names)
        OUT.write_text(json.dumps(result) + "\n")
    finally:
        if hand is not None:
            hand.close()
        server.stop()
        shutil.rmtree(run, ignore_errors=True)


def main(argv: list[str]) -> int:
    names = argv[1:] or ["stroll", "barter", "fire"]
    result = json.loads(OUT.read_text()) if OUT.exists() else {}
    flat = [n for n in names if n in ("stroll", "barter", "fire")]
    if flat:
        run_flat(flat, result)
    if "biomes" in names:
        run_world(result)
    table = analyze(result)
    result["analysis"] = table
    OUT.write_text(json.dumps(result) + "\n")
    print(json.dumps(table, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
