#!/usr/bin/env python3
"""La balise contre le vrai serveur 1.20.1 : les niveaux de pyramide, le menu,
le paiement, le paquet `Set Beacon Effect` et les effets posés sur le joueur.

La balise se relit toutes les 80 ticks : chaque lecture attend donc un peu
plus de quatre secondes. Relevé, pour chaque cas : les propriétés de fenêtre
(0 les niveaux, 1 l'effet principal, 2 le secondaire), le NBT du bloc-entité
(`Levels`, `Primary`, `Secondary`) et `data get entity … ActiveEffects`.

`Set Beacon Effect` (0x27 en 763) : un booléen de présence et un VarInt pour
l'effet principal, puis de même pour le secondaire — l'identifiant est celui
du registre minecraft:mob_effect (vitesse 1, célérité 3, force 5, saut 8,
régénération 10, résistance 11).

Sortie : `.scratch/beacon.json`. Usage : python3 scripts/measure_beacon.py
"""
from __future__ import annotations

import json
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from measure_crafting import SB_CLOSE_CONTAINER, SB_USE_ITEM_ON, Server as CraftServer  # noqa: E402
from measure_loom import CB_CONTAINER_PROPERTY, LoomProbe  # noqa: E402
from vanilla_miner import block_pos, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / ".scratch" / "measure-beacon"
OUT = ROOT / ".scratch" / "beacon.json"
PORT = 25622
NAME = "Beam0"
BEACON = (20, -56, 20)
STAND = (21.5, -56.0, 20.5)
SB_SET_BEACON = 0x27
SPEED, HASTE, STRENGTH, REGENERATION = 1, 3, 5, 10


def optional(effect: int | None) -> bytes:
    return bytes([0]) if effect is None else bytes([1]) + varint(effect)


class BeaconProbe(LoomProbe):
    def __init__(self, port: int, name: str) -> None:
        super().__init__(port, name)
        self.props: dict[int, int] = {}

    def handle(self, pid: int, payload: bytes) -> None:
        if pid == CB_CONTAINER_PROPERTY and self.window is not None and payload[0] == self.window:
            prop, value = struct.unpack_from(">hh", payload, 1)
            self.props[prop] = value
        super().handle(pid, payload)

    def open_block(self) -> None:
        self.window = None
        self.props = {}
        for _ in range(8):
            self.stand(*STAND)
            self.settle(0.3)
            self.send(SB_USE_ITEM_ON, varint(0) + block_pos(*BEACON) + varint(1)
                      + struct.pack(">fff", 0.5, 1.0, 0.5) + bytes([0]) + varint(0))
            for _ in range(20):
                if self.window is not None:
                    self.settle(0.6)
                    return
                self.settle(0.1)
        raise RuntimeError("la balise ne s'est pas ouverte")

    def choose(self, primary: int | None, secondary: int | None) -> None:
        self.send(SB_SET_BEACON, optional(primary) + optional(secondary))
        self.settle(0.6)

    def close(self) -> None:
        if self.window is not None:
            self.send(SB_CLOSE_CONTAINER, bytes([self.window]))
            self.window = None
            self.settle(0.3)


def pyramid(level: int) -> list[str]:
    """Une pyramide de blocs de fer de `level` étages sous la balise."""
    x, y, z = BEACON
    commands = [f"fill {x - 5} {y - 5} {z - 5} {x + 5} {y + 2} {z + 5} minecraft:air"]
    for step in range(1, level + 1):
        r = step
        commands.append(f"fill {x - r} {y - step} {z - r} {x + r} {y - step} {z + r} "
                        "minecraft:iron_block")
    commands.append(f"setblock {x} {y} {z} minecraft:beacon")
    return commands


def main() -> int:
    RUN.parent.mkdir(parents=True, exist_ok=True)
    server = CraftServer(RUN, port=PORT)
    out: dict = {"levels": {}, "cases": {}}
    try:
        server.batch(["gamerule doMobSpawning false", "difficulty peaceful", "time set noon",
                      "gamerule doDaylightCycle false"])
        probe = BeaconProbe(PORT, NAME)
        for _ in range(40):
            probe.settle(0.5)
            if any(NAME in line for line in server.batch(["list"])):
                break
        server.batch([f"gamemode survival {NAME}"])

        def read_state() -> dict:
            block = [l[l.find("data:"):] for l in server.batch(
                [f"data get block {BEACON[0]} {BEACON[1]} {BEACON[2]}"]) if "block data" in l]
            effects = [l[l.find("data:"):] for l in server.batch(
                [f"data get entity {NAME} ActiveEffects"]) if "entity data" in l]
            return {"block": block, "effects": effects}

        def setup(level: int, items: list[str]) -> None:
            probe.close()
            server.batch(pyramid(level) + [f"effect clear {NAME}", f"clear {NAME}",
                                           f"tp {NAME} {STAND[0]} {STAND[1]} {STAND[2]}"]
                         + [f"item replace entity {NAME} hotbar.{b} with {what}"
                            for b, what in enumerate(items)])
            time.sleep(4.5)  # une relecture de la balise
            probe.open_block()

        for level in (1, 2, 3, 4):
            setup(level, [])
            out["levels"][str(level)] = {"props": dict(probe.props), "menu_type": probe.menu_type,
                                         "title": probe.title, "slot_count": probe.slot_count}
            print(f"level {level}: {out['levels'][str(level)]}", flush=True)

        def case(name: str, level: int, pay: bool, primary: int | None,
                 secondary: int | None) -> None:
            setup(level, ["minecraft:iron_ingot 3"] if pay else [])
            if pay:
                probe.click(0, 0, 2)  # la barre 0 dans la case de paiement
                probe.settle(0.3)
            before = dict(probe.props)
            probe.choose(primary, secondary)
            after = dict(probe.props)
            payment = probe.full.get(0)
            probe.close()
            time.sleep(4.5)
            state = read_state()
            inventory = [l[l.find("data:"):] for l in server.batch(
                [f"data get entity {NAME} Inventory"]) if "entity data" in l]
            out["cases"][name] = {"before": before, "after": after, "payment_slot": payment,
                                  "inventory": inventory, **state}
            print(f"{name}: props {before} -> {after} effects={state['effects']}", flush=True)

        case("speed", 4, True, SPEED, None)
        case("unpaid", 4, False, HASTE, None)
        case("speed+regen", 4, True, SPEED, REGENERATION)
        case("haste+haste", 4, True, HASTE, HASTE)
        case("strength-at-1", 1, True, STRENGTH, None)
        probe.s.close()
    finally:
        server.stop()
    OUT.write_text(json.dumps(out, indent=1, default=str))
    print(f"écrit {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
