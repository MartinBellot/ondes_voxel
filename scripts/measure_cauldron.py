#!/usr/bin/env python3
"""Les quatre chaudrons contre le vrai serveur 1.20.1 : ce que chaque objet fait
à chaque état.

Chaque cas pose un chaudron dans un état (`setblock`), met un objet dans la
main de la sonde, l'utilise sur le chaudron, puis relit :

  * l'état du bloc (`execute if block` sur chaque état possible) ;
  * la main et l'inventaire (`data get entity … Inventory`).

Sortie : `.scratch/cauldron.json`. Usage : python3 scripts/measure_cauldron.py
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from measure_crafting import SB_USE_ITEM_ON, Probe, Server as CraftServer  # noqa: E402
from vanilla_miner import block_pos, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / ".scratch" / "measure-cauldron"
OUT = ROOT / ".scratch" / "cauldron.json"
PORT = 25624
NAME = "Pot0"
BLOCK = (4, -60, 50)
STAND = (4.5, -60.0, 52.5)

STATES = (["minecraft:cauldron", "minecraft:lava_cauldron"]
          + [f"minecraft:water_cauldron[level={n}]" for n in (1, 2, 3)]
          + [f"minecraft:powder_snow_cauldron[level={n}]" for n in (1, 2, 3)])

BANNER = ('minecraft:white_banner{BlockEntityTag:{Patterns:[{Pattern:"bs",Color:14},'
          '{Pattern:"ts",Color:11}]}}')
DYED = "minecraft:leather_chestplate{display:{color:16711680}}"

# (nom, état de départ, objet en main)
CASES: list[tuple[str, str, str]] = [
    ("water-bucket-into-empty", "minecraft:cauldron", "minecraft:water_bucket"),
    ("lava-bucket-into-empty", "minecraft:cauldron", "minecraft:lava_bucket"),
    ("snow-bucket-into-empty", "minecraft:cauldron", "minecraft:powder_snow_bucket"),
    ("bucket-from-water-3", "minecraft:water_cauldron[level=3]", "minecraft:bucket"),
    ("bucket-from-water-2", "minecraft:water_cauldron[level=2]", "minecraft:bucket"),
    ("bucket-from-lava", "minecraft:lava_cauldron", "minecraft:bucket"),
    ("bucket-from-snow-3", "minecraft:powder_snow_cauldron[level=3]", "minecraft:bucket"),
    ("bottle-from-water-3", "minecraft:water_cauldron[level=3]", "minecraft:glass_bottle"),
    ("bottle-from-water-1", "minecraft:water_cauldron[level=1]", "minecraft:glass_bottle"),
    ("water-bottle-into-empty", "minecraft:cauldron", 'minecraft:potion{Potion:"minecraft:water"}'),
    ("water-bottle-into-2", "minecraft:water_cauldron[level=2]",
     'minecraft:potion{Potion:"minecraft:water"}'),
    ("water-bottle-into-3", "minecraft:water_cauldron[level=3]",
     'minecraft:potion{Potion:"minecraft:water"}'),
    ("water-bucket-into-water-1", "minecraft:water_cauldron[level=1]", "minecraft:water_bucket"),
    ("wash-leather", "minecraft:water_cauldron[level=3]", DYED),
    ("wash-banner", "minecraft:water_cauldron[level=3]", BANNER),
    ("wash-leather-empty", "minecraft:cauldron", DYED),
]


def main() -> int:
    RUN.parent.mkdir(parents=True, exist_ok=True)
    server = CraftServer(RUN, port=PORT)
    out: dict = {}
    try:
        server.batch(["gamerule doMobSpawning false", "difficulty peaceful", "time set noon",
                      "gamerule randomTickSpeed 0", "weather clear 100000"])
        probe = Probe(PORT, NAME)
        for _ in range(40):
            probe.settle(0.5)
            if any(NAME in line for line in server.batch(["list"])):
                break
        server.batch([f"gamemode survival {NAME}", f"tp {NAME} {STAND[0]} {STAND[1]} {STAND[2]}"])
        probe.settle(1.0)
        x, y, z = BLOCK

        def state() -> list[str]:
            lines = server.batch([f"execute if block {x} {y} {z} {s} run say is {s}"
                                  for s in STATES])
            return [line[line.find("is ") + 3:] for line in lines if " is minecraft:" in line]

        for name, start, held in CASES:
            server.batch([f"clear {NAME}", f"setblock {x} {y} {z} {start}",
                          f"item replace entity {NAME} hotbar.0 with {held}"])
            probe.settle(0.3)
            probe.stand(*STAND)
            probe.send(SB_USE_ITEM_ON, varint(0) + block_pos(x, y, z) + varint(1)
                       + struct.pack(">fff", 0.5, 1.0, 0.5) + bytes([0]) + varint(0))
            probe.settle(0.5)
            inventory = [l[l.find("data:"):] for l in server.batch(
                [f"data get entity {NAME} Inventory"]) if "entity data" in l]
            out[name] = {"start": start, "held": held, "after": state(), "inventory": inventory}
            print(f"{name}: {out[name]['after']} {inventory}", flush=True)
        probe.s.close()
    finally:
        server.stop()
    OUT.write_text(json.dumps(out, indent=1))
    print(f"écrit {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
