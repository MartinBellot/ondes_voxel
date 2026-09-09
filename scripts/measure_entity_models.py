#!/usr/bin/env python3
"""La géométrie des entités : d'où elle vient, et la preuve qu'elle est la bonne.

Un modèle de bloc est un JSON du resource pack. Un modèle d'**entité** ne l'est
pas : en Java Edition il vit dans le code, et ce projet n'a pas le droit de le
lire. Il faut donc l'établir autrement, et le vérifier.

**D'où.** Mojang publie la géométrie vanilla comme *données* pour Bedrock
Edition, dans `Mojang/bedrock-samples`, sous la forme documentée
`resource_pack/models/entity/<espèce>.geo.json` : des `bones` avec un `pivot`,
des `cubes` avec `origin`, `size`, `uv`, `inflate` et `mirror`. Ces fichiers
sont sous EULA Minecraft — donc **exactement** la même catégorie que la sortie
du data generator : régénérés localement, jamais commités. La sortie de ce
script va dans `data/vanilla/1.20.1/entity_models.json`, que `.gitignore`
exclut.

**La preuve.** Rien ne garantit a priori que la géométrie Bedrock soit celle de
Java. Ce script la confronte à deux oracles indépendants qui, eux, sont locaux :

1. **Le patron de texture.** Chaque cube déplie un patron de
   `2·(w+d) × (h+d)` texels sur la texture *Java* du pack. Si un cube avait la
   mauvaise taille ou le mauvais `uv`, son patron tomberait à côté du dessin.
   On mesure donc, texel par texel : la part du patron qui est encrée
   (`couverture`), et la part du dessin de la texture qu'aucun patron ne
   recouvre (`orphelins`). Un modèle juste donne beaucoup de la première et
   très peu de la seconde.
2. **La boîte de collision mesurée.** `normalized/entities.json` porte la
   hitbox des 120 types, mesurée sur le vrai serveur. On compare la boîte du
   modèle au repos à celle-là. Elles ne sont **pas** censées être égales — un
   modèle déborde de sa hitbox chez Mojang aussi — mais un écart d'un facteur
   deux dit qu'on s'est trompé d'unité ou d'axe.

Usage :
    scripts/measure_entity_models.py [--fetch] [--geometry-dir DIR]
                                     [--pack run/assets]
                                     [--out data/vanilla/1.20.1/entity_models.json]

`--fetch` télécharge depuis bedrock-samples ; sans lui le script lit
`--geometry-dir`, ce qui permet de rejouer la mesure hors ligne.
"""

import argparse
import json
import os
import struct
import subprocess
import sys
import zlib

# Le commit de bedrock-samples auquel la géométrie a été prise. Épinglé : « la
# branche main » n'est pas une source, un commit en est une.
BEDROCK_COMMIT = "736072450c26a7c67f07b1661f29d9a5ebaa14b1"
BEDROCK_URL = ("https://raw.githubusercontent.com/Mojang/bedrock-samples/"
               "{commit}/resource_pack/models/entity/{name}.geo.json")

# Espèce -> (fichier bedrock, identifiant(s) de géométrie, texture Java, nom du
# modèle chez nous, décalage d'uv, os à ne garder que du dernier maillon).
#
# Les huit premières sont les huit que le serveur sait faire vivre
# (gameplay::mob_kinds) ; la neuvième est le joueur.
SPECIES = [
    ("zombie",   ["geometry.zombie.v1.8"],   "entity/zombie/zombie",       "zombie",
     (0, 0), False),
    ("skeleton", ["geometry.skeleton.v1.8"], "entity/skeleton/skeleton",   "skeleton",
     (0, 0), False),
    ("creeper",  ["geometry.creeper.v1.8"],  "entity/creeper/creeper",     "creeper",
     (0, 0), False),
    ("spider",   ["geometry.spider.v1.8"],   "entity/spider/spider",       "spider",
     (0, 0), False),
    ("cow",      ["geometry.cow.v1.8"],      "entity/cow/cow",             "cow",
     (0, 0), False),
    ("pig",      ["geometry.pig.v1.8"],      "entity/pig/pig",             "pig",
     (0, 0), False),
    # Le mouton est deux modèles rendus l'un sur l'autre, comme chez Java
    # (SheepModel puis SheepFurModel) : la peau, puis la laine, chacune avec sa
    # propre texture. `only_last` garde, pour la laine, les seuls cubes que la
    # géométrie dérivée ajoute — sans quoi la peau serait dessinée deux fois.
    ("sheep",    ["geometry.sheep.sheared.v1.8"], "entity/sheep/sheep",    "sheep",
     (0, 0), False),
    # ⚠ Le seul désaccord Bedrock/Java que l'oracle ait attrapé. La laine
    # Bedrock est découpée dans une feuille de 64×64 (uv 0,32 · 28,40 · 0,48) ;
    # `sheep_fur.png` de Java fait 64×32. Retirer 32 de v ramène exactement les
    # trois origines sur celles du mouton tondu (0,0 · 28,8 · 0,16), ce qui est
    # la disposition de Java — et la mesure le confirme, voir la colonne
    # « orphelins » avec et sans le décalage.
    ("sheep",    ["geometry.sheep.v1.8"],    "entity/sheep/sheep_fur",     "sheep_fur",
     (0, -32), True),
    # `geometry.chicken` et non `geometry.chicken.v1.12` : la seconde cuit le
    # quart de tour du corps dans le cube, la première le laisse à l'animation,
    # ce que fait Java. Notre format ne porte pas de rotation par cube, et c'est
    # voulu — un cube tourné serait refusé et nommé.
    ("chicken",  ["geometry.chicken"],       "entity/chicken",             "chicken",
     (0, 0), False),
    ("humanoid.custom", ["geometry.humanoid.custom"],
                                             "entity/player/wide/steve",   "humanoid",
     (0, 0), False),
]

# Les types d'entité que chaque modèle sert, pour la comparaison de hitbox.
HITBOX_OF = {
    "zombie": "minecraft:zombie",
    "skeleton": "minecraft:skeleton",
    "creeper": "minecraft:creeper",
    "spider": "minecraft:spider",
    "cow": "minecraft:cow",
    "pig": "minecraft:pig",
    "sheep": "minecraft:sheep",
    "sheep_fur": "minecraft:sheep",
    "chicken": "minecraft:chicken",
    # Le joueur est l'un des quatre types dont la hitbox n'est pas mesurée
    # (`absent` dans entities.json) : le serveur ne peut pas en invoquer un.
    # 0.6 × 1.8 est la valeur du protocole, pas une mesure de ce dépôt.
    "humanoid": None,
}


# ── PNG, décodé ici plutôt qu'importé ───────────────────────────────────────
def read_png(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, palette, trns = 8, b"", b"", b""
    width = height = depth = colour = 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
        elif kind == b"PLTE":
            palette = body
        elif kind == b"tRNS":
            trns = body
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    assert depth == 8, depth
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
    raw = zlib.decompress(idat)
    stride = width * channels
    out = bytearray(width * height * 4)
    prev = bytearray(stride)
    p = 0
    for y in range(height):
        filt = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if filt == 1:
                line[i] = (line[i] + a) & 0xFF
            elif filt == 2:
                line[i] = (line[i] + b) & 0xFF
            elif filt == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif filt == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        prev = line
        for x in range(width):
            i = x * channels
            o = (y * width + x) * 4
            if colour == 6:
                out[o:o + 4] = line[i:i + 4]
            elif colour == 2:
                out[o:o + 3] = line[i:i + 3]
                out[o + 3] = 255
            elif colour == 3:
                idx = line[i]
                out[o:o + 3] = palette[idx * 3:idx * 3 + 3]
                out[o + 3] = trns[idx] if idx < len(trns) else 255
            elif colour == 0:
                out[o] = out[o + 1] = out[o + 2] = line[i]
                out[o + 3] = 255
            else:
                out[o] = out[o + 1] = out[o + 2] = line[i]
                out[o + 3] = line[i + 1]
    return width, height, bytes(out)


# ── La géométrie Bedrock, lue dans ses deux formats ─────────────────────────
def geometries(document):
    """{identifiant: {"texture": (w, h), "bones": [...]}} pour les deux formats.

    Le format 1.8 met chaque géométrie à la racine sous sa clé ; le format 1.12
    et suivants les mettent dans un tableau `minecraft:geometry` avec une
    `description`. Les deux existent dans le même dossier chez Mojang.
    """
    found = {}

    def add(identifier, texture, bones):
        # `enfant:parent` dans la clé : Bedrock écrit l'héritage dans le nom
        # de la géométrie elle-même, pas dans un champ.
        parts = identifier.split(":")
        found[parts[0]] = {
            "texture": texture,
            "bones": bones,
            "inherits": parts[1] if len(parts) > 1 else None,
        }

    for key, value in document.items():
        if key.startswith("geometry.") and isinstance(value, dict):
            add(key, (value.get("texturewidth"), value.get("textureheight")),
                value.get("bones", []))
    for entry in document.get("minecraft:geometry", []):
        description = entry.get("description", {})
        add(description.get("identifier", "?"),
            (description.get("texture_width"), description.get("texture_height")),
            entry.get("bones", []))
    return found


def refuse(cube, bone, model):
    """Ce que ce format ne sait pas porter, nommé plutôt qu'ignoré."""
    reasons = []
    if "rotation" in cube:
        reasons.append(f"{model}/{bone}: rotation par cube")
    if isinstance(cube.get("uv"), dict):
        reasons.append(f"{model}/{bone}: uv par face")
    if "poly_mesh" in bone:
        reasons.append(f"{model}/{bone}: poly_mesh")
    return reasons


def convert(name, identifiers, geo_by_identifier, model_name, uv_shift, only_last):
    """Les os d'une géométrie et de ses parents, en ordre parent-avant-enfant."""
    bones = {}
    order = []
    refused = []
    declared_texture = (None, None)

    # La géométrie hérite des os de son parent, puis ajoute les siens. Un os
    # déjà présent voit ses cubes *ajoutés*, jamais remplacés — c'est ce qui
    # garde le corps sous la laine du mouton.
    chain = []
    for identifier in identifiers:
        link = identifier
        while link:
            if link not in geo_by_identifier:
                sys.exit(f"géométrie absente : {link} (fichier {name}.geo.json)")
            chain.insert(0, link)
            link = geo_by_identifier[link]["inherits"]

    for position, link in enumerate(chain):
        geo = geo_by_identifier[link]
        keep_cubes = (not only_last) or position == len(chain) - 1
        if geo["texture"][0]:
            declared_texture = geo["texture"]
        for bone in geo["bones"]:
            bone_name = bone["name"]
            cubes = []
            for cube in bone.get("cubes", []) if keep_cubes else []:
                refused.extend(refuse(cube, bone_name, model_name))
                if "rotation" in cube or isinstance(cube.get("uv"), dict):
                    continue
                cubes.append({
                    "origin": [float(v) for v in cube["origin"]],
                    "size": [float(v) for v in cube["size"]],
                    "uv": [float(cube["uv"][0]) + uv_shift[0],
                           float(cube["uv"][1]) + uv_shift[1]],
                    "inflate": float(cube.get("inflate", 0.0)),
                    "mirror": bool(cube.get("mirror", bone.get("mirror", False))),
                })
            if bone_name in bones:
                bones[bone_name]["cubes"].extend(cubes)
                if "pivot" in bone:
                    bones[bone_name]["pivot"] = [float(v) for v in bone["pivot"]]
            else:
                bones[bone_name] = {
                    "name": bone_name,
                    "parent": bone.get("parent", ""),
                    "pivot": [float(v) for v in bone.get("pivot", [0, 0, 0])],
                    "cubes": cubes,
                }
                order.append(bone_name)

    # Un os sans cube n'est qu'une charnière : il ne se dessine pas. C'est
    # exactement le critère qui laisse passer le calque « hat » — qui *a* des
    # cubes et que Java dessine — et écarte `waist`, `rightItem`, `cape`.
    for bone in bones.values():
        bone["render"] = len(bone["cubes"]) > 0

    # Tri topologique : parent avant enfant, sans quoi le loader C++ refuse.
    sorted_names, placed = [], set()
    remaining = list(order)
    while remaining:
        progressed = False
        for bone_name in list(remaining):
            parent = bones[bone_name]["parent"]
            if parent and parent not in placed:
                if parent not in bones:
                    # Un parent qui n'existe pas est une racine de fait.
                    bones[bone_name]["parent"] = ""
                else:
                    continue
            sorted_names.append(bone_name)
            placed.add(bone_name)
            remaining.remove(bone_name)
            progressed = True
        if not progressed:
            sys.exit(f"cycle dans la hiérarchie d'os de {model_name}")

    return [bones[n] for n in sorted_names], refused, declared_texture


# ── L'oracle : le patron contre les pixels ──────────────────────────────────
def net_rectangles(cube):
    """Les six rectangles du patron d'un cube, en texels de la texture logique."""
    u, v = cube["uv"]
    w, h, d = cube["size"]
    return [
        ("right",  u,             v + d, d, h),
        ("front",  u + d,         v + d, w, h),
        ("left",   u + d + w,     v + d, d, h),
        ("back",   u + d + w + d, v + d, w, h),
        ("top",    u + d,         v,     w, d),
        ("bottom", u + d + w,     v,     w, d),
    ]


def measure_coverage(model, texture, logical):
    """(couverture du patron, texels orphelins, hors-cadre)."""
    width, height, pixels = texture
    lw, lh = logical
    scale_x, scale_y = width / lw, height / lh

    covered = bytearray(width * height)
    net_texels = 0
    net_inked = 0
    outside = 0

    for bone in model["bones"]:
        if not bone["render"]:
            continue
        for cube in bone["cubes"]:
            for _, u, v, w, h in net_rectangles(cube):
                if w <= 0 or h <= 0:
                    continue
                x0, y0 = int(round(u * scale_x)), int(round(v * scale_y))
                x1, y1 = int(round((u + w) * scale_x)), int(round((v + h) * scale_y))
                if x0 < 0 or y0 < 0 or x1 > width or y1 > height:
                    outside += 1
                    continue
                for y in range(y0, y1):
                    for x in range(x0, x1):
                        net_texels += 1
                        if pixels[(y * width + x) * 4 + 3] > 0:
                            net_inked += 1
                        covered[y * width + x] = 1

    orphan = 0
    inked = 0
    for i in range(width * height):
        if pixels[i * 4 + 3] > 0:
            inked += 1
            if not covered[i]:
                orphan += 1

    coverage = net_inked / net_texels if net_texels else 0.0
    orphans = orphan / inked if inked else 0.0
    return coverage, orphans, outside, inked


def rest_bounds(model):
    lo = [1e9, 1e9, 1e9]
    hi = [-1e9, -1e9, -1e9]
    for bone in model["bones"]:
        if not bone["render"]:
            continue
        for cube in bone["cubes"]:
            f = cube["inflate"]
            for axis in range(3):
                lo[axis] = min(lo[axis], cube["origin"][axis] - f)
                hi[axis] = max(hi[axis], cube["origin"][axis] + cube["size"][axis] + f)
    return [v / 16.0 for v in lo], [v / 16.0 for v in hi]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fetch", action="store_true",
                        help="télécharger la géométrie depuis bedrock-samples")
    parser.add_argument("--geometry-dir", default="run/entity-geometry",
                        help="où les .geo.json sont (ou seront) écrits")
    parser.add_argument("--pack", default="run/assets")
    parser.add_argument("--entities",
                        default="data/vanilla/1.20.1/normalized/entities.json")
    parser.add_argument("--out", default="data/vanilla/1.20.1/entity_models.json")
    args = parser.parse_args()

    os.makedirs(args.geometry_dir, exist_ok=True)
    files = sorted({entry[0] for entry in SPECIES})

    if args.fetch:
        for name in files:
            url = BEDROCK_URL.format(commit=BEDROCK_COMMIT, name=name)
            path = os.path.join(args.geometry_dir, name + ".geo.json")
            subprocess.run(["curl", "-sSL", "-m", "30", "-o", path, url], check=True)
            print(f"  téléchargé {name}.geo.json ({os.path.getsize(path)} o)")

    documents = {}
    for name in files:
        path = os.path.join(args.geometry_dir, name + ".geo.json")
        if not os.path.exists(path):
            sys.exit(f"{path} absent — relancer avec --fetch")
        documents[name] = geometries(json.load(open(path)))

    hitboxes = {}
    if os.path.exists(args.entities):
        hitboxes = json.load(open(args.entities)).get("hitbox", {})

    models = {}
    refused_all = []
    rows = []

    for name, identifiers, texture_name, model_name, uv_shift, only_last in SPECIES:
        bones, refused, declared = convert(name, identifiers, documents[name],
                                           model_name, uv_shift, only_last)
        refused_all.extend(refused)

        texture_path = os.path.join(args.pack, "assets/minecraft/textures",
                                    texture_name + ".png")
        if not os.path.exists(texture_path):
            sys.exit(f"texture absente : {texture_path}")
        texture = read_png(open(texture_path, "rb").read())

        # La taille *logique* de la feuille vient de la texture Java, pas du
        # fichier Bedrock : le pack la livre en 2× et Bedrock se trompe d'échelle
        # pour le zombie. La largeur logique est celle que Bedrock déclare (64
        # partout) ; la hauteur suit le rapport d'aspect du fichier, ce qui est
        # indépendant de la résolution du pack.
        logical_w = float(declared[0] or 64)
        logical_h = round(texture[1] * logical_w / texture[0])
        note = ""
        if declared[1] and declared[1] != logical_h:
            note = f"bedrock dit {int(declared[1])}"

        model = {
            "texture_width": logical_w,
            "texture_height": float(logical_h),
            "bones": bones,
        }
        models[model_name] = model

        coverage, orphans, outside, inked = measure_coverage(
            model, texture, (logical_w, logical_h))
        lo, hi = rest_bounds(model)
        hitbox_name = HITBOX_OF.get(model_name)
        hitbox = hitboxes.get(hitbox_name) if hitbox_name else None

        rows.append((model_name, len(bones),
                     sum(len(b["cubes"]) for b in bones),
                     f"{int(logical_w)}x{logical_h}", note,
                     coverage, orphans, outside,
                     max(hi[0] - lo[0], hi[2] - lo[2]), hi[1] - lo[1],
                     hitbox["width"] if hitbox else None,
                     hitbox["height"] if hitbox else None))

    payload = {
        "format": 1,
        "source": (f"Mojang/bedrock-samples@{BEDROCK_COMMIT[:12]} "
                   "resource_pack/models/entity (EULA Minecraft, jamais commité)"),
        "refused": sorted(set(refused_all)),
        "models": models,
    }
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as handle:
        json.dump(payload, handle, indent=1, sort_keys=False)
    print(f"\nécrit {args.out} ({os.path.getsize(args.out)} o)\n")

    header = (f"{'modèle':10s} {'os':>3s} {'cubes':>5s} {'feuille':>8s} "
              f"{'couverture':>10s} {'orphelins':>9s} {'hors':>4s} "
              f"{'largeur':>16s} {'hauteur':>16s}")
    print(header)
    print("-" * len(header))
    for (model_name, bones, cubes, sheet, note, coverage, orphans, outside,
         model_w, model_h, box_w, box_h) in rows:
        width_cell = (f"{model_w:5.3f} / {box_w:5.3f}" if box_w
                      else f"{model_w:5.3f} /     ?")
        height_cell = (f"{model_h:5.3f} / {box_h:5.3f}" if box_h
                       else f"{model_h:5.3f} /     ?")
        print(f"{model_name:10s} {bones:3d} {cubes:5d} {sheet:>8s} "
              f"{coverage * 100:9.1f}% {orphans * 100:8.1f}% {outside:4d} "
              f"{width_cell:>16s} {height_cell:>16s}")
        if note:
            print(f"{'':10s} ⚠ hauteur de feuille : {note}, la texture Java dit {sheet}")

    if payload["refused"]:
        print("\nrefusé (nommé, jamais approximé) :")
        for reason in payload["refused"]:
            print(f"  {reason}")
    else:
        print("\nrefusé : rien")

    print("\nlargeur/hauteur : boîte du modèle au repos, en blocs, contre la "
          "hitbox mesurée.\nElles ne sont pas censées être égales — un modèle "
          "déborde de sa hitbox chez Mojang aussi.")


if __name__ == "__main__":
    main()
