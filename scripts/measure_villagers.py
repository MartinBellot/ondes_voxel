#!/usr/bin/env python3
"""Ask a real 1.20.1 server how its villagers work, trade and level up.

Every number the villager code carries comes out of one of these campaigns, run
against `tools/vanilla/server.jar` with a probe client connected (the probe is
the only way to open a trading screen and to see what the wire says).

  meta      Which metadata index carries VillagerData, the baby flag, the
            sleeping position, the "shake head" counter; the zombie villager's
            own indices (VillagerData, converting). One NBT field at a time
            against a baseline of the same type.
  offers    The trade tables. A villager draws its offers lazily, the first time
            anything asks for them — and saving the entity asks. So `data get
            entity … Offers` on a villager summoned with a profession and a level
            and no Offers returns a fresh draw from that level's pool. Sampled
            per profession and level, many villagers per cell.
  packet    Merchant Offers and Open Screen, byte for byte, for a villager whose
            offers were written by hand (so every field has a known value).
  trade     The probe trades: Select Trade, a click on the result slot. XP per
            trade, the experience orbs, the level thresholds (bracketed by one
            point on each side), the delay before a level up, the offers a level
            up adds, and the price a demand makes (n − 1 emeralds refused, n
            accepted).
  claim     Villagers with no profession on open grass: the 13 job blocks at
            three blocks, a lectern at 16 to 52 blocks, the losing of a job when
            the block is broken before the first trade (and the keeping of it
            after), and restocking at the lectern — twice, then no more.
  flee      A zombie at a range of distances from a villager: which distance
            makes it run. A villager hurt by nothing: does it run, how fast.
  zombify   A zombie kills a villager on easy, normal and hard: how many convert,
            and what the zombie villager keeps.
  cure      A golden apple on a weakened zombie villager: ConversionTime drawn;
            what the villager it becomes keeps.

Usage: python3 scripts/measure_villagers.py [campaign ...]     (default: all)
Run it under the machine's vanilla lock: lockf /tmp/ov-vanilla.lock python3 …

Writes .scratch/villagers.json (untracked), merging into what is there.
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
from capture_entity_packets import read_varint, varint  # noqa: E402
from measure_husbandry import DATA, Hand, Rig, parse_metadata, uuid_of  # noqa: E402
from measure_mobs import FlatServer  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SCRATCH = ROOT / ".scratch"
OUT = SCRATCH / "villagers.json"
RUN = SCRATCH / "villager-oracle"
PORT = 25631
Y = -60

PROFESSIONS = ["armorer", "butcher", "cartographer", "cleric", "farmer", "fisherman",
               "fletcher", "leatherworker", "librarian", "mason", "shepherd", "toolsmith",
               "weaponsmith"]
TYPES = ["desert", "jungle", "plains", "savanna", "snow", "swamp", "taiga"]
JOB_BLOCKS = {
    "armorer": "blast_furnace", "butcher": "smoker", "cartographer": "cartography_table",
    "cleric": "brewing_stand", "farmer": "composter", "fisherman": "barrel",
    "fletcher": "fletching_table", "leatherworker": "cauldron", "librarian": "lectern",
    "mason": "stonecutter", "shepherd": "loom", "toolsmith": "smithing_table",
    "weaponsmith": "grindstone",
}
VILLAGER, ZOMBIE_VILLAGER = 108, 120  # checked against registries.json at start

# Packets the recorder never keeps: chunks, movement, time, keep-alive.
NOISE = {0x24, 0x2B, 0x2C, 0x2D, 0x42, 0x68, 0x5E, 0x23, 0x25, 0x1E, 0x4E, 0x54}


# ── SNBT, as `data get` prints it ───────────────────────────────────────────

class Snbt:
    NUMBER = re.compile(r"^(-?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?)([bBsSlLfFdD]?)$")

    def __init__(self, text: str) -> None:
        self.s = text
        self.i = 0

    def ws(self) -> None:
        while self.i < len(self.s) and self.s[self.i] in " \n\t\r":
            self.i += 1

    def value(self):
        self.ws()
        c = self.s[self.i]
        if c == "{":
            return self.compound()
        if c == "[":
            return self.list()
        if c in "\"'":
            return self.quoted()
        return self.scalar()

    def quoted(self) -> str:
        quote = self.s[self.i]
        self.i += 1
        out = []
        while self.s[self.i] != quote:
            if self.s[self.i] == "\\":
                self.i += 1
            out.append(self.s[self.i])
            self.i += 1
        self.i += 1
        return "".join(out)

    def token(self) -> str:
        start = self.i
        while self.i < len(self.s) and self.s[self.i] not in ",}]:[{ \n":
            self.i += 1
        return self.s[start:self.i]

    def scalar(self):
        tok = self.token()
        m = self.NUMBER.match(tok)
        if not m:
            return tok
        num, suffix = m.groups()
        if suffix in "fFdD" and suffix or "." in num or "e" in num.lower():
            return float(num)
        return int(num)

    def compound(self) -> dict:
        self.i += 1
        out: dict = {}
        while True:
            self.ws()
            if self.s[self.i] == "}":
                self.i += 1
                return out
            key = self.quoted() if self.s[self.i] in "\"'" else self.token()
            self.ws()
            assert self.s[self.i] == ":", self.s[self.i:self.i + 20]
            self.i += 1
            out[key] = self.value()
            self.ws()
            if self.s[self.i] == ",":
                self.i += 1

    def list(self) -> list:
        self.i += 1
        self.ws()
        if self.s[self.i] in "IBL" and self.s[self.i + 1] == ";":
            self.i += 2
        out = []
        while True:
            self.ws()
            if self.s[self.i] == "]":
                self.i += 1
                return out
            out.append(self.value())
            self.ws()
            if self.s[self.i] == ",":
                self.i += 1


def snbt(text: str | None):
    if text is None:
        return None
    try:
        return Snbt(text).value()
    except Exception:
        return {"_raw": text}


# ── Wire decoding: slots with NBT, Merchant Offers ──────────────────────────

def skip_nbt_payload(p: bytes, i: int, tag: int) -> int:
    if tag == 1:
        return i + 1
    if tag == 2:
        return i + 2
    if tag in (3, 5):
        return i + 4
    if tag in (4, 6):
        return i + 8
    if tag == 7:
        n = struct.unpack_from(">i", p, i)[0]
        return i + 4 + n
    if tag == 8:
        n = struct.unpack_from(">H", p, i)[0]
        return i + 2 + n
    if tag == 9:
        inner = p[i]
        n = struct.unpack_from(">i", p, i + 1)[0]
        i += 5
        for _ in range(n):
            i = skip_nbt_payload(p, i, inner)
        return i
    if tag == 10:
        while True:
            t = p[i]
            i += 1
            if t == 0:
                return i
            n = struct.unpack_from(">H", p, i)[0]
            i += 2 + n
            i = skip_nbt_payload(p, i, t)
    if tag == 11:
        n = struct.unpack_from(">i", p, i)[0]
        return i + 4 + 4 * n
    if tag == 12:
        n = struct.unpack_from(">i", p, i)[0]
        return i + 4 + 8 * n
    raise ValueError(f"nbt tag {tag}")


def read_slot(p: bytes, i: int) -> tuple[dict | None, int]:
    present = p[i]
    i += 1
    if not present:
        return None, i
    item, i = read_varint(p, i)
    count = struct.unpack_from(">b", p, i)[0]
    i += 1
    start = i
    tag = p[i]
    i += 1
    if tag != 0:
        n = struct.unpack_from(">H", p, i)[0]  # the root's name, empty in 763
        i += 2 + n
        i = skip_nbt_payload(p, i, tag)
    return {"item": item, "count": count, "nbt_hex": p[start:i].hex()}, i


def decode_offers(p: bytes) -> dict:
    window, i = read_varint(p, 0)
    n, i = read_varint(p, i)
    trades = []
    for _ in range(n):
        a, i = read_slot(p, i)
        out, i = read_slot(p, i)
        b, i = read_slot(p, i)
        disabled = p[i] != 0
        i += 1
        uses, max_uses, xp, special = struct.unpack_from(">iiii", p, i)
        i += 16
        mult = struct.unpack_from(">f", p, i)[0]
        i += 4
        demand = struct.unpack_from(">i", p, i)[0]
        i += 4
        trades.append({"a": a, "result": out, "b": b, "disabled": disabled, "uses": uses,
                       "max_uses": max_uses, "xp": xp, "special_price": special,
                       "multiplier": mult, "demand": demand})
    level, i = read_varint(p, i)
    experience, i = read_varint(p, i)
    regular = p[i] != 0
    restock = p[i + 1] != 0
    return {"window": window, "trades": trades, "level": level, "experience": experience,
            "regular": regular, "can_restock": restock, "consumed": i + 2, "length": len(p)}


class Trader(Hand):
    """The husbandry probe, plus what a trading screen needs: the window id,
    the container's state id, and a recorder for everything that arrives."""

    def __init__(self, port: int, name: str = "ovhand") -> None:
        self.recording = False
        self.rec: list[tuple[float, int, bytes]] = []
        self.window: int | None = None
        self.state_id = 0
        self.slot_updates: list[tuple[int, int, dict | None]] = []
        super().__init__(port, name)

    def read(self):  # type: ignore[override]
        pid, p = super().read()
        try:
            if pid == 0x30:
                self.window, _ = read_varint(p, 0)
            elif pid == 0x12:
                self.state_id, _ = read_varint(p, 1)
            elif pid == 0x14:
                sid, i = read_varint(p, 1)
                self.state_id = sid
                slot = struct.unpack_from(">h", p, i)[0]
                stack, _ = read_slot(p, i + 2)
                self.slot_updates.append((p[0], slot, stack))
        except Exception:
            pass
        if self.recording and pid not in NOISE:
            self.rec.append((time.monotonic(), pid, p))
        return pid, p

    def open(self, eid: int, timeout: float = 3.0) -> bool:
        self.window = None
        self.rec = []
        self.slot_updates = []
        self.recording = True
        self.interact(eid)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and self.window is None:
            time.sleep(0.02)
        time.sleep(0.3)
        return self.window is not None

    def select(self, index: int) -> None:
        self.send(0x26, varint(index))

    def click(self, slot: int, button: int = 0, mode: int = 0) -> None:
        assert self.window is not None
        self.send(0x0B, bytes([self.window]) + varint(self.state_id)
                  + struct.pack(">hb", slot, button) + varint(mode) + varint(0) + b"\x00")

    def close_screen(self) -> None:
        if self.window is not None:
            self.send(0x0C, bytes([self.window]))
        self.window = None
        self.recording = False


# ── Helpers ─────────────────────────────────────────────────────────────────

def summon(rig: Rig, kind: str, x: float, z: float, nbt: str, y: float = Y) -> int:
    n = rig.uuid()
    extra = f",{nbt}" if nbt else ""
    rig.server.send(f"summon minecraft:{kind} {x} {y} {z} "
                    f"{{UUID:[I;{0x4F56},0,0,{n}],PersistenceRequired:1b{extra}}}")
    return n


def vdata(level: int, profession: str = "librarian", vtype: str = "plains") -> str:
    return (f'VillagerData:{{profession:"minecraft:{profession}",level:{level},'
            f'type:"minecraft:{vtype}"}}')


def recipe(buy: str, buy_count: int, sell: str, sell_count: int, *, max_uses: int = 16,
           uses: int = 0, xp: int = 2, mult: float = 0.05, demand: int = 0,
           buy_b: str | None = None, buy_b_count: int = 0, sell_tag: str = "") -> str:
    b = (f',buyB:{{id:"minecraft:{buy_b}",Count:{buy_b_count}b}}' if buy_b else "")
    tag = f",tag:{sell_tag}" if sell_tag else ""
    return (f'{{buy:{{id:"minecraft:{buy}",Count:{buy_count}b}}{b},'
            f'sell:{{id:"minecraft:{sell}",Count:{sell_count}b{tag}}},maxUses:{max_uses},'
            f'uses:{uses},xp:{xp},rewardExp:1b,priceMultiplier:{mult}f,specialPrice:0,'
            f'demand:{demand}}}')


def offers_nbt(*recipes: str) -> str:
    return "Offers:{Recipes:[" + ",".join(recipes) + "]}"


def pen(rig: Rig, x: int, z: int) -> None:
    """A 1 × 1 glass cell three high around (x, z)."""
    rig.server.batch([f"fill {x - 1} {Y} {z - 1} {x + 1} {Y + 2} {z + 1} minecraft:glass",
                      f"fill {x} {Y} {z} {x} {Y + 2} {z} minecraft:air"])


def metadata_fields(hand: Hand, eid: int) -> list[list]:
    return [[list(f) for f in fields] for _, fields in hand.metadata_of(eid)]


def pos3(raw) -> list[float] | None:
    if isinstance(raw, list) and len(raw) == 3:
        return [float(v) for v in raw]
    return None


# ── Campaigns ───────────────────────────────────────────────────────────────

def campaign_meta(rig: Rig) -> dict:
    out: dict = {}
    probes = [
        ("villager", "base", ""),
        ("villager", "type_desert", 'VillagerData:{type:"minecraft:desert"}'),
        ("villager", "profession_librarian", 'VillagerData:{profession:"minecraft:librarian"}'),
        ("villager", "level_3", "VillagerData:{level:3}"),
        ("villager", "baby", "Age:-24000"),
        ("villager", "sleeping", "SleepingX:3,SleepingY:-60,SleepingZ:9"),
        ("zombie_villager", "base", ""),
        ("zombie_villager", "profession_librarian",
         'VillagerData:{profession:"minecraft:librarian"}'),
        ("zombie_villager", "converting", "ConversionTime:2000"),
        ("zombie_villager", "baby", "IsBaby:1b"),
    ]
    for index, (kind, label, nbt) in enumerate(probes):
        joined = "NoAI:1b,Silent:1b" + (f",{nbt}" if nbt else "")
        n = summon(rig, kind, 3.5, 0.5 + (index % 3), joined)
        eid = rig.hand.eid_for(n)
        time.sleep(0.6)
        out[f"{kind}:{label}"] = {"nbt": nbt, "eid": eid,
                                  "metadata": metadata_fields(rig.hand, eid) if eid else None}
        rig.kill(n)
        print(f"  {kind}:{label}: {out[f'{kind}:{label}']['metadata']}")

    # The head shake: interact with a villager that has nothing to sell.
    for label, nbt in (("none", ""), ("baby_librarian", 'Age:-24000,' + vdata(1))):
        n = summon(rig, "villager", 2.5, 0.5, "NoAI:1b,Silent:1b" + (f",{nbt}" if nbt else ""))
        eid = rig.hand.eid_for(n)
        time.sleep(0.5)
        before = len(rig.hand.metadata_of(eid))
        rig.hand.interact(eid)
        time.sleep(1.0)
        after = rig.hand.metadata_of(eid)[before:]
        events = [(e, s) for _, e, s in rig.hand.events if e == eid]
        out[f"interact:{label}"] = {"metadata_after": [[list(f) for f in fs] for _, fs in after],
                                    "events": events}
        print(f"  interact {label}: {out[f'interact:{label}']}")
        rig.kill(n)
    return out


def campaign_offers(rig: Rig) -> dict:
    cells: list[tuple[str, int, str, int]] = []
    for profession in PROFESSIONS:
        for level in range(1, 6):
            count = 100 if profession == "librarian" and level <= 4 else 30
            cells.append((profession, level, "plains", count))
    for vtype in TYPES:
        cells.append(("fisherman", 5, vtype, 12))
    cells.append(("nitwit", 1, "plains", 5))
    cells.append(("none", 1, "plains", 5))

    out: dict = {"cells": []}
    rig.server.batch(["forceload add 16 16 111 111"])
    for profession, level, vtype, count in cells:
        samples = []
        done = 0
        while done < count:
            batch = min(50, count - done)
            ids = []
            for k in range(batch):
                x = 20.5 + (k % 10) * 2
                z = 20.5 + (k // 10) * 2
                ids.append(summon(rig, "villager", x, z, "NoAI:1b,Silent:1b,"
                                  + vdata(level, profession, vtype)))
            rig.server.batch([])
            answers = rig.datas([(uuid_of(n), "Offers.Recipes") for n in ids])
            for n, raw in zip(ids, answers):
                samples.append(snbt(raw) if raw is not None else None)
            rig.server.batch(["kill @e[type=minecraft:villager]"])
            done += batch
        out["cells"].append({"profession": profession, "level": level, "type": vtype,
                             "samples": samples})
        sizes = [len(s) if isinstance(s, list) else None for s in samples]
        print(f"  {profession} {level} {vtype}: {len(samples)} villagers, offer counts "
              f"{sorted(set(map(str, sizes)))}", flush=True)
    rig.server.batch(["forceload remove 16 16 111 111"])
    return out


def campaign_packet(rig: Rig) -> dict:
    """Two offers written by hand: every field of the packet has a known value."""
    rig.server.batch(["tp ovhand 0.5 -60 0.5 -90 0", "clear ovhand"])
    pen(rig, 3, 0)
    book = recipe("emerald", 17, "enchanted_book", 1, max_uses=12, uses=3, xp=5, mult=0.2,
                  demand=4, buy_b="book", buy_b_count=1,
                  sell_tag='{StoredEnchantments:[{id:"minecraft:mending",lvl:1s}]}')
    paper = recipe("paper", 24, "emerald", 1, max_uses=16, uses=16, xp=2, mult=0.05, demand=-7)
    n = summon(rig, "villager", 3.5, 0.5, "Silent:1b,Xp:37," + vdata(2) + "," +
               offers_nbt(book, paper))
    eid = rig.hand.eid_for(n)
    time.sleep(0.5)
    ok = rig.hand.open(eid)
    time.sleep(0.8)
    rig.hand.close_screen()
    time.sleep(0.3)
    packets = [{"id": pid, "hex": p.hex()} for _, pid, p in rig.hand.rec]
    decoded = None
    for _, pid, p in rig.hand.rec:
        if pid == 0x2A:
            try:
                decoded = decode_offers(p)
            except Exception as error:
                decoded = {"error": str(error)}
    out = {"opened": ok, "packets": packets, "merchant_offers": decoded,
           "villager": snbt(rig.data(uuid_of(n)))}
    print(f"  opened {ok}, ids {[p['id'] for p in packets]}")
    print(f"  offers {decoded}")
    rig.kill(n)
    return out


def trade_once(rig: Rig, eid: int, index: int = 0) -> dict:
    hand: Trader = rig.hand  # type: ignore[assignment]
    t0 = time.monotonic()
    opened = hand.open(eid)
    hand.select(index)
    time.sleep(0.4)
    after_select = list(hand.slot_updates)
    hand.click(2)
    time.sleep(0.4)
    after_click = list(hand.slot_updates)
    hand.close_screen()
    closed = time.monotonic()
    time.sleep(0.3)
    with hand.state_lock:
        orbs = [(round(t - t0, 2), v) for t, _, v, _ in hand.orbs if t >= t0]
    # Every packet id while the screen was open: does a trade re-send the
    # offers (0x2A), or does the client keep its own count?
    ids = [pid for _, pid, _ in hand.rec]
    return {"opened": opened, "select": [[w, s, st] for w, s, st in after_select],
            "click": [[w, s, st] for w, s, st in after_click], "orbs": orbs,
            "packet_ids": ids, "closed_at": closed}


def read_villager(rig: Rig, n: int) -> dict:
    raw = rig.datas([(uuid_of(n), "Xp"), (uuid_of(n), "VillagerData"),
                     (uuid_of(n), "Offers.Recipes"), (uuid_of(n), "ActiveEffects")])
    return {"xp": snbt(raw[0]), "data": snbt(raw[1]), "offers": snbt(raw[2]),
            "effects": snbt(raw[3])}


def campaign_trade(rig: Rig) -> dict:
    out: dict = {}
    x0 = 3
    slot = [0]

    def trader(level: int, xp: int, *recipes: str) -> tuple[int, int]:
        z = slot[0] * 4
        slot[0] += 1
        pen(rig, x0, z)
        rig.server.batch([f"tp ovhand 0.5 -60 {z + 0.5} -90 0"])
        n = summon(rig, "villager", x0 + 0.5, z + 0.5, f"Silent:1b,Xp:{xp},"
                   + vdata(level) + "," + offers_nbt(*recipes))
        eid = rig.hand.eid_for(n)
        time.sleep(0.6)
        return n, eid

    paper = recipe("paper", 24, "emerald", 1, max_uses=16, xp=2)
    rig.server.batch(["forceload add -16 -16 31 255"])

    # 1. Five trades in a row from Xp 1: XP per trade, orbs, the level up.
    rig.server.batch(["clear ovhand", "give ovhand minecraft:paper 256"])
    n, eid = trader(1, 1, paper)
    runs = []
    for k in range(5):
        result = trade_once(rig, eid)
        state = read_villager(rig, n)
        runs.append({"trade": {k2: v for k2, v in result.items() if k2 != "closed_at"},
                     "xp": state["xp"], "uses": [r.get("uses") for r in state["offers"] or []]})
        print(f"  trade {k}: xp {state['xp']}, orbs {result['orbs']}, "
              f"uses {runs[-1]['uses']}", flush=True)
        if k == 4:
            closed = result["closed_at"]
            polls = []
            while time.monotonic() - closed < 5.0:
                d = snbt(rig.data(uuid_of(n), "VillagerData"))
                polls.append([round(time.monotonic() - closed, 2), d])
                time.sleep(0.2)
            out["level_up_polls"] = polls
    out["five_trades"] = runs
    out["after_level_up"] = read_villager(rig, n)
    print(f"  after: {out['after_level_up']['data']}, "
          f"{len(out['after_level_up']['offers'] or [])} offers, "
          f"effects {out['after_level_up']['effects']}", flush=True)

    # 2. Thresholds, one point each side, and a master that has nowhere to go.
    thresholds = []
    for level, t in ((1, 10), (2, 70), (3, 150), (4, 250), (5, 1000)):
        for start in (t - 3, t - 2):
            rig.server.batch(["clear ovhand", "give ovhand minecraft:paper 64"])
            n, eid = trader(level, start, paper)
            result = trade_once(rig, eid)
            time.sleep(3.0)
            state = read_villager(rig, n)
            thresholds.append({"level": level, "xp_before": start, "xp_after": state["xp"],
                               "data_after": state["data"], "orbs": result["orbs"],
                               "offers_after": len(state["offers"] or [])})
            print(f"  level {level} from {start}: -> {state['xp']} {state['data']} "
                  f"orbs {result['orbs']}", flush=True)
            rig.kill(n)
    out["thresholds"] = thresholds

    # 3. The price a demand makes: n - 1 emeralds refused, n accepted.
    prices = []
    for base, mult, demand, expected in ((10, 0.2, 5, 20), (10, 0.05, 3, 11),
                                         (10, 0.2, -4, 10), (9, 0.05, 11, 13)):
        offer = recipe("emerald", base, "bookshelf", 1, max_uses=12, xp=1, mult=mult,
                       demand=demand)
        for have in (expected - 1, expected):
            rig.server.batch(["clear ovhand", f"give ovhand minecraft:emerald {have}"])
            n, eid = trader(2, 20, offer)
            hand: Trader = rig.hand  # type: ignore[assignment]
            hand.open(eid)
            hand.select(0)
            time.sleep(0.5)
            result_slot = [st for _, s, st in hand.slot_updates if s == 2]
            paid_slot = [st for _, s, st in hand.slot_updates if s == 0]
            hand.close_screen()
            prices.append({"base": base, "multiplier": mult, "demand": demand,
                           "expected": expected, "had": have,
                           "result": result_slot[-1] if result_slot else None,
                           "payment": paid_slot[-1] if paid_slot else None})
            print(f"  price {base}x{mult}x{demand}: had {have}, result "
                  f"{prices[-1]['result']}", flush=True)
            rig.kill(n)
    out["prices"] = prices
    rig.server.batch(["forceload remove -16 -16 31 255", "clear ovhand"])
    return out


def campaign_claim(rig: Rig) -> dict:
    out: dict = {}
    X = 210
    commands = ["forceload add 192 0 287 655", "forceload add 192 656 287 1311"]
    rigs: list[dict] = []
    for i, profession in enumerate(PROFESSIONS):
        rigs.append({"name": profession, "z": 8 + 24 * i, "d": 3,
                     "block": JOB_BLOCKS[profession]})
    rigs.append({"name": "loss_a", "z": 8 + 24 * 13, "d": 3, "block": "lectern"})
    rigs.append({"name": "loss_b", "z": 8 + 24 * 14, "d": 3, "block": "lectern"})
    for k, d in enumerate((16, 32, 44, 47, 49, 52)):
        rigs.append({"name": f"distance_{d}", "z": 400 + 128 * k, "d": d, "block": "lectern"})
    rig.server.batch(commands)
    # 492 chunks: a `setblock` or `summon` in a chunk not loaded yet fails
    # without a word, so wait, and check the farthest rig's block took.
    time.sleep(20.0)
    for attempt in range(6):
        rig.server.batch([f"setblock {X + r['d']} {Y} {r['z']} minecraft:{r['block']}"
                          for r in rigs])
        far = rigs[-1]
        lines = rig.server.batch([f"execute if block {X + far['d']} {Y} {far['z']} "
                                  f"minecraft:{far['block']}"])
        if any("Test passed" in line for line in lines):
            break
        time.sleep(10.0)
    out["blocks_placed_after_attempts"] = attempt + 1
    start = rig.gametime()
    for r in rigs:
        r["n"] = summon(rig, "villager", X + 0.5, r["z"] + 0.5, "Silent:1b")
    rig.server.batch([])
    polls = []
    t0 = time.monotonic()
    while time.monotonic() - t0 < 100.0:
        queries = []
        for r in rigs:
            queries += [(uuid_of(r["n"]), "VillagerData.profession"),
                        (uuid_of(r["n"]), "Brain.memories"), (uuid_of(r["n"]), "Pos")]
        answers = rig.datas(queries)
        now = rig.gametime() - start
        row = {"tick": now, "rigs": {}}
        for k, r in enumerate(rigs):
            prof, mem, pos = answers[3 * k:3 * k + 3]
            memories = snbt(mem) or {}
            row["rigs"][r["name"]] = {
                "profession": snbt(prof),
                "potential": (memories.get("minecraft:potential_job_site") or {}).get("value")
                if isinstance(memories, dict) else None,
                "job_site": (memories.get("minecraft:job_site") or {}).get("value")
                if isinstance(memories, dict) else None,
                "pos": pos3(snbt(pos))}
        polls.append(row)
        taken = sum(1 for v in row["rigs"].values() if v["profession"] not in
                    (None, "minecraft:none"))
        print(f"  tick {now}: {taken} of {len(rigs)} employed", flush=True)
        time.sleep(1.5)
    out["rigs"] = [{k: v for k, v in r.items()} for r in rigs]
    out["polls"] = polls

    # Loss: B has traded (Xp 5), A has not. Both lecterns go.
    by = {r["name"]: r for r in rigs}
    rig.server.batch([f"data modify entity {uuid_of(by['loss_b']['n'])} Xp set value 5",
                      f"setblock {X + 3} {Y} {by['loss_a']['z']} minecraft:air",
                      f"setblock {X + 3} {Y} {by['loss_b']['z']} minecraft:air"])
    loss = []
    t0 = time.monotonic()
    broken = rig.gametime()
    while time.monotonic() - t0 < 60.0:
        answers = rig.datas([(uuid_of(by["loss_a"]["n"]), "VillagerData.profession"),
                             (uuid_of(by["loss_b"]["n"]), "VillagerData.profession")])
        loss.append([rig.gametime() - broken, snbt(answers[0]), snbt(answers[1])])
        time.sleep(2.0)
    out["loss"] = loss
    print(f"  loss: {loss[-1]}", flush=True)

    # Restock, at the librarian's own lectern.
    lib = uuid_of(by["librarian"]["n"])
    phases = [(0, 5, 12), (12, 12, 0), (12, 12, 12)]
    restock: dict = {"phases": []}

    def set_uses(uses: tuple[int, int, int], demands: tuple[int, int, int] | None) -> None:
        recs = [recipe("paper", 24, "emerald", 1, max_uses=12, uses=u, xp=2,
                       demand=(demands[j] if demands else 0)) for j, u in enumerate(uses)]
        rig.server.batch([f"data modify entity {lib} Offers set value "
                          f"{{Recipes:[{','.join(recs)}]}}",
                          f"data modify entity {lib} Xp set value 1"])

    set_uses(phases[0], None)
    begin = rig.gametime()
    for phase_index, uses in enumerate(phases):
        if phase_index > 0:
            current = snbt(rig.data(lib, "Offers.Recipes")) or []
            demands = tuple(r.get("demand", 0) for r in current)
            set_uses(uses, demands if len(demands) == 3 else None)
        rows = []
        t0 = time.monotonic()
        limit = 60.0 if phase_index == 0 else 200.0
        seen = None
        while time.monotonic() - t0 < limit:
            answers = rig.datas([(lib, "RestocksToday"), (lib, "LastRestock"),
                                 (lib, "Offers.Recipes"), (lib, "Pos")])
            recs = snbt(answers[2]) or []
            row = [rig.gametime() - begin, snbt(answers[0]), snbt(answers[1]),
                   [(r.get("uses"), r.get("demand")) for r in recs], pos3(snbt(answers[3]))]
            rows.append(row)
            if seen is None:
                seen = row[1]
            elif row[1] != seen:
                break
            time.sleep(2.0)
        restock["phases"].append({"uses_set": uses, "rows": rows})
        print(f"  restock phase {phase_index}: last {rows[-1]}", flush=True)
    restock["gametime_begin"] = begin
    out["restock"] = restock
    rig.server.batch(["kill @e[type=minecraft:villager]",
                      "forceload remove 192 0 287 655", "forceload remove 192 656 287 1311"])
    return out


HELMET = 'ArmorItems:[{},{},{},{id:"minecraft:leather_helmet",Count:1b}]'


def campaign_flee(rig: Rig) -> dict:
    out: dict = {"zombie": [], "hurt": []}
    rig.server.batch(["difficulty easy", "forceload add 384 -16 479 255"])
    time.sleep(3.0)
    rigs = []
    for k, d in enumerate((5.0, 7.0, 7.8, 8.6, 10.0, 12.0)):
        z = 40 * k
        n = summon(rig, "villager", 410.5, z + 0.5, "Silent:1b")
        zn = summon(rig, "zombie", 410.5 + d, z + 0.5, f"NoAI:1b,Silent:1b,{HELMET}")
        rigs.append((d, n, zn))
    rig.server.batch([])
    samples = []
    t0 = time.monotonic()
    while time.monotonic() - t0 < 8.0:
        answers = rig.datas([(uuid_of(n), "Pos") for _, n, _ in rigs])
        samples.append([round(time.monotonic() - t0, 2)] + [pos3(snbt(a)) for a in answers])
        time.sleep(0.5)
    for k, (d, n, zn) in enumerate(rigs):
        track = [[s[0], s[1 + k]] for s in samples]
        out["zombie"].append({"distance": d, "track": track})
        first, last = track[0][1], track[-1][1]
        moved = None if first is None or last is None else round(
            ((last[0] - first[0]) ** 2 + (last[2] - first[2]) ** 2) ** 0.5, 2)
        print(f"  zombie at {d}: villager moved {moved}", flush=True)
    rig.server.batch(["kill @e[type=minecraft:villager]", "kill @e[type=minecraft:zombie]"])

    for label, cause in (("generic", "minecraft:generic"),
                         ("player", "minecraft:player_attack by ovhand")):
        n = summon(rig, "villager", 410.5, 200.5 if label == "generic" else 240.5, "Silent:1b")
        rig.server.batch([])
        time.sleep(1.0)
        rig.server.batch([f"damage {uuid_of(n)} 1 {cause}"])
        track = []
        t0 = time.monotonic()
        while time.monotonic() - t0 < 7.0:
            track.append([round(time.monotonic() - t0, 2), pos3(snbt(rig.data(uuid_of(n), "Pos")))])
            time.sleep(0.25)
        out["hurt"].append({"cause": label, "track": track})
        print(f"  hurt by {label}: {track[0][1]} -> {track[-1][1]}", flush=True)
        rig.kill(n)
    rig.server.batch(["difficulty peaceful", "forceload remove 384 -16 479 255"])
    return out


def campaign_zombify(rig: Rig, plan: tuple = (("easy", 10), ("normal", 30), ("hard", 10))
                     ) -> dict:
    out: dict = {}
    # Midnight: a converted zombie villager has no helmet and would burn at
    # noon before it is counted.
    rig.server.batch(["forceload add 592 -16 687 383", "time set midnight"])
    time.sleep(3.0)
    lib_offers = offers_nbt(recipe("paper", 24, "emerald", 1, uses=3, xp=2))
    for difficulty, count in plan:
        rig.server.batch([f"difficulty {difficulty}"])
        cmds = []
        for k in range(count):
            x = 600 + (k % 10) * 6
            z = (k // 10) * 6
            cmds += [f"fill {x - 1} {Y} {z - 1} {x + 3} {Y + 2} {z + 1} minecraft:glass",
                     f"fill {x} {Y} {z} {x + 2} {Y + 2} {z} minecraft:air"]
        rig.server.batch(cmds)
        for k in range(count):
            x = 600 + (k % 10) * 6
            z = (k // 10) * 6
            summon(rig, "villager", x + 0.5, z + 0.5,
                   "NoAI:1b,Silent:1b,Health:1f,Xp:60," + vdata(3) + "," + lib_offers)
            summon(rig, "zombie", x + 2.5, z + 0.5, f"Silent:1b,{HELMET}")
        rig.server.batch([])
        time.sleep(15.0)
        converted = rig.count("@e[type=minecraft:zombie_villager]")
        villagers = rig.count("@e[type=minecraft:villager]")
        sample = snbt(rig.data("@e[type=minecraft:zombie_villager,limit=1]"))
        out[difficulty] = {"count": count, "zombie_villagers": converted,
                           "villagers_left": villagers, "sample": sample}
        print(f"  {difficulty}: {converted} of {count} converted, {villagers} alive",
              flush=True)
        rig.server.batch(["kill @e[type=!minecraft:player]"])
        rig.server.batch([f"fill {600 + c * 32 - 1} {Y} -1 {600 + c * 32 + 31} {Y + 2} 20 "
                          "minecraft:air" for c in range(2)])
    rig.server.batch(["difficulty peaceful", "forceload remove 592 -16 687 383",
                      "time set noon"])
    return out


def campaign_cure(rig: Rig) -> dict:
    out: dict = {"conversion_time": []}
    rig.server.batch(["difficulty easy", "tp ovhand 0.5 -60 0.5 -90 0"])
    for k in range(24):
        rig.server.batch(["clear ovhand", "item replace entity ovhand weapon.mainhand with "
                          "minecraft:golden_apple 1"])
        n = summon(rig, "zombie_villager", 2.5, 0.5, f"NoAI:1b,Silent:1b,{HELMET},"
                   + vdata(3))
        eid = rig.hand.eid_for(n)
        rig.server.batch([f"effect give {uuid_of(n)} minecraft:weakness 120 0"])
        time.sleep(0.4)
        rig.hand.interact(eid)
        time.sleep(0.5)
        raw = rig.datas([(uuid_of(n), "ConversionTime"), ("ovhand", "SelectedItem")])
        out["conversion_time"].append([snbt(raw[0]), raw[1]])
        rig.kill(n)
    print(f"  conversion times: {[c for c, _ in out['conversion_time']]}", flush=True)

    # What the villager it becomes keeps.
    lib_offers = offers_nbt(recipe("paper", 24, "emerald", 1, uses=3, xp=2))
    n = summon(rig, "zombie_villager", 6.5, 0.5, f"Silent:1b,ConversionTime:40,{HELMET},"
               f"Xp:60,{vdata(3)},{lib_offers}")
    rig.server.batch([])
    time.sleep(5.0)
    out["cured"] = snbt(rig.data("@e[type=minecraft:villager,limit=1,sort=nearest]"))
    out["events"] = [(e, s) for _, e, s in rig.hand.events][-10:]
    cured = out["cured"] if isinstance(out["cured"], dict) else {}
    kept = {key: cured.get(key) for key in ("VillagerData", "Xp", "Offers", "Gossips")}
    print(f"  cured: {kept}", flush=True)
    rig.server.batch(["kill @e[type=!minecraft:player]", "difficulty peaceful"])
    return out


def campaign_meta2(rig: Rig) -> dict:
    """The VillagerData bytes themselves, which `meta` only located: a villager
    of a known type, profession and level, its spawn metadata kept raw."""
    hand: Trader = rig.hand  # type: ignore[assignment]
    out: dict = {}
    for index, (vtype, profession, level, extra) in enumerate((
            ("desert", "librarian", 3, ""), ("taiga", "farmer", 5, ""),
            ("snow", "none", 1, ""), ("swamp", "cleric", 2, ",Age:-24000"))):
        hand.rec = []
        hand.recording = True
        n = summon(rig, "villager", 3.5, 0.5 + index * 2,
                   "NoAI:1b,Silent:1b," + vdata(level, profession, vtype) + extra)
        eid = hand.eid_for(n)
        time.sleep(0.8)
        hand.recording = False
        raw = []
        for _, pid, payload in hand.rec:
            if pid == 0x52 and eid is not None:
                who, _ = read_varint(payload, 0)
                if who == eid:
                    raw.append(payload.hex())
        out[f"{vtype}:{profession}:{level}{extra}"] = {"eid": eid, "metadata_hex": raw}
        print(f"  {vtype} {profession} {level}{extra}: {raw}", flush=True)
        rig.kill(n)
    return out


def campaign_zombify_normal(rig: Rig) -> dict:
    """Sixty more on normal: `zombify` had 10 of 30, and the rule says a half."""
    return campaign_zombify(rig, (("normal", 60),))


CAMPAIGNS = {
    "meta": campaign_meta, "offers": campaign_offers, "packet": campaign_packet,
    "trade": campaign_trade, "claim": campaign_claim, "flee": campaign_flee,
    "zombify": campaign_zombify, "cure": campaign_cure, "meta2": campaign_meta2,
    "zombify_normal": campaign_zombify_normal,
}


def main(argv: list[str]) -> int:
    names = [a for a in argv if a != "all"] or list(CAMPAIGNS)
    registries = json.loads((ROOT / "data/vanilla/1.20.1/generated/reports/registries.json")
                            .read_text())
    types = registries["minecraft:entity_type"]["entries"]
    assert types["minecraft:villager"]["protocol_id"] == VILLAGER
    assert types["minecraft:zombie_villager"]["protocol_id"] == ZOMBIE_VILLAGER
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
                      "forceload add -64 -64 63 63"])
        time.sleep(5.0)
        hand = Trader(PORT, "ovhand")
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
