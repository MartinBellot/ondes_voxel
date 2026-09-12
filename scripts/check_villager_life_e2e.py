#!/usr/bin/env python3
"""De bout en bout, sur **notre** serveur : un petit village de trois villageois,
trois lits, trois blocs de travail et une cloche, sur une journée — puis le
monde qu'il a sauvé, relu par le **vrai** serveur 1.20.1.

Le serveur est mené par sa console (`summon`, `setblock`, `time set`), comme le
jar ; une sonde protocole 763 se tient à côté et juge **sur le fil seulement** :

  1. midi : les trois prennent chacun un métier (métadonnée 18) — bibliothécaire
     9, fermier 5, pêcheur 6 ;
  2. 11900 : chacun se couche (pose 2, indice 6) ; l'heure du coucher est lue sur
     *Update Time* ;
  3. 23950 : chacun se lève (pose 0) ; l'heure du lever, idem — le vrai serveur :
     `last_woken` au jour 19 (docs/provenance/cerveaux.md § 3) ;
  4. 8900 : à l'heure du rassemblement, les trois vont à la cloche, à l'autre
     bout de l'enclos : leur distance à la cloche avant et après 9000 ;
  5. un marchand ambulant invoqué : six offres et son propre titre à l'écran ;
  6. un coup de la sonde sur un villageois, puis `save-all` : le fichier
     `entities/` porte `Brain.memories` (maison, travail, cloche, `last_slept`,
     `last_woken`) et le ragot `minor_negative` 25 sur la sonde.

`readback` (voie java) : le vrai serveur, lancé sur une copie de ce monde, relit
les mémoires des trois villageois, le ragot et les offres du marchand.

Usage : python3 scripts/check_villager_life_e2e.py            (OV_E2E_PORT, 25624)
        python3 scripts/check_villager_life_e2e.py readback   (le vrai serveur, 25724)
"""
from __future__ import annotations

import os
import queue
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_villagers_e2e import Probe as VillagerProbe  # noqa: E402
from vanilla_miner import read_varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PRESET = os.environ.get("OV_PRESET", "macos-debug")
BINARY = ROOT / "build" / PRESET / "bin" / "ov_dedicated"
RUN = ROOT / ".scratch" / "e2e-village"
KEPT = ROOT / ".scratch" / "e2e-village-world"   # the saved world, for `readback`
PORT = int(os.environ.get("OV_E2E_PORT", "25624"))
NAME = "OndeVillage"

VILLAGER, WANDERING_TRADER = 108, 110
Y = -60                       # standing height on the superflat
BEDS = [(6, 6), (9, 6), (12, 6)]       # foot; the head is one block south (+z)
JOBS = [(6, 12, "lectern", 9), (9, 12, "composter", 5), (12, 12, "barrel", 6)]
BELL = (28, 9)
PEN = (3, 3, 30, 15)          # x0, z0, x1, z1: glass, three high


class Server:
    """Our dedicated server, driven through its console like the jar is."""

    def __init__(self, directory: Path, port: int) -> None:
        if directory.exists():
            shutil.rmtree(directory)
        directory.mkdir(parents=True)
        self.directory = directory
        self.log = open(directory / "server.log", "w")
        self.process = subprocess.Popen(
            [str(BINARY), f"--world={directory / 'world'}", f"--port={port}", "--log-level=info"],
            cwd=directory, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.lines: queue.Queue[str] = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    return
            except OSError:
                if self.process.poll() is not None:
                    raise RuntimeError("ov_dedicated exited before listening")
                time.sleep(0.2)
        raise RuntimeError("ov_dedicated never listened")

    def _pump(self) -> None:
        assert self.process.stdout is not None
        for raw in self.process.stdout:
            self.log.write(raw)
            self.lines.put(raw.rstrip("\n"))

    def send(self, *commands: str) -> None:
        assert self.process.stdin is not None
        self.process.stdin.write("".join(c + "\n" for c in commands))
        self.process.stdin.flush()

    def stop(self) -> None:
        try:
            self.send("stop")
            self.process.wait(timeout=60)
        except Exception:
            self.process.kill()
        self.log.close()


class Probe(VillagerProbe):
    """The villager probe, plus the pose of every villager and the day time."""

    def __init__(self, port: int, name: str) -> None:
        self.poses: dict[int, list[tuple[float, int, int | None]]] = {}
        self.day: int | None = None
        super().__init__(port, name)

    def handle(self, pid: int, payload: bytes) -> None:
        super().handle(pid, payload)
        if pid == 0x5E and len(payload) >= 16:
            self.day = abs(struct.unpack_from(">qq", payload, 0)[1]) % 24000
        elif pid == 0x52:
            eid, i = read_varint(payload, 0)
            while i < len(payload):
                index = payload[i]
                i += 1
                if index == 0xFF:
                    break
                kind, i = read_varint(payload, i)
                if kind == 20:
                    pose, i = read_varint(payload, i)
                    if index == 6:
                        self.poses.setdefault(eid, []).append((time.monotonic(), pose, self.day))
                elif kind == 18:
                    for _ in range(3):
                        _, i = read_varint(payload, i)
                elif kind == 1:
                    _, i = read_varint(payload, i)
                elif kind == 3:
                    i += 4
                elif kind in (0, 8):
                    i += 1
                elif kind == 11:
                    i += 1 + (8 if payload[i] else 0)
                else:
                    break

    def pose_of(self, eid: int) -> int:
        seen = self.poses.get(eid)
        return seen[-1][1] if seen else 0


def distance(a: tuple[float, float, float], x: float, z: float) -> float:
    return ((a[0] - x) ** 2 + (a[2] - z) ** 2) ** 0.5


def report(results: dict[str, str], ok: bool, done: str) -> None:
    print()
    print("── résultat ─────────────────────────────────────────────")
    for key, value in results.items():
        print(f"  {key:22} {value}")
    print(f"  {'verdict':22} {done if ok else 'incomplet'}")


def village() -> int:
    results: dict[str, str] = {}
    verdicts: list[bool] = []
    server = Server(RUN, PORT)
    probe = None
    try:
        x0, z0, x1, z1 = PEN
        server.send(f"fill {x0 - 1} {Y} {z0 - 1} {x1 + 1} {Y + 2} {z1 + 1} minecraft:glass",
                    f"fill {x0} {Y} {z0} {x1} {Y + 2} {z1} minecraft:air")
        for x, z in BEDS:
            server.send(
                f"setblock {x} {Y} {z} minecraft:red_bed[facing=south,part=foot,occupied=false]",
                f"setblock {x} {Y} {z + 1} minecraft:red_bed[facing=south,part=head,occupied=false]")
        for x, z, block, _ in JOBS:
            server.send(f"setblock {x} {Y} {z} minecraft:{block}")
        server.send(f"setblock {BELL[0]} {Y} {BELL[1]} minecraft:bell", "time set 6000")
        for _ in range(30):
            try:
                probe = Probe(PORT, NAME)
                break
            except (OSError, EOFError):
                time.sleep(2.0)
        assert probe is not None
        probe.settle(4.0)
        server.send(f"tp {NAME} 16.5 {Y + 3} 20.5", f"gamemode spectator {NAME}")
        for k in range(3):
            server.send(f"summon minecraft:villager {8.5 + 2 * k} {Y} 9.5")
        probe.settle(3.0)
        villagers = [e for e, t in probe.entities.items() if t == VILLAGER]
        results["villageois"] = f"{len(villagers)} arrivés sur le fil"
        if len(villagers) != 3:
            verdicts.append(False)
            return 1

        # 1. Professions.
        t0 = time.monotonic()
        professions: list[int] = []
        while time.monotonic() - t0 < 90.0:
            probe.settle(0.5)
            professions = sorted((probe.data_of(e) or (0, 0, 0))[1] for e in villagers)
            if professions == [5, 6, 9]:
                break
        results["métiers (midi)"] = (f"{professions} en {time.monotonic() - t0:.1f} s, types "
                                     f"{[(probe.data_of(e) or (0, 0, 0))[0] for e in villagers]}")
        verdicts.append(professions == [5, 6, 9])

        # 2. To bed.
        server.send("time set 11900")
        t0 = time.monotonic()
        asleep: dict[int, int | None] = {}
        while time.monotonic() - t0 < 90.0 and len(asleep) < 3:
            probe.settle(0.25)
            for e in villagers:
                if e not in asleep and probe.pose_of(e) == 2:
                    asleep[e] = probe.day
        results["couchés (jour)"] = (f"{sorted(d for d in asleep.values() if d is not None)} "
                                     f"({len(asleep)}/3)")
        verdicts.append(len(asleep) == 3 and all(d is not None and d >= 12000
                                                 for d in asleep.values()))

        # 3. Up.
        server.send("time set 23950")
        t0 = time.monotonic()
        awake: dict[int, int | None] = {}
        while time.monotonic() - t0 < 60.0 and len(awake) < 3:
            probe.settle(0.25)
            for e in villagers:
                if e not in awake and probe.pose_of(e) == 0:
                    awake[e] = probe.day
        days = [d for d in awake.values() if d is not None]
        results["levés (jour)"] = f"{sorted(days)} ({len(awake)}/3) — vanilla : last_woken au jour 19"
        verdicts.append(len(awake) == 3 and all(10 <= d <= 60 for d in days))

        # 4. The bell.
        server.send("time set 8900")
        probe.settle(4.0)
        before = [distance(probe.entity_pos[e], BELL[0] + 0.5, BELL[1] + 0.5)
                  for e in villagers if e in probe.entity_pos]
        t0 = time.monotonic()
        after: list[float] = []
        while time.monotonic() - t0 < 60.0:
            probe.settle(1.0)
            after = [distance(probe.entity_pos[e], BELL[0] + 0.5, BELL[1] + 0.5)
                     for e in villagers if e in probe.entity_pos]
            if after and max(after) <= 8.0:
                break
        results["cloche (distance)"] = (f"avant 9000 {[round(d, 1) for d in before]}, "
                                        f"après {[round(d, 1) for d in after]} (jour {probe.day})")
        verdicts.append(bool(after) and max(after) <= 8.0 and min(before) > 8.0)

        # 5. A wandering trader: six offers, and a screen of its own.
        server.send("time set 6000", f"gamemode creative {NAME}",
                    f"summon minecraft:wandering_trader 16.5 {Y} 9.5", f"tp {NAME} 16.5 {Y} 11.5")
        probe.settle(3.0)
        traders = [e for e, t in probe.entities.items() if t == WANDERING_TRADER]
        offers = None
        if traders:
            probe.offers = None
            probe.interact(traders[0])
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline and probe.offers is None:
                probe.settle(0.1)
            offers = probe.offers
            probe.close_screen()
        n = len(offers["trades"]) if offers else 0
        results["marchand ambulant"] = (f"{len(traders)} vu, {n} offres, titre …{probe.title[-44:]}"
                                        if traders else "non vu")
        verdicts.append(n == 6 and "wandering_trader" in probe.title)

        # 6. A hit on a villager, then the save.
        target = villagers[0]
        tx, ty, tz = probe.entity_pos.get(target, (8.5, Y, 9.5))
        server.send(f"tp {NAME} {tx:.2f} {Y} {tz + 1.2:.2f}")
        probe.settle(1.0)
        probe.attack(target)
        probe.settle(1.0)
        server.send("save-all")
        probe.settle(3.0)
    finally:
        if probe is not None:
            try:
                probe.s.close()
            except OSError:
                pass
        server.stop()
        entities = RUN / "world" / "entities"
        found = sorted(p.name for p in entities.glob("*.mca")) if entities.exists() else []
        results["entities/"] = f"{found}"
        if found:
            from anvil_read import chunks  # (cx, cz, nbt) per chunk
            brains, gossip = [], []
            for path in entities.glob("*.mca"):
                for _cx, _cz, root in chunks(path):
                    for ent in root.get("Entities", []):
                        if ent.get("id") == "minecraft:villager":
                            brains.append(sorted(ent.get("Brain", {}).get("memories", {})))
                            gossip += [(g.get("Type"), g.get("Value"))
                                       for g in ent.get("Gossips", [])]
            wanted = {"minecraft:home", "minecraft:job_site", "minecraft:meeting_point",
                      "minecraft:last_slept", "minecraft:last_woken"}
            results["Brain.memories"] = str(brains)
            results["Gossips"] = str(gossip)
            verdicts.append(len(brains) == 3 and all(wanted <= set(b) for b in brains))
            verdicts.append(("minor_negative", 25) in gossip)
            shutil.rmtree(KEPT, ignore_errors=True)
            shutil.copytree(RUN / "world", KEPT)
        shutil.rmtree(RUN, ignore_errors=True)
        ok = bool(verdicts) and all(verdicts)
        report(results, ok, "le village a vécu sa journée")
    return 0 if ok else 1


def readback() -> int:
    """The real server on a copy of the world ours saved."""
    from measure_entities import Server as Vanilla
    results: dict[str, str] = {}
    run = ROOT / ".scratch" / "e2e-village-vanilla"
    shutil.rmtree(run, ignore_errors=True)
    run.mkdir(parents=True)
    shutil.copytree(KEPT, run / "world")
    server = Vanilla(run, port=PORT + 100)
    ok = False
    try:
        server.batch(["forceload add 0 0 31 31"])
        time.sleep(8.0)
        lines = server.batch([
            "execute as @e[type=minecraft:villager] run data get entity @s Brain.memories",
            "execute as @e[type=minecraft:villager] run data get entity @s Gossips",
            "execute as @e[type=minecraft:villager] run data get entity @s VillagerData",
            "execute as @e[type=minecraft:wandering_trader] run data get entity @s Offers.Recipes"],
            timeout=60)
        data = [line for line in lines if "has the following entity data" in line]
        memories = [d for d in data if "minecraft:home" in d or "minecraft:last_slept" in d]
        gossip = [d for d in data if "minor_negative" in d]
        recipes = [d for d in data if "maxUses" in d]
        homes = sum(1 for d in memories if all(k in d for k in (
            "minecraft:home", "minecraft:job_site", "minecraft:meeting_point",
            "minecraft:last_slept", "minecraft:last_woken")))
        results["villageois relus"] = f"{homes} avec maison, travail, cloche, sommeil, réveil"
        results["ragot relu"] = (re.sub(r"^.*entity data: ", "", gossip[0])[:120]
                                 if gossip else "aucun")
        results["marchand relu"] = f"{len(recipes)} (offres : {recipes[0].count('maxUses') if recipes else 0})"
        ok = homes == 3 and bool(gossip) and bool(recipes) and recipes[0].count("maxUses") == 6
    finally:
        server.stop()
        shutil.rmtree(run, ignore_errors=True)
        report(results, ok, "le vrai serveur relit le village")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(readback() if sys.argv[1:] == ["readback"] else village())
