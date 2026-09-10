#!/usr/bin/env python3
"""The sounds of things that switch and things that live, on a real 1.20.1 server.

Two tables the server needs and no data generator report holds:

  toggle  Every door, trapdoor, fence gate, button, lever and pressure plate.
          Which event opening and closing plays, at what volume and pitch —
          five cycles each, so a random pitch shows its range — and, the
          question capture_sound_packets.py showed matters most, whether the
          player who did it hears it from the server. Iron doors and trapdoors
          do not open by hand; they are switched by a redstone block placed
          beside them, the way a circuit would. Plates are pressed by the actor
          standing on them.

  mobs    Every living entity type of entities.json. Summoned as an adult,
          hurt three times by /damage, 0.7 s apart (past the ten-tick
          invulnerability window), then /kill: the hurt and death events, the
          category, the volume, and the pitch range.

Usage: python3 scripts/measure_sound_events.py [toggle|mobs|all]

Writes data/vanilla/1.20.1/normalized/sound_events.json (gitignored).
"""
from __future__ import annotations

import json
import os
import socket
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_sound_packets import Probe, spawn_entity_ids  # noqa: E402
from decode_sound_packets import decode  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
RUN = ROOT / "run" / "sound-events"
OUT = NORMALIZED / "sound_events.json"
PORT = int(os.environ.get("OV_SOUND_EVENTS_PORT", "25749"))
CYCLES = 5

TOGGLE_SUFFIXES = ("_door", "_trapdoor", "_fence_gate", "_button", "_pressure_plate")


def sounds(records, names):
    return [d for _, pid, payload in records
            if pid == 0x62 and (d := decode(pid, payload, names)) is not None]


def start():
    from measure_entities import Server
    from measure_block_sounds import wait_for_port
    wait_for_port(PORT, "OV_SOUND_EVENTS_PORT")
    server = Server(RUN, port=PORT)
    server.batch(["gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                  "gamerule doWeatherCycle false", "gamerule randomTickSpeed 0",
                  "gamerule doTileDrops false", "gamerule doEntityDrops false",
                  "gamerule doMobLoot false", "gamerule sendCommandFeedback false",
                  "difficulty normal", "time set midnight", "forceload add -16 -16 16 16"])
    actor = Probe(PORT, "Actor")
    time.sleep(1.0)
    ear = Probe(PORT, "Ear")
    server.batch(["gamemode creative Actor", "gamemode creative Ear",
                  "tp Actor 0.5 -60 0.5 0 0", "tp Ear 4.5 -60 0.5"])
    time.sleep(2.0)
    return server, actor, ear


def listen(actor, ear, seconds, names):
    time.sleep(seconds)
    return {"actor": sounds(actor.drain(), names), "ear": sounds(ear.drain(), names)}


def phase_toggle(server, actor, ear, blocks, names) -> dict:
    out = {}
    targets = [b["name"] for b in blocks
               if b["name"].endswith(TOGGLE_SUFFIXES) or b["name"] == "minecraft:lever"]
    actor.hold(None)
    at = (2, -60, 0)
    for name in targets:
        short = name.split(":")[1]
        iron = short.startswith("iron_")
        plate = short.endswith("_pressure_plate")
        server.batch(["fill 1 -60 -2 4 -57 2 minecraft:air",
                      "fill 1 -61 -2 4 -61 2 minecraft:stone"])
        if short.endswith("_door"):
            server.batch([f"setblock 2 -60 0 {name}[half=lower,facing=west]",
                          f"setblock 2 -59 0 {name}[half=upper,facing=west]"])
        elif short.endswith("_button") or short == "lever":
            server.batch([f"setblock 2 -60 0 {name}[face=floor]"])
        else:
            server.batch([f"setblock 2 -60 0 {name}"])
        time.sleep(0.3)
        actor.drain()
        ear.drain()
        cycles = []
        for _ in range(CYCLES):
            if plate:
                server.batch(["tp Actor 2.5 -60 0.5"])
                opened = listen(actor, ear, 0.6, names)
                server.batch(["tp Actor 0.5 -60 0.5"])
                closed = listen(actor, ear, 1.6, names)
            elif iron:
                server.batch(["setblock 3 -60 0 minecraft:redstone_block"])
                opened = listen(actor, ear, 0.4, names)
                server.batch(["setblock 3 -60 0 minecraft:air"])
                closed = listen(actor, ear, 0.4, names)
            elif short.endswith("_button"):
                actor.use_item_on(*at, face=1, cursor=(0.5, 0.1, 0.5))
                opened = listen(actor, ear, 0.3, names)
                closed = listen(actor, ear, 1.9, names)  # releases on its own
            else:
                actor.use_item_on(*at, face=1, cursor=(0.5, 0.5, 0.5))
                opened = listen(actor, ear, 0.4, names)
                actor.use_item_on(*at, face=1, cursor=(0.5, 0.5, 0.5))
                closed = listen(actor, ear, 0.4, names)
            cycles.append({"open": opened, "close": closed})
        out[name] = {"how": "plate" if plate else "redstone" if iron else "hand",
                     "cycles": cycles}
        first = cycles[0]
        print(f"  {short:36s} open {[s['sound'].split(':')[1] for s in first['open']['ear']]}"
              f" close {[s['sound'].split(':')[1] for s in first['close']['ear']]}")
    return out


def living_types() -> list[str]:
    doc = json.loads((NORMALIZED / "entities.json").read_text())
    attributes = doc["attributes"]
    return sorted(t for t, values in attributes.items()
                  if values and t not in ("minecraft:player", "minecraft:armor_stand"))


def phase_mobs(server, actor, ear, names) -> dict:
    out = {}
    for kind in living_types():
        short = kind.split(":")[1]
        server.batch(["kill @e[type=!minecraft:player]", "fill -2 -60 -6 8 -55 -2 minecraft:air"])
        time.sleep(0.3)
        actor.drain()
        ear.drain()
        server.batch([f"summon {kind} 3.5 -60 -4.5 {{NoAI:1b,PersistenceRequired:1b,"
                      f"IsBaby:0b,Age:0,Tags:[\"ovmob\"]}}"])
        time.sleep(0.4)
        spawned = spawn_entity_ids(ear.drain())
        actor.drain()
        hurts = []
        for _ in range(3):
            server.batch(["damage @e[tag=ovmob,limit=1] 1 minecraft:generic"])
            hurts.append(listen(actor, ear, 0.7, names)["ear"])
        server.batch(["kill @e[tag=ovmob]"])
        death = listen(actor, ear, 1.2, names)["ear"]
        out[kind] = {"spawned": bool(spawned), "hurt": hurts, "death": death}
        first = hurts[0][0]["sound"].split(":")[1] if hurts and hurts[0] else "-"
        last = death[0]["sound"].split(":")[1] if death else "-"
        print(f"  {short:28s} hurt {first:36s} death {last}")
    return out


# ── the tables tools/ov_datagen/ovpack.py packs ─────────────────────────────

# The toggled block sits at (2, -60, 0); its sounds are heard at the centre.
TOGGLE_CENTRE = (2.5, -59.5, 0.5)


def summarise(listens: list[dict], at: tuple[float, float, float] | None) -> dict | None:
    """One gesture over every cycle: the event, volume, pitch range, audience.

    Audience is 'everyone' when the actor heard it on every cycle, 'others'
    when it never did, and 'mixed' otherwise — named rather than rounded."""
    ear, actor_heard, per_cycle = [], 0, 0
    for listen in listens:
        own = [s for s in listen["ear"] if at is None or
               all(abs(a - b) < 0.01 for a, b in zip(s["pos"], at))]
        if not own:
            continue
        per_cycle += 1
        ear.extend(own)
        if any(s["sound"] == own[0]["sound"] for s in listen["actor"]):
            actor_heard += 1
    if not ear:
        return None
    names = sorted({s["sound"] for s in ear})
    pitches = [s["pitch"] for s in ear]
    volumes = sorted({s["volume"] for s in ear})
    return {"sound": names[0] if len(names) == 1 else None, "sounds": names,
            "volume": volumes[0] if len(volumes) == 1 else None, "volumes": volumes,
            "pitch_lo": min(pitches), "pitch_hi": max(pitches), "samples": len(ear),
            "audience": ("everyone" if actor_heard == per_cycle else
                         "others" if actor_heard == 0 else "mixed")}


def derive(document: dict, names: list[str]) -> None:
    toggles = {}
    for block, record in document.get("toggle", {}).items():
        opened = summarise([c["open"] for c in record["cycles"]], TOGGLE_CENTRE)
        closed = summarise([c["close"] for c in record["cycles"]], TOGGLE_CENTRE)
        toggles[block] = {"how": record["how"], "open": opened, "close": closed}
    registered = set(names)

    def named(window: list[dict], stem: str) -> list[dict]:
        # ⚠ An ambient sound can fall inside the 0.7 s after a hit: the first
        #   derivation recorded the piglin brute's hurt as its ambient. Kept by
        #   name — `hurt`, `hurt_land`, `death`, `death_land` — and anything
        #   else heard in the window is set aside rather than taken for it.
        return [s for s in window if s["sound"].rsplit(".", 1)[-1].startswith(stem)]

    mobs = {}
    for kind, record in document.get("mobs", {}).items():
        hurt = summarise([{"ear": named(h, "hurt"), "actor": []} for h in record["hurt"]], None)
        death = summarise([{"ear": named(record["death"], "death"), "actor": []}], None)
        ambient = f"minecraft:entity.{kind.split(':')[1]}.ambient"
        mobs[kind] = {"hurt": hurt, "death": death,
                      "ambient": ambient if ambient in registered else None,
                      "category": (hurt or death or {}).get("category")}
        # summarise() drops the category; read it back from the first sample.
        for s in (record["hurt"][0] if record["hurt"] else []) + record["death"]:
            mobs[kind]["category"] = s["category"]
            break
    document["toggle_sounds"] = toggles
    document["mob_sounds"] = mobs


def main() -> int:
    phase = sys.argv[1] if len(sys.argv) > 1 else "all"
    if phase == "table":
        names = json.loads((NORMALIZED / "registries.json").read_text())[
            "registries"]["minecraft:sound_event"]["entries"]
        document = json.loads(OUT.read_text())
        derive(document, names)
        OUT.write_text(json.dumps(document, indent=1) + "\n")
        toggles, mobs = document["toggle_sounds"], document["mob_sounds"]
        print(f"toggles {len(toggles)}: "
              f"{sum(1 for t in toggles.values() if t['open'] and t['open']['sound'])} with one "
              f"open sound; mobs {len(mobs)}: "
              f"{sum(1 for m in mobs.values() if m['hurt'] and m['hurt']['sound'])} with one hurt "
              f"sound, {sum(1 for m in mobs.values() if m['death'])} with a death sound")
        return 0
    blocks = json.loads((NORMALIZED / "blocks.json").read_text())["blocks"]
    names = json.loads((NORMALIZED / "registries.json").read_text())[
        "registries"]["minecraft:sound_event"]["entries"]
    document = json.loads(OUT.read_text()) if OUT.is_file() else {
        "$comment": "Sons des blocs qui basculent et des créatures, relevés sur un vrai serveur "
                    "1.20.1. Voir docs/provenance/son.md."}
    server, actor, ear = start()
    try:
        if phase in ("toggle", "all"):
            document["toggle"] = phase_toggle(server, actor, ear, blocks, names)
            OUT.write_text(json.dumps(document, indent=1) + "\n")
        if phase in ("mobs", "all"):
            document["mobs"] = phase_mobs(server, actor, ear, names)
            OUT.write_text(json.dumps(document, indent=1) + "\n")
    finally:
        server.stop()
    derive(document, names)
    OUT.write_text(json.dumps(document, indent=1) + "\n")
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
