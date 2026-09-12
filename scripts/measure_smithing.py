#!/usr/bin/env python3
"""La table de forge contre le vrai serveur 1.20.1 : le menu, l'amélioration en
netherite et les garnitures, NBT compris.

Une sonde en survie ouvre une table de forge et relit, sur le fil :

  * `Open Screen` : l'identifiant de fenêtre, le **type de menu** et le titre ;
  * `Set Container Content` : le nombre de cases et chacune, **NBT décodé** ;
  * la case de résultat après chaque pose, puis l'inventaire tel que
    `data get entity … Inventory` l'imprime une fois l'objet pris.

Les objets sont posés dans la barre par `item replace`, puis échangés dans les
cases de la table par un clic en mode 2 (la case s, la barre b : clic(s, b, 2)).
Chaque cas se termine par une fermeture et un `clear`.

Sortie : `.scratch/smithing.json`. Usage : python3 scripts/measure_smithing.py
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from measure_crafting import (  # noqa: E402
    CB_CONTAINER_CONTENT, CB_CONTAINER_SLOT, CB_OPEN_SCREEN, SB_CLOSE_CONTAINER, SB_USE_ITEM_ON,
    Probe, Server as CraftServer, skip_nbt)
from measure_player_data import read_nbt  # noqa: E402
from vanilla_miner import block_pos, read_varint, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / ".scratch" / "measure-smithing"
OUT = ROOT / ".scratch" / "smithing.json"
REGISTRIES = ROOT / "data" / "vanilla" / "1.20.1" / "generated" / "reports" / "registries.json"
PORT = 25618
NAME = "Smith0"
TABLE = (4, -60, 20)
STAND = (4.5, -60.0, 22.5)

# La table de forge : 0 le gabarit, 1 la base, 2 l'ajout, 3 le résultat, puis
# les 36 cases du joueur. Relu sur le fil, pas supposé : `slot_count` le dit.
TEMPLATE, BASE, ADDITION, RESULT = 0, 1, 2, 3


def read_string(buf: bytes, i: int) -> tuple[str, int]:
    n, i = read_varint(buf, i)
    return buf[i:i + n].decode("utf-8", "replace"), i + n


def read_slot_nbt(buf: bytes, i: int):
    """Une case, NBT décodé : (id, nombre, nbt ou None) ou None."""
    if not buf[i]:
        return None, i + 1
    i += 1
    item, i = read_varint(buf, i)
    count = buf[i]
    i += 1
    start = i
    i = skip_nbt(buf, i)
    tag = read_nbt(buf[start:i]) if buf[start] == 10 else None
    return (item, count, tag), i


class SmithProbe(Probe):
    def __init__(self, port: int, name: str) -> None:
        super().__init__(port, name)
        self.window = None
        self.menu_type: int | None = None
        self.title: str | None = None
        self.slot_count: int | None = None
        self.full: dict[int, object] = {}
        self.carried = None

    def handle(self, pid: int, payload: bytes) -> None:
        if pid == CB_OPEN_SCREEN:
            window, i = read_varint(payload, 0)
            self.menu_type, i = read_varint(payload, i)
            self.title, i = read_string(payload, i)
            self.window = window
            self.full = {}
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

    def open_table(self) -> None:
        self.window = None
        for _ in range(8):
            self.stand(*STAND)
            self.settle(0.3)
            self.send(SB_USE_ITEM_ON, varint(0) + block_pos(*TABLE) + varint(1)
                      + struct.pack(">fff", 0.5, 1.0, 0.5) + bytes([0]) + varint(0))
            for _ in range(20):
                if self.window is not None:
                    self.settle(0.3)
                    return
                self.settle(0.1)
        raise RuntimeError("la table de forge ne s'est pas ouverte")

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
                      f"setblock {TABLE[0]} {TABLE[1]} {TABLE[2]} minecraft:smithing_table"])
        probe = SmithProbe(PORT, NAME)
        for _ in range(40):
            probe.settle(0.5)
            if any(NAME in line for line in server.batch(["list"])):
                break
        server.batch([f"gamemode survival {NAME}", f"tp {NAME} {STAND[0]} {STAND[1]} {STAND[2]}"])
        probe.settle(1.0)

        hotbar_first: list[int] = []

        def case(name: str, hotbar: list[str], swaps: list[tuple[int, int]],
                 then: list[tuple[int, int, int]] | None = None, backpack: list[str] | None = None):
            server.batch([f"clear {NAME}", "kill @e[type=minecraft:item]"]
                         + [f"item replace entity {NAME} hotbar.{b} with {what}"
                            for b, what in enumerate(hotbar)]
                         + [f"item replace entity {NAME} inventory.{b} with {what}"
                            for b, what in enumerate(backpack or [])])
            probe.settle(0.4)
            probe.open_table()
            opened = {"menu_type": probe.menu_type, "title": probe.title,
                      "slot_count": probe.slot_count}
            if not hotbar_first:
                hotbar_first.append((probe.slot_count or 40) - 9)
            for slot, button in swaps:
                probe.click(slot, button, 2)
                probe.settle(0.3)
            placed = {str(s): show(probe.full.get(s)) for s in range(4)}
            for slot, button, mode in then or []:
                probe.click(slot, button, mode)
                probe.settle(0.3)
            after = {str(s): show(probe.full.get(s)) for s in range(probe.slot_count or 40)
                     if probe.full.get(s) is not None}
            carried = show(probe.carried)
            probe.close()
            inventory = [l[l.find("data:"):] for l in server.batch(
                [f"data get entity {NAME} Inventory"]) if "entity data" in l]
            out[name] = {"opened": opened, "placed": placed, "after": after, "carried": carried,
                         "inventory": inventory}
            print(f"{name}: result={placed['3']} carried={carried}", flush=True)

        coast = "minecraft:coast_armor_trim_smithing_template"
        upgrade = "minecraft:netherite_upgrade_smithing_template"
        into_table = [(TEMPLATE, 0), (BASE, 1), (ADDITION, 2)]
        take_to_backpack = [(RESULT, 0, 0), (4, 0, 0)]  # le curseur posé en case 4, le sac

        case("trim", [coast, "minecraft:iron_chestplate", "minecraft:gold_ingot"], into_table,
             take_to_backpack)
        case("netherite",
             [upgrade, 'minecraft:diamond_sword{Damage:10,Enchantments:[{id:"minecraft:sharpness",'
                       "lvl:2s}],display:{Name:'\"Blade\"'}}", "minecraft:netherite_ingot"],
             into_table, take_to_backpack)
        case("invalid", [coast, "minecraft:diamond_sword", "minecraft:gold_ingot"], into_table)
        trimmed = ('minecraft:iron_chestplate{Trim:{material:"minecraft:gold",'
                   'pattern:"minecraft:coast"}}')
        case("retrim-same", [coast, trimmed, "minecraft:gold_ingot"], into_table)
        case("retrim-other", [coast, trimmed, "minecraft:emerald"], into_table, take_to_backpack)
        case("leather", [coast, "minecraft:leather_chestplate{display:{color:16711680}}",
                         "minecraft:gold_ingot"], into_table, take_to_backpack)
        case("counts", [f"{coast} 2", "minecraft:iron_chestplate", "minecraft:gold_ingot 2"],
             into_table, [(RESULT, 0, 0), (4, 0, 0)])
        case("result-shift", [coast, "minecraft:iron_chestplate", "minecraft:gold_ingot"],
             into_table, [(RESULT, 0, 1)])
        # Shift-clic depuis le sac : cases 4, 5, 6 de la fenêtre (le sac 0, 1, 2).
        case("shift-in", [], [], [(4, 0, 1), (5, 0, 1), (6, 0, 1)],
             backpack=["minecraft:gold_ingot", "minecraft:iron_chestplate", coast])
        out["_hotbar_first"] = hotbar_first
        probe.s.close()
    finally:
        server.stop()
    OUT.write_text(json.dumps(out, indent=1, default=str))
    print(f"écrit {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
