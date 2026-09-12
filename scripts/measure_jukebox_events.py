#!/usr/bin/env python3
"""What a real 1.20.1 server sends when a disc goes into a jukebox, and out.

The client plays a record itself: no Sound Effect carries it. What travels is a
World Event (0x25) whose number and data the client turns into a streamed
sound at the jukebox. The archived protocol page names 1010 "play record" with
the record's item id as data; whether 1.20.1 also sends a *stop* event when the
disc is ejected, and to whom each goes, is what this script measures.

Two probes, as in capture_sound_packets.py: the **actor** inserts and ejects,
the **ear** stands four blocks away. Every World Event, Sound Effect and Stop
Sound either receives is decoded and printed; the raw capture is written to
.scratch/ (never to run/, which is the shared asset tree).

Usage:
    OV_SOUND_PORT=25715 python3 scripts/measure_jukebox_events.py [output.json]
"""
from __future__ import annotations

import json
import os
import shutil
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_sound_packets as capture  # noqa: E402
from vanilla_miner import read_varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SCRATCH = ROOT / ".scratch" / "jukebox"
PORT = capture.PORT

CB_WORLD_EVENT = 0x25
CB_SOUND = 0x62
CB_STOP_SOUND = 0x63


def decode(packet_id: int, payload: bytes) -> dict | None:
    if packet_id == CB_WORLD_EVENT:
        event, packed, data, relative = struct.unpack_from(">iQii", payload + b"\0\0\0", 0)[:4]
        x = packed >> 38
        y = packed & 0xFFF
        z = (packed >> 12) & 0x3FFFFFF
        x -= (1 << 26) if x >= (1 << 25) else 0
        z -= (1 << 26) if z >= (1 << 25) else 0
        y -= (1 << 12) if y >= (1 << 11) else 0
        return {"packet": "world_event", "event": event, "pos": [x, y, z],
                "data": data, "global": payload[16] != 0 if len(payload) > 16 else None}
    if packet_id == CB_SOUND:
        sound, i = read_varint(payload, 0)
        name = None
        if sound == 0:
            length, i = read_varint(payload, i)
            name = payload[i:i + length].decode()
            i += length
            has_range = payload[i]
            i += 1 + (4 if has_range else 0)
        category, i = read_varint(payload, i)
        x, y, z, volume, pitch = struct.unpack_from(">iiiff", payload, i)
        return {"packet": "sound", "id": sound - 1 if sound else None, "name": name,
                "category": category, "pos": [x / 8, y / 8, z / 8],
                "volume": round(volume, 4), "pitch": round(pitch, 4)}
    if packet_id == CB_STOP_SOUND:
        return {"packet": "stop_sound", "hex": payload.hex()}
    return None


def main() -> int:
    out_path = Path(sys.argv[1]) if len(sys.argv) > 1 else SCRATCH / "jukebox_vanilla.json"
    normalized = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
    registries = json.loads((normalized / "registries.json").read_text())["registries"]
    items = registries["minecraft:item"]["entries"]
    sounds = registries["minecraft:sound_event"]["entries"]

    def item(name: str) -> int:
        return items.index(f"minecraft:{name}")

    # The capture module's server writes under run/; ours goes to .scratch/.
    capture.RUN = SCRATCH / "server"
    if SCRATCH.exists():
        shutil.rmtree(SCRATCH)
    SCRATCH.mkdir(parents=True)
    server = capture.Vanilla()
    document: dict = {"target": "vanilla", "items": {}, "gestures": {}}
    try:
        server.run("gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                   "gamerule doWeatherCycle false", "gamerule sendCommandFeedback false",
                   "time set noon")
        actor = capture.Probe(PORT, "Actor")
        time.sleep(1.0)
        ear = capture.Probe(PORT, "Ear")
        server.run("gamemode creative Actor", "gamemode creative Ear",
                   "tp Actor 0.5 -60 0.5 0 90", "tp Ear 4.5 -60 0.5",
                   "setblock 1 -60 2 minecraft:jukebox", "setblock -1 -60 2 minecraft:jukebox")
        time.sleep(3.0)
        actor.drain()
        ear.drain()

        def record(label: str, seconds: float) -> None:
            time.sleep(seconds)
            gesture = {"actor": [], "ear": []}
            for who, probe in (("actor", actor), ("ear", ear)):
                for _, packet_id, payload in probe.drain():
                    decoded = decode(packet_id, payload)
                    if decoded is not None:
                        if decoded.get("id") is not None:
                            decoded["event_name"] = sounds[decoded["id"]]
                        gesture[who].append(decoded)
            document["gestures"][label] = gesture
            print(f"  {label:22s} actor {gesture['actor']}\n  {'':22s} ear   {gesture['ear']}")

        for disc in ("music_disc_cat", "music_disc_stal"):
            document["items"][disc] = item(disc)
        print(f"  item ids: {document['items']}")

        actor.hold(item("music_disc_cat"))
        time.sleep(0.6)
        actor.drain()
        ear.drain()
        actor.use_item_on(1, -60, 2, face=1, cursor=(0.5, 0.5, 0.5))
        record("insert.cat", 1.0)
        actor.hold(None)
        time.sleep(0.4)
        actor.use_item_on(1, -60, 2, face=1, cursor=(0.5, 0.5, 0.5))
        record("eject.cat", 1.0)

        actor.hold(item("music_disc_stal"))
        time.sleep(0.6)
        actor.drain()
        ear.drain()
        actor.use_item_on(-1, -60, 2, face=1, cursor=(0.5, 0.5, 0.5))
        record("insert.stal", 1.0)
        server.run("setblock -1 -60 2 minecraft:air")
        record("break.stal_jukebox", 1.0)
    finally:
        server.stop()
    out_path.write_text(json.dumps(document, indent=1) + "\n")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
