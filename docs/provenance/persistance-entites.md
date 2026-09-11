# Persistance des entités qui ne sont pas des mobs

> Vague du 2026-09-11. Avant elle, `EntityStorage` (le seul écrivain de `entities/`, `mobs-3.md`
> § 3.4) sauvait les mobs de l'Overworld et les wagonnets ; tout le reste mourait avec le processus :
> objets au sol, orbes, flèches, tridents, lancers, TNT amorcée, blocs qui tombent, nuages, mobs du
> Nether, dragon et cristaux de l'End, et le wagonnet du joueur qui se déconnecte assis dedans.

---

## 1. Les sources

- **minecraft.wiki**, « Entity format » (les clés communes, puis une section par type), « Player.dat
  format » (`RootVehicle`) : les **noms** des champs.
- **Le vrai serveur 1.20.1** (`tools/vanilla/server.jar`), campagne `oracle` de
  `scripts/measure_persistence.py` : chaque type invoqué sur un superplat, lu par
  `data get entity` (le SNBT typé : `3b`, `40s`, `1.0f`, `2.0d`), puis le monde sauvé et ses fichiers
  `entities/`, `DIM-1/entities`, `DIM1/entities` et `playerdata/` relus. Ce sont ces **types** que le
  code écrit, et que les tests vérifient un par un. Le monde est gardé dans
  `.scratch/persistence-vanilla/` pour la relecture par notre serveur.

Aucun code d'un autre projet n'a été lu.

---

## 2. Ce que le vrai serveur écrit (mesuré)

Les clés communes à toute entité : `Pos`, `Motion` (doubles), `Rotation` (deux flottants), `UUID`
(quatre entiers), `OnGround`, `Invulnerable` (octets), `Air` (court, 300), `Fire` (court),
`FallDistance` (flottant), `PortalCooldown` (entier). **`Fire` vaut −1 pour un objet, une orbe, une
TNT, et 0 pour une flèche, un lancer, un bloc qui tombe, un cristal, un nuage, un dragon** — mesuré
type par type ; un `default` unique aurait été faux pour la moitié d'entre eux.

| type | clés propres, types mesurés |
|---|---|
| `item` | `Item` {id, `Count` octet, `tag`}, `Age` **court**, `PickupDelay` court, `Health` court — **5** pour une pile tombée d'un cratère, **0** pour une pile invoquée sans (mesuré les deux) |
| `experience_orb` | `Value` **court**, `Count` **entier** (combien d'orbes de cette valeur l'entité représente), `Age` court, `Health` court (5) |
| `arrow` | `life` court, `shake` octet, `inGround` octet, `inBlockState` {Name, Properties}, `pickup` octet, `damage` double, `crit`, `ShotFromCrossbow`, `PierceLevel` octets, `SoundEvent` (`minecraft:entity.arrow.hit`), `HasBeenShot`, `LeftOwner` octets (`LeftOwner` absent tant qu'il est faux), `Owner` si un tireur |
| `spectral_arrow` | ceux de la flèche, et `Duration` entier (200) |
| `trident` | ceux de la flèche (même `SoundEvent`, mesuré), `Trident` {id, Count, tag}, `DealtDamage` octet |
| `snowball`, `egg`, `ender_pearl`, `experience_bottle` | les clés communes et `HasBeenShot` — **pas d'`Item`**, même invoqué avec un `Item` égal à l'objet par défaut (mesuré sur la boule de neige) |
| `potion` | les mêmes, et `Item` {splash_potion ou lingering_potion, tag Potion} — toujours |
| `tnt` | `Fuse` **court**, sous ce nom en 1.20.1 (la mèche restante) |
| `falling_block` | `BlockState` {Name, Properties}, `Time` **entier**, `DropItem`, `HurtEntities`, `CancelDrop` octets, `FallHurtMax` entier (40), `FallHurtAmount` flottant |
| `area_effect_cloud` | `Age`, `Duration`, `DurationOnUse`, `WaitTime`, `ReapplicationDelay` **entiers** (l'`Age` d'un nuage est un entier, celui d'un objet un court), `Radius`, `RadiusOnUse`, `RadiusPerTick` flottants, `Particle` (`minecraft:entity_effect`), `Potion` |
| `end_crystal` | `ShowBottom` octet (1 pour les dix des piliers), `BeamTarget` {X, Y, Z} entiers, absent sans faisceau |
| `ender_dragon` | les clés d'un vivant (`Brain`, `Attributes`, `HandItems`…), `Health`, `DragonPhase` **entier**, `DragonDeathTime` entier |
| mobs du Nether | les clés d'un mob de l'Overworld (`mobs-3.md` § 3), plus : piglin `TimeInOverworld`, `Inventory` ; piglin brute `TimeInOverworld` ; zombifié `AngerTime`, et les clés de la famille zombie ; ghast `ExplosionPower` octet ; cube de magma `Size` entier, `wasOnGround` ; hoglin et strider les clés d'un animal (`Age`, `ForcedAge`, `InLove`), strider `Saddle` |

**`RootVehicle`** — mesuré sur la sonde assise dans un wagonnet par `/ride` :
`{Attach: [I; …], Entity: {id: "minecraft:minecart", Pos, Motion, UUID, …}}`, où `Attach` est
l'UUID du wagonnet et `Entity` son composé **sans** le joueur dans ses `Passengers`. La sonde se
déconnecte : **0** wagonnet dans le monde (il part avec elle) ; elle revient : **1** wagonnet, et la
sauvegarde suivante réécrit le même `RootVehicle` — le joueur y est de nouveau assis. (La commande
qui devait le lire directement, `execute as … on vehicle`, n'a rien rendu, ni avant ni après : la
remise en selle est **déduite** du `RootVehicle` réécrit, pas lue.)

Où chaque entité est rangée : dans le chunk où elle se tient au moment de la sauvegarde, dans
`entities/` pour l'Overworld, `DIM-1/entities` pour le Nether (les neuf mobs relus là),
`DIM1/entities` pour l'End (les dix cristaux des piliers, le mien, et le dragon).

---

## 3. La conception : un écrivain par dimension, deux sortes d'adoptants

Chaque dimension a **son** `EntityStorage`, seul lecteur et seul écrivain de son répertoire
(`dimension_entities.hpp` enveloppe ceux du Nether et de l'End). Les modules s'y branchent :

| module | entités | branchement |
|---|---|---|
| `tnt_gravity` | TNT amorcée, bloc qui tombe | `EntityAdopter` (ils vivent dans le monde d'entités de l'Overworld) |
| `projectiles` | flèches, flèche spectrale, trident, boule de neige, œuf, perle, fiole d'expérience, potion jetée | `EntityAdopter` |
| `rails_session` | les 7 wagonnets | `EntityAdopter` (vague précédente) |
| `ground_entities` | objets au sol, orbes — une instance par dimension | **`LooseAdopter`** |
| `brewing_session` | nuages de potion persistante | `LooseAdopter` |
| `end_fight` | dragon, cristaux | `LooseAdopter` (dans `DIM1/entities`) |
| `nether_mobs` | les mobs du Nether | le stockage de `DIM-1/entities` lit **dans le monde d'entités du Nether**, avec un `EntityStorageHost` que `NetherMobs::storage_host` fournit |

**`LooseAdopter`** est nouveau : pour les entités qui ne vivent pas dans un `EntityWorld` (les objets
et les orbes sont deux listes du serveur, les nuages une liste de `Brewing`, le dragon un
`gameplay::Dragon` du combat). Il dit quels types il reprend (`owns_type`), les ramène
(`adopt_saved`), dit où se tiennent les siens (pour lire d'abord les chunks pas encore lus :
`read_before_write`), rend leurs composés (`save`) et rend puis oublie ceux d'un chunk qui part
(`release`, avec leurs identifiants pour le `Remove Entities`).

**`transient` ne cache plus un adoptant.** Le serveur appelle transitoires la TNT et les projectiles
pour que le despawn les laisse tranquilles ; le stockage les sautait pour la même raison. Il
regarde maintenant l'adoptant d'abord : un type qu'un module fait vivre est sauvé.

**Le Nether** : le stockage de `DIM-1/entities` lit les mobs dans le monde d'entités du Nether ;
`storage_host` leur donne le cerveau qu'une apparition donne (`behaviour`, extrait de
`Impl::spawn`), relit la main principale (`HandItems`) et la taille d'un cube de magma, écrit
`AngerTime` et `ExplosionPower` par défaut, et **ignore** les doublures des joueurs (le villageois
qui sert de proie aux piglins) et les boules de feu. Les chunks que le Nether évince
(`NetherWorld::take_evicted`, nouveau) passent au stockage, qui écrit et retire leurs mobs.

**L'End** : son stockage ne fait vivre aucun mob (`spawn_mobs` faux) — un enderman lu là est rendu
intact. Le combat lu dans `level.dat` avec `NeedsStateScanning` à 0 **ne redémarre plus** : avant,
un redémarrage avec un dragon vivant rebâtissait un dragon neuf à (0, 128, 0) et **dix cristaux**,
les détruits compris. Maintenant le dragon revient de `DIM1/entities` avec sa santé, sa phase, sa
position et son UUID ; les cristaux reviennent un par un, et un cristal détruit, absent du fichier,
ne revient pas. Si le dragon n'est pas revenu **100 ticks** après que les quatre chunks de l'arène ont
été lus (un monde sauvé avant cette vague), un dragon neuf est fait, avec l'UUID du combat — ce délai
est **le nôtre**, pas celui du jeu. Un cristal lu à moins d'un bloc d'un cristal vivant est le même
cristal (le départ du combat en pose un sur chaque pilier ; un monde vanilla les a déjà).

**`RootVehicle`** : à la déconnexion (sous le verrou des joueurs, qui garde le monde d'entités), la
session des rails **prend** le wagonnet du joueur (`take_vehicle`) : son composé part dans le fichier
du joueur, il quitte le monde, les autres clients reçoivent son `Remove Entities`. À la connexion, le
`RootVehicle` du fichier est remis (`request_restore`) : le tick attend que le client ait confirmé sa
position, remet le wagonnet (ou reprend celui du même UUID s'il est déjà là) et y assoit le joueur. À
l'arrêt du serveur, chaque joueur assis part de même dans son fichier avant la sauvegarde du monde.
`PlayerRecord::root_vehicle` est écrit, et **retiré** du fichier quand le joueur ne monte plus rien.

**Les verrous** : la sauvegarde automatique et celle de l'arrêt prennent maintenant le verrou des
joueurs autour de `save_world`, comme `/save-all` le faisait déjà (les commandes tournent sous lui) :
les objets au sol et le monde d'entités sont à ce verrou, et la sauvegarde les lisait sans.

---

## 4. L'état rendu au rechargement

| entité | ce qui revient | comment c'est tenu |
|---|---|---|
| objet | la pile (tag compris), l'âge : `born = now − Age`, donc il part au même tick qu'avant ; le délai de ramassage ; `Owner`/`Thrower`/`Health` rendus tels quels | test « an item keeps its stack… » ; `Age` −32768 (ne vieillit jamais) et `PickupDelay` 32767 (jamais ramassé) réécrits tels quels |
| orbe | la valeur et l'âge ; `Count` × `Value` lus en une orbe | test « an orb is a Value short… » |
| TNT | la mèche restante (`Fuse`) | test : lue 334, écrite 334 |
| bloc qui tombe | l'état (propriétés une par une depuis l'état par défaut, piège 8), `Time`, `DropItem:0b` respecté à l'atterrissage | test : sable, `Time` 17, `DropItem` 0 rendu |
| flèche plantée | `inGround`, `life`, `pickup` (donc ramassable), `damage`, `crit`, le bloc (`inBlockState`, et la case : la pointe, 0,1 le long du `Motion`) | test : la flèche du vrai serveur, (10,5 ; −59,95 ; 4,5), plantée dans l'herbe en (10, −61, 4) |
| trident | l'objet (`Trident`, avec son tag) que le ramassage rend, `DealtDamage` | test |
| potion jetée | l'`Item` qui dit ce qu'elle fera en se brisant | test |
| nuage | durée, âge, rayons, attente, potion et effets propres | test |
| mob du Nether | ce qu'un mob de l'Overworld garde, plus main principale et taille du cube | test « the Nether's mobs go to DIM-1/entities… » |
| dragon | santé, phase, position, UUID ; barre de boss renvoyée | test « the dragon comes back with its health… » : 137 PV, 9 cristaux sur 10 |
| wagonnet du joueur | le wagonnet et le joueur dedans | tests `RootVehicle` (fichier et session) |

---

## 5. Preuves

- **Tests unitaires** (`src/ov_server/tests/test_persistence.cpp`, `[persistence]`, 10 cas,
  369 assertions) : chaque type écrit avec les **types** mesurés (§ 2) et relu ; un chunk écrit à la
  main avec une TNT, un sable, une flèche, un trident, une potion, un nuage et un objet revient
  entier, tourne, et se réécrit **là où chaque entité se tient** (la TNT et le sable, lus en (0,0), se
  tiennent en (1,0) et y sont écrits) ; un redémarrage relit chacun une fois ; un déchargement emporte
  objets et TNT sur disque et hors du monde ; les mobs du Nether dans `DIM-1/entities` ; le dragon et
  les cristaux ; `RootVehicle` dans le fichier et dans la session.
- **Suite complète** : voir § 5.3.
- **De bout en bout** : voir § 5.1.
- **Relus par le vrai serveur** : voir § 5.2.

### 5.1 De bout en bout, contre notre serveur

`scripts/check_persistence_e2e.py` (ov_dedicated, port 25671, superplat neuf, sonde créative) :

1. une TNT allumée par un bloc de redstone (`setblock` à la console) saute, son cratère lâche des
   objets ; la sonde tire une flèche vers +z, un peu vers le haut, et lance une fiole d'expérience
   vers −z, puis s'éloigne — au premier passage, debout à côté, elle avait **ramassé** l'orbe et la
   flèche avant l'arrêt (piège 6) ;
2. une seconde TNT est allumée, un bloc de sable posé à y = 40, une perle lancée à la verticale, et le
   serveur arrêté **aussitôt**.

Le fichier `entities/` après l'arrêt (passage retenu, tous les contrôles verts) : **89** objets (la
terre du cratère, `Age` 101), **1** orbe (`Value` 7, `Count` 1, `Age` 121), **1** flèche plantée
(`inGround` 1, `pickup` 2 — tirée en créatif —, `life` 126, `inBlockState` herbe `snowy=false`, à
61,6 blocs, y = −60,0), **1** TNT (`Fuse` **59**), **1** sable en l'air (`Time` 20, à y = 32,57),
**1** perle en vol (y = −38,9, `Motion` y = +0,79).

Redémarré sur ce monde : les six types reviennent sur le fil (`Spawn Entity`, `Spawn Experience Orb`) ;
la métadonnée de mèche de la TNT, renvoyée à chaque tick, vaut **59, 58, 57…** — **59** paquets, puis
**une** explosion : elle reprend exactement là où elle était (un passage précédent : 78 sauvés,
78 paquets) ; le sable atterrit (mise à jour de bloc en x = 3, sable) ; la sonde marche sur la
flèche et sur des objets : **9** ramassages (`Take Item`).

Un passage a échoué **dans la sonde** : le lecteur de paquets partagé (`capture_entity_packets.py`)
perd la trame quand un délai d'attente tombe au milieu d'un paquet (`IndexError` dans
`read_varint`), avant tout contrôle ; relancé, tout passe. Nommé, non corrigé ici.

Puis la sonde pose un wagonnet, s'y assoit (`Set Passengers`), se déconnecte ; un `save-all` : son
fichier porte `RootVehicle` {`Attach` = l'UUID du wagonnet, `Entity` = le wagonnet à
(40,5 ; −59,9375 ; 4,5)} et `entities/` ne contient **aucun** wagonnet ; elle revient : **1** wagonnet
réapparaît et `Set Passengers` (wagonnet, 1, la sonde) l'y rassoit.

### 5.2 Le monde de vanilla chez nous

`check_persistence_e2e.py vanilla` sert à notre serveur le monde que la campagne `oracle` a laissé.
Le fichier : 6 objets, 4 orbes, 2 flèches, 1 flèche spectrale, 1 trident, 1 nuage, 1 poulet (l'œuf),
17 slimes. Sur le fil, à la sonde : **6** objets, **4** orbes, **2** flèches, **1** flèche spectrale,
**1** trident, **1** nuage, 1 poulet — tout ce que le fichier tient près d'elle ; 4 slimes sur 17 (les
autres sont dans des chunks hors de sa vue). Aucun composé refusé. La sonde, que vanilla a sauvée
assise dans un wagonnet, est **remise dedans** par notre serveur : 1 wagonnet et `Set Passengers`
(wagonnet, 1, la sonde) — le `RootVehicle` écrit par le vrai serveur, lu par le nôtre.

Au premier passage, rien n'était apparu : c'est ce passage qui a trouvé le piège 7.

### 5.3 La suite complète

`ctest --preset macos-debug` sur le code final : **16/16** (428 s) ; compilation `-Werror` sans
avertissement ; `check_layers.py` et `check_assets.py` passent.

---

## 6. Ce qui n'est pas fait — nommé

| sujet | état |
|---|---|
| `Owner` d'un projectile relu | c'est un UUID sur disque et un identifiant réseau ici : **non résolu**. Il est rendu tel quel à la sauvegarde ; une perle relue en vol ne téléporte donc personne, un trident relu n'a pas de lanceur |
| `Owner` des projectiles tirés par un mob | seul le tireur **joueur** est retrouvé (`set_owner_lookup`) |
| `shake` d'une flèche, `Color` d'une flèche trempée ou d'un nuage | `shake` écrit 0 ; `Color` rendu s'il venait du disque, jamais calculé |
| Orbes | le délai de ramassage n'est pas sauvé (vanilla ne l'a pas non plus sur l'orbe) ; une orbe de `Count` N se ramasse en une fois, vanilla en N |
| Objets et orbes | n'ont pas de physique ici : `Motion` est écrit nul |
| Mobs du Nether | la colère d'un zombifié (`AngerTime`, `AngryAt`) n'est pas relue ; l'armure, le `Brain`, l'`Inventory` du piglin sont rendus tels quels, pas modélisés ; les boules de feu (`fireball`, `small_fireball`) ne sont pas sauvées |
| End | les boules de feu du dragon, les nuages de souffle et ses orbes d'expérience ne sont pas sauvés ; un dragon relu en phase `dying` reprend son animation depuis 0 ; les quatre cristaux d'une réinvocation relus ne relancent pas la réinvocation |
| `RootVehicle` | seul le **wagonnet** est remis (le seul véhicule que ce serveur fait vivre) ; un joueur assis sur une entité elle-même dans le wagonnet (`Attach` ≠ UUID du wagonnet) retrouve le wagonnet mais pas sa place, nommé dans le journal |
| Objets créés hors de leur dimension | quelques chemins du serveur créent un objet sans dire sa dimension (il vaut alors Overworld) ; la mort d'un joueur les dit maintenant, les autres non |
| Projectiles et TNT hors de l'Overworld | ils vivent dans le monde d'entités de l'Overworld (déjà vrai avant) : une flèche tirée dans le Nether est sauvée dans `entities/` |

---

## 7. Pièges payés ici

1. **`Fire` n'a pas une valeur par défaut, il en a deux** : −1 pour ce qui ne brûle pas dans la durée
   (objet, orbe, TNT), 0 pour le reste. Relevé type par type.
2. **L'`Age` d'un objet est un court, celui d'un nuage un entier** ; `Count` d'une orbe est un entier
   quand `Value` est un court. Une table écrite « à l'œil » se trompe au moins une fois sur trois.
3. **Une entité est écrite là où elle se tient, pas là où elle a été lue** : le premier test attendait
   la TNT dans le chunk (0,0) où il l'avait écrite ; elle est à x = 22,5, donc dans (1,0), où le
   stockage l'a correctement rangée.
4. **L'herbe du superplat est en −61** ; la face du dessus en −60. Une flèche plantée à y = −59,95 a sa
   pointe dans le bloc −61, pas −60.
5. **Un combat relu ne doit pas redémarrer.** `started_ = killed_ && portal` faisait repartir tout
   combat vivant à la première arrivée : dragon neuf, dix cristaux neufs.
6. **Une sonde créative ramasse ce qu'on veut sauver.** Orbe lancée à ses pieds, flèche tirée à deux
   blocs : ramassées avant l'arrêt, absentes du fichier — ce qui ressemble exactement à une sauvegarde
   qui les oublie. Lancer loin, puis s'éloigner.
7. **`tick_count() % 10 == 0` peut ne jamais tomber.** La lecture de `entities/` (mobs-3) tournait à
   ce modulo ; `TickClock` avale les ticks d'un serveur en retard (piège 22 du briefing) : sur le
   monde écrit par vanilla, notre serveur Debug a fait **16** itérations en 13,4 s pour 154 ticks
   d'horloge, et **aucun** chunk n'a été lu (« saved 0 mobs across 0 chunks »). Rien n'a été perdu —
   un chunk jamais lu n'est jamais réécrit — mais rien n'est apparu non plus. La lecture tourne
   maintenant « dix ticks après la précédente ».
8. **La mèche renvoyée à l'apparition est la mèche sauvée.** Après le redémarrage, la première
   métadonnée de la TNT vaut la mèche du fichier (78 sauvés : 78 d'abord, pas 77) : l'apparition la
   porte avant tout tick. Compter les paquets — autant que de ticks restants, puis une explosion —
   est la preuve, pas le premier.

---

## 8. Rejouer

```bash
lockf /tmp/ov-vanilla.lock python3 scripts/measure_persistence.py oracle    # ce que vanilla écrit
./build/macos-debug/bin/test_ov_server "[persistence]"                     # 10 cas
python3 scripts/check_persistence_e2e.py                                   # notre serveur, deux fois
python3 scripts/check_persistence_e2e.py vanilla                           # le monde de vanilla chez nous
lockf /tmp/ov-vanilla.lock python3 scripts/measure_persistence.py readback  # notre monde chez vanilla
```

| fichier | rôle |
|---|---|
| `src/ov_server/src/entity_storage.{hpp,cpp}` | `LooseAdopter`, `EntityStorageHost::ignore/write_extra/read_extra`, un adoptant n'est plus « transitoire », `spawn_mobs` |
| `src/ov_server/src/entity_nbt.{hpp,cpp}` | les clés communes, UUID, pile d'objets, état de bloc |
| `src/ov_server/src/ground_entities.{hpp,cpp}` | `ItemEntity`, `GroundOrb` (sortis de `server.cpp`) et leur adoptant, par dimension |
| `src/ov_server/src/dimension_entities.{hpp,cpp}` | le stockage du Nether et celui de l'End |
| `src/ov_server/src/tnt_gravity.{hpp,cpp}` | adoptant de la TNT et du bloc qui tombe |
| `src/ov_server/src/projectiles.{hpp,cpp}` | adoptant des huit projectiles |
| `src/ov_server/src/brewing_session.{hpp,cpp}` | adoptant des nuages |
| `src/ov_server/src/nether_mobs.{hpp,cpp}` | `world()`, `storage_host`, `forget`, `behaviour` extrait de l'apparition |
| `src/ov_server/src/nether_travel.{hpp,cpp}` | `take_evicted` |
| `src/ov_server/src/end_fight.{hpp,cpp}` | adoptant du dragon et des cristaux, le combat relu qui ne redémarre pas |
| `src/ov_server/src/rails_session.{hpp,cpp}`, `player_data.{hpp,cpp}` | `RootVehicle` |
| `src/ov_gameplay/include/ov/gameplay/falling_block.hpp` | `set_time` |
| `src/ov_server/src/server.cpp` | blocs `// ── persistence ──` |
| `src/ov_server/tests/test_persistence.cpp` | les tests |
| `scripts/measure_persistence.py`, `scripts/check_persistence_e2e.py` | l'oracle, la relecture, le bout en bout |
