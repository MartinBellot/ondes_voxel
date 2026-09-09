#!/usr/bin/env python3
"""Lecteur NBT et Anvil minimal, juste assez pour lire un chunk de région."""
import struct
import zlib
import gzip


class R:
    def __init__(self, b):
        self.b = b
        self.i = 0

    def take(self, n):
        v = self.b[self.i:self.i + n]
        self.i += n
        return v

    def u1(self):
        return self.take(1)[0]

    def i1(self):
        return struct.unpack(">b", self.take(1))[0]

    def i2(self):
        return struct.unpack(">h", self.take(2))[0]

    def u2(self):
        return struct.unpack(">H", self.take(2))[0]

    def i4(self):
        return struct.unpack(">i", self.take(4))[0]

    def i8(self):
        return struct.unpack(">q", self.take(8))[0]

    def f4(self):
        return struct.unpack(">f", self.take(4))[0]

    def f8(self):
        return struct.unpack(">d", self.take(8))[0]

    def s(self):
        return self.take(self.u2()).decode("utf-8", "replace")


def payload(r, t):
    if t == 1: return r.i1()
    if t == 2: return r.i2()
    if t == 3: return r.i4()
    if t == 4: return r.i8()
    if t == 5: return r.f4()
    if t == 6: return r.f8()
    if t == 7: return list(r.take(r.i4()))
    if t == 8: return r.s()
    if t == 9:
        et = r.u1(); n = r.i4()
        return [payload(r, et) for _ in range(n)]
    if t == 10:
        out = {}
        while True:
            ct = r.u1()
            if ct == 0: return out
            # Le nom est lu dans une variable AVANT la valeur : dans
            # `out[r.s()] = payload(...)` Python évalue la partie droite en
            # premier, donc la valeur serait lue avant le nom et tout le flux
            # se décalerait d'un tag.
            name = r.s()
            out[name] = payload(r, ct)
    if t == 11: return [r.i4() for _ in range(r.i4())]
    if t == 12: return [r.i8() for _ in range(r.i4())]
    raise ValueError(f"tag inconnu {t}")


def parse(data):
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    r = R(data)
    t = r.u1()
    if t != 10:
        raise ValueError("racine non composée")
    r.s()
    return payload(r, 10)


def chunks(path):
    """Rend (cx, cz, nbt) pour chaque chunk présent dans la région."""
    with open(path, "rb") as f:
        raw = f.read()
    # Une région tout juste créée peut n'avoir aucun en-tête écrit.
    if len(raw) < 8192:
        return
    for idx in range(1024):
        off = struct.unpack(">I", b"\x00" + raw[idx * 4:idx * 4 + 3])[0]
        count = raw[idx * 4 + 3]
        if off == 0 or count == 0:
            continue
        start = off * 4096
        length = struct.unpack(">I", raw[start:start + 4])[0]
        scheme = raw[start + 4]
        body = raw[start + 5:start + 4 + length]
        if scheme == 1:
            body = gzip.decompress(body)
        elif scheme == 2:
            body = zlib.decompress(body)
        r = R(body)
        r.u1(); r.s()
        yield idx % 32, idx // 32, payload(r, 10)
