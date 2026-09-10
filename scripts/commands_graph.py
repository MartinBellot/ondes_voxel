#!/usr/bin/env python3
"""An independent decoder for the Commands packet (0x10), protocol 763.

Written from the protocol page and the Command Data article (revision of
2023-05-09, the last one before 1.20.1), and deliberately **not** from our C++
encoder: its job is to read what `ov_dedicated` sends the way a client written by
somebody else would, and to read what the vanilla jar sends the same way, so the
two graphs can be compared node for node.

Parser ids are the `minecraft:command_argument_type` registry of
`data/vanilla/1.20.1/generated/reports/registries.json`, read at run time rather
than copied.

Usage: python3 scripts/commands_graph.py <capture.json> [command ...]
prints the usage tree of the named commands as the packet declares them.
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REGISTRIES = ROOT / "data" / "vanilla" / "1.20.1" / "generated" / "reports" / "registries.json"


def parser_names() -> dict[int, str]:
    with open(REGISTRIES) as handle:
        entries = json.load(handle)["minecraft:command_argument_type"]["entries"]
    return {value["protocol_id"]: name for name, value in entries.items()}


class Reader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.i = 0

    def byte(self) -> int:
        value = self.data[self.i]
        self.i += 1
        return value

    def varint(self) -> int:
        value, shift = 0, 0
        while True:
            b = self.byte()
            value |= (b & 0x7F) << shift
            if not b & 0x80:
                break
            shift += 7
            if shift > 35:
                raise ValueError("varint too long")
        return value - (1 << 32) if value >= 1 << 31 else value

    def string(self) -> str:
        length = self.varint()
        text = self.data[self.i:self.i + length].decode("utf-8")
        if len(text.encode("utf-8")) != length:
            raise ValueError("string runs past the packet")
        self.i += length
        return text

    def take(self, fmt: str):
        size = struct.calcsize(fmt)
        values = struct.unpack_from(fmt, self.data, self.i)
        self.i += size
        return values


def read_properties(reader: Reader, parser: str) -> dict:
    """The properties of one argument, by parser name."""
    if parser in ("brigadier:float", "brigadier:double", "brigadier:integer", "brigadier:long"):
        flags = reader.byte()
        fmt = {"brigadier:float": ">f", "brigadier:double": ">d",
               "brigadier:integer": ">i", "brigadier:long": ">q"}[parser]
        out: dict = {}
        if flags & 1:
            out["min"] = reader.take(fmt)[0]
        if flags & 2:
            out["max"] = reader.take(fmt)[0]
        return out
    if parser == "brigadier:string":
        return {"type": ["word", "phrase", "greedy"][reader.varint()]}
    if parser == "minecraft:entity":
        flags = reader.byte()
        return {"amount": "single" if flags & 1 else "multiple",
                "type": "players" if flags & 2 else "entities"}
    if parser == "minecraft:score_holder":
        return {"amount": "multiple" if reader.byte() & 1 else "single"}
    if parser == "minecraft:time":
        return {"min": reader.take(">i")[0]}
    if parser in ("minecraft:resource_or_tag", "minecraft:resource_or_tag_key",
                  "minecraft:resource", "minecraft:resource_key"):
        return {"registry": reader.string()}
    return {}


def decode(payload: bytes) -> dict:
    """The graph as a list of nodes plus the root index."""
    names = parser_names()
    reader = Reader(payload)
    count = reader.varint()
    nodes = []
    for _ in range(count):
        flags = reader.byte()
        kind = flags & 3
        if kind == 3:
            raise ValueError("node type 3")
        node: dict = {"kind": ["root", "literal", "argument"][kind],
                      "executable": bool(flags & 4)}
        node["children"] = [reader.varint() for _ in range(reader.varint())]
        if flags & 8:
            node["redirect"] = reader.varint()
        if kind in (1, 2):
            node["name"] = reader.string()
        if kind == 2:
            parser_id = reader.varint()
            if parser_id not in names:
                raise ValueError(f"unknown parser id {parser_id}")
            node["parser"] = names[parser_id]
            node["properties"] = read_properties(reader, node["parser"])
            if flags & 0x10:
                node["suggestions"] = reader.string()
        elif flags & 0x10:
            raise ValueError("suggestions on a non-argument node")
        nodes.append(node)
    root = reader.varint()
    if reader.i != len(payload):
        raise ValueError(f"{len(payload) - reader.i} trailing bytes")
    for node in nodes:
        for child in node["children"] + ([node["redirect"]] if "redirect" in node else []):
            if not 0 <= child < count:
                raise ValueError(f"index {child} outside {count} nodes")
    return {"nodes": nodes, "root": root}


def subtree(graph: dict, index: int, seen: frozenset = frozenset()) -> dict:
    """A node and everything under it, as nested dicts in declared order.

    Redirects are named by the path of their target rather than followed, so a
    cycle (`execute run execute …`) terminates and two graphs with the same shape
    compare equal whatever their node numbering.
    """
    node = graph["nodes"][index]
    out = {key: node[key] for key in ("kind", "name", "executable", "parser", "properties",
                                      "suggestions") if key in node}
    if "redirect" in node:
        out["redirect"] = path_of(graph, node["redirect"])
    if index in seen:
        return out
    out["children"] = [subtree(graph, child, seen | {index}) for child in node["children"]]
    return out


def path_of(graph: dict, target: int) -> str:
    """The literal path from the root to a node, for naming a redirect."""
    parents: dict[int, int] = {}
    order = [graph["root"]]
    while order:
        current = order.pop(0)
        for child in graph["nodes"][current]["children"]:
            if child not in parents and child != graph["root"]:
                parents[child] = current
                order.append(child)
    if target == graph["root"]:
        return "<root>"
    names = []
    while target in parents:
        names.append(graph["nodes"][target].get("name", "?"))
        target = parents[target]
    return " ".join(reversed(names))


def command_trees(graph: dict) -> dict[str, dict]:
    """Every top-level command of the graph, by name."""
    root = graph["nodes"][graph["root"]]
    return {graph["nodes"][c]["name"]: subtree(graph, c) for c in root["children"]}


def render(tree: dict, depth: int = 0) -> list[str]:
    label = tree.get("name", "<root>")
    if tree["kind"] == "argument":
        label = f"<{label}: {tree['parser']} {tree.get('properties') or ''}".rstrip() + ">"
        if "suggestions" in tree:
            label += f" ?{tree['suggestions']}"
    if tree.get("executable"):
        label += " *"
    if "redirect" in tree:
        label += f" -> {tree['redirect']}"
    lines = ["  " * depth + label]
    for child in tree.get("children", []):
        lines += render(child, depth + 1)
    return lines


def main() -> int:
    capture = json.load(open(sys.argv[1]))
    wanted = sys.argv[2:]
    packets = [p for p in capture["op"] if p["id"] == 0x10]
    if not packets:
        print("no Commands packet after op", file=sys.stderr)
        return 1
    graph = decode(bytes.fromhex(packets[-1]["hex"]))
    trees = command_trees(graph)
    print(f"{len(graph['nodes'])} nodes, {len(trees)} commands")
    for name in wanted or list(trees):
        if name in trees:
            print("\n".join(render(trees[name])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
