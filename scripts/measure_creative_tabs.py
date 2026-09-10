#!/usr/bin/env python3
"""L'inventaire créatif : sa géométrie, lue dans les textures ; son contenu,
demandé au jeu lui-même.

Deux mesures, indépendantes l'une de l'autre.

**La géométrie** se lit dans les pixels, exactement comme
`scripts/measure_gui_sprites.py` lit ceux de `inventory.png` : on compte les
carrés 16×16 du gris d'emplacement dans `creative_inventory/tab_items.png`, et
on relève les bornes opaques de `creative_inventory/tabs.png` pour trouver le
pas des onglets et les poignées d'ascenseur. Une disposition écrite de mémoire
est *presque* juste, et « presque » veut dire un clic un emplacement à côté.

**Le contenu** n'a pas d'oracle réseau : en 1.20.1 les onglets créatifs sont
construits par `CreativeModeTabs`, et le client vanilla ne les reçoit jamais du
serveur. Mais cette classe est aussi dans le **jar serveur**. On peut donc la
faire tourner : `scripts/creative_tabs_oracle.java` amorce le jeu, appelle
`tryRebuildTabContents`, et imprime les piles dans l'ordre où le jeu les a
mises. C'est la même méthode que `tools/ov_datagen` — exécuter le jar, pas le
lire —, et les mappings officiels n'y servent qu'à *nommer* (CLAUDE.md § 1).

Rien de tout ça n'est commité : la sortie va dans
`data/vanilla/1.20.1/creative_tabs.json`, gitignoré comme le reste des données
dérivées de Mojang. Seul le **hash** est écrit ici, et `--check` le vérifie.

Usage:
    scripts/measure_creative_tabs.py                # géométrie + contenu
    scripts/measure_creative_tabs.py --geometry     # seulement les pixels
    scripts/measure_creative_tabs.py --contents     # seulement l'oracle
    scripts/measure_creative_tabs.py --check        # le hash de la sortie
"""

import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Le jar serveur 1.20.1, et ses mappings officiels. Le SHA-1 du jar est celui
# que `scripts/setup_vanilla.sh` vérifie ; celui des mappings vient du même
# manifeste de version que Mojang publie.
SERVER_SHA1 = "84194a2f286ef7c14ed7ce0090dba59902951553"
MAPPINGS_URL = ("https://piston-data.mojang.com/v1/objects/"
                "0b4dba049482496c507b2387a73a913230ebbd76/server.txt")
MAPPINGS_SHA1 = "0b4dba049482496c507b2387a73a913230ebbd76"

# Le hash de la sortie de l'oracle, avec `op_permissions = true`. C'est la
# seule chose de cette mesure qui entre dans le dépôt.
EXPECTED_SHA256 = "e53d702cacaec6f85995c22b1ba98000a916c841b0ac918b608f35d0302f80c4"

CACHE = os.path.join(ROOT, "data", "vanilla", "1.20.1", "generated", "creative")
OUTPUT = os.path.join(ROOT, "data", "vanilla", "1.20.1", "creative_tabs.json")


# ── La géométrie ────────────────────────────────────────────────────────────
#
# Le décodeur PNG et le détecteur d'emplacements de measure_gui_sprites.py sont
# réutilisés tels quels : deux lecteurs de PNG dans le même dépôt, c'est deux
# occasions de ne pas être d'accord.

def gui_sprites():
    spec = importlib.util.spec_from_file_location(
        "measure_gui_sprites", os.path.join(ROOT, "scripts", "measure_gui_sprites.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def opaque_runs(px, w, scale, y, width=256):
    """Les intervalles de x où la ligne y est opaque."""
    out, start = [], None
    for x in range(width):
        opaque = px[((y * scale) * w + x * scale) * 4 + 3] > 0
        if opaque and start is None:
            start = x
        elif not opaque and start is not None:
            out.append((start, x - 1))
            start = None
    if start is not None:
        out.append((start, width - 1))
    return out


def destroy_slot(px, w, scale, width=195, height=136):
    """Le carré 16×16 rose de l'emplacement de destruction.

    Même détecteur que les carrés gris, une autre couleur : le rouge y domine
    d'au moins vingt niveaux et le pixel reste clair.
    """
    def rose(x, y):
        o = ((y * scale) * w + x * scale) * 4
        r, g, b, a = px[o:o + 4]
        return a == 255 and r > g + 20 and r > b + 20 and r > 150

    for y in range(height - 16):
        for x in range(width - 16):
            if not rose(x, y):
                continue
            if (x and rose(x - 1, y)) or (y and rose(x, y - 1)):
                continue
            if all(rose(x + dx, y + dy) for dx in (0, 15) for dy in (0, 15)):
                return (x, y, 16, 16)
    return None


def report_geometry(pack, label, mgs):
    print("── %s ─────────────────────────────────────────────" % label)

    # 1. Les emplacements des trois fonds de page.
    #
    # Une page de catégorie montre 9×5 cases d'items plus la barre d'action ;
    # la page de recherche y ajoute le champ de saisie, dessiné dans le même
    # gris et donc compté comme un carré de plus — c'est nommé, pas corrigé.
    for name, expect, note in (
            ("tab_items.png", 54, "45 cases (9×5) + 9 de barre d'action"),
            ("tab_item_search.png", 55, "les 54 mêmes + le champ de saisie"),
            ("tab_inventory.png", 41, "l'inventaire de survie")):
        w, h, px = mgs.load(pack, "container/creative_inventory/" + name)
        scale = w // 256
        boxes = mgs.slot_boxes(px, w, scale, 195, 136)
        verdict = "✓" if len(boxes) == expect else "✗ attendu %d" % expect
        print("  %-22s %4dx%-4d échelle %dx : %2d carrés  %s  (%s)"
              % (name, w, h, scale, len(boxes), verdict, note))
        rows = {}
        for x, y in boxes:
            rows.setdefault(y, []).append(x)
        for y in sorted(rows):
            xs = sorted(rows[y])
            step = xs[1] - xs[0] if len(xs) > 1 else 0
            print("      y=%-3d %2d carrés  x=%s%s"
                  % (y, len(xs), xs[0], (" pas %d" % step) if step else ""))

        # L'emplacement de destruction n'est pas gris : il est rose. Le
        # détecteur de carrés gris ne peut pas le voir, alors on le cherche par
        # sa couleur — sinon on rendrait une page à laquelle il manque la
        # seule case dont l'effet est irréversible.
        if name == "tab_inventory.png":
            box = destroy_slot(px, w, scale)
            print("      emplacement de destruction (rose) : %s"
                  % ("(%d,%d) %dx%d" % box if box else "introuvable"))

    # 2. tabs.png : le pas des onglets, les quatre bandes, les poignées.
    w, h, px = mgs.load(pack, "container/creative_inventory/tabs.png")
    scale = w // 256
    print("  %-22s %4dx%-4d échelle %dx" % ("tabs.png", w, h, scale))

    # Chaque bande se termine par un biseau d'arrondi — en haut pour la rangée
    # supérieure, en bas pour l'inférieure. C'est la seule ligne où les sept
    # formes ne se touchent pas, donc la seule qui donne le pas.
    for band, y in (("haut, non sélectionné", 2), ("haut, sélectionné", 34),
                    ("bas, non sélectionné", 90), ("bas, sélectionné", 126)):
        runs = [r for r in opaque_runs(px, w, scale, y) if r[0] < 182]
        step = runs[1][0] - runs[0][0] if len(runs) > 1 else 0
        print("      y=%-3d %-22s %d formes, pas %d" % (y, band, len(runs), step))

    # La hauteur de chaque bande : les lignes où la colonne x=13 (le milieu du
    # premier onglet) est opaque.
    column = [px[((y * scale) * w + 13 * scale) * 4 + 3] > 0 for y in range(140)]
    spans, start = [], None
    for y, on in enumerate(column):
        if on and start is None:
            start = y
        elif not on and start is not None:
            spans.append((start, y - 1))
            start = None
    print("      colonne x=13 opaque sur : %s"
          % ", ".join("y=%d..%d (%d haut)" % (a, b, b - a + 1) for a, b in spans))

    # L'ascenseur : une bande opaque à droite, coupée en deux par le bord
    # sombre de la seconde poignée.
    handles = [r for r in opaque_runs(px, w, scale, 4) if r[0] >= 208]
    if handles:
        left, right = handles[0]
        print("      ascenseur : x=%d..%d, y=0..14 — deux poignées de %dx15 "
              "(activée puis grisée)" % (left, right, (right - left + 1) // 2))
    legend = [r for r in opaque_runs(px, w, scale, 0) if 182 <= r[0] < 208]
    print("      légende non dessinée : %s  (les mots « UNSEL » et « SEL »)"
          % ", ".join("x=%d..%d" % r for r in legend))


# ── Le contenu ──────────────────────────────────────────────────────────────

def sha1_of(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def find_server_jar():
    """Le jar serveur, dégroupé si besoin, avec ses bibliothèques."""
    bundled = os.path.join(ROOT, "tools", "vanilla", "server.jar")
    if not os.path.exists(bundled):
        raise SystemExit("tools/vanilla/server.jar manquant — voir scripts/setup_vanilla.sh")
    digest = sha1_of(bundled)
    if digest != SERVER_SHA1:
        print("  ⚠ server.jar a le SHA-1 %s, pas %s : ce n'est pas la 1.20.1 attendue"
              % (digest, SERVER_SHA1), file=sys.stderr)

    libs = os.path.join(CACHE, "libraries")
    if not os.path.isdir(libs):
        os.makedirs(libs, exist_ok=True)
        with zipfile.ZipFile(bundled) as z:
            for name in z.namelist():
                if name.endswith(".jar"):
                    target = os.path.join(libs, os.path.basename(name))
                    with z.open(name) as src, open(target, "wb") as dst:
                        shutil.copyfileobj(src, dst)
    jars = sorted(os.path.join(libs, f) for f in os.listdir(libs) if f.endswith(".jar"))
    if not jars:
        raise SystemExit("aucune bibliothèque extraite de server.jar")
    return jars


def fetch_mappings():
    """Les mappings officiels de Mojang. Téléchargés, jamais commités."""
    path = os.path.join(CACHE, "server-mappings.txt")
    if os.path.exists(path) and sha1_of(path) == MAPPINGS_SHA1:
        return path
    os.makedirs(CACHE, exist_ok=True)
    print("  téléchargement des mappings officiels 1.20.1…")
    try:
        with urllib.request.urlopen(MAPPINGS_URL, timeout=120) as r, open(path, "wb") as f:
            shutil.copyfileobj(r, f)
    except Exception as exc:  # noqa: BLE001 — la cause est réimprimée telle quelle
        # Derrière un proxy qui réémet les certificats, urllib refuse la chaîne
        # là où curl, qui lit le trousseau du système, passe. Le SHA-1 est
        # vérifié juste après dans les deux cas, donc rien n'est affaibli.
        print("    urllib : %s — nouvel essai avec curl" % exc)
        subprocess.run(["curl", "-sSfL", "-o", path, MAPPINGS_URL], check=True)
    got = sha1_of(path)
    if got != MAPPINGS_SHA1:
        raise SystemExit("mappings : SHA-1 %s au lieu de %s" % (got, MAPPINGS_SHA1))
    return path


def run_oracle(op_permissions=True):
    jars = find_server_jar()
    mappings = fetch_mappings()
    classes = os.path.join(CACHE, "classes")
    os.makedirs(classes, exist_ok=True)
    # javac insiste pour qu'une classe publique soit dans un fichier du même
    # nom, et le dépôt nomme ses scripts en snake_case : on recopie.
    staged = os.path.join(CACHE, "src", "ov")
    os.makedirs(staged, exist_ok=True)
    source = os.path.join(staged, "CreativeTabsOracle.java")
    shutil.copyfile(os.path.join(ROOT, "scripts", "creative_tabs_oracle.java"), source)
    subprocess.run(["javac", "-nowarn", "-d", classes, source], check=True)
    classpath = os.pathsep.join([classes] + jars)
    proc = subprocess.run(
        ["java", "-cp", classpath, "ov.CreativeTabsOracle", mappings,
         "true" if op_permissions else "false"],
        capture_output=True, text=True)
    if proc.returncode != 0:
        # Le jeu installe un appender log4j qui réémet stderr sur *stdout* : la
        # trace d'une exception sort donc du mauvais côté, et ne la montrer que
        # d'un seul rend l'échec muet.
        sys.stderr.write(proc.stderr)
        sys.stderr.write(proc.stdout[-8000:])
        raise SystemExit("l'oracle a échoué (code %d)" % proc.returncode)
    return json.loads(proc.stdout)


def stack_item(entry):
    return entry if isinstance(entry, str) else entry["item"]


def report_contents(data):
    print("── le contenu, tel que le jeu le construit ─────────────────────")
    print("  %d onglets, %d items au registre" % (len(data["tabs"]), len(data["items"])))
    print()
    print("  %-30s %-7s %3s %-10s %-26s %6s %6s"
          % ("onglet", "rangée", "col", "type", "icône", "cases", "uniq"))
    covered = set()
    total = 0
    for tab in data["tabs"]:
        names = [stack_item(e) for e in tab["display"]]
        if tab["type"] == "CATEGORY":
            covered |= set(names)
            total += len(names)
        print("  %-30s %-7s %3d %-10s %-26s %6d %6d"
              % (tab["id"], tab["row"], tab["column"], tab["type"], tab["icon"],
                 len(names), len(set(names))))
    print()
    print("  cases dans les onglets de catégorie : %d" % total)
    print("  items distincts qui y apparaissent  : %d" % len(covered))
    missing = [i for i in data["items"] if i not in covered]
    print("  items du registre dans aucun onglet : %d" % len(missing))
    for name in missing:
        print("      %s" % name)

    with_nbt = sum(1 for tab in data["tabs"] for e in tab["display"]
                   if isinstance(e, dict) and "nbt" in e)
    print("  cases portant un NBT                : %d" % with_nbt)


def write_output(data):
    """La forme que le client lit : un onglet, ses cases, rien d'autre."""
    out = {
        "version": data["version"],
        "op_permissions": data["op_permissions"],
        "source": "CreativeModeTabs, server.jar 1.20.1 " + SERVER_SHA1,
        "tabs": [
            {
                "id": tab["id"],
                "translation_key": tab["translation_key"],
                "row": tab["row"],
                "column": tab["column"],
                "type": tab["type"],
                "aligned_right": tab["aligned_right"],
                "icon": tab["icon"],
                # La page de recherche porte l'union : c'est déjà l'onglet
                # `minecraft:search`, dont le jeu remplit lui-même la liste.
                "display": [
                    e if isinstance(e, str)
                    else {k: v for k, v in e.items() if k != "snbt"}
                    for e in tab["display"]
                ],
            }
            for tab in data["tabs"]
        ],
    }
    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    body = json.dumps(out, separators=(",", ":"), sort_keys=False).encode()
    with open(OUTPUT, "wb") as f:
        f.write(body)
    return hashlib.sha256(body).hexdigest(), len(body)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pack", default=os.path.join(ROOT, "run", "assets"))
    parser.add_argument("--jar", default=None,
                        help="un client 1.20.1, pour comparer la feuille vanilla au pack")
    parser.add_argument("--geometry", action="store_true")
    parser.add_argument("--contents", action="store_true")
    parser.add_argument("--check", action="store_true",
                        help="régénère et compare le hash à EXPECTED_SHA256")
    parser.add_argument("--no-op", action="store_true",
                        help="sans les permissions d'opérateur")
    args = parser.parse_args()

    do_geometry = args.geometry or not (args.contents or args.check)
    do_contents = args.contents or args.check or not (args.geometry or args.check)

    if do_geometry:
        mgs = gui_sprites()
        packs = [(args.pack, mgs.DirectoryPack(args.pack))]
        if args.jar:
            packs.append((os.path.basename(args.jar), mgs.JarPack(args.jar)))
        for label, pack in packs:
            report_geometry(pack, label, mgs)
            print()

    if do_contents:
        data = run_oracle(op_permissions=not args.no_op)
        report_contents(data)
        digest, size = write_output(data)
        print()
        print("  écrit %s (%d octets)" % (os.path.relpath(OUTPUT, ROOT), size))
        print("  sha256 %s" % digest)
        if args.check:
            if digest == EXPECTED_SHA256:
                print("  ✓ conforme à EXPECTED_SHA256")
            else:
                print("  ✗ attendu %s" % EXPECTED_SHA256)
                return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
