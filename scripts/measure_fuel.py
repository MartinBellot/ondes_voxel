#!/usr/bin/env python3
"""Mesure, contre un vrai serveur 1.20.1, combien de ticks brûle chaque objet.

Les temps de combustion ne sont ni dans les rapports du data generator ni dans
les datapacks : `AbstractFurnaceBlockEntity.getFuel()` est du code Java. Ils
sont donc mesurés, objet par objet, en demandant au jeu — pas recopiés d'une
table trouvée ailleurs.

Ce que le jeu donne exactement
-----------------------------
`BurnTime` décroît d'exactement un par tick et le four s'éteint à zéro. Donc,
pour un four allumé, la quantité

    C = BurnTime(t) + t

est **constante** pendant toute la combustion, et vaut le tick de jeu auquel le
four s'éteindra. Une seule lecture datée la donne sans approximation, à
condition que la date et la valeur voyagent dans le même lot de commandes — le
serveur exécute un lot dans un seul tick, donc elles sont bien simultanées.

La seule inconnue
-----------------
La durée cherchée est `F = C - t_allumage`, et `t_allumage` n'est pas commandé
directement : le four s'allume au premier tick de bloc-entité qui suit sa pose.
On écrit donc `t_allumage = T_pose + k`, où `T_pose` est daté exactement (la
commande `setblock` part avec un `time query gametime`) et `k` est un petit
entier, le même à chaque fois puisque c'est le même chemin de code.

`k` n'est pas supposé, il est **encadré** : en interrogeant l'état `lit` du
bloc autour de la pose, on obtient à chaque essai
`k ∈ ]T_dernier_éteint - T_pose, T_premier_allumé - T_pose]`. Les essais ne
tombent pas en phase, donc l'intersection de leurs encadrements se réduit à une
seule valeur en une dizaine de tentatives. Si elle ne s'y réduit pas, le script
s'arrête plutôt que d'écrire une table plausible.

Le four ordinaire, le haut-fourneau et le fumoir sont mesurés séparément : rien
ne garantit qu'ils lisent la même durée, et le rapport entre eux est un chiffre
à établir.

Ce qui reste dans la case à combustible après l'allumage est relevé au passage :
c'est le `craftingRemainingItem` de l'objet — le seau vide que rend un seau de
lave — et c'est du code Java lui aussi.

Usage : python3 scripts/measure_fuel.py [sortie.json]
"""
from __future__ import annotations

import json
import os
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from measure_entities import Server as BaseServer  # noqa: E402


class Server(BaseServer):
    """Le serveur de mesure, plus un bloc de commande répétitif.

    Un aller-retour console ne descend jamais sous deux ticks, donc la console
    seule ne sait pas dire dans quel tick exact un four s'est allumé. Un bloc de
    commande répétitif, lui, s'exécute une fois par tick — c'est le seul
    chronomètre au tick près que le jeu offre sans modifier le jeu.
    """

    EXTRA_PROPERTIES = "enable-command-block=true\n"

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = Path(os.environ.get("OV_NORMALIZED", ROOT / "data/vanilla/1.20.1/normalized"))
RUN = ROOT / "run" / "measure-fuel"

# Une grille de fours dans le vide d'un monde plat, deux blocs d'écart pour que
# `data get block` ne puisse jamais désigner le voisin.
COLUMNS = 40
SPACING = 2
Y = -60

GAMETIME = re.compile(r"The time is (\d+)")
BLOCKDATA = re.compile(r"^(-?\d+), (-?\d+), (-?\d+) has the following block data: (.*)$")
# Une entrée de la liste `Items`. L'ordre des champs dans ce que la console
# imprime est celui du NBT, pas celui qu'on croit : `Slot`, puis `id`, puis
# `Count`. Chercher l'inverse ne rate pas bruyamment, il rend « aucun reste ».
FUEL_ENTRY = re.compile(r'\{[^{}]*\}')
SLOT_FIELD = re.compile(r"Slot: (\d+)b")
ID_FIELD = re.compile(r'id: "([a-z0-9_.:/]+)"')

# Ce que chaque four essaie de fondre. Un four ne s'allume que s'il a quelque
# chose à cuire, et les trois n'acceptent pas la même chose : un haut-fourneau
# refuse la pierre, un fumoir refuse le minerai. Les charger tous du même objet
# donne deux grilles qui ne s'allument jamais — et une table vide qui ressemble
# à « aucun objet n'est un combustible ».
INPUT_ITEM = {
    "minecraft:furnace": "minecraft:cobblestone",
    "minecraft:blast_furnace": "minecraft:iron_ore",
    "minecraft:smoker": "minecraft:potato",
}

# Des combustibles délibérément courts, pour la calibration : leur valeur est
# justement ce qu'on mesure, ils ne sont choisis que pour ne pas durer.
CALIBRATION = {
    "minecraft:furnace": ["minecraft:stick", "minecraft:bowl", "minecraft:oak_sapling",
                          "minecraft:bamboo", "minecraft:oak_button"],
    "minecraft:blast_furnace": ["minecraft:stick", "minecraft:bowl",
                                "minecraft:oak_sapling", "minecraft:bamboo"],
    "minecraft:smoker": ["minecraft:stick", "minecraft:bowl", "minecraft:oak_sapling",
                         "minecraft:bamboo"],
}

FURNACES = ["minecraft:furnace", "minecraft:blast_furnace", "minecraft:smoker"]

# Où les essais d'encadrement posent leur four, loin de la grille de mesure.
PROBE_X, PROBE_Z = -16, -16


def cell(index: int) -> tuple[int, int]:
    return ((index % COLUMNS) * SPACING, (index // COLUMNS) * SPACING)


def gametime_of(lines: list[str]) -> int:
    for line in lines:
        m = GAMETIME.search(line)
        if m:
            return int(m.group(1))
    raise RuntimeError("le serveur n'a pas répondu à time query gametime")


def block_data(lines: list[str]) -> dict[tuple[int, int], str]:
    out = {}
    for line in lines:
        m = BLOCKDATA.search(line)
        if m:
            out[(int(m.group(1)), int(m.group(3)))] = m.group(4).strip()
    return out


class Rig:
    """La grille de fours et les questions qu'on lui pose."""

    def __init__(self, server: Server, kind: str) -> None:
        self.server = server
        self.kind = kind

    def load_command(self, x: int, z: int, fuel: str) -> str:
        return (f'setblock {x} {Y} {z} {self.kind}[lit=false]{{'
                f'Items:[{{Slot:0b,id:"{INPUT_ITEM[self.kind]}",Count:64b}},'
                f'{{Slot:1b,id:"{fuel}",Count:1b}}]}} replace')

    def clear(self, count: int) -> None:
        self.server.batch([f"setblock {cell(i)[0]} {Y} {cell(i)[1]} air replace"
                           for i in range(count)], timeout=900.0)

    def place_all(self, items: list[str]) -> int:
        """Pose un four chargé par objet, tous dans le même tick. Rend ce tick."""
        commands = ["time query gametime"]
        for i, item in enumerate(items):
            x, z = cell(i)
            commands.append(self.load_command(x, z, item))
        return gametime_of(self.server.batch(commands, timeout=900.0))

    def burn_all(self, count: int) -> tuple[int, dict[int, int]]:
        """`BurnTime` de chaque four et le tick de jeu de la lecture."""
        commands = ["time query gametime"]
        for i in range(count):
            x, z = cell(i)
            commands.append(f"data get block {x} {Y} {z} BurnTime")
        lines = self.server.batch(commands, timeout=900.0)
        when = gametime_of(lines)
        values: dict[int, int] = {}
        for (x, z), raw in block_data(lines).items():
            if not raw.endswith("s"):
                continue
            index = (z // SPACING) * COLUMNS + (x // SPACING)
            if 0 <= index < count:
                values[index] = int(raw[:-1])
        return when, values

    def fuel_slots(self, indices: list[int]) -> dict[int, str | None]:
        commands = [f"data get block {cell(i)[0]} {Y} {cell(i)[1]} Items" for i in indices]
        blobs = block_data(self.server.batch(commands, timeout=900.0))
        out: dict[int, str | None] = {}
        for i in indices:
            x, z = cell(i)
            blob = blobs.get((x, z))
            if blob is None:
                out[i] = None
                continue
            out[i] = None
            for entry in FUEL_ENTRY.findall(blob):
                slot = SLOT_FIELD.search(entry)
                name = ID_FIELD.search(entry)
                if slot is not None and name is not None and slot.group(1) == "1":
                    out[i] = name.group(1)
        return out

    # ── L'encadrement de k ──────────────────────────────────────────────────

    def probe_lit(self) -> tuple[int, bool]:
        lines = self.server.batch(["time query gametime",
                                   f"execute if block {PROBE_X} {Y} {PROBE_Z} "
                                   f"{self.kind}[lit=true] run say ovlit"], timeout=900.0)
        return gametime_of(lines), any("ovlit" in line for line in lines)

    def measure_k(self, calibration: list[str]) -> int:
        """Le décalage constant entre la pose d'un four et son allumage.

        Interroger la console ne suffit pas : un aller-retour console ne
        descend jamais sous deux ticks, si bien que l'encadrement de
        l'allumage ne se referme pas et laisse `k` dans {0, 1}. Un tick
        d'erreur ici se retrouverait tel quel dans les 1250 durées écrites.

        Un **bloc de commande répétitif**, lui, s'exécute exactement une fois
        par tick. Un compteur qu'il incrémente tant que le bloc est `lit`
        donne la durée de combustion sans aucune convention : le décalage de
        phase entre le bloc de commande et la bloc-entité s'applique
        identiquement à l'allumage et à l'extinction, donc s'annule.

        `k` est alors la différence entre ce que le compteur a vu et ce que la
        lecture datée annonçait. Plusieurs combustibles courts doivent donner
        le même `k` ; sinon le modèle est faux et le script s'arrête.
        """
        found: dict[str, int] = {}
        for fuel in calibration:
            self.server.batch([
                f"setblock {PROBE_X} {Y} {PROBE_Z} air replace",
                f"setblock {PROBE_X} {Y + 2} {PROBE_Z} air replace",
                "scoreboard objectives remove ovlit",
                "scoreboard objectives add ovlit dummy",
                "scoreboard players set ov ovlit 0",
            ])
            # `auto:1b` rend le bloc répétitif actif sans levier redstone. Sans
            # lui il ne tourne jamais et le compteur reste à zéro — ce qui se
            # lirait comme « ce combustible ne brûle pas ».
            self.server.batch([
                f'setblock {PROBE_X} {Y + 2} {PROBE_Z} minecraft:repeating_command_block'
                f'{{Command:"execute if block {PROBE_X} {Y} {PROBE_Z} {self.kind}[lit=true] '
                f'run scoreboard players add ov ovlit 1",auto:1b}} replace'])
            placed = gametime_of(self.server.batch(
                ["time query gametime", self.load_command(PROBE_X, PROBE_Z, fuel)]))
            when, values = None, {}
            deadline = time.monotonic() + 120.0
            counted = None
            first_read = None
            while time.monotonic() < deadline:
                lines = self.server.batch(["time query gametime",
                                           f"data get block {PROBE_X} {Y} {PROBE_Z} BurnTime",
                                           "scoreboard players get ov ovlit"])
                now = gametime_of(lines)
                burn = None
                for (x, _z), raw in block_data(lines).items():
                    if x == PROBE_X and raw.endswith("s"):
                        burn = int(raw[:-1])
                score = None
                for line in lines:
                    m = re.search(r"ov has (\d+) \[ovlit\]", line)
                    if m:
                        score = int(m.group(1))
                if burn is not None and burn > 0 and first_read is None:
                    first_read = burn + now  # C, le tick d'extinction
                if burn == 0 and score:
                    counted = score
                    break
                time.sleep(0.1)
            self.server.batch([f"setblock {PROBE_X} {Y + 2} {PROBE_Z} air replace"])
            if counted is None or first_read is None:
                print(f"    calibration : {fuel} n'a rien donné, ignoré")
                continue
            found[fuel] = (first_read - placed) - counted
            print(f"    {fuel} : compteur {counted} ticks, C-T_pose {first_read - placed}, "
                  f"k = {found[fuel]}")
            if len(found) >= 3:
                break
        if not found:
            raise RuntimeError("aucun combustible de calibration n'a brûlé")
        distinct = set(found.values())
        if len(distinct) != 1:
            raise RuntimeError(f"k n'est pas constant : {found}")
        return distinct.pop()


def measure(server: Server, kind: str, items: list[str]) -> tuple[dict[str, int],
                                                                  dict[str, str], int]:
    rig = Rig(server, kind)
    k = rig.measure_k(CALIBRATION[kind])
    print(f"    k = {k} tick(s) entre la pose et l'allumage")

    rig.clear(len(items))
    time.sleep(0.5)
    placed = rig.place_all(items)
    when, values = rig.burn_all(len(items))

    burn: dict[str, int] = {}
    for index, value in values.items():
        if value <= 0:
            continue
        # C = BurnTime + t est le tick d'extinction ; F = C - (T_pose + k).
        burn[items[index]] = value + when - placed - k

    # La pente vaut-elle bien -1 par tick ? Une seconde lecture datée doit
    # donner exactement le même C pour chaque four. Si ce n'est pas le cas, le
    # modèle est faux et tout ce qui suit le serait aussi.
    time.sleep(1.0)
    when2, values2 = rig.burn_all(len(items))
    drift = {items[i]: (values2[i] + when2) - (values[i] + when)
             for i in values2 if i in values and values[i] > 0 and values2[i] > 0}
    bad = {n: d for n, d in drift.items() if d != 0}
    if bad:
        raise RuntimeError(f"C n'est pas constant pour {len(bad)} fours, "
                           f"exemples {list(bad.items())[:5]}")
    print(f"    pente vérifiée sur {len(drift)} fours : C constant")

    index_of = {name: i for i, name in enumerate(items)}
    residue = rig.fuel_slots([index_of[n] for n in burn])
    remainder = {}
    for name in burn:
        left = residue.get(index_of[name])
        if left is not None and left != name:
            remainder[name] = left

    return burn, remainder, k


def main() -> int:
    out_path = Path(sys.argv[1]) if len(sys.argv) > 1 else NORMALIZED / "fuel.json"
    with open(NORMALIZED / "registries.json") as f:
        registries = json.load(f)["registries"]
    all_items: list[str] = registries["minecraft:item"]["entries"]
    items = [i for i in all_items if i != "minecraft:air"]

    print(f"{len(items)} objets candidats, {len(FURNACES)} types de four")
    server = Server(RUN)
    try:
        server.batch([
            "gamerule doMobSpawning false", "gamerule randomTickSpeed 0",
            "gamerule doDaylightCycle false", "gamerule doWeatherCycle false",
            "gamerule doFireTick false", "gamerule sendCommandFeedback true",
            "difficulty peaceful", "time set noon",
        ])
        width = COLUMNS * SPACING
        depth = ((len(items) - 1) // COLUMNS + 1) * SPACING
        for x0 in range(-64, width + 32, 128):
            for z0 in range(-64, depth + 32, 128):
                server.batch([f"forceload add {x0} {z0} "
                              f"{min(x0 + 127, width + 32)} {min(z0 + 127, depth + 32)}"],
                             timeout=900.0)
        time.sleep(5.0)

        results: dict[str, dict[str, int]] = {}
        remainders: dict[str, str] = {}
        ks: dict[str, int] = {}
        for kind in FURNACES:
            print(f"── {kind}")
            burn, remainder, k = measure(server, kind, items)
            print(f"    {len(burn)} combustibles, {len(remainder)} avec un reste")
            results[kind] = burn
            ks[kind] = k
            remainders.update(remainder)

        base = results["minecraft:furnace"]
        ratios = {}
        for kind in FURNACES[1:]:
            other = results[kind]
            observed = sorted({round(base[n] / other[n], 6)
                               for n in base if n in other and other[n]})
            ratios[kind] = observed
            print(f"    four / {kind.split(':')[1]} : {observed[:4]}"
                  f"{'…' if len(observed) > 4 else ''}")

        doc = {
            "$comment": "Temps de combustion mesurés sur un vrai serveur 1.20.1. "
                        "BurnTime + tick de lecture est constant et vaut le tick "
                        "d'extinction ; la durée est cet instant moins le tick "
                        "d'allumage, lui-même encadré par observation de l'état "
                        "lit du bloc. Rien n'est recopié.",
            "version": "1.20.1",
            "candidates": len(items),
            "ignition_delay_ticks": ks,
            "ratio_furnace_over_kind": ratios,
            "measured": {k: len(v) for k, v in results.items()},
            "burn_ticks": {k: dict(sorted(v.items())) for k, v in results.items()},
            "fuel_remainder": dict(sorted(remainders.items())),
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
