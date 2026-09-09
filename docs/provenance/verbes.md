# Brancher les verbes — le levier qui bouge, le mob qui lâche, la faim qui monte

Ce fichier ne décrit pas un moteur de plus. Il décrit le **branchement** de deux
moteurs écrits, mesurés et couverts par des tests — `CombatSession`
(`docs/provenance/combat.md`) et `gameplay::ItemUse` — que personne n'appelait.

L'état avant cette vague est nommé par l'agent d'intégration lui-même, dans
`docs/provenance/branchement.md` § 5 :

> **Aucun handler d'interaction de bloc.** […] un levier, un bouton, une porte,
> une trappe ne réagissent à rien. **C'est le trou le plus visible qui reste**.

Et par l'agent du combat, en tête de `src/ov_server/src/combat_session.hpp` :

> **Nothing calls this yet.**

Les deux sont fermés.

---

## 1. Ce que le branchement a demandé

### Le `LevelWriter` d'un clic droit — `player_level.{hpp,cpp}`

`ItemUse::use_on` prend un `world::LevelWriter&`. Le serveur en possédait déjà
un, `ServerLevel`, et **c'était le mauvais** : il appartient au thread de tick,
il enregistre chaque écriture dans `changed_` pour que le drain la règle, et son
crochet `set_block` est celui du drain — il suppose le verrou de chunk déjà
tenu et diffère le rallumage.

L'interaction d'un joueur arrive sur le **thread réseau**, dans l'aiguillage de
paquets. Elle doit se comporter comme une pose : prendre le verrou par accès,
diffuser son propre `Block Update`, et mettre la notification de voisinage en
file pour le tick suivant. `PlayerLevel` est exactement cela — six crochets et
aucun état sauf un compteur d'écritures.

**Le seul point délicat est le tick programmé.** Un bouton programme un tick de
bloc trente ticks plus loin, et la file qui le reçoit vit dans `ServerLevel`, sur
le thread de tick ; `BlockTickScheduler` n'est pas thread-safe. Il est atteint
quand même, parce que **tous** les accès à cette file dans ce serveur — le drain,
le chargeur de chunks, la sauvegarde — se font sous `chunk_mutex`. Le crochet
`schedule_tick` prend le même verrou et écrit dans la vraie file. Aucune file
différée, aucune latence supplémentaire, aucun second chemin qui puisse dériver.

### Les cinq points d'appel

Le header de `combat_session.hpp` les listait ; ils sont tous là, en blocs
délimités `// ── combat and interaction ──` dans `server.cpp` :

| paquet | où |
|---|---|
| `Interact` (0x10) | nouveau `case`, → `on_interact` |
| `Use Item` (0x32) | nouveau `case`, → `on_use_item` |
| `Swing Arm` (0x2F) | nouveau `case`, → `on_swing` |
| `Use Item On` (0x31) | `on_use_item_on` **avant** le chemin de pose |
| `Player Action` status 5 · `Set Held Item` | → `on_release` |
| le tick | `who.combat.tick(view, step)` + la faim |

L'ordre du quatrième est la règle et non un détail : le bloc a le premier refus,
puis l'objet. Et **`Fail` n'est pas `Pass`** — une porte en fer cliquée à mains
nues refuse, et laisser ce refus retomber sur la pose poserait un bloc *à
travers* la porte. `CombatOutcome` a gagné un champ `result` pour cela ; `hit`
seul ne distingue pas les trois manières de ne pas toucher.

### Le mob qu'on frappe — `mob_combat.{hpp,cpp}`

Deux choses n'avaient nulle part où vivre :

* **une fenêtre d'invulnérabilité par mob.** `EntityState` (couche 8) porte une
  vie et une vie maximale et rien d'autre, délibérément : les *règles* de dégâts
  sont couche 9. La fenêtre vit donc dans une table à côté du monde d'entités,
  indexée par l'id réseau — celui qu'un client nomme dans un `Interact`. Les
  règles appliquées sont `gameplay::apply_damage`, les mêmes que pour le joueur :
  un mob frappé deux fois en moins de dix ticks prend la **différence**, pas la
  somme.
* **les tables de butin d'entité.** `EntityLootTables` était confrontée à
  `/loot give <joueur> kill <entité>` depuis la vague combat — 91 tables
  conformes sur 92 — et rien dans le serveur ne l'appelait.

---

## 2. La preuve : sur le banc, avec un vrai client

`scripts/check_interaction_e2e.py`. Le décor est le banc `ov_lab`, le client est
celui des autres sondes du dépôt : protocole 763, mêmes paquets, mêmes champs,
même ordre, pas d'écran.

### Le levier — 14 / 14

Parcelle « dust », levier `face=floor` en (1, −59, 12), quatorze fils de
(2, −60, 12) à (15, −60, 12).

```
attendu  15 14 13 12 11 10 9 8 7 6 5 4 3 2      (max(0, 15 − i))
obtenu   15 14 13 12 11 10 9 8 7 6 5 4 3 2
```

**14 positions sur 14**, la table de `docs/provenance/redstone.md` § 3. La vague
précédente avait obtenu le même 14/14 **en poussant le circuit par une
écriture**, faute de pouvoir actionner le levier. Celui-ci est tiré.

### Ce qui s'ouvre

| | résultat |
|---|---|
| `oak_door` (99, −60, 133) | `open` basculé puis rebasculé |
| `oak_trapdoor` (99, −60, 138) | idem |
| `oak_fence_gate` (69, −60, 131) | idem |
| `oak_button` posé puis cliqué | `powered=true` — **et il y reste** |
| `iron_door`, main nue | refusé (`Fail`), test unitaire |

### Le butin

Serveur en survie, mobs placés par `--mobs=`, frappés à mains nues, butin lu sur
les `Spawn Entity` + `Set Entity Metadata` des piles au sol.

| mob | tués | butin observé | table |
|---|---|---|---|
| vache | 5 | bœuf ×14, cuir ×3 | bœuf 1–3, cuir 0–2 |
| vache | 8 (seconde campagne) | bœuf ×19, cuir ×4 | espérance 16 et 8 |
| zombie | 5 | chair putréfiée ×7 | 0–2 |

Confronté à notre propre tirage hors ligne, `ov_mobloot` sur 300 tirages :
vache → cuir 307, bœuf 635 (espérances 300 et 600). Les deux chemins tirent la
même table ; ce que la sonde vérifie est que le **serveur** l'atteint.

### Manger

La sonde ne peut pas se donner de nourriture : **en survie, le serveur ignore
`Set Creative Slot`** — vanilla le fait, et `server.cpp` le faisait déjà avec le
commentaire qui le dit. Alors elle mange ce qu'elle a tué. La chaîne complète
tient en une campagne :

> tuer une vache → tirer sa table → poser la pile au sol → marcher dessus →
> la ramasser → courir 390 blocs → tenir le bouton 32 ticks → **faim 13 → 16**

+3, qui est la nutrition mesurée du bœuf cru (`docs/provenance/survie.md`).
Chacun de ces maillons est du code que cette vague a branché.

---

## 3. Le bug que le branchement a trouvé : un anneau de notification trop court

Le levier n'allumait **rien**. Zéro fil sur quatorze, alors que le levier
lui-même basculait correctement.

Une vague notifie une position et ses six voisins — exactement ce que fait
`Level.updateNeighborsAt` de vanilla. **Et ce n'est pas assez pour un levier** :
un levier au sol alimente le bloc *sous* lui, et le fil qu'il doit allumer est en
diagonale du levier, un anneau trop loin. Vanilla y arrive parce que
`LeverBlock` notifie **deux fois**, autour de lui-même et autour du bloc auquel
il est fixé ; le bouton et la plaque de pression font pareil.

`ov_gameplay` n'a pas de crochet par bloc pour cela — `Redstone::neighbour_changed`
est tout le vocabulaire. La graine d'une modification de joueur est donc
maintenant la position **et ses six voisins** (`WorldTicks::notify`), ce qui
notifie l'anneau du support parmi d'autres : un sur-ensemble de la règle de
vanilla.

**Sur-notifier est sans danger ici, et ce n'est pas un espoir** : chaque règle
recalcule son état depuis le monde au lieu d'intégrer une différence, donc une
position prévenue deux fois pour rien n'écrit rien et ne produit pas de seconde
vague. Sous-notifier, lui, laissait le levier du banc n'allumer rien du tout.

C'est le piège 9 du briefing (« `/setblock` ne notifie que six voisins et
s'arrête ») rencontré depuis l'autre côté : cette fois c'est *notre* serveur qui
s'arrêtait à six.

---

## 4. Ce qui n'est pas fait, et qui doit être su

* **Un bouton enfoncé ne se relève jamais.** Le tick programmé part bien — il
  passe par `PlayerLevel::schedule_tick` dans la vraie file, et le test unitaire
  le vérifie — mais `Redstone::scheduled_tick` connaît la torche, le répéteur, le
  comparateur, l'observateur et les consommateurs, **et pas le bouton** : il
  tombe dans `consumer_rule`, qui ne trouve rien, et rend `false`. C'est une
  lacune de `src/ov_gameplay/src/redstone.cpp`, hors des fichiers de cette vague.
  La plaque de pression a le même trou.
* **Les écrans que le serveur n'ouvre pas.** Coffre, établi et fours sont
  interceptés avant `interact_block` par le code qui existait ; un tonneau, une
  enclume, un lutrin ressortent en `ScreenKind` et sont **journalisés** au lieu
  d'être ignorés.
* **La TNT amorcée** est nommée et non engendrée : il faudrait une entité TNT.
* **Le clic droit sur une entité** (tondre, seller, nommer) est refusé et nommé
  par `on_interact`.
* **Quatre disqualifications de coup critique ne sont pas connues du serveur** :
  les yeux dans l'eau, une échelle, la cécité, une monture. Les quatre sont donc
  `false`, ce qui rend un critique légèrement trop facile. Écrit dans
  `combat_view`.
* **Ni le feu ni la taille des slimes** ne sont suivis, donc `KillContext` les
  passe à leur valeur neutre : un mob en flammes ne fait pas fondre son butin, et
  une slime tire sa ligne de taille 0.
* **L'XP lâchée par un mob** n'existe pas : il n'y a pas de table d'XP de mob
  dans `src/`, et `docs/provenance/combat.md` § 9 le disait déjà.
* **La sneak n'est pas consultée par le coffre.** `player.sneaking` est
  maintenant suivi (Player Command et le champ que porte chaque `Interact`) et
  `ItemUse::use_on` l'applique ; le chemin coffre/établi de `server.cpp`, lui,
  gagne toujours. Non touché : c'est le montage d'un autre agent.
* **Le banc sur disque peut être plus vieux que les parcelles.** `run/lab` est
  persistant et `lab.sh` ne le régénère que sur `--rebuild` : le levier de la
  parcelle « dust » était de **l'air** dans le banc trouvé sur disque. Une sonde
  qui clique de l'air ne dit rien de plus qu'un serveur cassé. La campagne se
  bâtit donc son propre banc avec `ov_lab`.
