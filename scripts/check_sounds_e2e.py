#!/usr/bin/env python3
"""The same gestures, the same two probes, the real server and ours: compared.

capture_sound_packets.py records what an actor and an ear receive for every
gesture. Run once against the vanilla 1.20.1 jar and once against our
ov_dedicated, the two captures are compared here gesture by gesture, on what a
player would hear:

  * which events arrive, and to whom — the actor, the ear, or both;
  * their category and volume, exactly;
  * their pitch: equal when vanilla's is fixed, and inside vanilla's range
    when it is random (the same gesture draws a different pitch every time);
  * World Event 2001 and its block state, for a break.

Positions are compared to the eighth of a block the packet carries.

Usage:
    python3 scripts/check_sounds_e2e.py            # capture ours, then compare
    python3 scripts/check_sounds_e2e.py --compare  # compare two existing captures

Exit status 0 when every gesture both servers can perform agrees.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from decode_sound_packets import decode  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
VANILLA = NORMALIZED / "sound_packets_vanilla.json"
OURS = NORMALIZED / "sound_packets_ours.json"

# Gestures ours cannot perform, by the command or feature they need. Named, not
# counted as agreement or disagreement.
NOT_APPLICABLE = {
    "layout.playsound": "/playsound is not implemented by our server",
    "layout.playsound_inline": "/playsound is not implemented by our server",
    "layout.playsound_min_volume": "/playsound is not implemented by our server",
    "layout.stopsound_all": "/stopsound is not implemented by our server",
    "layout.stopsound_source": "/stopsound is not implemented by our server",
    "layout.stopsound_sound": "/stopsound is not implemented by our server",
    "layout.stopsound_both": "/stopsound is not implemented by our server",
    "bucket.water.empty": "buckets are not implemented by our server",
    "bucket.water.fill": "buckets are not implemented by our server",
    "bucket.lava.empty": "buckets are not implemented by our server",
    "bucket.lava.fill": "buckets are not implemented by our server",
    "mob.ambient": "no ambient sound timer in our server",
}


def heard(capture: dict, names: list[str]) -> dict:
    out = {}
    for label, gesture in capture["gestures"].items():
        row = {}
        for who in ("actor", "ear"):
            row[who] = [d for pid, hx in gesture[who]
                        if (d := decode(pid, bytes.fromhex(hx), names)) is not None]
        out[label] = row
    return out


def key(d: dict) -> tuple:
    if d["kind"] == "world_event":
        return ("world_event", d["event"], d["data"])
    if d["kind"] == "stop":
        return ("stop", d.get("source"), d.get("sound"))
    return (d["kind"], d.get("sound"), d.get("category"), round(d.get("volume", 0), 4))


def compare(vanilla: dict, ours: dict, ranges: dict) -> tuple[list, int, int, int]:
    rows, agree, disagree, skipped = [], 0, 0, 0
    for label, theirs in vanilla.items():
        if label in NOT_APPLICABLE and NOT_APPLICABLE[label]:
            rows.append((label, "n/a", NOT_APPLICABLE[label]))
            skipped += 1
            continue
        mine = ours.get(label)
        if mine is None:
            rows.append((label, "MISSING", "not in our capture"))
            disagree += 1
            continue
        problems = []
        for who in ("actor", "ear"):
            want = sorted(key(d) for d in theirs[who])
            got = sorted(key(d) for d in mine[who])
            if want != got:
                problems.append(f"{who}: vanilla {want} / ours {got}")
                continue
            # Same events: now the pitch, event by event in arrival order.
            for a, b in zip(sorted(theirs[who], key=key), sorted(mine[who], key=key)):
                if a["kind"] != "sound":
                    continue
                lo, hi = ranges.get(a["sound"], (a["pitch"], a["pitch"]))
                if not (lo - 1e-4 <= b["pitch"] <= hi + 1e-4):
                    problems.append(f"{who}: {a['sound']} pitch {b['pitch']} outside "
                                    f"vanilla's [{lo}, {hi}]")
                if any(abs(p - q) > 0.13 for p, q in zip(a["pos"], b["pos"])):
                    problems.append(f"{who}: {a['sound']} at {b['pos']}, vanilla {a['pos']}")
        if problems:
            rows.append((label, "DIFFERS", "; ".join(problems)))
            disagree += 1
        else:
            n = len(theirs["actor"]) + len(theirs["ear"])
            rows.append((label, "same", f"{n} packets"))
            agree += 1
    return rows, agree, disagree, skipped


def pitch_ranges(*captures: dict) -> dict:
    """Every pitch vanilla was seen to use for each event, across all captures."""
    ranges: dict = {}
    for capture in captures:
        for gesture in capture.values():
            for who in ("actor", "ear"):
                for d in gesture[who]:
                    if d["kind"] != "sound":
                        continue
                    lo, hi = ranges.get(d["sound"], (d["pitch"], d["pitch"]))
                    ranges[d["sound"]] = (min(lo, d["pitch"]), max(hi, d["pitch"]))
    # Random pitches seen once or twice understate their range; the measured
    # tables (sound_events.json) widen them where they exist.
    events = NORMALIZED / "sound_events.json"
    if events.is_file():
        doc = json.loads(events.read_text())
        for row in doc.get("toggle_sounds", {}).values():
            for gesture in ("open", "close"):
                g = row.get(gesture) or {}
                if g.get("sound"):
                    lo, hi = ranges.get(g["sound"], (g["pitch_lo"], g["pitch_hi"]))
                    ranges[g["sound"]] = (min(lo, g["pitch_lo"]), max(hi, g["pitch_hi"]))
        for row in doc.get("mob_sounds", {}).values():
            for gesture in ("hurt", "death"):
                g = row.get(gesture) or {}
                if g.get("sound"):
                    lo, hi = ranges.get(g["sound"], (g["pitch_lo"], g["pitch_hi"]))
                    ranges[g["sound"]] = (min(lo, g["pitch_lo"]), max(hi, g["pitch_hi"]))
    # Documented ranges (the wiki's sound tables), where a capture cannot pin one.
    ranges.setdefault("minecraft:block.chest.open", (0.9, 1.0))
    ranges.setdefault("minecraft:block.chest.close", (0.9, 1.0))
    for event in ("minecraft:block.chest.open", "minecraft:block.chest.close"):
        lo, hi = ranges[event]
        ranges[event] = (min(lo, 0.9), max(hi, 1.0))
    ranges["minecraft:entity.player.hurt"] = (0.8, 1.2)
    return ranges


def main() -> int:
    if "--compare" not in sys.argv:
        env = dict(os.environ, OV_SOUND_PORT=os.environ.get("OV_SOUND_PORT", "25751"))
        subprocess.run([sys.executable, str(ROOT / "scripts" / "capture_sound_packets.py"), "ours"],
                       check=True, env=env)
    names = json.loads((NORMALIZED / "registries.json").read_text())[
        "registries"]["minecraft:sound_event"]["entries"]
    vanilla = heard(json.loads(VANILLA.read_text()), names)
    ours = heard(json.loads(OURS.read_text()), names)
    rows, agree, disagree, skipped = compare(vanilla, ours, pitch_ranges(vanilla))
    width = max(len(label) for label, _, _ in rows)
    for label, verdict, detail in rows:
        print(f"  {label:{width}s}  {verdict:8s} {detail}")
    print(f"\n{agree} gestures identical, {disagree} differ, {skipped} not applicable "
          f"(of {len(rows)})")
    return 0 if disagree == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
