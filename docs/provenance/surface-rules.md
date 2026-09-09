# L'interpréteur de `surface_rules`

Ce que fait cet étage : après l'étage de bruit, qui ne pose que de la pierre, de l'eau, de la
lave et de l'air, les **règles de surface** posent l'herbe, la terre, le sable, le gravier, la
neige, la glace, le grès des déserts, les bandes d'argile des badlands et le socle de bedrock.

Comme l'interpréteur de `density_function` (`src/ov_worldgen/src/density.cpp`), c'est un
**interpréteur** : l'arbre de règles est lu depuis `worldgen/noise_settings/<réglage>.json`,
membre `surface_rule`, et rien n'est codé en dur. Un type de règle ou de condition non
implémenté est **refusé et nommé** (`SurfaceError::Unsupported`), jamais traité comme « ne
correspond jamais ».

Fichiers : `src/ov_worldgen/{include/ov/worldgen,src}/surface_rules.{hpp,cpp}` et
`surface_system.{hpp,cpp}` ; harnais `tools/ov_surfparity` ; tests
`src/ov_worldgen/tests/test_surface_rules.cpp`.

---

## 1. Ce qui est couvert

Les **4 types de règles** (`sequence`, `condition`, `block`, `bandlands`) et les **11 types de
conditions** (`biome`, `noise_threshold`, `vertical_gradient`, `y_above`, `water`,
`temperature`, `steep`, `not`, `hole`, `above_preliminary_surface`, `stone_depth`) que la
1.20.1 utilise. C'est l'intégralité de ce qui apparaît dans les sept fichiers de
`noise_settings` : `overworld`, `large_biomes`, `amplified`, `nether`, `end`, `caves`,
`floating_islands`. Un test charge les sept et échoue si un seul type manque — c'est ainsi
qu'une condition oubliée se découvre, plutôt qu'en livrant un monde sans plages.

Compte des nœuds dans l'overworld : 724 `condition`, 523 `block`, 252 `sequence`,
200 `biome`, 176 `noise_threshold`, 122 `stone_depth`, 100 `water`, 63 `y_above`,
31 `not`, 25 `steep`, 18 `hole`, 12 `vertical_gradient`, 10 `bandlands`,
5 `temperature`, 3 `above_preliminary_surface`.

## 2. Méthode : l'oracle est le vrai jeu

`tools/ov_surfparity` compare contre `run/reference-1234567890`, généré par le vrai serveur
1.20.1 à la seed 1234567890.

**La mesure a dû être conçue avant de vouloir dire quoi que ce soit.** Comparer notre
génération complète bloc par bloc n'aurait rien dit de cet étage : l'étage de bruit place sa
surface **1 à 8 blocs plus bas** que le jeu dans neuf colonnes sur dix (mesuré par
`ov_parity --terrain` : seulement 9,22 % des colonnes ont la même hauteur de surface). Chaque
colonne aurait donc été en désaccord, pour une raison qui n'est pas celle qu'on mesure.

Le harnais donne donc aux règles **le terrain du jeu lui-même** : chaque colonne du monde de
référence est ramenée à ce que l'étage de bruit aurait laissé — pierre là où le jeu a du
solide, eau là où il a de l'eau, air là où il a de l'air ou un arbre — puis les règles sont
exécutées dessus. Ce qui revient appartient à cet étage et à rien d'autre.

Seuls les chunks au statut `minecraft:full` sont lus. Un chunk arrêté à un statut partiel a
un tableau de biomes qui vaut `plains` par défaut ; le lire fait dire à chaque désaccord
« le jeu a choisi plains ». `tools/ov_parity` filtre déjà ainsi et ce harnais fait pareil.

## 3. Les chiffres

250 chunks `minecraft:full`, 25 biomes, les 8 couches sous la surface de chaque colonne,
seed 1234567890.

| | avant (aucune règle) | après |
|---|---|---|
| colonnes dont les 8 couches concordent | 2 602 / 64 000 — **4,07 %** | 58 378 / 64 000 — **91,22 %** |
| blocs individuels | 253 713 / 390 812 — **64,92 %** | 429 195 / 439 432 — **97,67 %** |
| plancher de bedrock (y −64..−60) | 127 906 / 320 000 — **39,97 %** | 320 000 / 320 000 — **100,000 %** |

Le « avant » n'est pas zéro parce qu'une colonne de pierre nue concorde partout où le jeu a
laissé de la pierre ; il est mesuré (`ov_surfparity --no-rules`), pas supposé.

**Par profondeur sous la surface** (après) : −0 : 95,80 % · −1 : 96,21 % · −2 : 97,89 % ·
−3 : 98,26 % · −4 : 98,48 % · −5 : 98,71 % · −6 : 98,36 % · −7 : 97,93 %.

**Par biome, les extrêmes** :

| biome | concordance |
|---|---|
| `deep_cold_ocean` | 99,73 % |
| `savanna_plateau` | 99,58 % |
| `wooded_badlands` | 99,56 % |
| `old_growth_birch_forest` | 99,45 % |
| `forest` | 98,55 % |
| `plains` | 98,35 % |
| `desert` | 95,72 % |
| `beach` | 94,42 % |
| `bamboo_jungle` | 92,52 % |
| `badlands` | 86,67 % *(240 blocs seulement)* |
| **`frozen_ocean`** | **65,06 %** |

`frozen_ocean` est le seul biome nettement à l'écart, et l'écart est **expliqué et attendu** :
voir § 5.

## 4. Ce qui a été établi par la mesure, et non déduit

Trois valeurs de cet étage n'étaient pas déductibles du schéma JSON. Chacune a été tranchée
en comparant deux implémentations contre le monde de référence — c'est la méthode du dépôt,
et chaque interrupteur reste dans le code (variable d'environnement) pour que la mesure soit
rejouable.

### 4.1 La profondeur de surface porte bien un tirage aléatoire

`surface_depth = (int)(bruit_surface(x,0,z) × 2,75 + 3,0 + rng.next_double() × 0,25)`, avec
`rng = factory.at(x, 0, z)` sur la **factory positionnelle racine** — la même que celle dont
le routeur de densité tire ses bruits nommés.

Une colonne d'herbe finie par le jeu annonce sa propre profondeur : de l'herbe puis exactement
`surface_depth` blocs de terre, parce que la règle du sol est
`stone_depth(floor, add_surface_depth)`. Compter la terre donne donc le nombre que le jeu a
calculé. Sur 27 167 colonnes d'herbe :

| | écart exact (+0) | écart −1 |
|---|---|---|
| avec le terme aléatoire | **95,07 %** | 0,70 % |
| sans (`OV_SURFACE_DEPTH_JITTER=0`) | 83,21 % | **12,62 %** |

L'erreur sans le terme se concentre exactement à −1, ce qu'un terme manquant dans `[0 ; 0,25)`
produit. Le terme existe, et il vient de cette factory-là.

### 4.2 `stone_depth(ceiling)` mesure bien la coulée de pierre en dessous

`ceiling` utilise `stone_depth_below` = nombre de blocs de pierre consécutifs depuis le bas de
la coulée jusqu'à la position incluse (le bloc le plus bas d'une masse de pierre vaut 1).
Comparé à l'alternative « toujours vrai » (`OV_CEILING_FROM_RUN=0`), sur 64 chunks :

| | colonnes concordantes |
|---|---|
| profondeur calculée depuis la coulée | **89,94 %** |
| toujours satisfaite | 59,93 % |

### 4.3 Le plancher de bedrock est exact au bloc près

`vertical_gradient` : vrai au-dessous de l'ancre basse, faux au-dessus de l'ancre haute, et
entre les deux un tirage `factory.from_hash_of(nom).fork_positional().at(x,y,z).next_float()`
comparé à l'interpolation linéaire des deux ancres. Le plancher `minecraft:bedrock_floor` va
de `above_bottom 0` (y = −64) à `above_bottom 5` (y = −59).

**320 000 blocs sur 320 000, soit 100,000 %**, sur les cinq couches, sans exception. C'est le
contrôle le plus net de tout l'étage : il ne dépend d'aucun bruit, seulement du tirage
positionnel, du double fork (`from_hash_of` puis `fork_positional`) et de la résolution des
ancres. Une erreur d'un seul niveau de fork le ramènerait à ~50 %.

### 4.4 Les bandes d'argile des badlands sont la bonne table

192 bandes, tirées dans cet ordre : orange par pas de `nextInt(5)+1`, puis trois passes
`makeBands` (jaune longueur ≥ 1, marron ≥ 2, rouge ≥ 1, chacune `nextIntBetweenInclusive(6,15)`
séries), puis `nextIntBetweenInclusive(9,15)` bandes blanches par pas de `nextInt(16)+4`, avec
deux tirages `nextBoolean()` pour les épaules gris clair — **tirés même quand l'écriture est
hors table**, car l'état est consommé de toute façon. Générateur :
`factory.from_hash_of("minecraft:clay_bands")`.

Preuve : `wooded_badlands` à **99,56 %** (8 156 / 8 192), et **100,00 %** sur un échantillon
antérieur de 2 048 blocs. Un ordre de tirage faux donnerait des bandes de couleurs
plausibles et une concordance de l'ordre de 20 %.

Le décalage vertical est `round(bruit_clay_bands_offset(x,0,z) × 4)`, et l'index est
`((y + offset) % 192 + 192) % 192` — le double modulo n'est pas décoratif : y est négatif sur
les deux tiers du monde et C++ laisse le signe du modulo au dividende.

### 4.5 La surface préliminaire

`above_preliminary_surface` teste `y >= niveau`, où le niveau est le plus haut y de la grille
de cellules (pas de 8) dont `initial_density_without_jaggedness` dépasse **0,390625** (= 25/64),
évalué au coin de quart (`(x>>2)<<2`). Non mesuré isolément — il n'a pas d'oracle direct — mais
un remplacement grossier par le terrain du monde de référence
(`ov_surfparity --prelim-reference`) fait **baisser** la concordance de 92,5 % à 85,9 %, ce qui
indique que la version issue de notre graphe de densité est la meilleure des deux disponibles.

## 5. Ce qui n'est PAS fait, nommé

1. **Les icebergs des océans gelés.** `frozenOceanExtension` n'est pas une règle : c'est une
   passe séparée du même étage, qui empile `packed_ice`, `blue_ice` et `snow_block` en piliers.
   Les bruits `iceberg_pillar`, `iceberg_pillar_roof` et `iceberg_surface` existent dans les
   données et ne sont pas chargés. **Coût mesuré** : `frozen_ocean` à 65,06 % contre 97 % et
   plus pour les autres océans ; en désaccords, `packed_ice → stone` (538),
   `packed_ice → dirt` (524), `packed_ice → grass_block` (196).
2. **Les piliers des badlands érodés.** `erodedBadlandsExtension`, même remarque ; les bruits
   `badlands_pillar`, `badlands_pillar_roof` et `badlands_surface` ne sont pas chargés.
   `eroded_badlands` n'apparaît pas dans l'échantillon de 250 chunks, donc le coût n'est
   **pas mesuré** ; `badlands` est à 86,67 % sur 240 blocs seulement, ce qui est trop peu pour
   conclure.
3. **Le flou de `BiomeManager`.** Les biomes sont stockés par cellule de 4×4×4 ; le jeu ne lit
   pas ce tableau directement, il décale la lecture d'un offset pseudo-aléatoire allant
   jusqu'à environ une demi-cellule (`FuzzyOffsetBiomeZoomer`), ensemencé par
   `sha256(seed)`. Ce harnais et cet étage lisent la cellule brute. **Coût mesuré** : sur
   10 237 blocs erronés, **2 347 (22,9 %)** sont adjacents à une cellule de biome différente et
   donc atteignables par ce décalage. C'est ce qui explique les colonnes de désert traversées
   de bandes de terracotta. Le correctif appartient à la source de biomes, pas ici, et exige
   une implémentation de SHA-256 que le dépôt n'a pas encore.
4. **L'ajustement de température en altitude.** `temperature` teste `température < 0,15`. La
   vraie fonction retire, au-dessus de y = 80, un terme tiré d'un bruit simplex statique
   ensemencé à 1234, et applique le modificateur `frozen` des océans gelés. Ni l'un ni l'autre
   n'est reproduit ; le dépôt n'a pas de bruit simplex. La condition n'apparaît **qu'une fois**
   dans l'overworld, et les biomes qui montent au-dessus de y = 80 ont une température de base
   assez éloignée de 0,15 pour que l'ajustement (≈ 0,15 à y = 200) ne change pas le verdict —
   raisonnement, pas mesure : c'est dit ici pour que ce soit sachant.
5. **`interpolated` reste ponctuel** dans la surface préliminaire, avec la même réserve que
   dans `density.cpp`.

## 6. Les pièges rencontrés

Ils ont tous coûté un chiffre faux avant d'être trouvés, et ils sont valables pour quiconque
écrit un comparateur sur ce monde.

1. **`WORLD_SURFACE_WG` inclut l'eau.** La marche descend depuis le plus haut bloc **non-air**,
   pas depuis le plus haut bloc solide. Partir du fond marin fait que l'eau n'est jamais vue,
   `water_height` reste à « aucune eau », chaque fond d'océan prend la branche terrestre et le
   gravier du jeu revient en herbe. Coût : `cold_ocean` à **30 %** au lieu de 98 %.
2. **Une algue n'est pas de l'air.** Retirer un `seagrass` ou un `kelp` d'une colonne de
   référence doit laisser de l'**eau**. Le ramener à de l'air coupe la colonne d'eau au même
   endroit et produit exactement le bug précédent, en plus localisé. Coût :
   `lukewarm_ocean` à **81 %** au lieu de 98,4 %.
3. **`cave_air` et `void_air` sont de l'air.** Comparer les noms bruts fait diverger 117 cellules
   sur 16 016 à propos d'un bloc qui n'est pas là.
4. **La glace en surface d'une rivière gelée vient de `freeze_top_layer`**, une *feature*, pas
   des règles : dans le monde que les règles ont vu, c'était de l'eau. La lire comme du solide
   déplace la surface de chaque rivière gelée. Coût : 605 désaccords à elle seule.
5. **Le podzol des jungles de bambou vient de la végétation**, pas des règles : le fichier de
   règles ne nomme le podzol que pour `old_growth_pine_taiga` et `old_growth_spruce_taiga`.
   Lu dans les données, pas supposé.
6. **`ore_dirt` et `ore_gravel` sont des features d'minerai** : elles posent des amas de terre
   et de gravier dans la pierre profonde, après cet étage. De la terre là où nous avons laissé
   de la pierre, à trois blocs sous la surface, n'est pas forcément une erreur de profondeur.
7. **Un chunk de région n'est pas un échantillon de monde.** Prendre 40 chunks dans le premier
   fichier de région mesure trois biomes ; les répartir sur les 118 régions en mesure 25.

## 7. Attributions du harnais

18 177 blocs sur 250 chunks sont comptés à part parce qu'un étage *postérieur* les a posés là
où nos règles ont correctement laissé autre chose : minerais et variantes de pierre
(andesite 4 446, granite 4 436, diorite 3 490, coal_ore 735) sur notre pierre ; disques de
sable et de gravier des berges (1 585 + 477) sur notre terre ; amas `ore_dirt` / `ore_gravel`
(1 228 + 1 008) sur notre pierre.

Ce sont des **attributions, pas des preuves** : un vrai bug peut se cacher derrière chacune.
Elles sont nommées, comptées et affichées séparément plutôt que fondues dans le total, et le
harnais affiche les deux nombres.

## 8. Sources

- **Schéma des règles de surface** (types de règles, de conditions, noms et sémantique des
  champs `surface_type`, `add_surface_depth`, `secondary_depth_range`, `add_stone_depth`,
  `surface_depth_multiplier`, ancres `absolute` / `above_bottom` / `below_top`) :
  documentation de la génération de monde personnalisée de `minecraft.wiki`, plus les données
  du data generator elles-mêmes, qui sont l'autorité sur ce qui existe réellement en 1.20.1.
- **Toutes les constantes numériques** (2,75 · 3,0 · 0,25 · 0,390625 · 0,15 · 192 bandes ·
  l'ordre des tirages · le double fork des générateurs positionnels) : **mesurées contre le
  monde de référence**, selon la méthode du § 4. Aucune ne vient de code décompilé.
- **Générateurs** : `ov_math` (Xoroshiro128++, factory positionnelle, MD5 des noms), déjà
  vérifiés contre des vecteurs de référence externes — voir `docs/PROVENANCE.md`.

## 9. Rejouer les mesures

```bash
cmake --build build/macos-debug --parallel 2 --target ov_surfparity

# le chiffre principal
./build/macos-debug/bin/ov_surfparity --chunks=250 --per-region=4

# la ligne de base, sans les règles
./build/macos-debug/bin/ov_surfparity --chunks=250 --per-region=4 --no-rules

# un biome, avec des colonnes en désaccord affichées en entier
./build/macos-debug/bin/ov_surfparity --chunks=250 --per-region=4 \
    --biome=minecraft:desert --show=4

# les deux mesures du § 4
OV_SURFACE_DEPTH_JITTER=0 ./build/macos-debug/bin/ov_surfparity --chunks=250 --per-region=4
OV_CEILING_FROM_RUN=0     ./build/macos-debug/bin/ov_surfparity --chunks=250 --per-region=4
```
