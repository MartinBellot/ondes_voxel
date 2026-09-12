#!/usr/bin/env python3
"""Nos entités contre celles du vrai client, espèce par espèce.

Lit ce que `measure_entity_render.py scenes` a produit sous
`.scratch/entity-render/` :

* `vanilla/<scène>.png` et `<scène>_empty.png` — le vrai client, après et avant
  l'invocation ; `vanilla/<scène>.json` — les sommets que le jeu a émis pour
  l'entité (EntityRenderDispatcher, relevés par entity_render_oracle.java) ;
* `ours/<scène>.ppm` et `<scène>_empty.ppm` — notre client, avec et sans ses
  entités ; `ours/<scène>.json` — les sommets qu'il a soumis.

Pour chaque scène :

* **silhouette** : les pixels où la capture avec l'entité diffère de la capture
  sans elle (plus de 24 niveaux sur un canal), chez chacun ; puis le rapport
  intersection / union des deux silhouettes (IoU) et leur hauteur en pixels ;
* **pixels** : dans la silhouette vanilla, la part des pixels à ±8 niveaux sur
  chaque canal et l'écart moyen, entre les deux captures avec l'entité ;
* **sommets** : la boîte des sommets de l'entité (hors ombre et hors plaque de
  nom), l'écart maximal entre les coins des deux boîtes, et la distance moyenne
  de chaque sommet vanilla au sommet le plus proche des nôtres (et
  réciproquement), en blocs.

Usage : scripts/compare_entity_render.py [--only a,b] [--json out.json]
"""

import argparse
import json
import math
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(ROOT, ".scratch", "entity-render")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import entity_scenes  # noqa: E402

THRESHOLD = 24


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(maxsplit=4)
    width, height = int(parts[1]), int(parts[2])
    pixels = parts[4]
    return width, height, pixels, 3


def read_png(path):
    """Through `sips` to an uncompressed BMP: no image library on this machine,
    and a pure-Python inflate of a Retina capture takes minutes."""
    bmp = path + ".bmp"
    if not os.path.exists(bmp) or os.path.getmtime(bmp) < os.path.getmtime(path):
        subprocess.run(["sips", "-s", "format", "bmp", path, "--out", bmp], check=True,
                       capture_output=True)
    with open(bmp, "rb") as f:
        data = f.read()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bits = struct.unpack_from("<H", data, 28)[0]
    channels = bits // 8
    stride = (width * channels + 3) & ~3
    flip = height > 0
    height = abs(height)
    rows = []
    for y in range(height):
        source = (height - 1 - y) if flip else y
        row = data[offset + source * stride: offset + source * stride + width * channels]
        # BGR(A) to RGB.
        rgb = bytearray(width * 3)
        rgb[0::3] = row[2::channels]
        rgb[1::3] = row[1::channels]
        rgb[2::3] = row[0::channels]
        rows.append(bytes(rgb))
    return width, height, b"".join(rows), 3


def mask(full, empty):
    width, height, a, _ = full
    _, _, b, _ = empty
    out = bytearray(width * height)
    for i in range(width * height):
        o = i * 3
        if (abs(a[o] - b[o]) > THRESHOLD or abs(a[o + 1] - b[o + 1]) > THRESHOLD
                or abs(a[o + 2] - b[o + 2]) > THRESHOLD):
            out[i] = 1
    return out


def rows_of(m, width, height):
    top = bottom = None
    for y in range(height):
        if any(m[y * width:(y + 1) * width]):
            top = y if top is None else top
            bottom = y
    return (top, bottom) if top is not None else (0, -1)


def vertices_vanilla(path, scene):
    try:
        entities = json.load(open(path))
    except (OSError, ValueError):
        return None
    best = None
    for entity in entities:
        x, y, z = entity["position"]
        distance = (x - scene.x) ** 2 + (z - scene.z) ** 2
        if best is None or distance < best[0]:
            best = (distance, entity)
    if best is None:
        return None
    points = []
    for layer in best[1]["layers"]:
        # The render type's own name, before its state: every entity render
        # type's string contains "texture[…]", so a filter on the whole string
        # for "text" once threw every vertex away.
        kind = layer["render_type"].split("[", 1)[-1].split(":", 1)[0]
        if "shadow" in kind or kind.startswith("text"):
            continue
        points.extend((v[0], v[1], v[2]) for v in layer["vertices"])
    return points


def vertices_ours(path, scene):
    try:
        records = json.load(open(path))
    except (OSError, ValueError):
        return None
    best = None
    for record in records:
        x, y, z = record["origin"]
        distance = (x - scene.x) ** 2 + (z - scene.z) ** 2
        if best is None or distance < best[0] - 1e-6:
            best = (distance, record["id"])
    if best is None:
        return None
    points = []
    for record in records:
        if record["id"] != best[1] or record["pass"] in ("name", "name_plate"):
            continue
        points.extend((v[0], v[1], v[2]) for v in record["vertices"])
    return points


def box(points):
    lo = [min(p[i] for p in points) for i in range(3)]
    hi = [max(p[i] for p in points) for i in range(3)]
    return lo, hi


def mean_nearest(a, b):
    """Mean distance from each point of a to its nearest in b, on at most 1500
    points of a — the clouds are small, the quadratic search is not."""
    step = max(1, len(a) // 1500)
    total = 0.0
    count = 0
    for p in a[::step]:
        total += min(math.dist(p, q) for q in b)
        count += 1
    return total / max(count, 1)


OURS_VERTICES = "ours"


def compare(scene):
    v_full = os.path.join(WORK, "vanilla", scene.name + ".png")
    v_empty = os.path.join(WORK, "vanilla", scene.name + "_empty.png")
    o_full = os.path.join(WORK, "ours", scene.name + ".ppm")
    o_empty = os.path.join(WORK, "ours", scene.name + "_empty.ppm")
    row = {"scene": scene.name}
    if all(os.path.exists(p) for p in (v_full, v_empty, o_full, o_empty)):
        vf, ve, of, oe = read_png(v_full), read_png(v_empty), read_ppm(o_full), read_ppm(o_empty)
        if vf[:2] != of[:2]:
            row["error"] = "tailles %sx%s contre %sx%s" % (vf[0], vf[1], of[0], of[1])
        else:
            width, height = vf[0], vf[1]
            mv, mo = mask(vf, ve), mask(of, oe)
            inter = sum(1 for i in range(width * height) if mv[i] and mo[i])
            union = sum(1 for i in range(width * height) if mv[i] or mo[i])
            row["iou"] = inter / union if union else float("nan")
            row["vanilla_px"] = sum(mv)
            row["ours_px"] = sum(mo)
            tv, bv = rows_of(mv, width, height)
            to, bo = rows_of(mo, width, height)
            row["height_vanilla"] = bv - tv + 1
            row["height_ours"] = bo - to + 1
            within = 0
            diff = 0
            n = 0
            a, b = vf[2], of[2]
            for i in range(width * height):
                if not mv[i]:
                    continue
                o = i * 3
                d = (abs(a[o] - b[o]), abs(a[o + 1] - b[o + 1]), abs(a[o + 2] - b[o + 2]))
                within += max(d) <= 8
                diff += sum(d)
                n += 1
            row["within8"] = within / n if n else float("nan")
            row["mean_diff"] = diff / (3 * n) if n else float("nan")
    else:
        row["error"] = "captures manquantes"
    pv = vertices_vanilla(os.path.join(WORK, "vanilla", scene.name + ".json"), scene)
    po = vertices_ours(os.path.join(WORK, OURS_VERTICES, scene.name + ".json"), scene)
    if pv and po:
        (vl, vh), (ol, oh) = box(pv), box(po)
        row["box_error"] = max(max(abs(vl[i] - ol[i]), abs(vh[i] - oh[i])) for i in range(3))
        row["vanilla_vertices"] = len(pv)
        row["ours_vertices"] = len(po)
        row["nearest"] = 0.5 * (mean_nearest(pv, po) + mean_nearest(po, pv))
        row["height_ratio"] = (oh[1] - ol[1]) / (vh[1] - vl[1]) if vh[1] > vl[1] else float("nan")
    return row


def fmt(value, pattern):
    if value is None or (isinstance(value, float) and math.isnan(value)):
        return "—"
    return pattern % value


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", default="")
    parser.add_argument("--json", default=os.path.join(WORK, "comparison.json"))
    parser.add_argument("--check", action="store_true",
                        help="nos sommets depuis la vérification hors ligne (ov_voxel --entity-check)")
    args = parser.parse_args()
    global OURS_VERTICES
    if args.check:
        OURS_VERTICES = "check"
    only = [s for s in args.only.split(",") if s]
    rows = []
    print("| espèce | IoU | ±8 dans la silhouette | écart moyen | hauteur px V/N | "
          "sommets V/N | boîte (blocs) | plus proche (blocs) |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|")
    for scene in entity_scenes.SCENES:
        if only and scene.name not in only:
            continue
        row = compare(scene)
        rows.append(row)
        print("| %s | %s | %s | %s | %s | %s | %s | %s |%s" % (
            scene.name, fmt(row.get("iou"), "%.3f"), fmt(row.get("within8"), "%.1f %%" if False else "%.3f"),
            fmt(row.get("mean_diff"), "%.1f"),
            "%s/%s" % (row.get("height_vanilla", "—"), row.get("height_ours", "—")),
            "%s/%s" % (row.get("vanilla_vertices", "—"), row.get("ours_vertices", "—")),
            fmt(row.get("box_error"), "%.4f"), fmt(row.get("nearest"), "%.4f"),
            (" " + row["error"]) if "error" in row else ""))
    with open(args.json, "w") as f:
        json.dump(rows, f, indent=1)
    print("\nécrit %s" % os.path.relpath(args.json, ROOT))


if __name__ == "__main__":
    main()
