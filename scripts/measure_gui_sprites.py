#!/usr/bin/env python3
"""Où sont les sprites de l'interface, lus dans les textures du pack.

Les coordonnées d'un cœur, d'une haunche, d'une bulle ou d'un emplacement de
coffre ne sont écrites nulle part : ce sont des faits sur les pixels de
`icons.png`, `widgets.png` et `gui/container/*.png`. Une disposition écrite de
mémoire est juste *presque* juste — et « presque » veut dire des cœurs
empoisonnés là où on attend des cœurs dorés, ou un clic qui tombe un
emplacement à côté.

Ce script les mesure :

  * `icons.png` est balayé en cellules de 9×9 à partir de (16, 0) ; pour chacune
    on donne le nombre de texels opaques et leur couleur moyenne. C'est ce qui
    distingue les cinq familles de cœurs, qui ont toutes la même forme.
  * les fonds de conteneur sont balayés à la recherche des carrés 16×16 du gris
    d'emplacement (#8B8B8B) : chaque carré trouvé est un slot, et leur nombre
    doit être celui que le protocole annonce (46 pour la fenêtre 0).

Usage:  scripts/measure_gui_sprites.py [--pack run/assets] [--jar <client.jar>]

Rien n'est écrit, rien n'est commité : les textures sont gitignorées.
"""

import argparse
import os
import struct
import sys
import zipfile
import zlib


def read_png(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, palette, trns = 8, b"", b"", b""
    width = height = depth = colour = 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
        elif kind == b"PLTE":
            palette = body
        elif kind == b"tRNS":
            trns = body
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    assert depth == 8, depth
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
    raw = zlib.decompress(idat)
    stride = width * channels
    out = bytearray(width * height * 4)
    prev = bytearray(stride)
    p = 0
    for y in range(height):
        filt = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if filt == 1:
                line[i] = (line[i] + a) & 0xFF
            elif filt == 2:
                line[i] = (line[i] + b) & 0xFF
            elif filt == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif filt == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        prev = line
        for x in range(width):
            o = (y * width + x) * 4
            if colour == 6:
                out[o:o + 4] = line[x * 4:x * 4 + 4]
            elif colour == 2:
                out[o:o + 3] = line[x * 3:x * 3 + 3]
                out[o + 3] = 255
            elif colour == 4:
                g, al = line[x * 2], line[x * 2 + 1]
                out[o:o + 4] = bytes((g, g, g, al))
            elif colour == 0:
                g = line[x]
                out[o:o + 4] = bytes((g, g, g, 255))
            elif colour == 3:
                i = line[x]
                out[o:o + 3] = palette[i * 3:i * 3 + 3]
                out[o + 3] = trns[i] if i < len(trns) else 255
    return width, height, out


class DirectoryPack:
    def __init__(self, root):
        self.root = root

    def read(self, path):
        full = os.path.join(self.root, path)
        if not os.path.exists(full):
            return None
        with open(full, "rb") as f:
            return f.read()


class JarPack:
    def __init__(self, path):
        self.zip = zipfile.ZipFile(path)
        self.names = set(self.zip.namelist())

    def read(self, path):
        return self.zip.read(path) if path in self.names else None


def load(pack, name):
    body = pack.read("assets/minecraft/textures/gui/" + name)
    if body is None:
        raise SystemExit("missing " + name)
    return read_png(body)


def scale_of(width, logical):
    """Faithful 32x livre ces feuilles à 512 ; la disposition reste en 256."""
    assert width % logical == 0, (width, logical)
    return width // logical


def cell_report(px, w, scale, x, y, size=9):
    n = r = g = b = 0
    for dy in range(size * scale):
        for dx in range(size * scale):
            o = ((y * scale + dy) * w + x * scale + dx) * 4
            if px[o + 3] > 0:
                n += 1
                r += px[o]
                g += px[o + 1]
                b += px[o + 2]
    if n == 0:
        return 0, None
    return n, (r // n, g // n, b // n)


def report_icons(pack):
    w, h, px = load(pack, "icons.png")
    scale = scale_of(w, 256)
    print("icons.png %dx%d (échelle %dx)" % (w, h, scale))
    rows = [(0, "cœurs"), (9, "armure"), (18, "bulles"), (27, "haunches")]
    for y, label in rows:
        line = []
        for k in range(0, 24):
            x = 16 + 9 * k
            if x + 9 > 256:
                break
            n, colour = cell_report(px, w, scale, x, y)
            if n:
                line.append("%3d:%d/#%02X%02X%02X" % (x, n // (scale * scale), *colour))
        print("  y=%-3d %-9s %s" % (y, label, "  ".join(line)))
    # La barre d'expérience : deux bandes de 182x5, l'une sombre et l'autre verte.
    for y in (64, 69):
        n = r = g = b = 0
        for dy in range(5 * scale):
            for dx in range(182 * scale):
                o = ((y * scale + dy) * w + dx) * 4
                if px[o + 3] > 0:
                    n += 1
                    r += px[o]
                    g += px[o + 1]
                    b += px[o + 2]
        print("  barre xp y=%d : %d texels, moyenne #%02X%02X%02X"
              % (y, n // (scale * scale), r // n, g // n, b // n))


def report_widgets(pack):
    w, h, px = load(pack, "widgets.png")
    scale = scale_of(w, 256)
    print("widgets.png %dx%d (échelle %dx)" % (w, h, scale))
    # La barre d'action : 182x22 en (0,0), la sélection 24x24 en (0,22).
    for name, x, y, bw, bh in (("hotbar", 0, 0, 182, 22),
                               ("selection", 0, 22, 24, 24),
                               ("main gauche", 24, 22, 29, 24)):
        n = 0
        for dy in range(bh * scale):
            for dx in range(bw * scale):
                o = ((y * scale + dy) * w + x * scale + dx) * 4
                if px[o + 3] > 0:
                    n += 1
        print("  %-12s (%d,%d) %dx%d : %d texels opaques"
              % (name, x, y, bw, bh, n // (scale * scale)))


SLOT_GREY = (0x8B, 0x8B, 0x8B, 255)


def slot_boxes(px, w, scale, width, height):
    """Le coin de chaque carré 16×16 du gris d'emplacement."""
    def at(x, y):
        o = ((y * scale) * w + x * scale) * 4
        return tuple(px[o:o + 4])

    found = []
    for y in range(height):
        for x in range(width):
            if at(x, y) != SLOT_GREY:
                continue
            if x > 0 and at(x - 1, y) == SLOT_GREY:
                continue
            if y > 0 and at(x, y - 1) == SLOT_GREY:
                continue
            if x + 16 > width or y + 16 > height:
                continue
            if all(at(x + dx, y + dy) == SLOT_GREY for dx in (0, 15) for dy in (0, 15)):
                found.append((x, y))
    return found


def report_container(pack, name, width, height, expected=None):
    w, h, px = load(pack, "container/" + name)
    scale = scale_of(w, 256)
    boxes = slot_boxes(px, w, scale, width, height)
    verdict = ""
    if expected is not None:
        verdict = "  ✓" if len(boxes) == expected else "  ✗ attendu %d" % expected
    print("container/%s %dx%d (échelle %dx) : %d emplacements%s"
          % (name, w, h, scale, len(boxes), verdict))
    for i in range(0, len(boxes), 9):
        print("    " + " ".join("(%3d,%3d)" % b for b in boxes[i:i + 9]))
    return boxes


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pack", default="run/assets")
    parser.add_argument("--jar", default=None)
    args = parser.parse_args()

    for label, pack in [(args.pack, DirectoryPack(args.pack))] + (
            [(args.jar, JarPack(args.jar))] if args.jar and os.path.exists(args.jar) else []):
        print("=" * 72)
        print(label)
        print("=" * 72)
        report_icons(pack)
        print()
        report_widgets(pack)
        print()
        # 46 emplacements dans la fenêtre 0 : c'est ce que le protocole
        # transporte, et c'est ce que la texture dessine. Les deux d'accord,
        # c'est ce qui dit que la disposition et le protocole parlent de la même
        # fenêtre.
        report_container(pack, "inventory.png", 176, 166, expected=46)
        report_container(pack, "generic_54.png", 176, 222, expected=54 + 36)
        report_container(pack, "crafting_table.png", 176, 166, expected=9 + 1 + 36)
        report_container(pack, "furnace.png", 176, 166, expected=3 + 36)
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
