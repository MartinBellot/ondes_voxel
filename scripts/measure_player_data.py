#!/usr/bin/env python3
"""Ask a real 1.20.1 server what a player file is, and whether ours is one.

Campaigns
---------

  capture      A bot joins a fresh flat world. The console gives it an
               inventory (hotbar, backpack, two armour pieces, the off hand, an
               item with a tag), an ender chest item, 30 levels and 17 points,
               five points of damage, a drained food bar, three effects (one
               hidden, one infinite) and a spawn point; the bot picks hotbar
               slot 2. It disconnects and the file the server writes is kept as
               V1. `save-all` is run once while it is connected so that the
               `.dat_old` rule is observed, not assumed.

  control      V1 is put back and read by vanilla: the bot joins, leaves, and
               vanilla writes V1'. diff(V1, V1') is what a vanilla load+save
               cycle changes on its own — the baseline every other diff is read
               against.

  roundtrip    V1 is read by us and written back as O1 (tools/ov_playerdata).
               diff(V1, O1) must be empty. Then O1 is read by vanilla the same
               way and written as V2: diff(O1, V2) must change the same keys
               the control changed, and nothing else.

  ours         A file written by our own server (argument --ours=PATH, for the
               bot name in --ours-name) is read by vanilla: `data get entity`
               of the position, the inventory and the effects.

  level        A level.dat carrying Data.Player is opened by the dedicated
               server and saved. Does the dedicated server keep the tag?

Usage: python3 scripts/measure_player_data.py [--only a,b] [--ours=F --ours-name=N]
Writes .scratch/playerdata/report.json.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import time
import uuid as uuidlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import measure_survival as ms  # noqa: E402
from measure_entities import Server  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / "run" / "playerdata-oracle"
OUT = ROOT / ".scratch" / "playerdata"
TOOL = ROOT / "build" / "macos-debug" / "bin" / "ov_playerdata"
PORT = 25631
BOT = "ovplayer"


class SmallServer(Server):
    # A small view: nothing here needs terrain, and the disk is shared.
    EXTRA_PROPERTIES = "view-distance=3\nsimulation-distance=3\n"
    HEAP = "-Xmx1G"


def offline_uuid(name: str) -> str:
    digest = bytearray(hashlib.md5(f"OfflinePlayer:{name}".encode()).digest())
    digest[6] = (digest[6] & 0x0F) | 0x30
    digest[8] = (digest[8] & 0x3F) | 0x80
    return str(uuidlib.UUID(bytes=bytes(digest)))


# ── Typed NBT ───────────────────────────────────────────────────────────────

NAMES = {1: "byte", 2: "short", 3: "int", 4: "long", 5: "float", 6: "double",
         7: "byte_array", 8: "string", 9: "list", 10: "compound", 11: "int_array",
         12: "long_array"}


def read_nbt(data: bytes):
    """Every value as (type, value); lists as (list, (element type, [values]))."""
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    pos = 0

    def take(n: int) -> bytes:
        nonlocal pos
        out = data[pos:pos + n]
        pos += n
        return out

    def string() -> str:
        (length,) = struct.unpack(">H", take(2))
        return take(length).decode("utf-8", "replace")

    def payload(kind: int):
        if kind == 1:
            return struct.unpack(">b", take(1))[0]
        if kind == 2:
            return struct.unpack(">h", take(2))[0]
        if kind == 3:
            return struct.unpack(">i", take(4))[0]
        if kind == 4:
            return struct.unpack(">q", take(8))[0]
        if kind == 5:
            return struct.unpack(">f", take(4))[0]
        if kind == 6:
            return struct.unpack(">d", take(8))[0]
        if kind == 7:
            (n,) = struct.unpack(">i", take(4))
            return list(take(n))
        if kind == 8:
            return string()
        if kind == 9:
            element = take(1)[0]
            (n,) = struct.unpack(">i", take(4))
            return (NAMES.get(element, "end"), [payload(element) for _ in range(n)])
        if kind == 10:
            out = {}
            while True:
                child = take(1)[0]
                if child == 0:
                    return out
                name = string()
                out[name] = (NAMES[child], payload(child))
        if kind in (11, 12):
            (n,) = struct.unpack(">i", take(4))
            fmt = ">i" if kind == 11 else ">q"
            size = 4 if kind == 11 else 8
            return [struct.unpack(fmt, take(size))[0] for _ in range(n)]
        raise ValueError(kind)

    root = take(1)[0]
    string()
    return ("compound", payload(root))


def flatten(tag, path: str = "") -> dict[str, tuple[str, object]]:
    """Every leaf, by path, with its type. A list of compounds is keyed by
    index — and an inventory list by its Slot, so a reordering is a reorder
    and not a hundred differences."""
    kind, value = tag
    out: dict[str, tuple[str, object]] = {}
    if kind == "compound":
        if not value:
            out[path or "/"] = ("compound", "{}")
        for name, child in value.items():
            out.update(flatten(child, f"{path}/{name}"))
    elif kind == "list":
        element, items = value
        if not items:
            out[path] = (f"list<{element}>", "[]")
        for i, item in enumerate(items):
            key = str(i)
            if element == "compound" and "Slot" in item:
                key = f"slot{item['Slot'][1]}"
            elif element == "compound" and "Name" in item:
                key = item["Name"][1]
            elif element == "compound" and "Id" in item:
                key = f"id{item['Id'][1]}"
            out.update(flatten((element, item), f"{path}[{key}]"))
    else:
        out[path] = (kind, value)
    return out


def diff(a: bytes, b: bytes) -> dict:
    fa, fb = flatten(read_nbt(a)), flatten(read_nbt(b))
    changed = sorted(k for k in fa.keys() & fb.keys() if fa[k] != fb[k])
    return {
        "leaves_a": len(fa), "leaves_b": len(fb),
        "equal": sum(1 for k in fa.keys() & fb.keys() if fa[k] == fb[k]),
        "changed": {k: [fa[k], fb[k]] for k in changed},
        "only_a": {k: fa[k] for k in sorted(fa.keys() - fb.keys())},
        "only_b": {k: fb[k] for k in sorted(fb.keys() - fa.keys())},
    }


def summary(d: dict) -> str:
    return (f"{d['equal']} equal, {len(d['changed'])} changed, "
            f"{len(d['only_a'])} only before, {len(d['only_b'])} only after")


# ── Driving the server ──────────────────────────────────────────────────────


def player_file(name: str = BOT) -> Path:
    return RUN / "world" / "playerdata" / f"{offline_uuid(name)}.dat"


def start() -> SmallServer:
    server = SmallServer(RUN, port=PORT)
    server.batch(["gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                  "gamerule doWeatherCycle false", "difficulty normal"])
    return server


def leave(server: SmallServer, bot: ms.Bot, name: str = BOT) -> None:
    bot.socket.close()
    server._await(f"{name} left the game", timeout=30.0)
    server.batch(["list"])


def data_get(server: SmallServer, path: str, name: str = BOT) -> str | None:
    return ms.scalar(server.batch([f"data get entity {name} {path}"]))


def reload(source: bytes, name: str = BOT, probe: tuple[str, ...] = ()) -> tuple[bytes, dict]:
    """Vanilla reads `source` as this player's file, the player joins and
    leaves, and vanilla writes it back. Returns what it wrote, and the console's
    answers for the paths in `probe` read while the player was online."""
    target = player_file(name)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(source)
    old = target.with_name(target.name + "_old")
    if old.exists():
        old.unlink()
    server = start()
    answers: dict = {}
    try:
        bot = ms.Bot(PORT, name=name)
        bot.pump(2.0)
        # Clientbound Set Held Item: which id carries the saved slot at join.
        answers["set_held_item_0x4D"] = [p.hex() for pid, p in bot.captured if pid == 0x4D]
        # Entity Effect at the join: the file's durations, or one tick less?
        answers["entity_effect_0x6C"] = [p.hex() for pid, p in bot.captured if pid == 0x6C]
        for path in probe:
            answers[path] = data_get(server, path, name)
        leave(server, bot, name)
    finally:
        server.stop()
    return target.read_bytes(), answers


# ── Campaigns ───────────────────────────────────────────────────────────────


def campaign_capture(report: dict) -> None:
    if RUN.exists():
        shutil.rmtree(RUN)
    server = start()
    try:
        bot = ms.Bot(PORT, name=BOT)
        bot.pump(2.0)
        server.batch([
            f"gamemode survival {BOT}",
            f"tp {BOT} 12.5 -60 -7.25 90 10",
            f"give {BOT} minecraft:cobblestone 64",
            f"give {BOT} minecraft:diamond_sword{{Damage:5,display:{{Name:'\"Blade\"'}}}} 1",
            f"give {BOT} minecraft:oak_log 20",
            f"item replace entity {BOT} inventory.20 with minecraft:bread 7",
            f"item replace entity {BOT} armor.head with minecraft:iron_helmet",
            f"item replace entity {BOT} armor.feet with "
            "minecraft:leather_boots{display:{color:16711680}}",
            f"item replace entity {BOT} weapon.offhand with minecraft:shield",
            f"item replace entity {BOT} enderchest.3 with minecraft:ender_pearl 16",
            f"xp add {BOT} 30 levels",
            f"xp add {BOT} 17 points",
            f"damage {BOT} 5 minecraft:generic",
            f"effect give {BOT} minecraft:hunger 2 255 true",
        ])
        bot.pump(3.0)
        server.batch([
            f"effect give {BOT} minecraft:speed 300 1",
            f"effect give {BOT} minecraft:speed 30 3",
            f"effect give {BOT} minecraft:night_vision infinite 0 true",
            f"spawnpoint {BOT} 3 -60 4 45",
        ])
        bot.hold(2)
        bot.pump(1.0)
        # A save while connected, then the disconnect's own: two writes, so
        # the second leaves the first behind as .dat_old if that is the rule.
        server.batch(["save-all flush"])
        bot.pump(1.0)
        first = player_file().read_bytes() if player_file().exists() else None
        report["console_before_leave"] = ms.scalar(server.batch([f"data get entity {BOT}"]))
        leave(server, bot)
    finally:
        server.stop()
    v1 = player_file().read_bytes()
    old = player_file().with_name(player_file().name + "_old")
    report["capture"] = {
        "file": player_file().name,
        "bytes": len(v1),
        "gzip": v1[:2] == b"\x1f\x8b",
        "dat_old_exists": old.exists(),
        "dat_old_equals_first_save": old.exists() and first is not None
        and old.read_bytes() == first,
        "top_level": {k: v[0] for k, v in read_nbt(v1)[1].items()},
        "leaves": {k: [t, v] for k, (t, v) in flatten(read_nbt(v1)).items()},
    }
    (OUT / "V1.dat").write_bytes(v1)
    print(f"capture: {len(v1)} bytes, {len(report['capture']['leaves'])} leaves, "
          f"dat_old {report['capture']['dat_old_exists']}, "
          f"old == first save {report['capture']['dat_old_equals_first_save']}")


PROBE = ("Pos", "Rotation", "Health", "foodLevel", "XpLevel", "XpP", "SelectedItemSlot",
         "Inventory", "EnderItems", "ActiveEffects", "playerGameType")


def campaign_control(report: dict) -> None:
    v1 = (OUT / "V1.dat").read_bytes()
    v1b, answers = reload(v1, probe=PROBE)
    (OUT / "V1b.dat").write_bytes(v1b)
    d = diff(v1, v1b)
    report["control"] = {"diff": d, "console": answers}
    print(f"control: vanilla load+save of its own file: {summary(d)}")


def run_tool(*args: str) -> str:
    result = subprocess.run([str(TOOL), *args], capture_output=True, text=True, cwd=ROOT)
    if result.returncode != 0:
        raise RuntimeError(f"{TOOL.name} {' '.join(args)}: {result.stdout}{result.stderr}")
    return result.stdout


def campaign_roundtrip(report: dict) -> None:
    v1_path = OUT / "V1.dat"
    o1_path = OUT / "O1.dat"
    dump = run_tool("roundtrip", str(v1_path), str(o1_path))
    v1, o1 = v1_path.read_bytes(), o1_path.read_bytes()
    ours = diff(v1, o1)
    v2, answers = reload(o1, probe=PROBE)
    (OUT / "V2.dat").write_bytes(v2)
    theirs = diff(o1, v2)
    control = report.get("control", {}).get("diff")
    same_keys = None
    if control is not None:
        keys = lambda d: set(d["changed"]) | set(d["only_a"]) | set(d["only_b"])  # noqa: E731
        same_keys = {"control_only": sorted(keys(control) - keys(theirs)),
                     "ours_only": sorted(keys(theirs) - keys(control))}
    report["roundtrip"] = {"tool": dump, "v1_vs_o1": ours, "o1_vs_v2": theirs,
                           "console": answers, "against_control": same_keys}
    print(f"roundtrip: V1 -> ours: {summary(ours)}")
    print(f"roundtrip: ours -> vanilla: {summary(theirs)}")
    if same_keys is not None:
        print(f"  keys vanilla changed on our file and not on its own: {same_keys['ours_only']}")
        print(f"  keys vanilla changed on its own file and not on ours: "
              f"{same_keys['control_only']}")


def campaign_ours(report: dict, path: Path, name: str) -> None:
    source = path.read_bytes()
    written, answers = reload(source, name=name, probe=PROBE)
    (OUT / "ours_by_vanilla.dat").write_bytes(written)
    d = diff(source, written)
    report["ours"] = {"source": str(path), "console": answers, "diff": d}
    print(f"ours: vanilla read our file for {name}: {summary(d)}")
    for key, value in answers.items():
        print(f"  {key}: {str(value)[:200]}")


def campaign_level(report: dict) -> None:
    level = RUN / "world" / "level.dat"
    run_tool("level-inject", str(level), str(OUT / "V1.dat"))
    before = "Player" in read_nbt(level.read_bytes())[1]["Data"][1]
    server = start()
    try:
        server.batch(["save-all flush"])
    finally:
        server.stop()
    after = "Player" in read_nbt(level.read_bytes())[1]["Data"][1]
    report["level"] = {"injected": before, "kept_by_dedicated_server": after}
    print(f"level: Data.Player injected {before}, kept after a dedicated save {after}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", default="capture,control,roundtrip,level")
    parser.add_argument("--ours")
    parser.add_argument("--ours-name", default="ovfaller")
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    report_path = OUT / "report.json"
    report = json.loads(report_path.read_text()) if report_path.exists() else {}
    wanted = args.only.split(",")
    campaigns = {"capture": campaign_capture, "control": campaign_control,
                 "roundtrip": campaign_roundtrip, "level": campaign_level}
    for name in wanted:
        if name == "ours":
            campaign_ours(report, Path(args.ours), args.ours_name)
        else:
            campaigns[name](report)
        report_path.write_text(json.dumps(report, indent=1, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
