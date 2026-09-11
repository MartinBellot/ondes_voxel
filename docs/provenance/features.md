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

---

# Les arbres et la végétation

Suite directe du travail sur l'ensemencement. La graine et l'index étant
établis, le reste de la chaîne — quel arbre, où, et de quelle forme — devient
mesurable. Ce qui suit est ce qui a été mesuré, et ce qui ne l'a pas été.

| fichier | contenu |
|---|---|
| `tree_feature.{hpp,cpp}` | `tree` : 9 trunk placers, 11 foliage placers, le root placer de la mangrove, 6 décorateurs, les deux tailles, l'ordre `java.util.HashSet` |
| `vegetation_feature.{hpp,cpp}` | `simple_block`, `random_patch`, `flower`, `no_bonemeal_flower`, `block_pile`, `block_column`, les trois sélecteurs, la table de survie, `would_survive` |
| `feature.{hpp,cpp}` | le résolveur récursif de références, les fournisseurs pondérés et tournés |
| `tools/ov_features --trees` | la mesure contre `run/reference-*` |

## Le chargement : 39 → 113 des 194 features configurées

| | avant | après |
|---|---:|---:|
| configured features construites | 39 | **113** de 194 |
| placed features construites | 47 | **134** |
| placed features nommées par une biome et non construites | 114 | **68** |

Trois choses ont débloqué ce chiffre, et deux d'entre elles n'avaient rien à
voir avec les arbres.

### Le chargement n'est plus un passage sur un répertoire trié

`trees_taiga` nomme `minecraft:spruce_checked`, qui est une **placed** feature,
qui nomme `minecraft:spruce`, qui est une **configured** feature. Un
sélecteur peut donc nommer n'importe quoi, dans les deux registres, avant que
le fichier correspondant n'ait été lu. `FeatureRegistry::load` est désormais un
parcours en profondeur du graphe de références, avec deux piles de noms en
cours — une par registre.

**Deux piles, et pas une.** `birch_tall` nomme `super_birch_bees_0002`, et ce
nom existe *à la fois* comme configured feature et comme placed feature :
résoudre la placed résout la configured, et une pile partagée y voit un cycle
et refuse tout le sélecteur. Ça a coûté seize sélecteurs d'arbres — c'est-à-dire
tous les arbres du jeu — jusqu'à ce que le message « takes part in a cycle »
le nomme.

Chaque document a maintenant son propre `simdjson::dom::parser` : un parseur
réutilisé suppose qu'un document est consommé avant le suivant, ce qui n'est
plus vrai quand la lecture est récursive.

### `would_survive` : une table nommée, pas une devinette

Les 36 placed features `*_checked` — c'est-à-dire **tous les arbres** — passent
par `block_predicate_filter { would_survive: <sapling> }`. Sans lui, rien de ce
mandat n'est mesurable.

`would_survive` demande si un bloc *tiendrait* à une position, ce qui est une
question de gameplay, et `ov_gameplay` est une couche au-dessus. La sortie prise
ici est une **table nommée** : `plant_survival_rule()` donne la règle de
`canSurvive` pour chacun des 47 blocs que les features de végétation de vanilla
posent réellement, groupés par la famille dont ils l'héritent (dirt-ou-farmland,
dirt-ou-argile, dead bush, nylium, cactus, canne à sucre, nénuphar, seagrass,
tapis, fleur suspendue). Les huit états que `would_survive` interroge en 1.20.1
sont tous des pousses, et la règle d'une pousse tient en une ligne.

Un bloc absent de la table **refuse sa feature au chargement**, par son nom.
C'est tout le périmètre de l'approximation, et il est visible : les quatre
features que ça coûte sont `patch_brown_mushroom` et `patch_red_mushroom`, dont
la règle lit le niveau de lumière, et `patch_fire` / `patch_soul_fire`, dont la
règle demande ce qui est inflammable autour.

Le fait qui rend cette table acceptable là où la même chose serait inacceptable
pour un minerai : **le nombre de tirages d'un patch ne dépend pas du sol.**
`random_patch` fait six tirages par essai, ses 64 essais, que le terrain soit de
l'herbe ou de la pierre ; le fournisseur d'état est interrogé *avant* le test de
survie. Une règle fausse coûte donc des blocs et jamais la graine. C'est
vérifié par un test : la même graine sur deux mondes différents laisse le
générateur dans le même état.

`solid`, `replaceable` et `unobstructed` restent refusés — ils n'ont pas de
table équivalente. C'est ce qui bloque encore `disk_grass`, `patch_melon` et
`pointed_dripstone`.

Le branchement dans `placement.cpp` est **trois lignes**, encadrées par un
commentaire, pour que la fusion reste triviale.

### L'ordre d'itération d'un `java.util.HashSet` fait partie de la graine

`TreeDecorator.Context` reçoit les positions des bûches et des feuilles depuis
des `HashSet<BlockPos>`, triées par y avec un tri **stable**. Ce qui survit au
tri, à y égal, est l'ordre des seaux de la table de hachage — et les
décorateurs tirent une fois par élément de ces listes. `trunk_vine` tire quatre
fois par bûche, `leave_vine` quatre fois par feuille, `beehive` mélange une
liste construite à partir de cet ordre.

`java_hash_order()` reproduit `HashMap` : `hash = h ^ (h >>> 16)`, index
`hash & (capacité - 1)`, ordre d'insertion dans un seau, et le découpage
lo/hi qui préserve l'ordre relatif à chaque redimensionnement. `Vec3i.hashCode()`
est `(y + z·31)·31 + x`. La seule chose non modélisée est la *treeification* —
un seau de huit entrées dans une table d'au moins 64 — qui n'a jamais été
observée sur un arbre et qui, si elle arrive, écrit une erreur dans le journal
plutôt que de changer silencieusement la forme du monde.

## La mesure : `ov_features --trees`

Un minerai peut être remis en place : il a remplacé exactement un bloc de
pierre, et la pierre est connue. Un arbre, non — il a poussé dans de l'air que
le reste de la décoration a ensuite rempli d'herbe, de fleurs et de neige. Le
protocole est donc l'inverse : **on retire**. Toute bûche, feuille, pousse,
liane et petite plante des neuf chunks est mise à l'air, ce qui est très proche
de ce à quoi ressemblait le monde quand le jeu est arrivé à l'étape végétale, et
notre décoration est rejouée par-dessus. L'eau, les seagrass et le varech ne
sont pas retirés : ce sont du terrain, et les retirer viderait chaque marais.

Deux échecs sont comptés séparément parce qu'ils n'ont pas la même cause :

* un tronc au mauvais **endroit** — le pipeline de placement ou la graine est
  faux, et le code de l'arbre n'est même pas atteint ;
* un tronc au bon endroit avec la mauvaise **forme** — le placement est juste
  et un placer tire autrement.

Un pourcentage sur tous les blocs de feuilles moyennerait les deux et ne dirait
rien sur aucun. Un troisième chiffre est donné, plus dur que les deux autres :
la forme du **premier essai** d'une feature dans son chunk. Le pipeline est
consommé en profondeur d'abord, donc l'essai 1 commence là où la feature de
l'essai 0 s'est arrêtée : seul l'essai 0 a un état de générateur dont on sait
qu'il est juste, et une mauvaise forme là est un mauvais placer et rien d'autre.

Chaque écriture est étiquetée par l'essai qui l'a faite. Sans ça une forêt est
une mer indistincte de bûches et de feuilles, et il n'y a aucun moyen de dire
que *cet* arbre-ci a la mauvaise forme.

**Comparaison par bloc, pas par état.** Le jeu fait une dernière passe sur un
arbre fini qui réécrit la propriété `distance` de chaque feuille depuis la
bûche la plus proche. Cette passe ne tire rien et ne fait pas partie de la
forme ; comparer les états appelait « différence » chaque feuille correcte et
donnait 3,8 % là où la vraie valeur est 45 %. Cette passe n'est **pas**
implémentée : nos feuilles sortent avec la `distance` du fournisseur.

### Les chiffres — graine 1234567890, **avant** la sonde d'arbre

> Ces deux tableaux sont l'état d'avant, gardé pour mémoire. Les chiffres
> d'après, mesurés sur exactement le même échantillon, sont plus bas dans
> « La sonde d'arbre ».

200 chunks, un sur treize par région du monde de référence — les minerais sont
partout et une région en est un échantillon honnête, les arbres non : la
première région dans l'ordre des coordonnées est de l'océan, ce qui se lisait
« aucun arbre nulle part » au lieu de « aucun arbre ici ».

| | |
|---|---:|
| troncs dans le monde du jeu | **378** |
| troncs chez nous | 395 |
| **au même endroit** | **153** (40,476 %) |
| arbres que nous avons fait pousser | 367 |
| dont la forme est identique | 66 (17,984 %) |
| arbres posés sur un vrai tronc du jeu | 153 |
| dont la forme est identique | **66** (43,137 %) |
| blocs de bûche et de feuille : jeu / nous / même bloc | 24 635 / 24 182 / **9 591** (38,932 %) |

Par feature, parmi les arbres posés sur un vrai tronc :

| placed feature | forme identique | ce qu'elle contient |
|---|---:|---|
| `trees_savanna` | 1 / 1 | forking trunk + acacia foliage |
| `trees_birch` | 8 / 9 | straight trunk + blob foliage |
| `birch_tall` | 7 / 12 | straight + blob + `beehive` |
| `trees_birch_and_oak` | 49 / 111 | chêne et bouleau (justes) mêlés au **fancy oak** |
| `trees_jungle` | 1 / 18 | jungle : bush, blob, mega jungle, trois décorateurs |
| `bamboo_vegetation` | 0 / 1 | |

Premier essai dans son chunk : **17 sur 57**, dont `birch_tall` 3/3,
`trees_savanna` 1/1, `trees_birch` 1/2, `trees_birch_and_oak` 12/35,
`trees_jungle` 0/14.

### Hors échantillon : graine 987654321

Rien n'a été retouché entre les deux mesures.

| | |
|---|---:|
| troncs dans le monde du jeu | 364 |
| **au même endroit** | 68 (18,681 %) |
| arbres posés sur un vrai tronc, forme identique | 33 / 59 (55,932 %) |
| premier essai, forme identique | 8 / 16 (50,0 %) |
| blocs : jeu / nous / même bloc | 17 950 / 7 517 / 3 998 (22,273 %) |

Ce 18,7 % demande son explication, et elle est nette : **242 des 296 troncs
manquants sont dans `dark_forest`**, dont la feature `dark_forest_vegetation`
ne se charge pas — elle a besoin de `huge_brown_mushroom` et
`huge_red_mushroom`. Hors `dark_forest`, il reste 122 troncs et 68 trouvés, soit
**55,7 %**. Les 46 autres sont dans `old_growth_spruce_taiga`, dont l'épicéa
géant est faux (voir plus bas).

Par feature à cette graine : `trees_birch` 25/32 (78,1 %),
`trees_birch_and_oak` 7/8 (87,5 %), `trees_windswept_forest` 1/1,
`trees_old_growth_spruce_taiga` **0/18**.

### Ce qui est établi et ce qui ne l'est pas

**Établi, mesuré :**

* `straight_trunk_placer` + `blob_foliage_placer` — le chêne, le bouleau, la
  jungle simple — sont **exacts au bloc près** : `trees_birch` fait 8/9 à une
  graine et 25/32 à l'autre, et les échecs restants sont des feuilles d'un
  voisin que nous n'avons pas fait pousser, pas la forme de l'arbre.
* `forking_trunk_placer` + `acacia_foliage_placer` : 1/1 et 1/1. Deux
  observations, ce n'est pas une preuve, et c'est dit tel quel.
* L'**index à l'étape végétale** est le nôtre. C'est le seul point du mandat
  qui a demandé un oracle en plus de la comparaison de formes, et il en valait
  la peine : l'index n'avait été mesuré qu'à l'étape des minerais.
  `ov_features --trees --calibrate=<feature> --step=9` balaie l'index et
  compte, pour chacun, combien de fois la **première** position que le pipeline
  produit tombe exactement dans une colonne où le jeu a fait pousser un tronc.

  | feature | meilleur index | touches | plancher de bruit | notre trieur |
  |---|---:|---:|---:|---:|
  | `trees_birch_and_oak` | 20 | **31 / 37** | 1 à 3 | 20 |
  | `trees_jungle` | 7 | **9 / 14** | 1 à 2 | 7 |

  Un ordre de grandeur au-dessus du bruit, aux deux. La graine, la formule et
  le tri partagé tiennent donc aussi à l'étape 9.

**Pas établi, et nommé :**

> Les trois premières entrées de cette liste ont été tranchées depuis, par la
> sonde d'arbre. Elles sont gardées telles quelles parce que **deux des trois
> diagnostics étaient faux**, et que c'est cela qu'il faut retenir : « le
> sélecteur de la jungle choisit autrement que le jeu » et « l'épicéa géant est
> faux dès le premier tirage » étaient des conclusions raisonnables tirées d'une
> mesure trop grossière pour les porter. Voir « La sonde d'arbre » plus bas.

* ~~**`fancy_trunk_placer` / `fancy_foliage_placer`.**~~ Le tronc était juste ;
  seul le feuillage était faux, et pas « deux niveaux trop bas » mais *à
  l'envers*. Le `Math.min(1, …)` mesuré ici est confirmé à 1291 arbres sur 1329.
* ~~**La jungle.**~~ Le sélecteur était juste (97,6 % de troncs identiques). Ce
  qui manquait est un `nextInt(2)` dans `bush_foliage_placer`.
* ~~**`giant_trunk_placer` + `mega_pine_foliage_placer`.**~~ Le
  `giant_trunk_placer` est juste à 96,97 %, et la hauteur qu'il tire tombe juste
  789 fois sur 790 : ce n'est pas une divergence dès le premier tirage. Reste
  `mega_pine_foliage_placer`, toujours faux.
* `cherry_trunk_placer` et `cherry_foliage_placer` sont écrits mais **n'ont
  aucun oracle** : ni `run/reference-1234567890` ni `run/reference-987654321` ne
  contient de `cherry_grove`. Ils se chargent, ils produisent un arbre, et rien
  ne dit que c'est le bon. Les feuilles suspendues en particulier sont une
  reconstruction plausible et non mesurée.
* `mega_jungle_trunk_placer`, `dark_oak_*`, `bending_trunk_placer`,
  `upwards_branching_trunk_placer` et `mangrove_root_placer` : aucun n'apparaît
  dans l'échantillon mesuré. Ils se chargent ; leur exactitude n'est pas
  démontrée.
* La passe qui recalcule la propriété `distance` des feuilles n'est pas
  implémentée. Elle ne tire rien, mais un monde sauvegardé par nous n'est pas
  identique bloc-état pour bloc-état tant qu'elle manque.

---

# La sonde d'arbre : un arbre connu par chunk

La parité des arbres était bloquée à **40,5 % de troncs au bon endroit** et
**43,1 % de formes exactes** parmi eux, et la raison n'était pas qu'on manquait
d'idées : c'est que la mesure ne pouvait pas trancher. Un monde de référence
mélange dans un seul pourcentage le sélecteur, la biome, le pipeline de
placement, le voisinage, le protocole de dépouillement et le placer. Une
hypothèse sur un placer y vaut deux points de pourcentage, c'est-à-dire rien.

C'est le même mur que celui des minerais, et la réponse est la même :
**faire écrire au jeu ce qu'on veut lire.**

## Le montage

`scripts/probe_tree.sh` et `scripts/probe_tree.py`, calqués sur
`probe_decoration.*`.

Un datapack vide les onze listes de features de `plains` et n'en laisse qu'une,
à l'étape 9 (`vegetal_decoration`), index 0 :

```
count(1) → in_square → heightmap(OCEAN_FLOOR) → <la feature demandée>
```

Les carvers sont vidés eux aussi, et un preset de monde privé met `plains`
partout. Le monde qui en sort a **un arbre par chunk**, isolé, sur un terrain de
plaine. Les deux premiers tirages de la graine de feature donnent sa colonne ;
tout le reste du flux appartient à l'arbre.

La feature est **nommée**, jamais copiée : le datapack ne redéfinit rien de la
forme de l'arbre, sinon la sonde mesurerait le datapack. L'exception est
délibérée et sert à autre chose — voir « faire varier un champ » plus bas.

Deux vérifications avant toute conclusion, toutes deux passées :

* la colonne prédite par la formule de graine tombe sur une bûche du jeu dans
  **1329 chunks sur 1329** ;
* la hauteur prédite par `base + nextInt(a+1) + nextInt(b+1)` égale la longueur
  du tronc vertical observé dans **789 cas sur 790** pour l'épicéa géant, aux
  dix-sept hauteurs possibles.

Autrement dit : l'ensemencement, l'index, `in_square` et le heightmap sont
justes, et tout écart qui reste appartient au placer. C'est exactement ce que la
mesure sur le monde de référence ne pouvait pas dire.

## `ov_features --probe` : la sonde passe par notre code

Un modèle Python de la géométrie aurait été une deuxième implémentation, pas une
mesure de la première — le piège n° 13. `ov_features --probe` lit donc le monde
de la sonde, dépouille son bois, rejoue **le C++** sur les neuf chunks du
voisinage et compare arbre par arbre :

```bash
./build/macos-debug/bin/ov_features --probe --world=run/probe-tree-fancy_oak-1234/world \
    --seed=1234 --feature=minecraft:fancy_oak --chunks=300
```

`--feature` accepte plusieurs noms séparés par des virgules : ils vont aux index
0, 1, 2… comme le datapack les a écrits. Ce n'est pas un confort — un monde de
sonde qui porte trois espèces les a toutes les trois sur le disque, et noter une
espèce contre un monde qui contient les deux autres appelle « différence » les
feuilles d'un voisin.

Trois chiffres en sortent : *bûches identiques*, *arbre entier identique*, et
l'accord bloc à bloc. Le premier sépare le trunk placer du foliage placer sans
qu'on ait à regarder une silhouette.

## Ce que la sonde a trouvé

### 1. Le fancy oak : le tronc était juste, la canopée était à l'envers

Sur 300 arbres : **96,3 % de troncs identiques, 0 % d'arbres entiers**. Le
`fancy_trunk_placer` était donc déjà bon — y compris le `Math.min(1, …)` que la
provenance précédente avait mesuré à l'aveugle, et qu'un modèle indépendant
confirme à **1291 arbres sur 1329**. Toute l'erreur était dans le feuillage.

Pour lire la règle plutôt que la deviner, trois copies du fancy oak, chacune ne
différant que par un champ de son foliage placer, ont été semées **dans le même
monde** avec trois essences de bois différentes — bouleau, épicéa, acajou — pour
rester distinguables même quand deux canopées se recouvrent :

| radius | offset | height | rangées, du haut vers le bas | arbres mesurés |
|---:|---:|---:|---|---:|
| 2 | 4 | 4 | 1, 2, 2, 2, 1 | 448 |
| 4 | 4 | 4 | 3, 4, 4, 4, 3 | 316 |
| 2 | 6 | 6 | 1, 2, 2, 2, 2, 2, 1 | 312 |
| 2 | 1 | 4 | 1, 2, 2, 2, 1, à `dy` 1 … −3 | 307 |

La règle est donc : les rangées vont de `offset` à `offset - height` comme
partout ailleurs, la portée vaut le rayon, et **la première et la dernière
rangée sont d'un bloc plus étroites**.

Ce que nous avions à la place — `portée = rayon + 1 − y` — fait un cône
**le plus large en bas**, ce qui est précisément le symptôme décrit dans la
provenance précédente comme « le feuillage est deux niveaux trop bas ».

Le compte de cellules fixe le masque aussi serré : **21 sur 25** à la portée 2,
**37 sur 49** à la portée 3, **61 sur 81** à la portée 4. Ce sont exactement les
points entiers vérifiant `x² + z² < portée² + portée`, et rien d'autre. Le
`x + z >= 7` qui était écrit ici ne se déclenche jamais à ces portées et n'est
donc pas mesurable ; il a été retiré plutôt que gardé sans preuve.

| `minecraft:fancy_oak`, 300 chunks | avant | après |
|---|---:|---:|
| bûches identiques | 96,333 % | **97,333 %** |
| arbre entier identique | **0,000 %** | **95,333 %** |
| blocs de bois au même endroit | 45,721 % | **98,542 %** |

### 2. La jungle : ce n'était pas le sélecteur, c'était un tirage manquant

`trees_jungle` valait 1 arbre sur 18 sur le monde de référence, et la provenance
précédente concluait « le sélecteur choisit autrement que le jeu ». La sonde dit
le contraire, et c'est le genre d'erreur qu'aucun pourcentage global ne pouvait
attraper : sur 126 arbres, **97,6 % de troncs identiques** pour **38,9 %
d'arbres entiers**. Le sélecteur, lui, ne se trompait presque jamais.

Ce qui manquait est dans `bush_foliage_placer`. Le jeu garde **trois** coins sur
quatre d'une rangée de portée 1 — donc le coin n'est pas jeté d'office — et il
les garde aussi à `y` local nul, donc la clause `|| y == 0` du blob n'y est pas
non plus. Et le bloc unique au sommet d'un buisson, qui est le coin dégénéré
d'une rangée de portée **zéro**, est tantôt écrit tantôt non : c'est ainsi qu'on
sait que le tirage a lieu même quand il n'y a pas de coin à proprement parler.

```
should_skip(x, y, z, portée) = x == portée && z == portée && nextInt(2) == 0
```

Ce tirage manquait entièrement. Sa portée dépasse de loin les trois rangées d'un
buisson : **toute feature placée après un buisson dans le même chunk lisait le
générateur un cran trop tôt.**

| `minecraft:trees_jungle`, 200 chunks | avant | après |
|---|---:|---:|
| bûches identiques | 97,619 % | 97,619 % |
| arbre entier identique | **38,889 %** | **92,063 %** |
| blocs de bois au même endroit | 95,681 % | **97,939 %** |

### 3. L'épicéa : une rangée de feuillage de moins

`spruce_foliage_placer` s'arrêtait une rangée trop tôt, en bas. La progression
des portées était juste — la suite `1, 0, 1, 2, 1` du jeu sort telle quelle de
notre boucle — mais la boucle finissait un cran trop haut, parce que la hauteur
de feuillage était `hauteur − 1 − tronc` au lieu de `max(4, hauteur − tronc)`.

Les deux formes ont été mesurées sur le même échantillon plutôt que choisies :
`max(4, h − t)` donne 27 épicéas exacts sur 44, `h − t` en donne 25, et
`h − 1 − t` en donnait 0. Le `max(4, …)` ne se distingue que sur les épicéas les
plus courts — un sur douze — donc l'écart de deux arbres est faible et il est
dit tel quel.

### 4. Ce que la sonde dit des autres placers, sans que ce soit corrigé

| feature | arbres | exacts | ce qu'on en sait |
|---|---:|---:|---|
| `trees_birch_and_oak` | 119 | **98,3 %** | sélecteur, chêne, bouleau, fancy oak et le décorateur `beehive`, tous justes ensemble |
| `birch` | 45 | 91,1 % | |
| `jungle_tree` | 55 | 89,1 % | |
| `oak` | 49 | 85,7 % | |
| `spruce` | 44 | 61,4 % | reste un écart non expliqué |
| `acacia` | 33 | 48,5 % | `forking_trunk_placer` + `acacia_foliage_placer` |
| `mega_spruce` | 33 | **0 %** | bûches à 97,0 % ; **tous** nos blocs de feuillage sont justes, il en manque 3237 sur 13 848 |
| `dark_oak` | 55 | **0 %** | bûches parfaites ; le masque des rangées diffère |

Deux diagnostics valent d'être notés parce qu'ils remplacent des hypothèses par
des faits :

* **`giant_trunk_placer` est juste.** 96,97 % de bûches identiques, et la
  hauteur prédite tombe juste 789 fois sur 790. Le « 0/18 » de
  `trees_old_growth_spruce_taiga` n'était pas une divergence dès le premier
  tirage, contrairement à ce qui était supposé : c'est
  `mega_pine_foliage_placer` seul.
* **`mega_pine_foliage_placer` est trop étroit d'exactement une portée sur
  certains niveaux, jamais trop large.** Sur un arbre lu couche par couche, les
  portées du jeu descendent `0, 0, 1, 0, 2, 1, 2` là où les nôtres font
  `0, 0, 0, 0, 1, 1, 1`. La suite du jeu n'est pas monotone, ce que la formule
  `floor(descente / hauteur × 3,5)` ne peut pas produire ; il y a donc là une
  récurrence, comme celle de l'épicéa, et pas un simple décalage. **Non
  résolu.**
* **`dark_oak_foliage_placer`** : sur la rangée de portée 2 d'une attache
  large, le jeu garde les cellules vérifiant `x' + z' ≤ 2` (fold de la double
  colonne), nous gardons tout sauf les quatre coins. Un masque en disque a été
  essayé et **mesuré moins bon** (acajou 36,4 % contre 48,5 %), donc la règle
  n'est ni l'un ni l'autre. **Non résolu.**

## Les chiffres de parité, avant et après

Même échantillon, même commande, rien retouché entre les deux graines.

```bash
./build/macos-debug/bin/ov_features --trees --chunks=200 --stride=13
./build/macos-debug/bin/ov_features --trees --chunks=200 --stride=13 \
    --world=run/reference-987654321/world --seed=987654321
```

### Graine 1234567890 — l'échantillon sur lequel le travail a été fait

| | avant | après |
|---|---:|---:|
| troncs du jeu | 378 | 378 |
| troncs chez nous | 395 | 366 |
| **au même endroit** | 153 (40,476 %) | **157 (41,534 %)** |
| arbres posés sur un vrai tronc | 153 | 155 |
| **dont la forme est identique** | 66 (43,137 %) | **81 (52,258 %)** |
| premier essai dans son chunk | 17 / 57 (29,825 %) | **24 / 57 (42,105 %)** |
| blocs : jeu / nous / même bloc | 24 635 / 24 182 / 9591 (38,932 %) | 24 635 / 25 232 / **10 985 (44,591 %)** |

Par famille, parmi les arbres posés sur un vrai tronc :

| placed feature | avant | après |
|---|---:|---:|
| `trees_savanna` | 1 / 1 | 1 / 1 |
| `trees_birch` | 8 / 9 | 8 / 9 |
| `birch_tall` | 7 / 12 | 7 / 12 |
| `trees_birch_and_oak` | 49 / 111 (44,1 %) | **57 / 112 (50,9 %)** |
| `trees_jungle` | 1 / 18 (5,6 %) | **6 / 19 (31,6 %)** |
| `trees_sparse_jungle` | 0 / 1 | **1 / 1** |
| `bamboo_vegetation` | 0 / 1 | **1 / 1** |

### Graine 987654321 — hors échantillon, rien retouché entre les deux

| | avant | après |
|---|---:|---:|
| troncs du jeu | 364 | 364 |
| **au même endroit** | 68 (18,681 %) | 67 (18,407 %) |
| arbres posés sur un vrai tronc, forme identique | 33 / 59 (55,932 %) | **39 / 58 (67,241 %)** |
| premier essai, forme identique | 8 / 16 (50,0 %) | **11 / 16 (68,750 %)** |
| blocs : jeu / nous / même bloc | 17 950 / 7517 / 3998 (22,273 %) | 17 950 / 7559 / **4158 (23,164 %)** |

| placed feature | avant | après |
|---|---:|---:|
| `trees_birch` | 25 / 32 (78,1 %) | 25 / 32 (78,1 %) |
| `trees_birch_and_oak` | 7 / 8 (87,5 %) | 7 / 8 (87,5 %) |
| `trees_windswept_forest` | 1 / 1 | 1 / 1 |
| `trees_old_growth_spruce_taiga` | **0 / 18 (0 %)** | **6 / 17 (35,3 %)** |

Le taux de troncs au bon endroit ne bouge pas à cette graine, et il ne pouvait
pas : **242 des 297 troncs manquants sont dans `dark_forest`**, dont la feature
ne se charge toujours pas. Hors `dark_forest`, il reste 122 troncs et 67
trouvés.

### Le chargement n'a pas bougé

**113 des 194 features configurées**, 134 placed features, **68 nommées par une
biome et non construites** — les mêmes qu'avant, à la feature près. Aucun type
de feature nouveau n'a été implémenté dans ce travail : le deuxième chantier du
mandat, les 68 refus, **n'a pas été entamé**, et le premier d'entre eux par le
nombre d'arbres qu'il débloquerait reste `huge_brown_mushroom` /
`huge_red_mushroom`, qui commandent `dark_forest_vegetation` et ses 242 troncs.
Un monde de sonde pour les deux champignons a été généré
(`run/probe-mush`) et n'a pas été exploité.

`ov_features --missing` liste les 68 par leur nom ; le tableau des causes
racines plus bas est inchangé.

## Reproduire

```bash
# un monde de sonde par espèce (≈ 2 min chacun)
PATCHES="0,0" ./scripts/probe_tree.sh minecraft:fancy_oak 1234 run/probe-fancy
PATCHES="0,0" ./scripts/probe_tree.sh "minecraft:oak minecraft:birch minecraft:jungle_tree" \
        1234 run/probe-p1

# la mesure, qui passe par notre C++
./build/macos-debug/bin/ov_features --probe --world=run/probe-fancy/world \
    --seed=1234 --feature=minecraft:fancy_oak --chunks=300
./build/macos-debug/bin/ov_features --probe --world=run/probe-p1/world --seed=1234 \
    --feature=minecraft:oak,minecraft:birch,minecraft:jungle_tree --chunks=150 --show=2

# faire varier un champ d'un placer et lire la règle du jeu
#   (un @fichier.json est une feature configurée à nous, semée à côté des autres)
PATCHES="0,0" ./scripts/probe_tree.sh "@variante_a.json @variante_b.json" 1234 run/probe-fv
```

## Pièges pour les autres agents

* **Un membre d'un limbe n'est pas 6-connexe.** Une branche du fancy oak avance
  d'un pas en `x` et en `z` à la fois : ses bûches se touchent par la diagonale.
  Un remplissage par les six faces coupe la branche du tronc et fait dire à la
  mesure « nous plaçons des bûches en trop, jamais en moins », ce qui envoie
  chercher un mécanisme de rejet qui n'existe pas. Une heure perdue là.
* **Un monde de sonde à trois espèces se note à trois.** Noter une espèce contre
  un monde qui contient les deux autres compte les feuilles du voisin comme une
  différence : le chêne tombait de 98 % à 63 % pour cette seule raison.
* **Un placer qui « ne tire pas » est une hypothèse, pas un fait.** Trois masques
  de coin sur quatre dans ce fichier tiraient dans le jeu et pas chez nous. Le
  symptôme n'est jamais la forme du placer lui-même : c'est la feature
  *suivante* du chunk qui se déplace.
* **Faire varier un champ à la fois est un datapack, pas un raisonnement.**
  Trois variantes du même foliage placer, semées dans un seul monde avec trois
  essences de bois, ont donné la règle des rangées en une génération de deux
  minutes. Le champ `offset` du fichier ne veut pas dire ce qu'on croit tant
  qu'on ne l'a pas bougé.
* **La colonne du tronc est un oracle gratuit.** `heightmap` ne tire pas, donc
  la position d'un arbre est exactement `nextInt(16)` deux fois. Si elle tombe
  juste 1329 fois sur 1329, l'ensemencement est hors de cause et il n'y a plus
  qu'un suspect.

## Ce qui reste refusé, et pourquoi — les 81 features configurées

`ov_features --missing`. Groupées par la cause **racine**, pas par le type de la
feature : dire « `random_patch` n'est pas implémenté » quand la vraie raison est
« pas de règle de survie pour `minecraft:fire` » est une seconde raison, fausse.

| cause | features |
|---|---|
| `nether_forest_vegetation` | les 6 végétations du Nether |
| `vegetation_patch` | `moss_patch`, `moss_patch_bonemeal`, `moss_patch_ceiling`, `clay_with_dripleaves` |
| `huge_fungus` | les 4 champignons géants du Nether |
| `seagrass` | les 4 seagrass |
| `huge_brown_mushroom` / `huge_red_mushroom` | ces deux-là, plus `dark_forest_vegetation` et `mushroom_island_vegetation` qui les nomment |
| `bamboo` | `bamboo_no_podzol`, `bamboo_some_podzol` |
| `netherrack_replace_blobs`, `basalt_columns`, `basalt_pillar`, `delta_feature`, `glowstone_blob`, `twisting_vines`, `weeping_vines` | le Nether |
| `end_gateway`, `end_island`, `end_spike`, `chorus_plant`, `void_start_platform` | l'End |
| `fossil`, `geode`, `iceberg`, `ice_spike`, `lake`, `monster_room`, `desert_well`, `forest_rock`, `freeze_top_layer`, `blue_ice`, `bonus_chest` | features autonomes, hors mandat |
| `multiface_growth`, `sculk_patch`, `root_system`, `sea_pickle`, `kelp`, `vines`, `underwater_magma`, `large_dripstone`, `dripstone_cluster`, `pointed_dripstone`, `coral_tree` | idem |
| `waterlogged_vegetation_patch` | `clay_pool_with_dripleaves`, et `lush_caves_clay` qui le nomme |
| **fournisseurs de bruit** (`noise_provider`, `dual_noise_provider`, `noise_threshold_provider`) | `flower_plain`, `flower_meadow`, `flower_flower_forest` |
| **`solid`** | `disk_grass` |
| **`replaceable`** | `patch_melon` |
| **pas de règle de survie** | `patch_fire` (fire), `patch_soul_fire` (soul_fire), `patch_brown_mushroom`, `patch_red_mushroom`, `dripleaf` (small_dripleaf) |
| `matching_fluids` nomme `minecraft:flowing_water`, qui n'est pas un bloc | `patch_sugar_cane` |
| état de fluide du Nether | `spring_nether_open`, `spring_nether_closed` |

Les trois fournisseurs de bruit lisent un `NormalNoise` construit sur une source
**legacy**, et `noise.cpp` ne sait en construire un que sur Xoroshiro : la
fabrique positionnelle legacy appartient à ce fichier-là, pas à celui-ci.

## Reproduire

```bash
./build/macos-debug/bin/ov_features --missing
./build/macos-debug/bin/ov_features --trees --chunks=200 --stride=13
./build/macos-debug/bin/ov_features --trees --chunks=200 --stride=13 \
    --world=run/reference-987654321/world --seed=987654321

# la forme d'un arbre, la nôtre et la sienne, couche par couche
./build/macos-debug/bin/ov_features --trees --chunks=200 --stride=13 \
    --show=3 --first --only=trees_jungle

# l'index à l'étape végétale, balayé
./build/macos-debug/bin/ov_features --trees --chunks=200 --stride=13 \
    --calibrate=minecraft:trees_birch_and_oak --step=9 --span=80
```

## Pièges pour les autres agents

* **Un nom peut désigner deux choses.** `super_birch_bees_0002` est à la fois
  une configured feature et une placed feature. Une pile de détection de cycles
  partagée entre les deux registres voit un cycle qui n'existe pas et refuse
  seize sélecteurs d'arbres d'un coup.
* **Une table triée qui ne l'est plus ment en silence.** La table de survie est
  lue par recherche binaire ; `birch_sapling` classé après `blue_orchid` a
  répondu « pas de règle » pour la moitié des entrées et fait tomber tous les
  arbres du jeu. Un test vérifie maintenant que les sondes se trouvent.
* **La propriété `distance` d'une feuille n'est pas la forme de l'arbre.** Le
  jeu la recalcule après coup. Comparer des états de bloc plutôt que des blocs
  a divisé le score par douze avant qu'on comprenne pourquoi.
* **Deux canopées se recouvrent tout le temps.** Un enregistrement d'écritures
  « dernier écrivain gagne » attribue les feuilles partagées au mauvais arbre et
  invente des trous. Il faut garder *toutes* les écritures avec l'essai qui les
  a faites.
* **La première région d'un monde de référence n'est pas un échantillon.** Les
  minerais sont partout, les arbres non. Prendre les 44 premiers chunks dans
  l'ordre des coordonnées donnait quatre blocs de feuilles en tout.
* **`nextInt(1)` tire.** `base + nextInt(a+1) + nextInt(b+1)` fait toujours deux
  tirages, même quand `b` vaut 0 et que le second vaut forcément zéro.
* **`Math.min(1, …)` de `FancyTrunkPlacer` n'est pas une faute de lecture.**
  Mesuré dans les deux sens ; `max` est moins bon.
* **`Mth.sin`/`Mth.cos` pour le tronc mega jungle, `Math.sin`/`Math.cos` pour le
  fancy.** Le premier est la table de 65536 flottants, le second est du double.
  Ils ne sont pas interchangeables et l'erreur est invisible.

---

# Les features de l'Overworld (mandat `features-2`)

Deux chantiers : les arbres ramifiés que la sonde d'arbre avait laissés nommés
et faux, puis les types de feature que le serveur refusait au démarrage.

| fichier | contenu |
|---|---|
| `tree_feature.cpp` | mega pine, dark oak, jungle (mega), cerisier, mangrove : placers corrigés |
| `huge_mushroom_feature.cpp` | `huge_brown_mushroom`, `huge_red_mushroom` |
| `cave_feature.cpp` | `geode` (et l'aiguillage de la famille des grottes) |
| `dripstone_feature.cpp` | `pointed_dripstone`, `dripstone_cluster`, `large_dripstone` |
| `lush_feature.cpp` | `vegetation_patch`, `waterlogged_vegetation_patch`, `multiface_growth` (lichen) |
| `root_system_feature.cpp` | `root_system` (l'azalée enracinée) |
| `ocean_feature.cpp` | `seagrass`, `kelp`, `sea_pickle`, les trois coraux, `underwater_magma` |
| `surface_feature.cpp` | `vines`, `forest_rock`, `lake` (lave), `ice_spike` |
| `terrain_feature.cpp` | les refus nommés, chacun avec sa raison |
| `biome_info_noise.cpp` | `BIOME_INFO_NOISE`, `noise_threshold_count`, `noise_based_count` |
| `noise_state_provider.cpp` | `noise_provider`, `noise_threshold_provider`, `dual_noise_provider` |
| `overworld_feature.{hpp,cpp}` | l'aiguillage, les prédicats `solid`, `replaceable`, `matching_fluids` |

| `bamboo_feature.cpp` | `bamboo` |

## Le chargement : 113 → 150 des 194 features configurées

`ov_features --missing` :

| | avant | après |
|---|---:|---:|
| configured features construites | 113 de 194 | **150** de 194 |
| placed features construites | 134 | **183** |
| placed features nommées par une biome et non construites | 68 | **20** |

Les vingt qui restent sont exactement les refus nommés plus bas : les huit
placements des champignons (`brown_mushroom_*`, `red_mushroom_*` — leur survie
lit la lumière), glace bleue, icebergs, fossiles, puits du désert, donjons,
couche gelée, sculk, et `seagrass_simple` (qui demande `carving_mask`).

Un des déblocages n'était pas une feature mais un **bug de lecture** :
`forest_flowers` et `flower_forest_flowers` étaient refusées sans aucun message.
Leur `count` est un fournisseur `clamped` dont la `source` est rangée *dans*
`value`, à côté des bornes ; le lecteur la cherchait au niveau supérieur, ne la
trouvait pas et rendait « malformed ». Une ligne dans `placement.cpp`.

Les fichiers partagés ne portent que des crochets d'une à trois lignes,
marqués `features-2` : un appel dans `feature.cpp` (types de feature et
fournisseurs d'état), deux dans `placement.cpp` (prédicats, modificateurs),
une ligne dans `decoration.cpp` (la graine du monde dans `FeatureContext`).

## La mesure : une sonde « bloc pour bloc »

La sonde d'arbre ne compare que le bois. Une géode, un lac ou un champignon
géant ne sont pas faits de bûches. `ov_features --probe --control=<monde>`
généralise la sonde :

* le **monde témoin** a la même graine, le même preset `plains` et **aucune
  feature** — c'est le terrain avant la feature, bloc pour bloc ;
* le **monde sonde** est le même terrain après la feature du jeu ;
* notre feature est rejouée **par notre C++** sur le témoin (neuf chunks), et
  le chunk central est comparé bloc à bloc là où l'un des deux côtés a changé
  quelque chose.

`scripts/probe_tree.py pack` accepte deux formes de plus : `=minecraft:nom`
nomme une **placed** feature de vanilla, pipeline compris, que la biome liste
sous son propre nom ; `%fichier.json` est une placed feature écrite par nous.
`STEP=` choisit l'étape. Une liste vide (`none`) donne le témoin.

**Le piège du fluide.** Entre le témoin et la sonde, 3193 blocs d'eau
différaient à des endroits que la feature n'avait pas touchés : un tick de
fluide qui a tourné dans un monde et pas dans l'autre. Ils sont désormais
comptés à part (« fluid settled differently »), et seulement là où nous
n'avons rien écrit.

**Le témoin absurde (piège 14).** Chaque mesure est refaite avec l'index
décalé d'un cran — donc une graine de feature fausse. Pour les champignons
géants : **94,986 %** de blocs identiques avec la bonne graine, **2,010 %**
avec la fausse. La métrique mesure bien la feature.

## Les arbres ramifiés

Mondes sonde `probe-f2-a` (épicéa géant, chêne noir, acacia) et `probe-f2-b`
(jungle géante, cerisier, mangrove), graine 1234, trois essences par monde
pour rester distinguables. Arbre entier identique, 300 chunks :

| feature | avant | après |
|---|---:|---:|
| `mega_spruce` | 0 / 142 (0 %) | **54 / 144 (37,5 %)** |
| `dark_oak` | 0 / 255 (0 %) | **98 / 255 (38,4 %)** |
| `acacia` | 104 / 217 (47,9 %) | **161 / 216 (74,5 %)** |
| `mega_jungle_tree` | 0 / 135 (0 %) | **39 / 133 (29,3 %)** |
| `mangrove` | 0 / 46 (0 %) | **64 / 145 (44,1 %)** |
| `cherry` | 0 / 215 (0 %) | 0 / 214 (0 %) |
| bûches identiques, monde B | 15,9 % | **82,5 %** |
| blocs de bois identiques, monde A | 75,5 % | **85,4 %** |

Ce que chaque correction a été :

* **`mega_pine_foliage_placer` : une crête.** La suite non monotone
  `0, 0, 1, 0, 2, 1, 2` que la sonde précédente avait lue couche par couche
  vient d'une règle : un niveau dont la portée lissée égale celle du niveau
  d'en dessous gagne un bloc quand son `y` est pair.
* **`dark_oak_foliage_placer` : le masque lu sur la sonde.** Sur la rangée
  large d'une attache deux-par-deux, le jeu retire les cellules dont `x` et `z`
  sont tous deux dans {−r, r, r+1} : la colonne lointaine du carré (r+1) *et*
  celle d'avant comptent comme son bord. La rangée du haut garde
  `x + z ≤ 2r − 2`, la rangée basse d'une attache simple perd ses coins.
* **`jungle_foliage_placer`** (seule la jungle géante l'utilise) portait le
  masque de l'acacia ; c'est un disque coupé à `x + z ≥ 7`.
* **`cherry_trunk_placer`** a été réécrit : deux départs de branche (le second
  tiré dans l'intervalle du premier réduit d'un cran en haut, décalé s'ils
  coïncident), puis le nombre de branches, une direction partagée, la seconde
  branche à l'opposé, et une marche vers la cible avec un flottant par pas. Le
  tronc et les branches sont désormais **justes** sur les exemples lus ; ce qui
  reste faux est le **bord inférieur du feuillage** (trous et feuilles
  pendantes), superposé dans les exemples lus à la canopée d'un voisin. **Non
  résolu**, et c'est pourquoi le cerisier reste à 0 % d'arbres entiers.
* **`upwards_branching_trunk_placer`** (mangrove) tire deux fois
  `extra_branch_length` et ajoute des attaches à chaque pas et en bout de
  branche ; **`mangrove_root_placer`** simule quatre racines qui descendent
  jusqu'au sol et fait échouer tout l'arbre si l'une d'elles reste en l'air à
  sa quinzième marche.

**Ce qui reste des échecs n'est plus, pour l'essentiel, le placer.** Les
exemples lus d'épicéa géant et de chêne noir « faux » sont identiques arbre
pour arbre ; les cellules qui diffèrent appartiennent à l'arbre d'un chunk
voisin qui déborde dans la boîte, et que le jeu a fait pousser avant ou après
le nôtre. La sonde compte l'arbre comme faux. Ce n'est pas mesuré en chiffre ;
c'est lu sur les exemples, et dit comme tel.

### Sur le monde de référence, graine 1234567890

`ov_features --trees --chunks=200 --stride=13`, même échantillon que les
chiffres précédents :

| | avant (ce fichier) | après |
|---|---:|---:|
| troncs au même endroit | 157 / 378 (41,534 %) | **160 / 378 (42,328 %)** |
| forme identique, sur un vrai tronc | 81 / 155 (52,258 %) | 75 / 158 (47,468 %) |
| blocs de bois identiques | 10 985 (44,591 %) | 11 140 (45,220 %) |

La forme semblait **baisser** — dans `trees_birch_and_oak` (57 → 52) et
`trees_jungle` (6 → 5), alors que ni le chêne, ni le bouleau, ni le fancy oak
n'utilisent un placer modifié ici. La contre-épreuve l'a tranché : le même
échantillon, mesuré avec **l'ancien `tree_feature.cpp` seul** remis en place
(`.scratch/tree_regression.sh` — deux binaires, le fichier restauré à l'octet) :

| | ancien `tree_feature.cpp`, ici | le nôtre |
|---|---:|---:|
| troncs au même endroit | 157 / 378 (41,534 %) | **160 / 378 (42,328 %)** |
| forme identique, sur un vrai tronc | 74 / 155 (47,742 %) | **75 / 158 (47,468 %)** |

L'« avant » de 81 formes identiques **ne se reproduit pas dans cet
environnement** : le code d'origine y donne 74. La baisse apparente vient de là
(paquet de registre régénéré, code fusionné dans `main` depuis la mesure
précédente), pas des placers modifiés, qui gagnent trois troncs et un arbre
juste sur le même échantillon. Le taux de forme bouge d'un cheveu parce que le
dénominateur passe de 155 à 158.

### Hors échantillon, graine 987654321

Même commande avec `--world=run/reference-987654321/world --seed=987654321`,
même échantillon (38 chunks, 364 troncs du jeu), rien retouché entre les deux
graines :

| | avant (ce fichier) | après |
|---|---:|---:|
| **troncs au même endroit** | 67 / 364 (18,407 %) | **164 / 364 (45,055 %)** |
| arbres posés sur un vrai tronc | 58 | 92 |
| dont la forme est identique | 39 (67,241 %) | 42 (45,652 %) |
| blocs de bois identiques | 4158 (23,164 %) | **8876 (49,448 %)** |
| troncs du jeu sans rien de nous, `dark_forest` | 242 | 145 |

C'est l'effet le plus visible de ce travail sur une carte : les forêts noires
avaient **zéro** arbre, faute des deux champignons géants que leur sélecteur
nomme. Le nombre de troncs au bon endroit fait plus que doubler, les blocs de
bois identiques aussi.

Le taux de forme baisse parce que le dénominateur change de nature : 34 des 92
arbres posés sur un vrai tronc sont maintenant des arbres de forêt noire, et
`dark_forest_vegetation` n'en fait que **4 sur 34** de la bonne forme. La forêt
noire est la plus dense du jeu (16 essais par chunk, chêne noir à double tronc) :
c'est là que l'ordre dans lequel les voisins poussent décide le plus, et c'est
aussi là que les 145 troncs manquants restent. **Non résolu** ; la sonde, qui
isole un arbre par chunk, donne 38,4 % d'arbres entiers justes au chêne noir et
des exemples « faux » identiques à l'arbre près, ce qui désigne l'interaction
entre voisins plutôt que le placer.

Lu directement sur le monde de référence
(`--show=3 --only=dark_forest_vegetation --matched`), trois arbres « faux » :

* **deux sont identiques au jeu dans toutes les cellules de notre arbre.** Ce
  qui les rend « faux » pour l'outil, ce sont des feuilles et des troncs
  d'arbres **voisins** qui tombent dans la boîte englobante du nôtre. L'un des
  deux porte en plus trois feuilles de trop sur une rangée basse, que le jeu
  n'a pas posées ;
* **le troisième est un autre arbre** : le tronc du jeu est une colonne plus
  loin, un voisin a pris la place avant.

Le chiffre de forme en forêt noire mesure donc surtout l'ordre de pousse entre
voisins dans la forêt la plus dense du jeu, pas le placer du chêne noir. C'est
lu sur trois exemples, pas compté sur l'échantillon, et dit comme tel.

## Les champignons géants

`huge_brown_mushroom` : chapeau plat de rayon 3, coins coupés.
`huge_red_mushroom` : trois anneaux creux et un carré plein. Hauteur
`nextInt(3) + 4`, doublée une fois sur douze — deux tirages toujours. Le test de
dégagement du jeu est appelé avec une hauteur de −1, si bien que pour le rouge
seule la colonne du pied est vérifiée.

Monde `probe-f2-mush`, graine 1234, 200 chunks :

| bloc | jeu | nous | identiques |
|---|---:|---:|---:|
| `red_mushroom_block` | 4302 | 4302 | 95,6 % |
| `brown_mushroom_block` | 4171 | 4183 | 94,7 % |
| `mushroom_stem` | 1021 | 1021 | 93,5 % |
| **tout** | 9494 | 9506 | **94,986 %** |

80,833 % des chunks touchés sont identiques bloc pour bloc. Les écarts lus sont
des chapeaux voisins qui se recouvrent (l'ordre de décoration des chunks) et
un champignon rouge que nous faisons pousser là où le jeu ne l'a pas fait.

C'est ce qui débloque `dark_forest_vegetation` — donc les arbres de toutes les
forêts noires, 242 troncs manquants à la graine 987654321 — et
`mushroom_island_vegetation`.

## Les grottes : géode et spéléothèmes

Monde `probe-f2-cave`, graine 1234, les quatre placed features de vanilla
(`amethyst_geode`, `dripstone_cluster`, `large_dripstone`, `pointed_dripstone`)
aux index 0 à 3 de l'étape 9, contre le témoin `probe-f2-control`, 200 chunks :

| bloc | jeu | nous | identiques | témoin absurde |
|---|---:|---:|---:|---:|
| `smooth_basalt` | 3685 | 3685 | **100,0 %** | 0,0 % |
| `calcite` | 2836 | 2836 | **100,0 %** | 0,0 % |
| `amethyst_block` | 2166 | 2166 | **100,0 %** | 0,0 % |
| `budding_amethyst` | 215 | 215 | **100,0 %** | 0,0 % |
| les quatre bourgeons | 40 | 40 | **100,0 %** | 0,0 % |
| `air` (creusé) | 3536 | 3536 | 99,8 % | 0,0 % |
| `dripstone_block` | 73 058 | 70 327 | 56,3 % | 24,8 % |
| `pointed_dripstone` | 13 146 | 14 723 | 40,7 % | 10,1 % |
| **tout** | 98 875 | 97 851 | **59,763 %** | 19,761 % |

**La géode est exacte**, bloc pour bloc, couche par couche, bourgeons compris,
et le témoin décalé d'un index tombe à zéro sur chacun de ses blocs. C'est aussi
la preuve que le `NormalNoise` sur générateur *legacy* (graine du monde,
fabrique positionnelle tirée d'un `nextLong`, octave semée par le hachage Java
de `octave_-4`) est juste au bit : une erreur de bruit déplacerait la frontière
entre calcite et basalte, qui est à 100 %.

**Les spéléothèmes ne le sont pas.** 56,3 % des blocs de dripstone et 40,7 % des
pointes sont à leur place, contre 24,8 % et 10,1 % pour le témoin : c'est au
dessus du hasard et loin d'être juste. Le témoin est haut parce que les amas
couvrent une grande part de chaque chunk et se recouvrent par coïncidence ;
c'est le plancher contre lequel lire le chiffre, pas zéro. Les trois features
partagent le même monde sonde, donc cette mesure **ne dit pas laquelle** est
fausse ; un monde par feature la départagerait. **Non résolu.**

## Les plaines : `BIOME_INFO_NOISE` et les fleurs

Monde `probe-f2-plains`, graine 1234, les placed features de vanilla
`patch_grass_plain` et `flower_plains` aux index 0 et 1 de l'étape 9, contre le
témoin `probe-f2-control`, 200 chunks. Le nombre de touffes d'herbe par chunk
(5 ou 10) sort de `noise_threshold_count`, donc de `BIOME_INFO_NOISE` ; l'espèce
de chaque fleur sort de `noise_threshold_provider`.

| bloc | jeu | nous | identiques | témoin absurde |
|---|---:|---:|---:|---:|
| `grass` | 5598 | 5554 | **92,4 %** | 20,6 % |
| `dandelion` | 102 | 103 | 69,6 % | 0,0 % |
| `cornflower` | 14 | 15 | **100,0 %** | 0,0 % |
| `poppy` | 13 | 16 | 84,6 % | 0,0 % |
| `oxeye_daisy` | 8 | 11 | 62,5 % | 0,0 % |
| `azure_bluet` | 12 | 4 | 33,3 % | 0,0 % |
| **tout** | 5747 | 5703 | **91,822 %** | 20,045 % |

**Le bruit est juste.** Les totaux concordent à 1 % près (5747 contre 5703) : un
`BIOME_INFO_NOISE` faux ferait basculer des chunks entiers entre 5 et 10 essais
et le total s'écarterait d'autant. Les fleurs, dont l'espèce dépend d'un second
bruit, sont à 0 % pour le témoin sur chaque espèce et bien au-dessus avec la
bonne graine — le bleuet à 100 %.

Ce qui reste — 8 % de l'herbe, un tiers des fleurs — n'est pas départagé : deux
patches voisins se disputent les mêmes cellules et l'ordre de décoration des
chunks décide qui gagne ; c'est l'hypothèse la plus probable et elle **n'est
pas mesurée**. Le témoin de l'herbe est haut pour la même raison que celui du
dripstone : des touffes denses se recouvrent par hasard.

## L'océan : la mesure n'a pas eu lieu

La famille marine (`seagrass`, `kelp`, `sea_pickle`, les trois coraux,
`underwater_magma`) est écrite et **n'est pas mesurée**. La tentative a raté,
et la raison vaut d'être écrite.

Une sonde marine demande une zone de mer : le preset `plains` ne change pas le
terrain, donc une plaine reste une plaine et un océan reste un océan. La zone a
été choisie dans le monde de référence (région `r.-205.13`, biomes
`lukewarm_ocean`), mais **à partir du premier chunk océanique rencontré**, en
z = 437, avec une zone qui commençait en z = 420 : elle frôlait l'océan par un
coin et couvrait surtout de la terre et des grottes. Résultat, sur 200 chunks
finis : le jeu n'a posé **aucune** herbe marine, aucun cornichon, aucun varech,
aucun corail, et nous non plus — trois blocs de magma de part et d'autre, à
l'identique. Un score de 100 % sur trois blocs ne dit rien, et il n'est pas
retenu.

Deux leçons, payées là :

* **Un recensement « chunk avec de l'eau sous le niveau de la mer » ne
  distingue pas la mer d'un aquifère.** Il comptait 105 chunks « mouillés » dans
  une zone sans mer. Il faut lire le biome de la référence, *et* son étendue —
  la bande océanique de `r.-205.13` va de x −6565 à −6553 et de z 435 à 447.
* **La sonde bloc à bloc tronquait sa liste de chunks candidats** à quatre fois
  le nombre demandé, dans l'ordre des régions. Loin du point d'apparition, des
  régions entières de chunks de bord inachevés passent avant la zone sondée.
  Corrigé dans `ov_features` : en mode `--control`, la liste n'est plus
  tronquée. Les mesures des grottes, des champignons et des plaines n'en
  dépendaient pas — leurs mondes sont décorés partout et les chunks comparés
  étaient des chunks finis près de l'apparition.

## Les prédicats et fournisseurs débloqués

* **`replaceable`** se lit dans le tag `#minecraft:replaceable`, que 1.20.1
  exporte et qui liste exactement les blocs portant la propriété.
* **`solid`** est `BlockState.isSolid()` : une forme de collision dont la boîte
  englobante fait en moyenne au moins 0,7291666 de bloc, ou un bloc de haut.
  Les surcharges par bloc (`forceSolidOn` / `forceSolidOff`) ne sont dans
  aucun rapport et **ne sont pas modélisées** — nommé.
* **`matching_fluids`** interroge l'état de *fluide*, pas le bloc : `water` est
  la source (niveau 0 ou bloc waterlogged), `flowing_water` tout autre niveau,
  `empty` l'absence de fluide. L'ancienne lecture « fluide = bloc » refusait
  `flowing_water` (la canne à sucre) et `empty` (le melon), et prenait l'eau
  courante pour une source.
* **Règle de survie ajoutée** : `small_dripleaf` (tag
  `#small_dripleaf_placeable`, ou source d'eau ici et terre dessous). Elle ne lit
  que des blocs, comme le reste de la table.
* **Les deux champignons restent refusés.** Leur règle lit le niveau de
  lumière, que la génération ne calcule pas. Une règle « supposée sombre » a
  été écrite, puis **retirée** : aucun monde sonde n'a pu la mesurer (la file
  du verrou JVM partagé est restée bloquée plus de deux heures), et le test
  existant qui épingle ce refus est une décision du projet, pas un détail à
  assouplir. `patch_brown_mushroom` et `patch_red_mushroom` restent donc
  nommés et non construits.
* **`BIOME_INFO_NOISE`** : un `PerlinSimplexNoise` sur un générateur *legacy*
  de graine 2345, octave unique 0 — donc un bruit simplex 2D. Il décide combien
  d'herbes et de fleurs par chunk dans les plaines (`noise_threshold_count`) et
  combien de varech et de coraux dans les océans chauds (`noise_based_count`).
* **Les fournisseurs à bruit** lisent un `NormalNoise` construit sur un
  générateur *legacy* semé par leur propre champ `seed` : chaque pile de Perlin
  prend une fabrique positionnelle d'un `nextLong`, chaque octave est semée par
  le hachage Java de `octave_<n>`. C'est ce qui choisit les tulipes des
  plaines et les bandes de la forêt de fleurs.

## Refusés, nommés, et pourquoi

| type | raison |
|---|---|
| `monster_room` | le générateur d'araignées et les coffres sont des *entités de bloc* ; `FeatureLevel` n'écrit que des états |
| `desert_well` | laissé au mandat des structures à gabarits |
| `bonus_chest` | placé par la logique d'apparition selon une option du monde, contient une table de butin |
| `freeze_top_layer` | lit la température de biome avec son ajustement en altitude et le modificateur « frozen » — deux `PerlinSimplexNoise` non construits |
| `multiface_growth` de `sculk_vein` | demande le propagateur de sculk, une autre machine que celui du lichen |
| `sculk_patch`, `iceberg`, `blue_ice`, `fossil` | non écrits dans ce mandat |
| `seagrass_simple` | sa placement demande `carving_mask`, les masques des carvers |
| le Nether et l'End | laissés au mandat Nether |

## Pièges pour les autres agents

* **Le paquet de registre partagé peut changer sous vos pieds.**
  `data/vanilla/1.20.1/registry.ovpack` est un lien vers le fichier du dépôt
  principal ; un autre agent l'a régénéré avec un format que ce worktree ne
  lit pas (780 312 octets contre 729 688), et chaque outil a échoué d'un coup
  avec « run tools/ov_datagen first ». Remède : supprimer **le lien** (pas la
  cible) et lancer `python3 tools/ov_datagen/ovpack.py` — il n'écrit qu'un
  fichier, sous la racine du worktree.
* **Un arbre voisin rend « faux » un arbre juste.** Sur une sonde à un arbre
  par chunk, les grands arbres débordent ; le score par arbre compte les
  feuilles du voisin. Lire les exemples avant de chercher une règle.
* **Un fluide qui s'est posé autrement n'est pas une différence de feature.**
  Voir « Le piège du fluide » plus haut.
* **Un tag est un ensemble pour `BlockTags`, une liste pour le jeu.** Les
  coraux tirent un indice dans le *contenu* d'un tag : l'ordre du fichier fait
  partie de la graine. `ocean_feature.cpp` relit ces trois tags dans l'ordre.
* **Un `HashSet<BlockPos>` parcouru avec un tirage par élément** — les patches
  de végétation comme les décorateurs d'arbre — impose `java_hash_order`.
