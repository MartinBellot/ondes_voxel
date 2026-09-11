#!/usr/bin/env python3
"""Compare our captures of the scenes with the real client's, pixel by pixel.

For each scene of scripts/render_parity_scenes.txt:

* **identique** — share of pixels with all three channels equal;
* **±8** — share with every channel within 8 levels;
* **écart moyen** — mean absolute difference per channel, 0..255;
* two **witnesses**, which must do clearly worse than a faithful capture: the
  vanilla capture against *itself shifted by one pixel*, and against the
  vanilla capture of *another scene*. If our capture does no better than the
  shifted one, the number measures nothing (piège 14 of the briefing).

A difference map is written next to our capture (`<scene>-diff.png`): black
where equal, brighter with the error, red where it exceeds 8.

Our client writes PPM (P6), the real one PNG; both are read with the
standard library only, like the rest of scripts/.

Usage:
    scripts/compare_render_parity.py [--tag=before] [--region=x0,y0,x1,y1]
"""

import argparse
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OURS = os.path.join(ROOT, "run", "render-parity")
VANILLA = os.path.join(ROOT, "data", "vanilla", "1.20.1", "generated", "render-parity", "client",
                       "screenshots")
SCENES = os.path.join(ROOT, "scripts", "render_parity_scenes.txt")


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos = 8
    width = height = 0
    colour = depth = 0
    idat = b""
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    assert depth == 8 and colour in (2, 6), (path, depth, colour)
    channels = 3 if colour == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * channels
    out = bytearray(width * height * 3)
    prev = bytearray(stride)
    i = 0
    for y in range(height):
        kind = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b = prev[x]
            c = prev[x - channels] if x >= channels else 0
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        for x in range(width):
            out[(y * width + x) * 3:(y * width + x) * 3 + 3] = line[x * channels:x * channels + 3]
        prev = line
    return width, height, bytes(out)


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = []
    pos = 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos)
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end])
        pos = end
    assert parts[0] == b"P6", path
    width, height = int(parts[1]), int(parts[2])
    pos += 1
    return width, height, data[pos:pos + width * height * 3]


def write_png(path, width, height, rgb):
    raw = b"".join(b"\x00" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def resample(img, width, height):
    """Nearest-neighbour to (width, height): a retina framebuffer is 2x the window."""
    w, h, rgb = img
    if (w, h) == (width, height):
        return img
    out = bytearray(width * height * 3)
    for y in range(height):
        sy = y * h // height
        for x in range(width):
            sx = x * w // width
            out[(y * width + x) * 3:(y * width + x) * 3 + 3] = rgb[(sy * w + sx) * 3:(sy * w + sx) * 3 + 3]
    return width, height, bytes(out)


def compare(a, b, region=None, diff_path=None):
    w, h, pa = a
    _, _, pb = b
    x0, y0, x1, y1 = region or (0, 0, w, h)
    same = near = total = 0
    err = 0
    diff = bytearray(w * h * 3) if diff_path else None
    for y in range(y0, y1):
        base = y * w
        for x in range(x0, x1):
            i = (base + x) * 3
            d0 = abs(pa[i] - pb[i])
            d1 = abs(pa[i + 1] - pb[i + 1])
            d2 = abs(pa[i + 2] - pb[i + 2])
            m = max(d0, d1, d2)
            total += 1
            err += d0 + d1 + d2
            if m == 0:
                same += 1
            if m <= 8:
                near += 1
            if diff is not None:
                if m > 8:
                    diff[i:i + 3] = bytes((255, min(255, m), 0))
                else:
                    v = m * 16
                    diff[i:i + 3] = bytes((v, v, v))
    if diff is not None:
        write_png(diff_path, w, h, bytes(diff))
    return 100.0 * same / total, 100.0 * near / total, err / (3.0 * total)


def shifted(img, dx):
    w, h, p = img
    out = bytearray(p)
    for y in range(h):
        row = y * w * 3
        out[row + dx * 3:row + w * 3] = p[row:row + (w - dx) * 3]
    return w, h, bytes(out)


def scene_names():
    names = []
    with open(SCENES) as f:
        for line in f:
            p = line.split()
            if p and p[0] == "scene":
                names.append(p[1])
    return names


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", default="before")
    parser.add_argument("--region", default="")
    parser.add_argument("--only", default="")
    parser.add_argument("--vanilla", default="",
                        help="un dossier de captures vanilla gardé (le suivant écrase le dossier)")
    args = parser.parse_args()
    global VANILLA
    if args.vanilla:
        VANILLA = os.path.abspath(args.vanilla)
    region = tuple(map(int, args.region.split(","))) if args.region else None
    names = [n for n in scene_names() if not args.only or n in args.only.split(",")]
    vanilla = {}
    for n in scene_names():
        p = os.path.join(VANILLA, n + ".png")
        if os.path.exists(p):
            vanilla[n] = read_png(p)
    print("%-14s %9s %9s %8s | %s" % ("scène", "identique", "±8", "écart", "témoins : décalé 1 px / autre scène (±8)"))
    for n in names:
        ours_path = os.path.join(OURS, "ours-" + args.tag, n + ".ppm")
        if n not in vanilla or not os.path.exists(ours_path):
            print("%-14s  (capture manquante)" % n)
            continue
        v = vanilla[n]
        o = resample(read_ppm(ours_path), v[0], v[1])
        s, near, e = compare(o, v, region, os.path.join(OURS, "ours-" + args.tag, n + "-diff.png"))
        _, wshift, _ = compare(shifted(v, 1), v, region)
        other = next((m for m in names if m != n and m in vanilla), None)
        wother = compare(vanilla[other], v, region)[1] if other else float("nan")
        print("%-14s %8.2f%% %8.2f%% %8.2f | %6.2f%% / %6.2f%%" % (n, s, near, e, wshift, wother))
    return 0


if __name__ == "__main__":
    sys.exit(main())
