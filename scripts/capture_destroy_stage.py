#!/usr/bin/env python3
"""What a real 1.20.1 server tells the *other* players while one of them digs.

Two probe clients join, as in scripts/capture_sound_packets.py: the **actor**
digs, the **ear** stands four blocks away and records. The questions:

  * which packet carries a crack stage — identified by **content** (the actor's
    entity id, then the dug position, then one byte), never by the archive's id;
  * who receives it: the digger, the others, or both;
  * how often, and with which values, over a dig the server has to time itself;
  * what an abort and a finished break send afterwards;
  * who receives the arm swing (Entity Animation) that a Swing Arm triggers.

Every packet is stamped with the server's own world age, read from the Update
Time packet that arrives once a second, plus the wall time since it — the same
anchoring scripts/vanilla_miner.py uses to turn a stopwatch into ticks.

Usage (one JVM on the machine at a time):

    lockf /tmp/ov-vanilla.lock python3 scripts/capture_destroy_stage.py vanilla
    python3 scripts/capture_destroy_stage.py ours

Writes data/vanilla/1.20.1/normalized/destroy_stage_<target>.json (gitignored).
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
import capture_sound_packets as base  # noqa: E402
from vanilla_miner import read_varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
RUN = ROOT / "run" / "destroy-capture"
PORT = int(os.environ.get("OV_DESTROY_PORT", "25643"))

CB_UPDATE_TIME = 0x5E


class Probe(base.Probe):
    """base.Probe, but Update Time is kept (it is the clock) and every record
    carries the world age it was received at."""

    def __init__(self, port: int, name: str) -> None:
        self.clock: tuple[int, float] | None = None
        super().__init__(port, name)

    def _run(self) -> None:
        try:
            while self.alive:
                packet_id, payload = self.read()
                now = time.monotonic()
                if packet_id == base.CB_KEEP_ALIVE:
                    self.send(base.SB_KEEP_ALIVE, payload[:8])
                elif packet_id == base.CB_SYNC_POSITION:
                    x, y, z = struct.unpack_from(">ddd", payload, 0)
                    self.position = (x, y, z)
                    teleport_id, _ = read_varint(payload, 33)
                    self.send(base.SB_CONFIRM_TELEPORT, base.varint(teleport_id))
                    self.send(base.SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([1]))
                elif packet_id == base.CB_LOGIN and self.entity_id is None:
                    self.entity_id = struct.unpack_from(">i", payload, 0)[0]
                elif packet_id == CB_UPDATE_TIME:
                    age = struct.unpack_from(">q", payload, 0)[0]
                    self.clock = (age, now)
                if packet_id in base.NOISE:
                    continue
                with self.lock:
                    self.captured.append((now, packet_id, payload))
        except (EOFError, OSError):
            self.alive = False

    def age_at(self, when: float) -> float | None:
        if self.clock is None:
            return None
        age, anchor = self.clock
        return age + (when - anchor) * 20.0

    def swing(self) -> None:
        self.send(base.SB_SWING_ARM, base.varint(0))


class Vanilla:
    def __init__(self) -> None:
        from measure_entities import Server
        from measure_block_sounds import wait_for_port
        wait_for_port(PORT, "OV_DESTROY_PORT")
        if RUN.exists():
            shutil.rmtree(RUN)
        self.server = Server(RUN / "vanilla", port=PORT)

    def run(self, *commands: str) -> list[str]:
        return self.server.batch(list(commands))

    def stop(self) -> None:
        self.server.stop()


class Ours(base.Ours):
    def __init__(self) -> None:
        base.RUN = RUN
        base.PORT = PORT
        super().__init__()


def decode_stage(payload: bytes, entity: int) -> dict | None:
    """(entity VarInt, Position, Byte) — or None when the payload is not that."""
    try:
        who, i = read_varint(payload, 0)
    except IndexError:
        return None
    if who != entity or len(payload) != i + 9:
        return None
    packed = struct.unpack_from(">Q", payload, i)[0]
    x = packed >> 38
    y = packed & 0xFFF
    z = (packed >> 12) & 0x3FFFFFF
    x = x - (1 << 26) if x >= 1 << 25 else x
    z = z - (1 << 26) if z >= 1 << 25 else z
    y = y - (1 << 12) if y >= 1 << 11 else y
    stage = struct.unpack_from(">b", payload, i + 8)[0]
    return {"pos": [x, y, z], "stage": stage}


def main() -> int:
    target = sys.argv[1] if len(sys.argv) > 1 else "vanilla"
    if target not in ("vanilla", "ours"):
        sys.exit(f"unknown target {target!r}: vanilla or ours")
    out_path = NORMALIZED / f"destroy_stage_{target}.json"

    registries = json.loads((NORMALIZED / "registries.json").read_text())["registries"]
    items = registries["minecraft:item"]["entries"]

    server = Vanilla() if target == "vanilla" else Ours()
    document: dict = {
        "$comment": "Paquets reçus pendant qu'un joueur casse un bloc : 'actor' casse, 'ear' "
                    "regarde à quatre blocs. Voir docs/provenance/cassage-bloc.md.",
        "target": target,
        "gestures": {},
    }
    try:
        server.run("gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                   "gamerule doWeatherCycle false", "gamerule randomTickSpeed 0",
                   "gamerule sendCommandFeedback false", "difficulty peaceful",
                   "time set noon", "forceload add -32 -32 32 32")
        actor = Probe(PORT, "Actor")
        time.sleep(1.0)
        ear = Probe(PORT, "Ear")
        server.run("gamemode survival Actor", "gamemode creative Ear",
                   "tp Actor 0.5 -60 0.5 0 45", "tp Ear 4.5 -60 0.5")
        time.sleep(3.0)
        actor.drain()
        ear.drain()
        entity = actor.entity_id
        document["actor_entity_id"] = entity

        def record(label: str, seconds: float, note: str, started: float) -> None:
            time.sleep(seconds)
            gesture: dict = {"note": note, "actor": [], "ear": []}
            for who, probe in (("actor", actor), ("ear", ear)):
                for when, packet_id, payload in probe.drain():
                    row = {"id": packet_id, "hex": payload.hex(),
                           "ticks_after_start": round((when - started) * 20.0, 2)}
                    age = probe.age_at(when)
                    if age is not None:
                        row["world_age"] = round(age, 2)
                    stage = decode_stage(payload, entity)
                    if stage is not None:
                        row["destroy_stage"] = stage
                    gesture[who].append(row)
            document["gestures"][label] = gesture
            stages = [r for r in gesture["ear"] if "destroy_stage" in r]
            mine = [r for r in gesture["actor"] if "destroy_stage" in r]
            print(f"  {label:28s} ear {len(gesture['ear']):3d} ({len(stages)} stage-shaped)  "
                  f"actor {len(gesture['actor']):3d} ({len(mine)} stage-shaped)")

        def settle() -> None:
            time.sleep(0.6)
            actor.drain()
            ear.drain()

        # ── 1. stone, bare hand, left to the server's clock, then aborted ───
        # 150 ticks bare-handed (docs/provenance on hardness). Held for 200 so
        # every stage the server sends is seen, then cancelled.
        actor.hold(None)
        server.run("setblock 1 -60 3 minecraft:stone")
        settle()
        started = time.monotonic()
        actor.dig(1, -60, 3, 0, face=2)
        time.sleep(10.0)
        actor.dig(1, -60, 3, 1, face=2)
        record("stone.hand.abort", 1.5, "start at t0, abort at 200 ticks, never finished",
               started)

        # ── 2. dirt, bare hand, finished on time ────────────────────────────
        # 15 ticks bare-handed: finish sent at ~20 ticks.
        server.run("setblock 1 -60 4 minecraft:dirt")
        settle()
        started = time.monotonic()
        actor.dig(1, -60, 4, 0, face=2)
        time.sleep(1.0)
        actor.dig(1, -60, 4, 2, face=2)
        record("dirt.hand.finish", 1.5, "start at t0, finish at ~20 ticks (needs 15)", started)

        # ── 3. oak planks with a wooden axe, finished on time ───────────────
        actor.hold(items.index("minecraft:wooden_axe"))
        server.run("setblock 1 -60 5 minecraft:oak_planks")
        settle()
        started = time.monotonic()
        actor.dig(1, -60, 5, 0, face=2)
        time.sleep(1.8)
        actor.dig(1, -60, 5, 2, face=2)
        record("planks.wooden_axe.finish", 1.5, "start at t0, finish at ~36 ticks", started)

        # ── 4. an arm swing alone ───────────────────────────────────────────
        settle()
        started = time.monotonic()
        actor.swing()
        record("swing", 1.0, "Swing Arm, main hand, nothing else", started)
    finally:
        server.stop()

    NORMALIZED.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(document, indent=1))
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
