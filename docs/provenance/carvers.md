# Carvers — grottes et ravins

Ce document trace ce qui a été établi pour `src/ov_worldgen/{carver,carving_mask}.{hpp,cpp}`,
comment ça a été établi, et les pièges rencontrés.

Le résultat court, d'abord, parce que c'est lui qui décide si le reste vaut la peine d'être lu :

> **1 200 chunks du monde de référence, 1 615 858 cellules creusées : le masque de carving que
> nous produisons est identique bit pour bit à celui que le jeu a écrit. 1 200 / 1 200 chunks
> exacts, zéro cellule en trop, zéro cellule manquante.**

---

## 1. L'oracle : le masque sauvegardé, pas les blocs

Un carver est presque entièrement du RNG. Il n'y a pas de champ de bruit à approcher, pas
d'interpolation qui lisse une petite erreur : un tunnel est une chaîne d'ellipsoïdes dont chaque
centre, chaque rayon et chaque virage sortent d'un seul générateur, dans un seul ordre. Un tirage
de trop, ou un de moins, et les grottes restent des grottes — bon nombre, bonne taille, bonne
profondeur — et pas une seule n'est là où le jeu l'a mise. **Cet échec ressemble à un succès dans
toutes les mesures sauf la seule qui compte**, et il fait *baisser* la parité de terrain, puisqu'on
creuse de la pierre que le jeu a gardée en plus de ne pas creuser celle qu'il a enlevée.

Comparer des blocs ne répond donc pas à la question. Un monde de référence le fait :

> Un chunk que le jeu a généré mais n'a pas amené jusqu'à `minecraft:full` conserve
> `CarvingMasks: {AIR: [L;…]}` — l'ensemble exact des cellules que ses carvers ont marquées.

C'est une vérité de terrain au bit près, obtenue sans avoir besoin que quoi que ce soit d'autre
fonctionne : le masque ne dépend **que** de la graine, du chunk et de la hauteur du monde. Ni du
terrain, ni du biome, ni de l'aquifère.

`tools/ov_carveparity` lit ces chunks et compare. Sur `run/reference-1234567890` :

| chunks comparés | cellules du jeu | cellules à nous | communes | à nous seules | au jeu seules | chunks exacts |
|---|---|---|---|---|---|---|
| 60 | 60 145 | 60 145 | 60 145 | 0 | 0 | 60 / 60 (100 %) |
| 600 | 880 557 | 880 557 | 880 557 | 0 | 0 | 600 / 600 (100 %) |
| 1 200 | 1 615 858 | 1 615 858 | 1 615 858 | 0 | 0 | 1 200 / 1 200 (100 %) |

Note d'usage : la règle habituelle « ne comparer que les chunks `minecraft:full` » s'inverse ici,
et pour la même raison de fond. Un chunk `full` a **perdu** son masque (il est jeté au passage en
`LevelChunk`) ; ce sont les chunks inachevés qui portent la preuve. Sur les 20 premières régions du
monde de référence, 578 chunks portent un masque `AIR` non vide.

### Disposition des bits

Le tableau est un `LongArray` NBT, pas un tableau d'octets — l'erreur coûte une heure si on lit
`len()` comme un nombre d'octets et qu'on conclut que les grottes s'arrêtent à y = −51.

```
index = (x & 15) | ((z & 15) << 4) | ((y - min_y) << 8)
```

bits empaquetés dans des mots de 64 bits, **bit de poids faible d'abord** (`BitSet.toLongArray`),
les mots nuls de queue supprimés. La disposition a été retrouvée par force brute : parmi les six
ordres de champs possibles, celui-ci est le seul qui donne 99,7 % de cellules ayant au moins un
voisin en 6-connexité, et une plage de `y` (−57 .. 41) qui correspond aux plages configurées des
carvers. Les autres dispersent les points.

---

## 2. Ce qui a servi de spécification

| Fait | Source |
|---|---|
| Probabilités, plages de `y`, multiplicateurs de rayon, `floor_level`, `yScale`, `lava_level`, forme du ravin | `data/vanilla/1.20.1/generated/data/minecraft/worldgen/configured_carver/{cave,cave_extra_underground,canyon}.json` — sortie du data generator, régénérée localement |
| Liste et **ordre** des carvers par biome | les 53 fichiers `worldgen/biome/*.json` de l'overworld, énumérés : tous portent exactement `air: [cave, cave_extra_underground, canyon]` |
| Ce que les carvers ont le droit de remplacer | `tags/blocks/overworld_carver_replaceables.json` (contient `minecraft:water`, ne contient **pas** la lave ni la bedrock) |
| Génération des grottes (structure d'ensemble) | `https://minecraft.wiki/w/Cave` et `https://minecraft.wiki/w/Custom_world_generation#Configured_carver` |
| `java.util.Random` | spécification JDK, déjà implémentée et vérifiée dans `ov_math` |

Le flux de tirages lui-même n'a pas de source documentaire publiée : il a été **reconstruit puis
vérifié contre l'oracle**. C'est la méthode du dépôt — un chiffre non mesuré n'est pas un résultat —
et ici la vérification est totale, pas statistique : 880 557 cellules, aucune divergence. Un flux
qui serait faux d'un seul tirage ne pourrait pas produire ce résultat.

---

## 3. Les pièges, dans l'ordre où ils coûtent cher

### 3.1 Les angles ne viennent pas de libm

Le jeu lit ses sinus dans une table de 65536 flottants :

```
sin(v) = TABLE[(int)(v * 10430.378f) & 0xFFFF]
cos(v) = TABLE[(int)(v * 10430.378f + 16384.0f) & 0xFFFF]
```

L'écart avec `std::sin` est d'environ 5·10⁻⁵ — assez petit pour que le tunnel *ait l'air* juste,
assez grand pour que sur cent pas il dérive de plusieurs dizaines de blocs. C'est le piège le plus
silencieux du fichier : le code compile, les grottes sont belles, et rien ne coïncide.

Corollaire utile : `mth_sin(π/2)` vaut **exactement** 1.0f (l'index tombe pile sur 16384), ce qui
est pourquoi le rayon d'une salle est exactement `1.5 + épaisseur`.

### 3.2 C++ ne séquence pas les opérandes, Java oui

`random.nextFloat() - random.nextFloat()` est du Java parfaitement défini : gauche d'abord. En C++
l'ordre d'évaluation des opérandes de `-`, `*` et `+` n'est **pas** spécifié, et le compilateur a le
droit d'inverser les deux tirages. Toutes les expressions de ce fichier qui tirent plus d'un nombre
sur une ligne ont été découpées en variables nommées. Il y en a huit.

### 3.3 `yScale` du ravin est une constante, pas une uniforme

`canyon.json` écrit `"yScale": 3.0` — un nombre nu, donc un `ConstantFloat`, qui **ne tire rien**.
Le lire comme les autres champs de forme (qui sont des `uniform`) consomme un flottant de plus et
déplace tous les ravins du monde.

### 3.4 Le court-circuit dans le profil de largeur du ravin

Le tableau de facteurs de largeur est construit couche par couche avec

```
si (couche == 0 || next_int(3) == 0) → redessiner le facteur (deux tirages)
```

Java court-circuite : à la couche 0, le `next_int` n'est **pas** tiré. Un tirage de plus ici décale
les 383 couches suivantes.

### 3.5 L'index décalé d'un dans le test de largeur du ravin

Le test de forme d'un ravin utilise `width_factors[y - min_y - 1]`, pas `[y - min_y]`. La couche `y`
est élargie par le facteur tiré pour la couche du dessous. C'est le comportement du jeu et ça fait
partie de la silhouette ; le « corriger » change la forme.

### 3.6 `setLargeFeatureSeed` prend **deux** longs

```
set_seed(graine) ; a = next_long() ; b = next_long()
set_seed(chunk_x * a ^ chunk_z * b ^ graine)
```

Sauter l'un des deux donne une graine par chunk parfaitement correcte, parfaitement déterministe, et
qui ne partage rien avec celle du jeu. Le multiple est signé 64 bits et **déborde** — passer par
`u64` pour que le repli soit défini.

L'index ajouté à la graine (`graine + 0`, `+ 1`, `+ 2`) est la position du carver dans la liste du
biome. Une addition, pas un mélange : deux graines voisines partagent donc un flux de carver, ce qui
est visible si on génère les deux.

### 3.7 Le masque enregistre ce qui a été *considéré*, pas ce qui est devenu de l'air

`carve_ellipsoid` pose le bit avant de décider quoi que ce soit sur le bloc. Un bloc non
remplaçable, ou un aquifère qui refuse, laisse la pierre en place **et** le bit posé. C'est ce qui
est sauvegardé, et c'est pour ça que la séparation masque / blocs n'est pas de la cosmétique : sans
elle, la comparaison ci-dessus serait impossible et le fichier sauvegardé serait faux.

### 3.8 Le voisinage de 8 chunks

Un chunk est creusé par tous les chunks à 8 de distance, lui compris : 17 × 17 × 3 = 867
ensemencements par chunk. Un tunnel court jusqu'à 112 blocs, donc l'essentiel de ce qui est creusé
dans un chunk a commencé ailleurs. Sans la boucle, les grottes s'arrêtent net aux bords de chunk.

`can_reach` élague cette boucle sans rien tirer — c'est ce qui la rend légitime : couper un tunnel
qui ne peut plus revenir ne change aucun bit.

---

## 4. Effet mesuré sur la parité de terrain

`ov_parity --terrain`, graine 1234567890, chunks `minecraft:full` uniquement, 600 chunks,
1 536 000 blocs échantillonnés. Le drapeau `--carvers` donne l'avant et l'après avec le même
binaire, ce qui évite de comparer deux mesures prises dans deux états du dépôt.

| | accord solide/non | pierre en trop | pierre manquante |
|---|---|---|---|
| **avant** (bruit seul) | 97,967 % | 20 073 — **1,307 %** | 11 157 — 0,726 % |
| **après** (bruit + carvers) | **98,789 %** | 6 372 — **0,415 %** | 12 234 — 0,796 % |

- **+0,822 point** d'accord ; le désaccord total passe de 2,033 % à 1,211 %, soit **−40 %**.
- Sur le mode d'erreur que les carvers doivent corriger — « de la pierre là où le jeu a de l'air » —
  ils en récupèrent **13 701 cellules sur 20 073, soit 68,3 %**.
- Par bande de hauteur, l'excédent de pierre est essentiellement éliminé en profondeur :

| y | avant | après |
|---|---|---|
| −64 .. −33 | 2 163 | **86** |
| −32 .. −1 | 2 967 | **475** |
| 0 .. 31 | 5 314 | **449** |
| 32 .. 63 | 5 323 | 1 383 |
| 64 .. 95 | 4 306 | 3 979 |

Ce qui reste au-dessus de y = 64 n'est presque pas touché par les carvers, et c'est attendu : c'est
la surface et les features. Ce qui reste entre 32 et 63 est très majoritairement de l'aquifère.

Sur un échantillon plus court (200 chunks, 512 000 blocs) les mêmes mesures donnent
98,142 % → 99,061 %, excédent 1,315 % → 0,344 %, soit 73,9 % de l'excédent récupéré. Les deux
échantillons ne couvrent pas les mêmes régions ; c'est le second qui fait foi, il est trois fois
plus grand.

### Le coût, nommé

La « pierre manquante » passe de 11 157 à 12 234, soit **+1 077 cellules (+0,070 point)**. Ce n'est
pas une erreur de nos carvers — le masque est exact au bit près, donc chaque cellule creusée est une
cellule que le jeu a creusée aussi. C'est l'**aquifère qui manque** : quand `computeSubstance`
répond « barrière », le jeu pose le bit du masque et **laisse la pierre**. Sans aquifère nous
transformons ces cellules en air. Le chiffre est donc une mesure de ce que l'étage aquifère doit
récupérer, et il est petit devant les 13 701 gagnées.

---

## 5. Ce qui n'est pas fait

- **Le remplissage d'une cellule creusée** est de l'air, ou de la lave à `y ≤ min_y + 8` (= −56).
  Le jeu demande à l'aquifère, qui peut répondre eau ou « ne touche pas ». Cette dépendance est
  laissée au module aquifère ; le point d'accroche est le masque.
- **La règle herbe → terre** (`carveBlock` transforme la terre sous un bloc d'herbe creusé en
  matériau de surface du biome) n'est pas câblée : elle suppose que les surface rules soient passées
  avant les carvers, ce qui est l'ordre du jeu (`noise → surface → carvers → features`) mais pas
  encore le nôtre. Le masque, lui, est déjà correct ; c'est l'application qui attendra.
- **`CarvingStep::Liquid`** n'est jamais peuplé. C'est aussi le cas dans le jeu en 1.20.1 pour
  l'overworld : l'étape existe dans le format et reste vide.
- **Le nether** (`minecraft:nether_cave`) n'est pas implémenté. Sa liste de carvers diffère, son
  `getCaveBound` aussi ; il est refusé par absence, pas traité comme un overworld.
