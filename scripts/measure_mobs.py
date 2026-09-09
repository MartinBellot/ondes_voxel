#!/usr/bin/env python3
"""Ask a real 1.20.1 server how it populates a world.

Four campaigns, each answering one question this project refuses to take from a
wiki page:

  light       At which light level does a monster stop spawning, and does sky
              light have a say of its own? A row of 7x7 rooms in a **void**
              world — a superflat of one air layer — so the room floors are the
              only spawnable surface for a hundred blocks and every mob counted
              spawned somewhere whose light is known. Each room's floor is lit
              to one level by a full layer of `minecraft:light` two blocks
              above it, which emits without occluding and leaves the mob's own
              two blocks of body clear. Run three times: sealed at midnight,
              open to the sky at midnight, open at noon. Comparing the three is
              what separates "block light decides" from "the larger of block
              light and dimmed sky light decides".

  caps        How many mobs of each category does the server maintain around a
              single player? On an ordinary superflat, one player, spawning on,
              counted with a scoreboard — `execute store result score <name>
              <objective> if entity <selector>` stores the number of matches,
              which is the only way a console gets a count out of a selector.
              Sampled every ten seconds for four minutes so the plateau is
              visible and not one lucky reading.

  maze        Given a maze, which way does a mob go? A zombie at one end, a
              villager — NoAI, NoGravity, kept alive by health rather than by
              invulnerability — at the other, and nothing supplied from outside
              once the two exist. The zombie's Pos and the world's gametime are
              sampled together, so the trace carries the game's clock and not
              the measuring machine's. `test_pathfinding.cpp` replays the same
              maze through our A* and compares which gap the route takes at
              each wall.

  speed       How fast does a chasing mob actually move, against its measured
              movement_speed attribute? The same rig on an open floor, with the
              quarry near enough to be acquired.

  acquire     And how near is that? Written because the speed campaign ran into
              the question rather than around it: at sixty blocks apart the
              zombie never moved at all. Each separation gets its own arena and
              sixty ticks, and the verdict is displacement towards the quarry.

Usage: python3 scripts/measure_mobs.py [campaign ...]

Writes data/vanilla/1.20.1/normalized/mob_spawning.json. Campaign names are
`light`, `caps`, `maze`, `speed`, `acquire`; with none given, all of them run.

── Traps this rig pays for, each already worth a whole lost run ─────────────

  * **`Invulnerable:1b` makes an entity untargetable.** Vanilla will not accept
    one as a target at all, so the first version of the maze had a zombie
    standing three blocks from an invulnerable iron golem and ignoring it. Every
    workaround built on top of that — hurting the zombie to force a target,
    then hurting it every tick because the forced target is dropped without line
    of sight — was aimed at the wrong symptom. A quarry is kept alive with
    health and resistance instead.

  * **A zombie needs line of sight for a player or an iron golem, and not for a
    villager.** That is why the quarry is a villager: a maze wall denies sight
    by construction, and the experiment is the path, not the acquisition.

  * `spawn-animals=false` does not merely stop natural spawning: the server
    discards every Animal on its first tick, summoned ones included, and
    `PersistenceRequired` does not save them. Natural spawning is switched with
    the `doMobSpawning` gamerule instead and the properties stay `true`.

  * `NoAI:1b` stops a Mob's physics, not only its brain. Right for a quarry that
    must stand still; catastrophic for the walker, which would then be measured
    not moving.

  * `PersistenceRequired:1b` resets `noActionTime` on every despawn check, and
    vanilla's random-stroll goal goes quiet only once `noActionTime` passes 100.
    Pinning a mob against despawning therefore also keeps its wander goal
    competing for the movement control.

  * **A vanilla server with no player connected spawns nothing at all.** The
    natural spawner runs over the chunks a player ticket reaches, and there are
    none. Every spawning campaign here therefore joins a probe client and keeps
    it alive for the whole run; a run with the probe dropped reads zero
    everywhere and looks exactly like a threshold of zero.

  * A mob is refused within 24 blocks of a player and never offered past 128,
    so the probe stands off to one side of the test site rather than in it.
"""
from __future__ import annotations

import json
import re
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_entity_packets import Probe  # noqa: E402
from measure_entities import Server  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"

SCORE = re.compile(r"has (-?\d+) \[")
POS = re.compile(r"following entity data: \[([-0-9.dEe]+), ([-0-9.dEe]+), ([-0-9.dEe]+)\]")
GAMETIME = re.compile(r"The time is (\d+)")

# The ordinary superflat: bedrock, two dirt, grass. Its top face — and so the y
# a mob stands at — is -60.0, with the grass block itself at -61.
GROUND_Y = -60

# The void world's floor is whatever this rig builds; the rooms are put at the
# same y so the two campaigns talk about the same heights.
VOID_Y = -60


class FlatServer(Server):
    """A superflat with the wildlife left on. See the module docstring."""

    EXTRA_PROPERTIES = (
        "level-type=minecraft\\:flat\n"
        "generate-structures=false\n"
        "spawn-protection=0\n"
        "view-distance=10\n"
        "simulation-distance=10\n"
        "spawn-animals=true\n"
        "spawn-monsters=true\n"
        "spawn-npcs=true\n"
    )
    HEAP = "-Xmx1500M"


class VoidServer(FlatServer):
    """A superflat of one air layer: nothing spawns except on what we build."""

    EXTRA_PROPERTIES = FlatServer.EXTRA_PROPERTIES + (
        'generator-settings={"layers":[{"block":"minecraft:air","height":1}],'
        '"biome":"minecraft:plains"}\n'
    )


class KeptAlive:
    """A probe client that stays connected while a campaign runs.

    Pumped on a thread of its own: the vanilla server drops a client that has
    not answered a keep-alive in thirty seconds, and a campaign that runs for
    four minutes between console commands would lose it in the middle and
    silently stop spawning anything.
    """

    def __init__(self, port: int, name: str = "ovprobe") -> None:
        self.probe = Probe(port, name)
        self.name = name
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                self.probe.pump(0.5)
                self.probe.drain()  # nothing here reads the packets; drop them
            except Exception:
                return

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2.0)
        try:
            self.probe.socket.close()
        except Exception:
            pass


def count(server: Server, selectors: dict[str, str]) -> dict[str, int]:
    """How many entities match each selector.

    `execute store result score` is the documented way to get a number out of a
    selector on a console: `if entity <selector>` succeeds once per match, and
    the count of successes is what is stored. `scoreboard players get` on a
    holder whose score is zero prints an error rather than a zero, so every
    holder is set to zero first — which also makes the reply lines line up one
    per selector, in order.
    """
    order = list(selectors)
    commands = [f"scoreboard players set ov{i} ovcount 0" for i in range(len(order))]
    commands += [f"execute store result score ov{i} ovcount if entity {selectors[name]}"
                 for i, name in enumerate(order)]
    commands += [f"scoreboard players get ov{i} ovcount" for i in range(len(order))]
    lines = server.batch(commands, timeout=120.0)
    found = [int(m.group(1)) for line in lines if (m := SCORE.search(line))]
    if len(found) != len(order):
        raise RuntimeError(f"expected {len(order)} counts, got {len(found)}: {lines[-6:]}")
    return dict(zip(order, found))


def sample_pos(server: Server, tag: str) -> tuple[int, list[float]] | None:
    """One (gametime, Pos) reading, or None if the entity is gone."""
    lines = server.batch([f"time query gametime",
                          f'data get entity @e[tag={tag},limit=1] Pos'], timeout=30.0)
    tick = None
    pos = None
    for line in lines:
        m = GAMETIME.search(line)
        if m:
            tick = int(m.group(1))
        m = POS.search(line)
        if m:
            pos = [float(g.rstrip("d")) for g in m.groups()]
    if tick is None or pos is None:
        return None
    return tick, pos


# ── Campaign 1: the light thresholds ────────────────────────────────────────

LIGHT_ROOM = 7          # inner size, in blocks
# The emission of the light block, not the light on the floor. The block sits
# two above the floor, so the floor reads `level - 2`; the campaign reports the
# floor value, which is what the spawn rule actually tests.
LIGHT_LEVELS = list(range(2, 13))
LIGHT_DROP = 2
LIGHT_PITCH = LIGHT_ROOM + 5
LIGHT_BASE_X = -((len(LIGHT_LEVELS) * LIGHT_PITCH) // 2)
# The probe stands here: off the row on Z, so every room is past the 24-block
# refusal radius and inside the 128-block offer radius.
PROBE_AT = (0, VOID_Y + 40, 48)
LIGHT_SECONDS = 200.0

MONSTERS = ("minecraft:zombie", "minecraft:skeleton", "minecraft:spider",
            "minecraft:creeper", "minecraft:enderman", "minecraft:witch",
            "minecraft:slime", "minecraft:zombie_villager")


def build_light_rooms(server: Server, roof_open: bool) -> list[tuple[int, int]]:
    """One room per level. Returns each room's (x0, x1) inner span.

    Three things here were got wrong the first time round and each one made the
    result meaningless in a different, plausible-looking way.

      * **A corner light does not light a floor.** The light block was first put
        in two ceiling corners, six or seven blocks away from the middle of the
        floor; block light falls off one per block, so a room "at level 8" had a
        floor lit somewhere between 2 and 0 depending where you stood. The whole
        *layer* two blocks above the floor is filled instead, so every floor
        cell is exactly `level - 2` and the room has one light value rather than
        a gradient.

      * **Mobs spawn on the roofs.** The rooms sit in a void world, so their
        stone roofs were the only other surface for a hundred blocks — and at
        midnight a roof in the open reads light 4. They consume the same
        category cap the rooms are competing for. The top surface is bottom
        slabs, whose upper face is not sturdy and which nothing will spawn on.

      * **Room `n` was counted in room `n + 1`.** The counting selector was a
        sphere of radius 7 around a room 7 wide with a pitch of 10, so each
        count included its neighbours' edges. It is a box now, exactly the
        interior.
    """
    spans = []
    for index, level in enumerate(LIGHT_LEVELS):
        x0 = LIGHT_BASE_X + index * LIGHT_PITCH
        x1 = x0 + LIGHT_ROOM - 1
        z0, z1 = -3, 3
        y0, y1 = VOID_Y, VOID_Y + 3
        cmds = [
            # A solid block, then hollowed out. Stone rather than glass: glass
            # lets sky light through and the sealed run would not be sealed.
            f"fill {x0 - 1} {y0 - 1} {z0 - 1} {x1 + 1} {y1 + 1} {z1 + 1} minecraft:stone",
            f"fill {x0} {y0} {z0} {x1} {y1} {z1} minecraft:air",
        ]
        if roof_open:
            cmds.append(f"fill {x0} {y1 + 1} {z0} {x1} {y1 + 1} {z1} minecraft:air")
        else:
            cmds.append(f"fill {x0} {y1 + 1} {z0} {x1} {y1 + 1} {z1} minecraft:stone")
        # Nothing stands on a bottom slab: its top face is not sturdy. This is
        # what keeps the void world's only other surface — these roofs — out of
        # the category cap the rooms are competing for.
        cmds.append(f"fill {x0 - 1} {y1 + 2} {z0 - 1} {x1 + 1} {y1 + 2} {z1 + 1} "
                    "minecraft:smooth_stone_slab[type=bottom]")
        # The light layer: the whole cross-section, two above the floor, so the
        # floor is uniformly at `level - LIGHT_DROP` and the mob's own two
        # blocks of body are clear of it.
        cmds.append(f"fill {x0} {y0 + LIGHT_DROP} {z0} {x1} {y0 + LIGHT_DROP} {z1} "
                    f"minecraft:light[level={level}]")
        server.batch(cmds, timeout=120.0)
        spans.append((x0, x1))
    return spans


def light_run(server: Server, roof_open: bool, at_time: str) -> dict[int, int]:
    server.batch(["gamerule doMobSpawning false",
                  "kill @e[type=!minecraft:player]",
                  f"time set {at_time}"], timeout=60.0)
    spans = build_light_rooms(server, roof_open)
    server.batch(["gamerule doMobSpawning true"], timeout=60.0)
    deadline = time.monotonic() + LIGHT_SECONDS
    while time.monotonic() < deadline:
        time.sleep(5.0)
    server.batch(["gamerule doMobSpawning false"], timeout=60.0)

    selectors = {}
    for index, (x0, x1) in enumerate(spans):
        # A box, exactly the room's interior. A sphere of radius LIGHT_ROOM
        # around a room LIGHT_ROOM wide reaches into both neighbours, which is
        # how the first run of this campaign produced counts that rose again at
        # the bright end.
        box = f"x={x0},y={VOID_Y},z=-3,dx={LIGHT_ROOM - 1},dy=3,dz=6"
        for kind, type_name in enumerate(MONSTERS):
            selectors[f"{index}:{kind}"] = f"@e[type={type_name},{box}]"
    counts = count(server, selectors)
    per_level = {}
    for index, level in enumerate(LIGHT_LEVELS):
        # Keyed by the light on the floor, which is what the spawn rule tests —
        # not by the emission of the block two above it.
        per_level[level - LIGHT_DROP] = sum(counts[f"{index}:{k}"] for k in range(len(MONSTERS)))
    server.batch(["kill @e[type=!minecraft:player]"], timeout=60.0)
    return per_level


def campaign_light(server: Server, probe: KeptAlive) -> dict:
    server.batch([f"gamemode creative {probe.name}",
                  f"tp {probe.name} {PROBE_AT[0]} {PROBE_AT[1]} {PROBE_AT[2]}",
                  f"effect give {probe.name} minecraft:resistance 999999 4 true",
                  f"effect give {probe.name} minecraft:levitation 1 0 true"], timeout=60.0)
    out = {}
    for key, roof_open, at_time in (("sealed_midnight", False, "midnight"),
                                    ("open_midnight", True, "midnight"),
                                    ("open_noon", True, "noon")):
        print(f"   light: {key}", flush=True)
        # A creative player falls through a void world; put them back before
        # each run, because a probe that has fallen to y=-200 stops being the
        # reference point the spawner measures from.
        server.batch([f"tp {probe.name} {PROBE_AT[0]} {PROBE_AT[1]} {PROBE_AT[2]}"])
        out[key] = light_run(server, roof_open, at_time)
        print(f"      {out[key]}", flush=True)
    return out


# ── Campaign 2: the per-category caps ───────────────────────────────────────

CATEGORIES = {
    "monster": ("zombie", "skeleton", "creeper", "spider", "enderman", "witch",
                "slime", "zombie_villager", "drowned", "husk", "stray",
                "cave_spider", "silverfish", "phantom"),
    "creature": ("cow", "pig", "sheep", "chicken", "horse", "donkey", "rabbit",
                 "wolf", "llama"),
    "ambient": ("bat",),
    "water_creature": ("squid", "dolphin"),
    "water_ambient": ("cod", "salmon", "tropical_fish", "pufferfish"),
    "underground_water_creature": ("glow_squid",),
}

CAPS_SAMPLES = 24
CAPS_INTERVAL = 10.0


def campaign_caps(server: Server, probe: KeptAlive) -> dict:
    server.batch([
        "gamerule doMobSpawning false",
        "kill @e[type=!minecraft:player]",
        f"gamemode survival {probe.name}",
        f"effect give {probe.name} minecraft:resistance 999999 4 true",
        f"tp {probe.name} 0 {GROUND_Y} 0",
        "time set midnight",
        "difficulty hard",
    ], timeout=120.0)
    server.batch(["gamerule doMobSpawning true"], timeout=60.0)

    selectors = {}
    for name, members in CATEGORIES.items():
        for i, kind in enumerate(members):
            selectors[f"{name}:{i}"] = f"@e[type=minecraft:{kind}]"
    series: dict[str, list[int]] = {name: [] for name in CATEGORIES}
    for step in range(CAPS_SAMPLES):
        time.sleep(CAPS_INTERVAL)
        server.batch([f"tp {probe.name} 0 {GROUND_Y} 0"])
        counts = count(server, selectors)
        for name, members in CATEGORIES.items():
            series[name].append(sum(counts[f"{name}:{i}"] for i in range(len(members))))
        if step % 6 == 5:
            print(f"   caps @{step + 1}: "
                  + " ".join(f"{n}={v[-1]}" for n, v in series.items()), flush=True)
    server.batch(["gamerule doMobSpawning false"], timeout=60.0)
    tail = max(1, CAPS_SAMPLES // 3)
    return {
        "interval_seconds": CAPS_INTERVAL,
        "samples": series,
        # The last third, where the count has stopped climbing. A mean over the
        # whole run would be dragged down by the empty start and understate the
        # cap by about a third.
        "plateau": {n: round(sum(v[-tail:]) / tail, 2) for n, v in series.items()},
        "peak": {n: max(v) for n, v in series.items()},
    }


# ── Campaign 3: the maze ────────────────────────────────────────────────────
#
# The same maze is compiled into test_pathfinding.cpp. Changing one without the
# other makes the comparison meaningless, so the layout is written into the JSON
# too and the test asserts the shape it replays.

MAZE_BASE_X = 6000
# Three transverse walls rather than four, and eight blocks either side rather
# than ten. The first maze that worked was 21 by 21 with four walls, and the
# zombie crossed two of them at exactly the right gaps and then oscillated for a
# thousand ticks between the second and third — a route longer than the vanilla
# navigator will hold at once. Shrinking the maze until a whole traversal fits
# is the difference between measuring a path and measuring a node budget.
MAZE_WALL_Z = (-4, 0, 4)
MAZE_HALF = 8


def maze_walls() -> list[tuple[int, int]]:
    """A serpentine with one corridor, not a random wall field.

    A random field usually has many equally short routes, and two path finders
    agreeing on the *length* while disagreeing on every step would look like
    agreement. A serpentine has one way through, so a mob that reaches the end
    reached it the same way or did something we should see.
    """
    walls: list[tuple[int, int]] = []
    for index, z in enumerate(MAZE_WALL_Z):
        gap = -MAZE_HALF + 1 if index % 2 == 0 else MAZE_HALF - 1
        for x in range(-MAZE_HALF, MAZE_HALF + 1):
            if x != gap:
                walls.append((x, z))
    return walls


def crossings(trace: list[list[float]]) -> list[list[float]]:
    """Which gap the walker went through, and in which order.

    This, and not the sequence of positions, is what the comparison against our
    own A* is made on. A vanilla zombie re-paths several times a second and
    wobbles a block either side of its route; the *openings it chooses* do not
    wobble, and a path finder that picks a different gap has genuinely found a
    different way through.
    """
    out = []
    for a, b in zip(trace, trace[1:]):
        for wall_z in MAZE_WALL_Z:
            if (a[3] - wall_z) * (b[3] - wall_z) < 0:
                out.append([wall_z, round((a[1] + b[1]) / 2.0, 3), a[0]])
    return out


def build_arena(server: Server, ox: int, half_x: int, half_z: int,
                walled: bool = False) -> None:
    """Flat stone floor, cleared headroom, and optionally a fence of stone.

    The wall matters for the maze: the superflat has ground everywhere, so a mob
    that loses its target simply walks off the experiment. The first run of this
    rig ended with the zombie eight blocks outside the arena.
    """
    y = GROUND_Y
    server.batch([
        f"forceload add {ox - half_x - 16} {-half_z - 16} {ox + half_x + 16} {half_z + 16}",
    ], timeout=180.0)
    server.batch([
        f"fill {ox - half_x} {y} {-half_z} {ox + half_x} {y + 5} {half_z} minecraft:air",
        f"fill {ox - half_x} {y - 1} {-half_z} {ox + half_x} {y - 1} {half_z} minecraft:stone",
    ], timeout=120.0)
    if walled:
        for a, b in ((f"{ox - half_x} {y} {-half_z} {ox - half_x} {y + 3} {half_z}", ""),
                     (f"{ox + half_x} {y} {-half_z} {ox + half_x} {y + 3} {half_z}", ""),
                     (f"{ox - half_x} {y} {-half_z} {ox + half_x} {y + 3} {-half_z}", ""),
                     (f"{ox - half_x} {y} {half_z} {ox + half_x} {y + 3} {half_z}", "")):
            server.batch([f"fill {a} minecraft:stone"], timeout=60.0)


# The walker: a plain zombie, pinned so it cannot be despawned mid-run, with a
# helmet so it does not burn if a run straddles dawn. `NoAI` is *not* here — it
# stops a Mob's physics, not only its brain.
WALKER_NBT = ('{{PersistenceRequired:1b,Silent:1b,Tags:["{tag}"],'
              'ArmorItems:[{{}},{{}},{{}},{{id:"minecraft:leather_helmet",Count:1b}}]}}')

# The quarry: a **villager**, and the choice is the whole rig.
#
# A zombie acquires a player or an iron golem only with line of sight, and a
# maze wall is precisely what denies it. Two earlier versions of this campaign
# worked around that by hurting the zombie every tick — being hurt needs no
# sight — and both produced a trace that is a measurement of the workaround
# rather than of the path: the target set that way is dropped the very next
# tick, so the zombie alternated between chasing and wandering and crossed the
# same wall three times without ever finishing.
#
# A villager is acquired *without* line of sight and kept, so the chase goal
# holds the movement control continuously and outranks the wander goal for the
# whole run. Nothing is supplied from outside once the two entities exist.
#
# **Not Invulnerable** — and that tag was the root cause of the whole saga
# above. Vanilla refuses an invulnerable entity as a target outright, so the
# very first run of this rig had a zombie standing three blocks from an
# invulnerable iron golem and ignoring it completely; every workaround that
# followed was aimed at the wrong symptom. The quarry is kept alive by health
# and resistance instead, which leave it a legal target.
#
# NoAI on the villager is right for the same reason it was wrong on the zombie:
# it must not move.
GOAL_NBT = ('{{NoAI:1b,NoGravity:1b,Silent:1b,PersistenceRequired:1b,'
            'Tags:["{tag}"]}}')
TARGET_TYPE = "minecraft:villager"


def arm_target(server: Server, target: str) -> None:
    """Keep the quarry alive without making it un-targetable.

    Resistance V and a thousand hit points rather than `Invulnerable:1b`: see
    the note above. A zombie deals a few points a second and only reaches the
    villager at the very end of a run, so this is generous by two orders of
    magnitude — which is the point, since a quarry that dies halfway turns the
    rest of the trace into noise without saying so anywhere.
    """
    server.batch([
        f"attribute @e[tag={target},limit=1] minecraft:generic.max_health base set 1024",
        f"data merge entity @e[tag={target},limit=1] {{Health:1024f}}",
        f"effect give @e[tag={target},limit=1] minecraft:resistance 99999 4 true",
    ], timeout=60.0)


def arm_walker(server: Server, walker: str) -> None:
    """Give the walker sight long enough to see across the whole arena.

    The only thing changed from a summoned zombie. `follow_range` is what bounds
    both target acquisition and the navigator's search, and a default of 35 is
    shorter than the diagonal of the speed run.
    """
    server.batch([
        f"attribute @e[tag={walker},limit=1] minecraft:generic.follow_range base set 128",
    ], timeout=60.0)


def walk_trace(server: Server, tag: str, done, limit: int) -> list[list[float]]:
    """Sample (gametime, Pos) until `done` or the budget runs out.

    One console round trip per sample, which lands at roughly one sample per
    game tick — dense enough that a route is a route and not a set of end
    points, and the gametime on every row says exactly how dense it was.
    """
    samples: list[list[float]] = []
    for _ in range(limit):
        got = sample_pos(server, tag)
        if got is None:
            break
        tick, pos = got
        samples.append([tick, round(pos[0], 5), round(pos[1], 5), round(pos[2], 5)])
        if done(pos):
            break
    return samples


def campaign_maze(server: Server, probe: KeptAlive) -> dict:
    ox, y = MAZE_BASE_X, GROUND_Y
    walls = maze_walls()
    server.batch(["gamerule doMobSpawning false", "gamerule mobGriefing false",
                  "kill @e[type=!minecraft:player]", "time set midnight",
                  "difficulty hard"], timeout=120.0)
    # The perimeter sits exactly on the transverse walls' ends. The first
    # run of this rig made the arena two blocks wider than the walls, and the
    # zombie solved the maze by strolling round the end of every one of them.
    build_arena(server, ox, MAZE_HALF, MAZE_HALF, walled=True)
    for x, z in walls:
        server.batch([f"fill {ox + x} {y} {z} {ox + x} {y + 2} {z} minecraft:stone"],
                     timeout=60.0)

    # One block inside the perimeter, not on it. The perimeter wall stands at
    # z = ±MAZE_HALF, so a start of -MAZE_HALF + 0.5 summons the zombie inside
    # the wall and the collision resolution shoots it clean out of the arena —
    # which is what one run of this rig measured, ending 25 blocks outside.
    start = (ox + 0.5, float(y), -(MAZE_HALF - 1) + 0.5)
    goal = (ox + 0.5, float(y), (MAZE_HALF - 1) + 0.5)
    server.batch([
        f"summon {TARGET_TYPE} {goal[0]} {goal[1]} {goal[2]} "
        + GOAL_NBT.format(tag="ovgoal"),
        f"summon minecraft:zombie {start[0]} {start[1]} {start[2]} "
        + WALKER_NBT.format(tag="ovwalker"),
    ], timeout=60.0)
    # Forty blocks away: past the 32 that resets `noActionTime` and so keeps the
    # wander goal alive, and well inside the 128 past which a monster is
    # discarded outright.
    server.batch([f"gamemode spectator {probe.name}",
                  f"tp {probe.name} {ox} {y + 6} {MAZE_HALF + 40}"], timeout=60.0)
    arm_walker(server, "ovwalker")
    arm_target(server, "ovgoal")

    trace = walk_trace(server, "ovwalker",
                       lambda p: abs(p[2] - goal[2]) < 2.0 and abs(p[0] - goal[0]) < 4.0,
                       limit=1800)
    local = [[t, round(x - ox, 5), yy, z] for t, x, yy, z in trace]
    return {
        "walls": [list(w) for w in walls],
        "half": MAZE_HALF,
        "origin_x": ox,
        "start": [start[0] - ox, start[1], start[2]],
        "goal": [goal[0] - ox, goal[1], goal[2]],
        "wall_z": list(MAZE_WALL_Z),
        "trace": local,
        "crossings": crossings(local),
        "reached": bool(trace and abs(trace[-1][3] - goal[2]) < 2.0),
    }


# ── Campaign 4: chase speed ─────────────────────────────────────────────────

SPEED_BASE_X = 8000


def campaign_speed(server: Server, probe: KeptAlive) -> dict:
    ox, y = SPEED_BASE_X, GROUND_Y
    server.batch(["gamerule doMobSpawning false", "kill @e[type=!minecraft:player]",
                  "time set midnight", "difficulty hard"], timeout=120.0)
    # Thirty blocks apart, not sixty. At sixty the zombie never acquired the
    # villager at all — measured, with follow_range raised to 128 and the
    # chunks forceloaded — and the trace was a wander. Thirty is inside
    # whatever the real acquisition envelope is and still leaves twenty
    # blocks of straight running to average over.
    build_arena(server, ox, 22, 8, walled=True)
    server.batch([
        f"summon {TARGET_TYPE} {ox + 15}.5 {y} 0.5 " + GOAL_NBT.format(tag="ovtarget"),
        f"summon minecraft:zombie {ox - 15}.5 {y} 0.5 " + WALKER_NBT.format(tag="ovrunner"),
    ], timeout=60.0)
    server.batch([f"gamemode spectator {probe.name}",
                  f"tp {probe.name} {ox} {y + 6} 45"], timeout=60.0)
    arm_walker(server, "ovrunner")
    arm_target(server, "ovtarget")
    trace = walk_trace(server, "ovrunner", lambda p: p[0] - ox > 12.0, limit=400)
    speeds = []
    for a, b in zip(trace, trace[1:]):
        dt = b[0] - a[0]
        if dt <= 0:
            continue
        step = ((b[1] - a[1]) ** 2 + (b[3] - a[3]) ** 2) ** 0.5 / dt
        speeds.append(step)
    # The middle half of the run: the first samples are the acquisition and the
    # last are the mob stopping in front of the golem, and neither is a speed.
    middle = sorted(speeds)[len(speeds) // 4: 3 * len(speeds) // 4] if speeds else []
    return {
        "trace": [[t, round(x - ox, 5), yy, z] for t, x, yy, z in trace],
        "blocks_per_tick": round(sum(middle) / len(middle), 6) if middle else None,
        "samples_used": len(middle),
    }


# ── Campaign 5: how far a mob sees ──────────────────────────────────────────

ACQUIRE_BASE_X = 9000
ACQUIRE_DISTANCES = (8, 16, 24, 32, 40, 48, 56, 64)


def campaign_acquire(server: Server, probe: KeptAlive) -> dict:
    """At what separation does a zombie start walking towards a villager?

    Asked because the speed campaign ran into it rather than around it: at
    sixty blocks apart the zombie never moved, and the honest response to "I
    shortened it until it worked" is to find out what the number is.

    Each distance gets its own arena, its own pair of entities and sixty ticks.
    The verdict is displacement towards the quarry, which is unambiguous — a
    wandering zombie does not walk thirty blocks in one direction in three
    seconds. Run twice: once with the type's own `follow_range` of 35, and once
    with it raised to 128, which says whether the radius is that attribute or
    something else.
    """
    y = GROUND_Y
    out: dict = {}
    for label, follow_range in (("default_follow_range", None), ("follow_range_128", 128)):
        verdicts = {}
        for index, gap in enumerate(ACQUIRE_DISTANCES):
            ox = ACQUIRE_BASE_X + index * 256 + (0 if follow_range is None else 4096)
            half = gap // 2 + 6
            server.batch(["kill @e[type=!minecraft:player]"], timeout=60.0)
            build_arena(server, ox, half, 8, walled=True)
            server.batch([f"gamemode spectator {probe.name}",
                          f"tp {probe.name} {ox} {y + 6} 45"], timeout=60.0)
            server.batch([
                f"summon {TARGET_TYPE} {ox + gap // 2}.5 {y} 0.5 "
                + GOAL_NBT.format(tag="ovq"),
                f"summon minecraft:zombie {ox - gap // 2}.5 {y} 0.5 "
                + WALKER_NBT.format(tag="ovz"),
            ], timeout=60.0)
            arm_target(server, "ovq")
            if follow_range is not None:
                server.batch([f"attribute @e[tag=ovz,limit=1] "
                              f"minecraft:generic.follow_range base set {follow_range}"],
                             timeout=60.0)
            start = sample_pos(server, "ovz")
            trace = walk_trace(server, "ovz", lambda p: False, limit=60)
            if start is None or not trace:
                verdicts[gap] = None
                continue
            moved = trace[-1][1] - start[1][0]
            verdicts[gap] = round(moved, 4)
            print(f"   acquire {label} gap={gap}: moved {moved:+.2f} towards", flush=True)
        out[label] = verdicts
    return out


CAMPAIGNS = {
    "light": (campaign_light, VoidServer, 25596),
    "caps": (campaign_caps, FlatServer, 25595),
    "maze": (campaign_maze, FlatServer, 25594),
    "speed": (campaign_speed, FlatServer, 25593),
    "acquire": (campaign_acquire, FlatServer, 25592),
}

SETUP = [
    "gamerule doDaylightCycle false",
    "gamerule doWeatherCycle false",
    "gamerule randomTickSpeed 0",
    "gamerule doFireTick false",
    "gamerule doMobSpawning false",
    "gamerule doImmediateRespawn true",
    "gamerule keepInventory true",
    "difficulty hard",
    "scoreboard objectives add ovcount dummy",
]


def main(argv: list[str]) -> int:
    wanted = argv[1:] or list(CAMPAIGNS)
    unknown = [w for w in wanted if w not in CAMPAIGNS]
    if unknown:
        print(f"unknown campaign(s): {', '.join(unknown)}", file=sys.stderr)
        print(f"known: {', '.join(CAMPAIGNS)}", file=sys.stderr)
        return 2

    NORMALIZED.mkdir(parents=True, exist_ok=True)
    out_path = NORMALIZED / "mob_spawning.json"
    result: dict = json.loads(out_path.read_text()) if out_path.exists() else {}

    for name in wanted:
        fn, server_class, port = CAMPAIGNS[name]
        # A server per campaign: `light` needs a void world and the others need
        # an ordinary superflat, and a world's generator cannot be changed once
        # it exists. Cheaper than it looks — start-up is a few seconds on a
        # flat world with no structures.
        print(f"── campaign {name} ({server_class.__name__}, port {port})", flush=True)
        run_dir = ROOT / "run" / f"mob-oracle-{name}"
        server = server_class(run_dir, port=port)
        probe = None
        started = time.monotonic()
        try:
            server.batch(SETUP, timeout=120.0)
            probe = KeptAlive(port)
            time.sleep(2.0)
            result[name] = fn(server, probe)
            print(f"   done in {time.monotonic() - started:.0f} s", flush=True)
            out_path.write_text(json.dumps(result, indent=1) + "\n")
        finally:
            if probe is not None:
                probe.close()
            server.stop()

    out_path.write_text(json.dumps(result, indent=1) + "\n")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
