#!/usr/bin/env python3
"""Le HUD de notre client contre celui du vrai client 1.20.1, au pixel.

Chaque scène de scripts/hud_scenes.txt est capturée deux fois par chaque
client (scripts/measure_hud.py) : avec l'interface, et sans (F1). La
différence entre les deux est l'**encre** de l'interface — exactement, quel que
soit le fond (ciel, eau, neige poudreuse), et sans rien deviner.

    compare_hud.py compare <vanilla> <ours> [--shot 04-hurt]
        par scène et par zone du HUD (cœurs, faim, armure, bulles, xp, barre
        d'action, effets, barres de boss, titre, barre d'action, tab, F3) :
          * IoU des deux masques d'encre ;
          * part des pixels d'encre (union) de même couleur, exacte et à ±8.
    compare_hud.py grid <shot.png> [--region x,y,w,h]
        la zone en pixels d'interface (échelle 3), un caractère par pixel :
        '.' pas d'encre, sinon une lettre par couleur, et la palette.
    compare_hud.py boxes <shot.png>
        les boîtes englobantes des taches d'encre, en pixels d'interface.

Les captures sont à 2560×1440, échelle d'interface 3 : un pixel d'interface est
un carré de 3×3 pixels, et l'interface fait ceil(2560/3) = 854 × 480.

Aucune dépendance : les PNG sont convertis en BMP par `sips` (macOS), dans
.scratch/bmp, et lus par tranches ; nos PPM sont lus directement.
"""

import argparse
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, ".scratch", "bmp")
SCALE = 3
GUI_W, GUI_H = 854, 480

# Zones, en pixels d'interface, pour 854×480. cx = 427.
REGIONS = {
    "cœurs":        (334, 400, 94, 44),    # 336..417, rangées au-dessus de h-39
    "faim":         (434, 420, 86, 22),
    "air":          (434, 400, 86, 20),
    "xp":           (334, 440, 186, 18),   # barre h-29, niveau h-35
    "barre":        (330, 454, 196, 26),   # la barre d'action (hotbar)
    "effets":       (600, 0, 254, 60),
    "boss":         (240, 0, 374, 110),
    "titre":        (150, 180, 554, 110),
    "action":       (200, 400, 454, 20),
    "tab":          (200, 0, 454, 120),
    "f3":           (0, 0, 854, 200),
    # A container window: 176 wide, centred; 166 high (133 for the hopper).
    "screen":       (339, 157, 176, 166),
    "trémie":       (339, 173, 176, 133),
}

SHOT_REGIONS = {
    "01-plain": ["cœurs", "faim", "xp", "barre"],
    "02-xp": ["cœurs", "faim", "xp"],
    "03-hurt-blink": ["cœurs"],
    "04-hurt": ["cœurs"],
    "05-armour": ["cœurs", "barre"],
    "06-absorption": ["cœurs"],
    "07-poison": ["cœurs"],
    "08-wither": ["cœurs"],
    "09-hunger": ["faim"],
    "10-regen-a": ["cœurs"], "10-regen-b": ["cœurs"], "10-regen-c": ["cœurs"], "10-regen-d": ["cœurs"],
    "11-effects": ["effets"], "11-effects-b": ["effets"], "11-effects-c": ["effets"],
    "11-effects-d": ["effets"],
    "12-frozen": ["cœurs"],
    "13-air": ["air"],
    "14-mount": ["faim", "xp"],
    "15-bossbars": ["boss"],
    "16-title-in": ["titre"], "16-title-stay": ["titre"], "16-title-out": ["titre"],
    "17-actionbar": ["action"], "17-actionbar-fade": ["action"],
    "18-tab": ["tab"], "18-tab-score": ["tab"], "18-tab-header": ["tab"], "18-tab-spectator": ["tab"],
    "19-f3": ["f3"],
    "20-low-a": ["cœurs"],
    "14-mount-pig": ["faim", "xp"],
    "30-hopper": ["trémie"], "31-dispenser": ["screen"], "32-shulker": ["screen"],
    "33-anvil": ["screen"], "34-grindstone": ["screen"], "35-enchanting": ["screen"],
    "36-brewing": ["screen"], "37-crafting": ["screen"], "38-chest": ["screen"],
    "39-horse-inventory": ["screen"], "40-donkey-inventory": ["screen"],
    "41-llama-inventory": ["screen"],
    "h1-hurt": ["cœurs"], "h2-absorption": ["cœurs"], "h3-poison": ["cœurs"],
}


class Image:
    def __init__(self, path):
        self.path = path
        if path.endswith(".ppm"):
            self._ppm(path)
        else:
            bmp = self._convert(path)
            try:
                self._bmp(bmp)
            finally:
                os.remove(bmp)  # 14.7 MB each: read, then gone (the disk is shared)

    @staticmethod
    def _convert(path):
        os.makedirs(CACHE, exist_ok=True)
        key = re.sub(r"[^A-Za-z0-9._-]", "_", os.path.relpath(os.path.abspath(path), ROOT))
        out = os.path.join(CACHE, key + ".bmp")
        if os.path.getsize(path) == 0:
            raise SystemExit("capture vide (écriture interrompue) : %s" % path)
        subprocess.run(["sips", "-s", "format", "bmp", path, "--out", out], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not os.path.exists(out):
            raise SystemExit("sips n'a rien écrit pour %s" % path)
        return out

    def _ppm(self, path):
        data = open(path, "rb").read()
        parts = data.split(b"\n", 3)
        assert parts[0] == b"P6", path
        self.w, self.h = map(int, parts[1].split())
        self.data = parts[3]
        self.stride = self.w * 3
        self.bpp = 3
        self.order = (0, 1, 2)
        self.top_down = True

    def _bmp(self, path):
        data = open(path, "rb").read()
        offset, = struct.unpack("<I", data[10:14])
        _, w, h, _, bpp = struct.unpack("<IiiHH", data[14:30])
        compression, = struct.unpack("<I", data[30:34])
        self.w, self.h = w, abs(h)
        self.bpp = bpp // 8
        self.top_down = h < 0
        self.stride = ((self.w * bpp + 31) // 32) * 4
        self.data = data[offset:]
        if compression == 3:
            masks = struct.unpack("<III", data[54:66])
            self.order = tuple({0xFF: 0, 0xFF00: 1, 0xFF0000: 2, 0xFF000000: 3}[m] for m in masks)
        else:
            self.order = (2, 1, 0)

    def px(self, x, y):
        row = y if self.top_down else self.h - 1 - y
        i = row * self.stride + x * self.bpp
        d = self.data
        o = self.order
        return (d[i + o[0]], d[i + o[1]], d[i + o[2]])


def nohud_of(path):
    base, ext = os.path.splitext(path)
    return base + "-nohud" + ext


def ink_mask(hud, bare, region, fine=False):
    """Pixels (fb, or GUI centres) where the HUD changed the image."""
    x0, y0, w, h = region
    out = {}
    if fine:
        coords = ((x, y) for y in range(y0 * SCALE, (y0 + h) * SCALE)
                  for x in range(x0 * SCALE, (x0 + w) * SCALE))
    else:
        coords = ((x * SCALE + 1, y * SCALE + 1) for y in range(y0, y0 + h) for x in range(x0, x0 + w))
    for x, y in coords:
        if x >= hud.w or y >= hud.h:
            continue
        a, b = hud.px(x, y), bare.px(x, y)
        if max(abs(a[i] - b[i]) for i in range(3)) > 6:
            out[(x, y)] = a
    return out


def cmd_grid(args):
    hud = Image(args.shot)
    bare = Image(nohud_of(args.shot))
    x0, y0, w, h = map(int, args.region.split(","))
    ink = ink_mask(hud, bare, (x0, y0, w, h))
    palette, letters = {}, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
    print("     " + "".join(str((x0 + i) // 10 % 10) if (x0 + i) % 10 == 0 else " " for i in range(w)))
    for y in range(y0, y0 + h):
        line = []
        for x in range(x0, x0 + w):
            c = ink.get((x * SCALE + 1, y * SCALE + 1))
            if c is None:
                line.append(".")
                continue
            key = tuple(v >> 3 for v in c) if args.coarse else c
            if key not in palette:
                palette[key] = (letters[len(palette) % len(letters)], c)
            line.append(palette[key][0])
        print("%4d %s" % (y, "".join(line)))
    for key, (letter, c) in palette.items():
        print("  %s #%02X%02X%02X" % (letter, *c))


def cmd_boxes(args):
    hud = Image(args.shot)
    bare = Image(nohud_of(args.shot))
    ink = ink_mask(hud, bare, (0, 0, GUI_W, GUI_H))
    cells = {(x // SCALE, y // SCALE) for (x, y) in ink}
    seen, boxes = set(), []
    for cell in sorted(cells):
        if cell in seen:
            continue
        stack, xs, ys = [cell], [], []
        seen.add(cell)
        while stack:
            cx, cy = stack.pop()
            xs.append(cx)
            ys.append(cy)
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    n = (cx + dx, cy + dy)
                    if n in cells and n not in seen:
                        seen.add(n)
                        stack.append(n)
        boxes.append((min(ys), min(xs), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1, len(xs)))
    for y, x, w, h, n in sorted(boxes):
        if n >= args.min:
            print("  x %3d y %3d  %3dx%-3d  %d px" % (x, y, w, h, n))


def compare_region(v, vb, o, ob, region):
    vm = ink_mask(v, vb, region, fine=True)
    om = ink_mask(o, ob, region, fine=True)
    union = set(vm) | set(om)
    inter = set(vm) & set(om)
    if not union:
        return None
    same = near = 0
    for p in union:
        a, b = v.px(*p), o.px(*p)
        d = max(abs(a[i] - b[i]) for i in range(3))
        same += d == 0
        near += d <= 8
    return (len(vm), len(om), len(inter) / len(union), same / len(union), near / len(union))


def cmd_compare(args):
    vdir = os.path.join(args.vanilla, "screenshots")
    odir = os.path.join(args.ours, "screenshots")
    shots = [args.shot] if args.shot else list(SHOT_REGIONS)
    total = []
    for shot in shots:
        vpath = os.path.join(vdir, shot + ".png")
        opath = os.path.join(odir, shot + ".png")
        if not os.path.exists(opath):
            opath = os.path.join(odir, shot + ".ppm")
        if not os.path.exists(vpath) or not os.path.exists(opath):
            print("%-20s absente (%s)" % (shot, "vanilla" if not os.path.exists(vpath) else "nôtre"))
            continue
        v, vb = Image(vpath), Image(nohud_of(vpath))
        o, ob = Image(opath), Image(nohud_of(opath))
        for name in (args.region.split(",") if args.region else SHOT_REGIONS[shot]):
            r = compare_region(v, vb, o, ob, REGIONS[name])
            if r is None:
                print("%-20s %-8s aucune encre" % (shot, name))
                continue
            nv, no, iou, same, near = r
            total.append(near)
            print("%-20s %-8s encre %6d / %6d  IoU %5.1f %%  identique %5.1f %%  ±8 %5.1f %%"
                  % (shot, name, nv, no, 100 * iou, 100 * same, 100 * near))
    if total:
        print("moyenne ±8 sur %d zones : %.1f %%" % (len(total), 100 * sum(total) / len(total)))


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("grid")
    g.add_argument("shot")
    g.add_argument("--region", default="334,400,190,80")
    g.add_argument("--coarse", action="store_true")
    b = sub.add_parser("boxes")
    b.add_argument("shot")
    b.add_argument("--min", type=int, default=1)
    c = sub.add_parser("compare")
    c.add_argument("vanilla")
    c.add_argument("ours")
    c.add_argument("--shot")
    c.add_argument("--region")
    args = parser.parse_args()
    {"grid": cmd_grid, "boxes": cmd_boxes, "compare": cmd_compare}[args.cmd](args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
