#!/usr/bin/env python3
"""Un client qui reste connecté : il répond aux keep-alive.

Usage : python3 scripts/keepalive_client.py <port> <pseudo>

Sert de sonde de mesure contre un serveur en mode hors-ligne — le nôtre ou
un vrai serveur 1.20.1 utilisé comme oracle. Il se connecte, ne joue pas, et
reste là : c'est tout ce qu'il faut pour que `give`, `data get` ou `setblock`
aient une cible stable pendant qu'on lit le journal du serveur.

Sans cette réponse le serveur vanilla expulse après trente secondes, et les
commandes qui visent le joueur échouent en silence pendant que `say` continue
de fonctionner — ce qui donne un journal plein de sondes et vide de résultats.
"""
import socket, struct, sys, zlib
PORT=int(sys.argv[1]); NAME=sys.argv[2].encode()
def varint(n):
    o=b""
    while True:
        b_=n&0x7F;n>>=7;o+=bytes([b_|(0x80 if n else 0)])
        if not n: return o
s=socket.create_connection(("127.0.0.1",PORT),timeout=1200)
h=b"127.0.0.1"
threshold=None
def send(pid,p):
    body=varint(pid)+p
    if threshold is None:
        s.sendall(varint(len(body))+body)
    else:
        inner=varint(0)+body if len(body)<threshold else varint(len(body))+zlib.compress(body)
        s.sendall(varint(len(inner))+inner)
send(0x00,varint(763)+varint(len(h))+h+struct.pack(">H",PORT)+varint(2))
send(0x00,varint(len(NAME))+NAME+bytes([0]))
def read():
    global threshold
    ln=0;sh=0
    while True:
        c=s.recv(1)
        if not c: raise EOFError
        b_=c[0]; ln|=(b_&0x7F)<<sh
        if not b_&0x80: break
        sh+=7
    d=b""
    while len(d)<ln:
        chunk=s.recv(ln-len(d))
        if not chunk: raise EOFError
        d+=chunk
    i=0
    if threshold is not None:
        size=0;sh2=0
        while True:
            b_=d[i];i+=1;size|=(b_&0x7F)<<sh2
            if not b_&0x80: break
            sh2+=7
        d = d[i:] if size==0 else zlib.decompress(d[i:]); i=0
    pid=0;sh3=0
    while True:
        b_=d[i];i+=1;pid|=(b_&0x7F)<<sh3
        if not b_&0x80: break
        sh3+=7
    return pid, d[i:]
play=False
try:
    while True:
        pid,p=read()
        if not play and pid==0x03:            # login/set_compression
            v=0;sh=0;i=0
            while True:
                b_=p[i];i+=1;v|=(b_&0x7F)<<sh
                if not b_&0x80: break
                sh+=7
            threshold=v; continue
        if not play and pid==0x02:            # login success
            play=True; continue
        if play and pid==0x23:                # keep alive -> répondre
            send(0x12, p[:8])
        if play and pid==0x3c:                # téléportation -> confirmer
            i=32; tid=0; sh=0
            while True:
                b_=p[i];i+=1;tid|=(b_&0x7F)<<sh
                if not b_&0x80: break
                sh+=7
            send(0x00, varint(tid))
except Exception:
    pass
