#!/usr/bin/env python3
"""Compare the dedicated server's administration: the real 1.20.1 jar and ours.

Both sides come from scripts/capture_admin.py, run in the same fresh
directory layout (same server.properties, same pre-written list files, same
server-icon.png). Each comparison prints its count and its differences:

  properties  the file a first start writes: header, date line (its pattern),
              every key, its value, the order of the keys
  status      the Status Response JSON, member by member
  query       basic and full stat, field by field (the challenge token is
              random on both sides and compared by shape)
  rcon        every answer: request id, type, body, the two trailing NULs
  logins      the first packet each connection was sent: result and JSON
  commands    the operator's replies, byte for byte (check_commands' reply())
  console     the lines each console command printed
  files       ops.json, whitelist.json, banned-*.json and server.properties as
              the servers left them — `created` dates and the date line aside
  shutdown    what a connected player is sent by `stop`

Normalised, and only this: the probe's own offline uuid is kept, the seed,
dates written at run time (`created`, the properties date), a debug report's
seconds and ticks per second. Usage:

  python3 scripts/check_admin.py [vanilla.json ov.json] [props_vanilla.json props_ov.json]
"""
from __future__ import annotations

import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_commands import normalise, reply  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SCRATCH = ROOT / ".scratch" / "admin"
DATE = re.compile(r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d [+-]\d{4}")


def show(label: str, same: int, total: int, diffs: list[str]) -> None:
    print(f"{label}: {same}/{total} identical")
    for line in diffs[:40]:
        print(f"  {line}")


def properties_lines(text: str) -> tuple[list[str], list[str]]:
    lines = text.split("\n")
    comments = [line for line in lines if line.startswith("#")]
    body = [line for line in lines if line and not line.startswith("#")]
    return comments, body


def compare_properties(vanilla: dict, ours: dict) -> int:
    v_text = vanilla["files"].get("server.properties") or ""
    o_text = ours["files"].get("server.properties") or ""
    v_comments, v_body = properties_lines(v_text)
    o_comments, o_body = properties_lines(o_text)
    diffs = []
    header_ok = v_comments[:1] == o_comments[:1]
    date_re = re.compile(r"^#[A-Z][a-z]{2} [A-Z][a-z]{2} \d\d \d\d:\d\d:\d\d \S+ \d{4}$")
    date_ok = len(o_comments) > 1 and bool(date_re.match(o_comments[1])) and bool(
        date_re.match(v_comments[1]))
    if not header_ok:
        diffs.append(f"header: {v_comments[:1]} / {o_comments[:1]}")
    if not date_ok:
        diffs.append(f"date line: {v_comments[1:2]} / {o_comments[1:2]}")
    v_map = dict(line.split("=", 1) for line in v_body)
    o_map = dict(line.split("=", 1) for line in o_body)
    keys = sorted(set(v_map) | set(o_map))
    same = 0
    for key in keys:
        if v_map.get(key) == o_map.get(key):
            same += 1
        else:
            diffs.append(f"{key}: vanilla {v_map.get(key)!r} / ours {o_map.get(key)!r}")
    show("properties keys and values", same, len(keys), diffs)
    v_order = [line.split("=", 1)[0] for line in v_body]
    o_order = [line.split("=", 1)[0] for line in o_body]
    order_same = sum(a == b for a, b in zip(v_order, o_order))
    print(f"properties order: {order_same}/{len(v_order)} keys at the same line"
          f"{' (identical)' if v_order == o_order else ''}")
    if v_order != o_order:
        for i, (a, b) in enumerate(zip(v_order, o_order)):
            if a != b:
                print(f"  first difference at line {i}: vanilla {a} / ours {b}")
                break
    print(f"properties text (date line aside): "
          f"{'identical' if v_comments[:1] == o_comments[:1] and v_body == o_body else 'differs'}")
    return len(diffs) + (0 if v_order == o_order else 1)


def cstrings(raw: bytes) -> list[str]:
    return raw.split(b"\x00")


def decode_query(entry: dict) -> dict:
    out: dict = {}
    if entry.get("handshake"):
        raw = bytes.fromhex(entry["handshake"])
        out["handshake"] = {"type": raw[0], "session": raw[1:5].hex(),
                            "token_digits": raw[5:].rstrip(b"\x00").isdigit(),
                            "terminated": raw.endswith(b"\x00")}
    if entry.get("basic"):
        raw = bytes.fromhex(entry["basic"])
        parts = raw[5:].split(b"\x00", 5)
        rest = parts[5]
        out["basic"] = {"type": raw[0], "session": raw[1:5].hex(),
                        "fields": [p.decode("utf-8", "replace") for p in parts[:5]],
                        "port_le": struct.unpack_from("<H", rest, 0)[0],
                        "ip": rest[2:].rstrip(b"\x00").decode()}
    if entry.get("full"):
        raw = bytes.fromhex(entry["full"])
        out["full"] = {"type": raw[0], "session": raw[1:5].hex(), "bytes": raw[5:].hex()}
        out["full_text"] = [s.decode("utf-8", "replace") for s in cstrings(raw[5:])]
    out["bad_token"] = entry.get("bad_token")
    out["error"] = entry.get("error")
    return out


def compare_query(vanilla: dict, ours: dict) -> int:
    v, o = decode_query(vanilla["query"]), decode_query(ours["query"])
    keys = ["handshake", "basic", "full", "bad_token", "error"]
    diffs = [f"{k}: vanilla {v.get(k)} / ours {o.get(k)}" for k in keys if v.get(k) != o.get(k)]
    if v.get("full") != o.get("full"):
        diffs.append(f"full as text: vanilla {v.get('full_text')} / ours {o.get('full_text')}")
    show("query", len(keys) - sum(v.get(k) != o.get(k) for k in keys), len(keys), diffs)
    return sum(v.get(k) != o.get(k) for k in keys)


def rcon_view(packets: list[dict]) -> list:
    out = []
    for p in packets:
        if p.get("closed"):
            out.append("closed")
            continue
        body = re.sub(r"Seed: \[-?\d+\]", "Seed: [<seed>]", p["body"])
        body = re.sub(r"after [\d.]+ seconds and \d+ ticks \([\d.]+ ticks per second\)",
                      "after <s> seconds and <n> ticks (<tps> ticks per second)", body)
        out.append([p["request"], p["type"], body, p["tail"]])
    return out


def compare_rcon(vanilla: dict, ours: dict) -> int:
    v, o = vanilla["rcon"], ours["rcon"]
    cases = [("bad_auth", v.get("bad_auth"), o.get("bad_auth")),
             ("auth", v.get("auth"), o.get("auth"))]
    for vc, oc in zip(v.get("commands", []), o.get("commands", [])):
        cases.append((f"command {vc['command']!r}", vc["packets"], oc["packets"]))
    for key in ("split", "unknown_type", "unauthenticated_command"):
        cases.append((key, v.get(key), o.get(key)))
    diffs, same = [], 0
    for name, a, b in cases:
        va, ob = rcon_view(a or []), rcon_view(b or [])
        if va == ob:
            same += 1
        else:
            diffs.append(f"{name}: vanilla {str(va)[:300]} / ours {str(ob)[:300]}")
    show("rcon", same, len(cases), diffs)
    return len(diffs)


def compare_logins(vanilla: dict, ours: dict) -> int:
    cases = list(zip(vanilla["logins"], ours["logins"]))
    cases.append((vanilla.get("full"), ours.get("full")))
    cases.append(({"duplicate_old": [p.get("decoded") for p in vanilla.get("duplicate_old", [])]},
                  {"duplicate_old": [p.get("decoded") for p in ours.get("duplicate_old", [])]}))
    cases.append(({"ip_ban_kick": [(p["id"], p.get("decoded")) for p in vanilla.get("ip_ban_kick", [])]},
                  {"ip_ban_kick": [(p["id"], p.get("decoded")) for p in ours.get("ip_ban_kick", [])]}))
    diffs = [f"vanilla {a} / ours {b}" for a, b in cases if a != b]
    show("logins and kicks", len(cases) - len(diffs), len(cases), diffs)
    return len(diffs)


def compare_commands(vanilla: dict, ours: dict) -> int:
    diffs, same = [], 0
    for v, o in zip(vanilla["commands"], ours["commands"]):
        assert v["command"] == o["command"]
        vr = [re.sub(r'"-?\d+\.\d\d"', '"<x.xx>"', line) if "debug.stopped" in line else line
              for line in reply(v["packets"])]
        orr = [re.sub(r'"-?\d+\.\d\d"', '"<x.xx>"', line) if "debug.stopped" in line else line
               for line in reply(o["packets"])]
        vr = [re.sub(r'(commands\.debug\.stopped","with":\["<x.xx>",)"\d+"', r'\1"<n>"', x) for x in vr]
        orr = [re.sub(r'(commands\.debug\.stopped","with":\["<x.xx>",)"\d+"', r'\1"<n>"', x) for x in orr]
        if vr == orr:
            same += 1
        else:
            first = next((i for i in range(max(len(vr), len(orr)))
                          if (vr[i] if i < len(vr) else None) != (orr[i] if i < len(orr) else None)), 0)
            diffs.append(f"/{v['command']}: vanilla {vr[first] if first < len(vr) else '—'}"
                         f" / ours {orr[first] if first < len(orr) else '—'}")
    show("commands", same, len(vanilla["commands"]), diffs)
    return len(diffs)


def console_view(lines: list[str]) -> list[str]:
    out = []
    for line in lines:
        line = normalise(line)
        if ("ovprobe" in line and ("joined" in line or "logged in" in line or "lost connection" in line)) \
                or re.match(r"saved \d+ chunks", line) or "Thread RCON" in line \
                or "logging in as" in line or "handshake" in line or "Disconnecting" in line \
                or "UUID of player" in line or "left the game" in line:
            continue
        line = re.sub(r"Seed: \[-?\d+\]", "Seed: [<seed>]", line)
        line = re.sub(r"after [\d.]+ seconds and \d+ ticks \([\d.]+ ticks per second\)",
                      "after <s> seconds and <n> ticks (<tps> ticks per second)", line)
        out.append(line)
    return out


def compare_console(vanilla: dict, ours: dict) -> int:
    diffs, same = [], 0
    for v, o in zip(vanilla["console"], ours["console"]):
        vl, ol = console_view(v["lines"]), console_view(o["lines"])
        if vl == ol:
            same += 1
        else:
            diffs.append(f"{v['command']!r}: vanilla {vl[-3:]} / ours {ol[-3:]}")
    show("console", same, len(vanilla["console"]), diffs)
    return len(diffs)


def list_view(text: str | None):
    if text is None:
        return None
    try:
        entries = json.loads(text)
    except json.JSONDecodeError as error:
        return f"unreadable: {error}"
    for entry in entries:
        if "created" in entry:
            entry["created"] = "<date>" if DATE.fullmatch(entry["created"]) else entry["created"]
    return entries


def compare_files(vanilla: dict, ours: dict) -> int:
    diffs, same, total = [], 0, 0
    for name in ["ops.json", "whitelist.json", "banned-players.json", "banned-ips.json"]:
        total += 1
        a, b = list_view(vanilla["files"].get(name)), list_view(ours["files"].get(name))
        if a == b:
            same += 1
            # Byte for byte too, dates aside: key order, indentation, escaping.
            ta = DATE.sub("<date>", vanilla["files"].get(name) or "")
            tb = DATE.sub("<date>", ours["files"].get(name) or "")
            if ta != tb:
                diffs.append(f"{name}: same entries, different text")
        else:
            diffs.append(f"{name}: vanilla {json.dumps(a)[:400]} / ours {json.dumps(b)[:400]}")
    total += 1
    v_props = [l for l in (vanilla["files"].get("server.properties") or "").split("\n")[2:]]
    o_props = [l for l in (ours["files"].get("server.properties") or "").split("\n")[2:]]
    if v_props == o_props:
        same += 1
    else:
        vm = dict(l.split("=", 1) for l in v_props if "=" in l)
        om = dict(l.split("=", 1) for l in o_props if "=" in l)
        changed = [k for k in sorted(set(vm) | set(om)) if vm.get(k) != om.get(k)]
        diffs.append("server.properties after the run: "
                     + ", ".join(f"{k} vanilla {vm.get(k)!r} ours {om.get(k)!r}" for k in changed)
                     + ("" if changed else " (order)"))
    show("files after the run", same, total, diffs)
    return len(diffs)


def compare_status(vanilla: dict, ours: dict) -> int:
    v = json.loads(vanilla["status"]["json"])
    o = json.loads(ours["status"]["json"])
    keys = sorted(set(v) | set(o))
    diffs = [f"{k}: vanilla {json.dumps(v.get(k))[:200]} / ours {json.dumps(o.get(k))[:200]}"
             for k in keys if v.get(k) != o.get(k)]
    show("status members", len(keys) - len(diffs), len(keys), diffs)
    print(f"status text: {'identical' if vanilla['status']['json'] == ours['status']['json'] else 'differs'}"
          f"; pong {'identical' if vanilla['status']['pong'] == ours['status']['pong'] else 'differs'}")
    return len(diffs)


def compare_shutdown(vanilla: dict, ours: dict) -> int:
    a = [p.get("decoded") for p in vanilla["shutdown"]["packets"]]
    b = [p.get("decoded") for p in ours["shutdown"]["packets"]]
    same = a == b
    print(f"shutdown: {'identical' if same else f'vanilla {a} / ours {b}'}"
          f" (exit codes {vanilla['shutdown']['exit']} / {ours['shutdown']['exit']})")
    return 0 if same else 1


def main() -> int:
    args = sys.argv[1:]
    vanilla_path = Path(args[0]) if len(args) > 1 else SCRATCH / "admin_vanilla.json"
    ov_path = Path(args[1]) if len(args) > 1 else SCRATCH / "admin_ov.json"
    pv = Path(args[2]) if len(args) > 3 else SCRATCH / "admin_properties.json"
    po = Path(args[3]) if len(args) > 3 else SCRATCH / "admin_properties-ov.json"
    failures = 0
    if pv.exists() and po.exists():
        failures += compare_properties(json.load(open(pv)), json.load(open(po)))
    if vanilla_path.exists() and ov_path.exists():
        vanilla, ours = json.load(open(vanilla_path)), json.load(open(ov_path))
        for compare in (compare_status, compare_query, compare_rcon, compare_logins,
                        compare_commands, compare_console, compare_files, compare_shutdown):
            failures += compare(vanilla, ours)
    return failures


if __name__ == "__main__":
    sys.exit(main())
