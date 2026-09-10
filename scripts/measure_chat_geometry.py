#!/usr/bin/env python3
"""Où le chat se dessine : bandes de pixels modifiés, en pixels d'interface.

Une capture avec le chat est comparée à une capture **du même point de vue
sans rien** (la référence). Tout pixel qui diffère a été dessiné par
l'interface ; les lignes consécutives qui diffèrent forment une bande, et pour
chacune le script donne ses bornes en pixels d'interface et l'assombrissement
médian du fond (image / référence), dont on tire l'alpha d'un fond noir :
`alpha = 1 - rapport` quand le mélange se fait dans l'espace de l'image.

Sert aux deux côtés : les PNG du vrai client (scripts/measure_chat_screen.py)
et les PPM de notre client (`ov_voxel --screenshot`). Aucune dépendance : les
PNG passent par `sips` (macOS) en BMP, lu ici.

Usage:
    scripts/measure_chat_geometry.py --scale 3 REF IMAGE [IMAGE...]
    scripts/measure_chat_geometry.py --scale 3 --region 0,380,340,480 REF IMAGE
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile


def load(path):
    """(largeur, hauteur, octets RGB) d'un PPM P6, d'un BMP, ou d'un PNG via sips."""
    with open(path, "rb") as f:
        head = f.read(2)
    if head == b"P6":
        return load_ppm(path)
    if head == b"BM":
        return load_bmp(path)
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "image.bmp")
        subprocess.run(["sips", "-s", "format", "bmp", path, "--out", out],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return load_bmp(out)


def load_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    pos += 1
    width, height = int(fields[1]), int(fields[2])
    return width, height, bytes(data[pos:pos + width * height * 3])


def load_bmp(path):
    with open(path, "rb") as f:
        data = f.read()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    top_down = height < 0
    height = abs(height)
    step = bpp // 8
    stride = (width * step + 3) & ~3
    out = bytearray(width * height * 3)
    for y in range(height):
        row = y if top_down else height - 1 - y
        src = offset + row * stride
        line = data[src:src + width * step]
        o = y * width * 3
        # BGR(A) -> RGB
        out[o:o + width * 3:3] = line[2::step][:width]
        out[o + 1:o + width * 3:3] = line[1::step][:width]
        out[o + 2:o + width * 3:3] = line[0::step][:width]
    return width, height, bytes(out)


def sky_of(img):
    """Une image de la couleur du ciel, prise au milieu du quart supérieur droit."""
    w, h, px = img
    samples = []
    for y in range(int(h * 0.2), int(h * 0.3), 7):
        for x in range(int(w * 0.7), int(w * 0.8), 7):
            o = (y * w + x) * 3
            samples.append(tuple(px[o:o + 3]))
    samples.sort()
    colour = bytes(samples[len(samples) // 2])
    return w, h, colour * (w * h)


def bands(ref, img, scale, region):
    w, h, a = ref
    w2, h2, b = img
    if (w, h) != (w2, h2):
        raise SystemExit("tailles différentes : %dx%d et %dx%d" % (w, h, w2, h2))
    x0, y0, x1, y1 = [int(v * scale) for v in region] if region else (0, 0, w, h)
    x1, y1 = min(x1, w), min(y1, h)
    rows = []
    for y in range(y0, y1):
        lo, hi, ratios = None, None, []
        base = y * w * 3
        for x in range(x0, x1):
            o = base + x * 3
            ra, ga, ba = a[o], a[o + 1], a[o + 2]
            rb, gb, bb = b[o], b[o + 1], b[o + 2]
            if abs(ra - rb) > 3 or abs(ga - gb) > 3 or abs(ba - bb) > 3:
                lo = x if lo is None else lo
                hi = x
                if rb < ra and gb < ga and bb < ba and ra > 40 and ga > 40 and ba > 40:
                    ratios.append(((rb / ra) + (gb / ga) + (bb / ba)) / 3)
        rows.append((y, lo, hi, ratios))
    out, current = [], None
    for y, lo, hi, ratios in rows:
        if lo is None:
            if current:
                out.append(current)
                current = None
            continue
        if current is None:
            current = [y, y, lo, hi, list(ratios)]
        else:
            current[1] = y
            current[2] = min(current[2], lo)
            current[3] = max(current[3], hi)
            current[4].extend(ratios)
    if current:
        out.append(current)
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--scale", type=float, required=True)
    parser.add_argument("--region", help="x0,y0,x1,y1 en pixels d'interface")
    parser.add_argument("--sky", action="store_true",
                        help="référence = la couleur du ciel de chaque image (capture vers le haut)")
    parser.add_argument("images", nargs="+",
                        help="sans --sky : la référence d'abord, puis les images")
    args = parser.parse_args()
    region = [float(v) for v in args.region.split(",")] if args.region else None
    images = args.images
    ref = None
    if not args.sky:
        ref, images = load(images[0]), images[1:]
    s = args.scale
    for path in images:
        print("== %s" % os.path.basename(path))
        img = load(path)
        for y0, y1, x0, x1, ratios in bands(sky_of(img) if args.sky else ref, img, s, region):
            ratios.sort()
            median = ratios[len(ratios) // 2] if ratios else None
            print("  y %7.2f..%7.2f  x %7.2f..%7.2f  (%d px de haut)%s" % (
                y0 / s, (y1 + 1) / s, x0 / s, (x1 + 1) / s, y1 - y0 + 1,
                "" if median is None else "  assombrissement médian %.3f (alpha ≈ %.3f)"
                % (median, 1 - median)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
