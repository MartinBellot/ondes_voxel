# Fabrication et fonte — d'où viennent les chiffres

Ce document trace ce qui a été établi pour le système de recettes, comment, et
les pièges rencontrés. Il complète `docs/PROVENANCE.md` ; les sources
documentaires générales y restent.

---

## 1. Ce qui est data, ce qui est code

En 1.20.1 les recettes vivent vraiment dans le datapack. Les 1174 fichiers de
`data/minecraft/recipes/` sont régénérés localement par
`tools/ov_datagen/datagen.py` et **jamais commités**. Ce que le dépôt écrit,
c'est l'**appariement**, pas une table de recettes.

Trois choses, en revanche, ne sont dans aucune donnée — ce sont des classes Java
en vanilla — et ont donc été **mesurées** sur un vrai serveur 1.20.1 :

| Quoi | Script | Sortie |
|---|---|---|
| Temps de combustion par objet et par four | `scripts/measure_fuel.py` | `normalized/fuel.json` |
| Restes de fabrication (`craftingRemainingItem`) | `scripts/measure_fuel.py` + `scripts/measure_crafting.py` | idem + `normalized/crafting.json` |
| Ce que le jeu met dans la case de sortie pour une grille donnée | `scripts/measure_crafting.py` | `normalized/crafting.json` |

---

## 2. Les recettes qui se chargent, et celles qui sont refusées

`tools/ov_datagen/recipes.py` aplatit les 1174 fichiers en trois tableaux —
recettes, ingrédients, choix — que `ov_registry` lit sans rien analyser. Les
tags d'ingrédients (`#minecraft:planks`) sont développés **à la compilation** en
listes d'ids triées, pour la même raison que les tags de blocs : résoudre un tag
à chaque case de grille mettrait une recherche de chaîne dans le chemin d'une
fabrication.

Les neuf genres data-driven sont appariés : `crafting_shaped`,
`crafting_shapeless`, `smelting`, `blasting`, `smoking`, `campfire_cooking`,
`stonecutting`, `smithing_transform`, `smithing_trim`.

**Quatorze recettes ne sont pas appariées, et les voici nommées** — leur fichier
ne contient qu'un type et une catégorie, parce qu'en vanilla ce sont des classes
Java sans données :

```
minecraft:crafting_decorated_pot
minecraft:crafting_special_armordye
minecraft:crafting_special_bannerduplicate
minecraft:crafting_special_bookcloning
minecraft:crafting_special_firework_rocket
minecraft:crafting_special_firework_star
minecraft:crafting_special_firework_star_fade
minecraft:crafting_special_mapcloning
minecraft:crafting_special_mapextending
minecraft:crafting_special_repairitem
minecraft:crafting_special_shielddecoration
minecraft:crafting_special_shulkerboxcoloring
minecraft:crafting_special_suspiciousstew
minecraft:crafting_special_tippedarrow
```

Elles sont **conservées comme déclarations** : elles partent dans
`Update Recipes` pour que le livre de recettes du client les connaisse, et
portent le drapeau `kFlagDeclarationOnly`, que l'appariement saute. Les traiter
comme des recettes vides ferait fabriquer un feu d'artifice avec une grille
vide.

`smithing_trim` porte le même drapeau, pour une autre raison : son résultat est
la pièce d'armure d'entrée, ornée. Il n'existe pas comme objet, et `-1` le dit —
`0` voudrait dire `minecraft:air`, ce qui est une autre affirmation.

### Le piège de la longue-vue

Un seul motif de 1.20.1 n'est pas rogné dans le fichier :

```json
"pattern": [" # ", " X ", " X "]
```

Trois cases de large pour une colonne utile. Le jeu rogne les lignes et colonnes
vides au chargement ; sans cette coupe, le motif exige une empreinte de trois de
large et **la longue-vue ne se fabrique jamais** — un seul objet manquant, dans
un jeu qui a l'air de marcher. `recipes.trim()` s'en charge et compte ce qu'il
rogne.

---

## 3. Les temps de combustion — la mesure et son piège

`AbstractFurnaceBlockEntity.getFuel()` est du code Java. Le protocole retenu
repose sur une identité exacte : un four allumé décrémente `BurnTime` d'un par
tick, donc

    C = BurnTime(t) + t

est **constant** pendant toute la combustion et vaut le tick d'extinction. Une
lecture datée suffit, à condition que la date et la valeur partent dans le même
lot de commandes — le serveur exécute un lot dans un seul tick.

La durée cherchée est `F = C − t_allumage`, et `t_allumage = T_pose + k` où `k`
est un petit entier inconnu.

**Le piège :** encadrer `k` par la console ne marche pas. Un aller-retour console
ne descend jamais sous deux ticks, si bien que l'encadrement ne se referme
jamais et laisse `k ∈ {0, 1}` même après quatorze essais. Un tick d'erreur ici
se retrouve tel quel dans les 248 durées écrites.

**La sortie :** un **bloc de commande répétitif** s'exécute exactement une fois
par tick. Un compteur qu'il incrémente tant que le bloc est `lit` donne la durée
sans aucune convention — le décalage de phase entre le bloc de commande et la
bloc-entité s'applique identiquement à l'allumage et à l'extinction, donc
s'annule. Trois combustibles courts donnent tous `k = 1`, pour les trois fours.

Cela demande `enable-command-block=true`, d'où l'attribut `EXTRA_PROPERTIES`
ajouté à `measure_entities.Server`.

### Résultats

* **248 combustibles** sur 1254 objets candidats — tous les objets du registre
  ont été mis dans un four et observés, donc « pas un combustible » est une
  mesure, pas un défaut.
* `k = 1` pour les trois fours.
* La pente a été vérifiée : `C` est identique sur deux lectures datées pour les
  248 fours des trois grilles.
* Trois tables sont écrites dans le pack, pas une et un diviseur. Le rapport
  four / haut-fourneau n'est **pas** exactement 2 partout : le bloc d'algues
  séchées vaut 4001 dans un four et **2000** dans un haut-fourneau, pas 2000,5.
  Un diviseur appliqué à l'exécution arrondirait dans le mauvais sens pour
  exactement les objets où cela se voit.
* Un seul objet rend quelque chose depuis la case à combustible : le seau de
  lave rend un seau.

### Le piège de la charge

Les trois fours n'acceptent pas la même chose. Un haut-fourneau chargé de pavé
ne s'allume jamais, un fumoir chargé de minerai non plus — et une grille qui ne
s'allume pas donne une table vide qui ressemble à « aucun objet n'est un
combustible ». Chaque four est donc chargé de ce qu'il sait cuire : pavé,
minerai de fer, pomme de terre.

---

## 4. L'ordre du tick d'un four

La mesure fixe l'ordre, elle ne le suppose pas. Un four posé au tick `T` avec du
combustible et de quoi cuire s'allume au tick `T+1`, et `BurnTime` vaut alors la
valeur pleine — **il n'est pas décrémenté dans le tick de l'allumage**. Le
compteur du bloc de commande a vu exactement 100 ticks allumés pour un bâton, et
la lecture datée donnait `C − T_pose = 101`.

`gameplay::furnace_tick` fait donc, dans cet ordre : décrémenter si allumé,
allumer si éteint et qu'il y a de quoi, puis cuire. Allumer *après* avoir
décrémenté est ce qui fait durer un combustible exactement son nombre de ticks
mesuré plutôt qu'un de moins.

La progression perdue vaut deux ticks par tick éteint, ce qui empêche de cuire
quoi que ce soit avec une poignée de bâtons donnés un par un.

### Le four que personne ne regarde

Un four est un bloc-entité tické : il cuit, que quelqu'un le regarde ou non.
`src/ov_server/src/furnace_entity.cpp` fait tourner à chaque tick tous les fours
des chunks chargés, dans chaque dimension : une passe par dimension (overworld,
Nether, End), chacune sur ses propres chunks. Chaque passe tient un index
reconstruit une fois par seconde ; un four qu'on ouvre ou qu'on clique y entre
aussitôt. Le NBT du
bloc-entité est **la seule copie** du four : `Items`, `BurnTime`, `CookTime` et
`CookTimeTotal` (des shorts, écrits sur place), plus `RecipesUsed`. L'écran d'un
four ne fait plus rien tourner : il relit le bloc-entité avant chaque clic et à
chaque rafraîchissement.

Trois bugs de la version précédente, trouvés à la lecture et épinglés par
`src/ov_server/tests/test_furnace_entity.cpp` :

* la passe ne parcourait que l'overworld, et l'écran d'un four lisait
  l'overworld aux mêmes coordonnées : un four du Nether ou de l'End ne cuisait
  pas et ne s'ouvrait même pas. Chaque dimension a maintenant sa passe (c'est
  ce que le test épingle) et l'écran lit la dimension du joueur ;

* la passe des fours non regardés n'écrivait les compteurs que quand une case
  changeait. Elle relisait donc à chaque tick un `BurnTime` et un `CookTime`
  périmés : un four fermé brûlait sans fin et ne finissait jamais un objet ;
* l'écran allumait son four par `Chunk::set_block`, qui efface le bloc-entité
  de la position écrite. Un four qui s'allumait pendant qu'on regardait son
  écran perdait son minerai, son combustible et son expérience.
  `relight_furnace_block` change la propriété `lit` en gardant le bloc-entité.

L'expérience suit le format de vanilla : un compte par recette dans
`RecipesUsed`. À l'extraction, chaque recette donne `nombre × expérience` en
simple précision : la partie entière, plus un point avec une probabilité égale
à la partie fractionnaire. Le tout est versé en orbes aux pieds du joueur.
L'ancien champ `ovExperience`, propre à ce projet, est effacé à la première
écriture.

Mesuré sur le vrai serveur par `scripts/measure_furnaces.py` (campagnes `xp`
et `xp-iron`), avec une sonde qui vide la sortie au shift-clic :

| Cas | Vanilla |
|---|---|
| 10 lingots de fer, `RecipesUsed` = 10 (10 × 0,7 = 7) | **7 points, 6 fois sur 6** |
| 5 pierres, `RecipesUsed` = 5 (5 × 0,1 = 0,5) | **1 point 18 fois sur 40**, 0 sinon |
| Sortie vidée par un joueur | `RecipesUsed: {}` |
| Sortie vidée par un entonnoir dessous | les 3 lingots dans l'entonnoir, `RecipesUsed` **gardé** (3) |
| Un lingot lancé depuis la sortie (mode 4, bouton 0) | 2 lingots restent, `RecipesUsed` **vidé**, 2 points (3 × 0,7) |
| La pile lancée depuis la sortie (mode 4, bouton 1) | sortie vide, `RecipesUsed` vidé, 2 points |
| Touche numérique vers une case vide de la barre (mode 2) | la pile y va, `RecipesUsed` vidé, 2 points |
| Touche numérique vers une case occupée (un pavé) | rien ne bouge, `RecipesUsed` gardé, 0 point |

Lancer un seul objet paie donc **tout** `RecipesUsed`, comme prendre la pile.

La touche numérique vers une case de la barre qui tient **déjà le même objet**
(un lingot) a été mesurée une fois : rien ne bouge, `RecipesUsed` reste à 3, et
la sonde relève pourtant 2 points. Ce n'est pas expliqué, donc pas repris : ce
serveur n'y déplace rien et ne paie rien.

### Le piège du niveau

`xp query … points` ne compte que les points **dans le niveau courant**. Sept
points font exactement le niveau 1 et s'y lisent 0 : la première campagne a lu
0, 7, 0, 0 pour dix lingots de fer. La sonde additionne maintenant les niveaux,
les points et les orbes encore au sol. Deux autres pièges du même relevé :
`setblock` sur un four identique répond « Could not set the block » et
n'applique pas le NBT (chaque four est donc posé sur de l'air), et une sonde
qui flotte un bloc au-dessus du sol est expulsée au bout de quatre secondes.

### Douze fours sans joueur

`scripts/measure_furnaces.py ticks` pose douze fours par `/setblock`, avec leur
NBT et `CookTimeTotal`, sans aucun joueur connecté, et les relit à dt = 22,
153, 603, 1202, 1702 et 2503 ticks. dt est exact : pose et lecture partent dans
le même lot qu'un `time query gametime`. `BurnTime`, `CookTime`,
`CookTimeTotal` et les trois cases, à chaque date, sont repris tels quels par
le test de parité de `src/ov_server/tests/test_furnace_entity.cpp`. Ce que la
table fixe :

* un charbon cuit exactement 8 objets, dans un four comme dans un haut-fourneau
  ou un fumoir (100 ticks par objet, 800 de combustible) ; un bloc d'algues
  séchées fait 20 lingots dans un haut-fourneau ;
* un bâton seul (100 ticks) ne cuit rien, deux bâtons cuisent une pierre : le
  second s'allume au tick même où le premier s'éteint, et la cuisson ne perd
  rien (`CookTime` 153, `BurnTime` 48 à dt = 153) ;
* une sortie qui ne peut rien recevoir (un lingot d'or devant du fer) n'allume
  jamais le four ; une sortie qui arrive à 64 **laisse brûler** le combustible
  déjà allumé sans plus rien cuire ;
* un four qui ne peut pas cuire **garde** son `CookTimeTotal` (200 ou 100). Ce
  serveur le remettait à 0 : corrigé dans `gameplay::furnace_tick` ;
* le seau de lave laisse un seau dans la case de combustible.

### Le piège du `CookTimeTotal` manquant

La première campagne a posé ses fours sans `CookTimeTotal` : aucun n'a rien
produit en 2500 ticks. Le témoin de la seconde le confirme : il brûle son
charbon et compte `CookTime` jusqu'à 1202 sans jamais finir un objet. Vanilla
ne recalcule donc pas le total à chaque tick, et un objet ne sort qu'à
l'égalité exacte des deux compteurs.

Ce serveur fait maintenant de même, sans que chaque écrivain de la case
d'entrée (clic, entonnoir, commande) ait à le signaler. La passe des fours
garde, pour chaque four, l'entrée telle que son dernier tick l'a laissée :
l'objet et ses tags, pas le nombre. Au tick suivant, une entrée différente est
un changement venu d'ailleurs : `CookTime` repart de 0 et `CookTimeTotal` est
relu sur la recette. Sinon le total stocké sert tel quel, 0 compris. La passe
retrouve les fours à chaque tick et tourne avant les entonnoirs et les joueurs,
pour voir un four posé, chargé ou écrit par une commande avant quiconque.

Mesuré sur le vrai serveur (`scripts/measure_furnaces.py changes`) : huit fours
avec 8 minerais de fer et un charbon, leur entrée changée par
`item replace … container.0` au tick 52, relus 251 ticks plus tard.

| Changement | Juste après | 251 ticks plus tard |
|---|---|---|
| du sable à la place du minerai | `CookTime` 0, total 200 | 1 verre, `CookTime` 51 |
| de la terre (rien à cuire) | 0, total **200 gardé** | rien, le feu brûle |
| l'entrée retirée | 0, total 200 gardé | rien |
| le même minerai, 3 au lieu de 8 | **52, pas de remise à zéro** | 1 lingot, `CookTime` 103 |
| le même minerai avec un tag (`display.Name`) | 0 | 1 lingot, `CookTime` 51 |
| retiré puis remis dans le même tick | **0** | 1 lingot, `CookTime` 51 |
| témoin sans total, le même minerai (7) | 52, total 0 | **toujours bloqué**, `CookTime` 303 |
| témoin sans total, du fer brut | 0, total 200 | 1 lingot, `CookTime` 51 |

Le test `an input changed by someone else…` de `test_furnace_entity.cpp`
rejoue ces cas, sauf le retrait et la remise dans le même tick. Restent :

* **l'écart résiduel, mesuré** : le même objet retiré puis remis dans un seul
  tick fait deux changements pour vanilla, qui remet la progression à 0, et
  aucun pour une comparaison faite une fois par tick. Ce serveur garde la
  progression ;
* **non distingué** : pour une entrée qui ne cuit pas, ou vide, vanilla lit un
  total de 200 là où il y avait 200. Ce serveur garde le total stocké. Un
  haut-fourneau (100) dont l'entrée devient de la terre n'a pas été mesuré.

---

## 5. L'oracle d'appariement — 2885 grilles, 2885 identiques

`scripts/measure_crafting.py` fait remplir des grilles à un vrai serveur et lit
sa réponse. La technique :

* `Set Creative Slot` écrit dans le **menu d'inventaire du joueur**, que le
  serveur applique quel que soit l'écran ouvert ; les cases 36 à 44 sont la
  barre d'action, partagée avec la fenêtre de l'établi ;
* `Click Container` en mode 2 échange une case de la fenêtre avec une case de la
  barre d'action — une case de grille remplie par paquet.

Trois familles de grilles sont posées :

1. la grille exacte de chaque recette façonnée, **dans toutes les positions où
   son motif tient et dans son miroir horizontal** ;
2. les ingrédients de chaque recette informe, mélangés de façon déterministe
   (graine 1234567890) ;
3. 120 grilles aléatoires qui ne doivent **rien** donner — c'est la moitié qui
   attrape un appariement trop généreux, puisque tout ce qui doit marcher
   marchera.

La sortie est convertie par `scripts/check_crafting.py` en une table plate que
lit `test_crafting_parity`, dans `ov_gameplay` : le test C++ n'a pas
d'analyseur JSON et n'en a pas besoin.

**Le chiffre : 2885 grilles posées sur le vrai serveur, 2885 verdicts
identiques au nôtre.** 2533 grilles façonnées (chaque recette dans chaque
position où son motif tient, et son miroir), 232 informes mélangées, 120
aléatoires. 2766 donnent un résultat, 119 n'en donnent aucun — dont 119 des 120
aléatoires, la cent-vingtième tombant par hasard sur une recette réelle.

### Le piège du curseur

Un clic ordinaire sur la case de sortie met le résultat **sur le curseur**. Le
jeter n'est pas le mode 4 mais le **mode 0 sur la case -999** ; le mode 4 sur
-999 ne fait rien, et ne le dit pas. La première campagne de restes a donc
gardé le premier objet fabriqué en main, et le jeu a refusé toutes les
fabrications suivantes — silencieusement. Résultat lu : « aucun objet ne laisse
de reste ». La sonde emploie maintenant un shift-clic, qui envoie le résultat
dans l'inventaire et ne touche pas au curseur.

Deux corollaires, appris de la même façon : vider la barre d'action **avant**
l'échange (sinon l'objet fabriqué y retourne dans la grille), et vider aussi
l'inventaire du joueur entre deux recettes (un inventaire plein fait refuser le
shift-clic, silencieusement encore).

### Trois restes, et ce sont les trois

`milk_bucket` et `lava_bucket` rendent un `bucket`, `honey_bottle` rend un
`glass_bottle`. Rien d'autre, sur les 822 recettes de fabrication et les 248
combustibles.

### Le piège de la mémoire

Sur une machine qui compile en même temps, macOS tue la JVM en cours de
campagne et ne le dit nulle part : le journal du serveur s'arrête au milieu
d'une ligne et la sonde reçoit une fin de flux. Trois passes ont été perdues
ainsi avant que le banc n'apprenne à se remonter et à reprendre aux recettes
qu'il n'avait pas faites. Le tas de la JVM est aussi descendu à 768 Mo.

### Le piège du NBT dans une case

La sonde doit sauter le NBT d'une pile en entier. S'arrêter au premier octet
décale toutes les cases suivantes du paquet — et une grille décalée ressemble à
une grille valide. `skip_nbt` parcourt le tag complet.

### Le piège de l'ordre des champs NBT

`data get block … Items` imprime `Slot`, puis `id`, puis `Count` — pas l'ordre
qu'on suppose. Une expression régulière qui cherche l'autre ordre ne rate pas
bruyamment : elle rend « aucun reste », et la première passe de mesure a
effectivement conclu que le seau de lave ne rendait rien.

---

## 6. Les identifiants de paquets — relevés, pas recopiés

`scripts/capture_recipe_packets.py` se connecte à un vrai serveur 1.20.1, ouvre
un four, déverrouille les recettes, et note ce qui arrive.

| Paquet | Identifiant | Comment il a été établi |
|---|---|---|
| Update Recipes | **0x6D** | le plus gros paquet de la session (136 056 octets), relu champ par champ |
| Update Recipe Book | **0x3D** | provoqué par `recipe give`, puis relu |
| Set Container Property | **0x13** | 161 exemplaires de cinq octets pendant qu'un four brûle |
| Open Screen | 0x30 | confirme la constante déjà présente |

`Update Recipes` est **relu champ par champ** : un ordre de champs faux ne rate
pas doucement, il désynchronise à la première recette et le décodage n'atteint
jamais la fin du paquet. Il s'est arrêté **pile sur l'octet 136 056 après 1174
recettes**, ce qui est la preuve que la disposition écrite par
`src/ov_protocol/src/recipe_packets.cpp` est la bonne.

Un détail contre-intuitif de ce paquet : le **type** vient avant l'identifiant,
et non l'inverse. Le client lit le type pour savoir quel lecteur employer.

### Le piège de 0x3B

`Update Recipe Book` avait d'abord été écrit à `0x3B`, de mémoire. Il vaut
`0x3D`. Il n'arrive pas à la connexion d'un joueur neuf — son livre est vide et
le serveur n'a rien à annoncer — donc il ne se trouve pas en écoutant une
connexion : il faut le provoquer. Une fois provoqué, il se décode exactement,
avec **1159 recettes déverrouillées sur 1174** : le vrai serveur en retient
quinze, celles qu'un livre ne sait pas poser.

Ce serveur en retient trente — les quatorze spéciales et les seize
`smithing_trim` — c'est-à-dire un **sous-ensemble** de la liste vanilla et non
un sur-ensemble. Dire à un client de ranger dans son livre une recette qu'il ne
sait pas classer est une façon de le faire tomber ; lui en envoyer moins n'en
est pas une.

---

## 7. Le test de bout en bout

`scripts/check_workbench_e2e.py` sert une copie du banc (`run/lab`, parcelle
« workbenches », x 0 z 160) avec notre propre serveur, y connecte une sonde qui
parle le protocole 763 et rien d'autre, puis :

1. ouvre l'établi de la parcelle ;
2. y fabrique une pioche en bois, et vérifie que la grille s'est consommée ;
3. fabrique un four avec huit pavés ;
4. **pose ce four**, l'ouvre, y met du minerai de fer et du charbon ;
5. voit les deux barres bouger — `Set Container Property` — et le lingot sortir ;
6. le récupère au shift-clic et vérifie que la case de sortie se vide.

Tout passe. La réserve à énoncer : ce n'est pas le client graphique, qui ne
s'automatise pas ici. C'est un client qui envoie exactement les mêmes paquets,
dans le même ordre ; ce que le test prouve, c'est que le serveur y répond
correctement.

## 7 bis. La fenêtre 0 du joueur et sa grille 2×2

`scripts/measure_window0.py` fait cliquer une sonde en survie dans la fenêtre 0
d'un vrai serveur 1.20.1. Cette fenêtre n'a pas d'`Open Screen` ; elle compte
46 cases : 0 le résultat, 1 à 4 la grille, 5 à 8 l'armure, 9 à 35 le sac, 36 à
44 la barre, 45 la main secondaire. La sonde relit la fenêtre, le curseur,
l'inventaire (`data get entity`) et le sol. Quatre bûches de chêne dans la
grille :

| Clic sur le résultat | Vanilla |
|---|---|
| clic gauche ou droit | une fabrication (4 planches) sur le curseur |
| shift-clic | toutes les fabrications, l'inventaire rempli **par la fin** : 16 planches dans la dernière case de la barre |
| touche numérique vers une case vide | une fabrication dans cette case |
| touche numérique vers une case occupée | rien : ni déplacé, ni fabriqué |
| lancer, un bouton ou l'autre | **une** fabrication, lancée entière (4 planches) |

| Autre cas | Vanilla |
|---|---|
| fermer, la grille et le curseur pleins | le curseur revient d'abord (barre 0), puis la grille (barre 1) |
| shift-clic sur un casque de fer du sac | la case de la tête |
| shift-clic sur un bouclier du sac | la main secondaire |
| double clic, 10 terres au curseur, 5 et 3 ailleurs | 18 au curseur |
| 2 bouteilles de miel, un clic | 3 sucres au curseur ; la bouteille vide en barre 0, la case tenant encore du miel |
| 2 bouteilles de miel, shift-clic | 6 sucres en fin de barre, une bouteille vide en barre 0, la dernière dans la case vidée |

Ce serveur rendait la touche numérique et le lancer comme un clic (sur le
curseur), remplissait le sac d'abord, ne rendait rien à la fermeture,
n'équipait rien au shift-clic, ignorait le double clic et rangeait les restes
au sac d'abord. Tout est aligné et rejoué par `test_player_inventory.cpp`. La
sauvegarde range désormais le curseur avant la grille, dans l'ordre de la
fermeture.

Non mesuré : la fusion avec une pile existante à la fermeture (l'inventaire
mesuré était vide ; ce serveur fusionne d'abord, comme `Inventory.add`) ;
l'ordre du double clic entre piles pleines et non pleines ; les autres objets
portables (élytres, citrouille, têtes, d'après la liste du wiki) ; la touche
numérique vers une case qui tient déjà le même objet.

## 7 ter. La table de forge — mesurée sur le fil

`scripts/measure_smithing.py` : une sonde en survie ouvre une table de forge sur
un vrai serveur 1.20.1 et relit `Open Screen`, `Set Container Content` (NBT
décodé) et l'inventaire une fois l'objet pris.

* **Le menu** : type **20** (`minecraft:smithing`), titre
  `{"translate":"container.upgrade"}`, **40 cases** : 0 le gabarit, 1 la base,
  2 l'ajout, 3 le résultat, puis les 27 cases du sac et les 9 de la barre.
* **Une garniture** (gabarit côte, lingot d'or, plastron de fer) : le résultat
  est la base, avec `Trim: {material: "minecraft:gold", pattern:
  "minecraft:coast"}` ajouté à son tag. Le reste du tag est gardé : `Damage`,
  et la couleur `display.color` d'un plastron de cuir teint. La **même**
  garniture une seconde fois ne donne rien ; un autre matériau remplace le
  `Trim`. Le matériau vient de l'ajout (`trim_material/*.json`, `ingredient`),
  le motif du gabarit (`trim_pattern/*.json`, `template_item`).
* **L'amélioration en netherite** (épée de diamant usée de 10, tranchant II,
  renommée) : l'objet change, le tag passe **entier** : usure, enchantements,
  nom.
* Une combinaison qu'aucune recette n'accepte (gabarit de garniture, épée,
  or) : rien.
* **La prise** coûte un de chaque entrée : deux gabarits et deux lingots en
  laissent un de chaque. Le **shift-clic** sur le résultat remplit par la fin
  (la dernière case de la barre). Depuis le sac, le shift-clic envoie le
  gabarit en 0, l'armure en 1, le lingot en 2.
* À la fermeture, les entrées reviennent à l'inventaire (barre 0, 1, 2).

L'ordre des clés d'un compound NBT sur le fil suit une table de hachage chez
vanilla : il ne porte aucun sens et n'est pas reproduit.

## 7 quater. Le tailleur de pierre — l'ordre des boutons

`scripts/measure_stonecutter.py` : le client choisit une coupe par son
**indice** (`Click Container Button`, 0x0A) dans une liste qu'il calcule
lui-même ; le serveur doit calculer la même, dans le même ordre. La sonde clique
chaque indice et lit la case de résultat.

* **Le menu** : type **23** (`minecraft:stonecutter`), titre
  `{"translate":"container.stonecutter"}`, **38 cases** : 0 l'entrée,
  1 le résultat, puis 27 + 9.
* **L'ordre** : par nom de l'objet produit. Pour la pierre : briques de pierre
  sculptées, dalle de briques, escalier de briques, muret de briques, briques,
  dalle, escalier. Andésite, grès, bloc de cuivre, pavé, briques de boue et
  pierre noire (12 coupes) suivent la même règle. Tous les produits du tailleur
  sont des blocs : trier par nom ou par clé de traduction revient au même.
* La propriété de fenêtre 0 renvoie l'indice choisi ; un indice au-delà de la
  liste ne change rien (ni propriété, ni résultat).
* Une prise coûte une entrée et **garde** le choix ; le shift-clic taille toute
  l'entrée ; un autre objet dans l'entrée **efface** le choix.

## 8. Ce qui n'est pas fait

Nommé plutôt que caché :

* **La table de forge et la pierre de taille** ont leur appariement
  (`match_smithing`, `stonecutting_options`) et leurs tests, mais pas leur
  fenêtre.
* **Les recettes spéciales** sont déclarées, pas appariées. Les quatorze noms
  sont ci-dessus.
* **Le placement automatique depuis le livre de recettes** (`Place Recipe`)
  n'est pas implémenté ; le déverrouillage l'est.
* Le code de clic générique vit dans `src/ov_server/src/workbench.cpp`. Le
  chemin du coffre dans `server.cpp` devrait y être ramené une fois la vague de
  travail parallèle atterrie ; l'y ramener maintenant aurait rendu la fusion
  impossible.
