#!/usr/bin/env python3
"""Ask a real 1.20.1 server how fast each mob walks, goal by goal.

The question this answers: `docs/provenance/mobs.md` turned the measured
`movement_speed` attribute into blocks per tick by halving it, which is right for
a chasing zombie and 16 % fast for a strolling cow (docs/provenance/elevage.md
§ 8). The husbandry wave fitted seven speeds to `v = 2.1586 · s²`, `s` being the
attribute times the goal's modifier. This rig measures the cruise speed of
every species we ship or add, under every goal we can provoke on a superflat of
bare grass, so that the modifiers are read off the game rather than assumed.

Campaigns:

  stroll   Fields of persistent mobs on bare grass, sampled for a hundred
           seconds. A strolling mob covers a few blocks now and then; the
           cruise speed is the top plateau of its per-tick steps.
  panic    The same passive fields, each mob hurt by the probe every four
           seconds (`/damage … by ovprobe` sets the last attacker, which is
           what makes an animal panic).
  chase    One hostile species at a time, four of them thirty blocks from a
           survival probe kept alive by Resistance V, `follow_range` raised
           to 64 so that the acquisition is not the question. Wolves and
           endermen are provoked first by a hit from the probe.
  ice      The zombie chase again, on packed ice and on blue ice. The law's
           dependence on the floor is the discriminating test of *why* it is
           quadratic — see docs/provenance/mobs-2.md.

Every mob is summoned with `CustomName:'"mNN"'`, so a console `data get` reply
names the mob it answers for, and `PersistenceRequired:1b`, so a mob far from
the probe keeps its idle timer at zero and its stroll goal alive (mobs.md § 0).

Usage: python3 scripts/measure_mobs2.py [stroll|panic|chase|ice|analyze ...]

Writes data/vanilla/1.20.1/normalized/mobs2_speed.json (gitignored).
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
from measure_mobs import FlatServer, KeptAlive  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
OUT = NORMALIZED / "mobs2_speed.json"
# Under the worktree's own scratch, not `run/`: in an agent worktree `run/` is a
# link into another checkout, and a measurement writes nothing outside its own.
RUN = ROOT / ".scratch" / "mobs2-oracle"
# Ports of this rig's own. 25631 turned out to be another rig's, and a server
# that cannot bind waits for "Done (" until the timeout.
PORT = 25641
WORLD_PORT = 25637
Y = -60

GAMETIME = re.compile(r"The time is (\d+)")
NAMED = re.compile(r"(m\d+) has the following entity data: \[([-0-9.Ee]+)d?, ([-0-9.Ee]+)d?, "
                   r"([-0-9.Ee]+)d?\]")
# "Value of attribute Movement Speed for m3 is 0.2" — the attribute's display
# name has spaces, which is why the first version of this pattern read nothing.
ATTR = re.compile(r"attribute .+ for (m\d+) is ([-0-9.Ee]+)")

# The law the husbandry wave fitted, and what it predicts for a speed `s`.
LAW = 2.1586

PASSIVE_FIELD = ["cow", "pig", "sheep", "chicken", "rabbit", "horse"]
HOSTILE_FIELD = ["zombie", "husk", "drowned", "skeleton", "stray", "creeper", "spider",
                 "cave_spider", "witch", "enderman"]
ALONE_FIELDS = [["wolf", "cat"], ["fox"]]
CHASERS = ["zombie", "husk", "drowned", "skeleton", "stray", "creeper", "spider",
           "cave_spider", "witch", "wolf", "enderman"]
PROVOKED = {"wolf", "enderman"}


class Field:
    """Named, persistent mobs and their traces."""

    def __init__(self, server) -> None:
        self.server = server
        self.mobs: dict[str, dict] = {}
        self.next = 0

    def summon(self, kind: str, x: float, z: float, extra: str = "", y: float = Y) -> str:
        self.next += 1
        name = f"m{self.next}"
        nbt = f"CustomName:'\"{name}\"',PersistenceRequired:1b,Silent:1b"
        if extra:
            nbt += "," + extra
        self.server.batch([f"summon minecraft:{kind} {x:.2f} {y} {z:.2f} {{{nbt}}}"])
        self.mobs[name] = {"type": kind, "trace": [], "attribute": None}
        return name

    def read_attributes(self) -> None:
        commands = [f"attribute @e[name={n},limit=1] minecraft:generic.movement_speed get"
                    for n in self.mobs]
        for line in self.server.batch(commands, timeout=120):
            m = ATTR.search(line)
            if m and m.group(1) in self.mobs:
                self.mobs[m.group(1)]["attribute"] = float(m.group(2))

    def sample(self, names: list[str] | None = None) -> int | None:
        names = names if names is not None else list(self.mobs)
        commands = ["time query gametime"]
        commands += [f"data get entity @e[name={n},limit=1] Pos" for n in names]
        lines = self.server.batch(commands, timeout=60)
        tick = None
        for line in lines:
            m = GAMETIME.search(line)
            if m:
                tick = int(m.group(1))
        if tick is None:
            return None
        for line in lines:
            m = NAMED.search(line)
            if m and m.group(1) in self.mobs:
                self.mobs[m.group(1)]["trace"].append(
                    [tick, float(m.group(2)), float(m.group(3)), float(m.group(4))])
        return tick

    def run(self, seconds: float, every=None, names: list[str] | None = None) -> None:
        start = time.monotonic()
        last_hook = -1e9
        while time.monotonic() - start < seconds:
            if every is not None and time.monotonic() - last_hook > every[0]:
                every[1]()
                last_hook = time.monotonic()
            self.sample(names)

    def dump(self) -> list[dict]:
        return [{"name": n, **v} for n, v in self.mobs.items()]


def grid(kinds: list[str], per: int, spacing: float, origin=(0.0, 0.0)):
    cells = []
    for kind in kinds:
        for _ in range(per):
            cells.append(kind)
    side = math.ceil(math.sqrt(len(cells)))
    out = []
    for i, kind in enumerate(cells):
        x = origin[0] + (i % side - side / 2) * spacing + 0.5
        z = origin[1] + (i // side - side / 2) * spacing + 0.5
        out.append((kind, x, z))
    return out


def reset(server, probe_mode: str = "creative") -> None:
    server.batch(["kill @e[type=!minecraft:player]",
                  f"gamemode {probe_mode} ovprobe",
                  "tp ovprobe -40 -60 0.5",
                  "effect give ovprobe minecraft:resistance 999999 4 true",
                  "effect give ovprobe minecraft:regeneration 999999 4 true",
                  "time set midnight"], timeout=120)
    time.sleep(1.0)


def stroll_field(server, kinds: list[str], per: int, seconds: float, panic: bool) -> list[dict]:
    reset(server)
    field = Field(server)
    for kind, x, z in grid(kinds, per, 8.0):
        field.summon(kind, x, z)
    field.read_attributes()
    time.sleep(1.0)
    hook = None
    if panic:
        def hurt() -> None:
            server.batch([f"damage @e[name={n},limit=1] 1 minecraft:player_attack by ovprobe"
                          for n in field.mobs] +
                         [f"effect give @e[name={n},limit=1] minecraft:instant_health 1 0 true"
                          for n in field.mobs], timeout=60)
        hook = (4.0, hurt)
    field.run(seconds, hook)
    return field.dump()


def campaign_stroll(server) -> dict:
    out = {"passive": stroll_field(server, PASSIVE_FIELD, 5, 110, False)}
    out["hostile"] = stroll_field(server, HOSTILE_FIELD, 4, 110, False)
    for kinds in ALONE_FIELDS:
        out["_".join(kinds)] = stroll_field(server, kinds, 5, 100, False)
    return out


def campaign_panic(server) -> dict:
    out = {"passive": stroll_field(server, PASSIVE_FIELD, 4, 60, True)}
    out["cat_fox"] = stroll_field(server, ["cat", "fox"], 4, 60, True)
    return out


def chase_one(server, kind: str, floor: str | None = None) -> list[dict]:
    reset(server, "survival")
    server.batch(["tp ovprobe 0.5 -60 0.5"])
    if floor is not None:
        server.batch([f"fill -4 -61 -10 36 -61 10 {floor}"], timeout=60)
    field = Field(server)
    for z in (-6, -2, 2, 6):
        name = field.summon(kind, 30.5, z + 0.5)
        server.batch([f"attribute @e[name={name},limit=1] minecraft:generic.follow_range base "
                      "set 64"])
    field.read_attributes()
    if kind in PROVOKED:
        server.batch([f"damage @e[name={n},limit=1] 1 minecraft:player_attack by ovprobe"
                      for n in field.mobs])
    start = time.monotonic()
    while time.monotonic() - start < 25.0:
        field.sample()
        server.send("tp ovprobe 0.5 -60 0.5")
        near = 0
        for mob in field.mobs.values():
            if mob["trace"] and abs(mob["trace"][-1][1]) < 12.0:
                near += 1
        if near == len(field.mobs):
            break
    if floor is not None:
        server.batch(["fill -4 -61 -10 36 -61 10 minecraft:grass_block"], timeout=60)
    return field.dump()


def campaign_chase(server) -> dict:
    out = {}
    for kind in CHASERS:
        print(f"   chase {kind}", flush=True)
        out[kind] = chase_one(server, kind)
    return out


def campaign_ice(server) -> dict:
    out = {}
    for floor in ("minecraft:packed_ice", "minecraft:blue_ice", "minecraft:grass_block"):
        print(f"   ice {floor}", flush=True)
        out[floor] = chase_one(server, "zombie", floor)
    return out


# ── Campaign: who spawns where ──────────────────────────────────────────────
#
# A real generated world at the reference seed. For each biome: `locate biome`,
# then the candidate centre whose ring of 24..128 blocks is most purely that
# biome (`execute if biome` on 64 points), the probe placed there on the
# surface (`spreadplayers`) in creative — a creative player counts for
# spawning, a spectator does not — then everything killed, midnight, and every
# Spawn Entity packet that follows for four minutes, with the biome of its
# position tested one by one. Creatures that were already there when the chunks
# generated are recorded separately: that is chunk-generation population, which
# this server does not do.

BIOMES = ["plains", "desert", "snowy_plains", "snowy_taiga", "swamp"]
LOCATED = re.compile(r"is at \[(-?\d+), [^,]+, (-?\d+)\]")
TEST = re.compile(r"Test (passed|failed)")
POS_ANY = re.compile(r"\[([-0-9.Ee]+)d, ([-0-9.Ee]+)d, ([-0-9.Ee]+)d\]")
SPAWN_SECONDS = 240.0


class WorldServer(FlatServer):
    EXTRA_PROPERTIES = FlatServer.EXTRA_PROPERTIES + (
        "level-type=minecraft\\:normal\n"
        "level-seed=1234567890\n"
        "view-distance=8\n"
        "simulation-distance=8\n"
    )


def entity_names() -> list[str]:
    reg = json.loads((NORMALIZED / "registries.json").read_text())
    return reg["registries"]["minecraft:entity_type"]["entries"]


def tests(server, commands: list[str]) -> list[bool | None]:
    """One answer per command, in order: passed, failed, or None.

    A marker before each command, because `execute if biome` on a position
    that is not loaded prints an error instead of a verdict — and a list that
    only collects verdicts then silently shifts every answer after it onto the
    wrong question. The first version of this rig read a purity of 0.00 for
    plains that way.
    """
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


def purity(server, biome: str, x: int, z: int) -> float:
    commands = []
    for radius in (32, 64, 96, 120):
        for k in range(16):
            a = 2 * math.pi * k / 16
            commands.append(f"execute if biome {int(x + radius * math.cos(a))} 70 "
                            f"{int(z + radius * math.sin(a))} minecraft:{biome}")
    got = [g for g in tests(server, commands) if g is not None]
    return sum(got) / len(got) if got else 0.0


def campaign_biomes(server) -> dict:
    from measure_husbandry import Hand  # noqa: E402 — a probe that records spawns

    names = entity_names()
    out: dict = {}
    hand = Hand(WORLD_PORT, "ovbiome")
    try:
        time.sleep(2.0)
        server.batch(["gamemode creative ovbiome", "gamerule doMobSpawning false",
                      "gamerule doDaylightCycle false"], timeout=60)
        for biome in BIOMES:
            lines = server.batch([f"locate biome minecraft:{biome}"], timeout=600)
            found = next((LOCATED.search(l) for l in lines if LOCATED.search(l)), None)
            if found is None:
                out[biome] = {"error": "not located", "lines": lines[-3:]}
                continue
            cx, cz = int(found.group(1)), int(found.group(2))
            with hand.state_lock:
                hand.spawns.clear()
            server.batch([f"spreadplayers {cx} {cz} 0 1 false ovbiome"], timeout=600)
            time.sleep(25.0)
            # Purity only now: `execute if biome` answers loaded positions only.
            p = purity(server, biome, cx, cz)
            print(f"   {biome}: centre {cx} {cz}, ring purity {p:.2f}", flush=True)
            with hand.state_lock:
                generated = [names[s["type"]] for s in hand.spawns.values()]
            lines = server.batch(["data get entity ovbiome Pos"], timeout=60)
            probe = next((POS_ANY.search(l) for l in lines if POS_ANY.search(l)), None)
            px, py, pz = (float(probe.group(1)), float(probe.group(2)), float(probe.group(3))) \
                if probe else (cx + 0.5, 70.0, cz + 0.5)
            server.batch(["kill @e[type=!minecraft:player]", "time set midnight"], timeout=60)
            time.sleep(1.0)
            with hand.state_lock:
                hand.spawns.clear()
            server.batch(["gamerule doMobSpawning true"], timeout=60)
            time.sleep(SPAWN_SECONDS)
            server.batch(["gamerule doMobSpawning false"], timeout=60)
            with hand.state_lock:
                natural = [(names[s["type"]], s["pos"]) for s in hand.spawns.values()]
            natural = [(n, pos) for n, pos in natural
                       if n not in ("minecraft:item", "minecraft:experience_orb",
                                    "minecraft:arrow", "minecraft:player")]
            inside = tests(server, [f"execute if biome {int(math.floor(pos[0]))} "
                                    f"{int(math.floor(pos[1]))} {int(math.floor(pos[2]))} "
                                    f"minecraft:{biome}" for _, pos in natural])
            counts: dict[str, int] = {}
            for (n, _), ok in zip(natural, inside):
                if ok:
                    counts[n] = counts.get(n, 0) + 1
            gen_counts: dict[str, int] = {}
            for n in generated:
                gen_counts[n] = gen_counts.get(n, 0) + 1
            out[biome] = {"centre": [cx, cz], "probe": [px, py, pz], "purity": p,
                          "natural_in_biome": counts, "natural_total": len(natural),
                          "generated": gen_counts,
                          "spawns": [[n, list(pos)] for n, pos in natural]}
            print(f"      natural in biome: {counts}", flush=True)
            print(f"      at generation: {gen_counts}", flush=True)
            server.batch(["kill @e[type=!minecraft:player]"], timeout=60)
    finally:
        hand.close()
    return out


# ── Analysis ────────────────────────────────────────────────────────────────

def steps_of(trace: list[list[float]], x_limit: float | None = None) -> list[float]:
    out = []
    for a, b in zip(trace, trace[1:]):
        dt = b[0] - a[0]
        if dt <= 0 or dt > 4:
            continue
        if x_limit is not None and (b[1] < x_limit):
            continue
        out.append(math.hypot(b[1] - a[1], b[3] - a[3]) / dt)
    return out


def plateau(steps: list[float]) -> dict:
    moving = sorted(s for s in steps if s > 0.02)
    if len(moving) < 5:
        return {"n": len(moving)}
    q = lambda f: moving[min(len(moving) - 1, int(f * len(moving)))]  # noqa: E731
    # The cruise: the median of the steps within 4 % of the 90th percentile.
    # A stroll starts, turns and stops, and all three are slower than the
    # cruise; the top of the distribution is where the mob is going straight.
    top = q(0.9)
    band = [s for s in moving if abs(s - top) <= 0.04 * top]
    cruise = band[len(band) // 2] if band else top
    return {"n": len(moving), "p50": q(0.5), "p75": q(0.75), "p90": top, "p97": q(0.97),
            "cruise": cruise}


def analyze(result: dict) -> dict:
    table: dict = {}
    for campaign in ("stroll", "panic"):
        for group in result.get(campaign, {}).values():
            by_type: dict[str, list[float]] = {}
            attrs: dict[str, float] = {}
            for mob in group:
                by_type.setdefault(mob["type"], []).extend(steps_of(mob["trace"]))
                if mob.get("attribute"):
                    attrs[mob["type"]] = mob["attribute"]
            for kind, steps in by_type.items():
                row = plateau(steps)
                row["attribute"] = attrs.get(kind)
                table.setdefault(kind, {})[campaign] = row
    for key, campaign in (("chase", "chase"), ("ice", "ice")):
        for kind, group in result.get(key, {}).items():
            steps = []
            attr = None
            for mob in group:
                steps.extend(steps_of(mob["trace"], x_limit=16.0))
                attr = mob.get("attribute") or attr
            row = plateau(steps)
            row["attribute"] = attr
            label = kind if key == "chase" else f"zombie@{kind.split(':')[1]}"
            table.setdefault(label, {})[campaign] = row
    for kind, rows in table.items():
        for campaign, row in rows.items():
            if "cruise" in row and row.get("attribute"):
                s = math.sqrt(row["cruise"] / LAW)
                row["implied_modifier"] = round(s / row["attribute"], 4)
    return table


def print_table(table: dict) -> None:
    for kind in sorted(table):
        for campaign, row in table[kind].items():
            if "cruise" not in row:
                print(f"  {kind:12} {campaign:6} n={row['n']}")
                continue
            print(f"  {kind:12} {campaign:6} n={row['n']:5} attr={row['attribute']} "
                  f"p50={row['p50']:.4f} p90={row['p90']:.4f} p97={row['p97']:.4f} "
                  f"cruise={row['cruise']:.5f} mod={row.get('implied_modifier')}")


SIZE_TAG = re.compile(r"has the following entity data: (-?\d+)")


def slime_count(server) -> int:
    lines = server.batch(["scoreboard players set ovs ovcount 0",
                          "execute store result score ovs ovcount if entity "
                          "@e[type=minecraft:slime]",
                          "scoreboard players get ovs ovcount"])
    for line in lines:
        m = re.search(r"has (-?\d+) \[", line)
        if m:
            return int(m.group(1))
    return -1


def campaign_slime(server) -> dict:
    """A slime killed splits: how many children, and how big.

    Summoned with an explicit `Size` (the tag is size − 1), killed by /kill,
    and counted once the corpse has been removed — the split happens then,
    after the twenty-tick death animation, not on the killing blow.
    """
    reset(server)
    server.batch(["scoreboard objectives add ovcount dummy"])
    out: dict = {}
    for size_tag, trials in ((1, 45), (3, 30)):
        children: list[int] = []
        sizes: list[int] = []
        for _ in range(trials):
            for _ in range(4):
                if slime_count(server) == 0:
                    break
                server.batch(["kill @e[type=minecraft:slime]"])
                time.sleep(1.6)
            server.batch([f"summon minecraft:slime 0.5 -60 0.5 "
                          f"{{Size:{size_tag},Tags:[\"big\"],PersistenceRequired:1b}}"])
            time.sleep(0.2)
            server.batch(["kill @e[tag=big]"])
            time.sleep(1.8)
            children.append(slime_count(server))
            for line in server.batch(["data get entity @e[type=minecraft:slime,limit=1] Size"]):
                m = SIZE_TAG.search(line)
                if m:
                    sizes.append(int(m.group(1)))
        out[f"size_tag_{size_tag}"] = {"children": children, "child_size_tags": sizes}
        hist = {k: children.count(k) for k in sorted(set(children))}
        print(f"   slime Size:{size_tag}: children {hist}, child Size tags {sorted(set(sizes))}",
              flush=True)
    server.batch(["kill @e[type=!minecraft:player]"])
    return out


def campaign_drown(server) -> dict:
    """How long a zombie (and a husk) takes to become a drowned (a zombie).

    Each mob alone in a 1×1 shaft of water three deep under a glass lid, so its
    eyes can never leave the water; the pits are polled together, and the
    verdict is the gametime at which the pit first holds the converted type.
    """
    reset(server)
    pits = []
    for index, kind in enumerate(["zombie"] * 8 + ["husk"] * 4):
        x, z = index * 4 - 24, 12
        server.batch([f"fill {x - 1} -61 {z - 1} {x + 1} -56 {z + 1} minecraft:stone",
                      f"fill {x} -60 {z} {x} -58 {z} minecraft:water",
                      f"setblock {x} -57 {z} minecraft:glass"])
        pits.append((kind, x, z))
    lines = server.batch(["time query gametime"] +
                         [f"summon minecraft:{kind} {x + 0.5} -60 {z + 0.5} "
                          "{PersistenceRequired:1b}" for kind, x, z in pits])
    start = next(int(m.group(1)) for l in lines if (m := GAMETIME.search(l)))
    done: dict[int, int] = {}
    target = {"zombie": "drowned", "husk": "zombie"}
    deadline = time.monotonic() + 120.0
    while time.monotonic() < deadline and len(done) < len(pits):
        commands = ["time query gametime"]
        for i, (kind, x, z) in enumerate(pits):
            commands.append(f"scoreboard players set p{i} ovcount 0")
            commands.append(f"execute store result score p{i} ovcount if entity "
                            f"@e[type=minecraft:{target[kind]},x={x},y=-61,z={z},dx=0,dy=4,dz=0]")
            commands.append(f"scoreboard players get p{i} ovcount")
        lines = server.batch(commands)
        now = next((int(m.group(1)) for l in lines if (m := GAMETIME.search(l))), None)
        scores = [int(m.group(1)) for l in lines if (m := re.search(r"has (-?\d+) \[", l))]
        for i, score in enumerate(scores):
            if score > 0 and i not in done and now is not None:
                done[i] = now - start
        time.sleep(0.2)
    out = {"ticks": {f"{pits[i][0]}{i}": t for i, t in sorted(done.items())},
           "unconverted": [f"{pits[i][0]}{i}" for i in range(len(pits)) if i not in done]}
    print(f"   drown: {out}", flush=True)
    server.batch(["kill @e[type=!minecraft:player]"])
    return out


CAMPAIGNS = {"stroll": campaign_stroll, "panic": campaign_panic, "chase": campaign_chase,
             "ice": campaign_ice, "slime": campaign_slime, "drown": campaign_drown}
# Campaigns on a generated world at the reference seed, one server for all.
WORLD_CAMPAIGNS = {"biomes": campaign_biomes}


def run_flat(wanted: list[str], result: dict) -> None:
    if RUN.exists():
        shutil.rmtree(RUN)
    server = FlatServer(RUN, port=PORT)
    probe = None
    try:
        server.batch(["gamerule doDaylightCycle false", "gamerule doWeatherCycle false",
                      "gamerule doMobSpawning false", "gamerule mobGriefing false",
                      "gamerule randomTickSpeed 0", "difficulty hard",
                      "gamerule doImmediateRespawn true"], timeout=120)
        probe = KeptAlive(PORT)
        time.sleep(2.0)
        server.batch(["forceload add -48 -48 48 48"], timeout=120)
        for name in wanted:
            print(f"── campaign {name}", flush=True)
            result[name] = CAMPAIGNS[name](server)
            OUT.write_text(json.dumps(result) + "\n")
    finally:
        if probe is not None:
            probe.close()
        server.stop()
        shutil.rmtree(RUN, ignore_errors=True)


def run_world(wanted: list[str], result: dict) -> None:
    # The world is thrown away afterwards: five biomes' worth of generated
    # chunks, well under the 300 MB the briefing allows, and nothing to keep.
    run = RUN.with_name("mobs2-world-oracle")
    if run.exists():
        shutil.rmtree(run)
    server = WorldServer(run, port=WORLD_PORT)
    try:
        server.batch(["gamerule doWeatherCycle false", "gamerule doMobSpawning false",
                      "gamerule randomTickSpeed 0", "difficulty hard"], timeout=120)
        for name in wanted:
            print(f"── campaign {name}", flush=True)
            result[name] = WORLD_CAMPAIGNS[name](server)
            OUT.write_text(json.dumps(result) + "\n")
    finally:
        server.stop()
        shutil.rmtree(run, ignore_errors=True)


def main(argv: list[str]) -> int:
    names = argv[1:] or list(CAMPAIGNS)
    result = json.loads(OUT.read_text()) if OUT.exists() else {}
    flat = [n for n in names if n in CAMPAIGNS]
    world = [n for n in names if n in WORLD_CAMPAIGNS]
    if flat:
        run_flat(flat, result)
    if world:
        run_world(world, result)
    table = analyze(result)
    result["analysis"] = table
    OUT.write_text(json.dumps(result) + "\n")
    print_table(table)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
