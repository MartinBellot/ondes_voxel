#!/usr/bin/env python3
"""Le composteur contre le vrai serveur 1.20.1 : la chance de chaque objet,
comptée assez de fois pour un χ².

Une sonde en survie tient un objet et l'utilise sur un composteur. Pour isoler
la chance, chaque essai part d'un composteur au niveau 1 (`setblock
…[level=1]`) : l'objet est versé, puis `execute if block …[level=2]` dit si le
niveau est monté. Relevé aussi :

  * au niveau 0, le premier objet fait-il toujours monter ?
  * au niveau 7, un objet est-il refusé (le composteur « prêt » à 8 suit) ?
  * la récolte d'un composteur plein : ce qui tombe.

Sortie : `.scratch/composter.json`. Usage : python3 scripts/measure_composter.py [essais]
"""
from __future__ import annotations

import json
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from measure_crafting import SB_USE_ITEM_ON, Probe, Server as CraftServer  # noqa: E402
from vanilla_miner import block_pos, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / ".scratch" / "measure-composter"
OUT = ROOT / ".scratch" / "composter.json"
PORT = 25623
NAME = "Comp0"
BLOCK = (4, -60, 44)
STAND = (4.5, -60.0, 46.5)

# Une classe de chance par objet, pour que le χ² ait de quoi mordre.
ITEMS = ["wheat_seeds", "cactus", "apple", "bread", "pumpkin_pie", "kelp"]


def main() -> int:
    trials = int(sys.argv[1]) if len(sys.argv) > 1 else 150
    RUN.parent.mkdir(parents=True, exist_ok=True)
    server = CraftServer(RUN, port=PORT)
    out: dict = {"trials": trials, "chance": {}}
    try:
        server.batch(["gamerule doMobSpawning false", "difficulty peaceful", "time set noon",
                      "gamerule randomTickSpeed 0"])
        probe = Probe(PORT, NAME)
        for _ in range(40):
            probe.settle(0.5)
            if any(NAME in line for line in server.batch(["list"])):
                break
        server.batch([f"gamemode survival {NAME}", f"tp {NAME} {STAND[0]} {STAND[1]} {STAND[2]}"])
        probe.settle(1.0)
        x, y, z = BLOCK

        def use() -> None:
            probe.stand(*STAND)
            probe.send(SB_USE_ITEM_ON, varint(0) + block_pos(x, y, z) + varint(1)
                       + struct.pack(">fff", 0.5, 1.0, 0.5) + bytes([0]) + varint(0))
            probe.settle(0.12)

        def level_is(n: int) -> bool:
            lines = server.batch([f"execute if block {x} {y} {z} minecraft:composter[level={n}] "
                                  "run say yes"])
            return any("yes" in line for line in lines)

        for item in ITEMS:
            up = 0
            server.batch([f"item replace entity {NAME} hotbar.0 with minecraft:{item} 64"])
            for trial in range(trials):
                if trial % 60 == 0:
                    server.batch([f"item replace entity {NAME} hotbar.0 with minecraft:{item} 64"])
                server.batch([f"setblock {x} {y} {z} minecraft:composter[level=1]"])
                use()
                up += 1 if level_is(2) else 0
            out["chance"][item] = up
            print(f"{item}: {up}/{trials}", flush=True)

        # Au niveau 0 : le premier objet monte-t-il toujours ?
        server.batch([f"item replace entity {NAME} hotbar.0 with minecraft:wheat_seeds 64"])
        first = 0
        for _ in range(40):
            server.batch([f"setblock {x} {y} {z} minecraft:composter[level=0]"])
            use()
            first += 1 if level_is(1) else 0
        out["from_empty"] = {"seeds": first, "of": 40}

        # Au niveau 7 : refusé ? Puis prêt à 8, et la récolte.
        server.batch([f"setblock {x} {y} {z} minecraft:composter[level=7]",
                      f"item replace entity {NAME} hotbar.0 with minecraft:pumpkin_pie 4"])
        use()
        out["at_7"] = {"still_7": level_is(7),
                       "pie_left": [l[l.find("data:"):] for l in server.batch(
                           [f"data get entity {NAME} Inventory"]) if "entity data" in l]}
        time.sleep(1.5)
        out["at_7"]["became_8"] = level_is(8)
        server.batch([f"clear {NAME}", "kill @e[type=minecraft:item]"])
        use()
        probe.settle(0.5)
        out["harvest"] = {
            "level_after": [n for n in range(9) if level_is(n)],
            "ground": [l[l.find("data:"):] for l in server.batch(
                ["execute as @e[type=minecraft:item] run data get entity @s Item"])
                if "entity data" in l]}
        print(f"from_empty={out['from_empty']} at_7={out['at_7']} harvest={out['harvest']}",
              flush=True)
        probe.s.close()
    finally:
        server.stop()
    OUT.write_text(json.dumps(out, indent=1))
    print(f"écrit {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
