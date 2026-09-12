#!/usr/bin/env python3
"""Ask a real 1.20.1 server how its villagers live: gossip, reputation, the day,
beds and job sites, breeding, iron golems, the wandering trader, and the type a
biome gives.

The probe and the console helpers are measure_villagers.py's. Every campaign
reads what the game itself says — `data get entity`, the wire — and each keeps
a failing control next to what it measures (a gossip about someone else, a
villager that has not slept, a bed that is taken).

  price     Merchant Offers' special price under gossip about the probe: each of
            the five gossip types alone, mixed, clamped; gossip about another
            player (control); Hero of the Village at amplifiers 0, 1, 4.
  events    What a trade, a hit and a killing write into Gossips.
  decay     `LastGossipDecay` due, not yet due, and zero: what each type loses.
  share     Two villagers at a bell in meeting hours: does B learn A's gossip,
            and at what value.
  schedule  A librarian in a room with a bed, a lectern and a bell, the daylight
            cycle on: the day time of each transition (work, meet, rest, wake),
            the pose, and the Brain memories as saved.
  breed     Pairs with bread, carrots, too little food, and no free bed: time to
            a baby, food left, the baby's Age and type.
  breed_type  24 desert pairs in plains: the baby's type, the biome's or a
            parent's (12 more came from a first, smaller run).
  farm      A farmer at a composter by ripe wheat: harvested, replanted, what it
            carries.
  golem     Villagers that slept recently panic at a zombie: 3 of them (golem),
            3 unslept, 2 slept, 3 slept by a golem (controls).
  golem_meet Five slept villagers meeting at a bell (golem), five unslept.
  anger     An iron golem by a villager that dislikes the probe, on Easy: its
            swings (entity event 4) at reputation -500, -100, -99, -50.
  trader    Wandering traders summoned: their offers (sampled), DespawnDelay.
  biome     A villager summoned without VillagerData in each of 50 biomes.

Usage: python3 scripts/measure_villager_life.py [campaign ...]   (default: all)
Run it in the java lane; each campaign is a few minutes.

Writes .scratch/villager_life.json (untracked), merging into what is there.
"""
from __future__ import annotations

import json
import re
import shutil
import struct
import sys
import time
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_entity_packets import read_varint  # noqa: E402
from measure_husbandry import DATA, Rig, uuid_of  # noqa: E402
from measure_mobs import FlatServer  # noqa: E402
from measure_villagers import (Trader, decode_offers, offers_nbt, pos3, recipe,  # noqa: E402
                               snbt, summon, vdata)

ROOT = Path(__file__).resolve().parent.parent
SCRATCH = ROOT / ".scratch"
OUT = SCRATCH / "villager_life.json"
RUN = SCRATCH / "villager-life-oracle"
PORT = 25724
Y = -60
IRON_GOLEM, VILLAGER, WANDERING_TRADER = 53, 108, 110  # checked against registries.json

HELMET = 'ArmorItems:[{},{},{},{id:"minecraft:leather_helmet",Count:1b}]'


class Clocked(Trader):
    """The trading probe, plus the day time of every Update Time."""

    def __init__(self, port: int, name: str = "ovhand") -> None:
        self.day: tuple[int, int, float] | None = None
        super().__init__(port, name)

    def read(self):  # type: ignore[override]
        pid, p = super().read()
        if pid == 0x5E and len(p) >= 16:
            age, day = struct.unpack_from(">qq", p, 0)
            self.day = (age, day, time.monotonic())
        return pid, p


def uuid_ints(raw: str | None) -> list[int]:
    return [int(v) for v in re.findall(r"-?\d+", raw or "")][:4]


def target(ints: list[int]) -> str:
    return "[I;" + ",".join(str(v) for v in ints) + "]"


def gossips(entries: list[tuple[str, int]], who: list[int]) -> str:
    body = ",".join(f'{{Type:"{t}",Target:{target(who)},Value:{v}}}' for t, v in entries)
    return f"Gossips:[{body}]"


def memories(**values: int) -> str:
    """Brain memories that are plain longs (last_slept, last_woken …)."""
    body = ",".join(f'"minecraft:{k}":{{value:{v}L}}' for k, v in values.items())
    return f"Brain:{{memories:{{{body}}}}}"


def offers_in(hand: Clocked) -> dict | None:
    for _, pid, p in hand.rec:
        if pid == 0x2A:
            try:
                return decode_offers(p)
            except Exception as error:
                return {"error": str(error)}
    return None


def fence_pen(rig: Rig, x: int, z: int) -> list[str]:
    return [f"fill {x - 1} {Y} {z - 1} {x + 1} {Y} {z + 1} minecraft:oak_fence",
            f"setblock {x} {Y} {z} minecraft:air"]


def box(rig: Rig, x0: int, z0: int, x1: int, z1: int, height: int = 3) -> None:
    """A glass room with a roof, the inside cleared."""
    rig.server.batch([
        f"fill {x0 - 1} {Y} {z0 - 1} {x1 + 1} {Y + height} {z1 + 1} minecraft:glass",
        f"fill {x0} {Y} {z0} {x1} {Y + height - 1} {z1} minecraft:air"])


def count_in(rig: Rig, kind: str, x: int, z: int, dx: int, dz: int) -> int:
    return rig.count(f"@e[type=minecraft:{kind},x={x},y={Y - 8},z={z},dx={dx},dy=24,dz={dz}]")


# ── Campaigns ───────────────────────────────────────────────────────────────

PRICE_RECIPES = (
    recipe("paper", 24, "emerald", 1, max_uses=16, xp=2, mult=0.05),
    recipe("emerald", 10, "bookshelf", 1, max_uses=12, xp=5, mult=0.2),
    recipe("emerald", 1, "lantern", 1, max_uses=12, xp=5, mult=0.05),
    recipe("emerald", 5, "glass", 4, max_uses=12, xp=5, mult=0.05),
    recipe("emerald", 20, "bell", 1, max_uses=12, xp=5, mult=0.2),
)


def campaign_price(rig: Rig) -> dict:
    hand: Clocked = rig.hand  # type: ignore[assignment]
    me = uuid_ints(rig.data("ovhand", "UUID"))
    other = [11, 22, 33, 44]
    out: dict = {"probe_uuid": me, "cells": []}
    cells: list[tuple[str, list[tuple[str, int]], list[int], int | None]] = [
        ("none", [], me, None),
        ("minor_positive 10", [("minor_positive", 10)], me, None),
        ("minor_positive 100", [("minor_positive", 100)], me, None),
        ("minor_positive 200", [("minor_positive", 200)], me, None),
        ("minor_positive 250", [("minor_positive", 250)], me, None),
        ("major_positive 20", [("major_positive", 20)], me, None),
        ("major_positive 100", [("major_positive", 100)], me, None),
        ("trading 25", [("trading", 25)], me, None),
        ("trading 7", [("trading", 7)], me, None),
        ("minor_negative 25", [("minor_negative", 25)], me, None),
        ("major_negative 25", [("major_negative", 25)], me, None),
        ("cured", [("major_positive", 20), ("minor_positive", 25)], me, None),
        ("all five", [("major_positive", 10), ("minor_positive", 30), ("trading", 12),
                      ("minor_negative", 40), ("major_negative", 3)], me, None),
        ("other player, minor_positive 100", [("minor_positive", 100)], other, None),
        ("hero 0", [], me, 0),
        ("hero 1", [], me, 1),
        ("hero 4", [], me, 4),
        ("hero 0 + minor_positive 100", [("minor_positive", 100)], me, 0),
    ]
    for k, (label, entries, who, hero) in enumerate(cells):
        z = 4 * k
        rig.server.batch([f"fill 2 {Y} {z - 1} 4 {Y + 2} {z + 1} minecraft:glass",
                          f"fill 3 {Y} {z} 3 {Y + 2} {z} minecraft:air",
                          f"tp ovhand 0.5 -60 {z + 0.5} -90 0"])
        if hero is not None:
            rig.server.batch([f"effect give ovhand minecraft:hero_of_the_village 60 {hero} true"])
        extra = "," + gossips(entries, who) if entries else ""
        n = summon(rig, "villager", 3.5, z + 0.5, "Silent:1b,Xp:1," + vdata(1) + ","
                   + offers_nbt(*PRICE_RECIPES) + extra)
        eid = hand.eid_for(n)
        time.sleep(0.6)
        opened = hand.open(eid)
        time.sleep(0.4)
        decoded = offers_in(hand)
        hand.close_screen()
        time.sleep(0.3)
        after = snbt(rig.data(uuid_of(n), "Offers.Recipes"))
        specials = [t["special_price"] for t in (decoded or {}).get("trades", [])]
        out["cells"].append({"label": label, "gossips": entries, "hero": hero,
                             "opened": opened, "wire": decoded,
                             "special_prices": specials,
                             "saved_after_close": [r.get("specialPrice") for r in after or []]})
        print(f"  {label}: specials {specials}, after close "
              f"{out['cells'][-1]['saved_after_close']}", flush=True)
        if hero is not None:
            rig.server.batch(["effect clear ovhand minecraft:hero_of_the_village"])
        rig.kill(n)
    return out


def campaign_events(rig: Rig) -> dict:
    hand: Clocked = rig.hand  # type: ignore[assignment]
    out: dict = {}
    me = uuid_ints(rig.data("ovhand", "UUID"))
    out["probe_uuid"] = me
    # A trade.
    rig.server.batch([f"fill 2 {Y} 99 4 {Y + 2} 101 minecraft:glass",
                      f"fill 3 {Y} 100 3 {Y + 2} 100 minecraft:air",
                      "tp ovhand 0.5 -60 100.5 -90 0", "clear ovhand",
                      "give ovhand minecraft:paper 64"])
    n = summon(rig, "villager", 3.5, 100.5, "Silent:1b,Xp:1," + vdata(1) + ","
               + offers_nbt(PRICE_RECIPES[0]))
    eid = hand.eid_for(n)
    time.sleep(0.6)
    rows = []
    for k in range(3):
        hand.open(eid)
        hand.select(0)
        time.sleep(0.4)
        hand.click(2)
        time.sleep(0.4)
        hand.close_screen()
        time.sleep(0.4)
        rows.append(snbt(rig.data(uuid_of(n), "Gossips")))
        print(f"  after trade {k + 1}: {rows[-1]}", flush=True)
    out["trades"] = rows
    rig.kill(n)
    # A hit by the probe, then a second.
    rig.server.batch(["kill @e[type=minecraft:experience_orb]"])
    n = summon(rig, "villager", 3.5, 100.5, "Silent:1b,NoAI:1b")
    time.sleep(0.5)
    hits = []
    for k in range(2):
        rig.server.batch([f"damage {uuid_of(n)} 1 minecraft:player_attack by ovhand"])
        time.sleep(0.6)
        hits.append(snbt(rig.data(uuid_of(n), "Gossips")))
        print(f"  after hit {k + 1}: {hits[-1]}", flush=True)
    out["hits"] = hits
    rig.kill(n)
    # A killing, witnessed by villagers at 4, 12 and 20 blocks.
    # Witnesses with their brains (a NoAI villager senses nothing: the first
    # run's witnesses heard nothing), kept in fence pens so they stay put.
    rig.server.batch([f"fill 20 {Y} 90 56 {Y + 3} 110 minecraft:air"])
    # The victim too: the witnesses are the ones *it* sees (a NoAI victim has
    # no senses, and the second run's witnesses heard nothing either).
    pens = fence_pen(rig, 30, 100)
    for d in (4, 12, 20):
        pens += fence_pen(rig, 30 + d, 100)
    rig.server.batch(pens)
    victim = summon(rig, "villager", 30.5, 100.5, "Silent:1b")
    witnesses = {d: summon(rig, "villager", 30.5 + d, 100.5, "Silent:1b")
                 for d in (4, 12, 20)}
    time.sleep(3.0)
    rig.server.batch([f"damage {uuid_of(victim)} 100 minecraft:player_attack by ovhand"])
    time.sleep(1.0)
    out["killed"] = {d: snbt(rig.data(uuid_of(w), "Gossips")) for d, w in witnesses.items()}
    print(f"  witnesses of a killing: {out['killed']}", flush=True)
    rig.server.batch(["kill @e[type=minecraft:villager]", "clear ovhand"])
    return out


def campaign_decay(rig: Rig) -> dict:
    me = uuid_ints(rig.data("ovhand", "UUID"))
    out: dict = {}
    full = [("major_negative", 50), ("minor_negative", 50), ("minor_positive", 50),
            ("major_positive", 50), ("trading", 10)]
    small = [("trading", 1), ("minor_negative", 5), ("minor_positive", 1),
             ("major_negative", 3)]
    now = rig.gametime()
    cells = {
        "due": (full, now - 24000),
        "due_small": (small, now - 24000),
        "due_in_200": (full, now - 23800),
        "zero": (full, 0),
        "long_overdue": (full, now - 100000),
    }
    ids = {}
    # Where a decayed value is dropped: one gossip per target, four targets.
    edge_targets = [[7, 7, 7, k] for k in range(1, 7)]
    edge = [("minor_positive", 2), ("minor_positive", 3), ("trading", 3), ("trading", 4),
            ("minor_negative", 21), ("minor_negative", 22)]
    body = ",".join(f'{{Type:"{t}",Target:{target(w)},Value:{v}}}'
                    for (t, v), w in zip(edge, edge_targets))
    ids["due_edge"] = summon(rig, "villager", 60.5 + 3 * len(cells), 0.5,
                             f"NoAI:1b,Silent:1b,LastGossipDecay:{now - 24000}L,"
                             f"Gossips:[{body}]")
    for k, (label, (entries, last)) in enumerate(cells.items()):
        # A negative long is legal, and needed: a young world's game time is
        # under 24000. The first run clamped these to 0 and so measured only
        # the zero case (0 becomes the current time, nothing decays).
        ids[label] = summon(rig, "villager", 60.5 + 3 * k, 0.5,
                            f"NoAI:1b,Silent:1b,LastGossipDecay:{last}L,"
                            + gossips(entries, me))
    rig.server.batch([])
    cells["due_edge"] = (edge, now - 24000)  # polled with the others (the first run forgot)
    start = rig.gametime()
    polls = []
    t0 = time.monotonic()
    while time.monotonic() - t0 < 16.0:
        q = []
        for label in cells:
            q += [(uuid_of(ids[label]), "Gossips"), (uuid_of(ids[label]), "LastGossipDecay")]
        answers = rig.datas(q)
        row = {"tick": rig.gametime() - start}
        for i, label in enumerate(cells):
            row[label] = {"gossips": snbt(answers[2 * i]), "last": snbt(answers[2 * i + 1])}
        polls.append(row)
        time.sleep(2.0)
    out = {"summoned_at": now, "cells": {k: {"entries": v[0], "last": v[1]}
                                         for k, v in cells.items()}, "polls": polls}
    for label in cells:
        print(f"  {label}: first {polls[0][label]}, last {polls[-1][label]}", flush=True)
    rig.server.batch(["kill @e[type=minecraft:villager]"])
    return out


def campaign_share(rig: Rig) -> dict:
    me = uuid_ints(rig.data("ovhand", "UUID"))
    out: dict = {}
    rig.server.batch(["time set 9500", "forceload add 96 -16 223 95"])
    time.sleep(2.0)
    cells = {"bell": 100, "no_bell": 180}
    ids = {}
    for label, x in cells.items():
        box(rig, x, 0, x + 6, 6)
        if label == "bell":
            rig.server.batch([f"setblock {x + 3} {Y} {3} minecraft:bell"])
        a = summon(rig, "villager", x + 1.5, 1.5, "Silent:1b,"
                   + gossips([("minor_negative", 100), ("trading", 20), ("major_positive", 50),
                              ("major_negative", 40), ("minor_positive", 60)], me))
        b = summon(rig, "villager", x + 5.5, 5.5, "Silent:1b")
        ids[label] = (a, b)
    rig.server.batch([])
    polls = []
    t0 = time.monotonic()
    while time.monotonic() - t0 < 100.0:
        q = []
        for a, b in ids.values():
            q += [(uuid_of(a), "Gossips"), (uuid_of(b), "Gossips"), (uuid_of(b), "Brain.memories")]
        answers = rig.datas(q)
        row: dict = {"t": round(time.monotonic() - t0, 1)}
        for i, label in enumerate(ids):
            row[label] = {"a": snbt(answers[3 * i]), "b": snbt(answers[3 * i + 1]),
                          "b_memories": snbt(answers[3 * i + 2])}
        polls.append(row)
        print(f"  {row['t']} s: bell b {row['bell']['b']}; no bell b {row['no_bell']['b']}",
              flush=True)
        time.sleep(4.0)
    out["polls"] = polls
    rig.server.batch(["kill @e[type=minecraft:villager]", "time set noon",
                      "forceload remove 96 -16 223 95"])
    return out


def campaign_schedule(rig: Rig) -> dict:
    hand: Clocked = rig.hand  # type: ignore[assignment]
    out: dict = {}
    X, Z = 300, 0
    rig.server.batch(["forceload add 288 -16 335 31", "time set 6000"])
    time.sleep(2.0)
    box(rig, X, Z, X + 8, Z + 8)
    rig.server.batch([f"setblock {X + 1} {Y} {Z + 1} minecraft:red_bed[facing=south,part=foot]",
                      f"setblock {X + 1} {Y} {Z + 2} minecraft:red_bed[facing=south,part=head]",
                      f"setblock {X + 7} {Y} {Z + 7} minecraft:lectern",
                      f"setblock {X + 7} {Y} {Z + 1} minecraft:bell"])
    n = summon(rig, "villager", X + 4.5, Z + 4.5, "Silent:1b")
    eid = hand.eid_for(n)
    # Claim at noon: bed, lectern, bell.
    t0 = time.monotonic()
    claimed = None
    while time.monotonic() - t0 < 60.0:
        mem = snbt(rig.data(uuid_of(n), "Brain.memories")) or {}
        prof = snbt(rig.data(uuid_of(n), "VillagerData.profession"))
        if isinstance(mem, dict) and "minecraft:home" in mem and "minecraft:job_site" in mem:
            claimed = mem
            break
        time.sleep(2.0)
    out["claimed"] = claimed
    out["claim_seconds"] = round(time.monotonic() - t0, 1)
    print(f"  claimed after {out['claim_seconds']} s: {claimed}, {prof}", flush=True)

    def watch(start_day: int, seconds: float) -> list:
        rig.server.batch([f"time set {start_day}", "gamerule doDaylightCycle true"])
        rows = []
        t1 = time.monotonic()
        seen_pose = None
        while time.monotonic() - t1 < seconds:
            answers = rig.datas([(uuid_of(n), "Brain.memories"), (uuid_of(n), "Pos")])
            day = hand.day[1] if hand.day else None
            poses = [f for _, f in hand.metadata_of(eid)]
            pose = None
            for fields in poses:
                for index, _kind, value in fields:
                    if index == 6:
                        pose = value
            if pose != seen_pose:
                seen_pose = pose
            rows.append({"day": day, "pose": pose, "memories": snbt(answers[0]),
                         "pos": pos3(snbt(answers[1]))})
            time.sleep(0.5)
        rig.server.batch(["gamerule doDaylightCycle false"])
        return rows

    for label, start, seconds in (("work", 1900, 20.0), ("meet", 8900, 25.0),
                                  ("idle", 10900, 12.0), ("rest", 11900, 40.0),
                                  ("wake", 23900, 20.0)):
        rows = watch(start, seconds)
        out[label] = rows
        changes = []
        last = None
        for r in rows:
            keys = sorted((r["memories"] or {}).keys()) if isinstance(r["memories"], dict) else []
            state = (r["pose"], tuple(keys))
            if state != last:
                changes.append([r["day"], r["pose"], keys])
                last = state
        print(f"  {label}: {changes}", flush=True)
    out["final"] = snbt(rig.data(uuid_of(n)))
    rig.server.batch(["kill @e[type=minecraft:villager]", "time set noon",
                      "forceload remove 288 -16 335 31"])
    return out


def campaign_breed(rig: Rig) -> dict:
    out: dict = {"cells": []}
    # Pens a long way apart: a villager looks for beds 48 blocks round.
    cells = [
        ("bread 3 each, 3 beds", 'Inventory:[{id:"minecraft:bread",Count:3b}]', 3, "plains"),
        ("carrots 12 each, 3 beds", 'Inventory:[{id:"minecraft:carrot",Count:12b}]', 3, "plains"),
        ("bread 2 each, 3 beds", 'Inventory:[{id:"minecraft:bread",Count:2b}]', 3, "plains"),
        ("bread 3 each, 2 beds", 'Inventory:[{id:"minecraft:bread",Count:3b}]', 2, "plains"),
        ("desert parents, bread 6", 'Inventory:[{id:"minecraft:bread",Count:6b}]', 3, "desert"),
        ("potato 12 + beetroot 12", 'Inventory:[{id:"minecraft:potato",Count:12b},'
                                    '{id:"minecraft:beetroot",Count:12b}]', 3, "plains"),
    ]
    xs = [400 + 120 * k for k in range(len(cells))]
    rig.server.batch([f"forceload add {x - 16} -16 {x + 31} 31" for x in xs] + ["time set 1000"])
    time.sleep(4.0)
    ids = []
    for (label, inv, beds, vtype), x in zip(cells, xs):
        box(rig, x, 0, x + 8, 8)
        cmds = []
        for b in range(beds):
            cmds += [f"setblock {x + 1 + 3 * b} {Y} 1 minecraft:red_bed[facing=south,part=foot]",
                     f"setblock {x + 1 + 3 * b} {Y} 2 minecraft:red_bed[facing=south,part=head]"]
        rig.server.batch(cmds)
        a = summon(rig, "villager", x + 2.5, 6.5, f"Silent:1b,{vdata(1, 'none', vtype)},{inv}")
        b = summon(rig, "villager", x + 6.5, 6.5, f"Silent:1b,{vdata(1, 'none', vtype)},{inv}")
        ids.append((a, b))
    rig.server.batch([])
    start = rig.gametime()
    polls = []
    t0 = time.monotonic()
    while time.monotonic() - t0 < 150.0:
        row: dict = {"tick": rig.gametime() - start,
                     "villagers": [count_in(rig, "villager", x, 0, 9, 9) for x in xs]}
        polls.append(row)
        print(f"  tick {row['tick']}: {row['villagers']}", flush=True)
        time.sleep(5.0)
    out["polls"] = polls
    for k, ((label, _, _, _), x, (a, b)) in enumerate(zip(cells, xs, ids)):
        parents = rig.datas([(uuid_of(a), "Inventory"), (uuid_of(a), "Age"),
                             (uuid_of(a), "FoodLevel"), (uuid_of(b), "Inventory"),
                             (uuid_of(b), "Age"), (uuid_of(b), "FoodLevel"),
                             (uuid_of(a), "Brain.memories")])
        # Born, not summoned: the only villager here without PersistenceRequired.
        child = rig.data(f"@e[type=minecraft:villager,x={x},y={Y - 8},z=0,dx=9,dy=24,dz=9,"
                         "limit=1,nbt=!{PersistenceRequired:1b}]")
        first = next((i for i, r in enumerate(polls) if r["villagers"][k] > 2), None)
        out["cells"].append({"label": label, "parents": [snbt(v) for v in parents],
                             "child": snbt(child), "villagers": polls[-1]["villagers"][k],
                             "first_birth_tick": polls[first]["tick"] if first is not None
                             else None})
        c = out["cells"][-1]
        ch = c["child"] if isinstance(c["child"], dict) else {}
        print(f"  {label}: {c['villagers']} villagers; parents {c['parents'][:6]}; child "
              f"Age {ch.get('Age')} {ch.get('VillagerData')}", flush=True)
    rig.server.batch(["kill @e[type=minecraft:villager]"] +
                     [f"forceload remove {x - 16} -16 {x + 31} 31" for x in xs])
    return out


def campaign_breed_type(rig: Rig) -> dict:
    """Twelve pairs of desert parents in plains: is a baby the biome's type or
    a parent's? One desert baby in `breed` could not tell."""
    out: dict = {}
    # 24 pairs: the first run's 12 gave 9 desert, 3 plains, which the wiki's
    # half-biome rule only reaches 7 % of the time.
    cols, rows, step = 6, 4, 24
    rig.server.batch([f"forceload add 3200 -16 {3200 + cols * step + 16} {rows * step + 16}",
                      "time set 1000"])
    time.sleep(4.0)
    origins = []
    for r in range(rows):
        for c in range(cols):
            x, z = 3200 + c * step, r * step
            origins.append((x, z))
            box(rig, x, z, x + 8, z + 8)
            cmds = []
            for b in range(3):
                cmds += [f"setblock {x + 1 + 3 * b} {Y} {z + 1} "
                         "minecraft:red_bed[facing=south,part=foot]",
                         f"setblock {x + 1 + 3 * b} {Y} {z + 2} "
                         "minecraft:red_bed[facing=south,part=head]"]
            rig.server.batch(cmds)
            for dx in (2, 6):
                summon(rig, "villager", x + dx + 0.5, z + 6.5,
                       f"Silent:1b,{vdata(1, 'none', 'desert')},"
                       'Inventory:[{id:"minecraft:bread",Count:3b}]')
    rig.server.batch([])
    time.sleep(40.0)
    t0 = time.monotonic()
    types: list = []
    while time.monotonic() - t0 < 120.0:
        answers = rig.datas([(f"@e[type=minecraft:villager,x={x},y={Y - 8},z={z},dx=9,dy=24,dz=9,"
                              "limit=1,nbt=!{PersistenceRequired:1b}]", "VillagerData.type")
                             for x, z in origins])
        types = [snbt(a) for a in answers]
        print(f"  babies' types: {types}", flush=True)
        if all(t is not None for t in types):
            break
        time.sleep(10.0)
    out["types"] = types
    rig.server.batch(["kill @e[type=minecraft:villager]",
                      f"forceload remove 3200 -16 {3200 + cols * step + 16} {rows * step + 16}"])
    return out


def campaign_farm(rig: Rig) -> dict:
    out: dict = {}
    X = 1200
    rig.server.batch([f"forceload add {X - 16} -16 {X + 31} 31", "time set 3000"])
    time.sleep(3.0)
    box(rig, X, 0, X + 8, 8)
    cmds = [f"setblock {X + 4} {Y} 0 minecraft:composter"]
    for dx in range(1, 8):
        for dz in range(3, 8):
            cmds.append(f"setblock {X + dx} {Y - 1} {dz} minecraft:farmland[moisture=7]")
            cmds.append(f"setblock {X + dx} {Y} {dz} minecraft:wheat[age=7]")
    cmds.append(f"setblock {X + 4} {Y - 1} 5 minecraft:water")
    cmds.append(f"setblock {X + 4} {Y} 5 minecraft:air")
    rig.server.batch(cmds)
    n = summon(rig, "villager", X + 4.5, 1.5, "Silent:1b," + vdata(1, "farmer") +
               ',Inventory:[{id:"minecraft:wheat_seeds",Count:4b}]')
    rows = []
    t0 = time.monotonic()
    while time.monotonic() - t0 < 120.0:
        ripe = rig.server.batch([f"execute store result score ovc ovcount run fill {X + 1} {Y} 3 "
                                 f"{X + 7} {Y} 7 minecraft:wheat[age=7] replace minecraft:wheat[age=7]",
                                 "scoreboard players get ovc ovcount"])
        inv = snbt(rig.data(uuid_of(n), "Inventory"))
        m = [re.search(r"has (-?\d+) \[", line) for line in ripe]
        count = next((int(x.group(1)) for x in m if x), None)
        rows.append({"t": round(time.monotonic() - t0, 1), "ripe": count, "inventory": inv})
        print(f"  {rows[-1]['t']} s: ripe {count}, inventory {inv}", flush=True)
        time.sleep(8.0)
    planted = rig.server.batch([f"execute store result score ovc ovcount run fill {X + 1} {Y} 3 "
                                f"{X + 7} {Y} 7 minecraft:wheat[age=0] replace minecraft:wheat[age=0]",
                                "scoreboard players get ovc ovcount"])
    out = {"rows": rows, "planted_age0": planted[-1:]}
    rig.server.batch(["kill @e[type=minecraft:villager]", "kill @e[type=minecraft:item]",
                      f"forceload remove {X - 16} -16 {X + 31} 31", "time set noon"])
    return out


def campaign_golem(rig: Rig) -> dict:
    out: dict = {"cells": []}
    now = rig.gametime()
    cells = [
        ("3 slept + zombie", 3, True, True, False),
        ("3 unslept + zombie", 3, False, True, False),
        ("2 slept + zombie", 2, True, True, False),
        ("3 slept + zombie + golem", 3, True, True, True),
        ("3 slept, no zombie", 3, True, False, False),
    ]
    xs = [1400 + 80 * k for k in range(len(cells))]
    rig.server.batch([f"forceload add {x - 32} -32 {x + 31} 31" for x in xs] +
                     ["time set 6000", "difficulty easy"])
    time.sleep(4.0)
    for (label, count, slept, zombie, golem), x in zip(cells, xs):
        rig.server.batch([f"fill {x - 20} {Y} -20 {x + 20} {Y + 6} 20 minecraft:air"])
        cmds = []
        spots = [(x - 3, 0), (x + 3, 0), (x, 3), (x, -3), (x - 3, 3)][:count]
        for vx, vz in spots:
            cmds += fence_pen(rig, vx, vz)
        if zombie:
            cmds += fence_pen(rig, x, 0)
        if golem:
            cmds += [f"fill {x + 7} {Y} -2 {x + 11} {Y} 2 minecraft:oak_fence",
                     f"fill {x + 8} {Y} -1 {x + 10} {Y} 1 minecraft:air"]
        rig.server.batch(cmds)
        mem = "," + memories(last_slept=now) if slept else ""
        for vx, vz in spots:
            summon(rig, "villager", vx + 0.5, vz + 0.5, "Silent:1b" + mem)
        if golem:
            summon(rig, "iron_golem", x + 9.5, 0.5, "Silent:1b,NoAI:1b,PlayerCreated:0b")
    rig.server.batch([])
    time.sleep(1.0)
    for (label, count, slept, zombie, golem), x in zip(cells, xs):
        if zombie:
            summon(rig, "zombie", x + 0.5, 0.5, f"NoAI:1b,Silent:1b,{HELMET}")
    start = rig.gametime()
    polls = []
    t0 = time.monotonic()
    while time.monotonic() - t0 < 50.0:
        row = {"tick": rig.gametime() - start,
               "golems": [count_in(rig, "iron_golem", x - 24, -24, 48, 48) for x in xs]}
        polls.append(row)
        print(f"  tick {row['tick']}: golems {row['golems']}", flush=True)
        time.sleep(3.0)
    for (label, *_), x in zip(cells, xs):
        golems = rig.datas([(f"@e[type=minecraft:iron_golem,x={x - 24},y={Y - 8},z=-24,dx=48,"
                             f"dy=24,dz=48,limit=1,nbt={{PlayerCreated:0b}},sort=nearest]", "Pos")])
        memory = snbt(rig.data(f"@e[type=minecraft:villager,x={x - 24},y={Y - 8},z=-24,dx=48,"
                               "dy=24,dz=48,limit=1]", "Brain.memories"))
        out["cells"].append({"label": label, "centre": [x, Y, 0],
                             "golems_final": polls[-1]["golems"][len(out["cells"])],
                             "a_golem_pos": pos3(snbt(golems[0])), "villager_memories": memory})
        print(f"  {label}: {out['cells'][-1]}", flush=True)
    out["polls"] = polls
    rig.server.batch(["kill @e[type=!minecraft:player]", "difficulty peaceful"] +
                     [f"forceload remove {x - 32} -32 {x + 31} 31" for x in xs])
    return out


def campaign_golem_meet(rig: Rig) -> dict:
    out: dict = {"cells": []}
    now = rig.gametime()
    xs = {"5 slept at a bell": 1900, "5 unslept at a bell": 2000}
    rig.server.batch([f"forceload add {x - 16} -16 {x + 31} 31" for x in xs.values()] +
                     ["time set 9100"])
    time.sleep(3.0)
    for label, x in xs.items():
        box(rig, x, 0, x + 10, 10, height=5)
        rig.server.batch([f"setblock {x + 5} {Y} 5 minecraft:bell"])
        mem = "," + memories(last_slept=now) if "unslept" not in label else ""
        for k in range(5):
            summon(rig, "villager", x + 1.5 + 2 * k, 1.5, "Silent:1b" + mem)
    rig.server.batch([])
    start = rig.gametime()
    polls = []
    t0 = time.monotonic()
    while time.monotonic() - t0 < 90.0:
        row = {"tick": rig.gametime() - start,
               "golems": [count_in(rig, "iron_golem", x - 16, -16, 44, 44) for x in xs.values()]}
        polls.append(row)
        print(f"  tick {row['tick']}: golems {row['golems']}", flush=True)
        time.sleep(4.0)
    out["polls"] = polls
    rig.server.batch(["kill @e[type=!minecraft:player]", "time set noon"] +
                     [f"forceload remove {x - 16} -16 {x + 31} 31" for x in xs.values()])
    return out


def campaign_anger(rig: Rig) -> dict:
    me = uuid_ints(rig.data("ovhand", "UUID"))
    out: dict = {"cells": []}
    # Easy: on Peaceful no mob may attack a player, and the first run (after
    # the golem campaign had set Peaceful back) saw no swing at all.
    rig.server.batch(["difficulty easy"])
    for k, (label, entries) in enumerate((("major_negative 100 (-500)", [("major_negative", 100)]),
                                          ("minor_negative 50 (-50)", [("minor_negative", 50)]),
                                          ("minor_negative 100 (-100)", [("minor_negative", 100)]),
                                          ("minor_negative 99 (-99)", [("minor_negative", 99)]))):
        x = 2200 + 40 * k
        rig.server.batch([f"forceload add {x - 16} -16 {x + 15} 15"])
        time.sleep(2.0)
        box(rig, x, 0, x + 10, 10, height=4)
        rig.server.batch([f"tp ovhand {x + 9.5} -60 9.5", "gamemode survival ovhand"])
        v = summon(rig, "villager", x + 1.5, 1.5, "Silent:1b,NoAI:1b," + gossips(entries, me))
        g = summon(rig, "iron_golem", x + 5.5, 5.5, "Silent:1b")
        geid = rig.hand.eid_for(g)
        # The first run read AngryAt / AngerTime: always empty. A target the
        # village defence picks is not persistent anger; the swing is what
        # shows — entity event 4 (the golem's attack) on the wire.
        t0 = time.monotonic()
        rows = []
        while time.monotonic() - t0 < 12.0:
            p = rig.data("ovhand", "Pos")
            rows.append([round(time.monotonic() - t0, 1), pos3(snbt(p))])
            time.sleep(1.5)
        with rig.hand.state_lock:
            swings = [round(t - t0, 2) for t, e, s in rig.hand.events
                      if e == geid and s == 4 and t >= t0]
        out["cells"].append({"label": label, "golem_attack_events": swings, "probe": rows})
        print(f"  {label}: {len(swings)} swings {swings[:6]}", flush=True)
        rig.server.batch(["kill @e[type=!minecraft:player]", f"forceload remove {x - 16} -16 "
                          f"{x + 15} 15", "tp ovhand 0.5 -60 0.5"])
    return out


def campaign_trader(rig: Rig) -> dict:
    out: dict = {}
    rig.server.batch(["forceload add 2496 -16 2591 79"])
    time.sleep(2.0)
    samples = []
    for batch in range(4):
        ids = []
        for k in range(50):
            ids.append(summon(rig, "wandering_trader", 2500.5 + (k % 10) * 2,
                              0.5 + (k // 10) * 2, "NoAI:1b,Silent:1b"))
        rig.server.batch([])
        answers = rig.datas([(uuid_of(n), "Offers.Recipes") for n in ids])
        if batch == 0:
            out["one"] = snbt(rig.data(uuid_of(ids[0])))
        samples += [snbt(a) for a in answers]
        rig.server.batch(["kill @e[type=minecraft:wandering_trader]"])
    out["samples"] = samples
    print(f"  {len(samples)} traders, offer counts "
          f"{sorted(set(len(s) if isinstance(s, list) else -1 for s in samples))}", flush=True)
    # The despawn counter.
    n = summon(rig, "wandering_trader", 2530.5, 40.5, "Silent:1b,DespawnDelay:100")
    rig.server.batch([])
    start = rig.gametime()
    rows = []
    t0 = time.monotonic()
    while time.monotonic() - t0 < 10.0:
        rows.append([rig.gametime() - start, snbt(rig.data(uuid_of(n), "DespawnDelay"))])
        time.sleep(0.5)
    out["despawn"] = rows
    print(f"  despawn: {rows}", flush=True)
    rig.server.batch(["kill @e[type=minecraft:wandering_trader]",
                      "forceload remove 2496 -16 2591 79"])
    return out


BIOMES = [
    "plains", "sunflower_plains", "snowy_plains", "ice_spikes", "desert", "swamp",
    "mangrove_swamp", "forest", "flower_forest", "birch_forest", "dark_forest",
    "old_growth_birch_forest", "old_growth_pine_taiga", "old_growth_spruce_taiga", "taiga",
    "snowy_taiga", "savanna", "savanna_plateau", "windswept_hills", "windswept_gravelly_hills",
    "windswept_forest", "windswept_savanna", "jungle", "sparse_jungle", "bamboo_jungle",
    "badlands", "eroded_badlands", "wooded_badlands", "meadow", "cherry_grove", "grove",
    "snowy_slopes", "frozen_peaks", "jagged_peaks", "stony_peaks", "river", "frozen_river",
    "beach", "snowy_beach", "stony_shore", "warm_ocean", "lukewarm_ocean", "deep_lukewarm_ocean",
    "ocean", "deep_ocean", "cold_ocean", "deep_cold_ocean", "frozen_ocean", "deep_frozen_ocean",
    "mushroom_fields", "dripstone_caves", "lush_caves", "deep_dark",
]


def campaign_biome(rig: Rig) -> dict:
    out: dict = {}
    rig.server.batch(["forceload add 2800 -16 2999 63"])
    time.sleep(3.0)
    # Summoned *without* NBT: with any, `summon` skips finalizeSpawn, and the
    # type is never drawn (the first run read plains in all 53 biomes).
    spots = {}
    for k, biome in enumerate(BIOMES):
        x = 2800 + (k % 10) * 16
        z = (k // 10) * 16
        spots[biome] = (x + 8.5, z + 8.5)
        lines = rig.server.batch([f"fillbiome {x} {Y - 4} {z} {x + 15} {Y + 8} {z + 15} "
                                  f"minecraft:{biome}",
                                  f"summon minecraft:villager {x + 8.5} {Y} {z + 8.5}"])
        if k == 0:
            out["first_lines"] = lines[-4:]
    rig.server.batch([])
    # ..7: a bare-summoned villager has its brain and walks off (the first
    # fixed run lost 11 of 53 at ..2); the cells are 16 apart.
    answers = rig.datas([(f"@e[type=minecraft:villager,x={x},y={Y},z={z},distance=..7,limit=1]",
                          "VillagerData.type") for x, z in spots.values()])
    out.update({b: snbt(a) for b, a in zip(spots, answers)})
    for b, t in out.items():
        print(f"  {b}: {t}", flush=True)
    rig.server.batch(["kill @e[type=minecraft:villager]", "forceload remove 2800 -16 2999 63"])
    return out


CAMPAIGNS = {
    "price": campaign_price, "events": campaign_events, "decay": campaign_decay,
    "share": campaign_share, "schedule": campaign_schedule, "breed": campaign_breed,
    "breed_type": campaign_breed_type,
    "farm": campaign_farm, "golem": campaign_golem, "golem_meet": campaign_golem_meet,
    "anger": campaign_anger, "trader": campaign_trader, "biome": campaign_biome,
}


def main(argv: list[str]) -> int:
    names = [a for a in argv if a != "all"] or list(CAMPAIGNS)
    registries = json.loads((ROOT / "data/vanilla/1.20.1/generated/reports/registries.json")
                            .read_text())
    types = registries["minecraft:entity_type"]["entries"]
    assert types["minecraft:villager"]["protocol_id"] == VILLAGER
    assert types["minecraft:iron_golem"]["protocol_id"] == IRON_GOLEM
    assert types["minecraft:wandering_trader"]["protocol_id"] == WANDERING_TRADER
    SCRATCH.mkdir(exist_ok=True)
    results = json.loads(OUT.read_text()) if OUT.exists() else {}
    if RUN.exists():
        shutil.rmtree(RUN)
    server = FlatServer(RUN, port=PORT)
    hand = None
    try:
        server.batch(["gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                      "gamerule doWeatherCycle false", "gamerule sendCommandFeedback true",
                      "gamerule doInsomnia false", "gamerule doPatrolSpawning false",
                      "gamerule doTraderSpawning false", "gamerule disableRaids true",
                      "gamerule maxEntityCramming 0", "difficulty peaceful",
                      "time set noon", "setworldspawn 0 -60 0",
                      "scoreboard objectives add ovcount dummy",
                      "forceload add -64 -64 63 127"])
        time.sleep(5.0)
        hand = Clocked(PORT, "ovhand")
        time.sleep(2.0)
        rig = Rig(server, hand)
        server.batch(["tp ovhand 0.5 -60 0.5 -90 0", "gamemode survival ovhand",
                      "effect give ovhand minecraft:resistance infinite 5 true",
                      "kill @e[type=!minecraft:player]"])
        time.sleep(1.0)
        for name in names:
            print(f"── {name}", flush=True)
            started = time.monotonic()
            try:
                results[name] = CAMPAIGNS[name](rig)
            except Exception:
                results[name] = {"error": traceback.format_exc()}
                print(results[name]["error"], flush=True)
                server.batch(["kill @e[type=!minecraft:player]", "difficulty peaceful"])
            results[name]["_seconds"] = round(time.monotonic() - started, 1)
            OUT.write_text(json.dumps(results, indent=1))
    finally:
        if hand is not None:
            hand.close()
        server.stop()
        shutil.rmtree(RUN, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
