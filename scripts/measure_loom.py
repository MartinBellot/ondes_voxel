#!/usr/bin/env python3
"""Le métier à tisser contre le vrai serveur 1.20.1 : le menu, la liste des
motifs par indice de bouton, les objets à motif et la limite de six couches.

Le client choisit un motif par son **indice** (`Click Container Button`, 0x0A)
dans une liste qu'il calcule : sans objet à motif, les motifs qui n'en
demandent pas ; avec un objet, les motifs de cet objet. Un indice invalide ne
change rien : la propriété de fenêtre 0 (le motif choisi), relue après chaque
clic, dit où la liste s'arrête.

Relevé aussi : `Open Screen`, le nombre de cases, le NBT de la bannière
produite (`BlockEntityTag.Patterns`), ce que coûte une prise, l'ordre d'ajout
d'une couche, la limite de six couches et où le shift-clic envoie chaque objet.

Sortie : `.scratch/loom.json`. Usage : python3 scripts/measure_loom.py
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from measure_crafting import (  # noqa: E402
    CB_CONTAINER_CONTENT, CB_CONTAINER_SLOT, CB_OPEN_SCREEN, SB_CLOSE_CONTAINER, SB_USE_ITEM_ON,
    Probe, Server as CraftServer)
from measure_smithing import read_slot_nbt  # noqa: E402
from vanilla_miner import block_pos, read_varint, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / ".scratch" / "measure-loom"
OUT = ROOT / ".scratch" / "loom.json"
REGISTRIES = ROOT / "data" / "vanilla" / "1.20.1" / "generated" / "reports" / "registries.json"
PORT = 25620
NAME = "Loom0"
BLOCK = (4, -60, 32)
STAND = (4.5, -60.0, 34.5)
CB_CONTAINER_PROPERTY = 0x13
SB_CLICK_BUTTON = 0x0A
BANNER, DYE, PATTERN, RESULT = 0, 1, 2, 3

PATTERN_ITEMS = ["flower_banner_pattern", "creeper_banner_pattern", "skull_banner_pattern",
                 "mojang_banner_pattern", "globe_banner_pattern", "piglin_banner_pattern"]


def layers(n: int) -> str:
    """Une bannière blanche portant n couches, en SNBT."""
    cells = ",".join('{Pattern:"bs",Color:14}' for _ in range(n))
    return f"minecraft:white_banner{{BlockEntityTag:{{Patterns:[{cells}]}}}}"


class LoomProbe(Probe):
    def __init__(self, port: int, name: str) -> None:
        super().__init__(port, name)
        self.window = None
        self.menu_type: int | None = None
        self.title: str | None = None
        self.slot_count: int | None = None
        self.full: dict[int, object] = {}
        self.carried = None
        self.selected: int | None = None

    def handle(self, pid: int, payload: bytes) -> None:
        if pid == CB_OPEN_SCREEN:
            window, i = read_varint(payload, 0)
            self.menu_type, i = read_varint(payload, i)
            n, i = read_varint(payload, i)
            self.title = payload[i:i + n].decode("utf-8", "replace")
            self.window = window
            self.full = {}
            self.selected = None
        elif pid == CB_CONTAINER_CONTENT and self.window is not None and payload[0] == self.window:
            state, i = read_varint(payload, 1)
            count, i = read_varint(payload, i)
            self.state_id = state
            self.slot_count = count
            for index in range(count):
                self.full[index], i = read_slot_nbt(payload, i)
            self.carried, i = read_slot_nbt(payload, i)
        elif pid == CB_CONTAINER_SLOT:
            window = struct.unpack_from(">b", payload, 0)[0]
            state, i = read_varint(payload, 1)
            slot = struct.unpack_from(">h", payload, i)[0]
            i += 2
            stack, i = read_slot_nbt(payload, i)
            if window == -1 and slot == -1:
                self.carried = stack
            elif self.window is not None and window == self.window:
                self.state_id = state
                self.full[slot] = stack
        elif pid == CB_CONTAINER_PROPERTY and self.window is not None and payload[0] == self.window:
            prop, value = struct.unpack_from(">hh", payload, 1)
            if prop == 0:
                self.selected = value

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
        raise RuntimeError("le métier à tisser ne s'est pas ouvert")

    def button(self, index: int) -> None:
        # 0,25 s ne suffisait pas : sept réponses sur trente-quatre arrivaient
        # après la lecture et passaient pour des refus.
        self.send(SB_CLICK_BUTTON, bytes([self.window, index]))
        self.settle(0.6)

    def close(self) -> None:
        if self.window is not None:
            self.send(SB_CLOSE_CONTAINER, bytes([self.window]))
            self.window = None
            self.settle(0.3)


def main() -> int:
    ids = json.loads(REGISTRIES.read_text())["minecraft:item"]["entries"]
    names = {v["protocol_id"]: k for k, v in ids.items()}

    def show(stack):
        if stack is None:
            return None
        item, count, tag = stack
        return {"item": names.get(item, item), "count": count, "nbt": tag}

    RUN.parent.mkdir(parents=True, exist_ok=True)
    server = CraftServer(RUN, port=PORT)
    out: dict = {}
    try:
        server.batch(["gamerule doMobSpawning false", "difficulty peaceful", "time set noon",
                      f"setblock {BLOCK[0]} {BLOCK[1]} {BLOCK[2]} minecraft:loom"])
        probe = LoomProbe(PORT, NAME)
        for _ in range(40):
            probe.settle(0.5)
            if any(NAME in line for line in server.batch(["list"])):
                break
        server.batch([f"gamemode survival {NAME}", f"tp {NAME} {STAND[0]} {STAND[1]} {STAND[2]}"])
        probe.settle(1.0)

        def load(hotbar: list[str], backpack: list[str] | None = None) -> None:
            probe.close()
            server.batch([f"clear {NAME}", "kill @e[type=minecraft:item]"]
                         + [f"item replace entity {NAME} hotbar.{b} with {what}"
                            for b, what in enumerate(hotbar)]
                         + [f"item replace entity {NAME} inventory.{b} with {what}"
                            for b, what in enumerate(backpack or [])])
            probe.settle(0.4)
            probe.open_block()
            if "_menu" not in out:
                out["_menu"] = {"menu_type": probe.menu_type, "title": probe.title,
                                "slot_count": probe.slot_count}
            for slot in range(len(hotbar)):
                probe.click(slot, slot, 2)  # la barre b dans la case b de la table
                probe.settle(0.25)

        def sweep(limit: int) -> list[dict]:
            seen = []
            for index in range(limit):
                probe.button(index)
                seen.append({"index": index, "selected": probe.selected,
                             "result": show(probe.full.get(RESULT))})
            return seen

        # 1. Sans objet à motif : chaque indice.
        load(["minecraft:white_banner", "minecraft:red_dye 4"])
        out["plain"] = sweep(36)
        print(f"plain: {sum(1 for s in out['plain'] if s['selected'] == s['index'])} selectable",
              flush=True)

        # 2. Avec chaque objet à motif.
        out["pattern_items"] = {}
        for pattern in PATTERN_ITEMS:
            load(["minecraft:white_banner", "minecraft:red_dye 4", f"minecraft:{pattern}"])
            out["pattern_items"][pattern] = sweep(4)
        print(f"pattern items: { {k: v[0]['result'] for k, v in out['pattern_items'].items()} }",
              flush=True)

        # 3. Une prise : ce qui reste, le NBT de la bannière emportée.
        load(["minecraft:white_banner 2", "minecraft:red_dye 4", "minecraft:creeper_banner_pattern"])
        probe.button(0)
        probe.click(RESULT, 0, 0)
        probe.settle(0.4)
        taken = {"slots": {str(s): show(probe.full.get(s)) for s in range(4)},
                 "carried": show(probe.carried)}
        probe.click(4, 0, 0)  # le curseur dans le sac 0
        probe.settle(0.3)
        probe.close()
        taken["inventory"] = [l[l.find("data:"):] for l in server.batch(
            [f"data get entity {NAME} Inventory"]) if "entity data" in l]
        out["take"] = taken
        print(f"take: {taken['slots']}", flush=True)

        # 4. Une couche de plus : où va-t-elle ; et la limite de six.
        out["layers"] = {}
        # Six couches ne bloquaient pas la première campagne : où est la limite ?
        for n in (1, 5, 6, 7, 10, 16, 20):
            load([layers(n), "minecraft:red_dye 4"])
            probe.button(1)
            out["layers"][str(n)] = {"selected": probe.selected,
                                     "result": show(probe.full.get(RESULT))}
        print(f"layers: { {k: v['result'] is not None for k, v in out['layers'].items()} }",
              flush=True)

        # 5. Shift-clic depuis le sac : cases 4, 5, 6 de la fenêtre.
        load([], backpack=["minecraft:creeper_banner_pattern", "minecraft:red_dye",
                           "minecraft:white_banner"])
        for slot in (4, 5, 6):
            probe.click(slot, 0, 1)
            probe.settle(0.3)
        out["shift_in"] = {str(s): show(v) for s, v in sorted(probe.full.items()) if v is not None}
        print(f"shift_in: {out['shift_in']}", flush=True)
        probe.close()
        probe.s.close()
    finally:
        server.stop()
    OUT.write_text(json.dumps(out, indent=1, default=str))
    print(f"écrit {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
