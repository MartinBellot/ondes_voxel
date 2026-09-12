# Génération, troisième passe : icebergs, fossiles, couche gelée, fluides réveillés, zoom des biomes

*2026-09-11. Mandat `worldgen-3`. Graine 1234567890 sauf mention contraire ; graine hors
échantillon 987654321. Mondes de référence : `run/reference-1234567890`,
`run/reference-987654321` (générés par le vrai serveur 1.20.1, déjà sur disque). Mondes sonde :
`.scratch/probe-*` (scripts/probe_tree.sh), effacés une fois lus.*

Ce document dit ce qui a été construit, comment c'est mesuré, avec quels chiffres, et ce qui ne
l'est **pas**. Les sources sont la documentation du jeu (wiki : « Iceberg », « Fossil », « Snow »,
« Biome ») et FIPS 180-4 pour SHA-256 ; aucune ligne ne vient d'un code décompilé. Toute règle
dont la documentation ne fixe pas l'ordre des tirages est **mesurée** contre le vrai serveur, avec
un témoin qui échoue.

## 0. Le recensement qui a fixé l'ordre

`.scratch/census.py` sur un chunk `full` sur quatre du monde de référence (1 273 chunks) :

| bloc | nombre | posé par |
|---|---:|---|
| `packed_ice` | 55 943 | passe iceberg de la surface + feature `iceberg` |
| `pointed_dripstone` | 13 890 | spéléothèmes (déjà construits, 59 %) |
| `snow` | 9 643 | `freeze_top_layer` |
| `sculk` + `sculk_vein` | 11 065 | `sculk_patch`, `multiface_growth` |
| `ice` | 6 072 | `freeze_top_layer` |
| `blue_ice` | 948 | feature `blue_ice`, `iceberg_blue` |
| coraux | ~443 blocs de corail tube | coraux (déjà construits, 37–53 %) |
| `bone_block` | 135 | `fossil` |

## 1. La passe iceberg des océans gelés (surface)

`surface_extension.cpp`, branché dans `SurfaceSystem::build_column` : le biome est lu une fois,
un bloc au-dessus du sommet de la colonne ; dans `frozen_ocean` et `deep_frozen_ocean`, *après*
les règles, des piliers de glace compactée (et un chapeau de neige) sont empilés dans l'air et
dans l'eau. Trois bruits du jeu (`iceberg_pillar`, `iceberg_pillar_roof`, `iceberg_surface`),
un tirage positionnel par colonne (le générateur du monde, `at(x, 0, z)`), et la « fonte
légère » : −2 blocs quand la température du biome **avec son modificateur `frozen`** dépasse 0,1
au niveau de la mer. La colonne s'arrête au *niveau de surface minimal* du contexte : la surface
préliminaire interpolée entre les quatre coins de la cellule de chunk, −8, + la profondeur de
surface de la colonne.

Les températures sont trois bruits simplex à graine fixe (`climate_noise.cpp`) :
`BIOME_INFO_NOISE` (2345, octave 0), `TEMPERATURE_NOISE` (1234, octave 0),
`FROZEN_TEMPERATURE_NOISE` (3456, octaves −2, −1, 0 ; poids 1/7, 2/7, 4/7, l'octave 0 construite
en premier).

### Le harnais devait changer d'abord

`ov_surfparity` rangeait la glace compactée parmi le **terrain** : le pilier du jeu redevenait de
la pierre, et c'est son sommet qu'on comparait à nos règles. D'où les 65,06 % de `frozen_ocean`
de `surface-rules.md` § 5, et d'où l'impossibilité de voir une passe qui n'écrit que dans l'air et
l'eau. Dans une colonne d'océan gelé (biome du jeu en y = 62), la glace compactée, la glace bleue
et la neige en bloc redeviennent de l'eau sous 63 et de l'air au-dessus ; ailleurs elles restent
du terrain (les pics gelés les posent par leurs règles). Le seul effet de ce changement sur le
score habituel : `frozen_ocean` passe de 65,06 % à **92,24 %**, parce qu'on y compare enfin le
fond marin.

Un second compte est ajouté : chaque cellule au-dessus du fond marin, glace du jeu contre la
nôtre.

### Les chiffres

`ov_surfparity --chunks=250 --per-region=4`, même binaire, 688 colonnes d'océan gelé :

| | glace du jeu | la nôtre | glace des deux côtés | même bloc |
|---|---:|---:|---:|---:|
| passe coupée (`OV_SURFACE_PASSES=0`) | 2 347 | 0 | 0 | 0 |
| **passe active** | 2 347 | 4 333 | **2 347 (100 %)** | **2 347** |
| témoin décalé de 37 blocs (`OV_SURFACE_PASS_SHIFT=37`) | 2 347 | 0 | 0 | 0 |

**Toute** la glace du jeu est reproduite, au bloc près — glace compactée et neige en bloc à leur
place. Le témoin décalé ne tombe sur aucun pilier : l'échantillon ne compte que 688 colonnes
(trois chunks d'océan gelé), trop peu pour qu'un décalage retombe dans une zone de piliers ; il est
donné tel quel et il ne prouve rien de plus que « le motif n'est pas partout ».

Reste un excès : 1 986 cellules de glace chez nous que le jeu n'a pas. Le diagnostic ajouté au
harnais le localise : 934 au-dessus de la mer, 1 016 au-dessous, et surtout **54 colonnes où nous
seuls gelons** — là où les deux côtés ont de la glace, notre sommet est celui du jeu dans 182
colonnes sur 190. Le zoom des biomes (§ 1.1) n'en retire que trois (54 → 51) : ce n'est donc
**pas** un effet de bord de biome, et l'excès reste **non expliqué**.

### 1.1 Les règles de surface à travers le zoom

Le contexte des règles de surface lit lui aussi son biome à travers `BiomeManager` dans le jeu.
`ZoomedQueries` le fait maintenant pour les règles et pour les deux passes ; une cellule d'un
voisin, que le zoom atteint au bord du chunk, est répondue par la source de biomes (exacte à la
cellule près) — dans le générateur comme dans `ov_surfparity`. `OV_BIOME_ZOOM=0` pour l'avant,
même binaire, 250 chunks :

| | blocs | colonnes entières | erreurs au bord d'une cellule de biome |
|---|---:|---:|---:|
| sans zoom | 429 999 / 439 111 (97,925 %) | 58 489 / 64 000 (91,389 %) | 2 224 |
| **avec zoom** | **431 596 / 439 241 (98,259 %)** | **59 052 / 64 000 (92,269 %)** | **1 015** |

Le passage sans zoom reproduit au bloc près la mesure d'avant ce changement. Les erreurs au
bord d'une cellule de biome — 22,9 % du reste dans `surface-rules.md` § 5 — sont plus que
divisées par deux.

## 2. Les features construites

| type | fichier | ce qui est posé |
|---|---|---|
| `iceberg` | `ice_feature.cpp` | corps rond ou elliptique au-dessus de la mer, racine plus raide dessous, lissage, évidement ; neige en bloc au sommet une fois sur trois environ |
| `blue_ice` | `ice_feature.cpp` | une graine sous la mer contre de la glace compactée, puis 200 essais autour d'elle au contact de glace bleue déjà posée |
| `fossil` | `fossil_feature.cpp` | deux gabarits du jar serveur (os, puis surcouche de minerai), enfouis 15 à 24 blocs sous le fond, refusés si plus de 4 des 8 coins de la boîte sont de l'air ou un fluide |
| `freeze_top_layer` | `freeze_feature.cpp` | glace sur l'eau source du sommet, neige en couche sur le bloc du sommet, `snowy` sur le bloc dessous |

Chargement : **182 des 194** features configurées (177 avant), et la couche gelée en plus
une fois construite. Restent refusés et nommés : `monster_room` (autre mandat), `desert_well`,
`bonus_chest`, les deux champignons (leur survie lit la lumière), `sculk_patch` et `sculk_vein`,
`seagrass_simple` (`carving_mask`), les champignons géants plantés du Nether,
`void_start_platform`.

### 2.1 Les fossiles et les gabarits

Les gabarits sont lus **à l'exécution** dans le jar serveur par `TemplateLibrary` (famille
`fossil/`, 16 fichiers), jamais extraits ni commités. `FeatureContext.templates` les porte
jusqu'à la feature ; le décorateur les reçoit par `Decorator::set_templates`, le serveur les
ouvre pour l'Overworld seulement. Sans jar, un fossile ne pose rien et le journal le dit.

Deux choses que la documentation ne fixe pas et qui sont des choix ouverts, tranchés par la
sonde (§ 2.5) : le processeur `block_rot` tire dans le **générateur de la feature**, bloc par bloc
dans l'ordre du gabarit (`StructurePlaceSettings.setRandom`), et non dans un générateur semé par
la position comme pour les pièces de structure ; et la palette est tirée même quand il n'y en a
qu'une (`nextInt(1)` tire). `ProcessorContext.shared_random` et
`PlaceSettings.processor_random` portent le premier choix sans changer le comportement des
structures, qui n'en donnent pas.

### 2.2 La couche gelée

Pour chacune des 256 colonnes, au sommet de `MOTION_BLOCKING` (qui compte l'eau) : la source
d'eau sous le sommet gèle si le biome est froid **à cette hauteur** ; le bloc du sommet reçoit
une couche de neige si c'est de l'air, si le biome y est froid, et si la neige tiendrait (ni
glace ni glace compactée dessous ; face du dessus pleine, ou bloc des tags qui la portent) ; le
bloc dessous passe `snowy=true` s'il a la propriété. La glace d'abord : aucune neige sur la
glace faite dans la même colonne. « Froid » : la température du biome après son modificateur et
après le refroidissement au-dessus de y = 80, sous 0,15.

Approximations nommées : la lumière de bloc d'un chunk en génération est nulle, donc le test
« < 10 » passe toujours.

Le harnais (`ov_features --freeze`) retire du chunk central toute la neige en couche, remet
toute la glace en eau source et tout `snowy` à faux, puis rejoue notre feature : c'est exact au
protocole près, parce que la couche gelée est la dernière feature du chunk et n'écrit que ses
propres colonnes. Correction au passage : la `ReferenceLevel` de l'outil rangeait
`MOTION_BLOCKING` avec le fond marin (sans fluide) ; elle a désormais trois cartes.

### 2.3 Le zoom flou des biomes

Le jeu ne lit jamais le biome de la cellule qui contient un bloc : `BiomeManager` décale la
lecture d'environ une demi-cellule, avec une graine qui est le SHA-256 de la graine du monde
(les huit premiers octets du condensé des huit octets petit-boutistes de la graine, relus
petit-boutistes). Chacun des huit coins autour du bloc reçoit un décalage pseudo-aléatoire issu
d'un générateur congruentiel linéaire mêlant ses coordonnées et cette graine ; le coin le plus
proche gagne. `biome_zoom.{hpp,cpp}` — SHA-256 écrit depuis FIPS 180-4 et vérifié sur ses trois
vecteurs de l'annexe B.

Branché là où le jeu pose une question de biome au monde pendant la décoration : le filtre
`biome` de chaque placed feature, et la couche gelée. `OV_BIOME_ZOOM=0` est l'instrument qui
donne l'avant depuis le même binaire. **Pas branché** dans les règles de surface : leur contexte
passe lui aussi par le zoom dans le jeu, mais une cellule voisine hors du chunk y demande la
source de biomes, et les requêtes de surface (celles du générateur comme celles du harnais) ne
savent répondre que pour leur propre chunk. Nommé, c'est la suite naturelle : 22,9 % des erreurs
de surface sont au bord d'une cellule (`surface-rules.md` § 5).

### 2.4 Icebergs et glace bleue : la sonde

Monde sonde `probe-ocean-ice`, graine 1234567890, contre son témoin sans feature : trois placed
features à nous à l'étape 9 — `rarity_filter(3) → in_square → iceberg_packed`, la placed
`blue_ice` de vanilla telle quelle, `rarity_filter(8) → in_square → iceberg_blue`. (Le premier
passage du script de sonde a manqué la zone océanique : le serveur a mis 73 s à démarrer sur la
machine chargée, `probe_tree.sh` envoyait ses commandes sur des attentes fixes, et le `forceload`
lointain est arrivé avant que le serveur soit prêt. Le script attend désormais la ligne « Done »
du serveur. Ces deux mondes-là sont donc la zone d'apparition : de la plaine. Un iceberg se pose au niveau de la mer quoi qu'il y ait dessous,
donc sa forme se mesure là aussi ; la glace bleue, elle, n'y trouve de l'eau que dans les
creux.)

`ov_features --probe --control=… --chunks=200`, 200 chunks comparés, 119 touchés :

| | la bonne graine | témoin décalé d'un index |
|---|---:|---:|
| blocs changés par le jeu / par nous | 20 515 / 20 770 | 20 515 / 13 717 |
| **même bloc** | **20 004 (97,509 %)** | 640 (3,120 %) |
| chunks identiques bloc pour bloc | **82 / 119 (68,9 %)** | 0 / 155 |

| bloc | jeu | nous | identiques |
|---|---:|---:|---:|
| `packed_ice` | 12 218 | 12 394 | 97,8 % |
| `snow_block` | 5 558 | 5 566 | 96,6 % |
| `blue_ice` | 2 677 | 2 748 | 98,2 % |

L'ordre des tirages — le corps (x, puis z, puis y), le lissage, la racine, l'évidement dont la
borne de boucle tire à chaque tour, le `nextBoolean` et les `nextInt` de l'évidement — tient donc
tel qu'écrit. Les différences lues (`--show`) sont des bergs qui franchissent un bord de chunk :
notre glace compactée en y 64–65 là où le jeu a de l'air, dans deux chunks voisins — l'ordre de
décoration des chunks, que ce dépôt nomme déjà pour les features denses.

### 2.5 Les fossiles : la sonde

Monde sonde `probe-fossil`, graine 1234, étape 3 : deux placed features à nous,
`in_square → height_range(0 … sommet) → fossil_coal` et
`in_square → height_range(fond … −8) → fossil_diamonds` — un fossile de chaque sorte par chunk au
lieu d'un sur 64, avec le pipeline de hauteur de vanilla. Contre le témoin sans feature,
200 chunks :

| | la bonne graine | témoin décalé d'un index |
|---|---:|---:|
| `bone_block` | **21 797 / 21 807** | 303 / 21 807 |
| `coal_ore` (surcouche) | **1 400 / 1 406** | 0 |
| `deepslate_diamond_ore` (surcouche, règle `coal_ore → deepslate_diamond_ore`) | **1 338 / 1 340** | 1 |
| blocs du fossile, tout compris | **24 535 / 24 553 (99,93 %)** | 304 (1,10 %) |
| chunks identiques bloc pour bloc | 158 / 200 | 0 / 200 |

(Les 3 186 cellules d'eau que le jeu a et pas nous sont l'eau qui s'est posée autrement entre le
monde sonde et le témoin, loin de tout fossile — les mêmes ~3 190 que dans chaque sonde de
plaine de `features.md` — et elles sont hors de ce total.)

Les deux choix ouverts du § 2.1 sont donc tranchés par la mesure : `block_rot` tire dans le
générateur de la feature, bloc par bloc dans l'ordre du fichier du gabarit (qui est déjà l'ordre
trié du jeu), et la palette est tirée même unique. Un seul des deux faux, et la moitié des os
tombait à côté.

### 2.6 Les coraux : la forme est juste, c'est le récif qui ne l'était pas

`features.md` (« L'océan ») donnait les coraux à 37–53 % et concluait « arbre, pince et
champignon tirent des dizaines de flottants ; lequel des trois est faux n'est pas isolé ».
Aucune ligne de `ocean_feature.cpp` n'a été changée ici ; la mesure, si.

Monde sonde `probe-ocean-coral`, graine 1234567890, la zone d'océan chaud de la référence
(x −105 120, z 6 960 ; régions `r.-206.13` …), trois placed features à nous à l'étape 9 :
`rarity_filter(2) → in_square → heightmap(OCEAN_FLOOR_WG) → coral_tree` (puis `coral_claw`, puis
`coral_mushroom`), chacune tenue en ligne dans son fichier — ces trois types n'ont pas de
configured feature à eux dans les données. Un corail pour deux chunks et par forme, au lieu du
récif dense de `warm_ocean_vegetation` (`noise_based_count`). Contre le témoin sans feature,
200 chunks comparés, 184 touchés :

| | la bonne graine | témoin décalé d'un index |
|---|---:|---:|
| blocs changés par le jeu / par nous | 8 806 / 8 821 | 8 806 / — |
| **même bloc** | **8 627 (97,967 %)** | 346 (3,929 %) |
| chunks identiques bloc pour bloc | **174 / 184 (94,6 %)** | 0 |

| famille | identiques |
|---|---:|
| blocs de corail (5 couleurs) | 95,1 à 100 % (cerveau et bulle 100 %) |
| éventails muraux (5) | 97,4 à 99,0 % |
| coraux et éventails posés dessus (10) | 94,5 à 100 % |
| cornichons de mer | 97,6 % |

Les trois formes et `placeCoralBlock` — la plante une fois sur quatre, sinon le cornichon une
fois sur vingt, l'éventail de chaque côté une fois sur cinq, dans l'ordre nord, est, sud, ouest —
sont donc justes à la précision près des chevauchements entre chunks voisins. Les 37–53 %
d'avant mesuraient le **récif** : des dizaines de coraux par chunk qui se recouvrent, dont
l'ordre de pose entre chunks voisins décide lequel tient. Ce n'est pas corrigé ici, et ce n'est
plus attribué aux formes.

### 2.7 La veine de sculk : mesurée, puis refusée

La veine est une `multiface_growth` dont l'étalement suit la règle de la veine (jamais contre du
sculk, un catalyseur ou un piston en mouvement ; jamais autour d'un coin qu'une face pleine
ferme ; jamais dans la lave, l'eau qui coule ou le feu ; tout `#replaceable` en plus de l'air,
des veines et des sources d'eau). Monde sonde `probe-vein` (graine 1234, la placed de vanilla à
l'étape 7), 200 chunks :

| | la bonne graine | témoin décalé |
|---|---:|---:|
| veines du jeu / les nôtres | 977 / **2 241** | 977 / 2 282 |
| veines en commun | 340 (34,8 % des siennes) | 28 |
| chunks identiques | 26 / 151 | 0 / 166 |

Le motif est pour une part le bon (340 contre 28), mais nous posons **2,3 fois** trop de veines.
Ce n'est pas une approximation acceptable : la feature est **refusée** comme avant, avec ces
chiffres dans son message, et la règle d'étalement reste dans `lush_feature.cpp` comme point de
départ. Pistes non vérifiées : la condition de pose d'origine, `canBeReplaced` de la veine
elle-même, et le marquage `PostProcessing` que le jeu fait à chaque pose.

### 2.8 Icebergs et glace bleue sur le vrai océan

Le monde sonde refait avec le script corrigé (la zone océanique de la référence, x −105 120,
z 6 960), les trois features ensemble — c'est la seule mesure qui compte, la glace bleue et
les icebergs bleus se posant dans et contre les autres :

| | la bonne graine | témoin décalé |
|---|---:|---:|
| blocs changés par le jeu / par nous | 78 481 / 77 918 | 78 481 / 76 368 |
| **même bloc** | **75 206 (95,827 %)** | 2 630 (3,351 %) |
| chunks identiques | 86 / 155 | 0 / 189 |

(Un passage avec `iceberg_packed` seul contre ce monde à trois features donnait 56,8 % : la glace
compactée y était à 97,0 %, et les 29 970 blocs de glace bleue non rejoués comptaient contre
nous. Le protocole, pas l'iceberg.)

### 2.9 Dripstone et cerisier : où ils en sont

Mesurés, **pas** corrigés ici (graine 1234, étape 9, 200 chunks chacun, chaque placed de vanilla
seule dans son monde) :

| feature | même bloc | témoin décalé | chunks identiques |
|---|---:|---:|---:|
| `large_dripstone` | **86,0 %** (15 208 / 17 675) | 16,0 % | 41 / 75 |
| `dripstone_cluster` | 59,8 % (39 780 / 66 494) | 21,0 % | 39 / 144 |
| `pointed_dripstone` | 20,2 % (3 173 / 15 718) | 6,4 % | 5 / 172 |

Le grand spéléothème est presque juste ; l'amas l'est à moitié ; la pointe isolée est loin, et
c'est elle qui compte le plus de blocs dans le monde (13 890 dans le recensement). C'est la
prochaine cible.

Cerisier (`probe-cherry`, 300 chunks, 208 arbres de notre chunk central) : troncs au bon endroit
**200 / 208 (96,2 %)**, bois 51 403 / 55 512 (**92,6 %**), témoin décalé 11,9 % ; **aucun arbre
entier identique** — ce sont les feuilles et les branches. `ROADMAP` disait « cerisier 0 % » : ce
chiffre ne décrivait plus le code.

### 2.10 La couche gelée : les chiffres et ses témoins

`ov_features --freeze`, 260 chunks de `run/reference-1234567890` (le harnais corrigé : `snowy`
compté sous une couche de neige seulement) :

| | glace | neige en couche | `snowy` |
|---|---:|---:|---:|
| jeu / nous / les deux | 1 394 / 1 393 / 1 393 | 2 455 / 2 472 / 2 386 | 1 168 / 1 120 / 1 103 |
| **rappel / précision** | **99,93 % / 100 %** | **97,19 % / 96,52 %** | 94,44 % / 98,48 % |
| témoin climat plat (`OV_CLIMATE_FLAT=1`) | 99,93 % / **86,15 %** | 97,19 % / 95,67 % | inchangé |
| témoin sans zoom (`OV_BIOME_ZOOM=0`) | **98,49 % / 97,58 %** | 97,19 % / 96,44 % | inchangé |

Les deux témoins font ce qu'on attend d'eux : sans les plaques gelées, 224 glaces de trop sur
l'océan gelé ; sans le zoom, la glace se trompe de biome au bord des cellules. L'océan gelé
(42/42 couches de neige) et la rivière gelée (5/5) sont exacts ; le reste de la neige
(68 manquées, 85 de trop) est dans la taïga enneigée, sous et sur les arbres, et n'est pas
attribué.

### 2.11 Déterminisme

`ov_gendet --side=1 --workers=4` : **1 572 864 cellules de blocs et 24 576 de biomes comparées,
0 écart** — le monde parallèle est le monde série, fossiles, icebergs et couche gelée compris.

### 2.12 L'End : le zoom referme le chorus et la passerelle en trop

`end.md` laissait le chorus à 92 % et une passerelle de retour de trop, avec pour hypothèse
l'ordre de décoration des chunks. La croissance du chorus a été relue pas à pas contre la règle
documentée (`generatePlant`, `growTreeRecursive` : tronc, branches à moins de 8 de l'origine,
fleur d'âge 5) : elle est juste. Les deux placed features en cause, `chorus_plant` et
`end_gateway_return`, se terminent par le filtre `biome` — `end_highlands` seul les liste — et le
biome de l'End change **par chunk** : là où le jeu lit le biome à travers le zoom, une position
proche du bord d'un chunk lit parfois le voisin.

`ov_endparity --full=120 --interesting`, même binaire, `OV_BIOME_ZOOM=0` pour l'avant :

| | `chorus_plant` | `chorus_flower` | blocs des chunks finis | passerelle en trop |
|---|---:|---:|---:|---|
| sans zoom | 4 388 / 4 650 (94,37 %) | 540 / 571 (94,57 %) | 99,9909 % | une, en (−1918, 64, 1824) |
| **avec zoom** | **4 612 / 4 650 (99,18 %)** | **565 / 571 (98,95 %)** | **99,9986 %** | **aucune** |

(Les 92 % d'`end.md` et les 94,37 % ici diffèrent parce que le monde de référence de l'End a été
régénéré — même graine, même serveur ; la mesure « sans zoom » est celle qui compte, faite sur le
même monde et le même binaire.) Restent 38 + 56 blocs de chorus au bord des chunks, que l'ordre
de décoration peut encore expliquer, et les 50 blocs de chorus des chunks encore non finis du
jeu (étage d'avant les features), qui sont hors de la comparaison.

### 2.13 L'ordre d'un `HashSet` Java, cases arborescentes comprises

Les arbres (décorateurs, feuillage) et les patchs de végétation des grottes luxuriantes
parcourent un `java.util.HashSet<BlockPos>` et tirent une fois par élément : l'ordre
d'itération fait partie de la graine (`java_hash_order`). Le modèle ne faisait pas les **cases
arborescentes** de `HashMap`, et le disait au journal — au niveau ERROR, depuis chaque worker de
génération, des centaines de fois par minute. Premier correctif, livré seul : un compteur
partagé, dit en WARN une fois par minute au plus avec le total.

Second correctif, la parité. Ce que la documentation de `HashMap` fixe, et qui est maintenant
modélisé (`JavaHashSet`, `tree_feature.cpp`) :

* une case en liste que l'insertion porte à **neuf** entrées (huit déjà là) devient un arbre —
  ou, tant que la table a moins de 64 cases, la table double à la place. L'ancien modèle
  doublait **une entrée trop tôt** (à huit) : une seconde divergence, corrigée du même coup ;
* l'arbre est un rouge-noir construit dans l'ordre de la liste, rangé par le hachage (comparé
  comme l'`int` signé de Java) ; sa **racine passe en tête** de la liste de la case ;
* une insertion dans une case arborescente place le nouveau nœud **juste après son parent**
  dans la liste, rééquilibre, et remet la racine en tête ;
* au doublement, une case arborescente se coupe en ses moitiés basse et haute dans l'ordre de la
  liste : une moitié de six entrées ou moins redevient une liste, une moitié qui a tout gardé
  garde son arbre, sinon chaque moitié est replantée.

Deux positions de **même hachage complet** seraient départagées par l'`identityHashCode` de la
JVM, que rien hors d'elle ne peut connaître ; parmi les quelques centaines de positions d'un
arbre, cela n'arrive pas (le hachage de `BlockPos` est injectif sur de si petites étendues).

Le test `a HashSet bin of nine in a table of 64 iterates as Java's treeified bin` fixe un cas
calculé **à la main** depuis ces règles, pas depuis le code : trente positions dans les cases
1 à 30 font monter la table à 64, neuf de hachages 0, 64 … 512 tombent dans la case 0 et la
neuvième la change en arbre ; neuf clés croissantes font de la quatrième la racine, d'où
l'ordre 192, 0, 64, 128, 256, … 512 puis le reste. Vert. `OV_HASHMAP=legacy` rend l'ancien
modèle, depuis le même binaire, pour la mesure.

**Mesure, premier échantillon** — `ov_features --trees`, 300 chunks de
`run/reference-1234567890` (forêts de bouleaux et de chênes, jungle), même binaire :

| | troncs au bon endroit | formes identiques (sur un tronc du jeu) | blocs de bois et de feuilles |
|---|---:|---:|---:|
| ancien modèle (`OV_HASHMAP=legacy`) | 239 (44,26 %) | 112 (46,86 %) | 15 851 (44,86 %) |
| modèle arborescent | 239 (44,26 %) | 112 (46,86 %) | 15 851 (44,86 %) |
| témoin, mauvaise graine | 16 (2,96 %) | 0 | 5 359 (15,17 %) |

**Identiques** : cet échantillon ne remplit jamais une case de neuf dans une table de 64 — il
ne dit donc rien du correctif, dans un sens ni dans l'autre. D'où un compteur de cases
arborescentes (`java_hash_treeified_bins`, imprimé par le rapport des arbres), et la même mesure
sur le monde hors échantillon 987654321, qui a la forêt noire et la vieille taïga d'épicéas, les
grands arbres.

## 3. Les fluides à réveiller

L'aquifère marquait déjà les fluides que le jeu réveille (`aquiferes.md` § 10.4, règle
`d₂ − d₁ < 45`, 98,3 % des marques `PostProcessing`), et la liste s'arrêtait là. Elle voyage
maintenant avec le chunk, par le chemin le plus court que le serveur offre :

* `ChunkPipeline` garde, par entrée, les marques de l'étage de bruit et des carvers ;
  `take(x, z, &marks)` les rend avec le chunk ;
* `GeneratedWorld::generate_square(…, &marks)` les recueille pour le carré ;
  `GeneratedBlock::fluid_wakeups` les porte du worker au thread de tick ;
* au moment où le thread de tick publie un chunk **généré à l'instant** (pas un chunk lu sur
  disque, qui porte ses propres ticks), chaque marque passe par
  `FluidRules::on_neighbour_changed` — exactement ce qu'un voisin modifié ferait : un tick de
  fluide planifié à son délai, rien si un tick est déjà là, rien si ce n'est plus un fluide.

### 3.1 La preuve : une cascade générée coule

`src/ov_server/tests/test_fluid_wakeups.cpp`, de bout en bout sur du terrain généré (graine
1234567890, `GeneratedWorld::generate_square`, un chunk à la fois à partir de (0, 0)) : on prend
la première marque dont le fluide a de l'air à côté ou dessous — une cascade — et on attache le
chunk à un `ServerLevel` et à un `WorldTicks`, comme le serveur.

* **le témoin** : soixante ticks sans réveil — rien ne bouge, l'air voisin reste de l'air ;
* **le réveil** : chaque marque passe par `on_neighbour_changed`, soixante ticks encore — le
  fluide est entré dans l'air (`flowed_into_air > 0`) et la cellule voisine tient maintenant un
  fluide.

Vert sous `ctest` (507 assertions). Toutes les marques rendues tombent dans leur chunk.

Différence nommée avec le jeu : le jeu fait couler le fluide **à la promotion** du chunk
(`LevelChunk.postProcessGeneration` appelle directement le tick du fluide), nous un délai de
fluide plus tard (5 ticks pour l'eau, 30 pour la lave). Et le chemin synchrone de secours
(`chunk_at`, compté par `synchronous_generations`, jamais pris par le streaming) ne réveille
rien.
