#!/usr/bin/env python3
"""Interroge un vrai serveur 1.20.1 sur ce que chaque grille de fabrication rend.

Le fichier de recette dit ce qu'une recette *devrait* faire. Il ne dit pas ce
que le jeu fait quand on remplit une grille : à quelle position un motif 2×2
s'apparie dans une grille 3×3, si le miroir horizontal compte, ce qu'une recette
informe accepte, ni ce qui reste dans les cases après la fabrication. Ce script
demande tout cela au jeu.

La sonde
--------
Un joueur créatif ouvre un établi. Deux paquets suffisent à remplir une grille :

  * `Set Creative Slot` écrit dans le menu d'inventaire du joueur — le serveur
    l'applique quel que soit l'écran ouvert, et les cases 36 à 44 sont la barre
    d'action, partagée avec la fenêtre de l'établi ;
  * `Click Container` en mode 2 échange une case de la fenêtre avec une case de
    la barre d'action. Une case de grille remplie par paquet, pas deux.

Le serveur répond avec `Set Container Slot` sur la case 0 : c'est son verdict.
Un clic ordinaire sur cette case 0 fabrique, et relire la grille donne les
**restes** — le seau vide qui revient, la bouteille. Ceux-là non plus ne sont
nulle part dans les données : `craftingRemainingItem` est du code Java.

Trois familles de grilles sont posées :

  * la grille exacte de chaque recette façonnée, dans **toutes** ses positions
    et son miroir horizontal ;
  * les ingrédients de chaque recette informe, dans un ordre mélangé de façon
    déterministe ;
  * des grilles qui ne doivent rien donner, pour que « rien » soit vérifié
    aussi — une implémentation trop généreuse ne se voit que là.

Usage : python3 scripts/measure_crafting.py [sortie.json] [--limit N]
"""
from __future__ import annotations

import json
import os
import random
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from measure_entities import Server  # noqa: E402
from vanilla_miner import Miner, read_varint, varint, block_pos  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = Path(os.environ.get("OV_NORMALIZED", ROOT / "data/vanilla/1.20.1/normalized"))
GENERATED = NORMALIZED.parent / "generated" / "data" / "minecraft" / "recipes"
RUN = ROOT / "run" / "measure-crafting"
PORT = 25599

TABLE = (4, -60, 0)
STAND = (4.5, -59.0, 2.5)

# Paquets, protocole 763.
CB_OPEN_SCREEN = 0x30
CB_CONTAINER_CONTENT = 0x12
CB_CONTAINER_SLOT = 0x14
SB_CLICK_CONTAINER = 0x0B
SB_CLOSE_CONTAINER = 0x0C
SB_SET_CREATIVE_SLOT = 0x2B
SB_USE_ITEM_ON = 0x31

# Numérotation des cases dans la fenêtre d'un établi : 0 le résultat, 1 à 9 la
# grille en lignes, 10 à 36 l'inventaire, 37 à 45 la barre d'action.
GRID_FIRST = 1
HOTBAR_FIRST = 37


def write_slot(item_id: int | None, count: int = 1) -> bytes:
    if item_id is None:
        return bytes([0])
    return bytes([1]) + varint(item_id) + bytes([count & 0xFF]) + bytes([0])


_PAYLOAD_SIZE = {1: 1, 2: 2, 3: 4, 4: 8, 5: 4, 6: 8}


def skip_nbt(buf: bytes, i: int, tag: int | None = None, named: bool = True) -> int:
    """Saute un tag NBT sans le lire.

    Il faut bien le sauter en entier : un objet enchanté ou nommé porte un
    compound, et le paquet continue derrière. S'arrêter au premier octet
    décalerait toutes les cases suivantes — et une grille décalée ressemble à
    une grille valide.
    """
    if tag is None:
        tag = buf[i]
        i += 1
        if tag == 0:
            return i
    if named:
        length = struct.unpack_from(">H", buf, i)[0]
        i += 2 + length
    if tag in _PAYLOAD_SIZE:
        return i + _PAYLOAD_SIZE[tag]
    if tag == 7:            # byte array
        n = struct.unpack_from(">i", buf, i)[0]
        return i + 4 + n
    if tag == 8:            # string
        n = struct.unpack_from(">H", buf, i)[0]
        return i + 2 + n
    if tag == 9:            # list
        inner = buf[i]
        n = struct.unpack_from(">i", buf, i + 1)[0]
        i += 5
        for _ in range(n):
            i = skip_nbt(buf, i, tag=inner, named=False)
        return i
    if tag == 10:           # compound
        while buf[i] != 0:
            i = skip_nbt(buf, i)
        return i + 1
    if tag in (11, 12):     # int array, long array
        n = struct.unpack_from(">i", buf, i)[0]
        return i + 4 + n * (4 if tag == 11 else 8)
    raise RuntimeError(f"unknown NBT tag {tag}")


def read_slot(buf: bytes, i: int) -> tuple[tuple[int, int] | None, int]:
    present = buf[i]
    i += 1
    if not present:
        return None, i
    item, i = read_varint(buf, i)
    count = buf[i]
    i += 1
    i = skip_nbt(buf, i)
    return (item, count), i


class Probe(Miner):
    """Le client sonde : il ouvre l'établi et remplit des grilles."""

    def __init__(self, port: int, name: str) -> None:
        super().__init__(port, name)
        self.window = None
        self.state_id = 0
        self.slots: dict[int, tuple[int, int] | None] = {}

    def handle(self, pid: int, payload: bytes) -> None:
        if pid == CB_OPEN_SCREEN:
            window, i = read_varint(payload, 0)
            self.window = window
            self.slots = {}
        elif pid == CB_CONTAINER_CONTENT and self.window is not None:
            if payload[0] != self.window:
                return
            state, i = read_varint(payload, 1)
            count, i = read_varint(payload, i)
            self.state_id = state
            for index in range(count):
                stack, i = read_slot(payload, i)
                self.slots[index] = stack
        elif pid == CB_CONTAINER_SLOT:
            window = struct.unpack_from(">b", payload, 0)[0]
            state, i = read_varint(payload, 1)
            slot = struct.unpack_from(">h", payload, i)[0]
            i += 2
            stack, i = read_slot(payload, i)
            if window == self.window:
                self.state_id = state
                self.slots[slot] = stack
            elif window == 0:
                # La fenêtre 0 est l'inventaire du joueur : le serveur y répond
                # aux Set Creative Slot. On l'ignore, mais il faut la décoder
                # pour ne pas la confondre avec la nôtre.
                pass

    def settle(self, seconds: float = 0.25) -> None:
        """Laisse arriver ce que le serveur a à dire, en tenant le keep-alive."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.pump(until=lambda pid, p: self.handle(pid, p), timeout=0.05)

    def creative_set(self, slot: int, item_id: int | None, count: int = 1) -> None:
        self.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", slot) + write_slot(item_id, count))

    def click(self, slot: int, button: int, mode: int) -> None:
        payload = (bytes([self.window]) + varint(self.state_id) + struct.pack(">h", slot)
                   + struct.pack(">b", button) + varint(mode) + varint(0) + write_slot(None))
        self.send(SB_CLICK_CONTAINER, payload)

    def open_table(self) -> None:
        self.stand(*STAND)
        # Face du dessus, coordonnées au centre du bloc : un clic hors du bloc
        # ne l'ouvre pas et ne dit rien.
        payload = (varint(0) + block_pos(*TABLE) + varint(1)
                   + struct.pack(">fff", 0.5, 1.0, 0.5) + bytes([0]) + varint(0))
        self.send(SB_USE_ITEM_ON, payload)
        deadline = time.monotonic() + 5.0
        while self.window is None and time.monotonic() < deadline:
            self.settle(0.1)
        if self.window is None:
            raise RuntimeError("l'établi ne s'est pas ouvert")


def grid_now(probe: Probe) -> list[int | None]:
    return [None if probe.slots.get(GRID_FIRST + i) is None
            else probe.slots[GRID_FIRST + i][0] for i in range(9)]


def wait_for(probe: Probe, wanted: list[int | None], timeout: float = 6.0) -> bool:
    """Attend que la grille du serveur soit bien celle qu'on a demandée.

    Une temporisation fixe ne suffit pas : le serveur vanilla annonce
    lui-même « Can't keep up! Running 5015ms behind » pendant une campagne de
    trois mille grilles, et une réponse en retard se lit alors comme « cette
    recette ne donne rien ». Trente et une recettes parfaitement valides ont
    été perdues ainsi à la première passe. On attend donc l'état, pas le temps.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if grid_now(probe) == wanted:
            # La case 0 est calculée après la grille : un tour de plus pour la
            # laisser arriver, sinon on lit le résultat de la grille d'avant.
            probe.settle(0.12)
            return True
        probe.settle(0.05)
    return False


def fill(probe: Probe, grid: list[int | None]) -> tuple[int, int] | None:
    """Pose neuf objets dans la grille et rend ce que le serveur met en case 0."""
    for index in range(9):
        probe.creative_set(36 + index, grid[index], 1)
    probe.settle(0.08)
    for index in range(9):
        probe.click(GRID_FIRST + index, index, 2)
    if not wait_for(probe, grid):
        raise TimeoutError(f"la grille n'a jamais pris la forme demandée : "
                           f"voulu {grid}, vu {grid_now(probe)}")
    return probe.slots.get(0)


def empty(probe: Probe) -> None:
    """Vide la grille et la barre d'action, sans rien laisser derrière."""
    for index in range(9):
        probe.click(GRID_FIRST + index, index, 2)
    if not wait_for(probe, [None] * 9):
        # Une grille qu'on n'arrive pas à vider empoisonnerait toutes les
        # mesures suivantes, qui liraient un mélange de deux recettes.
        raise TimeoutError(f"la grille ne s'est pas vidée : {grid_now(probe)}")
    for index in range(9):
        probe.creative_set(36 + index, None)
    probe.settle(0.08)


def positions(width: int, height: int) -> list[tuple[int, int]]:
    return [(dx, dy) for dy in range(3 - height + 1) for dx in range(3 - width + 1)]


def lay_out(cells: list[int | None], width: int, height: int, dx: int, dy: int,
            mirrored: bool) -> list[int | None]:
    grid: list[int | None] = [None] * 9
    for y in range(height):
        for x in range(width):
            source = cells[y * width + (width - 1 - x if mirrored else x)]
            grid[(y + dy) * 3 + (x + dx)] = source
    return grid


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    limit = None
    for a in sys.argv[1:]:
        if a.startswith("--limit="):
            limit = int(a.split("=", 1)[1])
    out_path = Path(args[0]) if args else NORMALIZED / "crafting.json"

    with open(NORMALIZED / "registries.json") as f:
        registries = json.load(f)["registries"]
    items: list[str] = registries["minecraft:item"]["entries"]
    item_index = {name: i for i, name in enumerate(items)}
    with open(NORMALIZED / "tags.json") as f:
        item_tags = json.load(f)["tags"]["minecraft:item"]

    def one_of(ing) -> str:
        """Un représentant d'un ingrédient : le premier, pour rester reproductible."""
        if isinstance(ing, list):
            return one_of(ing[0])
        if "item" in ing:
            return ing["item"]
        tag = ing["tag"]
        if not tag.startswith("minecraft:"):
            tag = "minecraft:" + tag
        return item_tags[tag][0]

    shaped, shapeless = [], []
    for path in sorted(GENERATED.glob("*.json")):
        with open(path) as f:
            doc = json.load(f)
        recipe_id = "minecraft:" + path.stem
        if doc["type"] == "minecraft:crafting_shaped":
            pattern, key = doc["pattern"], doc["key"]
            height = len(pattern)
            width = max(len(row) for row in pattern)
            cells: list[str | None] = []
            for row in pattern:
                for symbol in row.ljust(width):
                    cells.append(None if symbol == " " else one_of(key[symbol]))
            shaped.append((recipe_id, width, height, cells))
        elif doc["type"] == "minecraft:crafting_shapeless":
            shapeless.append((recipe_id, [one_of(i) for i in doc["ingredients"]]))

    if limit:
        shaped = shaped[:limit]
        shapeless = shapeless[:limit]
    print(f"{len(shaped)} façonnées, {len(shapeless)} informes")

    rng = random.Random(1234567890)
    server = Server(RUN, port=PORT)
    grids = []
    remainder: dict[str, str] = {}
    try:
        server.batch([
            "gamerule doMobSpawning false", "gamerule randomTickSpeed 0",
            "gamerule doDaylightCycle false", "gamerule doWeatherCycle false",
            "gamerule sendCommandFeedback true", "difficulty peaceful", "time set noon",
            "forceload add -32 -32 32 32",
        ])
        time.sleep(3.0)
        server.batch([f"setblock {TABLE[0]} {TABLE[1]} {TABLE[2]} minecraft:crafting_table",
                      f"setblock {TABLE[0]} {TABLE[1] - 1} {TABLE[2]} minecraft:stone",
                      f"setblock {TABLE[0]} {TABLE[1]} {TABLE[2] + 2} minecraft:stone"])

        probe = Probe(PORT, "Craft0")
        probe.settle(2.0)
        server.batch(["gamemode creative Craft0", "gamerule doImmediateRespawn true",
                      f"tp Craft0 {STAND[0]} {STAND[1]} {STAND[2]}"])
        probe.settle(1.5)
        probe.open_table()
        print(f"établi ouvert, fenêtre {probe.window}")

        def ask(label: str, grid: list[str | None], expect_kind: str) -> None:
            ids = [item_index[n] if n else None for n in grid]
            got = fill(probe, ids)
            grids.append({
                "label": label,
                "kind": expect_kind,
                "grid": grid,
                "result": None if got is None else
                          {"item": items[got[0]], "count": got[1]},
            })
            empty(probe)

        for recipe_id, width, height, cells in shaped:
            for dx, dy in positions(width, height):
                for mirrored in (False, True):
                    if mirrored and width == 1:
                        continue
                    ask(f"{recipe_id}@{dx},{dy}{'m' if mirrored else ''}",
                        lay_out(cells, width, height, dx, dy, mirrored), "shaped")

        for recipe_id, ingredients in shapeless:
            order = list(ingredients)
            rng.shuffle(order)
            grid: list[str | None] = [None] * 9
            spots = rng.sample(range(9), len(order))
            for spot, name in zip(sorted(spots), order):
                grid[spot] = name
            ask(f"{recipe_id}#shuffled", grid, "shapeless")

        # Des grilles qui ne doivent rien donner. Sans elles, un appariement
        # trop généreux passe inaperçu : tout ce qui doit marcher marche.
        pool = [n for n in ("minecraft:stick", "minecraft:oak_planks", "minecraft:cobblestone",
                            "minecraft:iron_ingot", "minecraft:diamond", "minecraft:string",
                            "minecraft:coal", "minecraft:redstone") if n in item_index]
        for trial in range(120):
            grid = [rng.choice(pool) if rng.random() < 0.5 else None for _ in range(9)]
            ask(f"random#{trial}", grid, "random")

        # ── Les restes ──────────────────────────────────────────────────────
        #
        # Fabriquer pour de vrai et relire la grille : ce qui subsiste est le
        # `craftingRemainingItem` de l'ingrédient, et il n'est nulle part dans
        # les données.
        print("restes de fabrication")
        for recipe_id, width, height, cells in shaped + [
                (rid, len(ings), 1, ings) for rid, ings in shapeless]:
            if not any(cells):
                continue
            ids = [item_index[n] if n else None for n in
                   lay_out(cells, width, height, 0, 0, False)] if width <= 3 and height <= 3 \
                else None
            if ids is None:
                continue
            got = fill(probe, ids)
            if got is None:
                empty(probe)
                continue
            before = [probe.slots.get(GRID_FIRST + i) for i in range(9)]
            probe.click(0, 0, 0)          # ramasse le résultat : la grille se consomme
            probe.settle(0.2)
            after = [probe.slots.get(GRID_FIRST + i) for i in range(9)]
            for index, (was, now) in enumerate(zip(before, after)):
                if was is None or now is None:
                    continue
                if now[0] != was[0]:
                    remainder[items[was[0]]] = items[now[0]]
            probe.click(-999, 0, 4)       # jette ce que le curseur tient
            probe.settle(0.1)
            empty(probe)

        matched = sum(1 for g in grids if g["result"])
        print(f"{len(grids)} grilles posées, {matched} avec un résultat, "
              f"{len(grids) - matched} sans")
        print(f"{len(remainder)} objets à reste : {remainder}")

        doc = {
            "$comment": "Ce qu'un vrai serveur 1.20.1 met dans la case résultat pour "
                        "chaque grille posée par une sonde créative, et ce qui reste "
                        "dans les cases après fabrication. Oracle d'appariement, pas "
                        "une reformulation des fichiers de recette.",
            "version": "1.20.1",
            "grids": len(grids),
            "with_result": matched,
            "crafting_remainder": dict(sorted(remainder.items())),
            "cases": grids,
        }
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps(doc, indent=1, ensure_ascii=False) + "\n",
                            encoding="utf-8")
        print(f"écrit {out_path}")
    finally:
        server.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
