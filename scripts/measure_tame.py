#!/usr/bin/env python3
"""Ask a real 1.20.1 server how its tamable and rideable animals behave.

Every number the taming code carries comes out of one of these campaigns, run
against `tools/vanilla/server.jar` with a probe client connected (the probe of
measure_husbandry.py: it answers keep-alives on a thread and sends Interact).

  meta      Which metadata index says "tame", "sitting", "collar", "anger",
            "variant", "chest", "strength", "carpet", "rabbit type"... One NBT
            field at a time against a baseline of the same species.
  tame      Bones on wolves, cod on cats, seeds on parrots: the Entity Event of
            every try (7 tamed, 6 refused), so the probability is a count.
  ocelot    Cod offered to an ocelot that sees it: trust events (41 / 40).
  anger     A wolf hit by the probe: AngerTime, AngryAt, and its pack.
  wolf      A tame wolf: its maximum health, the heal of each meat, sitting
            toggled by an empty hand, the collar dyed, love when full.
  follow    A tame wolf left behind: does it walk, how fast, when does it
            teleport to its owner.
  spawn     Horses, donkeys, mules, llamas summoned without NBT (so drawn by
            the game): health, speed, jump strength, variant, strength.
  breed     Tame horse pairs in love with set attributes: the foal's.
  temper    Mounting an untamed horse (and llama, donkey) until it is tame:
            the tries, the Temper after each, the ticks to each decision.
  zoo       One of each species with its NBT, `data get` of each, save-all,
            and the world copied to .scratch/tame-zoo-vanilla for the
            server's round-trip test.

Usage: python3 scripts/measure_tame.py [campaign ...]
Run it through the java lane: `ovlane.sh java python3 scripts/measure_tame.py`.

Writes data/vanilla/1.20.1/normalized/tame.json (gitignored), merging into
what is already there so campaigns can be run one at a time.
"""
from __future__ import annotations

import hashlib
import json
import re
import shutil
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_entity_packets import read_varint, varint  # noqa: E402
from measure_husbandry import Hand, Rig, nbt_int, parse_metadata, pos_of, uuid_nbt, uuid_of  # noqa: E402,F401
from measure_mobs import FlatServer  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
OUT = NORMALIZED / "tame.json"
RUN = ROOT / ".scratch" / "tame-oracle"
ZOO_COPY = ROOT / ".scratch" / "tame-zoo-vanilla"
PORT = 25705
Y = -60
PROBE = "ovhand"

ATTRIBUTE = re.compile(r"for entity .* is (-?[0-9.Ee-]+)$")
DATA = re.compile(r"has the following entity data: (.*)$")
GAMETIME = re.compile(r"The time is (\d+)")


def offline_uuid(name: str) -> list[int]:
    """The UUID an offline server gives a player: MD5 of "OfflinePlayer:name",
    version 3 — as four signed ints, the NBT form."""
    raw = bytearray(hashlib.md5(f"OfflinePlayer:{name}".encode()).digest())
    raw[6] = (raw[6] & 0x0F) | 0x30
    raw[8] = (raw[8] & 0x3F) | 0x80
    return list(struct.unpack(">iiii", bytes(raw)))


OWNER = "Owner:[I;{}]".format(",".join(str(v) for v in offline_uuid(PROBE)))


class Rider(Hand):
    """The husbandry probe, plus the packets a rider needs: Set Passengers."""

    def __init__(self, port: int, name: str = PROBE) -> None:
        self.passengers: list[tuple[float, int, list[int]]] = []
        super().__init__(port, name)

    def _run(self) -> None:  # noqa: C901 - one reader, one switch
        import socket
        self.socket.settimeout(0.5)
        while not self._stop.is_set():
            try:
                pid, p = self.read()
            except (socket.timeout, TimeoutError):
                continue
            except Exception:
                return
            now = time.monotonic()
            if pid == 0x59:
                vehicle, i = read_varint(p, 0)
                n, i = read_varint(p, i)
                riders = []
                for _ in range(n):
                    r, i = read_varint(p, i)
                    riders.append(r)
                with self.state_lock:
                    self.passengers.append((now, vehicle, riders))
                continue
            # Everything else as the husbandry probe reads it.
            self._dispatch(pid, p, now)

    def _dispatch(self, pid: int, p: bytes, now: float) -> None:
        if pid == 0x23:
            self.send(0x12, p[:8])
        elif pid == 0x3C:
            x, y, z = struct.unpack_from(">ddd", p, 0)
            flags = p[32]
            if self.position is not None:
                px, py, pz = self.position
                x = px + x if flags & 0x01 else x
                y = py + y if flags & 0x02 else y
                z = pz + z if flags & 0x04 else z
            self.position = (x, y, z)
            tid, _ = read_varint(p, 33)
            self.send(0x00, varint(tid))
            self.send(0x14, struct.pack(">ddd", x, y, z) + bytes([1]))
        elif pid == 0x5E and len(p) >= 8:
            self.clock = (struct.unpack_from(">q", p, 0)[0], now)
        elif pid == 0x01:
            eid, i = read_varint(p, 0)
            msb, lsb = struct.unpack_from(">QQ", p, i)
            i += 16
            etype, i = read_varint(p, i)
            x, y, z = struct.unpack_from(">ddd", p, i)
            with self.state_lock:
                self.spawns[eid] = {"type": etype, "lsb": lsb, "msb": msb, "t": now,
                                    "pos": (x, y, z)}
                if msb == (0x4F56 << 32):
                    self.by_lsb[lsb] = eid
        elif pid == 0x52:
            eid, i = read_varint(p, 0)
            try:
                fields = parse_metadata(p, i)
            except Exception:
                fields = [("unparsed", p[i:].hex())]
            with self.state_lock:
                self.metadata.append((now, eid, fields))
        elif pid == 0x1C and len(p) >= 5:
            eid = struct.unpack_from(">i", p, 0)[0]
            status = struct.unpack_from(">b", p, 4)[0]
            with self.state_lock:
                self.events.append((now, eid, status))

    def attack(self, eid: int) -> None:
        self.send(0x10, varint(eid) + varint(1) + bytes([0]))
        self.send(0x2F, varint(0))

    def wait_event(self, eid: int, statuses: set[int], since: float,
                   timeout: float = 3.0) -> tuple[int, float] | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self.state_lock:
                for t, e, s in self.events:
                    if e == eid and s in statuses and t >= since:
                        return s, t
            time.sleep(0.01)
        return None

    def wait_passengers(self, vehicle: int, since: float, riding: bool,
                        timeout: float) -> float | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self.state_lock:
                for t, v, riders in self.passengers:
                    if v == vehicle and t >= since and (len(riders) > 0) == riding:
                        return t
            time.sleep(0.01)
        return None

    def ticks(self) -> int | None:
        return self.clock[0] if self.clock else None


class Oracle(Rig):
    def ask(self, commands: list[str], timeout: float = 300) -> list[list[str]]:
        """Each command's own console lines, in order."""
        batch = []
        for i, command in enumerate(commands):
            batch.append(f"say ovq{i}")
            batch.append(command)
        lines = self.server.batch(batch, timeout=timeout)
        out: list[list[str]] = [[] for _ in commands]
        current = -1
        for line in lines:
            m = re.search(r"\[Server\] ovq(\d+)$", line)
            if m:
                current = int(m.group(1))
                continue
            if current >= 0:
                out[current].append(line)
        return out

    def attributes(self, selector: str, names: list[str]) -> list[float | None]:
        answers = self.ask([f"attribute {selector} minecraft:{n} base get" for n in names])
        out: list[float | None] = []
        for lines in answers:
            value = None
            for line in lines:
                m = ATTRIBUTE.search(line)
                if m:
                    value = float(m.group(1))
            out.append(value)
        return out


def value_of(lines: list[str]) -> str | None:
    for line in lines:
        m = DATA.search(line)
        if m:
            return m.group(1)
    return None


# ── meta ────────────────────────────────────────────────────────────────────

BASE = "NoAI:1b,Silent:1b"
META_CASES = [
    ("wolf", ""), ("wolf", OWNER), ("wolf", OWNER + ",Sitting:1b"),
    ("wolf", OWNER + ",CollarColor:14b"), ("wolf", "AngerTime:400"), ("wolf", "Age:-24000"),
    ("cat", 'variant:"minecraft:tabby"'), ("cat", 'variant:"minecraft:black"'),
    ("cat", 'variant:"minecraft:tabby",' + OWNER),
    ("cat", 'variant:"minecraft:tabby",' + OWNER + ",Sitting:1b"),
    ("cat", 'variant:"minecraft:tabby",' + OWNER + ",CollarColor:5b"),
    ("ocelot", ""), ("ocelot", "Trusting:1b"),
    ("horse", "Variant:0"), ("horse", "Variant:513"), ("horse", "Variant:0,Tame:1b"),
    ("horse", 'Variant:0,SaddleItem:{id:"minecraft:saddle",Count:1b}'),
    ("horse", 'Variant:0,ArmorItem:{id:"minecraft:iron_horse_armor",Count:1b}'),
    ("horse", "Variant:0," + OWNER), ("horse", "Variant:0,Bred:1b"),
    ("horse", "Variant:0,EatingHaystack:1b"), ("horse", "Variant:0,Age:-24000"),
    ("donkey", ""), ("donkey", "ChestedHorse:1b"), ("donkey", "Tame:1b"),
    ("mule", ""), ("mule", "ChestedHorse:1b"),
    ("llama", "Variant:0,Strength:1"), ("llama", "Variant:2,Strength:1"),
    ("llama", "Variant:0,Strength:5"),
    ("llama", 'Variant:0,Strength:1,DecorItem:{id:"minecraft:red_carpet",Count:1b}'),
    ("llama", "Variant:0,Strength:1,ChestedHorse:1b"), ("llama", "Variant:0,Strength:1,Tame:1b"),
    ("trader_llama", "Variant:0,Strength:1"),
    ("rabbit", "RabbitType:0"), ("rabbit", "RabbitType:1"), ("rabbit", "RabbitType:99"),
    ("fox", 'Type:"red"'), ("fox", 'Type:"snow"'), ("fox", 'Type:"red",Sleeping:1b'),
    ("fox", 'Type:"red",Sitting:1b'), ("fox", 'Type:"red",Crouching:1b'),
    ("parrot", "Variant:0"), ("parrot", "Variant:3"), ("parrot", "Variant:0," + OWNER),
    ("parrot", "Variant:0," + OWNER + ",Sitting:1b"),
    ("turtle", ""), ("turtle", "HasEgg:1b"),
    ("bee", ""), ("bee", "HasNectar:1b"), ("bee", "AngerTime:400"), ("bee", "HasStung:1b"),
    ("goat", ""), ("goat", "IsScreamingGoat:1b"), ("goat", "HasLeftHorn:0b"),
    ("goat", "HasRightHorn:0b"),
    ("camel", ""), ("camel", 'SaddleItem:{id:"minecraft:saddle",Count:1b}'),
    ("sniffer", ""),
]


def campaign_meta(rig: Oracle) -> dict:
    out = []
    for kind, extra in META_CASES:
        nbt = BASE + ("," + extra if extra else "")
        n, eid = rig.summon_near(kind, nbt, (3.5, Y, 3.5))
        time.sleep(0.5)
        fields = [f for _, f in rig.hand.metadata_of(eid)]
        out.append({"type": kind, "nbt": extra,
                    "fields": [[list(map(str, fl)) for fl in f] for f in fields]})
        print(f"  {kind:12} {extra[:50]:50} {fields}", flush=True)
        rig.kill(n)
    return {"owner_uuid": offline_uuid(PROBE), "cases": out}


# ── tame ────────────────────────────────────────────────────────────────────

def tame_series(rig: Oracle, kind: str, item: str, animals: int, nbt: str,
                success: int = 7, failure: int = 6, cap: int = 60) -> list[list[int]]:
    """For each animal, the statuses of every try until it was tamed."""
    hand: Rider = rig.hand  # type: ignore[assignment]
    series = []
    for a in range(animals):
        n, eid = rig.summon_near(kind, nbt, (1.5, Y, 0.5))
        rig.hold(item, 64)
        time.sleep(0.15)
        tries: list[int] = []
        for _ in range(cap):
            since = time.monotonic()
            hand.interact(eid)
            got = hand.wait_event(eid, {success, failure}, since, timeout=2.0)
            if got is None:
                tries.append(0)
                break
            tries.append(got[0])
            if got[0] == success:
                break
        series.append(tries)
        rig.kill(n)
        if a % 20 == 19:
            flat = [s for t in series for s in t]
            print(f"  {kind}: {a + 1} animals, {flat.count(success)} tamed in {len(flat)} tries",
                  flush=True)
    return series


def summarize(series: list[list[int]], success: int) -> dict:
    flat = [s for t in series for s in t]
    return {"animals": len(series), "tries": len(flat), "tamed": flat.count(success),
            "timeouts": flat.count(0), "series": series}


def campaign_tame(rig: Oracle) -> dict:
    out = {}
    out["wolf"] = summarize(tame_series(rig, "wolf", "minecraft:bone", 150, BASE), 7)
    out["cat"] = summarize(tame_series(rig, "cat", "minecraft:cod", 150,
                                       BASE + ',variant:"minecraft:tabby"'), 7)
    out["cat_salmon"] = summarize(tame_series(rig, "cat", "minecraft:salmon", 30,
                                              BASE + ',variant:"minecraft:tabby"'), 7)
    out["parrot"] = summarize(tame_series(rig, "parrot", "minecraft:wheat_seeds", 60,
                                          BASE + ",Variant:0", cap=120), 7)
    for k, v in out.items():
        print(f"  {k}: {v['tamed']}/{v['tries']} ({v['timeouts']} timeouts)", flush=True)
    return out


def campaign_parrot(rig: Oracle) -> dict:
    """The parrot again, on 150 birds: `tame` read 60 tamed in 781 tries,
    z = -2.2 against the documented one in ten — this decides it."""
    s = summarize(tame_series(rig, "parrot", "minecraft:wheat_seeds", 150,
                              BASE + ",Variant:0", cap=150), 7)
    print(f"  parrot: {s['tamed']}/{s['tries']} ({s['timeouts']} timeouts)", flush=True)
    return s


def campaign_ocelot(rig: Oracle) -> dict:
    """Ocelots trust only while their tempt goal runs: AI on, a glass box
    round the probe and the ocelot, cod in hand before the ocelot appears."""
    hand: Rider = rig.hand  # type: ignore[assignment]
    rig.server.batch([f"fill -3 {Y} -3 5 {Y + 2} 4 minecraft:glass",
                      f"fill -2 {Y} -2 4 {Y + 2} 3 minecraft:air",
                      f"tp {PROBE} -0.5 {Y} 0.5 -90 0"])
    series = []
    for a in range(40):
        rig.hold("minecraft:cod", 64)
        n, eid = rig.summon_near("ocelot", "Silent:1b", (2.5, Y, 0.5))
        time.sleep(2.0)
        tries: list[int] = []
        for _ in range(20):
            since = time.monotonic()
            hand.interact(eid)
            got = hand.wait_event(eid, {40, 41}, since, timeout=1.5)
            tries.append(got[0] if got else 0)
            if got and got[0] == 41:
                break
            time.sleep(0.3)
        series.append(tries)
        rig.kill(n)
    rig.server.batch([f"fill -3 {Y} -3 5 {Y + 2} 4 minecraft:air"])
    s = summarize(series, 41)
    s["refused"] = sum(t.count(40) for t in series)
    print(f"  ocelot: {s['tamed']} trusted, {s['refused']} refused, {s['timeouts']} silent",
          flush=True)
    return s


# ── anger ───────────────────────────────────────────────────────────────────

def campaign_anger(rig: Oracle) -> dict:
    hand: Rider = rig.hand  # type: ignore[assignment]
    rig.server.batch(["difficulty normal"])
    singles = []
    for _ in range(40):
        n, eid = rig.summon_near("wolf", "Silent:1b", (1.5, Y, 0.5))
        rig.hold("minecraft:stick", 1)
        time.sleep(0.2)
        hand.attack(eid)
        t0 = rig.gametime()
        raw = rig.datas([(uuid_of(n), "AngerTime"), (uuid_of(n), "AngryAt")])
        t1 = rig.gametime()
        singles.append({"anger": nbt_int(raw[0]), "angry_at": raw[1], "t": [t0, t1]})
        rig.kill(n)
    # A pack: four wolves near, one hit; who is angry a second later.
    packs = []
    for _ in range(6):
        ids = []
        for k in range(4):
            ids.append(rig.summon_near("wolf", "Silent:1b", (1.5 + 2 * k, Y, 2.5 + (k % 2))))
        hand.attack(ids[0][1])
        time.sleep(1.0)
        raw = rig.datas([(uuid_of(n), "AngerTime") for n, _ in ids])
        packs.append([nbt_int(r) for r in raw])
        for n, _ in ids:
            rig.kill(n)
    rig.server.batch(["difficulty peaceful"])
    values = [s["anger"] for s in singles if s["anger"] is not None]
    print(f"  anger: {len(values)} read, min {min(values) if values else None}, "
          f"max {max(values) if values else None}; packs {packs}", flush=True)
    return {"singles": singles, "packs": packs}


# ── wolf ────────────────────────────────────────────────────────────────────

def campaign_wolf(rig: Oracle) -> dict:
    hand: Rider = rig.hand  # type: ignore[assignment]
    out: dict = {}
    n, eid = rig.summon_near("wolf", BASE, (1.5, Y, 0.5))
    out["wild_max_health"] = rig.attributes(uuid_of(n), ["generic.max_health"])[0]
    rig.kill(n)
    n, eid = rig.summon_near("wolf", BASE + "," + OWNER, (1.5, Y, 0.5))
    out["tame_max_health"] = rig.attributes(uuid_of(n), ["generic.max_health",
                                                        "generic.attack_damage"])
    rig.kill(n)
    # Taming by bone: what health and state after.
    n, eid = rig.summon_near("wolf", BASE + ",Health:6f", (1.5, Y, 0.5))
    rig.hold("minecraft:bone", 64)
    for _ in range(40):
        since = time.monotonic()
        hand.interact(eid)
        got = hand.wait_event(eid, {6, 7}, since)
        if got and got[0] == 7:
            break
    raw = rig.datas([(uuid_of(n), "")])
    out["after_tame"] = raw[0]
    out["after_tame_max_health"] = rig.attributes(uuid_of(n), ["generic.max_health"])[0]
    rig.kill(n)
    # Heal per food.
    heals = {}
    for food in ["cooked_beef", "beef", "rotten_flesh", "chicken", "cooked_porkchop", "mutton",
                 "rabbit_stew", "bone", "cod", "cookie", "bread"]:
        n, eid = rig.summon_near("wolf", BASE + "," + OWNER + ",Health:4f", (1.5, Y, 0.5))
        rig.hold(f"minecraft:{food}", 2)
        time.sleep(0.15)
        hand.interact(eid)
        time.sleep(0.4)
        raw = rig.datas([(uuid_of(n), "Health"), (uuid_of(n), "Sitting")])
        held = rig.held()
        heals[food] = {"health": raw[0], "sitting": raw[1], "held": held}
        rig.kill(n)
    out["heal"] = heals
    # Full health, adult, tame, fed beef: love?
    n, eid = rig.summon_near("wolf", BASE + "," + OWNER + ",Health:20f", (1.5, Y, 0.5))
    rig.hold("minecraft:cooked_beef", 2)
    time.sleep(0.15)
    since = time.monotonic()
    hand.interact(eid)
    time.sleep(0.4)
    out["full_fed"] = {"inlove": rig.data(uuid_of(n), "InLove"), "held": rig.held(),
                       "events": [s for t, e, s in hand.events if e == eid and t >= since]}
    rig.kill(n)
    # Sitting toggled by an empty hand, twice; and by a stick.
    n, eid = rig.summon_near("wolf", BASE + "," + OWNER, (1.5, Y, 0.5))
    rig.server.batch(["item replace entity ovhand weapon.mainhand with minecraft:air"])
    sits = []
    for _ in range(3):
        hand.interact(eid)
        time.sleep(0.3)
        sits.append(rig.data(uuid_of(n), "Sitting"))
    rig.hold("minecraft:stick", 1)
    hand.interact(eid)
    time.sleep(0.3)
    sits.append(rig.data(uuid_of(n), "Sitting"))
    out["sit_toggle"] = sits
    # Collar dye.
    rig.hold("minecraft:red_dye", 2)
    hand.interact(eid)
    time.sleep(0.3)
    out["collar"] = {"CollarColor": rig.data(uuid_of(n), "CollarColor"), "held": rig.held(),
                     "sitting": rig.data(uuid_of(n), "Sitting")}
    rig.hold("minecraft:red_dye", 2)
    hand.interact(eid)
    time.sleep(0.3)
    out["collar_again"] = {"held": rig.held()}
    rig.kill(n)
    # A wild wolf offered meat; a tame one of someone else's.
    n, eid = rig.summon_near("wolf", BASE + ",Health:4f", (1.5, Y, 0.5))
    rig.hold("minecraft:cooked_beef", 2)
    hand.interact(eid)
    time.sleep(0.3)
    out["wild_fed"] = {"health": rig.data(uuid_of(n), "Health"), "held": rig.held()}
    rig.kill(n)
    # Baby tame wolf fed: grows?
    n, eid = rig.summon_near("wolf", BASE + "," + OWNER + ",Age:-24000,Health:8f",
                             (1.5, Y, 0.5))
    rig.hold("minecraft:cooked_beef", 2)
    hand.interact(eid)
    time.sleep(0.3)
    out["baby_fed"] = {"age": rig.data(uuid_of(n), "Age"), "held": rig.held(),
                       "health": rig.data(uuid_of(n), "Health")}
    rig.kill(n)
    # Two tame wolves in love: a pup, and whose.
    rig.server.batch([f"fill 20 {Y} 20 23 {Y + 1} 22 minecraft:glass",
                      f"fill 21 {Y} 21 22 {Y + 1} 21 minecraft:air"])
    a = rig.summon_near("wolf", f"Silent:1b,{OWNER},InLove:600,Health:20f", (21.5, Y, 21.5))
    b = rig.summon_near("wolf", f"Silent:1b,{OWNER},InLove:600,Health:20f", (22.5, Y, 21.5))
    t0 = time.monotonic()
    pup = None
    while time.monotonic() - t0 < 15 and pup is None:
        with hand.state_lock:
            for eid2, s in hand.spawns.items():
                if s["type"] == 116 and s["t"] > t0 and eid2 not in (a[1], b[1]):
                    pup = eid2
        time.sleep(0.2)
    out["breed"] = {"pup_seen": pup is not None,
                    "seconds": round(time.monotonic() - t0, 1)}
    if pup is not None:
        lines = rig.ask([f"data get entity @e[type=minecraft:wolf,x=22,y={Y},z=21.5,"
                         f"distance=..3,sort=arbitrary] Age"])
        out["breed"]["ages"] = lines[0]
        lines = rig.ask([f"execute as @e[type=minecraft:wolf,x=22,y={Y},z=21.5,distance=..3] "
                         f"run data get entity @s Owner"])
        out["breed"]["owners"] = lines[0]
    rig.server.batch([f"kill @e[type=minecraft:wolf]", f"fill 20 {Y} 20 23 {Y + 1} 22 minecraft:air"])
    print(json.dumps(out, indent=None)[:3000], flush=True)
    return out


# ── follow ──────────────────────────────────────────────────────────────────

def campaign_follow(rig: Oracle) -> dict:
    out = []
    for gap in (6, 9, 11, 13, 20):
        rig.server.batch([f"tp {PROBE} -0.5 {Y} 0.5 -90 0"])
        time.sleep(0.5)
        n, eid = rig.summon_near("wolf", f"Silent:1b,{OWNER}", (0.5, Y, 0.5))
        time.sleep(1.0)
        rig.server.batch([f"tp {PROBE} {0.5 + gap} {Y} 0.5 -90 0"])
        t0 = rig.gametime()
        samples = []
        for _ in range(20):
            raw = rig.datas([(uuid_of(n), "Pos")])
            samples.append((rig.gametime() - t0, pos_of(raw[0])))
            time.sleep(0.25)
        out.append({"gap": gap, "samples": samples})
        print(f"  follow {gap}: {samples[0]} … {samples[-1]}", flush=True)
        rig.kill(n)
    rig.server.batch([f"tp {PROBE} -0.5 {Y} 0.5 -90 0"])
    return {"runs": out}


# ── spawn ───────────────────────────────────────────────────────────────────

HORSE_ATTRIBUTES = ["generic.max_health", "generic.movement_speed", "horse.jump_strength"]


def campaign_spawn(rig: Oracle) -> dict:
    out: dict = {}
    for kind, count in (("horse", 200), ("donkey", 80), ("mule", 40), ("llama", 120),
                        ("trader_llama", 40)):
        rows = []
        for start in range(0, count, 20):
            spots = []
            for k in range(start, min(count, start + 20)):
                x, z = 100.5 + (k % 20) * 6, 100.5 + (k // 20) * 6
                spots.append((x, z))
            rig.server.batch([f"summon minecraft:{kind} {x} {Y} {z}" for x, z in spots])
            for x, z in spots:
                sel = f"@e[type=minecraft:{kind},x={x},y={Y},z={z},sort=nearest,limit=1]"
                values = rig.attributes(sel, HORSE_ATTRIBUTES)
                extra = rig.ask([f"data get entity {sel} Variant",
                                 f"data get entity {sel} Strength",
                                 f"data get entity {sel} Age"])
                rows.append({"health": values[0], "speed": values[1], "jump": values[2],
                             "variant": nbt_int(value_of(extra[0])),
                             "strength": nbt_int(value_of(extra[1])),
                             "age": nbt_int(value_of(extra[2]))})
            rig.server.batch([f"kill @e[type=minecraft:{kind}]"])
        out[kind] = rows
        hs = [r["health"] for r in rows if r["health"] is not None]
        print(f"  {kind}: {len(rows)} read, health {min(hs) if hs else None}..{max(hs) if hs else None}",
              flush=True)
    return out


# ── breed ───────────────────────────────────────────────────────────────────

def attrs_nbt(h: float, s: float, j: float) -> str:
    return (f'Health:{h}f,Attributes:[{{Name:"minecraft:generic.max_health",Base:{h}d}},'
            f'{{Name:"minecraft:generic.movement_speed",Base:{s}d}},'
            f'{{Name:"minecraft:horse.jump_strength",Base:{j}d}}]')


BREED_CONFIGS = [
    ("apart", "horse", "horse", (20.0, 0.2, 0.5), (28.0, 0.3, 0.9), 60),
    ("equal", "horse", "horse", (24.0, 0.25, 0.7), (24.0, 0.25, 0.7), 60),
    ("mule", "horse", "donkey", (22.0, 0.22, 0.6), (20.0, 0.175, 0.5), 12),
]


def campaign_breed(rig: Oracle) -> dict:
    out: dict = {}
    for name, a_kind, b_kind, a, b, pairs in BREED_CONFIGS:
        # Interiors of 4 × 2: a horse is 1.4 wide, a one-deep pen holds none.
        corners = rig.pens(-70, -70, 10, (pairs + 9) // 10, 4, 2, 6, 5)[:pairs]
        cmds = []
        for x, z in corners:
            common = "Tame:1b,InLove:600,Age:0,Silent:1b,PersistenceRequired:1b,Tags:[\"par\"]"
            cmds.append(f"summon minecraft:{a_kind} {x + 0.8} {Y} {z + 1.0} "
                        f"{{{common},{attrs_nbt(*a)}}}")
            cmds.append(f"summon minecraft:{b_kind} {x + 3.2} {Y} {z + 1.0} "
                        f"{{{common},{attrs_nbt(*b)}}}")
        rig.server.batch(cmds, timeout=120)
        time.sleep(25.0)
        foals = []
        for x, z in corners:
            for kind in ("horse", "donkey", "mule"):
                sel = (f"@e[type=minecraft:{kind},tag=!par,x={x},y={Y},z={z},"
                       f"dx=3,dy=2,dz=1,limit=1]")
                values = rig.attributes(sel, HORSE_ATTRIBUTES)
                if values[0] is not None:
                    age = rig.ask([f"data get entity {sel} Age", f"data get entity {sel} Variant"])
                    foals.append({"kind": kind, "health": values[0], "speed": values[1],
                                  "jump": values[2], "age": nbt_int(value_of(age[0])),
                                  "variant": nbt_int(value_of(age[1]))})
        out[name] = {"a": a, "b": b, "kinds": [a_kind, b_kind], "pairs": pairs, "foals": foals}
        print(f"  {name}: {len(foals)} foals of {pairs} pairs", flush=True)
        rig.server.batch(["kill @e[type=!minecraft:player]"])
        rig.clear(-72, -72, 0, 0)
    return out


# ── temper ──────────────────────────────────────────────────────────────────

def campaign_temper(rig: Oracle) -> dict:
    hand: Rider = rig.hand  # type: ignore[assignment]
    out: dict = {}
    for kind, animals, nbt in (("horse", 25, "Variant:0"), ("donkey", 8, ""),
                               ("llama", 10, "Variant:0,Strength:3")):
        runs = []
        for _ in range(animals):
            rig.server.batch([f"tp {PROBE} -0.5 {Y} 0.5 -90 0",
                              f"fill 0 {Y} -2 4 {Y + 2} 2 minecraft:glass",
                              f"fill 1 {Y} -1 3 {Y + 2} 1 minecraft:air",
                              "item replace entity ovhand weapon.mainhand with minecraft:air"])
            n, eid = rig.summon_near(kind, "Silent:1b" + ("," + nbt if nbt else ""),
                                     (2.0, Y, 0.0))
            tries = []
            for _ in range(20):
                rig.server.batch([f"tp {PROBE} -0.5 {Y} 0.5 -90 0"])
                time.sleep(0.3)
                since = time.monotonic()
                t_game = hand.ticks()
                hand.interact(eid)
                mounted = hand.wait_passengers(eid, since, True, 3.0)
                got = hand.wait_event(eid, {6, 7}, since, timeout=30.0)
                off = hand.wait_passengers(eid, since, False, 0.5)
                temper = nbt_int(rig.data(uuid_of(n), "Temper"))
                tame = rig.data(uuid_of(n), "Tame")
                tries.append({"mounted": mounted is not None,
                              "status": got[0] if got else None,
                              "seconds": round(got[1] - since, 2) if got else None,
                              "ticks": (hand.ticks() - t_game) if (got and t_game) else None,
                              "thrown": off is not None, "temper": temper, "tame": tame})
                if got is None or got[0] == 7:
                    break
            runs.append(tries)
            print(f"  {kind}: {[t['status'] for t in tries]} tempers {[t['temper'] for t in tries]}",
                  flush=True)
            rig.server.batch([f"ride {PROBE} dismount"])
            rig.kill(n)
        out[kind] = runs
    rig.server.batch([f"fill 0 {Y} -2 4 {Y + 2} 2 minecraft:air", f"tp {PROBE} -0.5 {Y} 0.5 -90 0"])
    return out


def campaign_ride_timing(rig: Oracle) -> dict:
    """How long a ridden wild horse takes to decide, to the tick.

    `temper` read the game time from the probe's copy, which the server only
    sends once a second: both ends of a ride were up to 19 ticks stale, and its
    mean of 78 carries that. Here the server is asked itself (`time query
    gametime`, run on its next tick) as soon as the probe is seated and as
    soon as the throw or the hearts arrive: good to about a tick at each end.
    Only each fresh horse's first ride counts — at temper 0 it always ends in
    a throw, and a fresh horse is a fresh sample.
    """
    hand: Rider = rig.hand  # type: ignore[assignment]
    rides = []
    for _ in range(30):
        rig.server.batch([f"tp {PROBE} -0.5 {Y} 0.5 -90 0",
                          f"fill 0 {Y} -2 4 {Y + 2} 2 minecraft:glass",
                          f"fill 1 {Y} -1 3 {Y + 2} 1 minecraft:air",
                          "item replace entity ovhand weapon.mainhand with minecraft:air"])
        n, eid = rig.summon_near("horse", "Silent:1b,Variant:0", (2.0, Y, 0.0))
        time.sleep(0.3)
        since = time.monotonic()
        hand.interact(eid)
        mounted = hand.wait_passengers(eid, since, True, 3.0)
        t0 = rig.gametime() if mounted is not None else None
        got = hand.wait_event(eid, {6, 7}, since, timeout=60.0) if mounted is not None else None
        t1 = rig.gametime() if got else None
        ticks = (t1 - t0) if (t0 is not None and t1 is not None) else None
        rides.append({"status": got[0] if got else None, "ticks": ticks})
        print(f"  horse {len(rides)}: status {rides[-1]['status']}, {ticks} ticks", flush=True)
        rig.server.batch([f"ride {PROBE} dismount"])
        rig.kill(n)
    rig.server.batch([f"fill 0 {Y} -2 4 {Y + 2} 2 minecraft:air", f"tp {PROBE} -0.5 {Y} 0.5 -90 0"])
    delays = [r["ticks"] for r in rides if r["ticks"] is not None]
    if delays:
        print(f"  {len(delays)} decisions: mean {sum(delays) / len(delays):.1f}, "
              f"sorted {sorted(delays)}", flush=True)
    return {"rides": rides}


# ── zoo ─────────────────────────────────────────────────────────────────────

ZOO = [
    ("wolf", "{" + OWNER + ",Sitting:1b,CollarColor:14b,Health:20f}"),
    ("wolf", "{AngerTime:0}"),
    ("cat", '{variant:"minecraft:calico",' + OWNER + ",Sitting:1b,CollarColor:3b}"),
    ("ocelot", "{Trusting:1b}"),
    ("horse", '{Variant:515,Tame:1b,' + OWNER + ',Temper:15,SaddleItem:{id:"minecraft:saddle",'
     'Count:1b},ArmorItem:{id:"minecraft:golden_horse_armor",Count:1b},Bred:1b}'),
    ("donkey", '{Tame:1b,ChestedHorse:1b,Items:[{Slot:2b,id:"minecraft:apple",Count:5b}]}'),
    ("mule", "{Tame:1b}"),
    ("llama", '{Variant:3,Strength:4,Tame:1b,DecorItem:{id:"minecraft:blue_carpet",Count:1b}}'),
    ("trader_llama", "{Variant:1,Strength:2,DespawnDelay:0}"),
    ("rabbit", "{RabbitType:3,MoreCarrotTicks:0}"),
    ("fox", '{Type:"snow",Sleeping:1b}'),
    ("parrot", "{Variant:2," + OWNER + ",Sitting:1b}"),
    ("turtle", "{HasEgg:1b,HomePosX:4,HomePosY:-60,HomePosZ:4}"),
    ("bee", "{HasNectar:1b,CannotEnterHiveTicks:0}"),
    ("goat", "{IsScreamingGoat:1b,HasLeftHorn:1b,HasRightHorn:0b}"),
    ("camel", '{SaddleItem:{id:"minecraft:saddle",Count:1b}}'),
]


def campaign_zoo(rig: Oracle) -> dict:
    cmds = []
    for i, (mob, nbt) in enumerate(ZOO):
        extra = nbt[1:-1]
        merged = ("{NoAI:1b,PersistenceRequired:1b,Silent:1b,Tags:[\"tamezoo\"]"
                  + ("," + extra if extra else "") + "}")
        cmds.append(f"summon minecraft:{mob} {4.5 + (i % 6) * 3} {Y} {-10.5 - (i // 6) * 3} {merged}")
    rig.server.batch(cmds, timeout=60)
    time.sleep(2.0)
    dump = {}
    for i, (mob, _) in enumerate(ZOO):
        x = 4.5 + (i % 6) * 3
        z = -10.5 - (i // 6) * 3
        lines = rig.ask([f"data get entity @e[tag=tamezoo,limit=1,sort=nearest,x={x},y={Y},z={z}]"])
        dump[f"{i}:{mob}"] = value_of(lines[0])
        print(f"  {mob}: {(dump[f'{i}:{mob}'] or '')[:160]}", flush=True)
    rig.server.batch(["save-all flush"], timeout=120)
    time.sleep(2.0)
    if ZOO_COPY.exists():
        shutil.rmtree(ZOO_COPY)
    shutil.copytree(RUN / "world", ZOO_COPY)
    rig.server.batch(["kill @e[tag=tamezoo]"])
    return {"zoo": [m for m, _ in ZOO], "data_get": dump}


def campaign_noai(rig: Oracle) -> dict:
    """`NoAI` on a Mob: no brain *and* no physics.

    A zombie summoned six blocks up with `NoAI` should stay there; a cow given
    `Motion:[0.5,0,0]` should not move while its Motion decays by 0.98 a tick
    (explosions.md § 4 saw the decay on knocked-back zombies). And a zombie
    without `NoAI`: does vanilla write the key as 0b, or leave it out?
    """
    x0, z0 = 40.5, 40.5
    # Cows, not zombies: the rig runs on peaceful, where the real server
    # deletes a hostile mob the moment it appears (the first run read no
    # zombie at all, and "absent" for a key on an entity that was gone).
    rig.server.batch([
        f'summon minecraft:cow {x0} {Y + 6} {z0} {{NoAI:1b,Silent:1b,Tags:["noai_air"]}}',
        f'summon minecraft:cow {x0 + 4} {Y} {z0} '
        f'{{NoAI:1b,Silent:1b,Motion:[0.5d,0.0d,0.0d],Tags:["noai_push"]}}',
        f'summon minecraft:cow {x0 + 8} {Y} {z0} {{Silent:1b,Tags:["with_ai"]}}',
    ])
    samples = []
    for _ in range(8):
        raw = rig.ask(["time query gametime",
                       "data get entity @e[tag=noai_air,limit=1] Pos",
                       "data get entity @e[tag=noai_air,limit=1] Motion",
                       "data get entity @e[tag=noai_push,limit=1] Pos",
                       "data get entity @e[tag=noai_push,limit=1] Motion"])
        time_match = GAMETIME.search(" ".join(raw[0]))
        samples.append({
            "gametime": int(time_match.group(1)) if time_match else None,
            # pos_of reads one string; each reply here is a list of lines.
            "air_pos": pos_of(" ".join(raw[1])), "air_motion": pos_of(" ".join(raw[2])),
            "push_pos": pos_of(" ".join(raw[3])), "push_motion": pos_of(" ".join(raw[4])),
        })
        print(f"  {samples[-1]}", flush=True)
        time.sleep(0.5)
    full = value_of(rig.ask(["data get entity @e[tag=with_ai,limit=1]"])[0])
    rig.server.batch(["kill @e[tag=noai_air]", "kill @e[tag=noai_push]", "kill @e[tag=with_ai]"])
    if full is None:
        key = "mob missing"  # nothing was read: no answer, not "absent"
    elif "NoAI: 0b" in full:
        key = "NoAI: 0b"
    elif "NoAI: 1b" in full:
        key = "NoAI: 1b"
    else:
        key = "absent"
    print(f"  NoAI on a mob without it: {key}", flush=True)
    return {"samples": samples, "without_noai": key}


OURS_ZOO = ROOT / ".scratch" / "tame-zoo-ours"


def campaign_zoo_back(rig: Oracle) -> dict:
    """Vanilla reads the zoo back from the world **our** server rewrote.

    `check_tame_e2e.py zoo` loads the vanilla zoo into ov_dedicated, which
    saves it through its own entities/ writer; this campaign runs on that
    world (see `main`) and reads every zoo mob with `data get`.
    """
    rig.server.batch([f"tp {PROBE} 4.5 {Y} -12.0"], timeout=30)
    time.sleep(3.0)
    # By type, not by the spot each was summoned at: ov_dedicated moved the
    # standing ones while the world was loaded (only the sitting wolf, cat and
    # parrot were still on their spots — the first run's positional lookup).
    owner = "Owner: [I; 289470649, -236178987, -1494576147, 376949766]"
    expected = {
        "wolf": ("type=minecraft:wolf,nbt={Sitting:1b}", [owner, "Sitting: 1b", "CollarColor: 14b"]),
        "cat": ("type=minecraft:cat", [owner, "Sitting: 1b", 'variant: "minecraft:calico"',
                                       "CollarColor: 3b"]),
        "ocelot": ("type=minecraft:ocelot", ["Trusting: 1b"]),
        "horse": ("type=minecraft:horse", ["Variant: 515", "Tame: 1b", "Temper: 15", owner,
                                           'SaddleItem: {id: "minecraft:saddle"',
                                           'ArmorItem: {id: "minecraft:golden_horse_armor"',
                                           "Bred: 1b"]),
        "donkey": ("type=minecraft:donkey", ["Tame: 1b", "ChestedHorse: 1b",
                                             'id: "minecraft:apple"']),
        "mule": ("type=minecraft:mule", ["Tame: 1b"]),
        "llama": ("type=minecraft:llama", ["Variant: 3", "Strength: 4", "Tame: 1b",
                                           'DecorItem: {id: "minecraft:blue_carpet"']),
        "rabbit": ("type=minecraft:rabbit", ["RabbitType: 3"]),
        "fox": ("type=minecraft:fox", ['Type: "snow"', "Sleeping: 1b"]),
        "parrot": ("type=minecraft:parrot", ["Variant: 2", owner, "Sitting: 1b"]),
        "turtle": ("type=minecraft:turtle", ["HasEgg: 1b"]),
        "bee": ("type=minecraft:bee", ["HasNectar: 1b"]),
        "goat": ("type=minecraft:goat", ["IsScreamingGoat: 1b", "HasLeftHorn: 1b",
                                         "HasRightHorn: 0b"]),
        "camel": ("type=minecraft:camel", ['SaddleItem: {id: "minecraft:saddle"']),
    }
    dump, verdict = {}, {}
    for mob, (selector, fields) in expected.items():
        lines = rig.ask([f"data get entity @e[tag=tamezoo,{selector},limit=1]"])
        text = value_of(lines[0])
        dump[mob] = text
        missing = [f for f in fields if text is None or f not in text]
        verdict[mob] = "ok" if not missing else ("ABSENT" if text is None else f"missing {missing}")
        print(f"  {mob}: {verdict[mob]}", flush=True)
    count = rig.count("@e[tag=tamezoo]")
    ok = sum(1 for v in verdict.values() if v == "ok")
    print(f"  {ok}/{len(expected)} read back with every field; {count} zoo mobs loaded", flush=True)
    return {"zoo_count": count, "verdict": verdict, "data_get": dump}


CAMPAIGNS = {"meta": campaign_meta, "tame": campaign_tame, "parrot": campaign_parrot,
             "ocelot": campaign_ocelot,
             "anger": campaign_anger, "wolf": campaign_wolf, "follow": campaign_follow,
             "spawn": campaign_spawn, "breed": campaign_breed, "temper": campaign_temper,
             "zoo": campaign_zoo, "zoo_back": campaign_zoo_back, "noai": campaign_noai,
             "ride_timing": campaign_ride_timing}


def main(argv: list[str]) -> int:
    names = argv or [n for n in CAMPAIGNS if n != "zoo_back"]
    results = json.loads(OUT.read_text()) if OUT.exists() else {}
    if RUN.exists():
        shutil.rmtree(RUN)
    if names == ["zoo_back"]:
        # Vanilla on the world ov_dedicated rewrote, copied in as `world`.
        if not OURS_ZOO.exists():
            print(f"no {OURS_ZOO}: run check_tame_e2e.py zoo first")
            return 1
        RUN.mkdir(parents=True)
        shutil.copytree(OURS_ZOO, RUN / "world")
        (RUN / "world" / "session.lock").unlink(missing_ok=True)
    server = FlatServer(RUN, port=PORT)
    hand = None
    try:
        server.batch(["gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                      "gamerule doWeatherCycle false", "gamerule sendCommandFeedback true",
                      "gamerule doInsomnia false", "gamerule doPatrolSpawning false",
                      "gamerule doTraderSpawning false", "gamerule naturalRegeneration false",
                      "difficulty peaceful", "time set noon", "setworldspawn 0 -60 0",
                      "scoreboard objectives add ovcount dummy",
                      "forceload add -128 -144 255 255"])
        time.sleep(5.0)
        hand = Rider(PORT, PROBE)
        time.sleep(2.0)
        rig = Oracle(server, hand)
        server.batch([f"tp {PROBE} -0.5 {Y} 0.5 -90 0", f"gamemode survival {PROBE}",
                      f"effect give {PROBE} minecraft:resistance infinite 5 true"])
        if names != ["zoo_back"]:
            # A fresh world starts empty. Not the one ov_dedicated rewrote: the
            # forceload above has already brought its zoo in, and the first
            # zoo_back run killed it here and read back nothing (zoo_count 0).
            server.batch(["kill @e[type=!minecraft:player]"])
        time.sleep(1.0)
        for name in names:
            print(f"── {name}", flush=True)
            started = time.monotonic()
            try:
                results[name] = CAMPAIGNS[name](rig)
            except Exception as error:  # keep what the others measured
                print(f"  {name} FAILED: {error!r}", flush=True)
                results[name] = {"error": repr(error)}
            results[name]["_seconds"] = round(time.monotonic() - started, 1)
            OUT.parent.mkdir(parents=True, exist_ok=True)
            OUT.write_text(json.dumps(results, indent=1))
            server.batch(["kill @e[type=!minecraft:player]", f"tp {PROBE} -0.5 {Y} 0.5 -90 0"])
    finally:
        if hand is not None:
            hand.close()
        server.stop()
        shutil.rmtree(RUN, ignore_errors=True)
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
