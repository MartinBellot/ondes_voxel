#!/usr/bin/env python3
"""Every gamerule of 1.20.1, and its default, read off a fresh vanilla server.

The names and types come from the data generator's command report (the
children of `gamerule` in reports/commands.json). The defaults are *asked*: a
new flat world, nothing changed, one `gamerule <name>` per rule from the
console, the answer parsed out of "Gamerule X is currently set to: Y".

Then the table in src/ov_server/src/commands/game_rules.hpp is compared with
it, rule by rule. Exit status is the number of disagreements.

Usage: python3 scripts/measure_gamerules.py
"""
from __future__ import annotations

import json
import re
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from measure_entities import Server  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
REPORT = ROOT / "data" / "vanilla" / "1.20.1" / "generated" / "reports" / "commands.json"
TABLE = ROOT / "src" / "ov_server" / "src" / "commands" / "game_rules.hpp"
OUT = ROOT / "data" / "vanilla" / "1.20.1" / "normalized" / "gamerules.json"
RUN = ROOT / "run" / "gamerule-oracle"
ANSWER = re.compile(r"Gamerule (\w+) is currently set to: (\S+)")
ENTRY = re.compile(r'\{"(\w+)", (true|false), (-?\d+)\}')


def main() -> int:
    report = json.load(open(REPORT))["children"]["gamerule"]["children"]
    rules = {name: node["children"]["value"]["parser"] for name, node in report.items()}
    if RUN.exists():
        shutil.rmtree(RUN)
    server = Server(RUN, port=25693)
    measured: dict[str, str] = {}
    try:
        lines = server.batch([f"gamerule {name}" for name in rules])
        for line in lines:
            match = ANSWER.search(line)
            if match:
                measured[match.group(1)] = match.group(2)
    finally:
        server.stop()
        shutil.rmtree(RUN, ignore_errors=True)

    document = {"$comment": "Valeurs par défaut lues sur un serveur 1.20.1 neuf. "
                            "Voir docs/provenance/commandes.md.",
                "rules": {name: {"type": rules[name], "default": measured.get(name)}
                          for name in rules}}
    OUT.parent.mkdir(parents=True, exist_ok=True)
    json.dump(document, open(OUT, "w"), indent=1, sort_keys=True)

    ours = {m.group(1): (m.group(2) == "true", int(m.group(3)))
            for m in ENTRY.finditer(TABLE.read_text())}
    disagreements = 0
    for name, parser in rules.items():
        want = measured.get(name)
        have = ours.get(name)
        if want is None or have is None:
            print(f"  {name}: measured {want!r}, ours {have!r}")
            disagreements += 1
            continue
        integer, value = have
        text = str(value) if integer else ("true" if value else "false")
        if text != want or integer != (parser == "brigadier:integer"):
            print(f"  {name}: vanilla {want} ({parser}), ours {text}")
            disagreements += 1
    print(f"{len(rules)} rules, {len(measured)} measured, {len(ours)} in our table, "
          f"{disagreements} disagreements")
    return disagreements


if __name__ == "__main__":
    sys.exit(main())
