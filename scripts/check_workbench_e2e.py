#!/usr/bin/env python3
"""De bout en bout : ouvrir un établi, fabriquer une pioche, un four, fondre du fer.

Le client utilisé est celui des autres sondes du dépôt : un client qui parle
exactement le protocole 763 et rien d'autre — mêmes paquets, mêmes champs,
même ordre — mais qui n'a pas d'écran. Le vrai client graphique ne s'automatise
pas ici, et le dire vaut mieux que de prétendre le contraire ; ce que ce script
prouve, c'est que le serveur répond correctement à la suite exacte de paquets
qu'un client 1.20.1 envoie quand un joueur fait ces gestes.

La séquence, dans l'ordre :

  1. poser un établi et l'ouvrir (Use Item On) ;
  2. remplir la grille — trois planches et deux bâtons — et lire la case 0 ;
  3. prendre la pioche, vérifier que la grille s'est consommée ;
  4. fabriquer un four avec huit pavés, à l'établi ;
  5. poser le four, l'ouvrir, y mettre du minerai de fer et du charbon ;
  6. attendre, et vérifier que le lingot sort et que la barre a bougé.

Usage : python3 scripts/check_workbench_e2e.py [--keep]
"""
from __future__ import annotations

import json
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from measure_crafting import Probe, read_slot  # noqa: E402
from vanilla_miner import block_pos, read_varint, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = Path(os.environ.get("OV_NORMALIZED", ROOT / "data/vanilla/1.20.1/normalized"))
BINARY = ROOT / "build" / "macos-debug" / "apps" / "ov_dedicated" / "ov_dedicated"
PORT = 25577

TABLE = (2, 5, 2)
FURNACE = (4, 5, 2)
STAND = (2.5, 5.0, 4.5)

CB_CONTAINER_PROPERTY = 0x13


class Client(Probe):
    """La sonde, plus les deux gestes que ce scénario demande."""

    def __init__(self, port: int, name: str) -> None:
        self.properties: dict[int, int] = {}
        super().__init__(port, name)

    def handle(self, pid: int, payload: bytes) -> None:
        if pid == CB_CONTAINER_PROPERTY and len(payload) == 5:
            window, prop, value = struct.unpack(">bhh", payload)
            if self.window is None or window == self.window:
                self.properties[prop] = value
            return
        super().handle(pid, payload)

    def use_on(self, pos, face: int = 1) -> None:
        self.send(0x31, varint(0) + block_pos(*pos) + varint(face)
                  + struct.pack(">fff", 0.5, 1.0, 0.5) + bytes([0]) + varint(0))


def fail(message: str) -> None:
    print(f"\033[0;31m✗\033[0m {message}")
    raise SystemExit(1)


def ok(message: str) -> None:
    print(f"\033[0;32m▸\033[0m {message}")


def main() -> int:
    if not BINARY.is_file():
        fail(f"{BINARY} not built")
    with open(NORMALIZED / "registries.json") as f:
        items = json.load(f)["registries"]["minecraft:item"]["entries"]
    index = {name: i for i, name in enumerate(items)}
    name_of = {i: name for name, i in index.items()}

    run_dir = ROOT / "run" / "e2e-workbench"
    run_dir.mkdir(parents=True, exist_ok=True)
    server = subprocess.Popen([str(BINARY), f"--port={PORT}", "--log-level=warn"],
                              cwd=run_dir, stdout=subprocess.DEVNULL,
                              stderr=subprocess.STDOUT)
    try:
        time.sleep(4.0)
        client = Client(PORT, "E2E0")
        client.settle(2.0)
        client.stand(*STAND)
        client.settle(0.5)

        # ── 1. L'établi ─────────────────────────────────────────────────────
        # Le joueur pose l'établi comme le ferait n'importe qui : l'objet en
        # main, un clic sur le sol.
        client.creative_set(36, index["minecraft:crafting_table"], 1)
        client.settle(0.3)
        client.send(0x28, struct.pack(">h", 0))          # Set Held Item, case 0
        client.use_on((TABLE[0], TABLE[1] - 1, TABLE[2]))
        client.settle(0.6)

        client.window = None
        client.use_on(TABLE)
        deadline = time.monotonic() + 5.0
        while client.window is None and time.monotonic() < deadline:
            client.settle(0.1)
        if client.window is None:
            fail("l'établi ne s'est pas ouvert")
        ok(f"établi ouvert, fenêtre {client.window}")

        # ── 2. La pioche ────────────────────────────────────────────────────
        grid = [index["minecraft:oak_planks"]] * 3 + [None, index["minecraft:stick"], None] \
            + [None, index["minecraft:stick"], None]
        for slot in range(9):
            client.creative_set(36 + slot, grid[slot], 1)
        client.settle(0.3)
        for slot in range(9):
            client.click(1 + slot, slot, 2)
        client.settle(0.5)

        result = client.slots.get(0)
        if not result or name_of.get(result[0]) != "minecraft:wooden_pickaxe":
            fail(f"la case de sortie montre {result}, pas une pioche en bois")
        ok("la grille annonce une pioche en bois")

        # ── 3. La prendre ───────────────────────────────────────────────────
        client.click(0, 0, 0)
        client.settle(0.5)
        left = [client.slots.get(1 + i) for i in range(9)]
        if any(left):
            fail(f"la grille n'a pas été consommée : {left}")
        if client.slots.get(0):
            fail("la case de sortie montre encore quelque chose")
        ok("la pioche est prise et la grille est vide")
        client.click(-999, 0, 4)  # jeter ce que le curseur tient
        client.settle(0.3)

        # ── 4. Le four ──────────────────────────────────────────────────────
        for slot in range(9):
            client.creative_set(36 + slot, index["minecraft:cobblestone"] if slot != 4 else None, 1)
        client.settle(0.3)
        for slot in range(9):
            client.click(1 + slot, slot, 2)
        client.settle(0.5)
        result = client.slots.get(0)
        if not result or name_of.get(result[0]) != "minecraft:furnace":
            fail(f"huit pavés donnent {result}, pas un four")
        ok("huit pavés donnent un four")
        client.click(0, 0, 0)
        client.settle(0.4)
        client.send(0x0C, bytes([client.window]))         # Close Container
        client.settle(0.4)

        # ── 5. Le poser et le charger ───────────────────────────────────────
        client.creative_set(36, index["minecraft:furnace"], 1)
        client.settle(0.3)
        client.use_on((FURNACE[0], FURNACE[1] - 1, FURNACE[2]))
        client.settle(0.6)

        client.window = None
        client.properties.clear()
        client.use_on(FURNACE)
        deadline = time.monotonic() + 5.0
        while client.window is None and time.monotonic() < deadline:
            client.settle(0.1)
        if client.window is None:
            fail("le four ne s'est pas ouvert")
        ok(f"four ouvert, fenêtre {client.window}")

        client.creative_set(36, index["minecraft:iron_ore"], 8)
        client.creative_set(37, index["minecraft:coal"], 4)
        client.settle(0.4)
        client.click(0, 0, 2)   # case d'entrée   <- barre d'action 0
        client.settle(0.3)
        client.click(1, 1, 2)   # case combustible <- barre d'action 1
        client.settle(0.5)

        if not client.slots.get(0) or not client.slots.get(1):
            fail(f"le four n'a pas reçu ses objets : {client.slots.get(0)}, {client.slots.get(1)}")
        ok("minerai et charbon en place")

        # ── 6. La cuisson ───────────────────────────────────────────────────
        # 200 ticks, soit dix secondes. On regarde la barre bouger en chemin :
        # sans elle, l'écran resterait figé même si le serveur cuisait.
        seen_burning = False
        seen_progress = False
        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline:
            client.settle(0.2)
            if client.properties.get(0, 0) > 0:
                seen_burning = True
            if client.properties.get(2, 0) > 0:
                seen_progress = True
            output = client.slots.get(2)
            if output and name_of.get(output[0]) == "minecraft:iron_ingot":
                break
        else:
            fail(f"aucun lingot après vingt secondes ; propriétés {client.properties}, "
                 f"sortie {client.slots.get(2)}")

        if not seen_burning:
            fail("la barre de flamme n'a jamais bougé")
        if not seen_progress:
            fail("la flèche de progression n'a jamais bougé")
        ok(f"lingot de fer produit : {client.slots.get(2)}")
        ok(f"propriétés vues : brûlage {client.properties.get(0)}, "
           f"durée {client.properties.get(1)}, cuisson {client.properties.get(2)}, "
           f"total {client.properties.get(3)}")

        # Le récupérer, et vérifier que la case se vide.
        client.click(2, 0, 1)
        client.settle(0.5)
        if client.slots.get(2):
            fail("la sortie n'a pas été vidée par le shift-clic")
        ok("le lingot est passé dans l'inventaire")

        print("\n\033[0;32mtout est passé\033[0m")
    finally:
        server.terminate()
        try:
            server.wait(timeout=20)
        except subprocess.TimeoutExpired:
            server.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
