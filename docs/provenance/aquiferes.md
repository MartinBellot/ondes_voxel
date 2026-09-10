# Les aquifères : l'oracle, la moitié qui se mesure, et celle qui ne se mesure pas

*Seed 1234567890, monde de référence `run/reference-1234567890`.*

> **Mise à jour (§ 10).** Les §§ 1–9 décrivent l'oracle et refusaient l'implémentation faute de
> source pour la barrière. Cette source existe maintenant (`Cave#Aquifer` sur le wiki), et le § 10
> rapporte **l'aquifère implémenté**, branché dans l'étage de bruit et dans les carvers, avec ses
> chiffres avant/après et un témoin par choix ouvert. Ce qui suit jusqu'au § 9 est gardé tel quel :
> c'est l'oracle sur lequel le § 10 est mesuré.

Les §§ 1–9 ne rapportaient **aucun changement de génération**. Les quatre chiffres de parité y sont
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

---

## 10. L'aquifère implémenté (`aquiferes-2`)

*2026-09-10. Seed 1234567890, même monde de référence.*

Ce qui a changé depuis les §§ 1–9 n'est pas l'oracle, c'est la documentation. L'article
`Aquifer` du wiki que `etage-de-bruit.md` § 5 citait comme vide est devenu une redirection vers
**`Cave#Aquifer`**, et cette section décrit maintenant l'algorithme — y compris la moitié barrière
que ce dépôt refusait faute de source. Le gist de jacobsjo, lui, est inchangé et marque toujours la
barrière « TODO ».

### 10.1 Ce que chaque source dit, et ce qu'elle ne dit pas

| pièce | `Cave#Aquifer` (wiki) | gist jacobsjo | ce qui restait ouvert |
|---|---|---|---|
| grille | 16 × 12 × 16, « décalée de 5 en X/Z et de 1 en Y » | — | **le sens** du décalage |
| centre | offset 0–9 / 0–8 / 0–9, « tiré de la graine et des coordonnées de cellule » | — | **le nom** du hachage positionnel, **l'ordre** des trois tirages |
| voisins | 12 cellules (2 × 3 × 2), 4 plus proches, écart de 25 sur les distances au carré | — | l'ordre de parcours (départage des égalités) |
| règle globale | lave sous `min(−54, niveau de la mer)`, surface à −54 ; fluide par défaut jusqu'au niveau de la mer | idem | — |
| surface préliminaire | 13 échantillons, relevés de 8 | `initial_density_without_jaggedness > 0,390625`, liste des 13 décalages **en chunks** | **où** dans le chunk, **le pas** de la recherche |
| statut | trois règles (au-dessus du sol → global ; touche une surface sous la mer → mer ; sinon le bruit) | « désactivé si la lave est choisie » | — |
| inondation | seuils −0,3 / −0,8 sous la mer, 0,8 / 0,4 sur terre, interpolés sur 64 blocs | idem | la profondeur est-elle comptée depuis la surface relevée ou brute |
| niveau partiel | `40⌊Y/40⌋ + 20 + spread`, « plafonné à la plus basse surface » | `spread × 10`, **arrondi vers le bas au multiple de 3**, échantillonné à l'échelle 1/16, 1/40, 1/16 | **quelle** surface plafonne (relevée ou brute) |
| lave | niveau ≤ −10 et `|lava| > 0,3`, une valeur par région 64 × 40 × 64 | idem, échelle 1/64, 1/40, 1/64 | — |
| exclusion | `erosion < −0,225` et `depth > 0,9` | les mêmes en flottants | — |
| barrière | **qualitative** : positive entre les niveaux, pic au milieu ; couvercle « jusqu'à environ 5 blocs » au-dessus ; plancher « jusqu'à environ 23 blocs » en dessous ; pression fixe forte eau/lave ; bruit `barrier` « dans une bande étroite » ; statuts égaux → pression nulle | TODO | **toute la formule** |
| ticks | « un tick planifié » pour les fluides près d'une frontière entre statuts différents ; l'eau juste au-dessus de la couche de lave, toujours | — | la règle exacte de « près » |

La colonne de droite est ce qui a été **mesuré** (§ 10.3). Aucune ligne de code tiers n'a servi à la
remplir : chaque choix ouvert a été posé comme hypothèse, puis confronté à l'oracle du § 1 avec au
moins un témoin qui aurait dû faire baisser l'accord.

### 10.2 La forme de la pression, depuis les deux nombres de l'article

L'article ne donne pas la formule, mais il donne **deux épaisseurs**, et elles contraignent une
famille. Soit `m` le milieu des deux niveaux, `h` la demi-distance entre eux et
`d = h − |y + ½ − m|` (positif entre les niveaux, maximal au milieu) :

- moitié haute : `d / a₁` si `d > 0`, `d / a₂` sinon ;
- moitié basse : avec `q = b + d`, `q / b₁` si `q > 0`, `q / b₂` sinon ;
- bruit `barrier` ajouté seulement si la pression brute est dans `[−B, B]` ;
- le tout multiplié par `g` et par la similarité `1 − (d₂ − d₁)/25` des centres, puis ajouté à la
  densité ; pierre si la somme est positive.

Le bruit ne peut plus rien quand la pression brute sort de la bande. Le couvercle s'arrête donc à
`B × a₂` blocs au-dessus du niveau haut, et le plancher à `b + B × b₂` blocs sous le niveau bas.
« Environ 5 » et « environ 23 » donnent deux équations ; `B = 2`, `a₂ = 2,5`, `b = 3`, `b₂ = 10`
les satisfont exactement, avec `a₁ = 1,5`, `b₁ = 3`, `g = 2` et une pression eau/lave de 2. Ce
sont les valeurs par défaut de `AquiferTuning`, et chacune est balayée des deux côtés au § 10.3.

### 10.3 L'oracle des cellules creusées : avant, après, et un témoin par choix ouvert

`ov_parity --aquifer`, bras de prédiction. Pour chaque cellule de l'oracle du § 1, notre aquifère
répond à la question des carvers (densité nulle ; lave à et sous y = −56) et la réponse est
comparée à ce que le jeu a laissé. Notre terrain et nos carvers ne participent pas : la liste des
cellules et la réponse viennent toutes deux du jeu. « Cellules simples » = celles où le jeu a de
l'air, un fluide, de la pierre ou de la deepslate ; la bedrock et les blocs de features (dripstone,
sculk) n'appartiennent pas à l'aquifère.

**L'oracle entier — les 1 775 chunks et 1 490 251 cellules du § 1**, `ov_parity --aquifer
--chunks=4000`. L'avant reproduit exactement la base du § 1 (69,200 % d'air, 23,456 % d'eau,
2,836 % de lave, 4,508 % de solide chez le jeu) :

| | avant (lave ≤ −56, air au-dessus) | **après (notre aquifère)** |
|---|---|---|
| accord, cellules simples | 1 062 950 / 1 481 524 — 71,747 % | **1 480 842 / 1 481 524 — 99,954 %** |
| accord, toutes cellules | 1 062 950 / 1 490 251 — 71,327 % | 1 482 853 / 1 490 251 — 99,504 % |

| jeu \ nous (après) | air | eau | lave | solide |
|---|---|---|---|---|
| air — 1 031 251 | 1 030 690 | 3 | 0 | 558 |
| eau — 349 558 | 2 | 349 497 | 0 | 59 |
| lave — 42 259 | 1 | 0 | 42 258 | 0 |
| solide — 67 183 | 1 562 | 133 | 5 080 | 60 408 |

**Cellule par cellule : l'eau à 99,983 %, la lave à 99,998 %, l'air à 99,946 %.** Le solide garde
60 408 cellules sur 67 183 ; les 5 080 « solide → lave » sont la bedrock sous −56 (hors des cellules
simples), et les 1 562 + 133 restants — de la pierre que le jeu garde et que nous creusons — sont,
avec les 558 cellules d'air que nous gardons en pierre, **l'écart de la barrière : 2 253 cellules,
0,15 % de l'oracle.**

**300 chunks, 284 514 cellules, dont 283 001 simples** — le sous-échantillon des témoins, même
tendance :

| | avant (lave ≤ −56, air au-dessus) | **après (notre aquifère)** |
|---|---|---|
| accord, cellules simples | 203 738 — 71,992 % | **282 820 — 99,936 %** |
| accord, toutes cellules | 203 738 — 71,609 % | 283 188 — 99,534 % |

Le tableau de confusion d'après, en entier (jeu en ligne, nous en colonne) :

| jeu \ nous | air | eau | lave | solide |
|---|---|---|---|---|
| air | 197 350 | 0 | 0 | 157 |
| eau | 2 | 66 901 | 0 | 12 |
| lave | 1 | 0 | 8 555 | 0 |
| solide | 229 | 71 | 854 | 10 382 |

Les 854 « solide → lave » sont la bedrock sous y = −56, que le jeu garde et que la mesure compte
parce qu'elle ne distingue pas l'étage ; ils sont hors des cellules simples. Ce qui reste — 157
cellules d'air que nous gardons en pierre, 229 + 71 de pierre que nous creusons — est la barrière
aux quelques cellules près, et c'est l'écart nommé.

Par bande de 16, l'accord sur les cellules simples va de **99,143 %** (−64 … −49, la couche de
lave) à **100,000 %** (−16 … −1, et au-dessus de 64).

**Les témoins.** 200 chunks, 179 958 cellules simples ; le défaut fait **179 860 — 99,946 %**.
Chaque ligne change **un** choix :

| choix ouvert | défaut | témoin | accord du témoin | verdict |
|---|---|---|---|---|
| nom du hachage positionnel | `minecraft:aquifer` | `minecraft:witness` | 90,678 %¹ | **tranché** |
| ordre des tirages | x, y, z | z, y, x | 91,795 % | **tranché** |
| sens du décalage X/Z | −5 | +5 | 96,313 % | **tranché** |
| pas de la surface préliminaire | 8 | 1 | 92,356 % | **tranché** |
| position des 13 surfaces | centre + 16 × décalage | origine du chunk | 97,872 % | **tranché** |
| plafond du niveau partiel | surface brute | surface relevée | 99,894 % | **tranché** (−92 cellules) |
| bruit `barrier` | lu | ignoré | 99,926 % | **tranché** (−36) |
| `below_bias` | 3 | 2 / 4 | 99,904 % / 99,921 % | **maximum intérieur** |
| `below_in` | 3 | 2 / 4 | 99,941 % / 99,939 % | maximum intérieur (−9 / −12) |
| `below_out` | 10 | 8 / 12 | 99,942 % / 99,941 % | maximum intérieur (−7 / −9) |
| `above_in` | 1,5 | 1 / 2 | 99,939 % / 99,943 % | maximum intérieur (−12 / −5) |
| sens du décalage Y | +1 | −1 | 99,946 % | **non tranché** — identique à la cellule près |
| gain de la pression | 2 | 1,5 / 2,5 | 99,946 % / 99,946 % | **non mesurable ici** |
| similarité chaînée | oui | non | 99,946 % | **non mesurable ici** |
| pression eau/lave | 2 | 1 / 4 | 99,946 % / 99,946 % | **non mesurable ici** |
| bande du bruit | 2 | 1,5 / 2,5 | 99,946 % / 99,946 % | **non mesurable ici** |

¹ sur 300 chunks.

**Pourquoi quatre lignes ne peuvent pas trancher, et ce n'est pas un manque d'échantillon.** Les
carvers posent la question avec une densité nulle : une cellule devient pierre si et seulement si
`bruit + pression brute > 0`. Un gain positif multiplie toute la somme, les similarités sont des
poids positifs, la pression eau/lave est une constante positive — aucun des trois ne peut changer
un signe. La bande, elle, ne compte que si le bruit `barrier` dépasse ±2 en valeur absolue, ce
qu'il ne fait pratiquement jamais. Ces quatre choix n'agissent que dans l'étage de bruit, où la
densité n'est pas nulle, et là l'erreur de relief de notre terrain domine toute comparaison. **Leurs
valeurs reposent donc sur la seule contrainte des deux épaisseurs de l'article (§ 10.2), pas sur une
mesure**, et c'est dit.

**`above_out` : une tendance jusqu'au bord, donc pas ajusté.** C'est le seul choix dont le témoin
fait *mieux* que le défaut, et il le fait de façon monotone :

| `above_out` | 0,5 | 1,0 | 1,5 | 2,0 | 2,25 | **2,5** | 3,0 |
|---|---|---|---|---|---|---|---|
| accord (200 chunks) | 99,989 % | 99,978 % | 99,962 % | 99,953 % | 99,951 % | **99,946 %** | 99,932 % |
| air du jeu gardé en pierre | 2 | 24 | — | 70 | 73 | **83** | — |
| solide du jeu gardé | 7 122 | 7 123 | — | 7 124 | 7 124 | **7 124** | — |

Tout le gain est dans la ligne de l'air : une pente plus raide au-dessus du niveau haut ne fait
que retirer de la fausse pierre, et ne coûte que 2 vraies cellules de barrière à 0,5. **Le
couvercle « jusqu'à environ 5 blocs » de l'article ne se voit pas dans les cellules creusées.**
Deux lectures sont possibles et la mesure ne les sépare pas : soit la branche « au-dessus du niveau
haut » de la famille du § 10.2 n'a pas la bonne forme (et aucune constante ne la corrigera), soit
le couvercle de l'article est un effet de l'étage de bruit, où la densité n'est pas nulle, et pas
des carvers. Une valeur ajustée au bord d'un balayage monotone est exactement ce que les pièges 7 et
14 du briefing interdisent : **la valeur reste celle que l'article impose (2,5)**, et l'écart —
83 cellules sur 179 958 — est nommé ici plutôt qu'absorbé.

Le décalage Y non tranché s'explique aussi : avec trois rangées de cellules explorées sur l'axe Y,
les quatre centres les plus proches tombent dans les mêmes rangées quel que soit le sens du
décalage d'un bloc. Le choix est sans effet mesurable sur cet échantillon ; il n'est pas pour autant
établi.

La base de comparaison qui rend ces chiffres lisibles : « air partout au-dessus de −56 » fait
71,992 %. Le témoin le plus faux (mauvais nom de hachage, tous les centres déplacés) fait encore
90,678 %, parce que la plupart des cellules sont de l'air au-dessus de tout niveau quels que soient
les centres. **L'échelle utile est donc entre 90,7 % et 99,9 %, pas entre 72 % et 100 %**, et c'est
dans la ligne de l'eau qu'elle se lit : 66 901 cellules d'eau sur 66 915 avec le bon nom, 58 052 avec
le mauvais.

### 10.4 Le réveil des fluides : un oracle de plus, et un désaccord avec l'article

**Vanilla ne planifie pas de tick à la génération : il marque.** Les 115 chunks `carvers` des six
premières régions portent tous une liste `PostProcessing`, 145 022 entrées au total, contre
**22** entrées `fluid_ticks`. Le « tick planifié » de l'article est donc une marque de
post-traitement, convertie en tick quand le chunk est terminé. Et c'est un oracle direct : pour
chaque cellule creusée que le jeu a remplie d'un fluide, la liste dit si le jeu a voulu la réveiller.

**L'empaquetage du short se mesure lui-même.** Décodée `x | y<<4 | z<<8`, **99,615 %** des marques
tombent sur un fluide du jeu ; décodée `x | z<<4 | y<<8`, 70,970 %. Le premier est le bon, et le
second sert de témoin.

**La règle.** Sur les cellules creusées où le jeu et nous mettons le même fluide (100 chunks,
29 683 cellules), le jeu marque selon l'écart `d₂ − d₁` des deux centres les plus proches, et
selon rien d'autre :

| `d₂ − d₁` | marquées | non marquées |
|---|---|---|
| 0 … 44 (par bandes de 5) | 1 205 à 1 697 par bande | 17 à 36 par bande |
| 44 | 293 | 4 |
| **45** | **0** | **308** |
| 46 … 69 | 0, sauf 48 (2), 56 (4), 64 (4) | 185 à 510 |

| règle candidate | accord |
|---|---|
| la première (statuts différents parmi les proches, à moins de 25) | 56,665 % |
| `statut₁ ≠ statut₂`, toute distance | 45,568 % |
| un des trois autres statuts diffère, toute distance | 43,025 % |
| `d₂ − d₁ < 25`, tout statut | 78,493 % |
| `d₂ − d₁ < 40`, tout statut | 93,997 % |
| **`d₂ − d₁ < 45`, tout statut** | **98,915 %** |
| `d₂ − d₁ < 48` | 96,213 % |
| `d₂ − d₁ < 50` | 93,643 % |
| tout fluide | 44,298 % |

La coupure est franche (293 contre 4 à 44, 0 contre 308 à 45) et le maximum est intérieur, avec des
témoins des deux côtés. **Les statuts n'y entrent pas** : exiger qu'ils diffèrent fait tomber
l'accord de 98,9 % à 45,6 %. C'est un **désaccord avec l'article**, qui dit que « les fluides à
l'intérieur d'une cellule, ou entre cellules de statuts identiques, ne sont jamais planifiés ». La
mesure dit l'inverse pour la seconde moitié de la phrase, et c'est la mesure qui est retenue
(`AquiferTuning::schedule_gap = 45`). Les quelques marques au-delà de 45 tombent toutes à 48, 56
et 64 — des multiples de 8, comme les faux niveaux du § 3.1 — et ne sont pas expliquées ; elles
représentent 10 cellules sur 29 683.

**La règle posée dans le générateur, mesurée.** `d₂ − d₁ < 45` pour tout fluide que l'aquifère
place, plus l'eau juste au-dessus de la couche de lave (règle de l'article, gardée telle quelle :
l'échantillon creusé n'a pas assez de cellules à y = −54 pour la trancher) :

| échantillon | les deux marquent | le jeu seul | nous seuls | ni l'un ni l'autre | accord |
|---|---|---|---|---|---|
| 100 chunks | 13 040 | 109 | 0 | 16 534 | **99,633 %** |
| 300 chunks | 31 936 | 999 | 0 | 42 521 | **98,676 %** |
| **oracle entier, 1 775 chunks** | **166 437** | **6 547** | **0** | **218 771** | **98,329 %** |

Sur l'oracle entier, les 6 547 cellules que le jeu marque et pas nous sont, **toutes**, de la lave
des carvers à ou sous y = −56 (le jeu en marque 6 547 et en laisse 25 152). **Au-dessus de ce
niveau, nos marques et celles du jeu coïncident sur les 385 208 cellules comparées, sans une
exception.**

**Nous ne marquons jamais une cellule que le jeu ne marque pas.** Sur 100 chunks, les 109 cellules
que le jeu marque et pas nous sont **toutes** de la lave posée par les carvers **à ou sous leur
niveau de lave** (y ≤ −56) : le jeu en marque 109 et en laisse 216, nous n'en marquons aucune,
parce que les carvers n'y demandent rien à l'aquifère. Au-dessus de ce niveau, l'accord est total
sur cet échantillon. Que le jeu marque un tiers de cette lave-là, et lequel, n'est pas expliqué :
c'est l'écart restant, nommé.

**Ce qui n'est pas fait.** `ChunkGenerator::generate(chunk, &fluid_updates)` rend la liste des
positions à réveiller, mais **rien ne la livre** à la file de ticks d'un niveau : `world::Chunk`
n'a pas de liste de post-traitement, et le serveur n'en lit pas à la génération. Les fluides
générés restent donc immobiles chez nous là où le jeu ferait couler une cascade au premier tick.
C'est nommé ici et dans `chunk_generator.hpp`, pas caché.

### 10.5 Déterminisme multi-worker

L'aquifère mémorise statuts, centres et surfaces : c'est exactement le genre d'état qui fait
diverger deux workers. Il est donc **par chunk et par appel** (`AquiferSampler`, construit dans
chaque étage), la moitié graine (`Aquifer`) est immuable, et un test unitaire vérifie que l'ordre
des questions ne change aucune réponse. De bout en bout :

`ov_gendet --side=1 --workers=4`, pipeline complet (bruit, biomes, surface, carvers, features) :
16 chunks, **1 572 864 cellules de blocs comparées, 0 différente ; 24 576 cellules de biomes,
0 différente** — le monde à 4 workers est le monde série. (Les temps imprimés, 120 s en série
contre 212 s en parallèle, ont été pris pendant que deux sondes tournaient sur la même machine et
ne mesurent rien.)

### 10.6 Ce qui est branché

- **L'étage de bruit** (`ChunkGenerator::generate_noise`) demande à l'aquifère ce que devient
  chaque bloc, avec la `final_density` du bloc : pierre si la densité est positive **ou** si une
  barrière la rend positive, sinon eau, lave ou air selon le statut. La couche de lave sous −54
  reste de la lave quoi qu'il arrive (règle globale).
- **Les carvers** (`apply_carving`) posent de la lave **à et sous leur propre niveau de lave**
  (`lava_level: {above_bottom: 8}`, y ≤ −56 — c'était déjà le cas, et c'est gardé avant l'aquifère,
  pas après). Au-dessus, ils demandent à l'aquifère avec une **densité nulle** : son fluide sous son
  niveau, de l'air au-dessus, et **pas de coupe du tout** là où la barrière tient — la cellule
  garde alors ce que la surface y avait mis.
- **Les fluides à réveiller** (§ 10.4) sont collectés par `generate(chunk, &fluid_updates)` et par
  les deux étages, mais ne sont livrés à aucune file de ticks.
- **L'échantillonneur est par chunk** (§ 10.5) ; la moitié graine vit dans le `ChunkGenerator` et
  se construit toute seule depuis le routeur — `NoiseRouter::seed()` a été ajouté pour ça, rien
  d'autre dans le serveur ni dans les outils n'a eu à être câblé.
- **`OV_AQUIFER=0`** remet l'ancien comportement (règle globale seule, carvers qui creusent en
  air), comme instrument : c'est ce qui donne l'« avant » de chaque tableau ci-dessous depuis le
  même binaire.

### 10.7 Parité de bout en bout, sur chunks `full`

L'oracle du § 1 isole l'aquifère ; il ne dit pas ce que l'aquifère vaut dans un monde généré
entier, où notre terrain a ses propres erreurs. C'est la question de cette section, et la sonde
**passe par `ChunkGenerator::generate()`** (piège 13) : `ov_caveedge` génère chaque chunk avec
bruit, biomes, surface et carvers, aquifère activé, puis une seconde fois aquifère désactivé,
dans le même processus, et range chaque cellule en air / eau / lave / solide contre le jeu. La
glace et les plantes aquatiques du jeu comptent comme eau, la neige comme air : ce sont des
étages ultérieurs posés sur une cellule d'eau ou d'air.

`ov_caveedge --chunks=60 --per-region=2`, 60 chunks `full`, **5 603 328 cellules** :

| | aquifère désactivé | **aquifère activé** |
|---|---|---|
| toutes les cellules | 5 478 887 — 97,779 % | **5 553 521 — 99,111 %** |
| sous le niveau de la mer (y < 63) | 1 760 146 / 1 853 184 — 94,980 % | **1 834 769 — 99,006 %** |

**L'apparié, qui annule tout ce que les deux bras partagent** — notre erreur de relief comprise :

| | cellules |
|---|---|
| cellules dont l'aquifère change la classe | 76 053 |
| **seul l'aquifère donne la classe du jeu** | **74 684 — 98,200 %** |
| seule la règle globale la donne | 50 — 0,066 % |
| aucun des deux | 1 319 — 1,734 % |

Les changements, par ordre : 56 426 cellules d'**air** que la règle globale avait **noyées**
(des grottes sèches sous le niveau de la mer, exactement le défaut que `ordre-des-etages.md` § 5
compensait en vidant les fluides creusés), 12 790 cellules creusées qui doivent **garder leur
eau**, 2 725 + 2 410 cellules que la **barrière** garde en pierre là où le jeu en a, 273 + 60 de
**lave** d'aquifère. Dans l'autre sens, 1 277 cellules que le jeu a en pierre et que nous vidons
d'une eau qui n'aurait pas dû y être non plus — l'erreur de relief sous-jacente, que l'aquifère ne
peut pas corriger.

**La peau des grottes**, même échantillon, même binaire, ordre du jeu (bruit, biomes, surface,
puis carvers), `OV_AQUIFER=0` pour l'avant :

| | aquifère désactivé | **aquifère activé** |
|---|---|---|
| parois (cellules voisines d'une cellule creusée) | 41 952 / 46 927 — 89,398 % | **43 698 / 46 905 — 93,163 %** |
| intérieurs (cellules creusées) | 53 374 / 70 061 — 76,182 % | **68 394 / 70 061 — 97,621 %** |

Les intérieurs passent de 76 % à 97,6 % : c'est l'eau qui reste dans les cellules creusées sous une
nappe, et la barrière qui garde la pierre là où le jeu refuse de couper. Les 80,629 % d'intérieurs
du § 7 ont été pris sur un autre échantillon (`--chunks=150 --per-region=4`) et ne se comparent
pas à ces deux colonnes.

**Les minerais de bout en bout** (`ov_genparity --chunks=48`, pipeline complet avec features,
même binaire, `OV_AQUIFER=0` pour l'avant) :

| | aquifère désactivé | **aquifère activé** |
|---|---|---|
| minerais du jeu au bon bloc | 14 877 / 18 057 — 82,389 % | **14 977 / 18 057 — 82,943 %** |
| notre bloc là où le jeu a un minerai : eau | 78 | 16 |
| notre bloc là où le jeu a un minerai : air | 58 | 35 |

+100 minerais, +0,554 point. Le chiffre inscrit dans `PROGRESS.json`, 82,406 %, a été pris sur un
commit antérieur ; le bras « désactivé » de ce binaire donne 82,389 %, 17 minerais de moins,
écart qui appartient à ce qui a été fusionné entre-temps et non à l'aquifère. L'avant honnête est
celui du même binaire.

**`terrain_parity` (97,79 %) ne bouge pas, par construction** : `ov_parity --terrain` interroge
`ChunkGenerator::is_solid()`, c'est-à-dire le signe de `final_density`, et ne passe ni par
`generate()` ni par l'aquifère — c'est exactement le piège 13, et c'est pour ça que la mesure de
cette section a été prise ailleurs. Le chiffre de bout en bout qui le remplace pour les fluides est
le tableau des classes ci-dessus.

**Ce que l'aquifère coûte.** `ov_gendet --side=1 --workers=1`, 16 chunks, pipeline complet
(features comprises), build **debug**, machine sans autre sonde de ce travail :

| | bras série | bras « parallèle » (1 worker) | cellules différentes |
|---|---|---|---|
| aquifère désactivé | 32,438 s | 33,379 s | 0 |
| aquifère activé | **80,329 s** | **181,343 s** | 0 |

**La génération coûte environ 2,5 fois plus avec l'aquifère**, sur un chemin que `PROGRESS.json`
note déjà comme non servable. Rien n'a été optimisé : la surface préliminaire est recherchée par
pas de 8 depuis y = 320, en treize colonnes par centre, et chaque étage reconstruit son propre
échantillonneur. Le bras « parallèle » à un seul worker, 2,25 fois plus lent que le bras série
**seulement quand l'aquifère est activé**, n'est pas expliqué : une seule mesure, sur une machine
partagée avec d'autres agents. Ce n'est pas un problème de justesse — les deux bras rendent le
même monde au bloc près — mais c'est un écart de temps nommé et non compris.

Le reste du tableau n'est pas l'aquifère : les ~18 000 cellules d'air du jeu où nous avons de la
pierre, et les ~20 000 de pierre où nous avons de l'air, sont les mêmes à 7 près avec et sans
aquifère — c'est le relief (`amplitude-old-blended-noise.md`).

### 10.8 Instruments laissés en place

- **`ov_parity --aquifer`** a maintenant un **bras de prédiction** : pour chaque cellule de
  l'oracle du § 1, la réponse de notre aquifère (densité nulle), en tableau de confusion contre le
  jeu, avant (lave ≤ −56, air au-dessus) et après, par bande de 16. Il décode aussi
  `PostProcessing` des deux façons et note chaque règle de réveil candidate contre les marques du
  jeu.
- **`OV_AQ_*`** (`SHIFT_XZ`, `SHIFT_Y`, `DRAW`, `NAME`, `SURF_CENTRE`, `SURF_STEP`, `CAP_RAISED`,
  `CHAIN`, `NOISE`, `ABOVE_IN`, `ABOVE_OUT`, `BELOW_BIAS`, `BELOW_IN`, `BELOW_OUT`, `BAND`,
  `WATER_LAVA`, `GAIN`, `SCHEDULE_GAP`) — change un choix ouvert à la fois dans le harnais, sans
  recompiler. Lus par `ov_parity` seulement ; le générateur n'en lit aucun.
- **`ov_caveedge`** a un **bras « classes »** : chaque cellule de chaque chunk généré, rangée en
  air / eau / lave / solide, contre le jeu, aquifère activé et désactivé, avec le décompte apparié
  des cellules que l'aquifère change.
- **`AquiferSampler::neighbourhood`** — les quatre centres les plus proches et leurs statuts, pour
  tester une règle contre l'oracle sans toucher à `compute`.

### 10.9 Ce qui n'est pas fait, et les pièges payés

**Pas fait, nommé :**

1. **La livraison des fluides à réveiller.** `generate(chunk, &fluid_updates)` rend la liste ;
   `world::Chunk` n'a pas de liste de post-traitement et le pipeline comme le serveur l'ignorent.
   Chez nous, une cascade générée reste immobile jusqu'à ce qu'un voisin la touche.
2. **La lave des carvers sous leur niveau.** Le jeu marque 109 de ces 325 cellules (100 chunks),
   nous aucune ; la règle qui en choisit un tiers n'est pas trouvée.
3. **Quatre constantes de pression non mesurées** — gain, similarité chaînée, pression eau/lave,
   bande du bruit (§ 10.3). L'oracle des cellules creusées ne peut pas les voir, et l'oracle de bout
   en bout est dominé par l'erreur de relief. Leurs valeurs viennent des deux épaisseurs de l'article.
4. **Le sens du décalage Y**, sans effet mesurable sur l'échantillon, donc non établi.
5. **Le coût** : environ 2,5 fois le temps de génération en debug (§ 10.7), non optimisé, et un
   bras « parallèle » à un worker inexplicablement plus lent avec l'aquifère.
6. **Les égalités de distance** : l'ordre de parcours des douze cellules (x, puis y, puis z, avec
   comparaisons strictes) décide qui est « le plus proche » à égalité. Il n'est ni documenté ni
   mesuré séparément ; ce qui en dépend est dans les quelques centaines de cellules d'écart.

**Pièges, à connaître des autres agents :**

1. **La page `Aquifer` du wiki n'existe plus comme telle** : c'est une redirection vers
   `Cave#Aquifer`, et c'est là que la barrière est décrite. Un `WebFetch` de `/w/Aquifer?action=raw`
   rend la redirection et rien d'autre ; le résumeur de `WebFetch` tronque par ailleurs les
   passages longs — télécharger le wikitext avec `curl` et le lire soi-même.
2. **Un témoin identique au défaut ne confirme rien.** Quatre des témoins du § 10.3 tombent pile sur
   le chiffre du défaut, et ce n'est pas qu'ils sont faux « de peu » : la question posée (densité
   nulle, signe d'une somme) ne peut pas les voir. Avant de conclure qu'un choix est bon parce que
   son témoin ne fait pas mieux, vérifier que le témoin peut faire *moins bien*.
3. **Une tendance qui court jusqu'au bord n'est pas un optimum** (pièges 7 et 14 du briefing) :
   c'est ce que fait `above_out`, et c'est pourquoi sa valeur n'a pas été ajustée sur l'agrégat.
4. **`PostProcessing` est empaqueté `x | y<<4 | z<<8`**, une liste par section à partir de la
   section la plus basse. L'autre sens place encore 66–71 % des marques sur un fluide — assez pour
   avoir l'air de marcher.
5. **`ov_parity --terrain` ne voit pas l'aquifère** (il appelle `is_solid`) : un chiffre de terrain
   inchangé après ce travail n'est pas une preuve que l'aquifère n'a rien changé. Mesurer par
   `generate()` (`ov_caveedge`, `ov_genparity`).
