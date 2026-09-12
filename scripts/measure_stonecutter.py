#!/usr/bin/env python3
"""Le tailleur de pierre contre le vrai serveur 1.20.1 : le menu, l'ordre des
recettes et l'indice du bouton.

Le client choisit une recette par son **indice** dans une liste qu'il calcule
lui-même (`Click Container Button`, 0x0A : fenêtre, bouton). Le serveur doit
calculer la même liste dans le même ordre, sinon le bouton 3 donne une autre
coupe que celle que le joueur a vue. Cet ordre n'est dans aucun fichier : on le
relit en cliquant chaque indice et en lisant la case de résultat.

Relevé aussi : `Open Screen` (type de menu, titre), le nombre de cases, la
propriété de fenêtre envoyée, ce que coûte une prise et un shift-clic, et si le
choix survit à un changement d'entrée.

Sortie : `.scratch/stonecutter.json`. Usage : python3 scripts/measure_stonecutter.py
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from measure_crafting import (  # noqa: E402
    CB_CONTAINER_CONTENT, CB_CONTAINER_SLOT, CB_OPEN_SCREEN, SB_CLOSE_CONTAINER, SB_USE_ITEM_ON,
    Probe, Server as CraftServer, read_slot)
from vanilla_miner import block_pos, read_varint, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / ".scratch" / "measure-stonecutter"
OUT = ROOT / ".scratch" / "stonecutter.json"
REGISTRIES = ROOT / "data" / "vanilla" / "1.20.1" / "generated" / "reports" / "registries.json"
PORT = 25619
NAME = "Cutter0"
BLOCK = (4, -60, 26)
STAND = (4.5, -60.0, 28.5)
CB_CONTAINER_PROPERTY = 0x13
SB_CLICK_BUTTON = 0x0A

INPUTS = ["stone", "cobblestone", "andesite", "copper_block", "quartz_block", "sandstone",
          "blackstone", "mud_bricks"]


class CutterProbe(Probe):
    def __init__(self, port: int, name: str) -> None:
        super().__init__(port, name)
        self.window = None
        self.menu_type: int | None = None
        self.title: str | None = None
        self.slot_count: int | None = None
        self.carried = None
        self.properties: list[tuple[int, int]] = []

    def handle(self, pid: int, payload: bytes) -> None:
        if pid == CB_OPEN_SCREEN:
            window, i = read_varint(payload, 0)
            self.menu_type, i = read_varint(payload, i)
            n, i = read_varint(payload, i)
            self.title = payload[i:i + n].decode("utf-8", "replace")
            self.window = window
            self.slots = {}
        elif pid == CB_CONTAINER_CONTENT and self.window is not None and payload[0] == self.window:
            state, i = read_varint(payload, 1)
            count, i = read_varint(payload, i)
            self.state_id = state
            self.slot_count = count
            for index in range(count):
                self.slots[index], i = read_slot(payload, i)
            self.carried, i = read_slot(payload, i)
        elif pid == CB_CONTAINER_SLOT:
            window = struct.unpack_from(">b", payload, 0)[0]
            state, i = read_varint(payload, 1)
            slot = struct.unpack_from(">h", payload, i)[0]
            i += 2
            stack, i = read_slot(payload, i)
            if window == -1 and slot == -1:
                self.carried = stack
            elif self.window is not None and window == self.window:
                self.state_id = state
                self.slots[slot] = stack
        elif pid == CB_CONTAINER_PROPERTY and self.window is not None and payload[0] == self.window:
            prop, value = struct.unpack_from(">hh", payload, 1)
            self.properties.append((prop, value))

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
        raise RuntimeError("le tailleur de pierre ne s'est pas ouvert")

    def button(self, index: int) -> None:
        self.send(SB_CLICK_BUTTON, bytes([self.window, index]))
        self.settle(0.25)

    def close(self) -> None:
        if self.window is not None:
            self.send(SB_CLOSE_CONTAINER, bytes([self.window]))
            self.window = None
            self.settle(0.3)


def main() -> int:
    ids = json.loads(REGISTRIES.read_text())["minecraft:item"]["entries"]
    names = {v["protocol_id"]: k for k, v in ids.items()}

    def show(stack):
        return None if stack is None else f"{names.get(stack[0], stack[0])} x{stack[1]}"

    RUN.parent.mkdir(parents=True, exist_ok=True)
    server = CraftServer(RUN, port=PORT)
    out: dict = {"orders": {}}
    try:
        server.batch(["gamerule doMobSpawning false", "difficulty peaceful", "time set noon",
                      f"setblock {BLOCK[0]} {BLOCK[1]} {BLOCK[2]} minecraft:stonecutter"])
        probe = CutterProbe(PORT, NAME)
        for _ in range(40):
            probe.settle(0.5)
            if any(NAME in line for line in server.batch(["list"])):
                break
        server.batch([f"gamemode survival {NAME}", f"tp {NAME} {STAND[0]} {STAND[1]} {STAND[2]}"])
        probe.settle(1.0)

        # L'ordre : pour chaque entrée, chaque indice jusqu'au premier vide.
        for material in INPUTS:
            server.batch([f"clear {NAME}",
                          f"item replace entity {NAME} hotbar.0 with minecraft:{material} 16"])
            probe.settle(0.3)
            probe.open_block()
            if "_menu" not in out:
                out["_menu"] = {"menu_type": probe.menu_type, "title": probe.title,
                                "slot_count": probe.slot_count}
            probe.click(0, 0, 2)  # la barre 0 dans la case d'entrée
            probe.settle(0.4)
            order = []
            for index in range(40):
                probe.properties.clear()
                probe.button(index)
                result = show(probe.slots.get(1))
                if result is None:
                    order.append({"index": index, "result": None,
                                  "properties": list(probe.properties)})
                    break
                order.append({"index": index, "result": result,
                              "properties": list(probe.properties)})
            out["orders"][material] = order
            print(f"{material}: {[o['result'] for o in order]}", flush=True)
            probe.close()

        # Une prise, un shift-clic, un changement d'entrée.
        server.batch([f"clear {NAME}",
                      f"item replace entity {NAME} hotbar.0 with minecraft:stone 8",
                      f"item replace entity {NAME} hotbar.1 with minecraft:cobblestone 8"])
        probe.settle(0.3)
        probe.open_block()
        probe.click(0, 0, 2)
        probe.settle(0.3)
        probe.button(0)
        first = show(probe.slots.get(1))
        probe.click(1, 0, 0)  # prendre le résultat
        probe.settle(0.4)
        after_take = {"input": show(probe.slots.get(0)), "result": show(probe.slots.get(1)),
                      "carried": show(probe.carried)}
        probe.click(2, 0, 0)  # poser le curseur dans le sac (case 2 = sac 0)
        probe.settle(0.3)
        probe.button(0)
        probe.click(1, 0, 1)  # shift-clic sur le résultat
        probe.settle(0.6)
        after_shift = {s: show(v) for s, v in sorted(probe.slots.items()) if v is not None}
        # Changer d'entrée : la barre 1 (pavé) échangée dans la case d'entrée.
        probe.button(1)
        probe.properties.clear()
        probe.click(0, 1, 2)
        probe.settle(0.4)
        after_swap = {"input": show(probe.slots.get(0)), "result": show(probe.slots.get(1)),
                      "properties": list(probe.properties)}
        probe.close()
        inventory = [l[l.find("data:"):] for l in server.batch(
            [f"data get entity {NAME} Inventory"]) if "entity data" in l]
        out["take"] = {"first": first, "after_take": after_take, "after_shift": after_shift,
                       "after_swap": after_swap, "inventory": inventory}
        print(f"take: {out['take']}", flush=True)
        probe.s.close()
    finally:
        server.stop()
    OUT.write_text(json.dumps(out, indent=1))
    print(f"écrit {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
