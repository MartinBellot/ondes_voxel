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
features**, 166 tags de blocs. `FeatureRegistry::unavailable()` et
`Decorator::missing()` listent le reste par nom — `ov_features --missing`.

## L'ordre de décoration

Les onze étapes de `GenerationStep.Decoration` dans l'ordre du jeu, la graine de
chunk (`decoration_seed`), la graine par feature (`decoration + index +
10000·étape`), et le tri partagé qui donne à chaque étape un ordre unique sur
lequel tous les biomes s'accordent.

Le tri est un tri topologique sur la contrainte « dans chaque biome, cette
feature vient avant celle-là ». Deux features qui ne se croisent jamais dans un
biome ne sont pas ordonnées par la contrainte ; l'égalité est tranchée par
l'ordre de lecture des biomes, d'où la lecture des fichiers **triée** et pas dans
l'ordre du système de fichiers. 64 biomes, 190 placed features ordonnées.

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

### Comptes et positions

| minerai | jeu | nous | même bloc |
|---|---:|---:|---:|
| coal_ore | 2261 | 1812 | 7 |
| deepslate_coal_ore | 31 | 33 | 0 |
| copper_ore | 2456 | 1851 | 3 |
| deepslate_copper_ore | 359 | 298 | 0 |
| iron_ore | 2143 | 1748 | 7 |
| deepslate_iron_ore | 1326 | 930 | 4 |
| gold_ore | 174 | 159 | 0 |
| deepslate_gold_ore | 1053 | 818 | 1 |
| redstone_ore | 111 | 87 | 0 |
| deepslate_redstone_ore | 1561 | 1251 | 7 |
| lapis_ore | 403 | 345 | 0 |
| deepslate_lapis_ore | 646 | 572 | 0 |
| diamond_ore | 23 | 14 | 0 |
| deepslate_diamond_ore | 702 | 643 | 1 |
| **total** | **13 249** | **10 561** | **30 (0,226 %)** |

### Distribution par tranche de seize y

Extrait ; le tableau complet sort de `ov_features` :

```
  minecraft:iron_ore                    minecraft:deepslate_diamond_ore
    y  -16 ..   -1     44 /     11        y  -64 ..  -49    257 /    255
    y    0 ..   15    753 /    556        y  -48 ..  -33    156 /    181
    y   16 ..   31    847 /    797        y  -32 ..  -17    182 /    123
    y   32 ..   47    493 /    362        y  -16 ..   -1    103 /     79
    y   48 ..   63      3 /     22        y    0 ..   15      4 /      5
```

**Les formes sont bonnes.** Le fer culmine entre 16 et 31, le diamant s'écrase
contre le fond du monde, et les variantes deepslate n'apparaissent que sous
y = 0. Autrement dit les fournisseurs de hauteur — trapèze, uniforme,
`very_biased_to_bottom` — et les ancres sont lus correctement : un minerai au bon
compte à la mauvaise altitude aurait sauté aux yeux ici, et ce n'est pas le cas.

Le déficit d'environ 20 % en volume s'explique par la mesure elle-même : notre
décoration rejoue aussi `ore_dirt`, `ore_gravel` et les blobs de granite, diorite
et andésite **par-dessus** ceux que le jeu a déjà posés. La terre et le gravier
ne sont pas dans `stone_ore_replaceables`, donc ce doublement de couverture
retire des sites aux minerais métalliques qui viennent après. C'est un artefact
du protocole de mesure, pas un défaut du code des veines.

### Ce qui n'est PAS établi : la graine

**Les positions exactes ne correspondent pas : 30 blocs sur 13 249, soit
0,226 %, c'est-à-dire le bruit de fond.** L'étage produit des veines de la bonne
forme, de la bonne taille et à la bonne altitude, et pas aux bons blocs.

La cause est la dérivation de la graine, et voici ce que la mesure a **éliminé**,
par balayage plutôt que par raisonnement (`ov_features --calibrate`) :

* **l'index de la feature** — balayé de 0 à 400 à l'étape 6 : aucun pic ;
* **l'étape** — balayée de 0 à 10, chacune avec les index 0 à 47 : aucun pic ;
* **la famille de générateur** — Xoroshiro128++ et `java.util.Random`, pour les
  tirages des features comme pour les deux multiplicateurs de la graine de
  chunk, y compris les deux combinaisons croisées : aucun pic ;
* **la forme de `decoration_seed`** — `(x·a + z·b) ^ graine` (retenue),
  `+ graine`, sans XOR, en coordonnées de chunk, sans forcer les multiplicateurs
  impairs : aucun pic ;
* **le décalage brut** — 130 000 décalages consécutifs depuis la graine de chunk,
  ce qui couvre toute combinaison (index, étape) avec index < 10 000 : aucun pic.

La métrique du balayage est « le jeu a-t-il un minerai de cette espèce dans la
boîte que la veine couvre autour de son origine ». Le bruit de fond est de
l'ordre de 10 % ; une graine correcte donnerait 70 % et plus. Le maximum observé
sur 130 000 essais est de 35 %, ce qui est exactement la queue attendue du hasard.

Conclusion honnête : **la graine de décoration est fausse d'une manière qu'aucune
des hypothèses ci-dessus ne couvre**, et le chiffre de parité des minerais est
donc 0,226 % au bloc près, pas davantage. Ce qui reste à trouver est une seule
formule ; tout ce qui est autour d'elle — le pipeline, les distributions, les
veines, l'ordre des étapes, le tri partagé — est en place et mesuré.

Par conséquent l'étage de décoration **n'est pas branché dans `ChunkGenerator`**.
Le brancher mettrait du minerai au mauvais endroit dans tous les mondes générés,
et une génération plausible et fausse est précisément ce que ce dépôt refuse.
L'API (`Decorator::decorate`) est publique et prête ; il manque un chiffre.

## Reproduire

```bash
./build/macos-release/bin/ov_features --missing
./build/macos-release/bin/ov_features --order=underground_ores
./build/macos-release/bin/ov_features --chunks=48
./build/macos-release/bin/ov_features --chunks=4 --calibrate=ore_iron_middle \
    --step=6 --span=130000 --raw --ore=iron_ore
```

`OV_FEATURE_RANDOM=legacy`, `OV_DECORATION_RANDOM=legacy` et
`OV_DECORATION_SEED=plus|chunk|even|bare|swap` rebranchent les variantes
énumérées plus haut, pour que le tableau des éliminations soit à une commande et
non à une relecture.

## Pièges pour les autres agents

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
