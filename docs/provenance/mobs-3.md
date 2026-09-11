# Les mobs, troisième vague — le coup, le despawn, la sauvegarde, le villageois zombie

Ce dossier reprend quatre choses que `mobs.md`, `mobs-2.md` et `villageois.md` avaient nommées sans
les faire : **aucun mob hostile ne blessait un joueur**, **`decide_despawn` n'était appelé par
personne**, **aucun mob n'était sauvé**, et la **zombification** mesurée n'était pas branchée. Tout
chiffre ci-dessous a été mesuré contre le vrai serveur 1.20.1 (`tools/vanilla/server.jar`) par
`scripts/measure_mobs3.py`, avec un client sonde connecté ; les preuves de bout en bout sur notre
serveur sont de `scripts/check_mobs3_e2e.py`. Les relevés bruts vont dans `.scratch/mobs3.json` et
`.scratch/mobs3_e2e.json` (non suivis). Ce qui n'est pas mesuré est dit là où c'est utilisé, et
regroupé au § 6.

Le résultat court :

| mesure | vanilla | avant | après |
|---|---|---|---|
| un zombie à côté d'un joueur en survie | frappe toutes les 20 ticks | **ne le vise même pas** (§ 1.1) | frappe toutes les 20 ticks |
| dégâts par difficulté (zombie 3) | 2,5 / 3 / 4,5 | — | 2,5 / 3 / 4,5 |
| armure cuir, fer, diamant, fer + Protection IV (normal) | 2,34 · 1,38 · 0,69 · 1,1592 | l'armure n'était appliquée à **aucun** dégât | identiques, 22 cellules sur 22 (§ 1.3) |
| Faim du husk (facile / normal / difficile) | aucune / 140 / 280 ticks | — | aucune / 140 / 280 |
| Poison de l'araignée venimeuse | aucun / 140 / 300 ticks | — | aucun / 140 / 300 |
| flèche de stray | Lenteur 600 ticks | flèche ordinaire | Lenteur 600 |
| squelette : difficulté | celle du monde | toujours « normal » (codé en dur) | celle du monde |
| explosion sur un joueur, facile | `min(D/2 + 1, D)` (6,0 pour 10) | 10 (pas d'échelle) | 6,0 |
| despawn au-delà de 128 blocs | immédiat | jamais | immédiat |
| despawn entre 32 et 128 | 600 ticks puis ≈ 1/800 par tick (ajusté : 600 et 1/850) | jamais | 600 puis 1/800 |
| vache, villageois, `PersistenceRequired` à 140 blocs | restent | — | restent |
| zombie nommé (`CustomName` seul) à 140 blocs | **disparaît** | — | disparaît |
| mobs dans `entities/r.x.z.mca` | oui | **aucun fichier** | écrits et relus, vanilla ↔ nous (§ 3) |
| villageois tué par un zombie | 0/10 · 37/90 · 10/10 (villageois.md) | meurt | 0 · ≈ ½ · toujours, garde métier, niveau, XP, offres |
| guérison | 3734 à 5974 ticks (villageois.md) | — | 3600 + tirage 0..2400, puis le même villageois |

---

## 1. Le coup

### 1.1 Pourquoi rien ne frappait : les joueurs ne sont pas dans le monde des entités

`mobs-2.md` § 6.1 disait qu'aucun mob ne blessait un joueur parce que `MeleeAttackGoal` comptait son
recul sans que rien ne transforme le coup en dégâts. **C'était pire** : le but de cible
(`NearestAttackableTargetGoal`) cherche dans le `EntityWorld`, et les joueurs de ce serveur n'y sont
pas — ils vivent dans la table des joueurs. Un zombie n'**acquérait** jamais un joueur. La course et
la poursuite mesurées dans `mobs-2.md` l'étaient sur le vrai serveur ; notre bout en bout ne mesurait
que l'errance.

Réparé comme l'élevage avait réglé la tentation : l'appelant liste à chaque tick les joueurs qu'un
mob hostile peut chasser — connectés, vivants, en survie ou aventure, dans l'Overworld, **aucun sur
Paisible** — dans `MobContext::quarries` (`mob_attack.hpp`). Le but de cible y cherche le type
joueur, avec la ligne de vue ; la cible d'un mob est alors soit une entité (`MobBrain::target`), soit
un joueur (`MobBrain::target_player`, son identifiant réseau). Le but d'attaque **ne blesse pas** :
il ajoute un `MobAttack` que le serveur termine (`mob_attacks.cpp`) — c'est la règle de la couche 9,
qui sait ce qu'est un coup mais pas à qui le dire.

### 1.2 Le deuxième bug, trouvé par le premier test qui laissait un zombie atteindre sa cible

Une fois la cible acquise, le zombie avançait de **0,4 bloc et s'arrêtait**. `Mob::tick` efface
l'intention de marche en haut de chaque tick, et `MeleeAttackGoal` ne la reposait qu'au tick où il
recalculait son chemin — un sur dix. Le mob était poussé un tick sur dix et la friction l'arrêtait.
Le but appelle maintenant `keep_walking` à chaque tick, comme l'errance et la reproduction. Le test
`a zombie hunts a player it can see and hits every 20 ticks` le garde.

### 1.3 Les dégâts (campagne `melee`)

**Protocole.** Superplat, minuit, `naturalRegeneration` coupé ; la sonde en survie ne bouge pas ;
un mob invoqué avec NBT (donc sans équipement ni bébé tiré — piège 32) à 1,5 bloc ; `Health` et
`time query gametime` relus ensemble en boucle ; chaque baisse est un coup. Soin par Soin instantané
sous 9 PV. L'armure est posée par `item replace`.

| mob (`attack_damage`) | difficulté | armure | perdu par coup (5 coups) | intervalle |
|---|---|---|---|---|
| zombie (3) | facile | aucune · cuir · fer · diamant · fer+P IV | 2,5 · 1,925 · 1,125 · 0,5625 · 0,945 | 20 |
| zombie | normal | idem | 3,0 · 2,34 · 1,38 · 0,69 · 1,1592 | 20 |
| zombie | difficile | idem | 4,5 · 3,645 · 2,205 · 1,1025 · 1,8522 | 20 |
| husk, noyé (3) | f / n / d | aucune | 2,5 / 3 / 4,5 | 20 |
| araignée, araignée venimeuse (2) | f / n / d | aucune | 2 / 2 / 3 | 20 |
| enderman provoqué (7) | normal | fer | 3,78 | 20 |
| zombie piglin provoqué (5) | normal | fer | 2,5 | 20 |

(Le premier intervalle lu vaut 18 : c'est l'échantillonnage de la console, pas une autre règle.)

Tout se lit avec trois règles, dans cet ordre, **en flottant** :

1. **la difficulté** (`scale_for_difficulty`), pour les types dont le `scaling` du data generator le
   demande — `when_caused_by_living_non_player` pour `mob_attack` et `arrow`, `always` pour
   `explosion` : rien en paisible, `min(a/2 + 1, a)` en facile, `a` en normal, `1,5·a` en difficile ;
2. **l'armure** (`after_armour`, formule du wiki) :
   `a · (1 − clamp(armure − a / (2 + robustesse/4), armure/5, 20) / 25)`, avec les points de la table
   du wiki (cuir 7, fer 15, diamant 20 et robustesse 8) ;
3. **la Résistance puis la Protection** (EPF ÷ 25), déjà mesurées (`effets.md`, `enchantement.md`).

Les 22 cellules tombent au chiffre imprimé près (`test_mobs3.cpp`, `[parity]`). L'armure est portée
par `DamageMitigation::armour`/`toughness`, relus des quatre cases d'armure du joueur **à chaque
tick** ; elle s'applique à tout dégât hors `#bypasses_armor` — flèche et explosion comprises, ce qui
n'était pas le cas avant cette vague.

**Le premier coup** arrive 20 à 23 ticks après l'invocation, puis un toutes les 20 : le but frappe au
premier tick à portée, puis attend `kMeleeCooldownTicks` = 20.

**La portée** est celle du jeu, `(2·l)² + l_cible` au carré, pieds à pieds (1,43 bloc pour un zombie
sur un joueur, 2,9 pour une araignée) : **documentaire**, non mesurée séparément — un mob qui frappe
depuis 2,2 blocs, notre ancienne portée, ne se distinguerait pas dans cette campagne où la sonde est
à 1,5 bloc. Le mob qui ne frappe pas (squelette, stray, sorcière : ils tirent ; creeper : il gonfle ;
slime : il blesse au contact, non fait) garde son `hold_at` : le creeper s'arrête désormais à 3 blocs,
là où son gonflement commence (`kCreeperSwellStart`).

**Le rayon de recherche** est l'attribut `follow_range` mesuré de chaque espèce (`entities.json`) : 35
pour la famille zombie, 64 pour l'enderman, **16** pour le creeper, l'araignée, le squelette et la
sorcière — là où tous recevaient 35. C'est vraisemblablement la réponse à la question laissée ouverte
par `mobs-2.md` § 1.4 (creeper, araignée et sorcière ne s'approchaient pas d'une sonde à 30 blocs) ;
**non remesuré**.

**Le recul** sur le joueur est l'impulsion de 0,4 déjà mesurée pour un coup de joueur, envoyée au
client par `Set Entity Velocity`, amortie par la résistance au recul de l'armure (netherite 0,1 par
pièce). **Non mesuré** pour un coup de mob : la sonde ne simule pas son propre mouvement.

### 1.4 Les effets au coup

| mob | facile | normal | difficile | lu sur le vrai serveur |
|---|---|---|---|---|
| husk : Faim | aucune | 140 | 280 | 137 lu 3 ticks après, 278 lu 2 ticks après |
| araignée venimeuse : Poison | aucun | 140 | 300 | 138, 298 |

La Faim du husk vaut `140 × ⌊difficulté régionale⌋` : dans un monde neuf la difficulté régionale
effective vaut `0,75 × id` (0,75, 1,5, 2,25), d'où 0, 1, 2 fois 140. La formule complète du wiki
(âge du monde, temps habité du chunk, lune — `effective_regional_difficulty`) est écrite ; **le temps
habité n'est pas suivi par ce serveur** et vaut 0, ce qui est exact pour un monde jeune et bas pour un
vieux. Le Poison de l'araignée venimeuse suit le wiki (7 s, 15 s). Le Wither du squelette wither est
dans la table ; l'espèce n'a pas de cerveau ici.

### 1.5 À distance (campagne `ranged`)

Squelette et stray invoqués **sans NBT** (pour qu'ils aient leur arc — piège 32), rendus persistants,
à 8 et 14 blocs, 45 s par cellule, la sonde en diamant :

| | coups reçus (8 / 14 blocs) | flèches restées au sol | Lenteur lue au coup |
|---|---|---|---|
| facile | squelette 7 / 6, stray 10 / 8 | 8 / 9, 5 / 7 | 598 (tous les coups de stray) |
| normal | 14 / 12, 14 / 13 | 1 / 4, 1 / 2 | 598 |
| difficile | 22 / 21, 22 / 22 | 1 / 0, 0 / 0 | 598 |

Quinze tirs en 45 s en facile et en normal, vingt-deux en difficile : les 60 et 40 ticks de
`projectiles.md`. La précision croît avec la difficulté (la moitié des tirs au sol en facile, presque
aucun en difficile), ce que `skeleton_aim` faisait déjà — **mais le serveur lui passait toujours 2**.
Il reçoit maintenant la difficulté du monde, et une flèche de mob sur un joueur est mise à l'échelle
comme un coup. **La flèche de stray** porte la Lenteur 600 comme effet *propre* (lu 598 un tick après
le coup, sur tous les coups et toutes les difficultés), appliquée entière par la règle des flèches
trempées de `alchimie.md`, et non divisée par 8.

### 1.6 Le creeper (campagne `creeper`)

Le gonflement (30 ticks, allumage à 3 blocs, abandon à 7) et l'explosion (puissance 3, 6 chargé)
étaient faits par `tnt-et-gravite.md`. Ce qui manquait : **le creeper n'approchait pas** (pas de
cible), et l'explosion ne suivait pas la difficulté sur un joueur. Creepers allumés `NoAI` à 4 blocs :

| difficulté | armure | perdu |
|---|---|---|
| facile | aucune · fer · diamant | 6,0 · 3,12 · 1,56 |
| normal | aucune | 10,0 |

D = 10 à cette distance, et `min(D/2 + 1, D)` = 6 en facile, puis l'armure : c'est l'échelle
`always` et la règle du § 1.3, que notre serveur applique maintenant. La mèche relue va de 31 à
32 ticks entre deux lectures de console (30 plus la latence de lecture). **Les autres cellules
(normal en armure, difficile, chargé) ne sont pas lues** : la sonde a perdu la lecture de sa santé
après les premières explosions (vraisemblablement éjectée par le recul qu'elle ne suit pas) — non
élucidé, et nommé. Le premier passage avait des creepers sans `NoAI` : ils marchaient sur la sonde en
gonflant et l'ont tuée deux fois (piège 4 du § 7).

---

## 2. Le despawn (campagne `despawn`)

**Protocole.** `doMobSpawning` coupé, minuit, la sonde en créatif immobile à l'origine, sa position
relue à chaque échantillon. Des enclos de verre avec 16 zombies non persistants à 16, 48, 96 et
140 blocs ; à 48 et 140, quatre vaches et deux villageois ; à 140, un zombie `CustomName` et un zombie
`PersistenceRequired`. Comptage toutes les 4 s pendant 170 s.

| ticks | zombies 16 | 48 | 96 | 140 | vaches 48/140 | villageois | nommé 140 | persistant 140 |
|---|---|---|---|---|---|---|---|---|
| 0 | 16 | 16 | 16 | **0** | 4 / 4 | 2 / 2 | **0** | 1 |
| 513 | 16 | 16 | 16 | 0 | 4 / 4 | 2 / 2 | 0 | 1 |
| 979 | 16 | 10 | 10 | 0 | 4 / 4 | 2 / 2 | 0 | 1 |
| 1466 | 16 | 5 | 9 | 0 | 4 / 4 | 2 / 2 | 0 | 1 |
| 1959 | 16 | 1 | 4 | 0 | 4 / 4 | 2 / 2 | 0 | 1 |
| 3058 | 16 | 0 | 1 | 0 | 4 / 4 | 2 / 2 | 0 | 1 |

* **Au-delà de 128 blocs** : partis avant le premier échantillon.
* **Entre 32 et 128** : rien pendant environ 600 ticks, puis une décroissance exponentielle. Le
  maximum de vraisemblance sur les 32 zombies (comptages censurés par intervalle) donne **un délai de
  600 et un taux de 1/850 par tick** ; `decide_despawn` porte 600 et 1/800 (`mobs.md` § 6 les disait
  choisis) : l'écart est dans le bruit de 32 mobs, et les constantes restent.
* **Sous 32** : aucun.
* **Les vaches et les villageois ne partent jamais**, à 48 comme à 140. Notre pass laisse désormais
  toute la catégorie `Creature` en place (seule la vache est mesurée — nommé) ; un villageois est de
  catégorie `Misc` pour le spawner et n'est donc jamais retiré.
* **Un `CustomName` seul ne protège pas** : le zombie nommé à 140 est parti avec les autres. Le
  mandat et le code d'origine supposaient le contraire ; c'est l'étiquette nommée (objet) qui protège,
  en posant `PersistenceRequired` — et c'est le seul drapeau lu.

**Le premier passage ne mesurait rien** : 16 zombies à 140 blocs sont restés 170 s. Il suivait la
campagne `creeper`, où la sonde était morte deux fois ; sans joueur valide près des enclos, rien n'est
retiré, et les chunks de spawn gardaient les enclos vivants. Le second passage a été mis en premier et
relit la position de la sonde (piège 5).

**Chez nous** (`mob_despawn.cpp`) : chaque mob, chaque tick, contre le joueur le plus proche (non
spectateur, créatif compris) ; `noActionTime` compté par mob et remis à 0 sous 32 blocs ; puis
`decide_despawn`. Paisible retire tous les monstres, persistants compris (règle du wiki, **non
mesurée**). Les mobs de `--mobs=` sont épinglés persistants : ce sont des montages de test. Le cap par
catégorie était déjà compté sur les mobs vivants ; le despawn le libère enfin.

---

## 3. La sauvegarde : `entities/r.x.z.mca`

### 3.1 Le format, lu sur un monde du vrai serveur

Depuis 1.17 les entités vivent à part des blocs : un fichier région par 32 × 32 chunks sous
`entities/`, chaque chunk `{DataVersion: 3465, Position: [I; x, z], Entities: [...]}`. La campagne
`anvil` a invoqué un zoo de 24 mobs `NoAI` avec leur NBT d'espèce (bébé zombie, zombie nommé,
creeper chargé `Fuse:40`, slime `Size:1`, veau, vache amoureuse, cochon sellé, mouton coloré tondu,
poulet, bibliothécaire désert niveau 3 `Xp:20`, villageois zombie fermier taïga niveau 2, loup et
chat avec `Owner`, et le reste des espèces à cerveau), puis `save-all flush`. Le `data get` de chaque
mob a donné les clés et leurs types : `Pos` et `Motion` en doubles, `Rotation` en flottants, `UUID`
en `[I; …]` de quatre entiers, `Health` en flottant, `Fire`, `Air`, `HurtTime`, `DeathTime` en
shorts, `PersistenceRequired`, `OnGround`, `Invulnerable` en octets, `CustomName` en texte JSON
(`'{"text":"Bob"}'`), `Brain: {memories: {}}`, `Attributes`, `HandItems`, `ArmorItems` et leurs
`DropChances` ; puis par espèce `IsBaby`, `DrownedConversionTime`, `InWaterTime`, `CanBreakDoors`
(famille zombie), `Age`, `ForcedAge`, `InLove` (animaux), `Color`, `Sheared` en octets (mouton),
`Saddle`, `EggLayTime`, `Size` (la taille **moins un**), `powered`, `Fuse` (short),
`ExplosionRadius`, `ignited`, `VillagerData: {profession, level, type}` en noms `minecraft:`, `Xp`,
`Offers: {Recipes: [{buy, buyB, sell, uses, maxUses, xp, priceMultiplier, specialPrice, demand,
rewardExp}]}` avec des objets `{id, Count, tag}`, `Gossips`, `RestocksToday`, `LastRestock`,
`ConversionTime`.

### 3.2 Ce que fait notre serveur

`entity_storage.cpp`. **Écrit** depuis son propre état tout ce qu'il modélise : position, mouvement,
rotation, UUID, santé, `PersistenceRequired`, `CustomName`, l'âge et l'amour d'un animal, la laine,
la selle, l'œuf, les données, l'XP et les offres d'un villageois, la taille d'un slime, la charge d'un
creeper, le villageois d'un villageois zombie et sa guérison. **Rend intact** tout le reste d'un mob
lu sur disque (`Brain`, `Attributes`, l'`Owner` d'un loup, les `Gossips`, un `Fuse` de 40…), et
**toute entité qu'il ne fait pas vivre** (un objet au sol, une flèche, un tableau) retourne dans son
chunk telle qu'elle a été lue. Un champ qu'il ne comprend pas n'est pas un champ qu'il peut jeter.

Le cycle : les chunks résidents dont les entités n'ont pas été lues le sont toutes les 10 ticks ; un
chunk déchargé écrit ses mobs et les retire du monde (sous le verrou des joueurs — sans lui, rien
n'est déchargé à ce passage) ; chaque sauvegarde du monde écrit tous les chunks chargés. Un
`DataVersion` autre que 3465 est refusé et nommé dans le journal.

### 3.3 Aller et retour

* **Nous → nous** (`test_mobs3_server.cpp`) : zombie nommé persistant, mouton, bibliothécaire avec
  une offre usée, slime de taille 4, creeper chargé — écrits, relus par un deuxième monde, champ par
  champ ; le fichier a bien `DataVersion`, `Position` et une liste `Entities` à la vanilla.
* **Déchargement puis rechargement d'un chunk** : ses deux vaches partent sur disque et reviennent.
* **Vanilla → nous** (même fichier, `[parity]`) : le zoo du vrai serveur, lu par notre serveur —
  **24 mobs sur 24**, tous `PersistenceRequired` ; bibliothécaire (désert, niveau 3, `Xp` 20, deux
  offres, dont un livre de Leurre III), villageois zombie (taïga, fermier, 2, `Xp` 12), mouton
  (couleur 3, tondu), slime (`Size` 1, soit taille 2), creeper chargé dont le `Fuse` de 40 ressort
  tel quel, et l'`Owner` du loup rendu intact ; 43 assertions. Le zoo est reconnu à son `Tags:["zoo"]`,
  lui aussi rendu intact.
* **Nous → vanilla** (`measure_mobs3.py anvil_back`) : le vrai serveur lancé sur le monde que le
  nôtre a réécrit (§ 5.4) compte **24 mobs `tag=zoo` sur 24**, et `data get` y relit ce que notre
  serveur a écrit : `VillagerData`, `Xp` et `Offers` du bibliothécaire, `VillagerData`, `Xp` et
  `ConversionTime:-1` du villageois zombie, `powered:1b` et `Fuse:40s` du creeper, `Size:1` du slime,
  `Color:3b` et `Sheared:1b` du mouton, `Saddle:1b`, l'`Owner` du chat, `PersistenceRequired` et les
  `Tags` partout. L'`Age` du veau est passé de −24000 à −23773 et l'`InLove` de la vache de 600 à 373 :
  les 227 ticks que notre serveur a fait vivre le zoo, et c'est juste. L'araignée et le loup ne sont
  pas relus par position (`distance=..1.5`) : **notre serveur ignore `NoAI`**, ils ont marché pendant
  sa minute de vie ; ils sont dans le compte des 24.
* **De bout en bout** : § 5.

### 3.4 Un seul écrivain pour `entities/`

**Le défaut.** À la fusion de cette vague (3c378f6), deux modules écrivaient les mêmes fichiers
`entities/r.x.z.mca`, chacun depuis sa propre copie de leur contenu : `RailsSession` lisait **tous**
les fichiers au démarrage, gardait les non-wagonnets en `kept_` et réécrivait chaque chunk lu ou
écrit avec `kept_` + les wagonnets vivants ; `EntityStorage` gardait ce qu'il ne fait pas vivre (les
wagonnets compris) en `foreign_`, dans l'état du chargement. Le dernier écrivain effaçait l'état
récent de l'autre : un wagonnet dans un chunk qui contient aussi des mobs était sauvé **là où il avait
été lu**, un wagonnet posé depuis pouvait **disparaître** au redémarrage, et au déchargement d'un
chunk `EntityStorage` réécrivait des `foreign_` périmés — en laissant le wagonnet vivant dans le
monde, dans un chunk déchargé.

**La conception.** `EntityStorage` est désormais le **seul** code qui ouvre `entities/` : lecture par
chunk résident, écriture au déchargement et à chaque sauvegarde. Un module qui fait vivre ses propres
entités s'y branche par l'interface `EntityAdopter` (`entity_storage.hpp`) :

| appel | quand | ce que fait l'adoptant |
|---|---|---|
| `owns(type)` | à la lecture et à l'écriture | dit si le type (identifiant protocole) est le sien |
| `adopt_saved(world, compound)` | lecture d'un chunk | fait apparaître l'entité et la fait vivre, en gardant tout le composé (`Passengers` compris) ; `nullopt` = refusée, le stockage la rend intacte en `foreign_` et le nomme dans le journal |
| `save_entity(world, handle)` | écriture d'un chunk | rend le composé 1.20.1 de l'entité vivante (`cart_nbt` pour un wagonnet) |
| `release(world, handle)` | déchargement du chunk | oublie l'entité (et son passager) avant que le stockage la retire du monde |

Le stockage écrit chaque entité **dans le chunk où elle se tient au moment de l'écriture** ; le chunk
qu'elle a quitté est réécrit sans elle parce qu'il est dans `on_disk_` (ce que faisait `written_` côté
rails). Un chunk qui ne contient qu'un wagonnet est écrit comme un chunk qui ne contient qu'un mob. Au
déchargement, le wagonnet est écrit, `release`, retiré du monde, et son identifiant part dans le
`Remove Entities` des mobs.

**Un deuxième défaut, trouvé en écrivant les tests** : une sauvegarde qui écrit un chunk dont le fichier
**n'a pas encore été lu** (un chunk résident depuis moins de 10 ticks, ou jamais lu, où un mob ou un
wagonnet vient d'entrer) remplaçait son entrée sur disque par les seules entités vivantes — et la
lecture suivante aurait ramené **en double** ce qui était déjà vivant. `save_all` et `unload_chunks`
lisent maintenant d'abord ces chunks (`read_before_write`), puis écrivent.

`RailsSession::load` / `save` n'existent plus ; `server.cpp` n'a plus qu'un appel de sauvegarde
(bloc `── entities ──`) et l'enregistrement de l'adoptant après la construction du stockage.

**Les autres entités.** Inventaire de ce qui vit hors des mobs, et de ce qui en est sauvé :

| module | entités | sauvé ici ? | vanilla |
|---|---|---|---|
| `rails_session` | les 7 wagonnets | **oui, adoptant** | dans le chunk |
| `tnt_gravity` | TNT amorcée, bloc qui tombe | non (`transient`) | sauvés dans le chunk (`Fuse`, `BlockState`, `Time`) |
| `projectiles` | flèches, projectiles | non (`transient`) | sauvés dans le chunk |
| `server.cpp` `ground_items` / `ground_orbs` | objets au sol, orbes | non (hors `EntityWorld`) | sauvés dans le chunk |
| `nether_mobs` | les mobs du Nether | non (magasin à part) | `DIM-1/entities/` |
| `end_fight` | le dragon, les orbes de l'End | non (nommé dans `end_fight.hpp`) | `DIM1/entities/` |

Aucun de ceux-là n'écrit dans `entities/` : il n'y a pas d'autre conflit d'écrivain. Ce sont des
**manques de persistance**, nommés, pas des écrivains concurrents : chacun pourra devenir adoptant par
la même interface. Les entités de ces types lues dans un monde vanilla restent rendues intactes
(`foreign_`).

**Preuves.**

* **Tests unitaires** (`test_rails_session.cpp`, `[entities]`, 4 cas, 82 assertions) : un chunk écrit
  à la vanilla avec un cochon, un wagonnet à fourneau et un tableau revient entier (cochon vivant,
  wagonnet adopté avec `Fuel` 1200 et son `DisplayState`, tableau rendu intact avec son `variant`),
  deux sauvegardes de suite n'en écrivent qu'un de chaque, et un redémarrage relit un cochon et un
  wagonnet ; un wagonnet voisin d'une vache passe du chunk (0,0) à (1,0) puis à (37,0) — chaque
  sauvegarde l'écrit là où il est et nulle part ailleurs, (0,0) garde la vache seule, et le
  redémarrage le relit **une fois**, à x = 600,5, même UUID ; un chunk qui ne contient qu'un wagonnet
  est écrit au déchargement, le wagonnet retiré du monde (la vache d'ailleurs reste) puis relu une fois ;
  un chunk non lu (un tableau sur disque) où un wagonnet entre garde le tableau à la sauvegarde.
* **Suite complète** : `ctest --preset macos-debug`, 16/16.
* **De bout en bout** (`scripts/check_entities_e2e.py`, `ov_dedicated`, port 25651) : 31 rails de
  x = 0 à 30, un wagonnet posé à (2,5 ; −59,9375 ; 4,5) et une vache invoquée à (6,5 ; −60 ; 9,5) —
  enfermée dans un anneau de verre de deux blocs, parce qu'une vache libre est sortie du chunk dans
  les deux premiers passages (notre serveur ignore `NoAI`) —, les deux dans le chunk (0,0) ; la sonde
  pousse le wagonnet jusqu'à x = 17,54 sur le fil, descend ; `stop`. Le fichier : **une** vache, dans
  (0,0), à (6,5 ; −60 ; 9,5) ; **un** wagonnet dans tout `entities/`, dans (1,0), à
  (19,573 ; −59,9375 ; 4,5), seul dans son chunk. Redémarrage : un seul `Spawn Entity` de wagonnet, à
  la position du fichier, et la vache à la sienne.
* **Relu par le vrai serveur 1.20.1** (`lockf /tmp/ov-vanilla.lock python3
  scripts/check_entities_e2e.py readback`, port 25652) : sur le monde que l'étape précédente a
  sauvé, `execute as @e[type=…] run data get entity @s Pos` compte **1 wagonnet** et **1 vache** —
  la vache à (6,5 ; −60 ; 9,5) exactement ; le wagonnet à x = 20,050 pour 19,573 écrit, sur la même
  ligne (y et z identiques). Cause **supposée, non vérifiée** : il a continué de rouler sur le
  `Motion` sauvé pendant les ~4 s où vanilla a fait tourner le monde avant la lecture ; ce passage
  ne relit pas le `Motion` pour le prouver.

**Passagers, nommé.** Un joueur sur un wagonnet n'est jamais écrit dans le chunk ; le wagonnet l'est, à
sa place, et au redémarrage le joueur n'y est pas rassis. Vanilla range le véhicule d'un joueur dans le
`RootVehicle` de ses données (format documenté de `player.dat`) — non fait ici. Un mob passager d'un
wagonnet lu dans un monde vanilla voyage dans le `Passengers` du composé gardé, sans être animé.

---

## 4. Zombification et guérison

`zombie_villagers.cpp`, sur les mesures de `villageois.md` § 10.

* **La montée.** Un villageois tué par un zombie, un husk, un noyé ou un villageois zombie se relève
  en villageois zombie : jamais en facile (mesuré 0/10), une fois sur deux en normal
  (`next_int(2)`, mesuré 37/90), toujours en difficile (10/10). Il garde type, métier, niveau, XP et
  offres (mesuré) ; sa métadonnée 20 (VillagerData) est envoyée à l'apparition. Le zombie chasse le
  villageois **sans ligne de vue** (`mobs.md` § 0), à la priorité 3 ; un joueur vu prend la cible
  (priorité 2).
* **La Faiblesse.** Les effets de mob ne sont pas modélisés sur ce serveur ; celle-ci l'est, parce
  que la guérison la demande : une potion jetable dont les effets contiennent la Faiblesse la donne
  aux villageois zombies dans sa boîte, à `1 − d/4` de sa durée. **`/effect` sur une entité non
  joueur n'existe pas ici** — nommé.
* **La guérison.** Une pomme d'or sur un villageois zombie affaibli (consommée hors créatif) :
  `ConversionTime` = 3600 + `next_int(2401)` (le wiki ; mesuré 3734 à 5974 sur 23), métadonnée 19 à
  vrai, événement d'entité 16. Chaque tick retire 1, et un tick sur cent chaque barreau de fer ou lit
  à moins de quatre blocs a trois chances sur dix de retirer un tick de plus, quatorze blocs au plus
  (le wiki ; **non mesuré**). À zéro : le même villageois, métier, niveau, XP et offres.
* **Non fait, et nommé** : la remise de prix — elle passe par les ragots (`major_positive`,
  `minor_positive`) et **ce serveur n'a pas de réputation** (`villageois.md` § 12) ; la Force pendant
  la guérison et la Nausée à la fin (effets de mob) ; les événements sonores de niveau 1026/1027.

---

## 5. De bout en bout, sur notre serveur

`check_mobs3_e2e.py` : `ov_dedicated` sur un monde neuf, la sonde protocole 763 jugée sur le fil
seulement — chaque `Set Health` (0x57) qui baisse est un coup, daté par `Update Time`, et chaque
`Entity Effect` (0x6C) reçu pour elle-même est lu.

### 5.1 Le coup (`melee`)

Mob invoqué à 2 blocs de la sonde en survie, armure posée en créatif par `Set Creative Slot` :

| mob | difficulté | armure | perdu par coup (5 coups), notre serveur | vanilla | intervalle | effet reçu |
|---|---|---|---|---|---|---|
| zombie | facile | aucune · fer · diamant | 2,5 · 1,125 · 0,5625 | idem | 20 | — |
| zombie | normal | aucune · fer · diamant | 3,0 · 1,38 · 0,69 | idem | 20 | — |
| zombie | difficile | aucune · fer · diamant | 4,5 · 2,205 · 1,1025 | idem | 20 | — |
| husk | f / n / d | aucune | 2,5 / 3 / 4,5 | idem | 20 | aucun / Faim 140 / Faim 280 |
| araignée venimeuse | f / n / d | aucune | 2 / 1 (voir plus bas) / 3 | 2 / 2, 1, 1 / 3 | 20 | aucun / Poison 140 / Poison 300 |

**Quinze cellules sur quinze** au chiffre imprimé, par le vrai chemin de code (buts, `MobAttacks`,
`SurvivalSession::hurt` avec l'armure). En normal, l'araignée venimeuse empoisonne : le Poison ouvre
la fenêtre d'invulnérabilité toutes les 25 ticks, et un coup de 2 qui tombe dedans ne passe que pour
la différence, 1 — le vrai serveur lit 2, 1, 1 pour la même raison (§ 1.3).

### 5.2 Le despawn (`despawn`)

Quatre zombies à 16 blocs, huit à 100, la sonde en créatif immobile ; les `Remove Entities` reçus
comptés toutes les 5 s pendant 150 s, puis la sonde téléportée à 300 blocs :

| secondes (horloge murale) | 5 → 70 | 76 | 86 | 106 | 131 | 141 → 151 | après la téléportation |
|---|---|---|---|---|---|---|---|
| zombies à 100 blocs | 8 | 7 | 6 | 4 | 3 | 2 | — |
| zombies à 16 blocs | 4 | 4 | 4 | 4 | 4 | 3 | **0** |

La forme est celle du vrai serveur : rien pendant le délai d'inactivité, puis un amincissement ; tout
part d'un coup au-delà de 128 blocs. **Les temps sont en secondes d'horloge et non en ticks** : le
serveur Debug, sur une machine chargée, tournait sous 20 ticks par seconde (le délai de 600 ticks est
tombé vers 70 s), et cette vérification ne relevait pas `Update Time` — la loi est jugée par les tests
unitaires et le § 2, pas ici. Le zombie « proche » parti à 141 s n'était pas enfermé : il avait erré
(la sonde en créatif n'est pas une cible), vraisemblablement au-delà de 32 blocs — non vérifié.

### 5.3 Le villageois zombie (`villager`)

Un villageois et un zombie dans un enclos de verre de 3 × 3, en difficile ; puis, du haut du mur,
une potion jetable de Faiblesse lancée droit vers le bas (`Set Creative Slot` avec son NBT, `Use
Item`), la pomme d'or en main et un `Interact` sur le villageois zombie :

```
villageois zombie        apparu (Spawn Entity du type 120), métadonnée 20 présente
guérison lancée          métadonnée 19 = vrai, 10 s après l'invocation
guéri                    4018 ticks plus tard (3600 à 6000 attendus ; vanilla 3734 à 5974)
villageois               réapparu à la même place, métadonnée 18 présente
```

La chaîne entière passe par le vrai chemin de code : le but de cible villageois, `MobAttacks`, la mort
et la montée (`ZombieVillagers::on_villager_killed`), la potion cassée par `Projectiles` puis
`on_splash`, le clic, le compte à rebours. **Le contenu** des indices 20 et 18 n'est pas décodé par
cette sonde (son analyseur ne connaît pas le type VillagerData) : leur présence est vue, leurs octets
sont ceux de l'encodeur vérifié octet pour octet par `villageois.md` § 2 ; que le métier et le niveau
soient conservés est jugé par `test_mobs3_server.cpp`.

### 5.4 Le monde du vrai serveur, servi par le nôtre (`anvil`)

`ov_dedicated` lancé sur une copie du monde du zoo (`measure_mobs3.py anvil`), la sonde posée à
l'origine ; chaque `Spawn Entity` compté par type, puis `save-all` :

```
types reçus        22 sur 22, 24 mobs sur 24, aucun manquant
slime              indice 16 = 2      (Size:1 sur disque, soit la taille 2)
mouton             indice 17 = 19     (couleur 3, bit 0x10 tondu)
villageois, villageois zombie   indices 18 et 20 présents (non décodés par la sonde)
sauvegarde         24 mobs réécrits dans entities/ (2 puis 3 chunks)
```

**Le premier passage n'a rien vu** : un monde lu sur disque n'a pas de zone de spawn gardée résidente
(elle ne l'est que pour un monde que ce serveur génère), ses chunks arrivent avec le joueur, et ce
serveur Debug a tourné 147 ticks en 27 s sur une machine chargée — les chunks, et donc leurs
entités, sont arrivés après les 20 s d'attente de la sonde. Rien n'était faux dans le chargement ; le
passage suivant a tout vu, et l'attente est portée à 45 s. Nommé parce que le symptôme — « aucun mob
relu » — ressemble exactement à un chargement cassé.

---

## 6. Ce qui n'est pas fait, ou pas mesuré

* **La sorcière** ne lance pas de potions (elle garde ses 10 blocs et ne fait rien d'autre) ; le
  **squelette wither** n'a pas de cerveau (sa table d'effet est prête) ; le **slime** ne blesse pas au
  contact ; l'**araignée** reste hostile en plein jour et ne bondit pas ; l'**enderman** et le
  **zombie piglin** ne se mettent pas en colère (rien ne les provoque ici).
* **Le recul** d'un coup de mob sur un joueur : impulsion de 0,4 et vitesse précédente prise nulle,
  non mesurés. **L'usure de l'armure** par les coups : non faite.
* **Le temps habité** d'un chunk (difficulté régionale) : non suivi, pris à 0.
* **La portée** `(2·l)² + l_cible` et le **premier coup au premier tick à portée** : documentaires.
* **Le creeper** en normal avec armure, en difficile et chargé : non lus (§ 1.6).
* **Le despawn** des autres animaux que la vache, et le retrait des monstres en Paisible : règles
  appliquées, non mesurées.
* **La persistance par objet ramassé** : les mobs ne ramassent rien ici. **L'étiquette nommée** (objet)
  n'est pas faite ; `/summon` refuse le NBT (`not_modelled`).
* **Les mobs de `--mobs=`** sont sauvés et relus : au redémarrage sur le même monde, `--mobs=` en
  ajoute d'autres. Montage de test, nommé.
* **`NoAI`** est gardé dans le NBT (rendu intact) mais **ignoré** : un mob relu avec `NoAI:1b` a un
  cerveau et marche (§ 3.3). De même `IsBaby`, `Invulnerable`, `Silent`, l'équipement : rendus, non
  appliqués.
* **Les effets de mob** en général (Nausée, Force) ; la remise de prix d'un villageois guéri.

---

## 7. Pièges payés

1. **Les joueurs ne sont pas des entités du `EntityWorld`.** Tout but qui cherche « le joueur le plus
   proche » dans le monde des entités ne trouve rien, et un mob hostile qui erre au lieu de chasser
   ressemble à un mob calme. C'était la cause de « aucun mob ne blesse un joueur ».
2. **Une intention effacée à chaque tick doit être reposée à chaque tick.** `MeleeAttackGoal` ne la
   posait qu'en recalculant son chemin : le zombie avançait de 0,4 bloc et s'arrêtait.
3. **`moon_brightness` existait déjà** (`spawn_rules.hpp`, mobs-2) : la redéfinir ailleurs donne un
   symbole en double à l'édition de liens, pas à la compilation.
4. **Un creeper allumé garde son IA** : il marche sur la sonde en gonflant. `NoAI:1b` ne l'empêche pas
   d'exploser (la mèche n'est pas de l'IA).
5. **Une campagne de despawn qui suit une campagne où la sonde est morte ne lit rien** : relire la
   position de la sonde à chaque échantillon.
6. **Les campagnes suivantes tuent le zoo** (`kill @e[type=!player]`) : la copie du monde vanilla doit
   être prise juste après `anvil`, pas à la fin du passage. La première copie ne contenait que les
   enclos du despawn.
7. **Un `CustomName` n'épingle pas un mob** (§ 2).
8. **Le verrou partagé des serveurs vanilla** est tenu par les campagnes des autres agents : un
   passage peut attendre une demi-heure avant de démarrer.
9. **Un port libre à un moment ne l'est plus au suivant** : le serveur vanilla d'un autre agent a pris
   25657 entre deux vérifications de bout en bout ; `ov_dedicated` n'a pas pu se lier, **et la sonde
   s'est connectée à ce serveur-là** sans erreur — seule la console cassée (`BrokenPipe`) l'a dit. Le
   port se choisit par `OV_E2E_PORT`, et le journal du serveur doit montrer « listening » avant de
   croire quoi que ce soit.

---

## 8. Fichiers

| fichier | rôle |
|---|---|
| `src/ov_gameplay/{include/ov/gameplay,src}/mob_attack.{hpp,cpp}` | proies, coups, portée, difficulté, difficulté régionale, effets au coup, table d'armure |
| `src/ov_gameplay/{include/ov/gameplay/damage.hpp,src/damage.cpp}` | `after_armour`, l'armure dans `DamageMitigation` (blocs `mobs-3`) |
| `src/ov_gameplay/{include/ov/gameplay/goals.hpp,src/goals.cpp}` | cible joueur, cible villageois, portée du jeu, `keep_walking` (blocs `mobs-3`) |
| `src/ov_gameplay/{include/ov/gameplay/mob_logic.hpp,src/mob_logic.cpp,src/mob_species.cpp}` | `melee`, `hunts_villagers`, `follow_range`, le villageois zombie |
| `src/ov_server/src/mob_attacks.{hpp,cpp}` | le coup terminé : joueur blessé, repoussé, effet ; villageois blessé, tué |
| `src/ov_server/src/mob_despawn.{hpp,cpp}` | le despawn et les fiches des mobs (persistance, nom, `noActionTime`) |
| `src/ov_server/src/entity_storage.{hpp,cpp}` | `entities/r.x.z.mca`, seul lecteur et seul écrivain, champs rendus intacts, `EntityAdopter` (§ 3.4) |
| `src/ov_server/src/rails_session.{hpp,cpp}` | adoptant des 7 wagonnets (`adopt_saved`, `save_entity`, `release`) |
| `src/ov_server/tests/test_rails_session.cpp` | un seul écrivain : mob + wagonnet + étranger, wagonnet déplacé, chunk à un seul wagonnet, chunk non lu |
| `scripts/check_entities_e2e.py` | wagonnet et vache dans le même chunk, wagonnet déplacé, redémarrage ; `readback` par vanilla |
| `src/ov_server/src/zombie_villagers.{hpp,cpp}` | la montée, la Faiblesse, la guérison |
| `src/ov_server/src/projectiles.{hpp,cpp}` | difficulté du monde, flèche de stray, flèches de mob mises à l'échelle (blocs `mobs-3`) |
| `src/ov_server/src/tnt_gravity.hpp` | `creeper_powered` |
| `src/ov_server/src/server.cpp` | blocs `// ── mobs-3 ──` |
| `src/ov_gameplay/tests/test_mobs3.cpp`, `src/ov_server/tests/test_mobs3_server.cpp` | les mesures `[parity]`, les buts, la sauvegarde, le despawn, la guérison |
| `scripts/measure_mobs3.py` | l'oracle (`melee ranged creeper despawn anvil anvil_back`) |
| `scripts/check_mobs3_e2e.py` | la preuve de bout en bout |

```bash
lockf /tmp/ov-vanilla.lock python3 scripts/measure_mobs3.py melee ranged creeper despawn anvil
python3 scripts/check_mobs3_e2e.py melee despawn villager anvil
lockf /tmp/ov-vanilla.lock python3 scripts/measure_mobs3.py anvil_back
```
