#!/usr/bin/env python3
"""Ask a server a list of commands and write down, byte for byte, what it answers.

The same script is pointed at two servers:

  vanilla   the real 1.20.1 jar (`tools/vanilla/server.jar`), flat world, no
            structures, no mob spawning, the daylight cycle stopped;
  ov        our `ov_dedicated`, on a fresh superflat of the same layers.

A probe client written from the protocol page joins as `ovprobe`, is made an
operator from the console, and then types every command of `COMMANDS` through a
real Chat Command packet (0x04). Whatever comes back — System Chat Message,
Player Chat Message, Disguised Chat Message, Game Event, titles, the Commands
tree itself — is recorded with its bytes and, for the chat packets, decoded.

Nothing here is a claim about what the answer *should* be. It is the oracle's
side of the comparison; `scripts/check_commands.py` does the comparing.

Usage:
  python3 scripts/capture_commands.py vanilla [out.json]
  python3 scripts/capture_commands.py ov [out.json]
"""
from __future__ import annotations

import json
import os
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
from capture_entity_packets import Probe, read_varint, varint  # noqa: E402
from measure_entities import Server  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PRESET = os.environ.get("OV_PRESET", "macos-debug")
BINARY = ROOT / "build" / PRESET / "bin" / "ov_dedicated"
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"

# ── Packets ──────────────────────────────────────────────────────────────────
SB_CHAT_COMMAND = 0x04
SB_CHAT_MESSAGE = 0x05
SB_SUGGESTIONS = 0x09

CB_SUGGESTIONS = 0x0F
CB_COMMANDS = 0x10
CB_DISGUISED_CHAT = 0x1B
CB_PLAYER_CHAT = 0x35
CB_SYSTEM_CHAT = 0x64
CB_DISCONNECT = 0x1A

# Traffic that says nothing about a command and would drown what does.
NOISE = {0x23, 0x24, 0x27, 0x1E, 0x4E, 0x2B, 0x2C, 0x2D, 0x42, 0x54, 0x68, 0x03,
         0x52, 0x6A}

# The commands, in order. Order matters: several read state an earlier one set.
# The first block puts the probe somewhere both servers agree on, so that any
# coordinate in an answer is a coordinate both of them can have.
COMMANDS: list[str] = [
    "tp @s 0.5 -60 0.5",
    "gamemode creative",
    # ── time ──
    "time set 1000", "time set day", "time set noon", "time set night", "time set midnight",
    "time set 0", "time add 100", "time add 1d", "time add 3s", "time add 20t",
    "time query daytime", "time query day",
    "time set -1", "time set 1.5", "time set 1x", "time sett day", "time set", "time",
    "time set day extra", "time add -5", "time query", "time query foo",
    # ── weather ──
    "weather clear", "weather rain", "weather thunder", "weather clear 100", "weather rain 1d",
    "weather thunder 10s", "weather foo", "weather clear 0", "weather clear 1000001",
    "weather clear",
    # ── difficulty ──
    "difficulty", "difficulty hard", "difficulty hard", "difficulty peaceful", "difficulty easy",
    "difficulty normal", "difficulty bogus",
    # ── gamerule ──
    "gamerule doDaylightCycle", "gamerule randomTickSpeed", "gamerule randomTickSpeed 10",
    "gamerule randomTickSpeed", "gamerule randomTickSpeed 3", "gamerule keepInventory true",
    "gamerule keepInventory", "gamerule keepInventory false", "gamerule nosuchrule",
    "gamerule randomTickSpeed abc", "gamerule keepInventory yes", "gamerule spawnRadius",
    "gamerule maxEntityCramming", "gamerule doWeatherCycle",
    # ── misc queries ──
    "seed", "list", "list uuids", "list foo",
    # ── chat-like ──
    "say hello there", "me waves", "msg ovprobe hi", "tell ovprobe yo", "w nobody hi",
    "msg @s test", "say", "msg ovprobe",
    # ── tellraw / title ──
    'tellraw @s "plain"', 'tellraw @s {"text":"hi","color":"red"}',
    'tellraw @s ["a",{"text":"b","bold":true}]', "tellraw @s {bad json",
    'tellraw @s {"translate":"commands.time.set","with":["5"]}', "tellraw @s 12",
    'tellraw @s {"text":"x","color":"nocolor"}',
    'title @s title {"text":"T"}', 'title @s subtitle "S"', 'title @s actionbar "A"',
    "title @s times 10 70 20", "title @s clear", "title @s reset", "title @s foo",
    # ── setblock ──
    "setblock 0 -60 0 stone", "setblock 0 -60 0 stone", "setblock 0 -60 0 dirt keep",
    "setblock 0 -60 0 minecraft:oak_stairs[facing=north]", "setblock 0 -60 0 oak_stairs[facing=up]",
    "setblock 0 -60 0 oak_stairs[foo=bar]", "setblock 0 -60 0 oak_stairs[facing=north",
    "setblock 0 -60 0 nosuchblock", "setblock 0 -60 0 air destroy", "setblock 0 400 0 stone",
    "setblock 1.5 -60 0 stone", "setblock ~ ~ ~ glass", "setblock ~1 ~ ~ glass replace",
    "setblock 0 -60 0 stone foo",
    # ── fill ──
    "fill 0 -60 0 3 -58 3 stone", "fill 0 -60 0 3 -58 3 stone", "fill 0 -60 0 3 -58 3 glass hollow",
    "fill 0 -60 0 3 -58 3 air replace glass", "fill 0 -60 0 3 -58 3 glass outline",
    "fill 0 -60 0 3 -58 3 dirt keep", "fill 0 -60 0 3 -58 3 air destroy",
    "fill 0 -60 0 200 -50 200 stone", "fill 0 -60 0 31 -29 31 air",
    "fill 0 -60 0 3 -58 3 stone replace nosuchblock", "fill 0 -60 0 3 -58 3 air",
    # ── give / clear ──
    "clear @s", "give @s diamond 5", "give @s diamond_sword", "give @s minecraft:stone 100",
    "give @s nosuchitem", "give @s diamond 0", "give @s diamond 6401", "give nobody diamond",
    "give @s stone{display:{Name:'\"x\"'}} 1", "clear @s diamond 2", "clear @s diamond 0",
    "clear @s diamond", "clear @s", "clear @s", "clear @s nosuchitem",
    # ── summon / kill ──
    "kill @e[type=cow]", "summon cow 5 -60 5", "summon minecraft:zombie 6 -60 5",
    "summon nosuch 5 -60 5", "summon cow 5 400 5", "kill @e[type=cow]",
    "kill @e[type=!player,distance=..30]", "kill @e[type=zombie]",
    "summon cow 5 -60 5", "summon cow 8 -60 5", "kill @e[type=cow,limit=1,sort=nearest]",
    "kill @e[type=cow,limit=1,sort=furthest]", "kill @q", "kill @e[foo=1]", "kill @e[limit=0]",
    "kill @e[type=nosuchtype]", "kill @e[distance=-1]", "kill @e[type=cow", "kill @e[type=cow]",
    "kill nobody", "kill @e[x=0,y=-60,z=0,dx=1,dy=1,dz=1]", "kill @e[gamemode=creative]",
    "kill @e[name=Bob]", "kill @e[sort=bogus]", "kill @r[type=cow]", "kill @a[type=cow]",
    # ── tp ──
    "tp @s 0.5 -60 0.5", "tp 10 -60 10", "tp @s ~ ~5 ~", "tp @s 0 -60 0 90 0", "tp @s ^ ^ ^1",
    "tp ovprobe ovprobe", "tp @s 0 -60 0 facing 10 -60 0", "teleport 0.5 -60 0.5", "tp @s ~ ~",
    "tp @s ^ ~ ~", "tp nobody 0 0 0", "tp @s 0 -60 0 foo", "tp @s 30000000 0 0",
    "tp @s 0.5 -60 0.5",
    # ── gamemode / defaultgamemode ──
    "gamemode survival", "gamemode survival ovprobe", "gamemode adventure @s",
    "gamemode spectator @p", "gamemode foo", "gamemode creative nobody", "gamemode creative @e",
    "gamemode creative", "defaultgamemode survival", "defaultgamemode creative",
    "defaultgamemode foo",
    # ── experience ──
    "xp add @s 10", "xp add @s 3 levels", "xp set @s 5 levels", "xp set @s 10 points",
    "xp query @s points", "xp query @s levels", "experience add @s 1", "xp set @s -1",
    "xp set @s 1000 points", "xp add nobody 1", "xp query @s", "xp set @s 0 levels",
    # ── spawn points ──
    "spawnpoint", "spawnpoint @s 1 -60 1", "spawnpoint @s 1 -60 1 90", "setworldspawn",
    "setworldspawn 0 -60 0", "setworldspawn 0 -60 0 45", "setworldspawn 0 -60",
    # ── help ──
    "help time", "help gamemode", "help tp", "help nosuch", "help give", "help weather",
    # ── op ──
    "op ovprobe", "deop nobody", "op nobody",
    # ── unknown ──
    "nosuchcommand", "", "time set day ", "  time set day", "gamemode  creative",
    "save-all", "help",
    # ── every gamerule, read back: the catalogue and its defaults ──
    "gamerule announceAdvancements",
    "gamerule blockExplosionDropDecay",
    "gamerule commandBlockOutput",
    "gamerule commandModificationBlockLimit",
    "gamerule disableElytraMovementCheck",
    "gamerule disableRaids",
    "gamerule doDaylightCycle",
    "gamerule doEntityDrops",
    "gamerule doFireTick",
    "gamerule doImmediateRespawn",
    "gamerule doInsomnia",
    "gamerule doLimitedCrafting",
    "gamerule doMobLoot",
    "gamerule doMobSpawning",
    "gamerule doPatrolSpawning",
    "gamerule doTileDrops",
    "gamerule doTraderSpawning",
    "gamerule doVinesSpread",
    "gamerule doWardenSpawning",
    "gamerule doWeatherCycle",
    "gamerule drowningDamage",
    "gamerule fallDamage",
    "gamerule fireDamage",
    "gamerule forgiveDeadPlayers",
    "gamerule freezeDamage",
    "gamerule globalSoundEvents",
    "gamerule keepInventory",
    "gamerule lavaSourceConversion",
    "gamerule logAdminCommands",
    "gamerule maxCommandChainLength",
    "gamerule maxEntityCramming",
    "gamerule mobExplosionDropDecay",
    "gamerule mobGriefing",
    "gamerule naturalRegeneration",
    "gamerule playersSleepingPercentage",
    "gamerule randomTickSpeed",
    "gamerule reducedDebugInfo",
    "gamerule sendCommandFeedback",
    "gamerule showDeathMessages",
    "gamerule snowAccumulationHeight",
    "gamerule spawnRadius",
    "gamerule spectatorsGenerateChunks",
    "gamerule tntExplosionDropDecay",
    "gamerule universalAnger",
    "gamerule waterSourceConversion",
]

# Suggestion requests, as the client sends them: everything up to the cursor.
SUGGESTIONS: list[str] = [
    "/tim", "/time ", "/time s", "/time set ", "/gamemode ", "/gamemode c", "/give @",
    "/give @s dia", "/tp ovp", "/weather ", "/difficulty ", "/gamerule do", "/kill @e[",
    "/kill @e[type=", "/setblock ", "/setblock ~ ~ ~ oak_st", "/setblock 0 0 0 oak_stairs[",
    "/", "/xp ", "/msg ", "tim", "/summon co",
]

CONSOLE: list[str] = [
    "time set 1000", "list", "seed", "difficulty", "gamerule doDaylightCycle", "time sett",
    "help time", "kill @e[type=cow]", "gamemode creative ovprobe", "tp ovprobe 0.5 -60 0.5",
    "weather clear", "say from the console", "time query daytime", "xp query ovprobe points",
    "setblock 0 -60 0 stone", "nosuchcommand", "gamemode creative",
]

CHAT: list[str] = ["hello world", "a second line"]


def read_string(buf: bytes, i: int) -> tuple[str, int]:
    length, i = read_varint(buf, i)
    return buf[i:i + length].decode("utf-8"), i + length


def decode(pid: int, payload: bytes) -> dict | None:
    """The chat-shaped packets, decoded field by field from the protocol page."""
    try:
        if pid == CB_SYSTEM_CHAT:
            text, i = read_string(payload, 0)
            return {"content": text, "overlay": bool(payload[i])}
        if pid == CB_DISGUISED_CHAT:
            text, i = read_string(payload, 0)
            chat_type, i = read_varint(payload, i)
            name, i = read_string(payload, i)
            has_target = payload[i]
            i += 1
            target = None
            if has_target:
                target, i = read_string(payload, i)
            return {"message": text, "chat_type": chat_type, "name": name, "target": target}
        if pid == CB_PLAYER_CHAT:
            sender = payload[0:16].hex()
            i = 16
            index, i = read_varint(payload, i)
            signed = payload[i]
            i += 1
            if signed:
                i += 256
            body, i = read_string(payload, i)
            timestamp, salt = struct.unpack_from(">qq", payload, i)
            i += 16
            previous, i = read_varint(payload, i)
            for _ in range(previous):
                message_id, i = read_varint(payload, i)
                if message_id == 0:
                    i += 256
            unsigned_present = payload[i]
            i += 1
            unsigned = None
            if unsigned_present:
                unsigned, i = read_string(payload, i)
            filter_type, i = read_varint(payload, i)
            if filter_type == 2:
                longs, i = read_varint(payload, i)
                i += 8 * longs
            chat_type, i = read_varint(payload, i)
            name, i = read_string(payload, i)
            has_target = payload[i]
            i += 1
            target = None
            if has_target:
                target, i = read_string(payload, i)
            return {"sender": sender, "index": index, "signed": bool(signed), "body": body,
                    "salt": salt, "previous": previous, "unsigned": unsigned,
                    "filter": filter_type, "chat_type": chat_type, "name": name,
                    "target": target, "trailing": len(payload) - i}
        if pid == CB_SUGGESTIONS:
            transaction, i = read_varint(payload, 0)
            start, i = read_varint(payload, i)
            length, i = read_varint(payload, i)
            count, i = read_varint(payload, i)
            matches = []
            for _ in range(count):
                match, i = read_string(payload, i)
                has_tooltip = payload[i]
                i += 1
                tooltip = None
                if has_tooltip:
                    tooltip, i = read_string(payload, i)
                matches.append([match, tooltip])
            return {"id": transaction, "start": start, "length": length, "matches": matches}
        if pid == CB_DISCONNECT:
            text, _ = read_string(payload, 0)
            return {"reason": text}
        if pid == 0x1F:
            return {"event": payload[0], "value": struct.unpack_from(">f", payload, 1)[0]}
        if pid in (0x5F, 0x5D, 0x46):
            text, _ = read_string(payload, 0)
            return {"text": text}
        if pid == 0x60:
            return {"times": list(struct.unpack_from(">iii", payload, 0))}
        if pid == 0x0E:
            return {"reset": bool(payload[0])}
        if pid == 0x0C:
            return {"difficulty": payload[0], "locked": bool(payload[1])}
        if pid == 0x1C:
            return {"entity": struct.unpack_from(">i", payload, 0)[0], "status": payload[4]}
    except (IndexError, struct.error, UnicodeDecodeError) as error:
        return {"decode_error": str(error)}
    return None


class CommandProbe(Probe):
    def __init__(self, port: int, name: str = "ovprobe") -> None:
        super().__init__(port, name)
        self.clock_ms = int(time.time() * 1000)

    def _timestamp(self) -> int:
        # Strictly increasing: a server is entitled to refuse a message that is
        # older than the one before it.
        self.clock_ms = max(self.clock_ms + 1, int(time.time() * 1000))
        return self.clock_ms

    def command(self, text: str) -> None:
        body = text.encode("utf-8")
        self.send(SB_CHAT_COMMAND,
                  varint(len(body)) + body + struct.pack(">qq", self._timestamp(), 0)
                  + varint(0)          # no argument signatures
                  + varint(0)          # message count
                  + bytes(3))          # acknowledged: a fixed bitset of 20 bits

    def chat(self, text: str) -> None:
        body = text.encode("utf-8")
        self.send(SB_CHAT_MESSAGE,
                  varint(len(body)) + body + struct.pack(">qq", self._timestamp(), 0)
                  + bytes([0])         # no signature
                  + varint(0) + bytes(3))

    def suggest(self, transaction: int, text: str) -> None:
        body = text.encode("utf-8")
        self.send(SB_SUGGESTIONS, varint(transaction) + varint(len(body)) + body)

    def take(self) -> list[dict]:
        out = []
        for pid, payload in self.drain():
            if pid in NOISE:
                continue
            entry = {"id": pid, "hex": payload.hex() if len(payload) < 65536 else None,
                     "size": len(payload)}
            decoded = decode(pid, payload)
            if decoded is not None:
                entry["decoded"] = decoded
            out.append(entry)
        return out


class OvServer:
    """Our dedicated server, driven through its console like the jar is."""

    def __init__(self, directory: Path, port: int) -> None:
        if directory.exists():
            shutil.rmtree(directory)
        directory.mkdir(parents=True)
        self.directory = directory
        self.process = subprocess.Popen(
            [str(BINARY), f"--world={directory / 'world'}", f"--port={port}",
             "--log-level=info"],
            cwd=directory, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.lines: queue.Queue[str] = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        self._token = 0
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    return
            except OSError:
                if self.process.poll() is not None:
                    raise RuntimeError("ov_dedicated exited before listening")
                time.sleep(0.1)
        raise RuntimeError("ov_dedicated never listened")

    def _pump(self) -> None:
        assert self.process.stdout is not None
        for raw in self.process.stdout:
            self.lines.put(raw.rstrip("\n"))

    def batch(self, commands: list[str], timeout: float = 30.0) -> list[str]:
        self._token += 1
        marker = f"ovsync{self._token}"
        assert self.process.stdin is not None
        self.process.stdin.write("".join(c + "\n" for c in commands) + f"say {marker}\n")
        self.process.stdin.flush()
        seen: list[str] = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if f"[Server] {marker}" in line:
                return seen
            seen.append(line)
        raise TimeoutError(f"never saw {marker}; last lines {seen[-6:]}")

    def stop(self) -> None:
        try:
            assert self.process.stdin is not None
            self.process.stdin.write("stop\n")
            self.process.stdin.flush()
            self.process.wait(timeout=60)
        except Exception:
            self.process.kill()
        shutil.rmtree(self.directory, ignore_errors=True)


class FlatVanilla(Server):
    # No structures: a village two chunks away would put villagers in every
    # `@e` answer and nothing in ours.
    EXTRA_PROPERTIES = "generate-structures=false\n"


def is_sync(entry: dict) -> bool:
    decoded = entry.get("decoded") or {}
    text = json.dumps(decoded)
    return "ovsync" in text


def main() -> int:
    target = sys.argv[1] if len(sys.argv) > 1 else "vanilla"
    out_path = (Path(sys.argv[2]) if len(sys.argv) > 2
                else NORMALIZED / f"commands_capture_{target}.json")
    port = 25691 if target == "vanilla" else 25692
    run_dir = ROOT / "run" / f"commands-{target}"
    if target == "vanilla":
        if run_dir.exists():
            shutil.rmtree(run_dir)
        server = FlatVanilla(run_dir, port=port)
    else:
        server = OvServer(run_dir, port)

    document: dict = {"target": target, "login": [], "op": [], "suggestions": [],
                      "commands": [], "chat": [], "console": []}
    try:
        server.batch(["gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                      "gamerule doWeatherCycle false", "time set 1000", "weather clear",
                      "difficulty easy"])
        probe = CommandProbe(port)
        probe.pump(4.0)
        document["login"] = [{"id": pid, "size": len(p),
                              "hex": p.hex() if pid in (CB_COMMANDS, 0x45, 0x0C, 0x1F, 0x50,
                                                        0x34, 0x3A, 0x1C, 0x64) else None}
                             for pid, p in probe.drain()
                             if pid not in (0x24, 0x27, 0x23)]
        print(f"login: {len(document['login'])} packets")

        server.batch(["op ovprobe"])
        probe.pump(1.5)
        document["op"] = [e for e in probe.take() if not is_sync(e)]
        print(f"op: {len(document['op'])} packets")

        for transaction, text in enumerate(SUGGESTIONS, start=1):
            probe.suggest(transaction, text)
            probe.pump(0.4)
            document["suggestions"].append({"text": text, "packets": probe.take()})

        for text in COMMANDS:
            probe.command(text)
            probe.pump(0.7)
            packets = [e for e in probe.take() if not is_sync(e)]
            document["commands"].append({"command": text, "packets": packets})
            chat = [e["decoded"] for e in packets if e["id"] == CB_SYSTEM_CHAT]
            print(f"  /{text}: {len(packets)} packets, {len(chat)} system messages")

        for text in CHAT:
            probe.chat(text)
            probe.pump(0.6)
            document["chat"].append({"message": text, "packets": probe.take()})

        for text in CONSOLE:
            lines = server.batch([text])
            probe.pump(0.3)
            packets = [e for e in probe.take() if not is_sync(e)]
            document["console"].append({"command": text, "lines": lines, "packets": packets})

        # Kicked last, because nothing can follow it.
        probe.command("kick @s go away")
        try:
            probe.pump(1.0)
        except (EOFError, ConnectionError):
            pass  # the server closed the socket, which is what a kick is
        document["kick"] = probe.take()

        out_path.parent.mkdir(parents=True, exist_ok=True)
        with open(out_path, "w") as handle:
            json.dump(document, handle, indent=1, sort_keys=True)
        print(f"wrote {out_path}")
    finally:
        server.stop()
        if target == "vanilla":
            shutil.rmtree(run_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
