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

* **Aucune géométrie.** Pas un bloc de structure n'est posé. Une structure complète, bloc pour
  bloc, était prévue dans le même passage ; elle n'est pas là. Le socle mesuré a été préféré à une
  géométrie plausible, parce qu'une géométrie reconstruite de mémoire au lieu d'être spécifiée
  n'aurait pas d'oracle et n'aurait donc pas de chiffre.
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
