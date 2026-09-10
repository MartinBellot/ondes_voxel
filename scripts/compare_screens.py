#!/usr/bin/env python3
"""Nos menus contre ceux du vrai client 1.20.1, widget par widget.

Les deux captures sont prises à 2560×1440 et à l'échelle d'interface 3
(854×480 pixels d'interface). Les rectangles comparés sont ceux que le vrai
client a rapportés pour l'écran (`facts.txt` de scripts/measure_screens.py,
bloc `── screen <label>`) : chaque bouton, curseur et champ. Ce qui est
derrière — panorama, monde assombri, terre — n'est pas comparé : ce n'est pas
le même monde, ni la même minute du panorama.

Pour chaque widget : la part de pixels identiques, la part à ±8 niveaux près.
Un widget absent de notre capture (rectangle décalé) se voit tout de suite :
sa part tombe au niveau du fond.

Usage:
    scripts/compare_screens.py --facts <facts.txt> --label "pause" <nôtre.ppm|png> <vanilla.png>
"""

import argparse
import re
import struct
import sys
import zlib

SCALE = 3


def load_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos, idat = 8, b""
    while pos < len(data):
        n, = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, _, ctype = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    bpp = {2: 3, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[i]
        i += 1
        line = bytearray(raw[i:i + stride])
        i += stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line[x] for x in range(stride) if x % bpp < 3))
        prev = line
    return w, h, rows


def load_ppm(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    assert parts[0] == b"P6", path
    w, h = map(int, parts[1].split())
    body = parts[3]
    return w, h, [body[y * w * 3:(y + 1) * w * 3] for y in range(h)]


def load(path):
    return load_ppm(path) if path.endswith(".ppm") else load_png(path)


def widgets(facts, label):
    """The widget rectangles of the block `── screen <label>:`."""
    out, inside = [], False
    for line in open(facts, encoding="utf-8"):
        if line.startswith("── screen "):
            inside = line[len("── screen "):].startswith(label + ":")
            continue
        if line.startswith("──"):
            inside = False
        if not inside:
            continue
        m = re.match(r"\s+(\S+) (-?\d+),(-?\d+) (\d+)x(\d+) active (\w+) visible (\w+) text (\".*?\")", line)
        if m and m.group(7) == "true":
            out.append((m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)),
                        int(m.group(5)), m.group(8)))
    return out


def compare(a, b, x, y, w, h):
    same = near = total = 0
    for yy in range(y * SCALE, (y + h) * SCALE):
        ra, rb = a[yy], b[yy]
        for xx in range(x * SCALE, (x + w) * SCALE):
            pa, pb = ra[xx * 3:xx * 3 + 3], rb[xx * 3:xx * 3 + 3]
            d = max(abs(pa[i] - pb[i]) for i in range(3))
            total += 1
            same += d == 0
            near += d <= 8
    return same / total, near / total


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--facts", required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("ours")
    parser.add_argument("vanilla")
    args = parser.parse_args()
    wa, ha, ours = load(args.ours)
    wb, hb, theirs = load(args.vanilla)
    if (wa, ha) != (wb, hb):
        sys.exit("tailles différentes : %dx%d contre %dx%d" % (wa, ha, wb, hb))
    rects = widgets(args.facts, args.label)
    if not rects:
        sys.exit("aucun widget pour « %s » dans %s" % (args.label, args.facts))
    all_same = all_near = 0.0
    for kind, x, y, w, h, text in rects:
        same, near = compare(ours, theirs, x, y, w, h)
        all_same += same
        all_near += near
        print("%-22s %4d,%-4d %3dx%-3d identique %5.1f %%  ±8 %5.1f %%  %s"
              % (kind, x, y, w, h, 100 * same, 100 * near, text))
    print("moyenne sur %d widgets : identique %.1f %%, ±8 %.1f %%"
          % (len(rects), 100 * all_same / len(rects), 100 * all_near / len(rects)))


if __name__ == "__main__":
    main()
