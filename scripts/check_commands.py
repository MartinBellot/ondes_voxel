#!/usr/bin/env python3
"""Compare what our server answers to what the vanilla jar answered.

Both captures come from scripts/capture_commands.py, the same probe typing the
same commands in the same order into a fresh flat world:

    data/vanilla/1.20.1/normalized/commands_capture_vanilla.json
    data/vanilla/1.20.1/normalized/commands_capture_ov.json

Three comparisons, each with its own count:

  replies      for every command, the chat-shaped packets that came back —
               System Chat (its JSON, byte for byte), Player and Disguised Chat
               (body, chat type, sender and target names), titles, Change
               Difficulty, the game-mode Game Event — in order;
  suggestions  every completion request: start, length and the matches;
  graph        the Commands packet each server sent an operator, decoded by
               scripts/commands_graph.py (a decoder written from the protocol
               page, not from our encoder), compared command by command for
               the commands both servers have.

What is normalised, and only this: a mob's uuid (random in vanilla,
derived in ours), the world seed, and the "max players" number of /list —
configuration, not behaviour. Everything else must be equal to count.

Usage: python3 scripts/check_commands.py [vanilla.json ov.json]
Exit status: the number of differing replies.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from commands_graph import command_trees, decode  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
PROBE_UUID = "f3f20367-e988-37d8-ac20-1a1929fd1e59"
UUID = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")

# The packets a reply is made of. Others (sounds, entity spawns, slot updates,
# the weather's level changes) are world traffic, compared elsewhere or not at
# all, and listed in docs/provenance/commandes.md.
REPLY_IDS = {0x64, 0x35, 0x1B, 0x5F, 0x5D, 0x46, 0x60, 0x0E, 0x0C}


def normalise(text: str) -> str:
    text = UUID.sub(lambda m: m.group(0) if m.group(0) == PROBE_UUID else "<uuid>", text)
    if "commands.seed.success" in text:
        # Configuration: vanilla drew a random seed, our flat world has 0.
        text = re.sub(r'(copy_to_clipboard","value":"|"insertion":"|"text":")-?\d+', r"\1<seed>", text)
    text = re.sub(r'"-?\d{6,}"', '"<seed>"', text)
    text = re.sub(r'(copy_to_clipboard","value":")-?\d+', r"\1<seed>", text)
    text = re.sub(r'("insertion":")-?\d{6,}', r"\1<seed>", text)
    text = re.sub(r'("text":")-?\d{6,}', r"\1<seed>", text)
    text = re.sub(r'(commands\.list\.players","with":\["\d+",)"\d+"', r'\1"<max>"', text)
    return text


def reply(packets: list[dict]) -> list[str]:
    out = []
    for p in packets:
        pid = p["id"]
        if pid not in REPLY_IDS:
            continue
        d = p.get("decoded") or {}
        if pid == 0x64:
            if "ovsync" in d.get("content", ""):
                continue
            out.append("chat " + normalise(d.get("content", "")))
        elif pid == 0x35:
            out.append("player_chat " + normalise(json.dumps(
                {k: d.get(k) for k in ("body", "chat_type", "name", "target", "unsigned",
                                       "signed", "previous", "filter")}, sort_keys=True)))
        elif pid == 0x1B:
            out.append("disguised " + normalise(json.dumps(d, sort_keys=True)))
        elif pid in (0x5F, 0x5D, 0x46):
            out.append(f"title{pid:02x} " + normalise(d.get("text", "")))
        elif pid == 0x60:
            out.append(f"times {d.get('times')}")
        elif pid == 0x0E:
            out.append(f"clear_titles {d.get('reset')}")
        elif pid == 0x0C:
            out.append(f"difficulty {d.get('difficulty')} {d.get('locked')}")
    return out


def main() -> int:
    vanilla_path = Path(sys.argv[1]) if len(sys.argv) > 2 else NORMALIZED / "commands_capture_vanilla.json"
    ov_path = Path(sys.argv[2]) if len(sys.argv) > 2 else NORMALIZED / "commands_capture_ov.json"
    vanilla = json.load(open(vanilla_path))
    ov = json.load(open(ov_path))

    # ── Replies ──
    same = 0
    differences = []
    by_command = {c["command"]: None for c in vanilla["commands"]}
    for v, o in zip(vanilla["commands"], ov["commands"]):
        assert v["command"] == o["command"], (v["command"], o["command"])
        vr, orr = reply(v["packets"]), reply(o["packets"])
        if vr == orr:
            same += 1
        else:
            differences.append((v["command"], vr, orr))
    total = len(vanilla["commands"])
    print(f"replies: {same}/{total} identical")
    for command, vr, orr in differences:
        print(f"  /{command}")
        for i in range(max(len(vr), len(orr))):
            a = vr[i] if i < len(vr) else "—"
            b = orr[i] if i < len(orr) else "—"
            if a != b:
                print(f"    vanilla: {a[:300]}")
                print(f"    ours:    {b[:300]}")
                break

    # ── /help, restricted to the commands this server has ──
    def help_lines(capture: dict) -> list[str]:
        for c in capture["commands"]:
            if c["command"] == "help":
                return [json.loads(p["decoded"]["content"]).get("text", "")
                        for p in c["packets"] if p["id"] == 0x64]
        return []

    ours_help = help_lines(ov)
    ours_names = {line.split()[0] for line in ours_help}
    vanilla_help = [line for line in help_lines(vanilla) if line.split()[0] in ours_names]
    print(f"help: {sum(a == b for a, b in zip(vanilla_help, ours_help))}/{len(ours_help)} usage "
          f"lines identical to vanilla's for the same commands"
          f"{'' if vanilla_help == ours_help else ' (order or content differs)'}")

    # ── The console, log prefix stripped ──
    prefix = re.compile(r"^\[[0-9:.]+\] \[[^\]]+\] \[[^\]]+\] ")

    def console_lines(lines: list[str], strip: bool) -> list[str]:
        # The first console batch also sweeps up what the probe's own commands
        # logged before it; the same filter runs on both sides.
        out = []
        for line in lines:
            line = normalise(prefix.sub("", line) if strip else line)
            if (line.startswith("[ovprobe:") or "ovprobe>" in line or "* ovprobe" in line
                    or "[ovprobe]" in line or "moved too quickly" in line
                    or "advancement" in line or "was killed" in line
                    or re.match(r"saved \d+ chunks", line)):  # our autosave's own log
                continue
            line = re.sub(r"Seed: \[-?\d+\]", "Seed: [<seed>]", line)
            out.append(re.sub(r"of a max of \d+", "of a max of <max>", line))
        return out

    c_same = 0
    c_diff = []
    for v, o in zip(vanilla["console"], ov["console"]):
        vl = console_lines(v["lines"], False)
        ol = console_lines(o["lines"], True)
        if vl == ol:
            c_same += 1
        else:
            c_diff.append((v["command"], vl, ol))
    print(f"console: {c_same}/{len(vanilla['console'])} commands print the same lines")
    for command, a, b in c_diff:
        print(f"  {command!r}: vanilla {a[-2:]} / ours {b[-2:]}")

    # ── Suggestions ──
    s_same = 0
    s_diff = []
    for v, o in zip(vanilla["suggestions"], ov["suggestions"]):
        vd = [p.get("decoded") for p in v["packets"] if p["id"] == 0x0F]
        od = [p.get("decoded") for p in o["packets"] if p["id"] == 0x0F]
        strip = lambda ds: [(d["start"], d["length"], d["matches"]) for d in ds if d]  # noqa: E731
        if strip(vd) == strip(od):
            s_same += 1
        else:
            s_diff.append((v["text"], strip(vd), strip(od)))
    print(f"suggestions: {s_same}/{len(vanilla['suggestions'])} identical")
    for text, a, b in s_diff:
        if text == "/" and a and b:
            # Vanilla has 79 commands and we have fewer: compare the ones we have.
            theirs = [m for m in a[0][2] if m[0] in {n[0] for n in b[0][2]}]
            same = theirs == b[0][2] and a[0][:2] == b[0][:2]
            print(f"  '/': {'identical' if same else 'differs'} when restricted to our "
                  f"{len(b[0][2])} commands")
            continue
        print(f"  {text!r}: vanilla {str(a)[:200]} / ours {str(b)[:200]}")

    # ── The Commands graph, as an operator receives it ──
    def graph_of(capture: dict) -> dict:
        packets = [p for p in capture["op"] if p["id"] == 0x10]
        if not packets:
            packets = [p for p in capture["login"] if p["id"] == 0x10]
        return decode(bytes.fromhex(packets[-1]["hex"]))

    vt = command_trees(graph_of(vanilla))
    ot = command_trees(graph_of(ov))
    shared = [name for name in ot if name in vt]
    g_same = [name for name in shared if vt[name] == ot[name]]
    print(f"graph: {len(g_same)}/{len(shared)} command trees identical "
          f"({len(ot)} ours, {len(vt)} vanilla)")
    for name in shared:
        if vt[name] != ot[name]:
            print(f"  {name}: differs")
    order_v = [n for n in vt if n in ot]
    order_o = [n for n in ot if n in vt]
    print(f"graph: root order {'identical' if order_v == order_o else 'differs'}")

    # ── The level-0 tree, as a non-operator receives it at login ──
    def login_tree(capture: dict) -> list[str]:
        packets = [p for p in capture["login"] if p["id"] == 0x10]
        return list(command_trees(decode(bytes.fromhex(packets[0]["hex"])))) if packets else []

    print(f"login tree: vanilla {login_tree(vanilla)} / ours {login_tree(ov)}")
    return len(differences)


if __name__ == "__main__":
    sys.exit(main())
