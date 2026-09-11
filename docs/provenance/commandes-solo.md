# « Autoriser les commandes » — le solo avec et sans commandes

## Le problème

L'écran Créer un monde avait le bouton « Autoriser les commandes » grisé, et
notre serveur intégré donnait **toujours** le niveau de permission 4 à tout
joueur. `level.dat` écrivait `allowCommands: 1` en dur. Un monde vanilla créé
sans commandes, rouvert chez nous, donnait donc les commandes à son hôte ; et le
client ne lisait pas l'Entity Event qui porte le niveau d'op.

Ce document dit ce qui a été construit et **d'où viennent ses règles** : du
vrai client 1.20.1 en fonctionnement, piloté par `scripts/screens_oracle.java`
(parcours `allow-commands` et `allow-commands-list`).

---

## 1. L'écran Créer un monde — mesuré bouton par bouton

Mesure : le vrai client sur son écran Créer un monde, onglet Jeu ; à chaque
pas, le texte, la clé de traduction et l'état actif des boutons Mode de jeu et
Autoriser les commandes, et l'état interne de l'écran (`WorldCreationUiState` :
`getGameMode`, `isAllowCheats`, `isHardcore`), lu par réflexion.

| pas | mode | Autoriser les commandes | actif |
|---|---|---|---|
| ouvert, non touché | Survie | NON | oui |
| 1 clic mode | Hardcore | NON | **non** |
| 2 clics | Créatif | **OUI** | oui |
| 3 clics | Survie | NON | oui |
| clic Commandes (→ OUI) | Survie | OUI | oui |
| puis mode ×1 | Hardcore | NON | non |
| ×2 | Créatif | OUI | oui |
| ×3 | Survie | **OUI** (gardé) | oui |
| clic Commandes (→ NON) | Survie | NON | oui |
| puis mode ×2 | Créatif | **NON** (gardé) | oui |
| ×3 | Survie | NON | oui |

Les onze observations tiennent en une règle (`client::AllowCheats`,
`menu_layouts.hpp`) :

- **tant que le joueur n'a pas appuyé** sur le bouton, il suit le mode :
  OUI en Créatif, NON sinon ;
- **dès qu'il a appuyé**, sa valeur reste, à travers tout changement de mode ;
- en **Hardcore**, il affiche NON et est **inactif**, quel que soit le choix —
  et le choix revient avec le mode suivant ;
- le mode cycle **Survie → Hardcore → Créatif → Survie**.

Le bouton est `options.generic_value(selectWorld.allowCommands, options.on|off)`,
210×20 à (322, 174) en 854×480 — la disposition des écrans l'avait déjà.
Libellés tirés du fichier de langue du jeu : « Allow Cheats: OFF » en `en_us`,
« Commandes : Non » en `fr_fr`.

Captures du vrai client (Faithful 32x, échelle 3) : `ac-01-game-survival.png`,
`ac-02-game-hardcore.png` (bouton grisé), `ac-03-game-creative.png`.

**Côte à côte avec le nôtre** (`ov_voxel --dump-menu`, 1280×720, échelle 3) :
à l'ouverture, `cheats 322,174 210x20 active true "Allow Cheats: OFF"` sous
`game_mode … "Game Mode: Survival"` ; après un clic sur le mode,
`"Game Mode: Creative"` et `"Allow Cheats: ON"` — la position, la taille, le
texte et l'état de vanilla. Le seul écart visible sur la capture est le
bouton Difficulté, grisé et sans valeur chez nous (refusé, `ecrans.md` § 5),
« Difficulty: Normal » et actif chez vanilla.

## 2. level.dat — ce que vanilla écrit

Les deux mondes créés par le vrai client, relus après « Sauvegarder et
quitter » (`scripts/anvil_read.py`) :

| monde | choix à l'écran | `GameType` | `allowCommands` | `hardcore` |
|---|---|---|---|---|
| AC Off | Survie, commandes NON | 0 | **0** (octet) | 0 |
| AC Creative | Créatif, commandes non touchées | 1 | **1** (octet) | 0 |

`Data.allowCommands` est un octet. Nous l'écrivons tel que choisi
(`LevelSettings::allow_commands`) et le relisons ; un monde vanilla se relit
avec sa valeur.

## 3. Une clé absente

Mesuré par la liste des mondes du vrai client (§ 5), sur les deux `level.dat`
de vanilla recopiés **sans la clé** (retirée à l'octet, § 7) :

| monde planté | `GameType` | clé | `hasCheats` lu par le client |
|---|---|---|---|
| NK Creative | 1 | absente | **true** |
| NK Off | 0 | absente | **false** |

Une clé absente vaut donc « commandes en Créatif seulement ».
`read_level_settings` applique cette règle (test `a level.dat without
allowCommands`).

## 4. L'effet en jeu — ce que le vrai client reçoit

Le niveau que le client tient de l'Entity Event 24 + niveau, lu dans
`LocalPlayer.permissionLevel` :

| monde | niveau de l'hôte |
|---|---|
| AC Off (sans commandes) | **0** |
| AC Creative (commandes, non touchées) | **4** |
| AC Off, après « Ouvrir au réseau local » avec « Autoriser les commandes : OUI » | **4** |

**Chez nous, le même parcours** — deux mondes superplats créés par nos menus
(`ov_voxel --menu-press=singleplayer,…,create`), l'un non touché en Survie,
l'autre en Créatif, commandes non touchées :

| monde | `GameType` | `allowCommands` écrit | niveau reçu (journal du client) |
|---|---|---|---|
| Survie, non touché | 0 | **0** | **0** (Entity Event 24) |
| Créatif, non touché | 1 | **1** | **4** (Entity Event 28) |

Les mêmes valeurs que vanilla, fichier et niveau.

Au niveau 0, les réponses du serveur intégré vanilla, lues dans le chat du client :

- `/gamemode creative` → `command.unknown.command` puis
  `gamemode creative<--[HERE]` (`command.context.here`) ;
- `/time set day` → la même chose ;
- F3+N → `[Debug]: ` + `debug.creative_spectator.error` ;
- F3+F4 → `[Debug]: ` + `debug.gamemodes.error`.

Après l'ouverture au réseau local avec commandes : `commands.publish.started`
(« Local game hosted on port [25641] »), niveau 4, et `/gamemode creative`
répond `commands.gamemode.success.self`. Au niveau 4 dans le monde créatif,
`/time set day` répond `commands.time.set`.

### Ce qui est construit

| pièce | où | rôle |
|---|---|---|
| `join_permission` | `ov_server/commands/service.{hpp,cpp}` | dédié : `ops.json` ou 0 ; intégré : l'hôte a 4 si `allowCommands`, 0 sinon ; tout autre joueur 0 |
| `ServiceConfig::host_player` | idem, `server.cpp` (bloc `// ── allow-commands ──`) | l'hôte, depuis `--host-player` |
| `allow_commands_` | `CommandService::load_world` | lu de `level.dat` |
| l'Entity Event 24..28 | `ov_netclient` (`ClientEvents::op_level`) | le client garde le niveau que le serveur lui donne |
| l'onglet opérateur | `Interface::refresh_operator_tab`, `CreativeScreen::set_operator_tab` | option « Onglet des objets d'opérateur » **et** créatif **et** niveau ≥ 2 |
| l'option | `GameOptions::operator_items_tab` (`operatorItemsTab`), bouton de Contrôles | lue et écrite dans `options.txt` |

Le refus d'une commande trop haute était déjà celui de vanilla : l'arbre
élagué par niveau fait que la commande **n'existe pas** pour ce joueur, d'où
`command.unknown.command` (test `singleplayer: Allow Cheats is the host's
level`). La complétion suit le même arbre.

Les admins : un message « [X: …] » en italique gris ne va, en solo, qu'à un
joueur de niveau > 0 — l'hôte avec commandes.

La règle de l'onglet opérateur (option + créatif + niveau 2) vient de
l'article *Creative inventory* du Minecraft Wiki ; elle n'a pas été mesurée
sur le vrai client.

## 5. La liste des mondes et « Modifier le monde »

Parcours `allow-commands-list` : quatre mondes plantés (les deux de vanilla,
et les deux mêmes sans la clé). La troisième ligne de chaque rangée, lue dans
`LevelSummary.getInfo()` :

| monde | `hasCheats` | ligne |
|---|---|---|
| AC Creative | true | `gameMode.creative` + `, ` + `selectWorld.cheats` + `, ` + `selectWorld.version` + ` 1.20.1` → « Creative Mode, Cheats, Version: 1.20.1 » |
| AC Off | false | `gameMode.survival` + `, ` + `selectWorld.version` + ` 1.20.1` → « Survival Mode, Version: 1.20.1 » |

Notre liste écrit la même ligne : le mode, « , Commandes » quand le monde les
autorise, « , Version : » et `Data.Version.Name`. En français : « Mode Survie,
Commandes, Version : 1.20.1 ».

**« Modifier le monde » ne touche pas aux commandes** chez vanilla : l'écran
n'a que Nom du monde, Réinitialiser l'icône, Ouvrir le dossier, Faire une
sauvegarde, Ouvrir le dossier des sauvegardes, Optimiser, Sauvegarder,
Annuler (`ac-07-edit-world.png`). La seule façon de les changer en cours de
partie est « Ouvrir au réseau local » (§ 4). Chez nous, « Modifier » reste
grisé.

⚠️ Pas mesuré : la ligne d'un monde **sans** `Version.Name` ; la nôtre omet
alors la partie « Version ».

## 6. Ce qui n'est pas fait, nommé

- **Hardcore** n'est pas dans le cycle du mode de jeu chez nous : notre
  serveur n'a pas de hardcore. La règle « Hardcore ⇒ NON, grisé » est dans
  `AllowCheats` et testée, mais l'écran ne l'atteint jamais.
- **« Ouvrir au réseau local »** n'existe pas chez nous (bouton grisé du menu
  pause, `ecrans.md` § 5) : ni l'écran, ni son « Autoriser les commandes », ni
  le passage de l'hôte au niveau 4 en cours de partie. **`/publish`** n'est pas
  une commande de notre serveur.
- **F3+N et F3+F4** ne sont pas implémentés dans notre client : ni l'action,
  ni les messages `debug.creative_spectator.error` / `debug.gamemodes.error`.
- **L'infobulle** du bouton (`selectWorld.allowCommands.info`) : nos widgets
  n'ont pas d'infobulle.
- **Modifier le monde** reste refusé (grisé) chez nous.

## 7. Reproduire

```bash
# le vrai client : l'écran, deux mondes, le niveau, le chat, le LAN
lockf /tmp/ov-vanilla.lock python3 scripts/measure_screens.py --walk allow-commands \
    --cache .scratch/screens
#   → .scratch/screens/client-faithful-allow-commands/oracle/{facts.txt,level-*.dat}
# la liste des mondes, avec des level.dat plantés (sous-dossiers de DIR)
lockf /tmp/ov-vanilla.lock python3 scripts/measure_screens.py --walk allow-commands-list \
    --plant DIR --cache .scratch/screens
```

Une clé retirée d'un `level.dat` se retire **à l'octet** dans le NBT
décompressé : l'étiquette `01 00 0D "allowCommands"` et son octet de valeur,
17 octets, rien d'autre ne bouge ; le nom du monde se remplace par un nom de
même longueur.

⚠️ Piège : `--cache` relatif perdait les classes de l'oracle (le client tourne
dans son dossier de jeu) ; le script le rend absolu.
