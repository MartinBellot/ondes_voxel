# Provenance

Ondes VOXEL est une réimplémentation **clean-room** : chaque système est
spécifié depuis de la **documentation**, jamais depuis le code source d'une
autre implémentation.

Ce fichier trace, pour chaque système non trivial, la source documentaire
utilisée. Il n'est pas une formalité : c'est ce qui permet de répondre à la
question « d'où vient ce comportement ? » sans avoir à s'en souvenir, et ce qui
rend vérifiable l'affirmation que le projet ne contient pas de code dérivé.

Toute entrée doit être ajoutée **au moment** où le système est implémenté.

---

## Sources de référence

### Protocole réseau

⚠️ **wiki.vg a fermé le 30 novembre 2024.** Son contenu (CC BY-SA) a été fusionné
dans le wiki officiel. Deux pièges en découlent :

1. `https://minecraft.wiki/w/Java_Edition_protocol` documente la **version
   courante du jeu**, pas la 1.20.1. S'y référer produit du code pour le mauvais
   protocole, et l'erreur ne se voit qu'à la connexion.
2. Il n'existe pas d'URL officielle dédiée au protocole 763 ; il faut passer par
   une révision figée.

| Sujet | URL | Statut |
|---|---|---|
| **Protocole 763 (archive figée)** | `https://minecraft.wiki/w/Minecraft_Wiki:Projects/wiki.vg_merge/Protocol?oldid=2773082` | ✅ **la référence à utiliser** |
| Numéros de version | `https://minecraft.wiki/w/Protocol_version` | ✅ confirme 763 = 1.20 / 1.20.1 |
| Historique du protocole | `https://minecraft.wiki/w/Minecraft_Wiki:Projects/wiki.vg_merge/Protocol_History` | ✅ |
| Types de données (VarInt, VarLong) | `https://minecraft.wiki/w/Java_Edition_protocol/Data_types` | ⚠️ version courante |
| Chiffrement et login | `https://minecraft.wiki/w/Java_Edition_protocol/Encryption` | AES/CFB8, secret 16 o, hash SHA-1 |
| Métadonnées d'entité | `https://minecraft.wiki/w/Java_Edition_protocol/Entity_metadata` | ⚠️ version courante |
| Format de chunk **réseau** | `https://minecraft.wiki/w/Java_Edition_protocol/Chunk_format` | ⚠️ **distinct du format disque** |
| Miroir statique de secours | `https://c4k3.github.io/wiki.vg/` | état pré-novembre 2024 |

### Format de sauvegarde

| Sujet | URL | Points clés vérifiés |
|---|---|---|
| Fichiers région | `https://minecraft.wiki/w/Region_file_format` | En-tête 8 KiB = 1024 offsets + 1024 timestamps. Chunk = longueur (4 o BE) + type de compression (1 o) + données. **1 = GZip, 2 = Zlib (défaut), 3 = aucune.** LZ4 (4) et custom (127) sont postérieurs à 1.20.1. |
| Format Anvil | `https://minecraft.wiki/w/Anvil_file_format` | Ordre YZX, biomes par section |
| Format de chunk | `https://minecraft.wiki/w/Chunk_format` | Depuis 1.18 : `sections[].block_states.{palette,data}` et `sections[].biomes.{palette,data}` (4×4×4, ≤ 64 entrées). **Pas** l'ancien `Level.Sections[].Palette`. |
| `level.dat` | `https://minecraft.wiki/w/Java_Edition_level_format` | NBT gzippé, racine → `Data`, écriture sûre via `level.dat_old` |
| NBT | `https://minecraft.wiki/w/NBT_format` | 13 types (ID 0-12), big-endian |
| NBT (spec historique) | `https://github.com/twoolie/NBT/wiki/Specification` | Spec originale de Notch. L'URL d'origine (`minecraft.net/docs/NBT.txt`) n'existe plus. |

### Données de jeu

| Source | Licence | Usage |
|---|---|---|
| Data generator officiel (`server.jar --all`) | données Mojang | Source de vérité des IDs de registre. **Généré localement, jamais commité.** |
| `PrismarineJS/minecraft-data` | **MIT** (vérifiée) | `protocol.json` → génération des codecs de paquets ; blocks, items, recipes, entities, biomes pour 1.20.1 |
| `misode/mcmeta` | **inconnue** — aucun LICENSE trouvé | ❌ **Écarté.** Traité comme « tous droits réservés » par défaut. |

---

## Implémentations tierces

Consultables **pour l'architecture**, jamais pour copier du code.

| Projet | Licence | Verdict |
|---|---|---|
| Cuberite (C++) | Apache-2.0 | ✅ Compatible, la référence C++ la plus proche |
| Valence (Rust) | MIT | ✅ Bon modèle de protocole et d'ECS |
| MCHPRS (Rust) | MIT | ✅ Référence redstone |
| Feather (Rust) | Apache-2.0 | ✅ Compatible, mais abandonné (~avril 2024) |
| Glowstone cœur (Java) | MIT | ✅ — ⚠️ **Glowkit est GPL**, ne pas y toucher |
| Luanti / Minetest (C++) | LGPL-2.1+ | ⚠️ Architecture seulement ; un copier-collé contamine le fichier |
| **Paper / Purpur / Spigot / Bukkit** | **GPLv3** | 🚫 **Ne pas lire.** Incompatible avec Apache-2.0. |

---

## Journal par système

Une ligne par système non trivial, ajoutée au moment de son implémentation.

| Système | Module | Source | Notes |
|---|---|---|---|
| Cadence de tick 20 Hz | `ov_base` | `https://minecraft.wiki/w/Tick` | 1 tick = 50 ms exactement. Le plafond de rattrapage et l'abandon du backlog reproduisent le comportement « Can't keep up » de vanilla. |
| Numérotation des faces | `ov_math` | archive protocole 763, paquet *Use Item On* | down=0, up=1, north=2, south=3, west=4, east=5. Valeurs de protocole, pas un choix interne. |
| Index de section YZX | `ov_math` | `Chunk_format` | `(y * 16 + z) * 16 + x`. Partagé par Anvil et le paquet de chunk. |
| Slot de région | `ov_math` | `Region_file_format` | Table de 1024 entrées indexée z-major : `(z % 32) * 32 + (x % 32)`. |
| Limites de construction | `ov_math` | `Chunk_format` | Overworld y ∈ [-64, 319] depuis 1.18, sections -4 à 19. Impose la division plancher. |
| Registres et blockstates | `ov_datagen` | data generator officiel, `server.jar` 1.20.1 (SHA-1 `84194a2f286ef7c14ed7ce0090dba59902951553`) | Voir les deux pièges ci-dessous. |

---

## Pièges mesurés dans les données officielles

Deux comportements du dataset 1.20.1 qui cassent silencieusement une
implémentation naïve. Tous deux vérifiés sur les données réelles, pas déduits.

### L'ordre des propriétés dans `blocks.json` n'est pas l'ordre des IDs

Les 24 135 blockstates sont bien un nombre en **base mixte** sur les propriétés
du bloc, poids fort en tête, avec des IDs contigus par bloc — c'est ce qui rend
l'accès aux propriétés arithmétique et O(1), et ce dont dépend la jouabilité de
la redstone.

Mais l'ordre des clés imprimé dans `blocks.json` **n'est pas** celui qu'utilise
cette arithmétique. Quatre blocs sur 1003 divergent :

| Bloc | Ordre dans le fichier | Ordre réel |
|---|---|---|
| `minecraft:chest` | `type, facing, waterlogged` | `facing, type, waterlogged` |
| `minecraft:trapped_chest` | `type, facing, waterlogged` | `facing, type, waterlogged` |
| `minecraft:moving_piston` | `type, facing` | `facing, type` |
| `minecraft:piston_head` | `type, facing, short` | `facing, short, type` |

Se fier à l'ordre du fichier donne des IDs faux **pour ces quatre blocs
seulement**. Rien ne le détecte avant qu'un client vanilla ne se connecte et que
quelques coffres ne deviennent autre chose — un symptôme qui coûte des semaines
à faire remonter jusqu'ici.

`tools/ov_datagen` ne lit donc jamais cet ordre : il le **déduit** des IDs
d'états et vérifie les 24 135 états contre la permutation trouvée.

### `minecraft:mob_effect` est 1-based

Tous les registres sont des plages denses, mais un seul ne commence pas à zéro :
`mob_effect` va de 1 (`speed`) à 33 (`darkness`), parce que l'ID 0 signifie
« aucun effet » dans les paquets qui en transportent un.

Un `protocol_id == index` générique décalerait **tous** les effets de statut d'un
cran. La sortie normalisée porte donc un `first_id` explicite par registre.

### Chiffres mesurés (et non estimés)

| Grandeur | Valeur réelle |
|---|---|
| Blocs | **1003** |
| Blockstates | **24 135** — IDs 0 à 24134, contigus, tiennent sur `u16` |
| `minecraft:air` | id **0** (permet à `memset(0)` de produire une section vide) |
| Bloc au plus d'états | `redstone_wire`, **1296** |
| Items | 1255 · Entités 124 · Fluides 5 · Block entities 41 |
| Sons 1474 · Particules 95 · Effets 33 · Enchantements **39** · Potions 43 |
| Blocs ayant `default` ≠ premier ID | 484 sur 1003 — le défaut est une donnée à part |
