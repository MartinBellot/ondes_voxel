#!/usr/bin/env python3
"""Compare what our server answered to what the vanilla jar answered, for the
scoreboard, the teams, /trigger and /teammsg — and compare two scoreboard.dat.

Both captures come from scripts/capture_scoreboard.py:

    .scratch/scoreboard/capture_vanilla.json
    .scratch/scoreboard/capture_ov.json

What is compared, command by command and in order:

  * System Chat (its JSON, byte for byte);
  * Player Chat (body, chat type, sender name and target, as JSON);
  * the four scoreboard packets (Display Objective, Update Objectives,
    Update Teams, Update Score), byte for byte;
  * for the console, the line each command printed, and the operators' log
    lines ("[ovprobe: …]") of the whole run.

One normalisation, named: `scoreboard players list <holder>` with several
scores. Vanilla walks an identity-hashed map there, so its order changes from
one run of the jar to the next; those lines are compared as a set and counted
apart ("same lines, another order").

    python3 scripts/check_scoreboard.py [vanilla.json ov.json]
    python3 scripts/check_scoreboard.py dat A.dat B.dat

Exit status: the number of differing replies (or leaves).
"""
from __future__ import annotations

import gzip
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / ".scratch" / "scoreboard"
SCOREBOARD_IDS = {0x51, 0x58, 0x5A, 0x5B}
# Damage, health, death: what an attack is answered with.
COMBAT_IDS = {0x18, 0x21, 0x57, 0x38}


def reply(packets: list[dict], combat: bool = False) -> list[str]:
    out = []
    for p in packets:
        pid = p["id"]
        d = p.get("decoded") or {}
        if pid == 0x64:
            if "ovsync" not in d.get("content", ""):
                # The one normalisation of check_commands.py that applies here:
                # /list's "max players" is the rig's configuration.
                content = re.sub(r'(commands\.list\.players","with":\["\d+",)"\d+"', r'\1"<max>"',
                                 d.get("content", ""))
                out.append("chat " + content)
        elif pid == 0x35:
            out.append("player_chat " + json.dumps(
                {k: d.get(k) for k in ("body", "chat_type", "name", "target")}, sort_keys=True))
        elif pid in SCOREBOARD_IDS:
            out.append(f"0x{pid:02x} {p.get('hex')}")
        elif combat and pid in COMBAT_IDS:
            out.append(f"0x{pid:02x}")
    return out


def unordered_listing(command: str, lines: list[str]) -> bool:
    return command.startswith("scoreboard players list ") and len(lines) > 2


class Tally:
    def __init__(self, name: str) -> None:
        self.name = name
        self.same = 0
        self.reordered = 0
        self.total = 0
        self.diffs: list[tuple[str, list[str], list[str]]] = []

    def add(self, label: str, vanilla: list[str], ours: list[str], unordered: bool = False) -> None:
        self.total += 1
        if vanilla == ours:
            self.same += 1
        elif unordered and sorted(vanilla) == sorted(ours):
            self.reordered += 1
        else:
            self.diffs.append((label, vanilla, ours))

    def report(self) -> int:
        extra = f" (+{self.reordered} same lines, another order)" if self.reordered else ""
        print(f"{self.name}: {self.same}/{self.total} identical{extra}")
        for label, vanilla, ours in self.diffs:
            print(f"  ✗ {label}")
            for line in vanilla:
                print(f"      vanilla {line[:300]}")
            for line in ours:
                print(f"      ours    {line[:300]}")
        return len(self.diffs)


def compare(vanilla_path: Path, ours_path: Path) -> int:
    vanilla = json.loads(vanilla_path.read_text())
    ours = json.loads(ours_path.read_text())
    failures = 0

    one = Tally("phase one (ovprobe, operator)")
    for v, o in zip(vanilla["phase_one"], ours["phase_one"]):
        command = v["command"]
        vr, orr = reply(v["packets"]), reply(o["packets"])
        one.add("/" + command, vr, orr, unordered_listing(command, vr))
    for v, o in zip(vanilla["chat_one"], ours["chat_one"]):
        one.add("chat " + v["message"], reply(v["packets"]), reply(o["packets"]))
    failures += one.report()

    arrival = Tally("arrival (what ovother is sent)")
    arrival.add("scoreboard packets",
                [f"0x{p['id']:02x} {p['hex']}" for p in vanilla["arrival"] if p["id"] in SCOREBOARD_IDS],
                [f"0x{p['id']:02x} {p['hex']}" for p in ours["arrival"] if p["id"] in SCOREBOARD_IDS])
    failures += arrival.report()

    two = Tally("phase two (both probes, seen by each)")
    for v, o in zip(vanilla["phase_two"], ours["phase_two"]):
        step = v["step"]
        combat = step in ("attack", "wait", "respawn")
        for side in ("a", "b"):
            vr, orr = reply(v[side], combat), reply(o[side], combat)
            label = f"{step} → seen by {'ovprobe' if side == 'a' else 'ovother'}"
            two.add(label, vr, orr, unordered_listing(step[2:], vr))
    failures += two.report()

    # Our dedicated server prints through its logger: "[18:01:23.020]
    # [ov-tick/INFO ] [server] " before the line the jar prints bare.
    logger = re.compile(r"^\[[0-9:.]+\] \[[^\]]+\] \[[^\]]+\] ")

    def lines_of(entry: dict) -> list[str]:
        return [logger.sub("", line) for line in entry["lines"]]

    console = Tally("console")
    for v, o in zip(vanilla["console"], ours["console"]):
        console.add(v["command"], lines_of(v)[-1:], lines_of(o)[-1:])
    failures += console.report()

    def admin(document: dict) -> list[str]:
        lines = lines_of(document["console"][0]) if document["console"] else []
        return [line for line in lines if line.startswith("[") and ": " in line
                and not line.startswith("[Not Secure]")]
    log = Tally("operators' log")
    for i, (v, o) in enumerate(zip(admin(vanilla), admin(ours))):
        log.add(f"line {i}", [v], [o])
    if len(admin(vanilla)) != len(admin(ours)):
        log.add("line count", [str(len(admin(vanilla)))], [str(len(admin(ours)))])
    failures += log.report()
    return failures


# ── scoreboard.dat ──────────────────────────────────────────────────────────

def nbt(data: bytes):
    def payload(t: int, i: int):
        if t == 1:
            return struct.unpack_from(">b", data, i)[0], i + 1
        if t == 2:
            return struct.unpack_from(">h", data, i)[0], i + 2
        if t == 3:
            return struct.unpack_from(">i", data, i)[0], i + 4
        if t == 4:
            return struct.unpack_from(">q", data, i)[0], i + 8
        if t == 5:
            return struct.unpack_from(">f", data, i)[0], i + 4
        if t == 6:
            return struct.unpack_from(">d", data, i)[0], i + 8
        if t == 8:
            n = struct.unpack_from(">H", data, i)[0]
            return data[i + 2:i + 2 + n].decode(), i + 2 + n
        if t == 9:
            element, n = data[i], struct.unpack_from(">i", data, i + 1)[0]
            i += 5
            out = []
            for _ in range(n):
                v, i = payload(element, i)
                out.append(v)
            return ("list", element, out), i
        if t == 10:
            out = {}
            while True:
                tt = data[i]
                i += 1
                if tt == 0:
                    return out, i
                n = struct.unpack_from(">H", data, i)[0]
                key = data[i + 2:i + 2 + n].decode()
                i += 2 + n
                v, i = payload(tt, i)
                out[key] = (tt, v)
        raise ValueError(f"tag {t}")
    n = struct.unpack_from(">H", data, 1)[0]
    return payload(data[0], 3 + n)[0]


def leaves(path: Path) -> set[str]:
    """Every fact the file states, keyed so that order does not matter where
    the game's own order is not reproducible (a holder's scores)."""
    root = nbt(gzip.decompress(path.read_bytes()))
    out = {f"DataVersion={root['DataVersion'][1]}"}
    data = root["data"][1]
    for key, (tt, value) in data.items():
        if key == "Objectives":
            for o in value[2]:
                out.add("objective " + json.dumps({k: v[1] for k, v in o.items()}, sort_keys=True))
        elif key == "PlayerScores":
            for s in value[2]:
                out.add("score " + json.dumps({k: v[1] for k, v in s.items()}, sort_keys=True))
        elif key == "Teams":
            for t in value[2]:
                fields = {k: (sorted(v[1][2]) if k == "Players" else v[1]) for k, v in t.items()}
                out.add("team " + json.dumps(fields, sort_keys=True))
        elif key == "DisplaySlots":
            for slot, (_, name) in value.items():
                out.add(f"slot {slot}={name}")
        else:
            out.add(f"data.{key}={value!r}")
    for key in root:
        if key not in ("data", "DataVersion"):
            out.add(f"root.{key}")
    return out


def compare_dat(a: Path, b: Path) -> int:
    left, right = leaves(a), leaves(b)
    print(f"{a.name}: {len(left)} facts · {b.name}: {len(right)} facts · "
          f"{len(left & right)} identical")
    for fact in sorted(left - right):
        print(f"  only in {a.name}: {fact}")
    for fact in sorted(right - left):
        print(f"  only in {b.name}: {fact}")
    return len(left ^ right)


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == "dat":
        return compare_dat(Path(sys.argv[2]), Path(sys.argv[3]))
    vanilla = Path(sys.argv[1]) if len(sys.argv) > 1 else OUT / "capture_vanilla.json"
    ours = Path(sys.argv[2]) if len(sys.argv) > 2 else OUT / "capture_ov.json"
    return min(compare(vanilla, ours), 255)


if __name__ == "__main__":
    sys.exit(main())
