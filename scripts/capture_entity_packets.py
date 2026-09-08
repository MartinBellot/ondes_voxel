#!/usr/bin/env python3
"""Record what a real 1.20.1 server sends about its entities.

The archived protocol page has been wrong about every packet id this project
has checked against it, so nothing here is read off a summary. A probe client
written from the spec joins the vanilla jar, mobs are summoned next to it from
the console, and whatever arrives is written down byte for byte.

What it captures, per scenario:

  spawn       Spawn Entity (0x01) and the Set Entity Metadata (0x52) that
              follows it. The metadata is the interesting half: it is the only
              statement of which indices a given type actually sends, and with
              what types.
  movement    Update Entity Position (0x2B) and its rotation variants, with the
              mob's Pos read from the console before and after — so the units
              of the delta are measured rather than assumed.
  damage      Damage Event (0x18), Hurt Animation (0x21) and the health
              metadata that goes with them.

Usage: python3 scripts/capture_entity_packets.py [output.json]
"""
from __future__ import annotations

import json
import socket
import struct
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from measure_entities import Server, parse_pos  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / "run" / "entity-capture"
PORT = 25602
OUT = ROOT / "data" / "vanilla" / "1.20.1" / "normalized" / "entity_packets.json"


def varint(n: int) -> bytes:
    out = b""
    while True:
        piece = n & 0x7F
        n >>= 7
        out += bytes([piece | (0x80 if n else 0)])
        if not n:
            return out


def read_varint(buf: bytes, i: int) -> tuple[int, int]:
    value, shift = 0, 0
    while True:
        byte = buf[i]
        i += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, i
        shift += 7


class Probe:
    """A client that joins, stands still, and remembers what it was told."""

    def __init__(self, port: int, name: str = "ovprobe", host: str = "127.0.0.1") -> None:
        self.socket = socket.create_connection((host, port), timeout=60)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.threshold: int | None = None
        self.buffer = b""
        self.captured: list[tuple[int, bytes]] = []
        self.position: tuple[float, float, float] | None = None

        handshake = host.encode()
        self.send(0x00, varint(763) + varint(len(handshake)) + handshake
                  + struct.pack(">H", port) + varint(2))
        self.send(0x00, varint(len(name.encode())) + name.encode() + bytes([0]))
        while True:
            packet_id, payload = self.read()
            if packet_id == 0x03 and self.threshold is None:
                self.threshold, _ = read_varint(payload, 0)
            elif packet_id == 0x02:
                break

    def send(self, packet_id: int, payload: bytes) -> None:
        body = varint(packet_id) + payload
        if self.threshold is None:
            self.socket.sendall(varint(len(body)) + body)
        else:
            inner = (varint(0) + body if len(body) < self.threshold
                     else varint(len(body)) + zlib.compress(body))
            self.socket.sendall(varint(len(inner)) + inner)

    def _exact(self, n: int) -> bytes:
        while len(self.buffer) < n:
            chunk = self.socket.recv(65536)
            if not chunk:
                raise EOFError
            self.buffer += chunk
        out, self.buffer = self.buffer[:n], self.buffer[n:]
        return out

    def read(self) -> tuple[int, bytes]:
        length, shift = 0, 0
        while True:
            byte = self._exact(1)[0]
            length |= (byte & 0x7F) << shift
            if not byte & 0x80:
                break
            shift += 7
        data = self._exact(length)
        i = 0
        if self.threshold is not None:
            size, i = read_varint(data, i)
            data = data[i:] if size == 0 else zlib.decompress(data[i:])
            i = 0
        packet_id, i = read_varint(data, i)
        return packet_id, data[i:]

    def pump(self, seconds: float) -> None:
        """Answer housekeeping and record everything, for a while."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.socket.settimeout(max(0.01, deadline - time.monotonic()))
            try:
                packet_id, payload = self.read()
            except (socket.timeout, TimeoutError):
                return
            self.captured.append((packet_id, payload))
            if packet_id == 0x23:  # keep alive
                self.send(0x12, payload[:8])
            elif packet_id == 0x3C:  # synchronize position
                x, y, z = struct.unpack_from(">ddd", payload, 0)
                self.position = (x, y, z)
                teleport_id, _ = read_varint(payload, 33)
                self.send(0x00, varint(teleport_id))
                self.send(0x14, struct.pack(">ddd", x, y, z) + bytes([1]))

    def drain(self) -> list[tuple[int, bytes]]:
        out = self.captured
        self.captured = []
        return out


def hexed(data: bytes) -> str:
    return data.hex()


def main() -> int:
    out_path = Path(sys.argv[1]) if len(sys.argv) > 1 else OUT
    server = Server(RUN, port=PORT)
    document: dict = {"$comment": "Paquets releves sur un vrai serveur 1.20.1. "
                                  "Voir docs/PROVENANCE.md.",
                      "spawn": {}, "movement": [], "damage": []}
    try:
        server.batch(["gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                      "gamerule sendCommandFeedback true", "gamerule doImmediateRespawn true",
                      "difficulty normal", "time set midnight"])
        probe = Probe(PORT)
        probe.pump(3.0)
        if probe.position is None:
            raise RuntimeError("the probe was never told where it is")
        px, py, pz = probe.position
        print(f"probe at {px:.2f} {py:.2f} {pz:.2f}")
        server.batch([f"forceload add {int(px) - 64} {int(pz) - 64} "
                      f"{int(px) + 64} {int(pz) + 64}"])
        probe.pump(1.0)
        probe.drain()

        # ── One of each, spawned three blocks away ──────────────────────────
        for index, name in enumerate([
                "minecraft:zombie", "minecraft:creeper", "minecraft:cow", "minecraft:skeleton",
                "minecraft:armor_stand", "minecraft:arrow", "minecraft:slime",
                "minecraft:villager"]):
            x, z = px + 3.0 + index * 4.0, pz
            extra = ",Size:0" if name == "minecraft:slime" else ""
            server.batch([f'summon {name} {x} {py} {z} '
                          f'{{NoAI:1b,Silent:1b,PersistenceRequired:1b,'
                          f'Tags:["cap{index}"]{extra}}}'])
            probe.pump(1.5)
            packets = [(pid, payload) for pid, payload in probe.drain()
                       if pid in (0x01, 0x52, 0x54, 0x6A, 0x42, 0x68)]
            document["spawn"][name] = {
                "x": x, "y": py, "z": z,
                "packets": [{"id": pid, "hex": hexed(payload)} for pid, payload in packets],
            }
            print(f"  {name}: {len(packets)} packets")

        # ── Movement, in the server's own units ─────────────────────────────
        #
        # A teleport of a known distance, with the mob's Pos read on both sides,
        # so the delta packet's units are measured and not taken on trust.
        server.batch(['summon minecraft:zombie '
                      f'{px + 3.0} {py} {pz + 3.0} '
                      '{NoAI:1b,Silent:1b,PersistenceRequired:1b,NoGravity:1b,Tags:["mover"]}'])
        probe.pump(1.5)
        probe.drain()
        for step in (0.25, 1.0, -0.125):
            before = server.batch(['data get entity @e[tag=mover,limit=1] Pos'])
            server.batch([f"tp @e[tag=mover,limit=1] ~{step} ~ ~"])
            probe.pump(1.0)
            after = server.batch(['data get entity @e[tag=mover,limit=1] Pos'])
            moves = [(pid, payload) for pid, payload in probe.drain()
                     if pid in (0x2B, 0x2C, 0x2D, 0x68)]
            document["movement"].append({
                "step": step,
                "before": [p for line in before for p in [parse_pos(line)] if p],
                "after": [p for line in after for p in [parse_pos(line)] if p],
                "packets": [{"id": pid, "hex": hexed(payload)} for pid, payload in moves],
            })
            print(f"  step {step}: {len(moves)} movement packets")

        # ── Which index carries what ────────────────────────────────────────
        #
        # The metadata index table is the part of the protocol most often copied
        # from a summary and most often wrong, because a wrong index is not an
        # error: it is a mob that renders with someone else's property. So it is
        # derived instead. One NBT field is set at a time on an otherwise
        # identical zombie, and the index whose value changes is the answer.
        probes = [
            ("baseline", "{}"),
            ("Silent", "{Silent:1b}"),
            ("NoGravity", "{NoGravity:1b}"),
            ("Glowing", "{Glowing:1b}"),
            ("Invisible", "{Invisible:1b}"),
            ("HasVisualFire", "{HasVisualFire:1b}"),
            ("CustomName", '{CustomName:\'{"text":"Bob"}\'}'),
            ("CustomNameVisible", '{CustomName:\'{"text":"Bob"}\',CustomNameVisible:1b}'),
            ("Air", "{Air:123s}"),
            ("TicksFrozen", "{TicksFrozen:123}"),
            ("Health", "{Health:7.0f}"),
            ("NoAI", "{NoAI:1b}"),
            ("LeftHanded", "{LeftHanded:1b}"),
            ("IsBaby", "{IsBaby:1b}"),
            ("ArrowsInEntity", "{ArrowsInEntity:3}"),
            ("BeeStingers", "{BeeStingers:3}"),
        ]
        document["metadata_probe"] = {}
        for label, nbt in probes:
            body = nbt[1:-1]
            # Every probe is the baseline plus exactly one field, so the index
            # that differs from the baseline's packet is the one that field
            # feeds. PersistenceRequired is on all of them, itself included, so
            # it cannot be the difference.
            joined = f"{body},PersistenceRequired:1b" if body else "PersistenceRequired:1b"
            server.batch(['kill @e[tag=meta]',
                          f'summon minecraft:zombie {px + 5.0} {py} {pz + 5.0} '
                          f'{{{joined},Tags:["meta"]}}'])
            probe.pump(1.2)
            found = [payload for pid, payload in probe.drain() if pid == 0x52]
            document["metadata_probe"][label] = {"nbt": nbt,
                                                 "packets": [hexed(p) for p in found]}
            print(f"  metadata {label}: {len(found)} packets")

        # ── Damage ──────────────────────────────────────────────────────────
        server.batch(["damage @e[tag=mover,limit=1] 3 minecraft:generic"])
        probe.pump(1.5)
        hurt = [(pid, payload) for pid, payload in probe.drain()
                if pid in (0x18, 0x21, 0x52, 0x1C, 0x04)]
        document["damage"] = [{"id": pid, "hex": hexed(payload)} for pid, payload in hurt]
        print(f"  damage: {len(hurt)} packets")

        out_path.parent.mkdir(parents=True, exist_ok=True)
        with open(out_path, "w") as f:
            json.dump(document, f, indent=1, sort_keys=True)
        print(f"wrote {out_path}")
    finally:
        server.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
