#!/usr/bin/env python3
"""Record which sound packets a real 1.20.1 server sends, to whom, for each gesture.

Two probe clients join: the **actor** does something, the **ear** stands four
blocks away and does nothing. Both record every packet they receive. Whether a
sound reaches the actor or only the others is the question this script exists
to answer, because the answer is different gesture by gesture: vanilla leaves
out the player whose own client already predicted the sound, and only that one.

The archived protocol page has been wrong about packet ids before (Explosion is
0x1D, not 0x1E), so nothing here is identified by id. The first scenario,
`layout`, makes the server emit sounds whose every field is chosen from the
console — `/playsound` with a known position, volume, pitch and category, an
inline sound name, and three forms of `/stopsound` — and the packets are then
recognised by those values.

Usage:
    python3 scripts/capture_sound_packets.py [vanilla|ours] [output.json]

Writes data/vanilla/1.20.1/normalized/sound_packets_<target>.json (gitignored:
it is a capture of Mojang's server, like every other oracle under normalized/).
"""
from __future__ import annotations

import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vanilla_miner import read_varint, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
RUN = ROOT / "run" / "sound-capture"
PORT = int(os.environ.get("OV_SOUND_PORT", "25631"))

# Serverbound, protocol 763, as the other probes of this repository send them.
SB_CONFIRM_TELEPORT = 0x00
SB_INTERACT = 0x10
SB_KEEP_ALIVE = 0x12
SB_POSITION = 0x14
SB_PLAYER_ACTION = 0x1D
SB_SET_HELD_ITEM = 0x28
SB_SET_CREATIVE_SLOT = 0x2B
SB_SWING_ARM = 0x2F
SB_USE_ITEM_ON = 0x31
SB_USE_ITEM = 0x32
SB_CLOSE_CONTAINER = 0x0C

# Clientbound housekeeping the probe answers.
CB_LOGIN = 0x28
CB_KEEP_ALIVE = 0x23
CB_SYNC_POSITION = 0x3C

# Traffic that is never about sound and would bury what is. Dropped from the
# record by id — these ids were all confirmed by content in earlier captures
# (docs/provenance/*.md): chunk data, light, time, keep-alive, entity movement,
# head rotation, velocity, attributes, and the chunk batch / centre packets.
NOISE = {0x24, 0x27, 0x5E, 0x23, 0x2B, 0x2C, 0x2D, 0x42, 0x54, 0x6A, 0x4E, 0x1F}


def block_pos(x: int, y: int, z: int) -> bytes:
    packed = ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)
    return struct.pack(">Q", packed & 0xFFFFFFFFFFFFFFFF)


class Probe:
    """A client that records everything it is sent, on a thread of its own."""

    def __init__(self, port: int, name: str) -> None:
        self.name = name
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=60)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.threshold: int | None = None
        self.buffer = b""
        self.lock = threading.Lock()
        self.send_lock = threading.Lock()
        self.captured: list[tuple[float, int, bytes]] = []
        self.position: tuple[float, float, float] | None = None
        self.entity_id: int | None = None
        self.sequence = 0
        self.alive = True

        host = b"127.0.0.1"
        self.send(0x00, varint(763) + varint(len(host)) + host + struct.pack(">H", port)
                  + varint(2))
        self.send(0x00, varint(len(name.encode())) + name.encode() + bytes([0]))
        while True:
            packet_id, payload = self.read()
            if packet_id == 0x03 and self.threshold is None:
                self.threshold, _ = read_varint(payload, 0)
            elif packet_id == 0x02:
                break
        self.socket.settimeout(None)
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def send(self, packet_id: int, payload: bytes) -> None:
        body = varint(packet_id) + payload
        with self.send_lock:
            if self.threshold is None:
                self.socket.sendall(varint(len(body)) + body)
            else:
                import zlib
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
            import zlib
            size, i = read_varint(data, i)
            data = data[i:] if size == 0 else zlib.decompress(data[i:])
            i = 0
        packet_id, i = read_varint(data, i)
        return packet_id, data[i:]

    def _run(self) -> None:
        try:
            while self.alive:
                packet_id, payload = self.read()
                if packet_id == CB_KEEP_ALIVE:
                    self.send(SB_KEEP_ALIVE, payload[:8])
                elif packet_id == CB_SYNC_POSITION:
                    x, y, z = struct.unpack_from(">ddd", payload, 0)
                    self.position = (x, y, z)
                    # 33 bytes in: x, y, z, yaw, pitch, flags, then the id.
                    teleport_id, _ = read_varint(payload, 33)
                    self.send(SB_CONFIRM_TELEPORT, varint(teleport_id))
                    self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([1]))
                elif packet_id == CB_LOGIN and self.entity_id is None:
                    self.entity_id = struct.unpack_from(">i", payload, 0)[0]
                if packet_id in NOISE:
                    continue
                with self.lock:
                    self.captured.append((time.monotonic(), packet_id, payload))
        except (EOFError, OSError):
            self.alive = False

    def drain(self) -> list[tuple[float, int, bytes]]:
        with self.lock:
            out, self.captured = self.captured, []
        return out

    # ── verbs ───────────────────────────────────────────────────────────────

    def next_sequence(self) -> int:
        self.sequence += 1
        return self.sequence

    def look(self, yaw: float, pitch: float) -> None:
        """Set Player Rotation (0x16). Use Item ray-traces from the eyes along
        this, so a bucket needs it: Use Item On alone never empties one."""
        self.send(0x16, struct.pack(">ff", yaw, pitch) + bytes([1]))

    def move(self, x: float, y: float, z: float, on_ground: bool = True) -> None:
        self.position = (x, y, z)
        self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([1 if on_ground else 0]))

    def hold(self, item_id: int | None) -> None:
        """Put an item in hotbar slot 0 (window slot 36) and select it."""
        if item_id is None:
            self.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", 36) + bytes([0]))
        else:
            self.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", 36) + bytes([1]) + varint(item_id)
                      + bytes([1]) + bytes([0]))
        self.send(SB_SET_HELD_ITEM, struct.pack(">h", 0))

    def use_item_on(self, x: int, y: int, z: int, face: int = 1,
                    cursor: tuple[float, float, float] = (0.5, 1.0, 0.5)) -> None:
        self.send(SB_USE_ITEM_ON, varint(0) + block_pos(x, y, z) + varint(face)
                  + struct.pack(">fff", *cursor) + bytes([0]) + varint(self.next_sequence()))
        self.send(SB_SWING_ARM, varint(0))

    def use_item(self) -> None:
        self.send(SB_USE_ITEM, varint(0) + varint(self.next_sequence()))

    def dig(self, x: int, y: int, z: int, status: int, face: int = 1) -> None:
        self.send(SB_PLAYER_ACTION, varint(status) + block_pos(x, y, z) + bytes([face])
                  + varint(self.next_sequence()))

    def attack(self, entity_id: int) -> None:
        self.send(SB_INTERACT, varint(entity_id) + varint(1) + bytes([0]))
        self.send(SB_SWING_ARM, varint(0))

    def close_container(self, window: int) -> None:
        self.send(SB_CLOSE_CONTAINER, bytes([window]))


# ── the two servers ─────────────────────────────────────────────────────────


class Vanilla:
    def __init__(self) -> None:
        from measure_entities import Server
        # ⚠ A taken port does not fail fast: vanilla logs "FAILED TO BIND TO
        #   PORT", crashes while stopping, and the harness waits three minutes
        #   for a "Done (" that never comes. Several agents run vanilla servers
        #   on this machine, and the first run of this script lost its port to
        #   one of them. Checked here, and named.
        probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            probe.bind(("0.0.0.0", PORT))
        except OSError:
            sys.exit(f"port {PORT} is taken — set OV_SOUND_PORT to a free one")
        finally:
            probe.close()
        if RUN.exists():
            shutil.rmtree(RUN)
        self.server = Server(RUN / "vanilla", port=PORT)

    def run(self, *commands: str) -> list[str]:
        return self.server.batch(list(commands))

    def stop(self) -> None:
        self.server.stop()


def spawn_entity_ids(records: list[tuple[float, int, bytes]]) -> list[tuple[int, int]]:
    """(entity id, type id) of every Spawn Entity (0x01) in a capture."""
    out = []
    for _, packet_id, payload in records:
        if packet_id != 0x01:
            continue
        entity_id, i = read_varint(payload, 0)
        i += 16  # uuid
        type_id, _ = read_varint(payload, i)
        out.append((entity_id, type_id))
    return out


def main() -> int:
    target = sys.argv[1] if len(sys.argv) > 1 else "vanilla"
    out_path = Path(sys.argv[2]) if len(sys.argv) > 2 else \
        NORMALIZED / f"sound_packets_{target}.json"
    if target != "vanilla":
        sys.exit("only the vanilla target is implemented by this script; "
                 "scripts/check_sounds_e2e.py replays the same gestures against ours")

    registries = json.loads((NORMALIZED / "registries.json").read_text())["registries"]
    items = registries["minecraft:item"]["entries"]
    entity_types = registries["minecraft:entity_type"]["entries"]

    def item(name: str) -> int:
        return items.index(f"minecraft:{name}")

    server = Vanilla()
    document: dict = {
        "$comment": "Paquets reçus par deux sondes sur un vrai serveur 1.20.1, geste par geste. "
                    "'actor' est le joueur qui fait le geste, 'ear' un joueur à quatre blocs. "
                    "Voir docs/provenance/son.md.",
        "target": target,
        "gestures": {},
    }
    try:
        server.run("gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                   "gamerule doWeatherCycle false", "gamerule randomTickSpeed 0",
                   "gamerule sendCommandFeedback false", "difficulty normal",
                   "time set noon", "forceload add -32 -32 32 32")
        actor = Probe(PORT, "Actor")
        time.sleep(1.0)
        ear = Probe(PORT, "Ear")
        server.run("gamemode creative Actor", "gamemode creative Ear",
                   "tp Actor 0.5 -60 0.5 0 90", "tp Ear 4.5 -60 0.5")
        time.sleep(3.0)
        actor.drain()
        ear.drain()

        def record(label: str, seconds: float, note: str = "") -> None:
            time.sleep(seconds)
            gesture = {"note": note, "actor": [], "ear": []}
            for who, probe in (("actor", actor), ("ear", ear)):
                for _, packet_id, payload in probe.drain():
                    gesture[who].append([packet_id, payload.hex()])
            document["gestures"][label] = gesture
            print(f"  {label:24s} actor {len(gesture['actor']):3d}  ear {len(gesture['ear']):3d}")

        def settle() -> None:
            time.sleep(0.6)
            actor.drain()
            ear.drain()

        # ── layout: fields chosen from the console, recognised by value ─────
        server.run("playsound minecraft:block.stone.place block Ear 10 -59 20 0.7 1.3")
        record("layout.playsound", 0.8,
               "block.stone.place, block, (10,-59,20), volume 0.7, pitch 1.3")
        server.run("playsound minecraft:ondes.custom hostile Ear 10.5 -59.25 20.125 2.0 0.5")
        record("layout.playsound_inline", 0.8,
               "unregistered name: ondes.custom, hostile, (10.5,-59.25,20.125), volume 2, pitch 0.5")
        server.run("playsound minecraft:entity.cow.ambient neutral Ear 4.5 -60 0.5 1 1 0.5")
        record("layout.playsound_min_volume", 0.8, "minVolume 0.5 at the listener")
        server.run("stopsound Ear")
        record("layout.stopsound_all", 0.8, "no source, no sound")
        server.run("stopsound Ear block")
        record("layout.stopsound_source", 0.8, "source block, no sound")
        server.run("stopsound Ear * minecraft:block.stone.place")
        record("layout.stopsound_sound", 0.8, "any source, block.stone.place")
        server.run("stopsound Ear weather minecraft:block.stone.place")
        record("layout.stopsound_both", 0.8, "weather, block.stone.place")

        # ── placing and breaking ────────────────────────────────────────────
        actor.hold(item("stone"))
        settle()
        actor.use_item_on(1, -61, 2, face=1)
        record("place.stone", 0.8, "stone placed at (1,-60,2) on the grass floor")
        actor.dig(1, -60, 2, 0)
        record("break.stone.creative", 0.8, "creative break of (1,-60,2)")

        server.run("gamemode survival Actor")
        server.run("setblock 1 -60 3 minecraft:dirt")
        settle()
        actor.dig(1, -60, 3, 0)
        time.sleep(0.05)
        actor.dig(1, -60, 3, 2)
        record("break.dirt.survival", 1.5, "survival break of dirt at (1,-60,3), start then finish")
        server.run("gamemode creative Actor")

        # ── blocks that react to a click ────────────────────────────────────
        actor.hold(None)
        for label, commands, click in [
            ("door.oak", ["setblock 2 -60 -2 minecraft:oak_door[half=lower]",
                          "setblock 2 -59 -2 minecraft:oak_door[half=upper]"], (2, -60, -2)),
            ("trapdoor.oak", ["setblock 3 -60 -2 minecraft:oak_trapdoor"], (3, -60, -2)),
            ("fence_gate.oak", ["setblock 1 -60 -2 minecraft:oak_fence_gate"], (1, -60, -2)),
            ("lever", ["setblock 0 -60 -2 minecraft:lever[face=floor]"], (0, -60, -2)),
            ("button.stone", ["setblock -1 -60 -2 minecraft:stone_button[face=floor]"],
             (-1, -60, -2)),
            ("button.oak", ["setblock -2 -60 -2 minecraft:oak_button[face=floor]"],
             (-2, -60, -2)),
            ("iron_trapdoor", ["setblock 4 -60 -2 minecraft:iron_trapdoor"], (4, -60, -2)),
        ]:
            server.run(*commands)
            settle()
            actor.use_item_on(*click, face=1, cursor=(0.5, 0.5, 0.5))
            # Buttons release on their own; the release is a sound too.
            record(f"{label}.open", 2.2 if label.startswith("button") else 0.8)
            if not label.startswith("button"):
                actor.use_item_on(*click, face=1, cursor=(0.5, 0.5, 0.5))
                record(f"{label}.close", 0.8)

        server.run("setblock -1 -60 3 minecraft:chest")
        settle()
        actor.use_item_on(-1, -60, 3, face=1, cursor=(0.5, 0.5, 0.5))
        record("chest.open", 0.8)
        actor.close_container(1)
        record("chest.close", 0.8)

        # ── fluids ──────────────────────────────────────────────────────────
        # ⚠ A bucket acts on Use Item, ray-traced along the look, and not on
        #   Use Item On: the first capture clicked the floor with one and the
        #   server did nothing, silently. So the actor looks straight down and
        #   uses the bucket, and the water lands in the actor's own cell.
        for fluid, spot in (("water", (-4.5, -60.0, 5.5)), ("lava", (5.5, -60.0, 6.5))):
            server.run(f"tp Actor {spot[0]} {spot[1]} {spot[2]} 0 90")
            time.sleep(0.6)
            actor.look(0.0, 90.0)
            actor.hold(item(f"{fluid}_bucket"))
            settle()
            actor.use_item()
            record(f"bucket.{fluid}.empty", 0.8, f"{fluid} bucket used looking down at {spot}")
            actor.hold(item("bucket"))
            settle()
            actor.use_item()
            record(f"bucket.{fluid}.fill", 0.8, f"empty bucket used on the {fluid} just placed")
        server.run("fill -8 -60 2 8 -58 9 minecraft:air", "tp Actor 0.5 -60 0.5 0 0")
        time.sleep(0.6)
        actor.look(0.0, 0.0)
        settle()

        # ── TNT, lit by hand, and its explosion ─────────────────────────────
        # Within reach: the first capture put it 11.5 blocks away and the
        # server ignored the click, which is a reach check and not a sound rule.
        server.run("setblock 0 -60 4 minecraft:tnt")
        actor.hold(item("flint_and_steel"))
        settle()
        actor.use_item_on(0, -60, 4, face=1)
        record("tnt.ignite", 1.0, "flint and steel on TNT at (0,-60,4)")
        record("tnt.explode", 4.0, "the same TNT, 80 ticks later")
        server.run("fill -8 -64 -1 8 -58 9 minecraft:air",
                   "fill -8 -64 -1 8 -61 9 minecraft:grass_block",
                   "tp Actor 0.5 -60 0.5", "tp Ear 4.5 -60 0.5")
        time.sleep(0.6)
        settle()

        # ── mobs ────────────────────────────────────────────────────────────
        server.run("summon minecraft:cow 2.5 -60 -5.5 {NoAI:1b,PersistenceRequired:1b,Tags:[\"ovcow\"]}")
        time.sleep(0.8)
        spawned = spawn_entity_ids(actor.drain())
        ear.drain()
        cow = next((e for e, t in spawned if entity_types[t] == "minecraft:cow"), None)
        if cow is not None:
            server.run("gamemode survival Actor")
            settle()
            actor.attack(cow)
            record("cow.hurt", 0.8, "survival punch on a cow")
            server.run("kill @e[tag=ovcow]")
            record("cow.death", 2.0, "/kill on the cow")
            server.run("gamemode creative Actor")
        else:
            print("  cow never spawned for the actor; cow gestures skipped")

        # ⚠ At noon a zombie burns, and the first capture's twenty "ambient"
        #   sounds were twenty hurt sounds and a death. Midnight, here.
        server.run("time set midnight",
                   "summon minecraft:zombie 6.5 -60 -5.5 {PersistenceRequired:1b,"
                   "Tags:[\"ovzombie\"],Attributes:[{Name:\"generic.movement_speed\",Base:0.0}]}",
                   "summon minecraft:cow 8.5 -60 -5.5 {PersistenceRequired:1b,Tags:[\"ovcow2\"],"
                   "Attributes:[{Name:\"generic.movement_speed\",Base:0.0}]}")
        record("mob.ambient", 25.0, "a zombie and a cow left alone at midnight")
        server.run("kill @e[tag=ovzombie]", "kill @e[tag=ovcow2]", "time set noon")
        settle()

        # ── eating, picking up, levels ──────────────────────────────────────
        # A golden apple can be eaten at full hunger, so what is measured is the
        # eating and not whether a hunger effect had time to bite.
        # ⚠ The item first, in creative: a survival server ignores Set
        #   Creative Slot, and the second capture "ate" with an empty hand.
        actor.hold(item("golden_apple"))
        settle()
        server.run("gamemode survival Actor")
        settle()
        actor.use_item()
        record("eat.golden_apple", 2.5, "use held until the golden apple is eaten")

        server.run("summon minecraft:item 0.5 -60 0.5 {Item:{id:\"minecraft:stone\",Count:1b},"
                   "PickupDelay:0s}")
        record("pickup.item", 1.5, "a stone summoned at the actor's feet")
        server.run("summon minecraft:experience_orb 0.5 -60 0.5 {Value:3s}")
        record("pickup.orb", 1.5, "an experience orb of value 3 at the actor's feet")
        server.run("xp add Actor 1 levels")
        record("xp.levels_command", 0.8, "/xp add 1 levels")
        server.run("xp set Actor 0 levels", "xp set Actor 0 points",
                   "summon minecraft:experience_orb 0.5 -60 0.5 {Value:20s}")
        record("pickup.orb_levelup", 1.5, "an orb of 20 from level 0: a level is crossed")
        server.run("xp set Actor 4 levels", "xp set Actor 0 points",
                   "summon minecraft:experience_orb 0.5 -60 0.5 {Value:20s}")
        record("pickup.orb_level5", 1.5, "an orb of 20 from level 4: level 5 is crossed")

        # ── moving: steps and a fall ────────────────────────────────────────
        server.run("fill -3 -61 -8 12 -61 -8 minecraft:stone", "tp Actor -2.5 -60 -7.5")
        time.sleep(0.8)
        actor.drain()
        ear.drain()
        x = -2.5
        for _ in range(30):
            x += 0.2158
            actor.move(x, -60.0, -7.5, True)
            time.sleep(0.05)
        record("walk.stone", 0.5, "survival walk over stone, 30 ticks at walking speed")

        server.run("tp Actor 0.5 -55 -12.5")
        time.sleep(0.6)
        actor.drain()
        ear.drain()
        y = -55.0
        velocity = 0.0
        while y > -60.0:
            velocity = (velocity - 0.08) * 0.98
            y = max(-60.0, y + velocity)
            actor.move(0.5, y, -12.5, y <= -60.0)
            time.sleep(0.05)
        record("fall.grass.5", 1.0, "survival fall of 5 blocks onto grass")
        server.run("gamemode creative Actor")

        print(f"  actor entity id {actor.entity_id}, ear at {ear.position}")
    finally:
        server.stop()
    out_path.write_text(json.dumps(document, indent=1) + "\n")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
