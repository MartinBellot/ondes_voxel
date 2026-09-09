#!/usr/bin/env python3
"""Ask a real 1.20.1 server how big every entity is, and what it is worth.

Nothing in Mojang's reports carries a hitbox, an eye height or an attribute
base value: they live in Java code, and this project does not read Java code.
But the running game answers questions about them, through commands — which are
documented data, not source:

  hitbox      `execute positioned <p> if entity @e[...,dx=0,dy=0,dz=0]` passes
              exactly when the probe volume meets the entity's bounding box.
              Bisecting that boundary along +X and +Y gives the box.

              Whether the probe volume is a point is not assumed. It is
              calibrated first against `minecraft:interaction`, the one entity
              whose width and height ARE its NBT: summoned at five declared
              sizes and measured the same way, it says what the probe adds to a
              boundary, and whether that is additive or a scale factor.

  eye height  `execute as <mob> at @s anchored eyes positioned ^ ^ ^ run summon
              minecraft:marker ~ ~ ~` puts a marker exactly where the game
              thinks that entity's eyes are. Reading the marker's Pos back and
              subtracting the mob's own gives the eye height to the last decimal
              the double carries.

  attributes  `attribute <target> <attribute> base get` prints the base value.
              Asked for all thirteen attributes of every type, so the answer is
              also *which* attributes a type has — a type without one says so
              rather than being handed a plausible zero.

  default NBT `data get entity <target>` — the whole tag a freshly summoned
              entity of that type carries.

Usage: python3 scripts/measure_entities.py [output.json]

Writes data/vanilla/1.20.1/normalized/entities.json by default. The server is
started and stopped by this script, on a port of its own so it cannot collide
with one already running.
"""
from __future__ import annotations

import json
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
JAR = ROOT / "tools" / "vanilla" / "server.jar"
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
RUN = ROOT / "run" / "entity-oracle"

# The grid the entities stand on. Sixteen blocks apart is far enough that the
# biggest hitbox in the game (the ender dragon's) cannot reach a neighbour.
SPACING = 16
COLUMNS = 12
STAND_Y = -59.0

# Summoned with these so nothing moves, burns, drowns or despawns between being
# placed and being measured. Every one is a documented NBT tag on the base
# entity or on Mob; a type that does not have one ignores it.
SUMMON_NBT = "NoAI:1b,NoGravity:1b,Silent:1b,Invulnerable:1b,PersistenceRequired:1b"

# The types that remove themselves unless told not to. Each of these is a
# documented NBT field, and each one is here because the type was measured as
# absent without it — not because a list said it would be.
EXTRA_NBT = {
    "minecraft:item": 'Item:{id:"minecraft:stone",Count:1b},Age:-32768s,PickupDelay:32767s',
    "minecraft:tnt": "Fuse:32767s",
    "minecraft:area_effect_cloud": "Duration:2147483647,Radius:3.0f,WaitTime:0",
    "minecraft:firework_rocket": "LifeTime:2147483647",
    "minecraft:experience_orb": "Age:-32768s,Value:1s",
    "minecraft:falling_block": 'BlockState:{Name:"minecraft:sand"},Time:-32768',
    # Slimes, magma cubes and phantoms come out of `summon` at a random size,
    # and their hitbox is a multiple of it. Pinned to the smallest, so the
    # number recorded is one size and not whichever the RNG chose.
    "minecraft:slime": "Size:0",
    "minecraft:magma_cube": "Size:0",
    "minecraft:phantom": "Size:0",
}

LINE = re.compile(r"^\[[0-9:]+\] \[[^\]]+\]: (.*)$")
TEST = re.compile(r"Test (passed|failed)")
POS = re.compile(r"following entity data: \[([-0-9.dEe]+), ([-0-9.dEe]+), ([-0-9.dEe]+)\]")
ATTRIBUTE = re.compile(r"Base value of attribute .* is ([-0-9.Ee]+)$")

MARKER_ATTR = "[Server] ovattr "
MARKER_NBT = "[Server] ovnbt "


class Server:
    """The vanilla server, driven through its console."""

    #: Extra server.properties lines a measurement needs. Empty here on
    #: purpose: every default above was chosen for a reason, and a subclass
    #: that wants another one has to say which.
    EXTRA_PROPERTIES = ""

    def __init__(self, directory: Path, port: int = 25599) -> None:
        directory.mkdir(parents=True, exist_ok=True)
        shutil.copy(JAR, directory / "server.jar")
        (directory / "eula.txt").write_text("eula=true\n")
        (directory / "server.properties").write_text(
            f"server-port={port}\n"
            "level-type=minecraft\\:flat\n"
            "online-mode=false\n"
            "spawn-protection=0\n"
            "max-players=4\n"
            "view-distance=10\n"
            "simulation-distance=10\n"
            "sync-chunk-writes=true\n"
            # Every category left ON, which is the opposite of what a
            # measurement rig wants and is nevertheless required. With
            # spawn-animals=false the server *discards* animals on their first
            # tick — summoned ones included, PersistenceRequired included — and
            # the sweep loses every Animal subclass while the console cheerfully
            # says "Summoned new Cow". Natural spawning is stopped by the
            # doMobSpawning gamerule instead, and everything measured here is
            # picked out by a tag of its own, so the wildlife is harmless.
            "spawn-npcs=true\n"
            "spawn-animals=true\n"
            "spawn-monsters=true\n"
            "enable-command-block=false\n"
            # Une mesure qui a besoin d'autre chose l'ajoute en surchargeant cet
            # attribut. Les lignes ajoutées sont écrites après, donc elles
            # remplacent celles d'au-dessus : c'est la règle de server.properties.
            + self.EXTRA_PROPERTIES
        )
        self.process = subprocess.Popen(
            ["java", "-Xmx2G", "-jar", "server.jar", "nogui"],
            cwd=directory, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.lines: queue.Queue[str] = queue.Queue()
        self._reader = threading.Thread(target=self._pump, daemon=True)
        self._reader.start()
        self._token = 0
        try:
            # "Done (" is the only line that means the world is up. Waiting for
            # anything else — the port message, say — lets a server that failed
            # to bind look like a server that started, and every command after
            # it then silently goes nowhere.
            self._await("Done (", timeout=180.0)
        except TimeoutError:
            self.process.kill()
            raise

    def _pump(self) -> None:
        assert self.process.stdout is not None
        for raw in self.process.stdout:
            match = LINE.match(raw.rstrip("\n"))
            self.lines.put(match.group(1) if match else raw.rstrip("\n"))

    def _await(self, needle: str, timeout: float) -> list[str]:
        seen: list[str] = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            seen.append(line)
            if needle in line:
                return seen
        raise TimeoutError(f"never saw {needle!r}; last lines: {seen[-8:]}")

    def send(self, *commands: str) -> None:
        assert self.process.stdin is not None
        self.process.stdin.write("".join(c + "\n" for c in commands))
        self.process.stdin.flush()

    def batch(self, commands: list[str], timeout: float = 300.0) -> list[str]:
        """Run commands and return every console line they produced.

        A `say` marker closes the batch: the server runs console commands in
        order, so the marker arriving means everything before it has answered.
        """
        self._token += 1
        marker = f"ovsync{self._token}"
        self.send(*commands, f"say {marker}")
        return self._await(f"[Server] {marker}", timeout)[:-1]

    def stop(self) -> None:
        try:
            self.send("stop")
            self.process.wait(timeout=120)
        except Exception:
            self.process.kill()


def cell(index: int) -> tuple[float, float]:
    return (float((index % COLUMNS) * SPACING), float((index // COLUMNS) * SPACING))


def parse_pos(line: str) -> tuple[float, float, float] | None:
    match = POS.search(line)
    if match is None:
        return None
    return tuple(float(g.rstrip("d")) for g in match.groups())  # type: ignore[return-value]


def bisect_edges(server: Server, probes: list[tuple], rounds: int = 30) -> list[float]:
    """Bisect many boundaries at once.

    Each probe is (selector, x, y, z, axis, lo, hi) with `lo` inside the box and
    `hi` outside it. All of them advance one round per batch, so the number of
    console round-trips is the number of rounds and not the number of probes.
    """
    los = [p[5] for p in probes]
    his = [p[6] for p in probes]
    for _ in range(rounds):
        commands = []
        mids = []
        for (selector, x, y, z, axis, _lo, _hi), lo, hi in zip(probes, los, his):
            mid = (lo + hi) / 2.0
            mids.append(mid)
            px = x + (mid if axis == 0 else 0.0)
            py = y + (mid if axis == 1 else 0.0)
            pz = z + (mid if axis == 2 else 0.0)
            commands.append(f"execute positioned {px:.9f} {py:.9f} {pz:.9f} "
                            f"if entity {selector}")
        answers = [m.group(1) for line in server.batch(commands)
                   for m in [TEST.search(line)] if m]
        if len(answers) != len(probes):
            raise RuntimeError(f"expected {len(probes)} test answers, got {len(answers)}")
        for i, answer in enumerate(answers):
            if answer == "passed":
                los[i] = mids[i]
            else:
                his[i] = mids[i]
    return [(lo + hi) / 2.0 for lo, hi in zip(los, his)]


def main() -> int:
    out_path = Path(sys.argv[1]) if len(sys.argv) > 1 else NORMALIZED / "entities.json"
    with open(NORMALIZED / "registries.json") as f:
        registries = json.load(f)["registries"]
    types: list[str] = registries["minecraft:entity_type"]["entries"]
    attributes: list[str] = registries["minecraft:attribute"]["entries"]

    print(f"{len(types)} entity types, {len(attributes)} attributes")
    server = Server(RUN)
    try:
        server.batch([
            "gamerule doMobSpawning false", "gamerule randomTickSpeed 0",
            "gamerule doDaylightCycle false", "gamerule doWeatherCycle false",
            "gamerule doFireTick false", "gamerule mobGriefing false",
            "gamerule doMobLoot false", "gamerule sendCommandFeedback true",
            # Not peaceful: peaceful deletes every hostile mob the moment it is
            # summoned, and the measurement would be of an empty world.
            "difficulty normal",
            # Midnight, so nothing undead catches fire while being measured.
            "time set midnight",
        ])
        width = COLUMNS * SPACING
        depth = ((len(types) - 1) // COLUMNS + 1) * SPACING
        for x0 in range(-96, width + 64, 128):
            for z0 in range(-96, depth + 64, 128):
                server.batch([f"forceload add {x0} {z0} "
                              f"{min(x0 + 127, width + 64)} {min(z0 + 127, depth + 64)}"])
        time.sleep(5.0)

        # ── Calibration ─────────────────────────────────────────────────────
        declared = [(0.5, 0.5), (1.0, 2.0), (3.0, 0.25), (2.0, 1.0), (0.25, 4.0)]
        calib_probes = []
        for i, (w, h) in enumerate(declared):
            x, z = -64.0 + 16.0 * i, -64.0
            server.batch([f'summon minecraft:interaction {x} {STAND_Y} {z} '
                          f'{{width:{w}f,height:{h}f,Tags:["ovcal{i}"]}}'])
            selector = f"@e[tag=ovcal{i},dx=0,dy=0,dz=0,limit=1]"
            calib_probes.append((selector, x, STAND_Y, z, 0, 0.0, 32.0))
            calib_probes.append((selector, x, STAND_Y, z, 1, 0.0, 32.0))
        calib = bisect_edges(server, calib_probes)
        calibration = []
        for i, (w, h) in enumerate(declared):
            calibration.append({
                "declared_width": w, "declared_height": h,
                "measured_half_width": calib[2 * i], "measured_top": calib[2 * i + 1],
                "width_offset": calib[2 * i] - w / 2.0,
                "height_offset": calib[2 * i + 1] - h,
            })
            print(f"  calibration {w}x{h}: half-width {calib[2 * i]:.7f} "
                  f"top {calib[2 * i + 1]:.7f}")
        offsets = ([c["width_offset"] for c in calibration]
                   + [c["height_offset"] for c in calibration])
        offset = sum(offsets) / len(offsets)
        spread = max(offsets) - min(offsets)
        print(f"  probe offset {offset:.3e} (spread {spread:.3e})")
        server.batch(["kill @e[type=minecraft:interaction]"])

        # ── Summon one of everything ────────────────────────────────────────
        #
        # Each carries a unique tag. The probe position moves during a
        # bisection, so a selector filtering on distance from the execution
        # position would lose its entity halfway through and report a box that
        # stops wherever the filter did.
        placed: dict[str, tuple[float, float]] = {}
        tag = {name: f"ovent{i}" for i, name in enumerate(types)}
        commands = []
        for index, name in enumerate(types):
            x, z = cell(index)
            extra = EXTRA_NBT.get(name)
            nbt = SUMMON_NBT + (f",{extra}" if extra else "") + f',Tags:["{tag[name]}"]'
            commands.append(f'summon {name} {x + 0.5} {STAND_Y} {z + 0.5} {{{nbt}}}')
            placed[name] = (x + 0.5, z + 0.5)
        server.batch(commands)
        time.sleep(3.0)

        alive = {}
        for start in range(0, len(types), 20):
            group = types[start:start + 20]
            lines = server.batch([f"execute if entity @e[tag={tag[n]}]" for n in group])
            answers = [m.group(1) for line in lines for m in [TEST.search(line)] if m]
            if len(answers) != len(group):
                raise RuntimeError("presence check lost an answer")
            for name, answer in zip(group, answers):
                alive[name] = answer == "passed"
        present = [n for n in types if alive.get(n)]
        print(f"{len(present)}/{len(types)} types present after summoning")

        # ── Hitboxes ────────────────────────────────────────────────────────
        probes = []
        for name in present:
            x, z = placed[name]
            selector = f"@e[tag={tag[name]},dx=0,dy=0,dz=0,limit=1]"
            probes.append((selector, x, STAND_Y, z, 0, 0.0, 32.0))  # +X face
            probes.append((selector, x, STAND_Y, z, 1, 0.0, 32.0))  # top face
        edges = bisect_edges(server, probes)
        hitbox = {}
        for i, name in enumerate(present):
            hitbox[name] = {"width": round((edges[2 * i] - offset) * 2.0, 6),
                            "height": round(edges[2 * i + 1] - offset, 6)}

        # ── Eye heights ─────────────────────────────────────────────────────
        #
        # `anchored eyes` offsets the position of the **executing** entity, so
        # the mob has to be the executor. Run from the console with only `at`,
        # there is no @s to anchor to and the marker lands on the mob's feet — a
        # wrong answer that looks exactly like a right one.
        server.batch(["kill @e[type=minecraft:marker]"])
        server.batch([f'execute as @e[tag={tag[n]},limit=1] at @s anchored eyes '
                      f'positioned ^ ^ ^ run summon minecraft:marker ~ ~ ~ '
                      f'{{Tags:["oveye{i}"]}}'
                      for i, n in enumerate(present)])
        eye = {}
        for start in range(0, len(present), 20):
            group = list(enumerate(present))[start:start + 20]
            for i, name in group:
                lines = server.batch([f"data get entity @e[tag=oveye{i},limit=1] Pos"])
                position = next((p for line in lines for p in [parse_pos(line)]
                                 if p is not None), None)
                if position is None:
                    continue
                x, z = placed[name]
                if abs(position[0] - x) > 1e-6 or abs(position[2] - z) > 1e-6:
                    continue
                eye[name] = position[1] - STAND_Y
        server.batch(["kill @e[type=minecraft:marker]"])

        # ── Attributes ──────────────────────────────────────────────────────
        values: dict[str, dict[str, float]] = {}
        for start in range(0, len(present), 8):
            group = present[start:start + 8]
            commands = []
            for name in group:
                target = f"@e[tag={tag[name]},limit=1]"
                commands.append(f"say ovattr {name}")
                for attribute in attributes:
                    commands.append(f"attribute {target} {attribute} base get")
            lines = server.batch(commands)
            current, index = None, 0
            for line in lines:
                if MARKER_ATTR in line:
                    # `say` output arrives as "[Not Secure] [Server] <text>" on
                    # a 1.20.1 server, so the marker is found rather than
                    # matched from the start of the line. Anchoring it at the
                    # start silently found no markers at all, and the whole
                    # attribute sweep came back empty.
                    current, index = line.split(MARKER_ATTR, 1)[1], 0
                    values.setdefault(current, {})
                    continue
                if current is None or index >= len(attributes):
                    continue
                match = ATTRIBUTE.search(line)
                if match is not None:
                    values[current][attributes[index]] = float(match.group(1))
                    index += 1
                elif "no attribute" in line:
                    # A real answer, and the reason the sweep asks for all
                    # thirteen rather than a guessed subset.
                    index += 1

        # ── Default NBT ─────────────────────────────────────────────────────
        default_nbt = {}
        for start in range(0, len(present), 10):
            group = present[start:start + 10]
            commands = []
            for name in group:
                commands.append(f"say ovnbt {name}")
                commands.append(f"data get entity @e[tag={tag[name]},limit=1]")
            lines = server.batch(commands)
            current = None
            for line in lines:
                if MARKER_NBT in line:
                    current = line.split(MARKER_NBT, 1)[1]
                elif current is not None and "following entity data:" in line:
                    default_nbt[current] = line.split("following entity data: ", 1)[1]
                    current = None

        document = {
            "$comment": "Mesure sur un vrai serveur 1.20.1, adultes et sans monture. "
                        "Voir docs/PROVENANCE.md.",
            "probe_calibration": calibration,
            "probe_offset": offset,
            "probe_offset_spread": spread,
            "types": len(types),
            "present": len(present),
            "absent": [n for n in types if not alive.get(n)],
            "hitbox": hitbox,
            "eye_height": eye,
            "attributes": values,
            "default_nbt": default_nbt,
        }
        out_path.parent.mkdir(parents=True, exist_ok=True)
        with open(out_path, "w") as f:
            json.dump(document, f, indent=1, sort_keys=True)
        print(f"wrote {out_path}: {len(hitbox)} hitboxes, {len(eye)} eye heights, "
              f"{sum(len(v) for v in values.values())} attribute values")
    finally:
        server.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
