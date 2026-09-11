# Les écrans de notre client — menu principal, solo, options, pause, mort, F3

## Le problème

Le client démarrait directement dans un monde (`--singleplayer`, `--connect`)
et Échap ne faisait que libérer la souris. Il n'y avait ni écran titre, ni
liste de mondes, ni création de monde par graine, ni options, ni pause, ni
écran de mort, ni F3. Ce document dit ce qui a été construit, **d'où viennent
ses nombres** — du vrai client 1.20.1 en fonctionnement — et ce qui ne l'est
pas.

---

## 1. L'oracle : le vrai client, démarré sur son écran titre

`scripts/measure_screens.py` lance l'instance PrismLauncher 1.20.1 de
l'utilisateur **sans serveur** : le client démarre sur son écran titre, et
c'est lui qui crée le monde (serveur intégré compris). `scripts/screens_oracle.java`
le pilote par les gestionnaires d'entrée du jeu (`KeyboardHandler.keyPress`,
`MouseHandler.onMove/onPress`), comme les oracles de l'inventaire créatif et du
chat, après avoir débranché les vrais périphériques.

Sur chaque écran atteint, le jeu donne **chaque widget** : classe, position,
taille, actif ou non, texte affiché, et surtout **la clé de traduction et ses
arguments** (`options.generic_value(options.fov, options.fov.min)`), lue dans
le composant du bouton. Les listes (monde, options) donnent leur boîte et le
haut de chaque rangée. Puis une capture par le chemin de capture du jeu.

Le parcours : titre → Options (Vidéo, Musique et sons, Contrôles, Touches,
Souris, Langue) → Solo → Créer un monde (trois onglets, un nom, commandes
activées, la graine 1234567890) → dans le monde : F3, le menu pause (et si le
serveur intégré s'arrête vraiment), les options en jeu, une mort par `/kill`,
Réapparaître → `options.txt` écrit par le jeu avec des valeurs connues →
Sauvegarder et quitter → la liste des mondes → Multijoueur → Connexion directe.

Même conditions que nos captures : fenêtre 1280×720 (tampon 2560×1440 Retina),
échelle d'interface 3 (854×480 pixels d'interface), Faithful 32x chargé.

Rien n'est lu ni traduit du code de Mojang : les mappings officiels ne font que
**nommer** classes, champs et méthodes (CLAUDE.md § 1). Rien n'est commité :
tout va sous `data/vanilla/1.20.1/generated/screens/` (gitignoré) ; le monde
créé par l'oracle est effacé à la fin.

```bash
lockf /tmp/ov-vanilla.lock python3 scripts/measure_screens.py
#   → data/vanilla/1.20.1/generated/screens/client-faithful/{oracle/facts.txt,screenshots/}
#   → oracle/options-initial.txt, oracle/options-changed.txt
```

<!-- NUMBERS: filled from facts.txt -->

---

## 2. options.txt

Le format est celui que le vrai client écrit, lu sur trois fichiers : les
deux que l'oracle fait écrire au jeu (au démarrage, puis après avoir reçu des
valeurs connues par ses propres objets d'option) et l'`options.txt` de
l'instance PrismLauncher de l'utilisateur.

- une ligne `clé:valeur` par option, coupée au **premier** deux-points
  (`lastServer:` a une valeur vide ; `resourcePacks:["vanilla","file/…"]`
  contient des deux-points) ;
- les nombres réels écrits comme Java les écrit (`Double.toString`) : `0.0`,
  `1.0`, `0.5`, `0.4375`, les chiffres les plus courts qui relisent le même
  double, et la notation `1.0E-4` sous 10⁻³ ;
- certains textes sont entre guillemets (`renderClouds:"true"`,
  `mainHand:"right"`, `soundDevice:""`), d'autres non (`lang:fr_fr`) ;
- **le champ de vision n'est pas stocké en degrés** : `fov:0.0` est 70°,
  `fov:0.5` est 90°, la valeur est `(fov − 70) / 40` ;
- les touches par leur **nom vanilla**, pas un code : `key_key.forward:key.keyboard.w`,
  `key_key.attack:key.mouse.left`, `key_key.smoothCamera:key.keyboard.unknown`.
  Le fichier de l'utilisateur a `key_key.sneak:key.keyboard.q` et
  `key_key.drop:key.keyboard.z` : il a rebindé, et notre client lit ces lignes ;
- les volumes par catégorie, `soundCategory_master` … `soundCategory_voice`,
  dans l'ordre même des catégories d'`ov_audio`.

**Aucune ligne n'est perdue.** `OptionsFile` garde toutes les lignes dans leur
ordre, connues ou non ; `GameOptions` ne lit et n'écrit que les siennes. Un
`options.txt` copié d'un dossier de jeu vanilla est réécrit avec ses ~130
lignes, celles que ce client ignore comprises. Une valeur illisible
(`renderDistance:far`) garde la valeur par défaut et **est nommée** dans le
journal, jamais changée en zéro.

Le fichier du client est `run/options.txt` (`--options=` pour un autre). Il
remplit ce que la ligne de commande n'a pas dit : `--lang`, `--gui-scale`,
`--radius`, `--no-vsync` et `--volume` gagnent toujours.

<!-- round trip numbers -->

---

## 3. Ce qui est construit

| Pièce | Où | Rôle |
|---|---|---|
| `OptionsFile`, `GameOptions`, `java_double`, noms de touches | `ov_client/options_file.{hpp,cpp}` | le fichier vanilla, ligne pour ligne |
| `Widget`, bouton, curseur, champ, terre | `ov_client/menu_widgets.{hpp,cpp}` | dessinés depuis `widgets.png`, `slider.png`, `options_background.png` |
| dispositions | `ov_client/menu_layouts.{hpp,cpp}` | chaque écran en fonction de la taille d'interface |
| F3 | `ov_client/debug_overlay.{hpp,cpp}` | les lignes, sans appareil |
| l'automate des menus | `apps/ov_voxel/src/menus.{hpp,cpp}` | titre, mondes, création, connexion, options, pause, mort |
| câblage | `main.cpp` (blocs `// ── screens ──`) | rejoindre, quitter, mettre en pause, réapparaître |
| pause du serveur intégré | `ov::server::run(…, external_pause)` | aucun tick derrière le menu pause |
| graine persistée | `world::LevelSettings::generated` | `level.dat` déclare `minecraft:noise` et la graine |
| mort, réapparition | `netclient` : Combat Death (0x38), Respawn (0x41), Client Command (0x07) | l'écran de mort et son bouton |

---

## 3 bis. La pause du serveur intégré, mesurée

Vanilla arrête le serveur intégré derrière le menu pause d'une partie solo.
Le nôtre aussi : `ov::server::run` prend un `external_pause`, et tant qu'il
vaut vrai la boucle ne fait aucun tick (et remet son horloge à zéro, pour ne
pas rattraper la pause ensuite). Le client le lève quand un menu est ouvert
sur un monde hébergé — sauf l'écran de mort, qui en vanilla non plus n'est
pas un écran de pause — et cesse lui-même de simuler.

Le chiffre, sur un monde superplat créé par les menus
(`.scratch/measure_pause.sh` et son témoin `measure_pause_control.sh`, même
parcours, même nombre d'images, sans le menu) :

| run | durée du serveur | ticks | ticks/s |
|---|---|---|---|
| témoin, sans pause | 16,9 s | 337 | **19,9** |
| avec pause (13,7 s en pause, 6,1 s en jeu, mesurées par le client) | 20,2 s | **128** | — |

Sans pause, le second run aurait fait ≈ 20,2 × 19,9 = **402** ticks. Arrêté
pendant exactement les 13,7 s de pause, il en aurait fait
(20,2 − 13,7) × 19,9 ≈ **129**. Mesuré : **128**.

⚠️ Un premier témoin — compter les paquets Update Time reçus en pause et en
jeu — rendait **0 dans les deux états**, en jeu compris : il ne mesurait rien
(piège 14 du briefing), et il a été retiré plutôt qu'imprimé.

---

## 3 ter. Un monde par graine, de bout en bout

Créé par les menus (`--menu-press=singleplayer,tab1,type:1234567890,create`),
dans `.scratch/saves/New World/`. Le `level.dat` écrit par le client, relu
octet par octet :

| champ | valeur |
|---|---|
| `LevelName` | `New World` (le nom par défaut, `selectWorld.newWorld`) |
| `GameType` | `0` (survie, le mode par défaut de l'écran) |
| `WorldGenSettings.seed` | `1234567890` |
| générateur de l'overworld | `minecraft:noise`, source de biomes `minecraft:multi_noise` |

Le serveur intégré l'a relu et a construit la pile de génération à cette
graine (`worldgen: 15 router entries … seed 1234567890`), puis a mis le
client en attente avec `menu.preparingSpawn`. La liste des mondes le montre
ensuite : « New World », son dossier, sa date, « Survival Mode ».

⚠️ **Pas encore mesuré : le premier chunk de ce monde.** Dans le build Debug,
sur une machine à charge 25 pour 8 cœurs, le serveur a tourné 6 973 ticks
(≈ 6 min) sans achever **un seul** bloc de génération (`chunk source: 0 blocks
generated`), avant que le processus soit arrêté de l'extérieur. Les captures
en monde (pause, F3, mort) sont donc prises sur un monde **superplat** créé
par les mêmes menus ; l'entrée dans un monde par graine reste à montrer quand
la machine le permet.

---

## 4. Pièges payés

- **64 liaisons de texture par image, pour tout le client.** Le RHI prend un
  jeu de descripteurs par `bind_textures` dans une réserve remise à zéro à
  chaque image, de 64 jeux (`ov_rhi/src/device.cpp`, `maxSets = 64`). Le GUI
  coupe un tirage à chaque changement de texture. Le premier écran des touches
  dessinait chaque widget en entier — feuille, puis page de police, puis
  feuille… — soit ~90 tirages : **850 avertissements « descriptor pool
  exhausted »** dans le journal de la capture, et les derniers widgets
  dessinés avec la mauvaise image (des glyphes de la police sur les boutons
  Réinitialiser). Tous les fonds d'abord, tous les textes ensuite : 0
  avertissement. ⚠️ La réserve est partagée par tout ce qui dessine dans
  l'image (HUD, inventaires, menus) : un écran qui alterne ses textures la
  vide pour les autres.
- **Une machine saturée ressemble à un serveur bloqué.** Un monde créé par
  graine est resté à « Préparation de la zone d'apparition : 0 % » plusieurs
  minutes (charge 25 sur 8 cœurs, 1,86 million de pages évincées). Un
  échantillon du processus (`sample`) montrait les quatre ouvriers dans
  `generate_biomes` / `generate_noise` et le fil du tick dans son attente
  normale : lent, pas bloqué. Regarder les piles avant de chercher un bogue.
- **zsh prend `[level_dat]` pour un motif de fichiers.** Un filtre Catch2 se
  cite (`'[level_dat]'`), et seul sur sa ligne (piège 27 du briefing).

---

## 5. Ce qui n'est pas fait, nommé

<!-- list -->
