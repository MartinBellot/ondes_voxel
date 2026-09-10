#!/usr/bin/env python3
"""Les feuilles d'un monde sauvé portent-elles leur vraie distance ?

    python3 scripts/locate_leaves.py <region-dir>

Lit les régions directement (`anvil_read.py`) — quelques secondes, là où
`ov_inspect state` position par position en demande vingt-cinq minutes — puis
refait un parcours en largeur depuis toutes les bûches (`*_log`, `*_wood`) à
travers les feuilles, six pas au plus, et compare à la propriété `distance`
stockée. Au bord de la zone sauvée le parcours ne voit pas les bûches des chunks
absents : le verdict propre est celui des chunks **intérieurs**, ceux dont les
huit voisins sont sauvés. Voir docs/provenance/agriculture.md § 9.
"""
import collections
import glob
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from anvil_read import chunks  # noqa: E402


def read(region):
    blocks = {}
    present = set()
    for path in glob.glob(os.path.join(region, "r.*.mca")):
        rx, rz = map(int, re.search(r"r\.(-?\d+)\.(-?\d+)\.mca", path).groups())
        for lx, lz, nbt in chunks(path):
            cx, cz = rx * 32 + lx, rz * 32 + lz
            present.add((cx, cz))
            for sec in nbt.get("sections", []):
                states = sec.get("block_states")
                if not states:
                    continue
                palette = states["palette"]
                wanted = {i for i, p in enumerate(palette)
                          if p["Name"].endswith(("_leaves", "_log", "_wood"))}
                if not wanted:
                    continue
                data = states.get("data")
                bits = max(4, math.ceil(math.log2(len(palette)))) if len(palette) > 1 else 0
                per = 64 // bits if bits else 0
                for idx in range(4096):
                    if bits == 0:
                        v = 0
                    else:
                        word = data[idx // per] & ((1 << 64) - 1)
                        v = (word >> ((idx % per) * bits)) & ((1 << bits) - 1)
                    if v not in wanted:
                        continue
                    x, z, y = idx & 15, (idx >> 4) & 15, idx >> 8
                    p = palette[v]
                    blocks[(cx * 16 + x, sec["Y"] * 16 + y, cz * 16 + z)] = (
                        p["Name"], p.get("Properties", {}))
    return blocks, present


def true_distances(blocks):
    dist = {}
    frontier = [p for p, (n, _) in blocks.items() if n.endswith(("_log", "_wood"))]
    for p in frontier:
        dist[p] = 0
    for d in range(1, 7):
        nxt = []
        for (x, y, z) in frontier:
            for dx, dy, dz in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)):
                q = (x + dx, y + dy, z + dz)
                if q in dist or q not in blocks or not blocks[q][0].endswith("_leaves"):
                    continue
                dist[q] = d
                nxt.append(q)
        frontier = nxt
    return dist


def main():
    blocks, present = read(sys.argv[1])
    dist = true_distances(blocks)
    interior = {c for c in present
                if all((c[0] + a, c[1] + b) in present for a in (-1, 0, 1) for b in (-1, 0, 1))}
    print("chunks saved", len(present), "interior", len(interior))

    sevens = collections.Counter()
    for p, (n, props) in blocks.items():
        if n.endswith("_leaves") and props.get("distance") == "7":
            sevens["interior" if (p[0] >> 4, p[2] >> 4) in interior else "rim"] += 1
    print("leaves stored at 7:", dict(sevens))

    inside = collections.Counter()
    for p, (n, props) in blocks.items():
        if n.endswith("_leaves") and props.get("persistent") == "false" and \
                (p[0] >> 4, p[2] >> 4) in interior:
            inside[(int(props["distance"]), dist.get(p, 7))] += 1
    total = sum(inside.values())
    same = sum(v for k, v in inside.items() if k[0] == k[1])
    wrong_decay = sum(v for k, v in inside.items() if k[0] == 7 and k[1] < 7)
    higher = sum(v for k, v in inside.items() if k[1] < k[0] < 7)
    held = sum(v for k, v in inside.items() if k[0] < 7 and k[1] == 7)
    print(f"interior leaves {total}: true distance {same}, stored 7 but held {wrong_decay}, "
          f"stored higher than true {higher}, stored held but no path {held}")
    print("interior mismatches (stored, true):",
          sorted((k, v) for k, v in inside.items() if k[0] != k[1]))


if __name__ == "__main__":
    main()
