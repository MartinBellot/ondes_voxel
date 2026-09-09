#!/usr/bin/env python3
"""De bout en bout : ouvrir un établi, fabriquer une pioche, un four, fondre du fer.

Le décor n'est pas bâti ici : c'est la parcelle « workbenches » du banc de test
(`run/lab`, x 0 z 160), où l'établi et les trois fours sont déjà posés. Le monde
est recopié avant d'être servi, pour que le banc reste ce qu'il était.

Le client utilisé est celui des autres sondes du dépôt : il parle exactement le
protocole 763 et rien d'autre — mêmes paquets, mêmes champs, même ordre — mais
il n'a pas d'écran. Le vrai client graphique ne s'automatise pas ici, et le dire
vaut mieux que de prétendre le contraire ; ce que ce script prouve, c'est que le
serveur répond correctement à la suite exacte de paquets qu'un client 1.20.1
envoie quand un joueur fait ces gestes.

La séquence, dans l'ordre :

  1. rejoindre le banc et ouvrir l'établi de la parcelle ;
  2. remplir la grille — trois planches et deux bâtons — et lire la case 0 ;
  3. prendre la pioche, et vérifier que la grille s'est bien consommée ;
  4. fabriquer un four avec huit pavés, au même établi ;
  5. poser ce four, l'ouvrir, y mettre du minerai de fer et du charbon ;
  6. attendre, et vérifier que le lingot sort, que les deux barres ont bougé, et
     que le shift-clic vide la case de sortie.

Usage : python3 scripts/check_workbench_e2e.py
"""
from __future__ import annotations

import json
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from measure_crafting import Probe  # noqa: E402
from vanilla_miner import block_pos, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = Path(os.environ.get("OV_NORMALIZED", ROOT / "data/vanilla/1.20.1/normalized"))
PRESET = os.environ.get("OV_PRESET", "macos-debug")
BINARY = ROOT / "build" / PRESET / "bin" / "ov_dedicated"
LAB = ROOT / "run" / "lab"
WORLD = ROOT / "run" / "e2e-workbench"
PORT = 25577

# La parcelle « workbenches » du banc : x 0, z 160, sol à -60. Les stations sont
# posées tous les deux blocs à partir de (x+2, z+4), dans l'ordre du catalogue —
# l'établi d'abord, le four ensuite.
FLOOR = -60
TABLE = (2, FLOOR, 164)
LAB_FURNACE = (4, FLOOR, 164)
# Un carré de sol libre de la même parcelle, pour y poser le four fabriqué.
OWN_FURNACE = (2, FLOOR, 169)

CB_CONTAINER_PROPERTY = 0x13


class Client(Probe):
    """La sonde, plus les gestes que ce scénario demande."""

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

    def walk_to(self, x: float, y: float, z: float) -> None:
        self.stand(x, y, z)
        self.settle(0.3)

    def use_on(self, pos, face: int = 1) -> None:
        self.send(0x31, varint(0) + block_pos(*pos) + varint(face)
                  + struct.pack(">fff", 0.5, 1.0, 0.5) + bytes([0]) + varint(0))

    def open_at(self, pos, tries: int = 8) -> int:
        self.window = None
        for _ in range(tries):
            self.walk_to(pos[0] + 0.5, float(pos[1] + 1), pos[2] + 1.5)
            self.use_on(pos)
            deadline = time.monotonic() + 2.0
            while self.window is None and time.monotonic() < deadline:
                self.settle(0.1)
            if self.window is not None:
                return self.window
        fail(f"rien ne s'est ouvert en cliquant {pos}")
        raise SystemExit(1)


def fail(message: str) -> None:
    print(f"\033[0;31m✗\033[0m {message}")
    raise SystemExit(1)


def ok(message: str) -> None:
    print(f"\033[0;32m▸\033[0m {message}")


def main() -> int:
    if not BINARY.is_file():
        fail(f"{BINARY} not built")
    if not (LAB / "level.dat").is_file():
        fail(f"{LAB} absent — lance ./scripts/lab.sh --rebuild d'abord")
    with open(NORMALIZED / "registries.json") as f:
        items = json.load(f)["registries"]["minecraft:item"]["entries"]
    index = {name: i for i, name in enumerate(items)}
    name_of = {i: name for name, i in index.items()}

    # Une copie : le banc doit rester ce qu'il était, et ce scénario y pose des
    # blocs et y allume un four.
    shutil.rmtree(WORLD, ignore_errors=True)
    shutil.copytree(LAB, WORLD)

    server = subprocess.Popen(
        [str(BINARY), f"--port={PORT}", f"--world={WORLD}", "--log-level=warn"],
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    try:
        time.sleep(5.0)
        client = Client(PORT, "E2E0")
        client.settle(3.0)
        ok(f"connecté, position {client.pos}")

        # ── 1. L'établi du banc ─────────────────────────────────────────────
        window = client.open_at(TABLE)
        ok(f"établi ouvert, fenêtre {window}")

        # ── 2. La pioche ────────────────────────────────────────────────────
        grid = [index["minecraft:oak_planks"]] * 3 \
            + [None, index["minecraft:stick"], None] \
            + [None, index["minecraft:stick"], None]
        for slot in range(9):
            client.creative_set(36 + slot, grid[slot], 1)
        client.settle(0.4)
        for slot in range(9):
            client.click(1 + slot, slot, 2)
        client.settle(0.6)

        result = client.slots.get(0)
        if not result or name_of.get(result[0]) != "minecraft:wooden_pickaxe":
            fail(f"la case de sortie montre {result}, pas une pioche en bois")
        ok("la grille annonce une pioche en bois")

        # ── 3. La prendre ───────────────────────────────────────────────────
        # Shift-clic : la pioche part dans l'inventaire et le curseur reste
        # vide. Jeter un curseur n'est pas le mode 4 mais le mode 0 sur -999,
        # et l'erreur ne dit rien — elle laisse l'objet en main et le jeu
        # refuse alors toutes les fabrications suivantes.
        client.click(0, 0, 1)
        client.settle(0.6)
        left = [client.slots.get(1 + i) for i in range(9)]
        if any(left):
            fail(f"la grille n'a pas été consommée : {left}")
        if client.slots.get(0):
            fail("la case de sortie montre encore quelque chose")
        ok("la pioche est prise et la grille est vide")

        # ── 4. Le four ──────────────────────────────────────────────────────
        for slot in range(9):
            client.creative_set(36 + slot,
                                index["minecraft:cobblestone"] if slot != 4 else None, 1)
        client.settle(0.4)
        for slot in range(9):
            client.click(1 + slot, slot, 2)
        client.settle(0.6)
        result = client.slots.get(0)
        if not result or name_of.get(result[0]) != "minecraft:furnace":
            fail(f"huit pavés donnent {result}, pas un four")
        ok("huit pavés donnent un four")
        client.click(0, 0, 1)
        client.settle(0.5)
        client.send(0x0C, bytes([client.window]))         # Close Container
        client.settle(0.5)

        # ── 5. Le poser et le charger ───────────────────────────────────────
        client.creative_set(36, index["minecraft:furnace"], 1)
        client.settle(0.3)
        client.send(0x28, struct.pack(">h", 0))           # Set Held Item, case 0
        client.walk_to(OWN_FURNACE[0] + 0.5, float(OWN_FURNACE[1]), OWN_FURNACE[2] + 1.5)
        client.use_on((OWN_FURNACE[0], OWN_FURNACE[1] - 1, OWN_FURNACE[2]))
        client.settle(0.8)

        client.properties.clear()
        window = client.open_at(OWN_FURNACE)
        ok(f"le four fabriqué s'ouvre, fenêtre {window}")

        client.creative_set(36, index["minecraft:iron_ore"], 8)
        client.creative_set(37, index["minecraft:coal"], 4)
        client.settle(0.5)
        client.click(0, 0, 2)   # case d'entrée    <- barre d'action 0
        client.settle(0.4)
        client.click(1, 1, 2)   # case combustible <- barre d'action 1
        client.settle(0.6)

        if not client.slots.get(0) or not client.slots.get(1):
            fail(f"le four n'a pas reçu ses objets : "
                 f"{client.slots.get(0)}, {client.slots.get(1)}")
        ok("minerai et charbon en place")

        # ── 6. La cuisson ───────────────────────────────────────────────────
        # 200 ticks, soit dix secondes. On regarde les deux barres bouger en
        # chemin : sans elles l'écran resterait figé même si le serveur cuisait.
        seen_burning = False
        seen_progress = False
        deadline = time.monotonic() + 25.0
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
            fail(f"aucun lingot après vingt-cinq secondes ; "
                 f"propriétés {client.properties}, sortie {client.slots.get(2)}")

        if not seen_burning:
            fail("la barre de flamme n'a jamais bougé")
        if not seen_progress:
            fail("la flèche de progression n'a jamais bougé")
        ok(f"lingot de fer produit : {client.slots.get(2)}")
        ok(f"barres vues : combustible {client.properties.get(0)} sur "
           f"{client.properties.get(1)}, cuisson {client.properties.get(2)} sur "
           f"{client.properties.get(3)}")

        # Le récupérer, et vérifier que la case se vide.
        client.click(2, 0, 1)
        client.settle(0.6)
        if client.slots.get(2):
            fail(f"la sortie n'a pas été vidée par le shift-clic : {client.slots.get(2)}")
        ok("le lingot est passé dans l'inventaire")

        print("\n\033[0;32mtout est passé\033[0m")
    finally:
        server.terminate()
        try:
            server.wait(timeout=30)
        except subprocess.TimeoutExpired:
            server.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
