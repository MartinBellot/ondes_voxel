# Explosions — les rayons, la résistance, le cratère

Ce qui suit est établi par **mesure contre le vrai serveur 1.20.1**
(`tools/vanilla/server.jar`, SHA-1 `84194a2f286ef7c14ed7ce0090dba59902951553`),
par `scripts/measure_blast.py`. Les résultats bruts vont dans
`data/vanilla/1.20.1/normalized/blast_*.json` (gitignorés, régénérables) ; la
table retenue entre dans le `.ovpack` et est relue par
`registry::BlockRegistry::blast_resistance`. Les règles vivent dans
`src/ov_gameplay/{include/ov/gameplay,src}/explosion.{hpp,cpp}` et sont rejouées
par `src/ov_gameplay/tests/test_explosion.cpp`.

Le point de départ de ce mandat est un refus, écrit dans `redstone.md` § 14 par
l'agent qui avait mesuré la mèche de la TNT :

> « la résistance au souffle n'est dans aucun rapport du data generator, et la
> déduire des formes demanderait une boîte par bloc, soit 987 boîtes. »

Le refus était juste. Ce document est la campagne qui le lève.

---

## 0. Ce qu'un oracle peut et ne peut pas dire ici

Une explosion **tire au sort**. L'énergie de chacun de ses 1352 rayons vaut
`puissance × (0,7 + 0,6 × aléa)`, tiré par rayon. Deux charges identiques au
même endroit dans la même boîte ne font pas le même cratère — ni chez eux, ni
chez nous. **Aucune comparaison coup par coup n'a de sens.**

Ce qui se compare, et ce que ce document compare :

- la **fréquence** de destruction de chaque cellule, sur K tirs ;
- l'**union** des K tirs : toute cellule qu'un tir a prise ;
- l'**intersection** : les cellules que les K tirs ont toutes prises.

L'union et l'intersection convergent, quand K grandit, vers deux formes nettes —
le cratère du rayon le plus chanceux et celui du moins chanceux. Ce sont elles
qui font une preuve cellule par cellule.

⚠ **Le piège du `/fill` sous le monde.** Un `fill` dont un coin descend sous
`y = −64` échoue **en entier**, sur une ligne de console que personne ne lit, et
chaque cellule se relit alors « pas le matériau » — ce qui est exactement
l'apparence d'une destruction totale. Ce piège a déjà coûté une campagne ici
(voir `redstone.md` § 14). Tout `y` de `measure_blast.py` passe par `check_y()`,
qui refuse et dit pourquoi ; le banc travaille à `y = −40`, vingt et un blocs
au-dessus du sol du superflat, donc rien de ce qui est mesuré ne touche le
terrain d'origine.

---

## 1. Le banc de résistance : une rangée, pas une boîte

L'idée qui rend la campagne faisable. La résistance décide de **la profondeur à
laquelle un rayon s'arrête**. Il suffit donc, par bloc, d'une **rangée** :

```
     y        [charge]  B B B B B B B B B        <- neuf cellules du bloc
     y−1      sol       sol …                    <- barrière, ou le substrat
     y−2      barrière  barrière …               <- tient les sols qui tombent
```

La rangée est **radiale** : pour atteindre la k-ième cellule, un rayon doit
traverser les k−1 précédentes, parce qu'un rayon parti du centre et penché sort
de la rangée et n'y revient jamais. Ce qui remonte est donc une profondeur de
pénétration pure, et rien d'autre.

La charge est une TNT amorcée invoquée avec `{Fuse:0,NoGravity:1b,Motion:[0,0,0]}` :
son centre est un point fixe, pas l'endroit où une entité a fini par tomber.

**987 cellules dans un seul monde**, 46 colonnes de 20 blocs, 22 rangées de 12,
soit 812 chunks forceloadés. Une passe = reconstruire les 959 rangées, invoquer
959 charges, attendre l'horloge **du serveur** (pas la nôtre : mille explosions
dans un tick font un tick de plusieurs secondes), sauver, relire 8 631 cellules.
Seize passes.

### Ce que la rangée refuse

| | blocs | pourquoi |
|---|---|---|
| se sont tenus debout sur barrière | 910 | |
| + après reprise sur terre | 959 | |
| **n'ont jamais tenu** | **28** | nommés ci-dessous |

Les 28 : `attached_melon_stem`, `attached_pumpkin_stem`, `big_dripleaf_stem`,
`brain_coral`, `brain_coral_block`, `brain_coral_fan`, `brain_coral_wall_fan`,
`cactus`, `cave_vines`, `cave_vines_plant`, `fire_coral`, `fire_coral_block`,
`fire_coral_fan`, `fire_coral_wall_fan`, `frogspawn`, `frosted_ice`,
`glow_lichen`, `lily_pad`, `sculk_vein`, `sugar_cane`, `tube_coral`,
`tube_coral_block`, `tube_coral_fan`, `tube_coral_wall_fan`,
`twisting_vines_plant`, `vine`, `weeping_vines`, `weeping_vines_plant`.

Trois familles, et aucune n'est une surprise une fois nommée : ce qui pousse sur
un support précis (tiges, cactus, canne, nénuphar), ce qui s'accroche à un mur
(lichen, vigne, veine de sculk), et **les coraux vivants, qui meurent hors de
l'eau**. Le piège du corail est déjà écrit dans `redstone.md` § 2 ; il est repayé
ici sur les **blocs** de corail, que la campagne de conduction n'avait pas
touchés.

### Ce que la rangée lit à côté de la résistance — 21 blocs

Nommés plutôt que jetés en silence, parce que chacun est un mécanisme du jeu et
pas du bruit :

- **les 20 coraux vivants** (`tube`, `brain`, `bubble`, `fire`, `horn` × bloc,
  corail, éventail, éventail mural) : ceux qui ont tenu à la première lecture
  sont morts pendant les passes. `bubble_coral_block` donne un score de
  **3,94** là où sa classe (résistance 6) donne **1,00** : le banc a mesuré une
  mort, pas un souffle ;
- **`minecraft:tnt`** : score **8,31** sur 9, profil `[16,16,16,16,16,16,14,13,10]`.
  La rangée s'est **allumée elle-même**. C'est le témoin le plus fort que le banc
  fonctionne : la détonation en chaîne est un vrai mécanisme du jeu et il est
  visible dans les chiffres bruts.

### Le résultat

Le score `S` d'un bloc est la somme, sur les neuf cellules, de la fréquence de
destruction. Il est **monotone en résistance et en rien d'autre**. Confronté au
candidat de PrismarineJS/minecraft-data (MIT, `pc/1.20`) — la même source, prise
de la même façon, que la dureté (`measure_hardness.py`) :

| candidat | blocs | S médian | | candidat | blocs | S médian |
|---|---|---|---|---|---|---|
| 0,0 | 131 | 4,188 | | 1,0 | 100 | 2,125 |
| 0,1 | 37 | 3,938 | | 1,4 | 16 | 2,000 |
| 0,2 | 32 | 3,844 | | 1,5 | 13 | 2,000 |
| 0,25 | 3 | 3,688 | | 1,8 | 16 | 2,000 |
| 0,3 | 42 | 3,625 | | 2,0 | 66 | 2,000 |
| 0,4 | 6 | 3,250 | | 2,5 … 9,0 | 340 | 1,000 |
| 0,5 | 80 | 3,000 | | 600 … 3,6 M | 20 | 0,000 |
| 0,6 … 0,8 | 57 | 2,875 | | | | |

**Aucune inversion sur 31 valeurs distinctes.** C'est le contrôle : une campagne
qui donnerait l'obsidienne plus fragile que le verre serait fausse quel que soit
le nombre.

- **959 blocs** passés devant une charge ;
- **947** tombent dans leur propre classe de résistance ;
- **3 écarts**, tous expliqués et nommés : `bamboo` et `bamboo_sapling`
  (S = 3,00 contre 2,125 pour la classe 1,0) reposent sur un sol de **terre**,
  qui est détruit sous eux et les fait tomber une cellule trop loin ;
  `spruce_wall_hanging_sign` (2,69), profil `[16,16,5,1,1,1,1,1,1]`, s'est
  décroché une fois sur seize.

### Ce que le banc **ne sépare pas**, et pourquoi

Un rayon de puissance 4 porte au plus `4 × 1,3 = 5,2` d'énergie ; un bloc de
résistance R lui en coûte `(R + 0,3) × 0,3` par pas. Passé
`R ≈ (5,2 − 0,09) / 0,3 ≈ 17`, **aucune explosion du jeu ne fait la différence**
entre deux résistances : la plus grosse charge que le jeu produise est celle du
Wither, puissance 7, soit un plafond à `R ≈ 30`. L'obsidienne à 1200, le
bedrock à 3,6 millions et l'ender chest à 600 sont **indiscernables par
construction**, et le seraient encore avec dix mille tirs.

Bandes que le banc à puissance 4 et sans écart ne sépare pas :

`[0,1 · 0,2]` · `[0,25 · 0,3]` · `[0,5 · 0,6 · 0,65 · 0,7 · 0,75]` ·
`[1,4 · 1,5 · 1,8 · 2,0]` · `[2,5 … 9,0]` · `[600 … 3,6 M]`

La dernière bande est un plafond du jeu. Les autres sont un problème de
résolution, et le banc en a un second pour ça.

### Le deuxième banc : quatre blocs d'air devant la rangée

`measure_blast.py resistance_gap` est la même chose avec **quatre cellules d'air
entre la charge et la rangée**. L'air mange les trois quarts de l'énergie avant
qu'elle n'arrive, ce qui déplace tout le seuil : là où le banc collé donnait
« la première cellule casse toujours » pour tout ce qui va de 2,5 à 9 — 340
blocs qui partageaient une seule lecture — le banc avec écart donne une
*fréquence*, et une fréquence mesure là où une certitude ne mesure rien.

Les deux bancs n'ont pas les mêmes angles morts, et c'est le point : ce qui
compte est **l'intersection de leurs partitions**.

| bande | banc collé | banc avec écart |
|---|---|---|
| bas (0 … 0,4) | sépare 0 · 0,1-0,2 · 0,25-0,3 · 0,4 | fond 0-0,2 ensemble |
| milieu (0,5 … 2,5) | sépare 0,5-0,75 · 0,8 · 1,0 · 1,4-2,0 | fond 0,7-2,5 ensemble |
| haut (2,5 … 9) | **une seule lecture pour 340 blocs** | sépare 2,5 · 3,0-3,5 · 4+ |

Ensemble, ils séparent **15 groupes sur les 32 valeurs candidates**, contre 10
pour le banc collé seul. Ce qui reste à égalité, nommé :

`[0,1 · 0,2]` · `[0,5 · 0,6]` · `[0,7 · 0,75]` · `[0,8 · 1,0]` ·
`[1,4 · 1,5 · 1,8 · 2,0]` · `[3,0 · 3,5]` · `[4,0 · 4,2 · 4,8 · 5,0 · 6,0 · 9,0]` ·
`[600 · 1200 · 3,6 M]`

Le total des deux bancs : **963 blocs** vus par au moins un banc,
**1885 lectures sur 1900** dans leur propre classe, **15 écarts** — 3 sur le banc
collé (nommés ci-dessus), 12 sur le banc avec écart, qui a moins de dynamique et
donc plus de bruit relatif. Aucun des deux n'a d'inversion.

### Le verrou

`scripts/check_blast.py` relit les flottants **dans le `.ovpack` lui-même**, pas
dans le JSON qui l'a produit, et revérifie la propriété sur laquelle tout repose :

```
  blocs ............ 1003
  identiques ....... 1003
  classes mesurées . 31
  inversions ....... 0
```

La comparaison est **relative** et pas absolue : `3 600 000,8` s'écrit
`3 600 000,75` dans un `float`, et un epsilon absolu déclarait faux le barrier et
le bloc de lumière pour porter la seule valeur qu'un flottant 32 bits sache
porter.

---

## 2. L'algorithme

Spécifié depuis la documentation (`https://minecraft.wiki/w/Explosion`, article
« Explosion », section « Java Edition »), jamais depuis du code, et chaque
constante confrontée au cratère mesuré.

1. **1352 rayons**, vers les cellules **extérieures** d'une grille 16×16×16 :
   `16³ − 14³`. Prendre les 4096 tirerait les directions intérieures plusieurs
   fois et épaissirait le cratère le long des axes.
2. Direction `(j/15 × 2 − 1, k/15 × 2 − 1, l/15 × 2 − 1)`, **calculée en
   `float` puis élargie**, puis normalisée en `double`.
3. Énergie `puissance × (0,7 + 0,6 × aléa)`, **un tirage par rayon**.
   ⚠ Un seul tirage nommé, dans l'ordre : C++ ne séquence pas les opérandes
   d'une addition, Java si.
4. Pas de **0,3 bloc**, coûtant **0,22500001** — pas 0,225. Le dernier bit est
   dans la constante du jeu et il fait entrer ou sortir la dernière cellule d'un
   rayon.
5. Un bloc non-air coûte `(résistance + 0,3) × 0,3` **par pas passé dedans**.
   L'air pur ne coûte **rien du tout** — pas « une résistance nulle », rien : le
   biais de 0,3 n'est pas payé. Un bloc fait un peu plus de trois pas, donc un
   bloc de résistance R coûte à peu près `R + 1,05`.
6. La cellule est prise si l'énergie est **encore strictement positive après**
   la soustraction.
7. Un fluide répond **100**, et un bloc noyé répond le **plus grand** des deux.
   C'est ce qui fait qu'un couloir inondé survit à une charge qui aplatit le
   même couloir à sec, et c'est la raison pour laquelle tout bloc du banc a été
   posé `waterlogged=false`.

Le RNG à passer est `math::LegacyRandomSource` — `java.util.Random`, ce que le
niveau 1.20.1 utilise pour ses explosions. Notre ordre de parcours de la grille
est `j, k, l` et il consomme exactement un `next_float()` par rayon, donc deux
appels avec le même état donnent le même cratère : le test
`« the same seed makes the same crater »` le vérifie dans les deux sens.

Vanilla garde les positions dans un `HashSet` puis les **mélange**. Cet ordre
n'est reproductible par rien, le nôtre est trié — l'ensemble est le même, et le
choix est écrit dans le code plutôt que subi.

---

## 3. Le cratère, cellule par cellule

Une charge au centre d'une boîte pleine d'un seul matériau, **16 fois**, la boîte
reconstruite entre chaque tir, chaque cellule relue dans les fichiers de région.
Chez nous : la même géométrie, la même puissance, le même centre, seize tirs sur
un `LegacyRandomSource` de graine fixe.

⚠ **Pas de sable ni de gravier dans cette boîte.** Une boîte de sable fait cinq
mille entités de bloc qui tombe par tir : elle a rempli le tas de 2 Go et tué le
serveur au dix-huitième tir sur vingt-quatre — et celles qui retombent changent
la boîte avant le tir suivant. C'est aussi pourquoi le banc écrit ses tables
**après chaque passe** : la première campagne a perdu 18 passes valides en
mourant avant la fin.

### Ce que le vrai serveur a fait

| matériau | résistance | union des 16 | intersection |
|---|---|---|---|
| `glass` | 0,3 | 312 | 188 |
| `dirt` | 0,5 | 211 | 128 |
| `oak_planks` | 3 | 18 | 18 |
| `stone` | 6 | 2 | 2 |
| `end_stone` | 9 | 1 | 1 |
| `obsidian` | 1200 | 0 | 0 |

Une charge au milieu d'une boîte **pleine** est bien plus faible qu'une charge
dans une poche d'air : le premier bloc qu'un rayon traverse est déjà le bloc où
il est né, et pour la pierre il coûte à lui seul plus que le rayon ne porte.
C'est ce qui sépare cette table des 26 blocs de pierre relevés dans
`redstone.md` § 14, où le bloc de TNT devenait de l'air en s'amorçant.

### Notre cratère contre le leur

| | cellules | d'accord | chez nous seulement | chez eux seulement |
|---|---|---|---|---|
| **union** des 16 tirs | 555 | **541 — 97,5 %** | 11 | 3 |
| **intersection** des 16 tirs | 555 | **513 — 92,4 %** | 17 | 25 |

Et cellule par cellule, l'écart **de fréquence** moyen vaut **0,042** sur seize
tirs — moins d'un tir sur vingt-quatre. Par matériau : 0 pour `oak_planks`,
`stone`, `end_stone` et `obsidian` (accord parfait, cellule par cellule),
0,045 pour `dirt`, 0,043 pour `glass`.

Les deux erreurs sont séparées, comme demandé, et elles sont **au bord** : une
union sur seize tirs est une estimation du rayon le plus chanceux, et son bord
bouge encore à seize échantillons des deux côtés. Aucune cellule de désaccord
n'est à l'intérieur du cratère.

---

## 4. Les dégâts et le recul

Un zombie par distance, de 1 à 10 blocs, `NoAI` pour qu'il reste où on l'a mis,
mille points de vie pour qu'il survive, **et pas `Invulnerable`** — un invulnérable
ne lit rien et ressemble exactement à une cible hors de portée. Minuit, parce
qu'un zombie en plein midi brûle et que les dégâts relus seraient un feu.

### Les dégâts : 10 sur 10

`dégâts = ⌊(i² + i) / 2 × 7 × 2P + 1⌋` avec `i = (1 − distance / 2P) × exposition`,
`P = 4`, la distance prise **des pieds** de l'entité au centre.

| distance | PV perdus | brut (÷ armure) | prédit |
|---|---|---|---|
| 1 | 45,264 | 46 | **46** |
| 2 | 36,408 | 37 | **37** |
| 3 | 28,536 | 29 | **29** |
| 4 | 20,664 | 21 | **21** |
| 5 | 14,760 | 15 | **15** |
| 6 | 8,856 | 9 | **9** |
| 7 | 3,936 | 4 | **4** |
| 8, 9, 10 | 0 | 0 | **0** |

Sept distances dans la portée, trois au-delà, **dix accords sur dix**, et
identiques sur les huit passes : l'exposition vaut 1 dans un monde vide et la
formule ne tire rien au sort.

Le facteur `0,984` est l'armure du zombie, pas un ajustement : deux points
d'armure, et la règle d'absorption du jeu donne `1 − max(2/5, …)/25 = 0,984`.
Que les sept valeurs relues soient toutes des multiples exacts de 0,984 d'un
**entier** est le contrôle qui dit que la formule est bien plancherée avant
l'armure et pas après.

La portée est exactement `2P` : à 8 blocs le zombie est à `√(64 + 0,06125²)`,
donc à un cheveu **au-delà** de 8, et prend zéro.

### Le recul : la direction d'abord, la norme ensuite

La direction est celle du centre vers **les yeux** de l'entité, pas vers ses
pieds ni vers le centre de sa boîte. Sur les sept distances, le rapport entre la
composante horizontale et la verticale du recul relu vaut celui de la direction
prédite à **1,00000** — ce qui fixe du même coup la hauteur des yeux du zombie
(1,74) et la hauteur du centre de la charge (`y + 0,06125`) à cinq décimales.

La **norme** a coûté un contre-témoin, et c'est le piège de cette section.

Relue telle quelle, elle valait `0,851 × impact` à un bloc, `0,817` à deux,
`0,785` à trois — une décroissance trop propre pour être du bruit et trop
régulière pour être une exposition. Chaque écart valait exactement `0,98²` du
précédent. Deux explications tiennent le même chiffre : *le recul décroît avec
la distance*, ou *le recul stocké décroît pendant qu'on parcourt la liste*, un
aller-retour de console coûtant des ticks.

Le banc lit donc les cibles **à l'endroit une passe sur deux et à l'envers
l'autre**. Si c'était la distance, le sens de lecture ne changerait rien :

| distance | ticks impliqués, lu à l'endroit | lu à l'envers |
|---|---|---|
| 1 | 7 | 26 |
| 2 | 9 | 24 |
| 3 | 11 | 22 |
| 4 | 13 | 20 |
| 5 | 15 | 18 |
| 6 | 17 | 16 |
| 7 | 19 | 14 |

Des **entiers exacts**, deux par cible, et l'ordre s'inverse avec le sens de
lecture. C'est le délai de la console, pas la physique. Extrapolée à zéro tick,
la norme vaut `impact` à cinq décimales aux sept distances **et dans les deux
sens** :

> **l'impulsion est `impact` le long du vecteur unitaire du centre vers les
> yeux**, et la vitesse stockée d'un mob `NoAI` perd 0,98 par tick.

Ce qui reste **non mesuré ici** : le dampener de Protection contre les
explosions. Il est un paramètre de `hit_entity`, pas une table, et vaut zéro
pour une entité sans enchantement — ce qui est le cas mesuré, pas un
bouche-trou.

---

## 5. Les sources

La puissance d'une source ne se lit nulle part : elle se lit **dans son cratère**.
Le banc `sources` fait exploser, dans la même boîte de terre que le § 3, une TNT
amorcée, un creeper allumé et un creeper chargé, et compte.

| source | puissance | union, eux | union, nous | intersection, eux | nous |
|---|---|---|---|---|---|
| creeper | 3 | 86 | 90 | 72 | 68 |
| TNT | 4 | 209 | 214 | 141 | 127 |
| creeper chargé | 6 | 603 | 617 | 356 | 339 |

Trois puissances, trois cratères, et l'ordre comme les tailles tiennent à
quelques cellules de bord près — sur des unions de 86, 209 et 603 cellules
estimées à huit tirs de chaque côté. Les autres puissances de la table
(`explosion_sources()`) ne sont **pas** mesurées ici et sont marquées comme
telles : ghast et tête de wither à 1, lit et ancre de résurrection à 5, cristal
de l'End à 6.

### Le butin : la TNT rend tout

Dix charges au centre d'une boîte de terre pleine, les objets comptés par la
**somme de leurs `Item.Count`** — pas par le nombre d'entités : la terre lâchée
fusionne en piles, et « 7 entités tuées » ce sont sept piles, pas sept mottes.

| | |
|---|---|
| blocs cassés, 10 tirs | **1754** |
| objets lâchés | **1754** |
| rendement | **1,000** |

Dix tirs sur dix exacts. La TNT de 1.20.1 lâche **tout** ce qu'elle casse ; le
`1 / puissance` que documente l'article existe dans le jeu mais **aucune source
mesurée ici ne l'utilise**, et le dire est plus honnête que de l'attribuer au
hasard. Le défaut d'`ExplosionSpec` est donc `Destroy`.

Le comptage des survivants passe par `fill … replace`, qui répond
« Successfully filled N blocks » en une commande et un tick. La première version
relisait trois mille cellules dans les fichiers de région après une sauvegarde,
et le serveur est mort de contention avant la fin du premier tir.

**La mèche du creeper vaut 30 ticks.** Lue sur le `Fuse` de l'entité pendant
qu'elle gonfle, dix captures, **dix fois 30** — sans la convergence par en
dessous qu'ont les autres délais de ce dépôt, parce que le compteur du creeper
monte au lieu de descendre et que ce qu'on lit est sa borne.

⚠ La première version de cette lecture posait le creeper à `x = 200`, **hors de
tout ticket de forceload**. Il n'était donc dans aucun niveau, `data get` ne
répondait rien, et « pas de mèche » ressemble exactement à un creeper qui n'a pas
de compte à rebours. Le piège n°11 du briefing, une variante de plus.

La mèche de la TNT vaut **80 ticks**, mesurée par la campagne redstone
(`redstone.md` § 14) sur le `Fuse` de l'entité amorcée, et reprise ici sans être
remesurée. Une TNT que *l'explosion d'une autre* allume n'attend pas 80 ticks
mais `fuse/8 + aléa(fuse/4)`, soit **10 à 29** — c'est l'échelonnement qui fait
qu'une pile de TNT projette des blocs au lieu de disparaître d'un coup, et le
test `chained_fuse` le vérifie sur les deux bornes.

---

## 6. Ce qui n'est pas fait, et nommé

- **Le feu.** `rolls_fire` existe et tire une fois sur trois, mais ce « une fois
  sur trois » vient de l'article du wiki et **n'a été mesuré par aucun banc de
  cette campagne** : aucun lit n'a été posé dans le Nether ici. C'est marqué dans
  le header, à l'endroit où quelqu'un qui implémente l'ancre de résurrection le
  lira. La liste que ce module rend ne convient d'ailleurs pas pour ça :
  `collect_blocks` ne rend que des **blocs**, alors que le feu va dans les
  cellules d'**air** que les rayons ont traversées, et que vanilla garde dans la
  même liste jusqu'à la fin.
- **Rien n'est branché dans le serveur.** `src/ov_server/` appartient à un autre
  mandat cette nuit. Une TNT amorcée ne saute toujours pas, un creeper toujours
  ne siffle pas, et `Explosions` n'a aucun appelant hors des tests. Les règles,
  la table et la parité sont là ; le câblage ne l'est pas.
- **Le dampener de Protection contre les explosions** est un paramètre, pas une
  table : zéro pour une entité sans enchantement, ce qui est le cas mesuré.
- **L'immunité par type** (dragon, nuage d'effet) n'est pas ici : c'est une
  propriété du type d'entité, et `hit_entity` répond pour la boîte qu'on lui
  donne. L'appelant décide qui il lui donne.
- **La résistance au-delà de ~30** est indiscernable pour tout oracle du jeu, et
  c'est démontré plus haut, pas supposé.

---

## 7. Reproduire

```bash
python3 scripts/measure_blast.py resistance 16     # ~25 min, 987 blocs
python3 scripts/measure_blast.py resistance_gap 16 # ~25 min, la bande haute
python3 scripts/measure_blast.py crater 16         # la forme, cellule par cellule
python3 scripts/measure_blast.py damage 8
python3 scripts/measure_blast.py drops 12
python3 scripts/measure_blast.py sources 8
python3 scripts/measure_blast.py table             # la confrontation, puis la table
python3 tools/ov_datagen/ovpack.py                 # FORMAT_VERSION 14
python3 scripts/check_blast.py                     # le verrou
```

Chaque scénario démarre et arrête son propre serveur, sur le port 25613, dans
`run/blast-oracle/<scénario>/`. Aucun ne dépend d'un autre, et
`OV_BLAST_PORT` permet d'en faire tourner deux côte à côte — ce qui est une
question de mémoire, pas de port, mais le port est ce qui empêche le second
d'échouer à s'attacher en ressemblant à un serveur qui a démarré.

⚠ `tools/ov_datagen/ovpack.py` est passé de **13 à 14** : le pack porte
maintenant un flottant de résistance par bloc, dans le `u32` que l'en-tête
gardait en réserve. Un pack de format 13 est refusé au chargement plutôt que
mal relu.
