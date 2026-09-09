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

## 8. Ce qui n'est pas fait

Nommé plutôt que caché :

* **La grille 2×2 de l'inventaire du joueur** est appariée par `ov_gameplay` et
  testée, mais le serveur ne câble pas encore l'écran de la fenêtre 0 : le code
  de fenêtre existant y répond pour le coffre, et le partager demandait une
  refonte de `server.cpp` que la parallélisation en cours interdit.
* **La table de forge et la pierre de taille** ont leur appariement
  (`match_smithing`, `stonecutting_options`) et leurs tests, mais pas leur
  fenêtre.
* **Les recettes spéciales** sont déclarées, pas appariées. Les quatorze noms
  sont ci-dessus.
* **Le placement automatique depuis le livre de recettes** (`Place Recipe`)
  n'est pas implémenté ; le déverrouillage l'est.
* **Un four ne tourne que pendant que quelqu'un le regarde.** La file de ticks
  de blocs arrivée avec les fluides (`ov_world/block_ticks.hpp`) porte des
  positions, pas des bloc-entités ; brancher les fours dessus demande de leur
  donner un état persistant côté monde, et c'est un chantier à part. En
  attendant, un four que personne n'a ouvert ne cuit pas — dit ici plutôt que
  découvert au retour.
* **L'expérience** est accumulée et remise à zéro à la récupération, mais rien
  ne la matérialise : les orbes appartiennent à un autre jalon.
* Le code de clic générique vit dans `src/ov_server/src/workbench.cpp`. Le
  chemin du coffre dans `server.cpp` devrait y être ramené une fois la vague de
  travail parallèle atterrie ; l'y ramener maintenant aurait rendu la fusion
  impossible.
