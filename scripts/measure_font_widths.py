#!/usr/bin/env python3
"""Glyph advances of the 1.20.1 default font, from the pixels, twice.

Un caractère n'a pas de largeur déclarée. `font/default.json` dit *où* est un
glyphe, jamais *quelle largeur* il fait : le jeu balaie les pixels du glyphe,
prend la dernière colonne qui n'est pas transparente, ajoute une colonne
d'espacement, et met le tout à l'échelle `height / hauteur_de_cellule`.

Ce script refait ce calcul indépendamment de notre C++ — un lecteur PNG écrit
ici, un parcours des `providers` écrit ici — et compare :

  * notre table (`ov_voxel --font-widths=...`) avec la sienne, glyphe par
    glyphe ;
  * la police du jar vanilla avec celle de `run/assets` (Faithful 32x par
    dessus), pour chiffrer ce que le pack change.

Usage:
    scripts/measure_font_widths.py [--pack run/assets] [--jar <client.jar>]
                                   [--binary build/macos-debug/bin/ov_voxel]

Rien n'est commité : le jar et `run/assets` sont gitignorés, et ce script ne
lit que des fichiers, il n'en écrit aucun hors de /tmp.
"""

import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib


# ── PNG ─────────────────────────────────────────────────────────────────────
#
# Écrit ici plutôt que pris d'une bibliothèque : l'oracle doit être indépendant
# du décodeur que le C++ utilise, sinon les deux partagent le même bug.

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
    alpha = bytearray(width * height)
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
            if colour == 6:
                alpha[y * width + x] = line[x * 4 + 3]
            elif colour == 4:
                alpha[y * width + x] = line[x * 2 + 1]
            elif colour == 3:
                i = line[x]
                alpha[y * width + x] = trns[i] if i < len(trns) else 255
            else:
                alpha[y * width + x] = 255
    return width, height, alpha


# ── Les packs ───────────────────────────────────────────────────────────────

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


def providers(pack, font="minecraft:default", depth=0):
    assert depth < 8, font
    ns, _, path = font.partition(":")
    if not path:
        ns, path = "minecraft", ns
    body = pack.read("assets/%s/font/%s.json" % (ns, path))
    if body is None:
        raise SystemExit("no font file for " + font)
    out = []
    for prov in json.loads(body)["providers"]:
        if prov["type"] == "reference":
            out += providers(pack, prov["id"], depth + 1)
        else:
            out.append(prov)
    return out


def advances(pack):
    """codepoint -> avance en pixels d'interface. Le premier provider gagne."""
    table = {}
    for prov in providers(pack):
        kind = prov["type"]
        if kind == "space":
            for character, adv in prov["advances"].items():
                table.setdefault(ord(character), adv)
        elif kind == "bitmap":
            ns, _, path = prov["file"].partition(":")
            if not path:
                ns, path = "minecraft", ns
            body = pack.read("assets/%s/textures/%s" % (ns, path))
            if body is None:
                raise SystemExit("missing font page " + prov["file"])
            w, h, alpha = read_png(body)
            rows = prov["chars"]
            height = prov.get("height", 8)
            cols = len(rows[0])
            cw, ch = w // cols, h // len(rows)
            scale = height / ch
            for r, row in enumerate(rows):
                for c, character in enumerate(row):
                    cp = ord(character)
                    if cp == 0 or cp in table:
                        continue
                    m = 0
                    for x in range(cw - 1, -1, -1):
                        if any(alpha[(r * ch + y) * w + c * cw + x] for y in range(ch)):
                            m = x + 1
                            break
                    # (int)(0.5 + m * scale) + 1 : l'arrondi du jeu, plus la
                    # colonne d'espacement.
                    table[cp] = int(0.5 + m * scale) + 1
        else:
            raise SystemExit("provider type not handled by this oracle: " + kind)
    return table


def compare(name_a, a, name_b, b, show=8):
    only_a = sorted(set(a) - set(b))
    only_b = sorted(set(b) - set(a))
    both = sorted(set(a) & set(b))
    differ = [cp for cp in both if a[cp] != b[cp]]
    print("  %s: %d glyphes · %s: %d glyphes · communs: %d"
          % (name_a, len(a), name_b, len(b), len(both)))
    if only_a:
        print("    seulement dans %s: %d" % (name_a, len(only_a)))
    if only_b:
        print("    seulement dans %s: %d" % (name_b, len(only_b)))
    print("    désaccords: %d (%.4f %%)"
          % (len(differ), 100.0 * len(differ) / max(1, len(both))))
    for cp in differ[:show]:
        print("      U+%04X  %s=%d  %s=%d" % (cp, name_a, a[cp], name_b, b[cp]))
    if len(differ) > show:
        print("      … et %d autres" % (len(differ) - show))
    return len(differ)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pack", default="run/assets")
    parser.add_argument("--jar", default=None,
                        help="le client jar 1.20.1, pour comparer au pack")
    parser.add_argument("--binary", default="build/macos-debug/bin/ov_voxel")
    args = parser.parse_args()

    pack = DirectoryPack(args.pack)
    print("→ %s" % args.pack)
    ours_oracle = advances(pack)
    print("  %d glyphes lus dans les pixels" % len(ours_oracle))

    failures = 0

    # 1. Notre C++ contre cet oracle, glyphe par glyphe.
    if os.path.exists(args.binary):
        with tempfile.NamedTemporaryFile(suffix=".tsv", delete=False) as tmp:
            dump = tmp.name
        subprocess.run([args.binary, "--assets=" + args.pack, "--font-widths=" + dump],
                       check=True, stdout=subprocess.DEVNULL)
        ours = {}
        with open(dump) as f:
            for line in f:
                cp, adv = line.split()
                ours[int(cp)] = int(adv)
        os.unlink(dump)
        print("\n→ notre table contre l'oracle du même pack")
        failures += compare("C++", ours, "oracle", ours_oracle)
    else:
        print("\n  (%s absent : la comparaison C++ est sautée)" % args.binary)

    # 2. Le jar vanilla contre le pack, pour chiffrer ce que Faithful change.
    if args.jar and os.path.exists(args.jar):
        print("\n→ le jar vanilla contre %s" % args.pack)
        vanilla = advances(JarPack(args.jar))
        compare("vanilla", vanilla, "pack", ours_oracle)

    if failures:
        print("\nDÉSACCORD : notre table et l'oracle ne disent pas la même chose.")
        return 1
    print("\nD'accord, glyphe par glyphe.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
