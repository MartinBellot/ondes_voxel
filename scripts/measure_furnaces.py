#!/usr/bin/env python3
"""Les fours que personne ne regarde : l'oracle du vrai serveur 1.20.1.

Deux campagnes, contre `tools/vanilla/server.jar` :

* `ticks` — des fours posés par `/setblock` avec leur NBT, **sans aucun joueur
  connecté** : aucune fenêtre ne peut être ouverte, donc tout ce qui avance est
  le bloc-entité seul. Chaque pose et chaque lecture partent dans le même lot
  qu'un `time query gametime` : le serveur exécute un lot dans un seul tick,
  donc l'écart entre deux dates est un nombre exact de ticks du bloc-entité.
  On relève, à plusieurs dates, `Items`, `BurnTime`, `CookTime`,
  `CookTimeTotal` et `RecipesUsed`, tels que `data get block` les imprime —
  types compris (`1599s` est un short).
* `xp` — l'expérience de `RecipesUsed` à l'extraction : une sonde vide la case
  de sortie au shift-clic et on lit `xp query … points`. Un cas entier (dix
  lingots de fer, 10 × 0,7 = 7) et un cas fractionnaire répété (cinq pierres,
  5 × 0,1 = 0,5 : 0 ou 1 point, tiré au sort).

Sortie : `.scratch/furnaces.json`, et sur la sortie standard les cellules
sous la forme que lit `src/ov_server/tests/test_furnace_entity.cpp`.

Usage : python3 scripts/measure_furnaces.py [ticks|xp|xp-iron|all]

`xp-iron` refait le fer, l'extraction et l'entonnoir en gardant les pierres
déjà mesurées.
"""
from __future__ import annotations

import json
import re
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from measure_crafting import (  # noqa: E402
    SB_CLOSE_CONTAINER, SB_USE_ITEM_ON, Probe, Server as CraftServer)
from vanilla_miner import block_pos, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / ".scratch" / "measure-furnaces"
OUT = ROOT / ".scratch" / "furnaces.json"
PORT = 25614
Y = -60

# (bloc, [(case, objet, nombre)]) — chaque montage vise une règle.
SETUPS: list[tuple[str, str, list[tuple[int, str, int]]]] = [
    ("exact", "furnace", [(0, "iron_ore", 8), (1, "coal", 1)]),            # 8 × 200 = 1600
    ("runs-out", "furnace", [(0, "iron_ore", 10), (1, "coal", 1)]),        # le 9e se refroidit
    ("blast", "blast_furnace", [(0, "iron_ore", 10), (1, "coal", 1)]),     # 800 ticks, 100 par objet
    ("smoker", "smoker", [(0, "potato", 10), (1, "coal", 1)]),
    ("stick", "furnace", [(0, "cobblestone", 3), (1, "stick", 1)]),        # 100 ticks puis −2/tick
    ("two-sticks", "furnace", [(0, "cobblestone", 3), (1, "stick", 2)]),   # 200 : le premier cuit
    ("lava", "furnace", [(0, "sand", 20), (1, "lava_bucket", 1)]),         # le seau reste
    ("blocked", "furnace", [(0, "iron_ore", 4), (1, "coal", 1), (2, "gold_ingot", 1)]),
    ("nearly-full", "furnace", [(0, "iron_ore", 4), (1, "coal", 1), (2, "iron_ingot", 63)]),
    ("smoker-refuses", "smoker", [(0, "iron_ore", 4), (1, "coal", 1)]),
    ("blast-kelp", "blast_furnace", [(0, "raw_iron", 30), (1, "dried_kelp_block", 1)]),
    ("no-fuel", "furnace", [(0, "iron_ore", 4)]),
    # Le témoin : sans `CookTimeTotal` dans le NBT. La première campagne a posé
    # tous ses fours ainsi et aucun n'a rien produit en 2500 ticks.
    ("no-total", "furnace", [(0, "iron_ore", 8), (1, "coal", 1)]),
]

# Posés sans `CookTimeTotal` : ce que `/setblock` laisse à 0 si on l'omet.
NO_TOTAL = {"no-total"}


def cook_total(block: str) -> int:
    """Ce qu'un four remplit lui-même quand l'entrée arrive par une case."""
    return 100 if block in ("blast_furnace", "smoker") else 200

# Dates de lecture, en secondes après la pose (le nombre exact de ticks est
# relu, pas supposé).
READS = [1.0, 7.5, 30.0, 60.0, 85.0, 125.0]

TIME = re.compile(r"The time is (\d+)")
DATA = re.compile(r"(-?\d+), (-?\d+), (-?\d+) has the following block data: (.*)$")


def position(index: int) -> tuple[int, int, int]:
    return (2 + 3 * index, Y, 2)


def snbt_items(items: list[tuple[int, str, int]]) -> str:
    return ",".join(f'{{Slot:{slot}b,id:"minecraft:{name}",Count:{count}b}}'
                    for slot, name, count in items)


def parse(data: str) -> dict:
    """Ce que `data get block` imprime, découpé en champs — types compris."""
    out: dict = {"raw": data}
    for key in ("BurnTime", "CookTime", "CookTimeTotal"):
        m = re.search(rf"\b{key}: (-?\d+)([bsL]?)", data)
        out[key] = int(m.group(1)) if m else None
        out[key + "_type"] = (m.group(2) or "i") if m else None
    items = {}
    for m in re.finditer(r'\{Slot: (\d+)b, id: "minecraft:([a-z_]+)", Count: (\d+)b\}', data):
        items[int(m.group(1))] = (m.group(2), int(m.group(3)))
    out["items"] = items
    used = re.search(r"RecipesUsed: \{([^}]*)\}", data)
    out["recipes_used"] = {}
    if used and used.group(1).strip():
        for m in re.finditer(r'"minecraft:([a-z_]+)": (-?\d+)', used.group(1)):
            out["recipes_used"][m.group(1)] = int(m.group(2))
    return out


def gametime(lines: list[str]) -> int:
    for line in lines:
        m = TIME.search(line)
        if m:
            return int(m.group(1))
    raise RuntimeError(f"no game time in {lines[-5:]}")


def open_server() -> CraftServer:
    server = CraftServer(RUN, port=PORT)
    server.batch([
        "gamerule doMobSpawning false", "gamerule randomTickSpeed 0",
        "gamerule doDaylightCycle false", "gamerule doWeatherCycle false",
        "gamerule sendCommandFeedback true", "difficulty peaceful", "time set noon",
        "forceload add -16 -16 64 16",
    ])
    time.sleep(3.0)
    return server


def campaign_ticks(server: CraftServer) -> dict:
    place = []
    for index, (name, block, items) in enumerate(SETUPS):
        x, y, z = position(index)
        total = "" if name in NO_TOTAL else f",CookTimeTotal:{cook_total(block)}s"
        place.append(f"setblock {x} {y} {z} minecraft:{block}{{Items:[{snbt_items(items)}]{total}}}")
    placed_at = gametime(server.batch(place + ["time query gametime"]))
    start = time.monotonic()
    reads = []
    for when in READS:
        while time.monotonic() - start < when:
            time.sleep(0.05)
        commands = ["time query gametime"]
        for index in range(len(SETUPS)):
            x, y, z = position(index)
            commands.append(f"data get block {x} {y} {z}")
        lines = server.batch(commands)
        now = gametime(lines)
        states: dict[int, dict] = {}
        for line in lines:
            m = DATA.search(line)
            if m:
                x = int(m.group(1))
                states[(x - 2) // 3] = parse(m.group(4))
        reads.append({"dt": now - placed_at,
                      "cells": {SETUPS[i][0]: states.get(i) for i in range(len(SETUPS))}})
        print(f"dt={now - placed_at}: " + ", ".join(
            f"{SETUPS[i][0]}={states[i]['items'].get(2)}" for i in sorted(states)), flush=True)
    # Le bloc lui-même : `lit` doit suivre BurnTime.
    lit_lines = server.batch([f"execute if block {position(i)[0]} {Y} 2 "
                              f"minecraft:{SETUPS[i][1]}[lit=true] run say lit{i}"
                              for i in range(len(SETUPS))])
    lit = sorted(int(m.group(1)) for line in lit_lines for m in [re.search(r"lit(\d+)$", line)] if m)
    return {"setups": [[n, b, it] for n, b, it in SETUPS], "reads": reads,
            "lit_at_end": [SETUPS[i][0] for i in lit]}


class XpProbe(Probe):
    def open_block(self, pos: tuple[int, int, int], stand: tuple[float, float, float]) -> None:
        self.window = None
        for _ in range(8):
            self.stand(*stand)
            self.settle(0.3)
            payload = (varint(0) + block_pos(*pos) + varint(1)
                       + struct.pack(">fff", 0.5, 1.0, 0.5) + bytes([0]) + varint(0))
            self.send(SB_USE_ITEM_ON, payload)
            deadline = time.monotonic() + 2.0
            while self.window is None and time.monotonic() < deadline:
                self.settle(0.1)
            if self.window is not None:
                return
        raise RuntimeError(f"le four en {pos} ne s'est pas ouvert")

    def close(self) -> None:
        if self.window is not None:
            self.send(SB_CLOSE_CONTAINER, bytes([self.window]))
            self.window = None
            self.settle(0.1)


def level_points(level: int) -> int:
    """Les points qu'il faut pour atteindre ce niveau depuis zéro."""
    if level <= 16:
        return level * level + 6 * level
    if level <= 31:
        return int(2.5 * level * level - 40.5 * level + 360)
    return int(4.5 * level * level - 162.5 * level + 2220)


def collected(server: CraftServer, name: str) -> int:
    """Tout ce que l'extraction a versé : le joueur, niveaux compris, plus
    les orbes encore au sol.

    `xp query … points` ne compte que les points *dans* le niveau courant :
    sept points font exactement le niveau 1 et s'y lisent 0. C'est ce qui a
    donné trois zéros sur quatre à dix lingots de fer (10 × 0,7 = 7).
    """
    lines = server.batch([f"xp query {name} levels", f"xp query {name} points",
                          "execute as @e[type=minecraft:experience_orb] "
                          "run data get entity @s Value"])
    levels = points = None
    ground = 0
    for line in lines:
        if m := re.search(r"has (\d+) experience levels", line):
            levels = int(m.group(1))
        elif m := re.search(r"has (\d+) experience points", line):
            points = int(m.group(1))
        elif m := re.search(r"has the following entity data: (\d+)s?$", line):
            ground += int(m.group(1))
    if levels is None or points is None:
        raise RuntimeError(f"xp query unanswered: {lines[-5:]}")
    return level_points(levels) + points + ground


def block_data(server: CraftServer, pos: tuple[int, int, int]) -> str | None:
    lines = server.batch([f"data get block {pos[0]} {pos[1]} {pos[2]}"])
    found = [line for line in lines if "block data" in line]
    return found[0] if found else None


def campaign_xp(server: CraftServer, trials: int = 40) -> dict:
    probe = XpProbe(PORT, "Oven0")
    for _ in range(40):
        probe.settle(0.5)
        if any("Oven0" in line for line in server.batch(["list"])):
            break
    pos = (4, Y, 8)
    # Les pieds au sol (le monde plat culmine à −61) : à −59, le serveur
    # expulse la sonde au bout de quatre secondes, « floating too long ».
    stand = (4.5, -60.0, 10.5)
    server.batch(["gamemode survival Oven0", f"tp Oven0 {stand[0]} {stand[1]} {stand[2]}",
                  f"setblock {pos[0]} {pos[1] - 1} {pos[2]} minecraft:stone"])
    probe.settle(1.0)
    results: dict = {"iron10": [], "raw": [], "cleared": None, "hopper_keeps": None}
    if trials:
        results["stone5"] = []

    def place(output: str, count: int, recipe: str, used: int) -> None:
        # L'air d'abord : `setblock` sur un four déjà là répond « Could not
        # set the block » et n'applique pas le NBT.
        server.batch([f"setblock {pos[0]} {pos[1]} {pos[2]} minecraft:air",
                      f'setblock {pos[0]} {pos[1]} {pos[2]} minecraft:furnace{{Items:[{{Slot:2b,'
                      f'id:"minecraft:{output}",Count:{count}b}}],RecipesUsed:{{"minecraft:{recipe}":{used}}}}}'])

    def one(output: str, count: int, recipe: str, used: int) -> int | None:
        """Les points versés, ou None si la sortie n'a pas été prise."""
        server.batch(["xp set Oven0 0 levels", "xp set Oven0 0 points",
                      "kill @e[type=minecraft:experience_orb]", "clear Oven0"])
        place(output, count, recipe, used)
        probe.settle(0.3)
        probe.open_block(pos, stand)
        probe.settle(0.2)
        probe.click(2, 0, 1)            # shift-clic sur la sortie
        probe.settle(0.5)
        probe.close()
        after = block_data(server, pos) or ""
        taken = "Slot: 2b" not in after and "RecipesUsed: {}" in after
        # Le joueur plus le sol : ramasser déplace les points sans en créer,
        # donc deux lectures égales suffisent.
        got, previous = collected(server, "Oven0"), -1
        for _ in range(8):
            if got == previous:
                break
            probe.settle(0.5)
            previous, got = got, collected(server, "Oven0")
        results["raw"].append({"output": output, "taken": taken, "got": got, "after": after})
        return got if taken else None

    for _ in range(6):
        results["iron10"].append(one("iron_ingot", 10, "iron_ingot_from_smelting_iron_ore", 10))
    for _ in range(trials):
        results["stone5"].append(one("stone", 5, "stone", 5))
    stone = [v for v in results.get("stone5", []) if v is not None]
    print(f"iron10 → {results['iron10']}; stone5 → {sum(stone)}/{len(stone)} gave 1",
          flush=True)

    # RecipesUsed vidé par l'extraction, et gardé si c'est un entonnoir qui vide.
    place("iron_ingot", 3, "iron_ingot_from_smelting_iron_ore", 3)
    probe.open_block(pos, stand)
    probe.click(2, 0, 1)
    probe.settle(0.5)
    probe.close()
    results["cleared"] = block_data(server, pos)
    # Un entonnoir dessous vide la sortie sans joueur : le four garde-t-il
    # `RecipesUsed` ? L'entonnoir d'abord, puis le four, sur de l'air.
    server.batch([f"setblock {pos[0]} {pos[1] - 1} {pos[2]} minecraft:hopper"])
    place("iron_ingot", 3, "iron_ingot_from_smelting_iron_ore", 3)
    time.sleep(3.0)
    results["hopper_keeps"] = block_data(server, pos)
    results["hopper_took"] = [line for line in server.batch(
        [f"data get block {pos[0]} {pos[1] - 1} {pos[2]} Items"]) if "block data" in line]
    probe.s.close()
    return results


def main() -> int:
    phase = sys.argv[1] if len(sys.argv) > 1 else "all"
    RUN.parent.mkdir(parents=True, exist_ok=True)
    result: dict = json.loads(OUT.read_text()) if OUT.exists() else {}
    server = open_server()
    try:
        if phase in ("ticks", "all"):
            result["ticks"] = campaign_ticks(server)
        if phase in ("xp", "all"):
            result["xp"] = campaign_xp(server)
        if phase == "xp-iron":
            # Les 40 pierres déjà mesurées sont gardées ; seuls le fer,
            # l'extraction et l'entonnoir sont refaits.
            result.setdefault("xp", {}).update(campaign_xp(server, trials=0))
    finally:
        server.stop()
    OUT.write_text(json.dumps(result, indent=1))
    print(f"écrit {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
