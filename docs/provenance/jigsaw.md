# Les structures jigsaw : villages, avant-postes, bastions, cités antiques, ruines de sentier

Ce document rend compte du système jigsaw de 1.20.1 tel qu'il est reproduit dans
`src/ov_worldgen/src/jigsaw_*.cpp`, et **de la mesure qui a fixé chaque règle**. Les neuf
structures `minecraft:jigsaw` de 1.20.1 sont couvertes : villages ×5 (plaines, désert, savane,
taïga, neige), avant-poste pillard, vestige de bastion, cité antique, ruines de sentier.

> Une structure jigsaw ne se tire pas, elle **pousse**. Un pool de départ donne la première
> pièce ; chaque bloc jigsaw d'une pièce posée nomme un pool où tirer une voisine et un nom que le
> bloc jigsaw de la voisine doit porter ; la voisine est tournée et glissée jusqu'à ce que les deux
> blocs se fassent face, gardée si sa boîte tient dans l'espace encore libre, et pousse à son
> tour, en largeur d'abord, jusqu'à épuisement de la profondeur. Tout sort d'**un seul** aléa, dans
> **un seul** ordre : un tirage déplacé fait pousser un village plausible qui ne partage rien avec
> celui du jeu.

## 1. L'oracle : les pièces que le jeu a stockées

Un chunk que le jeu a mené jusqu'à `structure_starts` porte déjà, dans `structures.starts`, toutes
les pièces des départs jigsaw dont il est le chunk de départ — bien avant le moindre bloc. Chaque
pièce (`id` `minecraft:jigsaw`) stocke : l'élément de pool (`pool_element`, sérialisé), la position
(`PosX/PosY/PosZ`), la rotation, la boîte (`BB`), le décalage de sol (`ground_level_delta`) et les
**jonctions** (`junctions` : `source_x`, `source_ground_y`, `source_z`, `delta_y`, `dest_proj`).
C'est un oracle exact, sans tolérance : la pièce est la même ou elle ne l'est pas.

Les quatre mondes de référence en portent **34 départs** (un doublon entre deux mondes de même
graine), soit 2 814 pièces :

| structure | départs | monde |
|---|---:|---|
| bastion_remnant | 9 | `reference-nether-987654321` (Nether) |
| village_plains | 7 | `reference-1234567890`, `struct-locate-1234567890` |
| ancient_city | 6 | `reference-1234567890`, `struct-locate-1234567890` |
| pillager_outpost | 4 | idem |
| trail_ruins | 3 (+1 doublon) | `reference-1234567890`, `reference-987654321` |
| village_desert | 2 | `reference-1234567890` |
| village_taiga | 1 | `reference-987654321` |
| village_savanna | 1 | `struct-locate-1234567890` |
| village_snowy | 0 | — aucun départ dans les mondes existants |

`tools/ov_jigsawparity` relit ces départs, fait pousser les mêmes depuis la graine et notre bruit,
et compare **pièce par pièce** (élément, position, rotation, boîte, décalage, jonctions) puis
**tag par tag** le NBT que notre serveur écrirait (types compris). `--control` fait pousser chaque
départ avec la graine + 1 : le témoin décalé qu'une comparaison honnête doit rater.

## 2. Ce que la mesure a fixé

Un prototype hors dépôt a d'abord tranché l'ordre des tirages sur les structures qui n'ont
**besoin d'aucun terrain** — pièces rigides, hauteur de départ constante, pas de projection : le
bastion et la cité antique. Toute la structure y découle de la graine et des pools.

| hypothèse | bastions | cités antiques |
|---|---:|---:|
| **rotation, puis gabarit** (retenu) | **9/9** (985 pièces) | **6/6** (524 pièces) |
| gabarit, puis rotation (témoin décalé) | 2/9 | 1/6 |

### 2.1 Le départ

* La hauteur de départ : toutes celles de 1.20.1 sont des ancres `absolute` constantes, qui ne
  tirent rien. Une autre forme tirerait avant la rotation ; elle est refusée par nom au chargement.
* Puis **une rotation** (`nextInt(4)`), puis **un gabarit** du pool de départ
  (`nextInt(taille de la liste pondérée)`). Un élément vide ne démarre rien.
* Un **jigsaw de départ nommé** (`start_jigsaw_name` : la cité antique et son `city_anchor`) :
  les blocs jigsaw de la pièce sont **mélangés une fois de plus** et le premier qui porte ce nom est
  posé sur la position de départ.
* Puis la pièce monte ou descend pour que son **sol** — le bas de sa boîte plus son
  `ground_level_delta` (1) — rejoigne la hauteur projetée : `WORLD_SURFACE_WG` au milieu de sa boîte
  pour les structures projetées, la hauteur de départ sinon. Conséquence mesurée : sans projection,
  la pièce **descend de son décalage** ; les neuf bastions du monde de référence sont à y = 32 pour
  une hauteur de départ de 33.

### 2.2 L'ancre du filtre de biome — le désaccord du § 5 de `structures.md`, levé

Le biome qui décide si la structure démarre est lu **au milieu de la boîte de la pièce de départ,
à la hauteur projetée** (plus le décalage vertical du jigsaw nommé : la cité antique lit donc à
y = −27, sa hauteur de départ). Ni le milieu du chunk ni son coin : c'est pour ça qu'aucune colonne
fixe ne satisfaisait à la fois le village de (3006, 10) et l'avant-poste de (−84, 105). Un pool de
départ qui tire l'élément vide, ou une pièce sans le jigsaw nommé, refuse le départ ; l'ensemble
passe alors au membre suivant comme pour un mauvais biome (`PlacementDecision::StartRefused`).

### 2.3 La pousse

Pour chaque pièce sortie de la file (en largeur d'abord) :

1. ses blocs jigsaw, listés dans l'ordre (y, x, z) du gabarit — qui est aussi l'ordre du fichier
   pour les 1.20.1 —, **mélangés** (`Util.shuffle` : de la fin, chaque case échangée avec une tirée
   en dessous ; un mélange d'un élément ne tire rien) ;
2. pour chacun : le pool nommé et son repli ; un pool inconnu, ou vide sans être
   `minecraft:empty`, saute ce bloc ;
3. l'espace : si le bloc devant le jigsaw est **dans la boîte de la pièce**, l'espace est celui de
   la pièce (sa boîte, moins ce qui y a déjà été posé) ; sinon celui que la pièce a hérité ;
4. les candidats : la liste pondérée du pool **mélangée** (sauf à la profondeur maximale), puis
   celle du repli **mélangée à part** ; un élément vide arrête la recherche ;
5. pour chaque candidat, les **quatre rotations mélangées**, et pour chacune ses blocs jigsaw
   **mélangés** : le premier qui s'accroche (faces opposées, sommets égaux sauf si le joint du
   parent est `rollable`, cible du parent = nom de l'enfant) donne la position ;
6. la hauteur : rigide sur rigide, par l'arithmétique du joint ; sinon `WORLD_SURFACE_WG` à la
   colonne du jigsaw parent, moins la hauteur du jigsaw enfant ;
7. le **« expansion hack »** (villages, avant-postes) : une pièce de 16 blocs de haut au plus
   réserve au-dessus d'elle la hauteur du plus haut élément que ses propres jigsaw intérieurs
   pourraient apporter ;
8. la pièce est gardée si **chaque bloc de sa boîte** est dans l'espace et hors de toute boîte déjà
   posée dans cet espace (le jeu teste la boîte rétrécie d'un quart de bloc contre une forme
   voxelisée ; les boîtes étant entières, c'est la même chose) ; l'espace initial est un cube de
   `max_distance_from_center` autour de l'ancre, moins la pièce de départ ;
9. les jonctions sont notées sur les deux pièces, et l'enfant rejoint la file si sa profondeur le
   permet.

### 2.4 Ce que 1.20.1 n'a pas

Les priorités de sélection et de placement des blocs jigsaw arrivent après 1.20.1 : sur les
**2 920** blocs jigsaw des gabarits des cinq familles, aucun ne porte `selection_priority` ni
`placement_priority` (tous : `name`, `target`, `pool`, `final_state`, `joint`, `id`). Il n'y a donc
rien à trier.

### 2.5 Le gabarit manquant

`ancient_city/walls/intact_horizontal_wall_stairs_5` est nommé par un pool et **absent du jar**.
Le jeu pose alors un gabarit vide (taille 0, aucun bloc, aucun jigsaw) — dont la boîte, par
l'arithmétique de la taille − 1, déborde d'un bloc derrière la position. Sans cette règle, les six
cités antiques échouent ; avec, 6/6.

## 3. Résultats

`tools/ov_jigsawparity`, pièces tirées de la graine et de **notre** bruit, contre les départs
stockés. « Identiques » : toutes les pièces égales (élément, position, rotation, boîte, décalage,
jonctions) ; « NBT » : la liste `Children` que notre serveur écrirait, égale tag pour tag, types
compris ; « placés » : notre placeur démarre bien cette structure dans ce chunk (ancre jigsaw).

| monde | départs | identiques | NBT | placés | pièces |
|---|---:|---:|---:|---:|---:|
| `reference-nether-987654321` (bastions) | 9 | **9** | **9** | 9 | **985 / 985** |
| — témoin graine + 1 | 9 | 0 | 0 | — | 0 / 985 |
| `reference-1234567890` | 18 | **18** | **18** | 18 | **1 427 / 1 427** |
| `reference-987654321` | 2 | **2** | **2** | 2 | **72 / 72** |
| `struct-locate-1234567890` | 4 | 2 | 2 | 4 | voir § 4 |

Détail de `reference-1234567890` : cités antiques 5/5 (438 pièces), avant-postes 3/3, ruines de
sentier 2/2, villages du désert 2/2 (270 pièces), villages des plaines 6/6 (647 pièces).

### 3.1 La hauteur des joints : `WORLD_SURFACE_WG` passe par l'aquifère

Les pièces `terrain_matching` et le départ projeté lisent `WORLD_SURFACE_WG` sur le bruit. La
première version reprenait la règle de l'échantillonneur du placement (`structures.md` § 5) :
« premier bloc solide, ou la mer globale sous le niveau de la mer ». Mesuré, dans un seul binaire
(`--aquifer-surface`) :

| règle | `reference-1234567890` | village de taïga (1895, −1859) |
|---|---:|---:|
| mer globale sous y = 63 | 16/18 | 0/1 — départ à y 62 contre 57 |
| **ce que l'aquifère pose** (air, eau, lave, solide) | **18/18** | **1/1** |

Le village de taïga est dans une poche **sèche sous le niveau de la mer** : le jeu y trouve la
surface à 58, la règle globale la noyait jusqu'à 63. Même chose, d'un bloc, pour une jonction du
village de (3006, 10). La hauteur de colonne du jeu est donc celle des blocs que l'étage de bruit
écrirait, aquifère compris.

## 4. Les deux départs de `/locate` : sans « expansion hack »

Les deux désaccords de `struct-locate-1234567890` — le village des plaines de (18, 124) et
l'avant-poste de (−84, 105) — ont un point commun que le journal du serveur de ce monde révèle :
ce sont **exactement les deux départs jigsaw à expansion qu'un `/locate` a fait générer** dans la
première session (`The nearest minecraft:village_plains is at [288, ~, 1984]`,
`... pillager_outpost is at [-1344, ~, 1680]`). Aucune de leurs pièces n'a de boîte agrandie
(0 sur 108 et 0 sur 17), alors que chaque autre départ en porte (17 à 40 par village, 3 par
avant-poste), exactement là où nous les mettons. Avec l'expansion coupée
(`OV_JIGSAW_EXPANSION_HACK=0`, instrument de mesure), l'avant-poste redevient identique (17/17) et
le village de savane du même monde, généré normalement, casse.

Retenu : l'expansion telle qu'une génération ordinaire la fait. Le chemin `/locate` n'est pas
modélisé ; il est nommé ici, avec le seul fait mesuré qui le distingue.

## 5. Niveau A — les pièces du jeu, posées par notre code

`tools/ov_structblocks --level=a` (même harnais que `structures.md` § 16) : les pièces stockées
dans les départs `full`, posées chunk par chunk par `JigsawLibrary::place`, comparées état complet
aux blocs du jeu. Témoin : chaque pièce un quart de tour à côté.

| départ | blocs identiques | témoin | coffres posés / table / graine |
|---|---:|---:|---:|
| village des plaines (3006, 10) | 3 573 / 4 005 — 89,213 % | 336 / 3 831 — 8,771 % | 5 / 5 / 4 |
| village des plaines (18, 124) | 3 002 / 3 529 — 85,067 % | — | 2 / 2 / 2 |
| avant-poste (−84, 105) | 1 495 / 1 501 — **99,600 %** | — | 1 / 1 / 1 |

Les écarts, nommés :

* **`structure_void` écrit là où le jeu a de l'air** (186 et 218 blocs) : un vide de structure
  n'est jamais posé, la position garde ce qu'elle avait (minecraft.wiki, *Structure Void*). Corrigé
  après cette mesure (les éléments ignorent `structure_void`).
* **`dirt_path` au lieu de planches, feuilles, herbe** : artefact du niveau A. Sa hauteur
  `WORLD_SURFACE_WG` est lue sur la carte de hauteurs du monde **fini**, qui contient déjà les
  maisons et les arbres ; la gravité des routes (`terrain_matching`) les pose donc dessus. Le niveau
  qui compte est l'export du serveur (§ 6), où la hauteur vient du bruit.
* **Les éléments `feature`** (`oak`, `flower_plain`, `pile_hay`…) ne sont pas posés : ils demandent
  le registre des features, que le constructeur de structures n'a pas. Nommés au journal.
* Avant-poste : 6 connexions de clôtures de chêne noir, la mise à jour de forme à la frontière des
  pièces.

## 6. Le coût de la portée, et ce qui le rend bon marché

Une pièce jigsaw peut tomber loin de son chunk de départ : l'étage de structures, qui pose chaque
chunk à partir des départs qui le traversent, doit donc interroger les départs d'un large
voisinage. La première version portait la portée globale de l'étage de 3 à 10 chunks : chaque
chunk demandait **21 × 21 = 441** chunks voisins, et pour chacun les **19** ensembles de
structures, au lieu de 7 × 7 = 49.

### 6.1 La portée de chaque ensemble, calculée, pas devinée

Une pièce tient dans `max_distance_from_center` autour de l'ancre ; l'ancre — le milieu de la
pièce de départ — est à moins de l'étendue horizontale de la pièce de départ (plus le décalage du
jigsaw nommé) du coin du chunk. D'où, calculée au chargement depuis le pool de départ
(`JigsawConfig::reach_chunks`) :

| structure | R | étendue du départ | décalage du jigsaw nommé | ⌈R/16⌉ | portée sûre |
|---|---:|---:|---:|---:|---:|
| ancient_city | 116 | 40 | 20 | 8 | **12** |
| bastion_remnant | 80 | 47 | 0 | 5 | **9** |
| village_taiga | 80 | 21 | 0 | 5 | **8** |
| autres villages, avant-poste, ruines de sentier | 80 | 6–16 | 0 | 5 | **7** |

⌈R/16⌉ seul **perdrait des pièces** ; et la portée globale de 10 coupait déjà jusqu'à deux
chunks du bord d'une cité antique. L'étage garde une borne (`kReach` = 12, pour les caches et la
recherche des `References`) et interroge chaque ensemble à **sa** portée : 3 pour les gabarits,
celle de sa plus grande structure pour un ensemble jigsaw.

### 6.2 Les candidats de la grille, pas le carré

Un ensemble à écartement n'a qu'**un** chunk candidat par cellule de sa grille. Au lieu de
demander aux 441 chunks du carré ce que chacun des 19 ensembles y démarre, l'étage énumère, pour
chaque ensemble, les candidats des cellules qui recoupent sa portée : pour un village (écartement
34, portée 7), une à quatre cellules. `References` passe par la même énumération (portée + 1 pour
la marge d'adaptation du terrain), et non plus par 27 × 27 appels.

### 6.3 Ce qui ne change jamais n'est calculé qu'une fois

Le serveur vide l'étage au début et à la fin de chaque carré de génération, pour que les
structures d'un carré ne dépendent que de la graine et du carré. Mais un départ jigsaw est
**entièrement réglé quand il pousse** — ses hauteurs viennent du bruit, jamais du terrain posé —
et rien ne le modifie ensuite : le garder d'un carré à l'autre ne change aucun bloc, et évite de
refaire pousser un village par chacun des carrés qu'il traverse. `clear` garde donc les décisions
sans départ et les départs jigsaw ; un départ à gabarit, dont la hauteur se règle sur le terrain où
il est posé pour la première fois, part comme avant. Le cache reste par pile de génération (aucun
partage entre fils, aucun verrou) et `trim` le borne autour du chunk en cours.

### 6.4 Mesure

`ov_gendet` (le chemin du serveur, pool de tâches à deux fils), un carré de 4 × 4 chunks autour du
village des plaines de (3006, 10), graine 1234567890, en **secondes CPU** (`/usr/bin/time`,
utilisateur) : la machine est partagée avec une quinzaine d'agents et le temps mural d'un bras
dépend de ce que font les autres. L'ancien étage reste dans le binaire comme instrument
(`OV_STRUCT_STAGE=scan OV_STRUCT_REACH=10`) : avant et après sortent du même binaire.

### 6.5 Ce que les secondes disent, et ce qu'elles ne disent pas

La parité ne bouge pas : après le changement, bastions **9/9** (985/985 pièces, témoin graine + 1 :
0/985), `reference-1234567890` **18/18** (1 427/1 427), `reference-987654321` **2/2**,
`struct-locate` 2/4 (les deux départs de `/locate`, § 4) — toujours **29/31**. Le monde série et le
monde parallèle d'`ov_gendet` restent **identiques** dans tous les bras.

Les secondes, elles, ne tranchent pas. Mesurées pendant que la machine portait une charge de 73
sur 8 cœurs (cinq outils de génération d'autres agents en même temps), les secondes CPU d'un même
bras varient de ±50 % d'une série à l'autre, et l'ordre des bras s'inverse :

| série (Release, 16 chunks × 2 bras, s CPU) | sans structures | portée 3 | nouvel étage | ancien étage, portée 10 |
|---|---:|---:|---:|---:|
| 1 | 49,6 | 57,8 | 57,4 | 61,3 |
| 2 | 44,1 | 86,1 | 80,9 | 70,1 |

(Debug, ancien code : 344,7 sans structures, 413,6 à portée 3, 349,4 à portée 10 — la portée 3
plus chère que la portée 10, ce qui n'a pas de sens autrement que par le bruit.) Le débit, pour
fixer les ordres de grandeur : **~0,6 chunk/s CPU en Release, ~0,09 en Debug**, structures
comprises ; et sur 144 chunks autour du village, l'étage de structures coûte **+3,5 %** du CPU
(§ 8).

La mesure retenue est donc le **travail**, compté et déterministe : combien de verdicts de
placement l'étage calcule, combien de départs il fait pousser, combien de hauteurs et de biomes il
demande à l'échantillonneur (une hauteur est une colonne de bruit, le coût d'un départ jigsaw).
L'étage le journalise en se détruisant ; `.scratch`-libre, `bench_work.sh` le somme par bras.

*(compte du travail : § 6.6)*

## 8. Posé par le serveur, relu comme le jeu l'écrit

`ov_gendet --export` génère par **le chemin du serveur** (`GeneratedWorld::generate_square`,
étage attaché, échantillonneur de surface par l'aquifère) et écrit les régions par
`world::to_nbt` ; `scripts/measure_structures.py` compare aux régions du jeu. Carré des chunks
(3000..3011, 4..15) autour du village des plaines de (3006, 10), graine 1234567890 : 144 chunks
exportés, 77 finis chez le jeu.

| mesure | résultat |
|---|---|
| départs du jeu dans les chunks exportés | 1 — trouvé, **identique champ pour champ, types compris** |
| pièces comparées | **128 / 128** identiques |
| départs chez nous et pas chez le jeu | 0 |
| `References` (chunk, structure) | **70 / 70** |
| blocs dans les boîtes des pièces du jeu | 47 879 / 51 190 — **93,532 %** |
| témoin : le même terrain exporté sans structures (`OV_STRUCTURES=0`) | 45 013 / 51 190 — 87,933 % |

Le témoin est faible — la plupart du volume des boîtes est du terrain et de l'air sur lesquels
les deux côtés s'accordent déjà — mais il situe l'écart : le village ajoute 5,6 points, et ce qui
manque est surtout l'adaptation du terrain (§ 8.1) et les éléments `feature` (arbres, fleurs,
meules de foin, § 5).

Le coût, build Release, chemin du serveur, ces 144 chunks : **196,2 s CPU avec les structures,
189,5 s sans** — l'étage de structures, villages compris, coûte +3,5 %.

### 8.1 L'adaptation du terrain : refusée, et pourquoi

Les villages, avant-postes, cités antiques et ruines de sentier déclarent un
`terrain_adaptation` (`beard_thin`, `beard_box`, `bury`) : le terrain autour d'eux est adouci ou
creusé pendant l'étage de bruit. Dans 1.20.1, ce terme **n'est pas dans les données** : le
`final_density` du routeur de bruit de l'Overworld ne contient aucun `minecraft:beardifier`, et
notre interpréteur de fonctions de densité ne le connaît pas. Le jeu l'ajoute dans son propre code,
avec un noyau que la documentation ne décrit pas ; le reconstituer depuis ce code serait le
traduire, ce que les règles du dépôt interdisent. Ce n'est pas fait, et c'est dit : les blocs de
terrain autour d'un village sont ceux du bruit seul, et l'écart du tableau ci-dessus en contient la
part.

## 7. Rejouer

```bash
cmake --build --preset macos-debug --target ov_jigsawparity test_ov_worldgen
./build/macos-debug/bin/test_ov_worldgen "[jigsaw]"
./build/macos-debug/bin/ov_jigsawparity --world=run/reference-1234567890/world --seed=1234567890
./build/macos-debug/bin/ov_jigsawparity --world=run/reference-1234567890/world --seed=1234567890 --control
./build/macos-debug/bin/ov_jigsawparity --world=run/reference-nether-987654321/world --seed=987654321 --dimension=nether
./build/macos-debug/bin/ov_jigsawparity --world=run/reference-987654321/world --seed=987654321 --aquifer-surface
# § 4 — l'expansion coupée, instrument
OV_JIGSAW_EXPANSION_HACK=0 ./build/macos-debug/bin/ov_jigsawparity --world=run/struct-locate-1234567890/world
# § 6 — l'ancien étage (carré, portée 10, tout oublié par carré) contre le nouveau, un binaire
OV_STRUCT_STAGE=scan OV_STRUCT_REACH=10 ./build/macos-release/bin/ov_gendet --origin=751,2 --side=1 --workers=2
./build/macos-release/bin/ov_gendet --origin=751,2 --side=1 --workers=2
# § 8 — le chemin du serveur, exporté et comparé
./build/macos-release/bin/ov_gendet --export=.scratch/v/region --chunks=3000,4,3011,15
python3 scripts/measure_structures.py .scratch/v/region run/reference-1234567890/world/region
```

## Sources

Les JSON du data generator (`worldgen/template_pool/`, `worldgen/structure/`,
`worldgen/processor_list/`), les gabarits du jar serveur 1.20.1 (lus en mémoire, jamais extraits
ni commités), et le NBT des départs stockés dans les mondes de référence. Les pages
*Jigsaw Block*, *Template pool* et *Structure* de minecraft.wiki pour le vocabulaire (pool, repli,
joint, projection, jonction). Aucun code tiers, aucun code du jeu : chaque règle ci-dessus a été
retenue parce qu'elle redonne les pièces stockées, et le témoin décalé dit ce que vaut l'accord.
