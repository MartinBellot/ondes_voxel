#!/usr/bin/env python3
"""Ask a real 1.20.1 server what enchanting, the anvil and the grindstone do.

None of the numbers behind these three blocks are in the data generator. The
table's cost formula, the weights, the per-level cost windows, the anvil's
multipliers and the grindstone's refund are Java constants; the wiki documents
most of them and says of the grindstone "What is the formula? Info needed".
Every campaign below drives the real server and writes down what it did.

  table        The enchanting table's ten Container Property values (three
               costs, the seed, three clue ids, three clue levels) for many
               (XpSeed, bookshelves, item) triples, and — once per seed — what
               clicking one of the three buttons actually put on the item, how
               many levels and how much lapis it took, and the seed after.
               XpSeed is read with `/data get entity`, so the offline check can
               be *exact*: same seed, same offers.

  obstruction  One bookshelf at each of the 32 offsets, with a blocker between
               it and the table. Whether it counted is read off the costs at a
               seed where 0 and 1 bookshelf give different costs.

  anvil        A panel of combinations: the cost (property 0) and the output
               stack, NBT included. Then some outputs taken in survival for the
               levels spent, and 150 renames for the 12 % degradation.

  grindstone   Outputs, and the experience a take pays out, repeated for its
               distribution.

  protection   `/damage` on the bot wearing a player head carrying the
               enchantment: a head has no armour points, so what is left of a
               hit is the enchantment alone.

  unbreaking   A hoe tilling grass, counted by the `used` statistic, against
               the Damage it ends with; and a helmet taking `/damage` hits.

  mending      An orb of known value summoned on a bot holding a damaged
               Mending pickaxe.

  weapons      Smite, Bane of Arthropods and Impaling against the mob group
               they name, and against a cow.

Usage: lockf /tmp/ov-vanilla.lock python3 scripts/measure_enchanting.py [campaign ...]
Writes data/vanilla/1.20.1/normalized/enchanting.json, merged campaign by
campaign so a run that dies late keeps what it learned.
"""
from __future__ import annotations

import json
import os
import random
import re
import shutil
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from measure_entities import Server as BaseServer  # noqa: E402
from vanilla_miner import Miner, block_pos, read_varint, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = Path(os.environ.get("OV_NORMALIZED", ROOT / "data/vanilla/1.20.1/normalized"))
REGISTRIES = ROOT / "data/vanilla/1.20.1/generated/reports/registries.json"
RUN = ROOT / "run" / "measure-enchanting"
PORT = 25623
BOT = "ovench"
OUT = NORMALIZED / "enchanting.json"


class Server(BaseServer):
    HEAP = "-Xmx768M"


# ── wire ────────────────────────────────────────────────────────────────────
CB_SPAWN_ENTITY = 0x01
CB_CONTAINER_CONTENT = 0x12
CB_CONTAINER_PROPERTY = 0x13
CB_CONTAINER_SLOT = 0x14
CB_OPEN_SCREEN = 0x30

SB_CLICK_BUTTON = 0x0A
SB_CLICK_CONTAINER = 0x0B
SB_CLOSE_CONTAINER = 0x0C
SB_INTERACT = 0x10
SB_RENAME_ITEM = 0x23
SB_SET_HELD_ITEM = 0x28
SB_SWING_ARM = 0x2F
SB_USE_ITEM_ON = 0x31

DATA = re.compile(r"following entity data: (.*)$")
XP = re.compile(r"has (\d+) experience (levels|points)")
SCORE = re.compile(r"has (\d+) \[")

# The rig. The table sits at T; its bookshelf ring is x,z in [-2,2] at the
# table's height and one above. The bot stands inside the ring.
T = (0, -60, 0)
STAND = (1.5, -60.0, 0.5)
ANVIL = (5, -60, 0)
GRINDSTONE = (5, -60, 2)
TILL = (0, -61, -4)


def ring_offsets() -> list[tuple[int, int, int]]:
    out = []
    for dy in (0, 1):
        for dx in range(-2, 3):
            for dz in range(-2, 3):
                if abs(dx) == 2 or abs(dz) == 2:
                    out.append((dx, dy, dz))
    return out


RING = ring_offsets()


def java_div(a: int, b: int) -> int:
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


# ── NBT off the wire (network form: a named root) ───────────────────────────
def read_tag(buf: bytes, i: int, kind: int):
    if kind == 1:
        return struct.unpack_from(">b", buf, i)[0], i + 1
    if kind == 2:
        return struct.unpack_from(">h", buf, i)[0], i + 2
    if kind == 3:
        return struct.unpack_from(">i", buf, i)[0], i + 4
    if kind == 4:
        return struct.unpack_from(">q", buf, i)[0], i + 8
    if kind == 5:
        return struct.unpack_from(">f", buf, i)[0], i + 4
    if kind == 6:
        return struct.unpack_from(">d", buf, i)[0], i + 8
    if kind == 7:
        n = struct.unpack_from(">i", buf, i)[0]
        return list(buf[i + 4:i + 4 + n]), i + 4 + n
    if kind == 8:
        n = struct.unpack_from(">H", buf, i)[0]
        return buf[i + 2:i + 2 + n].decode("utf-8", "replace"), i + 2 + n
    if kind == 9:
        inner = buf[i]
        n = struct.unpack_from(">i", buf, i + 1)[0]
        i += 5
        out = []
        for _ in range(max(n, 0)):
            v, i = read_tag(buf, i, inner)
            out.append(v)
        return out, i
    if kind == 10:
        out = {}
        while True:
            child = buf[i]
            i += 1
            if child == 0:
                return out, i
            n = struct.unpack_from(">H", buf, i)[0]
            name = buf[i + 2:i + 2 + n].decode("utf-8", "replace")
            i += 2 + n
            out[name], i = read_tag(buf, i, child)
    if kind in (11, 12):
        n = struct.unpack_from(">i", buf, i)[0]
        size = 4 if kind == 11 else 8
        fmt = ">i" if kind == 11 else ">q"
        return [struct.unpack_from(fmt, buf, i + 4 + k * size)[0] for k in range(n)], i + 4 + n * size
    raise ValueError(f"tag {kind}")


def read_slot(buf: bytes, i: int):
    present = buf[i]
    i += 1
    if not present:
        return None, i
    item, i = read_varint(buf, i)
    count = buf[i]
    i += 1
    kind = buf[i]
    i += 1
    tag = None
    if kind != 0:
        n = struct.unpack_from(">H", buf, i)[0]
        i += 2 + n
        tag, i = read_tag(buf, i, kind)
    return {"item": item, "count": count, "tag": tag}, i


# ── the client ──────────────────────────────────────────────────────────────
class Probe(Miner):
    def __init__(self, port: int, name: str, items: dict[int, str]) -> None:
        super().__init__(port, name)
        self.items = items
        self.window = None
        self.menu = None
        self.state_id = 0
        self.slots: dict[int, dict | None] = {}
        self.props: dict[int, int] = {}
        self.spawned: list[tuple[int, int, float, float, float]] = []

    def handle(self, pid: int, p: bytes) -> None:
        if pid == CB_OPEN_SCREEN:
            window, i = read_varint(p, 0)
            menu, _ = read_varint(p, i)
            self.window, self.menu = window, menu
            self.slots, self.props = {}, {}
        elif pid == CB_CONTAINER_CONTENT and self.window is not None and p[0] == self.window:
            state, i = read_varint(p, 1)
            count, i = read_varint(p, i)
            self.state_id = state
            for k in range(count):
                self.slots[k], i = read_slot(p, i)
        elif pid == CB_CONTAINER_SLOT:
            window = struct.unpack_from(">b", p, 0)[0]
            state, i = read_varint(p, 1)
            slot = struct.unpack_from(">h", p, i)[0]
            stack, _ = read_slot(p, i + 2)
            if self.window is not None and window == self.window:
                self.state_id = state
                self.slots[slot] = stack
        elif pid == CB_CONTAINER_PROPERTY and len(p) == 5:
            window, prop, value = struct.unpack(">bhh", p)
            if self.window is not None and window == self.window:
                self.props[prop] = value
        elif pid == CB_SPAWN_ENTITY:
            eid, i = read_varint(p, 0)
            i += 16
            kind, i = read_varint(p, i)
            x, y, z = struct.unpack_from(">ddd", p, i)
            self.spawned.append((eid, kind, x, y, z))

    def settle(self, seconds: float = 0.25) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.pump(until=lambda pid, p: self.handle(pid, p), timeout=0.05)

    def click(self, slot: int, button: int, mode: int) -> None:
        self.send(SB_CLICK_CONTAINER,
                  bytes([self.window]) + varint(self.state_id) + struct.pack(">h", slot)
                  + struct.pack(">b", button) + varint(mode) + varint(0) + bytes([0]))

    def swap(self, slot: int, hotbar: int) -> None:
        self.click(slot, hotbar, 2)

    def use_on(self, pos, face: int = 1) -> None:
        self.send(SB_USE_ITEM_ON, varint(0) + block_pos(*pos) + varint(face)
                  + struct.pack(">fff", 0.5, 1.0, 0.5) + bytes([0]) + varint(0))

    def open_at(self, pos, tries: int = 8) -> None:
        for _ in range(tries):
            self.window = None
            self.stand(*STAND)
            self.settle(0.2)
            self.use_on(pos)
            deadline = time.monotonic() + 2.0
            while self.window is None and time.monotonic() < deadline:
                self.settle(0.1)
            if self.window is not None:
                self.settle(0.2)
                return
        raise RuntimeError(f"nothing opened at {pos}")

    def close(self) -> None:
        if self.window is not None:
            self.send(SB_CLOSE_CONTAINER, bytes([self.window]))
            self.window = None
            self.settle(0.15)

    def name_of(self, stack: dict | None) -> dict | None:
        if stack is None:
            return None
        return {"id": self.items.get(stack["item"], str(stack["item"])),
                "count": stack["count"], "tag": stack["tag"]}


# ── console helpers ─────────────────────────────────────────────────────────
def sync(server: "Server", probe: "Probe") -> None:
    """Wait until everything the probe sent has been handled and answered.

    Console lines run at the start of a tick, player packets after them: two
    round trips through the console put a full tick behind the last packet,
    and the menu's changes go out at the end of that player's tick."""
    probe.settle(0.1)
    server.batch([])
    probe.settle(0.1)
    server.batch([])
    probe.settle(0.25)


def data_value(server: Server, target: str, path: str) -> str | None:
    for line in server.batch([f"data get entity {target} {path}"]):
        m = DATA.search(line)
        if m:
            return m.group(1).strip()
    return None


def number(raw: str | None) -> float | None:
    if raw is None:
        return None
    try:
        return float(raw.rstrip("bsfdL"))
    except ValueError:
        return None


def xp_seed(server: Server) -> int:
    value = number(data_value(server, BOT, "XpSeed"))
    if value is None:
        raise RuntimeError("no XpSeed")
    return int(value)


def xp_query(server: Server, what: str) -> int:
    for line in server.batch([f"xp query {BOT} {what}"]):
        m = XP.search(line)
        if m:
            return int(m.group(1))
    raise RuntimeError(f"xp query {what}")


def total_xp(levels: int, points: int) -> int:
    total = 0
    for level in range(levels):
        total += 2 * level + 7 if level < 16 else (5 * level - 38 if level < 31 else 9 * level - 158)
    return total + points


def ench(*pairs: tuple[str, int], stored: bool = False) -> str:
    key = "StoredEnchantments" if stored else "Enchantments"
    body = ",".join(f'{{id:"minecraft:{n}",lvl:{lvl}s}}' for n, lvl in pairs)
    return f"{key}:[{body}]"


def spec(item: str, *parts: str) -> str:
    parts = [p for p in parts if p]
    return f"minecraft:{item}" + (("{" + ",".join(parts) + "}") if parts else "")


def set_ring(server: Server, count: int) -> None:
    cmds = [f"fill {T[0]-2} {T[1]} {T[2]-2} {T[0]+2} {T[1]+1} {T[2]+2} minecraft:air",
            f"setblock {T[0]} {T[1]} {T[2]} minecraft:enchanting_table"]
    for dx, dy, dz in RING[:count]:
        cmds.append(f"setblock {T[0]+dx} {T[1]+dy} {T[2]+dz} minecraft:bookshelf")
    server.batch(cmds)


# ── campaigns ───────────────────────────────────────────────────────────────
TABLE_ITEMS = [
    "book", "wooden_sword", "stone_sword", "iron_sword", "golden_sword", "diamond_sword",
    "netherite_sword", "wooden_pickaxe", "stone_pickaxe", "iron_pickaxe", "golden_pickaxe",
    "diamond_pickaxe", "netherite_pickaxe", "iron_axe", "diamond_axe", "golden_shovel",
    "iron_shovel", "diamond_hoe", "stone_hoe", "leather_helmet", "chainmail_chestplate",
    "iron_leggings", "golden_boots", "diamond_helmet", "diamond_chestplate",
    "diamond_leggings", "diamond_boots", "netherite_chestplate", "netherite_boots",
    "turtle_helmet", "bow", "crossbow", "trident", "fishing_rod", "shears",
    "flint_and_steel", "shield", "elytra", "carrot_on_a_stick", "warped_fungus_on_a_stick",
    "brush", "compass", "stick", "carved_pumpkin", "golden_helmet", "leather_boots",
]


def table_offer(probe: Probe, server: Server, item: str) -> dict:
    server.batch([f"item replace entity {BOT} hotbar.0 with {spec(item)}"])
    probe.settle(0.1)
    probe.swap(0, 0)
    sync(server, probe)
    return {"item": item, "props": [probe.props.get(k, 0) for k in range(10)]}


def campaign_table(server: Server, probe: Probe, rounds: int, per_round: int) -> dict:
    rng = random.Random(20260911)
    offers, enchants = [], []
    for r in range(rounds):
        shelves = r % 16
        set_ring(server, shelves)
        server.batch([f"xp set {BOT} 100 levels", f"clear {BOT}"])
        probe.open_at(T)
        seed = xp_seed(server)
        server.batch([f"item replace entity {BOT} hotbar.1 with minecraft:lapis_lazuli 64"])
        probe.settle(0.1)
        probe.swap(1, 1)
        sync(server, probe)
        chosen = rng.sample(TABLE_ITEMS, per_round - 1) + [rng.choice(TABLE_ITEMS[:41])]
        last = None
        for item in chosen:
            last = table_offer(probe, server, item)
            offers.append({"seed": seed, "shelves": shelves, **last})
        costs = last["props"][:3]
        live = [k for k in range(3) if costs[k] > 0]
        record = {"seed": seed, "shelves": shelves, "item": last["item"], "props": last["props"]}
        if live:
            button = live[r % len(live)]
            levels_before = xp_query(server, "levels")
            probe.send(SB_CLICK_BUTTON, bytes([probe.window, button]))
            sync(server, probe)
            record.update({
                "button": button,
                "result": probe.name_of(probe.slots.get(0)),
                "lapis_left": (probe.slots.get(1) or {"count": 0})["count"],
                "levels_before": levels_before,
                "levels_after": xp_query(server, "levels"),
                "seed_after": xp_seed(server),
            })
        enchants.append(record)
        probe.close()
        print(f"  table round {r+1}/{rounds}: seed {seed} shelves {shelves} "
              f"{last['item']} -> {record.get('result', {}) and record['result'].get('tag')}")
    return {"offers": offers, "enchants": enchants}


def campaign_obstruction(server: Server, probe: Probe) -> dict:
    # A seed at which 0 and 1 bookshelf give different costs for a book.
    set_ring(server, 0)
    server.batch([f"clear {BOT}", f"xp set {BOT} 100 levels"])
    probe.open_at(T)
    server.batch([f"item replace entity {BOT} hotbar.1 with minecraft:lapis_lazuli 64"])
    probe.swap(1, 1)
    probe.settle(0.2)

    def reoffer() -> list[int]:
        probe.swap(0, 0)
        probe.settle(0.15)
        server.batch([f"item replace entity {BOT} hotbar.0 with minecraft:book"])
        probe.swap(0, 0)
        sync(server, probe)
        return [probe.props.get(k, 0) for k in range(3)]

    for _attempt in range(12):
        server.batch([f"fill {T[0]-2} {T[1]} {T[2]-2} {T[0]+2} {T[1]+1} {T[2]+2} minecraft:air",
                      f"setblock {T[0]} {T[1]} {T[2]} minecraft:enchanting_table"])
        zero = reoffer()
        server.batch([f"setblock {T[0]+2} {T[1]} {T[2]} minecraft:bookshelf"])
        one = reoffer()
        if zero != one:
            break
        # Re-roll the seed by enchanting the book.
        probe.send(SB_CLICK_BUTTON, bytes([probe.window, 0]))
        probe.settle(0.4)
        server.batch([f"xp set {BOT} 100 levels"])
    else:
        raise RuntimeError("no seed separates 0 and 1 bookshelf")
    seed = xp_seed(server)
    print(f"  obstruction seed {seed}: 0 shelves {zero}, 1 shelf {one}")

    blockers_same = ["air", "stone", "snow", "grass", "white_carpet", "torch", "glass",
                     "water", "fern", "oak_slab", "cobweb"]
    blockers_upper = ["air", "stone", "glass", "white_carpet"]
    cells = []
    for dx, dy, dz in RING:
        mid = (java_div(dx, 2), dy, java_div(dz, 2))
        tests = [(b, mid) for b in (blockers_same if dy == 0 else blockers_upper)]
        # The midpoint on the *other* layer must not matter.
        other = (mid[0], 1 - dy, mid[2])
        tests.append(("stone", other))
        for blocker, at in tests:
            cmds = [f"fill {T[0]-2} {T[1]} {T[2]-2} {T[0]+2} {T[1]+1} {T[2]+2} minecraft:air",
                    f"setblock {T[0]} {T[1]} {T[2]} minecraft:enchanting_table",
                    f"setblock {T[0]+dx} {T[1]+dy} {T[2]+dz} minecraft:bookshelf"]
            if blocker != "air":
                cmds.append(f"setblock {T[0]+at[0]} {T[1]+at[1]} {T[2]+at[2]} minecraft:{blocker}")
            server.batch(cmds)
            got = reoffer()
            verdict = "counted" if got == one else ("blocked" if got == zero else "ambiguous")
            cells.append({"offset": [dx, dy, dz], "blocker": blocker, "at": list(at),
                          "costs": got, "verdict": verdict})
    probe.close()
    return {"seed": seed, "zero": zero, "one": one, "cells": cells}


ANVIL_PANEL: list[tuple[str, str, str | None, str | None]] = [
    # (name, left, right, rename)
    ("rename_stick", spec("stick"), None, "Bob"),
    ("rename_penalty_3", spec("diamond_sword", "RepairCost:3"), None, "Bob"),
    ("rename_penalty_38", spec("diamond_sword", "RepairCost:38"), None, "Bob"),
    ("rename_penalty_39", spec("diamond_sword", "RepairCost:39"), None, "Bob"),
    ("rename_penalty_40", spec("diamond_sword", "RepairCost:40"), None, "Bob"),
    ("rename_penalty_60", spec("diamond_sword", "RepairCost:60"), None, "Bob"),
    ("rename_to_blank", spec("stick", 'display:{Name:\'{"text":"Old"}\'}'), None, ""),
    ("unit_repair_1", spec("diamond_pickaxe", "Damage:1000"), spec("diamond"), None),
    ("unit_repair_2", spec("diamond_pickaxe", "Damage:1000"), "minecraft:diamond 2", None),
    ("unit_repair_4", spec("diamond_pickaxe", "Damage:1000"), "minecraft:diamond 4", None),
    ("unit_repair_5", spec("diamond_pickaxe", "Damage:1000"), "minecraft:diamond 5", None),
    ("unit_repair_small", spec("diamond_pickaxe", "Damage:100"), "minecraft:diamond 4", None),
    ("unit_repair_rename", spec("iron_pickaxe", "Damage:200"), "minecraft:iron_ingot 3", "Pick"),
    ("unit_repair_penalty", spec("iron_pickaxe", "Damage:200,RepairCost:3"), spec("iron_ingot"), None),
    ("unit_repair_chain", spec("chainmail_chestplate", "Damage:200"), spec("iron_ingot"), None),
    ("unit_repair_elytra", spec("elytra", "Damage:300"), spec("phantom_membrane"), None),
    ("unit_repair_turtle", spec("turtle_helmet", "Damage:200"), spec("scute"), None),
    ("unit_repair_shield", spec("shield", "Damage:200"), spec("oak_planks"), None),
    ("unit_repair_full", spec("diamond_pickaxe"), spec("diamond"), None),
    ("combine_damaged", spec("iron_pickaxe", "Damage:200"), spec("iron_pickaxe", "Damage:150"), None),
    ("combine_damaged_bonus", spec("iron_pickaxe", "Damage:240"), spec("iron_pickaxe", "Damage:240"), None),
    ("combine_full_plain", spec("iron_pickaxe"), spec("iron_pickaxe"), None),
    ("sharp3_sharp3", spec("diamond_sword", ench(("sharpness", 3))),
     spec("diamond_sword", ench(("sharpness", 3))), None),
    ("sharp5_sharp5", spec("diamond_sword", ench(("sharpness", 5))),
     spec("diamond_sword", ench(("sharpness", 5))), None),
    ("sharp1_sharp3", spec("diamond_sword", ench(("sharpness", 1))),
     spec("diamond_sword", ench(("sharpness", 3))), None),
    ("wiki_equal", spec("diamond_sword", ench(("sharpness", 3), ("knockback", 2), ("looting", 3))),
     spec("diamond_sword", ench(("sharpness", 3), ("looting", 3))), None),
    ("wiki_unequal", spec("diamond_sword", ench(("sharpness", 3), ("knockback", 2), ("looting", 1))),
     spec("diamond_sword", ench(("sharpness", 1), ("looting", 3))), None),
    ("wiki_conflict", spec("diamond_sword", ench(("sharpness", 2), ("looting", 2))),
     spec("diamond_sword", ench(("smite", 5), ("looting", 2))), None),
    ("wiki_book", spec("diamond_sword", ench(("looting", 2))),
     spec("enchanted_book", ench(("protection", 3), ("sharpness", 1), ("looting", 2), stored=True)), None),
    ("book_sharp5_sword", spec("diamond_sword"), spec("enchanted_book", ench(("sharpness", 5), stored=True)), None),
    ("book_sharp5_axe", spec("diamond_axe"), spec("enchanted_book", ench(("sharpness", 5), stored=True)), None),
    ("book_sharp5_pick", spec("diamond_pickaxe"), spec("enchanted_book", ench(("sharpness", 5), stored=True)), None),
    ("book_eff5_shears", spec("shears"), spec("enchanted_book", ench(("efficiency", 5), stored=True)), None),
    ("book_thorns_helmet", spec("diamond_helmet"), spec("enchanted_book", ench(("thorns", 3), stored=True)), None),
    ("book_silk_on_fortune", spec("diamond_pickaxe", ench(("fortune", 3))),
     spec("enchanted_book", ench(("silk_touch", 1), stored=True)), None),
    ("book_mending_soulspeed", spec("enchanted_book", ench(("soul_speed", 3), stored=True)),
     spec("enchanted_book", ench(("mending", 1), stored=True)), None),
    ("book_soulspeed_mending", spec("enchanted_book", ench(("mending", 1), stored=True)),
     spec("enchanted_book", ench(("soul_speed", 3), stored=True)), None),
    ("book_book_same", spec("enchanted_book", ench(("sharpness", 4), stored=True)),
     spec("enchanted_book", ench(("sharpness", 4), stored=True)), None),
    ("book_unb3_unb3", spec("diamond_pickaxe", ench(("unbreaking", 3))),
     spec("enchanted_book", ench(("unbreaking", 3), stored=True)), None),
    ("penalty_both_1", spec("diamond_sword", "RepairCost:1"),
     spec("enchanted_book", ench(("sharpness", 1), stored=True), "RepairCost:1"), None),
    ("penalty_7_3", spec("diamond_sword", "RepairCost:7"),
     spec("enchanted_book", ench(("sharpness", 1), stored=True), "RepairCost:3"), None),
    ("repair_and_enchant", spec("diamond_sword", "Damage:500"),
     spec("diamond_sword", ench(("sharpness", 2), ("unbreaking", 1))), None),
    ("repair_enchant_rename", spec("diamond_sword", "Damage:500"),
     spec("diamond_sword", ench(("sharpness", 2), ("unbreaking", 1))), "Blade"),
    ("too_expensive", spec("diamond_sword", "RepairCost:31"),
     spec("enchanted_book", ench(("sharpness", 5), ("looting", 3), stored=True)), None),
    ("trident_conflict", spec("trident", ench(("loyalty", 3))),
     spec("enchanted_book", ench(("riptide", 3), ("channeling", 1), stored=True)), None),
    ("crossbow_conflict", spec("crossbow", ench(("multishot", 1))),
     spec("enchanted_book", ench(("piercing", 4), stored=True)), None),
    ("different_items", spec("diamond_sword"), spec("iron_sword"), None),
    ("curse_book", spec("diamond_chestplate"), spec("enchanted_book", ench(("binding_curse", 1), stored=True)), None),
    ("protection_types", spec("diamond_chestplate", ench(("protection", 4))),
     spec("enchanted_book", ench(("blast_protection", 4), ("unbreaking", 3), stored=True)), None),
    ("overlevel_target", spec("diamond_sword", ench(("sharpness", 7))),
     spec("enchanted_book", ench(("sharpness", 5), stored=True)), None),
    ("loyalty_items", spec("trident", ench(("loyalty", 2))), spec("trident", ench(("loyalty", 2))), None),
    ("named_no_rename", spec("diamond_sword", 'display:{Name:\'{"text":"Keep"}\'}'),
     spec("enchanted_book", ench(("sharpness", 1), stored=True)), None),
    ("binding_shield", spec("shield"), spec("enchanted_book", ench(("binding_curse", 1), stored=True)), None),
    ("binding_pumpkin", spec("carved_pumpkin"), spec("enchanted_book", ench(("binding_curse", 1), stored=True)), None),
    ("vanishing_compass", spec("compass"), spec("enchanted_book", ench(("vanishing_curse", 1), stored=True)), None),
    ("stacked_left", "minecraft:enchanted_book 2", spec("enchanted_book", ench(("sharpness", 1), stored=True)), None),
]

# Two of the same item at a known Damage: the output's Damage is
# `2D - max - floor(max * 12 / 100)`, strictly decreasing in max, so each of
# these recovers the item's maximum damage exactly. The guesses only choose D.
MAX_GUESSES = {
    "leather_helmet": 55, "leather_chestplate": 80, "leather_leggings": 75, "leather_boots": 65,
    "golden_helmet": 77, "golden_chestplate": 112, "golden_leggings": 105, "golden_boots": 91,
    "chainmail_helmet": 165, "chainmail_chestplate": 240, "chainmail_leggings": 225,
    "chainmail_boots": 195, "iron_helmet": 165, "iron_chestplate": 240, "iron_leggings": 225,
    "iron_boots": 195, "diamond_helmet": 363, "diamond_chestplate": 528, "diamond_leggings": 495,
    "diamond_boots": 429, "netherite_helmet": 407, "netherite_chestplate": 592,
    "netherite_leggings": 555, "netherite_boots": 481, "turtle_helmet": 275, "elytra": 432,
    "shield": 336, "bow": 384, "crossbow": 465, "trident": 250, "fishing_rod": 64,
    "flint_and_steel": 64, "carrot_on_a_stick": 25, "warped_fungus_on_a_stick": 100,
    "brush": 64, "shears": 238, "iron_pickaxe": 250,
}
for _item, _guess in MAX_GUESSES.items():
    _d = -(-3 * _guess // 4)
    ANVIL_PANEL.append((f"max_{_item}", spec(_item, f"Damage:{_d}"), spec(_item, f"Damage:{_d}"), None))


def anvil_combo(probe: Probe, server: Server, left: str, right: str | None,
                rename: str | None) -> dict:
    server.batch([f"clear {BOT}", f"item replace entity {BOT} hotbar.0 with {left}"]
                 + ([f"item replace entity {BOT} hotbar.1 with {right}"] if right else []))
    probe.open_at(ANVIL)
    probe.swap(0, 0)
    probe.settle(0.2)
    if right:
        probe.swap(1, 1)
        probe.settle(0.2)
    if rename is not None:
        name = rename.encode()
        probe.send(SB_RENAME_ITEM, varint(len(name)) + name)
    sync(server, probe)
    return {"cost": probe.props.get(0, 0), "output": probe.name_of(probe.slots.get(2))}


def anvil_state(server: Server) -> str:
    for block in ("anvil", "chipped_anvil", "damaged_anvil"):
        lines = server.batch([f"execute if block {ANVIL[0]} {ANVIL[1]} {ANVIL[2]} minecraft:{block}"])
        if any("Test passed" in line for line in lines):
            return block
    return "gone"


def campaign_anvil(server: Server, probe: Probe, renames: int) -> dict:
    server.batch([f"setblock {ANVIL[0]} {ANVIL[1]} {ANVIL[2]} minecraft:anvil"])
    panel = {}
    for name, left, right, rename in ANVIL_PANEL:
        panel[name] = {"left": left, "right": right, "rename": rename,
                       **anvil_combo(probe, server, left, right, rename)}
        probe.close()
        print(f"  anvil {name}: cost {panel[name]['cost']}")

    taken = {}
    for name in ("rename_stick", "unit_repair_2", "wiki_book", "penalty_7_3"):
        _, left, right, rename = next(row for row in ANVIL_PANEL if row[0] == name)
        server.batch([f"setblock {ANVIL[0]} {ANVIL[1]} {ANVIL[2]} minecraft:anvil"])
        server.batch([f"xp set {BOT} 50 levels", f"xp set {BOT} 0 points"])
        shown = anvil_combo(probe, server, left, right, rename)
        probe.click(2, 0, 1)
        sync(server, probe)
        taken[name] = {"cost": shown["cost"], "levels_after": xp_query(server, "levels"),
                       "left_after": probe.name_of(probe.slots.get(0)),
                       "right_after": probe.name_of(probe.slots.get(1))}
        probe.close()

    transitions = {"anvil->chipped_anvil": 0, "chipped_anvil->damaged_anvil": 0,
                   "damaged_anvil->gone": 0}
    uses = {"anvil": 0, "chipped_anvil": 0, "damaged_anvil": 0}
    state = "anvil"
    server.batch([f"setblock {ANVIL[0]} {ANVIL[1]} {ANVIL[2]} minecraft:anvil"])
    for k in range(renames):
        server.batch([f"xp set {BOT} 30 levels"])
        anvil_combo(probe, server, spec("stick"), None, f"n{k}")
        probe.click(2, 0, 1)
        sync(server, probe)
        probe.close()
        uses[state] += 1
        after = anvil_state(server)
        if after != state:
            transitions[f"{state}->{after}"] = transitions.get(f"{state}->{after}", 0) + 1
            if after == "gone":
                server.batch([f"setblock {ANVIL[0]} {ANVIL[1]} {ANVIL[2]} minecraft:anvil"])
                after = "anvil"
            state = after
    print(f"  anvil degradation: uses {uses} transitions {transitions}")
    return {"panel": panel, "taken": taken,
            "degradation": {"uses": uses, "transitions": transitions}}


# Repetitions kept modest: the JVM is shared by every agent on the machine, and
# 25 draws of a uniform on 23..45 already pin both ends of the range.
GRIND_CASES: list[tuple[str, str, str | None, int]] = [
    ("sharp5_sword", spec("diamond_sword", "Damage:100", ench(("sharpness", 5))), None, 25),
    ("unb1_book", spec("enchanted_book", ench(("unbreaking", 1), stored=True)), None, 25),
    ("prot4_unb3_binding", spec("diamond_chestplate", ench(("protection", 4), ("unbreaking", 3),
                                                            ("binding_curse", 1)), "RepairCost:7"), None, 10),
    ("mending_only", spec("iron_pickaxe", ench(("mending", 1))), None, 10),
    ("two_damaged_picks", spec("iron_pickaxe", "Damage:200"), spec("iron_pickaxe", "Damage:150"), 1),
    ("two_picks_bottom_enchanted", spec("iron_pickaxe", "Damage:200"),
     spec("iron_pickaxe", "Damage:150", ench(("efficiency", 2))), 5),
    ("two_books", spec("enchanted_book", ench(("sharpness", 1), stored=True)),
     spec("enchanted_book", ench(("smite", 1), stored=True)), 3),
    ("named_sword", spec("iron_sword", 'display:{Name:\'{"text":"Kept"}\'}', "RepairCost:7",
                         ench(("sharpness", 1))), None, 3),
    ("plain_sword", spec("iron_sword"), None, 1),
    ("vanishing_only", spec("iron_sword", ench(("vanishing_curse", 1))), None, 1),
    ("different_pair", spec("iron_pickaxe"), spec("diamond_pickaxe"), 1),
    ("bottom_only", "", spec("diamond_sword", ench(("sharpness", 5))), 3),
]


def campaign_grindstone(server: Server, probe: Probe) -> dict:
    server.batch([f"setblock {GRINDSTONE[0]} {GRINDSTONE[1]} {GRINDSTONE[2]} "
                  f"minecraft:grindstone[face=floor]"])
    out = {}
    for name, top, bottom, reps in GRIND_CASES:
        samples, shown = [], None
        for _ in range(reps):
            cmds = [f"clear {BOT}", f"xp set {BOT} 0 levels", f"xp set {BOT} 0 points",
                    f"kill @e[type=minecraft:experience_orb]"]
            if top:
                cmds.append(f"item replace entity {BOT} hotbar.0 with {top}")
            if bottom:
                cmds.append(f"item replace entity {BOT} hotbar.1 with {bottom}")
            server.batch(cmds)
            probe.open_at(GRINDSTONE)
            if top:
                probe.swap(0, 0)
            if bottom:
                probe.swap(1, 1)
            sync(server, probe)
            shown = probe.name_of(probe.slots.get(2))
            if shown is None:
                probe.close()
                break
            probe.click(2, 0, 1)
            probe.settle(2.5)
            probe.close()
            samples.append(total_xp(xp_query(server, "levels"), xp_query(server, "points")))
        out[name] = {"top": top, "bottom": bottom, "output": shown, "xp": samples}
        print(f"  grindstone {name}: {shown and shown['id']} xp {sorted(samples)[:5]}...")
    return out


PROTECTION_CASES: list[tuple[str, list[tuple[str, int]], str, float]] = [
    ("none_generic", [], "generic", 10.0),
] + [
    (f"protection{n}_generic", [("protection", n)], "generic", 10.0) for n in (1, 2, 3, 4, 5, 10, 20, 25)
] + [
    (f"fire_protection{n}_in_fire", [("fire_protection", n)], "in_fire", 10.0) for n in (1, 2, 3, 4)
] + [
    (f"blast_protection{n}_explosion", [("blast_protection", n)], "explosion", 10.0) for n in (1, 2, 3, 4)
] + [
    (f"projectile_protection{n}_arrow", [("projectile_protection", n)], "arrow", 10.0) for n in (1, 2, 3, 4)
] + [
    (f"feather_falling{n}_fall", [("feather_falling", n)], "fall", 10.0) for n in (1, 2, 3, 4)
] + [
    ("feather4_generic", [("feather_falling", 4)], "generic", 10.0),
    ("blast4_generic", [("blast_protection", 4)], "generic", 10.0),
    ("fire4_lava", [("fire_protection", 4)], "lava", 10.0),
    ("fire4_on_fire", [("fire_protection", 4)], "on_fire", 10.0),
    ("fire4_hot_floor", [("fire_protection", 4)], "hot_floor", 10.0),
    ("protection4_fall", [("protection", 4)], "fall", 10.0),
    ("protection4_explosion", [("protection", 4)], "explosion", 10.0),
    ("protection4_magic", [("protection", 4)], "magic", 10.0),
    ("protection4_starve", [("protection", 4)], "starve", 10.0),
    ("protection4_out_of_world", [("protection", 4)], "out_of_world", 10.0),
    ("protection4_drown", [("protection", 4)], "drown", 10.0),
    ("protection4_wither", [("protection", 4)], "wither", 10.0),
    ("protection4_sonic_boom", [("protection", 4)], "sonic_boom", 10.0),
    ("protection4_fireball", [("protection", 4)], "fireball", 10.0),
    ("projectile4_fireball", [("projectile_protection", 4)], "fireball", 10.0),
    ("fire4_fireball", [("fire_protection", 4)], "fireball", 10.0),
    ("prot4_blast4_explosion", [("protection", 4), ("blast_protection", 4)], "explosion", 10.0),
    ("prot4_feather4_fall", [("protection", 4), ("feather_falling", 4)], "fall", 10.0),
    ("prot10_feather4_fall", [("protection", 10), ("feather_falling", 4)], "fall", 10.0),
    ("protection4_generic_7", [("protection", 4)], "generic", 7.0),
    ("protection3_generic_3", [("protection", 3)], "generic", 3.0),
]


def campaign_protection(server: Server, probe: Probe) -> dict:
    out = {}
    for name, enchants, kind, amount in PROTECTION_CASES:
        head = spec("player_head", ench(*enchants)) if enchants else "minecraft:air"
        server.batch([f"clear {BOT}", "effect clear " + BOT,
                      f"item replace entity {BOT} armor.head with {head}",
                      f"effect give {BOT} minecraft:instant_health 1 10 true"])
        probe.settle(0.8)
        before = number(data_value(server, BOT, "Health"))
        server.batch([f"damage {BOT} {amount} minecraft:{kind}"])
        probe.settle(0.2)
        after = number(data_value(server, BOT, "Health"))
        out[name] = {"enchantments": enchants, "damage_type": kind, "amount": amount,
                     "health_before": before, "health_after": after}
        print(f"  protection {name}: {before} -> {after}")
        server.batch(["effect clear " + BOT])
        probe.settle(0.6)
    server.batch([f"item replace entity {BOT} armor.head with minecraft:air"])
    return out


def campaign_unbreaking(server: Server, probe: Probe, tills: int, hits: int) -> dict:
    server.batch([f"fill {T[0]-2} {T[1]} {T[2]-2} {T[0]+2} {T[1]+1} {T[2]+2} minecraft:air",
                  "scoreboard objectives add tilled minecraft.used:minecraft.diamond_hoe"])
    tools = {}
    for level in (0, 1, 2, 3):
        enchants = ench(("unbreaking", level)) if level else ""
        server.batch([f"clear {BOT}", f"scoreboard players set {BOT} tilled 0",
                      f"item replace entity {BOT} hotbar.0 with {spec('diamond_hoe', enchants)}"])
        probe.send(SB_SET_HELD_ITEM, struct.pack(">h", 0))
        probe.stand(*STAND)
        probe.settle(0.3)
        for _ in range(tills):
            probe.use_on(TILL)
            probe.settle(0.06)
            server.send(f"setblock {TILL[0]} {TILL[1]} {TILL[2]} minecraft:grass_block")
            probe.settle(0.06)
        sync(server, probe)
        used = None
        for line in server.batch([f"scoreboard players get {BOT} tilled"]):
            m = SCORE.search(line)
            if m:
                used = int(m.group(1))
        damage = number(data_value(server, BOT, "SelectedItem.tag.Damage"))
        tools[str(level)] = {"uses": used, "damage": damage}
        print(f"  unbreaking tool {level}: {damage} damage over {used} uses")

    armour = {}
    for level in (0, 3):
        enchants = ench(("unbreaking", level)) if level else ""
        server.batch([f"clear {BOT}", "effect clear " + BOT,
                      f"item replace entity {BOT} armor.head with {spec('leather_helmet', enchants)}",
                      f"effect give {BOT} minecraft:resistance 100000 4 true"])
        probe.settle(0.7)
        count = hits if level else max(30, hits // 4)
        for _ in range(count):
            server.send(f"damage {BOT} 4 minecraft:generic")
            probe.settle(0.6)
        damage = number(data_value(server, BOT, "Inventory[{Slot:103b}].tag.Damage"))
        armour[str(level)] = {"hits": count, "damage": damage}
        print(f"  unbreaking armour {level}: {damage} damage over {count} hits")
    server.batch(["effect clear " + BOT, f"clear {BOT}"])
    return {"tool": tools, "armour": armour}


def campaign_mending(server: Server, probe: Probe) -> dict:
    out = []
    for value in (1, 2, 3, 5, 10, 60, 200):
        server.batch([f"clear {BOT}", f"xp set {BOT} 0 levels", f"xp set {BOT} 0 points",
                      f"kill @e[type=minecraft:experience_orb]",
                      f"item replace entity {BOT} hotbar.0 with "
                      f"{spec('iron_pickaxe', 'Damage:200', ench(('mending', 1)))}"])
        probe.send(SB_SET_HELD_ITEM, struct.pack(">h", 0))
        probe.stand(*STAND)
        probe.settle(0.3)
        server.batch([f"summon minecraft:experience_orb {STAND[0]} {STAND[1]} {STAND[2]} "
                      f"{{Value:{value}s}}"])
        probe.settle(2.0)
        damage = number(data_value(server, BOT, "SelectedItem.tag.Damage"))
        xp = total_xp(xp_query(server, "levels"), xp_query(server, "points"))
        out.append({"value": value, "damage_before": 200, "damage_after": damage, "xp": xp})
        print(f"  mending orb {value}: damage 200 -> {damage}, xp {xp}")
    return {"orbs": out}


def target_nbt(tag: str) -> str:
    return (f'{{NoAI:1b,Silent:1b,PersistenceRequired:1b,Tags:["{tag}"],'
            f'Attributes:[{{Name:"generic.max_health",Base:1024.0}},'
            f'{{Name:"generic.knockback_resistance",Base:1.0}},'
            f'{{Name:"generic.armor",Base:0.0}}],Health:1024.0f}}')


def campaign_weapons(server: Server, probe: Probe, types: dict[str, int]) -> dict:
    cases = [
        ("plain_cow", spec("diamond_sword"), "cow"),
        ("plain_zombie", spec("diamond_sword"), "zombie"),
        ("smite5_zombie", spec("diamond_sword", ench(("smite", 5))), "zombie"),
        ("smite5_cow", spec("diamond_sword", ench(("smite", 5))), "cow"),
        ("smite3_zombie", spec("diamond_sword", ench(("smite", 3))), "zombie"),
        ("bane5_spider", spec("diamond_sword", ench(("bane_of_arthropods", 5))), "spider"),
        ("bane5_cow", spec("diamond_sword", ench(("bane_of_arthropods", 5))), "cow"),
        ("sharp5_zombie", spec("diamond_sword", ench(("sharpness", 5))), "zombie"),
        ("impaling5_guardian", spec("trident", ench(("impaling", 5))), "guardian"),
        ("impaling5_cow", spec("trident", ench(("impaling", 5))), "cow"),
        ("plain_trident_guardian", spec("trident"), "guardian"),
    ]
    out = {}
    for k, (name, weapon, mob) in enumerate(cases):
        tag = f"t{k}"
        probe.spawned.clear()
        server.batch([f"clear {BOT}", f"kill @e[type=!minecraft:player]",
                      f"item replace entity {BOT} hotbar.0 with {weapon}",
                      f"summon minecraft:{mob} {STAND[0] + 2.0} {STAND[1]} {STAND[2]} {target_nbt(tag)}"])
        probe.send(SB_SET_HELD_ITEM, struct.pack(">h", 0))
        probe.stand(*STAND)
        probe.settle(1.2)
        ids = [e for e in probe.spawned if e[1] == types[f"minecraft:{mob}"]]
        if not ids:
            out[name] = {"error": "no spawn seen"}
            continue
        eid = ids[-1][0]
        before = number(data_value(server, f"@e[tag={tag},limit=1]", "Health"))
        probe.send(SB_INTERACT, varint(eid) + varint(1) + bytes([0]))
        probe.send(SB_SWING_ARM, varint(0))
        sync(server, probe)
        after = number(data_value(server, f"@e[tag={tag},limit=1]", "Health"))
        out[name] = {"weapon": weapon, "mob": mob, "health_before": before, "health_after": after}
        print(f"  weapons {name}: {before} -> {after}")
    server.batch([f"kill @e[type=!minecraft:player]"])
    return out


CAMPAIGNS = ["table", "obstruction", "anvil", "grindstone", "protection", "unbreaking",
             "mending", "weapons"]


def main() -> int:
    wanted = [a for a in sys.argv[1:] if not a.startswith("--")] or CAMPAIGNS
    for name in wanted:
        if name not in CAMPAIGNS:
            print(__doc__)
            return 2
    registries = json.loads(REGISTRIES.read_text())
    items = {v["protocol_id"]: k for k, v in registries["minecraft:item"]["entries"].items()}
    types = {k: v["protocol_id"] for k, v in registries["minecraft:entity_type"]["entries"].items()}

    NORMALIZED.mkdir(parents=True, exist_ok=True)
    result = json.loads(OUT.read_text()) if OUT.is_file() else {}
    shutil.rmtree(RUN, ignore_errors=True)
    server = Server(RUN, PORT)
    try:
        server.batch(["gamerule doMobSpawning false", "gamerule randomTickSpeed 0",
                      "gamerule doDaylightCycle false", "gamerule doImmediateRespawn true",
                      "gamerule naturalRegeneration false", "time set noon",
                      "difficulty normal", "forceload add -16 -16 16 16"])
        probe = None
        for _ in range(10):
            try:
                probe = Probe(PORT, BOT, items)
                break
            except OSError:
                time.sleep(2.0)
        if probe is None:
            raise RuntimeError("could not join")
        probe.settle(2.0)
        server.batch([f"gamemode survival {BOT}", f"tp {BOT} {STAND[0]} {STAND[1]} {STAND[2]}"])
        probe.settle(1.0)
        for name in wanted:
            print(f"\n── {name} ──", flush=True)
            # One campaign failing must not cost the others their turn on the
            # shared JVM: the error is recorded in place of the result.
            try:
                probe.close()
                if name == "table":
                    result[name] = campaign_table(server, probe, rounds=64, per_round=8)
                elif name == "obstruction":
                    result[name] = campaign_obstruction(server, probe)
                elif name == "anvil":
                    result[name] = campaign_anvil(server, probe, renames=150)
                elif name == "grindstone":
                    result[name] = campaign_grindstone(server, probe)
                elif name == "protection":
                    result[name] = campaign_protection(server, probe)
                elif name == "unbreaking":
                    result[name] = campaign_unbreaking(server, probe, tills=300, hits=200)
                elif name == "mending":
                    result[name] = campaign_mending(server, probe)
                elif name == "weapons":
                    result[name] = campaign_weapons(server, probe, types)
            except Exception as error:  # noqa: BLE001
                import traceback
                traceback.print_exc()
                result[name] = {"error": repr(error)}
            OUT.write_text(json.dumps(result, indent=1, sort_keys=True) + "\n")
            print(f"  -> {OUT}", flush=True)
    finally:
        server.stop()
        shutil.rmtree(RUN, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
