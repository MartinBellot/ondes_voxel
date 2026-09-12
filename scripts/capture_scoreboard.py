#!/usr/bin/env python3
"""Ask a server the scoreboard, team, trigger and teammsg commands, and write
down, byte for byte, what it answers — and what it saves.

The same script is pointed at two servers, like scripts/capture_commands.py:

  vanilla   the real 1.20.1 jar (`tools/vanilla/server.jar`), flat world;
  ov        our `ov_dedicated`, on a fresh superflat.

Two probes written from the protocol page. `ovprobe` is an operator and types
every command of PHASE_ONE; everything that comes back is recorded with its
bytes — chat, and the four scoreboard packets (Display Objective 0x51, Update
Objectives 0x58, Update Teams 0x5A, Update Score 0x5B). Then `ovother` joins,
not an operator, and what it is sent on arrival is recorded (the scoreboard a
newcomer is shown). PHASE_TWO then runs with both: team chat, friendly fire,
a kill for the kill criteria, /trigger at permission level 0.

At the end `save-all` is run and `world/data/scoreboard.dat` is copied out.

    python3 scripts/capture_scoreboard.py vanilla   # in the java lane
    python3 scripts/capture_scoreboard.py ov
    python3 scripts/capture_scoreboard.py crossload FILE [name]

`crossload` starts vanilla on a world whose data/scoreboard.dat is FILE, asks
it what it holds (objectives, scores, teams — through a probe, so the answers
are JSON) and copies back the scoreboard.dat it saves. Run on vanilla's own
file it is the control; run on ours it is the measurement.

Writes .scratch/scoreboard/.
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
from capture_commands import CommandProbe, FlatVanilla, OvServer, is_sync  # noqa: E402
from capture_entity_packets import varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / ".scratch" / "scoreboard"

SB_INTERACT = 0x10
SB_CLIENT_COMMAND = 0x07
CB_LOGIN = 0x28

SCOREBOARD_IDS = {0x51, 0x58, 0x5A, 0x5B}

PHASE_ONE: list[str] = [
    "tp @s 0.5 -60 0.5",
    "gamemode creative",
    # ── objectives ──
    "scoreboard objectives list",
    "scoreboard objectives add kills playerKillCount",
    "scoreboard objectives add kills dummy",
    "scoreboard objectives add d dummy",
    'scoreboard objectives add d2 dummy "Two"',
    'scoreboard objectives add d3 dummy {"text":"Three","color":"gold"}',
    "scoreboard objectives add bad nosuchcriterion",
    "scoreboard objectives add abcdefghijklmnopqrstuvwxyz dummy",
    "scoreboard objectives add trig trigger",
    "scoreboard objectives add deaths deathCount",
    "scoreboard objectives add total totalKillCount",
    "scoreboard objectives add hp health",
    "scoreboard objectives add food food",
    "scoreboard objectives add air air",
    "scoreboard objectives add armor armor",
    "scoreboard objectives add xp xp",
    "scoreboard objectives add lvl level",
    "scoreboard objectives add tkblue teamkill.blue",
    "scoreboard objectives add kbred killedByTeam.red",
    "scoreboard objectives add tkbad teamkill.nocolor",
    "scoreboard objectives add mined minecraft.mined:minecraft.stone",
    "scoreboard objectives add jumps minecraft.custom:minecraft.jump",
    "scoreboard objectives add badstat minecraft.mined:minecraft.nosuch",
    "scoreboard objectives add 'q' dummy",
    "scoreboard objectives add",
    "scoreboard objectives list",
    'scoreboard objectives modify d2 displayname {"text":"Deux"}',
    "scoreboard objectives modify d rendertype hearts",
    "scoreboard objectives modify d rendertype integer",
    'scoreboard objectives modify nosuch displayname "x"',
    "scoreboard objectives setdisplay sidebar d",
    "scoreboard objectives setdisplay sidebar d",
    "scoreboard objectives setdisplay list hp",
    "scoreboard objectives setdisplay belowName d2",
    "scoreboard objectives setdisplay sidebar.team.red d3",
    "scoreboard objectives setdisplay sidebar.team.nocolor d3",
    "scoreboard objectives setdisplay nosuchslot d",
    "scoreboard objectives setdisplay sidebar.team.red",
    "scoreboard objectives setdisplay sidebar.team.red",
    "scoreboard objectives setdisplay sidebar.team.blue d3",
    "scoreboard objectives remove d3",
    "scoreboard objectives remove nosuch",
    # ── players ──
    "scoreboard players list",
    "scoreboard players set @s d 5",
    "scoreboard players set fake d 7",
    "scoreboard players set #hidden d 1",
    "scoreboard players set * d 3",
    "scoreboard players add @s d 10",
    "scoreboard players add @s d -1",
    "scoreboard players remove @s d 2",
    "scoreboard players get @s d",
    "scoreboard players get fake d",
    "scoreboard players get nobody d",
    "scoreboard players get @s nosuch",
    "scoreboard players get @s d2",
    "scoreboard players set @s hp 3",
    "scoreboard players set @s d 1.5",
    "scoreboard players list",
    "scoreboard players list @s",
    "scoreboard players list fake",
    "scoreboard players list nobody",
    "scoreboard players list *",
    "scoreboard players set @s d2 1",
    "scoreboard players set @s kills 4",
    "scoreboard players reset fake d",
    "scoreboard players reset fake",
    "scoreboard players reset nobody",
    "scoreboard players reset nobody d",
    "scoreboard players set a d 7",
    "scoreboard players set b d 2",
    "scoreboard players operation a d += b d",
    "scoreboard players operation a d -= b d",
    "scoreboard players operation a d *= b d",
    "scoreboard players operation a d /= b d",
    "scoreboard players operation a d %= b d",
    "scoreboard players operation a d = b d",
    "scoreboard players set a d 7",
    "scoreboard players operation a d < b d",
    "scoreboard players get a d",
    "scoreboard players set a d 1",
    "scoreboard players operation a d > b d",
    "scoreboard players get a d",
    "scoreboard players set a d 9",
    "scoreboard players operation a d >< b d",
    "scoreboard players get a d",
    "scoreboard players get b d",
    "scoreboard players set a d -7",
    "scoreboard players set b d 2",
    "scoreboard players operation a d /= b d",
    "scoreboard players set a d -7",
    "scoreboard players operation a d %= b d",
    "scoreboard players set b d 0",
    "scoreboard players operation a d /= b d",
    "scoreboard players operation a d %= b d",
    "scoreboard players operation a d foo b d",
    "scoreboard players operation a d += nobody d",
    "scoreboard players get nobody d",
    "scoreboard players operation a,b d += b d",
    "scoreboard players operation * d += b d",
    "scoreboard players operation a d += * d",
    "scoreboard players set a d 2147483647",
    "scoreboard players add a d 1",
    "scoreboard players get a d",
    "scoreboard players operation a hp = b d",
    "scoreboard players enable @s trig",
    "scoreboard players enable @s trig",
    "scoreboard players enable @s d",
    "scoreboard players enable fake trig",
    "trigger trig",
    "trigger trig add 5",
    "trigger trig set 9",
    "trigger d",
    "trigger nosuch",
    "scoreboard players enable @s trig",
    "trigger trig add 5",
    "scoreboard players get @s trig",
    "scoreboard players enable @s trig",
    "trigger trig set 9",
    "scoreboard players get @s trig",
    "scoreboard players list @s",
    # ── teams ──
    "team list",
    "team add red",
    "team add red",
    'team add blue "Blue Team"',
    'team add green {"text":"G","color":"green"}',
    "team add abcdefghijklmnopqrstuvwxyz",
    "team list",
    "team list red",
    "team list nosuch",
    "team join red",
    "team join red @s",
    "team join red fake",
    "team join blue fake2",
    "team join blue fake2 fake3",
    "team join nosuch @s",
    "team list red",
    "team list",
    "team modify red color red",
    "team modify red color red",
    "team modify red color nocolor",
    "team modify red friendlyFire false",
    "team modify red friendlyFire false",
    "team modify red seeFriendlyInvisibles false",
    "team modify red seeFriendlyInvisibles false",
    "team modify red nametagVisibility hideForOtherTeams",
    "team modify red nametagVisibility hideForOtherTeams",
    "team modify red deathMessageVisibility hideForOwnTeam",
    "team modify red deathMessageVisibility hideForOwnTeam",
    "team modify red collisionRule pushOwnTeam",
    "team modify red collisionRule pushOwnTeam",
    'team modify red displayName {"text":"Rouge","color":"dark_red"}',
    'team modify red displayName {"text":"Rouge","color":"dark_red"}',
    'team modify red prefix {"text":"[R] "}',
    'team modify red suffix " !"',
    "team modify red nosuchoption x",
    "team modify nosuch color red",
    "team modify blue color blue",
    "team modify green color reset",
    "team leave fake",
    "team leave fake",
    "team leave nobody",
    "team leave fake2 fake3",
    "say hi from red",
    "me waves in red",
    'tellraw @s {"selector":"@s"}',
    "msg @s psst",
    "teammsg hello team",
    "tm hello again",
    "list",
    "team leave @s",
    "teammsg alone",
    "team empty blue",
    "team empty blue",
    "team empty nosuch",
    "team remove green",
    "team remove nosuch",
    "team list",
    "team join red",
    "scoreboard objectives setdisplay sidebar.team.red d",
    "scoreboard players set @s d 12",
    "scoreboard objectives setdisplay list",
    "scoreboard objectives setdisplay list hp",
    # ── score components (appended: earlier captures stay aligned) ──
    'tellraw @s {"score":{"name":"@s","objective":"d"}}',
    'tellraw @s {"score":{"name":"*","objective":"d"}}',
    'tellraw @s ["n=",{"score":{"name":"nobody","objective":"d"}}]',
    'tellraw @s {"score":{"name":"ovprobe","objective":"nosuch"}}',
]

CHAT_ONE: list[str] = ["hello from red"]

# Run with both probes connected. "A:" is typed by ovprobe (operator), "B:" by
# ovother (level 0), "attack" is an Interact packet from A at B, "respawn" is
# B's Client Command, "wait" pumps both for a second.
PHASE_TWO: list[str] = [
    "A:team join red ovother",
    "A:gamemode survival @a",
    "A:tp ovprobe 0.5 -60 0.5",
    "A:tp ovother 1.5 -60 0.5",
    "wait",
    "attack",
    "wait",
    "A:team modify red friendlyFire true",
    "attack",
    "wait",
    "A:team modify red friendlyFire false",
    "A:team join blue ovother",
    "A:effect give ovother minecraft:instant_health 1 5",
    "wait",
    "attack",
    "wait",
    "B:teammsg lonely in blue",
    "A:team join red ovother",
    "A:teammsg to both of us",
    "B:tm and back",
    "B:say not allowed",
    "A:team join blue ovother",
    'A:give ovprobe diamond_sword{Enchantments:[{id:"minecraft:sharpness",lvl:255s}]}',
    "wait",
    "wait",
    "attack",
    "wait",
    "respawn",
    "wait",
    "A:scoreboard players list ovprobe",
    "A:scoreboard players list ovother",
    "A:scoreboard players get ovother deaths",
    "A:scoreboard players get ovprobe kills",
    "A:scoreboard players get ovprobe total",
    "A:scoreboard players get ovprobe tkblue",
    "A:scoreboard players get ovother kbred",
    "A:scoreboard players enable ovother trig",
    "B:trigger trig",
    "B:trigger trig",
    "B:trigger trig set 3",
    "B:scoreboard players list",
    "A:xp add ovother 3 levels",
    "A:xp add ovother 5 points",
    "wait",
    "A:scoreboard players get ovother xp",
    "A:scoreboard players get ovother lvl",
    "A:scoreboard players get ovother hp",
    "A:scoreboard players get ovother food",
    "A:scoreboard players get ovother air",
    "A:scoreboard players get ovother armor",
    "A:scoreboard players list ovother",
    "A:team leave ovother",
    "A:team remove blue",
    "A:scoreboard objectives remove xp",
]

CONSOLE: list[str] = [
    "scoreboard objectives list", "team list", "trigger trig", "teammsg from console",
    "scoreboard players get ovprobe d",
]

# What a server that read a scoreboard.dat is asked about it.
CROSSLOAD: list[str] = [
    "scoreboard objectives list", "scoreboard players list", "team list",
    "team list red", "team list blue",
    "scoreboard players list ovprobe", "scoreboard players list ovother",
    "scoreboard players list a", "scoreboard players list b",
    "scoreboard players list #hidden", "scoreboard players list fake2",
    "scoreboard players list fake3",
    "scoreboard players get ovother trig",
]


def interact_attack(entity: int) -> bytes:
    return varint(entity) + varint(1) + bytes([0])


def login_entity_id(packets: list[tuple[int, bytes]]) -> int | None:
    for pid, payload in packets:
        if pid == CB_LOGIN:
            return struct.unpack_from(">i", payload, 0)[0]
    return None


def start(target: str, port: int, run_dir: Path):
    if target == "vanilla":
        if run_dir.exists():
            shutil.rmtree(run_dir)
        return FlatVanilla(run_dir, port=port)
    return OvServer(run_dir, port)


def scoreboard_file(target: str, run_dir: Path) -> Path:
    return run_dir / "world" / "data" / "scoreboard.dat"


def capture(target: str) -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    port = int(os.environ.get("OV_CAPTURE_PORT", 25708 if target == "vanilla" else 25608))
    run_dir = OUT / f"run-{target}"
    server = start(target, port, run_dir)
    document: dict = {"target": target, "phase_one": [], "chat_one": [], "arrival": [],
                      "phase_two": [], "console": []}
    try:
        server.batch(["gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                      "gamerule doWeatherCycle false", "gamerule doImmediateRespawn true",
                      "time set 1000", "weather clear", "difficulty easy",
                      "defaultgamemode survival"])
        a = CommandProbe(port)
        a.pump(4.0)
        a.drain()
        server.batch(["op ovprobe"])
        a.pump(1.5)
        a.take()

        for text in PHASE_ONE:
            a.command(text)
            a.pump(0.7)
            packets = [e for e in a.take() if not is_sync(e)]
            document["phase_one"].append({"command": text, "packets": packets})
            print(f"  /{text}: {len(packets)} packets")
        for text in CHAT_ONE:
            a.chat(text)
            a.pump(0.7)
            document["chat_one"].append({"message": text, "packets": a.take()})

        b = CommandProbe(port, "ovother")
        b.pump(4.0)
        arrival = b.drain()
        b_id = login_entity_id(arrival)
        document["arrival"] = [{"id": pid, "hex": p.hex()} for pid, p in arrival
                               if pid in SCOREBOARD_IDS or pid in (0x64, 0x3A)]
        a.pump(0.5)
        document["arrival_seen_by_a"] = a.take()
        print(f"ovother arrived as entity {b_id}: {len(document['arrival'])} scoreboard packets")

        for step in PHASE_TWO:
            if step.startswith("A:"):
                a.command(step[2:])
            elif step.startswith("B:"):
                b.command(step[2:])
            elif step == "attack":
                a.send(SB_INTERACT, interact_attack(b_id or 0))
            elif step == "respawn":
                b.send(SB_CLIENT_COMMAND, varint(0))
            a.pump(0.5 if step != "wait" else 1.0)
            b.pump(0.3)
            document["phase_two"].append({"step": step,
                                          "a": [e for e in a.take() if not is_sync(e)],
                                          "b": [e for e in b.take() if not is_sync(e)]})
            print(f"  {step}")

        for text in CONSOLE:
            lines = server.batch([text])
            a.pump(0.3)
            document["console"].append({"command": text, "lines": lines,
                                        "packets": [e for e in a.take() if not is_sync(e)]})

        server.batch(["save-all flush"] if target == "vanilla" else ["save-all"])
        time.sleep(1.0)
        source = scoreboard_file(target, run_dir)
        if source.exists():
            shutil.copy(source, OUT / f"{target}_scoreboard.dat")
            print(f"copied {source}")
        else:
            print(f"no {source}")
        with open(OUT / f"capture_{target}.json", "w") as handle:
            json.dump(document, handle, indent=1, sort_keys=True)
        print(f"wrote {OUT / f'capture_{target}.json'}")
    finally:
        server.stop()
        if target == "vanilla":
            shutil.rmtree(run_dir, ignore_errors=True)
    return 0


def crossload(path: Path, name: str) -> int:
    """Vanilla reads `path` as its scoreboard, answers about it, and saves it."""
    OUT.mkdir(parents=True, exist_ok=True)
    port = int(os.environ.get("OV_CAPTURE_PORT", 25708))
    run_dir = OUT / f"run-crossload-{name}"
    if run_dir.exists():
        shutil.rmtree(run_dir)
    (run_dir / "world" / "data").mkdir(parents=True)
    shutil.copy(path, run_dir / "world" / "data" / "scoreboard.dat")
    server = FlatVanilla(run_dir, port=port)
    document: dict = {"file": str(path), "answers": []}
    try:
        a = CommandProbe(port)
        a.pump(4.0)
        document["arrival"] = [{"id": pid, "hex": p.hex()} for pid, p in a.drain()
                               if pid in SCOREBOARD_IDS]
        server.batch(["op ovprobe"])
        a.pump(1.5)
        a.take()
        # Every objective, holder and team the file names, asked one by one.
        listing = []
        for text in CROSSLOAD:
            a.command(text)
            a.pump(0.7)
            listing.append({"command": text, "packets": [e for e in a.take() if not is_sync(e)]})
        document["answers"] = listing
        server.batch(["save-all flush"])
        time.sleep(1.0)
        shutil.copy(run_dir / "world" / "data" / "scoreboard.dat",
                    OUT / f"crossload_{name}_saved.dat")
        with open(OUT / f"crossload_{name}.json", "w") as handle:
            json.dump(document, handle, indent=1, sort_keys=True)
        print(f"wrote {OUT / f'crossload_{name}.json'}")
    finally:
        server.stop()
        shutil.rmtree(run_dir, ignore_errors=True)
    return 0


def main() -> int:
    target = sys.argv[1] if len(sys.argv) > 1 else "vanilla"
    if target == "crossload":
        path = Path(sys.argv[2])
        return crossload(path, sys.argv[3] if len(sys.argv) > 3 else path.stem)
    return capture(target)


if __name__ == "__main__":
    sys.exit(main())
