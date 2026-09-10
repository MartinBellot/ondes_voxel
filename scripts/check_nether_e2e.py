#!/usr/bin/env python3
"""Drive our own server with a probe client: build a portal, light it, go and come back.

Everything the probe does goes through the protocol, as a player would do it:

  1. It is given obsidian and flint and steel (console), and builds a 4x5
     obsidian frame on the superflat ground with fourteen Use Item On packets.
  2. It lights the frame with flint and steel, and must see the six portal
     blocks arrive as Block Updates.
  3. It walks into the portal, in survival, and keeps reporting its position.
     It must receive a Respawn naming minecraft:the_nether — no earlier than 80
     ticks after it stepped in — then a Synchronize Player Position, then Nether
     chunks (Chunk Data and Update Light).
  4. It steps out of the portal it arrived in, waits out the 300-tick cooldown,
     steps back in, and must receive a Respawn naming minecraft:overworld and
     land in the portal it built, at the same relative place.
  5. The server is stopped; its save must hold DIM-1/region with the portal the
     crossing built, and the player file must say minecraft:overworld.

Usage: python3 scripts/check_nether_e2e.py [path/to/ov_dedicated]
Exit status 0 when every check passes. Writes .scratch/nether-e2e.json.
"""
from __future__ import annotations

import gzip
import json
import math
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
from capture_entity_packets import read_varint, varint  # noqa: E402
from measure_nether_portal import Walker, scan_portals  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
BINARY = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "macos-debug" / "bin" / "ov_dedicated"
RUN = ROOT / "run" / "nether-e2e"
PORT = 25679
NAME = "ovportal"

SB_SET_HELD_ITEM = 0x28
SB_USE_ITEM_ON = 0x31
SB_SWING = 0x2F
CB_BLOCK_UPDATE = 0x0A
CB_CHUNK = 0x24


class OvServer:
    def __init__(self, directory: Path, port: int) -> None:
        if directory.exists():
            shutil.rmtree(directory)
        directory.mkdir(parents=True)
        self.directory = directory
        self.process = subprocess.Popen(
            [str(BINARY), f"--world={directory / 'world'}", f"--port={port}", "--survival",
             "--log-level=info"],
            cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1)
        self.lines: queue.Queue[str] = queue.Queue()
        self.log: list[str] = []
        threading.Thread(target=self._pump, daemon=True).start()
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    return
            except OSError:
                if self.process.poll() is not None:
                    raise RuntimeError("ov_dedicated exited before listening")
                time.sleep(0.2)
        raise RuntimeError("ov_dedicated never listened")

    def _pump(self) -> None:
        assert self.process.stdout is not None
        for raw in self.process.stdout:
            line = raw.rstrip("\n")
            self.log.append(line)
            self.lines.put(line)

    def send(self, *commands: str) -> None:
        assert self.process.stdin is not None
        self.process.stdin.write("".join(c + "\n" for c in commands))
        self.process.stdin.flush()

    def stop(self) -> None:
        try:
            self.send("stop")
            self.process.wait(timeout=90)
        except Exception:
            self.process.kill()


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


class Builder(Walker):
    def __init__(self, port: int) -> None:
        super().__init__(port)
        self.sequence = 0
        self.blocks: dict[tuple[int, int, int], int] = {}
        self.chunks_since: list[tuple[str, int]] = []

    def pump_once(self, seconds: float) -> None:
        # The base pump, plus block updates and chunks, recorded.
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.socket.settimeout(max(0.005, deadline - time.monotonic()))
            try:
                packet_id, payload = self.read()
            except (TimeoutError, OSError):
                return
            if packet_id == CB_BLOCK_UPDATE:
                pos, i = unpacked(payload, 0)
                state, _ = read_varint(payload, i)
                self.blocks[pos] = state
            elif packet_id == CB_CHUNK:
                self.chunks_since.append((self.dimension, len(payload)))
            elif packet_id == 0x23:
                self.send(0x12, payload[:8])
            elif packet_id == 0x41:
                from measure_nether_portal import read_string
                dim_type, i = read_string(payload, 0)
                dim_name, i = read_string(payload, i)
                self.dimension = dim_name
                self.events.append(("respawn", (dim_name, time.monotonic())))
            elif packet_id == 0x3C:
                x, y, z = struct.unpack_from(">ddd", payload, 0)
                yaw, pitch = struct.unpack_from(">ff", payload, 24)
                teleport_id, _ = read_varint(payload, 33)
                self.position = (x, y, z)
                self.events.append(("sync", (self.dimension, x, y, z, yaw, 0)))
                self.send(0x00, varint(teleport_id))
                self.send(0x14, struct.pack(">ddd", x, y, z) + bytes([1]))

    def wait_for_dimension(self, dimension: str, timeout: float):
        mark = len(self.events)
        deadline = time.monotonic() + timeout
        seen = None
        while time.monotonic() < deadline:
            self.stand(0.1)
            for kind, value in self.events[mark:]:
                if kind == "respawn" and value[0] == dimension:
                    seen = value[1]
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
        self.pump_once(0.12)


def main() -> int:
    checks: list[tuple[str, bool, str]] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        checks.append((name, ok, detail))
        print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}")

    server = OvServer(RUN, PORT)
    report: dict = {}
    try:
        bot = None
        for _ in range(30):
            try:
                bot = Builder(PORT)
                break
            except OSError:
                time.sleep(1.0)
        assert bot is not None
        bot.stand(3.0)
        px, py, pz = bot.position
        ground = math.floor(py) - 1
        x0, z0 = math.floor(px) + 3, math.floor(pz) + 3
        base = ground + 1
        server.send(f"clear {NAME}", f"give {NAME} minecraft:obsidian 64",
                    f"give {NAME} minecraft:flint_and_steel 1")
        bot.stand(1.0)
        bot.hold(0)
        bot.stand(0.3)
        # Bottom row, on the ground; then the two columns; then the lintel.
        for x in range(x0 - 1, x0 + 3):
            bot.use_on(x, ground, z0, 1)
        for x in (x0 - 1, x0 + 2):
            for y in range(base, base + 4):
                bot.use_on(x, y, z0, 1)
        bot.use_on(x0 - 1, base + 4, z0, 5)
        bot.use_on(x0, base + 4, z0, 5)
        bot.stand(0.5)
        frame = [(x, base, z0) for x in range(x0 - 1, x0 + 3)] + \
                [(x, y, z0) for x in (x0 - 1, x0 + 2) for y in range(base + 1, base + 5)] + \
                [(x0, base + 4, z0), (x0 + 1, base + 4, z0)]
        placed = [bot.blocks.get(p) for p in frame]
        obsidian = placed[0]
        check("frame built by Use Item On", obsidian not in (None, 0) and
              all(state == obsidian for state in placed), f"{len(frame)} blocks, state {obsidian}")

        bot.hold(1)
        bot.stand(0.3)
        bot.use_on(x0, base, z0, 1)
        bot.stand(1.5)
        inside = [(x, y, z0) for x in (x0, x0 + 1) for y in range(base + 1, base + 4)]
        portal = [bot.blocks.get(p) for p in inside]
        lit = portal[0] not in (None, 0) and all(state == portal[0] for state in portal)
        check("flint and steel lights the frame", lit, f"inside states {portal}")

        # Into the portal, and stay.
        entry = (x0 + 1.0, float(base + 1), z0 + 0.5)
        bot.position = entry
        stepped_in = time.monotonic()
        arrived = bot.wait_for_dimension("minecraft:the_nether", 40.0)
        check("Respawn into minecraft:the_nether", arrived is not None)
        if arrived is None:
            return 1
        waited = arrived[0] - stepped_in
        # The first crossing also builds the Nether's worldgen stack and the
        # chunks round the new portal, on the tick thread: only the lower
        # bound is a rule here. The return crossing below times the portal.
        check("not before 80 ticks in survival", waited >= 80 * 0.05 - 0.3,
              f"{waited:.2f} s ({waited / 0.05:.0f} ticks)")
        report["first_crossing_seconds"] = waited
        _, nx, ny, nz, _, _ = arrived[1]
        report["nether_arrival"] = [nx, ny, nz]
        check("arrival near (x/8, y, z/8)",
              abs(nx - entry[0] / 8) <= 20 and abs(nz - entry[2] / 8) <= 20,
              f"({nx:.3f}, {ny:.3f}, {nz:.3f}) for target ({entry[0] / 8:.2f}, {entry[2] / 8:.2f})")
        # Liveness with a deadline, not luck: the 9x9 square round the arrival
        # within a minute. A Debug build generates the Nether at a few chunks
        # a second on a busy machine — 128 in 30 s was measured — so the full
        # view distance is reported, not required.
        deadline = time.monotonic() + 60.0
        nether_chunks = 0
        while time.monotonic() < deadline and nether_chunks < 81:
            bot.stand(0.5)
            nether_chunks = sum(1 for d, _ in bot.chunks_since if d == "minecraft:the_nether")
        report["nether_chunks_seconds"] = 60.0 - max(0.0, deadline - time.monotonic())
        check("Nether chunks streamed (the 9x9 round the player)", nether_chunks >= 81,
              f"{nether_chunks} chunk packets in {report['nether_chunks_seconds']:.1f} s")

        # Out of the new portal, the cooldown, and back in.
        bot.position = (nx, ny, nz + 3.0)
        bot.stand(16.5)
        bot.position = (nx, ny, nz)
        stepped_back = time.monotonic()
        back = bot.wait_for_dimension("minecraft:overworld", 40.0)
        check("Respawn back into minecraft:overworld", back is not None)
        if back is not None:
            returned = back[0] - stepped_back
            report["return_crossing_seconds"] = returned
            # 80 ticks of standing, plus the tick the move lands on and the
            # probe's own 50 ms cadence: 4.0 to 4.6 s.
            check("the return crossing takes 80 ticks", 3.9 <= returned <= 4.8,
                  f"{returned:.2f} s ({returned / 0.05:.0f} ticks)")
            _, ox, oy, oz, _, _ = back[1]
            report["overworld_return"] = [ox, oy, oz]
            check("back through the portal that was built",
                  abs(ox - entry[0]) < 0.01 and abs(oy - entry[1]) < 0.01 and abs(oz - entry[2]) < 0.01,
                  f"({ox:.4f}, {oy:.4f}, {oz:.4f}) vs entry {entry}")
        bot.stand(2.0)
        bot.socket.close()
        time.sleep(1.0)
    finally:
        server.stop()

    world = RUN / "world"
    created = scan_portals(world / "DIM-1" / "region", [report.get("nether_arrival")])
    portal_blocks = created[0]["portal"] if created else []
    check("DIM-1/region holds the Nether portal", len(portal_blocks) == 6,
          f"{len(portal_blocks)} portal blocks, {len(created[0]['obsidian']) if created else 0} obsidian")
    players = list((world / "playerdata").glob("*.dat"))
    dimension = None
    if players:
        raw = gzip.decompress(players[0].read_bytes())
        dimension = "minecraft:overworld" if b"minecraft:overworld" in raw else (
            "minecraft:the_nether" if b"minecraft:the_nether" in raw else None)
    check("the player file names the dimension", dimension == "minecraft:overworld", str(dimension))

    report["checks"] = [{"name": n, "ok": ok, "detail": d} for n, ok, d in checks]
    report["server_log_tail"] = [line for line in server.log if "nether" in line.lower()][-20:]
    out = ROOT / ".scratch" / "nether-e2e.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=1))
    shutil.rmtree(RUN, ignore_errors=True)
    failed = [n for n, ok, _ in checks if not ok]
    print(f"\n{len(checks) - len(failed)} / {len(checks)} checks pass")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
