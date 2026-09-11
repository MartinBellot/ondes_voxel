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

Le parcours **écrit** dans l'oracle : titre → Options (Vidéo, Musique et
sons, Contrôles, Touches, Souris, Langue) → Solo → Créer un monde (trois
onglets, un nom, commandes activées, la graine 1234567890) → dans le monde :
F3, le menu pause, les options en jeu, une mort par `/kill`, Réapparaître →
`options.txt` écrit par le jeu avec des valeurs connues → Sauvegarder et
quitter → la liste des mondes → Multijoueur → Connexion directe.

⚠️ **Le parcours n'a été fait qu'en partie.** Le premier passage a mesuré
l'écran titre et Créer un monde, et s'est arrêté (ci-dessous) ; le second,
corrigé, a attendu son tour sur le verrou partagé des JVM (une douzaine de
travaux d'autres agents devant lui) et a été retiré à la fin de ce travail
**sans avoir tourné**. Tout le reste — options, pause, mort, F3, liste des
mondes, connexion directe — **n'est pas mesuré contre le vrai client** (§ 5).

Même conditions que nos captures : fenêtre 1280×720 (tampon 2560×1440 Retina),
échelle d'interface 3 (854×480 pixels d'interface), Faithful 32x chargé.

Rien n'est lu ni traduit du code de Mojang : les mappings officiels ne font que
**nommer** classes, champs et méthodes (CLAUDE.md § 1). Rien n'est commité :
tout va sous `data/vanilla/1.20.1/generated/screens/` (gitignoré) ; le monde
créé par l'oracle est effacé à la fin.

```bash
lockf /tmp/ov-vanilla.lock python3 scripts/measure_screens.py
#   → data/vanilla/1.20.1/generated/screens/client-faithful/{oracle/facts.txt,screenshots/}
#   → oracle/options-initial.txt (et options-changed.txt, qu'aucun passage n'a encore atteint)
```

### Ce que le premier passage a rendu, et pourquoi il s'est arrêté

Le premier passage a mesuré l'écran titre et les trois onglets de Créer un
monde, puis s'est arrêté sur deux pièges, corrigés dans l'oracle :

- **Le clic sur « Options... » n'a rien ouvert** : la capture « titre » du
  vrai client est l'écran de chargement rouge de Mojang. Le rechargement des
  ressources (Faithful 32x) garde son voile au-dessus de l'écran titre, et le
  voile avale les clics. L'oracle attend maintenant que `Minecraft.getOverlay()`
  soit nul avant de piloter.
- **Le clic sur « Créer le nouveau monde » ne rend pas la main** : il charge
  le monde *dans* le gestionnaire du clic, sur le fil du jeu, jusqu'à ce que la
  zone d'apparition soit prête — plus de 60 s sur cette machine chargée, la
  limite de l'oracle. Ce clic est maintenant posté sans être attendu.

### L'écran titre — identique

| widget | vrai client | notre client |
|---|---|---|
| Solo / Multijoueur / Realms | 327,168 · 327,192 · 327,216, 200×20 | **identique** |
| Langue (icône) | 303,252 20×20 | **identique** |
| Options… / Quitter | 327,252 et 429,252, 98×20 | **identique** |
| Accessibilité (icône) | 531,252 20×20 | **identique** |

La règle : `haut = h/4 + 48`, pas de 24, la rangée du bas à `haut + 84`.

### Créer un monde — mesuré, puis corrigé

La première disposition (écrite avant l'oracle) avait les bons boutons du bas
et tout le reste faux. Mesuré à 854×480 :

- onglets de **124** de large depuis `w/2 − 185` (242, 366, 490), hauts de 24 ;
- page Jeu : libellé « Nom du monde » à y 75, champ 208×20 à (323, 89),
  Mode de jeu / Difficulté / Commandes à y **118, 146, 174** (un pas de 28, pas
  de 24), 210 de large à `w/2 − 105` ;
- page Monde : Type de monde (272, 75) et Personnaliser (432, 75), 150 de
  large ; libellé de la graine à y 103, champ 308×20 à (273, 117) ; les deux
  bascules sont un libellé à gauche et un bouton **44×20** ON/OFF à x 538 ;
- page Plus : Règles / Expériences / Packs de données à y 82, 110, 138 ;
- Créer / Annuler à (272, 452) et (432, 452), 150×20.

Et dans les pixels de la capture : une barre noire derrière les onglets, le
séparateur d'en-tête sous elle, celui du pied à `h − 36` ; les libellés blancs,
alignés à gauche ; « Laisser vide pour une graine aléatoire » est le **texte
indicatif gris du champ vide**, pas une ligne sous lui ; le titre de l'onglet
choisi aux rangées 8–14 avec un soulignement de sa largeur à la rangée 22, les
autres **clairs aussi** et deux pixels plus bas (10–16).

Les onglets, vérifiés après correction par la même sonde de pixels sur les
deux captures : titre choisi aux rangées 8–14 et soulignement de 24 pixels à
la rangée 22, titres non choisis aux rangées 10–16 — **les mêmes rangées chez
les deux clients**.

Comparaison widget par widget, page Monde (`scripts/compare_screens.py`) :
boutons à **92–98 %** des pixels à ±8 niveaux ; les bascules ON/OFF à 5–7 %,
parce que les nôtres sont grisées (refusées, § 5) et celles de vanilla non.

---

## 2. options.txt

Le format est celui que le vrai client écrit, lu sur deux fichiers : celui
que l'oracle a fait écrire au jeu par son propre `Options.save()` au démarrage
(`options-initial.txt`, 137 lignes) et l'`options.txt` de l'instance
PrismLauncher de l'utilisateur. Le troisième prévu — le même fichier après des
valeurs connues (champ de vision 90, sensibilité 0,75…) — n'a pas été produit
(§ 1) : les encodages de ces valeurs-là (`fov:0.5`…) viennent de la règle et
du fichier de l'utilisateur, pas d'une écriture du jeu observée.

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

**Le chiffre.** Le fichier écrit par le vrai client est la fixture
`src/ov_client/tests/data/options_vanilla_1.20.1.txt` (des réglages, aucun
asset ni sortie du data generator). Relu par `OptionsFile` puis réécrit :
**identique octet pour octet** (137 lignes). Lu par `GameOptions` : **0
valeur refusée**, toutes les touches à leur défaut. Et `GameOptions`
réécrivant ses propres valeurs dedans laisse le fichier **identique** — nos
`0.5`, `1.0`, `key.keyboard.left.shift` sont ceux du jeu
(`test_options_file.cpp`).

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

Le chiffre, sur un monde superplat créé par les menus, en deux runs de
`ov_voxel` sur le même parcours et le même nombre d'images — l'un tient le
menu pause 600 images, son témoin les joue (commandes au § 6) :

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

## 3 ter. L'écran de mort — construit, **pas montré de bout en bout**

Construit : l'écran (titre ×2, cause, score, Réapparaître / Écran titre,
boutons inactifs la première seconde, fond rouge), ouvert par l'arrivée de
**Combat Death** (0x38) et fermé par **Respawn** (0x41) ; Réapparaître envoie
**Client Command** action 0. Les paquets sont lus et écrits par `netclient`.

Pas montré : sur trois runs scriptés (superplat, survie, `/kill`), le serveur
a tué le joueur et le client a reçu le message de mort dans le chat — mais le
journal du dernier run, instrumenté, **n'a jamais « Combat Death received »** :
l'écran ne s'est pas ouvert. Et chaque fois, le serveur a fermé la connexion
40 à 49 s après la mort (`the server closed the connection`, `Broken pipe`,
`Connection reset by peer`). Les deux faits sont nommés, pas expliqués :

- que notre serveur envoie Combat Death sur ce chemin (`survival_session.cpp`
  l'envoie) sans que le client le reçoive reste à trouver ;
- le serveur renvoie un keep-alive **à nouvel identifiant toutes les 10 s,
  même si le précédent attend sa réponse**, et ferme sur une réponse à
  l'identifiant périmé (`server.cpp`) : tout retard de plus de 10 s côté client
  devient une déconnexion. Vanilla, lui, ne remplace pas un défi en attente.
  Dans ces runs le client a traité les paquets du serveur **25 à 40 s en
  retard** après le `/kill`. Hypothèse, pas mesure.

La disposition de l'écran (Réapparaître à `h/4 + 72`, Écran titre à
`h/4 + 96`) n'a **pas** été mesurée : le second passage de l'oracle, qui devait
atteindre la mort, n'a pas eu son tour sur le verrou partagé.

---

## 3 quater. Un monde par graine, de bout en bout

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
en monde (pause, F3) sont donc prises sur un monde **superplat** créé par les
mêmes menus ; l'entrée dans un monde par graine reste à montrer quand la
machine le permet.

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
- **Le contour du bloc visé disparaît au bout d'un moment — défaut antérieur,
  nommé, pas corrigé ici.** `Overlay` n'avance son anneau et ne remet
  `used_[ring_]` à zéro qu'à la fin de `draw_crosshair` (`overlay.cpp`), et
  `main.cpp` n'appelle `draw_crosshair` que HUD masqué. HUD affiché — le cas
  normal — le compte de sommets de l'image ne revient jamais à zéro : une fois
  le tampon plein, le contour n'est plus dessiné et « overlay line buffer full
  this frame » s'écrit à chaque image (314 et 540 fois dans les deux runs de
  mesure de la pause, **sans mort**, 0 dans les runs courts). Hors du mandat
  des écrans ; la correction est d'avancer l'anneau à chaque image.
- **zsh prend `[level_dat]` pour un motif de fichiers.** Un filtre Catch2 se
  cite (`'[level_dat]'`), et seul sur sa ligne (piège 27 du briefing).

---

## 5. Ce qui n'est pas fait, nommé

Refusé à l'écran — le bouton est là, grisé, et ne fait rien — plutôt que
simulé :

- **Realms**, **Accessibilité** (titre et options), **Progrès**,
  **Statistiques**, **Donner son avis**, **Signaler des bugs**, **Ouvrir au
  LAN**, **Signalement de joueurs** ;
- dans les options : **Personnalisation du skin**, **En ligne**, **Chat**,
  **Packs de ressources**, **Télémétrie**, **Crédits**, **Difficulté** (et son
  verrou) — la difficulté se change par `/difficulty` ;
- dans Vidéo : tout sauf Distance de rendu, Distance de simulation, Images
  par seconde max, Synchro verticale, Échelle de l'interface et Luminosité ;
- dans Musique et sons : Appareil, Sous-titres, Audio directionnel ;
- dans Contrôles : Accroupissement/Course (maintien ou bascule), Saut
  automatique ; dans Souris : tout sauf la Sensibilité (l'Onglet des objets
  d'opérateur est fait : `commandes-solo.md`) ;
- dans Créer un monde : **Hardcore** (le serveur n'a pas de hardcore : le
  mode de jeu alterne Survie / Créatif), Difficulté, Personnaliser,
  Structures, Coffre bonus, Règles, Expériences, Packs de données.
  **Autoriser les commandes** est fait, mesuré sur le vrai client :
  `commandes-solo.md` ;
- dans la liste des mondes : Modifier, Supprimer, Recréer ; pas d'icône de
  monde (un carré sombre à sa place).

Pas fait du tout :

- **La liste des serveurs** : Multijoueur ouvre directement la Connexion
  directe.
- **La confirmation « Êtes-vous sûr de vouloir quitter ? »** du bouton Écran
  titre de l'écran de mort (non hardcore) : le bouton quitte sans demander.
- **Le son du clic** des boutons.
- **Le texte d'accueil jaune** (splash) du titre, et le logo « Minceraft ».
- **Le titre ne dit pas « Minecraft 1.20.1 » ni le copyright de Mojang** : il
  dit « Ondes VOXEL 1.20.1 » et « Not an official Minecraft product ». C'est
  voulu (CLAUDE.md § 1, NOTICE), pas un écart à corriger.
- **La synchro verticale** est relue au démarrage seulement : le RHI ne
  recrée pas la chaîne d'échange à la volée.
- **La distance de rendu** change le brouillard tout de suite, mais le
  serveur ne l'apprend qu'à la connexion suivante (Client Information n'est
  envoyé qu'avec la connexion).
- **La langue** s'applique aux menus tout de suite, à l'interface en jeu au
  monde suivant.
- **Une touche rebindée vers un bouton de souris** est refusée (attaque et
  utilisation restent sur leurs boutons).
- **Le F3** n'a que les lignes dont ce client a la valeur ; la ligne mémoire
  donne la taille résidente du processus et la mémoire de la machine, pas le
  tas d'une JVM.

**Pas mesuré contre le vrai client** (le second passage de l'oracle n'a pas
tourné, § 1) — dispositions écrites d'après la structure connue de vanilla,
à confirmer par `scripts/measure_screens.py` puis `scripts/compare_screens.py` :

- **Options** (et en jeu), **Vidéo**, **Musique et sons**, **Contrôles**,
  **Touches**, **Souris**, **Langue** ;
- **le menu pause** (grille de deux colonnes de 98, Retour au jeu et
  Sauvegarder et quitter en 204, titre à y 40) ;
- **l'écran de mort** (§ 3 ter) ;
- **les lignes et la mise en page du F3** ;
- **la liste des mondes** (titre à y 8, champ de recherche à y 22, rangées de
  36) et **la connexion directe**.

Les deux seuls écrans mesurés sont **l'écran titre** (identique) et **Créer un
monde** (corrigé sur la mesure).

---

## 6. Reproduire

```bash
# l'oracle : le vrai client, sur son écran titre (un seul JVM sur la machine)
lockf /tmp/ov-vanilla.lock python3 scripts/measure_screens.py
#   → data/vanilla/1.20.1/generated/screens/client-faithful/{oracle/facts.txt,screenshots/}

# notre client, sans option : le menu principal
build/macos-debug/bin/ov_voxel

# nos captures, scriptées par le chemin d'un clic (1280×720, échelle 3)
ov_voxel --width=1280 --height=720 --gui-scale=3 --frames=90 --menu=options --menu-at=5 \
    --dump-menu --screenshot=run/screens/options.ppm
ov_voxel … --saves=run/saves --frames=0 --menu-at=10 \
    --menu-press=singleplayer,+10,tab1,type:1234567890,create,?game,+200,@pause,+60,end
#   pas du script : `id` appuie, `@écran` ouvre, `+N` attend N images,
#   `?écran` attend cet écran (`?game` : dans le monde), `type:texte`,
#   `world:N` choisit un monde de la liste, `end` termine et capture.

# la comparaison widget par widget
scripts/compare_screens.py --facts <facts.txt> --label "create, world tab" <nôtre.png> <vanilla.png>

# la pause du serveur intégré (§ 3 bis) : le même parcours deux fois, sur un
# monde superplat déjà créé dans <saves>, et les lignes « stopped after N
# ticks » / « pause: … s paused » du journal
ov_voxel --width=1280 --height=720 --gui-scale=3 --frame-ms=16 --no-sound --saves=<saves> \
    --frames=0 --menu-at=10 \
    --menu-press=singleplayer,+20,world:0,+10,play,?game,+300,@pause,+600,end
ov_voxel … --menu-press=singleplayer,+20,world:0,+10,play,?game,+300,+600,end   # témoin
```

Options du client ajoutées : `--saves=`, `--options=`, `--menu=`, `--menu-at=`,
`--menu-press=`, `--menu-type=`, `--dump-menu`, `--f3`. Côté serveur : `--seed=`.
