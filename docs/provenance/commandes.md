# Commandes et chat — ce que le jar vanilla répond, et ce que nous répondons

Avant cette vague, un client vanilla 1.20.1 qui tapait `/time set day` ou un
message n'obtenait **rien** : le serveur ignorait les paquets de chat et de
commande. Ce dossier décrit le moteur de commandes, le chat, et surtout **la
mesure** : le même client sonde tape la même liste de commandes, dans le même
ordre, au vrai serveur 1.20.1 puis au nôtre, et les réponses sont comparées
octet pour octet.

| Mesure | Résultat |
|---|---|
| Réponses aux commandes (System/Player/Disguised Chat, titres, difficulté), **octet pour octet** | **269 / 271** identiques |
| Lignes de `/help` pour les commandes que nous avons | **34 / 34** |
| Console du serveur dédié (lignes imprimées) | **17 / 17** |
| Suggestions (début, longueur, propositions) | **21 / 22** — la 22ᵉ (`/`) est identique restreinte à nos 34 commandes |
| Arbre Commands d'un opérateur, décodé par un décodeur indépendant | **34 / 34** arbres de commande identiques, **ordre de la racine identique** |
| Arbre de niveau 0 envoyé à l'arrivée | celui de vanilla moins `teammsg`, `tm`, `trigger` (absents ici) |
| Gamerules : noms, types, défauts | **45 / 45** (serveur vanilla neuf) |
| État du monde après redémarrage (`/time`, `/gamerule`, `/difficulty`, `/weather`, `/defaultgamemode`) | relu à l'identique |
| Tests unitaires `test_ov_commands` / `test_chat` | 240 et 850 assertions, vertes |

Les deux réponses qui diffèrent sont nommées au § 6 : la ligne « Diamonds! »
(un succès, que ce serveur n'a pas) et la liste complète de `/help` (vanilla a
79 commandes, nous 34). **Avant** les dernières corrections, la même mesure
donnait 256 / 271 ; ce qui a fait la différence est au § 7.

Reproduire : `python3 scripts/capture_commands.py vanilla`, puis
`… ov`, puis `python3 scripts/check_commands.py`. Les défauts des gamerules :
`python3 scripts/measure_gamerules.py`.

---

## 1. Les sources

| Sujet | Source | Statut |
|---|---|---|
| Paquets de chat et de commande | archive figée `…/Protocol?oldid=2773082`, sections Chat Command, Chat Message, Player/Disguised/System Chat, Commands, Command Suggestions, Server Data, Update Section Blocks | ✅ chaque champ ; **confirmé** par le jar, qui aurait déconnecté la sonde au premier champ mal lu |
| Format des nœuds de l'arbre Brigadier | `Java_Edition_protocol/Command_data?oldid=2766778` (9 mai 2023, dernière révision avant 1.20.1) | ✅ drapeaux, propriétés par parseur |
| Numéros des parseurs | registre `minecraft:command_argument_type` de `generated/reports/registries.json` (49 entrées) | ✅ lus dans le pack à l'exécution ; une table de secours est testée contre le rapport |
| Arbre de chaque commande | le paquet Commands **capturé** sur le jar (1363 nœuds), décodé par `scripts/commands_graph.py` | ✅ |
| Clés de traduction | `run/assets/.../lang/en_us.json` (asset local, jamais commité) ; les arguments exacts de chaque clé viennent de la capture | ✅ |
| Gamerules : noms et types | `reports/commands.json` (enfants de `gamerule`) | ✅ 45 |
| Gamerules : valeurs par défaut | `scripts/measure_gamerules.py` : un serveur vanilla neuf, `gamerule <nom>` pour chacune | ✅ **45/45** concordent avec `game_rules.hpp` |
| Durées de la météo | Minecraft Wiki, articles *Weather* et *Commands/weather* : pluie 12000–24000, orage 3600–15600, 12000–180000 entre deux | ✅ ; le tirage vanilla n'est pas reproductible (flux aléatoire du niveau), le nôtre est déterministe |

Aucun code de Paper/Spigot/Bukkit n'a été lu. Brigadier n'a pas été lu non
plus : son comportement a été **re-spécifié depuis ses réponses** (§ 3).

---

## 2. Le protocole — `ov_protocol/chat.{hpp,cpp}`

| Sens | Paquet | Id | Notes |
|---|---|---|---|
| → serveur | Chat Command | 0x04 | commande sans `/`, horodatage, sel, jusqu'à 8 signatures d'argument (256 o chacune), compteur, bitset fixe de 20 bits (3 o) |
| → serveur | Chat Message | 0x05 | message ≤ 256 **unités UTF-16**, signature optionnelle de 256 o |
| → serveur | Message Acknowledgment / Player Session | 0x03 / 0x06 | lus et ignorés : mode hors-ligne |
| → serveur | Command Suggestions Request | 0x09 | texte ≤ 32500 |
| → client | System Chat Message | 0x64 | composant JSON + drapeau barre d'action |
| → client | Player Chat Message | 0x35 | non signé : index 0, aucun message précédent, filtre PASS_THROUGH |
| → client | Disguised Chat Message | 0x1B | ce que la console envoie pour `/say` |
| → client | Commands | 0x10 | arbre élagué par niveau de permission, numéroté en largeur d'abord depuis la racine |
| → client | Command Suggestions Response | 0x0F | |
| → client | Server Data | 0x45 | **`enforcesSecureChat = false`**, ce que le jar hors-ligne envoie lui-même (octet final 0x00 capturé) |
| → client | Change Difficulty, titres (0x5F/0x5D/0x46/0x60/0x0E), Update Section Blocks (0x43), World Event (0x25), Look At (0x3B) | | |

**Piège 1 — la limite de 256 est en caractères, pas en octets.**
`read_string` du dépôt borne les *octets* (trois par unité UTF-16), si bien
qu'un message ASCII de 257 lettres passait. Le test l'a attrapé ; la limite du
chat est maintenant vérifiée en unités UTF-16.

**Piège 2 — Update Section Blocks n'a plus de booléen en 1.20.1.** Long de
section, VarInt, VarLongs : c'est ce que dit l'archive et ce que montre la
capture (`00000000000ffffc 30 …` pour 48 blocs). L'ordre des entrées vanilla
est celui d'un ensemble haché, pas un parcours : il se compare comme un
ensemble.

Le chat d'un joueur est relayé en **Player Chat Message non signé** avec le
type `minecraft:chat` — exactement ce que le jar hors-ligne a renvoyé à la
sonde. Nos numéros de `chat_type` (fichiers du datapack triés) sont ceux du
jar : 0 chat, 1 `/me`, 2/3 `/msg`, 4 `/say`, lus sur ses paquets.

Chaque paquet a son aller-retour octet pour octet et ses entrées hostiles
(toutes les troncatures, un parseur inconnu, un index d'enfant hors du tableau)
dans `test_chat.cpp`.

---

## 3. Le moteur — `ov_server/src/commands/`

### Ce que Brigadier fait, lu sur ses réponses

La règle d'analyse qui reproduit **toutes** les positions d'erreur de la
capture :

1. à chaque nœud, si le mot suivant est l'un de ses littéraux, seul ce littéral
   est essayé ; sinon **seuls ses arguments** le sont ;
2. chaque enfant qui s'analyse est suivi récursivement ; parmi les branches
   obtenues, la première qui a tout consommé gagne, puis la première **sans
   erreur**, dans l'ordre des enfants ;
3. un échec est l'erreur unique de la seule branche essayée, ou « commande
   inconnue » si rien n'a été analysé, ou « argument incorrect » au point le
   plus loin atteint.

La preuve que ce n'est pas une intuition : `tp @s 0 -60 0 foo` répond
*incorrect argument* **à la colonne 6** — pas sur `foo`. Seule la règle 2
produit ça : la branche `destination` (`@s` pris comme destination) n'a pas
d'erreur, s'est arrêtée à 6, et passe devant la branche `location` qui, elle,
est allée plus loin mais en échouant. Le test `the branch rule` le fige.

Les autres positions figées par la capture : `setblock 1.5 …` revient sur
`1.5` (entier invalide), `gamemode foo` reste **après** `foo`, `kill @r[type`
s'arrête **avant** le `=` (option inapplicable, sans retour arrière) alors que
`kill @e[foo` revient sur `foo` (option inconnue), `gamemode creative @e`
remonte à **la colonne 0** (le parseur d'entités remet le curseur au début de la
commande).

### L'ordre des clés JSON

Vanilla écrit un composant dans cet ordre, lu sur ses paquets : style (`bold`,
`italic`, `underlined`, `strikethrough`, `obfuscated`, `color`, `insertion`,
`clickEvent`, `hoverEvent`, `font`), puis `extra`, puis le contenu (`text`, ou
`translate` suivi de `with`). Une ligne d'erreur est donc
`{"color":"red","extra":[…],"text":""}` et non l'inverse. Un argument de
traduction qui est une chaîne nue voyage comme chaîne nue (`"with":["1000"]`) ;
un composant reste un composant (`"with":[{"text":"ovprobe"}]`).

### Les pièces

| Fichier | Rôle |
|---|---|
| `string_reader` | le lecteur, ses alphabets (nombres, mots non quotés), l'échappement, et **où il laisse le curseur** |
| `text`, `json` | composants, sérialisation à l'ordre vanilla, lecture JSON stricte (message de Gson reproduit pour le cas « malformed ») |
| `snbt` | NBT textuel, lu vers `nbt::Tag` et réimprimé comme le survol vanilla (`{Damage:0}`, `{display:{Name:'"x"'}}`) |
| `arguments` | 21 types d'argument avec leur id de parseur et leurs propriétés ; coordonnées `~` et `^` ; blocs avec état validé contre le registre (**une propriété à la fois depuis l'état par défaut**, piège 8 du briefing) ; items ; composants ; durées `1d 20t 5s` |
| `selector` | `@a @p @r @e @s`, noms, UUID ; options `name distance level x y z dx dy dz x_rotation y_rotation limit sort gamemode team type tag` ; `nbt scores advancements predicate` sont **analysées puis refusées par leur nom** à l'usage |
| `dispatcher` | l'arbre, l'analyse ci-dessus, l'exécution, les suggestions, l'usage « intelligent » de `/help`, le paquet Commands |
| `world_state` | l'horloge, la météo, la difficulté, les 45 gamerules |
| `ops`, `console` | `ops.json` au format vanilla ; la console du serveur dédié |
| `service`, `game_commands`, `names` | la file, l'accueil des joueurs, les retours, et les commandes |

### Le fil d'exécution

Une commande **change le monde**, et seul le thread de tick l'écrit. Le thread
réseau met donc en file (`enqueue`) ; la console aussi ; le tick vide la file
quand la table des joueurs est libre. Les suggestions sont servies tout de
suite depuis l'arbre, que rien ne modifie après le démarrage — c'est pourquoi
`op`/`deop` suggèrent tous les joueurs en ligne au lieu de filtrer ceux déjà
opérateurs : filtrer lirait, depuis le thread réseau, une liste que le tick
modifie.

---

## 4. La météo et l'horloge, mesurées

La capture contient une minute de paquets météo. Ce qu'ils disent :

* les niveaux de pluie et d'orage bougent de **0,01 par tick en flottant 32
  bits** : `0.009999999776`, `0.019999999553`, … — le test compare les bits ;
* ils bougent **même avec `doWeatherCycle` à false** (la capture tournait
  ainsi) : seule la minuterie s'arrête ;
* « il pleut » pour le client, c'est **niveau > 0,2** : le Game Event 1 part au
  tick où le niveau franchit 0,2 en montant, le 2 en descendant, chacun suivi des
  deux niveaux renvoyés ;
* Update Time n'est **pas** renvoyé à `/time set` : il part toutes les 20
  ticks, avec l'heure **négative** quand `doDaylightCycle` est à false, et **-1**
  pour une heure 0 gelée (le client ne distingue pas -0 de 0) ;
* `time add 1d` répond « Set the time to 100 » : l'heure modulo 24000, alors
  que `time set` répond la valeur demandée.

L'horloge, la météo, la difficulté et les gamerules sont écrites dans
`level.dat` (`Time`, `DayTime`, `rainTime`, `raining`, …, `GameRules` en
chaînes) et relues au démarrage : un monde rouvert est dans l'état où la
commande l'a laissé. Une gamerule inconnue est gardée et réécrite telle quelle.

---

## 5. Les commandes

**34 commandes livrées** (alias compris : `xp`, `tp`, `tell`, `w`), dans l'ordre d'enregistrement de vanilla (celui de
son paquet Commands, donc de `/help` et des suggestions du client) :

| Commande | Niveau | Ce qu'elle fait réellement ici |
|---|---|---|
| `clear` | 2 | inventaire, armure, main secondaire, grille, curseur ; filtre d'item et de tag ; `maxCount 0` compte sans retirer |
| `defaultgamemode` | 2 | écrit dans `level.dat` (`GameType`) ; les nouveaux joueurs y entrent |
| `difficulty` | 2 | requête et changement, Change Difficulty à tous, persistée ; **la faim et la régénération la lisent** |
| `effect give/clear` | 2 | sur l'API de `effect_session.hpp` ; `infinite`, amplificateur, `hideParticles` |
| `me` | 0 | Player Chat Message, type `emote_command` |
| `experience` / `xp` | 2 | add/set/query, points et niveaux, Set Experience ; `set … points` refuse **à partir** du coût du niveau (17 refusé au niveau 5, mesuré) |
| `fill` | 2 | replace [filtre, tags compris], keep, outline, hollow, destroy ; limite = gamerule `commandModificationBlockLimit` (32768) ; écriture groupée, un rallumage par chunk, Update Section Blocks par section |
| `gamemode` | 2 | **par joueur** : abilities, tab list, Game Event 3 dans l'ordre capturé ; le serveur l'honore partout (dégâts, faim, casse instantanée, emplacement créatif, cibles des creepers) |
| `gamerule` | 2 | les 45, persistées ; `doDaylightCycle`, `doWeatherCycle`, `doMobSpawning`, `keepInventory`, `naturalRegeneration`, `sendCommandFeedback`, `logAdminCommands`, `showDeathMessages` (pour `/kill`), `commandModificationBlockLimit`, `doImmediateRespawn` et `reducedDebugInfo` (envoyés au client) sont **honorées** |
| `give` | 2 | ordre de rangement de vanilla, dépassement jeté au sol, `Damage:0` ajouté aux objets usables, NBT d'item |
| `help` | 0 | usage « intelligent » de Brigadier, ligne par ligne comme vanilla |
| `kick` | 3 | Disconnect avec la raison, message d'administration |
| `kill` | 2 | joueurs (dégât `genericKill`, message de mort d'abord), mobs (animation, butin sans joueur), objets et orbes au sol |
| `list` | 0 | noms, ou noms et UUID |
| `msg` / `tell` / `w` | 0 | sortant et entrant, types `msg_command_*` |
| `say` | 2 | joueur : Player Chat `say_command` ; console : Disguised Chat |
| `seed` | 2 | la graine de `level.dat` ou de `OV_WORLDGEN_SEED`, copiable |
| `setblock` | 2 | replace/keep/destroy (particules et butin), bloc-entité de conteneur créé |
| `spawnpoint` | 2 | point de réapparition personnel, **utilisé à la réapparition** |
| `setworldspawn` | 2 | `level.dat` + Set Default Spawn ; point de réapparition par défaut |
| `summon` | 2 | les types que `EntityWorld` sait mesurer, avec le même cerveau qu'une apparition naturelle |
| `teleport` / `tp` | 2 | les sept formes, `~` et `^`, rotation, `facing` position et entité ; drapeaux relatifs de Synchronize Position comme vanilla ; `facing entity` résolu **avant** de déplacer, Look At de **forme entité** (§ 8) |
| `tellraw` | 2 | composant relu et réécrit à la manière de vanilla, sélecteurs résolus |
| `time` | 2 | set/add/query, day/noon/night/midnight ; horloge persistée |
| `title` | 2 | title/subtitle/actionbar/times/clear/reset |
| `weather` | 2 | clear/rain/thunder, durée ou tirage dans les plages du wiki ; montée des niveaux au flottant près |
| `op` / `deop` | 3 | `ops.json` au format vanilla, niveau 4, Entity Event 24+niveau et arbre renvoyé au joueur concerné |
| `save-all` | 4 | la sauvegarde du serveur, deux messages comme vanilla |
| `stop` | 4 | arrêt propre, monde sauvé |

Plus : **la console** du serveur dédié (stdin, niveau 4, sans `/`, rendu en
anglais par la table de langue) ; **le chat** (Player Chat Message non signé,
`chat.type.text`) ; les **suggestions** (Command Suggestions Response) ; l'arbre
**Commands** élagué par niveau, envoyé à l'arrivée et à chaque `op`/`deop`.

Les tables de cas (lecteur, sélecteurs, blocs, SNBT, JSON, composants, météo,
gamerules, `ops.json`) et les entrées hostiles (15 000 commandes et
suggestions tirées au hasard, JSON et SNBT profonds et bruités — aucun plantage)
sont dans `src/ov_server/tests/test_commands.cpp`.

---

## 6. Ce qui n'est pas fait, et doit être su

**Les ~45 autres commandes de vanilla** ne sont pas là, dont `execute`,
`scoreboard`, `data`, `clone`, `particle`, `playsound`, `worldborder`, `ban`,
`whitelist`. `/help` et la suggestion de `/` en listent donc 34 là où vanilla en
liste 79 : c'est la seule raison des deux différences correspondantes.

**Refusé par son nom, jamais traité en silence** — une commande qui touche l'un
de ces cas répond « Ondes VOXEL does not … yet » :

* les options de sélecteur `nbt`, `scores`, `advancements`, `predicate` (lues en
  entier pour que le curseur reste juste, puis refusées à l'usage) ;
* le NBT de bloc-entité dans `setblock`/`fill`, le NBT de `summon` ;
* les effets de statut sur un mob (les mobs de ce serveur n'en portent pas).

**Approximé, et dit :**

* **les messages d'erreur JSON de Gson** : le cas « malformed » (nom non quoté)
  est reproduit mot pour mot ; les autres (« Unterminated object », …) ont la
  bonne forme mais pas forcément le même texte ;
* **la rareté des objets** n'est pas connue du serveur : tout objet s'affiche en
  blanc (commun) dans le survol de `/give`, là où vanilla colore un objet rare ;
* **la clé `item.` ou `block.`** d'un nom d'objet se décide par la table de
  langue quand elle est présente (le cas du serveur de l'utilisateur), sinon par
  « existe-t-il un bloc de ce nom » ;
* **la barre d'expérience après `xp set … levels`** : vanilla garde sa
  progression flottante, nous gardons des points entiers ; le niveau et le total
  sont justes, la barre peut différer de quelques pixels ;
* **`keepInventory`** garde l'inventaire mais pas l'expérience : la session de
  survie remet l'expérience à zéro à la mort (hors du périmètre de cette vague) ;
* **`op`/`deop` suggèrent** tous les joueurs en ligne (§ 3) ;
* **l'ordre d'itération de `@e` sans tri** est celui de notre instantané
  (joueurs, mobs, objets, orbes) ; vanilla parcourt ses tables d'entités ;
* **la mort par `/kill`** : notre message et notre ligne de succès partent au
  tick de la commande, l'écran de mort au tick de survie suivant ;
* **la graine d'un monde plat** est 0 (celle de `level.dat`) ; vanilla en tire
  une.

**Non modélisé, sans commande pour le montrer :** les succès (« Diamonds! »
apparaît chez vanilla après le premier diamant donné — c'est la seule différence
de la ligne `give @s diamond 5`), la pioche factice et le son de ramassage de
`/give`, les statistiques, le déplacement du ticket de chunk forcé quand
`setworldspawn` déplace le point d'apparition.

**Pour les autres agents :**

* le jeu de mode est maintenant **par joueur** (`Player::game_mode`,
  `Player::mortal()`) ; `--survival` ne fait que choisir le mode par défaut du
  monde. Les tests `options.survival` qui décidaient pour un joueur ont été
  remplacés ; le bloc de survie tourne pour tous et la session épargne elle-même
  créatif et spectateur ;
* l'heure envoyée au client est celle du monde (`WorldState`), plus un midi
  figé : **le soleil tourne** sur un monde dont `doDaylightCycle` est à true, ce
  qui est la valeur par défaut de vanilla et de tout `level.dat` sans règle ;
* `level.dat` porte maintenant l'horloge, la météo, la difficulté et les règles
  (`world::read_level_settings`).

---

## 7. Ce que la mesure a trouvé — de 256 à 269

La première comparaison a donné 256 / 271. Les treize écarts corrigés, et ce
qu'ils avaient à dire :

* **`save_world` n'écrivait pas `level.dat` quand aucun chunk n'était sale.**
  Un `/time set` ou un `/gamerule` seul était perdu au redémarrage. Trouvé par
  un test qui redémarre le serveur, pas par la comparaison : les deux captures
  tournent sans redémarrer. Corrigé dans `save_world`, en bloc délimité.
* **`fill … destroy` relighait un voisinage de neuf chunks par bloc détruit.**
  Le tick calait plusieurs secondes, et les réponses des cinq commandes
  suivantes arrivaient hors de leur fenêtre — cinq écarts qui n'en étaient
  qu'un. La casse (particules, butin) et l'écriture sont maintenant séparées, et
  l'écriture passe par le chemin groupé.
* **`/effect` a un troisième argument que l'anglais n'imprime pas** : la durée
  en secondes, `"0"` pour `infinite`. Quatre écarts.
* **`@e` ne voit pas les morts** ; `@a` et `@p`, si. Et **`/kill` d'un joueur
  annonce la mort avant sa propre ligne**.
* **`xp set … points` refuse une valeur égale au coût du niveau**, pas
  seulement supérieure (17 refusé au niveau 5) ; **`xp set … levels` garde la
  fraction de la barre**, d'où « 4 points » là où nous en gardions 11.
* **La console logue un `/say` avec `[Not Secure]`** comme un message de joueur.
* Le mode de jeu par défaut du monde (créatif chez nous, survie chez vanilla) :
  un écart de montage, fixé dans la capture elle-même par `defaultgamemode`.

---

## 8. `/tp` avec rotation et `facing` (2026-09-11)

Signalé : « notre serveur refuse `/tp @s ~ ~ ~ 0 70` (« Unknown or incomplete command ») ». **Notre
moteur ne le refuse pas** : le test `tp takes a rotation…` le passe, avec les bons drapeaux, et
l'arbre Commands envoyé à un opérateur est identique à celui du jar (§ 7, 34/34). « Unknown or
incomplete command » est le message que le **client vanilla** affiche lui-même quand la commande
n'est pas dans l'arbre qu'il a reçu — et l'arbre est élagué par niveau : un joueur non opérateur ne
reçoit pas `teleport`. Le monde du laboratoire n'avait pas d'opérateur avant `bc6aa3a`
(« operators in the lab ») ; c'est l'explication la plus probable, **non rejouée** avec le client
graphique.

Ce que la mesure, elle, a trouvé — cinq formes ajoutées au banc (`tp @s ~ ~ ~ 0 70`,
`~ ~ ~ ~10 ~-5`, `~1 ~ ~ ~ ~`, `~ ~ ~ 0`, `tp ~ ~ ~ 0 70`, `teleport … -90 -10`, `^1 ^ ^`, `facing
entity @s eyes|feet`, `facing ~ ~ ~5`, `facing entity nobody`) et une comparaison des paquets de
position (`teleports:` dans `check_commands.py`) :

* **Les drapeaux relatifs sont ceux du jeu.** `tp @s ~ ~ ~ 0 70` : position relative (0x07,
  décalage nul), rotation absolue (0, 70) ; `~ ~ ~ ~10 ~-5` : 0x1F et (10, −5) ; sans rotation, la
  rotation voyage relative et nulle (0x18). Lu sur les paquets du jar.
* **`facing entity <cible>` envoie Look At sous sa forme entité** — la position, puis `true`, l'id
  de l'entité et son ancre (`…01 01 01` pour les yeux). Nous envoyions la forme position.
  `encode_look_at_entity` la produit maintenant.
* **`facing entity nobody` ne téléporte personne.** Le jar répond l'erreur et ne bouge pas ; nous
  téléportions d'abord et répondions l'erreur ensuite. La cible est maintenant résolue avant.
* **La hauteur des yeux** dans Look At : le seul échantillon (y −60, hauteur 1,62) porte
  **−58,380001068115234**, la somme arrondie à un flottant — pas le double −58,3799999952. Un
  échantillon : c'est une observation, pas une règle établie.

**Deux pièges du banc, payés ici :**

* **La sonde renvoyait le décalage comme une position.** À chaque Synchronize Position, elle
  confirmait et renvoyait Set Player Position avec les x, y, z **bruts** du paquet, drapeaux
  ignorés : après un `tp ~ ~5 ~`, elle annonçait « je suis en (0, 5, 0) ». Vanilla refuse ce saut
  (« moved too quickly », que `check_commands.py` filtre déjà) ; **notre serveur l'accepte** — il
  n'a pas ce contrôle, écart nommé, hors de ce mandat — et la position du joueur côté serveur
  devenait fausse (un `tp ovprobe ovprobe` qui renvoie (−1, 0, 0)). La sonde applique maintenant
  les drapeaux, comme un vrai client.
* **Une machine chargée décale les fenêtres.** Sous la charge d'autres calculs, les réponses de
  vanilla arrivent une fenêtre de 0,7 s trop tard et la comparaison tombe à 194/282 sans que rien
  n'ait changé. Vanilla renvoie aussi la position (absolue) environ une seconde après un `tp`
  tant que la sonde ne l'a pas confirmée : seul le **premier** Synchronize Position d'une commande
  est comparé.
