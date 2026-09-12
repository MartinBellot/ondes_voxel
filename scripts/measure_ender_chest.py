#!/usr/bin/env python3
"""Le coffre de l'Ender contre le vrai serveur 1.20.1 : le menu, le NBT
`EnderItems` du joueur, et ce que le bloc lui-même garde.

Une sonde en survie ouvre un coffre de l'Ender et relit :

  * `Open Screen` : le type de menu, le titre ; le nombre de cases ;
  * après y avoir mis deux piles et fermé : `data get entity … EnderItems` —
    les objets sont au joueur, pas au bloc ;
  * à la réouverture : le contenu renvoyé ;
  * `data get block` sur le coffre : ce que le bloc-entité porte ;
  * les paquets `Block Action` reçus pendant l'ouverture et la fermeture (le
    couvercle), relevés tels quels.

Sortie : `.scratch/ender_chest.json`. Usage : python3 scripts/measure_ender_chest.py
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from measure_crafting import SB_CLOSE_CONTAINER, SB_USE_ITEM_ON, Server as CraftServer  # noqa: E402
from measure_loom import LoomProbe  # noqa: E402
from vanilla_miner import block_pos, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / ".scratch" / "measure-ender"
OUT = ROOT / ".scratch" / "ender_chest.json"
REGISTRIES = ROOT / "data" / "vanilla" / "1.20.1" / "generated" / "reports" / "registries.json"
PORT = 25621
NAME = "Ender0"
BLOCK = (4, -60, 38)
STAND = (4.5, -60.0, 40.5)
CB_BLOCK_ACTION = 0x08


class EnderProbe(LoomProbe):
    def __init__(self, port: int, name: str) -> None:
        super().__init__(port, name)
        self.block_actions: list[str] = []

    def handle(self, pid: int, payload: bytes) -> None:
        if pid == CB_BLOCK_ACTION:
            self.block_actions.append(payload.hex())
        super().handle(pid, payload)

    def open_block(self) -> None:
        self.window = None
        for _ in range(8):
            self.stand(*STAND)
            self.settle(0.3)
            self.send(SB_USE_ITEM_ON, varint(0) + block_pos(*BLOCK) + varint(1)
                      + struct.pack(">fff", 0.5, 1.0, 0.5) + bytes([0]) + varint(0))
            for _ in range(20):
                if self.window is not None:
                    self.settle(0.3)
                    return
                self.settle(0.1)
        raise RuntimeError("le coffre de l'Ender ne s'est pas ouvert")

    def close(self) -> None:
        if self.window is not None:
            self.send(SB_CLOSE_CONTAINER, bytes([self.window]))
            self.window = None
            self.settle(0.5)


def main() -> int:
    ids = json.loads(REGISTRIES.read_text())["minecraft:item"]["entries"]
    names = {v["protocol_id"]: k for k, v in ids.items()}

    def show(stack):
        return None if stack is None else {"item": names.get(stack[0], stack[0]),
                                           "count": stack[1], "nbt": stack[2]}

    RUN.parent.mkdir(parents=True, exist_ok=True)
    server = CraftServer(RUN, port=PORT)
    out: dict = {}
    try:
        server.batch(["gamerule doMobSpawning false", "difficulty peaceful", "time set noon",
                      f"setblock {BLOCK[0]} {BLOCK[1]} {BLOCK[2]} minecraft:ender_chest"])
        probe = EnderProbe(PORT, NAME)
        for _ in range(40):
            probe.settle(0.5)
            if any(NAME in line for line in server.batch(["list"])):
                break
        server.batch([f"gamemode survival {NAME}", f"tp {NAME} {STAND[0]} {STAND[1]} {STAND[2]}",
                      f"clear {NAME}",
                      f"item replace entity {NAME} hotbar.0 with minecraft:diamond 5",
                      f"item replace entity {NAME} hotbar.1 with minecraft:dirt 64"])
        probe.settle(1.0)

        probe.block_actions.clear()
        probe.open_block()
        out["menu"] = {"menu_type": probe.menu_type, "title": probe.title,
                       "slot_count": probe.slot_count}
        out["actions_open"] = list(probe.block_actions)
        probe.click(0, 0, 2)    # la barre 0 dans la case 0 du coffre
        probe.settle(0.3)
        probe.click(26, 1, 2)   # la barre 1 dans la case 26
        probe.settle(0.3)
        probe.block_actions.clear()
        probe.close()
        out["actions_close"] = list(probe.block_actions)
        out["ender_items"] = [l[l.find("data:"):] for l in server.batch(
            [f"data get entity {NAME} EnderItems"]) if "entity data" in l]
        out["block"] = [l[l.find("data:"):] for l in server.batch(
            [f"data get block {BLOCK[0]} {BLOCK[1]} {BLOCK[2]}"]) if "block data" in l]
        probe.open_block()
        out["reopened"] = {str(s): show(v) for s, v in sorted(probe.full.items())
                           if v is not None and s < 27}
        probe.close()
        print(f"menu={out['menu']} ender={out['ender_items']} reopened={out['reopened']}",
              flush=True)
        probe.s.close()
    finally:
        server.stop()
    OUT.write_text(json.dumps(out, indent=1, default=str))
    print(f"écrit {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
