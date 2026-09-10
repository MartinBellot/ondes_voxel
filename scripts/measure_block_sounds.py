#!/usr/bin/env python3
"""Ask a real 1.20.1 server what every block sounds like.

A block's sounds — breaking, stepping, placing, hitting, falling, with one
volume and one pitch for the set — are Java code, in no data generator
report. Three of the five travel on the wire, and capture_sound_packets.py
showed which and how:

  place   Another player hears the placing as Sound Effect `…place`, at the
          block's centre, volume (v+1)/2, pitch p*0.8. The placer does not:
          its own client already played it.
  step    Another player hears a walker's footsteps as `…step`, at the
          walker's feet, category player, volume v*0.15, pitch p.
  fall    Another player hears a survival fall onto a block as `…fall`,
          volume v*0.5, pitch p*0.75, beside entity.player.small_fall/hurt.

Breaking and hitting never do: breaking is World Event 2001 with a state id
and the client plays the sound itself, and hitting is the client's own. So
those two are **inferred** from the family the other three name, and the
table says which fields were measured and which were inferred.

Three phases, one server:

  place   The actor holds each block's item (creative) and clicks the top of
          the floor two blocks away; the ear records the Sound Effect **and**
          the Block Update, so a placement is checked by the state that landed,
          not assumed. Blocks with no item of their own name are named.
  step    A walkway of three-block cells, one block per cell, laid by `fill`.
          The actor is teleported to each cell and walks 2.6 blocks on it: a
          step comes every 1/0.6 blocks of travel, so every cell gets one, and
          the sound's position says which cell it was.
  fall    A five-block survival fall onto each cell, under Regeneration, at
          least 0.6 s apart — the ten-tick invulnerability window would
          otherwise swallow every other landing and its sound.

Usage:
    python3 scripts/measure_block_sounds.py [place|step|fall|table|all]

Raw captures go to data/vanilla/1.20.1/normalized/block_sounds_raw.json and
the derived table to normalized/block_sounds.json (both gitignored: they are
measurements of Mojang's server), which tools/ov_datagen/ovpack.py packs.
"""
from __future__ import annotations

import bisect
import json
import os
import socket
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_sound_packets import Probe  # noqa: E402
from decode_sound_packets import decode, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
RUN = ROOT / "run" / "block-sounds"
RAW = NORMALIZED / "block_sounds_raw.json"
TABLE = NORMALIZED / "block_sounds.json"
PORT = int(os.environ.get("OV_BLOCK_SOUND_PORT", "25748"))

# The walkway: cells of five blocks along +X, six to a row, rows four apart.
# The ear stands between two rows at the row's middle, never farther than
# 12.7 blocks from a cell — inside the 16 a footstep carries.
#
# ⚠ Five and not three. The first walk used three-block cells and walked
#   0.2 .. 2.8 into each: at 2.8 the player's 0.6-wide box already overlaps the
#   next cell, and the game takes the footstep from a block the box stands on,
#   not strictly the one under the feet. 101 blocks came out with their
#   neighbour's step — allium with the amethyst block's, the acacia trapdoor
#   with a hanging sign's. Walking 0.8 .. 4.2 of five keeps the whole box on
#   one cell and still covers 3.4 blocks, two footsteps' worth.
CELL = 5
PER_ROW = 6
WALK_FROM = 0.8
WALK_TO = 4.2
ROW_SPACING = 4
ROW_Z0 = 20
FLOOR_Y = -61


# ── data ────────────────────────────────────────────────────────────────────

def load() -> tuple[list[dict], list[str], list[str]]:
    blocks = json.loads((NORMALIZED / "blocks.json").read_text())["blocks"]
    registries = json.loads((NORMALIZED / "registries.json").read_text())["registries"]
    return (blocks, registries["minecraft:item"]["entries"],
            registries["minecraft:sound_event"]["entries"])


class States:
    """State id -> block name, by bisection over the base states."""

    def __init__(self, blocks: list[dict]) -> None:
        ordered = sorted(blocks, key=lambda b: b["base_state"])
        self.bases = [b["base_state"] for b in ordered]
        self.names = [b["name"] for b in ordered]

    def name(self, state: int) -> str:
        return self.names[bisect.bisect_right(self.bases, state) - 1]


def block_updates(records, states: States) -> list[tuple[tuple[int, int, int], str]]:
    """Every (position, block name) a capture's Block Update and Update Section
    Blocks packets set. Section Blocks has no trust-edges boolean in 763: the
    capture reads long, varint count, varlongs."""
    out = []
    for _, packet_id, payload in records:
        if packet_id == 0x0A:
            packed = struct.unpack_from(">q", payload, 0)[0]
            x = packed >> 38
            y = packed & 0xFFF
            y = y - 4096 if y >= 2048 else y
            z = (packed >> 12) & 0x3FFFFFF
            z = z - (1 << 26) if z >= 1 << 25 else z
            state, _ = varint(payload, 8)
            out.append(((x, y, z), states.name(state)))
        elif packet_id == 0x43:
            section = struct.unpack_from(">q", payload, 0)[0]
            sx = section >> 42
            sy = section & 0xFFFFF
            sy = sy - (1 << 20) if sy >= 1 << 19 else sy
            sz = (section >> 20) & 0x3FFFFF
            sz = sz - (1 << 22) if sz >= 1 << 21 else sz
            count, i = varint(payload, 8)
            for _ in range(count):
                value, i = varint(payload, i)
                state = value >> 12
                lx, lz, ly = (value >> 8) & 0xF, (value >> 4) & 0xF, value & 0xF
                out.append(((sx * 16 + lx, sy * 16 + ly, sz * 16 + lz), states.name(state)))
    return out


def sounds_in(records, names: list[str]) -> list[dict]:
    return [d for _, pid, payload in records
            if pid == 0x62 and (d := decode(pid, payload, names)) is not None]


# ── the server ──────────────────────────────────────────────────────────────

def start():
    from measure_entities import Server
    wait_for_port(PORT, "OV_BLOCK_SOUND_PORT")
    server = Server(RUN, port=PORT)
    server.batch(["gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                  "gamerule doWeatherCycle false", "gamerule randomTickSpeed 0",
                  "gamerule doTileDrops false", "gamerule doFireTick false",
                  "gamerule sendCommandFeedback false", "gamerule doEntityDrops false",
                  "difficulty peaceful", "time set noon",
                  "forceload add -8 -8 40 720"])
    actor = Probe(PORT, "Actor")
    time.sleep(1.0)
    ear = Probe(PORT, "Ear")
    time.sleep(2.0)
    return server, actor, ear


def wait_for_port(port: int, variable: str, seconds: float = 30.0) -> None:
    """Refuse a port another server holds, but wait out one that is closing.

    Two phases in a row failed on the second: the first phase's JVM was still
    letting go of the port when the next one checked it. SO_REUSEADDR is what
    Java's own server socket sets, so a port that bind would accept for the
    real server is accepted here too."""
    deadline = time.monotonic() + seconds
    while True:
        probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            probe.bind(("0.0.0.0", port))
            return
        except OSError:
            if time.monotonic() > deadline:
                sys.exit(f"port {port} is taken — set {variable} to a free one")
            time.sleep(1.0)
        finally:
            probe.close()


def cell_of(index: int) -> tuple[int, int]:
    """West end x and z of a walkway cell."""
    return (index % PER_ROW) * CELL, ROW_Z0 + (index // PER_ROW) * ROW_SPACING


# ── phases ──────────────────────────────────────────────────────────────────

def phase_place(server, actor, ear, blocks, items, names, states, raw) -> None:
    server.batch(["gamemode creative Actor", "gamemode creative Ear",
                  "tp Actor 0.5 -60 0.5 -90 45", "tp Ear 6.5 -60 0.5",
                  "fill -2 -61 -3 5 -61 3 minecraft:grass_block"])
    time.sleep(1.0)
    target = (2, -60, 0)
    for number, block in enumerate(blocks):
        name = block["name"]
        entry = raw.setdefault(name, {})
        if name not in items:
            entry["place"] = {"skipped": "no item of this name"}
            continue
        actor.hold(items.index(name))
        time.sleep(0.12)
        actor.drain()
        ear.drain()
        actor.use_item_on(2, -61, 0, face=1)
        time.sleep(0.35)
        heard = ear.drain()
        landed = [b for pos, b in block_updates(heard, states)
                  if pos in (target, (2, -59, 0), (3, -60, 0))]
        entry["place"] = {"sounds": sounds_in(heard, names), "landed": landed}
        server.batch(["fill 0 -60 -2 4 -56 2 minecraft:air",
                      "fill 0 -61 -2 4 -61 2 minecraft:grass_block"])
        if number % 100 == 0:
            print(f"  place {number}/{len(blocks)} {name}: "
                  f"{[s['sound'] for s in entry['place']['sounds']]} {landed}")


def lay_walkway(server, blocks) -> None:
    rows = (len(blocks) + PER_ROW - 1) // PER_ROW
    last_z = ROW_Z0 + (rows - 1) * ROW_SPACING
    # A clean strip first: every row on grass over dirt, nothing above.
    commands = [f"fill -2 -60 {ROW_Z0 - 2} {PER_ROW * CELL + 2} -55 {z} minecraft:air"
                for z in range(ROW_Z0 - 2, last_z + 3, 16)]
    commands = [f"fill -2 -60 {z} {PER_ROW * CELL + 2} -55 {min(z + 15, last_z + 2)} minecraft:air"
                for z in range(ROW_Z0 - 2, last_z + 3, 16)]
    commands += [f"fill -2 {FLOOR_Y} {z} {PER_ROW * CELL + 2} {FLOOR_Y} "
                 f"{min(z + 15, last_z + 2)} minecraft:grass_block"
                 for z in range(ROW_Z0 - 2, last_z + 3, 16)]
    for index, block in enumerate(blocks):
        x, z = cell_of(index)
        commands.append(f"fill {x} {FLOOR_Y} {z} {x + CELL - 1} {FLOOR_Y} {z} {block['name']}")
    for i in range(0, len(commands), 200):
        server.batch(commands[i:i + 200], timeout=600.0)


def phase_step(server, actor, ear, blocks, names, raw) -> None:
    lay_walkway(server, blocks)
    server.batch(["gamemode survival Actor", "gamemode creative Ear",
                  "effect give Actor minecraft:saturation infinite 255 true"])
    for index, block in enumerate(blocks):
        x, z = cell_of(index)
        if index % PER_ROW == 0:
            server.batch([f"tp Ear {PER_ROW * CELL / 2} -60 {z + ROW_SPACING / 2}"])
        server.batch([f"tp Actor {x + WALK_FROM} -60 {z + 0.5} -90 0"])
        time.sleep(0.08)
        actor.drain()
        ear.drain()
        position = x + WALK_FROM
        while position < x + WALK_TO:
            position = min(position + 0.25, x + WALK_TO)
            actor.move(position, -60.0, z + 0.5, True)
            time.sleep(0.012)
        time.sleep(0.15)
        heard = sounds_in(ear.drain(), names)
        raw.setdefault(block["name"], {})["step"] = [
            s for s in heard if x <= s["pos"][0] < x + CELL and abs(s["pos"][2] - (z + 0.5)) < 0.01]
        if index % 100 == 0:
            print(f"  step {index}/{len(blocks)} {block['name']}: "
                  f"{[s['sound'] for s in raw[block['name']]['step']]}")


def phase_fall(server, actor, ear, blocks, names, raw) -> None:
    server.batch(["gamemode survival Actor", "gamemode creative Ear",
                  "effect give Actor minecraft:regeneration infinite 255 true",
                  "effect give Actor minecraft:saturation infinite 255 true"])
    for index, block in enumerate(blocks):
        x, z = cell_of(index)
        if index % PER_ROW == 0:
            server.batch([f"tp Ear {PER_ROW * CELL / 2} -60 {z + ROW_SPACING / 2}"])
        middle = x + CELL / 2
        server.batch([f"tp Actor {middle} -55 {z + 0.5}"])
        time.sleep(0.1)
        actor.drain()
        ear.drain()
        y, velocity = -55.0, 0.0
        while y > -60.0:
            velocity = (velocity - 0.08) * 0.98
            y = max(-60.0, y + velocity)
            actor.move(middle, y, z + 0.5, y <= -60.0)
            time.sleep(0.004)
        time.sleep(0.5)
        heard = sounds_in(ear.drain(), names)
        raw.setdefault(block["name"], {})["fall"] = [
            s for s in heard if abs(s["pos"][0] - middle) < 0.01]
        if index % 100 == 0:
            print(f"  fall {index}/{len(blocks)} {block['name']}: "
                  f"{[s['sound'] for s in raw[block['name']]['fall']]}")


# ── the table ───────────────────────────────────────────────────────────────

FACTORS = {  # heard = f(volume, pitch) of the set, per gesture — see the header
    "place": (lambda v: (v + 1) / 2, lambda p: p * 0.8),
    "step": (lambda v: v * 0.15, lambda p: p),
    "fall": (lambda v: v * 0.5, lambda p: p * 0.75),
}
INVERSE = {
    "place": (lambda v: 2 * v - 1, lambda p: p / 0.8),
    "step": (lambda v: v / 0.15, lambda p: p),
    "fall": (lambda v: v / 0.5, lambda p: p / 0.75),
}


def pick(sounds: list[dict], gesture: str) -> dict | None:
    for s in sounds:
        name = s["sound"]
        if name.startswith("minecraft:entity.player"):
            continue
        if gesture == "fall" and not name.endswith(".fall"):
            continue
        return s
    return None


def build_table(blocks, names, raw) -> dict:
    registered = set(names)
    # ⚠ A footstep over a block the player sinks through is not that block's.
    #   The walk over a button laid on dirt was heard as block.gravel.step —
    #   the dirt's. Step and fall are kept only for blocks that stop movement
    #   (normalized/motion.json, measured block by block); for the others they
    #   are set aside and named, never merged into the block's row.
    motion = json.loads((NORMALIZED / "motion.json").read_text())["blocks"]
    table: dict = {}
    counts = {"place": 0, "step": 0, "fall": 0, "consistent": 0, "resolved": 0,
              "discarded": 0}
    disagreements = []
    for block in blocks:
        name = block["name"]
        entry = raw.get(name, {})
        standable = bool(motion.get(name, {}).get("motion"))
        if not standable:
            set_aside = {g: [s["sound"] for s in entry.get(g, [])] for g in ("step", "fall")
                         if entry.get(g)}
            if set_aside:
                counts["discarded"] += 1
            entry = {k: v for k, v in entry.items() if k not in ("step", "fall")}
        else:
            set_aside = {}
        measured = {}
        place = entry.get("place", {})
        if place.get("sounds") and name in place.get("landed", []):
            if (s := pick(place["sounds"], "place")) is not None:
                measured["place"] = s
        elif place.get("sounds") and place.get("landed"):
            # Something else landed (a wall torch for a torch clicked on a
            # side, say): the sound is that block's, not this one's.
            pass
        if (s := pick(entry.get("step", []), "step")) is not None:
            measured["step"] = s
        if (s := pick(entry.get("fall", []), "fall")) is not None:
            measured["fall"] = s
        # ⚠ When the families disagree, the place wins: it is the only gesture
        #   whose block was confirmed by the Block Update that came back. A
        #   dry coral block dies where it is laid and is then walked on as the
        #   dead coral it became (stone); a fence gate is walked on as the dirt
        #   under it. Those are true sounds of something else, set aside.
        if "place" in measured:
            place_family = measured["place"]["sound"].rsplit(".", 1)[0]
            for gesture in [g for g in measured if g != "place"]:
                if measured[gesture]["sound"].rsplit(".", 1)[0] != place_family:
                    set_aside = {**set_aside, gesture: [measured[gesture]["sound"]],
                                 "reason": "family differs from the confirmed place"}
                    del measured[gesture]
        # Families that still disagree with no confirmed place to arbitrate:
        # nothing is kept. The orange wall banner stepped as gravel and fell
        # as wood; picking one would be a guess dressed as a measurement.
        if "place" not in measured and len({s["sound"].rsplit(".", 1)[0]
                                            for s in measured.values()}) > 1:
            set_aside = {**set_aside, **{g: [s["sound"]] for g, s in measured.items()},
                         "reason": "families disagree and no confirmed place arbitrates"}
            measured = {}
        for gesture in measured:
            counts[gesture] += 1

        # The set's volume and pitch, from each measured gesture; they must agree.
        estimates = {g: (round(INVERSE[g][0](s["volume"]), 4), round(INVERSE[g][1](s["pitch"]), 4))
                     for g, s in measured.items()}
        values = set(estimates.values())
        families = {s["sound"].rsplit(".", 1)[0] for s in measured.values()}
        row: dict = {"measured": sorted(measured), "events": {}, "volume": None, "pitch": None}
        if set_aside:
            row["discarded"] = {"reason": "does not stop movement: heard on the block below",
                                **set_aside}
        for g, s in measured.items():
            row["events"][g] = s["sound"]
        if len(values) == 1:
            row["volume"], row["pitch"] = next(iter(values))
        elif values:
            disagreements.append((name, estimates))
            row["disagreement"] = {g: list(v) for g, v in estimates.items()}
        if len(values) == 1 and len(families) == 1 and measured:
            counts["consistent"] += 1
            family = next(iter(families))
            for gesture in ("break", "step", "place", "hit", "fall"):
                if gesture not in row["events"]:
                    candidate = f"{family}.{gesture}"
                    row["events"][gesture] = candidate if candidate in registered else None
            if all(row["events"].get(g) for g in ("break", "step", "place", "hit", "fall")):
                counts["resolved"] += 1
        elif len(families) > 1:
            row["families"] = sorted(families)
        table[name] = row
    return {"$comment": "Sons de bloc mesurés sur un vrai serveur 1.20.1 : pose, pas et chute "
                        "relevés sur le fil, cassage et coup déduits de la famille. Voir "
                        "docs/provenance/son.md.",
            "count": len(blocks), "counts": counts,
            "disagreements": [{"block": n, "estimates": e} for n, e in disagreements],
            "blocks": table}


def main() -> int:
    phase = sys.argv[1] if len(sys.argv) > 1 else "all"
    blocks, items, names = load()
    states = States(blocks)
    raw = json.loads(RAW.read_text()) if RAW.is_file() else {}
    if phase != "table":
        server, actor, ear = start()
        try:
            if phase in ("place", "all"):
                phase_place(server, actor, ear, blocks, items, names, states, raw)
                RAW.write_text(json.dumps(raw, indent=1) + "\n")
            if phase in ("step", "all"):
                phase_step(server, actor, ear, blocks, names, raw)
                RAW.write_text(json.dumps(raw, indent=1) + "\n")
            if phase in ("fall", "all"):
                phase_fall(server, actor, ear, blocks, names, raw)
                RAW.write_text(json.dumps(raw, indent=1) + "\n")
        finally:
            server.stop()
    document = build_table(blocks, names, raw)
    TABLE.write_text(json.dumps(document, indent=1) + "\n")
    c = document["counts"]
    print(f"blocks {document['count']}: place {c['place']}, step {c['step']}, fall {c['fall']}; "
          f"{c['consistent']} consistent, {c['resolved']} with all five events; "
          f"{len(document['disagreements'])} volume/pitch disagreements; "
          f"{c['discarded']} blocks whose step/fall came from the block below")
    return 0


if __name__ == "__main__":
    sys.exit(main())
