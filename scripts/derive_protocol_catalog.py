#!/usr/bin/env python3
"""Derive data/protocol/763.json — every packet of protocol 763, with its id.

Two independent sources are read and must agree on every single id, or nothing
is written:

  * PrismarineJS/minecraft-data, `data/pc/1.20/protocol.json` (MIT). Its
    `dataPaths.json` maps pc 1.20.1 to the pc/1.20 protocol file.
  * The frozen wiki.vg archive CLAUDE.md names, oldid=2773082, whose banner
    reads "1.20.1, protocol 763". Only the packet headings and their
    "Packet ID" / "Bound To" cells are read, as raw wikitext.

What is kept is the catalogue — state, direction, id, the wiki's name and
minecraft-data's name — not the field layouts. Field layouts are specified in
the C++ packet by packet, from the archive, and tested byte for byte; a schema
file would only be a second copy of them.

Neither source is fetched here, on purpose: the network is not part of the
build. Download them once, then run this:

  curl -o /tmp/protocol.json \\
    https://raw.githubusercontent.com/PrismarineJS/minecraft-data/master/data/pc/1.20/protocol.json
  curl -o /tmp/wiki.txt \\
    'https://minecraft.wiki/w/Minecraft_Wiki:Projects/wiki.vg_merge/Protocol?oldid=2773082&action=raw'
  python3 scripts/derive_protocol_catalog.py /tmp/protocol.json /tmp/wiki.txt

The output is deterministic: running it twice gives the same bytes.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "data" / "protocol" / "763.json"

STATES = {"Handshaking": "handshaking", "Status": "status", "Login": "login", "Play": "play"}
BOUND = {"Client": "clientbound", "Server": "serverbound"}
MCDATA_DIRECTION = {"clientbound": "toClient", "serverbound": "toServer"}


def wiki_packets(text: str) -> list[tuple[str, str, int, str]]:
    """(state, direction, id, name) for every packet table in the archive."""
    packets: list[tuple[str, str, int, str]] = []
    state = None
    heading = None
    pending: tuple[str, int] | None = None
    for line in text.split("\n"):
        m = re.match(r"^(=+)\s*(.*?)\s*=+\s*$", line)
        if m:
            level = len(m.group(1))
            if level == 2:
                state = m.group(2)
            heading = m.group(2) if level == 4 else None
            pending = None
            continue
        if heading and pending is None:
            m = re.match(r'^\s*\|\s*(?:rowspan="?\d+"?\s*\|)?\s*(0x[0-9A-Fa-f]{2})\s*$', line)
            if m:
                pending = (heading, int(m.group(1), 16))
                continue
        if pending is not None:
            m = re.match(r'^\s*\|\s*(?:rowspan="?\d+"?\s*\|)?\s*(Client|Server)\s*$', line)
            if m and state in STATES:
                packets.append((STATES[state], BOUND[m.group(1)], pending[1], pending[0]))
                pending = None
                heading = None
    return packets


def mcdata_mappings(protocol: dict, state: str, direction: str) -> dict[int, str]:
    node = protocol[state][MCDATA_DIRECTION[direction]]["types"]["packet"]
    mappings = node[1][0]["type"][1]["mappings"]
    return {int(key, 16): value for key, value in mappings.items()}


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    protocol = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    wiki_text = Path(sys.argv[2]).read_text(encoding="utf-8")
    if "1.20.1, protocol 763" not in wiki_text:
        sys.exit("error: the wiki text is not the 1.20.1 / 763 archive")

    wiki = wiki_packets(wiki_text)
    errors: list[str] = []
    seen: set[tuple[str, str, int]] = set()
    entries = []
    for state, direction, packet_id, name in wiki:
        key = (state, direction, packet_id)
        if key in seen:
            errors.append(f"duplicate in the wiki: {key}")
        seen.add(key)
        mc = mcdata_mappings(protocol, state, direction).get(packet_id)
        if mc is None:
            errors.append(f"{state} {direction} 0x{packet_id:02X} {name!r}: absent from minecraft-data")
        entries.append({"state": state, "direction": direction, "id": f"0x{packet_id:02X}",
                        "name": name, "mcdata": mc})

    # The other direction: nothing minecraft-data lists may be missing from the wiki.
    for state in STATES.values():
        for direction in BOUND.values():
            try:
                mappings = mcdata_mappings(protocol, state, direction)
            except (KeyError, IndexError, TypeError):
                continue
            for packet_id, mc in mappings.items():
                if (state, direction, packet_id) not in seen:
                    errors.append(f"{state} {direction} 0x{packet_id:02X} {mc}: absent from the wiki")

    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1

    order = {s: i for i, s in enumerate(STATES.values())}
    entries.sort(key=lambda e: (order[e["state"]], e["direction"], int(e["id"], 16)))

    document = {
        "$comment": (
            "Every packet of protocol 763 (Minecraft Java 1.20.1): state, direction, id and "
            "names. Derived by scripts/derive_protocol_catalog.py from two sources that agree "
            "on every id: PrismarineJS/minecraft-data data/pc/1.20/protocol.json (MIT, "
            "copyright PrismarineJS) and the frozen wiki.vg archive, minecraft.wiki oldid "
            "2773082. Names only, no field layouts. See docs/provenance/protocole-763.md."
        ),
        "protocol": 763,
        "version": "1.20.1",
        "packets": entries,
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(document, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    counts: dict[str, int] = {}
    for e in entries:
        counts[f"{e['state']} {e['direction']}"] = counts.get(f"{e['state']} {e['direction']}", 0) + 1
    print(f"{len(entries)} packets, both sources agree on every id")
    for key, value in counts.items():
        print(f"  {key:26} {value}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
