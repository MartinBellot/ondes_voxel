# L'étage des features : placement, ordre de décoration, minerais

Ce qui est établi, ce qui est mesuré, et — surtout — **ce qui ne l'est pas**.

## Le cadre

Interprète, jamais générateur codé en dur, exactement comme `density.cpp` pour
le terrain. Il lit `worldgen/configured_feature/` (194 fichiers, 53 types) et
`worldgen/placed_feature/` (231 fichiers) et refuse par leur nom les types qu'il
ne sait pas construire.

| fichier | contenu |
|---|---|
| `placement.{hpp,cpp}` | le pipeline, les fournisseurs d'entiers et de hauteurs, les ancres, les prédicats de blocs, les tags de blocs |
| `decoration.{hpp,cpp}` | les onze étapes, la graine de décoration, la graine par feature, le tri partagé |
| `ore_feature.{hpp,cpp}` | `ore` et `scattered_ore` |
| `feature.{hpp,cpp}` | le registre, `spring_feature`, `disk` |
| `tools/ov_features` | la mesure contre `run/reference-1234567890` |

### Le pipeline est consommé en profondeur d'abord

C'est la propriété la plus importante et celle qu'aucune sortie ne révèle. Les
tirages vont : `count`, puis pour la **première** position son décalage, sa
hauteur et **tous les tirages de la feature elle-même**, et seulement ensuite le
décalage de la deuxième. Exécuté étape par étape sur un lot, le même pipeline
consomme les mêmes nombres dans un autre ordre et déplace chaque minerai du
monde, sans erreur nulle part. `expand()` est récursif pour cette raison et
`test_feature.cpp` teste l'ordre des appels, pas seulement le résultat.

### Modificateurs implémentés

`count`, `rarity_filter`, `in_square`, `height_range` (distributions `uniform`,
`trapezoid`, `biased_to_bottom`, `very_biased_to_bottom` ; ancres `absolute`,
`above_bottom`, `below_top`), `biome`, `heightmap`, `block_predicate_filter`,
`count_on_every_layer`, `environment_scan`, `surface_relative_threshold_filter`,
`surface_water_depth_filter`, `random_offset`.

### Refusés et nommés

* `noise_threshold_count` et `noise_based_count` lisent `Biome.BIOME_INFO_NOISE`,
  un `PerlinSimplexNoise` de graine fixe 2345 — une famille de bruit absente de
  `noise.cpp`. **Aucune feature constructible ici ne l'utilise**, donc une
  implémentation n'aurait pas d'oracle : ce serait un nombre plausible que
  personne n'a mesuré, ce que ce dépôt refuse. Les huit features concernées
  (végétation du Nether, bambou, fleurs de cerisier) ne se chargent pas.
* `carving_mask` a besoin des masques des carvers.
* `would_survive` et `solid` demandent si un bloc **tiendrait** à une position :
  c'est une question de gameplay, et `ov_gameplay` est une couche au-dessus de
  `ov_worldgen`. Les 36 features de saplings, fleurs et champignons ne se
  chargent donc pas.

Résultat du chargement : **39 des 194 configured features**, **47 placed
features**, 166 tags de blocs, et **114** placed features nommées par une biome
de l'overworld sans être constructibles. `FeatureRegistry::unavailable()` et
`Decorator::missing()` listent le reste par nom — `ov_features --missing`.

## L'ordre de décoration

Les onze étapes de `GenerationStep.Decoration` dans l'ordre du jeu, la graine de
chunk (`decoration_seed`), la graine par feature (`decoration + index +
10000·étape`), et le tri partagé qui donne à chaque étape un ordre unique sur
lequel tous les biomes s'accordent.

Le tri est un tri topologique sur la contrainte « dans chaque biome, cette
feature vient avant celle-là ». Deux features qui ne se croisent jamais dans un
biome ne sont pas ordonnées par la contrainte, et l'égalité est alors tranchée
par l'algorithme lui-même : c'est le **post-ordre inversé d'un parcours en
profondeur**, mesuré et non supposé — voir « Le tri partagé » plus bas. Les
biomes parcourues sont celles que la biome source de la dimension peut nommer,
dans l'ordre de cette source : **53** biomes pour l'overworld, 150 placed
features ordonnées.

L'ordre obtenu à `underground_ores` (`ov_features --order=underground_ores`)
commence par `ore_dirt`, `ore_gravel`, les trois pierres décoratives hautes puis
basses, `ore_tuff`, puis le charbon, le fer, l'or, la redstone, le diamant, le
lapis, le cuivre — c'est-à-dire l'ordre que chaque biome de surface liste.
`test_feature.cpp` vérifie que la chaîne est respectée.

Aucune structure n'appartient à `underground_ores`, donc l'index y part de zéro :
la parité des minerais n'attend pas le travail sur les structures.

## Les minerais — la mesure

`tools/ov_features` prend le terrain **du jeu** comme donné : il lit un chunk
généré par le vrai serveur, remet chaque bloc de minerai dans la pierre qu'il
avait remplacée, lance notre décoration par-dessus, et compare. Cela sépare deux
échecs qui se ressemblent : un minerai au mauvais endroit, et un minerai au bon
endroit dans un terrain de la mauvaise forme. Notre terrain n'a pas encore de
deepslate à l'endroit exact du jeu, donc mesurer les minerais contre notre propre
pierre ne dirait rien sur les minerais.

Graine 1234567890, 44 chunks au statut `minecraft:full` avec leurs huit voisins.

### L'état d'avant, pour mémoire

La mesure précédente donnait **30 blocs sur 13 249 — 0,226 %** au bloc près, avec
un déficit de volume d'environ 20 % (10 561 blocs posés contre 13 249). Les
histogrammes par tranche de seize y étaient, eux, déjà bons : le fer culminait
entre 16 et 31, le diamant s'écrasait contre le fond du monde, les variantes
deepslate n'apparaissaient que sous y = 0. Les fournisseurs de hauteur et les
ancres étaient donc lus correctement, et seul l'ensemencement était faux.

Le déficit de volume était attribué au rejeu de `ore_dirt` et `ore_gravel`
par-dessus ceux du jeu. **Cette explication était fausse** : c'était un bug de
l'outil de mesure, décrit plus bas.

### La graine : ce qui était faux, et comment on l'a su

La session précédente avait éliminé, par balayage : l'index de la feature (0 à
400), l'étape (0 à 10 croisée avec l'index 0 à 47), les deux familles de
générateur et leurs deux combinaisons croisées, cinq variantes de forme de
`decoration_seed`, et 130 000 décalages bruts consécutifs. Rien n'avait dépassé
le plancher de bruit. Ces éliminations étaient justes ; elles ne pouvaient
simplement pas atteindre la réponse, parce que **la réponse n'était pas un
décalage**.

#### Pourquoi le balayage ne pouvait pas aboutir

La métrique du balayage — « le jeu a-t-il un minerai de cette espèce autour de
l'origine de la veine ? » — répond oui une fois sur dix par hasard. Un balayage
de 130 000 essais dans un bruit pareil produit des maxima de 35 % qui ne
veulent rien dire, et il ne peut pas distinguer « graine fausse » de « pipeline
faux ». Sur un espace de 2⁶⁴, aucun nombre d'essais ne rattrape ça.

Il fallait un oracle **exact**. Il existe.

#### La sonde : faire écrire au jeu son propre flux de tirages

`scripts/probe_decoration.py` et `scripts/probe_decoration.sh`.

Un datapack vide la liste de features de `plains` et lui en donne quatre, chacune
`count(1) → in_square → height_range(uniforme sur seize blocs) → simple_block`
d'un bloc unique. Un preset de monde privé — une copie de
`minecraft:single_biome_surface` sous notre identifiant, pour éviter le
traitement spécial que le serveur dédié réserve au sien — met `plains` partout.

La position de chaque marqueur est alors **exactement trois `nextInt(16)**` :

| tirage | modificateur | ce qu'il donne |
|---|---|---|
| 1 | `in_square` | `dx` |
| 2 | `in_square` | `dz` |
| 3 | `height_range` uniforme sur 16 | `dy` dans la bande |

Douze bits exacts par marqueur, quarante-huit par chunk. Les marqueurs sont aux
index 0, 1, 2 de l'étape 6 et à l'index 0 de l'étape 8, donc les termes
`+ index` et `10000 × étape` sont contraints séparément. Une hypothèse sur la
graine cesse d'être un pourcentage : elle est fausse au premier chunk.

Deux contrôles avant toute conclusion : la graine lue dans `level.dat` est bien
celle demandée, et `dx`, `dz`, `dy` sont chacun uniformes sur 0..15 — le modèle
« trois tirages bornés à seize » est donc juste.

#### Le chunk (0, 0) élimine les multiplicateurs

À `x = z = 0`, `x·a + z·b` vaut zéro quels que soient `a` et `b`. La graine de
décoration s'y réduit à la graine du monde seule, pour **toutes** les formes
candidates. C'est ce qui rend ce chunk décisif : il teste le *générateur* sans
rien supposer du mélange par coordonnées.

Un balayage exhaustif de toutes les graines de feature dans `[-2³³, 2³³]`, pour
les deux familles, exigeant que les trois marqueurs de l'étape 6 tombent aux
graines `f`, `f+1`, `f+2` : **zéro solution**. Et les flux de `Legacy(S+60000)`
comme de `Xoroshiro(S+60000)` ne contiennent pas la valeur observée dans leurs
quatorze premiers tirages. La formule et l'index étaient donc corrects, ou bien
le générateur ne l'était pas.

#### `WorldgenRandom` est un hybride

C'est la réponse, et aucun des deux noms ne la suggère.

`WorldgenRandom` **enveloppe** une source et ne redéfinit que la primitive
`next(bits)`. Tout ce qui est construit au-dessus de cette primitive reste ce
que `java.util.Random` définit : le tirage borné, le flottant, le double, le
long. L'état avance donc en Xoroshiro128++ pendant que les bits en sortent à la
mode *legacy* :

```
next(bits)     = les bits de POIDS FORT d'un tirage 64 bits de Xoroshiro
next_int(16)   = (16 * next(31)) >> 31          — et non Lemire sur les 32 bits de poids faible
next_long()    = ((long)next(32) << 32) + next(32)   — et non un seul tirage 64 bits
```

Vérification, sur les positions exactes des marqueurs :

| graine du monde | marqueurs | exacts |
|---|---:|---:|
| 1234567890 | 3268 | **3268** |
| 0 | 2116 | **2116** |
| 1 | 2212 | **2212** |
| 2 | 2116 | **2116** |
| **total** | **9712** | **9712 (100,000 %)** |

Les graines 0, 1 et 2 sont **hors échantillon** : la formule a été trouvée sur
1234567890 et n'a pas été retouchée. Les taches vont jusqu'à ±30 000 blocs, donc
le produit `x·a` déborde et le mélange 64 bits est exercé. Xoroshiro pur et
`java.util.Random` pur ne reproduisent, eux, **aucun** marqueur.

Le reste de la formule est confirmé du même coup et sans ambiguïté :

```
a, b        = WorldgenRandom(graine du monde).next_long() | 1,  deux fois
decoration  = (x_min · a + z_min · b) ^ graine du monde         (coordonnées de BLOC)
feature     = decoration + index + 10000 × étape
```

Avec une source *legacy*, l'enveloppe est transparente — `java.util.Random`
s'enveloppant lui-même. C'est pourquoi **les carvers, qui enveloppent une source
legacy, n'ont jamais été touchés par ce bug**.

### Le tri partagé : la départage est un parcours en profondeur

Le générateur corrigé fait passer la parité des positions de 0,226 % à 66,6 %.
Restait un minerai, et un seul : **le cuivre à 0,285 %** quand tout le reste
était entre 68 % et 97 %.

Le balayage d'index ne le trouvait pas non plus, pour la même raison qu'avant :
sa métrique est trop bruitée. Il a fallu un second oracle exact.

#### Lire l'index que le jeu utilise, directement

Même astuce, retournée : un datapack remplace le **corps** de quatre placed
features réelles (`ore_copper`, `ore_lapis`, `ore_iron_middle`, `ore_coal_upper`)
par un marqueur, en gardant leur identité — et donc leur place dans l'ordre
partagé. Dans un monde à biomes réels, la position du marqueur donne la graine
de feature, donc l'index, chunk par chunk.

Sur 529 chunks, à deux graines de monde :

| feature | graine 1234567890 | graine 987654321 | notre tri d'alors |
|---|---:|---:|---:|
| `ore_coal_upper` | 9 (528 chunks) | 9 (527) | 9 |
| `ore_iron_middle` | 12 (523) | 12 (527) | 12 |
| `ore_lapis` | 21 (526) | 21 (526) | 21 |
| `ore_copper` | **24 (528)** | **24 (465)** | **23** |

(Les quelques chunks restants sont des coïncidences à 1/4096 sur un seul chunk,
ou — pour les 64 chunks « not-single » du cuivre à 987654321 — des positions que
le filtre `biome` a rejetées, ce qui est le comportement attendu.)

**L'index est global, pas local au chunk.** C'était l'hypothèse concurrente :
le jeu pourrait numéroter les features présentes *dans ce chunk-ci*, auquel cas
`ore_copper` vaudrait 23 dans un chunk sans `dripstone_caves` et 24 sinon. Le
monde 987654321 est riche en biomes de caverne et en contient aussi qui n'en ont
pas ; l'index y vaut 24 partout. L'hypothèse est éliminée par la mesure, pas par
le raisonnement.

#### Pourquoi 24 et non 23

`ore_copper` et `ore_copper_large` **ne partagent aucun biome** : toutes les
biomes de surface listent le premier en position 23 de leur étape 6, et
`dripstone_caves` liste le second en position 23 de la sienne. Aucune contrainte
ne les ordonne l'un par rapport à l'autre. C'est le **départage du tri** qui
décide, et les deux départages plausibles donnent des réponses opposées :

* **Kahn** prend, parmi les nœuds prêts, celui vu en premier → `ore_copper` à 23 ;
* **le parcours en profondeur** enfonce le premier successeur visité le plus
  profond, donc il est ajouté au post-ordre en premier, donc il finit **dernier**
  une fois le post-ordre inversé → `ore_copper` à 24.

Le jeu prend le second. Un modèle Python de l'algorithme
(post-ordre DFS inversé, nœuds ordonnés par `(étape, ordre de première
rencontre)`, chaînes construites sur la liste de la biome **aplatie à travers
les étapes**) reproduit les quatre index mesurés ; l'implémentation C++ aussi.

#### Quelles biomes, et dans quel ordre

Le numéro de première rencontre dépend de l'ordre dans lequel les biomes sont
parcourues, donc l'ordre des biomes **fait partie de la graine**. Le jeu parcourt
exactement les biomes que la biome source de la dimension peut nommer, dans
l'ordre de cette source — pour l'overworld, l'ordre de première occurrence dans
`reports/biome_parameters/minecraft/overworld.json`, soit **53** biomes et non
les 64 fichiers du disque.

`Decorator::load` prend donc désormais une `BiomeSource`. Ce n'est pas un
confort : lire le répertoire par ordre alphabétique donnait 64 biomes, des
minerais du Nether dans la numérotation de l'overworld, et `ore_copper` à 23.

### Deux bugs dans la mesure elle-même

Trouvés en cherchant d'où venait un déficit de volume de 15 % que la provenance
précédente attribuait — à tort — au rejeu de `ore_dirt` et `ore_gravel`.

1. **`written_.emplace()` gardait le premier écrivain.** `ReferenceLevel`
   enregistrait ses écritures dans une `std::map` avec `emplace`, qui ne remplace
   pas une clé existante, alors que le tableau de blocs, lui, garde la dernière
   écriture. Les features se recouvrent constamment — un blob de granite tombe
   sur de la pierre, une veine de fer tombe sur le granite — donc **2006 blocs
   qui étaient les nôtres et justes étaient enregistrés comme du granite** et
   comptés comme un manque. Le déficit de 15 % était presque entièrement cela.
2. **`z` n'était pas étendu en signe.** La position empaquetée met `z` sur
   vingt-huit bits ; `inside_chunk` le relisait comme non signé, ce qui place
   tout `z` négatif dans aucun chunk. La moitié des taches du monde de référence
   sont à `z` négatif.

Le diagnostic ajouté (`what our replay holds where one of their ores is
missing`) nomme désormais, pour chaque minerai du jeu que nous ne reproduisons
pas, ce que **nous** avons écrit à cette position — ou « rien de nous n'y est
arrivé ». C'est ce qui a rendu les deux bugs visibles en une commande.

### Parité des minerais : le chiffre

`ov_features --chunks=48`, 44 chunks au statut `minecraft:full` avec leurs huit
voisins, graine 1234567890 :

| minerai | jeu | nous | même bloc | % |
|---|---:|---:|---:|---:|
| coal_ore | 2261 | 2261 | 2213 | 97,877 |
| copper_ore | 2456 | 2457 | 2456 | 100,000 |
| deepslate_coal_ore | 31 | 41 | 30 | 96,774 |
| deepslate_copper_ore | 359 | 357 | 357 | 99,443 |
| deepslate_diamond_ore | 702 | 703 | 702 | 100,000 |
| deepslate_gold_ore | 1053 | 1057 | 1053 | 100,000 |
| deepslate_iron_ore | 1326 | 1321 | 1321 | 99,623 |
| deepslate_lapis_ore | 646 | 643 | 643 | 99,536 |
| deepslate_redstone_ore | 1561 | 1556 | 1556 | 99,680 |
| diamond_ore | 23 | 23 | 23 | 100,000 |
| gold_ore | 174 | 173 | 173 | 99,425 |
| iron_ore | 2143 | 2147 | 2143 | 100,000 |
| lapis_ore | 403 | 406 | 403 | 100,000 |
| redstone_ore | 111 | 116 | 111 | 100,000 |
| **tout** | **13 249** | **13 261** | **13 184** | **99,509** |

**0,226 % → 99,509 %**, au bloc près.

Les 65 blocs qui manquent encore, nommés par le diagnostic : 33 sont des blocs
qu'un de nos propres blobs de pierre décorative (diorite, andésite) a atteints en
premier, 17 ne sont atteints par rien de chez nous, et une quinzaine sont des
minerais à nous d'une autre espèce. L'explication la plus probable est l'ordre
dans lequel les neuf chunks du voisinage sont décorés : une veine qui déborde
d'un chunk sur l'autre écrase, ou non, selon qui passe en premier, et notre
protocole décore le 3×3 dans un ordre fixe qui n'est pas forcément celui du jeu.
**Ce n'est pas mesuré**, et tant que ça ne l'est pas c'est une hypothèse.

Cinq minerais sont à 100,000 % ; aucun n'est sous 96 %.

### Hors échantillon : graine 987654321

`PATCHES="0,0 30000,-30000" ./scripts/reference_world.sh 987654321`, puis
`ov_features --world=run/reference-987654321/world --seed=987654321 --chunks=48`.
Rien n'a été retouché entre les deux mesures.

| minerai | jeu | nous | même bloc | % |
|---|---:|---:|---:|---:|
| deepslate_redstone_ore | 1280 | 1280 | 1280 | 100,000 |
| redstone_ore | 126 | 126 | 126 | 100,000 |
| diamond_ore | 12 | 12 | 12 | 100,000 |
| gold_ore | 90 | 103 | 89 | 98,889 |
| deepslate_lapis_ore | 545 | 539 | 538 | 98,716 |
| iron_ore | 1841 | 1833 | 1813 | 98,479 |
| deepslate_diamond_ore | 548 | 551 | 539 | 98,358 |
| deepslate_iron_ore | 1042 | 1015 | 1004 | 96,353 |
| lapis_ore | 390 | 383 | 377 | 96,667 |
| deepslate_gold_ore | 1026 | 981 | 948 | 92,398 |
| deepslate_copper_ore | 554 | 568 | 480 | 86,643 |
| **copper_ore** | 4598 | 4124 | 3341 | **72,662** |
| **coal_ore** | 4173 | 4089 | 2511 | **60,173** |
| **deepslate_coal_ore** | 52 | 51 | 31 | **59,615** |
| **tout** | **16 277** | **15 655** | **13 089** | **80,414** |

C'est un chiffre plus bas que celui de l'échantillon, et il faut le dire tel
quel. Il n'est pas réparti : **onze minerais sur quatorze restent entre 86 % et
100 %**, et l'écart est concentré sur deux familles, le charbon et le cuivre.

Ce que l'on sait de cet écart :

* **Ce n'est pas l'index.** L'oracle d'index tourné à cette graine donne 9, 12,
  21 et 24 — les mêmes qu'à 1234567890, et les mêmes que notre tri.
* **Ce n'est pas la graine.** Les 9712 marqueurs de la sonde sont exacts à 100 %
  à quatre graines.
* Les deux familles touchées sont celles dont le comportement dépend de ce que
  le protocole de rejeu **ne peut pas** restituer. `ore_coal_lower` utilise
  `ore_coal_buried`, dont `discard_chance_on_air_exposure` vaut 0,5 : la veine
  saute les blocs adjacents à de l'air, et l'air à l'étape 6 n'est pas l'air du
  monde fini — les étapes 7 à 10 remplissent des cavernes (`lush_caves` en est
  pleine, et ce monde-ci est fait de `dark_forest`, `lush_caves` et
  `dripstone_caves`). Le cuivre, lui, est le seul minerai posé par **deux**
  placed features concurrentes, `ore_copper` et `ore_copper_large`, que seul le
  filtre `biome` sépare, cellule de biome 3D par cellule de biome 3D.
* **Ce partage entre artefact du protocole et défaut de notre code n'est pas
  mesuré.** C'est une hypothèse étayée, pas un résultat. Le trancher demande un
  oracle qui donne l'état du monde *au moment de l'étape 6* — par exemple un
  monde de référence arrêté au statut `features`.

## Ce qui reste à faire

**L'étage de décoration n'est toujours pas branché dans `ChunkGenerator`.** La
raison n'est plus la graine : c'est l'interface. `Decorator::decorate` prend un
`FeatureLevel` parce qu'il lui faut le voisinage 3×3 — une veine commencée dans
la dernière colonne d'un chunk finit dans le suivant — et un adaptateur
mono-chunk avalerait en silence toute écriture qui déborde. L'appel va **après
les carvers**, à l'étape `features` de `chunk_generator.cpp`, une fois qu'un
`FeatureLevel` couvrant le 3×3 existe.

Restent aussi hors de portée les 114 placed features nommées par une biome et
non construites (`ov_features --missing`) : elles réclament `would_survive`,
`solid`, `carving_mask` ou `BIOME_INFO_NOISE`, tous documentés plus haut.

## Reproduire

```bash
# La parité des minerais, avant/après
./build/macos-release/bin/ov_features --chunks=48
./build/macos-release/bin/ov_features --order=underground_ores
./build/macos-release/bin/ov_features --missing

# La sonde : faire écrire au jeu son flux de tirages, puis le vérifier
./scripts/probe_decoration.sh 0
python3 scripts/probe_decoration.py read run/probe-0/world /tmp/markers.json
python3 scripts/probe_decoration.py check /tmp/markers.json 0
```

`probe_decoration.py check` balaie les vingt-quatre combinaisons éliminées
(famille × forme × parité des multiplicateurs × coordonnées de bloc ou de chunk)
et n'en trouve aucune au-dessus du hasard, ce qui garde le tableau des
éliminations à une commande plutôt qu'à une relecture.

`OV_FEATURE_RANDOM=legacy`, `OV_DECORATION_RANDOM=legacy` et
`OV_DECORATION_SEED=plus|chunk|even|bare` rebranchent les variantes dans le code
C++.

## Pièges pour les autres agents

* **`WorldgenRandom` n'est pas la source qu'il enveloppe.** Il ne redéfinit que
  `next(bits)` ; `nextInt`, `nextLong`, `nextFloat`, `nextDouble` restent ceux de
  `java.util.Random`. Partout où le jeu écrit `new WorldgenRandom(new
  XoroshiroRandomSource(...))`, les tirages sont hybrides. Avec une source
  legacy l'enveloppe est transparente — c'est pourquoi les carvers vont bien.
* **Un pourcentage agrégé sur un proxy bruité ne tranche rien.** Le balayage de
  130 000 décalages était méthodologiquement correct et structurellement
  incapable d'aboutir. Quand la mesure a un plancher de bruit de 10 %, il faut
  changer de mesure, pas augmenter le nombre d'essais. Un datapack qui fait
  écrire au jeu la sortie brute de son RNG coûte quatre minutes et donne douze
  bits exacts par marqueur.
* **Le chunk (0, 0) est un banc d'essai gratuit.** `x·a + z·b` y vaut zéro, donc
  tout ce qui dépend des multiplicateurs disparaît. Toute question sur un
  mélange de coordonnées devrait commencer là.
* **Le départage d'un tri topologique fait partie de la graine.** Deux features
  que rien n'ordonne prennent l'index que l'algorithme leur donne, et Kahn et le
  parcours en profondeur donnent des réponses opposées. De même, **quelles**
  biomes sont parcourues et dans quel ordre : 64 fichiers alphabétiques ne sont
  pas les 53 biomes de l'overworld dans l'ordre de sa table climatique.
* **`std::map::emplace` ne remplace pas.** Dans un enregistrement d'écritures,
  c'est « le premier écrivain gagne » alors que le monde dit « le dernier gagne ».
  Ça a coûté 2006 blocs de parité et une explication fausse dans ce fichier.
* **La lazy stream de Java.** Le pipeline de placement doit être parcouru en
  profondeur d'abord, feature exécutée entre deux positions. Toute autre forme
  consomme les mêmes tirages dans un autre ordre.
* **`discard_chance_on_air_exposure` à 0 ou 1 ne tire pas.** Un tirage superflu
  décale toutes les features suivantes du chunk.
* **Les états de fluide ne sont pas des états de bloc.** Le `state` d'un
  `spring_feature` porte `falling`, qui n'existe pas sur le bloc ; le jeu
  convertit en bloc *legacy* et une source donne `level=0`. La propriété est
  ignorée, et seulement celle-là.
* **`below_top` vaut `min_y + hauteur - 1 - n`**, pas `min_y + hauteur - n`.
* **`ore_gold_nether`**, pas `ore_nether_gold` : le fichier ne s'appelle pas
  comme le bloc.
