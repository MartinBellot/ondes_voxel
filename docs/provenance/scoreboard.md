# Scoreboard, équipes, `/trigger`, `/teammsg` — ce que le jar répond, et ce que nous répondons

Avant cette vague, le serveur n'avait **aucun** scoreboard : `scoreboard`, `team`, `trigger`,
`teammsg` et `tm` étaient absents de l'arbre de commandes, `world/data/scoreboard.dat` n'était ni
lu ni écrit, un nom de joueur ne portait jamais la couleur de son équipe, et le sélecteur
`team=` répondait comme si personne n'était dans une équipe.

Ce dossier décrit le modèle, le fichier, les quatre paquets, les critères, les commandes — et
**la mesure** : deux sondes tapent la même liste de commandes au vrai serveur 1.20.1 puis au
nôtre, et les réponses sont comparées octet pour octet.

| Mesure (2026-09-11, `check_scoreboard.py`) | Résultat |
|---|---|
| Phase 1 — 189 commandes d'un opérateur : chat, et les quatre paquets du scoreboard, **octet pour octet** | **189 / 189** — 188 identiques, 1 aux mêmes lignes dans un autre ordre (§ 4) |
| Ce qu'un joueur qui arrive reçoit (équipes, objectifs, emplacements, scores triés, puis ses critères) | **identique**, paquet pour paquet |
| Console du serveur dédié (réponses des commandes tapées à la console) | **5 / 5** |
| Journal des opérateurs (`[ovprobe: …]`, toute la capture) | **120 / 120** |
| Phase 2 — deux sondes : chat d'équipe, `/trigger` au niveau 0, critères, coups d'épée entre joueurs et leur mort, ce que chacune voit | **103 / 108**, 3 dans un autre ordre, 2 écarts qui sont un seul paquet dont le jar ne fixe pas lui-même l'étape (§ 6 bis) ; 88 / 108 avant le coup d'épée |
| Témoin : le jar contre **lui-même**, deux captures | 184/185, 101/108, 5/5, 120/120 — ses propres écarts sont aux coups et à l'ordre d'identité : ce qui bouge d'une exécution à l'autre du jar |
| Arbre Commands et `/help` des cinq commandes nouvelles, par la sonde générale (`check_commands.py`) | `/help` : **39 / 39** lignes identiques ; `tp` : 26/26 ; réponses de la sonde générale **279 / 282** (les trois écarts préexistent : la ligne de progrès de `/give`, le nombre de mobs tués par `/kill`, le `/help` complet de vanilla) — aucune régression des 34 commandes existantes |
| Tests unitaires `test_ov_scoreboard`, `test_scoreboard_packets` (dans `test_ov_protocol`), `test_scoreboard_view` (dans `test_ov_client`) | verts ; `test_ov_commands` mis à jour (l'arbre de niveau 0 est désormais celui de vanilla, entier) |

Ce que la mesure a trouvé, et qui a été corrigé en route : `players list *` (un `*` dans un
argument à un seul détenteur ne nomme **personne** chez le jar) ; un composant `score` lu depuis un
tampon vidé ; le préfixe et le suffixe d'équipe, que le jar **n'annonce pas** aux opérateurs ; et le
nom de la source dans le journal, que le jar fige **au début** de la commande (`team leave @s` est
journalisé sous l'équipe qu'on quitte).

Reproduire :

```bash
$S/ovlane.sh java python3 scripts/capture_scoreboard.py vanilla   # le jar, dans le couloir java
python3 scripts/capture_scoreboard.py ov                          # notre ov_dedicated
python3 scripts/check_scoreboard.py                               # la comparaison
$S/ovlane.sh java python3 scripts/capture_scoreboard.py crossload FICHIER nom   # le jar relit un scoreboard.dat
python3 scripts/check_scoreboard.py dat A.dat B.dat               # deux fichiers, fait par fait
```

---

## 1. Les sources

| Sujet | Source | Statut |
|---|---|---|
| Paquets Display Objective (0x51), Update Objectives (0x58), Update Teams (0x5A), Update Score (0x5B) | archive figée `…/Protocol?oldid=2773082`, sections du même nom | ✅ chaque champ ; **confirmé** par la capture, octet pour octet (`test_scoreboard_packets.cpp`) |
| Arbre de `scoreboard`, `team`, `trigger`, `teammsg`, `tm` | le paquet Commands **capturé** sur le jar, décodé par `scripts/commands_graph.py` | ✅ littéraux, noms d'argument, parseurs, sources de suggestion, place dans la racine |
| Clés de traduction et leurs arguments | `run/assets/.../lang/en_us.json` (asset local, jamais commité) pour la liste ; la capture pour **quels arguments sont des chaînes nues** et lesquels des composants | ✅ |
| Critères (`dummy`, `trigger`, `deathCount`, `playerKillCount`, `totalKillCount`, `health`, `food`, `air`, `armor`, `xp`, `level`, `teamkill.<couleur>`, `killedByTeam.<couleur>`, `minecraft.<type>:<id>`) | Minecraft Wiki, article *Scoreboard* (liste et sens de chaque critère) ; registres `minecraft:stat_type`, `minecraft:custom_stat` de `generated/reports/registries.json` | ✅ ; la validation d'un critère de statistique (`minecraft.mined:minecraft.nosuch` refusé) est **mesurée** |
| Options d'équipe (`friendlyFire`, `seeFriendlyInvisibles`, `nametagVisibility`, `deathMessageVisibility`, `collisionRule`, couleur, préfixe, suffixe) | Minecraft Wiki, articles *Scoreboard* et *Commands/team* | ✅ ; les effets côté serveur sont au § 6 |
| Format de `data/scoreboard.dat` | le fichier **écrit par le jar** à la fin de la capture (909 octets gzip), relu clé par clé | ✅ (§ 3) |
| Ordre d'itération de `java.util.HashMap` et `String.hashCode` | comportement documenté de la bibliothèque Java (Javadoc) | ✅ ; reproduit dans `java_hash_order.hpp` et **vérifié** sur trois ordres capturés (§ 4) |

Aucun code de Paper/Spigot/Bukkit n'a été lu, ni aucun code du jeu décompilé : chaque comportement
ci-dessous est lu sur les réponses du jar.

---

## 2. Ce que la capture a appris

Ce qui n'est écrit nulle part et que les paquets disent :

* **Un objectif ne voyage que s'il est affiché.** `objectives add` n'envoie rien ; le premier
  `setdisplay` qui le montre envoie Update Objectives *Create*, un Display Objective par
  emplacement qui le montre, puis chaque score. Le dernier emplacement qui le lâche envoie
  *Remove* — sans Display Objective vide. Un emplacement vidé alors que l'objectif reste montré
  ailleurs n'envoie que l'emplacement.
* **Les scores d'un objectif qui arrive sont triés** par valeur croissante, puis par nom
  **à rebours**, casse ignorée : `a` (−2147483648), `nobody`, `b`, `a,b` (tous à 0), `#hidden`
  (3), `ovprobe` (12).
* **Un score naît à 0, et le client le voit naître.** `players set @s d 5` envoie **deux**
  Update Score : 0, puis 5. `operation a,b d += b d`, qui laisse `a,b` à 0, n'en envoie qu'un.
* **Un détenteur dont le dernier score part est effacé en entier** : `players reset fake d`, sur
  l'unique score de `fake`, envoie une suppression avec un nom d'objectif **vide** — « tout ».
  Un second `reset fake` n'envoie plus rien, et répond quand même par un succès.
* **`team add` crée puis renomme** : Update Teams *Create* avec le nom par défaut, puis *Update*
  avec le nom donné — même quand c'est le même.
* **`team join` d'un joueur déjà dans l'équipe** le retire puis le remet : *RemoveEntities*,
  puis *AddEntities*.
* **Un nom de joueur porte son équipe partout** : `tp`, `give`, `effect`, `xp`, `list`, les
  messages de mort, `say`, `me`, `msg`, le chat, la ligne d'administration de la console
  (`[[R] ovprobe !: …]`). La forme exacte : la couleur de l'équipe **sur le composant qui porte
  déjà** le clic, le survol et l'insertion, et le texte déplacé dans `extra` entre le préfixe et
  le suffixe — `{"color":"red","insertion":"ovprobe",…,"extra":[{"text":"[R] "},{"text":"ovprobe"},{"text":" !"}],"text":""}`.
  Une équipe sans couleur ne met pas de clé `color` ; un préfixe vide reste `{"text":""}`.
* **`/teammsg`** part en Player Chat de type 6 (*sortant*) pour l'expéditeur, 5 (*entrant*) pour
  les autres membres ; la cible est le nom d'équipe entre crochets, dans sa couleur, avec un clic
  qui propose `/teammsg ` et un survol `chat.type.team.hover`.
* **Les critères du joueur** (`health`, `food`, `air`, `armor`, `xp`, `level`) sont **tous posés
  au premier tick** d'un joueur qui arrive (`ovother` reçoit `hp` 0 puis 20 juste après les
  équipes et les objectifs), puis seulement quand la valeur change. Un joueur déjà là quand
  l'objectif est créé n'a pas de score tant que sa valeur ne bouge pas.
* **`xp` est le total de points**, que `xp add … levels` ne change pas : 3 niveaux puis 5 points
  donnent `xp` = 5, `level` = 3.
* **Ce qui annonce aux opérateurs** (`chat.type.admin`) : tout ce qui change quelque chose ;
  jamais `list` ni `get`. Lu sur le journal de la console, et sur `ovprobe` qui reçoit la ligne
  d'administration du `/trigger` d'`ovother`.
* **Les erreurs sans place.** `arguments.objective.notFound`, `team.notFound`,
  `argument.criteria.invalid`, `argument.scoreboardDisplaySlot.invalid`, `argument.color.invalid`,
  `arguments.operation.invalid` n'ont pas de seconde ligne `…<--[HERE]`. Seules les erreurs du
  lecteur (entier, séparateur, argument inconnu) en ont une.
* **Les opérations sont celles de Java** : `/=` est `Math.floorDiv` (−7 / 2 → −4), `%=` est
  `Math.floorMod` (−7 % 2 → 1), la division par zéro est refusée (`arguments.operation.div0`),
  l'addition déborde (2147483647 + 1 → −2147483648). Une source sans score en reçoit un, à 0.
* **1.20.1 n'a pas de limite de 16 caractères** : un objectif et une équipe de 26 lettres passent.

---

## 3. Le fichier — `data/scoreboard.dat`

gzip, NBT, racine `{data: {Objectives, PlayerScores, Teams, DisplaySlots}, DataVersion: 3465}` :

| Liste | Clés |
|---|---|
| `Objectives` | `Name`, `CriteriaName` (nom complet : `minecraft.mined:minecraft.stone`), `DisplayName` (JSON), `RenderType` (`integer` / `hearts`) |
| `PlayerScores` | `Name`, `Objective`, `Score`, `Locked` (octet) |
| `Teams` | `Name`, `DisplayName`, `TeamColor` (**absente** pour une équipe sans couleur), `AllowFriendlyFire`, `SeeFriendlyInvisibles`, `MemberNamePrefix`, `MemberNameSuffix`, `NameTagVisibility`, `DeathMessageVisibility`, `CollisionRule`, `Players` |
| `DisplaySlots` | `slot_<n>` → nom de l'objectif (0 list, 1 sidebar, 2 belowName, 3+couleur) |

Les clés composées sont écrites dans l'ordre d'une `HashMap` Java, comme le jeu les écrit.
**Une clé que nous ne modélisons pas repart intacte** — à la racine, dans `data`, ou un objectif
dont le critère nous est inconnu, avec ses scores. Un fichier illisible (pas gzip, pas NBT,
`DataVersion` plus récente) est refusé avec son chemin, et **n'est pas réécrit** : le serveur
démarre avec un scoreboard vide et le dit.

`Locked` absent se lit « déverrouillé » ; un score neuf, lui, naît verrouillé (il faut
`players enable` pour `/trigger`).

### Le fichier dans les deux sens (2026-09-11)

| Sens | Mesure | Résultat |
|---|---|---|
| **jar → nous → fichier** | le `scoreboard.dat` écrit par le jar, lu par notre serveur et réécrit (`.scratch/rewrite.py` : un détenteur posé puis effacé pour salir le scoreboard, puis `save-all`) | **43 / 43 faits identiques** (`check_scoreboard.py dat`), et **octet pour octet** : les 3447 octets décompressés sont ceux du jar. Deux écarts trouvés en route : `TeamColor` inséré au rang où le jeu l'insère (il partage une case de hachage avec `MemberNamePrefix`), et la liste vide d'une équipe sans membre, de type `TAG_End` |
| **nous → jar** | notre fichier, ouvert par le vrai serveur (`capture_scoreboard.py crossload`) : ce qu'il envoie à un arrivant | **19 / 19 paquets identiques** à ceux qu'il envoie après avoir lu **son propre** fichier |
| | ses réponses (`objectives list`, `players list`, `team list`, les membres, les scores de chaque détenteur) | **11 / 13 identiques** ; les deux autres listent les scores de `ovprobe` et `ovother`, qui diffèrent **par le contenu** : les scores de meurtre que notre capture n'a pas pu produire (pas de combat entre joueurs) |
| | le fichier qu'il réécrit | **39 / 39 de nos faits conservés**, 0 perdu ; il ajoute les cinq critères de la sonde qui s'est connectée |
| **témoin** | le jar qui relit **son** fichier et le réécrit | 43 / 43 conservés, les mêmes cinq critères ajoutés — le changement qu'un cycle vanilla fait de lui-même |

---

## 4. L'ordre de Java, reproduit

`scoreboard objectives list`, `team list`, les détenteurs que nomme `*`, l'ordre des équipes et
des objectifs envoyés à un joueur qui arrive, l'ordre des listes du fichier : tout cela parcourt
une `HashMap<String, …>` du jeu. Plutôt que de trier (ce qui se lirait mieux et se comparerait
moins bien), `java_hash_order.hpp` reproduit la table : hachage `String.hashCode` sur les unités
UTF‑16, étalé en `h ^ (h >>> 16)`, 16 cases qui doublent au-delà des trois quarts, parcours par
case puis par ordre d'insertion. Vérifié sur la capture : les **18** objectifs, les **4** équipes,
les **3** détenteurs de `*`, dans l'ordre du jar.

Deux listes ne suivent pas cet ordre, et sont **triées** comme le jar les trie : les détenteurs de
`players list` et les membres de `team list <équipe>`.

**Un ordre n'est pas reproductible, et c'est dit** : les scores d'**un** détenteur
(`players list ovprobe`, et l'ordre de `PlayerScores` dans le fichier). Le jeu les range dans une
table indexée par l'**identité** de l'objet objectif, dont le hachage change d'une exécution du jar
à l'autre. Nous les donnons dans l'ordre où ils ont été posés ; `check_scoreboard.py` compare ces
lignes comme un ensemble et les compte à part.

---

## 5. Le code

| Fichier | Rôle |
|---|---|
| `ov_protocol/scoreboard_packets.{hpp,cpp}` | les quatre paquets, dans `ov::net::scoreboard` (pas `clientbound`, pour qu'une autre vague qui déclare les mêmes identifiants ne casse pas la compilation à la fusion) |
| `ov_server/src/scoreboard/java_hash_order.hpp` | l'ordre d'une `HashMap` Java |
| `ov_server/src/scoreboard/scoreboard.{hpp,cpp}` | le modèle, ses paquets en file, les critères, le fichier |
| `ov_server/src/commands/scoreboard_commands.cpp` | `scoreboard`, `team`, `teammsg`, `tm`, `trigger`, et `decorate` (le nom d'un joueur dans son équipe) |
| `ov_server/tests/test_scoreboard.cpp`, `ov_protocol/tests/test_scoreboard_packets.cpp` | les réponses et les octets de la capture |

**Le fil d'exécution.** Le scoreboard est de l'état du monde : un seul écrivain, le thread de
tick, sans verrou. Les paquets qu'il produit sont mis en file et partent **avant** la ligne de
chat de la commande qui les a causés (`reply` vide la file d'abord), comme chez le jar. Le chat
d'un joueur, qui doit porter son équipe, passe maintenant par la file du tick
(`enqueue_chat`) au lieu d'être diffusé depuis le thread réseau ; un meurtre vu ailleurs aussi
(`enqueue_kill`).

**Pour `/execute`** (`if score`, `store … score`) : `Scoreboard::score`, `set_score`,
`get_or_create`, `reset`, et `CommandService::scoreboard()` — l'API est dans
`scoreboard/scoreboard.hpp`.

**Pour les statistiques** : `Scoreboard::on_stat(détenteur, type, id, total)` ; un critère
`minecraft.<type>:<id>` prend le total comme score, comme chez le jar. Aucune statistique n'est
tenue sur ce serveur : le crochet est posé, personne ne l'appelle encore.

---

## 6. Les équipes dans le jeu

| Effet | Où | État |
|---|---|---|
| **Le nom d'un joueur dans son équipe** (couleur, préfixe, suffixe) | `CommandService::decorate`, appliqué à chaque ligne qu'une commande envoie ou journalise, au chat, à `/say`, `/me`, `/msg`, `/teammsg`, `/tellraw`, `/title`, au message de mort de `/kill` | ✅ forme exacte de la capture |
| **Tir ami** (`friendlyFire false`) | les deux chemins où un joueur blesse un joueur : **le coup d'épée** (§ 6 bis, rien n'est envoyé — la capture) et **la flèche** (`ProjectileHost::may_hurt`, elle rebondit comme sur un joueur en créatif) | ✅ |
| **Critères de meurtre** | `enqueue_kill` depuis la mort d'un mob tué par un joueur, d'un joueur tué d'un coup d'épée ou d'une flèche de joueur ; `deathCount` au passage de vivant à mort, quelle qu'en soit la cause | ✅ mesuré : `kills`, `total`, `tkblue`, `kbred`, `deaths` de la capture |
| **Règle de collision** | `Scoreboard::can_push` écrit et testé ; le serveur ne pousse **aucune** entité (pas de poussée joueur–mob ni mob–mob côté serveur) : la règle n'a rien à régir ici. Les clients vanilla l'appliquent entre joueurs, depuis Update Teams | ⚠️ rien à brancher côté serveur |
| **Visibilité des étiquettes, invisibles alliés** | côté client, depuis Update Teams | ✅ envoyé ; notre client ne dessine pas encore d'étiquette de nom |
| **Messages de mort** | toute mort est annoncée à tous (`showDeathMessages`), noms habillés, tueur nommé — § 6 bis | ✅ ; la **visibilité par équipe** (`deathMessageVisibility`) n'est pas encore appliquée à cette diffusion |
| **Couleur dans la liste des joueurs** | vanilla ne l'envoie pas dans Player Info : le client la tire des équipes | ✅ côté serveur ; la liste de notre client est l'affaire d'une autre vague |
| **Barre latérale de notre client** | `ov_client/scoreboard_view` (modèle tenu depuis les quatre paquets, lignes, noms habillés) et deux lignes dans `Interface` | ⚠️ dessinée ; **mise en page non mesurée** contre le vrai client |

---

## 6 bis. Le coup d'épée d'un joueur à un joueur

Absent du serveur jusqu'ici : `hurt_entity` ne visait que les mobs, et la phase à deux sondes le
montrait (20 écarts sur 108, tous là). Il passe maintenant par **le même chemin** que le coup d'un
mob sur un joueur, sans second calcul de dégâts :

* la frappe est celle de `CombatSession` (jauge `0,2 + 0,8·f²`, critique ×1,5, élan et
  Recul, balayage — qui touche aussi les joueurs de la boîte —, arme et enchantements) ;
* la victime la reçoit dans **sa** `SurvivalSession::hurt` : armure, robustesse, Résistance,
  Protection et fenêtre d'invulnérabilité s'y appliquent déjà, tenus à jour à chaque tick ;
* refusée, elle n'atteint rien que les clients voient : `pvp` éteint (`--no-pvp`, l'équivalent
  de `pvp=false` de `server.properties`), victime en créatif ou morte, coéquipier sans tir ami ;
* le Damage Event **nomme la source** (`021f020200` : victime 2, `player_attack`, cause et
  source directe 1) ;
* **Hurt Animation** à la seule victime. Une mesure antérieure
  (`protocol_corrections_1201`) n'en trouvait aucune sur des coups d'autres types ; la capture à
  deux sondes en montre une **à chaque coup de joueur à joueur**, `024333ffff`. C'est elle qui
  décide. L'angle vaut 179,99998 et non 180 : la conversion en degrés est celle arrondie en
  float, 57,2957763671875 par radian (testé au bit près) ;
* à la mort, `death.attack.player` **nomme le tueur**, les deux noms habillés de leurs équipes,
  dans l'écran de mort de la victime (Combat Death, sans identifiant de tueur en 1.20.1) puis
  dans le chat de tous tant que `showDeathMessages` est vrai. Toutes les morts sont désormais
  annoncées à tous, comme chez le jar ; seul le message de `/kill` l'était ;
* `playerKillCount`, `totalKillCount`, `teamkill.<couleur>`, `killedByTeam.<couleur>` et
  `deathCount` suivent.

**Mesuré (2026-09-12, `check_scoreboard.py`, la même phase à deux sondes)** : **103 / 108**
identiques, 3 aux mêmes lignes dans un autre ordre (l'ordre d'identité du § 4), et **2** écarts —
qui sont **un seul paquet vu deux fois** : le score `hp` à 0 de la victime, que l'attaquant reçoit
dans l'étape du coup mortel chez nous, dans l'étape suivante chez le jar. Ce n'est pas une
différence de comportement : **les deux captures du jar lui-même se contredisent exactement là**.
Comparé à l'autre exécution du jar, nous sommes aussi à 103 + 3, et l'écart restant est l'autre
paquet que le jar ne place pas deux fois au même pas (le score `hp` à 20 après la régénération).
La capture ne date pas les paquets : elle ne peut pas départager plus finement qu'une étape.
Parti de 88 / 108 : le coup d'épée, puis les critères du joueur mis à jour **après** son tick de
survie (Set Health puis le score, comme le jar — ils l'étaient une phase trop tôt), et la couleur
d'un objet enchanté dans `/give`.

## 7. Refusé, approximé, non fait

**Refusé, comme le fait 1.20.1 :** `scoreboard players display …`, `objectives modify … numberformat`
et `displayautoupdate` sont de 1.20.3 ; l'arbre ne les contient pas, et le lecteur répond l'erreur
d'argument que le jar répond.

**Non reproductible, et dit :** l'ordre des scores d'un détenteur (§ 4).

**Approximé, et dit :**

* **`armor`** lit les points des pièces portées que le serveur tient à jour à chaque tick pour
  les dégâts (`mitigation.armour`, mobs-3) ; la capture n'a que des joueurs sans armure (0).
* **`health` est `⌈santé + absorption⌉`** : la capture n'a pas d'absorption ; sans elle, 20, 19
  et 0 sont mesurés.
* **Un emplacement remplacé** par un objectif déjà montré ailleurs envoie ici un seul Display
  Objective ; ce cas n'est pas dans la capture.
* **`/team join` depuis la console sans membres** répond `permissions.requires.entity` ;
  non capturé.

**Non fait :**

* **Aucune suggestion d'objectif ni d'équipe** : les suggestions sont servies depuis le thread
  réseau, qui ne peut pas lire le scoreboard du tick. Les vocabulaires fixes (emplacements,
  couleurs, opérations, critères) sont suggérés.
* **Les statistiques** : `Scoreboard::on_stat` est le crochet ; aucune statistique n'est tenue.
* **belowName** : envoyé et tenu par notre client (`below_name_score`) ; rien ne dessine
  d'étiquette de nom.
* **La barre latérale de notre client** n'est pas comparée au pixel avec le vrai client.
