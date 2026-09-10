#!/usr/bin/env python3
"""Drive our own server with a probe client: draw, shoot, hit, pick up.

The unit tests prove the rules against the numbers the vanilla server gave
(scripts/measure_projectiles.py). This proves the server carries them out, on a
socket, the way a client sees it. Nothing is called beside the protocol: the
probe presses Use Item (0x32) and lets go with Player Action 5, like a client.

Scenarios (each starts and stops its own ov_dedicated on OV_PROJ_E2E_PORT,
default 25633, in run/projectile-e2e/<scenario>/):

  creative  A cow is put three blocks in front of the probe (--mobs=cow). The
            probe draws a bow for a full second and lets go at it; then shoots
            into the ground at its feet, and walks nothing — the stuck arrow is
            within reach. Then a snowball, a dozen eggs, a crossbow loaded and
            fired, a trident, and an ender pearl.
  survival  The same, in survival, with a bow and eight arrows written into the
            probe's own playerdata file before it joins: every arrow the server
            takes and gives back is read off Set Container Slot.
  skeleton  A skeleton three blocks away, the probe in survival: the arrows it
            is shot with, their interval, and the health they cost.

Usage: python3 scripts/check_projectiles_e2e.py <scenario>... [--binary=PATH]
"""
from __future__ import annotations

import gzip
import hashlib
import json
import math
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
import uuid as uuidlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_entity_packets import read_varint, varint  # noqa: E402
from measure_projectiles import Archer, metadata_fields, spawn_fields  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / "run" / "projectile-e2e"
PORT = int(os.environ.get("OV_PROJ_E2E_PORT", "25633"))
BINARY = ROOT / "build" / "macos-debug" / "bin" / "ov_dedicated"

with open(ROOT / "data" / "vanilla" / "1.20.1" / "normalized" / "registries.json") as f:
    _REG = json.load(f)["registries"]
ITEMS: list[str] = _REG["minecraft:item"]["entries"]

CB_SPAWN = 0x01
CB_DAMAGE_EVENT = 0x18
CB_CONTAINER_SLOT = 0x14
CB_REMOVE = 0x3E
CB_METADATA = 0x52
CB_SET_HEALTH = 0x57
CB_SYNC = 0x3C
CB_TAKE = 0x67
DATA_VERSION = 3465
FEET_Y = -60.0


def item(name: str) -> int:
    return ITEMS.index(name)


# ── a playerdata file, written by hand ──────────────────────────────────────

def offline_uuid(name: str) -> uuidlib.UUID:
    raw = bytearray(hashlib.md5(f"OfflinePlayer:{name}".encode()).digest())
    raw[6] = (raw[6] & 0x0F) | 0x30
    raw[8] = (raw[8] & 0x3F) | 0x80
    return uuidlib.UUID(bytes=bytes(raw))


def nbt_string(s: str) -> bytes:
    data = s.encode()
    return struct.pack(">H", len(data)) + data


def nbt_named(tag: int, name: str, payload: bytes) -> bytes:
    return bytes([tag]) + nbt_string(name) + payload


def nbt_item(slot: int, name: str, count: int) -> bytes:
    return (nbt_named(1, "Slot", struct.pack(">b", slot))
            + nbt_named(8, "id", nbt_string(name))
            + nbt_named(1, "Count", struct.pack(">b", count)) + b"\x00")


def write_playerdata(world: Path, name: str, items: list[tuple[int, str, int]]) -> Path:
    who = offline_uuid(name)
    ints = struct.unpack(">iiii", who.bytes)
    body = (nbt_named(3, "DataVersion", struct.pack(">i", DATA_VERSION))
            + nbt_named(11, "UUID", struct.pack(">i", 4) + struct.pack(">iiii", *ints))
            + nbt_named(8, "Dimension", nbt_string("minecraft:overworld"))
            + nbt_named(3, "SelectedItemSlot", struct.pack(">i", 0))
            + nbt_named(9, "Inventory", bytes([10]) + struct.pack(">i", len(items))
                        + b"".join(nbt_item(*entry) for entry in items)))
    root = b"\x0a" + nbt_string("") + body + b"\x00"
    path = world / "playerdata" / f"{who}.dat"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(gzip.compress(root))
    return path


# ── the server ──────────────────────────────────────────────────────────────

def start_server(binary: Path, world: Path, extra: list[str]) -> subprocess.Popen:
    if not binary.exists():
        raise SystemExit(f"{binary} not found; build ov_dedicated first")
    log = open(RUN / f"{world.name}.log", "w")
    process = subprocess.Popen([str(binary), f"--port={PORT}", f"--world={world}",
                                "--ticks=3600", "--log-level=debug", *extra],
                               cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    for _ in range(300):
        time.sleep(0.1)
        try:
            with socket.create_connection(("127.0.0.1", PORT), timeout=0.2):
                return process
        except OSError:
            if process.poll() is not None:
                raise RuntimeError("the server stopped before it listened")
    raise RuntimeError("the server never listened")


def stop_server(process: subprocess.Popen) -> None:
    process.terminate()
    try:
        process.wait(timeout=60)
    except subprocess.TimeoutExpired:
        process.kill()


#: Appended to world and log names when another binary is checked, so a
#: "before" run does not overwrite the log of the run it is compared with.
SUFFIX = ""


def fresh_world(name: str) -> Path:
    world = RUN / f"{name}{SUFFIX}"
    shutil.rmtree(world, ignore_errors=True)
    world.mkdir(parents=True)
    return world


# ── reading what came back ──────────────────────────────────────────────────

class Log:
    """Everything the probe was told, decoded as far as this check needs."""

    def __init__(self, packets: list[tuple[int, bytes]]) -> None:
        self.spawns = [spawn_fields(p) for pid, p in packets if pid == CB_SPAWN]
        self.metadata = [metadata_fields(p) for pid, p in packets if pid == CB_METADATA]
        self.removed: set[int] = set()
        for pid, p in packets:
            if pid == CB_REMOVE:
                count, i = read_varint(p, 0)
                for _ in range(count):
                    eid, i = read_varint(p, i)
                    self.removed.add(eid)
        self.damage = []
        for pid, p in packets:
            if pid == CB_DAMAGE_EVENT:
                eid, i = read_varint(p, 0)
                kind, i = read_varint(p, i)
                cause, i = read_varint(p, i)
                direct, i = read_varint(p, i)
                self.damage.append({"entity": eid, "type": kind, "cause": cause - 1,
                                    "direct": direct - 1})
        self.takes = []
        for pid, p in packets:
            if pid == CB_TAKE:
                a, i = read_varint(p, 0)
                b, i = read_varint(p, i)
                c, _ = read_varint(p, i)
                self.takes.append((a, b, c))
        self.slots = []
        for pid, p in packets:
            if pid == CB_CONTAINER_SLOT and p[0] == 0:
                _, i = read_varint(p, 1)
                slot = struct.unpack_from(">h", p, i)[0]
                i += 2
                present = p[i]
                entry = {"slot": slot, "item": None, "count": 0, "nbt": b""}
                if present:
                    item_id, j = read_varint(p, i + 1)
                    entry.update(item=ITEMS[item_id], count=p[j], nbt=p[j + 1:])
                self.slots.append(entry)
        self.health = [struct.unpack_from(">f", p, 0)[0] for pid, p in packets
                       if pid == CB_SET_HEALTH]
        self.syncs = [struct.unpack_from(">ddd", p, 0) for pid, p in packets if pid == CB_SYNC]

    def of_type(self, name: str) -> list[dict]:
        return [s for s in self.spawns if s["type"] == name]

    def fields_of(self, entity_id: int) -> list[dict]:
        return [f for eid, fields in self.metadata if eid == entity_id for f in fields]


def speed(spawn: dict) -> float:
    return math.sqrt(sum((c / 8000.0) ** 2 for c in spawn["velocity_raw"]))


class Hands:
    def __init__(self, probe: Archer) -> None:
        self.probe = probe

    def creative(self, name: str, count: int = 1) -> None:
        self.probe.send(0x2B, struct.pack(">h", 36) + bytes([1]) + varint(item(name))
                        + bytes([count, 0]))
        self.probe.hold(0)
        self.probe.pump(0.3)

    def draw(self, seconds: float) -> None:
        self.probe.use()
        self.probe.pump(seconds)
        self.probe.release()


def report(failures: list[str], lines: list[str]) -> int:
    print("\n".join(lines))
    if failures:
        print("\nFAILED:")
        for f in failures:
            print("  -", f)
        return 1
    print("\nall checks passed")
    return 0


# ── scenario: creative ──────────────────────────────────────────────────────

def check_creative(binary: Path) -> int:
    world = fresh_world("creative")
    server = start_server(binary, world, ["--mobs=cow"])
    failures: list[str] = []
    lines: list[str] = []
    try:
        probe = Archer(PORT, name="ovarcher")
        hands = Hands(probe)
        probe.pump(4.5)
        me = probe.entity_id
        cows = Log(probe.drain()).of_type("minecraft:cow")
        cow = cows[0]["entity_id"] if cows else None
        lines.append(f"probe entity {me}, cow {cow}")

        # 1. A full draw at the cow, three blocks ahead along +z.
        hands.creative("minecraft:bow")
        probe.look(0.5, FEET_Y, 0.5, 0.0, 0.0)
        probe.pump(0.3)
        probe.drain()
        hands.draw(1.2)
        probe.pump(1.5)
        log = Log(probe.drain())
        arrows = log.of_type("minecraft:arrow")
        if not arrows:
            failures.append("a full draw spawned no arrow")
        else:
            a = arrows[0]
            flags = [f["value"] for f in log.fields_of(a["entity_id"]) if f["index"] == 8]
            lines.append(f"arrow: data={a['data']} speed={speed(a):.4f} pos={a['pos']} "
                         f"velocity={a['velocity_raw']} flags={flags}")
            if a["data"] != me:
                failures.append(f"Spawn Entity data {a['data']}, expected the shooter {me}")
            if not 2.9 <= speed(a) <= 3.1:
                failures.append(f"a full draw left at {speed(a):.3f}, expected 3 ± spread")
            if abs(a["pos"][1] - (FEET_Y + 1.52)) > 1e-6:
                failures.append(f"arrow started at y={a['pos'][1]}, expected eye - 0.1F")
            if flags[:1] != [1]:
                failures.append(f"a full draw is critical: index 8 should be 1, got {flags}")
            hits = [d for d in log.damage if d["entity"] == cow]
            health = [f["value"] for f in log.fields_of(cow) if f["index"] == 9] if cow else []
            lines.append(f"cow: damage events {hits}, health {health}, "
                         f"arrow removed {a['entity_id'] in log.removed}")
            if not hits:
                failures.append("the arrow did not hurt the cow")
            elif hits[0]["cause"] != me or hits[0]["direct"] != a["entity_id"]:
                failures.append(f"damage event should name the archer and the arrow: {hits[0]}")
            if not health or min(health) >= 10.0:
                failures.append("the cow's health never went down")
            if a["entity_id"] not in log.removed:
                failures.append("the arrow that hit was not removed")

        # 2. Into the ground at the probe's feet: it sticks, and a creative
        #    player takes it ("creative only") without getting anything.
        # Away from the cow: once hurt it panics and runs about in front of
        # the probe, and the first version shot it rather than the ground.
        probe.look(0.5, FEET_Y, 0.5, 180.0, 60.0)
        probe.pump(0.3)
        probe.drain()
        hands.draw(1.2)
        probe.pump(2.0)
        log = Log(probe.drain())
        arrows = log.of_type("minecraft:arrow")
        taken = [t for t in log.takes if arrows and t[0] == arrows[0]["entity_id"]]
        lines.append(f"ground shot: {len(arrows)} arrow, taken {taken}, "
                     f"removed {bool(arrows) and arrows[0]['entity_id'] in log.removed}")
        if not taken or taken[0][1] != me:
            failures.append("a stuck arrow at the feet of a creative player was not taken")

        # 3. A snowball: flies, breaks on the ground.
        hands.creative("minecraft:snowball", 16)
        probe.look(0.5, FEET_Y, 0.5, 0.0, 20.0)
        probe.pump(0.2)
        probe.drain()
        probe.use()
        probe.pump(2.0)
        log = Log(probe.drain())
        balls = log.of_type("minecraft:snowball")
        lines.append(f"snowball: {len(balls)} spawned, speed "
                     f"{[round(speed(b), 3) for b in balls]}, removed "
                     f"{[b['entity_id'] in log.removed for b in balls]}")
        if len(balls) != 1 or not 1.45 <= speed(balls[0]) <= 1.55:
            failures.append("a snowball should leave at 1.5")
        elif balls[0]["entity_id"] not in log.removed:
            failures.append("the snowball never broke")

        # 4. Eggs: a dozen, and the chickens that hatch.
        hands.creative("minecraft:egg", 16)
        probe.look(0.5, FEET_Y, 0.5, 90.0, 30.0)
        probe.pump(0.2)
        probe.drain()
        for _ in range(32):
            probe.use()
            probe.pump(0.08)
        probe.pump(2.0)
        log = Log(probe.drain())
        eggs = log.of_type("minecraft:egg")
        chickens = log.of_type("minecraft:chicken")
        lines.append(f"eggs: {len(eggs)} thrown, {len(chickens)} chickens hatched "
                     f"(expected about {len(eggs) * 0.1328:.1f})")
        if len(eggs) != 32:
            failures.append(f"32 eggs thrown, {len(eggs)} spawned")

        # 5. A crossbow: load it, see the NBT, fire it.
        hands.creative("minecraft:crossbow")
        probe.look(0.5, FEET_Y, 0.5, 180.0, -5.0)
        probe.pump(0.2)
        probe.drain()
        hands.draw(1.5)
        probe.pump(0.4)
        log = Log(probe.drain())
        loaded = [s for s in log.slots if s["slot"] == 36 and b"Charged" in s["nbt"]]
        probe.use()
        probe.pump(1.0)
        log2 = Log(probe.drain())
        bolts = log2.of_type("minecraft:arrow")
        flags = [f["value"] for f in log2.fields_of(bolts[0]["entity_id"]) if f["index"] == 8] \
            if bolts else []
        lines.append(f"crossbow: loaded slot update {bool(loaded)}, shot {len(bolts)}, "
                     f"speed {[round(speed(b), 3) for b in bolts]}, flags {flags}")
        if not loaded:
            failures.append("releasing a crossbow after 30 ticks did not load it")
        if len(bolts) != 1 or not 3.05 <= speed(bolts[0]) <= 3.25 or flags[:1] != [5]:
            failures.append("a crossbow bolt should leave at 3.15 with index 8 = 5")

        # 6. A trident, into the ground ahead.
        hands.creative("minecraft:trident")
        probe.look(0.5, FEET_Y, 0.5, 0.0, 70.0)
        probe.pump(0.2)
        probe.drain()
        hands.draw(0.8)
        probe.pump(2.0)
        log = Log(probe.drain())
        tridents = log.of_type("minecraft:trident")
        taken = [t for t in log.takes if tridents and t[0] == tridents[0]["entity_id"]]
        lines.append(f"trident: {len(tridents)} thrown, speed "
                     f"{[round(speed(t), 3) for t in tridents]}, taken {taken}")
        if len(tridents) != 1 or not 2.4 <= speed(tridents[0]) <= 2.6:
            failures.append("a trident should leave at 2.5")

        # 7. An ender pearl, thrown along +x: the probe is moved where it breaks.
        hands.creative("minecraft:ender_pearl", 4)
        probe.look(0.5, FEET_Y, 0.5, -90.0, -20.0)
        probe.pump(0.2)
        probe.drain()
        probe.use()
        probe.pump(4.0)
        log = Log(probe.drain())
        lines.append(f"pearl: {len(log.of_type('minecraft:ender_pearl'))} thrown, "
                     f"teleported to {log.syncs}")
        if not log.syncs or log.syncs[-1][0] < 20.0:
            failures.append("the pearl did not move the probe along +x")
    finally:
        stop_server(server)
    return report(failures, lines)


# ── scenario: survival ──────────────────────────────────────────────────────

def arrow_count(slots: list[dict], initial: int) -> list[int]:
    return [s["count"] if s["item"] == "minecraft:arrow" else 0 for s in slots
            if s["slot"] == 37] or [initial]


def check_survival(binary: Path) -> int:
    world = fresh_world("survival")
    write_playerdata(world, "ovarcher", [(0, "minecraft:bow", 1), (1, "minecraft:arrow", 8)])
    server = start_server(binary, world, ["--survival", "--mobs=cow"])
    failures: list[str] = []
    lines: list[str] = []
    try:
        probe = Archer(PORT, name="ovarcher")
        hands = Hands(probe)
        probe.pump(4.5)
        me = probe.entity_id
        cows = Log(probe.drain()).of_type("minecraft:cow")
        cow = cows[0]["entity_id"] if cows else None
        probe.hold(0)
        probe.look(0.5, FEET_Y, 0.5, 0.0, 0.0)
        probe.pump(0.3)
        probe.drain()

        # A full draw at the cow: one arrow spent, one point of wear.
        hands.draw(1.2)
        probe.pump(1.5)
        log = Log(probe.drain())
        arrows = log.of_type("minecraft:arrow")
        counts = arrow_count(log.slots, 8)
        bow = [s for s in log.slots if s["slot"] == 36]
        lines.append(f"survival shot: {len(arrows)} arrow, arrow slot now {counts}, "
                     f"bow slot updates {[(s['item'], s['nbt'].hex()) for s in bow]}")
        hits = [d for d in log.damage if d["entity"] == cow]
        lines.append(f"cow hurt: {hits}")
        if len(arrows) != 1:
            failures.append("no arrow in survival")
        if counts[-1] != 7:
            failures.append(f"eight arrows, one shot: expected 7 left, got {counts}")
        if not bow or b"Damage" not in bow[-1]["nbt"]:
            failures.append("the bow did not wear")
        if not hits:
            failures.append("the survival arrow did not hurt the cow")

        # Into the ground, then walk nothing: picked up, back to 7 + 1.
        # Away from the cow: once hurt it panics and runs about in front of
        # the probe, and the first version shot it rather than the ground.
        probe.look(0.5, FEET_Y, 0.5, 180.0, 60.0)
        probe.pump(0.3)
        probe.drain()
        hands.draw(1.2)
        probe.pump(2.5)
        log = Log(probe.drain())
        arrows = log.of_type("minecraft:arrow")
        counts = arrow_count(log.slots, 7)
        taken = [t for t in log.takes if arrows and t[0] == arrows[0]["entity_id"]]
        lines.append(f"pickup: taken {taken}, arrow slot went {counts}")
        if not taken:
            failures.append("a stuck survival arrow was not picked up")
        if counts[-1] != 7:
            failures.append(f"one spent and one picked up: expected 7, got {counts}")

        # Empty the quiver: six more shots, then a seventh does nothing.
        spawned = 0
        probe.look(0.5, FEET_Y, 0.5, 180.0, -10.0)
        probe.pump(0.2)
        for _ in range(8):
            probe.drain()
            hands.draw(0.4)
            probe.pump(0.4)
            spawned += len(Log(probe.drain()).of_type("minecraft:arrow"))
        lines.append(f"eight draws with seven arrows: {spawned} arrows")
        if spawned != 7:
            failures.append(f"seven arrows should make seven shots, made {spawned}")
    finally:
        stop_server(server)
    return report(failures, lines)


# ── scenario: skeleton ──────────────────────────────────────────────────────

def check_skeleton(binary: Path) -> int:
    world = fresh_world("skeleton")
    server = start_server(binary, world, ["--survival", "--mobs=skeleton"])
    failures: list[str] = []
    lines: list[str] = []
    try:
        probe = Archer(PORT, name="ovtarget")
        probe.pump(4.0)
        skeletons = Log(probe.drain()).of_type("minecraft:skeleton")
        skeleton = skeletons[0]["entity_id"] if skeletons else None
        probe.stamped.clear()
        probe.pump(16.0)
        stamped = [(t, spawn_fields(p)) for t, pid, p in probe.stamped if pid == CB_SPAWN]
        arrows = [(t, s) for t, s in stamped if s["type"] == "minecraft:arrow"]
        gaps = [round((b - a) * 20.0, 1) for (a, _), (b, _) in zip(arrows, arrows[1:])]
        log = Log([(pid, p) for _, pid, p in probe.stamped])
        lines.append(f"skeleton {skeleton}: {len(arrows)} arrows in 16 s, gaps {gaps} ticks, "
                     f"speeds {[round(speed(s), 3) for _, s in arrows]}, "
                     f"owners {sorted({s['data'] for _, s in arrows})}, health {log.health}")
        if len(arrows) < 3:
            failures.append("a skeleton three blocks away shot fewer than three arrows in 16 s")
        if gaps and not all(55.0 <= g <= 65.0 for g in gaps):
            failures.append(f"on normal the interval is 60 ticks, got {gaps}")
        if arrows and {s["data"] for _, s in arrows} != {skeleton}:
            failures.append("a skeleton's arrow must name the skeleton")
        if not log.health or min(log.health) >= 20.0:
            failures.append("the arrows never hurt the probe")
    finally:
        stop_server(server)
    return report(failures, lines)


SCENARIOS = {"creative": check_creative, "survival": check_survival, "skeleton": check_skeleton}


def main() -> int:
    binary = BINARY
    names = []
    for arg in sys.argv[1:]:
        if arg.startswith("--binary="):
            global SUFFIX
            binary = Path(arg.split("=", 1)[1])
            SUFFIX = "-other"
        elif arg in SCENARIOS:
            names.append(arg)
        else:
            print(__doc__)
            return 2
    RUN.mkdir(parents=True, exist_ok=True)
    status = 0
    for name in names or list(SCENARIOS):
        print(f"── {name} ──")
        status |= SCENARIOS[name](binary)
    shutil.rmtree(RUN, ignore_errors=True) if status == 0 else None
    return status


if __name__ == "__main__":
    sys.exit(main())
