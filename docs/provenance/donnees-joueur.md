# Données joueur — `playerdata/<uuid>.dat`, au format de la 1.20.1

Ce que ce serveur retient d'un joueur entre deux sessions, où, et comment on a établi que
vanilla le relit. Code : `src/ov_server/src/player_data.{hpp,cpp}` ; blocs `// ── player
data ──` dans `server.cpp` ; outil `tools/ov_playerdata` ; mesures
`scripts/measure_player_data.py` (vanilla) et `scripts/check_player_data_e2e.py` (notre
serveur) ; tests `src/ov_server/tests/test_player_data.cpp`.

---

## 1. Ce qui était persisté avant ce travail

**Rien, sur disque, pour le joueur.**

* La position et la rotation vivaient dans une table **en mémoire** (`saved_players`,
  `server.cpp`), mise à jour à chaque paquet de mouvement. Une reconnexion dans le même
  processus retrouvait la position ; **un redémarrage l'oubliait**. L'entrée de
  `PROGRESS.json` « position et rotation retenues entre sessions » n'était vraie que dans la vie
  d'un processus.
* L'inventaire, les PV, la faim, l'expérience et les effets étaient portés par l'objet
  `Player`, **effacé à la déconnexion** : un joueur qui revenait repartait de zéro, même sans
  redémarrage.
* La phrase de `ROADMAP.md` « une pile déposée survit à une reconnexion et à un redémarrage »
  décrit une pile déposée **dans un coffre** : c'est l'entité de bloc, écrite en Anvil, qui
  survit — pas l'inventaire du joueur.
* `level.dat` était régénéré entièrement depuis `LevelSettings` à chaque sauvegarde, sans
  `Data.Player`. Un monde solo vanilla ouvert ici **perdait son joueur** à la première
  sauvegarde.

---

## 2. Le fichier vanilla, mesuré

Campagne `capture` : serveur 1.20.1 officiel, monde plat neuf, un bot hors ligne `ovplayer`.
La console lui donne un inventaire (barre, sac, deux pièces d'armure, main secondaire, une épée
avec `Damage` et `display.Name`), une perle dans le coffre de l'Ender, 30 niveaux + 17 points,
5 points de dégâts, une faim vidée par `hunger 255`, trois effets (vitesse IV sur une vitesse II
**cachée**, vision nocturne **infinie** à particules masquées), un point d'apparition ; le bot
choisit la case 2 de la barre. `save-all flush` une fois connecté, puis déconnexion.

Le fichier (`world/playerdata/<uuid>.dat`, 1360 octets gzip, 3440 décompressés) :
**45 clés à la racine, 159 feuilles**. Ce qu'il apprend, et que le code reproduit :

* **gzip**, racine sans nom, `DataVersion` int 3465, `UUID` tableau de 4 int.
* **L'ordre des clés est celui d'une HashMap** (`Brain`, `HurtByTimestamp`, `SleepTimer`,
  `SpawnForced`, `Attributes`…) : aucune comparaison octet par octet n'a de sens entre deux
  écrivains ; tout ce qui suit compare **tag par tag**.
* `Inventory` : composés `{Slot, id, Count, tag}`, `Slot` en octet ; ordre d'écriture 0, 1, 2,
  29, 100, 103, −106 — barre et sac, puis l'armure des pieds vers la tête, puis la main
  secondaire. Correspondance avec la fenêtre 0 : barre 36..44 ↔ 0..8, sac 9..35 ↔ 9..35, tête
  5 ↔ 103, pieds 8 ↔ 100, main secondaire 45 ↔ −106. La grille 2×2 n'a pas de case.
* `previousPlayerGameType` **absent** quand il n'y en a pas (pas −1 écrit).
* `ActiveEffects` **absent** sans effet ; `HiddenEffect` imbriqué ; `Duration` −1 pour
  l'infini ; amplificateur en octet signé (déjà établi dans `effets.md` § 13).
* `Attributes` ne liste **que les attributs instanciés** (ici armure, résistance d'armure,
  vitesse), pas les huit du joueur ; `Modifiers` **absent** quand vide ; un modificateur est
  `{Amount, Operation int, UUID int[4], Name}`, le nom portant l'amplificateur
  (`effect.minecraft.speed 3`), le montant 0.800000011920929 (0,2f × 4).
* `Motion` d'un joueur immobile au sol : `[0.0, -0.0784000015258789, 0.0]` — un tick de
  gravité à travers la traînée, en float.
* `XpP` 0.1517857164144516 = 17/112 en float (le niveau 30 coûte 112) ; `Score` 17 = les
  **points** donnés, pas les niveaux ; `XpSeed` **0** pour un joueur neuf.
* `Fire` −20, `Air` 300, `abilities` en sept clés (`walkSpeed` 0.1f, `flySpeed` 0.05f).
* `Spawn{X,Y,Z}`, `SpawnAngle`, `SpawnForced`, `SpawnDimension` écrits par `/spawnpoint`.

**La règle `.dat_old`**, mesurée : après deux écritures (le `save-all` puis la déconnexion),
`<uuid>.dat_old` existe et est **octet pour octet la première**. Le serveur fait de même :
copie du fichier courant vers `.dat_old`, puis écriture atomique (`io::write_file_atomic`) du
nouveau. Une copie plutôt qu'un renommage, pour que `.dat` ne manque jamais.

**Ce qu'un aller-retour vanilla change de lui-même** (campagne `control` : V1 relu par vanilla,
le bot entre et sort, vanilla réécrit V1') : **155 feuilles égales, 4 changées** — les deux
durées de vitesse (le temps passé en ligne), `XpSeed` (0 → une valeur tirée au chargement) et
`warden_spawn_tracker.ticks_since_last_warning`. C'est la ligne de base contre laquelle se lit
l'aller-retour suivant.

---

## 3. Ce que nous modélisons, et ce que nous gardons

**Règle : une clé que ce serveur ne modélise pas est conservée, jamais jetée.** Le record est
fusionné **sur le composé lu** : les clés possédées sont remplacées **à leur place**, tout le
reste repart tel qu'il est venu.

| Écrites depuis notre état | Conservées telles quelles |
|---|---|
| `Pos`, `Motion`, `Rotation`, `OnGround`, `FallDistance`, `Dimension` | `Brain`, `recipeBook`, `warden_spawn_tracker`, `seenCredits` |
| `playerGameType`, `previousPlayerGameType`, `abilities` | `EnderItems` (pas de coffre de l'Ender ici) |
| `Health`, `AbsorptionAmount`, `HurtTime`, `DeathTime`, `Air` | `Spawn*` (voir § 7), `Score`, `XpSeed`, `Fire` |
| `foodLevel`, `foodSaturationLevel`, `foodExhaustionLevel`, `foodTickTimer` | `Invulnerable`, `PortalCooldown`, `FallFlying`, `SleepTimer`, `HurtByTimestamp` |
| `XpLevel`, `XpP`, `XpTotal` | `LastDeathLocation`, `ShoulderEntity*`… |
| `RootVehicle` (2026-09-11, `persistance-entites.md` : le wagonnet du joueur parti assis dedans ; retiré quand il ne monte plus rien) | |
| `Inventory`, `SelectedItemSlot`, `ActiveEffects`, `Attributes` (fusionnés) | tout ce qu'une autre version ou un mod ajoute |
| `DataVersion` 3465, `UUID` | |

Détails qui comptent :

* **`XpP`** : le serveur garde des points entiers (principe 5), le fichier un float. Si le
  niveau et les points n'ont pas bougé, le float **d'origine** est réécrit bit pour bit ; sinon
  `points / coût` en float. À la lecture, les points sont `round(XpP × coût)`.
* **`Attributes`** est fusionné entrée par entrée : une entrée d'un attribut que le joueur ne
  possède pas ici est gardée ; un attribut possédé n'est ajouté que si sa base a bougé ou qu'il
  porte un modificateur — comme vanilla. Les modificateurs d'effet ne sont **pas** relus depuis
  `Attributes` : l'effet relu les remet lui-même (même UUID, montant de son amplificateur) ;
  les autres modificateurs sont relus, et leur nom reste celui du fichier.
* **`AbsorptionAmount` et `Health`** sont lus **après** les effets : rajouter l'effet
  d'absorption regonfle la réserve, le fichier dit ce qu'il en reste.
* **Grille 2×2 et curseur** : vanilla les rend à l'inventaire à la fermeture de l'écran ; le
  fichier n'a pas de place pour eux. Ils sont rangés dans les premières cases libres (barre
  d'abord) de la copie sauvegardée ; ce qui ne trouve pas de place est compté et nommé dans le
  journal (vanilla le jetterait au sol).
* **Mode de jeu** : ce serveur a un mode global (`--survival`). Il est écrit dans
  `playerGameType` ; s'il diffère de celui du fichier, l'ancien devient
  `previousPlayerGameType`, comme vanilla. Quand le mode par joueur arrivera (agent des
  commandes), c'est `options.survival ? 0 : 1` dans `player_record_of` qu'il faudra remplacer.
* **`Motion`** : le serveur n'intègre pas la vitesse d'un joueur (le client le fait). Écrit :
  la valeur de repos mesurée au sol, zéro en l'air. Déclaré, pas inventé.
* **`flying`** : le client annonce son vol et ce serveur ne l'écoute pas encore ; un vol noté
  par vanilla est gardé tant que le mode permet de voler.

---

## 4. L'aller-retour, chiffré

Campagne `roundtrip` :

* **V1 → nous → O1** (`ov_playerdata roundtrip`) : **159 / 159 feuilles identiques**, 0
  changée, 0 perdue, 0 ajoutée. Octets : même longueur décompressée (3440), 3206 / 3440 octets
  identiques au même décalage — la différence commence à l'octet 2712, dans `ActiveEffects` :
  l'ordre des clés d'un composé d'effet est l'ordre de hachage de vanilla chez lui,
  `Id, Amplifier, Duration…` chez nous. Tag pour tag, rien ne diffère.
* **O1 → vanilla → V2** : vanilla lit notre fichier, le bot entre (la console lit `Pos`
  `[12.5d, -60.0d, -7.25d]`, l'inventaire complet avec ses `tag`, `SelectedItemSlot` 2, le
  coffre de l'Ender, la chaîne d'effets vitesse IV / II cachée et la vision nocturne infinie)
  puis sort : **155 égales, 4 changées — exactement les 4 clés que vanilla change sur son propre
  fichier**, et aucune autre (`against_control` vide dans les deux sens).
* **Un fichier écrit par notre serveur, ouvert par vanilla** (campagne `ours`, le fichier du
  bout en bout § 8) : le bot entre là où il était sorti de chez nous — console :
  `Pos [40.5d, -60.0d, 25.5d]`, `Health 13.0f`, `foodLevel 13`, `SelectedItemSlot 3`, pierre ×32
  en 0, pain ×7 en 20, casque en 103, torches ×10 en −106, vitesse II et vision nocturne
  infinie ; vanilla lui envoie lui-même Set Held Item `03`. Diff de ce que vanilla réécrit :
  **73 feuilles égales, 0 perdue, 2 changées** — la durée de vitesse (le temps en ligne) et
  `XpSeed` (retiré au chargement, comme sur son propre fichier) — et 51 ajoutées : les clés que
  nous n'écrivons pas pour un joueur neuf et que vanilla construit lui-même (`Attributes`,
  `Brain`, `recipeBook`, `warden_spawn_tracker`), plus `Damage: 0` qu'il pose sur le casque, un
  objet endommageable arrivé sans `tag`.
* **Set Held Item clientbound = 0x4D** : vanilla l'envoie à l'entrée avec la case du fichier
  (charge utile `02`). C'est l'identifiant que le serveur utilise.
* **Durée des effets à l'entrée** : vanilla envoie Entity Effect (0x6C) **avec la durée du
  fichier, exactement** — `01 01 03 a704 06 00` : vitesse IV, 551, la valeur de V1 ; la vision
  nocturne avec −1. Pas un tick de moins. Le serveur fait de même depuis
  `EffectSession::announce` (§ 9).

---

## 5. Refusé, jamais écrasé

Même règle que les chunks et `level.dat`. Un fichier qui n'est pas gzip, pas du NBT, sans
`DataVersion`, d'un autre `DataVersion`, rangé sous le mauvais UUID, ou dont `Dimension` n'est
pas l'Overworld (seule dimension de ce serveur) est **refusé et nommé** : le message donne le
chemin et la raison, et **le joueur n'entre pas** — un joueur admis serait sauvegardé, et la
sauvegarde remplacerait le fichier. `PlayerDataStore` retient aussi les refus : `save` refuse
d'écrire sur un fichier refusé, même si un appelant oublie de tenir le joueur dehors. Le client
reçoit un Login Disconnect générique (le chemin reste dans le journal du serveur).

---

## 6. Le solo : `level.dat` → `Data.Player`

**Décision** : `ov_voxel --singleplayer` passe `--host-player=<nom>` au serveur qu'il héberge.
Pour l'hôte, le serveur écrit le fichier `playerdata/<uuid>.dat` **et** le même composé dans
`level.dat` → `Data.Player` ; à la lecture, `Data.Player` passe **avant** le fichier.

**Pourquoi** : le client vanilla ouvre un monde solo avec l'UUID du **compte** Mojang, qui
n'est jamais notre UUID hors ligne. Chercher `playerdata/<uuid-hors-ligne>.dat` ne le trouverait
pas : il apparaîtrait neuf, au point d'apparition, les mains vides. `Data.Player` est
l'emplacement que le jeu solo lit pour son joueur, sans UUID dans le chemin. Deux conséquences :

* l'`UUID` écrit dans `Data.Player` est celui qu'il portait déjà s'il y en avait un (le compte),
  pour que vanilla ne voie pas un inconnu à sa place ; le fichier `playerdata` porte le nôtre ;
* un monde solo vanilla ouvert ici rend son joueur à l'hôte, où qu'il fût.

**Mesuré** (campagne `level`) : un `level.dat` portant `Data.Player`, ouvert et sauvegardé par
le **serveur dédié** vanilla, **garde** la clé. Notre serveur dédié fait de même : `Data.Player`
relu au démarrage est réécrit tel quel à chaque sauvegarde de `level.dat`, alors qu'il était
perdu avant ce travail.

**Non mesuré** : qu'un **client** vanilla en solo place effectivement son joueur depuis
`Data.Player`. Il faudrait piloter un vrai client graphique ; le serveur dédié ne lit pas cette
clé pour ses joueurs. La règle vient de la documentation du format (`level.dat`, clé `Player` :
l'état du joueur solo, prioritaire sur son fichier `.dat`) — la page actuelle du wiki décrit une
version où `Player` a été remplacé par `singleplayer_uuid`, ce qui confirme au passage le rôle
de la clé en 1.20.1 mais pas son comportement exact. Nommé plutôt qu'affirmé.

---

## 7. Couche

`ov_server`, pas `ov_world`. Le record est fait de piles d'objets du protocole (`ov_protocol`,
couche 7) et d'états d'effets, de faim, de santé et d'attributs (`ov_gameplay`, couche 9) ;
`ov_world` (6) est en dessous des deux et ne peut pas les nommer. Le générique — gzip, écriture
atomique, NBT — est déjà en dessous, dans `ov_io` et `ov_nbt`.

---

## 8. Bout en bout, contre notre serveur

`scripts/check_player_data_e2e.py`, trois démarrages sur un monde neuf et un intrus :
**45 vérifications, 45 passées.**

* **Démarrage 1**, créatif, `--effect=speed:1:2400,night_vision:0:infinite` : la sonde
  remplit quatre cases par Set Creative Mode Slot (barre, sac, tête, main secondaire), prend la
  case 3, marche à (40,5 ; 25,5) et sort. Le fichier est écrit **à la déconnexion** : gzip,
  `DataVersion` 3465, `Pos`, les quatre piles aux `Slot` 0 / 20 / 103 / −106,
  `SelectedItemSlot` 3, `playerGameType` 1, vitesse décomptée (2377 < 2400), vision nocturne −1.
* **Démarrage 2**, survie : la sonde est renvoyée à (40,5 ; 25,5), reçoit ses quatre piles
  (Set Container Content), la case 3 (Set Held Item 0x4D) et ses deux effets **avec les durées
  du fichier**. Chute de dix blocs puis sortie : `Health` 13, faim entamée (16),
  `playerGameType` 0, **`previousPlayerGameType` 1**, et `.dat_old` est **octet pour octet** le
  fichier du démarrage 1.
* **Démarrage 3**, survie sans effet : **le redémarrage** — position, piles, case, effets aux
  durées exactes du fichier (2310), premier Set Health (13,0 ; 13). Puis une déconnexion et une
  **reconnexion dans le même processus** : pareil, la vitesse ayant continué de décompter en
  ligne (2290) — et **pas hors ligne**, comme chez vanilla.
* **L'intrus** : un fichier `DataVersion` 3337 pour un autre nom. La connexion reçoit un Login
  Disconnect, le journal nomme le joueur et la version, et le fichier est **identique octet pour
  octet** après coup.

---

## 9. Pièges

* **Les effets restaurés arrivaient avec un tick de moins.** Relus comme événements `Added`, ils
  n'étaient envoyés qu'au premier `flush`, *après* le premier `tick` qui les avait décomptés : le
  fichier disait 2348, le client recevait 2347. Masqué dans le premier essai par `--effect`,
  dont l'`apply` vidait la file avant tout tick. Corrigé par `EffectSession::announce`, appelé à
  l'entrée sur le record que le tick possède.
* **Les effets sautent des ticks** quand le fil réseau tient `players_mutex` : le bloc des effets
  prend le verrou en `try_to_lock`. Une faim de 40 ticks a duré plus de 2,5 s pendant une chute
  (beaucoup de paquets) et figurait encore dans le fichier. Préexistant, pas corrigé ici ; une
  sonde qui suppose « N ticks = N/20 s » pour un effet se trompe.
* **La table `saved_players` en mémoire** est laissée en place (diff minimal) mais le fichier
  gagne : il est lu après elle.
* **Une comparaison d'octets entre deux écrivains NBT ne prouve rien** : l'ordre des clés de
  vanilla est celui d'une HashMap. Comparer feuille par feuille, indexer les listes par `Slot`,
  `Name` ou `Id`.

---

## 10. Ce qui n'est pas fait

* **L'expérience de bout en bout** : aucune source d'expérience n'est pilotable sur notre serveur
  sans commande ; elle est couverte par le test unitaire et par l'aller-retour vanilla (30
  niveaux + 17 points relus exactement), pas par `check_player_data_e2e.py`.
* **`Score`** n'est pas incrémenté par l'expérience gagnée ici (vanilla le fait) : conservé.
* **Le point d'apparition** (`Spawn*`) est conservé mais pas utilisé : la réapparition de ce
  serveur se fait toujours au point du monde.
* **Le coffre de l'Ender** : conservé, jamais modifié — il n'existe pas ici.
* **La grille 3×3 d'un établi ouvert** à la déconnexion n'est pas rendue à l'inventaire (seule
  la 2×2 et le curseur le sont).
* **`DeathTime`** est écrit 0 ; un joueur sauvegardé mort revient mort (PV 0, écran de mort,
  réapparition par le bouton) mais sans l'animation.
* **Les autres dimensions** : un fichier dans le Nether ou l'End est refusé, pas déplacé.
