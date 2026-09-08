#!/usr/bin/env python3
"""Quelle table de butin chaque bloc utilise réellement.

Ce n'est pas toujours celle qui porte son nom : une torche murale, un panneau
mural, une tête murale renvoient à la table de leur variante posée au sol, et
`wall_torch.json` n'existe pas. Le nom du fichier est donc une supposition qui
tient pour neuf cents blocs et tombe pour cinquante — ce qui est la pire des
proportions, parce que ça marche assez pour ne pas se voir.

Le serveur, lui, le dit : la commande `/loot` journalise « from loot table
minecraft:blocks/X » avec le X qu'elle a vraiment employé.

Usage : python3 scripts/measure_loot_tables.py <fifo> <log> <sortie.json>
"""
import json
import re
import sys
import time

NORMALIZED = "data/vanilla/1.20.1/normalized"
TABLE_RE = re.compile(r"from loot table (minecraft:[a-z_/]+)")
MARK_RE = re.compile(r"MARK (minecraft:[a-z_]+)")


def main() -> int:
    fifo_path, log_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    with open(f"{NORMALIZED}/registries.json") as f:
        blocks = json.load(f)["registries"]["minecraft:block"]["entries"]

    def run(*lines):
        with open(fifo_path, "w") as fifo:
            fifo.write("".join(line + "\n" for line in lines))

    name = "TableMap"
    run(f"gamemode survival {name}", f"tp {name} 0.5 -60.0 0.5", "forceload add -8 -8 8 8")
    time.sleep(1.5)

    mapping = {}
    for start in range(0, len(blocks), 40):
        chunk = blocks[start:start + 40]
        size = len(open(log_path, "rb").read())
        commands = [f"clear {name}"]
        for block in chunk:
            # Un repère avant chaque tirage : la ligne de la table ne nomme pas
            # le bloc, seulement la table, et sans ancre on ne sait pas de qui
            # elle parle.
            commands += [f"setblock 2 -60 0 {block} replace", f"say MARK {block}",
                         f"loot give {name} mine 2 -60 0", f"clear {name}"]
        run(*commands)
        deadline = time.time() + 20.0
        text = ""
        while time.time() < deadline:
            time.sleep(0.1)
            text = open(log_path, errors="ignore").read()[size:]
            if text.count("MARK") >= len(chunk) and text.count("loot table") >= len(chunk):
                break
        current = None
        for line in text.splitlines():
            mark = MARK_RE.search(line)
            if mark:
                current = mark.group(1)
                continue
            table = TABLE_RE.search(line)
            if table and current is not None:
                mapping[current] = table.group(1)
                current = None
        print(f"  {len(mapping)}/{len(blocks)}", flush=True)

    named = sum(1 for b, t in mapping.items()
                if t == "minecraft:blocks/" + b.split(":", 1)[1])
    with open(out_path, "w") as f:
        json.dump({"$comment": "Mesure : la table que le serveur emploie pour chaque bloc, "
                               "relevee dans le journal de la commande /loot.",
                   "measured": len(mapping), "named_after_the_block": named,
                   "tables": mapping}, f, indent=1)
    print(f"{len(mapping)} blocs ; {named} utilisent la table qui porte leur nom")
    return 0


if __name__ == "__main__":
    sys.exit(main())
