# La Grande Pyramide — structure originale Ondes VOXEL

> **Ce n'est pas du contenu Minecraft.** `ondes_voxel:great_pyramid` est une structure inventée pour
> le projet, à la demande de l'utilisateur (décision produit du 2026-09-11, `docs/ARCHITECTURE.md`
> § 9.1). Elle s'**ajoute** à vanilla ; la pyramide du désert du jeu (`minecraft:desert_pyramid`)
> reste celle de 1.20.1, reproduite ailleurs. Aucune source de code tiers n'a été lue : tout est
> conçu ici. La seule inspiration extérieure est l'architecture de la pyramide de Khéops (grande
> galerie en encorbellement, chambre de la Reine sur un couloir horizontal, conduits d'aération de
> la chambre du Roi), connaissance générale.

Code : `src/ov_worldgen/include/ov/worldgen/great_pyramid.hpp`, `src/ov_worldgen/src/great_pyramid_*.cpp`.
Tests : `test_great_pyramid.cpp` (ov_worldgen), `test_great_pyramid_traps.cpp` (ov_server).
Mesures : `tools/ov_pyramid`.

## 1. Placement

| Paramètre | Valeur | Où |
|---|---|---|
| ensemble | `ondes_voxel:great_pyramids`, `random_spread` linéaire | `great_pyramid_placement()` |
| écartement / séparation | 48 / 24 chunks | idem, et `data/ondes_voxel/ov_structures/great_pyramid.json` (descriptif, gardé égal par un test) |
| sel | 20260911 (aucun ensemble vanilla ne l'utilise) | idem |
| biome | `minecraft:desert` au milieu du chunk, à la surface (l'ancre vanilla), **et** 22 des 25 points d'une grille 5 × 5 sur l'emprise | `decide` |
| relief | 13 échantillons (centre, 4 coins, 4 milieux d'arête, 4 quarts) : écart max − min ≤ **14** | `kMaxSpread` |
| niveau | le socle à la **médiane** des 13 échantillons ; refusé si ≤ niveau de la mer + 1 | `decide` |
| voisinage vanilla | refusé si le placeur vanilla (`StructurePlacer::decide`) démarrerait un village ou une pyramide du désert à ≤ 10 chunks, un puits de mine sous l'emprise (≤ 4), toute autre structure à ≤ 6 chunks | `near_vanilla` |

Les hauteurs viennent de la **colonne de base** du générateur (le bruit seul, `is_solid`), par un
balayage grossier de 4 en 4 depuis y 160 affiné au bloc : le sol, pas ce qui le recouvre. Le filtre
vanilla est demandé au placeur du jeu avec les hauteurs de surface remplacées par celle du socle —
l'ancre vanilla est la surface du chunk, et sur l'emprise elle est à 14 blocs au plus du socle ; le
biome, lui, bouge par cellules de 4.

Les forts (anneaux concentriques) ne sont pas vérifiés : leur placement n'est pas une fonction par
chunk (`structures.md` § 6) et ils sont souterrains.

## 2. Le plan

Repère canonique : `u` en travers de la façade d'entrée, `v` vers l'intérieur (l'entrée est sur la
face `v = −50`), `y` depuis le sol (`y = 0` est le premier bloc libre au-dessus du socle). Quatre
orientations, tirées du hasard du départ : l'entrée regarde le nord, l'est, le sud ou l'ouest ; le
repère tourne d'un quart de tour horaire par cran, sans jamais se refléter, si bien que la
propriété `facing` d'un escalier ou d'un piston tourne avec lui.

| Élément | Emprise canonique | Détails |
|---|---|---|
| Enveloppe | 101 × 101, 51 assises (`y` 0..50), chaque assise en retrait d'un bloc | grès lisse, bandeaux de grès taillé tous les 8 niveaux, grès sculpté aux angles, trois losanges de terre cuite orange à cœur bleu par face à mi-hauteur (`y` 25..31), pyramidion d'or (5 × 5, 3 × 3, 1 × 1). Plein de grès ailleurs |
| Socle | 105 × 105, `y` −3..−1 | grès, dalle de grès taillé, bordure lisse ; chaque colonne **remplie jusqu'au premier bloc de sol** (au plus 48 blocs) ; le terrain au-dessus du socle dégagé jusqu'à `y` 16 |
| Sable adouci | 6 blocs autour du socle | talus de sable 1:1 là où le sol est plus bas |
| Chaussée | jusqu'à 14 marches, 7 de large | du socle au sable, devant l'entrée, calculée sur le terrain |
| Entrée | portail 7 × 9 (`u` −3..3, `y` 0..8), tunnel jusqu'à `v` −31 | deux piliers 2 × 2 de grès sculpté devant la façade, linteau, deux lanternes |
| Salle hypostyle | 31 × 31 × 12 (`u` −15..15, `v` −30..0, `y` 0..11) | 20 piliers 3 × 3 tous les 6 blocs de part et d'autre de la nef, lanternes suspendues, sol de grès taillé à treillis de terre cuite dans la nef et bordure bleue |
| Grande galerie | escalier de 3 de large, `v` −22 (`y` 0) → `v` 2 (`y` 24) | pente 1:1 ; dans la salle, rampe pleine à rebords ; au-delà, voûte en encorbellement (5 de large sur 3 assises, 3 sur 3, 1), lanternes sur les rebords ; la dernière marche se rétrécit à 1 devant la porte |
| Chambre de la Reine | `u` 8..16, `v` −14..−6, `y` 13..17 | au bout d'un couloir horizontal qui part de la galerie à `y` 12 ; 2 coffres, une niche avec bloc d'or et lanterne |
| Chambre du Pharaon | 15 × 9 × 9 (`u` −7..7, `v` 4..12, `y` 26..34) | sarcophage de pierre noire polie à angles d'or et couvercle de quartz, 4 coffres au trésor, blocs d'or et lanternes aux angles, frise bleue, **deux conduits 1 × 1** en escalier à 45° jusqu'aux faces latérales |
| Porte secrète | l'entrée de la chambre du Pharaon (`u` 0, `v` 3, `y` 26..27) | deux pistons collants **sortis**, tenus par un levier **allumé** caché dans l'encorbellement de la dernière volée (`u` −2, `y` 27, `v` 1) ; le levier alimente le bloc derrière lui, qui alimente le piston du haut, et celui du bas par quasi-connexion. Couper le levier rétracte les deux et ouvre la porte |
| Labyrinthe | anneau `|u|,|v|` 19..29, `y` 14..16, sol `y` 13 | 30 × 30 cellules sur une grille de 2 blocs, 576 dans l'anneau ; **labyrinthe parfait** (retour sur trace itératif) depuis la cellule où arrive l'escalier ; impasses : piège à TNT, piège à flèches ou coffre de couloir |
| Escalier du labyrinthe | `u` −17, `v` −28 → −15 | depuis une porte dans le mur gauche de la salle ; une seule ouverture dans l'anneau, donc aucun cycle |
| Crypte | 21 × 21 (`u`,`v` −10..10, `y` −12..−6) sous le socle | 4 piliers, **générateur de husks**, 2 coffres, fosse de sable où se cachent **6 sables suspects**, lanternes des âmes |
| Escalier caché | trappe de bouleau fermée dans le sol de la salle (`u` 14, `v` −3), derrière le dernier pilier de droite | 7 marches jusqu'à un palier, puis un passage dans le mur de la crypte |

**Pièges**, tous posés dans un état de redstone **cohérent** (vanilla ne réévalue pas la redstone au
chargement, `PROGRESS.json` → `vanilla_redstone_on_load`) :

- la fosse de la salle : une plaque de pierre dans la nef, neuf TNT sous le sol (la plaque alimente
  la dalle, la dalle alimente la TNT) ;
- le couloir de la Reine : un fil tendu entre deux crochets (attachés, éteints), et au-dessus du
  bloc de chaque crochet un distributeur de 16 flèches qui tire en travers ;
- les impasses du labyrinthe : plaque sur TNT, ou plaque devant un distributeur de flèches encastré
  dans le mur du fond ;
- la porte secrète ci-dessus.

**Butin** : nos tables, `data/ondes_voxel/loot_tables/` — `chests/great_pyramid/{treasure, queen,
corridor, crypt}` et `archaeology/great_pyramid` (tessons *arms up*, *brewer*, *skull*, *plenty*,
pépites, os, émeraude). Poids et objets choisis ici ; aucune table de Mojang n'a été copiée. Les
graines `LootTableSeed` des coffres et des sables suspects sont tirées du hasard du départ.

## 3. Déterminisme

Tout choix — orientation, labyrinthe, contenu des impasses, graines de butin, sables suspects — sort
d'**un** générateur, `setLargeFeatureSeed(graine, chunkX, chunkZ)` (celui des départs vanilla), dans
un ordre fixe. La décision de placement ne dépend que de la graine, du chunk et du bruit.

Les blocs sont écrits **par colonne de chunk**, dans un ordre d'opérations fixe (terrain et
enveloppe, puis les parois de toutes les salles, puis leur vide, puis le mobilier) ; les seules
lectures (remplissage sous le socle, sable, pied de la chaussée) portent sur la colonne en cours.
L'`ov_worldgen` le vérifie en posant la même pyramide dans trois ordres de chunks (avant, arrière,
mélangé) : identique bloc pour bloc et entité pour entité, et aucune écriture hors du chunk en cours.

## 4. Mesures

*(complétées ci-dessous au fil des mesures)*
