# Les fluides — ce qui a été mesuré, et comment

Tout ce qui suit vient d'un vrai serveur **1.20.1** (`tools/vanilla/server.jar`,
SHA-1 `84194a2f286ef7c14ed7ce0090dba59902951553`), piloté par sa console, laissé
tourner, puis relu **dans la sauvegarde Anvil** avec `ov-inspect state`.
Aucune valeur de ce document n'est déduite d'une formule ni lue dans le code
d'un autre projet.

Scripts : `scripts/measure_fluids.py` (l'écoulement, les mélanges, le
waterlogging), `scripts/measure_fluids2.py` (la chute, le rayon exact, la
cobblestone), `scripts/measure_fluid_delays.py` (les délais de tick),
`scripts/fluid_lab.py` (le pilote console + relecture),
`scripts/anvil_read.py` (lecteur NBT/Anvil pour les listes de ticks).

Le monde de mesure est un superplat `bedrock,stone*3`, sol à **y = -61**,
première couche d'air à **y = -60**. Chaque scénario a sa propre bande de 48
blocs le long de X ; l'eau porte à 7 et la lave à 3, donc les bandes sont
indépendantes.

---

## 1. L'écoulement

### La flaque

Une source posée sur un sol plat fait un **losange** dont le niveau est la
distance de Manhattan, jusqu'à 7. Relevé sur 19 × 19 = 361 positions :

```
.........7.........
........767........
.......76567.......
......7654567......
.....765434567.....
....76543234567....
...7654321234567...
..765432101234567..
...7654321234567...
        ⋮
```

**Décroissance de 1 par bloc, maximum 7.**

### La lave

La même source de lave donne un losange de rayon **3** avec les niveaux
0, 2, 4, 6 dans l'overworld :

```
......6......
.....646.....
....64246....
...642L246...
```

et un losange de rayon **7** avec les niveaux 0..7 dans le Nether, joué dans
DIM-1 via `execute in minecraft:the_nether run …` :

```
7654321234567
654321L123456
7654321234567
```

**La lave décroît de 2 dans l'overworld et de 1 dans le Nether.** Le Nether
rend la flaque de lave impossible à distinguer de celle de l'eau.

> Piège de vocabulaire : le mandat parlait de « 2 pour la lave dans l'overworld,
> 3 dans le Nether ». La mesure dit autre chose — ce sont des **décroissances**
> de 2 et de 1, ce qui donne des **portées** de 3 et de 7. Confondre les deux
> donne une lave qui va deux fois trop loin en surface et trois fois trop court
> sous terre.

### Les délais

Lus directement dans la sauvegarde, pas chronométrés : vanilla écrit dans le
chunk le **délai restant** (`t`) de chaque tick programmé, et une source posée
puis sauvée dans la foulée n'a rien consommé, donc son `t` est le délai complet.

| fluide | dimension | `t` |
|---|---|---|
| `minecraft:water` | overworld | **5** |
| `minecraft:lava` | overworld | **30** |
| `minecraft:lava` | nether | **10** |

---

## 2. La recherche du trou

C'est la règle qui fait qu'un filet d'eau est un filet et pas une flaque, et
c'est celle qu'aucun raisonnement n'aurait produite.

### Le trou droit devant

Un trou dans le sol à 4 blocs à l'est. L'eau fait une **ligne d'un bloc de
large** vers l'est et laisse les trois autres côtés parfaitement secs :

```
........01234....
```

### Le trou derrière un mur

Le trou est à 2 blocs à l'est ; un mur de deux blocs barre la route directe.
Le seul chemin fait quatre pas en passant par le sud. **Le premier pas de l'eau
est vers le sud**, à l'opposé du trou :

```
.......#.....
......0#4....
......123....
```

C'est la preuve que la recherche est un vrai parcours en largeur qui contourne
les obstacles, et non une visée en ligne droite.

### Le rayon exact : 5

| distance du trou | résultat |
|---|---|
| 3 (`deux_trous_equidistants`) | l'eau file vers les deux trous, rien ailleurs |
| 4 | ligne droite vers le trou |
| **5** | **ligne droite vers le trou** — `....01234` |
| 6 | **losange complet** : le trou n'est plus vu |

Le rayon est donc **exactement 5**. À 6, l'eau refait un disque et n'atteint le
trou que par hasard, en s'étalant.

### Toutes les directions à égalité coulent

Trou placé à (+3, +2), soit une distance de 5. L'eau remplit **tout le
rectangle** entre elle et le trou, chaque case au niveau de sa distance de
Manhattan :

```
....0123.
....1234.
....2345.
```

Toute case posée sur *un* plus court chemin reçoit de l'eau. Deux trous
équidistants sont alimentés tous les deux.

---

## 3. Les sources

| montage | résultat |
|---|---|
| deux sources d'eau alignées, un creux entre | le creux **devient une source** (`10001`) |
| deux sources d'eau en diagonale | rien ne devient source (`012 / 121 / 210`) |
| deux sources de lave alignées | le creux reste une **coulée de niveau 2** (`2L2L2`) |

L'adjacence doit être **horizontale et orthogonale**, et il faut **deux**
sources. La lave n'a pas cette règle.

---

## 4. La chute

Une source au bord d'une plateforme percée. Le puits se remplit de `level=8`
sur toute sa hauteur et **ne mouille rien sur les côtés**. Au fond, la case
d'arrivée est elle-même `level=8` et alimente ses quatre voisines à **niveau 1**
— comme une source, pas comme une coulée de niveau 8 :

```
y=-55   432181234
```

**Un fluide qui tombe alimente à niveau plein.** L'encodage disque est
`level = falling ? 8 : amount`, avec `amount` 0 pour une source et 1..7 pour une
coulée.

---

## 5. Les mélanges lave / eau

Le tableau est **asymétrique** et dépend de la géométrie. Chaque ligne est un
scénario distinct.

| ce qui arrive | sur quoi | résultat |
|---|---|---|
| eau (par le côté ou par-dessus) | **source** de lave | **obsidienne** |
| eau (par le côté ou par-dessus) | lave **coulante** | **cobblestone** |
| lave (source, coulée ou en chute) | eau, source **ou** coulée | la case d'eau devient de la **pierre** |

Le scénario le plus parlant est `lave_installee_puis_eau_dessus` : de l'eau
versée sur une nappe de lave déjà étalée laisse un **cœur d'obsidienne dans un
anneau de cobblestone**, exactement la frontière entre la source et sa coulée :

```
3218C8123
218CCC812
18CCCCC81
8CCCOCCC8
18CCCCC81
218CCC812
3218C8123
```

### Le piège de la course

Les cas « lave coulante » ont demandé une mesure **en deux temps**. L'eau avance
à 5 ticks par bloc, la lave à 30 : sur un sol plat l'eau arrive toujours avant
que la lave ait eu le temps de couler, si bien que seule la **source** de lave
se convertit et qu'on ne voit jamais de cobblestone. Il faut laisser la lave
s'installer d'abord, poser l'eau ensuite.

Symétriquement, « la lave arrive sur de l'eau » n'est visible qu'en faisant
**tomber** la lave — la chute est le seul mouvement que l'eau ne peut pas
devancer. Trois cibles ont été testées (source, coulée mince niveau 5, coulée
épaisse niveau 1) : **les trois donnent de la pierre**, et la lave au-dessus
survit intacte.

---

## 6. Le waterlogging

| montage | résultat |
|---|---|
| dalle `waterlogged=true` seule sur un sol | elle alimente ses voisines à 1 et fait pousser **le losange d'une source** |
| coulée d'eau rencontrant une dalle `waterlogged=false` | la dalle **reste sèche** et l'eau la **contourne** |

Le second point est le plus important : **un écoulement ne gorge jamais rien
d'eau.** Le waterlogging est le fait d'une *pose* (seau, placement de bloc),
pas de l'écoulement. Relevé du contournement, source en 0, dalle en +2 :

```
2101s56
```

Le +3 est à **5** et non à 3 : l'eau descend d'un cran, fait deux pas vers
l'est, remonte, et paie chacun des cinq pas.

---

## 7. L'éponge

Une éponge posée dans une flaque stabilisée l'absorbe **entièrement** et devient
`minecraft:wet_sponge`. La flaque mesurée (rayon 7) tenait sous le plafond de 65
blocs, donc **ni le rayon de 7 ni le plafond de 65 n'ont été mesurés ici** — ils
sont implémentés d'après la documentation et testés unitairement, pas contre
l'oracle. C'est le seul endroit de ce document où un chiffre n'est pas mesuré,
et il est signalé comme tel.

---

## 8. La sérialisation Anvil des ticks

Le format a été lu dans une sauvegarde prise **pendant** que l'eau coulait
encore : une flaque stabilisée n'a plus aucun tick en attente et le chunk ne
contient alors ni `block_ticks` ni `fluid_ticks`.

Les deux listes vivent au premier niveau du compound de chunk. Chaque entrée :

```
{i: String, p: Int, t: Int, x: Int, y: Int, z: Int}
```

Relevé brut :

```
block_ticks: {i: "minecraft:oak_leaves",  p: 0, t: 0, x: 176, y: -57, z: 189}
fluid_ticks: {i: "minecraft:flowing_lava", p: 0, t: 8, x: 719, y: -60, z: -3}
fluid_ticks: {i: "minecraft:lava",         p: 0, t: 0, x: 3839, y: 32, z: -34}
```

Deux points valent d'être écrits noir sur blanc :

1. **Un tick de bloc nomme le bloc ; un tick de fluide nomme le fluide.** Et le
   registre des fluides distingue `minecraft:water` de `minecraft:flowing_water`,
   `minecraft:lava` de `minecraft:flowing_lava`. Écrire le nom du bloc dans un
   `fluid_ticks` produit un fichier que vanilla charge en **perdant silencieusement**
   tous les écoulements en cours.
2. **`t` est un délai relatif au temps de jeu du chunk**, pas un tick absolu.
   C'est ce qui permet à un chunk de rester déchargé une heure et de reprendre
   où il en était.

`p` est la `TickPriority` de vanilla, de -3 à 3, **la plus basse s'exécutant en
premier**, et `p` est absent de certaines entrées — un `p` manquant vaut Normal,
il ne rend pas l'entrée illisible.

---

## 9. Parité obtenue

Chaque scénario est rejoué dans `src/ov_gameplay/tests/test_fluid.cpp` contre un
monde de test en mémoire, et comparé case par case au relevé.

| scénario | positions identiques |
|---|---|
| flaque d'eau sur sol plat | 361 / 361 |
| trou hors de portée (6) | 289 / 289 |
| trou à l'est, distance 4 | 289 / 289 |
| trou derrière un mur | 169 / 169 |
| flaque de lave, overworld | 169 / 169 |
| flaque de lave, Nether | 169 / 169 |
| deux trous équidistants | 81 / 81 |
| trou en diagonale, distance 5 | 81 / 81 |
| trou à l'est, distance 5 | 81 / 81 |
| eau infinie, deux sources alignées | 15 / 15 |
| waterlogging, dalle source | 15 / 15 |
| eau infinie refusée, diagonale | 9 / 9 |
| **total** | **1728 / 1728** |

### Et surtout : hors échantillon

Tout ce qui précède a servi à **écrire** la règle, donc le rejouer ne prouve que
la fidélité de la transcription. Quatre labyrinthes ont donc été tirés d'un
générateur pseudo-aléatoire à graine fixe — murs épars, trous à des distances
quelconques, aucun cas choisi pour être facile — joués sur le vrai serveur, puis
rejoués à l'identique dans le moteur (`scripts/measure_fluid_maze.py`).

| labyrinthe | murs | trous | positions identiques |
|---|---|---|---|
| `maze_1` (graine 1) | 40 | 2 | 361 / 361 |
| `maze_2` (graine 2) | 55 | 1 | 361 / 361 |
| `maze_3` (graine 3) | 25 | 4 | 361 / 361 |
| `maze_4` (graine 4) | 70 | 3 | 361 / 361 |
| **total** | | | **1444 / 1444** |

`maze_4` est le plus parlant : avec 70 murs, la source trouve un trou à deux
blocs et l'eau ne fait que quatre cases. `maze_2`, avec un seul trou au milieu
d'un champ de murs, en fait une soixantaine en serpentant. Les deux sortent
identiques.

**Total général : 3172 / 3172 positions.**

S'y ajoutent les vérifications ponctuelles, hors grille : les trois cas du
tableau de mélange, la colonne en chute et son niveau 1 au sol, le refus de
l'eau infinie pour la lave, le contournement de la dalle, les trois délais de
tick, et les noms de ticks `flowing_*`.

---

## 10. Pièges rencontrés

* **`forceload add` échoue en silence au-delà de 256 chunks.** Un seul rectangle
  couvrant les vingt bandes en demandait 544 ; les scénarios au-delà de la
  distance de vue n'ont jamais tické et la sauvegarde a rendu **de l'air partout
  sans rien signaler**. Une commande par bande, 3 × 3 chunks chacune.
* **Un `fill` qui dépasse le plancher du monde échoue en bloc.** Le premier
  scénario de chute creusait jusqu'à y = -65 ; rien n'a été creusé et le
  scénario a rendu une flaque ordinaire, parfaitement plausible et fausse. Les
  plateformes de la seconde passe sont **construites en l'air**.
* **La course eau/lave cache la moitié du tableau de mélange** (§ 5).
* Dans un lecteur NBT écrit en Python, `out[r.s()] = payload(r, ct)` lit la
  **valeur avant le nom** : Python évalue la partie droite en premier et tout le
  flux se décale d'un tag. Le nom doit passer par une variable.
