# Les aquifères : l'oracle, la moitié qui se mesure, et celle qui ne se mesure pas

*Seed 1234567890, monde de référence `run/reference-1234567890`.*

Ce document ne rapporte **aucun changement de génération**. Les quatre chiffres de parité sont
identiques avant et après, au bloc près, et c'est dit en § 7 plutôt que caché.

Ce qu'il rapporte est autre chose : **un oracle qui n'existait pas**, et ce qu'il dit.
`docs/provenance/etage-de-bruit.md` § 5 refusait l'aquifère parce que « la moitié barrière n'est
documentée nulle part où ce projet ait le droit de lire ». C'est toujours vrai. Mais une fonction
non documentée n'est pas une fonction non mesurable, et il se trouve que le jeu a écrit sur le
disque, dans le même fichier, **la question et sa réponse**.

---

## 1. L'oracle : le masque et les blocs du même chunk

Un chunk que le jeu a arrêté à `minecraft:carvers` porte les deux moitiés de la preuve à la fois :

- `CarvingMasks: {AIR: [L;…]}` — l'ensemble exact des cellules que ses carvers ont **considérées** ;
- son tableau de blocs, qui dit ce qu'elles **contiennent** une fois le creusement fini.

La différence entre les deux **est** la réponse de l'aquifère, cellule par cellule. Une cellule
marquée qui finit en eau est une cellule où `computeSubstance` a répondu « fluide » ; une qui finit
en pierre est la barrière qui a refusé la coupe. Rien de tout cela ne dépend de la justesse de
notre terrain : les deux bras viennent du jeu.

`ov_parity --aquifer` lit ces chunks et croise les deux. Le mode existe pour ça et pour rien
d'autre.

Deux pièges, tous deux payés pendant ce travail :

1. **Les chunks `full` ont jeté leur masque**, et les chunks arrêtés plus tôt que `carvers` n'ont
   pas de creusement dans leurs blocs. Seul `minecraft:carvers` porte les deux. Sur les 118 régions
   du monde de référence, 1 775 chunks conviennent.
2. **Une cellule marquée en plein ciel ne dit rien.** Le carver l'a considérée, il n'y avait rien à
   couper, elle est de l'air — et la compter comme « au-dessus du niveau de fluide » est
   exactement ce qui a fait se contredire la première version de cette mesure dans 17 % des cas.
   Le mode ne garde que les cellules **strictement sous le bloc solide le plus haut de leur
   colonne**. Cela retire 40 % des cellules marquées et rend le reste lisible.

### Ce que l'oracle dit

`ov_parity --aquifer --chunks=4000`, **1 775 chunks**, **1 490 251 cellules souterraines
considérées** :

| ce que le jeu a laissé | cellules | part |
|---|---|---|
| air | 1 031 251 | **69,200 %** |
| eau | 349 558 | **23,456 %** |
| lave | 42 259 | 2,836 % |
| solide (la barrière, plus ce qui n'est pas remplaçable) | 67 183 | **4,508 %** |

Par bande de hauteur :

| bande | air | eau | lave | solide |
|---|---|---|---|---|
| −64 .. −49 | 54 215 | 0 | 39 685 | 5 236 |
| −48 .. −33 | 172 467 | 3 536 | 289 | 2 296 |
| −32 .. −17 | 186 651 | 8 351 | 2 220 | 2 974 |
| −16 .. −1 | 175 735 | 9 997 | 65 | 9 115 |
| 0 .. 15 | 130 863 | 89 481 | 0 | 15 804 |
| 16 .. 31 | 145 441 | 91 721 | 0 | 14 163 |
| 32 .. 47 | 88 705 | 98 793 | 0 | 13 357 |
| 48 .. 63 | 43 937 | 45 128 | 0 | 3 034 |
| 64 .. 79 | 18 703 | 0 | 0 | 59 |
| ≥ 80 | 14 534 | 2 551 | 0 | 1 145 |

Trois faits tombent de ce tableau, et aucun n'était écrit nulle part :

1. **Au-dessus de y = 64 l'aquifère ne fait rien.** Zéro eau, 59 cellules solides sur 18 762.
   L'étage n'existe que sous la surface.
2. **Entre y = 0 et y = 63, le jeu remplit 44,3 % des cellules creusées avec de l'eau** (325 123
   contre 408 946 d'air). Nos deux seules options actuelles — tout à l'air, ou tout à l'eau — ont
   donc un plafond de 55,7 % et 44,3 % respectivement sur cette bande. C'est la mesure qui manquait
   au § 5 de `docs/provenance/ordre-des-etages.md`, où le choix « creuser les fluides » a été fait
   parce qu'il mesurait mieux : il mesure mieux **parce que 56 % est plus grand que 44 %**, et pas
   parce que la règle est bonne.
3. **La lave est en dessous de y = −49 et pratiquement nulle part ailleurs** : 39 685 des 42 259
   cellules de lave sont dans la bande la plus basse.

### Ce qu'une cellule gardée solide est faite de

| bloc | cellules |
|---|---|
| `stone` | 39 231 |
| `deepslate` | 19 225 |
| `bedrock` | 5 080 |
| `sandstone` | 653 |
| `sculk_vein` | 428 |
| `andesite` | 425 |
| `pointed_dripstone` | 358 |

`bedrock` n'est pas de l'aquifère — il n'est pas dans
`minecraft:overworld_carver_replaceables`, et nos carvers l'épargnent déjà. `sculk_vein`,
`pointed_dripstone` et `dripstone_block` sont des features. Le corps du chiffre est
**stone + deepslate = 58 456 cellules, 3,92 % des cellules considérées** : de la pierre parfaitement
remplaçable que le jeu a gardée. C'est la barrière, et c'est la première fois qu'elle est comptée.

---

## 2. La barrière : où elle tire, et pourquoi ça ne suffit pas

Sur 400 chunks, 14 415 cellules marquées gardées solides (bedrock exclu) contre 243 477 creusées.
On regarde ce qu'il y a autour :

| combien des six voisins sont un fluide | gardée solide | creusée |
|---|---|---|
| 0 | 8 382 — 58,15 % | 240 386 — **98,73 %** |
| 1 | 3 444 — 23,89 % | 3 086 — 1,27 % |
| 2 | 1 704 — 11,82 % | 5 — 0,00 % |
| ≥ 3 | 885 — 6,14 % | 0 — 0,00 % |

Et le bloc directement **au-dessus** d'une cellule gardée solide est de l'eau dans **25,07 %** des
cas (3 614 cellules), contre 0 % pour une cellule creusée ; le bloc directement **en dessous** est
de l'air dans exactement les mêmes 3 614 cas.

C'est la signature d'une barrière au sens propre : **un plancher de pierre qui retient une nappe
au-dessus d'une grotte sèche**, et que le jeu refuse de percer. Une cellule creusée n'a
pratiquement jamais de voisin fluide (98,73 % en ont zéro) ; une cellule gardée en a un dans
41,85 % des cas.

**Ce n'est pas une formule, et je ne prétends pas que ça en soit une.** 58 % des cellules gardées
n'ont aucun voisin fluide : ce sont vraisemblablement des barrières entre deux aquifères *secs* de
niveaux différents, et rien dans les blocs sauvegardés ne montre le niveau d'un aquifère sec. Un
label binaire par cellule ne détermine pas une fonction continue de trois niveaux voisins et d'un
bruit. **La moitié barrière reste refusée et nommée.**

---

## 3. La moitié qui se mesure : le niveau de fluide

Une cellule marquée qui contient de l'eau avec, juste au-dessus, une cellule marquée qui contient de
l'air, est une **surface libre**. Sa hauteur + 1 est le niveau de fluide de l'aquifère local. Sur
4 000 chunks : **4 340 surfaces libres dans 4 330 colonnes**.

### 3.1 Les niveaux ne prennent que quelques valeurs, et elles sont sur un réseau

Valeurs observées, toutes : **−26, −23, −20, −17, 14, 16, 17, 20, 23, 26, 48, 56, 63, 88, 96, 103.**

Rangées par bande de 40 blocs **calée sur y = 0** :

| bande | niveaux observés |
|---|---|
| [−40, 0) | −26, −23, −20, **−17** |
| [0, 40) | 14, *16*, **17**, 20, 23, 26 |
| [40, 80) | 48, *56*, 63 |
| [80, 120) | 88, *96*, 103 |

Dans chaque bande les valeurs sont espacées de **3**, et elles sont centrées sur **40k + 20** :
−20 pour k = −1, +20 pour k = 0, +60 pour k = 1, +100 pour k = 2. Écrit autrement :

> **niveau = 40k + 20 + 3j**, avec `k = ⌊y/40⌋`.

200 des 207 cellules mesurées vérifient `(niveau − 40k − 20) ≡ 0 (mod 3)`. Les 7 exceptions sont
en italique ci-dessus — 16, 56, 96 — et ce sont **exactement les multiples de 8** : le plafond
d'une cavité qui tombe sur une frontière de cellule d'interpolation, pris pour une surface libre.
Ce n'est pas une exception à la règle, c'est un défaut du détecteur, et il est nommé plutôt que
lissé.

Histogramme des décalages `niveau − (40k + 20)` sur les 159 cellules dont le niveau n'est pas le
niveau de la mer : `−12 : 3`, `−6 : 28`, `−4 : 7`, `−3 : 45`, `0 : 52`, `+3 : 23`, `+6 : 1`.

### 3.2 Le décalage suit `fluid_level_spread`, lu **à l'indice de cellule**

C'est le résultat de ce document, et il tient à un détail qui n'est écrit nulle part :
`fluid_level_spread` n'est pas échantillonné à une position en blocs. Il est échantillonné aux
**coordonnées de la grille d'aquifère** — `(gx, gy, gz)`, trois petits entiers.

Corrélation du décalage avec chacun des quatre bruits d'aquifère, n = 159 :

| bruit | à l'indice de cellule | au centre de la cellule en blocs | au coin de la cellule en blocs |
|---|---|---|---|
| **`fluid_level_spread`** | **+0,8523** | +0,0643 | −0,1805 |
| `fluid_level_floodedness` | −0,1127 | +0,1019 | +0,0757 |
| `lava` | −0,0155 | +0,0274 | +0,0074 |
| `barrier` | +0,1171 | −0,0797 | −0,0745 |

Et la loi elle-même. On ajuste `décalage = 3 × ⌊S × spread⌋` et on balaie `S` :

| source | meilleur `S` | accord |
|---|---|---|
| **`spread` à l'indice de cellule (candidat)** | **3,20** | **142 / 159 — 89,31 %** |
| `spread` au centre en blocs (témoin) | 1,47 | 50 / 159 — 31,45 % |
| `spread` au coin en blocs (témoin) | 0,01 | 50 / 159 — 31,45 % |
| `floodedness` à l'indice (témoin) | 3,22 | 47 / 159 — 29,56 % |
| `barrier` à l'indice (témoin) | 0,01 | 52 / 159 — 32,70 % |
| taux de base — toujours le décalage modal | — | **32,70 %** |

**Les témoins sont la partie importante.** Ce dépôt a déjà publié une conclusion fausse tirée d'une
corrélation sans témoin (`docs/provenance/amplitude-old-blended-noise.md` § 2), et le critère
utilisé ici est construit pour ne pas répéter l'erreur : les trois témoins sont **le même champ lu
au mauvais endroit** et **les autres champs lus au bon endroit**, et tous les quatre tombent
exactement sur le taux de base. Le candidat est à 89,31 %, soit 2,7 fois le hasard, et son
maximum en `S` est **intérieur** et net :

| `S` | 2,5 | 2,8 | 3,0 | **3,2** | **3,3** | 3,5 | 4,0 | 4,5 | 5,0 |
|---|---|---|---|---|---|---|---|---|---|
| accord | 69,8 % | 82,4 % | 86,8 % | **89,3 %** | **89,3 %** | 86,8 % | 74,8 % | 66,0 % | 54,1 % |

`S = 10/3` est dans le plateau (141 / 159, 88,68 %), et c'est la lecture qui réconcilie la mesure
avec la seule description publique connue de cette moitié — « `center_height + fluid_level_spread
× 10` », citée dans `docs/provenance/etage-de-bruit.md` § 5 : le facteur 10 est là, et ce qui
manquait à la description est que le résultat est **arrondi vers le bas à un multiple de trois**.
La mesure ne sépare pas 3,2 de 3,3333 ; les deux donnent 142 et 141 cellules sur 159, et l'écart
tient à une poignée de cellules à la frontière d'un palier. **Je ne tranche donc pas entre les
deux**, et le chiffre honnête est : `décalage = 3 × ⌊S × spread⌋` avec `S ∈ [3,1 ; 3,4]`.

### 3.3 Le niveau de la mer est un cas à part, et il n'est pas expliqué

48 des 207 cellules ont un niveau de **63** — le niveau de la mer. Elles **ne suivent pas** la loi
ci-dessus : 8 / 48, soit 16,67 %, contre 89,31 % pour les autres. Il y a donc bien un cas
« aquifère inondé jusqu'à la mer », c'est **23 % des cellules**, et `fluid_level_floodedness` ne le
sépare pas dans cet échantillon (moyenne −0,0013 sur les cellules à 63, +0,0719 ailleurs, lu à
l'indice de cellule). La règle d'inondation **n'est pas établie**.

---

## 4. Pourquoi rien n'est implémenté

L'étage aquifère a cinq pièces. Trois manquent :

| pièce | état |
|---|---|
| la grille — 16 × 40 × 16, calée sur y = 0, centre `40k + 20` | **mesurée** (§ 3.1) |
| le niveau, hors cas inondé — `40k + 20 + 3⌊S · spread⌋`, `spread` à l'indice de cellule | **mesurée** (§ 3.2), `S ∈ [3,1 ; 3,4]` |
| le cas inondé au niveau de la mer — 23 % des cellules | **non mesuré** (§ 3.3) |
| le choix eau / lave | **non mesuré** |
| la barrière — 3,92 % des cellules considérées | **non mesuré** (§ 2) |

Poser les deux pièces connues et deviner les trois autres produirait un étage qui creuserait là où
le jeu garde de la pierre, et qui mettrait de l'eau au mauvais niveau dans une cellule sur quatre.
C'est précisément « une demi-fonctionnalité sur une spécification incomplète », que
`docs/provenance/etage-de-bruit.md` refusait déjà, et le refus tient encore — **mais il tient
maintenant sur trois trous nommés et chiffrés, au lieu d'un « nulle part documenté ».**

Ce qui a changé, et qui vaut plus que l'implémentation absente : **il y a maintenant un oracle
direct.** `ov_parity --aquifer --dump=` écrit une ligne par cellule considérée et une ligne par
cellule de grille avec ses quatre bruits, aux trois positions d'échantillonnage candidates. Les
trois pièces manquantes sont des questions posées à ce fichier, pas à de la documentation absente.

### La piste pour la suite, nommée

La méthode qui a marché pour `spread` marche telle quelle pour les deux autres : prendre l'étiquette
que l'oracle donne (« cette cellule est-elle inondée ? », « eau ou lave ? »), balayer un seuil sur
chaque bruit à chaque position d'échantillonnage, et exiger que **tous** les témoins restent au
taux de base. Ce qui a manqué ici est l'échantillon : 207 cellules avec un niveau lisible, dont
48 inondées, ne suffisent pas à séparer un seuil. Les surfaces libres **hors** cellules creusées —
celles de l'étage de bruit, bien plus nombreuses — sont la source évidente, et
`ov_parity --aquifer` ne les lit pas encore : il ne regarde que les cellules du masque.

---

## 5. Le sélecteur d'`old_blended_noise` : le soupçon est vrai, ce n'est pas un bug

`docs/provenance/amplitude-old-blended-noise.md` § 5 laissait ouvert que
`blend = (sélecteur/10 + 1)/2` sature presque partout — le sélecteur ayant huit octaves dont le
poids double — et que les deux piles de limites seraient alors **un interrupteur dur au lieu d'un
fondu**. C'était nommé comme un possible « bug de fond ». Deux mesures, et elles ne disent pas la
même chose.

### La saturation est réelle

`ov_parity --selector`, 133 200 positions :

| | moyenne | écart-type | min | max | clampé haut | clampé bas | **réellement mélangé** |
|---|---|---|---|---|---|---|---|
| overworld | −0,3439 | 56,44 | −231,9 | +219,0 | 42,780 % | 43,024 % | **14,196 %** |
| nether | +0,1154 | 63,42 | −229,8 | +229,5 | 43,532 % | 42,995 % | **13,473 %** |

**Le fondu n'est un fondu que dans 14 % du monde.** Le soupçon était exact, et il est maintenant
chiffré au lieu d'être supposé.

### Mais le normaliser éloigne du jeu

`OV_SELECTOR_DIV` divise le sélecteur avant le mélange : c'est exactement l'hypothèse « le jeu
normalise cette pile ». Le Nether, où `base_3d_noise` est toute la densité, mesure la distribution
qui en sort. `ov_parity --nether --sweep-var=OV_SELECTOR_DIV --sweep=1,2,5,10,25,60`, seed
1234567890, 40 chunks par région, 26 900 colonnes :

| diviseur | **1** | 2 | 5 | 10 | 25 | 60 |
|---|---|---|---|---|---|---|
| distance moyenne à la courbe du jeu (toit) | **0,0423** | 0,0435 | 0,0477 | 0,0519 | 0,0599 | 0,0636 |
| distance moyenne (sol) | **0,0670** | 0,0685 | 0,0715 | 0,0743 | 0,0754 | 0,0752 |
| **les deux** | **0,0522** | 0,0535 | 0,0572 | 0,0609 | 0,0661 | 0,0683 |

**Monotone, et le minimum est au bord : diviseur 1.** Aucune normalisation ne rapproche la
distribution du jeu ; toutes l'éloignent. C'est la même statistique qui avait un minimum
*intérieur* net sur le balayage d'amplitude (0,0384 à gain 1,2, 0,0931 à gain 0,25), donc elle sait
discriminer — ici elle discrimine et elle dit non.

**Verdict : la saturation à 86 % est un fait, ce n'est pas un bug.** L'interrupteur dur est ce qui
reproduit le mieux la distribution que le jeu écrit. `OV_SELECTOR_DIV` reste comme instrument, à
1,0 par défaut, et un test unitaire tient la mesure (`test_noise.cpp`).

---

## 6. La rugosité de surface : trois suspects écartés, un coupable nommé

`docs/provenance/amplitude-old-blended-noise.md` § 6 relevait que notre surface est **40 % plus
rugueuse que celle du jeu à un bloc de distance** et nommait trois suspects : *l'interpolation de
la grille de cellules, la jaggedness, ou le seuil de `is_solid`*.

### Les trois sont écartés

**L'interpolation est implémentée et elle fonctionne.** `density.cpp` a un nœud `Interpolated` qui
échantillonne aux huit coins d'une cellule 4 × 8 × 4 et interpole trilinéairement dans l'ordre du
jeu (y, puis x, puis z) — le commentaire d'en-tête de `density.hpp` qui disait le contraire est
périmé de plusieurs commits. `ov_parity --column=100,100` montre la cassure de pente exactement à
`y = 56`, c'est-à-dire sur une frontière de cellule : le champ *est* linéaire par morceaux.

Et le test décisif : **retirer un seul terme fait disparaître tout l'excès**. Si la jaggedness ou
le seuil `is_solid` y étaient pour quelque chose, aucun terme de grotte ne pourrait les emporter.

### Le coupable

`ov_parity --surface --chunks=60`, 12 636 colonnes sèches, fonction de structure à d = 1 :

| | le jeu | nous | rapport | erreur moyenne de hauteur |
|---|---|---|---|---|
| tel quel | 1,028 | **1,555** | **1,513** | −1,278 |
| `OV_NO_CAVE_NOISE=1` (tous les `weird_scaled_sampler`) | 1,028 | 0,835 | 0,813 | −1,019 |
| `OV_MUTE_FUNCTION=minecraft:overworld/caves/entrances` | 1,028 | **0,830** | **0,807** | −0,968 |
| `OV_MUTE_FUNCTION=…/caves/spaghetti_roughness_function` | 1,028 | 0,835 | 0,813 | −1,019 |

**Museler la seule fonction `minecraft:overworld/caves/entrances` reproduit tout l'effet** — et
museler `spaghetti_roughness_function`, qui est la branche `argument2` du `min` interne à
`entrances`, le reproduit aussi au millième près. Le chemin est donc précis :

```
final_density → range_choice(sloped_cheese < 1,5625) → min(sloped_cheese, 5 × entrances)
entrances     = min(A, B)
B             = spaghetti_roughness_function + clamp(spaghetti_3d…, −1, 1)
```

Le facteur **5** multiplie la pente : là où `5 × entrances` est le minimum, la hauteur de surface
est fixée par le passage à zéro d'un bruit de faible amplitude, et elle bouge donc beaucoup d'un
bloc au suivant. Cela explique la rugosité *et* 0,31 bloc du déficit de hauteur de 1,28.

### Ce que ce n'est pas — et ce que je ne conclus pas

Sans `entrances`, notre surface est **plus lisse** que celle du jeu (0,807), pas plus rugueuse.
L'excès n'est donc pas « un terme en trop » : c'est un terme dont l'influence est trop grande d'un
facteur qui reste à établir.

**Je n'affirme pas que `entrances` est deux fois trop fort.** Ce serait exactement la faute de
méthode qui a coûté cher ici : un balayage de gain sur `entrances` aurait un optimum intérieur à
coup sûr, entre 0,807 et 1,513, et un optimum intérieur sans témoin ne vaut rien. Aucun oracle
pour la distribution d'`entrances` n'a été trouvé, et le Nether n'en a pas — sa `final_density` ne
nomme pas ce terme. Ce qui est acquis est la **localisation**, pas la correction :
l'interpolation, la jaggedness et le seuil de `is_solid` sont hors de cause, et le grain fin comme
un quart du déficit de surface entrent par `minecraft:overworld/caves/entrances`.

`OV_MUTE_FUNCTION` reste pour la suite : il remplace n'importe quelle fonction nommée par +64, ce
qui la retire d'un `min` sans toucher au reste, là où `OV_NO_CAVE_NOISE` les museler toutes et ne
peut donc dire *laquelle*.

---

## 7. Parité — inchangée, et c'est le résultat

Aucune constante de génération n'a bougé. `noise.cpp` a été réarrangé (la boucle du sélecteur est
extraite dans `BlendedNoise::selector`) et le réarrangement est **neutre au bit près** — c'est ce
que les quatre lignes ci-dessous vérifient, prises avec le binaire d'après sur les échantillons
d'avant.

| sonde | commande | avant | après |
|---|---|---|---|
| `ov_carveparity` | `--chunks=1200` | 1 200 / 1 200 — **100,000 %**, 1 615 858 cellules, 0 en trop, 0 manquante | **identique** |
| `ov_surfparity` | `--chunks=250 --per-region=4` | colonnes 58 378 / 64 000 — **91,216 %** · blocs 429 195 / 439 432 — **97,670 %** | **identique** |
| `ov_parity --terrain --carvers` | `--chunks=600` | 3 415 855 / 3 456 000 — **98,838 %** · pierre en trop 14 060 · manquante 26 085 | **identique** |
| `ov_caveedge` | `--chunks=150 --per-region=4` | parois **88,670 %** · intérieurs **80,629 %** | **identique** |

Le mandat demandait un avant/après avec les deux modes d'erreur séparés. Il est plat, dans les deux
sens, et c'est honnête : ce travail a produit un oracle et quatre mesures, pas un étage.

---

## 8. Instruments laissés en place

- **`ov_parity --aquifer [--dump=]`** — l'oracle du § 1. Sans `--dump` il imprime le résumé ; avec,
  il écrit une ligne `B x y z genre densité` par cellule considérée et, par cellule de grille
  16 × 40 × 16, trois lignes `C` / `I` / `O` portant les quatre bruits d'aquifère échantillonnés
  respectivement au centre en blocs, à l'indice de grille et au coin en blocs. Le choix entre les
  trois **est** la question du § 3.2, et c'est pour ça que les trois sont écrites.
- **`ov_parity --selector`** — la distribution du sélecteur et sa part clampée (§ 5).
- **`ov_parity --nether --sweep-var=<VAR>`** — le balayage du Nether porte maintenant sur n'importe
  quelle variable d'environnement, pas seulement `OV_BASE3D_GAIN`.
- **`OV_SELECTOR_DIV`** — divise le sélecteur avant le mélange. Défaut 1,0.
- **`OV_MUTE_FUNCTION=<nom>[,<nom>…]`** — remplace des fonctions nommées du datapack par +64
  (§ 6).
- **`BlendedNoise::selector`** et **`NoiseRouter::blended_noise()`** — les accès que ces mesures
  demandaient. Rien dans la génération ne les lit.

## 9. Sources

- **`run/reference-1234567890`** — généré par le vrai serveur 1.20.1 (SHA-1
  84194a2f286ef7c14ed7ce0090dba59902951553). Tous les chiffres des §§ 1–3 en viennent, par lecture
  directe des fichiers de région : masques `CarvingMasks/AIR` et tableaux de blocs.
- **`run/reference-nether-1234567890`** — l'oracle de distribution du § 5.
- **`worldgen/noise_settings/overworld.json`**, **`worldgen/noise/aquifer_*.json`** et
  **`worldgen/density_function/overworld/caves/*.json`** — sortie du data generator, régénérée
  localement. C'est de là que viennent la forme de `final_density` et de `entrances` (§ 6), lues et
  non devinées.
- La seule description publique de la moitié « niveau de fluide » de l'aquifère est celle déjà
  citée dans `docs/provenance/etage-de-bruit.md` § 5 (cellules, `center_height +
  fluid_level_spread × 10`, seuils 0,8 / 0,4 / ±0,3). Le § 3.2 la confirme sur le facteur 10 et y
  ajoute l'arrondi à un multiple de trois et la position d'échantillonnage, qui n'y sont pas.
- **Aucune source de code tierce n'a été consultée pour ce travail.**
