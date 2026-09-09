#!/usr/bin/env python3
"""Relève, sur un vrai serveur 1.20.1, les identifiants des paquets de recettes.

Les identifiants de paquets ne se devinent pas et ne sont pas stables d'une
version à l'autre. L'archive figée du protocole s'est déjà révélée fausse sur ce
dépôt — pour chaque identifiant vérifié — donc ils ne sont pas recopiés : une
sonde se connecte au serveur officiel, ouvre un four, et note ce qui arrive.

Trois paquets sont cherchés :

  * **Update Recipes**, envoyé une fois à la connexion. Il pèse plusieurs
    centaines de kilo-octets et ne ressemble à rien d'autre : c'est le plus
    gros paquet de la session, et le nombre de recettes qu'il annonce en tête
    doit valoir le nombre de fichiers du datapack.
  * **Update Recipe Book**, envoyé juste après, minuscule.
  * **Set Container Property**, que le four envoie à chaque tick où une de ses
    quatre valeurs bouge. On l'obtient en allumant un four sous les yeux de la
    sonde ; il fait exactement cinq octets de corps.

Usage : python3 scripts/capture_recipe_packets.py [sortie.json]
"""
from __future__ import annotations

import json
import os
import struct
import sys
import time
from collections import Counter
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from measure_entities import Server  # noqa: E402
from vanilla_miner import Miner, block_pos, read_varint, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = Path(os.environ.get("OV_NORMALIZED", ROOT / "data/vanilla/1.20.1/normalized"))
RECIPES = NORMALIZED.parent / "generated" / "data" / "minecraft" / "recipes"
RUN = ROOT / "run" / "capture-recipes"
PORT = 25599

FURNACE = (4, -60, 0)
STAND = (4.5, -59.0, 2.5)


def read_string(buf: bytes, i: int) -> tuple[str, int]:
    length, i = read_varint(buf, i)
    return buf[i:i + length].decode("utf-8"), i + length


def skip_slot(buf: bytes, i: int) -> int:
    from measure_crafting import skip_nbt
    if buf[i] == 0:
        return i + 1
    i += 1
    _item, i = read_varint(buf, i)
    i += 1                       # count
    return skip_nbt(buf, i)


def read_ingredient(buf: bytes, i: int) -> int:
    count, i = read_varint(buf, i)
    for _ in range(count):
        i = skip_slot(buf, i)
    return i


COOKING = {"minecraft:smelting", "minecraft:blasting", "minecraft:smoking",
           "minecraft:campfire_cooking"}


def decode_update_recipes(payload: bytes) -> dict:
    """Relit le paquet champ par champ, jusqu'au dernier octet.

    C'est la vérification du format : un ordre de champs faux ne rate pas
    doucement, il désynchronise à la première recette et le décodage n'atteint
    jamais la fin du paquet avec le bon compte. Terminer pile sur la fin après
    le nombre de recettes annoncé, c'est la preuve que la disposition est la
    bonne — et c'est cette disposition qu'écrit src/ov_protocol/src/recipe_packets.cpp.
    """
    count, i = read_varint(payload, 0)
    kinds: Counter[str] = Counter()
    for _ in range(count):
        kind, i = read_string(payload, i)
        _identifier, i = read_string(payload, i)
        kinds[kind] += 1
        if kind == "minecraft:crafting_shaped":
            width, i = read_varint(payload, i)
            height, i = read_varint(payload, i)
            _group, i = read_string(payload, i)
            _category, i = read_varint(payload, i)
            for _ in range(width * height):
                i = read_ingredient(payload, i)
            i = skip_slot(payload, i)
            i += 1                                   # show_notification
        elif kind == "minecraft:crafting_shapeless":
            _group, i = read_string(payload, i)
            _category, i = read_varint(payload, i)
            n, i = read_varint(payload, i)
            for _ in range(n):
                i = read_ingredient(payload, i)
            i = skip_slot(payload, i)
        elif kind in COOKING:
            _group, i = read_string(payload, i)
            _category, i = read_varint(payload, i)
            i = read_ingredient(payload, i)
            i = skip_slot(payload, i)
            i += 4                                   # experience, float
            _time, i = read_varint(payload, i)
        elif kind == "minecraft:stonecutting":
            _group, i = read_string(payload, i)
            i = read_ingredient(payload, i)
            i = skip_slot(payload, i)
        elif kind == "minecraft:smithing_transform":
            for _ in range(3):
                i = read_ingredient(payload, i)
            i = skip_slot(payload, i)
        elif kind == "minecraft:smithing_trim":
            for _ in range(3):
                i = read_ingredient(payload, i)
        else:
            _category, i = read_varint(payload, i)
    return {"declared": count, "consumed": i, "bytes": len(payload),
            "exact": i == len(payload), "kinds": dict(kinds)}


def main() -> int:
    out_path = Path(sys.argv[1]) if len(sys.argv) > 1 else NORMALIZED / "recipe_packets.json"
    expected = len(list(RECIPES.glob("*.json")))

    server = Server(RUN, port=PORT)
    seen: Counter[int] = Counter()
    largest: dict[int, int] = {}
    findings: dict[str, object] = {}
    try:
        server.batch(["gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                      "gamerule sendCommandFeedback true", "difficulty peaceful",
                      "time set noon", "forceload add -32 -32 32 32"])
        time.sleep(3.0)
        server.batch([
            f"setblock {FURNACE[0]} {FURNACE[1] - 1} {FURNACE[2]} minecraft:stone",
            f"setblock {FURNACE[0]} {FURNACE[1]} {FURNACE[2] + 2} minecraft:stone",
            f'setblock {FURNACE[0]} {FURNACE[1]} {FURNACE[2]} minecraft:furnace[lit=false]'
            f'{{Items:[{{Slot:0b,id:"minecraft:cobblestone",Count:64b}}]}}'])

        probe = Miner(PORT, "Capture0")

        def note(pid: int, payload: bytes):
            seen[pid] += 1
            largest[pid] = max(largest.get(pid, 0), len(payload))
            return None

        deadline = time.monotonic() + 12.0
        while time.monotonic() < deadline:
            probe.pump(until=note, timeout=0.1)

        # Le plus gros paquet de la connexion est Update Recipes. On le confirme
        # en lisant son compte de tête : il doit valoir le nombre de fichiers de
        # recette du datapack, sinon ce n'est pas lui.
        biggest = max(largest, key=lambda pid: largest[pid])
        findings["update_recipes"] = {"id": biggest, "bytes": largest[biggest]}

        # Le nombre annoncé, pour vérification.
        count = None
        holder: list[bytes] = []

        def grab(pid: int, payload: bytes):
            if pid == biggest:
                holder.append(payload)
                return True
            return None

        # Une seconde sonde, pour relire le paquet depuis le début.
        second = Miner(PORT, "Capture1")
        deadline = time.monotonic() + 12.0
        while not holder and time.monotonic() < deadline:
            second.pump(until=grab, timeout=0.1)
        if holder:
            count, _ = read_varint(holder[0], 0)
            findings["update_recipes"]["decoded"] = decode_update_recipes(holder[0])
        findings["update_recipes"]["declared_count"] = count
        findings["update_recipes"]["datapack_files"] = expected

        # ── Set Container Property ──────────────────────────────────────────
        server.batch(["gamemode creative Capture0",
                      f"tp Capture0 {STAND[0]} {STAND[1]} {STAND[2]}"])
        probe.pump(timeout=1.0)
        probe.stand(*STAND)
        probe.send(0x31, varint(0) + block_pos(*FURNACE) + varint(1)
                   + struct.pack(">fff", 0.5, 1.0, 0.5) + bytes([0]) + varint(0))
        before = time.monotonic()
        opened: dict[str, int] = {}
        window = [None]

        def watch_open(pid: int, payload: bytes):
            if window[0] is None and len(payload) > 2:
                # Open Screen : window id, type, titre. Le premier paquet qui
                # décode en trois VarInt plausibles suivis d'une chaîne.
                try:
                    wid, i = read_varint(payload, 0)
                    kind, i = read_varint(payload, i)
                except IndexError:
                    return None
                if 0 < wid < 100 and 0 <= kind < 64:
                    opened.setdefault("open_screen", pid)
                    window[0] = wid
            return None

        while time.monotonic() - before < 3.0:
            probe.pump(until=watch_open, timeout=0.1)

        # Le four s'allume : ses quatre valeurs bougent, et il les envoie une par
        # une. Un corps de cinq octets — un octet de fenêtre et deux Short.
        server.batch([f'data modify block {FURNACE[0]} {FURNACE[1]} {FURNACE[2]} Items '
                      f'append value {{Slot:1b,id:"minecraft:coal",Count:1b}}'])
        properties: Counter[int] = Counter()

        def watch_property(pid: int, payload: bytes):
            if len(payload) == 5:
                properties[pid] += 1
            return None

        before = time.monotonic()
        while time.monotonic() - before < 4.0:
            probe.pump(until=watch_property, timeout=0.1)

        findings["open_screen"] = opened.get("open_screen")
        findings["window_id"] = window[0]
        findings["set_container_property"] = (
            {"id": properties.most_common(1)[0][0], "seen": properties.most_common(1)[0][1]}
            if properties else None)
        findings["packets_seen"] = {str(pid): {"count": n, "largest": largest[pid]}
                                    for pid, n in seen.most_common()}

        print(json.dumps({k: v for k, v in findings.items() if k != "packets_seen"}, indent=1))
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps(
            {"$comment": "Identifiants de paquets relevés sur un vrai serveur 1.20.1, "
                         "pas recopiés d'une table.",
             "version": "1.20.1", "protocol": 763, **findings}, indent=1) + "\n",
            encoding="utf-8")
        print(f"écrit {out_path}")
    finally:
        server.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
