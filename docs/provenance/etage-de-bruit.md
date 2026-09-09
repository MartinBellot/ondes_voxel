# L'étage de bruit : ce que le décalage de surface n'est pas

*2026-09-09. Seed 1234567890, monde de référence `run/reference-1234567890`.*

> **§4 est infirmé.** Sa conclusion — « l'amplitude de `base_3d_noise` est environ
> quatre fois trop grande » — repose sur un balayage de gain sans témoin. Avec un
> témoin (le même bruit échantillonné dix mille blocs plus loin) le champ faux
> marque aussi bien que le vrai, et le Nether, où ce terme est presque toute la
> `final_density`, mesure l'amplitude directement : elle est correcte à ±20 %.
> Voir `docs/provenance/amplitude-old-blended-noise.md`. Les §§1–3, 5–7 tiennent.

Ce document ne rapporte aucun changement de génération. Il rapporte des
**mesures**, dont quatre écartent des hypothèses écrites ailleurs et une
localise la cause du déficit de surface sans l'établir.

---

## 1. Le mandat était périmé sur son point principal

Le mandat décrivait `final_density` comme manquante, bloquée sur
`old_blended_noise`, et les entrées de grottes (`spaghetti_2d`, `noodle`,
`pillars`) comme non implémentées.

**Ce n'est plus vrai.** Un recensement des types de nœuds que le routeur
overworld atteint réellement — en suivant chaque référence depuis les quinze
entrées de `noise_settings/overworld.json` — donne 25 types distincts, et
`density.cpp` les implémente **tous**. Le routeur charge ses quinze entrées,
`final_density` comprise, et `NoiseRouter::unavailable()` est vide.

C'est déjà couvert par un test (`the overworld router loads whole`). La leçon
est de méthode : la première mesure d'une session doit être de **re-mesurer**
ce que l'état écrit affirme.

## 2. Le chiffre, et pourquoi il a bougé sans que la génération bouge

| mesure | blocs | accord | pierre en trop | pierre manquante |
|---|---|---|---|---|
| ancienne (pas de 4, glace comptée) | 102 400 | 97,965 % | 1,176 % | 0,859 % |
| glace exclue, même échantillonnage | 102 400 | 98,047 % | 1,179 % | 0,774 % |
| **pas de 3, glace exclue, 60 chunks** | **345 600** | **98,309 %** | **1,016 %** | **0,675 %** |

Les trois lignes décrivent **le même générateur**. Seule la mesure change.

**La glace n'est pas du terrain.** Sur un océan gelé, les *surface rules* gèlent
le dessus de l'eau : le bloc solide le plus haut du jeu est alors à hauteur de
la mer, alors que le bruit a posé le fond vingt blocs plus bas. Comptée comme
terrain, chaque colonne gelée déclarait une surface huit blocs trop basse ou
plus, et ces 76 colonnes (11,9 % de l'échantillon) enterraient le vrai décalage
sous un artefact d'une étape ultérieure. Les sondes de l'ancienne mesure
pointaient toutes des colonnes d'océan gelé, avec des densités de −0,12 à −0,33
qui n'avaient rien à voir avec le biais cherché.

Le pas d'échantillonnage passe de 4 à 3 pour une raison précise : la cellule
d'interpolation fait quatre blocs de large, donc un pas de 4 ne visite **que**
`x % 4 == 0` et le test d'alignement horizontal n'a rien à comparer. Trois
visite les quatre résidus, et échantillonne 345 600 blocs au lieu de 102 400.

## 3. Quatre hypothèses écartées par la mesure

### Le champ n'est pas translaté en y

Nouveau diagnostic : pour chaque bloc comparé, on compte aussi l'accord qu'on
aurait en lisant notre densité `n` blocs plus haut ou plus bas.

| décalage | −4 | −3 | −2 | −1 | **0** | +1 | +2 | +3 | +4 |
|---|---|---|---|---|---|---|---|---|---|
| accord | 95,83 | 96,55 | 97,18 | 97,84 | **98,31** | 97,46 | 96,62 | 95,89 | 95,16 |

Le maximum est **en 0**, net, et la courbe retombe des deux côtés. Un champ
décalé de deux blocs culminerait en −2. **Il n'y a pas de décalage de −2.**

Ce n'est pas en contradiction avec l'histogramme de surface centré sur −1/−2 :
l'accord global est dominé par l'intérieur profond, qui est bien placé, tandis
que le biais ne touche que la mince couche du dessus.

### La grille de cellules est alignée, verticalement et horizontalement

Erreur moyenne de surface selon la place de la hauteur dans la cellule de huit :

| `(y+64) % 8` | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| erreur moyenne | −1,78 | −1,94 | −1,62 | −2,02 | −2,02 | −1,48 | −0,31 | −1,58 |

et selon la place en x dans la cellule de quatre :

| `x % 4` | 0 | 1 | 2 | 3 |
|---|---|---|---|---|
| erreur moyenne | −1,37 | −1,19 | −1,11 | −1,40 |

Plat des deux côtés. Une grille décalée d'une demi-cellule ferait monter et
descendre cette ligne au rythme de la cellule. Le seul creux, `% 8 == 6`,
correspond à `y = 62` : c'est le seau du niveau de la mer, pas un effet de
grille. **L'indexation de la grille de cellules et un décalage d'un
demi-échantillon sont écartés.**

### Ce ne sont pas les termes de grotte

Près de la surface, `final_density` vaut `min(sloped_cheese, 5 × caves/entrances)` :
les grottes ne peuvent que **baisser** le terrain. `OV_NO_CAVE_NOISE` les retire.

| | accord | pierre en trop | histogramme (mode) |
|---|---|---|---|
| avec | 98,047 % | 1,179 % | −2 à 25,2 % |
| sans | 97,343 % | 1,917 % | −2 à 24,5 % |

L'histogramme ne bouge pas, la pierre en trop empire de moitié. Les grottes sont
réelles et ne sont pas la cause du déficit de surface.

### Ce n'est pas la moyenne de `base_3d_noise`

Nouveau mode `--stats` : moyenne, écart-type et bornes de chaque terme sur
35 960 positions.

| terme | moyenne | écart-type | min | max |
|---|---|---|---|---|
| `overworld/base_3d_noise` | −0,00501 | 0,16367 | −0,600 | +0,665 |
| `overworld/depth` | +0,01545 | 0,34044 | −0,701 | +1,626 |
| `overworld/offset` | −0,52752 | 0,17550 | −0,787 | +0,626 |
| `overworld/factor` | +4,69406 | 1,04690 | +0,625 | +6,300 |
| `overworld/jaggedness` | +0,00160 | 0,01950 | 0,000 | +0,396 |

`depth` est cohérent avec sa propre définition au millième près (le gradient
moyen de la grille échantillonnée vaut +0,543, plus `offset` à −0,5275, donne
+0,0155 : c'est la valeur mesurée). La moyenne de `base_3d_noise` est faible et
ne déplace la surface que d'environ **0,03 bloc** : elle n'explique pas 1,5.

## 4. Ce qui est localisé : l'amplitude, pas la moyenne

`sloped_cheese = 4 × quarter_negative(D × factor) + base_3d_noise`.

`quarter_negative` a un **coude** en zéro : la pente vaut `4 × factor` du côté
positif et `1 × factor` du côté négatif. La hauteur moyenne de la surface dépend
donc de l'**amplitude** de `base_3d_noise`, pas seulement de sa moyenne.

`OV_BASE3D_GAIN` multiplie ce bruit et rien d'autre. Histogramme de surface sur
les colonnes sèches :

| gain | mode | part au mode | part en 0 | queue |
|---|---|---|---|---|
| 1,0 | −2 | 20,9 % | 13,1 % | jusqu'à −8 |
| 0,5 | −1 | 35,8 % | 29,5 % | presque plus |
| **0,25** | **0** | **39,1 %** | **39,1 %** | la plus serrée |
| 0,001 | 0 / +1 | 28,9 % / 24,9 % | 28,9 % | dépasse vers le haut, s'élargit |

**Le contrôle est la partie importante.** L'accord global, lui, est presque plat
de 0,001 (98,367 %) à 0,25 (98,403 %) : un champ plus lisse gagne mécaniquement
sur une métrique dominée par l'intérieur profond, donc **le pourcentage seul ne
peut pas identifier l'amplitude**. C'est la *forme de l'histogramme* qui le peut,
et elle a un optimum intérieur réel : à gain nul le centre passe au-dessus (0/+1)
et la distribution s'élargit, à gain 1 il passe en dessous.

Conclusion mesurée : **l'amplitude de `base_3d_noise` est environ quatre fois
trop grande.**

Deux arithmétiques donnent exactement quatre. Notre pile somme seize octaves dont
le poids double — `Σ 2·2^i = 131 070` — puis divise par 512 puis par 128. Deux
octaves de moins donnent `Σ_{i<14} = 32 766`, soit un rapport de 4,000 ; un
second diviseur de 512 au lieu de 128 donne aussi 4. L'écart-type prédit par la
seconde lecture, 0,041, est celui que le gain 0,25 produit.

**Rien n'a été changé.** La bonne constante n'est pas établie : la page
`Density_function` du wiki décrit `old_blended_noise` par « Samples a legacy
noise. [more information needed] » et ne donne ni le nombre d'octaves ni les
diviseurs. Poser 0,25 parce que la parité l'aime serait précisément « un
générateur qui ressemble à sa sortie », ce que ce dépôt s'interdit.
`OV_BASE3D_GAIN` reste **un instrument de mesure**, à 1,0 par défaut.

## 5. Ce qui n'est pas fait, et pourquoi

**Les aquifères.** L'étape qui change le verdict solide/air est la *barrière* et
le calcul de pression — c'est elle qui creuse du fluide dans la pierre. Elle
n'est documentée nulle part où ce projet ait le droit de lire : la page
`Aquifer` du wiki n'a pas d'algorithme, et l'explication en prose de jacobsjo
documente la moitié « niveau de fluide » (cellules 16×40×16 pour le niveau,
64×40×64 pour le choix eau/lave, `center_height + fluid_level_spread × 10`,
`min_disabled ≈ 0,8`, `max_empty ≈ 0,4`, seuil de lave ±0,3) mais marque la
barrière **TODO**. Aucun des projets lisibles (Cuberite, Valence, MCHPRS,
Feather) ne l'implémente, et de toute façon la règle d'or interdit de spécifier
depuis du code. Implémenter la seule moitié documentée changerait *quel fluide
remplit l'air*, pas le verdict solide/air : cela ne bougerait pas le chiffre et
poserait une demi-fonctionnalité sur une spécification incomplète.

**Les veines de minerai.** Elles remplacent de la pierre par du minerai. Elles ne
peuvent pas changer un verdict solide/air, donc elles ne peuvent pas bouger cette
mesure. Les entrées `vein_toggle`, `vein_ridged` et `vein_gap` existent déjà dans
le routeur et sont sous test de bornes.

## 6. Où sont les 1,691 % restants

| | blocs | ce que le jeu avait |
|---|---|---|
| pierre en trop | 3512 (1,016 %) | air 2283, eau 1045, neige 71, lave 33, cave_air 17 |
| pierre manquante | 2332 (0,675 %) | terre 780, herbe 591, pierre 578, deepslate 97, gravier 68 |

L'air et le `cave_air` (2300) appartiennent aux **carvers**, qui ne sont pas de
ce mandat. L'eau (1045) est pour partie les **aquifères**. La terre et l'herbe
(1371) sont le déficit de surface de la section 4 : ce sont exactement les blocs
que les surface rules posent au sommet de la colonne, donc ils comptent la
hauteur manquante.

## 7. Un test ajouté qui n'existait pas

Les bornes ne sont pas de la documentation : `BinaryOp::compute` s'en sert pour
**court-circuiter** — `a < b->min_value() ? a : min(a, b)`. Une borne trop
serrée fait donc renvoyer le *mauvais nombre*, silencieusement, aux positions où
la branche sautée aurait gagné. Le test échantillonne les quinze entrées du
routeur et vérifie que chaque valeur tombe dans `[min_value, max_value]`. Il
passe (8521 assertions au total dans le module).

Un second test fixe la moyenne de `base_3d_noise` à moins de 0,05 en valeur
absolue, tolérance large et volontairement non ajustée : c'est un garde-fou
contre un décompte d'octaves qui glisserait, pas un nombre calibré.
