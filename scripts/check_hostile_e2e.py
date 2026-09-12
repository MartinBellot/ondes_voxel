#!/usr/bin/env python3
"""De bout en bout, sur **notre** serveur : les effets de statut portés par un
mob (mobs-4). La sonde est celle de `check_mobs3_e2e.py`, jugée sur le fil ;
les commandes passent par la console d'`ov_dedicated`.

  effects   une vache : vitesse II (indice 10 = 0x33EBFF), invisibilité
            (bit 0x20 de l'indice 0), puis `effect clear` (indice 10 = 0).
  poison    une vache à 10 PV sous poison I 100 ticks : l'indice 9 relevé, qui
            doit finir à 6,0 (table périodique mesurée, effets.md § 4).
  undead    un zombie sous soin instantané IV : 48 de dégâts, il meurt —
            le `Remove Entities` compté ; un autre sous soin instantané I
            (20 → 14) puis dégâts instantanés I (→ 18). Sans NBT : notre
            `/summon` le refuse (mobs-3.md § 6).
  anvil     notre serveur sur une copie du monde de `measure_hostile.py anvil` :
            le zoo reçu sur le fil avec ses couleurs ; `save-all`, et le monde
            réécrit gardé sous .scratch/hostile-anvil-ours/ pour
            `measure_hostile.py anvil_back`.

Usage : python3 scripts/check_hostile_e2e.py [effects] [poison] [undead] [anvil]
Écrit .scratch/hostile_e2e.json (non suivi).
"""
from __future__ import annotations

import json
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_mobs3_e2e import ROOT, TYPES, Ours, connect, settle_until, spawned_of  # noqa: E402

OUT = ROOT / ".scratch" / "hostile_e2e.json"
VANILLA_ZOO = ROOT / ".scratch" / "hostile-anvil-vanilla"
OURS_ZOO = ROOT / ".scratch" / "hostile-anvil-ours"
NAME = "OndeMobs3"  # the probe's name, fixed by check_mobs3_e2e.connect


def fresh(server: Ours, probe) -> tuple[float, float, float]:
    server.console("kill @e[type=!minecraft:player]", f"gamemode creative {NAME}")
    probe.settle(1.0)
    return probe.pos


def summon(server: Ours, probe, kind: str, at: tuple[float, float, float]) -> int | None:
    # No NBT: our /summon refuses it (`not_modelled`, mobs-3.md § 6), and NoAI
    # would be ignored anyway. The mobs wander; nothing here needs them still.
    before = set(probe.types)
    x, y, z = at
    server.console(f"summon minecraft:{kind} {x:.2f} {y:.2f} {z:.2f}")
    settle_until(probe, 5.0, lambda: spawned_of(probe, kind, before))
    got = spawned_of(probe, kind, before)
    return got[0] if got else None


def check_effects() -> dict:
    server = Ours(ROOT / ".scratch" / "e2e-hostile-effects")
    out: dict = {}
    try:
        probe = connect()
        probe.settle(3.0)
        server.console("gamerule doMobSpawning false")
        x, y, z = fresh(server, probe)
        cow = summon(server, probe, "cow", (x + 3.0, y, z))
        out["cow"] = cow
        if cow is None:
            return out
        server.console("effect give @e[type=minecraft:cow] minecraft:speed 60 1")
        probe.settle(1.5)
        out["speed_index10"] = probe.field(cow, 10)
        out["speed_index11"] = probe.field(cow, 11)
        server.console("effect give @e[type=minecraft:cow] minecraft:invisibility 60 0")
        probe.settle(1.5)
        out["invisible_index0"] = probe.field(cow, 0)
        out["mixed_index10"] = probe.field(cow, 10)
        server.console("effect clear @e[type=minecraft:cow]")
        probe.settle(1.5)
        out["cleared_index10"] = probe.field(cow, 10)
        out["cleared_index0"] = probe.field(cow, 0)
        out["cleared_index11"] = probe.field(cow, 11)
        out["pass"] = (out["speed_index10"] == 0x33EBFF and
                       (out["invisible_index0"] or 0) & 0x20 == 0x20 and
                       out["cleared_index10"] == 0 and (out["cleared_index0"] or 0) & 0x20 == 0)
    finally:
        server.stop()
    return out


def check_poison() -> dict:
    server = Ours(ROOT / ".scratch" / "e2e-hostile-poison")
    out: dict = {}
    try:
        probe = connect()
        probe.settle(3.0)
        server.console("gamerule doMobSpawning false")
        # A warm world first: a debug server generating chunks runs behind the
        # clock (effets.md § 15), and the health is read, not timed, here.
        x, y, z = fresh(server, probe)
        cow = summon(server, probe, "cow", (x + 3.0, y, z))
        out["cow"] = cow
        if cow is None:
            return out
        server.console("effect give @e[type=minecraft:cow] minecraft:poison 5 0")
        seen = []
        begin = time.monotonic()
        while time.monotonic() - begin < 12.0:
            probe.settle(0.5)
            value = probe.field(cow, 9)
            if value is not None and (not seen or seen[-1] != value):
                seen.append(value)
        out["health"] = seen
        out["pass"] = bool(seen) and abs(float(seen[-1]) - 6.0) < 1e-4
    finally:
        server.stop()
    return out


def check_undead() -> dict:
    server = Ours(ROOT / ".scratch" / "e2e-hostile-undead")
    out: dict = {}
    try:
        probe = connect()
        probe.settle(3.0)
        server.console("gamerule doMobSpawning false", "time set 18000")
        x, y, z = fresh(server, probe)
        zombie = summon(server, probe, "zombie", (x + 3.0, y, z))
        healed = summon(server, probe, "zombie", (x - 3.0, y, z))
        out["zombie"], out["healed"] = zombie, healed
        if zombie is None or healed is None:
            return out
        probe.removed.clear()
        near = lambda dx: (f"@e[type=minecraft:zombie,x={x + dx:.2f},y={y:.2f},z={z:.2f},"  # noqa: E731
                           "distance=..3,limit=1,sort=nearest]")
        # Instant health IV on the undead: 6 << 3 = 48 of harm, it dies.
        server.console(f"effect give {near(3.0)} minecraft:instant_health 1 3")
        # The other: instant health I hurts it by 6 (20 → 14), then instant
        # damage I heals it by 4 (→ 18) — the undead inverted, both ways.
        server.console(f"effect give {near(-3.0)} minecraft:instant_health 1 0")
        probe.settle(1.0)
        out["hurt_index9"] = probe.field(healed, 9)
        server.console(f"effect give {near(-3.0)} minecraft:instant_damage 1 0")
        settle_until(probe, 5.0, lambda: any(e == zombie for _, e in probe.removed))
        probe.settle(1.0)
        out["removed"] = [e for _, e in probe.removed]
        out["healed_index9"] = probe.field(healed, 9)
        out["pass"] = (zombie in out["removed"] and healed not in out["removed"] and
                       abs(float(out["hurt_index9"] or 0.0) - 14.0) < 1e-4 and
                       abs(float(out["healed_index9"] or 0.0) - 18.0) < 1e-4)
    finally:
        server.stop()
    return out


def check_anvil() -> dict:
    if not VANILLA_ZOO.exists():
        return {"error": f"no {VANILLA_ZOO}: run measure_hostile.py anvil"}
    shutil.rmtree(OURS_ZOO, ignore_errors=True)
    shutil.copytree(VANILLA_ZOO, OURS_ZOO)
    (OURS_ZOO / "session.lock").unlink(missing_ok=True)
    server = Ours(OURS_ZOO, fresh=False)
    out: dict = {}
    try:
        probe = connect()
        probe.settle(45.0)  # a world read from disk: its chunks come with the player
        colours = {}
        for eid, t in probe.types.items():
            name = TYPES[t].removeprefix("minecraft:")
            if name in ("cow", "zombie", "spider"):
                colours.setdefault(name, []).append(probe.field(eid, 10))
        out["index10"] = colours
        server.console("save-all")
        probe.settle(3.0)
    finally:
        server.stop(keep=True)
    return out


CHECKS = {"effects": check_effects, "poison": check_poison, "undead": check_undead,
          "anvil": check_anvil}


def main(argv: list[str]) -> int:
    wanted = argv or list(CHECKS)
    result = json.loads(OUT.read_text()) if OUT.exists() else {}
    for name in wanted:
        print(f"== {name}", flush=True)
        result[name] = CHECKS[name]()
        OUT.write_text(json.dumps(result, indent=1, default=str))
        print(json.dumps(result[name], indent=1, default=str)[:3000], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
