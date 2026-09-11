#!/usr/bin/env python3
"""Confront our trade pools with a real 1.20.1 server's, offer by offer.

Reads the pools straight out of src/ov_gameplay/src/trading.cpp (the rows are
calls to a handful of constexpr helpers, parsed here) and the vanilla samples
of `scripts/measure_villagers.py offers` (.scratch/villagers.json), then says,
per profession and level:

  * every sampled offer that matches no row of our pool (a wrong item, count,
    price, uses, experience or multiplier — or a row we do not have);
  * every row of our pool that no sample matched, with the chance it had of
    being seen (a row that should have come up and did not is a row vanilla
    does not have);
  * pool order: two offers of one villager must appear in pool order;
  * how often each row came up, against the draw's 2/N.

Randomised rows are matched by their rule: an enchanted item's price in
[base + 5, min(base + 19, 64)], an enchanted book's in its level's range, a
suspicious stew's effect, a boat by villager type.

Usage: python3 scripts/check_trades.py [villagers.json]
"""
from __future__ import annotations

import json
import math
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "src" / "ov_gameplay" / "src" / "trading.cpp"
SAMPLES = ROOT / ".scratch" / "villagers.json"

PROFESSIONS = ["armorer", "butcher", "cartographer", "cleric", "farmer", "fisherman",
               "fletcher", "leatherworker", "librarian", "mason", "shepherd", "toolsmith",
               "weaponsmith"]
TYPES = ["desert", "jungle", "plains", "savanna", "snow", "swamp", "taiga"]
TREASURE = {"minecraft:frost_walker", "minecraft:binding_curse", "minecraft:mending",
            "minecraft:vanishing_curse"}
MAX_LEVEL = {  # 1.20.1, the 37 tradeable
    "protection": 4, "fire_protection": 4, "feather_falling": 4, "blast_protection": 4,
    "projectile_protection": 4, "respiration": 3, "aqua_affinity": 1, "thorns": 3,
    "depth_strider": 3, "frost_walker": 2, "binding_curse": 1, "sharpness": 5, "smite": 5,
    "bane_of_arthropods": 5, "knockback": 2, "fire_aspect": 2, "looting": 3, "sweeping": 3,
    "efficiency": 5, "silk_touch": 1, "unbreaking": 3, "fortune": 3, "power": 5, "punch": 2,
    "flame": 1, "infinity": 1, "luck_of_the_sea": 3, "lure": 3, "loyalty": 3, "impaling": 5,
    "riptide": 3, "channeling": 1, "multishot": 1, "quick_charge": 3, "piercing": 4,
    "mending": 1, "vanishing_curse": 1,
}

CALL = re.compile(r"\b(sell|buy|exchange|enchanted|book|stew|dyed|map_for|tipped|boat_by_type)"
                  r"\(([^()]*)\)")
# Up to the first "};": a pool of one row is written on one line, and a match
# that looked for "\n};" ran on into the next pool.
ARRAY = re.compile(r"constexpr std::array k([A-Z][a-z]+)([1-5])\{(.*?)\};", re.S)


def args_of(text: str) -> list:
    out = []
    for raw in [a.strip() for a in text.split(",") if a.strip()]:
        raw = raw.rstrip("F")
        if raw.startswith('"'):
            out.append(raw.strip('"'))
        else:
            try:
                out.append(float(raw) if "." in raw else int(raw))
            except ValueError:
                out.append(raw)
    return out


def our_pools() -> dict[tuple[str, int], list[dict]]:
    text = SOURCE.read_text()
    # Remove the helpers' own definitions: only rows inside the arrays count.
    pools: dict[tuple[str, int], list[dict]] = {}
    for match in ARRAY.finditer(text):
        name, level, body = match.group(1).lower(), int(match.group(2)), match.group(3)
        rows = []
        for call in CALL.finditer(body):
            kind, a = call.group(1), args_of(call.group(2))
            row = {"kind": kind}
            if kind == "sell":
                row.update(item=a[0], count=a[1], uses=a[2], xp=a[3], mult=0.05)
            elif kind == "buy":
                row.update(item=a[0], emeralds=a[1], count=a[2], uses=a[3], xp=a[4],
                           mult=a[5] if len(a) > 5 else 0.05)
            elif kind == "exchange":
                row.update(input=a[0], input_count=a[1], emeralds=a[2], item=a[3], count=a[4],
                           uses=a[5], xp=a[6], mult=0.05)
            elif kind == "enchanted":
                row.update(item=a[0], base=a[1], uses=a[2], xp=a[3],
                           mult=a[4] if len(a) > 4 else 0.2)
            elif kind == "book":
                row.update(xp=a[0], uses=12, mult=0.2)
            elif kind == "stew":
                row.update(effect=a[0], duration=a[1], uses=12, xp=15, mult=0.05)
            elif kind == "dyed":
                row.update(item=a[0], emeralds=a[1], xp=a[2], uses=12, mult=0.2)
            elif kind == "map_for":
                row.update(emeralds=a[0], xp=a[1], uses=12, mult=0.2)
            elif kind == "tipped":
                row.update(uses=12, xp=30, mult=0.05)
            elif kind == "boat_by_type":
                row.update(uses=12, xp=30, mult=0.05)
            rows.append(row)
        pools[(name, level)] = rows
    return pools


BOATS = dict(zip(TYPES, ["minecraft:jungle_boat", "minecraft:jungle_boat", "minecraft:oak_boat",
                         "minecraft:acacia_boat", "minecraft:spruce_boat",
                         "minecraft:dark_oak_boat", "minecraft:spruce_boat"]))


def stack(d) -> tuple[str, int]:
    if not isinstance(d, dict):
        return ("minecraft:air", 0)
    return (d.get("id", "minecraft:air"), int(d.get("Count", 0)))


def close(a: float, b: float) -> bool:
    return abs(a - b) < 1e-4


def matches(row: dict, offer: dict, vtype: str) -> bool:
    buy, buy_b, sell = stack(offer.get("buy")), stack(offer.get("buyB")), stack(offer.get("sell"))
    tag = dict((offer.get("sell") or {}).get("tag") or {})
    # Every damageable stack carries Damage:0 from birth: not a trade's tag.
    if tag.get("Damage") == 0:
        del tag["Damage"]
    common =(offer.get("maxUses") == row["uses"] and offer.get("xp") == row["xp"]
              and close(float(offer.get("priceMultiplier", -1)), row["mult"]))
    if not common:
        return False
    kind = row["kind"]
    if kind == "sell":
        return buy == (row["item"], row["count"]) and buy_b[1] == 0 and sell == ("minecraft:emerald", 1)
    if kind == "buy":
        return (buy == ("minecraft:emerald", row["emeralds"]) and buy_b[1] == 0
                and sell == (row["item"], row["count"]) and not tag)
    if kind == "exchange":
        return (buy == ("minecraft:emerald", row["emeralds"])
                and buy_b == (row["input"], row["input_count"])
                and sell == (row["item"], row["count"]))
    if kind == "enchanted":
        lo, hi = row["base"] + 5, min(row["base"] + 19, 64)
        return (buy[0] == "minecraft:emerald" and lo <= buy[1] <= hi and buy_b[1] == 0
                and sell == (row["item"], 1))
    if kind == "book":
        stored = tag.get("StoredEnchantments") or []
        if sell != ("minecraft:enchanted_book", 1) or buy_b != ("minecraft:book", 1) or len(stored) != 1:
            return False
        name = stored[0]["id"]
        level = int(stored[0]["lvl"])
        lo, hi = 2 + 3 * level, 6 + 13 * level
        if name in TREASURE:
            lo, hi = lo * 2, hi * 2
        lo, hi = min(lo, 64), min(hi, 64)
        return buy[0] == "minecraft:emerald" and lo <= buy[1] <= hi
    if kind == "stew":
        effects = tag.get("Effects") or []
        return (sell == ("minecraft:suspicious_stew", 1) and buy == ("minecraft:emerald", 1)
                and len(effects) == 1 and int(effects[0].get("EffectId", -1)) == row["effect"]
                and int(effects[0].get("EffectDuration", -1)) == row["duration"])
    if kind == "dyed":
        return (buy == ("minecraft:emerald", row["emeralds"]) and sell[0] == row["item"])
    if kind == "tipped":
        return (buy == ("minecraft:emerald", 2) and buy_b == ("minecraft:arrow", 5)
                and sell[0] == "minecraft:tipped_arrow")
    if kind == "boat_by_type":
        return buy == (BOATS[vtype], 1) and sell == ("minecraft:emerald", 1)
    return False


def describe(offer: dict) -> str:
    buy, buy_b, sell = stack(offer.get("buy")), stack(offer.get("buyB")), stack(offer.get("sell"))
    tag = (offer.get("sell") or {}).get("tag")
    b = f" + {buy_b[1]} {buy_b[0]}" if buy_b[1] else ""
    return (f"{buy[1]} {buy[0]}{b} -> {sell[1]} {sell[0]}"
            f"{' ' + json.dumps(tag)[:80] if tag else ''} uses {offer.get('maxUses')} "
            f"xp {offer.get('xp')} mult {offer.get('priceMultiplier')}")


def main(argv: list[str]) -> int:
    path = Path(argv[0]) if argv else SAMPLES
    cells = json.loads(path.read_text())["offers"]["cells"]
    pools = our_pools()
    problems = 0
    total_offers = 0
    matched_offers = 0
    for cell in cells:
        prof, level, vtype = cell["profession"], cell["level"], cell["type"]
        if prof in ("none", "nitwit"):
            continue
        pool = pools.get((prof, level), [])
        seen = [0] * len(pool)
        unmatched: dict[str, int] = {}
        order_errors = 0
        n = len(cell["samples"])
        for sample in cell["samples"]:
            indices = []
            for offer in sample or []:
                total_offers += 1
                hit = [i for i, row in enumerate(pool) if matches(row, offer, vtype)]
                if hit:
                    matched_offers += 1
                    seen[hit[0]] += 1
                    indices.append(hit[0])
                else:
                    key = describe(offer)
                    unmatched[key] = unmatched.get(key, 0) + 1
            # Listed as a HashSet<Integer> of two iterates: by index modulo 16,
            # and within one bucket in draw order (which the sample cannot show).
            if len(indices) == 2 and (indices[0] & 15) > (indices[1] & 15):
                order_errors += 1
        header = f"{prof} {level} ({vtype}, {n} villagers, pool {len(pool)})"
        lines = []
        for key, count in sorted(unmatched.items(), key=lambda kv: -kv[1]):
            lines.append(f"    NOT IN POOL x{count}: {key}")
        k = min(2, len(pool)) if pool else 0
        expected_unseen = 0.0
        unseen = 0
        for i, row in enumerate(pool):
            p_seen = k / len(pool) if pool else 0
            miss = (1 - p_seen) ** n
            if row["kind"] == "map_for":
                continue  # vanilla skips it too in a world without structures
            expected_unseen += miss
            if seen[i] == 0:
                unseen += 1
                # A row that had under 1 % chance of going unseen, and did, is
                # a row vanilla does not have. Above that, a big pool drawn 30
                # times leaves rows unseen: compare the count, not the row.
                if miss < 0.01:
                    lines.append(f"    NEVER SEEN (chance {miss:.2g}): {row}")
        if unseen:
            print(f"{prof} {level}: {unseen} rows unseen, {expected_unseen:.1f} expected "
                  f"by the draw ({n} villagers, pool {len(pool)})")
        if order_errors:
            lines.append(f"    ORDER: {order_errors} villagers list offers out of pool order")
        if lines:
            problems += 1
            print(header)
            print("\n".join(lines))
    print(f"\n{matched_offers} / {total_offers} sampled offers match a row of our pools; "
          f"{problems} cells with a problem")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
