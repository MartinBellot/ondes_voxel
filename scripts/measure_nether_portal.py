#!/usr/bin/env python3
"""Nether portals, measured on the real 1.20.1 server: where a player arrives.

For each start position the console builds a 4x5 obsidian frame (inside 2x3)
in the overworld, lights it with a fire block, and teleports a probe client
into it. The probe keeps sending its position every tick — a server only
notices a player is inside a portal when it processes a movement packet — and
records the Respawn packet (dimension) and the Synchronize Player Position that
follows it: the arrival. Then it steps out of the new portal, waits out the
300-tick cooldown, walks back in, and records where it lands in the overworld.

After all starts the server is stopped and the Nether's region files are read:
every nether_portal block within 24 blocks of each arrival, with its axis, and
the obsidian around it — the portal the game *created*.

The same scenario runs against our own server with `--ours`, and the two
reports are compared by `--compare`.

Usage:
  python3 scripts/measure_nether_portal.py            # vanilla -> .scratch/nether-portal/vanilla.json
  python3 scripts/measure_nether_portal.py --ours     # ov_dedicated -> .scratch/nether-portal/ours.json
  python3 scripts/measure_nether_portal.py --compare
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import anvil_read  # noqa: E402
from capture_entity_packets import Probe, read_varint, varint  # noqa: E402
from measure_entities import Server  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / ".scratch" / "nether-portal"
PORT = 25677
NAME = "ovportal"
SEED = 1234567890

# (x, y, z, axis) of the frame's bottom-left *inside* block. Chosen spread out
# so the Nether targets (x/8, y, z/8) fall in different terrain, at three
# heights so a search finds different floors, and both axes.
STARTS = [
    (0, 100, 0, "x"),
    (1000, 70, -600, "x"),
    (-2500, 90, 1800, "z"),
    (4000, 120, 4000, "z"),
    (-800, 40, -3200, "x"),
    (2400, 64, 3100, "z"),
]

CB_RESPAWN = 0x41
CB_SYNC = 0x3C
SB_POSITION = 0x14


class Vanilla(Server):
    EXTRA_PROPERTIES = (
        f"level-seed={SEED}\n"
        "level-type=minecraft\\:normal\n"
        "view-distance=4\nsimulation-distance=4\n"
        "allow-nether=true\n"
        "spawn-npcs=false\nspawn-animals=false\nspawn-monsters=false\n"
        "max-tick-time=-1\n"
    )
    HEAP = "-Xmx1536M"


def read_string(buf: bytes, i: int) -> tuple[str, int]:
    n, i = read_varint(buf, i)
    return buf[i:i + n].decode(), i + n


class Walker(Probe):
    """A probe that stands where it is told and says so every tick."""

    def __init__(self, port: int) -> None:
        super().__init__(port, name=NAME)
        self.dimension = "minecraft:overworld"
        self.events: list[tuple[str, object]] = []
        # Chunk Data and Update Light payloads received in the Nether: the
        # terrain as it was when the portal was built, before anything ticked.
        self.nether_chunks: list[bytes] = []

    def pump_once(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.socket.settimeout(max(0.005, deadline - time.monotonic()))
            try:
                packet_id, payload = self.read()
            except (TimeoutError, OSError):
                return
            if packet_id == 0x23:
                self.send(0x12, payload[:8])
            elif packet_id == 0x24 and self.dimension == "minecraft:the_nether":
                self.nether_chunks.append(payload)
            elif packet_id == CB_RESPAWN:
                dim_type, i = read_string(payload, 0)
                dim_name, i = read_string(payload, i)
                self.dimension = dim_name
                self.events.append(("respawn", dim_name))
            elif packet_id == CB_SYNC:
                x, y, z = struct.unpack_from(">ddd", payload, 0)
                yaw, pitch = struct.unpack_from(">ff", payload, 24)
                flags = payload[32]
                teleport_id, _ = read_varint(payload, 33)
                self.position = (x, y, z)
                self.events.append(("sync", (self.dimension, x, y, z, yaw, flags)))
                self.send(0x00, varint(teleport_id))
                self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([1]))

    def stand(self, seconds: float) -> None:
        """Keep reporting the current position, every 50 ms."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.pump_once(0.05)
            if self.position is not None:
                x, y, z = self.position
                self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([1]))

    def wait_for_dimension(self, dimension: str, timeout: float) -> tuple | None:
        """Stand until a Respawn into `dimension` and the sync after it."""
        mark = len(self.events)
        deadline = time.monotonic() + timeout
        seen_respawn = False
        while time.monotonic() < deadline:
            self.stand(0.1)
            for kind, value in self.events[mark:]:
                if kind == "respawn" and value == dimension:
                    seen_respawn = True
                if kind == "sync" and seen_respawn and value[0] == dimension:
                    return value
        return None


def frame_commands(x: int, y: int, z: int, axis: str, dimension: str) -> list[str]:
    """Obsidian 4x5 around an inside of 2x3 at (x, y, z), then the fire."""
    run = f"execute in {dimension} run "
    if axis == "x":
        outer = f"{x - 1} {y - 1} {z} {x + 2} {y + 3} {z}"
        inner = f"{x} {y} {z} {x + 1} {y + 2} {z}"
    else:
        outer = f"{x} {y - 1} {z - 1} {x} {y + 3} {z + 2}"
        inner = f"{x} {y} {z} {x} {y + 2} {z + 1}"
    return [
        run + f"forceload add {x - 16} {z - 16} {x + 16} {z + 16}",
        run + f"fill {outer} minecraft:obsidian",
        run + f"fill {inner} minecraft:air",
        run + f"setblock {x} {y} {z} minecraft:fire",
    ]


def centre_of(x: int, y: int, z: int, axis: str) -> tuple[float, float, float]:
    """The middle of a 2-wide inside, on its floor."""
    return (x + 1.0, float(y), z + 0.5) if axis == "x" else (x + 0.5, float(y), z + 1.0)


def run_vanilla() -> dict:
    directory = ROOT / "run" / "nether-portal-oracle"
    if directory.exists():
        shutil.rmtree(directory)
    server = Vanilla(directory, port=PORT)
    report: dict = {"seed": SEED, "starts": []}
    try:
        server.batch(["gamerule doDaylightCycle false", "gamerule doMobSpawning false",
                      "gamerule doFireTick false"])
        walker = Walker(PORT)
        walker.stand(2.0)
        server.batch([f"gamemode creative {NAME}"])
        walker.stand(0.5)
        for (x, y, z, axis) in STARTS:
            entry: dict = {"start": [x, y, z], "axis": axis}
            lines = server.batch(frame_commands(x, y, z, axis, "minecraft:overworld") + [
                f"execute in minecraft:overworld if block {x} {y + 1} {z} minecraft:nether_portal "
                f"run say lit-{x}"])
            entry["lit"] = any(f"lit-{x}" in line for line in lines)
            cx, cy, cz = centre_of(x, y, z, axis)
            entry["entry_position"] = [cx, cy, cz]
            walker.nether_chunks = []
            server.batch([f"execute in minecraft:overworld run tp {NAME} {cx} {cy} {cz} 0 0"])
            arrival = walker.wait_for_dimension("minecraft:the_nether", 30.0)
            entry["nether_arrival"] = list(arrival[1:5]) if arrival else None
            if arrival is None:
                report["starts"].append(entry)
                continue
            # The chunks round the arrival, as they were sent: length-prefixed.
            walker.stand(3.0)
            with open(OUT / f"nether-chunks-{len(report['starts'])}.bin", "wb") as dump:
                for payload in walker.nether_chunks:
                    dump.write(struct.pack(">I", len(payload)) + payload)
            entry["nether_chunk_packets"] = len(walker.nether_chunks)
            _, ax, ay, az, ayaw, _ = arrival
            # Out of the new portal, three blocks across it, and let the
            # cooldown run out; then back into where we arrived.
            server.batch([f"execute in minecraft:the_nether run tp {NAME} {ax} {ay + 6} {az} 0 0"])
            walker.stand(16.5)
            server.batch([f"execute in minecraft:the_nether run tp {NAME} {ax} {ay} {az} 0 0"])
            back = walker.wait_for_dimension("minecraft:overworld", 30.0)
            entry["overworld_return"] = list(back[1:5]) if back else None
            server.batch([f"execute in minecraft:overworld run tp {NAME} {cx} {cy + 30} {cz} 0 0"])
            walker.stand(16.5)
            report["starts"].append(entry)
        walker.socket.close()
        server.batch(["save-all flush"], timeout=120)
    finally:
        server.send("stop")
        try:
            server.process.wait(timeout=90)
        except Exception:
            server.process.kill()
    report["created"] = scan_portals(directory / "world" / "DIM-1" / "region",
                                     [s["nether_arrival"] for s in report["starts"]])
    shutil.rmtree(directory, ignore_errors=True)
    return report


def run_ours() -> dict:
    """The same scenario against ov_dedicated, generating the same seed.

    Our console has no `execute in` and no `forceload`: the frames are in the
    overworld, where the console acts, and a teleport moves a player inside the
    level they stand in — which is all the scenario needs.
    """
    import queue
    import socket
    import subprocess
    import threading

    binary = ROOT / "build" / "macos-debug" / "bin" / "ov_dedicated"
    directory = ROOT / "run" / "nether-portal-ours"
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    port = PORT + 1
    env = dict(os.environ, OV_WORLDGEN_SEED=str(SEED), OV_WORLDGEN_WORKERS="2")
    process = subprocess.Popen(
        [str(binary), f"--world={directory / 'world'}", f"--port={port}", "--log-level=info"],
        cwd=ROOT, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, bufsize=1)
    lines: queue.Queue[str] = queue.Queue()
    # Everything the server says, kept: a probe that loses its connection
    # has to be able to say whether the server crashed or kicked it.
    server_log = open(OUT / "ours-server.log", "w")

    def pump() -> None:
        assert process.stdout is not None
        for raw in process.stdout:
            line = raw.rstrip("\n")
            lines.put(line)
            server_log.write(line + "\n")
            server_log.flush()

    threading.Thread(target=pump, daemon=True).start()

    def send(*commands: str) -> None:
        assert process.stdin is not None
        process.stdin.write("".join(c + "\n" for c in commands))
        process.stdin.flush()

    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                break
        except OSError:
            time.sleep(0.3)
    report: dict = {"seed": SEED, "starts": []}
    try:
        # A generated world in Debug can take longer than the 20 s the server
        # waits for the spawn chunk at login, and it then refuses the join —
        # after Login Success, so the refusal arrives as a closed socket. The
        # forced ticket on the spawn keeps generating it: join again until a
        # deadline, and say how many tries it took.
        walker = None
        tries = 0
        deadline = time.monotonic() + 600
        while walker is None and time.monotonic() < deadline:
            tries += 1
            try:
                candidate = Walker(port)
                candidate.stand(3.0)
                if candidate.position is not None:
                    walker = candidate
                    break
                candidate.socket.close()
            except (OSError, EOFError):
                pass
            time.sleep(5.0)
        report["join_tries"] = tries
        assert walker is not None, "never joined: see ours-server.log"
        send(f"gamemode creative {NAME}")
        walker.stand(0.5)
        for (x, y, z, axis) in STARTS:
            entry: dict = {"start": [x, y, z], "axis": axis}
            if walker is None or getattr(walker, "lost", False):
                entry["error"] = "no connection"
                report["starts"].append(entry)
                continue
            # Our server has no /forceload, and /fill refuses a chunk that is
            # not loaded — as vanilla's does. The probe's own ticket loads it:
            # stand above the frame, and try a block until the answer is not
            # "not loaded".
            try:
                send(f"tp {NAME} {x} {y + 30} {z} 0 0")
                loaded = False
                wait_until = time.monotonic() + 300
                while not loaded and time.monotonic() < wait_until:
                    walker.stand(2.0)
                    while not lines.empty():
                        lines.get_nowait()
                    send(f"setblock {x - 1} {y - 1} {z} minecraft:obsidian")
                    answer_until = time.monotonic() + 5
                    while time.monotonic() < answer_until:
                        walker.stand(0.2)
                        seen = []
                        while not lines.empty():
                            seen.append(lines.get_nowait())
                        if any("not loaded" in line for line in seen):
                            break
                        if any("Changed the block" in line or "Could not set" in line
                               for line in seen):
                            loaded = True
                            break
                entry["loaded_after_s"] = round(300 - (wait_until - time.monotonic()), 1)
            except (EOFError, OSError) as error:
                entry["error"] = f"connection lost while loading: {error!r}"
                walker.lost = True
                report["starts"].append(entry)
                continue
            for command in frame_commands(x, y, z, axis, "minecraft:overworld")[1:]:
                send(command.split(" run ", 1)[1])
            walker.stand(1.5)
            cx, cy, cz = centre_of(x, y, z, axis)
            entry["entry_position"] = [cx, cy, cz]
            try:
                send(f"tp {NAME} {cx} {cy} {cz} 0 0")
                # Five minutes, not one: a Debug build generating the chunks
                # round a new portal on a busy machine took longer than sixty
                # seconds, and a probe that leaves the portal drops the crossing.
                arrival = walker.wait_for_dimension("minecraft:the_nether", 300.0)
                entry["nether_arrival"] = list(arrival[1:5]) if arrival else None
                if arrival is not None:
                    _, ax, ay, az, ayaw, _ = arrival
                    send(f"tp {NAME} {ax} {ay + 6} {az} 0 0")
                    # The cooldown is 300 *server* ticks and is refreshed for
                    # as long as the player stands in a portal. An overloaded
                    # Debug server runs fewer ticks than the wall clock says:
                    # after 16.5 s it had not run 300, the probe stepped back in
                    # on a running cooldown and stayed there for ever. A minute.
                    walker.stand(60.0)
                    send(f"tp {NAME} {ax} {ay} {az} 0 0")
                    back = walker.wait_for_dimension("minecraft:overworld", 300.0)
                    entry["overworld_return"] = list(back[1:5]) if back else None
                    if back is None:
                        # Still in the Nether: every later `tp` would move the
                        # probe there while the console builds in the overworld.
                        # Stop rather than measure nothing.
                        entry["error"] = "no return crossing; the remaining starts are not run"
                        report["starts"].append(entry)
                        break
                    send(f"tp {NAME} {cx} {cy + 30} {cz} 0 0")
                    walker.stand(60.0)
            except (EOFError, OSError) as error:
                # The server closed the connection: said, and the run goes on
                # to the report rather than dying with the probe.
                entry["error"] = f"connection lost: {error!r}"
                walker.lost = True
            report["starts"].append(entry)
        walker.socket.close()
        time.sleep(1.0)
    finally:
        try:
            send("stop")
        except (BrokenPipeError, OSError):
            pass  # the server is already gone: its log says why
        try:
            process.wait(timeout=120)
        except Exception:
            process.kill()
        report["server_exit_code"] = process.returncode
        time.sleep(0.5)
        server_log.close()
    report["created"] = scan_portals(directory / "world" / "DIM-1" / "region",
                                     [s.get("nether_arrival") for s in report["starts"]])
    shutil.rmtree(directory, ignore_errors=True)
    return report


def scan_portals(region_dir: Path, arrivals: list) -> list:
    """Every nether_portal and obsidian block within 24 blocks of each arrival."""
    wanted = {}
    for index, arrival in enumerate(arrivals):
        if arrival is None:
            continue
        ax, ay, az = int(arrival[0] // 1), int(arrival[1] // 1), int(arrival[2] // 1)
        for cx in range((ax - 24) >> 4, ((ax + 24) >> 4) + 1):
            for cz in range((az - 24) >> 4, ((az + 24) >> 4) + 1):
                wanted.setdefault((cx, cz), []).append((index, ax, ay, az))
    found: list = [{"portal": [], "obsidian": []} for _ in arrivals]
    by_region: dict = {}
    for (cx, cz) in wanted:
        by_region.setdefault((cx >> 5, cz >> 5), []).append((cx, cz))
    for (rx, rz), chunks in by_region.items():
        path = region_dir / f"r.{rx}.{rz}.mca"
        if not path.exists():
            continue
        for lx, lz, nbt in anvil_read.chunks(str(path)):
            cx, cz = nbt["xPos"], nbt["zPos"]
            if (cx, cz) not in wanted:
                continue
            for section in nbt.get("sections", []):
                states = section.get("block_states")
                if not states:
                    continue
                palette = states["palette"]
                names = [p["Name"] for p in palette]
                if "minecraft:nether_portal" not in names and "minecraft:obsidian" not in names:
                    continue
                data = states.get("data")
                bits = max(4, (len(palette) - 1).bit_length()) if len(palette) > 1 else 0
                for cell in range(4096):
                    if bits == 0:
                        which = 0
                    else:
                        per = 64 // bits
                        word = data[cell // per] & ((1 << 64) - 1)
                        which = (word >> ((cell % per) * bits)) & ((1 << bits) - 1)
                    name = names[which]
                    if name not in ("minecraft:nether_portal", "minecraft:obsidian"):
                        continue
                    x = cx * 16 + cell % 16
                    z = cz * 16 + (cell // 16) % 16
                    y = section["Y"] * 16 + cell // 256
                    for (index, ax, ay, az) in wanted[(cx, cz)]:
                        if abs(x - ax) <= 24 and abs(z - az) <= 24 and abs(y - ay) <= 24:
                            if name == "minecraft:nether_portal":
                                axis = palette[which].get("Properties", {}).get("axis", "?")
                                found[index]["portal"].append([x, y, z, axis])
                            else:
                                found[index]["obsidian"].append([x, y, z])
    for entry in found:
        entry["portal"].sort()
        entry["obsidian"].sort()
    return found


def terrain_snapshot() -> Path:
    """The Nether round each start's target, as the real server generates it,
    before any portal is built there — so that our placement algorithm can be
    run on the game's own terrain (`ov_netherparity --portal-world=`) and a
    disagreement split between the algorithm and the terrain."""
    directory = ROOT / "run" / "nether-portal-terrain"
    if directory.exists():
        shutil.rmtree(directory)
    server = Vanilla(directory, port=PORT)
    try:
        commands = []
        for (x, y, z, axis) in STARTS:
            cx, cy, cz = centre_of(x, y, z, axis)
            tx, tz = int(cx // 8), int(cz // 8)
            commands.append(f"execute in minecraft:the_nether run forceload add "
                            f"{tx - 32} {tz - 32} {tx + 32} {tz + 32}")
        server.batch(commands, timeout=600)
        time.sleep(20)
        server.batch(["save-all flush"], timeout=300)
        time.sleep(5)
    finally:
        server.send("stop")
        try:
            server.process.wait(timeout=120)
        except Exception:
            server.process.kill()
    return directory / "world" / "DIM-1"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ours", action="store_true")
    parser.add_argument("--compare", action="store_true")
    parser.add_argument("--terrain", action="store_true")
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    if args.compare:
        return compare()
    if args.terrain:
        print(terrain_snapshot())
        return 0
    if args.ours:
        report = run_ours()
        (OUT / "ours.json").write_text(json.dumps(report, indent=1))
    else:
        report = run_vanilla()
        (OUT / "vanilla.json").write_text(json.dumps(report, indent=1))
    print(json.dumps(report["starts"], indent=1))
    return 0


def compare() -> int:
    vanilla = json.loads((OUT / "vanilla.json").read_text())
    ours = json.loads((OUT / "ours.json").read_text())
    agree = 0
    total = 0
    # Two tolerances, reported apart: bit for bit, and within the 5e-5 block
    # of the game's final collision adjustment of the exit position, which is
    # named in docs/provenance/nether.md and not reproduced.
    for index, (v, o) in enumerate(zip(vanilla["starts"], ours["starts"])):
        for key in ("nether_arrival", "overworld_return"):
            total += 1
            both = v.get(key) is not None and o.get(key) is not None
            delta = max(abs(a - b) for a, b in zip(v[key][:3], o[key][:3])) if both else None
            exact = both and delta == 0.0
            close = both and delta <= 5e-5
            agree += close
            verdict = "EXACT" if exact else ("OK (<=5e-5)" if close else "DIFF")
            detail = f" max |d| {delta:.2e}" if both else ""
            print(f"start {v['start']} {key}: vanilla {v.get(key)} ours {o.get(key)}"
                  f"{detail} {verdict}")
        vp = vanilla["created"][index]
        op = ours["created"][index]
        total += 1
        same = vp["portal"] == op["portal"] and vp["obsidian"] == op["obsidian"]
        agree += same
        print(f"start {v['start']} created portal: vanilla {len(vp['portal'])} portal "
              f"{len(vp['obsidian'])} obsidian, ours {len(op['portal'])} / {len(op['obsidian'])}"
              f" {'OK' if same else 'DIFF'}")
    print(f"{agree} / {total} agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
