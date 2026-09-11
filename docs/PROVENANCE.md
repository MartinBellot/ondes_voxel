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

## Index des dossiers de provenance

Ce fichier porte les sources et les décisions transverses. Chaque système livré
depuis a son propre dossier sous `docs/provenance/`, avec ses mesures, ses
chiffres et — c'est le plus utile — **ce qu'il n'a pas réussi à établir**.

| Dossier | Ce qu'il établit |
|---|---|
| [`banc-de-test.md`](provenance/banc-de-test.md) | Le monde de test persistant : 31 parcelles écrites depuis un catalogue, ouvertes par le vrai serveur 1.20.1 sans une erreur |
| **Terrain** | |
| [`etage-de-bruit.md`](provenance/etage-de-bruit.md) | L'étage de bruit, et pourquoi la glace n'est pas du terrain |
| [`amplitude-old-blended-noise.md`](provenance/amplitude-old-blended-noise.md) | L'amplitude disculpée par le Nether, et un critère précédent montré dégénéré |
| [`surface-rules.md`](provenance/surface-rules.md) | L'interpréteur des règles de surface : 4,07 % → 91,22 % de colonnes exactes |
| [`carvers.md`](provenance/carvers.md) | Grottes et ravins, masques bit-exacts sur 1200 chunks |
| [`ordre-des-etages.md`](provenance/ordre-des-etages.md) | L'ordre des étages, et trois harnais structurellement aveugles au changement |
| [`aquiferes.md`](provenance/aquiferes.md) | L'oracle de l'aquifère, le niveau de fluide mesuré, la barrière refusée |
| [`biomes-egalites.md`](provenance/biomes-egalites.md) | Le départage des égalités de climat : 100,000 % |
| [`features.md`](provenance/features.md) | L'ensemencement de la décoration, et pourquoi `WorldgenRandom` n'est pas la source qu'il enveloppe |
| [`pipeline-de-chunks.md`](provenance/pipeline-de-chunks.md) | La couche qui possède neuf chunks : 27,97 % des écritures franchissent une frontière |
| [`structures.md`](provenance/structures.md) | Le placement des 19 `structure_set`, exact au chunk près — et le mineshaft comme oracle pur des réducteurs |
| [`chunkmap.md`](provenance/chunkmap.md) | Tickets, pool de jobs, génération hors du thread de tick, déterminisme sous TSan |
| **Jeu** | |
| [`fluides.md`](provenance/fluides.md) | Écoulement, recherche du trou, mélanges : 3172/3172 positions |
| [`redstone.md`](provenance/redstone.md) | Le modèle de puissance, circuit par circuit, et 36 exceptions nommées à « cube plein » |
| [`survie.md`](provenance/survie.md) | Vie, faim, expérience — et les dégâts de chute qui sont un `ceil` |
| [`crafting-and-smelting.md`](provenance/crafting-and-smelting.md) | 1174 recettes, 2885 grilles, et trois tables de cuisson plutôt qu'un diviseur |
| [`mobs.md`](provenance/mobs.md) | L'A\* qui prend la même route que le jeu dans le même labyrinthe |
| [`combat.md`](provenance/combat.md) | Frapper, utiliser, manger, user : la jauge d'attaque, 34 armes, 91 tables de butin |
| [`verbes.md`](provenance/verbes.md) | Les verbes branchés au serveur, et le levier enfin tiré |
| [`explosions.md`](provenance/explosions.md) | La résistance au souffle des 987 blocs, mesurée — et le plafond au-delà duquel aucune explosion ne peut plus distinguer deux valeurs |
| [`performance-tick.md`](provenance/performance-tick.md) | Pourquoi casser un bloc prenait 5 s : la lumière sur le thread réseau, des chunks générés sur le tick, et la part de la machine saturée |
| [`enchantement.md`](provenance/enchantement.md) | La table à 512/512 offres, l'enclume à 93/93, et l'archive du protocole qui se trompait sur la graine |
| [`ecrans.md`](provenance/ecrans.md) | Le menu principal, les mondes par graine, la pause et `options.txt` relu à l'octet |
| [`meteo-sommeil.md`](provenance/meteo-sommeil.md) | La pluie qui vient seule, la glace des océans gelés à 11 285/11 285, la foudre et les lits |
| [`feu.md`](provenance/feu.md) | Le feu qui se propage, la lave qui allume deux fois par tirage, et une fenêtre de dégâts de 10 ticks et non 11 |
| [`alchimie.md`](provenance/alchimie.md) | L'alambic et les 43 potions : 2709/2709 recettes, et l'éclaboussure qui part d'où était la fiole |
| [`end.md`](provenance/end.md) | L'End à 100 % des biomes et 99,9996 % des blocs, son portail, et un dragon qui ne vole pas encore |
| [`rendu-parite.md`](provenance/rendu-parite.md) | Le rendu mesuré contre le vrai client : 12 800/12 800 texels de lightmap, l'AO du jeu, et un sRGB caché qui délavait l'herbe |
| [`villageois.md`](provenance/villageois.md) | Métiers, commerce et niveaux : 4298/4298 offres vanilla retrouvées, et l'ordre d'un `HashSet` Java qui se voit à l'écran |
| [`mobs-2.md`](provenance/mobs-2.md) | La loi de marche, l'apparition par biome, douze espèces, et les monstres qui n'apparaissaient jamais en surface la nuit |
| [`nether.md`](provenance/nether.md) | Le Nether à 99,9 %, des portails là où le jeu les met, et le bruit de l'Overworld qui lisait ses octaves à l'envers |
| [`nether-2.md`](provenance/nether-2.md) | Les features du Nether, ses fossiles à 185 départs sur 185, et ses mobs dans un monde à eux : troc, salves, boules de feu |
| [`son.md`](provenance/son.md) | Les sons de 1003 blocs et 79 créatures, relevés sur le fil, et le client qui les joue |
| [`elevage.md`](provenance/elevage.md) | Veaux, agneaux et poussins : reproduction à 59-62 ticks, couleurs héritées, et une vitesse de marche qui n'est pas l'attribut divisé par deux |
| [`chat-client.md`](provenance/chat-client.md) | Le chat de notre client, mesuré sur le vrai client : un fondu de 10 s et non de 3, et des coupures de ligne identiques |
| [`projectiles.md`](provenance/projectiles.md) | Flèches, tridents et lancers : un vol ajusté à 1,3·10⁻¹⁴, et l'ordre des opérations qui expliquait le résidu des flèches |
| [`commandes.md`](provenance/commandes.md) | Le chat et 34 commandes : 269/271 réponses identiques au vrai serveur, octet pour octet |
| [`donnees-joueur.md`](provenance/donnees-joueur.md) | Le fichier joueur de vanilla, relu et réécrit sans perdre une feuille — et ce qui n'était pas sauvé du tout |
| [`agriculture.md`](provenance/agriculture.md) | Le random tick et ce qui pousse dessus : 676/676 cellules humides, 288/288 feuilles, et des forêts entières qui seraient tombées |
| [`effets.md`](provenance/effets.md) | Les 33 effets et leurs modificateurs : 522/522 intervalles, et l'ordre des seaux de hachage de Java |
| [`tnt-et-gravite.md`](provenance/tnt-et-gravite.md) | La TNT qui saute, le creeper qui siffle, le sable qui tombe — et le paquet Explosion qui n'est pas là où l'archive le met |
| [`branchement.md`](provenance/branchement.md) | Le câblage au tick, et les deux bugs qu'il a révélés |
| **Client** | |
| [`interface.md`](provenance/interface.md) | Police, HUD, inventaire : 2414 glyphes validés glyphe par glyphe |
| [`inventaire-creatif.md`](provenance/inventaire-creatif.md) | Les 14 onglets demandés au jar serveur, 1689 cases, et la géométrie comptée dans les pixels |
| [`rendu-entites.md`](provenance/rendu-entites.md) | Modèles d'entités, leur source, et le chest refusé faute de source permise |

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
| Collision AABB par axe | `ov_math` | `https://minecraft.wiki/w/Entity` + archive protocole 763 | Résolution Y puis X puis Z. Tester les trois d'un coup empêche de glisser le long d'un mur et de monter une marche. |
| VarInt / VarLong | `ov_protocol` | archive protocole 763, *Data types* | ≤ 5 et ≤ 10 octets. Un négatif occupe **toujours** la taille maximale (complément à deux). |
| Chaînes du protocole | `ov_protocol` | archive protocole 763, *Data types* | Préfixe VarInt en **octets** ; la limite déclarée est en unités UTF-16, donc la borne en octets vaut 3×. |
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

### Les fichiers région ne sont pas complétés jusqu'à la frontière de secteur

Un fichier `.mca` alloue à chaque chunk un nombre entier de secteurs de 4096
octets, indiqué dans l'en-tête. La lecture naïve de la documentation conduit à
valider `offset + sector_count ≤ taille_fichier / 4096` — et cette validation
**rejette des mondes parfaitement valides**.

Mesuré sur une vraie sauvegarde : `r.0.0.mca` fait **1 819 235 octets**, soit
444,15 secteurs, alors que son dernier chunk est alloué jusqu'au secteur **445**.
Le fichier s'arrête au milieu du secteur alloué. La charge utile, elle, est
intacte.

La règle correcte : le `sector_count` est une **allocation**, pas une promesse
de contenu. Ce qu'il faut valider, c'est que la **longueur déclarée** du chunk
tient dans ce qui existe réellement — `min(alloué, disponible)`.

Verrouillé par le test « a file not padded to a sector boundary is still
readable » dans `src/ov_nbt/tests/test_region.cpp`.

### Preuve de correction sur données réelles

`ov-inspect` décode, ré-encode et compare octet à octet. Exécuté sur une vraie
sauvegarde Minecraft (2026-09-07) :

| Fichier | Résultat |
|---|---|
| `level.dat` (gzip, 4257 tags, profondeur 11) | ✅ octet-identique |
| `servers.dat` (NBT non compressé) | ✅ octet-identique |
| `mfix_stronghold_cache_v2.nbt` (gzip) | ✅ octet-identique |
| **14 fichiers région, 2900 chunks, 795 277 tags** | ✅ **2900/2900 octet-identiques** |
| `minecraft-1.20.1-client.jar` (ZIP, 23 Mo) | ✅ **20 953/20 953 entrées extraites**, 41 206 169 octets, en 1,2 s |

Toutes les régions utilisaient le schéma **zlib (2)**, conformément à la
documentation. Reproductible avec :
```bash
ov-inspect nbt    <monde>/level.dat --verify
ov-inspect region <monde>/region/r.0.0.mca --verify
ov-inspect zip    <prism>/libraries/com/mojang/minecraft/1.20.1/minecraft-1.20.1-client.jar --verify
```

### Le parseur NBT ne peut pas être récursif

La limite de profondeur de 512 reprend celle de vanilla, et elle existe pour
empêcher un fichier hostile de faire déborder la pile. Elle ne suffisait pas :
un parseur **récursif** à 508 niveaux déborde la pile de **1 Mo** que Windows
donne à un thread — exactement le crash que la limite devait prévenir, à une
profondeur qu'elle autorisait encore. Découvert par le job CI Windows ; macOS et
Linux, avec 8 Mo de pile, passaient.

Baisser la limite aurait signifié rejeter des fichiers que vanilla accepte. Le
parseur utilise donc une **pile explicite sur le tas** : la profondeur ne coûte
plus rien à la pile d'appel, et la limite reste celle de vanilla.

Vérifié avec `ulimit -s 1024` : profondeur 508 acceptée et ré-encodée à
l'identique, profondeur 5000 rejetée par `nesting too deep`.

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

### Contenu du jar client 1.20.1

Mesuré, et non supposé — c'est ce que `ov-assetimport` doit aller chercher, un
resource pack ne contenant que des textures.

| Sous-arbre | Entrées |
|---|---|
| `assets/minecraft/blockstates/` | **1005** |
| `assets/minecraft/models/block/` | **2016** |
| `assets/minecraft/models/item/` | **1675** |
| `assets/minecraft/textures/block/` | 977 |
| `assets/minecraft/font/` | 7 |
| `assets/minecraft/lang/` | 1 (`en_us.json` ; les autres langues sont dans l'index d'assets) |
| `data/minecraft/` | **5887** — le jar client embarque aussi le datapack vanilla |
| **Total** | 20 953 entrées, toutes deflatées, 41 Mo décompressés |

---

## Palette bit-packée d'une section (`ov_world`)

Source : `Chunk_format` sur l'archive figée du wiki (§ 2 de `CLAUDE.md`).

### La règle qu'on croit connaître et qu'on écrit quand même de travers

Depuis 1.16, **une entrée ne chevauche jamais deux `long`**. À 5 bits, un `long`
en contient 12 et gaspille 4 bits — pas 12,8 entrées. Lire l'index se fait donc
par `index / entrées_par_long` et `index % entrées_par_long`, jamais par une
division du numéro de bit.

L'erreur est silencieuse dans les deux sens : un encodeur qui fait chevaucher les
entrées se relit parfaitement lui-même. Un aller-retour à travers notre propre
code passe donc même avec la règle inversée, et ne prouve rien.

### Ce qui le prouve

`ov-inspect chunk <region.mca>` dépaquette chaque entrée d'une section écrite par
le jeu, la repaquette depuis zéro, et compare les `long` obtenus à ceux du
disque. L'entrée vient de Mojang ; seul le codec est le nôtre.

| Mesure | Valeur |
|---|---|
| Sections traversant réellement le bit-packing | **165 318** — toutes octet-identiques |
| Largeurs couvertes | 4 bits : 73 363 · 5 : 85 656 · 6 : 6 221 · 7 : 78 |
| Entrées dépaquetées puis repaquetées | ≈ 677 millions |
| Sections single-valued (aucune donnée stockée) | 263 778 |

Deux réserves, posées honnêtement :

1. Le monde testé est en **DataVersion 3955 (1.21)**, pas 1.20.1. La règle de
   packing est inchangée depuis 1.16, donc la mesure est valide pour ce qu'elle
   couvre — mais elle ne dit **rien** sur les IDs de blocs, que la commande ne
   résout volontairement pas. La parité des IDs est vérifiée séparément par
   `check_registry_parity.py`.
2. Les sections *single-valued* sont comptées comme conformes sans repaquetage :
   il n'y a par définition aucun `long` à comparer. Le chiffre qui porte une
   affirmation est **165 318**, pas le total.

### Deux détails que vanilla impose et qu'aucune formule ne rattrape

- La largeur est **plancherée à 4 bits**. Une palette de deux entrées s'encode en
  4 bits, pas en 1 — l'encodage plus serré serait plus petit et illisible par le
  client.
- Une section uniforme n'écrit **aucune donnée du tout** : palette d'un élément,
  `data` absent. C'est le cas majoritaire (263 778 sections sur 429 096 ici), et
  celui qu'une implémentation oublie parce qu'il ne ressemble pas aux autres.

---

## `ResourceLocation` (`ov_base`)

Source : page *Resource location* du wiki, jeux de caractères vérifiés ensuite
contre les **1003 noms de blocs** produits par le data generator.

| Règle | Détail |
|---|---|
| Namespace | `[a-z0-9_.-]` — pas de `/`, pas de majuscule |
| Chemin | `[a-z0-9_.-/]` |
| Namespace absent | vaut `minecraft` |
| Deux-points en tête | `:stone` est `minecraft:stone`, **pas** une erreur |
| Deuxième deux-points | caractère de chemin invalide, pas un second séparateur |

Les jeux de caractères sont écrits en dur plutôt que dérivés de `std::isalnum`,
qui dépend de la locale : un identifiant devant correspondre octet pour octet à
celui de Mojang ne peut pas dépendre de la configuration de la machine.

### Le piège : la validité n'est pas une garantie de chemin de fichier

`.` et `/` sont **tous deux** des caractères de chemin légaux. Donc
`minecraft:../../etc/passwd` est un identifiant parfaitement conforme — et
vanilla l'accepte également. Un test le fixe **comme cas passant**, précisément
pour que personne n'en déduise l'inverse : avoir parsé un identifiant
n'autorise pas à le concaténer à un répertoire. Tout ce qui résout une
`ResourceLocation` vers un fichier doit valider le chemin séparément
(`io::is_safe_archive_path`).

C'est le même piège que Zip Slip, à un étage au-dessus.

### Écart assumé au plan

Le plan rangeait ce type dans `ov_registry` (couche 5). Il vit dans `ov_base`
(couche 0) : le chargeur de datapack appartient à `ov_data` (couche 4), donc
**sous** `ov_registry`, et manipule ces identifiants en permanence. Le graphe ne
remonte jamais ; le type n'ayant aucune dépendance, le descendre ne coûte rien.

---

## `LegacyRandomSource` (`ov_math`)

Source : la **javadoc du JDK** pour `java.util.Random`, qui spécifie l'algorithme
et ses constantes mot pour mot. C'est ce qui rend ce générateur vérifiable
contre n'importe quelle JVM, sur n'importe quelle machine.

`scripts/gen_random_vectors.java` produit les vecteurs de référence depuis une
vraie JVM et les imprime **en C++**, pour qu'aucune transcription humaine ne
s'intercale entre l'oracle et le test. Les flottants passent en bits bruts :
« assez proche » est exactement le mode de défaillance qu'on cherche à exclure.

| Tirage | Résultat contre une vraie JVM |
|---|---|
| `nextInt`, `nextLong`, `nextBoolean` | **bit-identiques** |
| `nextInt(bound)` — puissance de deux et non | **bit-identiques** |
| `nextFloat`, `nextDouble` | **bit-identiques** |
| `nextGaussian` | **44/56 bit-identiques, 4 ulp au pire** |

### L'écart, mesuré et assumé

`nextGaussian` appelle `log`. Java impose `StrictMath.log`, c'est-à-dire les
sémantiques **fdlibm** ; la libm de la plateforme est libre de diverger sur les
derniers bits, et celle de macOS diverge.

L'écart est borné dans un test qui **épingle la mesure au lieu d'affirmer un
succès**, précisément pour que personne n'en déduise plus tard la garantie plus
forte. Ce qui est concerné : la dispersion de vitesse des objets lâchés et
assimilés, où 4 ulp sont invisibles. Ce qui ne l'est **pas** : terrain, butin et
block ticks, qui ne reposent que sur des tirages bit-exacts.

Combler l'écart demande une implémentation de `log` fdlibm — inscrite à la
roadmap. Tant qu'elle n'est pas là, l'affirmation honnête est celle du tableau.

### Trois détails que la spécification impose

- Le **XOR de brouillage** au seed (`^ 0x5DEECE66D`) n'est pas une optimisation :
  sans lui, `Random(0)` et `Random(1)` produisent des flux visiblement liés.
- `nextInt(bound)` a **deux chemins**. Une borne puissance de deux prend les bits
  **hauts** d'un tirage 31 bits ; les autres rejettent la queue inégale. Un
  simple modulo biaise vers le bas — assez peu pour ressembler à du bruit, assez
  pour décaler chaque veine de minerai du monde.
- `nextDouble` tire **26 bits puis 27**, dans cet ordre. Tirer 32 et 32 consomme
  autant d'état et donne un résultat faux.

---

## `XoroshiroRandomSource` (`ov_math`)

Algorithmes publiés : **Xoroshiro128++** (Blackman & Vigna) et le finaliseur
**variante 13 de Stafford** utilisé par SplitMix64. Le tirage borné suit la
méthode de **Lemire**.

### L'oracle est meilleur que prévu

Le JDK 17 embarque `Xoroshiro128PlusPlus` comme algorithme standard. J'avais
supposé qu'il brouillait la graine avec le **ratio d'or** ; les vecteurs ont dit
non — c'est le **ratio d'argent** `0x6A09E667F3BCC909`, c'est-à-dire exactement
ce que fait le jeu.

Conséquence : `RandomGeneratorFactory.of("Xoroshiro128PlusPlus")` valide non pas
seulement la fonction de transition, mais **tout le chemin**, upgrade de graine
64 → 128 bits compris. Une graine simple entre, le flux complet est comparé.
6 graines × 8 tirages, tous identiques.

C'est un cas où une hypothèse fausse a rendu la vérification plus forte : je
cherchais un oracle pour le cœur seulement, la mesure en a livré un pour
l'ensemble.

### Ce qui diffère du générateur legacy, et pourquoi ça compte

| | `LegacyRandomSource` | `XoroshiroRandomSource` |
|---|---|---|
| Borné | rejet par modulo, deux chemins | Lemire, multiplication haute |
| `nextFloat` | 24 bits d'un tirage 24 bits | 24 bits **hauts** d'un tirage 64 bits |
| `nextDouble` | 26 bits puis 27, deux tirages | 53 bits hauts, **un seul** tirage |

Les deux consomment des quantités d'état différentes. Réutiliser une méthode de
l'un dans l'autre ne produit aucune erreur — seulement un monde différent.

### L'état interdit

Xoroshiro possède un état absorbant : tout à zéro, dont il ne ressort jamais.
C'est l'upgrade de graine qui éloigne la graine `0` de cet état, et une graine
`0` est trop courante pour que ce soit théorique. Un test le vérifie.

---

## Les 66 registres codés en dur (`ov_registry`)

Source : `generated/reports/registries.json`, produit par le data generator
officiel. Rien n'est écrit à la main — une liste de 5067 IDs maintenue
manuellement serait fausse avant la fin de la version.

C'est le risque **R1** du plan. Le client vanilla 1.20.1 connaît ces IDs avant
de se connecter et ne les reçoit **jamais**. Une seule entrée dans le mauvais
ordre et il affiche la mauvaise entité, sans qu'aucune erreur n'apparaisse
nulle part.

| Mesure | Valeur |
|---|---|
| Registres | **66** |
| IDs vérifiés entrée par entrée, ordre compris | **5067** |
| Plus gros | `sound_event` 1474 · `item` 1255 · `block` 1003 |
| Registres à `first_id ≠ 0` | **1** — `minecraft:mob_effect`, 1-based |

`scripts/check_registry_parity.py` relit le `.ovpack` **depuis la description du
format**, pas depuis l'émetteur, puis compare au rapport. Un émetteur et un
lecteur qui partagent un bug se valideraient mutuellement ; un tiers qui relit
la spec ne le peut pas.

### Ce que le rapport contient exactement

Mesuré : `registries.json` liste **uniquement** les 66 registres codés en dur.
Les six registres dynamiques — `dimension_type`, `worldgen/biome`, `chat_type`,
`damage_type`, `trim_material`, `trim_pattern` — n'y figurent pas du tout ; ils
vivent dans le datapack, et leurs IDs sont envoyés au client en NBT pendant le
login, donc ils sont **les nôtres**.

Le script les exclut malgré tout. L'exclusion ne se déclenche jamais
aujourd'hui, et c'est écrit tel quel dans le code : c'est un garde-fou. Si une
version future les faisait apparaître dans le rapport, les épingler casserait
le premier datapack ajoutant un biome — et cela doit échouer là plutôt qu'en jeu.

### `mob_effect` est stocké, pas traité en cas particulier

`first_id` est un champ du format. Coder en dur « si le nom est mob_effect,
commencer à 1 » marcherait aujourd'hui et deviendrait faux silencieusement le
jour où Mojang ajoute une seconde exception. Un décalage de un ici signifie que
chaque potion du jeu applique l'effet voisin.

---

## Tags (`ov_registry`)

Source : les 413 fichiers `data/minecraft/tags/**/*.json` du datapack vanilla,
produits par le data generator.

Un tag référence d'autres tags avec un `#` en tête, donc c'est un **graphe**.
Il est aplati **une fois, à la compilation**, en listes d'IDs triées — le jeu ne
parcourt jamais le graphe. Résoudre à chaque appel serait l'implémentation
évidente et mettrait une traversée de graphe dans le chemin de cassage de bloc.
Aplatir tôt fait aussi échouer un cycle ou une référence pendante **au build**
plutôt qu'à un tick.

| Mesure | Valeur |
|---|---|
| Fichiers de tags | **413** |
| Tags résolus | **305**, sur 10 registres |
| Membres | **4255** |
| Fichiers reportés (registres dynamiques) | **108** |
| Plus gros | `minecraft:mineable/pickaxe`, 375 membres |

### Le piège du découpage de chemin

Le répertoire du registre **et** le nom du tag peuvent contenir des `/` :

```
tags/banner_pattern/pattern_item/x.json  → registre banner_pattern, tag pattern_item/x
tags/worldgen/biome/y.json               → registre worldgen/biome,  tag y
tags/blocks/mineable/axe.json            → registre block,           tag mineable/axe
```

Découper au premier `/` se trompe sur le deuxième cas ; découper à l'avant-dernier
se trompe sur les deux autres. La bonne règle : l'ensemble des registres est
connu, donc on prend **le plus long préfixe qui nomme un registre**. Ma première
version découpait naïvement et a échoué sur `banner_pattern/pattern_item` — le
message d'erreur du normaliseur l'a montré immédiatement, ce qui est exactement
ce qu'on attend d'un `die()` explicite plutôt que d'un `get()` silencieux.

### Répertoires au pluriel

Cinq répertoires sont des pluriels hérités et ne correspondent pas au nom du
registre : `blocks` → `block`, `items` → `item`, `entity_types` → `entity_type`,
`fluids` → `fluid`, `game_events` → `game_event`. Tous les autres sont déjà au
singulier. La table est **mesurée depuis la sortie du générateur**, pas devinée.

### Ce que le datapack vanilla n'exerce pas

Mesuré : aucun `replace: true`, aucune entrée sous forme d'objet, aucune entrée
`required: false`, aucune référence hors du namespace `minecraft`. Ces cas sont
implémentés quand même — le format les définit, et le premier datapack tiers
s'en servira. Ne coder que ce que Mojang expédie donnerait un chargeur qui
marche jusqu'au premier pack de la communauté.

### Vérification indépendante

`check_registry_parity.py` **re-résout les `#` depuis les fichiers JSON bruts
avec sa propre implémentation**, puis compare au binaire. Un émetteur et un
lecteur partageant un résolveur s'accordent quoi qu'il fasse ; seule une seconde
traversée des mêmes sources peut dire que l'aplatissement est juste.
305 tags, 4255 membres, identiques.

---

## Section de chunk et stockage de la lumière (`ov_world`)

### L'ordre des indices

**YZX** — `(y * 16 + z) * 16 + x` — parce que c'est ce qu'utilisent le format de
fichier *et* le format réseau. XZY fonctionnerait parfaitement jusqu'au premier
chunk écrit, et produirait ensuite un monde qui est sa propre transposition :
chaque structure miroitée le long d'une diagonale, sans qu'aucune erreur
n'apparaisse.

### Les tableaux de lumière nullables

Mesuré sur un chunk réel : **7 sections sur 24** portaient un tableau
`BlockLight`, **2 sur 24** un `SkyLight`. Au-dessus du terrain le ciel est
uniformément plein et la lumière de bloc uniformément nulle ; sous la roche les
deux sont nulles. Un tableau entièrement uniforme ne stocke donc **rien** et se
souvient de la valeur ; il ne se matérialise que si quelque chose varie.

Le piège de la matérialisation : allouer un tampon rempli de zéros pour un
tableau uniformément **plein** plonge la section dans le noir, et seulement les
sections qu'on édite. On remplit avec la valeur implicite, pas avec zéro.

### L'ordre des demi-octets, tranché par la mesure

2048 octets portent 4096 cellules, et le format dit dans quelle moitié d'octet
vit la cellule paire. Le lire à l'envers échange chaque paire de voisins le long
de x. **Aucun aller-retour ne le détecte** — les octets ressortent identiques —
et le résultat est un monde éclairé en fin damier, qui ressemble à un bug de
shader et non de stockage.

Un éclairage réel varie doucement, donc la bonne lecture est celle qui donne le
plus petit écart entre cellules voisines. Mesuré par `ov-inspect chunk` sur les
tableaux que le jeu a lui-même écrits :

| Lecture | Écart moyen entre voisins | Gagne sur |
|---|---|---|
| cellule paire = demi-octet **bas** | **0,149** | **141 799 / 144 974** (97,8 %) |
| cellule paire = demi-octet haut | 0,230 | 3 175 |

Le contrôle est permanent dans `ov-inspect chunk`, pas une mesure jetable.

### Le compte de blocs non-air

C'est le champ « Block Count » du paquet de chunk, envoyé à chaque expédition.
Il est maintenu **incrémentalement** : recompter 4096 entrées par section à
chaque envoi dominerait le paquet. Un test compare le compte incrémental à un
recomptage complet, parce qu'un compteur incrémental est précisément le genre
qui dérive.

Les **trois** blocs d'air comptent comme air : `air` (état 0), `void_air`
(12817) et `cave_air` (12818), un seul état chacun — mesuré, pas supposé. Un
client à qui l'on dit qu'une grotte est pleine la rend pleine. Les identifiants
sont résolus depuis le registre, jamais écrits en dur.

---

## Heightmaps (`ov_world`)

Sémantique établie par **recalcul depuis les blocs**, pas par lecture de wiki.
`ov-inspect chunk` reconstruit `WORLD_SURFACE` à partir des palettes — **par nom**,
donc indépendamment de la version d'IDs du monde — et compare à ce que le jeu a
écrit.

| Mesure | Valeur |
|---|---|
| Colonnes recalculées | **4 577 024** |
| Identiques | **4 577 023** (99,99998 %) |
| À un près | **0** |

Zéro écart d'un bloc, ce qui est le point : la question était de savoir si la
valeur stockée est le sommet ou le premier espace libre au-dessus. C'est le
**premier espace libre** : `stored == top + 1 − minY`. Une colonne vide stocke 0.
L'autre convention fait tomber la pluie un bloc sous le sol.

### L'origine n'est pas la liste des sections

Deux chunks de ce monde listent **25 sections à partir de -5**, pas 24 à partir
de -4 : vanilla écrit une section supplémentaire sous le monde pour la lumière.
Déduire l'origine de la liste des sections décale **toutes** les colonnes de ces
chunks de seize blocs. L'origine est le plancher de la **dimension**, point.

C'est exactement le genre de fait qu'aucune lecture de spécification ne donne et
qu'une mesure livre en une passe.

### Le packing

La règle des palettes, encore : **9 bits par entrée, sept par long, aucune
entrée à cheval**. 37 longs pour 256 colonnes, pas 36. Neuf bits parce qu'un
monde de 384 blocs stocke les valeurs 0 à 384 — **385** possibilités, pas 384.
Dimensionner pour 384 rend le sommet du monde irreprésentable, ce qui reste
invisible jusqu'à ce que quelqu'un y construise.

### L'unique écart, expliqué et non arrondi

Une colonne sur 4,5 millions diverge de 3 blocs. Le bloc en cause est
`create:crushing_wheel_controller` — un bloc **moddé**. C'est le mod qui décide
si son bloc compte comme air ; une vérification par nom sur la liste vanilla ne
peut pas le savoir. Ce n'est pas un défaut de l'implémentation, c'est la limite
connue de la méthode de contrôle, et elle est écrite ici plutôt qu'arrondie.

---

## Le conteneur `Chunk` (`ov_world`)

### Décalage arithmétique, pas division

`section_index = (y >> 4) - min_section`. Une division tronque vers zéro, ce qui
placerait `y = -1` et `y = 0` dans la **même** section pendant que `y = -16` et
`y = -17` se retrouveraient séparés — une couture de seize blocs à l'origine du
monde. La majeure partie de l'Overworld est à `y` négatif, donc ce n'est pas un
cas limite.

### La forme du monde est un paramètre

L'Overworld fait 384 blocs depuis -64 ; le Nether et l'End font 256 depuis 0 ;
une dimension de datapack fait ce qu'elle veut. Coder -64 en dur fonctionne
jusqu'au premier chunk du Nether.

### `WORLD_SURFACE` maintenu, et vérifié comme tel

Poser un bloc, c'est une comparaison. **Casser** celui qui était la surface,
c'est un balayage vers le bas. C'est le sens coûteux, et celui qu'une
implémentation oublie : le heightmap continue alors de pointer un bloc qui
n'existe plus, et la pluie tombe sur du vide.

Vérification sur données réelles, via `ov-inspect chunk` : les blocs d'un chunk
du jeu sont **rejoués un `set_block` à la fois** dans un vrai `Chunk`, puis le
heightmap maintenu est comparé au balayage complet des mêmes blocs.

| Mesure | Valeur |
|---|---|
| Colonnes rejouées | **4 577 024** |
| Maintenu == balayé | **4 577 024** (100 %) |

Deux comparaisons distinctes, à ne pas confondre : celle-ci oppose le chemin
incrémental au balayage complet ; la ligne `WORLD_SURFACE` d'`ov-inspect` oppose
le balayage au fichier.

### Ce qui est bloqué, et pourquoi

`MOTION_BLOCKING`, `OCEAN_FLOOR`, le moteur de lumière et les collisions ont tous
besoin d'une **table de flags par état de bloc** — `blocksMotion`, émission
lumineuse, opacité, formes de collision. Les rapports officiels ne la portent
pas : en vanilla ce sont du code Java, pas des données.

**Piste écartée : `PrismarineJS/minecraft-data`.** Licence MIT, explicitement
autorisée par le `CLAUDE.md`, mais son `blocks.json` porte `emitLight` **par
bloc**, alors que l'émission dépend de l'**état** — mesuré :
`respawn_anchor` a 5 niveaux selon `charges`, `candle` 4 selon `candles`,
`cave_vines` selon `berries`, `redstone_ore` selon `lit`. L'utiliser coulerait
une approximation dans les fondations du moteur de lumière, exactement le genre
d'erreur qui donne un monde qui a l'air juste et ne l'est pas.

La source reste **à décider**. Elle doit être par état.

---

## L'état Play et le paquet de chunk (`ov_protocol`)

### Méthode : capturer, pas se souvenir

Les IDs de paquets ne se devinent pas et changent d'une version à l'autre. Trois
sources ont été confrontées :

| Source | Verdict |
|---|---|
| Page archivée du wiki, via résumé automatique | **fausse sur toute la ligne** — « Login (play) 0x01 », champs manquants |
| `PrismarineJS/minecraft-data` (MIT) | correcte |
| **Capture d'un vrai serveur 1.20.1** | correcte, et arbitre |

Le vrai `server.jar` a été lancé localement et un client écrit depuis la spec
s'y est connecté pour enregistrer ce qui arrive réellement. C'est ce qui a établi
`Login (play) = 0x28`, `Chunk Data = 0x24`, `Set Center Chunk = 0x4E`,
`Synchronize Position = 0x3C`.

Ça a aussi tranché un doute que j'avais : `portalCooldown` **existe bien** en 763.
Je soupçonnais un champ appartenant à 1.20.2 ; décoder la queue du vrai paquet a
montré l'octet. Mon doute était infondé, et mesuré plutôt que débattu.

### Comparaison directe avec vanilla

Les deux serveurs tournant côte à côte, un même décodeur a lu les deux. Les
charges de section sont **identiques** : mêmes largeurs de bits, mêmes tailles de
palette, mêmes nombres de longs, mêmes octets aux mêmes offsets.

Reste **non élucidé** : la charge de vanilla fait 23 octets de plus que la nôtre
et sa queue contient des sections à biome 0. Le client accepte la nôtre, donc ce
n'est pas bloquant, mais c'est écrit ici plutôt qu'arrondi.

### Trois pièges que seule la comparaison a révélés

**Les biomes n'ont pas la plage de bits des blocs.** Une palette de biomes fait
1 à 3 bits ; **4 signifie direct**. Notre `PalettedContainer` plancherait à 4
comme pour les blocs, et le client aurait lu des indices de palette comme des IDs
de registre — chaque biome du chunk devenu un autre. Corrigé : la plage fait
partie de l'identité du conteneur.

**« Vide » veut dire tout à zéro, pas « non stocké ».** Une section uniformément
*éclairée* doit quand même être écrite ; seule une section uniformément noire
peut être omise. L'inverse rend le monde entièrement noir, sans erreur nulle part.

**La lumière se pose par bloc, pas par section.** La surface d'un superflat est à
y = -61, dans la section allant de -64 à -49. Une règle « section entièrement
au-dessus de la surface » laisse noire précisément la section où le joueur se
tient : le ciel est éclairé, le sol non. Symptôme observé en jeu, puis corrigé.

### Le codec de registres

Six registres ne sont pas codés en dur dans le client et lui sont envoyés en NBT
au login. `tools/ov_datagen/codec.py` les construit depuis le datapack vanilla
local avec un **schéma typé champ par champ** : JSON n'a qu'un type numérique,
NBT en a six, et le décodeur du client est strict. `temperature` est un `float`,
`coordinate_scale` un `double`, chaque booléen un `byte`. Une conversion qui
déduit le type de la valeur produit un codec plausible, mal typé, et coupe la
connexion sans message utile.

Omettre un champ est sûr — les codecs du client ignorent ce qu'ils ne connaissent
pas. En envoyer un du mauvais type ne l'est pas.

Le serveur ne comprend jamais ce blob : il le recopie. Et il y **relit** l'id de
`minecraft:plains` plutôt que de le coder en dur, parce que le client n'apprend
les IDs de biomes que de ce codec-là.

### Casser et poser

`Player Action` (0x1D) et `Use Item On` (0x31), avec `Block Update` (0x0A) et
`Acknowledge Block Change` (0x06) en retour.

Trois détails qui décident du résultat :

- **Deux statuts de cassage comptent**, pas un. `0` est « commencé à creuser »,
  ce qui en créatif signifie que le bloc a déjà disparu côté client ; `2` est
  « fini de creuser » en survie. N'en traiter qu'un rend l'autre mode inopérant.
- **La position cliquée n'est pas celle où le bloc va.** La face indique le côté,
  et le nouveau bloc atterrit un pas plus loin. Poser à la position cliquée
  remplace ce que le joueur visait.
- **L'accusé de réception n'est pas optionnel.** Le client prédit le changement
  et l'affiche ; sans `Acknowledge`, il attend puis annule sa prédiction — ce qui
  ressemble exactement à un serveur qui ignore le joueur.

Le pont entre l'item tenu et le bloc posé est le **nom** : l'item
`minecraft:stone` pose le bloc `minecraft:stone`. Items et blocs sont deux
registres aux IDs distincts, et la plupart des items non-blocs n'ont simplement
pas de bloc homonyme — ce qui est exactement le test.

### La lumière se dérive du heightmap, pas d'une constante

Bug réel, trouvé en jouant : creuser un trou, se déconnecter, revenir — le trou
restait noir.

La génération cuisait la lumière du ciel à partir d'une hauteur de surface
**fixe**. Casser un bloc met à jour `WORLD_SURFACE`, mais pas le tableau de
lumière déjà stocké. Pendant la session tout allait bien parce que **le client
éclaire lui-même les blocs qu'il modifie** ; l'erreur ne se voit qu'au
rechargement, quand le serveur réenvoie sa version.

Deux conséquences de conception :

- La lumière est calculée **depuis le heightmap**, par une fonction unique
  qu'utilisent la génération *et* l'édition. Deux chemins qui calculent la
  lumière séparément s'accordent jusqu'au jour où quelqu'un creuse.
- La colonne est recalculée à chaque modification de bloc, parce que c'est
  précisément là que `WORLD_SURFACE` bouge.

Limites assumées : lumière **directe** seulement, sans propagation horizontale —
un puits est correctement éclairé, le dessous d'un surplomb ne l'est pas. Et
« non-air » tient lieu d'« opaque », ce qui est juste pour tout bloc d'un monde
plat et faux pour le verre. Les deux disparaissent avec le moteur de lumière et
la table de flags.

### La position du joueur, et son corollaire

Retenir la position ne suffit pas : réapparaître à mille blocs de l'origine avec
des chunks centrés sur le spawn fait tomber le joueur dans un monde vide. Les
deux vont ensemble — **la position restaurée détermine le centre de l'envoi**.

Elle est enregistrée à **chaque** mise à jour de mouvement, pas à la déconnexion :
un client tué n'envoie jamais de fermeture propre, et perdre la position d'une
session qui a planté est précisément le cas qu'on remarque.

En mémoire seulement. Un redémarrage du serveur l'oublie, exactement comme il
oublie les chunks — et n'en persister qu'une des deux replacerait quelqu'un à
l'intérieur d'un bloc qui n'existe plus.

### Le flood fill, et la graine qui manquait

La lumière directe seule laisse une **tache noire à bord net** sous le moindre
toit d'un bloc — mesuré : 0 sous le bloc, 15 dans la colonne voisine au même y.
C'est la première chose qu'un joueur remarque.

Le flood fill corrige ça : la lumière se propage dans les six directions en
perdant un niveau par pas. Sous un toit 3×3 : **15** dehors, **14** au pourtour,
**13** au centre.

La première version donnait **13 au lieu de 14** juste sous un bloc isolé. Cause :
je n'ensemençais que la cellule la plus basse de chaque colonne, en supposant que
tout au-dessus est déjà entouré de lumière. C'est faux exactement là où deux
colonnes ont des hauteurs différentes — c'est-à-dire précisément là où une
construction projette son ombre. Toutes les cellules directement éclairées de la
bande sont désormais des sources.

Coût mesuré : connexion complète (289 chunks générés, éclairés et envoyés) en
**0,75 s en build debug**.

Deux limites, écrites plutôt que sous-entendues : la propagation **s'arrête au
bord du chunk**, donc une construction à cheval sur une frontière ne projette pas
d'ombre chez le voisin ; et « non-air » tient lieu d'« opaque », donc du verre
ferait de l'ombre.

### Streaming de chunks, et le tick qu'il bloquait

Envoyer seulement la **différence** : un pas d'un chunk échange 17 chunks, un
saut de cinq en diagonale 157, et le total détenu reste à 289 centré sur le
joueur. Réémettre le carré entier à chaque franchissement ferait 289 chunks à
quelques pas d'intervalle, et le client passerait son temps à reconstruire des
maillages qu'il avait déjà.

**Le défaut que ça a révélé.** Le gestionnaire de paquets tient `players_mutex`
pendant toute la génération et l'envoi — des centaines de millisecondes pour un
grand saut — et le thread de tick attendait dessus pour ses keep-alive. Mesuré :
un événement « can't keep up » exactement pendant un saut de cinq chunks.

Le tick prend désormais ce verrou en `try_lock`. Un keep-alive différé d'un tick
est sans conséquence ; un tick bloqué derrière le réseau ne l'est pas. Deux
pièges évités au passage : sauter la passe avec `continue` aurait franchi le
`sleep` en fin de boucle et fait tourner le thread de tick à plein régime
précisément quand le serveur est chargé, et parcourir la map sans le verrou
serait une course à part entière.

Ce n'est **pas** la règle « un seul écrivain » du projet — c'est du verrouillage
honnête en attendant que le monde vive sur le thread de tick, ce qui est le
travail `ov_sim`.

Limite connue : le cache de chunks ne se vide jamais. Marcher longtemps le fait
croître sans borne. Une éviction demande de savoir quels chunks sont encore
référencés, c'est-à-dire le système de tickets.

---

## Écriture des fichiers région (`ov_nbt`)

Lire une région prouve qu'on sait en analyser une ; **l'écrire** est la moitié
dont dépend une sauvegarde, et celle dont les erreurs coûtent un monde.

Deux règles que le format n'énonce pas :

- **Le champ de longueur compte l'octet de compression.** N octets compressés
  s'écrivent `N+1`. Être un court tronque le dernier octet de *chaque* chunk du
  fichier, ce qui se lit comme un flux corrompu et non comme un flux court.
- **Un chunk commence sur une frontière de secteur.** La table d'offsets adresse
  des secteurs, pas des octets : il n'existe aucun moyen d'exprimer « commence
  au milieu de l'un d'eux ». Le remplissage n'est donc pas facultatif.

L'écriture est **entière puis renommée**. Réécrire un chunk sur place serait plus
rapide et laisserait, en cas de crash au milieu, une région dont l'en-tête pointe
des secteurs que le nouveau contenu a déplacés — chaque chunk du fichier se
lirait alors comme du bruit.

Les chunks que personne n'a touchés sont **reportés encore compressés** : les
ré-encoder risquerait de les modifier, alors que tout l'intérêt est qu'ils ne le
soient pas.

### Vérification

`ov-inspect region --verify` reconstruit le fichier depuis ses propres chunks et
compare. Sur le monde réel : **17 879 / 17 879 chunks octet-identiques**.

---

## Sérialisation des chunks (`ov_world`)

Le format disque est **basé sur les noms**, et c'est toute la raison pour
laquelle une sauvegarde peut être partagée entre implémentations. Sur le fil, une
entrée de palette est un identifiant numérique d'état ; sur disque c'est
`{Name: "minecraft:oak_stairs", Properties: {facing: "north", …}}`.

Écrire les ids du réseau sur disque produirait un fichier qui se recharge
parfaitement **ici** et ne veut rien dire dans Minecraft — et dont la version qui
l'a écrit serait la seule à pouvoir le décoder. La conversion passe donc par le
registre dans les deux sens, et un état dont le registre ignore le nom est
**refusé plutôt que deviné**.

Trois décisions à leurs raisons :

- **Le disque n'a pas de forme « directe ».** Un conteneur direct en mémoire est
  reconverti en palette à l'écriture : le format nomme, et 24 135 noms seraient
  absurdes.
- **La lumière est écrite, pas recalculée au chargement.** Il n'y a pas de moteur
  de lumière sur le chemin de lecture ; recalculer différerait silencieusement de
  ce que le client a vu en dernier.
- **Les heightmaps sont recalculés, pas lus.** C'est de la donnée dérivée, un
  heightmap stocké peut être périmé — mesuré sur un monde réel : une colonne sur
  4,5 millions l'était — et le recalcul coûte un balayage que le chargement paie
  déjà.

Les noms de biomes sont **passés en paramètre**. Les biomes vivent dans un
registre que le serveur *envoie* ; leurs identifiants appartiennent à qui a
construit le codec, et ce module n'a pas à les connaître.

### Le monde sur disque

`run/world/region/` — gitignoré, une sauvegarde appartient au joueur, pas au
dépôt. Le disque passe **avant** le générateur : l'ordre inverse fonctionne
jusqu'au premier rechargement puis jette silencieusement tout ce qui a été
construit.

Vérifié de bout en bout : construire une tour et creuser un trou, arrêter le
serveur, le relancer — blocs et lumière identiques, et le fichier produit passe
le contrôle d'aller-retour d'`ov-inspect`.

Autosave toutes les 30 secondes, en plus de l'arrêt propre : un serveur tué n'a
jamais d'arrêt propre, et perdre une heure de construction sur un crash est la
panne dont on se souvient. Mesuré : 4 chunks sur 3 régions, zéro surcharge de tick.

---

## `level.dat`, et l'aller-retour croisé avec Minecraft

Sans `level.dat`, Minecraft ne voit **aucun monde**, quels que soient les
fichiers région présents : la liste des sauvegardes se construit à partir de ce
fichier. Il est en **gzip**, alors que les chunks à l'intérieur d'une région sont
en zlib — rien ne signale la différence, et un `level.dat` écrit avec le mauvais
conteneur se lit comme un fichier corrompu.

Il déclare aussi le **générateur**, ce qui compte plus qu'il n'y paraît : un monde
dont les régions ont été faites à plat mais dont le `level.dat` annonce « noise »
fera pousser du terrain normal dès que le joueur dépassera ce qui était
sauvegardé, avec un mur visible à la jonction.

### Le serveur vanilla comme oracle

Premier essai : le vrai serveur a répondu **`key missing: DragonFight`**. Une
erreur qui nomme exactement le champ manquant — bien meilleure que la plupart, et
un bon rappel que le jeu est l'oracle de test le moins cher disponible. Ajouté,
puis **zéro erreur**.

### Les deux sens, prouvés

| Sens | Preuve |
|---|---|
| Notre sauvegarde → Minecraft | Le serveur 1.20.1 charge notre monde sans une erreur et **sert notre colonne de 8 pierres** dans ses propres paquets de chunk |
| Minecraft → notre sauvegarde | Le fichier région passe de 1 à 529 chunks — vanilla l'a réécrit — et notre serveur le relit avec les bons blocs |

La seconde ligne est établie sans ambiguïté par les champs présents dans le
chunk : `PostProcessing`, `isLightOn`, `structures`, `LastUpdate: 828` et
**25 sections** (vanilla en écrit une de plus pour la lumière). Aucun n'est écrit
par nous. Le chunk relu est donc bien celui de vanilla.

Cette section supplémentaire est ignorée au chargement : elle tombe hors de la
forme du monde et n'a nulle part où aller.

### Un test qui a échoué de mon fait

J'ai tenté de faire poser des blocs de diamant **par** le serveur vanilla via un
client scripté, pour un test plus direct. La pose n'a jamais abouti : le serveur
valide quelque chose que mon client ne satisfait pas. C'est une limite de mon
harnais, pas du code, et la preuve par les champs ci-dessus est plus forte de
toute façon — mais elle est notée plutôt que passée sous silence.

### La lumière traverse les frontières de chunk

La version limitée à un chunk arrêtait la propagation au bord : une construction
adossée à une frontière ne projetait aucune ombre chez le voisin, et la couture
apparaissait comme une ligne droite de sol mal éclairé. La lumière ne respecte
pas les frontières de chunk, le remplissage ne le peut pas non plus.

Le remplissage travaille désormais sur le **voisinage 3×3**, en coordonnées
monde — tout l'intérêt est qu'il ignore où sont les bords.

Seuls les chunks **déjà chargés** participent. Aller chercher les voisins
provoquerait une cascade : générer un chunk générerait ses voisins, puis les
leurs. Une construction contre le bord de la zone chargée présente donc encore
une couture — mais ce bord suit le joueur et reste hors de vue, alors qu'une
frontière de chunk au milieu d'une base, non.

La correction va **en mémoire, sans réémission**. Le client éclaire lui-même ses
propres modifications, donc la couture reste invisible jusqu'au prochain
chargement du chunk — c'est-à-dire précisément quand la valeur stockée est celle
qui compte.

#### Le test qui discrimine vraiment

Un toit à cheval sur une frontière ne prouve rien : chaque chunk a ses propres
sources de chaque côté. Il faut un cas où la lumière **ne peut venir que d'en
face** — un toit couvrant toute la largeur d'un chunk près du bord.

Mesuré : la case en x=16 (chunk 1) vaut **13**. Sa seule voisine éclairée est
x=15, à 14, **dans le chunk 0**. Depuis le chunk 1 seul, la source la plus proche
est à trois cases et donnerait 12. La valeur 13 ne peut donc venir que de l'autre
côté de la frontière.

---

## Orientation des blocs à la pose

Vanilla décide de l'état posé dans le **code Java de chaque bloc** : il n'existe
aucun fichier de données à lire. Ce qui *est* de la donnée — et que le registre
fournit — c'est la liste des propriétés d'un bloc. Les règles ne s'appliquent donc
que là où la propriété existe.

| Propriété | Règle |
|---|---|
| `axis` | déduite de la face cliquée — une bûche posée au sol est verticale, contre un mur elle est couchée |
| `facing` | déduite du lacet du joueur, **mais dans quel sens dépend du bloc** |
| `half` | `top` si le clic était sous un bloc, ou sur la moitié haute d'un côté |
| `type` | même règle, pour les dalles |

### La convention de `facing` a été mesurée, pas déduite

Vanilla n'est pas uniforme, et **le partage ne suit pas les familles**. Dix-huit
blocs ont été posés sur un vrai serveur 1.20.1, joueur orienté au sud, puis l'état
résultant relu.

| Résultat | Blocs |
|---|---|
| **Même direction** que le joueur | `stairs`, `fence_gate`, `door`, **`observer`** |
| **Opposée** | `trapdoor`, `furnace`, `chest`, `dispenser`, `lectern`, `carved_pumpkin`, `repeater`, `comparator`, `end_portal_frame`, `loom`, `stonecutter`, `beehive` |
| Autre chose | `anvil` — revenu tourné d'un quart de tour |

Deux résultats montrent pourquoi deviner ne marche pas. **`observer` suit la
direction du joueur**, ce qui se lit à l'envers de son comportement. **`trapdoor`
est opposée**, alors que portes et portillons — sa famille évidente — ne le sont
pas.

La majorité est « opposée », c'est donc le défaut. Les exceptions mesurées sont
étendues aux variantes par suffixe, celles-ci partageant la même classe Java.
`anvil`, qui échappe aux deux règles, est laissé à son état par défaut plutôt que
tourné de travers avec assurance.

Vérification : notre serveur reproduit **11/11** des orientations mesurées.

**Ce qui reste faux** : les 227 blocs à `facing` horizontal ne sont pas tous
mesurés, seulement un échantillon. Un bloc non couvert prend la convention
majoritaire et peut donc être orienté à l'envers — visible, sans gravité, et
corrigible dès que le harnais de parité couvre l'ensemble.

---

## Entités joueur

Trois paquets suffisent à faire exister un joueur chez les autres, et l'ordre
compte : **`Player Info Update` d'abord**. Le client construit l'entité à partir
de cette liste ; un `Spawn Player` pour un UUID dont il n'a jamais entendu parler
donne une entité sans nom, sans skin, ou pas d'entité du tout.

Le drapeau `update_listed` n'est pas cosmétique non plus : une entrée non listée
n'est pas rendue. Envoyer `add_player` seul ne suffit pas.

Les angles voyagent sur **un octet, 256 pas par tour**. La conversion doit
**boucler et non saturer** : un lacet de 350° et un de -10° désignent la même
direction, et saturer épinglerait un joueur regardant légèrement à l'ouest
plein sud.

La **tête tourne indépendamment du corps**. Un client à qui l'on n'envoie que la
rotation du corps affiche un joueur qui regarde éternellement droit devant.

### Absolu plutôt que relatif

Vanilla préfère les déplacements relatifs pour les petits pas — six octets contre
vingt-huit. Nous envoyons des téléportations absolues : un flux relatif qui perd
ou réordonne un paquet laisse l'entité **définitivement décalée**, et rien dans le
protocole ne permet de s'en apercevoir. L'optimisation viendra avec de quoi la
vérifier.

### Ne retransmettre que ce qui change

Un client envoie une mise à jour de position **à chaque tick**, qu'il ait bougé ou
non. Les relayer toutes représente l'essentiel du trafic d'un serveur peuplé.
Mesuré : 20 paquets d'un joueur immobile produisent **0** retransmission.

---

## Mesurer l'émission lumineuse : ce qui marche, ce qui ne marche pas

La table de flags par état reste le blocage nommé du projet. `emitLight` n'est
pas disponible par état dans les sources écartées, et le jeu lui-même est le seul
oracle exact. Une tentative de harnais a donc été montée.

**La technique.** Le serveur vanilla lit ses commandes sur l'entrée standard, et
`/setblock` place **n'importe quel état** sans contrainte de pose — contrairement
à un client, qui doit respecter portée, support et mode de jeu. On remplit une
zone de bedrock, on place les états à sonder espacés, on laisse la lumière se
propager, on sauvegarde, puis on relit `BlockLight` à chaque cellule.

**Ce qui est établi.** Plusieurs valeurs sortent exactement justes :

| État | Mesuré | Attendu |
|---|---|---|
| `glowstone` | 15 | 15 |
| `stone` | 0 | 0 |
| `redstone_ore[lit=true]` | 9 | 9 |
| `respawn_anchor[charges=0..4]` | 0, 3, 7, 11, 15 | idem |
| `furnace[lit=true]` / `[lit=false]` | 13 / 0 | idem |
| `candle[candles=2,3,4][lit=true]` | 6, 9, 12 | idem |

**Ce qui ne l'est pas.** D'autres états restent contaminés : `candles=1` donne 8
au lieu de 3, et les états éteints ne retombent pas à 0. L'isolation par bedrock
devrait pourtant rendre toute propagation impossible, et élargir l'espacement de
2 à 4 n'a pas suffi. La cause n'est **pas** établie — probablement la suppression
de lumière, qui est asynchrone chez vanilla, ou une zone partiellement chargée.

**Conséquence assumée** : aucune table n'est livrée. Publier une table dont une
partie est fausse serait exactement l'erreur refusée plus tôt avec
`minecraft-data` — une approximation coulée dans les fondations. Le harnais est
gardé au stade d'expérience documentée jusqu'à ce que ses mesures soient
reproductibles à 100 %.

### `ov-inspect light`

Ce qui *est* livré : la lecture, par le lecteur NBT vérifié du projet. La commande
prend des lignes `x y z` sur l'entrée standard et rend le bloc et sa lumière
stockée.

J'ai écrit trois parseurs NBT jetables en Python avant d'admettre qu'ils étaient
la partie la moins fiable de l'expérience — l'un d'eux a produit `glowstone = 0`
et `stone = 3`, des valeurs assez plausibles pour être crues. Le lecteur du
projet, lui, est vérifié octet à octet sur 17 879 chunks.

---

## Block entities

Un block entity porte ce qu'un état de bloc ne peut pas contenir : le texte d'un
panneau, le contenu d'un coffre. Ils sont rangés **à côté** des sections : il y en
a une poignée par chunk contre 98 304 blocs, et réserver de la place dans chaque
cellule coûterait plus que le monde lui-même.

### Deux conventions à ne pas confondre

| Destination | Coordonnées |
|---|---|
| Disque | **monde**, en trois champs `x`, `y`, `z` |
| Fil | **locales au chunk**, x et z tassés dans un octet de 4+4 bits |

Écrire l'une là où l'autre est attendue place chaque panneau et chaque coffre
dans un autre chunk, silencieusement.

### Le panneau de 1.20

Les deux faces existent : un panneau écrit seulement au recto et enregistré sans
`back_text` est refusé par le client. Et chaque ligne est un **composant de
texte**, pas une chaîne : `hello` n'est pas valide là où `{"text":"hello"}` l'est.

Un block entity ne survit jamais à son bloc : tout changement de bloc le supprime.
Un block entity orphelin est un coffre qu'on ne peut ni ouvrir ni enlever.

### Deux défauts trouvés en testant

**Un interblocage.** Le gestionnaire de paquets *Play* tient déjà `players_mutex`
pendant tout son corps ; le reprendre pour diffuser la mise à jour du panneau
bloquait la connexion — `std::mutex` n'est pas récursif. De l'extérieur, ça
ressemblait exactement à un paquet ignoré, et le serveur ne sauvegardait plus.

**Un type d'entité à zéro.** Le disque nomme les block entities, le protocole les
numérote. Un chunk rechargé repartait donc avec un `type_id` de 0 — et zéro est
un identifiant **valide**, appartenant à un autre type. L'omission ne cassait
rien de visible : elle re-typait tous les panneaux d'un chunk rechargé. Mon
client de test ne validait pas ce champ ; il a fallu aller le lire pour le voir.

Vérifié de bout en bout : poser, écrire (guillemet compris), sauvegarder, arrêter,
redémarrer — le texte est sur disque, il repart dans le paquet de chunk, et le
`type_id` vaut bien 7.

---

## Opacité à la lumière du ciel : la première colonne mesurée de la table de flags

Signalé en jouant : poser un panneau, se déconnecter, revenir — une tache noire.
Le moteur de lumière traitait **tout ce qui n'est pas de l'air comme opaque**,
donc un panneau projetait une ombre pleine. Invisible pendant la session, puisque
le client éclaire lui-même ses propres poses.

### Le montage, et l'erreur silencieuse qui l'a d'abord ruiné

Un plateau de bedrock percé de **puits d'un bloc de large**, le bloc testé posé
au sommet de chaque puits, la lumière du ciel lue au fond. La lumière ne peut
entrer que par le bloc testé.

Premier essai : tous les blocs opaques donnaient **14**, pas 0. Cause :
`/fill` plafonne à **32 768 blocs** et mon plateau en demandait 202 800 — la
commande a été **rejetée sans que rien ne s'arrête**. Il n'y avait donc aucun
plateau ; les blocs flottaient en plein ciel et le 14 était la lumière qui les
contournait. Refait couche par couche (16 384 blocs chacune), le signal est net.

### Le résultat, sur les 1003 blocs

| Classe | Nombre | Exemples |
|---|---|---|
| **Transparente** — le ciel passe intact | 483 | air, panneau, torche, verre, barrière |
| **Opaque** — le ciel s'arrête | 443 | pierre, dalle, escalier, glowstone |
| **Atténuante** — le ciel faiblit | 77 | eau, feuillage, glace |

Un bloc jamais mesuré est traité comme **opaque** : se tromper vers une pièce
sombre vaut mieux que vers un monde sans ombres.

### Une correction que cette mesure a rendue possible

La lumière directe s'arrêtait au premier bloc **non-air**, c'est-à-dire à
`WORLD_SURFACE`. Elle descend maintenant jusqu'au premier bloc **opaque** — ce
qui est la vraie règle, et ce qui ne pouvait pas s'écrire sans connaître
l'opacité. Un panneau, une torche ou une vitre reçoivent désormais 15 et non 14.

### Ce qui reste

C'est mesuré **par bloc**, pas par état : la variation selon l'état n'a pas été
sondée. Les blocs atténuants sont traités comme transparents — se tromper d'un
niveau ou deux vaut mieux que de quinze. Et les **formes de collision**, qui
bloquent le mouvement autoritatif, restent non mesurées.

### Vérification sur un vrai monde 1.20.1

Les vérifications d'aller-retour précédentes portaient sur un monde **1.21
moddé** — la seule sauvegarde réelle disponible. Un monde généré par le jeu en
**1.20.1**, la version cible, permet enfin de fermer la boucle sur la bonne
version.

| Mesure | Résultat |
|---|---|
| `level.dat` (gzip, DataVersion 3465) | aller-retour **octet-identique** |
| 4 fichiers région, 2601 chunks | **2601/2601** identiques en lecture |
| Les mêmes, réécrits par notre écrivain | **2601/2601** identiques |
| Blocs servis par notre serveur vs le fichier | **180/180** positions, 13 types de blocs |

Les 13 types rencontrés — air, andésite, bedrock, deepslate, minerai d'or du
deepslate, diorite, terre, granite, herbe, gravier, pierre, tuff, eau — couvrent
du terrain généré réel, pas un superflat.

Observation de performance : charger et envoyer les 289 chunks d'une connexion
prend **environ 10 secondes en build debug** sur un monde réel, contre 0,75 s sur
un superflat généré. La différence est le décodage des palettes et le calcul de
lumière sur des chunks pleins.

### Coffres

La numérotation des slots dans une fenêtre n'est pas l'identité : **0 à 26** sont
le coffre, **27 à 53** l'inventaire principal du joueur, **54 à 62** sa barre
d'action — alors que les slots *du joueur* sont numérotés 9 à 44. Se tromper de
correspondance déplace des objets entre les deux mauvais endroits.

Vanilla stocke le contenu comme une liste de `{Slot, id, Count}` en **omettant
les slots vides**. La liste n'est donc pas indexée par slot et sa longueur ne dit
rien de la taille du coffre.

Le NBT d'un objet est **conservé tel quel**, en octets bruts. Le serveur ne
comprend ni enchantements ni noms personnalisés, et ré-encoder une balise qu'on
ne comprend pas est la façon dont on perd des données. La laisser passer intacte
ne le peut pas.

`minecraft:generic_9x3` est lu dans le registre `minecraft:menu`, pas codé en
dur : un coffre 9×3 n'a pas le même numéro qu'un 9×6, et se tromper ouvre une
fenêtre de la mauvaise taille sur les bonnes données.

**Les six modes de clic.** Tous sont désormais appliqués côté serveur : clic
gauche et droit (0), shift-clic (1), touche numérique (2), lâcher (4) et glissé
(5). La fenêtre est réémise en entier après chaque clic, donc un mode non traité
disparaîtrait de l'écran au lieu d'y laisser un objet qui n'existe pas — visible
et sans danger, là où la duplication ne le serait pas.

Le glissé est envoyé en trois temps — un paquet pour commencer, un par slot
traversé, un pour finir — donc son état vit entre les paquets. Rien ne bouge
avant la fin, si bien qu'un glissé jamais terminé ne coûte rien. À gauche la
pile se répartit à parts égales et le reste demeure sur le curseur : 64 pierres
sur trois slots donnent 21, 21, 21 et un exemplaire en main. À droite, un
exemplaire par slot.

Les numéros de slot restent le piège : la barre d'action est 54 à 62 dans la
fenêtre et 36 à 44 chez le joueur. Une touche numérique sur la fenêtre 55 avec
le bouton 1 désigne **le même slot des deux côtés**, et le premier essai n'a
donc rien échangé — ce qui ressemblait à un bug et n'en était pas un.

**L'inventaire est autoritatif en survie.** Le paquet qui règle un slot en
créatif est ignoré hors créatif, comme le fait vanilla : l'honorer laisserait
n'importe quel client se donner n'importe quoi.

Cliquer un coffre l'ouvre toujours. Vanilla ne pose contre un coffre que si le
joueur est accroupi, ce qui n'est pas encore suivi.

---

## Taille de pile maximale par objet

Le shift-clic ne peut pas être écrit sans elle. Déplacer une pile vers un coffre
veut dire « fusionner dans les piles compatibles, puis remplir un slot vide » —
et « compatible » s'arrête à une limite qui n'est ni 64 pour tout le monde, ni
déductible du nom de l'objet. Une épée ne s'empile pas, une perle de l'Ender
s'arrête à 16, une pomme va à 64. Deviner produit soit des piles impossibles que
le client refuse d'afficher, soit de la duplication.

La valeur n'est **pas** dans `registries.json` : ce rapport liste les
identifiants, pas les propriétés des objets. Elle n'est pas non plus dans les
modèles ni dans les tags. Elle vit dans le code Java, ce que nous ne lisons pas.

**Mesure.** Un joueur (`scripts/keepalive_client.py`, qui répond aux keep-alive
et confirme les téléportations, sans quoi le serveur le déconnecte au bout de
30 s) reste connecté à un vrai serveur 1.20.1. Les commandes sont envoyées par
un FIFO sur son entrée standard, pour chacun des 1255 objets du registre :

```
clear StackProbe
give  StackProbe <item> 65
say   PROBE <item>
data get entity StackProbe Inventory
```

Demander **65** est ce qui fait la mesure : le serveur découpe la donation en
autant de piles que nécessaire, et le `Count` de la première pile *est* la
limite. Le `say` sert d'ancre pour associer chaque réponse à son objet, la
sortie de `data get` n'en portant pas le nom.

**Résultat : 1254 / 1255.** 1032 objets à 64, 45 à 16, 177 à 1. Le seul non
mesuré est `minecraft:air`, que `give` refuse — sans conséquence, un slot
d'air est un slot vide.

**Stockage.** Un octet par objet, dans l'ordre du registre `minecraft:item`,
dans le `.ovpack` (format v5, section `stacks_offset`). Lu par
`Registries::max_stack_size(ProtocolId)`, qui répond 64 pour un identifiant hors
table — c'est la valeur de la grande majorité, donc la moins nuisible en cas de
pack périmé. La table brute n'est pas commitée : elle est régénérée dans
`data/vanilla/1.20.1/normalized/stack_sizes.json`, gitignoré comme le reste des
données dérivées de Mojang.

---

## MOTION_BLOCKING et OCEAN_FLOOR : demander au jeu ce qu'il a lui-même écrit

Trois des quatre heightmaps demandent la même chose sous des formes différentes :
« ce bloc arrête-t-il le mouvement ? ». Rien dans les rapports officiels ne le
dit — en vanilla c'est du code Java. Sans cette réponse, `MOTION_BLOCKING`,
`MOTION_BLOCKING_NO_LEAVES` et `OCEAN_FLOOR` restaient vides, et c'est ce que
`PROGRESS.json` désignait depuis M3 comme le blocage nommé.

### D'abord, le sens des deux cartes

Il était inversé dans nos propres commentaires. Réglé par la mesure, sur un vrai
monde 1.20.1 : sur 10 411 colonnes où `MOTION_BLOCKING` et `OCEAN_FLOOR`
diffèrent, le bloc désigné par la première est de l'eau dans les 40 cas
examinés, et celui désigné par la seconde de la terre, du sable ou du gravier.
`MOTION_BLOCKING` compte donc les fluides, `OCEAN_FLOOR` non — c'est ce qui en
fait le *fond* d'un océan et pas sa surface.

### L'oracle

Le jeu écrit sa propre réponse dans chaque chunk qu'il sauvegarde. Il suffit
donc de poser un bloc au sommet d'une colonne sur un vrai serveur 1.20.1, de
laisser sauvegarder, et de relire quelle heightmap il a décidé de faire monter.
La lecture passe par `ov-inspect column`, donc par le lecteur NBT vérifié sur
17 879 chunks — pas par un parseur écrit pour l'occasion.

`scripts/measure_motion.py` pose les 1003 blocs en quatre zones, parce qu'un
bloc doit survivre à sa pose avant de pouvoir être lu :

| Zone | Support | Ce qu'elle rattrape |
|---|---|---|
| A | à nu sur l'herbe | tout ce qui tient seul |
| B | espacé de 16 blocs | ceux qui portent de l'eau, et l'étalent sinon sur sept blocs |
| C | muré de pierre sur quatre côtés | torches murales, échelles, boutons, panneaux |
| D | substrat choisi | cultures sur terre labourée, cactus sur sable, chorus sur end stone |

Chaque colonne est relue **par son nom** : un bloc qui n'a pas tenu — une torche
sans mur — est écarté au lieu d'être enregistré comme « n'arrête pas le
mouvement ». Les zones A et C se recouvrent largement et **ne se contredisent
nulle part**, ce qui est le contrôle croisé de la méthode elle-même.

**Résultat : 996 blocs sur 1003.** 778 arrêtent le mouvement, 187 non, 10 sont
des feuilles, 3 sont de l'air, 22 portent un fluide dans leur état par défaut.

Les sept restants — `kelp_plant`, `weeping_vines`, `weeping_vines_plant`,
`twisting_vines_plant`, `cave_vines`, `cave_vines_plant`, `big_dripleaf_stem` —
sont des segments de plantes verticales : ils ont toujours quelque chose
au-dessus d'eux, donc ils ne peuvent jamais *être* le sommet d'une colonne.
L'oracle ne peut pas les voir, et une heightmap n'en a jamais besoin. Ils sont
marqués non mesurés plutôt que supposés inoffensifs, et
`BlockRegistry::motion_measured` permet de faire la différence.

### Par bloc ou par état ?

La question décide entre une table de 1003 entrées et une de 24 135. Dix paires
d'états contrastés ont été posées côte à côte : neige à une couche et à huit,
trappe ouverte et fermée, portillon, porte, dalle et double dalle, échafaudage,
grand dripleaf incliné. **Les dix paires sont d'accord.** Le prédicat est donc
par bloc.

Et la neige n'arrête le mouvement à *aucune* épaisseur, pas même à huit couches
où elle remplit le cube entier. Aucun raisonnement sur la forme de collision ne
l'aurait prédit ; c'est exactement le genre de réponse qui justifie de mesurer.

L'eau, elle, est bien par état : `scaffolding[waterlogged=true]` fait monter
`MOTION_BLOCKING` et `scaffolding[waterlogged=false]` non, alors qu'aucun des
deux n'arrête le mouvement. Les deux ont été posés. Six blocs sont mouillés sans
avoir de propriété `waterlogged` à mettre — `water`, `lava`, `kelp`, `seagrass`,
`tall_seagrass`, `bubble_column` — d'où un bit par bloc *et* la lecture de la
propriété par état.

### Les feuilles, par deux chemins indépendants

`MOTION_BLOCKING_NO_LEAVES` a désigné exactement dix blocs. Ce sont exactement
les dix membres du tag `minecraft:leaves`, résolus séparément depuis les
fichiers JSON du datapack. Deux sources qui n'ont rien en commun donnent la même
liste.

### Vérification

`ov-inspect heightmaps <region>` recalcule les quatre heightmaps depuis les
blocs et les compare à celles que le jeu a écrites :

| Monde | Colonnes × 4 cartes | Identiques |
|---|---|---|
| Monde 1.20.1 vanilla (2601 chunks) | 274 944 | **274 944** |
| Monde moddé (2900 chunks) | 742 020 | **742 020** |

Sur le monde moddé, 380 colonnes sont écartées : elles contiennent un des 31
blocs de palette que cette version n'a pas (`create_hypertube:hypertube_entrance`,
`simulated:throttle_lever`…). Elles sont comptées comme sautées et non comme
justes — nous n'avons pas de réponse pour ces blocs, et faire comme s'ils ne
comptaient pour rien serait une réponse.

Enfin, l'aller-retour : un monde écrit par nous avec les quatre heightmaps se
charge dans le vrai serveur 1.20.1 sans une erreur, et les valeurs qu'il
réécrit sont identiques aux nôtres — `MOTION_BLOCKING_NO_LEAVES` comprise.

### Stockage

Un octet par bloc, celui qui portait déjà l'opacité à la lumière : bits 0-1
l'opacité, bit 2 « arrête le mouvement », bit 3 « feuilles », bit 4 « air »,
bit 5 « mesuré ». Plus un bit par **état** pour le fluide, soit 3 017 octets.
Format de pack v6. La table brute est régénérée dans
`data/vanilla/1.20.1/normalized/motion.json`, gitignoré comme le reste des
données dérivées de Mojang.

---

## Durées de cassage : la table d'un tiers, vérifiée tick par tick

La dureté d'un bloc n'est ni dans les rapports du data generator ni dans les
datapacks : c'est du code Java. **PrismarineJS/minecraft-data** (MIT, autorisé
par `CLAUDE.md`) la publie pour les 1003 blocs, avec le drapeau
`harvestTools` qui dit si un outil correct est exigé. C'est de là que vient la
table.

Mais sur ce projet cette source s'est déjà révélée fausse une fois — l'émission
lumineuse, donnée par bloc alors qu'elle est par état. Elle est donc prise comme
**hypothèse** et confrontée au jeu.

### L'oracle : le serveur ne casse pas le bloc tout seul

C'est le détail qui rend la mesure possible. Le serveur vanilla n'achève pas la
destruction de lui-même : il attend que le client dise « j'ai fini », puis
vérifie. Si la progression écoulée dépasse **0,7**, il croit le client sur
parole et casse immédiatement. En dessous, il bascule sur sa propre horloge et
casse le bloc au tick exact où vanilla le casserait.

Dire « fini » dans le même tick que « je commence » passe donc toujours sous le
seuil, et le nombre de ticks qui s'écoulent ensuite **est** la durée du jeu. Le
client n'a jamais eu besoin de la connaître.

Deux pièges ont coûté chacun une série de mesures fausses :

* Le compte doit se lire sur l'horloge du serveur, pas sur la montre du client.
  Le paquet `Update Time` (0x5E) porte l'âge du monde en ticks toutes les
  secondes ; il faut **le même ancrage aux deux bouts** d'une mesure. Avec deux
  ancrages différents la même mesure donnait 15, 18, 24 ou 26 pour de la terre.
  Avec un seul, elle donne 15 six fois sur six.
* L'envoi doit être aligné sur la grille de ticks. Envoyé au milieu d'un tick,
  le départ tombe dans celui-ci ou dans le suivant selon la milliseconde, et le
  compte sort une fois sur deux trop court d'un tick.

Le serveur tenait 50,00 ms par tick sur les vingt-cinq mesures de contrôle — ce
n'était donc pas lui qui variait.

### Ce que la mesure a établi, et que la table ne disait pas

| Question | Réponse mesurée | Comment |
|---|---|---|
| Vitesse des paliers | bois 2, pierre 4, or **12**, fer 6, diamant 8, netherite **9** | sur la pierre, `ceil(45 / v)` ne laisse qu'un entier possible ; netherite a demandé l'obsidienne pour séparer 9 de 10 et 11 |
| Efficacité | ajoute exactement `niveau² + 1` | 137, 79 et 43 ticks sur l'obsidienne aux niveaux I, III et V |
| Niveau de récolte de l'or | celui du **bois** | pioche en or sur minerai de fer : 25 ticks, soit le diviseur 100 |
| Ce qui autorise la récolte | la **famille** de l'outil, pas seulement son palier | pelle en netherite sur pierre : **150** ticks, exactement comme à main nue, là où une pioche en bois en prend 23 |

Ce dernier point est le seul qu'aucun raisonnement ne donne. Une pelle en
netherite dépasse tous les paliers existants et reste, sur de la pierre, aussi
lente qu'une main nue.

### Vérification

`scripts/measure_hardness.py` confronte la table au serveur, bloc par bloc, sur
la condition la moins chère : à main nue quand elle reste sous vingt secondes —
la vitesse y vaut exactement 1, donc le compte de ticks lit la dureté sans
intervalle — et avec l'outil correct au-delà, ce que l'obsidienne exige avec ses
deux cent cinquante secondes.

**985 blocs sur 996 donnent exactement le nombre de ticks prédit.** Les onze
autres ne sont pas des désaccords mais des blocs hors de portée du banc :

* `air`, `cave_air`, `void_air` — il n'y a rien à miner ;
* `water`, `lava`, `bubble_column` — un client ne vise jamais un fluide, et le
  serveur ne donne pas suite ;
* `snow`, `snow_block`, `cactus`, `powder_snow` — posés mais jamais cassés par
  le banc, sans explication trouvée.

Un douzième bloc, `dragon_egg`, donne 1 tick au lieu des 90 prédits : frappé, il
se **téléporte** au lieu de se casser. C'est un comportement, pas une dureté.

Six écarts apparents supplémentaires — dont la pierre à 149 ticks au lieu de
150 — ont disparu en remesurant seul : ils venaient de la contention entre les
six sondes parallèles. C'est la raison pour laquelle un écart n'est jamais
retenu sans être reproduit.

### De bout en bout

Le même banc, branché sur **notre** serveur en `--survival`, donne les mêmes
nombres que sur le vrai jeu :

| | vanilla | Ondes VOXEL |
|---|---|---|
| herbe, main nue | 18 | 18 |
| pierre, main nue | 150 | 150 |
| pierre, pioche en bois | 23 | 23 |
| pierre, pelle en netherite | 150 | 150 |
| planches de chêne, main nue | 60 | 60 |
| verre, main nue | 9 | 9 |
| obsidienne, pioche en fer | 834 | 834 |
| obsidienne, pioche en netherite | 167 | 167 |

Le premier essai donnait 149, 22 et 17 : un tick de moins partout. Vanilla écrit
sa condition `progression × (écoulé + 1)` avec un départ enregistré dans le tick
même qui le testera ; le nôtre est enregistré sur le thread réseau, un tick plus
tôt, donc le « + 1 » y est déjà. Un décalage d'un tick n'est pas cosmétique : le
client prédit la même arithmétique, et un bloc cassé un tick trop tôt
réapparaît sous les yeux du joueur.

---

## Tables de butin : la seule partie vraiment data-driven

Contrairement à la dureté ou à l'opacité, les tables de butin **sont** des
données en vanilla. Elles vivent dans le datapack, on les régénère localement
avec le data generator officiel, et rien n'a besoin d'être mesuré pour les
obtenir. Ce qui est du code, c'est l'**interpréteur** — et c'est lui qu'on
écrit et qu'on vérifie.

### Un vocabulaire fermé, et refusé bruyamment quand il déborde

Les 925 fichiers de tables de blocs de 1.20.1 n'emploient qu'un vocabulaire
petit et clos, ce qu'un relevé exhaustif confirme avant d'écrire une ligne :

| | |
|---|---|
| entrées | `item` (895), `alternatives` (72), `dynamic` (17) |
| conditions | `survives_explosion` (689), `block_state_property` (222), `match_tool` (138), `table_bonus` (27), `any_of` (15), `inverted` (13), `random_chance` (7), `location_check` (4), `entity_properties` (2) |
| fonctions | `set_count` (209), `explosion_decay` (136), `copy_name` (45), `copy_nbt` (37), `apply_bonus` (31), `set_contents` (17), `limit_count` (5), `copy_state` (2) |
| `match_tool` | **quatre** prédicats distincts en tout : Silk Touch, les cisailles, et deux tags |
| poids d'entrée | aucun n'est explicite : tous valent un |
| `rolls` | toujours une constante |

Tout ce qui sort de cette liste est **refusé à la compilation** plutôt que
compilé au jugé, et les fonctions qui demandent les données d'un block entity —
un coffre nommé, une shulker box remplie — laissent la pile telle quelle au lieu
de la supprimer. Une table à moitié comprise donne des objets qui n'existent
pas.

L'aplatissement se fait au build, comme pour les tags : descendre un arbre de
conditions à chaque bloc cassé mettrait un parcours de graphe dans le chemin du
joueur.

### La table d'un bloc ne porte pas toujours son nom

C'est le piège du jalon, et il est du genre à passer inaperçu. `wall_torch.json`
**n'existe pas** : une torche murale renvoie à `blocks/torch`, un panneau mural à
`blocks/oak_sign`, une tête murale à celle de la tête posée. Déduire la table du
nom du bloc marche pour **920 blocs sur 1003** — juste assez pour avoir l'air
juste, et la première comparaison a bien montré 48 blocs muraux qui ne
donnaient rien chez nous et un objet chez vanilla.

La correspondance est donc mesurée, pas devinée : la commande `/loot` journalise
`from loot table minecraft:blocks/X` avec le X qu'elle a réellement employé.
`scripts/measure_loot_tables.py` la relève pour les 1003 blocs. Vingt et un
d'entre eux pointent sur `minecraft:empty` — l'air, les fluides, la tête de
piston, le bedrock — ce qui est une réponse différente de « une table qui n'a
rien donné ».

### L'oracle : `/loot`, qui tire la table sans casser le bloc

`execute as <joueur> at <joueur> run loot give <joueur> mine <x> <y> <z> mainhand`
tire exactement la table du bloc avec l'objet tenu, et le résultat se lit dans
l'inventaire. Pas de durée, pas de position, pas de physique : juste le tirage.

Deux précautions que le banc a dû apprendre :

* **L'inventaire ne tient que 35 piles** une fois l'outil posé. Un minerai
  généreux sous Fortune III le remplit, et le total s'arrête alors net sur
  2240 — un chiffre parfaitement plausible et entièrement faux. Les tirages se
  font donc par paquets de vingt-quatre.
* **Un désaccord ne se retient pas sans être rejoué.** Un minerai sous Fortune
  multiplie un compte aléatoire par un multiplicateur aléatoire, et la variance
  du produit est bien plus large que la racine du total : sur trente-deux
  tirages, deux échantillons corrects diffèrent couramment assez pour passer
  pour une erreur. Trois cas ont ainsi été signalés à tort avant d'être rejoués
  plus longtemps.

### Ce que la comparaison a corrigé

`apply_bonus` en formule `ore_drops` tire `nextInt(niveau + 2) - 1`, plancher à
zéro, et le multiplicateur est ce résultat **plus un** : à Fortune III, 1, 1, 2,
3, 4 à chances égales, soit une moyenne de 2,2. Plancher le tirage à un au lieu
de zéro donne 1,6 — une valeur d'apparence raisonnable, et un tiers trop basse.
Le vrai serveur a tranché : 27 diamants pour douze tirages.

### Le vert qui ne prouvait rien

`tall_grass` et `large_fern` passaient la comparaison, et pour la mauvaise
raison. Leur table exige, par un `location_check`, que la moitié haute de la
plante soit bien là ; nous répondions faux faute de savoir regarder au-dessus,
et l'oracle répondait faux aussi — parce que le banc ne pose que la moitié
basse. Deux erreurs qui s'annulent donnent un vert.

Les quatre `location_check` des tables de blocs sont tous de la même forme,
« le bloc à un cran au-dessus ou au-dessous est X dans l'état Y », et servent
uniquement aux deux plantes à deux blocs. C'est donc implémenté, avec un
émetteur qui **refuse** toute autre forme plutôt que de supposer qu'elle se
comporte pareil. Mesuré ensuite sur la vraie plante entière : le serveur donne
22 graines pour 200 tirages, et rien du tout quand la moitié haute manque.

### Résultat

`scripts/check_loot.py` tire chaque cas des deux côtés et compare. La matrice
est ciblée : chaque configuration d'outil n'est essayée que sur les tables qui
la mentionnent, parce que tirer Fortune sur neuf cents tables qui l'ignorent
coûte une heure et ne prouve rien.

**1215 cas sur 1215 concordent**, sur 32 tirages chacun, quatre d'entre eux
ayant demandé 1024 tirages pour se départager.

Ce qui n'est **pas** fait, et qui est refusé bruyamment plutôt que compilé au
jugé : `copy_name`, `copy_nbt`, `copy_state` et `set_contents`, qui demandent
les données d'un block entity — un coffre nommé retombe donc anonyme, une
shulker box vide. Et l'entrée `dynamic` du pot décoré, qui porte ses tessons.
Ces fonctions laissent la pile telle quelle au lieu de la supprimer : l'objet
tombe, son contenu non.

---

## Entités objet : quatre paquets, relevés plutôt que devinés

Le butin ne sert à rien tant qu'il ne tombe pas. Quatre paquets suffisent, et
leurs identifiants ont été **capturés** sur un vrai serveur 1.20.1 — la page de
protocole archivée s'est trompée sur chacun de ceux que ce projet a vérifiés
contre elle.

Un `summon item` observé depuis un client sonde donne, dans l'ordre :

| Paquet | Id | Ce qu'il porte |
|---|---|---|
| Spawn Entity | `0x01` | id, uuid, **type 54**, x/y/z, trois angles, données, vélocité |
| Set Entity Metadata | `0x52` | index **8**, type **7** (un slot), puis la pile |
| Take Item Entity | `0x67` | ramassé, ramasseur, nombre |
| Remove Entities | `0x3E` | la liste |

Sans la métadonnée, l'entité existe et **ne rend rien du tout** — ce qui
ressemble exactement à un paquet qui ne serait jamais arrivé. `Take Item` est
purement cosmétique et le retrait reste nécessaire ensuite, mais sans lui les
objets s'éteignent à un bloc du joueur au lieu de voler vers lui.

Les octets exacts de ces trois encodages sont figés en test
(`src/ov_protocol/tests/test_entities.cpp`) : ce sont ceux que le jeu a produits
pour une entité connue, cinq diamants en 3,5 / -60 / 0,5. Un champ qui bouge ou
un varint qui grandit échoue là, et non sous la forme d'un objet invisible.

**Ce qui n'est pas vanilla, et qui est dit plutôt que caché.** Une pile tombe au
centre du bloc, sans la vitesse aléatoire que vanilla lui donne, et elle ne
subit pas la gravité : elle reste où le bloc était. Elle n'est pas non plus
sauvegardée — un redémarrage perd ce qui traînait au sol. Le ramassage se fait
dans une boîte de 1,2 bloc autour du joueur plutôt que par intersection de
volumes. Enfin, un inventaire plein laisse la pile au sol avec ce qui n'a pas
tenu : la faire disparaître serait une perte silencieuse.

---

## Conventions de pose : ce que `setblock` ne dit pas

`setblock` place exactement l'état qu'on lui donne. Il ne dit donc rien de ce
qu'un bloc **décide** au moment d'être posé — et c'est là que vivent les
conventions : quel côté d'une dalle, quelle charnière d'une porte, où va la tête
d'un lit. Cela ne se lit qu'en posant vraiment, avec un clic, une face et un
point de contact.

`scripts/measure_placement.py` joue des scénarios sur un vrai serveur 1.20.1 et
relit le monde avec `ov-inspect state`, qui rend le nom **et toutes les
propriétés** — `column` ne donnait que le nom, ce qui suffit à savoir qu'un bloc
est là et pas à savoir lequel.

### Le bogue d'un octet qui ne se voyait pas

Pendant un moment, aucune pose ne passait : le serveur renvoyait les deux blocs
concernés à leur état d'avant, sans une ligne de journal. La cause était dans la
sonde, pas dans le serveur. Le paquet de synchronisation de position porte X, Y,
Z, lacet, tangage, **un octet de drapeaux**, puis l'identifiant de
téléportation : trente-trois octets, pas trente-deux. En confirmant avec l'octet
de drapeaux la sonde répondait toujours zéro, le serveur restait en attente — et
un serveur qui attend une confirmation de téléportation **ignore silencieusement
toute interaction avec un bloc**.

Ce qui rend l'erreur durable : le **cassage**, lui, continue de marcher. Toutes
les mesures de durée ont donc été justes tout du long, et rien n'a signalé le
problème avant qu'on essaie de poser.

### Ce que le vrai serveur répond

| Scénario | État obtenu |
|---|---|
| dalle posée au sol | `type=bottom` |
| dalle recliquée par le dessus | **`type=double`** — un seul bloc, pas deux moitiés |
| clôture seule | aucune connexion |
| deux clôtures côte à côte | `east=true` / `west=true` |
| clôture contre de la pierre | `east=true` — elle s'accroche aussi au plein |
| deux murets | `east=low` / `west=low`, et **`up=true`** |
| porte | deux moitiés, `hinge=left`, orientée comme le regard |
| lit | pied au clic, tête un cran plus loin dans la direction du regard |

Les lits rejoignent donc les portes, les escaliers, les portillons et
l'observateur dans le groupe qui s'oriente **comme** le joueur regarde, et non à
son opposé.

### Ce qui reste, et pourquoi il n'est pas fait ici

Les connexions demandent un prédicat de plus : « une clôture s'accroche-t-elle à
ce bloc ? ». Il se mesure très bien — poser une clôture, poser le bloc à côté,
relire — et le relevé donne pierre oui, verre oui, portillon oui, mais dalle
non, escalier non, feuillage non, muret non. **C'est donc une propriété de
l'état et de la face**, pas du bloc : une dalle basse n'offre pas de face pleine
à l'est, la même dalle en `type=double` si. Une table par bloc serait fausse
exactement là où on la remarquerait, et elle attend d'être faite par état.
---

## Modèles de blocs : blockstate → variant → modèle → parent → faces (`ov_render`)

Le rendu de vanilla est **piloté par les modèles**, pas par une géométrie écrite
en dur. Chaque état de bloc traverse `blockstates/<bloc>.json`, y trouve une
référence de modèle et une rotation, puis `models/<chemin>.json` et sa chaîne de
`parent` jusqu'à une liste de boîtes. Un escalier n'est pas un cas particulier
du moteur : c'est trois boîtes dans un fichier JSON.

Source : `https://minecraft.wiki/w/Tutorials/Models` et
`https://minecraft.wiki/w/Model` (format de modèle et de blockstate). Les
assets eux-mêmes — 1005 blockstates, 2016 modèles de bloc de l'instance 1.20.1
— servent d'oracle secondaire partout où la documentation reste vague.

### Écart assumé au plan : pas de mailleur greedy

Le plan initial prévoyait « mesher greedy + AO ». **C'est une erreur pour un
objectif de parité, et elle est corrigée ici.** Un mailleur greedy fusionne des
faces adjacentes en un grand quad. Cela suppose deux choses que vanilla ne
respecte pas :

1. **que les blocs soient des cubes.** Un escalier, une clôture, une fleur, une
   dalle, une trappe — la majorité du contenu — ne sont pas des cubes pleins. Un
   greedy ne peut fusionner que le sous-ensemble cubique et doit émettre le
   reste face par face de toute façon, donc il ajoute un chemin de code sans
   supprimer l'autre ;
2. **que l'éclairage soit constant sur la face fusionnée.** L'occlusion
   ambiante et la lumière lissée de vanilla sont **par sommet**, calculées
   depuis les voisins de chaque coin. Fusionner deux faces détruit les sommets
   intermédiaires, donc détruit l'information. Le résultat n'est pas « presque
   pareil » : les dégradés disparaissent et le terrain devient plat.

L'émission est donc **par face, depuis le modèle baké**, ce que fait vanilla.
La ligne correspondante de `ROADMAP.md` a été réécrite.

### La règle que la documentation ne donne pas : l'`uv` généré

Le format documente `uv` comme « optionnel, généré depuis la position de
l'élément » sans dire *comment*. Il a fallu le retrouver.

D'abord par la géométrie : sur chacune des quatre faces latérales, la texture
doit apparaître **non miroitée vue de l'extérieur**, `v` croissant vers le bas.
Cela donne un axe `u` et un axe `v` par face. Ensuite par les données : les
modèles de vanilla écrivent souvent un `uv` explicite qui reproduit exactement
ce que le générateur aurait produit, ce qui rend l'hypothèse mesurable.

| Face | u | v |
|---|---|---|
| `down` | `x` | `z` |
| `up` | `x` | `z` |
| `north` | `16 − x` | `16 − y` |
| `south` | `x` | `16 − y` |
| `west` | `z` | `16 − y` |
| `east` | `16 − z` | `16 − y` |

`down` et `up` partagent la même base — `down` est donc miroitée par rapport à
la règle « vue de l'extérieur ». Ce n'est pas une simplification de notre part :
c'est ce que les données imposent, unanimement (12 faces asymétriques sur 12).

Corroboration directe, `block/stairs`, élément supérieur `from [8,8,0]`
`to [16,16,16]` : les cinq `uv` écrits à la main dans le fichier de vanilla —
`up [8,0,16,16]`, `north [0,0,8,8]`, `south [8,0,16,8]`, `west [0,0,16,8]`,
`east [0,0,16,8]` — sont **exactement** ceux que la table ci-dessus génère. Un
test unitaire les rejoue.

### Mesure à l'échelle du corpus, et l'expérience qui tranche

`tools/ov_modelbake` rejoue la table sur les 55 503 faces qui écrivent un `uv`
explicite et compte celles où la valeur écrite à la main est celle qu'on aurait
générée :

| Face | `uv` écrits | d'accord | table retenue | table miroir |
|---|---|---|---|---|
| `down` | 6 647 | 4 656 | **70,0 %** | — |
| `up` | 9 778 | 7 809 | **79,9 %** | — |
| `north` | 10 547 | 6 254 | **59,3 %** | 35,1 % |
| `south` | 10 377 | 6 922 | **66,7 %** | — |
| `west` | 9 485 | 6 470 | **68,2 %** | — |
| `east` | 8 669 | 5 515 | **63,6 %** | 55,3 % |
| **total** | **55 503** | **37 626** | **67,8 %** | 61,9 % |

Le taux n'est pas de 100 % et ne doit pas l'être : un `uv` explicite sert
justement, la plupart du temps, à s'écarter du défaut (un escalier réutilise la
moitié de la texture de son côté). Ce qui compte est qu'**aucune face ne
s'effondre**. `north` et `east` étaient les deux orientations dont un premier
échantillon de dix fichiers laissait douter ; l'expérience a été faite dans les
deux sens, et la table retenue gagne nettement sur les deux.

⚠️ **Ce que cette mesure ne prouve pas.** Elle valide l'orientation *relative*
et l'échelle, pas la chiralité absolue : une table entièrement miroitée sur les
quatre côtés produirait les mêmes rectangles. Seule une comparaison visuelle
avec le client vanilla la tranchera, et elle est à faire dès que le renderer
dessine.

### Le sens des rotations, mesuré et non choisi

Les degrés du format tournent **dans le sens horaire autour de l'axe positif**,
c'est-à-dire l'inverse de la matrice de rotation usuelle. Deux blocs le fixent
sans ambiguïté :

- `minecraft:furnace` est modélisé face au nord et utilise `y: 90` pour
  `facing=east` → `rotate_y(90)` envoie `−Z` sur `+X` ;
- `minecraft:observer` est modélisé face au nord et utilise `x: 90` pour
  `facing=down` → `rotate_x(90)` envoie `−Z` sur `−Y`.

Écrire les matrices dans l'autre sens met tous les blocs directionnels sur le
mauvais mur. Les deux faits sont des assertions de test.

⚠️ Le **signe de la rotation d'élément** (`rotation.angle` dans le modèle) n'est
lui pas discriminé par les données : les seuls éléments tournés de vanilla —
`cross`, `big_dripleaf` — sont symétriques à ±45°. La même convention que la
rotation de blockstate est appliquée, par cohérence ; à confirmer visuellement.

### `ambientocclusion` vient de la racine, pas de la feuille

Le format dit « only works on Parent file ». Vérifié sur les assets 1.20.1 :
tout modèle qui déclare `ambientocclusion` est lui-même une racine de chaîne.
C'est la valeur de la **racine** qui est retenue. Sans cela, `block/cross`
perdrait son `false` et l'herbe recevrait un éclairage lissé qu'elle ne doit
pas avoir.

### Autres points tranchés par les données

- **`cullface: "bottom"`** est une orthographe acceptée de `"down"`, et pas une
  hypothèse : quatre faces des assets 1.20.1 l'utilisent.
- **`"elements": []`** ne veut pas dire « hérite » : la liste la plus proche
  dans la chaîne remplace celle du parent, vide comprise.
- **Une clé de variante vide (`""`)** est une vraie clé, celle des blocs à un
  seul modèle. Un parseur JSON qui confond « clé vide » et « clé absente » perd
  tous ces blocs.
- **Les variantes pondérées sont toutes conservées**, jamais choisies ici :
  vanilla tire au sort d'après la position du bloc, donc seul le mailleur, qui
  connaît la position, peut choisir — et il doit choisir la même à chaque
  remaillage.

### Vérification de bout en bout

```
$ ov_modelbake run/assets
blockstate files .....   1005  (0 failed)
model references .....   6081
models resolved ......   6081  (0 failed)
distinct models cached   1810
quads baked ..........  62227
missing sprites ......      0
```

Aucun asset n'est commité : l'outil lit `run/assets`, produit par
`ov_assetimport` et exclu par `.gitignore`.

### Occlusion ambiante et lumière lissée

La règle par sommet vient de trois voisins de chaque coin — les deux côtés dans
le plan de la face, et la diagonale :

```
si côté1 et côté2 : 0
sinon             : 3 − (côté1 + côté2 + diagonale)
```

Le cas particulier « deux côtés pleins → 0 quelle que soit la diagonale » n'est
pas un détail : c'est lui qui creuse un angle rentrant au lieu de le gonfler.
La lumière par sommet moyenne les quatre valeurs mais **saute les voisins
opaques** — un bloc opaque a une lumière de 0, et le compter assombrirait le
sommet deux fois, une fois par l'occlusion et une fois par la moyenne. Les deux
fonctions doivent s'accorder sur ce qui est visible, sinon un sommet est à la
fois totalement occlus et pleinement éclairé.

### Vertex de 8 octets : ce qui n'y tient pas

Le budget est une contrainte, pas une préférence : à 12 chunks de distance, ce
sont les **pics d'upload en déplacement** qui décident du p99, pas le FPS moyen.

```
 0-10  x       11 bits   1/64 de bloc, plage [-8, 24[
11-21  y       11 bits
22-32  z       11 bits
33-40  u        8 bits   1/8 d'unité de sprite
41-48  v        8 bits
49-52  sky      4 bits
53-56  block    4 bits
57-58  ao       2 bits
59-61  facing   3 bits   0-5 une Direction, 6 « non ombré »
62-63  tint     2 bits
```

Deux décisions à assumer :

1. **L'échelle des `uv` est 1/8 d'unité de sprite, pas 1/16.** Un premier
   découpage à 1/16 rendait `u = 16` — la valeur la plus fréquente du corpus,
   celle de toute face pleine — non représentable exactement, ce qui rétrécit
   chaque face d'un fragment de texel. Le test l'a attrapé avant le renderer.
2. **Il n'y a pas de couleur de sommet.** Vanilla multiplie la teinte de biome
   dedans côté CPU ; trois octets de plus ne rentrent pas. Le champ `tint`
   nomme le canal (herbe, feuillage, eau) et le shader va chercher la couleur.
   C'est un mécanisme **différent** de celui de vanilla, choisi pour le budget,
   et valide tant que la teinte est constante sur un quad — ce qu'elle est pour
   un bloc.

### Ce qui n'est pas encore vérifié

- La chiralité absolue de la table `uv` (voir plus haut).
- Le signe de la rotation d'élément.
- Le comportement exact d'`uvlock` sous une rotation en `x`. L'implémentation
  est générique — elle défait la rotation subie par la base tangente de la face
  — et le cas `y` est verrouillé par un test qui vérifie la définition même
  (« un point du bloc qui n'a pas bougé garde sa coordonnée de texture »). Le
  cas `x` n'a pas d'équivalent mesurable sans image de référence.

Ces trois points se tranchent en une session dès que le renderer affiche un
chunk, et ils sont listés ici pour ne pas être oubliés.

---

## Connexions : pourquoi la mesure ne suffit pas, et ce qu'elle a montré

La règle qui décide si une clôture s'accroche à son voisin est, en vanilla,
`(le voisin n'est pas une exception ET il présente une face pleine) OU c'est une
clôture de la même famille OU c'est un portillon bien orienté`. La partie qui
manque est la **face pleine**, et elle dérive de la forme de collision.

Le prédicat composite, lui, se mesure : une clôture au centre d'une cellule, le
même état sur ses quatre côtés, et les quatre booléens de la clôture donnent les
quatre faces de cet état d'un coup
(`scripts/measure_sturdy.py`). Le relevé sur les 23 569 états posables donne
**23 358 faces**, dont 7 054 accrochent, sur **841 blocs**, et **zéro
contradiction sur la quasi-totalité** — douze en tout, imputables au fait que le
premier relevé tournait sans figer les ticks aléatoires, ce que le script fait
désormais.

**Et c'est le résultat qui a décidé de ne rien livrer.** Sur les 841 blocs
touchés, 695 répondent la même chose sur tous leurs états et toutes leurs faces
— mais **146 varient selon l'état**, et ce sont les escaliers, les portes, les
trappes, les portillons, la tête de piston. C'est-à-dire exactement les blocs
avec lesquels on construit. Une table par bloc serait juste 695 fois et fausse
là où ça se voit du premier coup d'œil.

La couverture par état est par ailleurs limitée à 6 281 des 23 569, pour une
raison de principe : le jeu **ajuste** le voisin qu'on vient de poser — un
escalier recalcule sa forme, une clôture ses connexions — donc un état comme
`shape=inner_left` ne peut pas être dicté, seulement obtenu. Les réponses sont
attribuées à l'état **observé**, jamais à celui demandé, et le reste n'est pas
atteignable par ce montage.

Les connexions attendent donc les formes de collision, qui sont déjà le blocage
nommé du mouvement autoritatif. Le relevé est conservé : il servira d'oracle le
jour où la règle sera écrite.

---

## Refuser un monde qu'on ne sait pas lire

Un chunk illisible était jusqu'ici **remplacé** par du terrain généré, puis
sauvegardé par-dessus. Sur un monde de joueur, c'est une perte de données que
rien ne signale.

Deux garde-fous, donc. Au démarrage, `level.dat` est lu et sa `DataVersion`
comparée à 3465 : un monde d'une autre version fait refuser l'ouverture, avec la
version trouvée dans le message. Et par chunk, un fichier présent mais illisible
— mauvaise version, ou décodage en échec — sert du terrain généré pour que le
joueur ne tombe pas dans un trou, mais la position est marquée **jamais
réécrite**.

Le piège pendant l'écriture : `level.dat` est gzippé et `nbt::read` attend du NBT
brut. Lui passer les octets compressés échoue en silence, ce qui se lit comme
« aucune version déclarée » — et le garde-fou refusait alors **tous** les mondes,
y compris ceux qu'on venait d'écrire. C'est le test du cas positif qui l'a
attrapé, pas celui du cas négatif, qui passait très bien.

---

## Formes de collision : une source tierce, vérifiée par une mesure indépendante

Les formes de collision sont du code Java. **PrismarineJS/minecraft-data** (MIT,
autorisé par `CLAUDE.md`) les publie **par état**, ce qui est la bonne
granularité — c'est de là que vient la table.

La question était comment la vérifier. La réponse était déjà sur l'étagère : une
face est « pleine » quand la tranche de la forme sur cette face couvre le carré
entier, et c'est **exactement** le prédicat qui décide si une clôture s'accroche
— qu'on avait mesuré face par face sur un vrai serveur, 23 358 fois.

### Ce que la confrontation a donné

En reconstruisant la règle de connexion à partir des seules formes :

| | |
|---|---|
| face pleine seule | 22 341 / 23 358 — **95,6 %** |
| + la règle de famille (clôtures entre elles, portillons perpendiculairement) | 23 043 |
| + la liste d'exceptions **dérivée des écarts restants** | 23 350 |
| + la clôture en brique du Nether, qui ne fréquente qu'elle-même | **23 358 / 23 358** |

Autrement dit, les deux classes de désaccord initiales n'étaient pas des erreurs
mais les deux **autres termes de la règle**, et elles se sont laissé lire
directement dans les écarts. La liste d'exceptions n'a donc pas été recopiée
d'ailleurs : elle a été **mesurée**. Elle contient les feuillages, les shulker
box, la barrière, les citrouilles, le melon, la lanterne — et `target`, que rien
n'aurait fait deviner.

Deux blocs, `mud` et `soul_sand`, accrochent alors que leur boîte de collision
est plus basse d'un seizième : vanilla interroge la forme de **support**, pas
celle de collision, et pour ces deux-là elles diffèrent. Ce sont les deux
derniers désaccords sur 23 358, et rien d'autre dans le jeu ne fait ça.

### Le format

Toutes les coordonnées tombent sur une grille de 1/32 — l'émetteur le vérifie au
lieu de le supposer — donc un octet **signé** par coordonnée : une boîte déborde
parfois du cube, une tête de piston étendue va de -8 à 48. 4327 formes, 12054
boîtes, plus un masque de six bits par forme pour les faces pleines, calculé à
la compilation parce que c'est une grille de 32 × 32 par face et que la question
se pose à chaque bloc posé.

### Et le rejeu par le code livré

Le prototype qui a dérivé la règle et le code qui tourne sont deux choses
différentes, donc le corpus est rejoué à travers le second :
`ov-inspect connects` répond pour chaque paire (état, face) et redonne
**23 358 / 23 358**.

Une erreur s'y est fait prendre au passage, et elle est instructive : la première
version du rejeu tombait à 87 %, les portes en tête. Le corpus note la face **du
voisin**, et l'outil la réinversait une seconde fois. Sur un bloc symétrique
l'inversion ne se voit pas ; sur une porte, si. Un test qui n'aurait porté que
sur de la pierre serait passé.

### Ce qui est livré, et ce qui ne l'est pas

Clôtures, portillons, vitres et barreaux se connectent, et se reconnectent quand
un voisin change — le changement va dans les deux sens, donc casser un bloc fait
aussi lâcher prise à la clôture qui le tenait.

Les **murets** sont reconnus et pas encore remodelés : ils ont trois valeurs par
côté au lieu de deux, et `low` contre `tall` dépend de ce qu'il y a au-dessus.
La **forme des escaliers** attend de même. Les deux sont maintenant à portée,
puisque les formes sont là.

---

## Forme des escaliers : un espace assez petit pour être mesuré en entier

Un escalier a cinq formes et il en change tout seul quand un escalier voisin
apparaît. La règle est du code Java — mais l'espace des cas est petit et fermé,
donc il a été relevé **exhaustivement** plutôt que déduit.

`scripts/measure_stairs.py` pose l'escalier du centre puis le voisin — c'est le
second qui déclenche le recalcul — et relit le centre. Quatre orientations, deux
moitiés, quatre côtés, quatre orientations et deux moitiés pour le voisin :
**256 cas, tous lus**. 224 restent droits ; les 32 autres se répartissent en
quatre coins, huit chacun.

Le motif est parfaitement régulier :

* un coin ne se forme que si le voisin est **devant** ou **derrière**, sur la
  **même moitié**, et **tourné en travers** ;
* devant donne un coin sortant, derrière un coin rentrant ;
* gauche ou droite suit le sens de la rotation, le sens antihoraire étant la
  gauche — nord/ouest, sud/est, ouest/sud, est/nord.

### Le troisième bloc

Vanilla annule le coin quand un troisième escalier s'en mêle, et la première
passe ne le voyait pas. Une seconde passe rejoue les 32 coins avec un escalier
supplémentaire sur chacune des deux directions perpendiculaires : **32 coins
empêchés, 32 conservés**. Exactement une des deux directions annule — celle
opposée à la rotation pour un coin sortant, celle de la rotation pour un coin
rentrant — et le bloc qui annule doit regarder dans notre direction, sur notre
moitié.

### Rejeu

Le prototype et le code livré sont deux choses différentes, donc les 256 cas
sont rejoués à travers le second par `ov-inspect stairs` : **256 / 256**. Le cas
du troisième bloc, que cet outil ne monte pas, est couvert par un test unitaire
qui reprend les quatre situations mesurées.

---

## Murets : une dimension de plus, et une surprise

Un muret porte trois valeurs par côté — `none`, `low`, `tall` — plus un poteau,
et les deux dépendent de ce qu'il y a **au-dessus** autant que des voisins.
C'est la dimension qui manquait aux clôtures.

Le relevé croise six configurations de voisinage (aucun, un, deux opposés, deux
adjacents, trois, quatre) avec six blocs au-dessus, puis sept sortes de voisin
avec deux blocs au-dessus. Ce qu'il donne :

**Le côté monte à `tall` exactement quand le bloc au-dessus remplit sa propre
face inférieure.** Pierre et dalle basse le font, une torche et le poteau d'un
autre muret non — et c'est le prédicat de face pleine qu'on avait déjà, pris
vers le bas.

**Le poteau disparaît quand les connexions sont symétriques sur les deux axes.**
Une ligne droite le supprime, un coin le garde, un T le garde — et une croix
complète le supprime aussi, ce qui est la surprise : quatre côtés connectés se
comportent comme deux. La lecture qui explique tout le tableau est
« nord vaut sud **et** est vaut ouest », le cas sans aucune connexion mis à part.

Le dernier terme est une donnée, pas du code : le tag `wall_post_override` — 73
blocs, dont la torche et tous les panneaux — remet le poteau quoi qu'en disent
les connexions. Il est donc lu, pas listé.

### Ce que le test fonctionnel a attrapé

Deux murets côte à côte sortaient justes du premier coup. Poser une pierre
**au-dessus** de l'un d'eux ne le faisait pas monter à `tall` : le serveur ne
remodelait que les quatre voisins horizontaux. Un muret lit ce qu'il a sur la
tête, donc tout bloc posé doit redonner sa chance à celui d'en dessous. Le test
unitaire, lui, passait — il appelait la règle directement, avec le bloc du
dessus déjà en main.

---

## Collisions : le pont entre un état de bloc et une boîte

La partie mathématique existait déjà dans `ov_math` — boîtes, balayage, découpe
par axe. Ce qui manquait était le pont : transformer un état de bloc en les
boîtes qu'il occupe, et interroger le monde plutôt qu'une liste.

Le monde est lu par un pointeur de fonction et un contexte, pas par un patron :
le header est public, et un patron y traînerait le type de monde de l'appelant
dans chaque unité de traduction qui l'inclut.

Trois choses que les tests vérifient et qu'une implémentation plausible rate :

* **Toucher une face n'est pas se chevaucher.** Sans cela un joueur s'enfonce
  dans le sol au lieu de s'y poser. Debout à `y = 1.0` sur un bloc : libre. À
  `0.9` : dedans.
* **Une dalle basse arrête à mi-hauteur**, donc on s'y tient à `0.5`. Lire une
  seule boîte par bloc placerait le joueur un bloc trop haut, et une dalle haute
  laisserait un vide sous elle.
* **Un escalier est deux boîtes.** Le test vérifie qu'à mi-hauteur, un coin est
  libre et l'autre non — une seule boîte rendrait les deux identiques.

Le déplacement se résout **axe par axe, dans l'ordre Y, X, Z**. Marcher en
diagonale contre un mur coupe le X et laisse passer le Z, ce qui est glisser le
long du mur ; tester le déplacement entier d'un coup arrêterait les deux, et
rendrait les escaliers infranchissables.

### Premier usage : ne plus poser un bloc dans quelqu'un

Le serveur acceptait jusqu'ici de poser un bloc à l'endroit exact où se tient un
joueur. Il refuse désormais, et — c'est la moitié qui compte — il **le dit** : le
client avait prédit la pose, donc sans le paquet de correction il continuerait
d'afficher un bloc qui n'existe pas.
---

## Le renderer : `ov_rhi`, l'atlas, le mailleur (`ov_rhi`, `ov_render`, `ov_client`)

### Le SDK Vulkan, tel qu'il est réellement installé

`1.4.357.1` (LunarG, macOS). Le loader et les ICD sont posés dans `/usr/local`,
donc **rien n'a besoin d'exporter `VULKAN_SDK`** : `libvulkan.dylib` est sur le
chemin de `dyld` et le loader trouve seul `/usr/local/share/vulkan/icd.d`.
`glslc` et `glslangValidator` sont dans `/usr/local/bin`. `cmake/OvShaders.cmake`
cherche quand même `$ENV{VULKAN_SDK}/bin` en premier, pour la machine où
l'installation n'est pas globale.

⚠️ **Deux pilotes répondent pour le même GPU** : MoltenVK 1.4.2 et
`DRIVER_ID_MESA_KOSMICKRISP`, tous deux annonçant « Apple M2 ». Prendre le
premier énuméré ferait dépendre le comportement du renderer de l'ordre
d'installation des ICD. `ov_rhi` classe les devices et préfère explicitement
MoltenVK, qui est la cible du projet et celui dont parle le risque R6.

### Ce que les couches de validation ont trouvé, et qui n'aurait pas été trouvé autrement

Cinq bugs réels, tous à la première exécution, tous invisibles autrement :

1. Un buffer `Upload` créé avec `TRANSFER_SRC` seul ne peut pas **recevoir** une
   relecture. La copie était silencieusement fausse.
2. Une capture d'écran prise *après* `end_frame` touche une image de swapchain
   qui n'est plus acquise. Corrigé en changeant l'API, pas en rustinant :
   `copy_swapchain_to_buffer` s'enregistre **dans** la frame propriétaire de
   l'image. Un state tracker automatique aurait masqué exactement ça.
3. Le `VkSwapchainKHR` n'était jamais détruit.
4. `HandlePool` est adossé à un `std::vector` : un pointeur obtenu par `get()`
   **dangle** dès l'insertion suivante. `upload_buffer` gardait le pointeur de
   destination pendant qu'il créait son buffer de staging. C'est précisément la
   faute que les handles existent pour rendre survivable — un pointeur périmé
   corrompt en silence, un handle périmé rend `nullptr`.
5. `discard` en GLSL compile vers `OpDemoteToHelperInvocation` en Vulkan 1.3, et
   demande `shaderDemoteToHelperInvocation`. Toute la couche cutout en dépend.

Un sixième, attrapé par un test et non par le pilote : `Camera::turn` corrigeait
le yaw d'**un** tour, donc un coup de souris rapide le laissait à 14 640°. Le
protocole envoie le yaw dans `[-180, 180]` ; ce n'est pas cosmétique.

### Le vertex de terrain fait 12 octets, pas 8

Le plan disait 8. En comptant les champs contre un vrai atlas :

| champ | bits | pourquoi |
|---|---|---|
| position, 3 axes | 33 | 1/64 de bloc sur 32 blocs |
| u, v dans l'atlas | 22 | un demi-texel sur un atlas 1024 |
| lumière ciel + bloc | 8 | les 4+4 de vanilla |
| occlusion ambiante | 2 | les quatre niveaux de vanilla |
| `facing` | 3 | six directions, plus « non ombré » |
| canal de teinte | 2 | aucun, herbe, feuillage, eau |
| **total** | **70** | |

Huit octets font 64 : il manque 6 bits. La première version tenait en donnant
**8 bits** aux coordonnées de texture, ce qui suffit pour un sprite et pas du
tout pour un atlas — 256 pas sur 1024 texels place chaque sommet sur une grille
de 4 texels, et chaque bloc du jeu aurait échantillonné le mauvais morceau de sa
propre texture. Le test de packing ne l'a pas vu : il ne demandait que « la
valeur revient-elle ». Il demande maintenant qu'un texel d'un atlas 1024 fasse
l'aller-retour exactement.

Douze octets laissent 26 bits libres — la place du second `uv` d'un overlay ou
d'une normale, sans nouveau changement de format.

**Huit octets restent atteignables**, et c'est noté pour ne pas perdre l'option :
une **palette de sprites par section**, un index de 6 bits, un `uv` local au
sprite, et la teinte déplacée dans l'entrée de palette — une teinte est une
propriété de matériau, pas de sommet. Cela échange une lecture dépendante dans
le shader contre 4 octets par sommet, et personne n'a mesuré si ça paie. Le faire
avant d'en avoir besoin, c'est finir avec un format astucieux et pas d'image.

### Une des trois lacunes de la session précédente est levée

Le tableau `uv` laissait trois questions ouvertes. L'image les tranche en partie.

✅ **L'orientation de `v` sur les faces latérales est confirmée.** La texture
`grass_block_side` porte sa frange verte **en haut**. Rendue par notre chaîne,
elle apparaît en haut. Un `v` inversé l'aurait mise en bas. `v = 16 − y` est donc
juste, et ce n'était pas discriminable sans image.

❌ **La chiralité absolue reste ouverte.** Un tableau entièrement miroité sur les
quatre côtés donnerait la même frange au même endroit ; il faut une capture du
client vanilla sur un bloc à texture asymétrique pour la trancher.

❌ **Le signe de la rotation d'élément reste ouvert.** Les seuls éléments tournés
de vanilla sont symétriques à ±45°, et la scène de démonstration n'en contient
aucun d'asymétrique.

### Décisions du RHI, et leurs raisons

- **La couture entre fenêtre et Vulkan est un `void*`.** Une surface a besoin
  d'une fenêtre et d'une instance au même instant. Remonter un `VkSurfaceKHR`
  mettrait Vulkan dans `ov_client` ; descendre un `NSWindow` mettrait de
  l'Objective-C dans `ov_rhi`. Le client passe son `GLFWwindow` en pointeur
  opaque, `ov_rhi` lie GLFW en privé pour le seul appel qui le convertit.
- **Deux frames en vol, pas trois.** La troisième coûte une frame de latence et
  une copie de plus de chaque buffer ; sur une mémoire unifiée, la ressource
  rare est la bande passante.
- **Un sémaphore `render_finished` par image de swapchain**, pas par frame en
  vol. Un sémaphore attendu par un present ne peut pas être réutilisé avant que
  l'image revienne ; l'indexer par frame est l'erreur que les couches signalent
  comme « semaphore already in use ».
- **Swapchain sRGB.** Les textures de Minecraft sont en sRGB, et les mélanger
  comme si elles étaient linéaires est la première raison pour laquelle une
  réimplémentation paraît délavée.
- **FIFO.** Le seul mode que toute implémentation doit supporter, et le tearing
  ne doit pas être un compromis pris par accident.
- **Timestamps GPU dès la première frame.** Un renderer qui les ajoute plus tard
  les obtient après les décisions qu'ils auraient dû éclairer.
- **`OV_FORBID_ov_rhi` excluait `ov_base`.** « Rien du jeu » avait été écrit
  `${OV_ALL_MODULES}`, ce qui interdisait aussi les entiers de largeur fixe et
  le log par lequel les couches de validation remontent. `ov_base` est
  l'exception unique, écrite en toutes lettres pour que `check_layers.py`, qui
  lit cette ligne littéralement, ne puisse pas diverger de CMake.

### Ce que la première image prouve

`ov_voxel --frames=10 --assets=run/assets --screenshot=…` construit une scène à
la main à partir de **vrais** blockstates 1.20.1, la maille, stitche l'atlas
depuis le pack, et dessine : 1666 quads, 12 sprites, atlas 128×128 avec 5
niveaux de mip, ~0,95 ms de GPU, zéro erreur de validation. Sur l'image : le
dégradé de terre et de pierre sous une surface d'herbe **teintée**, un escalier
en pierre taillée, deux chênes dont les feuilles sont en couche *cutout* — on
voit à travers —, une vitre, et l'ombrage directionnel par face.

Ce que cela vérifie d'un coup : la résolution des modèles, le bake, la table
`uv`, le stitching, les mips, le mailleur par face, le culling par `cullface`,
l'occlusion ambiante, le vertex packé, la caméra, et tout `ov_rhi`.

---

## Physique du joueur : le client comme oracle

C'est le seul système où l'implémentation de référence tourne **chez le
joueur**. Le client vanilla est la physique, et il annonce sa position vingt
fois par seconde. Le serveur enregistre donc ce qu'il reçoit
(`--record-motion`), et les constantes sont ajustées sur cette trace plutôt que
rappelées.

Le relevé demande une chorégraphie de deux minutes — rester immobile, marcher,
sprinter, sauter, sauter en sprintant, avancer accroupi, tomber de haut — et
donne 1238 positions.

### Ce que la chute donne

La plus longue chute libre de la trace fait 33 ticks. Le modèle est
`v ← (v − g) × k`, deux inconnues, résolues sur trois échantillons puis rejouées
sur les 33 :

| | |
|---|---|
| gravité | **0,079 998** |
| traînée verticale | **0,980 014** |
| erreur maximale sur 33 ticks rejoués | 3,7 × 10⁻⁴ |

L'ordre compte autant que les valeurs : la gravité **puis** la traînée. Le
premier tick d'une chute vaut 0,0784, et non 0,08 — l'inverse donnerait
0,0784 aussi au premier tick et divergerait ensuite.

Un détail du protocole se lit au passage : la trace **commence** à 0,0784, alors
que le premier tick d'une chute ne déplace rien du tout. Le client n'envoie pas
de paquet pour un tick où il n'a pas bougé. Le test l'écrit noir sur blanc,
faute de quoi l'implémentation serait décalée d'un tick pour de bon.

### Ce que la marche donne

Les plateaux tenus plusieurs dizaines de ticks, au sol et à altitude constante :

| | par tick | par seconde |
|---|---|---|
| marche | **0,215 78** | 4,316 |
| sprint | **0,280 58** | 5,612 |
| accroupi | **0,064 74** | 1,295 |
| premier tick d'un saut | **0,420 000** | — |

### Le terme qui manquait

Le modèle reconstruit à partir des constantes évidentes — poussée 0,1, friction
`0,6 × 0,91` — prédit un état stable de **0,2203** par tick. La mesure dit
0,21578. Quatre pour cent d'écart : assez pour être faux, assez peu pour passer
inaperçu.

La [documentation de la Minecraft Parkour Wiki](https://www.mcpk.wiki/wiki/Horizontal_Movement_Formulas)
donne le terme manquant : les impulsions d'entrée sont multipliées par **0,98**
avant tout le reste. Avec lui, `0,1 × 0,98 / (1 − 0,546) = 0,215 86` — contre
0,215 78 mesuré. Sprint : 0,280 62 contre 0,280 58. Accroupi : 0,064 76 contre
0,064 74.

Les deux sources ont été obtenues séparément et se rejoignent à la quatrième
décimale. C'est exactement l'usage qu'on veut d'une documentation : elle nomme
un terme qu'une mesure seule aurait laissé dans le bruit, et la mesure confirme
qu'il s'agit bien de celui-là.

---

## Émission lumineuse : lire ce que le jeu a écrit

Combien de lumière un bloc donne n'est nulle part dans les rapports officiels.
Mais le jeu écrit la lumière qu'il calcule dans chaque chunk qu'il sauvegarde,
et **la valeur dans la case du bloc est son émission**. Il suffit donc d'une
salle sans ciel, d'une cellule par bloc, et de relire.

Deux tentatives ont échoué avant celle-ci, chacune pour une raison qui vaut
d'être notée.

**Les cellules trop proches.** Une première version les espaçait de quelques
blocs. La lumière ne s'additionne pas — elle prend le maximum — mais deux
sources à portée l'une de l'autre donnent quand même un relevé qui n'est pas
celui qu'on croit. Dix-sept blocs d'écart : une source de quinze à seize cases
ne contribue plus rien.

**La coque trop juste.** La salle est creusée dans un bloc de pierre qui
débordait de quatre blocs. Or la lumière du ciel entre par la tranche et se
propage quinze cases vers l'intérieur : toutes les cellules de bordure lisaient
la somme d'une émission et d'un reste de jour. Cinquante-quatre blocs sortaient
faux, **dont la torche des âmes, qui éclaire à dix et rendait zéro** — une
valeur parfaitement plausible pour un bloc dont on ne sait rien. Avec vingt
blocs de marge, il en reste vingt-sept, et ce sont exactement ceux qu'on ne peut
pas poser sur un sol de pierre : cactus, coraux, éventails muraux.

Deux limites du jeu ont aussi coûté une passe : `forceload` plafonne à 256
chunks par commande et `fill` à 32 768 blocs. Les dépasser échoue **en
silence**, et le relevé rend alors une salle à moitié creusée dont la moitié des
cases n'existe pas.

### Par état, comme le reste

Une bougie éclaire à trois fois leur nombre, un minerai selon qu'il est allumé,
une ancre de résurrection selon sa charge. Une seconde passe donne une cellule
à chaque état des soixante familles concernées — 479 états — et **corrige 241
valeurs** que la passe par bloc avait laissées à celle de l'état par défaut.

**23 282 états sur 24 135.** Les valeurs sont celles qu'on attend une fois
qu'on les voit : verre luisant 15, torche 14, tige d'ender 14, obsidienne
pleureuse 10, table d'enchantement 7, torche de redstone 7, bloc de magma 3,
alambic 1.
---

## Le monde réel à l'écran : registre → modèles → sections (`ov_render`)

L'échafaudage a été retiré. La scène écrite à la main est remplacée par une
lecture directe des fichiers de région : `run/world` est la sauvegarde 1.20.1 du
joueur (`DataVersion` 3465), et tout ce qui s'affiche vient d'elle.

Chaîne complète : `BlockStateId` → nom du bloc et propriétés reconstruites depuis
l'id mixed-radix → `blockstates/<bloc>.json` → variantes ou multipart → modèles
bakés → sprites → atlas → mailleur par section → quads.

### L'occlusion demande la forme **et** l'opacité, et ça s'est vu

`NeighbourhoodView::occludes` de la scène de démonstration répondait « le bloc
est-il plein ? ». C'est faux pour tout ce qui n'est pas un cube : une dalle
aurait caché le sol sous elle. Le pack porte maintenant les formes de collision
par état avec un masque de faces pleines précalculé, vérifié — la règle de
connexion des clôtures reconstruite à partir de lui reproduit 23358 faces sur
23358 mesurées sur un vrai serveur.

**Mais `face_is_sturdy` seul ne suffit pas non plus, et c'est un test raté qui
l'a montré.** Le test affirmait que le verre n'est pas *sturdy* ; il l'est —
le verre remplit son cube. Un mailleur qui s'arrêterait à la forme supprimerait
donc tout ce qui se trouve derrière une fenêtre. La règle correcte demande les
deux :

```
occlus = face_is_sturdy(état, face)  ET  blocks_sky_light(bloc)
```

Les deux colonnes sont des données mesurées, pas des heuristiques. Le test a été
corrigé dans le sens inverse de ce qu'on attendait, ce qui est le seul cas où un
test apprend quelque chose.

### Les fluides n'ont pas de modèle, et c'était 30 % de l'image

Premier rendu du vrai monde : 29,77 % des pixels étaient exactement la couleur
d'effacement. Du ciel **à travers** le sol. La cause est dans les assets :

```
$ cat run/assets/assets/minecraft/models/block/water.json
{ "textures": { "particle": "block/water_still" } }
```

`block/water.json` déclare une texture de particule et **aucune géométrie**.
Vanilla dessine les fluides avec un moteur à part, qui lit le niveau des huit
colonnes autour de chaque coin et incline la surface vers l'écoulement. Sans
lui, l'océan et les rivières ne sont pas absents « par bug » : ils n'ont
simplement jamais été demandés à personne.

La géométrie est donc synthétisée ici : une boîte de 14/16 de haut pour une
source (c'est pourquoi on se tient légèrement sous la ligne d'eau dans le jeu),
pleine pour un fluide qui tombe (`level` ≥ 8). Ce qui **n'est pas** fait est
écrit tel quel : hauteurs de coin interpolées et direction d'écoulement.

Un corollaire non évident : deux cellules du même fluide partagent une face
qu'on ne voit jamais, et le test d'occlusion ordinaire ne peut pas la supprimer
— l'eau est transparente, donc il refuse *correctement* de cacher quoi que ce
soit derrière elle. Sans une règle « même fluide », un océan dessine toutes ses
faces internes. D'où `NeighbourhoodView::fluid_at`.

### La couche de rendu est lue dans les pixels, pas dans une table

Vanilla garde la couche de chaque bloc dans une table écrite à la main en Java.
Ici elle est déduite de l'alpha des sprites du modèle : entièrement opaque →
`solid`, alpha franc (0 ou 255) → `cutout`, alpha partiel → `translucent`.
C'est data-driven et ça s'accorde avec vanilla sur ce qui compte.

⚠️ **Ce que ça ne peut pas retrouver** : la distinction entre `cutout` et
`cutout_mipped`. Vanilla met le verre et les barreaux dans `cutout` pour que le
mipmapping ne mange pas leurs cadres d'un pixel, et les feuilles dans
`cutout_mipped`. C'est une décision de rendu, pas une propriété de la texture.
Tout ce qui a un alpha franc va pour l'instant dans `cutout_mipped`.

Les fluides sont exclus de ce classement : `water_still` est une texture
totalement opaque et serait classée `solid`, ce qui est exactement l'inverse.

### La lumière vient du chunk

`BlockLight` et `SkyLight` sont lus dans les sections, en YZX. Une section sans
tableau de lumière est traitée comme du plein jour plutôt que comme du noir, et
l'outil dit lequel des deux cas s'est produit : « une sauvegarde sans lumière »
et « un mailleur cassé » donnent la même image noire, et il faut pouvoir les
distinguer sans deviner.

### Mesures sur le vrai monde

`ov_voxel --radius=6 --at=0,0` sur `run/world`, Apple M2, MoltenVK :

| | |
|---|---|
| chunks lus | 169, 0 échec |
| états rencontrés | 216 sur 24135 — d'où la résolution **paresseuse** |
| sprites | 98, atlas 512×512, 5 niveaux de mip |
| sections avec géométrie | 2772 |
| culling frustum | 1643 dessinées sur 2772, soit 41 % supprimées avant tout binding |

Le culling frustum est la première grosse économie et elle est du bon côté du
compromis : un test de plan contre une boîte, sur le CPU, avant qu'on lie quoi
que ce soit. Le culling GPU par compute que le plan mentionne vient **après**
que celui-ci ait été mesuré et jugé insuffisant, pas avant.

⚠️ **Le p50 CPU de 16,7 ms n'est pas du travail, c'est l'attente du vsync.** Le
mode de présentation est FIFO, donc la boucle bloque à 60 Hz ; le chiffre à
regarder est le GPU. Mesurer le CPU utilement demandera un mode sans
synchronisation verticale, et c'est noté comme non fait.

### La cible de sortie du jalon, mesurée — et pas atteinte

`ov_voxel --radius=12` en **RelWithDebInfo**, sans couches de validation, sur le
vrai monde, 200 frames dont 190 comptées :

| | |
|---|---|
| sections avec géométrie | 9674 |
| quads | 5 314 101 |
| sommets sur le GPU | 243,3 Mio |
| dessinées après culling | 4312 sur 9674 (55 % supprimées) |
| **CPU p50 / p99 / max** | 16,66 / **32,52** / 34,80 ms |
| **GPU p50 / p99 / max** | 9,19 / **16,15** / 20,28 ms |

**Le critère de sortie est p99 ≤ 20 ms à 12 chunks. Il n'est pas tenu** : 32,5 ms
côté CPU. Le GPU passe de justesse.

La mesure désigne le coupable sans ambiguïté, et c'est exactement celui que le
plan avait nommé : **4312 draws individuels par frame**, chacun avec son bind de
vertex buffer et son push constant, plus **9674 `VkBuffer` distincts**. C'est
pour cela que le plan prévoit l'arène device-local de 384 Mo, le sous-allocateur
en pages de 4 Ko et `drawIndexedIndirect` — un draw par couche au lieu de
plusieurs milliers. Ce travail est maintenant **justifié par une mesure** plutôt
que par une intuition, ce qui était la condition posée.

Note au passage : 243 Mio de sommets pour un rayon de 12 tient sous les 384 Mo
de l'arène prévue, donc le budget du plan était bon.

### Le piège du `PositionMin`

Le vertex packé couvre `[-8, 24)` bloc **relatif à l'origine de la section**, et
c'est ce qui rend la position d'une section un push constant plutôt qu'une
coordonnée absolue : une coordonnée monde y = 319 n'entre pas dans 16 bits à
1/2048 de bloc. Le mailleur émet donc en local et la matrice ajoute l'origine.

## Le terrain en trois appels — et une mesure à refaire

*2026-09-08.*

### D'abord : la mesure précédente était fausse

La section ci-dessus conclut « **le critère de sortie n'est pas tenu** : 32,5 ms
côté CPU ». Ce chiffre a été relevé dans un build **debug**, et sous **vsync**.
Les deux invalident la conclusion, chacun pour sa propre raison :

- en debug, `-O0` ; ce n'est pas le binaire dont le jalon parle ;
- sous FIFO, chaque frame attend le rafraîchissement. Un renderer qui travaille
  4 ms et un qui en travaille 15 rapportent tous les deux 16,67 ms. Le p50 de
  16,66 ms de la mesure précédente n'est pas une coïncidence : c'est
  exactement 1/60 s. **On mesurait l'écran.**

Refaite en release, la même scène donne un p99 CPU de **17,83 ms** — sous les
20 — et un p99 GPU de **12,28 ms**. Le critère était déjà tenu, et le chiffre
qui le disait ne mesurait rien.

Deux corollaires, tirés depuis :

1. `--no-vsync` existe maintenant, et c'est un **instrument de mesure, pas un
   réglage de vitesse**. `DeviceDesc::vsync` le dit dans son commentaire.
2. Le chronomètre part **après** `begin_frame`. Acquérir une image attend
   l'écran *et* la frame que le GPU n'a pas finie ; l'inclure fait rapporter le
   coût du GPU comme s'il était celui du CPU. Avec le chronomètre au mauvais
   endroit l'enregistrement semblait coûter 3,2 ms ; au bon, 0,13.

### Ce qui restait à faire, et ce qu'il rapporte

Le vrai coût, une fois mesuré proprement, était bien celui que le plan avait
nommé : 2491 draws, chacun avec son bind de vertex buffer et son push constant,
puisés dans **9674 `VkBuffer` distincts**. Soit **5,64 ms p50 / 10,49 ms p99**
d'enregistrement CPU, pour un budget de frame de 20.

Les sommets vivent désormais dans **une seule arène device-local de 384 Mio**
(259 utilisés à 12 chunks, en un seul bloc), et le terrain se dessine en **un
`vkCmdDrawIndexedIndirect` par couche**.

Décomposition A/B, même monde, même caméra, même arène, seul le chemin de
soumission change (`--no-indirect`) :

| chemin | draws | enregistrement CPU p50 / p99 |
|---|---|---|
| un `VkBuffer` par section, un draw chacun | 2491 | 5,64 / 10,49 ms |
| arène partagée, un draw par section | 2491 | 2,22 / 2,76 ms |
| arène partagée, **indirect par couche** | **3** | **0,12 / 0,83 ms** |

L'arène supprime le rebind ; l'indirect supprime le reste. Les deux moitiés se
voient séparément, ce qui est le seul moyen de savoir laquelle a servi.

Sous vsync — c'est-à-dire tel que le jeu tourne — à 12 chunks en 2560×1440 sur
M2 : **CPU p99 17,77 ms**, dont **0,41 ms d'enregistrement**, et **GPU p99
10,91 ms**.

### Deux points de spécification

**La page de l'allocateur ne peut pas faire 4 Ko.** Le plan l'annonçait ainsi.
Mais le `vertexOffset` d'un `VkDrawIndexedIndirectCommand` compte des
**sommets**, pas des octets — c'est ce qui permet à toutes les sections de
partager le même index buffer — donc toute allocation doit commencer sur un
sommet entier. 4096 n'est pas un multiple de 12. **3072 l'est** : 256 sommets,
64 quads. Source : Vulkan 1.3, `VkDrawIndexedIndirectCommand`.

**L'origine de section n'est plus poussée, elle est lue.** Un seul appel
indirect dessine toutes les sections d'une couche : il n'existe plus d'instant
entre deux draws où le CPU pourrait pousser quoi que ce soit. Le seul canal
par-draw d'une commande indirecte qui atteigne le vertex shader est
`firstInstance`, et Vulkan définit `gl_InstanceIndex` comme le numéro
d'instance **plus** `firstInstance` — donc avec une instance par commande, il
vaut exactement le slot où le CPU a écrit l'origine. Les origines sont dans un
storage buffer indexé par lui. Source : Vulkan 1.3, « Built-in Variables »,
`InstanceIndex`, et `VkDrawIndexedIndirectCommand::firstInstance`.

Cela demande `multiDrawIndirect` **et** `drawIndirectFirstInstance`, deux
features optionnelles. Elles ne sont demandées que si le pilote les annonce, et
`DeviceInfo::indirect_first_instance` dit lesquelles ont été obtenues ; sinon le
renderer retombe sur un draw par section — qui garde l'arène et la lecture
d'origine, et ne perd que l'appel unique. MoltenVK sur M2 les fournit.

## Les couleurs de biome, vérifiées contre quinze valeurs publiées

*2026-09-08.*

Jusqu'ici, chaque bloc d'herbe et chaque feuille du monde prenait **la même
couleur** : le shader avait deux bits nommant un canal — herbe, feuillage, eau —
et lisait une constante. Une prairie, une jungle et une taïga étaient du même
vert. Ce n'était pas une approximation, c'était une couleur unique répétée.

Vanilla fait tout autre chose, et c'est vérifiable de bout en bout :

1. `water_color` est **stocké** dans le fichier du biome, en RGB exact ;
2. l'herbe et le feuillage sont **échantillonnés dans une texture du resource
   pack**, `colormap/grass.png` et `colormap/foliage.png`, à un point donné par
   la température et les précipitations du biome ;
3. quelques biomes remplacent l'échantillon (badlands, cherry grove) et deux le
   modifient après coup (dark forest, swamp) ;
4. la couleur affichée est la **moyenne des 25 cellules de biome autour du
   bloc**, à sa propre hauteur — le rayon de mélange par défaut est 2.

La règle d'échantillonnage :

```
t = clamp(temperature, 0, 1)
r = clamp(downfall, 0, 1) * t          ← multiplié par t, d'où le triangle
x = (int)((1 - t) * 255)
y = (int)((1 - r) * 255)
```

**Vérification.** Quinze couleurs d'herbe sont des valeurs publiées, citées
indépendamment de tout code. Les quinze reviennent exactement — prairie, forêt,
jungle, désert, badlands, plaines enneigées, taïga, savane, forêt de bouleaux,
forêt sombre, océan, collines balayées, champs de champignons, vieille pinède, et
marais. Si l'ordre des axes, la mise à l'échelle par la température ou l'un des
deux modificateurs était faux, le compte tomberait.

### Le piège du f32

Une des quinze ne passait pas : **birch_forest**, 0x88BB66 au lieu de 0x88BB67.
Un cran de bleu.

L'index tronque. Pour une température de 0,6 :

| | 1 − t | ×255 | index |
|---|---|---|---|
| double | 0,4 | 102,000000000000014 | **102** |
| float | 0,39999998 | 101,99999 | **101** |

Deux colonnes différentes du colormap, donc deux couleurs. La valeur publiée est
celle de la colonne 102, et sur l'ensemble des couleurs vérifiées le double en
donne 12 sur 12 contre 11 sur 12 pour le float. **Le climat est donc stocké en
f64 dans le pack** — une exigence mesurée et pas de la prudence, et le seul
biome du jeu où cela se voit.

### Ce qui reste faux, et le reste sciemment

- **Le marais.** Le modificateur `swamp` choisit entre deux valeurs selon un
  champ de bruit, si bien qu'un marais est vert par plaques. Le bruit exact
  n'est documenté nulle part que ce projet ait le droit de lire, donc la plus
  commune des deux valeurs est prise pour tout le biome. Un marais est donc
  uniformément du bon vert au lieu de l'être par plaques. Écrit ici pour que ce
  soit corrigé par mesure et non deviné.
- **L'épicéa et le bouleau** prennent une constante et ignorent le biome — un
  épicéa dans une jungle est du vert sombre d'une taïga. C'était noté « pas
  encore distingué » dans le code ; ce sont maintenant deux canaux à part,
  0x619961 et 0x80A755.

### Le coût

Le sommet est passé de 12 à 16 octets pour porter la couleur, soit **+81 Mio
dans l'arène à 12 chunks** (259 → 340). Les solutions sans coût mémoire ont été
examinées et écartées : il ne reste pas trois bits libres dans le mot 2, et
reprendre de la précision aux coordonnées d'atlas rejouerait exactement le bug
documenté plus haut. L'arène passe donc de 384 à 512 Mio.

## La lumière, le jour et le brouillard

*2026-09-08.*

Le shader calculait la luminosité ainsi :

```glsl
float light = max(float(sky), float(block)) / 15.0;
```

Une droite. Le jeu n'en utilise pas.

### La courbe — documentée et vérifiée

```
brightness(L) = ambient + (1 - ambient) · f / (4 - 3f),   f = L/15
```

`ambient` vaut **0** dans l'overworld et l'End, **0,1** dans le Nether — lu dans
nos propres `dimension_type/*.json`, pas supposé.

Les seize valeurs sont publiées. Les seize reviennent à 1e-7 près. L'écart avec
l'ancienne droite n'est pas cosmétique : **au niveau 7, 18 % au lieu de 47 %.**
C'est exactement ce qui faisait ressembler chaque bouche de grotte à un
après-midi couvert.

> ⚠️ La page *Light* du wiki ne donne ni la formule ni la table — seulement des
> valeurs de luminance qualitatives. La source est la documentation du mod
> Origins, qui l'énonce mot pour mot ; recoupée en la comparant à la table de
> TrueCraft (b1.7.3, ambient 0,05 à l'époque), dont les 32 valeurs se
> reproduisent à trois décimales avec la même formule.

**L'ordre d'évaluation est conservé exprès.** `f/(4-3f)` et `L/(60-3L)` sont
identiques sur le papier ; en binary32 la seconde donne *exactement* 0,5 au
niveau 12 et exactement 7/9 au niveau 14, alors que les valeurs publiées sont
0,50000006 et 0,77777773. Ces deux nombres sont l'empreinte de l'ordre réel, et
le test les surveille : si quelqu'un « simplifie » l'expression, il tombe.

### Le cycle du jour — vérifié au tick

```
X = frac(ticks/24000 - 0.25)
a = X + ((1 - (cos(X·π)+1)/2) - X)/3        ← le tiers qui étire l'aube
A = clamp(2·cos(a·2π) + 0.5, 0, 1)
A *= (1 - pluie·5/16) ; A *= (1 - orage·5/16)
```

Le jeu est documenté par les **ticks** auxquels la lumière du ciel change de
valeur, et c'est ce qui rend la fonction vérifiable plutôt que plausible : le
tiers de cosinus est invisible à midi et déplace ces repères de plusieurs
centaines de ticks.

| repère | tick documenté | calculé |
|---|---|---|
| minimum de nuit atteint | 13670 | **13670** |
| la lumière remonte | 22331 | **22331** |
| seuil d'apparition des monstres | 13188 | **13188** |
| midi clair / pluie / orage | 15 / 12 / 10 | **15 / 12 / 10** |

### Brouillard et ciel

```
fog.rg = biome_fog.rg · (0.94A + 0.06)
fog.b  = biome_fog.b  · (0.91A + 0.09)
sky    = biome_sky · A
```

Le bleu garde un plancher plus haut que le rouge et le vert, et c'est
précisément ce qui rend le brouillard de minuit **bleu sombre et non gris**. À
A = 1 la formule rend la couleur du biome inchangée, ce que le test vérifie —
sinon les constantes seraient inversées.

Le brouillard est **cylindrique** (`max(distance horizontale, |Δy|)`), comme
depuis la 1.18.1 : un brouillard sphérique se referme quand on lève les yeux.

> ⚠️ **Le 92 % est incertain.** La phrase du wiki (« commence à apparaître à
> 92 % de la distance de rendu ») ne porte pas d'étiquette d'édition, et
> `fog_start: 0.92` est aussi exactement la valeur du JSON de brouillard des
> resource packs **Bedrock**. Retenu comme probable, à mesurer.

### Ce qui n'est documenté nulle part, et qui est donc signalé plutôt que deviné

Le lightmap est reconstruit à chaque frame en 16×16 sur le CPU, échantillonné en
(lumière de bloc, lumière du ciel) — c'est le mécanisme de vanilla lui-même,
confirmé par la documentation d'OptiFine et de Polytone. Trois choses dedans ne
le sont pas :

1. **La teinte chaude de la lumière de bloc.** Toutes les sources sont
   qualitatives (« tire vers l'orange dans les valeurs moyennes »). La seule
   valeur numérique existante, `block_light_tint = #FFD88C`, appartient à la
   **26.1**, pas à la 1.20.1. L'implémentation reproduit la *description* — gris
   en bas, teinté au milieu, blanc en haut — et non le polynôme.
2. **La cible du lerp final.** La force 0,04 est corroborée (Polytone expose
   `base_light`, défaut 0,04). Le gris 0,75 vers lequel on interpole n'est
   attesté par aucune source ; c'est du folklore repris tel quel.
3. **Comment ciel et bloc se combinent.** Un tutoriel de 2011 dit *max*, la
   documentation moderne des dimensions dit *addition*. L'addition est retenue
   parce qu'elle seule permet à une torche de se voir en plein jour. **Non
   tranché par la documentation — à mesurer.**

Le sablier de la 1.21.9 est à ignorer : le lightmap y est passé dans un core
shader (`core/lightmap.fsh`, uniformes `SkyFactor`, `BlockFactor`…), donc la
page *Shader* actuelle du wiki ne décrit pas la 1.20.1.

### Un bug qui vaut d'être gardé

En ajoutant le lightmap, l'écran est devenu **entièrement de la couleur du
brouillard**. Le premier réflexe était de suspecter les distances de brouillard.
C'était faux : le terrain ne se dessinait pas du tout. Le layout de pipeline
place toutes les images avant tous les buffers, donc passer d'une image
échantillonnée à deux a **décalé le storage buffer des origines de section du
binding 1 au 2** — et le shader le déclarait toujours au 1. Un `--no-fog`, ajouté
comme diagnostic au même titre que `--no-cull`, a tranché la question en une
exécution : sans brouillard, l'écran restait uni, donc le brouillard était
innocent.

## Le client se connecte à son propre serveur

*2026-09-08.*

Jusqu'ici, `ov_voxel` était un **visualiseur** : il lisait un carré de chunks
sur le disque, maillait tout avant d'ouvrir la fenêtre, et laissait voler une
caméra sans collision. Le serveur, lui, savait déjà parler à un vrai client
1.20.1. Ce qui manquait entre les deux était le module que le graphe de couches
nomme depuis le début : `ov_netclient`, **un client sans rendu**.

C'est le deuxième principe rendu vrai plutôt qu'affiché : les octets qui
passent sur la socket sont les mêmes que le serveur soit à l'autre bout du
monde ou sur le fil d'à côté.

### Le paquet qui peut diverger en silence

Tous les autres paquets sont une poignée de champs fixes. Celui-ci est un blob
préfixé de sa longueur, contenant des conteneurs palettés **dont la largeur
décide elle-même du format**, puis des block entities, puis quatre bitsets, puis
deux séries de tableaux de lumière de longueur variable. Un seul octet mal
compté décale tout ce qui suit, et le symptôme est un monde qui a *presque*
l'air juste.

D'où la vérification : `parse_chunk_data` est le miroir exact de
`encode_chunk_data`, et le test prend **24 chunks réels sur le disque**, les
encode comme le serveur le ferait, les relit comme le client le fait, et compare
**cellule par cellule** — blocs, biomes, lumière du ciel, lumière de bloc, et
les heightmaps que le lecteur recalcule au lieu de les croire.

Au passage : deux implémentations de l'empaquetage d'une position de bloc
existaient, une privée et une nouvelle. Elles ont été fusionnées et testées —
x sur 26 bits, z sur 26, **y sur les 12 bits de poids faible**, tous signés. La
moitié du monde est à des coordonnées négatives et l'extension de signe ne se
voit que là.

### Le bug qui valait le détour

Au premier essai le monde arrivait mais l'écran ne montrait que des fragments de
pierre et du ciel. Le joueur **tombait à travers le monde** : il apparaît à
y = 63, la physique démarre, et les chunks sous lui ne sont pas encore là — donc
`block_at` répond « air » et il chute à vitesse terminale. Six secondes plus
tard il est mille blocs sous la carte et regarde le décor par en dessous.

C'est exactement ce à quoi sert l'écran « chargement du terrain » de vanilla.
Rien n'est simulé tant que le chunk sous le joueur n'est pas arrivé.

### Ce que ça donne

| | |
|---|---|
| chunks reçus (rayon 8) | 289 = 17 × 17 |
| sections résidentes | 2182 |
| joueur immobile | (0.50, **44.00**, 0.50), debout |
| après 400 frames en avançant | (0.50, 43.00, **4.70**), debout |

Le second chiffre est le test : il a avancé de 4,70 blocs **et descendu d'une
marche**, donc la physique, les formes de collision et le glissement axe par axe
fonctionnent contre un monde qui arrive par le réseau.

### Ce qui reste

- **Pas de nage.** Le point d'apparition de ce monde est sous l'eau, et le
  joueur y coule comme dans l'air : la traînée de l'eau n'est pas implémentée.
- **Le serveur intégré n'existe pas.** Le client se connecte à un serveur
  externe ; le solo passera par un `LoopbackTransport` qui transporte les mêmes
  octets.
- **Le serveur a signalé `can't keep up`** pendant l'envoi initial des 289
  chunks. Relevé plutôt que corrigé : c'est l'envoi de chunks qui n'a pas de
  budget, le pendant serveur du budget de maillage côté client.

### Casser et poser, vérifiés par le monde et non par le paquet

Un `--dig` a été ajouté au client pour la même raison que `--no-fog` : rendre
vérifiable une chose qu'aucune capture d'écran ne tranche. Il casse le bloc sous
le joueur, en pose un deux blocs plus loin, puis **regarde le monde** — pas le
fait qu'un paquet soit parti.

```
dug (0, 39, 4): minecraft:stone -> minecraft:air  BROKEN
placed at (2, 40, 4): minecraft:diamond_block     PLACED
```

Deux choses se sont mal passées avant d'y arriver, et les deux sont
instructives :

1. **La main était vide.** Sans `Set Creative Slot`, le serveur n'a rien à poser
   et ignore le clic en silence. Un envoi de placement parfaitement formé et un
   inventaire vide donnent exactement la même trace.
2. **Le premier essai posait le bloc sous les pieds du joueur.** Le serveur a
   refusé — correctement : un bloc ne peut pas apparaître dans un joueur, règle
   mesurée sur vanilla et implémentée côté serveur il y a plusieurs séances. Le
   symptôme était identique à celui d'un paquet de pose cassé.

C'est le même piège deux fois : côté client, « le paquet est parti » ne dit rien
du tout. Seul l'état du monde le dit.

### Le budget d'envoi de chunks

Le serveur signalait `can't keep up` à **chaque** connexion. La cause n'était
pas mystérieuse une fois regardée : les 289 chunks d'un rayon de 8 étaient
encodés et écrits **dans un seul tick**, pendant que l'horloge du tick continue
de tourner.

La file est maintenant choisie quand le joueur franchit une frontière de chunk
et drainée à **8 chunks par tick** — 160 par seconde, donc un rayon de 8 se
remplit en moins de deux secondes sans que le tick sorte de ses 50 ms.
Vérification : 0 occurrence de l'avertissement sur une connexion complète, les
289 chunks arrivant tout de même.

Et un second défaut que la correction a mis au jour : l'ancien code itérait un
`unordered_set`, donc les chunks partaient **dans l'ordre de hachage**. Le monde
s'assemblait par plaques autour du joueur au lieu de s'ouvrir depuis lui. La
file est triée par distance.

## Le serveur intégré : le solo est vraiment du multijoueur

*2026-09-08.*

Le serveur était une **application** : deux mille lignes dans un `main()`. C'était
juste tant que la seule façon de le lancer était un terminal, et faux dès que le
client en a voulu un — parce que « le solo est du multijoueur » veut dire que le
client héberge un vrai serveur et lui parle par une vraie socket, pas qu'il
attrape un objet monde au passage.

L'ensemble a donc déménagé tel quel dans `src/ov_server`, et `main()` fait trois
lignes. Le seul ajout est une façon de l'arrêter qui ne soit pas un signal : un
serveur dédié sort sur SIGINT, un serveur intégré sort quand la fenêtre se
ferme — et deux gestionnaires de signaux qui se disputent le même processus est
un bug que personne n'aime chercher.

`ov_voxel --singleplayer` lance donc `ov_server` sur un thread du même
processus. Vérifié de bout en bout, en une commande : 289 chunks reçus, joueur
debout, gravier cassé, bloc de diamant posé, **un seul joueur** dans la liste.

Ce dernier point a demandé une correction. La première version attendait que le
serveur écoute en ouvrant une connexion jetable pour tester — laquelle se
connectait, se **loguait**, créait un joueur, et laissait un fantôme. La vraie
connexion est simplement réessayée : cela ne coûte rien et ne crée personne.

Ce qui reste du plan sur ce point : le transport est aujourd'hui une socket TCP
sur la boucle locale, donc les octets sont bien sérialisés. Le `LoopbackTransport`
prévu remplace la socket par une file SPSC portant **les mêmes octets**, et rien
au-dessus du transport ne change quand il arrivera.

## L'eau et la lave

*2026-09-08.*

Le joueur coulait dans l'eau exactement comme dans l'air. Le point d'apparition
de ce monde étant sous l'eau, c'était la première chose qu'on voyait.

Dans un fluide, vanilla ne ralentit pas le tick terrestre : **il le remplace**.
Il n'y a ni glissance, ni vitesse de marche, ni multiplicateur de sprint, ni
saut — seulement une accélération d'un cinquantième, une traînée, et une gravité
bien plus faible.

```
saut tenu    -> vy += 0.04            accroupi -> vy -= 0.04
f = sprint ? 0.9 : 0.8                a = 0.02
deplacer, puis  vx *= f ; vy *= 0.8 ; vz *= f
si non sprint : vy -= 0.08/16 = 0.005
```

La lave : traînée 0,5, traînée verticale 0,8 en eau peu profonde et 0,5 en eau
profonde, et **une gravité au quart** — 0,02.

### Pourquoi ces chiffres sont vérifiables

Une vitesse terminale est le meilleur test possible d'une traînée : sous une
accélération `a` et une traînée `d` par tick, la vitesse se stabilise à
`a·0,98/(1−d)`, et le jeu **publie** ces vitesses en mètres par seconde. Six le
sont, les six reviennent :

| | calculé | publié |
|---|---|---|
| nage | 1,960 m/s | 1,97 |
| nage sprintée | 3,920 m/s | 3,918 |
| enfoncement dans la lave profonde | 0,800 m/s | 0,8 |

### Trois erreurs, trois causes différentes

Le premier jet ratait les trois, et aucune des trois n'avait la même cause :

1. **La nage sortait à 1,568 au lieu de 1,960** — exactement un facteur 0,8, la
   traînée elle-même. La vitesse *publiée* est la **distance parcourue en un
   tick**, c'est-à-dire la vélocité au moment du déplacement : après
   l'accélération du tick et avant sa traînée. Lue après, elle est un cinquième
   trop basse et ressemble à une constante fausse.
2. **La lave donnait 1,0 au lieu de 0,8.** J'appliquais la gravité de l'eau *en
   plus* de celle de la lave : 0,005 + 0,02 = 0,025, divisé par 0,5, donne
   exactement 0,05 bloc/tick. La lave n'a que son quart de gravité.
3. **Un saut dans une flaque devenait une brasse.** Le test répondait « eau » à
   toutes les hauteurs, donc la colonne entière comptait et la profondeur
   dépassait le seuil de 0,4. C'était le test qui était faux, pas le code — mais
   il aurait pu être l'inverse, et rien dans l'image ne l'aurait dit.

### Ce qui n'a pas été retenu faute de certitude

La recherche signale une clause spéciale produisant −0,003 dans la gravité de
l'eau. Elle est **inatteignable** à gravité normale : ses deux conditions
s'excluent quand `gravité/16 == 0,005`. Elle ne devient vivante que sous Chute
Lente. Non implémentée, et notée ici pour que la prochaine personne ne la
cherche pas.

Deux figures publiées ne se déduisent pas des constantes — la nage en surface
(2,20 m/s mesuré, 1,96 calculé) et la nage sprintée vers le haut (6,98 contre
~5). Elles sont donc à mesurer contre un vrai client, comme la physique
terrestre l'a été.

### Deux corrections venues d'une seconde lecture

Une seconde recherche indépendante a corrigé deux points de la première :

1. **La lave peu profonde applique les deux gravités**, pas une seule — le
   seizième de l'eau *puis* le quart de la lave, soit 0,025 par tick, sous une
   traînée verticale de 0,8. La lave *profonde* n'a que le quart, sous une
   traînée de 0,5, et c'est elle que la valeur publiée de 0,8 m/s mesure. Les
   deux branches ont des traînées différentes, donc ce n'est pas contradictoire.
2. **Un seuil de vélocité négligeable de 0,003.** Toute composante en dessous
   est mise à zéro. C'était 0,005 avant la 1.9. Sans lui, un joueur qui s'arrête
   dérive indéfiniment par quantités de plus en plus petites et chaque rapport
   de position porte un nombre différent. Ajouté, et la physique terrestre
   ajustée sur la trace du vrai client passe toujours — ce qui était la question.

### Le brouillard sous l'eau

Debout dans un océan, l'écran était presque vide : chaque face entre deux blocs
d'eau est supprimée, donc **il n'y a réellement rien à dessiner de près**, et le
brouillard de l'air laissait voir des îlots lointains flotter dans du bleu pâle.

La **couleur** est mesurée : c'est le `water_fog_color` du biome, qui est dans
le pack. Les **distances** ne le sont pas, et c'est écrit tel quel dans le code
plutôt que déguisé — le brouillard sous-marin de vanilla dépend aussi du temps
passé immergé, de Respiration et de Respiration Aquatique, dont rien n'existe
ici.

Un premier essai à 24 blocs de portée **noyait un fond marin situé à dix-sept
blocs**. C'est ce qui a rendu évident que deviner serré est pire que deviner
large : une valeur inventée trop courte supprime de l'information, une trop
longue en laisse.

## La parité de seed : l'oracle d'abord

*2026-09-08.*

Le plan verrouillait « **pas** de parité de seed bit-exacte ». Cette décision est
levée à la demande de l'utilisateur, et il faut dire ce qu'elle coûte : depuis
la 1.18 le terrain n'est pas un générateur paramétré, c'est une **expression**,
écrite en JSON et évaluée par position. `noise_settings` nomme un routeur d'une
quinzaine d'expressions au-dessus de trente-cinq fonctions nommées. Une
approximation écrite à la main serait fausse dès le premier datapack et ne
pourrait jamais être exacte à la seed.

### L'oracle avant le code

`scripts/reference_world.sh` fait générer un monde par le **vrai serveur
1.20.1** à une seed fixe. C'est le seul oracle possible : aucune formule ne
prouve la parité du terrain, seuls les blocs que le jeu écrit sur le disque.
Quatre fichiers de région à la seed 1234567890, gitignorés comme toute sortie
vanilla.

### Ce qui est fait

**Le bruit.** `ImprovedNoise` → `PerlinNoise` → `NormalNoise`. Un piège vérifié
par test : une amplitude nulle **saute** une octave sans décaler les autres,
parce que chaque octave est ensemencée par le hash de `"octave_<n>"` et non en
séquence. Un ensemencement séquentiel donnerait un monde différent dès qu'une
octave est absente.

**Le hachage de position**, dont dépend chaque minerai, chaque arbre et chaque
grotte :

```
l = (x * 3129871) ^ (z * 116129781) ^ y      ← le premier terme déborde en 32 bits
l = l*l*42317861 + l*11
return l >> 16
```

La première ligne est la trappe : `x * 3129871` est une multiplication **entière
32 bits** qui boucle avant d'être élargie, tandis que `z * 116129781` est une
multiplication 64 bits qui ne boucle pas. Tout faire en 64 bits donne un nombre
différent dès environ 686 blocs en x — assez loin pour qu'un petit monde de test
ne le voie jamais.

**L'interpréteur de fonctions de densité.** 23 types, lus depuis les JSON
vanilla. **14 des 15 entrées du routeur overworld se construisent**, dont les
six fonctions climatiques. Deux points valent d'être notés :

- `flat_cache` **n'est pas une identité** : il quantifie x et z à des multiples
  de quatre et évalue à y = 0. Le traiter comme transparent donne un champ lisse
  là où le jeu en a un en marches, et déplace chaque frontière de biome.
- Les **splines sont évaluées en `float`**, pas en `double`. C'est d'elles que
  vient la forme à grande échelle du terrain, et les élargir serait un autre
  monde.

### Ce qui manque, nommé plutôt que tu

`final_density` ne se construit pas : il atteint `old_blended_noise`, le bruit
de terrain de la 1.17, dont l'ensemencement des octaves est séquentiel et non
par nom. Un type non implémenté est **refusé et nommé** — jamais traité comme
zéro, parce qu'un terme silencieusement absent donne un terrain plausible et
faux.

Reste aussi le `NoiseChunk` : le terrain est échantillonné sur une grille de
cellules (4 blocs en horizontal, 8 en vertical pour l'overworld) puis interpolé
entre. C'est ce qui rend `interpolated` inexact point par point — exact pour le
climat, qui n'en utilise pas, et pas pour la densité finale.

## Les biomes : 613857 sur 614400

*2026-09-08.*

Premier chiffre de parité de terrain, et il est bon : **613857 cellules de biome
sur 614400 identiques à celles que le vrai serveur 1.20.1 a écrites pour la même
seed — 99,912 %**, sur 400 chunks entièrement générés.

### La table n'est pas dans le datapack

`multi_noise_biome_source_parameter_list/overworld.json` ne contient qu'un
`{"preset": "minecraft:overworld"}` : les ~7600 boîtes du monde sont du code
Java. Elles sont en revanche **exportées par le générateur de données officiel**
dans `reports/biome_parameters`, qui est d'où elles sont lues — généré
localement comme tout le reste, commité nulle part.

### Le harnais s'est trompé avant le générateur

La première mesure donnait **67 %**, et tous les désaccords disaient « le jeu a
choisi plains ». C'était le harnais : une région force-chargée contient des
chunks arrêtés à un statut intermédiaire, dont le tableau de biomes vaut
**plains par défaut**. Ce ne sont pas des résultats de génération.

Filtré sur `Status == minecraft:full`, **le même code donne 99,912 %**. La leçon
n'est pas nouvelle mais elle vaut d'être répétée : un chiffre de parité mesure
autant l'oracle que le code, et le premier réflexe quand il est mauvais doit
être de douter des deux.

### Les 543 restants sont tous des égalités

Pas « surtout » : **tous**. Pour chacun, la boîte du biome choisi par le jeu et
celle du nôtre sont **exactement à la même distance** du point climatique.
Autrement dit le calcul du climat — le bruit, les octaves, les décalages, les
splines, la quantification — est **identique**, et seule la départition diffère.

Vanilla range les boîtes dans un R-tree et prend la première trouvée au minimum
(`if (l > m)`, strictement) ; l'ordre des feuilles vient de la construction de
l'arbre, et le résultat **précédent** sert de borne initiale — si bien que la
réponse peut dépendre de la requête d'avant. Nous faisons un balayage linéaire
et gardons la première strictement plus proche. Reproduire les 0,088 % restants
demande de reproduire l'ordre de construction du R-tree *et* ce cache.

### Ce que le harnais a exclu

Avant de trouver le vrai problème, l'outil a fait son travail : il a montré que
distordre `continentalness`, `erosion` ou `weirdness` n'améliorait rien — ces
axes étaient déjà justes — et qu'aucune échelle ni aucun décalage sur la
température ne remontait au-dessus de 88 %, donc que l'erreur n'était pas un
facteur. Les deux conclusions étaient correctes : l'erreur n'était dans aucun
axe.

## Le terrain : 97,79 % de la forme, et les deux façons de se tromper

*2026-09-08.*

`old_blended_noise` a levé le dernier blocage : **les quinze entrées du routeur
overworld se construisent**, `final_density` comprise. Le générateur de chunks
existe et se mesure.

### Ce que le bruit ancien a de particulier

Trois piles d'octaves, pas une. Deux sont des **limites** — un plancher et un
plafond — et la troisième choisit entre elles. Le résultat est donc une
interpolation **dont le facteur de mélange est lui-même du bruit**, et c'est ce
qui donne au terrain de la 1.17 ses surplombs : un champ lisse ne peut pas en
produire, un champ qui choisit entre deux champs lisses, si.

Ses octaves sont ensemencées **en séquence** depuis un générateur, pas par nom.
C'est l'ancien schéma, et c'est pourquoi ce bruit ne peut pas réutiliser
`PerlinNoise::create` : les mêmes amplitudes ensemencées des deux façons donnent
deux mondes différents. L'octave de plus haute fréquence est créée **en
premier** et les autres descendent.

### La grille de cellules

`interpolated` n'est pas transparent, et c'est le seul enveloppeur qui ne l'est
pas. Le terrain n'est pas évalué bloc par bloc : il l'est aux coins de cellules
de **4 blocs de large et 8 de haut**, et chaque bloc à l'intérieur est un
mélange trilinéaire de ses huit coins. L'ordre est **y, puis x, puis z** — celui
de vanilla, et en flottant ce n'est pas le même résultat qu'un autre ordre.

Un bug corrigé au passage, latent plutôt que visible : la clé du cache des coins
décalait des valeurs 32 bits de 40 et 16 et les combinait par XOR, si bien que
les champs se chevauchaient et que des cellules distinctes pouvaient se répondre
l'une l'autre.

### Le chiffre, et les deux erreurs séparées

**97,79 % d'accord solide/air** sur 153 600 blocs échantillonnés. Ce qui compte
est que les deux façons de se tromper aient été séparées, parce qu'elles ne
veulent pas dire la même chose :

| | | |
|---|---|---|
| pierre là où le jeu n'en a pas | 1,20 % | air, eau, lave — **grottes et aquifères** |
| pas de pierre là où le jeu en a | 1,01 % | terre, pierre, herbe — la surface |

Le premier est entièrement expliqué : le recensement des blocs concernés donne
`air`, `water`, `cave_air`, `lava`. Ce sont les carvers et les aquifères, qui ne
sont pas implémentés. Ce n'est pas une erreur du bruit, c'est une étape absente.

Le second ne l'est **pas**. Il se concentre au-dessus de y = 64, et la
distribution des hauteurs de surface est centrée sur **−2 blocs** avec une queue
jusqu'à −8 : notre terrain est systématiquement un peu plus bas. Sondée au bloc
que le jeu appelle la surface, notre densité y vaut entre −0,005 et −0,076 — tout
juste sous le seuil. **La cause n'est pas localisée**, et elle est écrite ici
plutôt que laissée dans un pourcentage.

Trois hypothèses ont été écartées par mesure : ce ne sont pas les arbres (les
exclure change 1,38 % en 1,03 %), ce n'est pas la végétation de surface (0,02 %
de plus), et ce n'est pas le cache d'interpolation (le corriger ne change rien
sur cette zone).

### Le décalage de surface : ce qui a été écarté

Le harnais a gagné trois modes de diagnostic, et chacun a fermé une piste :

- **`--terrain`** sépare les deux erreurs et recense les blocs concernés. C'est
  lui qui a montré que le surplus de pierre n'est que `air`, `water`,
  `cave_air` et `lava` — donc les grottes et les aquifères, une étape absente
  et non un bruit faux.
- **`OV_NO_INTERPOLATION`** désactive la grille de cellules. Avec elle 97,74 %,
  sans elle 97,19 % : l'interpolation **aide**, donc elle n'est pas la cause.
- **`--column`** vide une colonne : chaque terme — `offset`, `factor`,
  `jaggedness`, `depth`, la densité initiale et la finale — pour chaque hauteur,
  à côté de ce que le jeu y a mis.

Ce que la colonne montre : à (−37032, 14960) notre densité vaut **+0,004 à
y = 69 et −0,012 à y = 70**, là où le jeu a du solide jusqu'à 70. Le pas de
densité par bloc y est d'environ 0,016, donc il manque à peu près un pas. Sur
l'ensemble la distribution est centrée sur −2 blocs.

Autrement dit : ce n'est pas un terme absent qui vaudrait des unités, c'est un
biais de l'ordre de 0,02 à 0,03 en densité. Écrit ici sans conclusion, parce
qu'aucune des trois pistes testées ne l'explique.

### Le monde de référence, et pourquoi il prenait quarante minutes

Un `forceload` est **permanent**. Le serveur garde tous les chunks qu'il tient
et les tick **tous**, à chaque tick, pour le reste de la session. La première
version du script ajoutait vingt-huit zones sans jamais en retirer, si bien que
la dernière était générée pendant que trois mille chunks étaient tickés : la
génération ralentissait progressivement jusqu'à ressembler à un blocage.

Chaque zone est maintenant sauvegardée puis **retirée** avant la suivante.
Même travail, même monde : **6 min 06 s** au lieu de plus de quarante.

Deux détails du même ordre, trouvés en route : le watchdog du serveur tue la
partie quand un tick dépasse une minute — ce qui est exactement ce que fait une
pré-génération, d'où `max-tick-time=-1` — et un serveur d'une mesure précédente
tournait encore depuis trois heures et demie, à se disputer la machine.

Avec 28 zones réparties jusqu'à ±110 000 blocs, la mesure porte maintenant sur
**7 821 312 cellules de biome et 38 biomes** : 99,972 % d'accord, et les 2197
écarts sont toujours **tous** des égalités exactes.
---

## Entités : demander au jeu sa taille, ses yeux et ses attributs

Trois nombres décident de tout ce qu'une entité peut faire, et **aucun des trois
n'existe dans les rapports officiels** : la boîte de collision, la hauteur des
yeux et la valeur de base de chaque attribut sont du code Java. Ce projet ne lit
pas de code Java. Il a donc fallu les faire dire au jeu lui-même, avec des
commandes — qui sont de la donnée documentée, pas de la source.

`scripts/measure_entities.py` lance un vrai serveur 1.20.1 sur un port à lui,
pose une entité de chaque type sur une grille espacée de 16 blocs, et pose trois
questions.

### La boîte : une bissection, et un étalon qui n'était pas supposé

`execute positioned <p> if entity @e[…,dx=0,dy=0,dz=0]` réussit exactement quand
le volume de sonde rencontre la boîte de l'entité. Bissecter cette frontière le
long de +X et de +Y donne la demi-largeur et la hauteur.

Le piège est que **la sémantique du volume `dx/dy/dz` n'est pas évidente** : la
documentation le décrit tantôt comme un point, tantôt comme une boîte gonflée
d'un bloc, et les deux lectures donnent des résultats plausibles. Plutôt que de
trancher par la lecture, la sonde est **étalonnée** contre `minecraft:interaction`
— la seule entité dont la largeur et la hauteur *sont* son NBT. Cinq tailles
déclarées, mesurées exactement comme les mobs le seront ensuite :

| déclaré | demi-largeur mesurée | sommet mesuré |
|---|---|---|
| 0,5 × 0,5 | 0,2500000 | 0,5000000 |
| 1,0 × 2,0 | 0,5000000 | 2,0000000 |
| 3,0 × 0,25 | 1,5000000 | 0,2500000 |
| 2,0 × 1,0 | 1,0000000 | 1,0000000 |
| 0,25 × 4,0 | 0,1250000 | 4,0000000 |

Décalage moyen **−1,5 × 10⁻⁸**, dispersion **exactement nulle** sur les dix
relevés. Le volume est donc un point, `width` est bien la largeur totale et non
la demi-largeur, et la boîte monte des pieds vers le haut. Cinq tailles plutôt
qu'une, parce qu'un décalage additif et un facteur d'échelle sont
indiscernables sur un seul point.

### Les yeux : un marqueur posé à l'ancre du regard

`execute as <mob> at @s anchored eyes positioned ^ ^ ^ run summon
minecraft:marker ~ ~ ~` pose un marqueur là où le jeu place les yeux ; la
différence entre son `Pos` et celui du mob est la hauteur des yeux, à la
dernière décimale que porte le double.

**`as @s` n'est pas décoratif.** `anchored eyes` décale la position de l'entité
qui *exécute*, pas de celle que `at` désigne. Lancée depuis la console avec le
seul `at`, la commande n'a aucun `@s` à décaler : le marqueur tombe **aux pieds
du mob**, la mesure rend 0,0 pour tout le monde, et rien ne signale l'erreur —
c'est une mauvaise réponse qui a exactement l'allure d'une bonne.

### Les attributs : les treize, demandés à chaque type

`attribute <cible> <attribut> base get` imprime la valeur de base. La campagne
demande **les treize** attributs du registre à chacun des types, si bien que la
réponse porte aussi sur *quels* attributs un type possède. Un zombie n'a pas de
`horse.jump_strength` et le serveur le dit ; une vache n'a pas d'`attack_damage`.
Cette absence est stockée comme une absence : `attribute_base` rend `nullopt`, et
jamais 0. « Ne frappe pas » et « frappe pour rien » sont deux choses différentes.

622 valeurs sur 120 types.

### Le piège qui a coûté 47 types

Premier passage : **77 types sur 124**, et les 47 manquants étaient *exactement*
les animaux — vache, poule, mouton, cheval, loup, poisson, villageois. La console
répondait pourtant « Summoned new Cow » à chaque fois.

La cause est `spawn-animals=false` dans `server.properties`, mis là par réflexe
de banc de mesure. Ce réglage ne se contente pas de couper l'apparition
naturelle : le serveur **supprime** les animaux à leur premier tick, y compris
ceux invoqués à la main, y compris avec `PersistenceRequired:1b`. Le relevé
perdait toute la classe `Animal` pendant que le journal affirmait le contraire.

Le banc laisse donc les trois catégories d'apparition **actives**, et coupe
l'apparition naturelle avec la règle `doMobSpawning`. Chaque entité mesurée porte
un `Tags` unique, donc la faune du monde ne gêne pas.

### Ce qui n'a pas pu être mesuré, et qui est nommé plutôt qu'arrondi

**120 types sur 124.** Les quatre restants sont refusés par leur nom :

| type | pourquoi |
|---|---|
| `minecraft:lightning_bolt` | vit un tick |
| `minecraft:evoker_fangs` | vit une vingtaine de ticks, la bissection en demande des milliers |
| `minecraft:fishing_bobber` | ne peut pas exister sans pêcheur |
| `minecraft:player` | ne s'invoque pas |

Trois hauteurs d'yeux manquent aussi (`marker`, `painting`, `eye_of_ender`), et
un bit du pack le dit — plutôt qu'un zéro qui ressemblerait à une mesure.

`EntityWorld::spawn` **refuse** un type sans boîte mesurée. Un mob à boîte nulle
est un mob que rien ne peut jamais toucher, et c'est pire qu'un mob qui n'est pas
apparu : seul le second le dit.

### Ce que le pack stocke, et pourquoi en f64

Les valeurs d'attribut sont stockées en **f64**. La vitesse d'un zombie n'est pas
0,23 mais **0,23000000417232513** — le double le plus proche du float que le jeu
tient. La faire passer par un f32 ne la ferait pas revenir, et le paquet Update
Attributes porte justement un f64 sur le fil. Les dimensions, elles, sont en f32,
comme le jeu les tient.

Un détail qui se voit en test : la demi-largeur d'un zombie n'est pas 0,3 mais
0,30000001192092896, parce que 0,6 est un f32. C'est ce nombre-là que la
collision doit utiliser.

### `ov_entity` : EnTT pour le stockage, et une vue qu'on ne crée pas

Le module suit `docs/ARCHITECTURE.md` § 5 : EnTT pour les handles et le
stockage, comportement polymorphe dans un `std::unique_ptr<IEntityLogic>`. EnTT
est un **PRIVATE_DEP** et n'apparaît dans aucun en-tête public ; le registry vit
derrière un PIMPL.

Le plan avertit que `view()` et `group()` **mutent l'état interne** même en
lecture et que le registry doit rester sur le thread de tick. Une deuxième raison
s'y ajoute, et elle est plus contraignante : **l'ordre d'itération d'une vue est
l'ordre du stockage, et le stockage est un swap-and-pop**. Détruire une entité y
déplace la dernière à sa place ; deux exécutions de la même suite d'apparitions
et de morts tiqueraient alors les mêmes entités dans des ordres différents, ce
qui viole le déterminisme (CLAUDE.md § 2.5).

Le tick parcourt donc une liste de handles en **ordre d'insertion**, et aucune
vue n'est créée sur le chemin du tick — ce qui rend le piège de thread-safety
inatteignable par la même occasion. Le test le vérifie : trois mobs, celui du
milieu meurt, et le tick suivant visite toujours le premier avant le troisième.

Deux autres propriétés sont tenues par des tests plutôt que par une intention :

- un handle vers une entité morte **ne résout jamais** vers celle qui a pris sa
  place — EnTT recycle les emplacements, et la génération portée dans le handle
  est ce qui empêche des dégâts destinés à une vache morte d'atterrir sur le
  cochon suivant ;
- un id réseau n'est **jamais réutilisé** ;
- une entité apparue *pendant* un tick n'est pas tiquée dans ce tick — sinon un
  mob qui en engendre un par tick empêcherait le tick de finir.

---

## Les paquets d'entité : relevés sur le fil, indices compris

`scripts/capture_entity_packets.py` fait joindre un client sonde écrit depuis la
spec au vrai serveur 1.20.1, invoque des mobs à côté de lui depuis la console, et
écrit ce qui arrive, octet par octet. Ce qui est figé en test
(`src/ov_protocol/tests/test_entity_packets.cpp`) est ce que le jeu a produit.

### La table d'indices n'est pas lue, elle est dérivée

Un index de métadonnée faux **n'est pas une erreur** : c'est un mob qui rend avec
la propriété d'un autre, sans un mot. C'est la raison pour laquelle il ne fallait
pas la recopier depuis un résumé.

Le relevé pose donc un zombie de référence, puis un zombie identique **plus un
seul champ NBT**, et regarde quel index a changé :

| champ NBT | index | type | valeur observée |
|---|---|---|---|
| `Glowing:1b` | 0 | byte | 64 → bit 0x40 |
| `Air:123s` | 1 | varint | 123 |
| `CustomName` | 2 | optional_component | le JSON tel quel |
| `CustomNameVisible:1b` | 3 | boolean | vrai |
| `Silent:1b` | 4 | boolean | vrai |
| `NoGravity:1b` | 5 | boolean | vrai |
| `TicksFrozen:123` | 7 | varint | 123 |
| `Health:7.0f` | 9 | float | 7,0 |
| `NoAI:1b` | 15 | byte | 1 |
| `LeftHanded:1b` | 15 | byte | 2 |
| `IsBaby:1b` | 16 | boolean | vrai |

Les indices que rien n'a fait bouger sont **absents** de `ov/protocol/entity.hpp`
plutôt que devinés : 6 (pose), 8 sur une entité vivante, et 10 à 14. Une
constante plausible y produirait un mob correct à l'écran et faux au fond.

Deux observations qui n'étaient pas demandées :

- **vanilla n'envoie que ce qui diffère du défaut.** Un zombie invoqué sans rien
  n'envoie qu'un champ, la santé — encore une fois, la santé est envoyée même à
  sa valeur par défaut.
- **un armor stand n'envoie pas l'index 15.** Il n'est pas un `Mob`. Une table
  d'indices écrite « pour toutes les entités » aurait mis des drapeaux de mob sur
  un support d'armure.

### L'unité des paquets de déplacement, mesurée

`Update Entity Position` porte un delta et pas une position. L'unité a été
mesurée plutôt que lue : un zombie téléporté d'une distance connue, `Pos` relu
des deux côtés.

| déplacement | `dX` observé | rapport |
|---|---|---|
| +0,75 bloc | 3072 | 4096 |
| −1,125 bloc | −4608 | 4096 |

L'unité est donc **1/4096 de bloc**, et un i16 porte un peu moins de 8 blocs.
La borne compte : 8 blocs exactement valent 32768, un de trop, et l'entité
repartirait de huit blocs dans l'autre sens. `fits_in_delta` refuse à 8,0 et
accepte à 7,999.

### Un ordre de champs qui n'est pas le même d'un paquet à l'autre

`Spawn Entity` écrit **pitch, puis yaw**, puis head yaw. `Update Entity Position
and Rotation` écrit **yaw, puis pitch**. Ce n'est pas une erreur dans l'un des
deux : les deux paquets sont réellement en désaccord, et les deux ordres viennent
de captures. Deviner l'un depuis l'autre donne un mob tourné de travers.

`Entity Event` (0x1C) est le seul paquet d'entité dont l'id est un **i32 fixe** et
non un varint.

### Les deux mesures se rejoignent

Le paquet `Update Attributes` d'un zombie porte
`minecraft:generic.movement_speed` et le f64 `3fcd70a3e0000000`, soit
**0,23000000417232513**. C'est exactement ce que `attribute … base get` imprime
pour le type, et exactement le double le plus proche du float `0.23f`. Les deux
relevés — la commande et le fil — sont indépendants et **coïncident bit pour
bit**. C'est ce qui justifie de stocker les attributs en f64 dans le pack.

---

## La chute d'un mob, lue dans son propre tag `Motion`

La gravité et la traînée du **joueur** ont été ajustées sur les positions
rapportées par un vrai client (plus haut dans ce fichier). Les réutiliser pour
les mobs aurait été une supposition — plausible, donc la pire espèce. Elles ont
donc été mesurées.

`data get entity <mob> Motion` imprime la vitesse en doubles exacts. Un mob lâché
de y = 300 tombe librement, et sa vitesse verticale suit

    v(0) = 0,   v(n+1) = (v(n) − g)·d

`scripts/measure_entity_fall.py` échantillonne `Motion` quarante fois pendant une
chute, sans savoir à quel tick chaque échantillon correspond : un couple (g, d)
candidat prédit une courbe, chaque échantillon est rapproché du point le plus
proche, et le couple qui minimise le résidu gagne. Deux inconnues contre quarante
échantillons : un mauvais couple ne peut pas passer.

| entité | g | d | résidu (rms) | vitesse limite |
|---|---|---|---|---|
| zombie | 0,08000 | 0,98000 | 1,6 × 10⁻⁶ | −3,92 |
| vache | 0,08000 | 0,98000 | 1,6 × 10⁻⁶ | −3,92 |
| support d'armure | 0,08000 | 0,98000 | 1,6 × 10⁻⁶ | −3,92 |
| **objet au sol** | **0,04000** | 0,98000 | **1,2 × 10⁻¹⁵** | **−1,96** |
| flèche | 0,02086 | 0,99420 | 2,6 × 10⁻³ | — |

Trois choses en sortent :

1. **Un mob tombe comme le joueur.** 0,08 et 0,98, et le résidu est la
   quantification de l'échantillonnage, pas un désaccord. Cela recoupe aussi
   l'ajustement du joueur : 0,079998 et 0,980014 mesurés à travers le bruit d'un
   client mesuraient bien ces deux constantes-là.
2. **Une pile au sol tombe à la moitié de la gravité.** 0,04, avec un résidu de
   10⁻¹⁵ — c'est-à-dire exact, pas un arrondi de 0,08. Sa vitesse limite est
   −1,96 et non −3,92 ; utiliser les constantes ordinaires ferait tomber chaque
   objet deux fois trop vite.
3. **Une flèche ne suit pas ce modèle.** Son ajustement est trois ordres de
   grandeur pire que celui d'un mob. Elle bouge sous d'autres règles, ces règles
   ne sont pas implémentées, et `step_entity` produirait pour elle une trajectoire
   plausible et fausse. C'est écrit dans l'en-tête plutôt que laissé à découvrir.

### Le piège : `NoAI` coupe la physique, pas seulement le cerveau

La première campagne a rendu **zéro échantillon** pour tous les mobs vivants. Un
zombie invoqué avec `NoAI:1b` à y = 300 y reste, indéfiniment, `Motion` à plat.
Le support d'armure et l'objet, qui ne sont pas des `Mob`, tombaient normalement —
ce qui rendait le résultat cohérent et faux.

Le banc de mesure des boîtes de collision, lui, **doit** garder `NoAI` (un mob
qui marche ne se laisse pas bissecter). Les deux campagnes sont donc réglées
différemment, et chacune dit pourquoi.

---

## Des mobs qu'un vrai client voit

`ov_dedicated --mobs=zombie,cow,creeper,…` pose les mobs demandés devant le point
d'apparition, trois blocs en l'air, trois secondes après le démarrage — assez
tard pour qu'un client déjà connecté les voie apparaître **et tomber**, ce qui
est aussi ce qui rend la chute observable de l'extérieur.

`scripts/check_entities.py` branche sur notre serveur le même client sonde que le
harnais de capture branche sur le jar vanilla. Même lecteur, mêmes attentes,
serveur différent. Relevé sur huit types :

```
8 spawns, 8 métadonnées, 8 jeux d'attributs, 72 deltas de déplacement
  minecraft:zombie    entité 1000000  type 118  vie 20  chute 9 ticks -> y=-60.0000
  minecraft:cow       entité 1000001  type  18  vie 10  chute 9 ticks -> y=-60.0000
  minecraft:creeper   entité 1000002  type  19  vie 20  chute 9 ticks -> y=-60.0000
  minecraft:chicken   entité 1000003  type  15  vie  4  chute 9 ticks -> y=-60.0000
  minecraft:enderman  entité 1000004  type  29  vie 40  chute 9 ticks -> y=-60.0000
  minecraft:slime     entité 1000005  type  88  vie  1  chute 9 ticks -> y=-60.0000
  minecraft:villager  entité 1000006  type 108  vie 20  chute 9 ticks -> y=-60.0000
  minecraft:skeleton  entité 1000007  type  86  vie 20  chute 9 ticks -> y=-60.0000
```

Les ids de type sont ceux de Mojang, relus depuis le registre par le vérificateur
lui-même ; les vies et les valeurs d'attribut sont comparées **à la campagne de
mesure**, pas à une constante recopiée dans le test.

### Un décalage de 0,000244 qui n'aurait jamais cessé de grandir

La première exécution du vérificateur a rendu **y = −59,999756** pour les huit.
L'écart vaut exactement 1/4096 — un quantum du paquet de delta.

La cause n'est pas un arrondi inoffensif : le serveur calculait chaque delta
depuis la **vraie** position précédente. Le paquet, lui, ne peut porter qu'un
multiple de 1/4096, donc le reste était **jeté à chaque tick**. Neuf ticks de
chute coûtaient déjà 0,000244 ; une minute de marche aurait mis le mob ailleurs
que là où il est, et rien dans le protocole ne l'aurait signalé.

Le correctif est de tenir, par entité, **la position que le client a
effectivement** (`EntityState::broadcast_position`), de calculer le delta contre
elle, et de l'avancer de ce qui a réellement été envoyé. Le reste est reporté sur
le delta suivant. Le vérificateur retombe alors sur **y = −60,0000** exactement,
et sa tolérance est désormais d'un quantum — bornée, et non cumulative.

C'est la même leçon que la section « Absolu plutôt que relatif » plus haut, prise
par l'autre bout : les entités joueur ont évité le problème en n'envoyant que des
téléports absolus ; les mobs paient six octets au lieu de vingt-huit et doivent
donc tenir le compte.

### Le comportement passe par `IEntityLogic`, pas à côté

Le serveur ne fait pas tomber les mobs lui-même : il appelle `EntityWorld::tick`,
et c'est `gameplay::FallingMob` — un `IEntityLogic` — qui applique la physique.
La distinction n'est pas cosmétique. Tout l'intérêt du composant polymorphe est
qu'un zombie et une pile au sol diffèrent par **ce qu'ils font** et non par qui
les appelle ; un serveur qui applique la physique à la main aurait un `if` par
type au lieu d'un appel virtuel, et le premier mob qui doit faire autre chose
casse la boucle.

Le pont entre les couches est `TickContext::user`, un pointeur opaque que
`ov_entity` (couche 8) ne peut pas nommer et que `ov_gameplay` (couche 9)
récupère. C'est exactement l'idiome que `CollisionWorld` utilise déjà pour lire
le monde, et c'est ce qui permet à la couche basse de tiquer du comportement
écrit au-dessus d'elle sans qu'aucune des deux ne connaisse l'autre.

Trois choses sont vérifiées plutôt que supposées : une pile au sol tombe
**exactement de moitié** moins loin qu'un mob au même tick (même objet de
comportement, constantes différentes) ; un tableau, qui n'a aucun composant de
logique, ne bouge pas du tout — un pointeur nul et zéro appel virtuel ; et un
appelant qui oublie de renseigner le contexte obtient un mob **immobile** plutôt
qu'un mob qui traverse le sol faute de collision.
