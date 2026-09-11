#!/usr/bin/env python3
"""End portals, measured on the real 1.20.1 server: eyes, activation, arrival, death.

The console builds a ring of twelve End portal frames, each facing the inside,
without eyes, on a floor of stone in the overworld. A probe client in survival
is given twelve eyes of ender and puts them in one Use Item On at a time,
recording every Block Update and World Event: the frame's new state, the
portal blocks when the ring completes, and the events the game plays.

The probe then stands in the portal, reporting its position every tick, and
records the Respawn into minecraft:the_end and the position the game puts it
at; it stays a few seconds in the End, recording the Boss Bar and the entities
it is shown (the dragon, the crystals) as raw payloads; it is killed from the
console, asks to respawn (Client Command 0), and records where it comes back.

After the server stops, DIM1/region is read round the arrival (the platform)
and round the origin (the exit portal the dragon fight builds), and level.dat's
DragonFight is dumped.

The same scenario runs against our own server with `--ours`.

Usage:
  lockf /tmp/ov-vanilla.lock python3 scripts/measure_end_portal.py   # -> .scratch/end-portal/vanilla.json
  python3 scripts/measure_end_portal.py --ours                        # -> .scratch/end-portal/ours.json
"""
from __future__ import annotations

import argparse
import gzip
import json
import os
import queue
import shutil
import socket
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
OUT = ROOT / ".scratch" / "end-portal"
PORT = 25693
NAME = "ovender"
SEED = 1234567890

# The ring: centre of the 3 x 3 inside, at the frames' height.
CENTRE = (8, 100, 8)

CB_BLOCK_UPDATE = 0x0A
CB_BOSS_BAR = 0x0B
CB_SPAWN_ENTITY = 0x01
CB_WORLD_EVENT = 0x25
CB_RESPAWN = 0x41
CB_SYNC = 0x3C
CB_CHUNK = 0x24
CB_GAME_EVENT = 0x1F
CB_COMBAT_DEATH = 0x38
CB_SPAWN_ORB = 0x02
SB_POSITION = 0x14
SB_USE_ITEM_ON = 0x31
SB_SET_HELD_ITEM = 0x28
SB_CLIENT_COMMAND = 0x07


def load_state_names() -> dict[int, str]:
    """Wire state id -> "name[props]", from the data generator's report."""
    path = ROOT / "data" / "vanilla" / "1.20.1" / "generated" / "reports" / "blocks.json"
    names: dict[int, str] = {}
    if path.exists():
        for name, block in json.loads(path.read_text()).items():
            for state in block["states"]:
                props = ",".join(f"{k}={v}" for k, v in sorted(state.get("properties", {}).items()))
                names[state["id"]] = name + (f"[{props}]" if props else "")
    return names


STATE_NAMES = load_state_names()


class Vanilla(Server):
    EXTRA_PROPERTIES = (
        f"level-seed={SEED}\n"
        "level-type=minecraft\\:normal\n"
        "view-distance=8\nsimulation-distance=8\n"
        "spawn-npcs=false\nspawn-animals=false\nspawn-monsters=false\n"
        "max-tick-time=-1\n"
    )
    HEAP = "-Xmx1536M"


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


def ring(cx: int, y: int, cz: int) -> list[tuple[int, int, int, str]]:
    """The twelve frames round (cx, y, cz), each facing the inside."""
    slots = []
    for k in (-1, 0, 1):
        slots.append((cx + k, y, cz - 2, "south"))
        slots.append((cx + k, y, cz + 2, "north"))
        slots.append((cx - 2, y, cz + k, "east"))
        slots.append((cx + 2, y, cz + k, "west"))
    return slots


class Ender(Probe):
    """A probe that reports its position every tick and keeps what it hears."""

    def __init__(self, port: int) -> None:
        super().__init__(port, name=NAME)
        self.dimension = "minecraft:overworld"
        self.events: list[tuple[str, object]] = []
        self.blocks: dict[tuple[int, int, int], int] = {}
        self.world_events: list[tuple[int, tuple[int, int, int], int]] = []
        self.boss_bars: list[str] = []
        self.spawned: list[str] = []
        self.game_events: list[tuple[int, float]] = []
        self.ids_in: dict[str, dict[int, int]] = {}
        self.orbs: list[tuple[float, float, float, int]] = []
        self.history: list[int] = []
        self.sequence = 0
        self.dead = False

    def pump_once(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.socket.settimeout(max(0.005, deadline - time.monotonic()))
            try:
                packet_id, payload = self.read()
            except (TimeoutError, OSError):
                return
            histogram = self.ids_in.setdefault(self.dimension, {})
            histogram[packet_id] = histogram.get(packet_id, 0) + 1
            self.history.append(packet_id)
            if packet_id == 0x23:
                self.send(0x12, payload[:8])
            elif packet_id == CB_BLOCK_UPDATE:
                pos, i = unpacked(payload, 0)
                state, _ = read_varint(payload, i)
                self.blocks[pos] = state
            elif packet_id == CB_WORLD_EVENT:
                event = struct.unpack_from(">i", payload, 0)[0]
                pos, i = unpacked(payload, 4)
                data = struct.unpack_from(">i", payload, i)[0]
                self.world_events.append((event, pos, data))
            elif packet_id == CB_BOSS_BAR and len(self.boss_bars) < 12:
                self.boss_bars.append(payload.hex())
            elif packet_id == CB_SPAWN_ENTITY and self.dimension == "minecraft:the_end":
                if len(self.spawned) < 40:
                    self.spawned.append(payload.hex())
            elif packet_id == CB_SPAWN_ORB:
                entity, i = read_varint(payload, 0)
                x, y, z = struct.unpack_from(">ddd", payload, i)
                value = struct.unpack_from(">h", payload, i + 24)[0]
                self.orbs.append((x, y, z, value))
            elif packet_id == CB_GAME_EVENT:
                event = payload[0]
                value = struct.unpack_from(">f", payload, 1)[0]
                self.game_events.append((event, value))
            elif packet_id == CB_COMBAT_DEATH:
                self.dead = True
                self.events.append(("death", self.dimension))
            elif packet_id == CB_RESPAWN:
                dim_type, i = read_string(payload, 0)
                dim_name, i = read_string(payload, i)
                self.dimension = dim_name
                self.events.append(("respawn", (dim_type, dim_name, payload.hex())))
            elif packet_id == CB_SYNC:
                x, y, z = struct.unpack_from(">ddd", payload, 0)
                yaw, pitch = struct.unpack_from(">ff", payload, 24)
                teleport_id, _ = read_varint(payload, 33)
                self.position = (x, y, z)
                self.events.append(("sync", (self.dimension, x, y, z, yaw, pitch)))
                self.send(0x00, varint(teleport_id))
                self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([1]))

    def stand(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.pump_once(0.05)
            if self.position is not None and not self.dead:
                x, y, z = self.position
                self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([1]))

    def wait_for_dimension(self, dimension: str, timeout: float):
        mark = len(self.events)
        deadline = time.monotonic() + timeout
        seen = None
        while time.monotonic() < deadline:
            self.stand(0.05)
            for kind, value in self.events[mark:]:
                if kind == "respawn" and value[1] == dimension:
                    seen = value
                if kind == "sync" and seen is not None and value[0] == dimension:
                    return seen, value
        return None

    def hold(self, slot: int) -> None:
        self.send(SB_SET_HELD_ITEM, struct.pack(">h", slot))

    def use_on(self, x: int, y: int, z: int, face: int) -> None:
        self.sequence += 1
        self.send(SB_USE_ITEM_ON, varint(0) + packed(x, y, z) + varint(face)
                  + struct.pack(">fff", 0.5, 1.0 if face == 1 else 0.5, 0.5) + bytes([0])
                  + varint(self.sequence))
        self.pump_once(0.15)


def scenario(bot: Ender, console, report: dict) -> None:
    """The part both servers run: eyes, portal, the End, death, return."""
    cx, cy, cz = CENTRE
    slots = ring(cx, cy, cz)
    frame_states: list[int | None] = []
    activated_after = None
    for index, (x, y, z, facing) in enumerate(slots):
        # One eye at a time, waiting for the frame's Block Update before the
        # next: a server that reads the twelve in one burst and takes more than
        # thirty seconds over them (a Debug build relighting after every write
        # on a loaded machine) trips its own idle timer, which is armed per
        # read, and closes the connection without a word.
        before = bot.blocks.get((x, y, z))
        bot.use_on(x, y, z, 1)
        deadline = time.monotonic() + 90
        while bot.blocks.get((x, y, z)) == before and time.monotonic() < deadline:
            bot.stand(0.1)
        bot.stand(0.25)
        frame_states.append(bot.blocks.get((x, y, z)))
        inside = [bot.blocks.get((cx + dx, cy, cz + dz)) for dx in (-1, 0, 1) for dz in (-1, 0, 1)]
        if activated_after is None and all(s not in (None, 0) for s in inside):
            activated_after = index + 1
    # The portal's own Block Updates may trail the last frame's on a slow
    # server: wait for them, and credit them to the last eye.
    deadline = time.monotonic() + 60
    while activated_after is None and time.monotonic() < deadline:
        bot.stand(0.1)
        inside = [bot.blocks.get((cx + dx, cy, cz + dz)) for dx in (-1, 0, 1) for dz in (-1, 0, 1)]
        if all(s not in (None, 0) for s in inside):
            activated_after = len(slots)
    report["frame_states_after_eye"] = frame_states
    report["portal_after_eyes"] = activated_after
    report["portal_states"] = [bot.blocks.get((cx + dx, cy, cz + dz))
                               for dx in (-1, 0, 1) for dz in (-1, 0, 1)]
    report["world_events"] = [list(e[:1]) + [list(e[1]), e[2]] for e in bot.world_events]

    # Into the portal: the slab is 6/16 to 12/16 of the block.
    def enter(key: str) -> bool:
        # Held in the portal against any late Synchronize Player Position
        # from an earlier teleport: a slow server's sync arriving after the
        # step in would put the probe back on top of the portal for good.
        target = (cx + 0.5, cy + 0.5, cz + 0.5)
        stepped_in = time.monotonic()
        mark = len(bot.events)
        arrived = None
        seen = None
        # Ten minutes: our Debug server generating the End's first blocks on a
        # loaded machine; vanilla answers in under a second.
        while arrived is None and time.monotonic() - stepped_in < 600.0:
            if bot.dimension == "minecraft:overworld":
                bot.position = target
            bot.stand(0.05)
            for kind, value in bot.events[mark:]:
                if kind == "respawn" and value[1] == "minecraft:the_end":
                    seen = value
                if kind == "sync" and seen is not None and value[0] == "minecraft:the_end":
                    arrived = (seen, value)
                    break
        report[key] = None
        if arrived is None:
            return False
        report[key + "_seconds"] = round(time.monotonic() - stepped_in, 2)
        respawn, sync = arrived
        report[key + "_respawn"] = {"type": respawn[0], "name": respawn[1], "payload": respawn[2]}
        report[key] = list(sync[1:6])
        return True

    if not enter("end_arrival"):
        return
    bot.stand(12.0)
    report["boss_bars"] = bot.boss_bars
    report["end_spawned_entities"] = bot.spawned
    report["packet_ids_in_end"] = {hex(k): v for k, v in
                                   sorted(bot.ids_in.get("minecraft:the_end", {}).items())}

    # Death, and the way back.
    console(f"kill {NAME}")
    deadline = time.monotonic() + 20
    while not bot.dead and time.monotonic() < deadline:
        bot.stand(0.1)
    report["died_in_end"] = bot.dead
    bot.send(SB_CLIENT_COMMAND, varint(0))
    back = bot.wait_for_dimension("minecraft:overworld", 60.0)
    if back is not None:
        respawn, sync = back
        report["respawn_after_death"] = {"type": respawn[0], "name": respawn[1],
                                         "payload": respawn[2]}
        report["overworld_return"] = list(sync[1:6])
    bot.dead = False
    bot.stand(2.0)

    # Back through the same portal: the platform is rebuilt; then the dragon is
    # killed from the console and the exit portal opens; then out through it.
    console(f"tp {NAME} {cx + 0.5} {cy + 1} {cz + 0.5} 0 90")
    bot.stand(1.0)
    if not enter("second_arrival"):
        return
    bot.stand(3.0)
    mark_blocks = dict(bot.blocks)
    orbs_before = len(bot.orbs)
    console("kill @e[type=minecraft:ender_dragon]")
    bot.stand(16.0)
    report["xp_orbs_after_kill"] = bot.orbs[orbs_before:]
    report["xp_total_after_kill"] = sum(o[3] for o in bot.orbs[orbs_before:])
    changed = {pos: state for pos, state in bot.blocks.items() if mark_blocks.get(pos) != state}
    report["blocks_changed_after_kill"] = sorted([list(pos) + [state, STATE_NAMES.get(state, "?")]
                                                  for pos, state in changed.items()])
    exit_portal = sorted(pos for pos, state in changed.items()
                         if STATE_NAMES.get(state, "").startswith("minecraft:end_portal"))
    report["exit_portal_blocks"] = [list(p) for p in exit_portal]
    report["game_events"] = list(bot.game_events)
    if exit_portal:
        x, y, z = exit_portal[0]
        before = len(bot.game_events)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline and not any(e[0] == 4 for e in bot.game_events[before:]):
            bot.position = (x + 0.5, y + 0.5, z + 0.5)  # held, as in `enter`
            bot.stand(0.1)
        report["win_game_events"] = bot.game_events[before:]
        bot.send(SB_CLIENT_COMMAND, varint(0))
        home = bot.wait_for_dimension("minecraft:overworld", 60.0)
        if home is not None:
            respawn, sync = home
            report["respawn_after_exit"] = {"type": respawn[0], "name": respawn[1],
                                            "payload": respawn[2]}
            report["exit_return"] = list(sync[1:6])
    bot.stand(2.0)


def scan(region_dir: Path, box: tuple[int, int, int, int, int, int], skip: set[str]) -> list:
    """Every block in the box whose name is not in `skip`: [x, y, z, name, props]."""
    x0, y0, z0, x1, y1, z1 = box
    found = []
    by_region: dict = {}
    for cx in range(x0 >> 4, (x1 >> 4) + 1):
        for cz in range(z0 >> 4, (z1 >> 4) + 1):
            by_region.setdefault((cx >> 5, cz >> 5), set()).add((cx, cz))
    for (rx, rz), wanted in by_region.items():
        path = region_dir / f"r.{rx}.{rz}.mca"
        if not path.exists():
            continue
        for _, _, nbt in anvil_read.chunks(str(path)):
            if (nbt["xPos"], nbt["zPos"]) not in wanted:
                continue
            for section in nbt.get("sections", []):
                states = section.get("block_states")
                if not states:
                    continue
                palette = states["palette"]
                data = states.get("data")
                bits = max(4, (len(palette) - 1).bit_length()) if len(palette) > 1 else 0
                for cell in range(4096):
                    y = section["Y"] * 16 + cell // 256
                    x = nbt["xPos"] * 16 + cell % 16
                    z = nbt["zPos"] * 16 + (cell // 16) % 16
                    if not (x0 <= x <= x1 and y0 <= y <= y1 and z0 <= z <= z1):
                        continue
                    if bits == 0:
                        which = 0
                    else:
                        per = 64 // bits
                        word = data[cell // per] & ((1 << 64) - 1)
                        which = (word >> ((cell % per) * bits)) & ((1 << bits) - 1)
                    name = palette[which]["Name"]
                    if name in skip:
                        continue
                    found.append([x, y, z, name, palette[which].get("Properties", {})])
    found.sort()
    return found


def after_stop(world: Path, report: dict) -> None:
    end = world / "DIM1" / "region"
    report["platform"] = scan(end, (96, 47, -4, 104, 53, 4), {"minecraft:end_stone"})
    report["origin_column"] = scan(end, (-4, 40, -4, 4, 90, 4), {"minecraft:end_stone",
                                                                  "minecraft:air"})
    level = world / "level.dat"
    if level.exists():
        root = anvil_read.parse(gzip.decompress(level.read_bytes()))
        data = root.get("Data", root)
        fight = data.get("DragonFight") or data.get("WorldGenSettings", {}).get("DragonFight")
        report["dragon_fight"] = json.loads(json.dumps(fight, default=str)) if fight else None


def run_vanilla() -> dict:
    directory = OUT / "vanilla-world"
    if directory.exists():
        shutil.rmtree(directory)
    server = Vanilla(directory, port=PORT)
    report: dict = {"seed": SEED, "centre": list(CENTRE)}
    try:
        cx, cy, cz = CENTRE
        commands = ["gamerule doDaylightCycle false", "gamerule doMobSpawning false",
                    "gamerule doImmediateRespawn false",
                    f"forceload add {cx - 16} {cz - 16} {cx + 16} {cz + 16}",
                    f"fill {cx - 3} {cy - 1} {cz - 3} {cx + 3} {cy - 1} {cz + 3} minecraft:stone"]
        for (x, y, z, facing) in ring(cx, cy, cz):
            commands.append(f"setblock {x} {y} {z} minecraft:end_portal_frame[facing={facing},eye=false]")
        server.batch(commands)
        bot = Ender(PORT)
        bot.stand(2.0)
        server.batch([f"gamemode survival {NAME}", f"clear {NAME}",
                      f"give {NAME} minecraft:ender_eye 12",
                      f"tp {NAME} {cx + 0.5} {cy + 1} {cz + 0.5} 0 90"])
        bot.hold(0)
        bot.stand(1.5)
        scenario(bot, lambda c: server.batch([c]), report)
        bot.socket.close()
        server.batch(["save-all flush"], timeout=120)
    finally:
        server.send("stop")
        try:
            server.process.wait(timeout=90)
        except Exception:
            server.process.kill()
    after_stop(directory / "world", report)
    shutil.rmtree(directory, ignore_errors=True)
    return report


def run_ours(seeded: bool) -> dict:
    """Our server, on a superflat overworld by default: the End is generated
    from the world seed whatever the overworld is, and a Debug build on a
    loaded machine took more than five minutes to prepare a generated spawn
    area. `--seeded` generates the overworld at SEED too."""
    binary = ROOT / "build" / "macos-debug" / "bin" / "ov_dedicated"
    directory = OUT / "ours-world"
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    port = PORT + 1
    env = dict(os.environ, OV_WORLDGEN_WORKERS="2")
    if seeded:
        env["OV_WORLDGEN_SEED"] = str(SEED)
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

    def send(*commands: str) -> None:
        assert process.stdin is not None
        process.stdin.write("".join(c + "\n" for c in commands))
        process.stdin.flush()

    report: dict = {"seed": SEED, "centre": list(CENTRE)}
    try:
        deadline = time.monotonic() + 600
        bot = None
        while bot is None and time.monotonic() < deadline:
            try:
                candidate = Ender(port)
                candidate.stand(3.0)
                if candidate.position is not None:
                    bot = candidate
                    break
                candidate.socket.close()
            except (OSError, EOFError):
                pass
            time.sleep(3.0)
        assert bot is not None, "never joined: see ours-server.log"
        cx, cy, cz = CENTRE
        send(f"tp {NAME} {cx + 0.5} {cy + 1} {cz + 0.5} 0 90")
        # Our console has no forceload: the probe's own ticket loads the ring.
        loaded = False
        until = time.monotonic() + 300
        while not loaded and time.monotonic() < until:
            bot.stand(1.0)
            while not lines.empty():
                lines.get_nowait()
            send(f"fill {cx - 3} {cy - 1} {cz - 3} {cx + 3} {cy - 1} {cz + 3} minecraft:stone")
            answer = time.monotonic() + 5
            while time.monotonic() < answer:
                bot.stand(0.2)
                seen = []
                while not lines.empty():
                    seen.append(lines.get_nowait())
                if any("not loaded" in line for line in seen):
                    break
                if any("filled" in line.lower() or "Successfully" in line for line in seen):
                    loaded = True
                    break
        for (x, y, z, facing) in ring(cx, cy, cz):
            send(f"setblock {x} {y} {z} minecraft:end_portal_frame[facing={facing},eye=false]")
        send(f"clear {NAME}", f"give {NAME} minecraft:ender_eye 12",
             f"tp {NAME} {cx + 0.5} {cy + 1} {cz + 0.5} 0 90")
        bot.hold(0)
        bot.stand(2.0)
        try:
            scenario(bot, lambda c: send(c), report)
        except (EOFError, OSError) as error:
            # The server closed the connection: said, with what came last, and
            # the report is still written.
            report["error"] = f"connection lost: {error!r}"
            report["last_packets"] = [hex(p) for p in bot.history[-20:]]
        bot.socket.close()
        time.sleep(1.0)
    finally:
        try:
            send("stop")
        except (BrokenPipeError, OSError):
            pass
        try:
            process.wait(timeout=120)
        except Exception:
            process.kill()
        report["server_exit_code"] = process.returncode
        log.close()
    after_stop(directory / "world", report)
    shutil.rmtree(directory, ignore_errors=True)
    return report


def compare() -> int:
    """Our server's run against the game's, check by check."""
    vanilla = json.loads((OUT / "vanilla.json").read_text())
    ours = json.loads((OUT / "ours.json").read_text())
    checks: list[tuple[str, bool, str]] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        checks.append((name, ok, detail))
        print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}")

    def named(states):
        return [STATE_NAMES.get(s, str(s)) for s in states or []]

    check("each eye sets eye=true on its frame, same states",
          named(vanilla["frame_states_after_eye"]) == named(ours["frame_states_after_eye"]),
          f"ours {named(ours['frame_states_after_eye'])[:2]}…")
    check("the portal opens on the twelfth eye, not before",
          vanilla["portal_after_eyes"] == ours["portal_after_eyes"] == 12,
          f"vanilla {vanilla['portal_after_eyes']} ours {ours['portal_after_eyes']}")
    check("3 x 3 end_portal", named(vanilla["portal_states"]) == named(ours["portal_states"]))
    check("world events (1503 per eye, 1038 on opening)",
          vanilla["world_events"] == ours["world_events"],
          f"vanilla {len(vanilla['world_events'])} ours {len(ours['world_events'])}")
    for key in ("end_arrival", "second_arrival"):
        check(f"{key}: position, yaw, pitch", vanilla.get(key) == ours.get(key) and ours.get(key),
              f"vanilla {vanilla.get(key)} ours {ours.get(key)}")
    check("Respawn into minecraft:the_end, of type minecraft:the_end",
          (ours.get("end_arrival_respawn") or {}).get("type") == "minecraft:the_end" and
          (ours.get("end_arrival_respawn") or {}).get("name") == "minecraft:the_end")
    check("the platform: 25 obsidian at y 49, nothing else round it",
          vanilla["platform"] == ours["platform"],
          f"vanilla {len(vanilla['platform'])} blocks ours {len(ours['platform'])}")
    check("death in the End", ours.get("died_in_end") is True)
    check("respawn after death names the overworld",
          (ours.get("respawn_after_death") or {}).get("name") == "minecraft:overworld")
    failed = [n for n, ok, _ in checks if not ok]
    print(f"\n{len(checks) - len(failed)} / {len(checks)} checks pass")
    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ours", action="store_true")
    parser.add_argument("--compare", action="store_true")
    parser.add_argument("--seeded", action="store_true")
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    if args.compare:
        return compare()
    report = run_ours(args.seeded) if args.ours else run_vanilla()
    name = "ours.json" if args.ours else "vanilla.json"
    (OUT / name).write_text(json.dumps(report, indent=1))
    brief = {k: v for k, v in report.items()
             if k not in ("end_spawned_entities", "boss_bars", "origin_column")}
    print(json.dumps(brief, indent=1)[:6000])
    return 0


if __name__ == "__main__":
    sys.exit(main())
