#!/usr/bin/env python3
"""Confronte nos tirages de butin d'entité à ceux du vrai serveur 1.20.1.

Même méthode que scripts/check_loot.py pour les blocs, et pour la même raison :
les tables sont des données de vanilla, régénérées localement ; c'est
l'**interpréteur** qui est du code, et c'est lui qu'on vérifie.

La commande `/loot give <joueur> kill <entité>` tire la table d'une entité sans
qu'il faille la tuer. Le contexte qu'elle construit est précis et il faut le
reproduire exactement de notre côté : pas de tueur, donc `killed_by_player`
faux et `looting` nul, et l'entité est lue telle qu'elle est — sa taille pour
une slime, son feu pour une vache. Les tirages rares que seul un joueur
déclenche sont donc absents des deux côtés, ce qui est une **égalité de
contexte**, pas une lacune : `scripts/measure_combat.py --only looting` les
mesure séparément, avec de vrais meurtres.

Trois pièges payés ici :

  * Les seize tables de laine vivent dans `entities/sheep/` et un glob `*.json`
    les rate toutes. Un mouton est tiré en fixant sa couleur par NBT.
  * Le `Size` NBT d'une slime est décalé de un : `Size:0` est la taille 1, celle
    que le prédicat `type_specific.size` nomme.
  * L'inventaire ne tient que trente-six piles. Un tirage généreux le remplit et
    le total s'arrête net sur un nombre parfaitement plausible. D'où les paquets
    de vingt-quatre.

Usage : python3 scripts/check_entity_loot.py [sortie.json] [tirages]
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_entity_packets import Probe  # noqa: E402
from measure_entities import Server  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
BASE = ROOT / "data" / "vanilla" / "1.20.1"
GENERATED = BASE / "generated" / "data" / "minecraft" / "loot_tables" / "entities"
PACK = BASE / "registry.ovpack"
ENTITY_PACK = BASE / "entity_loot.ovpack"
MOBLOOT = ROOT / "build" / "macos-debug" / "bin" / "ov_mobloot"
RUN = ROOT / "run" / "entity-loot-oracle"
PORT = 25613
BOT = "ovloot"

ITEM_RE = re.compile(r'id: "(minecraft:[a-z_0-9]+)", Count: (\d+)b')
DATA = re.compile(r"following entity data: (.*)$")

#: Les entités que `/loot kill` ne peut pas tirer, et pourquoi.
#:
#: Refusées et nommées plutôt que comptées comme conformes : une table qu'on
#: n'a pas confrontée n'est pas une table conforme.
SKIP = {
    "minecraft:ender_dragon": "ne peut pas être invoquée sans détruire le monde de test",
    "minecraft:wither": "casse le décor même avec mobGriefing false",
    "minecraft:player": "n'est pas invocable",
    "minecraft:giant": "n'a pas de table non vide",
    # `entities/sheep` n'est jamais tirée telle quelle : un mouton a toujours une
    # couleur et `Sheep.getDefaultLootTable()` rend `entities/sheep/<couleur>`,
    # qui délègue à celle-ci pour le mouton. La confronter directement compare
    # une table à un contexte qui n'existe pas — vanilla répond avec de la laine
    # blanche en plus, parce qu'il a tiré la table blanche.
    "minecraft:sheep": "jamais tirée directement ; les seize tables de couleur y délèguent",
}

#: Le NBT qui met une entité dans l'état que sa table interroge.
EXTRA_NBT = {
    "minecraft:slime": "Size:0",
    "minecraft:magma_cube": "Size:0",
}

#: Et l'état correspondant côté nous.
SLIME_SIZE = {"minecraft:slime": 1, "minecraft:magma_cube": 1}

SHEEP_COLOURS = ["white", "orange", "magenta", "light_blue", "yellow", "lime", "pink",
                 "gray", "light_gray", "cyan", "purple", "blue", "brown", "green",
                 "red", "black"]


def tables() -> list[tuple[str, str, str]]:
    """(nom de table, entité à invoquer, NBT supplémentaire)."""
    out: list[tuple[str, str, str]] = []
    for path in sorted(GENERATED.glob("*.json")):
        name = f"minecraft:{path.stem}"
        if name in SKIP:
            continue
        out.append((name, name, EXTRA_NBT.get(name, "")))
    for index, colour in enumerate(SHEEP_COLOURS):
        out.append((f"minecraft:sheep/{colour}", "minecraft:sheep", f"Color:{index}b"))
    return out


def within_noise(a: int, b: int) -> bool:
    """Deux comptes tirés au sort peuvent différer sans se contredire.

    Trois écarts-types binomiaux, plus une tolérance relative pour les tables
    qui multiplient un compte aléatoire par un autre — la variance du produit
    est bien plus large que la racine du total.
    """
    if abs(a - b) <= 3.0 * ((max(a, b) + 1) ** 0.5) + 1.0:
        return True
    return min(a, b) > 200 and abs(a - b) <= 0.10 * max(a, b)


def main() -> int:
    out_path = Path(sys.argv[1]) if len(sys.argv) > 1 else BASE / "normalized" / "entity_loot.json"
    draws = int(sys.argv[2]) if len(sys.argv) > 2 else 256

    plan = tables()
    print(f"{len(plan)} tables, {draws} tirages chacune", flush=True)

    ours_input = "\n".join(
        f"{name} 0 0 0 {SLIME_SIZE.get(entity, 0)} - {draws}" for name, entity, _ in plan)
    ours_raw = subprocess.run([str(MOBLOOT), str(PACK), str(ENTITY_PACK)],
                              input=ours_input, capture_output=True, text=True,
                              check=True).stdout.splitlines()
    if len(ours_raw) != len(plan):
        print(f"ov_mobloot a rendu {len(ours_raw)} lignes pour {len(plan)} cas")
        return 1

    server = Server(RUN, port=PORT)
    results: dict[str, dict] = {}
    agree = 0
    probe: Probe | None = None
    try:
        server.batch(["gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                      "gamerule sendCommandFeedback true", "gamerule mobGriefing false",
                      "difficulty normal", "time set noon",
                      "forceload add -16 -16 16 16",
                      "kill @e[type=!minecraft:player]"])
        # `/loot give` a besoin d'une cible qui tienne l'inventaire, et un
        # serveur sans joueur connecté lit zéro partout — ce qui ressemble
        # exactement à une table vide. D'où le bot, connecté et immobile. Il
        # répond aussi aux keep-alives : une campagne d'une demi-heure sans
        # cela se termine par une déconnexion sans explication.
        probe = Probe(PORT, name=BOT)
        probe.pump(3.0)
        server.batch([f"gamemode creative {BOT}", f"tp {BOT} 0.5 -60.0 0.5"])
        probe.pump(1.0)
        for index, (name, entity, extra) in enumerate(plan):
            nbt = ('{NoAI:1b,Silent:1b,PersistenceRequired:1b,NoGravity:1b,Tags:["ovtarget"]'
                   + (("," + extra) if extra else "") + "}")
            vanilla: dict[str, int] = {}
            ok = True
            for start in range(0, draws, 24):
                batch = min(24, draws - start)
                commands = ["kill @e[tag=ovtarget]", f"clear {BOT}",
                            f"summon {entity} 0.5 -60.0 4.5 {nbt}"]
                commands += [f"loot give {BOT} kill @e[tag=ovtarget,limit=1]"] * batch
                commands.append(f"data get entity {BOT} Inventory")
                lines = server.batch(commands)
                raw = None
                for line in lines:
                    match = DATA.search(line)
                    if match:
                        raw = match.group(1)
                if raw is None:
                    if any("No items" in line or "no items" in line for line in lines):
                        raw = ""
                    else:
                        ok = False
                        break
                for item, amount in ITEM_RE.findall(raw):
                    vanilla[item] = vanilla.get(item, 0) + int(amount)

            mine: dict[str, int] = {}
            gaps = None
            for token in ours_raw[index].split()[1:]:
                if token.startswith("!gaps:"):
                    gaps = token[6:]
                    continue
                item, total = token.rsplit(":", 1)
                mine[item] = int(total)

            same = ok and gaps is None and all(
                within_noise(mine.get(k, 0), vanilla.get(k, 0))
                for k in set(mine) | set(vanilla))
            agree += bool(same)
            results[name] = {"ours": mine, "vanilla": vanilla, "ok": bool(same),
                             "gaps": gaps, "drawn": ok}
            probe.pump(0.05)
            if (index + 1) % 10 == 0:
                print(f"  {index + 1}/{len(plan)}  {agree} conformes", flush=True)
    finally:
        if probe is not None:
            try:
                probe.socket.close()
            except OSError:
                pass
        server.stop()

    # Un désaccord se rejoue plus longtemps avant d'être retenu. Une table qui
    # multiplie un nombre de tirages aléatoire par un compte aléatoire — la
    # sorcière tire un à trois fois dans un sac de sept objets, chacun compté
    # zéro à deux — a une variance bien plus large que la racine du total, et
    # deux cent cinquante-six tirages la laissent passer pour une erreur.
    retries = [name for name, value in results.items()
               if not value["ok"] and value["gaps"] is None and value["drawn"]]
    if retries:
        print(f"{len(retries)} désaccords rejoués sur {draws * 8} tirages", flush=True)
        deep = draws * 8
        by_name = {name: (entity, extra) for name, entity, extra in plan}
        deep_input = "\n".join(
            f"{name} 0 0 0 {SLIME_SIZE.get(by_name[name][0], 0)} - {deep}" for name in retries)
        deep_raw = subprocess.run([str(MOBLOOT), str(PACK), str(ENTITY_PACK)],
                                  input=deep_input, capture_output=True, text=True,
                                  check=True).stdout.splitlines()
        server = Server(RUN, port=PORT + 1)
        probe = None
        try:
            probe = Probe(PORT + 1, name=BOT)
            probe.pump(3.0)
            server.batch(["gamerule doMobSpawning false", "gamerule mobGriefing false",
                          "gamerule sendCommandFeedback true", "difficulty normal",
                          "forceload add -16 -16 16 16", "kill @e[type=!minecraft:player]",
                          f"gamemode creative {BOT}", f"tp {BOT} 0.5 -60.0 0.5"])
            probe.pump(1.0)
            for name, line in zip(retries, deep_raw):
                entity, extra = by_name[name]
                nbt = ('{NoAI:1b,Silent:1b,PersistenceRequired:1b,NoGravity:1b,'
                       'Tags:["ovtarget"]' + (("," + extra) if extra else "") + "}")
                vanilla = {}
                for start in range(0, deep, 24):
                    batch = min(24, deep - start)
                    commands = ["kill @e[tag=ovtarget]", f"clear {BOT}",
                                f"summon {entity} 0.5 -60.0 4.5 {nbt}"]
                    commands += [f"loot give {BOT} kill @e[tag=ovtarget,limit=1]"] * batch
                    commands.append(f"data get entity {BOT} Inventory")
                    raw = ""
                    for text in server.batch(commands):
                        match = DATA.search(text)
                        if match:
                            raw = match.group(1)
                    for item, amount in ITEM_RE.findall(raw):
                        vanilla[item] = vanilla.get(item, 0) + int(amount)
                    probe.pump(0.05)
                mine = {}
                for token in line.split()[1:]:
                    item, total = token.rsplit(":", 1)
                    mine[item] = int(total)
                same = all(within_noise(mine.get(k, 0), vanilla.get(k, 0))
                           for k in set(mine) | set(vanilla))
                agree += bool(same)
                results[name] = {"ours": mine, "vanilla": vanilla, "ok": bool(same),
                                 "gaps": None, "drawn": True, "draws": deep}
        finally:
            if probe is not None:
                try:
                    probe.socket.close()
                except OSError:
                    pass
            server.stop()

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps({"tables": len(results), "agree": agree, "draws": draws,
                                    "skipped": SKIP, "results": results}, indent=1))
    print(f"{agree}/{len(results)} tables conformes ({draws} tirages)")
    for key, value in sorted(results.items()):
        if not value["ok"]:
            print(f"  écart {key}: nous {value['ours']} vanilla {value['vanilla']}"
                  f"{' gaps ' + value['gaps'] if value['gaps'] else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
