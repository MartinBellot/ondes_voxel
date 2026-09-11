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
> verts. Les quatre campagnes de l'oracle (`blocks`, `rain`, `lava`, `entity`) sont écrites et
> **en file sur le verrou partagé du JVM** (`/tmp/ov-vanilla.lock`), derrière une dizaine de
> campagnes d'autres agents : les §§ 1 à 7, qui porteront leurs chiffres, ne sont pas encore
> écrits. Jusque-là, chaque nombre du code est celui du wiki, et le code le dit.

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
