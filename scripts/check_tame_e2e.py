#!/usr/bin/env python3
"""De bout en bout : apprivoiser un loup, le faire se lever, le laisser derrière
soi, lui teindre le collier ; monter un cheval sauvage jusqu'à ce qu'il cède,
le seller, le conduire, en descendre ; redémarrer — sur **notre** serveur.

La sonde est un client protocole 763 (celle de `check_husbandry_e2e.py`) : elle
ne voit que ce que le serveur lui envoie. Chaque étape est jugée sur le fil :

  1. des os sur le loup jusqu'à un `Entity Event` 7 (les cœurs), chaque échec
     un 6 ; l'indice 17 porte alors 0x04 (apprivoisé) et 0x01 (assis), et
     l'indice 18 l'UUID de la sonde ;
  2. la main vide sur le loup : l'indice 17 perd 0x01 (debout) ;
  3. la sonde s'éloigne de 16 blocs : le loup la rejoint (téléporté à moins de
     3,5 blocs, ou arrivé en marchant) ;
  4. une teinture bleue : l'indice 20 passe à 11 ;
  5. la main vide sur le cheval : `Set Passengers` avec la sonde ; il la jette
     (statut 6) ou cède (statut 7) ; on remonte jusqu'à ce qu'il cède ;
  6. descendre (`Player Input` drapeau 0x02), une selle : l'indice 17 porte
     0x02 | 0x04 ;
  7. remonter et envoyer des `Move Vehicle` : le cheval avance sur le fil ;
     descendre ;
  8. `stop`, redémarrage sur le même monde : loup et cheval reviennent de
     `entities/`, apprivoisés, avec leur propriétaire, leur collier et la selle.

Le serveur est en créatif (le mode par défaut) : `Set Creative Slot` est la
seule façon pour la sonde d'avoir un os, une teinture, une selle ; le créatif ne
consomme rien et n'empêche aucun des tirages.

Usage : python3 scripts/check_tame_e2e.py   (port OV_E2E_PORT, 25605 par défaut)
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_husbandry_e2e import Probe as HusbandryProbe  # noqa: E402
from check_interaction_e2e import item_id, items_by_id  # noqa: E402
from vanilla_miner import read_varint, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PRESET = os.environ.get("OV_PRESET", "macos-debug")
BINARY = ROOT / "build" / PRESET / "bin" / "ov_dedicated"
WORLD = ROOT / ".scratch" / "e2e-tame"
LOG = ROOT / ".scratch" / "e2e-tame.log"
OUT = ROOT / ".scratch" / "tame_e2e.json"
PORT = int(os.environ.get("OV_E2E_PORT", "25605"))
NAME = "OndeTamer"

WOLF, HORSE = 116, 49


def offline_uuid_hex(name: str) -> str:
    raw = bytearray(hashlib.md5(f"OfflinePlayer:{name}".encode()).digest())
    raw[6] = (raw[6] & 0x0F) | 0x30
    raw[8] = (raw[8] & 0x3F) | 0x80
    return bytes(raw).hex()


class Tamer(HusbandryProbe):
    def __init__(self, port: int, name: str) -> None:
        super().__init__(port, name)
        self.passengers: list[tuple[float, int, list[int]]] = []

    def handle(self, pid: int, p: bytes) -> None:
        super().handle(pid, p)
        if pid == 0x59:
            vehicle, i = read_varint(p, 0)
            n, i = read_varint(p, i)
            riders = []
            for _ in range(n):
                r, i = read_varint(p, i)
                riders.append(r)
            self.passengers.append((time.monotonic(), vehicle, riders))

    def empty_hand(self) -> None:
        # Hotbar slot 8, which nothing ever fills: a Set Creative Slot with an
        # empty stack is not a reliable way to clear a hand.
        self.send(0x28, struct.pack(">h", 8))

    def hold(self, item: int, count: int = 1) -> None:
        self.creative_set(36, item, count)
        self.send(0x28, struct.pack(">h", 0))

    def get_off(self) -> None:
        self.send(0x1F, struct.pack(">ff", 0.0, 0.0) + bytes([0x02]))

    def drive(self, x: float, y: float, z: float) -> None:
        self.send(0x18, struct.pack(">dddff", x, y, z, -90.0, 0.0))

    def events_of(self, eid: int, since: float) -> list[int]:
        return [s for t, e, s in self.events if e == eid and t >= since]

    def riding(self, vehicle: int, since: float) -> bool | None:
        state = None
        for t, v, riders in self.passengers:
            if v == vehicle and t >= since:
                state = len(riders) > 0
        return state


def connect() -> Tamer:
    for _ in range(20):
        try:
            return Tamer(PORT, NAME)
        except (OSError, EOFError):
            time.sleep(4.0)
    raise RuntimeError("never joined")


def start(mobs: str | None) -> subprocess.Popen:
    log = open(LOG, "a")
    args = [str(BINARY), f"--world={WORLD}", f"--port={PORT}", "--log-level=debug"]
    if mobs:
        args.append(f"--mobs={mobs}")
    return subprocess.Popen(args, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)


def stop(server: subprocess.Popen) -> None:
    try:
        assert server.stdin is not None
        server.stdin.write(b"stop\n")
        server.stdin.flush()
        server.wait(timeout=60)
    except Exception:
        server.kill()


def wait_listening(timeout: float = 60.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if LOG.exists() and "listening" in LOG.read_text(errors="replace"):
            return True
        time.sleep(0.5)
    return False


def flags(probe: Tamer, eid: int) -> int:
    value = probe.field(eid, 17)
    return int(value) & 0xFF if value is not None else 0


def main() -> int:  # noqa: C901 - one scenario, told in order
    items = items_by_id()
    bone = item_id(items, "minecraft:bone")
    blue = item_id(items, "minecraft:blue_dye")
    saddle = item_id(items, "minecraft:saddle")
    me = offline_uuid_hex(NAME)

    shutil.rmtree(WORLD, ignore_errors=True)
    WORLD.mkdir(parents=True)
    LOG.write_text("")
    results: dict[str, object] = {}
    failures: list[str] = []
    server = start("wolf,horse")
    try:
        if not wait_listening():
            print("ov_dedicated n'écoute pas (journal : .scratch/e2e-tame.log)")
            return 1
        probe = connect()
        probe.settle(8.0)
        wolves = [e for e, t in probe.types.items() if t == WOLF]
        horses = [e for e, t in probe.types.items() if t == HORSE]
        if not wolves or not horses:
            print(f"les mobs de --mobs ne sont pas arrivés : {probe.types}")
            return 1
        wolf, horse = wolves[0], horses[0]

        # 1. Des os jusqu'aux cœurs.
        probe.hold(bone, 64)
        probe.settle(0.5)
        tries = []
        for _ in range(40):
            probe.near(wolf)
            probe.settle(0.1)
            since = time.monotonic()
            probe.interact(wolf)
            probe.settle(0.35)
            got = [s for s in probe.events_of(wolf, since) if s in (6, 7)]
            tries.append(got[-1] if got else None)
            if 7 in got:
                break
        probe.settle(0.5)
        tamed = 7 in tries
        results["loup apprivoisé"] = {"essais": tries, "indice 17": flags(probe, wolf),
                                      "indice 18": probe.field(wolf, 18)}
        if not tamed or flags(probe, wolf) & 0x05 != 0x05 or probe.field(wolf, 18) != me:
            failures.append("apprivoisement du loup")

        # 2. Debout.
        probe.empty_hand()
        probe.settle(0.3)
        probe.near(wolf)
        probe.interact(wolf)
        probe.settle(0.6)
        results["loup debout"] = {"indice 17": flags(probe, wolf)}
        if flags(probe, wolf) != 0x04:
            failures.append("loup debout")

        # 3. Laissé derrière.
        wx, wy, wz = probe.where[wolf]
        target = (wx + 16.0, wy, wz)
        probe.stand(*target)
        joined = None
        deadline = time.monotonic() + 15.0
        started = time.monotonic()
        while time.monotonic() < deadline:
            probe.settle(0.25)
            x, y, z = probe.where[wolf]
            if abs(x - target[0]) <= 3.5 and abs(z - target[2]) <= 3.5:
                joined = time.monotonic() - started
                break
        results["loup qui suit"] = {"secondes": round(joined, 2) if joined else None,
                                    "position": probe.where[wolf], "sonde": target}
        if joined is None:
            failures.append("le loup ne suit pas")

        # 4. Le collier.
        probe.hold(blue)
        probe.settle(0.3)
        probe.near(wolf)
        probe.interact(wolf)
        probe.settle(0.6)
        results["collier"] = {"indice 20": probe.field(wolf, 20)}
        if probe.field(wolf, 20) != 11:
            failures.append("collier bleu")

        # 5. Le cheval, monté jusqu'à ce qu'il cède.
        probe.empty_hand()
        probe.settle(0.3)
        rides = []
        for _ in range(25):
            probe.near(horse)
            probe.settle(0.3)
            since = time.monotonic()
            probe.interact(horse)
            probe.settle(0.5)
            mounted = probe.riding(horse, since)
            verdict = None
            deadline = time.monotonic() + 30.0
            while time.monotonic() < deadline:
                probe.settle(0.25)
                got = [s for s in probe.events_of(horse, since) if s in (6, 7)]
                if got:
                    verdict = got[-1]
                    break
            rides.append({"monté": mounted, "statut": verdict,
                          "secondes": round(time.monotonic() - since, 1)})
            if verdict == 7 or verdict is None:
                break
            probe.settle(1.0)
        results["cheval"] = {"montées": rides, "indice 17": flags(probe, horse)}
        if not rides or rides[-1]["statut"] != 7 or not flags(probe, horse) & 0x02:
            failures.append("cheval apprivoisé")

        # 6. Descendre, seller.
        probe.get_off()
        probe.settle(1.0)
        off = probe.riding(horse, 0.0) is False
        probe.hold(saddle)
        probe.settle(0.3)
        probe.near(horse)
        probe.interact(horse)
        probe.settle(0.6)
        results["selle"] = {"descendu": off, "indice 17": flags(probe, horse)}
        if flags(probe, horse) & 0x06 != 0x06:
            failures.append("selle")

        # 7. Conduire.
        probe.empty_hand()
        probe.settle(0.3)
        probe.near(horse)
        since = time.monotonic()
        probe.interact(horse)
        probe.settle(0.6)
        mounted = probe.riding(horse, since)
        hx, hy, hz = probe.where[horse]
        for step in range(1, 41):
            probe.drive(hx + 0.25 * step, hy, hz)
            probe.settle(0.05)
        probe.settle(1.0)
        moved = probe.where[horse][0] - hx
        probe.get_off()
        probe.settle(1.0)
        results["conduite"] = {"monté": mounted, "avancé (blocs)": round(moved, 3),
                               "descendu": probe.riding(horse, since) is False}
        if not mounted or moved < 9.0:
            failures.append("conduite")

        probe.s.close()
    finally:
        stop(server)

    # 8. Le redémarrage.
    time.sleep(1.0)
    server = start(None)
    try:
        if not wait_listening():
            failures.append("redémarrage")
        else:
            probe = connect()
            probe.settle(12.0)
            back = {t: e for e, t in probe.types.items() if t in (WOLF, HORSE)}
            wolf2, horse2 = back.get(WOLF), back.get(HORSE)
            results["redémarrage"] = {
                "loup": {"indice 17": flags(probe, wolf2) if wolf2 else None,
                         "indice 18": probe.field(wolf2, 18) if wolf2 else None,
                         "indice 20": probe.field(wolf2, 20) if wolf2 else None},
                "cheval": {"indice 17": flags(probe, horse2) if horse2 else None},
            }
            if wolf2 is None or flags(probe, wolf2) & 0x04 == 0 or probe.field(wolf2, 18) != me \
                    or probe.field(wolf2, 20) != 11:
                failures.append("loup relu")
            if horse2 is None or flags(probe, horse2) & 0x06 != 0x06:
                failures.append("cheval relu")
            probe.s.close()
    finally:
        stop(server)

    OUT.write_text(json.dumps(results, indent=1, ensure_ascii=False))
    for key, value in results.items():
        print(f"{key:18} {value}")
    if failures:
        print("ÉCHECS :", ", ".join(failures))
        return 1
    print("tout est vu sur le fil")
    return 0


# ── zoo ─────────────────────────────────────────────────────────────────────
#
# Notre serveur sur une copie du monde où le vrai serveur a posé le zoo de
# `measure_tame.py zoo` : chaque type reçu sur le fil, ses indices apprivoisés,
# puis `save-all` — le monde réécrit est gardé pour `measure_tame.py zoo_back`,
# qui le fait relire par le vrai serveur.

VANILLA_ZOO = ROOT / ".scratch" / "tame-zoo-vanilla"
OURS_ZOO = ROOT / ".scratch" / "tame-zoo-ours"
ZOO_TYPES = {116: "wolf", 11: "cat", 67: "ocelot", 49: "horse", 21: "donkey", 66: "mule",
             60: "llama", 103: "trader_llama", 79: "rabbit", 38: "fox", 70: "parrot",
             106: "turtle", 6: "bee", 45: "goat", 10: "camel"}


def check_zoo() -> int:
    if not VANILLA_ZOO.exists():
        print(f"pas de {VANILLA_ZOO} : lancer measure_tame.py zoo")
        return 1
    shutil.rmtree(OURS_ZOO, ignore_errors=True)
    shutil.copytree(VANILLA_ZOO, OURS_ZOO)
    (OURS_ZOO / "session.lock").unlink(missing_ok=True)
    LOG.write_text("")
    args = [str(BINARY), f"--world={OURS_ZOO}", f"--port={PORT}", "--log-level=debug"]
    server = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=open(LOG, "a"),
                              stderr=subprocess.STDOUT)
    out: dict = {}
    try:
        if not wait_listening():
            print("ov_dedicated n'écoute pas (journal : .scratch/e2e-tame.log)")
            return 1
        probe = connect()
        probe.stand(4.5, -60.0, -12.0)
        # 45 s, as mobs-3's anvil: a world read from disk keeps no spawn area
        # resident, and its chunks — and their entities — come with the player.
        probe.settle(45.0)
        seen: dict[str, int] = {}
        fields: dict[str, dict] = {}
        for eid, t in probe.types.items():
            name = ZOO_TYPES.get(t)
            if name is None:
                continue
            seen[name] = seen.get(name, 0) + 1
            fields[f"{name}#{seen[name]}"] = {i: repr(probe.field(eid, i))
                                             for i in (17, 18, 19, 20, 21, 22)
                                             if probe.field(eid, i) is not None}
        out["vus"] = seen
        out["indices"] = fields
        assert server.stdin is not None
        server.stdin.write(b"save-all\n")
        server.stdin.flush()
        probe.settle(4.0)
        probe.s.close()
    finally:
        stop(server)
    out["fichiers entities"] = sorted(p.name for p in (OURS_ZOO / "entities").glob("*.mca"))
    total = sum(out["vus"].values())
    out["zoo"] = f"{total} mobs sur 16"
    OUT.with_name("tame_zoo_e2e.json").write_text(json.dumps(out, indent=1, ensure_ascii=False))
    for key, value in out.items():
        print(f"{key:18} {value}")
    return 0 if total >= 16 else 1


if __name__ == "__main__":
    if sys.argv[1:] == ["zoo"]:
        sys.exit(check_zoo())
    sys.exit(main())
