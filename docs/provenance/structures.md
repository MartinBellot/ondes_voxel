# Les structures : le placement, et rien que le placement

Ce document rend compte d'une seule chose, mesurée : **dans quel chunk le jeu démarre une
structure, et est-ce que nous disons la même chose**. Il ne parle pas de géométrie — aucun bloc de
structure n'est posé à ce stade, et c'est dit ici plutôt que laissé à deviner.

> Le choix est délibéré. Une structure au bon endroit avec une géométrie approximative reste
> utile ; une structure parfaite au mauvais endroit ne l'est pas. Et le placement est la seule
> partie de la génération dont l'oracle est un **booléen** plutôt qu'un champ bruité : sur des
> milliers de chunks, il y a une structure ou il n'y en a pas.

---

## 1. L'oracle : le jeu écrit sa réponse dans chaque chunk

Un chunk que le vrai serveur a terminé porte, dans son NBT :

```
structures: { starts: { "minecraft:mineshaft": { … } , … }, References: { … } }
```

`starts` liste les structures **dont ce chunk est le point de départ**, avec leurs pièces et leurs
boîtes englobantes ; `References` liste les chunks de départ dont ce chunk dépend. Le premier est
un oracle exact, par chunk et par type, sur des milliers de chunks : pas de seuil, pas de
tolérance, pas de pourcentage d'un champ continu.

`tools/ov_structparity` lit ce NBT et compare. Les deux erreurs sont comptées séparément, parce
qu'elles se corrigent à des endroits différents :

| | signification | où est le bug |
|---|---|---|
| **faux négatif** | le jeu place, nous non | la grille ou le sel — rien de plus tard ne le rattrape |
| **faux positif** | nous plaçons, le jeu non | le filtre de biome, ou une structure dont la génération propre a refusé après coup |

### Deux précautions dont dépendent tous les chiffres

**Piège 4 du dépôt.** Seuls les chunks `Status == minecraft:full` sont lus. Un chunk inachevé n'a
pas décidé ses structures et se lit comme « aucune », ce qui est indiscernable d'un désaccord
réel : la mesure fabriquerait des milliers de faux négatifs.

**Le halo des patchs.** `scripts/reference_world.sh` force-charge des carrés de 128 blocs. Autour
de chaque carré, une bordure de chunks atteint `full` en tant que voisine plutôt que parce qu'on
l'a demandée. `--patches-only` restreint à l'intérieur des carrés. La différence est **un chunk
sur 5 092** et elle est nommée plutôt que cachée : le chunk (−1625, 2447), à deux chunks au nord
du patch `-26000,39000`, porte `Status: full` avec `starts` **et** `References` entièrement vides,
alors que le réducteur de fréquence l'accepte (`nextDouble` = 0,002218 < 0,004) et que son biome
est `plains`, qui est dans le tag. Aucune autre explication n'a été trouvée ; le chunk est
signalé, pas absous.

---

## 2. Les quatre réducteurs de fréquence — mesurés, pas supposés

Un `structure_set` peut porter un `frequency_reduction_method` dont le nom dans le JSON est opaque
(`legacy_type_1`, `2`, `3`). Ces noms sont des numéros de version d'un accident historique, pas des
descriptions, et se tromper de réducteur donne un monde parfaitement plausible qui ne partage
aucune structure avec le vrai.

Le **mineshaft** tranche la question tout seul. Son ensemble est `spacing: 1`, donc *tous* les
chunks sont candidats et le réducteur est la totalité du placement. Sur les 5 790 chunks `full`
des deux mondes de référence :

| réducteur candidat | prédits | départs observés retrouvés (sur 23) |
|---|---:|---:|
| `default` (`nextFloat` + sel du set) | 27 | **0** |
| `legacy_type_1` (graine bricolée, un tirage jeté, `nextInt(1/f)`) | 95 | **0** |
| `legacy_type_2` (sel fixe 10387320, `nextFloat`) | 23 | **0** |
| **`legacy_type_3`** (`setLargeFeatureSeed`, **`nextDouble`**) | 24 | **23** |

Le gagnant n'est pas « le meilleur » : c'est le seul qui en retrouve un seul. Les trois autres en
retrouvent **zéro**, ce qui est le résultat qu'on attend d'un flux RNG décalé — trap 14 du dépôt,
un témoin absurde ne passe pas la mesure.

Restreint à l'intérieur des patchs force-chargés, `legacy_type_3` donne **13 sur 13, zéro faux
positif, zéro faux négatif** sur 2 268 chunks.

Le mapping retenu, et la structure qui l'a fixé :

| JSON | arithmétique | fixé par |
|---|---|---|
| `default` | `setLargeFeatureWithSalt(seed, x, z, salt)` puis `nextFloat() < f` | — |
| `legacy_type_1` | `setSeed((x>>4 ^ (z>>4)<<4) ^ seed)`, un `nextInt()` jeté, `nextInt(1/f) == 0` | pillager outpost (le seul à l'utiliser) |
| `legacy_type_2` | `setLargeFeatureWithSalt(seed, x, z, **10387320**)` puis `nextFloat() < f` | buried treasure ; le sel est **fixe**, pas celui du set (qui vaut 0) |
| `legacy_type_3` | `setLargeFeatureSeed(seed, x, z)` puis `nextDouble() < f` | mineshaft, mesuré ci-dessus |

### Les deux fonctions de graine ne sont pas interchangeables

Elles prennent les mêmes arguments et portent des noms voisins :

* `large_feature_seed(seed, x, z)` — passe la graine du monde dans `java.util.Random`, en tire
  **deux longs**, et les utilise comme multiplicateurs du chunk. Utilisée par le réducteur du
  mineshaft et par le tirage pondéré à l'intérieur d'un set.
* `large_feature_with_salt(seed, x, z, salt)` — combinaison affine, aucun tirage intermédiaire.
  Utilisée par la grille.

Les échanger produit un monde crédible et étranger. Les deux tirages de la première sont extraits
en **variables nommées** (`a`, `b`) avant d'être combinés : C++ ne séquence pas les opérandes de
`^`, donc les écrire en ligne peut inverser le flux RNG (piège 2/18 du dépôt).

---

## 3. La grille : exacte sur les 54 départs observés

`random_spread` découpe le monde en cellules de `spacing × spacing` chunks et tire un décalage dans
les `spacing − separation` premiers chunks de la cellule. `linear` tire une fois par axe,
`triangular` fait la moyenne de deux tirages — quatre tirages au total, dans l'ordre x, x, z, z.

**Mesuré, grille seule (`--no-biomes`), sur les 5 092 chunks `full` de
`run/reference-1234567890`** : les 36 départs du jeu tombent tous sur un chunk que notre grille
accepte. **Zéro faux négatif au niveau du chunk, pour les 18 ensembles à écartement.**

C'est le chiffre qui compte : un faux négatif est une grille ou un sel faux, et rien de ce qui
vient après ne le rattrape.

La grille seule dit *quel chunk*, pas *quelle structure* : sur les 36, dix étaient le bon chunk
avec le mauvais membre de l'ensemble (`ocean_ruin_cold` au lieu de `warm`, `shipwreck_beached` au
lieu de `shipwreck`, `village_savanna` au lieu de `village_plains`). C'est le filtre de biome qui
départage, et il les corrige toutes les dix.

### Le tirage à l'intérieur d'un ensemble est une boucle, pas un balayage

Un ensemble de plusieurs structures est tiré au sort **pondéré**, et si la structure tirée est
refusée elle est **retirée de la liste** et un nouveau tirage est fait *sur le même flux*. Un
balayage dans l'ordre du fichier donne le même résultat pour un ensemble de deux et un résultat
différent dès trois : avec les cinq variantes de village, le balayage prend la première dont le
biome convient et le jeu prend celle que son deuxième tirage nomme.

Mesuré : le village de plaine du chunk (3006, 10) est un faux négatif sous le balayage et correct
sous la boucle.

---

## 4. Le filtre de dimension — 1 286 fossiles du Nether dans l'Overworld

Un chunk n'interroge que les ensembles que **sa dimension** peut produire : le jeu les filtre une
fois, en intersectant le tag de biome de chaque structure avec les biomes que la source de biomes
de la dimension sait nommer.

Sans ce filtre, l'arithmétique par chunk n'a aucune raison de dire non aux structures d'une autre
dimension. `nether_fossils` a `spacing: 2` : **1 286 faux positifs sur 5 092 chunks** de
l'Overworld, plus 14 cités de l'End, 4 forteresses et 4 bastions. Avec le filtre : zéro.

C'est le genre d'erreur qu'aucun raisonnement local ne trouve — la grille est juste, le réducteur
est juste, et la structure n'a simplement rien à faire là.

---

## 5. L'ancre du filtre de biome — et ce qui n'est pas résolu

Le jeu échantillonne le biome à **une** position, qui dépend du type de la structure :

| type | ancre | hauteur |
|---|---|---|
| `desert_pyramid`, `jungle_temple`, `swamp_hut`, `igloo`, `woodland_mansion`, `ruined_portal` | colonne du milieu du chunk | `WORLD_SURFACE_WG` |
| `ocean_monument`, `ocean_ruin`, `shipwreck`, `buried_treasure` | colonne du milieu | `OCEAN_FLOOR_WG` |
| `mineshaft` | colonne du milieu | **y = 50**, fixe |
| `stronghold` | aucune — le biome a été consulté au moment de poser les anneaux | — |
| `jigsaw` (villages, avant-postes, cités, bastions, ruines) | **non résolu**, voir ci-dessous | `WORLD_SURFACE_WG` |

### `WORLD_SURFACE_WG` n'est pas le fond marin

`WORLD_SURFACE` compte l'**eau** comme quelque chose ; `OCEAN_FLOOR` non. Sur une colonne de
rivière les deux diffèrent de la profondeur d'eau. La première version de la sonde lisait le fond
dans les deux cas : sur la rivière qui traverse le chunk (3006, 10), elle échantillonnait le biome
vingt blocs trop bas. C'est exactement le piège 5 du dépôt (« la glace n'est pas du terrain »),
retrouvé sur un autre bloc.

### Ce que l'ancre jigsaw est vraiment, et pourquoi elle n'est pas là

Une structure jigsaw ne démarre pas sur une colonne fixe du chunk : elle tire un **gabarit** dans
un pool, le place de façon que sa propre boîte englobante enjambe la position de départ, et le
biome est lu là où cette pièce atterrit. Ni le coin du chunk ni son milieu ne reproduisent ça.

Les deux approximations simples ont été mesurées, dans le même binaire, sur les mêmes mondes
(`OV_STRUCT_JIGSAW_ANCHOR=corner` bascule) :

| ancre | `reference-1234567890` | `struct-locate-1234567890` | `reference-987654321` |
|---|---|---|---|
| milieu (défaut) | rappel **36/36**, précision 36/38 | rappel 12/13, précision 12/12 | 5/5, 5/5 |
| coin | rappel 35/36, précision 35/37 | rappel **13/13**, précision 13/13 | 5/5, 5/5 |

Les deux agrégats sont **identiques** — rappel 53/54, précision 53/55 — et les erreurs
individuelles sont opposées. C'est le piège 7 du dépôt en vitrine : l'agrégat ne discrimine rien,
la liste des désaccords, si.

Et l'écart n'est pas dans notre champ de biomes. Les quatre colonnes concernées ont été décodées
depuis le NBT du jeu et comparées à la nôtre :

```
chunk 3006,10   coin minecraft:river    milieu minecraft:plains   (jeu et nous, identiques)
chunk -84,105   coin minecraft:plains   milieu minecraft:river    (jeu et nous, identiques)
```

Notre biome est juste au bloc près aux quatre endroits ; le jeu place un village de plaine dans le
premier chunk et un avant-poste dans le second. Aucune colonne fixe ne satisfait les deux. **Le
résidu est la règle d'ancrage jigsaw, et rien d'autre** — il faut le système de gabarits pour la
lever, ce qui n'est pas fait.

Le défaut est le milieu, parce que c'est ce que la documentation décrit pour les structures
dispersées et parce que c'est le bras qui a le meilleur rappel sur le plus gros échantillon.

---

## 6. Les forteresses : refusées et nommées

`strongholds` est le seul ensemble en `concentric_rings`. Ses 128 positions ne sont pas une
fonction du chunk : le jeu les calcule une fois par monde, en marchant vers l'extérieur depuis
l'origine et en accrochant chacune au chunk le plus proche dont le biome est dans
`#minecraft:stronghold_biased_to`.

Elles sont **lues et stockées**, jamais approximées. Un chunk interrogé sur cet ensemble reçoit
`PlacementDecision::Unsupported`, ce qui est visible dans la sortie de `ov_structparity` et dans le
pipeline. Une grille inventée aurait placé des forteresses crédibles et fausses partout.

---

## 7. Résultats

Trois mondes, tous générés par le vrai serveur 1.20.1 :

| monde | graine | chunks `full` | départs du jeu | rappel | précision |
|---|---:|---:|---:|---|---|
| `run/reference-1234567890` | 1234567890 | 5 092 | 36 | **36/36 = 100,00 %** | 36/38 = 94,74 % |
| — intérieur des patchs seul | 1234567890 | 2 268 | 17 | **17/17 = 100,00 %** | **17/17 = 100,00 %** |
| `run/reference-987654321` | 987654321 | 698 | 5 | **5/5 = 100,00 %** | **5/5 = 100,00 %** |
| `run/struct-locate-1234567890` | 1234567890 | 1 255 | 13 | 12/13 = 92,31 % | **12/12 = 100,00 %** |

Le troisième monde a été généré pour ce travail (`.scratch/locate.sh` puis un force-load autour
des positions rendues par `/locate`), précisément parce que les deux mondes existants ne
contenaient presque que des mineshafts. Il porte neuf types distincts : pyramide du désert, igloo,
temple de la jungle, hutte de marais, village, avant-poste, ruine océanique, épave échouée,
mineshaft. **Les treize départs, tous types confondus, sont dans le bon chunk.**

Les deux faux positifs de `reference-1234567890` :

* chunk (−1625, 2447), mineshaft — le chunk de halo décrit au § 1 ;
* chunk (946, 1623), pyramide du désert — biome `minecraft:desert` à y = 75 chez nous, et le jeu
  n'y a rien mis. Non expliqué. Candidat le plus probable : la génération propre de la pyramide a
  refusé après le placement (elle sonde le terrain), ce que cette couche ne simule pas et ne
  prétend pas simuler.

### Ce qui est compté « placé » est une borne supérieure

`PlacementDecision::PlacedByPlacement` veut dire « toutes les portes que cette couche implémente
ont dit oui ». La génération propre de la structure n'est pas lancée. Pour les types qui peuvent
encore refuser après coup — le monument océanique et sa sonde de profondeur, l'épave et son test
d'échouage, le portail en ruine et sa recherche de place — le chiffre est un majorant, et c'est
écrit dans le nom de l'énumérant plutôt que dans une note de bas de page.

---

## 8. Le statut `structure_starts` dans le pipeline

L'échelon existait, vide. Il est maintenant branché : `ChunkPipeline::set_structures(placer,
sampler)`, et `structure_starts(cx, cz)` répond en poussant le chunk jusqu'à cet échelon **et pas
plus loin**.

Ce qui compte architecturalement, et que le test vérifie : au moment où la décision est prise, le
chunk est **vide**. Aucun bloc n'existe, aucune biome grid n'est écrite, `stats().reached[Noise]`
vaut zéro. La décision lit le *générateur* — la fonction de bruit — pas le chunk. C'est obligatoire
et pas cosmétique : le jeu doit pouvoir répondre « y a-t-il un village en (x, z) » pour un chunk
qu'il n'a pas généré et ne générera jamais (`/locate`, un village dont le jigsaw s'étend à quatre
chunks). Un placement qui aurait besoin de blocs générés ne pourrait pas.

Sans placeur attaché, le pipeline traverse l'échelon et `structure_starts()` répond vide pour tout
chunk — la façon honnête de dire « ce monde a du terrain et pas de structures ».

### Le coût

Voir § 9 de `docs/provenance/pipeline-de-chunks.md` pour les bras de référence (1,712 s de terrain
seul, 3,820 s avec la décoration, par chunk en debug). Mesure faite ici, deux bras dans le même
processus, sur le même échantillon, `--bench` de `ov_structparity` :

| bras | 12 chunks, bande (x, 0) | par chunk |
|---|---:|---:|
| pipeline complet, sans structures | 31,923 s | **2,660 s** |
| pipeline complet, avec structures | 34,480 s | **2,873 s** |
| l'échelon de structures | | **+0,213 s (+8,01 %)** |

Le premier bras est joué **deux fois** et la première passe est jetée : le premier chunk paie
toutes les tables de bruit construites paresseusement dans la pile, et facturer ça au bras qui
tourne en premier serait une différence entre les bras qui n'est pas ce qu'on mesure. L'échantillon
est une **bande** et non un carré, parce que le pipeline met en cache : sur un carré, les
vingt-cinq voisins d'un chunk servent au suivant et le coût par chunk s'effondre pour une raison
qui n'est pas la bonne.

### Ce que ce chiffre a d'abord valu : +3 184 %

La première mesure a donné **74,859 s par chunk avec les structures contre 2,279 s sans**, soit
**+72,58 s par chunk, +3 184 %** — trente-deux fois le reste du pipeline réuni.

La cause : les dix-neuf ensembles interrogent tous les deux mêmes colonnes du chunk, et chacun
faisait son propre balayage d'une colonne de 384 blocs avec une évaluation complète de densité par
bloc. Le calcul des colonnes une fois par chunk (`StructurePlacer::AnchorColumns`) ramène l'écart
à +0,213 s.

Ce n'est pas un nettoyage : sans lui l'échelon était inutilisable, et rien dans le résultat ne
l'aurait montré — la parité était déjà exacte à 74 s par chunk.

---

## 9. Les trois harnais existants : inchangés, et aveugles à ce travail

Relevés après ce travail, sur la même machine :

| harnais | chiffre |
|---|---|
| `ov_carveparity` | 400 / 400 chunks bit pour bit, intersection/union **100,000 %** |
| `ov_surfparity` | colonnes dont les 8 couches concordent : 9 528 / 10 240 = **93,047 %** |
| `ov_parity --carvers` | biomes de cellules creusées à **100,000 %** sur les biomes listés |

Aucune régression. Mais **ces trois chiffres ne prouvent rien sur ce travail**, et c'est le piège
13 du dépôt qui l'exige : aucun des trois n'exécute le placement de structures. `ov_carveparity`
n'appelle que les carvers, `ov_surfparity` que les règles de surface sur le terrain du jeu,
`ov_parity --carvers` que le bruit et les biomes. Le nouvel échelon du pipeline est un
pass-through sans placeur attaché, et `ChunkGenerator::biome_name_at` est une méthode nouvelle que
rien d'ancien n'appelle.

Ils sont cités comme **absence de dégât collatéral**, pas comme preuve. La preuve de ce travail est
`ov_structparity` et rien d'autre.

---

## 10. Ce qui n'est pas fait, nommé

* ~~**Aucune géométrie.**~~ *Levé en partie par les §§ 12 à 17 : les structures à gabarit fixe
  (igloo, épaves, ruines océaniques, portails en ruine, trésor enfoui) sont posées bloc par bloc et
  mesurées. Le reste de cette liste est inchangé.*
* **Le système jigsaw** — pools de gabarits, ancrages, profondeur, résolution des connexions —
  n'est pas commencé. C'est aussi ce qui bloque le § 5.
* **Les gabarits `.nbt`** (1 010 fichiers dans `data/minecraft/structures/` du jar serveur) ne sont
  pas extraits. Ce sont des **données Mojang** : ils devront vivre dans
  `data/vanilla/1.20.1/generated/` (gitignoré, motif `*.nbt` déjà présent) et ne jamais être
  commités.
* **Les anneaux concentriques** des forteresses (§ 6).
* **Les boîtes englobantes** des structures, donc `References` et le rayon que le pipeline devra
  tenir. L'oracle existe pourtant : `References` dans le NBT du jeu dit exactement combien de
  chunks chaque structure touche.
* **La zone d'exclusion** des avant-postes est implémentée (10 chunks autour d'un candidat de
  village) mais **jamais exercée par la mesure** : aucun des trois mondes ne contient un avant-poste
  assez près d'un village pour la déclencher. Elle est écrite, elle compile, elle n'est pas prouvée.

---

## 11. Sources

* Le vrai serveur 1.20.1 (`tools/vanilla/server.jar`, SHA-1
  84194a2f286ef7c14ed7ce0090dba59902951553) : mondes de référence, et la commande console
  `/execute positioned <x> <y> <z> run locate structure <id>` qui donne la position exacte de la
  structure la plus proche — un oracle direct, utilisé pour trouver les neuf types du troisième
  monde.
* Les JSON du data generator : `worldgen/structure_set/` (19 fichiers),
  `worldgen/structure/` (33), `tags/worldgen/biome/` (les tags `has_structure/*` et leurs
  références).
* Le NBT des chunks générés : `structures.starts` et `structures.References`.

Aucune source de code tiers. Les tables de placement viennent des JSON du pack ; la sémantique des
réducteurs et l'ancre du filtre de biome ont été **fixées par la mesure** contre les mondes de
référence, ce qui est écrit ci-dessus étape par étape.

---

# Deuxième partie — les structures à gabarit, posées bloc par bloc

Le placement dit *où*. Cette partie dit *de quoi c'est fait*, pour les structures dont la géométrie
est un gabarit fixe ou une poignée de pièces : **igloo, épaves (en mer et échouées), ruines
océaniques (froides et chaudes), portails en ruine (les sept variantes), trésor enfoui.** Le jigsaw
(villages, avant-postes, bastions, cités antiques, ruines de sentier) est un autre chantier et n'est
pas touché ; le donjon est une *feature* et appartient à l'agent des features.

Résultat court :

> **Les pièces que le jeu a choisies, posées par notre code, redonnent les blocs du jeu à 100,000 %
> pour les épaves (1 236/1 236 et 383/383) et l'igloo (456/456), 99,45 % pour le second igloo, et
> les graines de butin des coffres au bit près : sur 28 coffres où le jeu a la même table que nous,
> 27 ont la même graine de 64 bits.** Les pièces que nous tirons de
> la graine seule sont celles du jeu dans **100 départs sur 100** (gabarit, rotation, miroir,
> origine, intégrité, réglages du portail). Un témoin — la même pièce tournée d'un quart de tour —
> tombe à 2,8 % sur les épaves. Dans un monde **entièrement généré par nous**, l'igloo du monde de
> référence sort au bloc près (546/546) ; ce qui manque ailleurs dans notre monde est la
> **hauteur** de pose (ruines océaniques, portails), pas la géométrie.

## 12. Le lecteur de gabarits, et ce que la pose fait d'un bloc

### 12.1 Lus à l'exécution, jamais commités

Les 1 010 gabarits de 1.20.1 vivent dans `data/minecraft/structures/` **du jar serveur intérieur** :
`tools/vanilla/server.jar` est le *bundler*, qui porte le vrai serveur sous
`META-INF/versions/1.20.1/server-1.20.1.jar`. `TemplateLibrary::open` ouvre l'un dans l'autre en
mémoire (le lecteur ZIP d'`ov_io` accepte un tampon), lit les familles demandées (84 gabarits :
`igloo/`, `shipwreck/`, `underwater_ruin/`, `ruined_portal/`), les décompresse (gzip) et les parse
(NBT). Rien n'est écrit sur le disque, rien n'entre dans le dépôt ; `check_assets.py` passe. Le
chargement est **eager** et la bibliothèque immuable ensuite : un cache paresseux derrière une
interface `const` est une course de données au premier second thread (piège 17).

Le format, tel que le fichier le porte : `size`, `palette` **ou** `palettes` (une liste de palettes
de même longueur — les huit essences de bois des épaves), `blocks` (`state` indexe la palette,
`pos`, `nbt` facultatif), `entities`. Un bloc de palette est résolu **propriété par propriété à
partir de l'état par défaut** (piège 8) ; un nom inconnu refuse le gabarit en le nommant.

### 12.2 La transformation — et le pivot, qui est celui de la pièce

Une position locale est **mirrorée d'abord** (`LEFT_RIGHT` nie z, `FRONT_BACK` nie x), **tournée
ensuite autour du pivot**, puis décalée par l'origine du gabarit (`TPX/TPY/TPZ` du NBT du jeu). Le
pivot n'est pas une propriété du gabarit mais de la pièce, et il a été **lu dans les boîtes que le
jeu a stockées** : pour chaque pièce, la boîte `BB` et l'origine `TP` fixent `px − pz` et `px + pz`.

| pièce | pivot | établi par |
|---|---|---|
| épave | (4, 0, 15) | la boîte de l'épave CCW90 du chunk (−6560, 442) |
| portail en ruine | (taille x / 2, 0, taille z / 2) | les deux `portal_4`, dont un en miroir |
| igloo `top` / `middle` / `bottom` | (3, 5, 5) / (1, 3, 1) / (3, 6, 7) | les deux igloos |
| ruine océanique | (0, 0, 0) | la ruine chaude CCW90 |

Les trois pivots de l'igloo **s'empilent sur une seule colonne** (origine + pivot = coin du chunk +
(3, ·, 5) pour les trois pièces). C'est pour ça que leurs décalages ne tournent pas avec l'igloo : la
rotation autour d'un pivot laisse le pivot en place. Seule la rotation CLOCKWISE_180 est observée
pour l'igloo (deux sur deux) ; les trois autres suivent de cette propriété, elles ne sont pas
mesurées.

Les états tournent avec le bloc : `facing`, `axis`, les seize pas de `rotation` (miroir
`FRONT_BACK` : 16 − r ; `LEFT_RIGHT` : 8 − r), les quatre côtés (clôtures, vitres, vignes,
redstone), les formes de rail, la charnière d'une porte et la moitié d'un coffre au miroir — et
**pas** le `type` d'une dalle, qui s'appelle pareil et ne veut pas dire la même chose.

### 12.3 Trois tirages positionnels, mesurés

Chaque tirage de la couche gabarit sort d'un `java.util.Random` semé par `Mth.getSeed` d'une
**position monde** — jamais du chunk, sans quoi un même bloc changerait selon le chunk qui le pose.
Le produit `x * 3129871` est une multiplication **entière** qui déborde à 32 bits avant d'être
élargie ; celui de z est long. Le test unitaire fige cette asymétrie.

| tirage | clé | mesure |
|---|---|---|
| palette (épaves) | l'**origine** du gabarit, `nextInt(nombre de palettes)` | épaves : 2 blocs sur 385 → 383/385 dès cette règle ; avant, palette 0 : 245/383 |
| `block_rot` (intégrité) | la position du bloc, `nextFloat() <= intégrité` | ruines chaudes : **177 gardés par nous, 177 gardés par le jeu**, 0 gardé à tort |
| `rule` / `block_age` | la position du bloc, un aléa par bloc et par processeur | portails : magma au-dessus du fond du gabarit, 223/226 (voir § 15) |

Conséquence directe, et vérifiée : les trois couches d'une ruine froide (`brick` 0,8, `cracked`
0,7, `mossy` 0,5) posées à la même origine tirent **le même nombre** en chaque position. Ce que la
couche moussue garde, les deux autres le gardent aussi ; la ruine se lit comme « moussu si le
tirage ≤ 0,5, fissuré si ≤ 0,7, brique si ≤ 0,8 (0,9 pour une grande), rien sinon ».

### 12.4 Les processeurs

`ProcessorList::parse_json` lit le corps `{"processors": [...]}` d'un `worldgen/processor_list`.
Implémentés : `block_rot`, `block_ignore`, `rule` (tests `always_true`, `block_match`,
`blockstate_match`, `tag_match`, `random_block_match`, `random_blockstate_match` ; modificateurs
`passthrough` et `append_loot`), `protected_blocks`, `gravity`, `jigsaw_replacement`, `nop`,
`block_age`. **Refusés et nommés** : `capped` (l'archéologie, § 16), `blackstone_replace`,
`lava_submerged_block`, les prédicats de position autres que `always_true`, et tout type inconnu —
un processeur ignoré en silence poserait une structure crédible et fausse.

Un test `random_block_match` court-circuite : **pas de tirage** quand le bloc ne correspond pas
(c'est le `&&` de Java, et c'est ce que la mesure du magma demande).

Les listes que le jeu construit en code (l'intégrité des ruines, le vieillissement des portails)
passent par les mêmes classes ; les règles du portail sont écrites **comme leur jumeau JSON** et
parsées par le même code, pour qu'aucune pièce n'épelle une règle à la main.

### 12.5 Ce que la pose fait après coup : la mise à jour de forme

Les gabarits stockent les formes telles qu'au jour de la sauvegarde : une épave est pleine
d'escaliers `outer_right` et de clôtures non reliées que le jeu redresse et relie à la pose.
Mesuré sur les épaves : **245/383 sans la mise à jour, 362/383 avec les escaliers seuls,
379/383 avec les clôtures, 383/383 avec la moitié basse des portes** (qui recopie la moitié
haute). Deux pièges dans cette mise à jour :

* **L'ordre compte.** Qu'une clôture se joigne à un escalier dépend de la *forme* de l'escalier (un
  côté est plein ou non), jamais l'inverse. Escaliers et portes d'abord, clôtures ensuite — sinon le
  résultat dépend du dernier chunk posé. Trouvé par le test « posé chunk par chunk = posé d'un
  coup », qui donnait 382/385 dans un ordre et 385/385 dans l'autre.
* **Un voisin d'un autre chunk n'existe pas encore** quand le premier chunk pose sa moitié. Le jeu
  y revient au post-traitement ; nous, chaque fois qu'un chunk du voisinage 3×3 est posé (§ 14).

### 12.6 Les coffres et leur graine de butin — un oracle au bit près

Un coffre de structure non ouvert porte `LootTable` et `LootTableSeed` : 64 bits, qui ne tombent
pas juste par hasard. Trois faits, tous fixés par la mesure :

1. **La graine vient de l'aléa du chunk**, `setFeatureSeed(décoration, index, étape)`, pas de la
   position.
2. **Tout conteneur porté par le gabarit tire un `nextLong` en étant posé**, qu'il ait une table ou
   non. Les coffres vides de l'épave (`Items: []`) décalent donc la graine du coffre que le marqueur
   de données pose ensuite : sans ce tirage, 0 épave sur 7 ; avec, 6 sur 7.
3. **L'index est le rang de la structure, par ordre alphabétique, parmi toutes les structures de son
   étape** (toutes dimensions confondues). Cherché à l'aveugle (`--find-index`, index 0 à 47), puis
   reconnu :

| structure | étape | index trouvé | rang alphabétique |
|---|---|---:|---:|
| buried_treasure | underground_structures | 0 | 0 |
| igloo | surface_structures | 3 | 3 (après bastion_remnant, desert_pyramid, end_city) |
| ocean_ruin_cold / warm | surface_structures | 7 / 8 | 7 / 8 |
| ruined_portal / _mountain | surface_structures | 10 / 13 | 10 / 13 |
| shipwreck | surface_structures | 17 | 17 |

Le registre des structures est donc dans l'ordre des noms, et c'est ce que
`structure_step_index()` calcule. Une épave échouée tire en plus un `nextInt(3)` **avant** de
poser, dans chaque chunk qu'elle traverse (sa hauteur) : sans lui, 0/2 ; avec, 2/2.

## 13. Les pièces, depuis la graine seule — 100 départs sur 100

`StructureBuilder::generate` tire les pièces d'un départ d'un `WorldgenRandom` à cœur legacy semé
par `setLargeFeatureSeed(graine, chunkX, chunkZ)`. **L'oracle n'a pas besoin de blocs** : un chunk
que le jeu a seulement mené à `structure_starts` a déjà écrit ses pièces dans `structures.starts`.
Les trois mondes de référence en portent **100** dans le périmètre (dont 19 seulement dans des
chunks `full`) — c'est l'échantillon. Aucun n'est dans `run/reference-987654321`.

| structure | ordre des tirages retenu | départs | exacts |
|---|---|---:|---:|
| igloo | rotation `nextInt(4)` ; sous-sol si `nextDouble() < 0,5` ; profondeur `nextInt(8) + 4`, **segments = profondeur − 1** | 2 | 2 |
| shipwreck | rotation ; gabarit `nextInt(20)` | 27 | 27 |
| shipwreck_beached | rotation ; gabarit `nextInt(11)` | 2 | 2 |
| ocean_ruin_cold / warm | rotation ; grande si `nextFloat() <= 0,3` ; gabarit `nextInt(4 ou 8)` ; amas si grande et `nextFloat() <= 0,9` | 23 / 11 | 23 / 11 |
| ruined_portal (×7) | réglage pondéré (tiré **seulement s'il y en a plusieurs**) ; poche d'air (tirée seulement si 0 < p < 1) ; géant si `nextFloat() < 0,05` ; gabarit `nextInt(3 ou 10)` ; rotation ; miroir `FRONT_BACK` si `nextFloat() >= 0,5` | 25 | 25 |
| buried_treasure | aucun tirage : coin du chunk + (9, ·, 9) | 10 | 10 |

L'ordre des listes de gabarits n'est dans aucun fichier : il a été **lu à l'envers**, index tiré
contre gabarit stocké. Pour les 20 épaves en mer, 14 index observés suffisent à reconnaître un motif
régulier (entier, avant, arrière × à l'endroit, sur le flanc, à l'envers, puis les mêmes dégradés) ;
**les 6 index restants (6, 9, 15, 16, 17, 18) sont déduits du motif, pas observés.** La liste des
11 épaves échouées est déduite de la même façon ; un seul index y est observé (le dernier, 10). Les
listes de ruines sont l'ordre des numéros (grandes froides 1, 2, 3, 8 ; grandes chaudes 4, 5, 6, 7).
Pour les portails, les 25 tirages rendent les six champs (réglage, poche d'air, géant, gabarit,
rotation, miroir) exacts — mais aucun des 25 n'est géant : la branche géante n'est pas exercée.

Deux choses restent absentes et **sont nommées dans le départ** (`StructureStart::incomplete`) plutôt
que cachées : l'**amas de petites ruines** autour d'une grande ruine (13 départs sur 34 ; la grande
ruine est bien posée, pas les petites), et, pour les portails, la **recherche de hauteur**, le
**test de biome froid** et l'**étalement de netherrack** (§ 15).

## 14. La pose dans le pipeline : `StructureStage`

`ChunkPipeline::set_structure_stage` attache une `StructureStage` ; l'étage `features` l'appelle
**avant** la décoration du chunk. Le bloc ajouté à `pipeline.cpp` est contigu et marqué
`// ── structures ──` ; `chunk_generator.cpp` n'est pas touché, parce que c'est `pipeline.cpp` qui
possède le voisinage 3×3 — le générateur ne voit qu'un chunk.

* Le chunk décoré collecte les départs dont la boîte le traverse, sur un rayon de 3 chunks
  (`kReach` : une épave fait au plus 28 blocs et part du coin de son chunk). Les départs sont
  construits une fois par chunk de départ et gardés.
* Chaque pièce écrit **la part qui tombe dans la colonne du chunk décoré**, et seulement elle : c'est
  ce que fait le jeu, et c'est ce qui rend le résultat indépendant de l'ordre des chunks. Les lectures
  (terrain, voisins, eau) couvrent le 3×3 ; au-delà, le `StructureWorldSampler` répond.
* Un aléa par structure et par chunk, partagé par ses départs et ses pièces dans l'ordre (§ 12.6) ;
  les structures dans l'ordre (étape, rang).
* Après chaque chunk, la mise à jour de forme est rejouée sur les positions enregistrées de tout le
  3×3 : c'est notre post-traitement.
* Tout ce qui n'est pas construit est **compté par raison** dans `StructureStageStats` : un départ
  refusé (`desert_pyramid is not built here`, `jigsaw is not built here`…), un départ incomplet.

Test : une épave dont la boîte traverse quatre chunks, posée chunk par chunk dans deux ordres
opposés, redonne **exactement** les 385 blocs de la même épave posée d'un coup.

**Écart d'ordre nommé.** Le jeu pose les structures des étapes 3 et 4 *après* les features des
étapes 0 à 2 (lacs, géodes, icebergs) ; ici elles passent avant toute la décoration, parce que
l'interface de `Decorator` ne découpe pas par étape et que ce fichier appartient à un autre
chantier. Un iceberg qui chevauche une ruine océanique est donc posé dans l'autre ordre.

**Les hauteurs** se règlent à la première pose d'une pièce, sur le terrain du voisinage, et restent :
igloo — `WORLD_SURFACE_WG` à la colonne des pivots, moins 91 ; épave — la moyenne
d'`OCEAN_FLOOR_WG` sur l'empreinte **non tournée** partant de l'origine (échouée : le minimum de
`WORLD_SURFACE_WG`, moins la moitié de la hauteur, moins le tirage de § 12.6) ; ruine — `OCEAN_FLOOR_WG`
à l'origine. Ce sont des **hypothèses**, dont le § 17 donne la mesure sur notre propre terrain.

## 15. Les portails en ruine

Le gabarit, le miroir, la rotation, le vieillissement et les règles sont en place. Mesuré (niveau A,
pièces du jeu) : **676/698, 200/214 et 193/206** blocs identiques pour les trois portails complets
des mondes de référence, avec le coffre et sa graine exacts pour deux d'entre eux.

* **L'air du gabarit n'est pas posé sans poche d'air** : la mer reste dans un portail océanique
  (536 blocs d'eau à tort avant cette règle), les feuilles dans un portail de forêt.
* **Le magma.** Le 7 % du netherrack se tire sur le **premier** tirage de l'aléa positionnel
  (autres hypothèses mesurées : 2e, 3e, 4e tirage, clé locale au lieu de monde — toutes pires). Au
  dessus de la couche du fond, 223 positions sur 226 concordent ; **dans la couche du fond, 35
  désaccords sur 43** : c'est l'**étalement de netherrack** que la pièce pose sous et autour
  d'elle, qui réécrit cette couche avec ses propres tirages, et qui n'est pas implémenté.
* **Le vieillissement** (`block_age`) : pierre taillée → fissurée / escalier au hasard / moussue,
  dalles, escaliers et murets → moussus, obsidienne → pleureuse à 15 %. L'ordre des tirages retenu
  (0,5 ; les deux escaliers au hasard tirés *avant* le choix ; mousse ; élément) donne les
  concordances ci-dessus ; la branche « escalier du gabarit » n'est pas exercée par les mondes.
* **Pas faits, nommés** : l'étalement de netherrack, les vignes et la végétation des portails de
  jungle, `lava_submerged_block`, le remplacement par la pierre noire du Nether. Le portail
  océanique a sa table de butin juste et **sa graine fausse** (0/1) : un tirage de plus ou de moins
  avant le coffre, non trouvé.

### 15.1 La hauteur et le froid — 34 départs sur 34 (2026-09-11)

La hauteur d'un portail est décidée **à la génération du départ**, pas à la pose : le jeu stocke
déjà son y réel dans les départs au statut `structure_starts`, avant qu'aucun bloc n'existe. Elle se
calcule donc sur le bruit seul (`src/ov_worldgen/src/ruined_portal.cpp`).

**Sources.** Les intervalles sont ceux de la page *Ruined Portal* de minecraft.wiki (souterrain
« de 15 à n − n2 », montagne « de 70 à n − n2 », à moitié enterré « n − n2 plus 2 à 8 », Nether
« 32 à 100 » avec poche d'air, sinon « 27 à 29 » ou « 29 à 100 » à 50 %). Le reste n'est pas écrit
dans la page ; il a été **ajusté sur les 25 départs de l'Overworld** des mondes de référence
(`ov_structblocks --level=h` sort pour chaque départ la graine, la boîte, nos hauteurs et les
colonnes de base aux coins ; l'ajustement a été fait à part, sur ces données) :

* **n** est le premier bloc **occupé** de la colonne du **milieu** de la boîte (un sous le premier
  libre) : le fond marin pour un portail océanique, la surface (mer comprise) sinon. Premier libre :
  0 à 14/25 ; colonne du coin : 10 à 19/25.
* Le « 15 » du souterrain est **15 au-dessus du fond du monde** (−49), pas y = 15 : trois portails
  du jeu sont à −48, −46 et −2. Avec y = 15 : 19/25.
* Un intervalle vide (`min ≥ max`) ne tire rien et donne son maximum.
* Puis le portail **descend** tant que moins de trois des quatre coins de sa boîte reposent sur
  quelque chose dans la colonne de base (bruit et aquifère) : tout sauf l'air, ou, au fond de la
  mer, un bloc solide ; jamais sous le fond + 15. Sans cette descente : 16 à 22/25.
* **Froid** si le réglage le permet (`can_be_cold`) et que la température du biome **à l'origine du
  gabarit**, taches gelées et correction d'altitude comprises (`gameplay::ClimateNoise`), est sous
  0,15. Au milieu de la boîte, le portail souterrain du chunk (3817, 2053) tombe dans un biome de
  grotte et n'est plus froid : 24/25. L'océan gelé (température 0) donne bien un portail non froid,
  par ses taches gelées.

Seule la dimension compte pour le fond : −64 dans l'Overworld, 0 dans le Nether, où la règle
vérifie 9/9 sans rien ajuster.

**Mesure** (niveau B, pièces tirées de la graine avec un vrai échantillonneur, comparées aux
pièces stockées : gabarit, rotation, miroir, origine y compris sa hauteur, propriétés, froid) :

| monde | portails exacts |
|---|---|
| reference-1234567890 | ruined_portal 9/9, _mountain 6/6, _ocean 5/5 |
| struct-locate-1234567890 | ruined_portal 3/3, _jungle 1/1, _ocean 1/1 |
| reference-nether-987654321 (`DIM-1`) | ruined_portal_nether 9/9 |
| **total** | **34/34** |

**Témoins.** La même mesure avec la graine décalée d'un (1234567891, 987654322) : **0/20** et
**0/9**. Les variantes de règle écartées ci-dessus (premier libre, coin, y = 15, sans descente,
froid au milieu) sont les témoins propres à la hauteur : aucune n'atteint 25/25.

Le serveur ne refuse plus les portails (§ 20.2) : ses départs portent leur vraie hauteur, et le
test `test_world_structures` vérifie celui du chunk (4571, 3940), `portal_4` à y = 64.

## 16. Niveau A — les pièces du jeu, notre code : le tableau

`tools/ov_structblocks --level=a` : chaque départ d'un chunk `full`, pièces lues dans le NBT du jeu,
posées **chunk par chunk** par `StructureBuilder::place`, comparées **état complet** (bloc et
propriétés) au monde de référence. Colonne « coffres » : coffres posés / table juste / graine
juste.

`run/reference-1234567890` :

| structure | départs | pièces | comparés | identiques | % | coffres |
|---|---:|---:|---:|---:|---:|---|
| shipwreck | 3 | 3 | 1 236 | 1 236 | **100,000** | 7 / 6 / 6 |
| shipwreck_beached | 1 | 1 | 383 | 383 | **100,000** | 2 / 2 / 2 |
| igloo | 1 | 12 | 546 | 543 | 99,451 | 1 / 1 / 1 |
| ruined_portal | 2 | 2 | 698 | 676 | 96,848 | 2 / 2 / 2 |
| ruined_portal_mountain | 1 | 1 | 214 | 200 | 93,458 | 1 / 1 / 1 |
| ruined_portal_ocean | 1 | 1 | 206 | 193 | 93,689 | 1 / 1 / 0 |
| ocean_ruin_cold | 4 | 39 | 1 500 | 1 382 | 92,133 | 14 / 12 / 12 |
| ocean_ruin_warm | 2 | 2 | 122 | 112 | 91,803 | 2 / 2 / 2 |
| buried_treasure | 1 | 1 | 1 | 1 | 100,000 | 1 / 1 / 1 |

`run/struct-locate-1234567890` : igloo **456/456**, épave échouée **383/383**, ruine chaude 56/61.
`run/reference-987654321` ne contient aucun départ de ces types dans un chunk `full`.

**Le témoin** (`--witness`, chaque pièce un quart de tour à côté, même origine) : épaves 2,845 %
et 2,872 %, ruines 10,8 % et 16,5 %, portails 1 à 52 %, igloo 47 % — et **aucune table ni graine de
coffre juste sur les 30 coffres de gabarit** (le 31e, celui du trésor enfoui, n'a pas de gabarit à
tourner et passe). Le trésor enfoui passe le témoin (un bloc, pas de gabarit) : son 1/1 ne prouve que la
position et l'orientation du coffre.

Les écarts restants, tous nommés :

| écart | blocs | cause |
|---|---:|---|
| gravier / sable → gravier / sable **suspect** (ruines) | 90 | le processeur `capped` d'archéologie (1.20), refusé et nommé : il choisit N blocs parmi les candidats avec un aléa qui n'est pas reproduit |
| gravier → eau (ruines froides) | 36 | le gravier **tombe** : posé sur de l'eau, il est planifié et chute au premier tick du chunk |
| netherrack ↔ magma, couche du fond (portails) | 35 | l'étalement de netherrack, non implémenté (§ 15) |
| pierre → granite (igloo) | 3 | un amas de granite de l'étape des minerais, posé après |
| herbes marines (ruines) | 2 | la végétation, posée après |
| pierre → gravier, pierre taillée → netherrack (portail de montagne) | 6 | l'étalement et la gravité, non implémentés |

**Précaution de mesure.** Le niveau A lit le monde *fini* comme « le monde avant la structure » là
où la pose le consulte (eau à garder, bloc protégé). Deux cas en dépendent, et sont traités
explicitement : un coffre du monde fini est lu comme de l'eau s'il est gorgé d'eau, comme de l'air
sinon — sans quoi `protected_blocks` refuse de poser le coffre du portail parce que le coffre du jeu
est déjà là.

## 17. Niveau C — notre monde

`tools/ov_structblocks --level=c` : le pipeline complet (bruit, biomes, surface, carvers, **étage
des structures**, features) génère les chunks que couvre chaque départ `full` du jeu ; on lit, dans
**notre** monde, les positions dont la structure du jeu est faite (celles qu'écrivent ses propres
pièces) et on compare aux blocs du jeu. C'est le seul niveau dont le chiffre contient notre
terrain : une pièce se pose sur le sol qu'elle trouve.

`run/reference-1234567890` (graine 1234567890) :

| structure | départs | trouvés | hauteurs exactes | comparés | identiques | % |
|---|---:|---:|---:|---:|---:|---:|
| igloo | 1 | 1 | **12/12** | 546 | **546** | **100,000** |
| shipwreck | 3 | 3 | 2/3 | 1 236 | 800 | 64,725 |
| shipwreck_beached | 1 | 1 | **1/1** | 383 | 360 | 93,995 |
| ocean_ruin_cold | 4 | 4 | 3/12 | 1 500 | 224 | 14,933 |
| ocean_ruin_warm | 2 | 2 | 0/2 | 122 | 25 | 20,492 |
| ruined_portal (×3 variantes) | 4 | 4 | 0/4 | 1 118 | 0 | 0,000 |
| buried_treasure | 1 | 1 | 0/1 | 1 | 0 | 0,000 |

`run/struct-locate-1234567890` : igloo trouvé, **ses six pièces un bloc trop bas** (y 62 contre 63) :
184/456 = 40,351 % ; ruine chaude un bloc trop haute (50 contre 49) : 25/61 ; l'épave échouée (le
même départ, chunk (9, 5), que dans l'autre monde : même graine) 360/383.

L'épave échouée est la mesure du tirage de § 12.6 : **sans le `nextInt(3)` retranché à sa
hauteur, elle se pose un bloc trop haut, 65/383 ; avec, à la bonne hauteur, 360/383.** Les 23 blocs
restants sont tous des escaliers, dalles et trappes dont seul `waterlogged` diffère : notre eau ne
monte pas exactement là où monte celle du jeu sur cette plage.

**Ce que ce tableau établit.** Là où la hauteur tombe juste, la chaîne entière — décision du
placement sur notre bruit, pièces tirées de la graine, pose chunk par chunk dans le pipeline, formes,
coffres — rend **la structure du jeu au bloc près dans un monde que nous avons généré** : l'igloo de
`reference-1234567890`, 546 blocs sur 546, ses douze pièces à la bonne hauteur ; l'épave échouée,
360/383, l'écart étant de l'eau et non de la structure. Tout le reste de
l'écart du niveau C est une question de **hauteur**, pas de géométrie : le niveau A a déjà montré
la géométrie juste.

**Ce qu'il n'établit pas, nommé :**

* **Les hauteurs des ruines océaniques sont fausses.** `OCEAN_FLOOR_WG` à l'origine donne 3 pièces
  justes sur 12 pour les froides, 0 sur 3 pour les chaudes, et jusqu'à 20 blocs d'écart (la grande
  ruine du chunk (3249, −4250) : 51 chez nous, 31 dans le jeu). La règle du jeu n'est pas trouvée ;
  ce n'est probablement pas notre terrain seul (20 blocs), et c'est l'hypothèse de § 14 qui tombe.
* **Un bloc d'écart sur l'igloo de `struct-locate` et sur trois des cinq autres pièces à hauteur
  réglable** : notre terrain un bloc plus bas ou plus haut que celui du jeu à la colonne lue, ou une
  règle décalée d'un bloc — les deux ne sont pas départagés, faute du terrain du jeu *avant* la
  structure (le monde de référence n'a que le terrain d'après). L'igloo de `reference-1234567890`,
  juste au bloc, dit que la règle de l'igloo n'est pas décalée.
* **Les portails (0 %) et le trésor (0 %)** ne règlent pas leur hauteur (§ 15, § 18) : ils sont
  posés à la hauteur de départ, et le tableau le montre au lieu de l'arrondir.

Coût (build debug, sous la charge de huit autres agents) : 16 départs, 18 min pour le monde de
référence ; la mesure du coût de l'étage seul n'est pas faite.

## 18. Ce qui n'est pas fait, nommé

* **Temple du désert, temple de la jungle, cabane de sorcière** : le jeu les construit **en code**,
  pas depuis un gabarit. Les reconstruire bloc par bloc de mémoire serait traduire du code — interdit
  ici ; les spécifier depuis la documentation est un chantier à part. Refusés par nom
  (`desert_pyramid is not built here`).
* **Fossiles et puits du désert** : ce sont des *features* (`fossil_upper`, `fossil_lower`,
  `desert_well`), pas des structures ; ils appartiennent à l'agent des features. Les gabarits
  `fossil/` sont lisibles par la même bibliothèque le jour où la feature les demande.
* **Fossiles du Nether** : structure d'une dimension que ce générateur ne produit pas.
* **L'amas de petites ruines océaniques** (13 départs sur 34 le demandent) : les positions observées
  suivent une grille de 3 × 3 cellules de 16 blocs autour de la grande ruine avec un décalage de 1 à
  8, mais la règle n'est pas trouvée pour les grandes ruines tournées ; rien n'est posé plutôt
  qu'une approximation.
* **Portails** : hauteur, froid, étalement de netherrack, vignes, végétation (§ 15). Un portail est
  posé à y = 0 tant que sa hauteur n'est pas réglée : le départ est marqué incomplet.
* **Le trésor enfoui** est posé à la hauteur réglée par son départ ; la recherche vers le bas du jeu
  (sable, grès, pierre) et le grès qu'il pose autour ne sont pas faits.
* **Les entités** des gabarits (villageois et zombie de l'igloo, noyés des ruines) ne sont pas
  créées : il n'y a pas d'entités dans le pipeline de génération.
* **L'archéologie** (`capped`) et la **chute du gravier** (§ 16).
* ~~**Le serveur** n'attache pas encore la `StructureStage`~~ — fait le 2026-09-11, § 20.

## 19. Rejouer

```bash
cmake --build --preset macos-debug --target ov_structblocks test_ov_worldgen
./build/macos-debug/bin/ov_structblocks --level=a --world=run/reference-1234567890/world
./build/macos-debug/bin/ov_structblocks --level=a --world=run/reference-1234567890/world --witness
./build/macos-debug/bin/ov_structblocks --level=a --world=run/reference-1234567890/world --find-index
./build/macos-debug/bin/ov_structblocks --level=b --world=run/reference-1234567890/world --seed=1234567890
./build/macos-debug/bin/ov_structblocks --level=c --world=run/reference-1234567890/world
./build/macos-debug/bin/test_ov_worldgen "[template],[pieces],[stage]"
# § 20 — le chemin du serveur
./build/macos-debug/bin/test_ov_worldgen "[nbt]"
./build/macos-debug/bin/ov_gendet --seed=1234567890 --export=.scratch/ow/region --chunks=8,4,10,6
OV_STRUCTURES=0 ./build/macos-debug/bin/ov_gendet --seed=1234567890 --export=.scratch/witness/region --chunks=8,4,10,6
python3 scripts/measure_structures.py .scratch/ow/region run/reference-1234567890/world/region \
    --witness=.scratch/witness/region
```

Sources de cette partie : les gabarits et les JSON du jar serveur 1.20.1 (lus, jamais copiés) ; le
NBT des chunks des trois mondes de référence (`structures.starts` : pièces, origines, rotations,
miroirs, intégrités ; `block_entities` : tables et graines de butin) ; la page *Ruined Portal* de
minecraft.wiki pour les probabilités du vieillissement et des remplacements (15 % d'obsidienne
pleureuse, 7 % de magma, 30 % d'or) et *Buried Treasure* pour la position (9, 9) et le coffre tourné
vers l'est. Aucun code tiers, aucun code du jeu.

## 20. Dans le serveur — `ov_dedicated` place ses structures (2026-09-11)

Jusqu'ici tout ce document se mesurait **hors du serveur** : un monde généré par `ov_dedicated`
(`scripts/lab.sh --seed=…`, ou le solo) ne contenait aucune structure, parce que
`generated_world.cpp` n'attachait ni le placeur ni l'étage. C'est fait, dans les trois dimensions.

### 20.1 Le branchement

`src/ov_server/src/world_structures.{hpp,cpp}` ; `generated_world.cpp` n'en reçoit que des blocs
courts marqués `// ── structures ──`.

* **Partagé par dimension, chargé une fois** : les ensembles de structures, les tags de blocs, le
  placeur (restreint aux biomes que la dimension produit — c'est ce qui garde les fossiles du Nether
  hors de l'overworld, § 4) et le constructeur (les 98 gabarits lus dans le jar). Rien n'y est écrit
  après le chargement ; toutes leurs requêtes sont `const`. Le jar est `OV_SERVER_JAR`, sinon
  `tools/vanilla/server.jar` à côté de la racine des données ; absent, le monde se génère **sans
  structures et le dit** (erreur au journal) plutôt que de refuser de démarrer.
* **Un par pile de génération** : l'échantillonneur (il interroge le générateur de sa pile, dont les
  nœuds de densité ont des caches `mutable` — piège 17) et l'étage (ses départs et ses mises à jour de
  forme en attente). Aucun verrou.
* **L'ordre de statut** : les départs sont décidés (`structure_starts`) avant tout bloc, sur le bruit
  seul ; l'étage pose au statut `features` de chaque chunk, avant sa décoration, la part des pièces
  qui tombe dans sa colonne — les pièces qui débordent sont posées par chacun des chunks qu'elles
  traversent, lectures sur le 3 × 3 (§ 14). `structure_references` est calculé à la sortie du chunk
  (ci-dessous).
* **Un seul écrivain** : tout cela se passe dans `generate_square`, sur des chunks que personne ne voit
  encore ; le thread de tick les publie ensuite (async_chunk_source.hpp, invariant 2).
* **Déterminisme** : l'étage est vidé avec le pipeline au début et à la fin de chaque carré
  (`StructureStage::clear`), donc les structures d'un carré, comme son terrain, ne dépendent que de la
  graine et du carré (invariant 3). `ov_gendet` (série contre parallèle) le vérifie — § 20.4.

### 20.2 Ce qui est refusé, par son nom

Les refus portent désormais **le nom de la structure** (`minecraft:village_plains: jigsaw is not
built here`), et le serveur les écrit une fois par pile au journal (`structures (overworld): not
placed — …`). Refusés parce que non construits : villages, avant-postes, cités antiques, ruines des
sentiers, bastions (jigsaw), forteresse, puits de mine, fort, monument, manoir, temples du désert et
de la jungle, cabane de sorcière, cité de l'End.

**Un genre que le constructeur sait faire est refusé aussi dans le serveur**
(`StructureStage::refuse`), parce que ce qu'il en fait aujourd'hui n'est pas la structure du jeu mais
une invention à une hauteur de remplacement : le **trésor enfoui** (sans sa recherche vers le bas, son
coffre flotterait à y = 90). Les outils de parité, eux, continuent de le poser pour le mesurer. Les
**portails en ruine** étaient refusés pour la même raison jusqu'à leur hauteur (§ 15.1, 34/34) ; ils
sont placés depuis.

Placés : igloo, épave (en mer et échouée), ruines océaniques (froides et chaudes — la grande ruine
sans l'amas de petites, § 13), fossiles du Nether, portails en ruine (les sept, sans l'étalement de
netherrack).

### 20.3 `structures.starts` et `References` dans le chunk

Un chunk porte désormais son composé `structures` (`world::Chunk::structures()`), écrit par
`to_nbt` — toujours les deux listes, vides au besoin, comme vanilla — et **relu tel quel** par
`from_nbt` : un chunk d'une sauvegarde vanilla que ce serveur réécrit garde les départs et références
du jeu (avant, il les perdait).

Le format est celui du jeu, lu sur les chunks des mondes de référence (`ov_worldgen/structure_nbt`) :
départ `{id, ChunkX, ChunkZ, references: 0, Children}` ; pièce `{id, BB (tableau de 6 int), GD 0,
O 2}` (`O −1` pour le trésor), plus `TPX/TPY/TPZ`, `Template` et `Rot` pour un gabarit —
`Rotation` et `Mirror` pour le portail. Les booléens sont des **octets** (`isBeached`, `IsLarge`,
les propriétés du portail), l'intégrité et la mousse des **flottants**. Une seule bizarrerie :
**l'igloo garde dans `TPY` sa hauteur de génération** (fond 54, boîte à 35), le jeu ne déplaçant que
sa boîte ; les autres stockent la hauteur réglée. `References` : un tableau de `long` par structure,
`ChunkPos.asLong` (x dans les 32 bits bas) — `(-7, -10)` → −38 654 705 671, lu dans le monde de
référence. Une structure est référencée par sa boîte, **élargie de 12 blocs** si elle adapte le
terrain (`terrain_adaptation` ≠ `none` ; de ce qui est construit, le fossile du Nether — mesuré en
§ 20.4) ; la recherche porte donc sur `kReach` + 1 chunks, dont les départs sont déjà en cache. Les
références d'une structure refusée n'existent pas, puisque son départ n'existe pas.

### 20.4 La mesure

`ov_gendet --export=<dir> --dimension=… --chunks=x0,z0,x1,z1` génère par **le chemin du serveur**
(`GeneratedWorld::generate_square`, étage attaché) et écrit les régions par `world::to_nbt` — ce que
`ov_dedicated` écrirait, sans joueur. `scripts/measure_structures.py` compare aux régions du jeu :
départs (présence, puis chaque champ **type compris**), `References` par chunk, et blocs dans les
boîtes des pièces du jeu, avec en option un **témoin** exporté sous `OV_STRUCTURES=0`.

Premier carré, graine 1234567890, chunks (8..11, 4..7) — l'épave échouée du chunk (9, 5) :

| | |
|---|---|
| départs du jeu dans les chunks exportés | 1 — trouvé, **identique champ pour champ, types compris** |
| `References` | 1 / 1 |
| blocs dans la boîte de l'épave | 1 252 / 1 296 — **96,605 %** |

**Les huit départs du jeu exportés** (graine 1234567890 : l'épave échouée ci-dessus, trois épaves,
l'igloo, deux ruines froides et une chaude ; 30 chunks, 29 finis chez le jeu, onze carrés) :

| | |
|---|---|
| départs trouvés (même nom, même chunk) | **8 / 8**, aucun de trop |
| identiques champ pour champ, types compris | **5 / 8** — les quatre épaves et une ruine froide |
| `References` (chunk, structure) | **20 / 20** |

Les trois départs non identiques ne le sont que par la **hauteur** — ce que § 17 nommait déjà : les
douze pièces de l'igloo de (3815, 2071) **un bloc trop bas** (y 34 contre 35 : le terrain au bloc
près, pas la règle — l'igloo de `reference-1234567890` y tombe juste au niveau C) ; la ruine chaude
de (9, −10) à 53 contre 49 ; la grande ruine froide de (3249, −4250) à 50 contre 31 — et **3 pièces
contre 18** chez le jeu, l'amas de petites ruines n'étant pas fait (§ 13).

Blocs dans les boîtes des pièces du jeu, et le **témoin** (le même terrain exporté sous
`OV_STRUCTURES=0`) là où il a été fait :

| structure | nous | témoin sans structures |
|---|---:|---:|
| épave échouée | 1 252 / 1 296 — **96,605 %** | 913 / 1 296 — 70,448 % |
| épaves (3) | 3 204 / 3 357 — **95,442 %** | — |
| ruine froide | 4 280 / 4 810 — 88,981 % | — |
| ruine chaude | 453 / 588 — 77,041 % | — |
| igloo | 581 / 928 — 62,608 % | 422 / 928 — 45,474 % |

L'igloo perd ce qu'il perd par son bloc de hauteur : ses boîtes sont décalées d'un cran sur toute
leur hauteur.

**Nether** (graine 987654321, chunks 252..259 × 0..7, 64 chunks dont 56 finis chez le jeu) :
**10 / 10 fossiles** trouvés et **identiques champ pour champ** ; blocs dans leurs boîtes 486 / 628 —
77,389 % ; `References` **37 / 37** — après la marge d'adaptation du terrain ci-dessous ; 11 / 37
avant.

**La marge des structures qui adaptent le terrain.** Les références des fossiles du jeu débordaient
d'un chunk autour de boîtes de quelques blocs. Le fossile est, de ce que nous construisons, le seul à
déclarer `terrain_adaptation` (`beard_thin`) — villages, avant-postes, cité antique, ruines des
sentiers et fort aussi. Mesuré sur les **84 départs de fossiles** des chunks du monde de référence
du Nether qui ont passé `structure_references`, en élargissant la boîte de N blocs et en comparant
les chunks prédits aux chunks qui listent le départ : N = 8 en manque 133, N = 11 en manque 47,
**N = 12 n'en manque aucun**, N = 16 non plus mais en prédit 455 de trop. Retenu : 12
(`kTerrainAdaptationMargin`). Il en reste de trop à N = 12 (247 sur ces 84 départs, dont des chunks
finis que la boîte du fossile touche elle-même et que le jeu ne liste pas) : **non expliqué** ; dans
le carré exporté l'accord est complet. Une référence de trop pointe un départ réel ; une
manquante ferait perdre la structure à qui la cherche.

**Déterminisme, étage attaché** : `ov_gendet --origin=2,1 --side=1 --workers=2` — le carré de
l'épave échouée, en série et par deux fils : **0 cellule différente sur 1 572 864**.

Tests : `test_ov_worldgen "[nbt]"` — noms et types des champs contre ceux relus dans les mondes de
référence, `ChunkPos.asLong` sur une position lue, aller-retour `piece_to_nbt` → `piece_from_nbt`
sur des départs tirés de la graine (igloo, épave échouée, ruine froide, trésor), un chunk qui garde
ses structures à travers `to_nbt`/`from_nbt` ; `test_ov_server "[structures]"` — un départ posé à
la main et traversant deux chunks : départ dans le chunk de départ seulement, référence dans chaque
chunk traversé et nulle part ailleurs ; le trésor enfoui du jeu refusé par le serveur, par son nom.

Coût, build debug : **133,7 s** pour un carré de 4 × 4 chunks (chargement compris) ; l'End,
49,0 s pour quatre carrés.

