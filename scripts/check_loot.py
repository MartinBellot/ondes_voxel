#!/usr/bin/env python3
"""Confronte nos tirages de butin à ceux du vrai serveur 1.20.1.

Les tables sont des données de vanilla, régénérées localement ; c'est
l'interpréteur qui est du code, et c'est lui qu'on vérifie. La commande `/loot`
du serveur tire exactement la même table sans qu'il faille casser le bloc, ce
qui en fait un oracle propre : pas de durée, pas de position, juste le tirage.

Chaque cas est tiré N fois des deux côtés. On compare l'**ensemble des objets**
obtenus — un désaccord là est une erreur de structure — puis les moyennes, avec
la tolérance qu'impose un tirage aléatoire.

Usage : python3 scripts/check_loot.py <fifo> <log> <sortie.json> [échantillons]
"""
import json
import re
import subprocess
import sys
import time

NORMALIZED = "data/vanilla/1.20.1/normalized"
PACK = "data/vanilla/1.20.1/registry.ovpack"
INSPECT = "build/macos-debug/bin/ov_inspect"

# Les configurations d'outil qui changent ce qui tombe. Silk Touch et Fortune
# sont l'objet même de l'exercice ; les cisailles décident de quatorze tables.
#
# Chacune n'est essayée que sur les tables qui la mentionnent : tirer Fortune
# sur neuf cents tables qui l'ignorent coûte une heure et ne prouve rien.
CASES = [
    ("nu", 0, 0, None, "-", None),
    ("silk", 1, 0, 'minecraft:diamond_pickaxe{Enchantments:[{id:"minecraft:silk_touch",lvl:1s}]}',
     "minecraft:diamond_pickaxe", "minecraft:silk_touch"),
    ("fortune3", 0, 3, 'minecraft:diamond_pickaxe{Enchantments:[{id:"minecraft:fortune",lvl:3s}]}',
     "minecraft:diamond_pickaxe", "minecraft:fortune"),
    ("cisailles", 0, 0, "minecraft:shears", "minecraft:shears", "minecraft:shears"),
]

LOOT_DIR = "data/vanilla/1.20.1/generated/data/minecraft/loot_tables/blocks"


def mentions(block: str, needle: str | None) -> bool:
    """Cette table parle-t-elle de cet enchantement ou de cet outil ?"""
    if needle is None:
        return True
    import os
    path = os.path.join(LOOT_DIR, block.split(":", 1)[1] + ".json")
    if not os.path.isfile(path):
        return False
    with open(path) as f:
        return needle in f.read()


def within_noise(a: int, b: int) -> bool:
    """Deux comptes tirés au sort peuvent différer sans se contredire.

    Trois écarts-types binomiaux pour les tables qui tirent une fois, plus une
    tolérance relative pour celles qui tirent deux fois. Un minerai sous
    Fortune multiplie un compte aléatoire par un multiplicateur aléatoire, et
    la variance du produit est bien plus large que la racine du total : sur
    deux cent cinquante-six tirages de cuivre, deux échantillons corrects
    diffèrent couramment de trois pour cent, ce que la borne en racine prend
    pour une erreur.
    """
    if abs(a - b) <= 3.0 * ((max(a, b) + 1) ** 0.5) + 1.0:
        return True
    return min(a, b) > 200 and abs(a - b) <= 0.10 * max(a, b)

ITEM_RE = re.compile(r'id: "(minecraft:[a-z_]+)", Count: (\d+)b')


def main() -> int:
    fifo_path, log_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    samples = int(sys.argv[4]) if len(sys.argv) > 4 else 16

    with open(f"{NORMALIZED}/registries.json") as f:
        blocks = json.load(f)["registries"]["minecraft:block"]["entries"]

    def run(*lines):
        with open(fifo_path, "w") as fifo:
            fifo.write("".join(line + "\n" for line in lines))

    name = "LootCheck"
    run(f"gamemode survival {name}", f"tp {name} 0.5 -60.0 0.5",
        "forceload add -8 -8 8 8")
    time.sleep(2.0)

    plan = []
    for label, silk, fortune, give, held, needle in CASES:
        for block in blocks:
            if mentions(block, needle):
                plan.append((label, block, silk, fortune, give, held))
    print(f"{len(plan)} cas à tirer, {samples} fois chacun", flush=True)

    ours_input = [f"{block} {silk} {fortune} {held} {samples}"
                  for _label, block, silk, fortune, _give, held in plan]

    ours_raw = subprocess.run([INSPECT, "loot", PACK], input="\n".join(ours_input),
                              capture_output=True, text=True, check=True).stdout.splitlines()

    def roll_vanilla(block, give, held, count):
        """Les tirages se lisent dans l'inventaire, qui ne tient que 35 piles.

        Un bloc généreux sous Fortune III le remplit en deux cents tirages, et
        le total s'arrête alors net sur 2240 — un chiffre parfaitement plausible
        et entièrement faux. D'où les paquets.
        """
        total = {}
        for start in range(0, count, 24):
            for item, amount in roll_vanilla_once(block, give, held,
                                                  min(24, count - start)).items():
                total[item] = total.get(item, 0) + amount
        return total

    def roll_vanilla_once(block, give, held, count):
        size = len(open(log_path, "rb").read())
        commands = [f"clear {name}", f"setblock 2 -60 0 {block} replace"]
        if give:
            commands.append(f"give {name} {give}")
        commands += [f"execute as {name} at {name} run loot give {name} mine 2 -60 0 mainhand"
                     ] * count
        commands.append(f"data get entity {name} Inventory")
        run(*commands)

        deadline = time.time() + 12.0
        text = ""
        while time.time() < deadline:
            time.sleep(0.05)
            text = open(log_path, errors="ignore").read()[size:]
            if "entity data" in text or "No items" in text:
                break
        found = {}
        for item, amount in ITEM_RE.findall(text):
            found[item] = found.get(item, 0) + int(amount)
        if give:
            # L'outil lui-même est dans l'inventaire ; il n'est pas tombé.
            found.pop(held, None)
        return found

    theirs = {}
    for index, (label, block, _silk, _fortune, give, held) in enumerate(plan):
        theirs[(label, block)] = roll_vanilla(block, give, held, samples)
        if (index + 1) % 100 == 0:
            print(f"  {index + 1}/{len(plan)}", flush=True)

    # Une ligne de sortie par ligne d'entrée, dans le même ordre.
    results = {}
    agree = 0
    retries: list[tuple[int, str, str]] = []
    for position, line in enumerate(ours_raw):
        label, block = plan[position][0], plan[position][1]
        parts = line.split()
        mine = {}
        for token in parts[1:]:
            item, total, _hits = token.rsplit(":", 2)
            mine[item] = int(total)
        vanilla = theirs.get((label, block), {})
        ok = all(within_noise(mine.get(k, 0), vanilla.get(k, 0))
                 for k in set(mine) | set(vanilla))
        if not ok:
            # Un désaccord se rejoue plus longtemps avant d'être retenu. Le
            # produit d'un compte tiré au sort et d'un multiplicateur tiré au
            # sort a une variance large, et trente-deux tirages la laissent
            # passer pour une erreur.
            retries.append((position, label, block))
        agree += ok
        results[f"{label}/{block}"] = {"ours": mine, "vanilla": vanilla, "ok": ok}

    if retries:
        print(f"{len(retries)} désaccords rejoués sur {samples * 8} tirages", flush=True)
        deep = samples * 32
        deep_input = []
        for position, _label, _block in retries:
            _l, block, silk, fortune, _g, held = plan[position]
            deep_input.append(f"{block} {silk} {fortune} {held} {deep}")
        deep_raw = subprocess.run([INSPECT, "loot", PACK], input="\n".join(deep_input),
                                  capture_output=True, text=True, check=True).stdout.splitlines()
        for (position, label, block), line in zip(retries, deep_raw):
            _l, _b, _s, _f, give, held = plan[position]
            mine = {}
            for token in line.split()[1:]:
                item, total, _hits = token.rsplit(":", 2)
                mine[item] = int(total)
            vanilla = roll_vanilla(block, give, held, deep)
            ok = all(within_noise(mine.get(k, 0), vanilla.get(k, 0))
                     for k in set(mine) | set(vanilla))
            agree += ok
            results[f"{label}/{block}"] = {"ours": mine, "vanilla": vanilla, "ok": ok,
                                           "samples": deep}

    with open(out_path, "w") as f:
        json.dump({"cases": len(results), "agree": agree, "samples": samples,
                   "results": results}, f, indent=1)
    print(f"{agree}/{len(results)} cas conformes")
    for key, value in sorted(results.items()):
        if not value["ok"]:
            print(f"  écart {key}: nous {value['ours']} vanilla {value['vanilla']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
