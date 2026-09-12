#!/usr/bin/env python3
"""La fenêtre 0 du joueur — la grille 2×2 et ses clics — contre le vrai serveur 1.20.1.

La fenêtre 0 est celle que le client ouvre seul (touche E) : aucun `Open
Screen`, 46 cases, 0 le résultat, 1 à 4 la grille, 5 à 8 l'armure, 9 à 35 le
sac, 36 à 44 la barre, 45 la main secondaire. Une sonde en survie y clique
sans rien ouvrir et relit, pour chaque cas :

  * les cases 0 à 4 et le curseur, tels que le serveur les renvoie
    (`Set Container Content` / `Set Container Slot`, fenêtre 0 et −1) ;
  * l'inventaire tel que `data get entity … Inventory` l'imprime ;
  * les objets au sol.

Chaque cas part d'un `clear` : les ingrédients sont posés dans la barre par
`item replace`, puis échangés dans la grille par un clic en mode 2.

Sortie : `.scratch/window0.json`. Usage : python3 scripts/measure_window0.py
"""
from __future__ import annotations

import json
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from measure_crafting import (  # noqa: E402
    CB_CONTAINER_CONTENT, CB_CONTAINER_SLOT, SB_CLOSE_CONTAINER, Probe, Server as CraftServer,
    read_slot)
from vanilla_miner import read_varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / ".scratch" / "measure-window0"
OUT = ROOT / ".scratch" / "window0.json"
PORT = 25617
NAME = "Grid0"
STAND = (4.5, -60.0, 4.5)


class WindowProbe(Probe):
    """La sonde, qui suit la fenêtre 0 et le curseur."""

    def __init__(self, port: int, name: str) -> None:
        super().__init__(port, name)
        self.window = 0
        self.carried: tuple[int, int] | None = None

    def handle(self, pid: int, payload: bytes) -> None:
        if pid == CB_CONTAINER_CONTENT:
            if payload[0] != 0:
                return
            state, i = read_varint(payload, 1)
            count, i = read_varint(payload, i)
            self.state_id = state
            for index in range(count):
                stack, i = read_slot(payload, i)
                self.slots[index] = stack
            self.carried, i = read_slot(payload, i)
        elif pid == CB_CONTAINER_SLOT:
            window = struct.unpack_from(">b", payload, 0)[0]
            state, i = read_varint(payload, 1)
            slot = struct.unpack_from(">h", payload, i)[0]
            i += 2
            stack, i = read_slot(payload, i)
            if window == -1 and slot == -1:
                self.carried = stack
            elif window == 0:
                self.state_id = state
                self.slots[slot] = stack


def snapshot(server: CraftServer, probe: WindowProbe) -> dict:
    probe.settle(0.6)
    inventory = [l[l.find("data:"):] for l in server.batch([f"data get entity {NAME} Inventory"])
                 if "entity data" in l]
    ground = [l[l.find("data:"):] for l in server.batch(
        ["execute as @e[type=minecraft:item] run data get entity @s Item"]) if "entity data" in l]
    return {"grid": {str(s): probe.slots.get(s) for s in range(0, 5)},
            "carried": probe.carried, "inventory": inventory, "ground": ground}


def reset(server: CraftServer, probe: WindowProbe, setup: list[str]) -> None:
    server.batch([f"clear {NAME}", "kill @e[type=minecraft:item]"] + setup)
    probe.settle(0.5)


def run(server: CraftServer, probe: WindowProbe) -> dict:
    out: dict = {}

    def case(name: str, setup: list[str], clicks: list[tuple[int, int, int]],
             close: bool = False) -> None:
        reset(server, probe, setup)
        before = snapshot(server, probe)
        for slot, button, mode in clicks:
            probe.click(slot, button, mode)
            probe.settle(0.3)
        if close:
            probe.send(SB_CLOSE_CONTAINER, bytes([0]))
            probe.settle(0.3)
        out[name] = {"setup": setup, "clicks": clicks, "close": close,
                     "before": before, "after": snapshot(server, probe)}
        print(f"{name}: {out[name]['after']['grid']} cursor={out[name]['after']['carried']}",
              flush=True)

    log4 = [f"item replace entity {NAME} hotbar.0 with minecraft:oak_log 4"]
    into_grid = [(1, 0, 2)]  # la barre 0 échangée avec la case 1 de la grille

    case("shift-result", log4, into_grid + [(0, 0, 1)])
    case("click-result", log4, into_grid + [(0, 0, 0)])
    case("right-click-result", log4, into_grid + [(0, 1, 0)])
    case("key-empty", log4, into_grid + [(0, 2, 2)])
    case("key-occupied", log4 + [f"item replace entity {NAME} hotbar.2 with minecraft:cobblestone"],
         into_grid + [(0, 2, 2)])
    case("throw-one", log4, into_grid + [(0, 0, 4)])
    case("throw-stack", log4, into_grid + [(0, 1, 4)])
    case("close-with-grid",
         log4 + [f"item replace entity {NAME} hotbar.1 with minecraft:dirt 5"],
         into_grid + [(37, 0, 0)], close=True)
    case("shift-helmet", [f"item replace entity {NAME} inventory.0 with minecraft:iron_helmet"],
         [(9, 0, 1)])
    case("shift-shield", [f"item replace entity {NAME} inventory.0 with minecraft:shield"],
         [(9, 0, 1)])
    case("double-click",
         [f"item replace entity {NAME} inventory.0 with minecraft:dirt 10",
          f"item replace entity {NAME} inventory.1 with minecraft:dirt 5",
          f"item replace entity {NAME} hotbar.3 with minecraft:dirt 3"],
         [(9, 0, 0), (9, 0, 6)])
    honey2 = [f"item replace entity {NAME} hotbar.0 with minecraft:honey_bottle 2"]
    case("honey-click", honey2, into_grid + [(0, 0, 0)])
    case("honey-shift", honey2, into_grid + [(0, 0, 1)])
    return out


def main() -> int:
    RUN.parent.mkdir(parents=True, exist_ok=True)
    server = CraftServer(RUN, port=PORT)
    result: dict = {}
    try:
        server.batch(["gamerule doMobSpawning false", "gamerule randomTickSpeed 0",
                      "gamerule doDaylightCycle false", "difficulty peaceful", "time set noon"])
        probe = WindowProbe(PORT, NAME)
        for _ in range(40):
            probe.settle(0.5)
            if any(NAME in line for line in server.batch(["list"])):
                break
        server.batch([f"gamemode survival {NAME}", f"tp {NAME} {STAND[0]} {STAND[1]} {STAND[2]}"])
        probe.settle(1.0)
        result = run(server, probe)
        probe.s.close()
    finally:
        server.stop()
    OUT.write_text(json.dumps(result, indent=1))
    print(f"écrit {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
