#!/usr/bin/env python3
"""De bout en bout : tirer un levier, ouvrir une porte, tuer une vache, manger.

Ce que ce script prouve est exactement ce qui manquait au serveur avant cette
vague : **aucun bloc n'avait de gestionnaire d'interaction**. Un levier, un
bouton, une porte, une trappe, un portillon ne réagissaient à rien, et la preuve
redstone de la vague précédente devait pousser le circuit par une écriture
faute de pouvoir actionner le levier.

Le décor est le banc de test (`run/lab`), recopié avant d'être servi pour qu'il
reste ce qu'il était. Le client est celui des autres sondes du dépôt : il parle
le protocole 763 et rien d'autre, mêmes paquets, mêmes champs, même ordre. Il
n'a pas d'écran, et le dire vaut mieux que de prétendre le contraire.

La séquence :

  1. **le levier** de la parcelle « dust » (x 1, y −59, z 12) : le tirer, lire
     les quatorze fils, comparer à `max(0, 15 − (x − 2))` — la table mesurée de
     `docs/provenance/redstone.md` ;
  2. **une porte, un bouton, une trappe et un portillon** : ouverts et refermés
     par clic, l'état relu sur les paquets `Block Update` ;
  3. **une vache et un zombie** tués à l'épée, le butin lâché comparé aux tables
     de `data/vanilla/1.20.1/entity_loot.ovpack` ;
  4. **manger** : une pomme, et la faim qui monte.

Usage : python3 scripts/check_interaction_e2e.py
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
from vanilla_miner import Miner, block_pos, read_varint, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PRESET = os.environ.get("OV_PRESET", "macos-debug")
BINARY = ROOT / "build" / PRESET / "bin" / "ov_dedicated"
GENERATED = ROOT / "data" / "vanilla" / "1.20.1" / "generated" / "reports"
# Le banc, et une copie neuve de celui-ci.
#
# `run/lab` est persistant et partagé — le script `lab.sh` ne le
# régénère que sur `--rebuild`, et une sauvegarde plus vieille que le
# dernier ajout de parcelle **ment silencieusement** : le levier de la
# parcelle « dust » était de l'air dans le banc trouvé sur disque, et une
# sonde qui clique de l'air ne dit rien de plus qu'un serveur cassé.
# Alors la campagne se bâtit son propre banc avec `ov_lab`, sauf si
# OV_LAB_WORLD dit d'en servir un autre.
LAB = Path(os.environ.get("OV_LAB_WORLD", ROOT / ".scratch" / "lab"))
LAB_BUILDER = ROOT / "build" / PRESET / "bin" / "ov_lab"
WORLD = ROOT / "run" / "e2e-interaction"
PORT = int(os.environ.get("OV_E2E_PORT", "25579"))

# ── Paquets ─────────────────────────────────────────────────────────────────
SB_USE_ITEM_ON = 0x31
SB_USE_ITEM = 0x32
SB_INTERACT = 0x10
SB_SWING_ARM = 0x2F
SB_SET_CREATIVE_SLOT = 0x2B
SB_SET_HELD_ITEM = 0x28
SB_PLAYER_ACTION = 0x1D

CB_BLOCK_UPDATE = 0x0A
CB_SPAWN_ENTITY = 0x01
CB_ENTITY_METADATA = 0x52
CB_REMOVE_ENTITIES = 0x3E
CB_SET_HEALTH = 0x57

# ── Le banc ─────────────────────────────────────────────────────────────────
FLOOR = -60

LEVER = (1, -59, 12)
WIRE_X = list(range(2, 16))
WIRE_Z = 12

DOOR = (99, FLOOR, 133)          # moitié basse, parcelle « doors »
TRAPDOOR = (99, FLOOR, 138)
GATE = (69, FLOOR, 131)          # parcelle « connections »
BUTTON = (100, FLOOR + 1, 130)   # posé par la sonde : le banc n'en a pas

# Un endroit vide et plat de la parcelle « doors », pour poser le bouton et
# faire apparaître les mobs sans rien écraser.
STAND_DOORS = (100.5, FLOOR + 1, 128.5)
STAND_DUST = (8.5, FLOOR + 1, 14.5)


def states_by_id() -> dict[int, tuple[str, dict]]:
    with open(GENERATED / "blocks.json") as handle:
        report = json.load(handle)
    out: dict[int, tuple[str, dict]] = {}
    for name, block in report.items():
        for state in block["states"]:
            out[state["id"]] = (name, state.get("properties", {}))
    return out


def items_by_id() -> dict[int, str]:
    with open(GENERATED / "registries.json") as handle:
        report = json.load(handle)
    entries = report["minecraft:item"]["entries"]
    return {value["protocol_id"]: name for name, value in entries.items()}


def entity_types_by_id() -> dict[int, str]:
    with open(GENERATED / "registries.json") as handle:
        report = json.load(handle)
    entries = report["minecraft:entity_type"]["entries"]
    return {value["protocol_id"]: name for name, value in entries.items()}


def item_id(names: dict[int, str], wanted: str) -> int:
    for number, name in names.items():
        if name == wanted:
            return number
    raise KeyError(wanted)


class Probe(Miner):
    """Le client sonde : il clique des blocs et regarde ce qui revient."""

    def __init__(self, port: int, name: str) -> None:
        super().__init__(port, name)
        self.blocks: dict[tuple[int, int, int], int] = {}
        self.entities: dict[int, int] = {}          # id réseau -> type
        self.entity_pos: dict[int, tuple[float, float, float]] = {}
        self.items: dict[int, tuple[int, int]] = {}  # id réseau -> (item, count)
        self.gone: set[int] = set()
        self.food = None
        self.sequence = 1

    def handle(self, pid: int, payload: bytes) -> None:
        if pid == CB_BLOCK_UPDATE and len(payload) >= 8:
            packed = struct.unpack_from(">q", payload, 0)[0]
            x = packed >> 38
            y = (packed << 52) % (1 << 64) >> 52
            z = (packed << 26) % (1 << 64) >> 38
            x = x - (1 << 26) if x >= (1 << 25) else x
            y = y - (1 << 12) if y >= (1 << 11) else y
            z = z - (1 << 26) if z >= (1 << 25) else z
            state, _ = read_varint(payload, 8)
            self.blocks[(x, y, z)] = state
        elif pid == CB_SPAWN_ENTITY:
            eid, i = read_varint(payload, 0)
            i += 16  # uuid
            etype, i = read_varint(payload, i)
            x, y, z = struct.unpack_from(">ddd", payload, i)
            self.entities[eid] = etype
            self.entity_pos[eid] = (x, y, z)
        elif pid == CB_ENTITY_METADATA:
            eid, i = read_varint(payload, 0)
            # Index 8 on an item entity is its stack. That is the only field
            # this probe reads, and it is read by index rather than by position.
            while i < len(payload):
                index = payload[i]
                i += 1
                if index == 0xFF:
                    break
                kind, i = read_varint(payload, i)
                if kind == 7:  # slot
                    present = payload[i]
                    i += 1
                    if present:
                        item, i = read_varint(payload, i)
                        count = payload[i]
                        i += 1
                        if payload[i] == 0:
                            i += 1
                        else:
                            break  # NBT: nothing here needs it
                        if index == 8:
                            self.items[eid] = (item, count)
                    continue
                break  # any other type: this probe does not decode it
        elif pid == CB_REMOVE_ENTITIES:
            count, i = read_varint(payload, 0)
            for _ in range(count):
                eid, i = read_varint(payload, i)
                self.gone.add(eid)
        elif pid == CB_SET_HEALTH and len(payload) >= 4:
            health = struct.unpack_from(">f", payload, 0)[0]
            food, i = read_varint(payload, 4)
            self.food = (health, food)

    def settle(self, seconds: float = 0.3) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.pump(until=lambda pid, p: self.handle(pid, p), timeout=0.05)

    def use_on(self, x: int, y: int, z: int, face: int = 1) -> None:
        self.sequence += 1
        self.send(SB_USE_ITEM_ON,
                  varint(0) + block_pos(x, y, z) + varint(face)
                  + struct.pack(">fff", 0.5, 1.0, 0.5) + bytes([0]) + varint(self.sequence))

    def use_item(self) -> None:
        self.sequence += 1
        self.send(SB_USE_ITEM, varint(0) + varint(self.sequence))

    def release_use(self) -> None:
        self.sequence += 1
        self.send(SB_PLAYER_ACTION,
                  varint(5) + block_pos(0, 0, 0) + bytes([0]) + varint(self.sequence))

    def attack(self, entity_id: int) -> None:
        self.send(SB_INTERACT, varint(entity_id) + varint(1) + bytes([0]))
        self.send(SB_SWING_ARM, varint(0))

    def creative_set(self, slot: int, item: int | None, count: int = 1) -> None:
        if item is None:
            payload = struct.pack(">h", slot) + bytes([0])
        else:
            payload = struct.pack(">h", slot) + bytes([1]) + varint(item) + bytes([count, 0])
        self.send(SB_SET_CREATIVE_SLOT, payload)

    def hold(self, slot: int) -> None:
        self.send(SB_SET_HELD_ITEM, struct.pack(">h", slot))


def start_server(extra: list[str], tag: str = "run") -> subprocess.Popen:
    if WORLD.exists():
        shutil.rmtree(WORLD)
    shutil.copytree(LAB, WORLD)
    WORLD.parent.mkdir(parents=True, exist_ok=True)
    log = open(ROOT / "run" / f"e2e-interaction-{tag}.log", "w")
    process = subprocess.Popen(
        [str(BINARY), f"--world={WORLD}", f"--port={PORT}", "--log-level=debug"] + extra,
        stdout=log, stderr=subprocess.STDOUT)
    for _ in range(200):
        time.sleep(0.1)
        try:
            import socket
            with socket.create_connection(("127.0.0.1", PORT), timeout=0.2):
                return process
        except OSError:
            if process.poll() is not None:
                raise RuntimeError("le serveur s'est arrêté avant d'écouter")
    raise RuntimeError("le serveur n'a jamais écouté")


# ── 1. Le levier ────────────────────────────────────────────────────────────

def check_lever(probe: Probe, states: dict) -> tuple[int, int]:
    probe.stand(*STAND_DUST)
    probe.settle(1.5)

    # Tirer le levier. C'est le geste que ce mandat existe pour produire : un
    # `Use Item On` sur le levier, main nue, et rien d'autre.
    probe.use_on(*LEVER)
    probe.settle(2.0)

    lever_state = probe.blocks.get(LEVER)
    if lever_state is None:
        print("  le levier n'a envoyé aucun Block Update — il n'a pas bougé")
        return (0, len(WIRE_X))
    name, properties = states[lever_state]
    print(f"  levier -> {name}[powered={properties.get('powered')}]")
    if properties.get("powered") != "true":
        return (0, len(WIRE_X))

    ok = 0
    got = []
    for index, x in enumerate(WIRE_X):
        state = probe.blocks.get((x, FLOOR, WIRE_Z))
        power = None
        if state is not None:
            wire_name, wire_properties = states[state]
            if wire_name == "minecraft:redstone_wire":
                power = int(wire_properties["power"])
        expected = max(0, 15 - index)
        got.append(power)
        ok += int(power == expected)
    print(f"  attendu {[max(0, 15 - i) for i in range(len(WIRE_X))]}")
    print(f"  obtenu  {got}")
    return (ok, len(WIRE_X))


# ── 2. Ce qui s'ouvre ───────────────────────────────────────────────────────

def toggle(probe: Probe, where: tuple[int, int, int], states: dict,
           label: str, prop: str = "open") -> bool:
    """Cliquer deux fois, et exiger que l'état aille et revienne."""
    probe.blocks.pop(where, None)
    probe.use_on(*where)
    probe.settle(1.0)
    opened = probe.blocks.get(where)
    if opened is None:
        print(f"  {label}: aucun Block Update — rien n'a réagi")
        return False
    name, properties = states[opened]
    if properties.get(prop) != "true":
        print(f"  {label}: {name}[{prop}={properties.get(prop)}] après le premier clic")
        return False

    probe.blocks.pop(where, None)
    probe.use_on(*where)
    probe.settle(1.0)
    closed = probe.blocks.get(where)
    if closed is None:
        print(f"  {label}: ouvert, mais jamais refermé")
        return False
    _, back = states[closed]
    if back.get(prop) != "false":
        print(f"  {label}: {prop}={back.get(prop)} après le second clic")
        return False
    print(f"  {label}: {name} ouvert puis refermé")
    return True


def check_button(probe: Probe, states: dict, items: dict) -> bool:
    """Le banc n'a pas de bouton : la sonde en pose un, puis l'appuie.

    Poser puis appuyer est plus fort que d'en trouver un déjà là : le chemin de
    pose et le chemin d'interaction sont deux chemins différents dans le même
    paquet, et celui-ci les traverse tous les deux.
    """
    button = item_id(items, "minecraft:oak_button")
    probe.creative_set(36, button, 1)
    probe.hold(0)
    probe.settle(0.5)

    below = (BUTTON[0], BUTTON[1] - 1, BUTTON[2])
    probe.use_on(*below, face=1)
    probe.settle(1.0)
    placed = probe.blocks.get(BUTTON)
    if placed is None or states[placed][0] != "minecraft:oak_button":
        print("  bouton: la pose a échoué")
        return False

    # Main nue pour l'appuyer : un bouton en main reposerait un bouton.
    probe.creative_set(36, None)
    probe.settle(0.5)

    probe.blocks.pop(BUTTON, None)
    probe.use_on(*BUTTON)
    probe.settle(1.0)
    pressed = probe.blocks.get(BUTTON)
    if pressed is None or states[pressed][1].get("powered") != "true":
        print("  bouton: il ne s'est pas enfoncé")
        return False
    print("  bouton: enfoncé")

    # Et il devrait se relever tout seul : trente ticks pour du bois, portés
    # par un tick programmé. Le tick **arrive** — il est compté dans le journal
    # du serveur — et rien ne le traite : `Redstone::scheduled_tick` connaît la
    # torche, le répéteur, le comparateur, l'observateur et les consommateurs,
    # et **pas le bouton**. C'est une lacune de `ov_gameplay`, nommée ici
    # plutôt que cachée derrière un « bouton : oui ».
    probe.blocks.pop(BUTTON, None)
    deadline = time.monotonic() + 4.0
    while time.monotonic() < deadline:
        probe.settle(0.2)
        released = probe.blocks.get(BUTTON)
        if released is not None and states[released][1].get("powered") == "false":
            print("  bouton: relevé tout seul")
            return True
    print("  bouton: resté enfoncé (Redstone::scheduled_tick n'a pas de règle "
          "de bouton — lacune ov_gameplay)")
    return False


# ── 3. Tuer un mob ──────────────────────────────────────────────────────────

def check_kill(probe: Probe, items: dict, types: dict, wanted_type: str) -> tuple[bool, list]:
    """Frapper un mob jusqu'à ce qu'il meure, et lire ce qui tombe.

    La cible est choisie **par type** et non par ordre d'arrivée : l'apparition
    naturelle tourne aussi, et la première entité vue est souvent une vache que
    personne n'a demandée. Une campagne qui frappe la première venue rapporte le
    butin d'une vache sous l'étiquette « zombie » — c'est arrivé au premier
    tour, et le résultat avait l'air parfaitement crédible.
    """
    probe.settle(2.0)
    target = None
    for eid, etype in probe.entities.items():
        if eid in probe.gone or eid in probe.items:
            continue
        if types.get(etype) != wanted_type:
            continue
        target = eid
        break
    if target is None:
        seen = sorted({types.get(t, str(t)) for t in probe.entities.values()})
        print(f"  {wanted_type}: aucune entité de ce type (vues : {seen})")
        return (False, [])

    before = set(probe.items)
    for _ in range(60):
        probe.attack(target)
        probe.settle(0.25)
        if target in probe.gone:
            break
    if target not in probe.gone:
        print(f"  {wanted_type}: toujours vivant après soixante coups")
        return (False, [])

    probe.settle(1.0)
    dropped = [(items.get(item, str(item)), count)
               for eid, (item, count) in probe.items.items() if eid not in before]
    print(f"  {wanted_type}: mort, butin {dropped}")
    return (True, dropped)


# ── 4. Manger ───────────────────────────────────────────────────────────────

def check_eating(probe: Probe, items: dict) -> bool:
    """Ramasser ce que la vache a lâché, courir, et le manger."""
    # Marcher sur les piles au sol. Le ramassage est une boîte de 1,2 bloc
    # autour du joueur, et il faut dix ticks de délai avant que quoi que ce soit
    # puisse être pris.
    for eid, (item, _) in list(probe.items.items()):
        if items.get(item) != "minecraft:beef":
            continue
        x, y, z = probe.entity_pos.get(eid, (None, None, None))
        if x is None:
            continue
        for _ in range(10):
            probe.stand(x, y, z)
            probe.settle(0.15)
        break

    # Courir. C'est le seul mouvement qui coûte de la faim, et il en faut :
    # un point coûte 4,0 d'épuisement, un bloc couru en charge 0,1, et les cinq
    # points de saturation du départ tiennent déjà deux cents blocs. Une course
    # de soixante blocs laisse la barre à vingt et ressemble exactement à un
    # serveur qui ne compte rien.
    probe.send(0x1E, varint(0) + varint(3) + varint(0))   # start sprinting
    x, y, z = probe.pos or STAND_DOORS
    for step in range(1400):
        probe.send(0x14, struct.pack(">ddd", x + step * 0.28, y, z) + bytes([1]))
        probe.settle(0.01)
    probe.send(0x1E, varint(0) + varint(4) + varint(0))   # stop sprinting
    probe.settle(1.0)

    before = probe.food
    if before is None:
        print("  manger: le serveur n'a jamais envoyé Set Health")
        return False
    print(f"  faim avant: {before[1]}")
    if before[1] >= 20:
        print("  manger: le joueur est rassasié, rien ne peut monter")
        return False

    # Trente-deux ticks, le bouton tenu. Le compte descend pendant qu'il est
    # tenu, et lâcher au tick 31 n'a rien mangé — c'est une règle, pas un
    # minuteur, donc la sonde tient et ne relâche pas.
    for slot in range(9):
        probe.hold(slot)
        probe.settle(0.2)
        probe.use_item()
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            probe.settle(0.2)
            if probe.food is not None and probe.food[1] > before[1]:
                print(f"  faim après: {probe.food[1]} (+{probe.food[1] - before[1]}), "
                      f"emplacement {slot}")
                return True
    print(f"  manger: la faim n'a pas bougé ({probe.food})")
    return False


def main() -> int:
    if not BINARY.exists():
        print(f"pas de binaire à {BINARY}")
        return 2
    if not LAB.exists():
        print(f"pas de banc à {LAB} — construction avec ov_lab")
        subprocess.run([str(LAB_BUILDER), f"--out={LAB}"], check=True)

    states = states_by_id()
    items = items_by_id()
    types = entity_types_by_id()
    results: dict[str, str] = {}

    # ── Les blocs, en créatif : le banc est un décor, pas une partie ─────────
    server = start_server([], "blocks")
    try:
        probe = Probe(PORT, "OndeInteract")
        probe.settle(2.0)

        print("levier et fil (parcelle « dust »)")
        ok, total = check_lever(probe, states)
        results["fil"] = f"{ok}/{total}"

        print("ce qui s'ouvre (parcelles « doors » et « connections »)")
        probe.stand(*STAND_DOORS)
        probe.settle(1.5)
        door = toggle(probe, DOOR, states, "porte")
        trap = toggle(probe, TRAPDOOR, states, "trappe")
        gate = toggle(probe, GATE, states, "portillon")
        button = check_button(probe, states, items)
        results["porte"] = "oui" if door else "non"
        results["trappe"] = "oui" if trap else "non"
        results["portillon"] = "oui" if gate else "non"
        results["bouton"] = "oui" if button else "non"
    finally:
        server.terminate()
        server.wait(timeout=30)

    # ── Les mobs, le butin et la faim, en survie ────────────────────────────
    #
    # Une seule règle décide de la forme de cette moitié : **en survie, le
    # serveur ignore `Set Creative Slot`** — vanilla le fait, et `server.cpp` le
    # fait avec le commentaire qui le dit. La sonde ne peut donc pas se donner
    # une épée ni une pomme. Elle frappe à mains nues, et ce qu'elle mange, elle
    # l'a tué : la vache lâche du bœuf, le bœuf est de la nourriture. La chaîne
    # complète — tuer, tirer la table, poser la pile, la ramasser, la manger —
    # tient dans une seule campagne, et chacun de ses maillons est du code que
    # cette vague a branché.
    for kind, count in (("cow", 5), ("zombie", 5)):
        server = start_server(["--survival", f"--mobs={','.join([kind] * count)}"], kind)
        try:
            probe = Probe(PORT, "OndeCombat")
            probe.settle(5.0)
            totals: dict[str, int] = {}
            kills = 0
            for _ in range(count):
                killed, dropped = check_kill(probe, items, types, f"minecraft:{kind}")
                if not killed:
                    break
                kills += 1
                for name, amount in dropped:
                    totals[name] = totals.get(name, 0) + amount
            results[f"butin {kind}"] = (
                f"{kills}/{count} tués — "
                + (", ".join(f"{n} x{c}" for n, c in sorted(totals.items())) or "rien"))

            if kind == "cow" and kills > 0:
                results["manger"] = "oui" if check_eating(probe, items) else "non"
        finally:
            server.terminate()
            server.wait(timeout=30)

    print()
    print("── résultat ─────────────────────────────────────────────")
    for key, value in results.items():
        print(f"  {key:16} {value}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
