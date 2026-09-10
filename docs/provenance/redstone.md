# Redstone — ce qui a été mesuré, et comment

La redstone n'apparaît nulle part dans les rapports du data generator de Mojang :
c'est du code Java de bout en bout. Rien ici n'est déduit d'une formule ni d'une
autre implémentation. Tout vient d'un serveur 1.20.1 réel
(`tools/vanilla/server.jar`, SHA-1 `84194a2f286ef7c14ed7ce0090dba59902951553`),
interrogé par `scripts/measure_redstone.py`.

**Méthode.** Construire le circuit avec `/setblock` sur un monde superflat gelé
(`randomTickSpeed 0`, `doFireTick false`, `doDaylightCycle false`, `peaceful`),
laisser reposer, `save-all flush`, puis relire les états de blocs dans les
fichiers de région avec `ov_inspect state`. Quand le résultat ne survit pas à une
sauvegarde — le burn-out d'une torche dure 160 ticks et une sauvegarde en prend
plus — la lecture se fait en direct avec `execute if block`, dont le
« Test passed » arrive sur la console dans la seconde.

Les mesures sont écrites dans `data/vanilla/1.20.1/normalized/redstone_*.json`
(gitignoré). Les tableaux qui en découlent sont recopiés dans le code, à leur
point d'usage, avec le nombre de blocs mesurés.

---

## 1. Le piège qui a coûté deux campagnes : `setblock` ne prévient que six voisins

`/setblock` pose le bloc puis notifie ses **six voisins directs**, et s'arrête
là. Toute mesure qui suppose qu'une source posée « quelque part » se propage
jusqu'au capteur est fausse, et elle est fausse **silencieusement** : le capteur
lit la valeur qu'il avait avant.

Trois symptômes rencontrés, tous dus à cela :

| Symptôme | Cause | Correctif |
|---|---|---|
| La pierre ne conduit pas la redstone (0 sur 987 blocs) | levier posé **après** le fil ; le fil est en diagonale du levier | poser le fil **en dernier** : il calcule sa propre force à la pose |
| Une torche ne s'éteint jamais | levier posé sous le bloc support, la torche est deux blocs plus loin | basculer aussi un bloc **à côté** de la torche, dont l'anneau de six la contient |
| Un piston ne réagit pas à une source diagonale | le piston était déjà posé | poser le **piston en dernier**, il se teste lui-même à la pose |

Ce n'est pas un artefact du protocole de mesure : c'est le comportement du jeu,
et c'est précisément pourquoi la quasi-connectivité s'appelle un *block update
detector*. Les deux ordres sont conservés dans la mesure (`qc_no_update` contre
`qc_diagonal_above`) plutôt que d'en garder un seul.

Second piège, plus court : **quatre blocs transportent de l'eau sans le dire.**
`kelp`, `kelp_plant`, `seagrass` et `tall_seagrass` n'ont pas de propriété
`waterlogged` et sont pleins d'eau ; un seul a noyé cinq cents cellules de la
grille. Tout ce qui a la propriété est posé `waterlogged=false`.

---

## 2. Ce qui conduit la redstone — 830 blocs mesurés

Sonde : un levier `face=floor` posé **sur** le bloc candidat (il n'alimente
fortement que le bloc directement sous lui), et un fil isolé à côté. Le fil lit
15 exactement quand le candidat relaie la puissance forte. La moitié de la grille
sans levier isole les blocs qui alimentent d'eux-mêmes, pour qu'une torche ne
soit pas prise pour un conducteur.

| Résultat | Nombre |
|---|---|
| Blocs candidats | 987 |
| Mesurés des deux côtés | 830 |
| Conducteurs | 346 |
| Sources en propre | 4 (`redstone_block`, `redstone_torch`, `redstone_wall_torch`, `daylight_detector`) |
| Incapables de porter un levier | 33 |
| Disparus avant la lecture | 124 |

**794 des 830 suivent la règle simple « cube de collision plein ».** Les 36 qui
ne la suivent pas sont la donnée utile :

- **34 cubes pleins qui ne conduisent pas** : les dix feuillages, les seize
  verres teintés, `glass`, `tinted_glass`, `ice`, `frosted_ice`, `sea_lantern`,
  `glowstone`, `beacon`, `observer`.
- **2 non-cubes qui conduisent** : `mud` et `soul_sand` — exactement la paire
  dont `connections.cpp` avait déjà trouvé que la face de support est pleine
  alors que la boîte de collision ne l'est pas. Deux mesures indépendantes, le
  même couple.

Ce qu'aucun raisonnement n'aurait donné : une **lampe de redstone**, une
**barrière**, un **bloc de slime**, une **cible** et toutes les **shulker boxes**
conduisent ; le **miel** et l'**observateur** non.

`piston` et `sticky_piston` ont demandé une seconde sonde, un répéteur pointant
dans le candidat au lieu d'un levier (un levier posé sur un piston le fait
sortir et le piston mange le fil témoin). Résultat : un piston **sorti** ne
conduit rien. Un piston rentré n'a pas pu être mesuré — la sonde qui l'alimente
est aussi ce qui le fait sortir — et il est traité comme le cube plein qu'il est.
**Cette hypothèse est la seule du fichier**, et elle est nommée ici plutôt que
cachée dans le code.

Trois blocs restent hors de portée de cette sonde : `chorus_flower` (rien pour
le tenir), `tnt` (le levier l'allume), et les coraux vivants (ils meurent hors de
l'eau — leurs versions mortes, elles, conduisent).

---

## 3. Le fil

Bloc de redstone en `x=0`, dix-neuf fils de `x=1` à `x=19` :

```
15 14 13 12 11 10 9 8 7 6 5 4 3 2 1 0 0 0 0
```

**19 positions sur 19** correspondent à `max(0, 16 - x)`. Un de moins par bloc,
et le fil s'éteint au seizième.

La règle de propagation qui reproduit cela, et dont chaque ligne compte :

1. la meilleure puissance venue d'un voisin **qui n'est pas un fil** ;
2. si elle vaut 15, on s'arrête ;
3. pour chacune des quatre horizontales : la puissance du fil voisin moins un,
   celle du fil **un cran plus haut** si ce côté est solide et que rien de solide
   n'est au-dessus de nous, ou celle du fil **un cran plus bas** si ce côté n'est
   pas solide.

Les conditions 3 ne sont pas symétriques, et les inverser fait passer le courant
à travers les plafonds.

**Le fil est invisible au fil, à travers un bloc.** Pendant qu'un fil calcule sa
propre force, tous les fils du monde sont muets. C'est ce qui fait que
`fil → bloc plein → fil` ne transporte rien alors que `fil → bloc plein →
répéteur` transporte 15. Vanilla le fait avec un booléen statique mutable ; ici
c'est un paramètre explicite (`wires_signal`), le projet n'ayant pas de global
mutable.

---

## 4. Puissance faible et puissance forte

La convention de direction est celle de vanilla et se lit à l'envers la première
fois : **la direction passée pointe du bloc alimenté vers la source.** Un levier
posé au sol est interrogé avec `Up`, et le bloc qu'il alimente est celui du
dessous. L'alternative — une direction sortante — est le même code avec tous les
appels inversés, et mélanger les deux est silencieux et total.

- **faible** : ce qu'un lampe, une porte, un piston lisent (« suis-je
  alimenté ? ») ;
- **forte** : ce qu'un bloc plein **relaie**. Un bloc plein alimenté fortement
  devient lui-même une source de niveau 15, pour le fil comme pour le répéteur.

Sans la distinction, la moitié des circuits jamais construits s'arrête, parce que
« fil dans un mur, répéteur de l'autre côté » est la manière standard de faire
passer un signal à travers une paroi.

Mesuré indirectement partout ci-dessus, et directement par la sonde du §2 : le
fil témoin ne lit 15 que si le candidat relaie de la puissance forte.

---

## 5. Le répéteur et le comparateur

| Cas | État lu |
|---|---|
| délai 1 à 4, alimenté | `powered=true`, sortie 15 — **4 sur 4** |
| verrouillé par un répéteur latéral alimenté | `locked=true`, `powered=false`, sortie **0** |
| même montage, répéteur latéral non alimenté | `locked=false`, `powered=true`, sortie **15** |

Le verrouillage ne se contente pas d'empêcher la mise à jour : un répéteur
verrouillé **ne planifie même pas**. Un changement qui arrive pendant qu'il est
verrouillé est perdu, pas mis en attente. C'est le point du verrouillage.

Seul un **diode** verrouille — un répéteur ou un comparateur pointant sur le
côté. Un levier collé au flanc d'un répéteur ne le verrouille pas.

**Tick par tick**, lu dans le `block_ticks` d'une sauvegarde attrapée en vol
(§7), un dispositif de chaque posé côte à côte et déclenchés ensemble :

| Dispositif | `t` demandé | `p` |
|---|---|---|
| répéteur délai 1 | 2 | −1 |
| répéteur délai 2 | 4 | −1 |
| répéteur délai 3 | 6 | −1 |
| répéteur délai 4 | 8 | −1 |
| comparateur | 2 | 0 |

**5 dispositifs sur 5.** Le délai est bien `2 × delay`, et — le point qui était
faux dans la première version du code — **les deux diodes n'utilisent pas la
même priorité** : le répéteur demande −1 là où le comparateur demande 0.

### Le comparateur, table complète

72 cellules : six niveaux d'entrée arrière × six niveaux d'entrée latérale ×
deux modes, chacune avec ses deux niveaux d'entrée **relus** et non supposés.

**72 calibrées sur 72, 72 conformes sur 72** à :

- `compare` : `side > back ? 0 : back`
- `subtract` : `max(0, back - side)`

Le cas qu'une implémentation fausse inverse est l'égalité en mode `compare` :
l'entrée arrière **passe**, elle ne s'annule pas.

La première campagne avait donné 1 sur 72. Les cellules étaient espacées de deux
blocs et la chaîne de calibration latérale, longue de seize, traversait les
quatre rangées suivantes ; chaque sortie lisait 13 à 15. Espacement porté à 24,
soit un de plus que la cellule la plus large.

### Le conteneur

Un coffre simple, 27 emplacements, `n` d'entre eux remplis d'une pile complète.
**28 lectures sur 28** conformes à `floor(14 n / 27) + (n > 0)` :

```
n     0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27
sortie 0 1 2 2 3 3 4 4 5 5  6  6  7  7  8  8  9  9 10 10 11 11 12 12 13 13 14 15
```

Le `+1` est ce qui fait qu'un seul objet dans un coffre lit 1 et non 0. La
première campagne remplissait un seul emplacement avec `n` objets, ce qui ne
dépasse jamais un quarantième du coffre et lit 1 partout : sweep inutile.

## 6. La torche et son burn-out

Lecture en direct, `execute if block`, parce qu'une torche grillée se rallume
après 160 ticks et qu'une sauvegarde met plus longtemps que cela à revenir : le
monde sur disque montre une torche allumée et affirme qu'il ne s'est rien passé.

| Cadence | Éteinte alors que le levier est au repos | Rallumée 9 s plus tard |
|---|---|---|
| 48 bascules toutes les 0,12 s | **oui** | oui |
| 32 bascules toutes les 0,2 s | **oui** | oui |
| 8 bascules toutes les 1,5 s | non | — |
| une seule bascule (témoin) | non | oui |
| maintenue éteinte par un répéteur | oui (attendu) | — |
| jamais touchée | non | — |

**7 scénarios sur 7** correspondent à la règle implémentée : huit changements
dans une fenêtre glissante de 60 ticks éteignent la torche, qui reste éteinte
160 ticks.

Un détail découvert en route, et qui a coûté une campagne : **une torche pilotée
par un répéteur ne peut pas griller.** Les deux ticks du répéteur plus les deux
de la torche font un cycle de huit ticks ; huit cycles demandent 64 ticks, quatre
de trop pour la fenêtre. Le circuit de mesure a dû piloter le bloc support
directement.

---

## 7. `block_ticks` sur disque

Attrapé en vol : déclencher, puis sauvegarder sans rien attendre.

```json
{"i": "minecraft:repeater",   "p": -1, "t": 8, "x": 5, "y": -60, "z": 2}
{"i": "minecraft:comparator", "p":  0, "t": 2, "x": 8, "y": -60, "z": 2}
```

Trois faits, tous les trois nécessaires à la parité de sauvegarde :

1. `i` est le **nom** du bloc, pas son id — comme la palette, et pour la même
   raison : une sauvegarde doit survivre à une version dont les numéros ont
   bougé.
2. `t` est un **délai relatif au `gameTime` de la sauvegarde**, pas un tick
   absolu. Les valeurs lues sont 8 et 2 alors que l'horloge du monde était à
   plusieurs centaines ; un monde rouvert un mois plus tard ne doit pas déclencher
   un mois d'arriéré d'un coup.
3. `p` est la priorité, et **les deux diodes n'utilisent pas la même échelle** :
   un répéteur qui s'allume demande `-1`, un comparateur `0`. C'est mesuré, et
   c'était faux dans la première version du code, qui donnait `-1` aux deux.

La capture est une course : un répéteur se déclenche huit ticks après avoir été
alimenté, et un `save-all flush` envoyé dans le même souffle arrive quand même
après. Ce qui la rend gagnable, c'est d'envoyer déclenchement et sauvegarde dans
un seul `write` sur l'entrée standard, sans marqueur de synchronisation entre
les deux : le serveur vide toute la file de console dans un même tick. Attrapé
au premier essai sur douze prévus.

Deux dispositifs n'ont **pas** été attrapés et ne sont donc pas mesurés : la
torche (deux ticks, trop court) et l'observateur. Leurs délais dans le code
valent 2 et 2, et c'est dit au §10.

---

## 8. Les pistons

Un piston vers l'est, `n` blocs de fer devant, alimenté par un bloc de redstone
posé dessus. Longueurs 0 à 14 :

| Longueur | 0–12 | 13, 14 |
|---|---|---|
| Résultat | tête posée, colonne déplacée | **rien ne bouge** |

**15 longueurs sur 15.** Le treizième bloc ne fait pas « pousser les douze
premiers » : il fait échouer toute la poussée.

### Réactions à la poussée — 979 blocs mesurés

Un piston pointé sur chaque bloc du jeu ; les trois cases relues.

| Verdict | Nombre |
|---|---|
| bouge | 552 |
| refuse et bloque le piston | 126 |
| absent après coup | 301 |

Les 301 ont demandé une seconde passe. Un bloc absent peut avoir été cassé par le
piston, ou s'être retiré tout seul — un plant sans terre, une porte dont la
moitié haute n'a jamais été posée, du corail hors de l'eau. Le recoupement avec
les blocs qui n'avaient pas survécu **seuls** dans la grille du §2 les sépare :
**200 sont réellement cassés**, **101 sont indéterminés** et sont nommés dans
`piston.cpp` (`kPushUnmeasuredNames`) puis rabattus sur la forme de collision,
avec `Pistons::unmeasured()` pour dire lesquels.

La surprise : **les feuillages sont cassés par un piston** alors que ce sont des
cubes pleins. Aucune règle sur les boîtes de collision ne l'aurait donné.

Deux verdicts ont été corrigés parce qu'ils venaient de la sonde et non du bloc,
et les deux sont dits dans le code :

- **slime et miel** collaient au sol de pierre de la grille ; la poussée demandée
  était en réalité celle de toute la dalle et elle échouait sur la limite de
  douze. Ils bougent.
- **un piston dans la case cible** était quasi-alimenté par le bloc de redstone
  qui déclenche le piston qui le pousse — la case au-dessus de lui touche ce bloc
  — donc il sortait et devenait immobile. Ce qui est une jolie preuve incidente
  que la quasi-connectivité existe, et une lecture inutilisable.

---

## 9. La quasi-connectivité, isolée

Un bloc de redstone **une case plus haut et une case au nord** du piston : il ne
partage aucune face avec lui, et ne touche que la case au-dessus.

| Montage | Piston sorti |
|---|---|
| source en diagonale au-dessus, piston posé **après** | **oui** |
| même chose, un cran plus haut (témoin) | non |
| même chose, piston posé **avant** (aucune mise à jour) | non |
| le même, puis une mise à jour posée à côté | **oui** |
| source directement au-dessus (adjacence ordinaire) | oui |
| piston collant, même montage | **oui** |
| piston vers le haut, source deux cases au-dessus | **oui** |
| **distributeur**, même montage | non |
| **dropper**, même montage | non |
| **lampe**, même montage | non |

**10 montages sur 10.** Trois choses en sortent, et les trois sont dans le code :

1. la quasi-connectivité est réelle et n'appartient **qu'aux pistons** — ni le
   distributeur, ni le dropper, ni aucun consommateur ordinaire ne l'ont ;
2. elle demande une **mise à jour** pour être remarquée ; c'est le *block update
   detector* ;
3. l'anneau du dessus n'exclut que la direction `Down` — d'où le piston tourné
   vers le haut qui sort alors que `facing` est exclu de son propre anneau.

---

## 10. Ce qui n'est pas mesuré

Nommé plutôt que passé sous silence :

- **Un piston rentré conduit-il la redstone ?** Non mesurable avec la sonde
  disponible (§2). Traité comme le cube plein qu'il est.
- **101 réactions à la poussée** (§8), listées par `Pistons::unmeasured()`.
- **L'ordre exact des mises à jour d'un fil qui se propage.** Vanilla récurse à
  travers les notifications de voisinage ; `update_wire` fait la même chose avec
  une pile explicite, qui donne le même état stable mais pas nécessairement le
  même ordre intermédiaire sur un circuit qui se ramifie. Seul l'état stable est
  vérifié.
- **Le `moving_piston` et son block entity.** L'état stable après une poussée est
  mesuré et reproduit (tête posée, colonne déplacée) ; les deux ticks
  intermédiaires pendant lesquels vanilla pose un `moving_piston` ne le sont pas.
- **Cible et rails.** Le modèle d'alimentation les couvre (ils sont dans la
  table des consommateurs et leur drapeau suit la puissance), mais leur *effet*
  n'est pas implémenté et n'est pas mesuré. L'entonnoir, le distributeur, le
  dropper et le note block, eux, le sont désormais : §§ 11 à 14.
- **`minecraft:tripwire`.** Son `powered` est posé par une entité sur le fil et
  le signal ressort par le crochet ; ce n'est ni une source ni un consommateur.
  Nommé explicitement dans le test, qui échoue si un autre bloc rejoint la liste.
- **Le délai de la torche (2 ticks) et celui de l'observateur (2 ticks).** Trop
  courts pour être attrapés dans un `block_ticks` sauvegardé en vol, là où les
  quatre délais du répéteur et celui du comparateur l'ont été. Le comportement
  de la torche est mesuré (§6) ; c'est le *nombre* qui ne l'est pas.

---

## 11. Les interrupteurs : bouton et plaque de pression

Le modèle de puissance pilotait correctement les deux, et **rien ne les
relevait**. `Redstone::scheduled_tick` tombait dans `consumer_rule`, ne trouvait
rien et rendait `false` : un bouton enfoncé restait enfoncé pour toujours,
pendant que son tick de relâchement était vidé une fois par tick, chaque tick,
sans effet. Un joueur qui tirait un levier voyait le fil s'allumer ; un joueur
qui appuyait sur un bouton voyait le bouton coller.

Ce ne sont pas des consommateurs. Rien autour d'un bouton ne décide de son état
et `consumer_powered` répond faux pour les deux ; ce sont des **sources à
minuterie**, et la minuterie est toute la règle.

### Les durées, mesurées tick par tick

Les deux constantes de `item_use.cpp` portaient un commentaire disant qu'elles
n'avaient jamais été chronométrées. Elles le sont.

Le banc n'est pas la console — un bouton ne s'enfonce que par un *use*, et la
console ne peut rien utiliser. C'est un client sonde qui parle 763 et rien
d'autre : il clique le bouton, et lit les deux `Block Update` de retour sur
**l'horloge du serveur** (`Update Time` porte l'âge du monde en ticks), pas sur
un chronomètre mural. La différence entre « environ une seconde » et 20.

| Matériau | Ticks | Cellules |
|---|---|---|
| `stone_button`, `polished_blackstone_button` | **20** | 2/2 |
| les onze boutons de bois (chêne → warped) | **30** | 11/11 |

13 matériaux sur 13, aucun écart. Les littéraux du code étaient justes — ce qui
vaut d'être su plutôt que supposé.

### La plaque, et le tick de trop

Sur le fil, les quinze plaques rendent **19** ticks (10 → **9** pour les deux
plaques pondérées) alors que les treize boutons tombent tous sur un 20 ou un 30
exact par la même méthode. Un biais qui n'apparaît que sur une famille n'est pas
un biais, donc le nombre a été confirmé une seconde fois, sans aucune mesure
réseau dedans :

`block_ticks` sur disque porte `t`, le délai **relatif au `gameTime` de la
sauvegarde** (§7). Attrapé dans le même tick que l'appui, `t` vaut le délai
entier ; attrapé plus tard il vaut moins. La campagne prend donc le **maximum**
sur quatorze tentatives, qui converge par en dessous et ne peut jamais dépasser.

```
stone_pressure_plate            t max = 20   (échantillons 20 12 5 20 12 4 19 …)
light_weighted_pressure_plate   t max = 10   (échantillons 10 3 10 3 9 1 3 9 …)
```

Le délai programmé vaut donc **20** pour les quatre plaques simples et **10**
pour les deux pondérées. Le tick d'écart vient du front d'appui — la plaque
s'enfonce pendant la phase des entités, son tick programmé retombe pendant la
phase des ticks de bloc — et pas du délai.

⚠ **Une plaque se réarme tant que quelque chose est dessus.** Chronométrer depuis
l'instant où l'entité s'en va donne un nombre étalé sur (0, D] et ressemble à du
bruit. Chronométrer depuis l'**appui** donne D exactement. C'est ce qui a fait
choisir le montage « invoquer un porte-armure, puis le tuer ».

### `entity_pressure`

Une plaque répond aux **entités**, pas à la puissance, et `RedstoneWorld` ne
connaissait pas les entités. Il gagne `entity_pressure`, non-pure et valant 0 par
défaut. Ce n'est pas un bouche-trou : un monde qui ne porte pas d'entités n'a
véritablement rien sur aucune plaque, et 0 est la vraie réponse. Le serveur, qui
en porte, doit surcharger — il ne le fait pas encore (§14).

---

## 12. L'entonnoir : la cadence, et le verrou

Montage : coffre au-dessus, entonnoir tourné vers le bas, coffre en dessous, 64
cobblestone dans le coffre du haut. La cadence n'est pas lue objet par objet — un
aller-retour de console vaut plusieurs ticks et la réponse serait l'aller-retour.
Elle est lue comme une **pente**, sur un intervalle que le serveur mesure
lui-même avec `time query gametime`.

| Montage | Objets | Ticks | Ticks/objet |
|---|---|---|---|
| libre | 30 | 242 | **8,07** |
| levier posé dessus | **0** | 242 | — |

8,07 contre un temps de recharge de **8**, le reste étant les deux aller-retours
de console aux extrémités. Et zéro objet sur le même intervalle avec un levier :
le verrou est total, pas un ralentissement.

L'état lu dans la sauvegarde :

```
libre     minecraft:hopper[enabled=true,facing=down]
verrouillé minecraft:hopper[enabled=false,facing=down]
```

C'est le seul consommateur dont le drapeau **se lit à l'envers** : `enabled` est
*faux* pendant qu'il est alimenté. `ConsumerRule` gagne donc un bit `inverted`
plutôt qu'une seconde table — sans lui, la même table **allumerait** un
entonnoir quand un levier à côté se lève.

⚠ Le levier se pose **en dernier** et **adjacent**. `/setblock` ne prévient que
six voisins et s'arrête (§1) : un déclencheur à deux blocs ne déclenche rien.

---

## 13. Le distributeur et le dropper

Le dropper est la moitié simple, et il est aussi le **témoin** : il éjecte
toujours, quoi qu'il porte. Donc tout objet où les deux machines s'accordent est
un objet où le distributeur est tombé dans son comportement par défaut, et tout
objet où elles diffèrent est un comportement qui a un nom. Une cellule par objet
et par machine, vingt-quatre blocs d'écart — un seau d'eau se répand sur sept
blocs, et une cellule inondée par sa voisine se lit exactement comme un
distributeur qui n'a rien fait.

Trois résultats qu'aucune règle ne prédit :

- le **seau d'eau** laisse un **seau vide** dans le distributeur : l'objet est
  *remplacé*, pas consommé ;
- le **briquet** n'est ni consommé ni remplacé : il revient avec `tag: {Damage:
  1}`. La règle est un point d'usure, et l'objet ne part que quand il casse ;
- la **TNT a détruit son propre distributeur**. La cellule est revenue avec
  « The target block is not a block entity », ce qui est la preuve la plus forte
  possible que la TNT distribuée est *amorcée* et non lâchée.

⚠ Deux pièges payés ici, tous deux généraux :

1. **Une sonde qui regarde trois secondes après ne voit aucun projectile.** Une
   flèche quitte un distributeur à environ un bloc par tick : à trois secondes
   elle est à soixante blocs et toute la table lit « rien ne s'est passé », ce
   qui est indiscernable d'un distributeur qui n'a jamais tiré. Un distributeur
   tire **quatre ticks** après qu'on le lui dit ; la fenêtre utile est ticks 4 à
   10.
2. **`data get entity <sélecteur> id` ne marche pas** et ne le dit pas
   utilement : il répond « Found no elements matching id », parce qu'une entité
   vivante est sérialisée *sans* son id. C'est la sortie de la commande `tag`
   elle-même qui nomme ce qu'elle a marqué — « Added tag 'c0' to Arrow » — et
   c'est la lecture qui marche.

---

## 14. Ce qui reste non branché, et pourquoi

Nommé plutôt que caché.

- **L'entonnoir ne transporte rien dans le serveur.** Les règles et leurs tests
  existent (`hopper.hpp`), mais les conteneurs du serveur sont du NBT lu au coup
  par coup dans `server.cpp`, que ce mandat n'avait pas le droit de toucher. Un
  entonnoir du banc ne bouge toujours aucun objet. Seul son drapeau `enabled`
  est piloté.
- **La plaque de pression ne s'enfonce pas dans le serveur.** `plate_step` existe
  et est testée ; rien n'appelle `entity_pressure` parce que le serveur n'a pas
  de détection « entité sur un bloc ». Le relâchement, lui, est branché : une
  plaque posée `powered=true` se relève.
- Le note block, lui, **est** branché. Sa table est complète et vérifiée :
  `redstone_noteblock` a relu l'état de bloc d'un note block posé au-dessus de
  chacun des **987 blocs** du jeu, 16 instruments, **0 refus**, et
  `scripts/check_noteblock.py` compare la table compilée à cette mesure bloc par
  bloc — **987/987**.

  | instrument | blocs | | instrument | blocs |
  |---|---|---|---|---|
  | harp | 401 | | guitar (laines) | 16 |
  | basedrum | 280 | | xylophone (bone_block) | 1 |
  | bass | 221 | | flute (clay) | 1 |
  | hat (verres) | 38 | | bit, pling, bell, banjo… | 1 chacun |
  | snare (sables) | 21 | | | |

  Il **fallait** la table : aucune règle de nom ne marche. Les boutons et les
  portes de bois sont `harp`, pas `bass` ; les dalles de cuivre taillé sont
  `harp` et le cuivre taillé plein ne l'est pas. Un heuristique par suffixe
  essayé contre la mesure se trompe sur **51 blocs** — le genre de faux qui
  sonne juste jusqu'à ce que quelqu'un joue un air.
  Reste non mesuré : les instruments de tête de mob (zombie, squelette…), qui
  viennent d'une tête posée **au-dessus** du note block et pas en dessous.
- **La TNT est mesurée mais pas implémentée.** La mèche vaut **80 ticks**, lue
  sur le `Fuse` de l'entité amorcée et prise au maximum sur douze captures :
  `80 80 80 79 79 78 78 77 77 77 76 76`, qui converge par en dessous exactement
  comme le délai de la plaque (§11).

  La forme de l'explosion l'est aussi. Une charge au centre d'une boîte pleine
  d'un seul matériau, 15×11×15, allumée puis relue cellule par cellule :

  | matériau | blocs détruits | portée (Chebyshev) |
  |---|---|---|
  | `obsidian` | 2 | 1 |
  | `stone` | 26 | 1 |
  | `oak_planks` | 30 | 2 |
  | `dirt` | 216 | 4 |
  | `glass` | 284 | 4 |

  L'ordre est monotone en résistance au souffle, ce qui est le contrôle : une
  campagne qui donnerait l'obsidienne plus fragile que le verre serait fausse
  quel que soit le nombre.

  Ce qui **manque** pour implémenter, et pourquoi ça n'a pas été fait ici : la
  résistance au souffle n'est dans aucun rapport du data generator, et la
  déduire des formes demanderait une boîte par bloc, soit 987 boîtes. Sans
  table de résistance, l'algorithme de rayons rendrait une moitié de règle. La
  mèche et les cinq formes restent comme oracle pour la vague suivante.

  ⚠ Le premier passage de ce scénario a centré la boîte sur le sol du superflat
  à y=−60, donc le `fill` descendait à y=−66 — **sous le monde**. La commande
  échouait en entier, aucune boîte n'était construite, et chaque cellule se
  lisait « pas le matériau » : le rapport annonçait l'obsidienne entièrement
  détruite par un bâton de TNT, avec une portée valant exactement la
  demi-largeur de la boîte. Le témoin qui l'a trahi est la cellule de terre, qui
  « survivait » 399 blocs — les deux couches de terre naturelles du superflat.

- **Rails et cible.** Non mesurés et non implémentés dans cette vague.
- **Le distributeur choisit la première case pleine, pas une au hasard.** Vanilla
  tire au sort parmi les cases non vides. Reproduire le tirage demande le flux
  RNG de la machine, que ce projet ne porte pas encore sur un block entity. Dit
  dans le code, pas seulement ici.

---

## 15. Reproduire

```bash
python3 scripts/measure_redstone.py conductors        # ~6 min, 987 blocs
python3 scripts/measure_redstone.py conductor_gaps    # les cubes pleins illisibles
python3 scripts/measure_redstone.py conductor_probe   # sonde par répéteur
python3 scripts/measure_redstone.py push              # ~6 min, réactions à la poussée
python3 scripts/measure_redstone.py wire
python3 scripts/measure_redstone.py repeater
python3 scripts/measure_redstone.py comparator        # ~10 min
python3 scripts/measure_redstone.py torch
python3 scripts/measure_redstone.py piston
python3 scripts/measure_redstone.py qc
python3 scripts/measure_redstone.py ticks
python3 scripts/measure_redstone.py switches      # bouton et plaque, tick par tick
python3 scripts/measure_redstone.py noteblock     # ~5 min, 987 blocs, un instrument chacun
python3 scripts/measure_redstone.py hopper        # cadence et verrou
python3 scripts/measure_redstone.py dispenser     # 18 objets, distributeur contre dropper
python3 scripts/measure_redstone.py tnt           # meche et forme de l'explosion
```

Et la vérification qui compare la table compilée à la mesure :

```bash
python3 scripts/check_noteblock.py                # 987/987
```

Chaque scénario démarre et arrête son propre serveur, sur le port 25611, dans
`run/redstone-oracle/<scénario>/`. Aucun ne dépend d'un autre.
