#!/usr/bin/env python3
"""Notre écran créatif contre celui du vrai client 1.20.1, pixel par pixel.

Les deux captures sont prises à la même taille (2560×1440) et à la même
échelle d'interface (3) : le panneau est à (329, 172) en pixels d'interface
dans les deux. On ne compare que ce que les deux clients dessinent de la même
source — le panneau et chaque bouton d'onglet — et pas ce qui est derrière
(le monde, assombri, n'est pas le même monde).

Pour chaque région : la part de pixels identiques, la part à ±8 niveaux près,
et l'écart maximal. Une image de différence est écrite à côté.

Usage:
    scripts/compare_creative_screen.py <nos captures> <captures vanilla> [noms…]
"""

import os
import struct
import sys
import zlib

SCALE = 3
PANEL = (329, 172)


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
        rows.append(bytes(line))
        prev = line
    return w, h, bpp, rows


def pixel(img, x, y):
    _, _, bpp, rows = img
    return rows[y][x * bpp:x * bpp + 3]


# Regions in GUI pixels relative to the panel: the panel, and the body of each
# button (inset two pixels, clear of the rounded corners through which the
# world shows).
def regions():
    out = [("panneau", 0, 0, 195, 136)]
    for col in range(7):
        for row, y in (("haut", -26), ("bas", 138)):
            x = 27 * col if col < 5 else 195 - 27 * (7 - col) + 1
            out.append(("onglet %s %d" % (row, col), x + 2, y, 22, 20))
    return out


def compare(ours, theirs):
    report = []
    for name, gx, gy, gw, gh in regions():
        same = near = total = 0
        worst = 0
        for y in range((PANEL[1] + gy) * SCALE, (PANEL[1] + gy + gh) * SCALE):
            for x in range((PANEL[0] + gx) * SCALE, (PANEL[0] + gx + gw) * SCALE):
                a, b = pixel(ours, x, y), pixel(theirs, x, y)
                d = max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2]))
                total += 1
                same += d == 0
                near += d <= 8
                worst = max(worst, d)
        report.append((name, same / total, near / total, worst))
    return report


def main():
    ours_dir, theirs_dir = sys.argv[1], sys.argv[2]
    names = sys.argv[3:] or sorted(n[:-4] for n in os.listdir(ours_dir) if n.endswith(".png"))
    for name in names:
        a = os.path.join(ours_dir, name + ".png")
        b = os.path.join(theirs_dir, name + ".png")
        if not (os.path.exists(a) and os.path.exists(b)):
            print("%-26s  (une des deux captures manque)" % name)
            continue
        report = compare(load_png(a), load_png(b))
        panel = report[0]
        tabs = report[1:]
        tabs_same = sum(r[1] for r in tabs) / len(tabs)
        tabs_near = sum(r[2] for r in tabs) / len(tabs)
        print("%-26s  panneau %5.1f %% identiques, %5.1f %% à ±8, écart max %3d   "
              "onglets %5.1f %% / %5.1f %%"
              % (name, 100 * panel[1], 100 * panel[2], panel[3], 100 * tabs_same, 100 * tabs_near))
        for r in tabs:
            if r[2] < 0.95:
                print("      %-14s %5.1f %% identiques, %5.1f %% à ±8, écart max %3d"
                      % (r[0], 100 * r[1], 100 * r[2], r[3]))


if __name__ == "__main__":
    main()
