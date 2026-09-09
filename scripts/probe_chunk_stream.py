#!/usr/bin/env python3
"""Compte les chunks qu'un serveur envoie, et en combien de temps.

Usage : python3 scripts/probe_chunk_stream.py <port> <pseudo> <secondes>

Se connecte comme `keepalive_client.py` — même poignée de main, mêmes réponses
aux keep-alive et aux téléportations — puis compte les paquets Chunk Data and
Light (0x24) et les Unload Chunk (0x1E) pendant `secondes`, et imprime le
compte, l'instant du premier et du dernier chunk.

C'est la sonde du mandat ChunkMap : « 219 chunks sur les 289 demandés arrivent »
est ce que cet outil mesure, et le 289 est le carré de rayon 8 que le serveur
promet à la connexion.
"""
import socket, struct, sys, time, zlib

PORT = int(sys.argv[1])
NAME = sys.argv[2].encode()
SECONDS = float(sys.argv[3]) if len(sys.argv) > 3 else 30.0


def varint(n):
    o = b""
    while True:
        b_ = n & 0x7F
        n >>= 7
        o += bytes([b_ | (0x80 if n else 0)])
        if not n:
            return o


s = socket.create_connection(("127.0.0.1", PORT), timeout=SECONDS + 30)
h = b"127.0.0.1"
threshold = None


def send(pid, p):
    body = varint(pid) + p
    if threshold is None:
        s.sendall(varint(len(body)) + body)
    else:
        inner = varint(0) + body if len(body) < threshold else varint(len(body)) + zlib.compress(body)
        s.sendall(varint(len(inner)) + inner)


send(0x00, varint(763) + varint(len(h)) + h + struct.pack(">H", PORT) + varint(2))
send(0x00, varint(len(NAME)) + NAME + bytes([0]))


def read():
    global threshold
    ln = 0
    sh = 0
    while True:
        c = s.recv(1)
        if not c:
            raise EOFError
        b_ = c[0]
        ln |= (b_ & 0x7F) << sh
        if not b_ & 0x80:
            break
        sh += 7
    d = b""
    while len(d) < ln:
        chunk = s.recv(ln - len(d))
        if not chunk:
            raise EOFError
        d += chunk
    i = 0
    if threshold is not None:
        size = 0
        sh2 = 0
        while True:
            b_ = d[i]
            i += 1
            size |= (b_ & 0x7F) << sh2
            if not b_ & 0x80:
                break
            sh2 += 7
        d = d[i:] if size == 0 else zlib.decompress(d[i:])
        i = 0
    pid = 0
    sh3 = 0
    while True:
        b_ = d[i]
        i += 1
        pid |= (b_ & 0x7F) << sh3
        if not b_ & 0x80:
            break
        sh3 += 7
    return pid, d[i:]


play = False
chunks = 0
unloads = 0
first = None
last = None
seen = set()
start = time.monotonic()
s.settimeout(1.0)
try:
    while time.monotonic() - start < SECONDS:
        try:
            pid, p = read()
        except socket.timeout:
            continue
        if not play and pid == 0x03:
            v = 0
            sh = 0
            i = 0
            while True:
                b_ = p[i]
                i += 1
                v |= (b_ & 0x7F) << sh
                if not b_ & 0x80:
                    break
                sh += 7
            threshold = v
            continue
        if not play and pid == 0x02:
            play = True
            continue
        if not play:
            continue
        if pid == 0x23:
            send(0x12, p[:8])
        elif pid == 0x3C:
            i = 32
            tid = 0
            sh = 0
            while True:
                b_ = p[i]
                i += 1
                tid |= (b_ & 0x7F) << sh
                if not b_ & 0x80:
                    break
                sh += 7
            send(0x00, varint(tid))
        elif pid == 0x24:
            cx, cz = struct.unpack(">ii", p[:8])
            seen.add((cx, cz))
            chunks += 1
            now = time.monotonic() - start
            if first is None:
                first = now
            last = now
        elif pid == 0x1E:
            unloads += 1
except Exception as exc:  # noqa: BLE001
    print(f"# stopped: {type(exc).__name__}: {exc}")

print(f"chunks_received  {chunks}")
print(f"distinct_chunks  {len(seen)}")
print(f"unloads          {unloads}")
print(f"first_chunk_s    {first if first is not None else -1:.3f}")
print(f"last_chunk_s     {last if last is not None else -1:.3f}")
print(f"window_s         {SECONDS:.1f}")
