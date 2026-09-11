#!/usr/bin/env python3
"""The mobs, third wave: what a hostile mob does to a player, when mobs go
away, and what a vanilla save of them looks like. The oracle is the real 1.20.1
server (tools/vanilla/server.jar); every campaign drives it through its console
with a probe client connected, and reads the player back with `data get`.

Campaigns (run all by default, or name some):

  melee     a zombie, husk, drowned, spider, cave spider, enderman or zombified
            piglin next to a survival probe that does not move: the health lost
            at each hit, the tick of each hit, and the effect a hit left
            (hunger, poison) — by difficulty, and for the zombie by armour.
  ranged    a skeleton and a stray (summoned WITHOUT NBT so they get their bow:
            piège 32) at 8 and 14 blocks, sixty seconds per difficulty: hits,
            arrows left on the ground, and the Slowness a stray's arrow gives.
  creeper   ignited creepers at four blocks (powered at nine): the health lost
            by armour and difficulty, and the fuse.
  despawn   pens of non-persistent zombies at 16, 48, 96 and 140 blocks from a
            creative probe (and cows, villagers, a named zombie, a
            PersistenceRequired zombie at 140): counted every five seconds.
  anvil     a zoo of mobs with species NBT, saved by vanilla and kept under
            .scratch/mobs3-anvil-vanilla/ for the Anvil round trip.

Nothing is written outside the worktree: raw readings go to
.scratch/mobs3.json (not tracked). docs/provenance/mobs-3.md has the results.

    lockf /tmp/ov-vanilla.lock python3 scripts/measure_mobs3.py [campaign ...]
"""
from __future__ import annotations

import json
import re
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from measure_mobs import FlatServer, KeptAlive, count  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / ".scratch" / "mobs3-oracle"
OUT = ROOT / ".scratch" / "mobs3.json"
ANVIL_COPY = ROOT / ".scratch" / "mobs3-anvil-vanilla"
PORT = 25653
Y = -60
PROBE = "ovprobe"

GAMETIME = re.compile(r"The time is (\d+)")
DATA = re.compile(r"has the following entity data: (.+)$")
EFFECT = re.compile(r"\{[^{}]*\}")
EFFECT_ID = re.compile(r"\bId: (-?\d+)")
EFFECT_DURATION = re.compile(r"\bDuration: (-?\d+)")
EFFECT_AMPLIFIER = re.compile(r"\bAmplifier: (-?\d+)b")

DIFFICULTIES = ["easy", "normal", "hard"]
ARMOUR = {
    "none": [],
    "leather": ["leather_helmet", "leather_chestplate", "leather_leggings", "leather_boots"],
    "iron": ["iron_helmet", "iron_chestplate", "iron_leggings", "iron_boots"],
    "diamond": ["diamond_helmet", "diamond_chestplate", "diamond_leggings", "diamond_boots"],
    # Iron, and Protection IV on the chestplate only: EPF 4.
    "iron_prot4": ["iron_helmet", "iron_chestplate{Enchantments:[{id:\"minecraft:protection\",lvl:4s}]}",
                   "iron_leggings", "iron_boots"],
}
SLOTS = ["armor.head", "armor.chest", "armor.legs", "armor.feet"]
PROVOKED = {"enderman", "zombified_piglin"}


class Oracle(FlatServer):
    EXTRA_PROPERTIES = FlatServer.EXTRA_PROPERTIES + "difficulty=normal\n"
    HEAP = "-Xmx1200M"


def value(lines: list[str]) -> str | None:
    for line in lines:
        m = DATA.search(line)
        if m:
            return m.group(1).strip()
    return None


def health(server) -> tuple[int | None, float | None]:
    lines = server.batch(["time query gametime", f"data get entity {PROBE} Health"], timeout=30)
    tick = None
    hp = None
    for line in lines:
        m = GAMETIME.search(line)
        if m:
            tick = int(m.group(1))
    raw = value(lines)
    if raw is not None:
        hp = float(raw.rstrip("f"))
    return tick, hp


def effects(server) -> tuple[int | None, list[dict]]:
    lines = server.batch(["time query gametime", f"data get entity {PROBE} ActiveEffects"],
                         timeout=30)
    tick = None
    for line in lines:
        m = GAMETIME.search(line)
        if m:
            tick = int(m.group(1))
    out = []
    raw = value(lines) or ""
    for compound in EFFECT.findall(raw):
        i = EFFECT_ID.search(compound)
        d = EFFECT_DURATION.search(compound)
        a = EFFECT_AMPLIFIER.search(compound)
        if i and d:
            out.append({"id": int(i.group(1)), "duration": int(d.group(1)),
                        "amplifier": int(a.group(1)) if a else 0})
    return tick, out


def setup(server) -> None:
    server.batch(["gamerule doMobSpawning false", "gamerule naturalRegeneration false",
                  "gamerule doDaylightCycle false", "gamerule doWeatherCycle false",
                  "gamerule doImmediateRespawn true", "gamerule sendCommandFeedback true",
                  "gamerule showDeathMessages false",
                  "scoreboard objectives add ovcount dummy",
                  "time set midnight", "weather clear"], timeout=60)


def heal(server) -> None:
    server.batch([f"effect clear {PROBE}",
                  f"effect give {PROBE} minecraft:instant_health 1 5 true",
                  f"effect give {PROBE} minecraft:saturation 1 9 true"], timeout=30)
    time.sleep(0.3)
    server.batch([f"effect clear {PROBE}"], timeout=30)


def dress(server, armour: str) -> None:
    commands = [f"clear {PROBE}"]
    for slot, item in zip(SLOTS, ARMOUR[armour]):
        commands.append(f"item replace entity {PROBE} {slot} with minecraft:{item}")
    server.batch(commands, timeout=30)


def fresh(server, difficulty: str, mode: str = "survival") -> None:
    server.batch(["kill @e[type=!minecraft:player]", f"difficulty {difficulty}",
                  f"gamemode {mode} {PROBE}", f"tp {PROBE} 0.5 {Y} 0.5 -90 0"], timeout=60)
    heal(server)


# ── melee ───────────────────────────────────────────────────────────────────

def melee_one(server, mob: str, difficulty: str, armour: str, hits_wanted: int = 5,
              limit: float = 25.0) -> dict:
    fresh(server, difficulty)
    dress(server, armour)
    heal(server)
    server.batch([f"summon minecraft:{mob} 2.0 {Y} 0.5 "
                  "{PersistenceRequired:1b,Silent:1b,Tags:[\"ovm\"]}"], timeout=30)
    if mob in PROVOKED:
        server.batch([f"damage @e[tag=ovm,limit=1] 0.5 minecraft:player_attack by {PROBE}"])
    start_tick, last = health(server)
    samples = [[start_tick, last]]
    hits = []
    effect_reads = []
    begin = time.monotonic()
    while len(hits) < hits_wanted and time.monotonic() - begin < limit:
        tick, hp = health(server)
        if tick is None or hp is None:
            continue
        samples.append([tick, hp])
        if last is not None and hp < last - 1e-4:
            hits.append({"tick": tick, "lost": round(last - hp, 5), "before": last})
            if mob in ("husk", "cave_spider") and len(hits) == 1:
                effect_reads.append({"hit_tick": tick, "read": effects(server)})
        last = hp
        if hp is not None and hp < 9.0:
            heal(server)
            _, last = health(server)
    server.batch(["kill @e[tag=ovm]"], timeout=30)
    return {"mob": mob, "difficulty": difficulty, "armour": armour, "start": start_tick,
            "hits": hits, "effects": effect_reads, "samples": len(samples)}


def campaign_melee(server) -> list[dict]:
    out = []
    for difficulty in DIFFICULTIES:
        for armour in ARMOUR:
            print(f"   melee zombie {difficulty} {armour}", flush=True)
            out.append(melee_one(server, "zombie", difficulty, armour))
        for mob in ("husk", "drowned", "spider", "cave_spider"):
            print(f"   melee {mob} {difficulty}", flush=True)
            out.append(melee_one(server, mob, difficulty, "none", hits_wanted=3))
    for mob in ("enderman", "zombified_piglin"):
        print(f"   melee {mob} normal", flush=True)
        out.append(melee_one(server, mob, "normal", "iron", hits_wanted=3))
    return out


# ── ranged ──────────────────────────────────────────────────────────────────

def ranged_one(server, mob: str, difficulty: str, distance: float, seconds: float) -> dict:
    fresh(server, difficulty)
    dress(server, "diamond")
    heal(server)
    # Without NBT, so finalizeSpawn hands the bow over; made persistent after.
    server.batch([f"summon minecraft:{mob} {0.5 + distance} {Y} 0.5",
                  f"data merge entity @e[type=minecraft:{mob},limit=1] "
                  "{PersistenceRequired:1b,Silent:1b}"], timeout=30)
    _, last = health(server)
    hits = []
    slowness = []
    begin = time.monotonic()
    while time.monotonic() - begin < seconds:
        tick, hp = health(server)
        if tick is None or hp is None:
            continue
        if last is not None and hp < last - 1e-4:
            hits.append({"tick": tick, "lost": round(last - hp, 5)})
            if mob == "stray" and len(slowness) < 3:
                slowness.append({"hit_tick": tick, "read": effects(server)})
        last = hp
        if hp < 9.0:
            heal(server)
            _, last = health(server)
        server.send(f"tp {PROBE} 0.5 {Y} 0.5 -90 0")
    ground = count(server, {"arrows": "@e[type=minecraft:arrow]"})["arrows"]
    return {"mob": mob, "difficulty": difficulty, "distance": distance, "seconds": seconds,
            "hits": hits, "arrows_left": ground, "slowness": slowness}


def campaign_ranged(server) -> list[dict]:
    out = []
    for difficulty in DIFFICULTIES:
        for mob in ("skeleton", "stray"):
            for distance in (8.0, 14.0):
                print(f"   ranged {mob} {difficulty} {distance}", flush=True)
                out.append(ranged_one(server, mob, difficulty, distance, 45.0))
    return out


# ── creeper ─────────────────────────────────────────────────────────────────

def creeper_one(server, difficulty: str, armour: str, distance: float, powered: bool) -> dict:
    fresh(server, difficulty)
    dress(server, armour)
    heal(server)
    _, before = health(server)
    # NoAI: an ignited creeper with its AI walks in while it swells, and the
    # first run lost the probe twice that way. The fuse is not AI: it still
    # counts down.
    nbt = "{ignited:1b,NoAI:1b,Silent:1b,Tags:[\"ovc\"]" + (",powered:1b}" if powered else "}")
    lines = server.batch(["time query gametime",
                          f"summon minecraft:creeper {0.5 + distance} {Y} 0.5 {nbt}"])
    lit = None
    for line in lines:
        m = GAMETIME.search(line)
        if m:
            lit = int(m.group(1))
    gone = None
    after = None
    begin = time.monotonic()
    while time.monotonic() - begin < 6.0:
        n = count(server, {"c": "@e[tag=ovc]"})["c"]
        tick, hp = health(server)
        if n == 0 and gone is None:
            gone = tick
        if gone is not None:
            time.sleep(0.3)
            _, after = health(server)
            break
    return {"difficulty": difficulty, "armour": armour, "distance": distance,
            "powered": powered, "lit": lit, "gone": gone, "before": before, "after": after}


def campaign_creeper(server) -> list[dict]:
    out = []
    for difficulty in DIFFICULTIES:
        for armour in ("none", "iron", "diamond"):
            print(f"   creeper {difficulty} {armour}", flush=True)
            out.append(creeper_one(server, difficulty, armour, 4.0, False))
    for armour in ("none", "diamond"):
        print(f"   creeper powered normal {armour}", flush=True)
        out.append(creeper_one(server, "normal", armour, 9.0, True))
    return out


# ── despawn ─────────────────────────────────────────────────────────────────

PENS = [16, 48, 96, 140]


def pen(server, x: int, z: int) -> None:
    server.batch([f"fill {x - 4} {Y} {z - 4} {x + 4} {Y + 3} {z + 4} minecraft:glass hollow"],
                 timeout=30)


def campaign_despawn(server) -> dict:
    fresh(server, "normal", "creative")
    server.batch([f"tp {PROBE} 0.5 {Y} 0.5"])
    groups = {}
    for d in PENS:
        pen(server, d, 0)
        groups[f"zombie_{d}"] = f"@e[type=minecraft:zombie,tag=d{d}]"
        cmds = []
        for i in range(16):
            cmds.append(f"summon minecraft:zombie {d - 2.5 + (i % 4) * 1.6:.1f} {Y + 1} "
                        f"{-2.5 + (i // 4) * 1.6:.1f} "
                        f"{{Tags:[\"d{d}\"]}}")
        server.batch(cmds, timeout=60)
    # Cows and villagers at 48 and 140, one named and one persistent zombie at 140.
    for d in (48, 140):
        pen(server, d, 20)
        cmds = [f"summon minecraft:cow {d - 2 + i:.1f} {Y + 1} 19.5 {{Tags:[\"c{d}\"]}}"
                for i in range(4)]
        cmds += [f"summon minecraft:villager {d - 2 + i:.1f} {Y + 1} 21.5 {{Tags:[\"v{d}\"]}}"
                 for i in range(2)]
        server.batch(cmds, timeout=60)
        groups[f"cow_{d}"] = f"@e[type=minecraft:cow,tag=c{d}]"
        groups[f"villager_{d}"] = f"@e[type=minecraft:villager,tag=v{d}]"
    pen(server, 140, -20)
    server.batch([f"summon minecraft:zombie 139.5 {Y + 1} -20.5 "
                  "{Tags:[\"named\"],CustomName:'\"Bob\"'}",
                  f"summon minecraft:zombie 141.5 {Y + 1} -20.5 "
                  "{Tags:[\"kept\"],PersistenceRequired:1b}"], timeout=30)
    groups["named_140"] = "@e[tag=named]"
    groups["persistent_140"] = "@e[tag=kept]"
    # Note: a summon with any NBT still leaves PersistenceRequired at 0 unless
    # given; Tags alone do not pin a mob.
    series = []
    begin = time.monotonic()
    while time.monotonic() - begin < 170.0:
        # The probe's own position every sample: the first run counted sixteen
        # zombies at 140 blocks for three minutes, which is what a server with
        # no player near anything reads.
        tick_lines = server.batch(["time query gametime", f"data get entity {PROBE} Pos",
                                   "list"])
        tick = None
        for line in tick_lines:
            m = GAMETIME.search(line)
            if m:
                tick = int(m.group(1))
        series.append({"tick": tick, "probe": value(tick_lines),
                       "list": [l for l in tick_lines if "players online" in l],
                       "counts": count(server, groups)})
        time.sleep(4.0)
    return {"groups": groups, "series": series}


# ── anvil ───────────────────────────────────────────────────────────────────

ZOO = [
    ("zombie", '{IsBaby:1b}'),
    ("zombie", "{CustomName:'\"Bob\"'}"),
    ("husk", "{}"),
    ("drowned", "{}"),
    ("skeleton", "{}"),
    ("stray", "{}"),
    ("creeper", "{powered:1b,Fuse:40s}"),
    ("spider", "{}"),
    ("cave_spider", "{}"),
    ("witch", "{}"),
    ("enderman", "{}"),
    ("slime", "{Size:1}"),
    ("cow", "{Age:-24000}"),
    ("cow", "{InLove:600}"),
    ("pig", "{Saddle:1b}"),
    ("sheep", "{Color:3b,Sheared:1b}"),
    ("chicken", "{EggLayTime:1234}"),
    ("villager", "{VillagerData:{type:\"minecraft:desert\",profession:\"minecraft:librarian\","
                 "level:3},Xp:20}"),
    ("zombie_villager", "{VillagerData:{type:\"minecraft:taiga\",profession:"
                        "\"minecraft:farmer\",level:2},Xp:12}"),
    ("wolf", "{Owner:[I;1,2,3,4]}"),
    ("cat", "{Owner:[I;1,2,3,4]}"),
    ("rabbit", "{}"),
    ("fox", "{}"),
    ("horse", "{}"),
]


def campaign_anvil(server) -> dict:
    fresh(server, "normal", "creative")
    cmds = []
    for i, (mob, nbt) in enumerate(ZOO):
        extra = nbt[1:-1]
        merged = "{NoAI:1b,PersistenceRequired:1b,Silent:1b,Tags:[\"zoo\"]" + \
                 ("," + extra if extra else "") + "}"
        cmds.append(f"summon minecraft:{mob} {4.5 + (i % 6) * 3} {Y} {4.5 + (i // 6) * 3} {merged}")
    server.batch(cmds, timeout=60)
    time.sleep(2.0)
    dump = {}
    for i, (mob, _) in enumerate(ZOO):
        x = 4.5 + (i % 6) * 3
        z = 4.5 + (i // 6) * 3
        lines = server.batch([f"data get entity @e[tag=zoo,limit=1,sort=nearest,x={x},y={Y},"
                              f"z={z}]"], timeout=30)
        dump[f"{i}:{mob}"] = value(lines)
    server.batch(["save-all flush"], timeout=120)
    time.sleep(2.0)
    # Copied now, before any other campaign fills the world: the first copy was
    # taken at the end of the run and carried the despawn pens along.
    if ANVIL_COPY.exists():
        shutil.rmtree(ANVIL_COPY)
    shutil.copytree(RUN / "world", ANVIL_COPY)
    return {"zoo": [m for m, _ in ZOO], "data_get": dump}


OURS_ZOO = ROOT / ".scratch" / "mobs3-anvil-ours"


def campaign_anvil_back(server) -> dict:
    """Vanilla reads the zoo back from the world **our** server rewrote.

    `check_mobs3_e2e.py anvil` loads the vanilla zoo into ov_dedicated, which
    saves it through its own entities/ writer; this campaign is then run on
    that world (see `main`), and every zoo mob is read with `data get`.
    """
    fresh_probe_spot = [f"tp {PROBE} 0.5 {Y} 0.5"]
    server.batch(fresh_probe_spot, timeout=30)
    time.sleep(3.0)
    dump = {}
    for i, (mob, _) in enumerate(ZOO):
        x = 4.5 + (i % 6) * 3
        z = 4.5 + (i // 6) * 3
        lines = server.batch([f"data get entity @e[tag=zoo,limit=1,sort=nearest,x={x},y={Y},"
                              f"z={z},distance=..1.5]"], timeout=30)
        dump[f"{i}:{mob}"] = value(lines)
    counted = count(server, {"zoo": "@e[tag=zoo]"})["zoo"]
    return {"zoo_count": counted, "data_get": dump}


CAMPAIGNS = {"melee": campaign_melee, "ranged": campaign_ranged, "creeper": campaign_creeper,
             "despawn": campaign_despawn, "anvil": campaign_anvil,
             "anvil_back": campaign_anvil_back}


def main(argv: list[str]) -> int:
    wanted = argv or list(CAMPAIGNS)
    result = json.loads(OUT.read_text()) if OUT.exists() else {}
    if RUN.exists():
        shutil.rmtree(RUN)
    if wanted == ["anvil_back"]:
        # Vanilla on the world ov_dedicated rewrote: copied in as `world`.
        if not OURS_ZOO.exists():
            print(f"no {OURS_ZOO}: run check_mobs3_e2e.py anvil first")
            return 1
        RUN.mkdir(parents=True)
        shutil.copytree(OURS_ZOO, RUN / "world")
        for stale in ("session.lock",):
            (RUN / "world" / stale).unlink(missing_ok=True)
    server = Oracle(RUN, PORT)
    probe = None
    try:
        setup(server)
        probe = KeptAlive(PORT, PROBE)
        time.sleep(3.0)
        for name in wanted:
            print(f"== {name}", flush=True)
            started = time.monotonic()
            result[name] = CAMPAIGNS[name](server)
            print(f"   {name}: {time.monotonic() - started:.0f} s", flush=True)
            OUT.write_text(json.dumps(result, indent=1))
    finally:
        if probe is not None:
            probe.close()
        server.stop()
    shutil.rmtree(RUN, ignore_errors=True)
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
