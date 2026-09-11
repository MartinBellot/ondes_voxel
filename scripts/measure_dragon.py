#!/usr/bin/env python3
"""The dragon fight, measured on the real 1.20.1 server — and the same run on ours.

A probe client in survival (resistance, regeneration and saturation, so the
fight cannot kill it) is put on an obsidian platform at the End's spawn point
and records every packet about the fight, stamped with the time it arrived:

  * the dragon: its spawn, every movement (Update Entity Position, Position and
    Rotation, Teleport), its metadata (health index 9, phase index 16);
  * the crystals: spawns, metadata (beam target index 8), removals;
  * fireballs and clouds: spawns, metadata (radius, particle), removals;
  * explosions (with their power), experience orbs (value), world events, the
    boss bar, the blocks that change.

Timeline (seconds of server time, roughly):
  1. 90 s of flight with the ten crystals;
  2. the ten crystals destroyed one by one, six seconds apart;
  3. 90 s of flight with none;
  4. a lethal hit (`damage` on vanilla; on ours, see `--ours`), 20 s of dying;
  5. level.dat copied (DragonFight after a death);
  6. four crystals placed on the exit portal, 45 s of the respawn;
  7. level.dat read again after the stop.

Usage:
  lockf /tmp/ov-vanilla.lock python3 scripts/measure_dragon.py      # -> .scratch/dragon/vanilla.json
  python3 scripts/measure_dragon.py --ours                           # -> .scratch/dragon/ours.json
  python3 scripts/measure_dragon.py --analyse                        # both, side by side
"""
from __future__ import annotations

import argparse
import gzip
import json
import math
import os
import queue
import shutil
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import anvil_read  # noqa: E402
from capture_entity_packets import Probe, read_varint, varint  # noqa: E402
from measure_entities import Server  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / ".scratch" / "dragon"
PORT = 25697
NAME = "ovdragon"
SEED = 1234567890

REGISTRIES = json.loads((ROOT / "data/vanilla/1.20.1/generated/reports/registries.json").read_text())
TYPES = {v["protocol_id"]: k for k, v in REGISTRIES["minecraft:entity_type"]["entries"].items()}
ITEMS = REGISTRIES["minecraft:item"]["entries"]

CB_SPAWN_ENTITY = 0x01
CB_SPAWN_ORB = 0x02
CB_BLOCK_UPDATE = 0x0A
CB_BOSS_BAR = 0x0B
CB_ENTITY_EVENT = 0x1C
CB_EXPLOSION = 0x1D
CB_KEEP_ALIVE = 0x23
CB_WORLD_EVENT = 0x25
CB_MOVE = 0x2B
CB_MOVE_ROT = 0x2C
CB_ROT = 0x2D
CB_SYNC = 0x3C
CB_REMOVE = 0x3E
CB_RESPAWN = 0x41
CB_SECTION_BLOCKS = 0x43
CB_METADATA = 0x52
CB_VELOCITY = 0x54
CB_SET_HEALTH = 0x57
CB_TELEPORT = 0x68
SB_POSITION = 0x14
SB_USE_ITEM_ON = 0x31
SB_SET_HELD_ITEM = 0x28
SB_INTERACT = 0x10
SB_SWING = 0x2F


def read_string(buf: bytes, i: int) -> tuple[str, int]:
    n, i = read_varint(buf, i)
    return buf[i:i + n].decode(), i + n


def packed(x: int, y: int, z: int) -> bytes:
    return struct.pack(">Q", ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF))


def unpacked(data: bytes, i: int) -> tuple[tuple[int, int, int], int]:
    value = struct.unpack_from(">q", data, i)[0]
    x = value >> 38
    y = value & 0xFFF
    if y >= 0x800:
        y -= 0x1000
    z = (value >> 12) & 0x3FFFFFF
    if z >= 0x2000000:
        z -= 0x4000000
    return (x, y, z), i + 8


def metadata(payload: bytes, i: int) -> dict[int, object]:
    """The entries of a Set Entity Metadata body, as far as their types are known."""
    out: dict[int, object] = {}
    while i < len(payload):
        index = payload[i]
        i += 1
        if index == 0xFF:
            break
        kind, i = read_varint(payload, i)
        if kind == 0:
            out[index] = payload[i]
            i += 1
        elif kind in (1, 12, 14, 15, 20):
            out[index], i = read_varint(payload, i)
        elif kind == 3:
            out[index] = struct.unpack_from(">f", payload, i)[0]
            i += 4
        elif kind == 8:
            out[index] = bool(payload[i])
            i += 1
        elif kind == 10:
            out[index], i = unpacked(payload, i)
        elif kind == 11:
            present = payload[i]
            i += 1
            if present:
                out[index], i = unpacked(payload, i)
            else:
                out[index] = None
        elif kind == 17:
            particle, i = read_varint(payload, i)
            out[index] = ("particle", particle)  # dragon_breath / entity_effect carry no data
        elif kind == 19:
            value, i = read_varint(payload, i)
            out[index] = value - 1 if value else None
        elif kind == 26:
            out[index] = struct.unpack_from(">fff", payload, i)
            i += 12
        else:
            out[index] = ("unparsed type", kind)
            break
    return out


class DragonProbe(Probe):
    """Records the fight; answers keep-alives and teleports."""

    def __init__(self, port: int) -> None:
        super().__init__(port, name=NAME)
        self.t0 = time.monotonic()
        self.log: list[list] = []
        self.types: dict[int, str] = {}
        self.dimension = "minecraft:overworld"
        self.dragon: int | None = None
        self.dragon_pos: list[float] | None = None
        self.blocks: dict[tuple[int, int, int], int] = {}
        self.sequence = 0

    def now(self) -> float:
        return round(time.monotonic() - self.t0, 4)

    def note(self, *entry) -> None:
        self.log.append([self.now(), *entry])

    def pump_once(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.socket.settimeout(max(0.005, deadline - time.monotonic()))
            try:
                packet_id, payload = self.read()
            except (TimeoutError, OSError):
                return
            self.handle(packet_id, payload)

    def handle(self, packet_id: int, payload: bytes) -> None:
        if packet_id == CB_KEEP_ALIVE:
            self.send(0x12, payload[:8])
        elif packet_id == CB_SYNC:
            x, y, z = struct.unpack_from(">ddd", payload, 0)
            teleport_id, _ = read_varint(payload, 33)
            self.position = (x, y, z)
            self.send(0x00, varint(teleport_id))
            self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([1]))
            self.note("sync", [x, y, z])
        elif packet_id == CB_RESPAWN:
            _, i = read_string(payload, 0)
            self.dimension, _ = read_string(payload, i)
            self.note("respawn", self.dimension)
        elif packet_id == CB_SPAWN_ENTITY:
            entity, i = read_varint(payload, 0)
            i += 16
            kind, i = read_varint(payload, i)
            x, y, z = struct.unpack_from(">ddd", payload, i)
            i += 24
            pitch, yaw, head = payload[i], payload[i + 1], payload[i + 2]
            i += 3
            data, i = read_varint(payload, i)
            vx, vy, vz = struct.unpack_from(">hhh", payload, i)
            name = TYPES.get(kind, str(kind))
            self.types[entity] = name
            if name in ("minecraft:ender_dragon", "minecraft:end_crystal", "minecraft:dragon_fireball",
                        "minecraft:area_effect_cloud"):
                self.note("spawn", entity, name, [x, y, z], yaw * 360 / 256, pitch * 360 / 256, data,
                          [vx / 8000, vy / 8000, vz / 8000])
            if name == "minecraft:ender_dragon":
                self.dragon = entity
                self.dragon_pos = [x, y, z]
        elif packet_id in (CB_MOVE, CB_MOVE_ROT):
            entity, i = read_varint(payload, 0)
            if entity in self.types and self.types[entity] != "minecraft:experience_orb":
                dx, dy, dz = struct.unpack_from(">hhh", payload, i)
                i += 6
                yaw = payload[i] * 360 / 256 if packet_id == CB_MOVE_ROT else None
                self.note("move", entity, [dx / 4096, dy / 4096, dz / 4096], yaw)
        elif packet_id == CB_ROT:
            entity, i = read_varint(payload, 0)
            if entity in self.types:
                self.note("rot", entity, payload[i] * 360 / 256, payload[i + 1] * 360 / 256)
        elif packet_id == CB_TELEPORT:
            entity, i = read_varint(payload, 0)
            if entity in self.types:
                x, y, z = struct.unpack_from(">ddd", payload, i)
                self.note("teleport", entity, [x, y, z], payload[i + 24] * 360 / 256)
        elif packet_id == CB_VELOCITY:
            entity, i = read_varint(payload, 0)
            if entity in self.types and self.types[entity] != "minecraft:item":
                vx, vy, vz = struct.unpack_from(">hhh", payload, i)
                self.note("velocity", entity, [vx / 8000, vy / 8000, vz / 8000])
        elif packet_id == CB_METADATA:
            entity, i = read_varint(payload, 0)
            if entity in self.types:
                fields = metadata(payload, i)
                self.note("meta", entity, {str(k): v for k, v in fields.items()})
        elif packet_id == CB_REMOVE:
            count, i = read_varint(payload, 0)
            gone = []
            for _ in range(count):
                entity, i = read_varint(payload, i)
                gone.append(entity)
            self.note("remove", [[e, self.types.get(e, "?")] for e in gone])
        elif packet_id == CB_EXPLOSION:
            x, y, z, power = struct.unpack_from(">dddf", payload, 0)
            records, j = read_varint(payload, 28)
            self.note("explosion", [x, y, z], power, records)
        elif packet_id == CB_SPAWN_ORB:
            entity, i = read_varint(payload, 0)
            x, y, z = struct.unpack_from(">ddd", payload, i)
            value = struct.unpack_from(">h", payload, i + 24)[0]
            self.types[entity] = "minecraft:experience_orb"
            self.note("orb", entity, [x, y, z], value)
        elif packet_id == CB_WORLD_EVENT:
            event = struct.unpack_from(">i", payload, 0)[0]
            pos, i = unpacked(payload, 4)
            data = struct.unpack_from(">i", payload, i)[0]
            self.note("world_event", event, list(pos), data)
        elif packet_id == CB_BOSS_BAR:
            self.note("boss_bar", payload.hex())
        elif packet_id == CB_ENTITY_EVENT:
            entity = struct.unpack_from(">i", payload, 0)[0]
            if entity in self.types:
                self.note("entity_event", entity, payload[4])
        elif packet_id == CB_SET_HEALTH:
            health = struct.unpack_from(">f", payload, 0)[0]
            self.note("health", health)
        elif packet_id == CB_BLOCK_UPDATE:
            pos, i = unpacked(payload, 0)
            state, _ = read_varint(payload, i)
            self.blocks[pos] = state
            if self.dimension == "minecraft:the_end":
                self.note("block", list(pos), state)
        elif packet_id == CB_SECTION_BLOCKS:
            self.section_blocks(payload)

    def section_blocks(self, payload: bytes) -> None:
        packed_section = struct.unpack_from(">q", payload, 0)[0]

        def signed(value: int, bits: int) -> int:
            return value - (1 << bits) if value >= 1 << (bits - 1) else value

        sx = signed((packed_section >> 42) & 0x3FFFFF, 22)
        sz = signed((packed_section >> 20) & 0x3FFFFF, 22)
        sy = signed(packed_section & 0xFFFFF, 20)

        def varlong(buf: bytes, i: int) -> tuple[int, int]:
            value, shift = 0, 0
            while True:
                byte = buf[i]
                i += 1
                value |= (byte & 0x7F) << shift
                shift += 7
                if not byte & 0x80:
                    return value, i

        for start in (8, 9):
            try:
                count, i = read_varint(payload, start)
                entries = []
                for _ in range(count):
                    value, i = varlong(payload, i)
                    entries.append(value)
            except IndexError:
                continue
            if i != len(payload):
                continue
            for value in entries:
                state = value >> 12
                pos = (sx * 16 + ((value >> 8) & 15), sy * 16 + (value & 15),
                       sz * 16 + ((value >> 4) & 15))
                self.blocks[pos] = state
                if self.dimension == "minecraft:the_end":
                    self.note("block", list(pos), state)
            return

    def stand(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.pump_once(0.05)
            if self.position is not None:
                x, y, z = self.position
                self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([1]))

    def use_on(self, x: int, y: int, z: int, face: int) -> None:
        self.sequence += 1
        self.send(SB_USE_ITEM_ON, varint(0) + packed(x, y, z) + varint(face)
                  + struct.pack(">fff", 0.5, 1.0 if face == 1 else 0.5, 0.5) + bytes([0])
                  + varint(self.sequence))
        self.pump_once(0.1)

    def attack(self, entity: int) -> None:
        # Interact: type 1 (attack), not sneaking; then the arm swing.
        self.send(SB_INTERACT, varint(entity) + varint(1) + bytes([0]))
        self.send(SB_SWING, varint(0))


#: The blocks the four respawn crystals are put on (the exit portal's rim,
#: the middle of each side), and where the probe stands to reach each one.
RESPAWN_CRYSTALS = (((3, 63, 0), (5.5, 70, 0.5)), ((-3, 63, 0), (-4.5, 70, 0.5)),
                    ((0, 63, 3), (0.5, 70, 5.5)), ((0, 63, -3), (0.5, 70, -4.5)))
#: Beside the fountain, clear of the exit portal: where the breath and the
#: death are watched from (dropped from y 70 onto the island).
FOUNTAIN_SPOT = (6.5, 70, 0.5)


def scenario(bot: DragonProbe, console, ours: bool, marks: dict, save_level) -> None:
    def mark(name: str) -> None:
        marks[name] = bot.now()
        bot.note("mark", name)

    def tp(x: float, y: float, z: float) -> None:
        # The console's `tp` lands in the console's own level on vanilla — the
        # overworld — unless told otherwise.
        console(f"tp {NAME} {x} {y} {z}" if ours else
                f"execute in minecraft:the_end run tp {NAME} {x} {y} {z}")

    # Into the End, on a platform of our own (the portal's is built by travel).
    wait = time.monotonic() + (900 if ours else 60)
    while bot.dragon is None and time.monotonic() < wait:
        bot.stand(0.5)
    mark("dragon_seen")
    if bot.dragon is None:
        return
    mark("with_crystals")
    bot.stand(90.0)

    # The crystals, one at a time, each hit by the probe standing on its
    # pillar (inside the cage where there is one): the explosion is heard.
    tp(*FOUNTAIN_SPOT)
    bot.stand(3.0)
    crystals = sorted({e[2]: tuple(e[4]) for e in bot.log
                       if e[1] == "spawn" and e[3] == "minecraft:end_crystal"}.items())
    gone = {g[0] for e in bot.log if e[1] == "remove" for g in e[2]}
    mark("crystals_destroyed")
    marks["crystals_known"] = len(crystals)
    for entity, (x, y, z) in crystals:
        if entity in gone:
            continue
        tp(round(x + 1.0, 2), round(y - 1.0, 2), round(z, 2))
        bot.stand(1.5)
        bot.attack(entity)
        bot.stand(5.0)
    mark("without_crystals")
    tp(*FOUNTAIN_SPOT)
    bot.stand(120.0)

    # The lethal hit, watched from beside the fountain (the orbs are in range).
    mark("lethal")
    if ours:
        # A head hit is whole: the probe is put beside the head and hits it
        # with a netherite sword until the dragon dies — the ordinary path, no
        # command damage (a bare hand does 1: 180 s of it took 38 of 200).
        console(f"give {NAME} minecraft:netherite_sword")
        bot.stand(1.0)
        bot.send(SB_SET_HELD_ITEM, struct.pack(">h", 0))
        hits = 0
        until = time.monotonic() + 180
        while time.monotonic() < until and not any(e[1] == "meta" and e[2] == bot.dragon and
                                                    e[3].get("9", 1.0) <= 0.0
                                                    for e in bot.log[-400:]):
            if bot.dragon_pos_now() is not None:
                x, y, z = bot.dragon_pos_now()
                console(f"tp {NAME} {x:.2f} {y + 1:.2f} {z:.2f}")
                bot.stand(0.3)
                bot.attack(bot.dragon + 1)
                hits += 1
            bot.stand(0.35)
        marks["hits"] = hits
        tp(*FOUNTAIN_SPOT)
    else:
        console(f"damage @e[type=minecraft:ender_dragon,limit=1] 1000 minecraft:player_attack by {NAME}")
    bot.stand(22.0)
    mark("dead")
    fight = save_level("after_death") or {}
    # The exit portal's height is the world's (63 at seed 1234567890).
    portal_y = (fight.get("ExitPortalLocation") or [0, 63, 0])[1]

    # The respawn: four crystals on the exit portal's sides, each put from two
    # blocks away (the use of an item is refused beyond reach). The sword goes
    # first, so the crystals land in the held slot.
    console(f"clear {NAME}")
    bot.stand(0.5)
    console(f"give {NAME} minecraft:end_crystal 4")
    bot.stand(1.0)
    bot.send(SB_SET_HELD_ITEM, struct.pack(">h", 0))
    for ((x, _, z), _), (sx, _, sz) in zip(RESPAWN_CRYSTALS, RESPAWN_STANDS):
        tp(sx, portal_y + 1, sz)
        bot.stand(2.0)
        bot.use_on(x, portal_y, z, 1)
        bot.stand(0.5)
    mark("respawn_placed")
    bot.stand(10.0)
    save_level("respawning")
    bot.stand(35.0)
    mark("respawn_end")
    save_level("respawned")


#: Where the probe stands to reach each respawn crystal: on the island, eye
#: within reach of the rim (a probe hovering at y 70 was out of reach twice).
RESPAWN_STANDS = ((5.5, 64.0, 0.5), (-4.5, 64.0, 0.5), (0.5, 64.0, 5.5), (0.5, 64.0, -4.5))


def scenario_respawn(bot: DragonProbe, console, ours: bool, marks: dict, save_level) -> None:
    """Only the respawn: the dragon removed with /kill (the portal opens at
    once), then four crystals, and 45 seconds of what follows."""
    def mark(name: str) -> None:
        marks[name] = bot.now()
        bot.note("mark", name)

    def tp(x: float, y: float, z: float) -> None:
        console(f"tp {NAME} {x} {y} {z}" if ours else
                f"execute in minecraft:the_end run tp {NAME} {x} {y} {z}")

    wait = time.monotonic() + (900 if ours else 60)
    while bot.dragon is None and time.monotonic() < wait:
        bot.stand(0.5)
    mark("dragon_seen")
    tp(*RESPAWN_STANDS[0])
    bot.stand(3.0)
    console("kill @e[type=minecraft:ender_dragon]")
    bot.stand(4.0)
    mark("dead")
    fight = save_level("after_death") or {}
    portal_y = (fight.get("ExitPortalLocation") or [0, 63, 0])[1]
    console(f"give {NAME} minecraft:end_crystal 4")
    bot.stand(1.0)
    bot.send(SB_SET_HELD_ITEM, struct.pack(">h", 0))
    for ((x, _, z), _), (sx, _, sz) in zip(RESPAWN_CRYSTALS, RESPAWN_STANDS):
        tp(sx, portal_y + 1, sz)
        bot.stand(1.5)
        bot.use_on(x, portal_y, z, 1)
        bot.stand(0.5)
    mark("respawn_placed")
    tp(*RESPAWN_STANDS[0])
    bot.stand(8.0)
    save_level("respawning")
    bot.stand(37.0)
    mark("respawn_end")
    save_level("respawned")


def dragon_pos_now(self) -> list[float] | None:
    """The dragon's position as the log has it, integrated from its moves."""
    pos = None
    for entry in self.log:
        if entry[1] == "spawn" and entry[2] == self.dragon:
            pos = list(entry[4])
        elif pos is not None and entry[1] == "move" and entry[2] == self.dragon:
            pos = [pos[k] + entry[3][k] for k in range(3)]
        elif pos is not None and entry[1] == "teleport" and entry[2] == self.dragon:
            pos = list(entry[3])
    return pos


DragonProbe.dragon_pos_now = dragon_pos_now

#: The scenario a run plays; `--respawn` swaps in `scenario_respawn`.
SCENARIO = [scenario]


def read_fight(path: Path) -> dict | None:
    if not path.exists():
        return None
    root = anvil_read.parse(gzip.decompress(path.read_bytes()))
    data = root.get("Data", root)
    fight = data.get("DragonFight")
    return json.loads(json.dumps(fight, default=str)) if fight is not None else None


class Vanilla(Server):
    EXTRA_PROPERTIES = (
        f"level-seed={SEED}\n"
        "level-type=minecraft\\:normal\n"
        "difficulty=normal\n"
        # The probe does not simulate its own fall: flung by a crystal's blast
        # it keeps reporting where it stood, and without this the server kicks
        # it for floating (measured: the first attempt ended that way).
        "allow-flight=true\n"
        "view-distance=8\nsimulation-distance=8\n"
        "max-tick-time=-1\n"
    )
    HEAP = "-Xmx1536M"


SETUP = ["gamerule doDaylightCycle false", "gamerule doMobSpawning false",
         "gamerule doImmediateRespawn true", "gamerule keepInventory true"]


def arrive(console) -> None:
    console(f"gamemode survival {NAME}")
    console(f"effect give {NAME} minecraft:resistance infinite 4 true")
    console(f"effect give {NAME} minecraft:regeneration infinite 4 true")
    console(f"effect give {NAME} minecraft:saturation infinite 0 true")


def run_vanilla() -> dict:
    directory = OUT / "vanilla-world"
    if directory.exists():
        shutil.rmtree(directory)
    server = Vanilla(directory, port=PORT)
    marks: dict = {}
    report: dict = {"seed": SEED, "marks": marks}
    world = directory / "world"

    def save_level(key: str) -> None:
        server.batch(["save-all flush"], timeout=120)
        report[f"dragon_fight_{key}"] = read_fight(world / "level.dat")
        return report[f"dragon_fight_{key}"]

    try:
        server.batch(SETUP)
        bot = DragonProbe(PORT)
        bot.stand(2.0)
        arrive(lambda c: server.batch([c]))
        # The ticket first; the fill a few seconds later, once the chunks are in.
        server.batch(["execute in minecraft:the_end run forceload add 96 -8 104 8"])
        bot.stand(4.0)
        server.batch(["execute in minecraft:the_end run fill 98 48 -2 102 48 2 minecraft:obsidian",
                      f"execute in minecraft:the_end run tp {NAME} 100.5 49 0.5 90 0"])
        try:
            SCENARIO[0](bot, lambda c: server.batch([c]), False, marks, save_level)
        except (EOFError, OSError) as error:
            report["error"] = f"connection lost: {error!r}"
        report["log"] = bot.log
        bot.socket.close()
        save_level("final")
    finally:
        server.send("stop")
        try:
            server.process.wait(timeout=90)
        except Exception:
            server.process.kill()
    report["dragon_fight_stopped"] = read_fight(world / "level.dat")
    shutil.rmtree(directory, ignore_errors=True)
    return report


def run_ours() -> dict:
    binary = ROOT / "build" / "macos-debug" / "bin" / "ov_dedicated"
    directory = OUT / "ours-world"
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    port = PORT + 1
    env = dict(os.environ, OV_WORLDGEN_WORKERS="2")
    process = subprocess.Popen(
        [str(binary), f"--world={directory / 'world'}", f"--port={port}", "--survival",
         "--log-level=info"],
        cwd=ROOT, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, bufsize=1)
    lines: queue.Queue[str] = queue.Queue()
    log = open(OUT / "ours-server.log", "w")

    def pump() -> None:
        assert process.stdout is not None
        for raw in process.stdout:
            lines.put(raw.rstrip("\n"))
            log.write(raw)
            log.flush()

    threading.Thread(target=pump, daemon=True).start()

    def console(command: str) -> None:
        assert process.stdin is not None
        process.stdin.write(command + "\n")
        process.stdin.flush()

    marks: dict = {}
    report: dict = {"seed": SEED, "marks": marks}
    world = directory / "world"

    def save_level(key: str) -> None:
        console("save-all")
        time.sleep(3.0)
        report[f"dragon_fight_{key}"] = read_fight(world / "level.dat")
        return report[f"dragon_fight_{key}"]

    try:
        deadline = time.monotonic() + 600
        bot = None
        while bot is None and time.monotonic() < deadline:
            try:
                candidate = DragonProbe(port)
                candidate.stand(3.0)
                if candidate.position is not None:
                    bot = candidate
                    break
                candidate.socket.close()
            except (OSError, EOFError):
                pass
            time.sleep(3.0)
        assert bot is not None, "never joined: see ours-server.log"
        for command in SETUP:
            console(command)
        arrive(console)
        # Our server has no `execute in`: the probe goes through a portal. A
        # floor is filled until its chunk is loaded (the probe's own ticket
        # brings it in), a 3 x 3 of end_portal is set on it, and the probe is
        # held in the slab (6/16 to 12/16) until the Respawn arrives.
        cx, cy, cz = 8, 100, 8
        console(f"tp {NAME} {cx + 0.5} {cy + 1} {cz + 0.5}")
        until = time.monotonic() + 300
        while time.monotonic() < until:
            bot.stand(1.0)
            while not lines.empty():
                lines.get_nowait()
            console(f"fill {cx - 2} {cy - 1} {cz - 2} {cx + 2} {cy - 1} {cz + 2} minecraft:stone")
            answered = time.monotonic() + 5
            done = False
            while time.monotonic() < answered and not done:
                bot.stand(0.2)
                while not lines.empty():
                    line = lines.get_nowait()
                    done = done or "filled" in line.lower() or "Successfully" in line
            if done:
                break
        console(f"fill {cx - 1} {cy} {cz - 1} {cx + 1} {cy} {cz + 1} minecraft:end_portal")
        bot.stand(1.0)
        until = time.monotonic() + 600
        while bot.dimension != "minecraft:the_end" and time.monotonic() < until:
            bot.position = (cx + 0.5, cy + 0.5, cz + 0.5)
            bot.stand(0.1)
        marks["crossed"] = bot.now()
        try:
            SCENARIO[0](bot, console, True, marks, save_level)
        except (EOFError, OSError) as error:
            report["error"] = f"connection lost: {error!r}"
        report["log"] = bot.log
        bot.socket.close()
        save_level("final")
    finally:
        try:
            console("stop")
        except (BrokenPipeError, OSError):
            pass
        try:
            process.wait(timeout=120)
        except Exception:
            process.kill()
        report["server_exit_code"] = process.returncode
        log.close()
    report["dragon_fight_stopped"] = read_fight(world / "level.dat")
    shutil.rmtree(directory, ignore_errors=True)
    return report


# ── Analysis ────────────────────────────────────────────────────────────────

PHASES = ["holding", "strafe", "approach", "landing", "takeoff", "sitting_flaming",
          "sitting_scanning", "sitting_attacking", "charging", "dying", "hover"]


def trajectory(report: dict) -> list[tuple[float, list[float], float | None]]:
    """(time, position, yaw) of the dragon at every packet that moved it."""
    dragon = None
    pos = None
    out = []
    for entry in report["log"]:
        kind = entry[1]
        if kind == "spawn" and entry[3] == "minecraft:ender_dragon":
            dragon = entry[2]
            pos = list(entry[4])
            out.append((entry[0], list(pos), entry[5]))
        elif dragon is not None and kind == "move" and entry[2] == dragon:
            pos = [pos[k] + entry[3][k] for k in range(3)]
            out.append((entry[0], list(pos), entry[4]))
        elif dragon is not None and kind == "teleport" and entry[2] == dragon:
            pos = list(entry[3])
            out.append((entry[0], list(pos), entry[4]))
    return out


def phases(report: dict) -> list[tuple[float, int]]:
    dragons = {e[2] for e in report["log"] if e[1] == "spawn" and e[3] == "minecraft:ender_dragon"}
    return [(e[0], e[3]["16"]) for e in report["log"]
            if e[1] == "meta" and e[2] in dragons and "16" in e[3]]


def window(samples, start: float, end: float):
    return [s for s in samples if start <= s[0] < end]


def stats(samples) -> dict:
    if len(samples) < 3:
        return {"samples": len(samples)}
    speeds, radii, heights = [], [], []
    for (t0, p0, _), (t1, p1, _) in zip(samples, samples[1:]):
        dt = t1 - t0
        if dt <= 0.02:
            continue
        ticks = max(1, round(dt * 20))
        speeds.append(math.dist(p0, p1) / ticks)
    for _, p, _ in samples:
        radii.append(math.hypot(p[0], p[2]))
        heights.append(p[1])

    def summary(values):
        values = sorted(values)
        n = len(values)
        return {"n": n, "mean": round(sum(values) / n, 3), "p10": round(values[n // 10], 3),
                "p50": round(values[n // 2], 3), "p90": round(values[9 * n // 10], 3),
                "max": round(values[-1], 3)}

    return {"speed_per_tick": summary(speeds), "radius": summary(radii), "height": summary(heights)}


def analyse_one(report: dict) -> dict:
    marks = report["marks"]
    samples = trajectory(report)
    out: dict = {"marks": marks}
    if "with_crystals" in marks:
        out["flight_with_crystals"] = stats(window(samples, marks["with_crystals"],
                                                   marks["crystals_destroyed"]))
    if "without_crystals" in marks:
        out["flight_without_crystals"] = stats(window(samples, marks["without_crystals"],
                                                      marks["lethal"]))
    changes = phases(report)
    out["phase_sequence"] = [[round(t, 2), PHASES[p] if 0 <= p < len(PHASES) else p]
                             for t, p in changes]
    explosions = [e for e in report["log"] if e[1] == "explosion"]
    out["explosions"] = [[round(e[0], 2), [round(v, 2) for v in e[2]], e[3], e[4]] for e in explosions]
    orbs = [e for e in report["log"] if e[1] == "orb"]
    out["orbs"] = {"count": len(orbs), "total": sum(e[4] for e in orbs),
                   "values": [e[4] for e in orbs][:40],
                   "first": round(orbs[0][0], 2) if orbs else None,
                   "last": round(orbs[-1][0], 2) if orbs else None}
    dragons = {e[2] for e in report["log"] if e[1] == "spawn" and e[3] == "minecraft:ender_dragon"}
    removed = [e for e in report["log"] if e[1] == "remove" and any(g[0] in dragons for g in e[2])]
    dying = [t for t, p in changes if p == 9]
    out["dying_started"] = round(dying[0], 2) if dying else None
    out["dragon_removed"] = [round(e[0], 2) for e in removed]
    healths = [(e[0], e[3]["9"]) for e in report["log"]
               if e[1] == "meta" and e[2] in dragons and "9" in e[3]]
    out["health_changes"] = [[round(t, 2), round(h, 2)] for t, h in healths][:120]
    out["fireballs"] = [round(e[0], 2) for e in report["log"]
                        if e[1] == "spawn" and e[3] == "minecraft:dragon_fireball"]
    out["clouds"] = [[round(e[0], 2), [round(v, 2) for v in e[4]]] for e in report["log"]
                     if e[1] == "spawn" and e[3] == "minecraft:area_effect_cloud"]
    beams = [(e[0], e[2], e[3]["8"]) for e in report["log"] if e[1] == "meta" and "8" in e[3]
             and report_type(report, e[2]) == "minecraft:end_crystal"]
    out["beams"] = [[round(t, 2), c, b] for t, c, b in beams][:80]
    out["world_events"] = [[round(e[0], 2), e[2], e[3], e[4]] for e in report["log"]
                           if e[1] == "world_event"]
    for key in ("dragon_fight_after_death", "dragon_fight_final", "dragon_fight_stopped"):
        out[key] = report.get(key)
    return out


def report_type(report: dict, entity: int) -> str:
    for e in report["log"]:
        if e[1] == "spawn" and e[2] == entity:
            return e[3]
    return "?"


def analyse() -> int:
    for name in ("vanilla", "ours"):
        path = OUT / f"{name}.json"
        if not path.exists():
            print(f"{name}: no run")
            continue
        summary = analyse_one(json.loads(path.read_text()))
        (OUT / f"{name}-summary.json").write_text(json.dumps(summary, indent=1))
        print(f"── {name}")
        print(json.dumps({k: v for k, v in summary.items()
                          if k not in ("health_changes", "beams")}, indent=1)[:12000])
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ours", action="store_true")
    parser.add_argument("--analyse", action="store_true")
    parser.add_argument("--respawn", action="store_true", help="only the respawn")
    parser.add_argument("--out", help="file name under .scratch/dragon (default vanilla.json / ours.json)")
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    if args.analyse:
        return analyse()
    if args.respawn:
        SCENARIO[0] = scenario_respawn
    report = run_ours() if args.ours else run_vanilla()
    name = args.out or ("ours.json" if args.ours else "vanilla.json")
    (OUT / name).write_text(json.dumps(report))
    print(f"wrote {OUT / name}: {len(report.get('log', []))} log entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
