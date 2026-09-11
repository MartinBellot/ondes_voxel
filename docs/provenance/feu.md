# Le feu

Ce qui suit trace le **bloc de feu** (pose, âge, survie, propagation, extinction), la **lave**
qui l'allume, les **entités en feu** (compteur `Fire`, dégâts, drapeau partagé 0x01, soleil),
les **feux de camp** et la règle de survie qui débloque les patchs de feu de la génération.

Les règles et la table d'inflammabilité sont spécifiées depuis l'article « Fire » du Minecraft
Wiki (table « Flammable blocks », paragraphes sur le délai, l'âge, la propagation, la pluie,
les biomes humides). Le banc qui les confronte au vrai serveur 1.20.1
(`tools/vanilla/server.jar`, SHA-1 `84194a2f286ef7c14ed7ce0090dba59902951553`) est
`scripts/measure_fire.py` (voir l'état ci-dessous). Les relevés bruts vont dans
`data/vanilla/1.20.1/normalized/fire_*.json` (gitignorés, régénérables).

> ⚠ L'article du wiki documente la version **courante** : sa table porte des lignes 1.21
> (bloc et tapis de mousse pâle, mousse suspendue pâle, fleur de cactus, buisson à lucioles…).
> Elles sont écartées ; `FireRules::unknown_names()` compte les lignes de la table qui ne
> résolvent pas dans le registre 1.20.1, et le test exige zéro.

Le code :

| | |
|---|---|
| `src/ov_gameplay/{include/ov/gameplay,src}/fire.{hpp,cpp}` | les règles, sur un `LevelWriter&` : table, forme, tick, lave, entités, soleil, feu de camp |
| `src/ov_gameplay/tests/test_fire.cpp` | les montages du banc, reconstruits bloc pour bloc |
| `src/ov_server/src/fire_session.{hpp,cpp}` | le branchement : extension de blocs, extension de random tick, pluie, mobs, joueurs |
| `src/ov_server/src/campfire.{hpp,cpp}` | le gril : NBT du block entity, recette, tick, paquets |
| `src/ov_server/src/world_ticks.{hpp,cpp}` | un emplacement `fire_extension_` à côté de celui de la TNT |
| `src/ov_server/src/agriculture.{hpp,cpp}` | `RandomTickExtension` : la lave au même tirage que les plantes |
| `src/ov_gameplay/src/item_use.cpp` | le briquet passe par `FireRules::placement` (§ 9) |
| `src/ov_server/src/combat_session.cpp`, `projectiles.cpp` | Aura de feu, flèches Flamme |
| `src/ov_server/src/mob_combat.{hpp,cpp}` | `loot(..., on_fire)` : une vache morte en brûlant lâche du bœuf cuit |
| `src/ov_worldgen/src/vegetation_feature.cpp` | règles de survie `Fire` et `SoulFire` |
| `scripts/measure_fire.py` | l'oracle : `blocks`, `rain`, `lava`, `entity` |
| `scripts/check_fire_e2e.py` | notre serveur et un client sonde : `forest`, `player`, `zombie` |

> **État.** Le code, les tests unitaires et le contrôle de bout en bout (§ 8) sont faits et
> verts. La campagne `blocks` a tourné : les §§ 1 à 3 portent ses chiffres. Les campagnes
> `rain`, `lava`, `entity` et `confirm` sont **en file sur le verrou partagé du JVM**
> (`/tmp/ov-vanilla.lock`), derrière une dizaine de campagnes d'autres agents : les §§ 4 à 7
> attendent leurs chiffres. Jusque-là, les nombres de ces parties sont ceux du wiki, et le code
> le dit.

---

## 0. La méthode : un datapack qui regarde chaque tick

Une console répond quand elle veut ; un relevé « par tick » lu depuis la console mélange des
ticks. `measure_fire.py` installe donc un **datapack** dans le monde avant le démarrage. Sa
fonction `tick` (étiquette `minecraft:tick`) observe le montage **une fois par tick de jeu** et
écrit `time query gametime` dans un score ou dans une liste de stockage au moment exact où
quelque chose change :

* l'âge d'un feu : seize `execute if block … fire[age=k]`, puis, si l'âge diffère du
  précédent, `(gametime, âge)` ajouté à `storage ovfire:log c<i>` ; −1 quand le bloc n'est
  plus du feu ;
* le premier tick où une condition tient (`first_time`) : un bloc qui n'est plus lui-même,
  une cellule qui devient feu, un objet au sol près d'un feu de camp ;
* une entité : `(gametime, Fire, Health × 100)` chaque tick.

La console ne fait que construire le montage, le **démarrer par un seul appel de fonction**
(une fonction s'exécute en un tick : tous les feux naissent au même tick, `#t0`), puis relire
les scores à la fin. Un délai de 30 ticks se lit 30, pas « environ 1,5 s ».

---

## 1. Le délai et l'âge — mesurés

Campagne `blocks` (4051 ticks, temps clair, difficulté normale) : 64 feux sur netherrack, dont
16 journalisent chaque changement d'âge au tick près.

* **Délai `30 + next_int(10)`.** Entre deux changements d'âge d'un même feu, 224 écarts : le plus
  court vaut **30**, et les 69 écarts de moins de 60 ticks (un seul tick de feu entre les deux)
  couvrent **30 à 39, les dix valeurs, 40 jamais** (6, 6, 7, 10, 6, 4, 8, 9, 7, 6). Le « 30 à 40 »
  du wiki est 30 à 39. Le témoin « 30..40 » est rejeté aussi par la durée de vie sur pierre
  (p = 0,010).
* **L'âge avance d'un cran une fois sur trois** (`next_int(3) / 2`) : 69 écarts simples sur 224,
  soit **0,308** (attendu 1/3 ; les autres écarts sont deux, trois… ticks de feu sans pas).
  Jamais deux crans d'un coup : les 16 feux journalisés sur pierre passent par 1, 2, 3, 4.
* **Netherrack : 0 / 64 éteints** en 4051 ticks. `#infiniburn_overworld` (netherrack, bloc de
  magma) saute l'extinction par la pluie, par le support et par l'âge 15.

## 2. La vie d'un feu sur la pierre — mesurée, et une question tranchée

64 feux sur pierre, rien d'inflammable autour : **64 / 64 éteints**, moyenne **409 ticks**
(de 201 à 869).

Le wiki ne dit pas si « l'âge dépasse 3 » se lit avant ou après le pas du tick. Les journaux
tranchent : **les 16 feux journalisés restent à l'âge 4 exactement un intervalle** (30 à 39
ticks, les seize) puis s'éteignent au tick suivant. Le test porte donc sur l'âge qu'avait le feu
**au début** du tick — c'est ce que fait `FireRules::scheduled_tick`, et notre banc
(`test_fire.cpp`, 2000 feux) montre le même séjour d'un intervalle, pour chaque feu.

Un écart nommé : la moyenne vanilla (409) est **1,9 écart-type sous** celle du modèle (448 : 13
ticks de feu de 34,5). KS : **p = 0,062 contre notre code** (`test_fire.cpp`, 2000 feux ; témoin
étiré ×1,25 plus loin), p = 0,024 contre le modèle Python de `measure_fire.py analyse`. La règle,
elle, est décidée par le séjour à l'âge 4 ; ce qui raccourcit les vies vanilla d'environ un tick
de feu en moyenne n'est pas trouvé.

## 3. Les deux nombres d'un bloc — mesurés sur un échantillon

### Brûler (« burn odds »)

Le montage `burn` : un feu sur netherrack, un bloc B à l'est, muré de pierre sur ses cinq autres
faces. Aucune cellule d'air ne touche B : la seule chose qui peut lui arriver est le jet du feu
contre lui, un par tick de feu, `next_int(300) < burn`. Seize B par sorte ; relevé : le tick où B
cesse d'être B. **0 objet au sol** à la fin : un bloc brûlé ne lâche rien.

| B | table | partis | p, notre code | p, modèle Python | taux mesuré par tick de feu | table / 300 |
|---|---|---|---|---|---|---|
| planches de chêne | 20 | 16/16 | 0,58 | 0,59 | 0,054 | 0,067 |
| clôture de chêne | 20 | 16/16 | 0,46 | 0,48 | 0,065 | 0,067 |
| botte de foin | 20 | 16/16 | 0,43 | 0,45 | 0,074 | 0,067 |
| bûche de chêne | 5 | 13/16 | 0,55 | 0,60 | — | 0,017 |
| bloc de charbon | 5 | 14/16 | 0,63 | 0,66 | — | 0,017 |
| bibliothèque | 20 | 16/16 | 0,10 | 0,11 | 0,12 | 0,067 |
| laine blanche | 60 | 16/16 | **0,039** | **0,024** | 0,33 | 0,20 |
| feuilles de chêne | 60 | 16/16 | 0,33 | 0,35 | 0,26 | 0,20 |
| bloc d'algues séchées | 60 | 16/16 | 0,077 | 0,058 | 0,32 | 0,20 |
| pierre | 0 | 0/16 | — | — | 0 | 0 |

« Notre code » : `test_fire.cpp`, le même montage rejoué 1000 fois par `FireRules`, KS à deux
échantillons contre les seize de vanilla. « Modèle Python » : la règle du wiki simulée par
`measure_fire.py analyse`. Les deux s'accordent à quelques centièmes près, ce qui vérifie aussi
que notre code est la règle écrite. Le témoin « moitié des odds » est plus loin que la table pour
les neuf sortes contre notre code, et rejeté pour huit sur neuf contre le modèle.

⚠ **Question ouverte.** La table tient bloc par bloc au seuil corrigé pour neuf tests, mais les
trois blocs à 60 brûlent ensemble à **0,30 ± 0,036** par tick de feu contre 0,20 attendu (48 B,
2,8 écarts-types), et la bibliothèque à 0,12 contre 0,067. Chaque sorte était sur **sa propre
rangée** : le bloc et la place sont confondus. La campagne `confirm` les remesure **entrelacées**
position par position, 32 de chaque ; la table n'est pas changée sur seize échantillons.

### Allumer (« ignite odds »)

Le montage `ignite` : un feu sur netherrack, une cellule d'air C à l'est, un bloc F à l'est de C,
muré. F est hors de portée du feu (jamais brûlé le premier) et C est la seule cellule vide à
portée qui ait un voisin inflammable. Relevé : le tick où C devient feu.

| F | table | allumés | KS (p) |
|---|---|---|---|
| planches, bûche, charbon, **ensemble** | 5 | 45/48 | **0,95** contre notre code (témoin odds 60 plus loin) ; 0,99 contre le modèle (témoin odds 15 : 0,23) |
| laine | 30 | 14/16 | 0,76 |
| feuilles | 30 | 15/16 | 0,50 |
| bibliothèque | 30 | 14/16 | 0,69 |
| algues séchées | 30 | 15/16 | 0,61 |
| cible | 15 | 14/16 | 0,59 |
| foin | 60 | 16/16 | 0,088 |
| **établi** | 0 | **0/16** | — (le feu ne le voit pas) |

⚠ Les trois rangées à 5, qui devraient suivre une seule loi, **diffèrent entre elles** : la
bûche (première allumée au tick 924 ; sous le modèle, P ≈ 1,6·10⁻⁴ pour les seize) contre le
charbon (trois allumées avant le tick 102), KS entre les deux **p = 0,0019**. Leur réunion suit
la table à p = 0,99. C'est le même confondement rangée/bloc que ci-dessus, et la même campagne
`confirm` le lève.

---

## 4. La pluie — mesurée

Campagne `rain` : `weather rain`, cinq secondes pour que le niveau de pluie passe 0,2, puis les
mêmes 64 feux sur pierre qu'au § 2, 16 feux sur netherrack, et huit vaches `Fire:200` (max 200
PV), quatre à ciel ouvert et quatre sous un toit de pierre.

* **Feux sur pierre sous la pluie : 64 / 64 éteints, moyenne 161 ticks** (409 au sec). Huit se
  sont éteints **au premier tick de feu** (30 à 38), âge 0 : c'est le jet `0,2 + 0,03 × âge` par
  tick, pas la limite d'âge. `test_fire.cpp` rejoue 2000 feux sous la pluie par `FireRules` :
  KS **p = 0,18** contre les 64 de vanilla (distance 0,14) ; le témoin « au sec » est à
  **0,73** — la mesure discrimine.
* **Netherrack sous la pluie : 0 / 16 éteints** : `#infiniburn` saute aussi le jet de pluie.
* **Les vaches à ciel ouvert** : un point au premier tick (à `Fire` = 200), et au tick suivant
  `Fire` = **−1**. La pluie éteint un mob sur-le-champ, et le compteur tombe à moins sa grâce
  d'un tick — ce que fait `tick_entity_fire`.
* **Les vaches sous le toit** brûlent : un point à `Fire` = 200, 180, …, 20 — **un tous les 20
  ticks, le premier au premier tick**, dix en tout. C'est la cadence de `on_fire`, mesurée.

« Il pleut sur ce bloc » est, dans ce serveur, `FireSession::is_raining_at` : il pleut, la
colonne n'a rien qui arrête le mouvement au-dessus (heightmap MOTION_BLOCKING), et le biome a des
précipitations à une température ≥ 0,15. Le toit de pierre des vaches témoins est ce test-là.

---

## 8. De bout en bout, contre notre serveur

`scripts/check_fire_e2e.py` fait tourner `ov_dedicated` (Debug) sur un monde plat neuf, construit
chaque montage par la console du serveur (stdin), et regarde avec un client sonde ce qu'un
client vanilla recevrait. Rien n'est appelé à côté du protocole.

⚠ **Les fenêtres sont comptées en ticks du serveur**, lus dans l'âge du monde des paquets
Update Time (0x5E, un toutes les vingt), jamais en secondes. La première version comptait en
secondes et a échoué sur `player` : le serveur Debug, sur une machine chargée par la suite de
tests et d'autres agents, tournait à **2,4 ticks par seconde** (« stopped after 328 ticks »,
p50 421 ms) ; cinq secondes dans le feu y font onze ticks, moins que les vingt de grâce d'un
joueur. Le même banc, lancé avant la charge, tournait à p50 6,3 ms avec la forêt en feu — la
lenteur était la machine, pas le feu.

### `player` — un joueur brûle

Le client sonde, en survie, reste debout ; la console pose un feu sous ses pieds.

| fenêtre | Set Health reçus | drapeau 0x01 de sa propre entité |
|---|---|---|
| 100 ticks dans le feu | de 19,0 à 8,3, un point environ tous les **10** ticks (un coup `in_fire` par fenêtre d'invulnérabilité), la régénération intercalée | posé dans la première fenêtre |
| 200 ticks après le retrait du feu | un point aux ticks 16, 36, 54, 91, 111, 131 : **un tous les 20** (`on_fire`) | effacé au tick **150** (8 s = 160 ticks, à la résolution de 20 près) |
| 80 ticks dans le feu, Résistance au feu | **aucun** | posé : la résistance arrête les dégâts, pas les flammes |

### `forest` — une forêt de planches

Un bloc de 7 × 7 × 3 planches de chêne, un feu posé dessus, `doFireTick` vrai, 1200 ticks :
**50** cellules devenues feu, **45** planches mangées sur 147, **0** objet au sol (un bloc brûlé ne
lâche rien). Le serveur est resté à p50 6,5 ms par tick avec la forêt en feu.

### `zombie` — un zombie à midi

Un zombie invoqué à midi, ciel ouvert : allumé (drapeau 0x01) et **20** Damage Event en 400
ticks.

⚠ La résolution du banc est d'**un paquet Update Time, vingt ticks**. Le premier embrasement de
la forêt et l'allumage du zombie sont arrivés avant le premier de ces paquets : le banc les lit
« 0 », ce qui veut dire « dans la première fenêtre de vingt ticks », pas un tick exact. Les
délais exacts sont ceux de l'oracle (§§ 1–6), lus tick par tick par le datapack.

---

## 9. Points d'extension, pour les agents voisins

* **Le briquet et le portail du Nether.** Le briquet décide de son feu en **un seul point** :
  `ItemUse::use_item_on`, bloc `// ── fire ──`, qui appelle `FireRules::placement(level,
  cible)` (une cellule vide où le feu que `state_for` façonne peut tenir —
  `BaseFireBlock.canBePlacedAt` sans sa moitié « portail »). L'allumage d'un cadre de portail
  se décide **avant** cette ligne : vanilla laisse un cadre prendre un briquet que le feu seul
  ne survivrait pas, et le portail remplace le feu qu'il aurait posé. Sans `FireRules` branché
  (`set_fire_rules(nullptr)`), l'ancienne règle tient : `fire[age=0]` dans toute cellule d'air.
* **La foudre** (l'agent météo) : `FireSession::ignite(level, pos)` écrit un feu seulement là
  où il peut tenir, à travers le niveau ; la notification qui suit lui donne son premier tick.
* **L'Aura de feu et Flamme** (l'agent enchantement) : `CombatIo::set_on_fire` et
  `ProjectileHost::set_on_fire` ; les niveaux viennent de `enchantment_level` existant.
* **La météo** : le feu lit `WorldState::weather.is_raining()` une fois par tick
  (`FireWorld`), jamais l'horloge ni le cycle — il ne duplique pas le cycle météo.

---

## 10. Nommés, et pas faits

* **`ignitedByLava` hors de la table** : la colonne « catches from lava » du wiki ne couvre que
  les blocs que le feu brûle. Que la lave allume aussi à côté de blocs de bois que le feu
  ignore (établi, coffres, panneaux…) n'est **pas établi** : le montage `crafting_roof_2` de la
  campagne `lava` le teste sur l'établi. D'ici là, rien n'est branché hors de la table
  (`kLavaOnly` est vide) — une liste devinée allumerait des feux que vanilla n'allume pas.
* **Les flèches qui traversent le feu ou la lave** ne s'enflamment pas ; seules les flèches
  Flamme allument leur cible (5 s, d'après l'article « Flame » du wiki, non mesuré).
* **Protection contre le feu** : `setSecondsOnFire` n'applique pas la réduction de
  l'enchantement — ce serveur ne lit pas l'armure enchantée des entités.
* **Le compteur `Fire` n'est pas sauvé** : ni dans le fichier du joueur, ni pour les mobs (qui
  ne sont pas sauvés du tout).
* **Les mobs immunisés** (blaze, strider, cube de magma…) n'existent pas parmi les huit espèces
  de ce serveur ; `EntityFire::fire_immune` est là pour eux. Aucun casque ne protège un zombie
  du soleil : les mobs de ce serveur ne portent rien.
* **Le joueur en créatif** ne brûle pas (vanilla non plus ne le blesse pas ; que son client
  affiche les flammes n'est pas mesuré).
* **Le feu de camp** : son contenu n'est pas lâché quand on le casse ; l'eau qui coule dans un
  feu de camp ne l'éteint pas (le moteur de fluides ne l'engorge pas) ; la fumée est une
  particule que le client dessine seul à partir de l'état `lit` — rien à envoyer.
* **La température au-dessus de y = 80** perd le terme de bruit de vanilla
  (`TEMPERATURE_NOISE`) dans la décision « il pleut ou il neige ici ».
