#!/usr/bin/env python3
"""Turn .scratch/weather-oracle.json (measure_weather.py) into the numbers
docs/provenance/meteo-sommeil.md reports: the draw ranges, the snow histogram
against the binomial model, the freeze by ring, cauldrons, the lightning rate
and the share drawn to rods, farmland in the rain, and the strike results.

Usage: python3 scripts/analyse_weather.py [.scratch/weather-oracle.json]
"""
import json
import math
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PATH = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / ".scratch" / "weather-oracle.json"
d = json.loads(PATH.read_text())

P_COLUMN = 1.0 / 16.0 / 256.0   # one chunk in 16 picks, one column of 256


def binom_pmf(n, k, p):
    return math.comb(n, k) * p ** k * (1 - p) ** (n - k)


def chi2_sf(x, df):
    # Wilson–Hilferty, good enough for a p-value to report.
    if df <= 0:
        return float("nan")
    z = ((x / df) ** (1 / 3) - (1 - 2 / (9 * df))) / math.sqrt(2 / (9 * df))
    return 0.5 * math.erfc(z / math.sqrt(2))


def chi2(observed, expected):
    # Merge bins until each expects at least 5.
    obs, exp = [], []
    o_acc = e_acc = 0.0
    for o, e in zip(observed, expected):
        o_acc += o
        e_acc += e
        if e_acc >= 5:
            obs.append(o_acc)
            exp.append(e_acc)
            o_acc = e_acc = 0.0
    if e_acc > 0 and exp:
        obs[-1] += o_acc
        exp[-1] += e_acc
    x = sum((o - e) ** 2 / e for o, e in zip(obs, exp))
    return x, len(obs) - 1, chi2_sf(x, len(obs) - 1)


if "draws" in d:
    dr = d["draws"]
    print("== draws")
    for kind in ("rain", "thunder", "clear"):
        v = dr[kind]
        print(f"  {kind}: n={len(v)} min={min(v)} max={max(v)} mean={sum(v)/len(v):.0f}")
    ac = dr["after_clear"]
    print(f"  after a timed clear: {sum(1 for a in ac if a['raining'])}/{len(ac)} raining, "
          f"{sum(1 for a in ac if a['thundering'])}/{len(ac)} thundering; rainTime "
          f"{min(a['rainTime'] for a in ac)}..{max(a['rainTime'] for a in ac)}, thunderTime "
          f"{min(a['thunderTime'] for a in ac)}..{max(a['thunderTime'] for a in ac)}")
    # Uniformity: ten equal bins over the documented range.
    for kind, lo, hi in (("rain", 12000, 24000), ("thunder", 3600, 15600), ("clear", 12000, 180000)):
        v = dr[kind]
        bins = [0] * 10
        for x in v:
            bins[min(9, (x - lo) * 10 // (hi - lo + 1))] += 1
        x2, df, p = chi2(bins, [len(v) / 10] * 10)
        print(f"  {kind} uniform on [{lo},{hi}]: bins {bins} chi2={x2:.2f} df={df} p={p:.3f}")

if "precip" in d:
    pr = d["precip"]
    t = pr["ticks"]
    a = t["height3"] - t["start"]
    b_mid = t["mid"] - t["height3"]
    b = t["end"] - t["height3"]
    print(f"== precip: {a} ticks at height 1, then {b} at height 3 (mid read at {b_mid})")

    def layers(field):
        c = Counter()
        for s in field:
            if s and s.startswith("minecraft:snow["):
                c[int(s.split("layers=")[1].split(",")[0].rstrip("]"))] += 1
            else:
                c[0] += 1
        return c

    def model(n_a, n_b, cap=3):
        # layers = min(cap, min(1, hits_a) + hits_b)
        dist = [0.0] * (cap + 1)
        pa0 = (1 - P_COLUMN) ** n_a
        for first, pf in ((0, pa0), (1, 1 - pa0)):
            for k in range(0, cap + 1):
                pk = binom_pmf(n_b, k, P_COLUMN) if k < cap else None
                if k < cap:
                    idx = min(cap, first + k)
                    dist[idx] += pf * pk
            # the remainder goes to the cap
        total = sum(dist)
        dist[cap] += 1 - total
        return dist

    for label, field, nb in (("mid", pr["mid_field"], b_mid), ("end", pr["field"], b)):
        c = layers(field)
        n = sum(c.values())
        m = model(a, nb)
        obs = [c[i] for i in range(4)]
        exp = [n * x for x in m]
        x2, df, p = chi2(obs, exp)
        # witness: the chance per column halved
        global_p = P_COLUMN
        print(f"  {label}: layers 0..3 observed {obs}, model {[round(e, 1) for e in exp]} "
              f"chi2={x2:.2f} df={df} p={p:.3f}")
    # Ponds: ring by ring.
    rings = {"edge": [0, 0], "inner": [0, 0], "centre": [0, 0]}
    for pond in pr["ponds"]:
        for i, s in enumerate(pond):
            dx, dz = divmod(i, 5)
            ring = "edge" if dx in (0, 4) or dz in (0, 4) else ("centre" if (dx, dz) == (2, 2) else "inner")
            rings[ring][1] += 1
            rings[ring][0] += 1 if s == "minecraft:ice" else 0
    print("  ice by ring:", {k: f"{v[0]}/{v[1]}" for k, v in rings.items()})
    cc = Counter(pr["cauldrons"])
    print("  cauldrons:", dict(cc))

if "lightning" in d:
    li = d["lightning"]
    centres = li["centres"]

    def ticked_chunks():
        n = 0
        for cx0, cz0 in centres:
            for cx in range((cx0 >> 4) - 9, (cx0 >> 4) + 10):
                for cz in range((cz0 >> 4) - 9, (cz0 >> 4) + 10):
                    dx = cx * 16 + 8 - (cx0 + 0.5)
                    dz = cz * 16 + 8 - (cz0 + 0.5)
                    if dx * dx + dz * dz < 128 * 128:
                        n += 1
        return n

    n = ticked_chunks()
    print(f"== lightning: {n} chunks within 128 blocks of the three probes")
    for phase in ("open", "rods"):
        ph = li[phase]
        expected = n * ph["ticks"] / 100000
        print(f"  {phase}: {len(ph['bolts'])} bolts in {ph['ticks']} ticks; "
              f"1 in 100000 per chunk predicts {expected:.1f}")
        if phase == "rods":
            # The rods stand 40 blocks east and south of each probe (the first
            # campaign stored their positions under the phase's own key, and
            # lost them; they are fixed by construction).
            rods = li.get("rod_positions") or [(cx + 40, -56, cz + 40) for cx, cz in centres]
            at_rod = sum(1 for (x, y, z) in ph["bolts"]
                         if any(abs(x - (rx + 0.5)) < 0.01 and abs(z - (rz + 0.5)) < 0.01 for rx, ry, rz in rods))
            ys = Counter(round(y, 2) for (x, y, z) in ph["bolts"])
            print(f"    at a rod: {at_rod}/{len(ph['bolts'])}; bolt y values {dict(ys)}")
        per = Counter()
        for (x, y, z) in ph["bolts"]:
            best = min(range(3), key=lambda i: (x - centres[i][0]) ** 2 + (z - centres[i][1]) ** 2)
            dist = math.hypot(x - centres[best][0], z - centres[best][1])
            per[best] += 1
        print(f"    per probe {dict(per)}; y {sorted(set(round(b[1]) for b in ph['bolts']))}")
    fo = Counter(li["farmland_open"])
    fc = Counter(li["farmland_covered"])
    print("  farmland open:", dict(fo))
    print("  farmland under glass:", dict(fc))
    print("  cauldrons in the rain:", dict(Counter(li["cauldrons"])))

if "sleep" in d and "edges_clear" in d["sleep"]:
    sl = d["sleep"]
    print("== sleep (each case: slept?, teleport confirmed?, the keys the chat carried)")

    def keys(chat):
        out = []
        for text, overlay in chat or []:
            try:
                key = json.loads(text).get("translate", "?")
            except Exception:
                key = "?"
            out.append(key + (" [bar]" if overlay else ""))
        return out

    for group in ("edges_clear", "edges_rain", "edges_thunder", "too_far", "obstructed", "monsters"):
        print(f"  {group}:")
        for case, r in sl[group].items():
            print(f"    {case:>22}: slept={r.get('slept')} confirmed={r.get('confirmed')} "
                  f"{keys(r.get('chat'))}")
    st = sl.get("sleep_through", {})
    print(f"  sleep through: slept={st.get('slept')} daytime {st.get('daytime_before')} -> "
          f"{st.get('daytime_after')}, weather after {st.get('weather_after')}")
    print(f"    animations {st.get('animations')}, metadata {st.get('metadata', [])[:2]}")
    for case in ("respawn_bed", "respawn_no_bed"):
        r = sl.get(case, {})
        print(f"  {case}: positions {r.get('positions')} game events "
              f"{[e for e in r.get('game_events', []) if e[0] not in (7, 8)]}")
    print(f"  occupied: {keys(sl.get('occupied', {}).get('chat'))}")
    print(f"  nether: {sl.get('nether')}")

if "strike" in d:
    st = d["strike"]
    print("== strike")
    for k, v in st.items():
        if k.startswith("_"):
            continue
        if k == "damage_box":
            print("  damage box health:", v)
            continue
        print(f"  {k}: before {v['before']} after {v['after']}")
        print(f"     data {v['data']}")
        print(f"     metadata {v['metadata']}")
