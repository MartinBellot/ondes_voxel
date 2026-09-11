#!/usr/bin/env python3
"""The entities that are not mobs survive a restart of our own server.

On ov_dedicated (port 25671), a fresh superflat, a creative probe:

  1. a TNT lit by a redstone block (console `setblock`) goes off and its
     crater drops items; the probe throws a bottle o' enchanting at its feet
     (orbs) and shoots an arrow into the grass;
  2. a second TNT is lit, a sand block is set 100 blocks up, the probe throws
     an ender pearl straight up — and the server is stopped at once, all three
     still burning or in the air;
  3. entities/ is read: the items (Age, Item), the orbs (Value), the arrow
     (inGround 1), the TNT (0 < Fuse < 80), the sand (Time, BlockState), the
     pearl in flight — the world is copied to .scratch/persistence-ours for the
     vanilla readback (measure_persistence.py readback);
  4. the server starts again: each comes back (Spawn Entity), the TNT goes off
     after the fuse it was saved with, the sand lands, the arrow and an item
     are picked up;
  5. the probe rides a cart and leaves: the cart leaves with it (RootVehicle in
     its file, no cart in entities/ after a save) and comes back ridden when
     the probe does.

    python3 scripts/check_persistence_e2e.py

Writes .scratch/persistence_e2e.json (untracked).
"""
from __future__ import annotations

import hashlib
import json
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
from anvil_read import chunks, parse  # noqa: E402
from capture_entity_packets import read_varint, varint  # noqa: E402
from measure_projectiles import Archer  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PRESET = os.environ.get("OV_PRESET", "macos-debug")
BINARY = ROOT / "build" / PRESET / "bin" / "ov_dedicated"
SCRATCH = ROOT / ".scratch"
RUN = SCRATCH / "persistence-e2e"
KEEP = SCRATCH / "persistence-ours"
OUT = SCRATCH / "persistence_e2e.json"
PORT = int(os.environ.get("OV_E2E_PORT", "25671"))
GENERATED = ROOT / "data" / "vanilla" / "1.20.1" / "generated" / "reports"

with open(GENERATED / "registries.json") as f:
    _REG = json.load(f)
ITEMS = {n: e["protocol_id"] for n, e in _REG["minecraft:item"]["entries"].items()}
TYPES = {n: e["protocol_id"] for n, e in _REG["minecraft:entity_type"]["entries"].items()}
TYPE_NAMES = {v: k for k, v in TYPES.items()}
with open(GENERATED / "blocks.json") as f:
    _BLOCKS = json.load(f)
SAND_STATE = next(s["id"] for s in _BLOCKS["minecraft:sand"]["states"])

CB_SPAWN = 0x01
CB_SPAWN_ORB = 0x02
CB_BLOCK_UPDATE = 0x0A
CB_EXPLOSION = 0x1D  # by content in a real capture (piège 24)
CB_REMOVE = 0x3E
CB_PASSENGERS = 0x59
CB_TAKE = 0x67
FEET = -60.0


def offline_uuid(name: str) -> str:
    raw = bytearray(hashlib.md5(f"OfflinePlayer:{name}".encode()).digest())
    raw[6] = (raw[6] & 0x0F) | 0x30
    raw[8] = (raw[8] & 0x3F) | 0x80
    return str(uuidlib.UUID(bytes=bytes(raw)))


class Ours:
    def __init__(self, world: Path, tag: str) -> None:
        self.log = open(RUN / f"server-{tag}.log", "w")
        self.process = subprocess.Popen(
            [str(BINARY), f"--world={world}", f"--port={PORT}", "--log-level=info"],
            cwd=ROOT, stdin=subprocess.PIPE, stdout=self.log, stderr=subprocess.STDOUT, text=True)

    def console(self, *lines: str) -> None:
        assert self.process.stdin is not None
        for line in lines:
            self.process.stdin.write(line + "\n")
        self.process.stdin.flush()

    def stop(self) -> None:
        try:
            self.console("stop")
            self.process.wait(timeout=120)
        except Exception:
            self.process.kill()
            self.process.wait()


class Hand:
    """A creative hand on an Archer (which knows its own entity id)."""

    def __init__(self, probe: Archer) -> None:
        self.probe = probe
        self.sequence = 100

    def hold(self, name: str) -> None:
        self.probe.send(0x2B, struct.pack(">h", 36) + bytes([1]) + varint(ITEMS[name]) + bytes([1, 0]))
        self.probe.hold(0)
        self.probe.pump(0.3)

    def use_on(self, x: int, y: int, z: int, face: int = 1) -> None:
        self.sequence += 1
        value = ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)
        self.probe.send(0x31, varint(0) + struct.pack(">Q", value) + varint(face) +
                        struct.pack(">fff", 0.5, 1.0, 0.5) + bytes([0]) + varint(self.sequence))

    def interact(self, entity: int) -> None:
        self.probe.send(0x10, varint(entity) + varint(0) + varint(0) + bytes([0]))


def connect() -> Archer:
    last = None
    for _ in range(40):
        try:
            probe = Archer(PORT, name="ovprobe")
            probe.pump(4.0)
            return probe
        except OSError as error:  # the spawn area takes a while in Debug (piège 29)
            last = error
            time.sleep(2.0)
    raise SystemExit(f"could not connect: {last}")


def spawns(stamped: list, start: float = 0.0) -> list[dict]:
    out = []
    for at, pid, payload in stamped:
        if at < start:
            continue
        if pid == CB_SPAWN:
            entity, i = read_varint(payload, 0)
            i += 16
            kind, i = read_varint(payload, i)
            x, y, z = struct.unpack_from(">ddd", payload, i)
            i += 27
            data, _ = read_varint(payload, i)
            out.append({"id": entity, "type": TYPE_NAMES.get(kind, kind), "pos": (x, y, z),
                        "data": data, "at": at})
        elif pid == CB_SPAWN_ORB:
            entity, i = read_varint(payload, 0)
            x, y, z = struct.unpack_from(">ddd", payload, i)
            out.append({"id": entity, "type": "minecraft:experience_orb", "pos": (x, y, z),
                        "at": at})
    return out


def saved(world: Path) -> list[dict]:
    out = []
    for region in sorted((world / "entities").glob("r.*.mca")):
        for _, _, chunk in chunks(region):
            for entity in chunk.get("Entities", []):
                entity["_chunk"] = chunk.get("Position")
                out.append(entity)
    return out


def of(entities: list[dict], kind: str) -> list[dict]:
    return [e for e in entities if e.get("id") == kind]


def phase_one(world: Path, report: dict, failures: list[str]) -> None:
    server = Ours(world, "first")
    try:
        probe = connect()
        hand = Hand(probe)
        probe.drain()
        # 1. A TNT whose crater drops items; orbs; an arrow in the grass.
        server.console("setblock 12 -60 12 minecraft:tnt",
                       "setblock 13 -60 12 minecraft:redstone_block")
        # Thrown and shot away from the probe, which then walks off: a creative
        # player standing by picks an orb or an arrow up before the save (the
        # first run lost both that way).
        hand.hold("minecraft:bow")
        probe.look(0.5, FEET, 0.5, 0.0, -10.0)  # facing +z, a little up: it lands far off
        probe.use()
        probe.pump(1.2)
        probe.release()
        probe.pump(0.3)
        hand.hold("minecraft:experience_bottle")
        probe.look(0.5, FEET, 0.5, 180.0, -30.0)  # facing -z
        probe.use()
        probe.pump(0.1)
        probe.look(-30.5, FEET, 30.5, 0.0, 0.0)
        probe.pump(6.0)
        # 2. Still burning, still falling, still flying when the server stops.
        server.console("setblock -12 -60 -12 minecraft:tnt",
                       "setblock -13 -60 -12 minecraft:redstone_block",
                       "setblock 3 40 -6 minecraft:sand")
        hand.hold("minecraft:ender_pearl")
        probe.look(-30.5, FEET, 30.5, 0.0, -90.0)
        probe.use()
        probe.pump(0.8)
        report["first_spawns"] = [{k: v for k, v in s.items() if k != "at"}
                                  for s in spawns(probe.stamped)]
        probe.socket.close()
    finally:
        server.stop()


def check_saved(world: Path, report: dict, failures: list[str]) -> dict:
    entities = saved(world)
    report["saved_ids"] = sorted({e["id"] for e in entities})
    found = {}
    items = of(entities, "minecraft:item")
    orbs = of(entities, "minecraft:experience_orb")
    arrows = of(entities, "minecraft:arrow")
    tnts = of(entities, "minecraft:tnt")
    sands = of(entities, "minecraft:falling_block")
    pearls = of(entities, "minecraft:ender_pearl")
    report["saved"] = {
        "items": [{"Item": e.get("Item"), "Age": e.get("Age"), "Pos": e.get("Pos")} for e in items],
        "orbs": [{"Value": e.get("Value"), "Count": e.get("Count"), "Age": e.get("Age")} for e in orbs],
        "arrows": [{"inGround": e.get("inGround"), "pickup": e.get("pickup"), "life": e.get("life"),
                    "inBlockState": e.get("inBlockState"), "Pos": e.get("Pos")} for e in arrows],
        "tnt": [{"Fuse": e.get("Fuse"), "Pos": e.get("Pos")} for e in tnts],
        "falling_block": [{"Time": e.get("Time"), "BlockState": e.get("BlockState"),
                           "Pos": e.get("Pos")} for e in sands],
        "ender_pearl": [{"Pos": e.get("Pos"), "Motion": e.get("Motion")} for e in pearls],
    }
    if not items:
        failures.append("no item in entities/ after the crater")
    if not orbs:
        failures.append("no experience orb in entities/")
    if not any(a.get("inGround") == 1 for a in arrows):
        failures.append(f"no arrow stuck in the ground saved: {report['saved']['arrows']}")
    if not any(0 < t.get("Fuse", 0) < 80 for t in tnts):
        failures.append(f"no burning TNT saved: {report['saved']['tnt']}")
    if not any(s.get("BlockState", {}).get("Name") == "minecraft:sand" and s.get("Time", 0) > 0
               for s in sands):
        failures.append(f"no falling sand saved: {report['saved']['falling_block']}")
    if not pearls:
        failures.append("no ender pearl in flight saved")
    found["tnt_fuse"] = min((t["Fuse"] for t in tnts if t.get("Fuse", 0) > 0), default=None)
    found["arrow"] = next((a for a in arrows if a.get("inGround") == 1), None)
    found["item"] = items[0] if items else None
    return found


def phase_two(world: Path, found: dict, report: dict, failures: list[str]) -> None:
    server = Ours(world, "restart")
    try:
        probe = connect()
        hand = Hand(probe)
        started = time.monotonic()
        probe.pump(8.0)
        back = spawns(probe.stamped)
        kinds = [s["type"] for s in back]
        report["restart_spawns"] = sorted(set(map(str, kinds)))
        for kind in ("minecraft:item", "minecraft:experience_orb", "minecraft:arrow",
                     "minecraft:tnt", "minecraft:falling_block"):
            if kind not in kinds:
                failures.append(f"{kind} did not come back after the restart")
        sand = [s for s in back if s["type"] == "minecraft:falling_block"]
        if sand and sand[0]["data"] != SAND_STATE:
            failures.append(f"the falling block came back as state {sand[0]['data']}, not sand")
        # The TNT goes on from the fuse it was saved with: the fuse metadata
        # (index 8, re-sent every tick) counts down from there, to the blast.
        tnt = [s for s in back if s["type"] == "minecraft:tnt"]
        blasts = [at for at, pid, _ in probe.stamped if pid == CB_EXPLOSION]
        fuses = []
        if tnt:
            prefix = varint(tnt[0]["id"]) + bytes([8, 1])
            for _, pid, payload in probe.stamped:
                if pid == 0x52 and payload.startswith(prefix):
                    fuses.append(read_varint(payload, len(prefix))[0])
        report["tnt"] = {"saved_fuse": found["tnt_fuse"], "fuse_sent_after_restart": fuses[:3],
                         "fuse_ticks_counted": len(fuses), "blasts": len(blasts),
                         "blast_after_s": round(blasts[0] - tnt[0]["at"], 2) if tnt and blasts else None}
        if not tnt or not blasts:
            failures.append(f"no blast after the restart (tnt {len(tnt)}, blasts {len(blasts)})")
        elif (not fuses or found["tnt_fuse"] is None or fuses[0] != found["tnt_fuse"]
              or len(fuses) != found["tnt_fuse"]):
            # The spawn's metadata carries the fuse as saved, then one per tick
            # down to 1: as many packets as ticks were left.
            failures.append(f"the fuse after the restart: {fuses[:3]}… ({len(fuses)} ticks) for "
                            f"{found['tnt_fuse']} saved")
        lands = [p for at, pid, p in probe.stamped if pid == CB_BLOCK_UPDATE]
        landed = False
        for payload in lands:
            value = struct.unpack_from(">q", payload, 0)[0]
            x, y = value >> 38, value & 0xFFF
            y = y - 0x1000 if y >= 0x800 else y
            state, _ = read_varint(payload, 8)
            if x == 3 and state == SAND_STATE:
                landed = True
        report["sand_landed"] = landed
        if not landed:
            failures.append("the sand that came back never landed")
        # The stuck arrow and an item are picked up.
        probe.stamped.clear()
        took = []
        for target in (found["arrow"], found["item"]):
            if target is None:
                continue
            x, y, z = target["Pos"]
            probe.look(x, FEET, z, 0.0, 0.0)
            probe.pump(1.5)
        took = [p for _, pid, p in probe.stamped if pid == CB_TAKE]
        report["takes"] = len(took)
        if len(took) < 2:
            failures.append(f"only {len(took)} pickups of the arrow and the item that came back")

        # 5. A cart, ridden, and a leave.
        server.console("setblock 40 -60 4 minecraft:rail")
        probe.look(40.5, FEET, 5.5, 0.0, 0.0)
        probe.pump(1.0)
        hand.hold("minecraft:minecart")
        probe.stamped.clear()
        hand.use_on(40, -60, 4)
        probe.pump(1.0)
        carts = [s for s in spawns(probe.stamped) if s["type"] == "minecraft:minecart"]
        if not carts:
            failures.append("no minecart spawned on the rail")
            return
        hand.hold("minecraft:stick")
        hand.interact(carts[0]["id"])
        probe.pump(1.0)
        rode = [p for _, pid, p in probe.stamped if pid == CB_PASSENGERS
                and p.startswith(varint(carts[0]["id"]) + varint(1))]
        if not rode:
            failures.append("the probe never got into the cart")
        probe.socket.close()
        time.sleep(2.0)
        server.console("save-all")
        time.sleep(3.0)
        player = parse((world / "playerdata" / f"{offline_uuid('ovprobe')}.dat").read_bytes())
        vehicle = player.get("RootVehicle")
        report["root_vehicle"] = vehicle
        if not vehicle or vehicle.get("Entity", {}).get("id") != "minecraft:minecart":
            failures.append(f"no RootVehicle with the cart in the probe's file: {vehicle}")
        elif vehicle.get("Attach") != vehicle["Entity"].get("UUID"):
            failures.append("RootVehicle's Attach is not the cart's UUID")
        carts_saved = of(saved(world), "minecraft:minecart")
        report["carts_in_entities_after_leave"] = len(carts_saved)
        if carts_saved:
            failures.append("the cart stayed in the world after its rider left with it")
        probe = connect()
        probe.pump(3.0)
        again = [s for s in spawns(probe.stamped) if s["type"] == "minecraft:minecart"]
        rides = [p for _, pid, p in probe.stamped if pid == CB_PASSENGERS]
        report["return"] = {"carts": len(again), "set_passengers": [p.hex() for p in rides],
                            "probe": probe.entity_id}
        if len(again) != 1:
            failures.append(f"{len(again)} carts after the return, not 1")
        elif not any(p.startswith(varint(again[0]["id"]) + varint(1) + varint(probe.entity_id))
                     for p in rides):
            failures.append("the probe is not back in its cart")
        probe.socket.close()
        report["restart_seconds"] = round(time.monotonic() - started, 1)
    finally:
        server.stop()


VANILLA = SCRATCH / "persistence-vanilla"


def phase_vanilla(report: dict, failures: list[str]) -> None:
    """Our server reads the world the real server saved (measure_persistence.py
    oracle): its items, orb, arrows, trident, cloud come back on the wire, and
    the probe — saved by vanilla sitting in a cart — is put back in it."""
    world = RUN / "vanilla-world"
    shutil.copytree(VANILLA, world)
    (world / "session.lock").unlink(missing_ok=True)
    before = saved(world)
    report["vanilla_file"] = {kind: len(of(before, kind)) for kind in sorted({e["id"] for e in before})}
    server = Ours(world, "vanilla")
    try:
        probe = connect()
        wanted = ("minecraft:item", "minecraft:experience_orb", "minecraft:arrow",
                  "minecraft:spectral_arrow", "minecraft:trident", "minecraft:area_effect_cloud")
        counts: dict[str, int] = {}
        # A Debug server on a world it did not generate steps slowly: wait
        # for what the file holds, a minute at most.
        deadline = time.monotonic() + 60.0
        while time.monotonic() < deadline:
            probe.pump(2.0)
            counts = {}
            for spawn in spawns(probe.stamped):
                counts[str(spawn["type"])] = counts.get(str(spawn["type"]), 0) + 1
            if all(counts.get(k, 0) >= report["vanilla_file"].get(k, 0) for k in wanted):
                break
        back = spawns(probe.stamped)
        report["vanilla_spawns"] = counts
        for kind in ("minecraft:item", "minecraft:experience_orb", "minecraft:arrow",
                     "minecraft:spectral_arrow", "minecraft:trident", "minecraft:area_effect_cloud"):
            if counts.get(kind, 0) < report["vanilla_file"].get(kind, 0):
                failures.append(f"vanilla's {kind}: {report['vanilla_file'].get(kind, 0)} in the "
                                f"file, {counts.get(kind, 0)} on the wire")
        carts = [s for s in back if s["type"] == "minecraft:minecart"]
        rides = [p for _, pid, p in probe.stamped if pid == CB_PASSENGERS]
        report["vanilla_root_vehicle"] = {"carts": len(carts), "probe": probe.entity_id,
                                          "set_passengers": [p.hex() for p in rides]}
        if len(carts) != 1 or not any(
                p.startswith(varint(carts[0]["id"]) + varint(1) + varint(probe.entity_id))
                for p in rides):
            failures.append("the probe vanilla saved in a cart is not back in it on our server")
        probe.socket.close()
    finally:
        server.stop()
    refused = [line.strip() for line in open(RUN / "server-vanilla.log")
               if "refused" in line and "entities" in line]
    report["vanilla_refused"] = refused[:10]


def main() -> int:
    if not BINARY.exists():
        raise SystemExit(f"{BINARY} not found")
    shutil.rmtree(RUN, ignore_errors=True)
    RUN.mkdir(parents=True)
    world = RUN / "world"
    report: dict = {}
    failures: list[str] = []
    if sys.argv[1:] == ["vanilla"]:
        if not VANILLA.exists():
            raise SystemExit(f"no {VANILLA}: run measure_persistence.py oracle first")
        phase_vanilla(report, failures)
        OUT.with_name("persistence_e2e_vanilla.json").write_text(
            json.dumps(report, indent=1, default=str))
        print(json.dumps(report, indent=1, default=str)[:4000])
        for failure in failures:
            print("  -", failure)
        return 1 if failures else 0
    phase_one(world, report, failures)
    found = check_saved(world, report, failures)
    shutil.rmtree(KEEP, ignore_errors=True)
    shutil.copytree(world, KEEP)
    phase_two(world, found, report, failures)
    OUT.write_text(json.dumps(report, indent=1, default=str))
    print(json.dumps(report, indent=1, default=str)[:6000])
    if failures:
        print("\nFAILED:")
        for failure in failures:
            print("  -", failure)
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
