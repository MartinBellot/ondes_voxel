# L'inventaire créatif — les onglets, leur contenu, leur géométrie, et le vrai client

## 0. « Notre client ne possède pas d'inventaire créatif » — la cause

Le code existait, et le joueur arrive bien en créatif : en `--singleplayer`, le
serveur intégré démarre sans `--survival`, donc `Login (play)` porte
`game_mode = 1`, et la capture du parcours de l'utilisateur (binaire de `main`,
monde neuf, aucune option) montre la barre de vie masquée et la pierre de
`--hold` dans la main.

**La cause exacte : `data/vanilla/1.20.1/creative_tabs.json` n'existait pas
dans le dépôt de l'utilisateur.** Ce fichier est gitignoré (il dérive du jar),
et **rien ne le produisait** : ni `bootstrap.sh`, ni `setup_vanilla.sh`, ni la
construction. L'agent qui l'avait écrit l'avait généré *dans un worktree git*
— `data/vanilla/1.20.1/` y est un vrai répertoire, seuls quelques fichiers
listés y sont des liens vers le dépôt principal — et le fichier a disparu avec
le worktree. Le cache de l'oracle, lui, passait par le lien symbolique
`generated/` et est resté : c'est ce qui rendait la chose trompeuse.

Sans ce fichier, le client refusait l'écran avec **un avertissement dans le
journal**, et la touche E, en créatif, **ouvrait silencieusement l'inventaire
de survie**. Le journal du parcours reproduit :

```
[WARN] creative catalogue data/vanilla/1.20.1/creative_tabs.json: not found.
       The creative inventory is refused; generate it with scripts/measure_creative_tabs.py.
[WARN] no creative catalogue loaded; the screen is refused
```

Il ne manque ni option de monde, ni mode par défaut, ni `level.dat` : le mode
était le bon. Un monde existant ne change rien (l'écran est côté client).

**Corrigé à trois niveaux :**

1. le fichier est régénéré dans le dépôt de l'utilisateur ;
2. `scripts/setup_vanilla.sh` le produit désormais lui-même (`measure_creative_tabs.py
   --check`) chaque fois qu'il manque — c'est le script que tout dépôt passe ;
3. le refus est **dit à l'écran** : E en créatif sans catalogue affiche, à la
   place du nom de l'objet tenu, `No creative inventory: … is missing
   (scripts/setup_vanilla.sh)`, puis ouvre l'inventaire de survie comme avant.
   Un refus que seul le journal connaît est ce qui a produit ce rapport.

⚠️ **Piège pour les autres agents** : tout fichier gitignoré sous
`data/vanilla/1.20.1/` écrit depuis un worktree *meurt avec lui* s'il n'est
pas dans la liste de liens du briefing. `creative_tabs.json`,
`creative_items.json` et `entity_models.json` n'y étaient pas ;
`entity_models.json` est d'ailleurs lui aussi absent du dépôt principal.

---

## 1. Le contenu : il n'y a pas d'oracle réseau, mais il y a un oracle

En 1.20.1 les onglets créatifs sont construits par `CreativeModeTabs`. Le client
vanilla ne les reçoit **jamais** du serveur : il les fabrique lui-même. Il n'y a
donc rien à capturer sur le fil, et l'ordre d'un onglet ne se déduit pas du
registre d'items (qui est dans l'ordre des ids réseau).

`PrismarineJS/minecraft-data` (MIT, autorisé) ne porte pas les onglets
créatifs. `CreativeModeTabs` est aussi dans le **jar serveur** : on le *fait
tourner* (`scripts/creative_tabs_oracle.java`, piloté par
`scripts/measure_creative_tabs.py`). Les mappings officiels n'y servent qu'à
**nommer** (CLAUDE.md § 1) ; chaque fait imprimé sort du bytecode du jeu.

La sortie va dans `data/vanilla/1.20.1/creative_tabs.json`, gitignoré. Seul le
SHA-256 est commité :

```
sha256 c7fb0e048ba3dc2c429d7d788a63df66f2de7862824350c38771ef3c3ddb26b1
       147 520 octets, 14 onglets, 1688 cases de catégorie
```

⚠️ **Ce hash a changé dans ce travail, et c'était une erreur mesurée.** Le
premier oracle appelait `tryRebuildTabContents` avec `allFlags()` — toutes les
fonctionnalités, y compris l'expérimentale « bundle ». Le catalogue portait
donc un **Sac** (onglet Outils) que le vrai client 1.20.1 ne montre pas. C'est
l'oracle client (§ 8) qui l'a trouvé : son onglet de recherche n'avait pas le
Sac. L'oracle serveur utilise maintenant `FeatureFlags.DEFAULT_FLAGS`, les
drapeaux d'un monde vanilla : 1688 cases au lieu de 1689, 1586 en recherche au
lieu de 1587. L'ancien hash était `e53d702c…`.

| onglet | rangée | col | type | icône | cases |
|---|---|---|---|---|---|
| `building_blocks` | haut | 0 | catégorie | `bricks` | 348 |
| `colored_blocks` | haut | 1 | catégorie | `cyan_wool` | 198 |
| `natural_blocks` | haut | 2 | catégorie | `grass_block` | 226 |
| `functional_blocks` | haut | 3 | catégorie | `oak_sign` | 196 |
| `redstone_blocks` | haut | 4 | catégorie | `redstone` | 62 |
| `hotbar` | haut | 5 (à droite) | barres sauvegardées | `bookshelf` | — |
| `search` | haut | 6 (à droite) | recherche | `compass` | 1586 |
| `tools_and_utilities` | bas | 0 | catégorie | `diamond_pickaxe` | 109 |
| `combat` | bas | 1 | catégorie | `netherite_sword` | 97 |
| `food_and_drinks` | bas | 2 | catégorie | `golden_apple` | 176 |
| `ingredients` | bas | 3 | catégorie | `iron_ingot` | 171 |
| `spawn_eggs` | bas | 4 | catégorie | `pig_spawn_egg` | 76 |
| `op_blocks` | bas | 5 | catégorie | `command_block` | 29 |
| `inventory` | bas | 6 (à droite) | inventaire de survie | `chest` | — |

Le piège des tags non liés (une peinture au lieu de 27, aucune corne de
chèvre) et son témoin absurde (`--no-op` vide l'onglet opérateur) sont décrits
dans l'historique git de ce document ; ils tiennent toujours.

### L'onglet opérateur est caché par défaut — mesuré

Le vrai client, **joueur non opérateur et réglage « Operator Items Tab » à sa
valeur par défaut**, ne montre **pas** l'onglet `op_blocks` : sa rangée du bas
n'a pas de colonne 5. Et sa recherche compte **1557** piles, pas 1586 : les 29
de l'onglet opérateur (blocs de commande, structure, barrière, lumière ×16,
bâton de débogage, les 4 peintures non posables) sont absentes **partout**.
Notre écran fait maintenant de même ; `--operator-tab` rétablit l'onglet.
Après ce filtre, **notre page de recherche est identique à celle du client, pile
pour pile et dans l'ordre (1557/1557)**.

---

## 2. La géométrie : comptée dans les pixels, puis lue sur le vrai client

Le premier passage comptait les carrés d'emplacement dans les textures
(`measure_creative_tabs.py --geometry`, Faithful et jar donnent les mêmes
nombres). Ce passage-ci **demande au client en marche** où il met chaque chose
(§ 8). Les deux concordent partout sauf sur deux valeurs *dérivées* du premier
passage, qui étaient fausses.

### Ce que le client en marche rapporte (2560×1440, échelle 3)

```
gui 854x480 scale 3.0
panel left 329 top 172 size 195x136 title at 8,6
tab building_blocks     x 0   y -32      tab tools_and_utilities x 0   y 136
tab colored_blocks      x 27  y -32      tab combat              x 27  y 136
tab natural_blocks      x 54  y -32      tab food_and_drinks     x 54  y 136
tab functional_blocks   x 81  y -32      tab ingredients         x 81  y 136
tab redstone_blocks     x 108 y -32      tab spawn_eggs          x 108 y 136
tab hotbar  (right)     x 142 y -32      tab inventory (right)   x 169 y 136
tab search  (right)     x 169 y -32
searchBox 411,178 80x9                   (82,6 dans le panneau)
slots 0..44 at (9+18c, 18+18r)           hotbar 45..53 at (9+18c, 112)
destroySlot 173,112
```

* **Le pas des onglets est 27, pas 28.** La valeur 28 avait été *dérivée* (sept
  boutons sur 195 pixels) et était fausse d'un pixel par colonne. Et un onglet
  « aligné à droite » — Barres sauvegardées, Recherche, Inventaire de survie —
  se place depuis le bord droit : `195 − 27 × (7 − colonne) + 1`. Les Barres
  sauvegardées sont donc à **142**, avec un vide de sept pixels après Blocs de
  redstone, que les captures montrent.
* **Zone de clic** : 26×32 à `getTabY`, soit −32..0 au-dessus du panneau et
  136..168 au-dessous — pas les quatre pixels où le bouton chevauche le
  panneau.
* **Dessin** : lu dans les pixels de la capture vanilla — bouton du haut blité à
  y = −28, du bas à 132 ; icône à +5 en x, **+9** sous le bord du bouton du haut
  (l'ancien code mettait +6, trois pixels trop haut).
* **La taille d'interface est entière, arrondie vers le haut** : 2560 / 3 donne
  **854**, pas 853,33. `Gui::begin` suit maintenant cette règle ; l'origine du
  panneau, `(854 − 195) / 2 = 329`, tombait juste par chance avec le flottant.

### La page de survie, emplacement par emplacement

Le premier passage ne savait pas lequel des quatre carrés d'armure était le
casque, et avait laissé l'armure non cliquable plutôt que de supposer. Le
client en marche le dit, par la case de conteneur que chaque emplacement
enveloppe :

| position | emplacement de l'inventaire | fenêtre 0 |
|---|---|---|
| (54, 6) | 39 | 5 — tête |
| (54, 33) | 38 | 6 — torse |
| (108, 6) | 37 | 7 — jambes |
| (108, 33) | 36 | 8 — pieds |
| (35, 20) | 40 | 45 — main secondaire |
| (9+18c, 54/72/90) | 9..35 | 9..35 |
| (9+18c, 112) | 0..8 | 36..44 |
| (173, 112) | — | destruction |

Les cinq sont maintenant cliquables, et vides ils montrent leur silhouette
(`item/empty_armor_slot_*`, ajoutées à l'atlas par nom : aucun modèle ne les
cite). Un emplacement d'armure **refuse** ce qui ne s'y porte pas : le client a
donné, pour chacune des 1557 piles, l'emplacement où il la porterait
(`LivingEntity.getEquipmentSlotForItem`) — 15 en tête (casques, citrouille
sculptée, crânes et têtes), 7 au torse (élytres compris), 6 aux jambes, 6 aux
pieds.

---

## 3. Le titre, le champ de recherche, les barres sauvegardées

* **Le titre** est dessiné sur **toutes** les pages sauf celle de survie — y
  compris la recherche (« Search Items », à gauche du champ) et les barres
  sauvegardées. L'ancien code l'omettait sur la recherche.
* **Le champ** n'a pas de bordure : son texte est à (82, 6), blanc, avec ombre ;
  au bout, un curseur `_` qui clignote 300 ms allumé, 300 ms éteint ; 50
  caractères au plus. Pas de texte indicatif : la capture vanilla d'un champ vide
  ne montre que le curseur. L'ancien code écrivait à (86, 10).
* **Les barres sauvegardées** : neuf rangées de neuf (défilement de 4). Une
  rangée vide montre, **sur la diagonale** (rangée r, colonne r), un papier dont
  l'infobulle est `inventory.hotbarInfo` — « Save hotbar with C+1 » — en
  italique blanc. ⚠️ Le client vanilla écrit la touche **comme la disposition du
  clavier l'imprime** : sur l'AZERTY de cette machine, il a écrit `C+&`,
  `C+É`… Notre client prend le même nom à GLFW (`glfwGetKeyName`).
* **C + chiffre** enregistre la barre d'action dans la rangée, **X + chiffre** la
  recharge (en créatif, hors de tout écran). Le fichier est `run/hotbar.nbt`, **au
  format de vanilla** : NBT non compressé, `DataVersion` 3465 et une liste de
  neuf `{id, Count, tag}` par rangée `"0"`…`"8"`, la case vide valant
  `{id:"minecraft:air", Count:0b}`. Un `hotbar.nbt` copié d'un dossier de jeu
  vanilla se lit ici, et inversement. Le message de chat « Saved toolbar… » n'a
  pas de chat où s'afficher : il s'affiche au-dessus de la barre d'action.

---

## 8. L'oracle client : faire tourner le vrai 1.20.1, et le piloter

`scripts/measure_creative_screen.py` lance **l'instance PrismLauncher 1.20.1 de
l'utilisateur** (classpath reconstruit depuis les métadonnées de Prism,
bibliothèques natives arm64, Java 17), la connecte (`--quickPlayMultiplayer`) à
un serveur vanilla créatif sur un monde plat neuf, avec Faithful 32x chargé
comme notre client, l'échelle 3 et la fenêtre 1280×720. Puis
`scripts/creative_screen_oracle.java` :

* **pilote le jeu par ses propres gestionnaires d'entrée** :
  `KeyboardHandler.keyPress`, `charTyped`, `MouseHandler.onMove`, `onPress`,
  `onScroll` — les méthodes que les callbacks GLFW appellent. Un clic qui choisit
  un onglet le choisit donc pour la raison qu'un vrai clic le ferait ;
* **débranche les vrais périphériques** d'abord (les callbacks GLFW sont mis à
  nul) : la fenêtre est sur le bureau de l'utilisateur, et la première capture
  montrait une infobulle sous le *vrai* pointeur ;
* **demande les nombres au jeu** après chaque geste (§ 2) et **prend les
  captures par son propre chemin** (`Screenshot.grab`) ;
* **vide le catalogue** : pour chacune des 1557 piles de la recherche,
  l'infobulle exacte en composants (sur la page de recherche), les teintes de
  chaque couche (`ItemColors`), la durabilité maximale, la rareté,
  l'emplacement d'armure ; puis la liste que la recherche donne pour 19 requêtes ;
  puis les 81 cases de la page des barres sauvegardées. Tout va dans
  `data/vanilla/1.20.1/creative_items.json`, gitignoré.

Rien n'est lu du code de Mojang : les mappings officiels (`client.txt`,
SHA-1 `6c48521e…`) ne font que **nommer** classes, champs et méthodes.

**Ce que l'oracle ne peut pas faire, nommé** : `Screen.hasShiftDown()` lit la
touche physique chez GLFW, pas le flux d'événements ; aucun maj-clic ne
s'injecte. Les gestes avec maj sont donc mesurés en appelant `slotClicked` avec
`QUICK_MOVE` — exactement ce que fait `mouseClicked` quand maj est tenu — sauf
sur la page de survie (voir § 10).

### Pièges payés

* **Un appel imbriqué sur le fil de rendu attend pour toujours.** Le premier
  tour s'est figé : `leftPos()` postait une tâche au fil de rendu depuis une
  tâche déjà sur ce fil.
* **Le serveur garde l'inventaire du joueur entre deux lancements.** Le
  deuxième tour mesurait donc les gestes du premier ; le script efface le monde
  à chaque tour.
* **Les entités lâchées arrivent un geste en retard.** Un compte d'entités
  pris 400 ms après un lancer ne le voit pas toujours : le tour suivant compte
  aussi la *somme des piles* au sol, qui départage.
* **`Minecraft.getItemColors()` n'existe pas en 1.20.1** (c'est un champ), et
  `getEquipmentSlotForItem` est sur `LivingEntity`, pas `Mob`. Les deux premiers
  essais ont rendu 1557 erreurs, pas une valeur inventée.
* **La page « catégorie » d'une infobulle** a d'abord été vidée alors que la
  recherche était sélectionnée : les deux listes étaient identiques. Sur la
  recherche, le client insère **juste après le nom** une ligne bleue par onglet
  qui contient l'objet (la pierre porte « Building Blocks » *et* « Natural
  Blocks ») ; l'infobulle de catégorie est la même liste sans cette suite.

---

## 9. La recherche : la règle mesurée, et son chiffre

Les 19 requêtes de l'oracle départagent les hypothèses :

| requête | client | ce qu'elle apprend |
|---|---|---|
| `diamond` | 15 | la 15ᵉ est le gabarit d'amélioration en netherite : « Applies to: Diamond Equipment » — **les lignes de l'infobulle comptent**, pas seulement le nom |
| `oak_l` | 0 | **l'identifiant n'est pas cherché** sans deux-points |
| `minecraft:` | 1557 | avec deux-points, **on cherche l'identifiant** |
| `#minecraft:logs` | 40 | `#` cherche les **tags** |
| `  stone` | 0 | la requête **n'est pas rognée** |
| `Stone` | 93 | la casse est pliée |
| `glass pane` | 17 | sous-chaîne, espaces compris |

Règle retenue : requête pliée en minuscules, non rognée ; `#…` → tags (espace
de noms et chemin contiennent chacun leur partie) ; `…:…` → identifiant (idem) ;
sinon → **sous-chaîne du texte de l'infobulle de catégorie**, toutes lignes,
sans les lignes bleues d'onglet, mises en forme retirées.

**Le chiffre** (`ov_voxel --creative-parity`, sans fenêtre) :

```
search page: ours 1557 stacks, client 1557; identical in order: yes
queries identical to the client's, in order: 18 of 19
```

Avant ce travail : la recherche filtrait sur le seul nom, et « diamond » rendait
14 cases, pas 15. Pendant ce travail, un premier chargeur gardait la *seconde*
ligne d'onglet des objets qui en ont deux : 14 sur 19, et c'est ce chiffre qui
l'a trouvé.

**La 19ᵉ, nommée** : `night vision` rend 9 piles chez le client, 8 chez nous.
L'écart est le **ragoût suspect** : l'index de recherche du client porte son
effet (drapeau d'infobulle « créatif »), que son infobulle au survol n'affiche
pas — et nous n'avons que l'infobulle au survol. Neuf piles de ragoût sont
concernées.

---

## 10. Les gestes, mesurés sur le vrai client

Chaque ligne est un état relevé par l'oracle après le geste (ce qui est en
main, dans l'inventaire, au sol). Les règles vivent dans
`client::CreativeInventory` (`creative_gestures.{hpp,cpp}`), sans appareil ni
socket, et chacune a son test.

| geste | effet mesuré |
|---|---|
| clic gauche/droit sur une case, main vide | **un** objet en main — pas une pile (l'ancien code en donnait une pleine) |
| re-clic gauche sur la même case | +1 ; clic droit : −1 |
| clic sur une *autre* case, pile en main | la pile en main **est supprimée** |
| clic milieu, main vide | une pile pleine (64, 16 ou 1) ; main pleine : rien |
| **maj-clic sur une case** | une pile pleine **sur le curseur** — pas dans la barre d'action |
| chiffre N au-dessus d'une case | une pile pleine dans la case N de la barre ; main pleine : rien |
| Q / Ctrl+Q au-dessus d'une case | lance 1 / 64 (somme au sol : +1, +64), main pleine ou non |
| Q / Ctrl+Q au-dessus d'un emplacement | lance 1 / tout |
| clic gauche / droit hors du panneau | lance toute la pile / une |
| maj-clic sur la barre d'action d'une page d'objets | la pile **disparaît** |
| clic sur l'emplacement de destruction | la pile en main disparaît |
| **maj-clic sur la destruction** | **tout l'inventaire est vidé** |
| pile posée sur un emplacement occupé | échange |
| E sur la page de recherche | **ne ferme pas** : c'est une lettre |
| Q sur la page de recherche | tapé dans le champ, rien n'est lancé |
| T sur une page de catégorie | ouvre la **recherche** |

Tous les changements d'emplacement partent en **Set Creative Mode Slot (0x2B)**,
un paquet par emplacement modifié, comme vanilla ; un lancer est le même paquet
avec l'emplacement **−1**.

**Pas mesuré, nommé** : le maj-clic *sur la page de survie* (le déplacement
rapide de l'inventaire vanilla est appliqué : barre ↔ inventaire, armure vers
son emplacement) ; le glisser-déposer (mode 5) sur les emplacements du joueur
dans l'écran créatif n'est pas fait ; le double-clic qui rassemble non plus.

---

## 11. Captures : notre client contre le vrai, pixel par pixel

Mêmes conditions des deux côtés : fenêtre 1280×720 (tampon 2560×1440 Retina),
échelle d'interface 3, **Faithful 32x chargé dans les deux clients**, panneau à
(329, 172). Le vrai client est piloté par l'oracle (§ 8), le nôtre par
`--creative-tab`, `--creative-search` et `--creative-pointer`, le pointeur posé
où celui de l'oracle s'est réellement posé. `scripts/compare_creative_screen.py`
ne compare que ce que les deux dessinent de la même source : le panneau
195×136 et le corps de chaque bouton d'onglet (ce qui est derrière, le monde
assombri, n'est pas le même monde).

| capture | panneau identique | panneau à ±8 | corps d'onglets identique / à ±8 |
|---|---|---|---|
| Building Blocks | 75,3 % | 83,4 % | 67,8 % / 80,8 % |
| Colored Blocks | 74,7 % | 85,8 % | 〃 |
| Natural Blocks | 69,0 % | 79,1 % | 〃 |
| Functional Blocks | 75,3 % | 84,0 % | 〃 |
| Redstone Blocks | 76,4 % | 85,3 % | 〃 |
| Saved Hotbars | **94,6 %** | 96,9 % | 〃 |
| Search Items (vide) | 75,2 % | 83,2 % | 〃 |
| Tools & Utilities | 84,0 % | 94,5 % | 〃 |
| Combat | 84,4 % | 93,9 % | 〃 |
| Food & Drinks | 79,6 % | 93,4 % | 〃 |
| Ingredients | 79,4 % | 93,5 % | 〃 |
| Spawn Eggs | 80,0 % | 94,5 % | 〃 |
| Survival Inventory | **95,9 %** | 95,9 % | 〃 |
| recherche « diamond » | 90,4 % | 94,7 % | 〃 |
| survol d'une case + infobulle | 73,2 % | 81,2 % | 〃 |
| survol + infobulle (recherche) | 83,5 % | 88,6 % | 〃 |
| survol d'un onglet | 84,2 % | 93,9 % | 61,1 % / 72,5 % |
| survol de la destruction | 91,7 % | 91,7 % | 67,8 % / 80,8 % |

**Avant les corrections de ce passage** (pas 28, projection sur 2562 pixels) :
Building Blocks 69,9 %, Saved Hotbars 89,9 %, Tools 78,2 %, onglets 65,1 %.

Ce qui reste, et pourquoi — chaque cause a été vérifiée dans les pixels :

* **La géométrie ne diffère plus.** Mesurée à la résolution de l'écran et non
  plus du pixel d'interface, un bloc de la première case couvre les rangées
  571..616 dans les deux clients ; l'intérieur d'un emplacement est identique
  pixel pour pixel. ⚠️ Un premier relevé, pris au pixel central de chaque pixel
  d'interface, annonçait des cubes « deux rangées trop courts » : c'était
  l'échantillonneur. Faithful 32x met deux texels par pixel d'interface, soit
  1,5 pixel d'écran à l'échelle 3, et le pixel central tombe **sur** une
  frontière de texel. Les lignes d'un texel (contours d'emplacement) diffèrent
  pour cette raison, dans un sens ou dans l'autre : c'est de l'arrondi, pas une
  position.
* **L'éclairage des blocs dans l'interface.** Le dessus d'une bûche de chêne
  vaut `#b6925e` chez vanilla, `#ac8c56` chez nous ; son flanc `#2e2415`
  contre `#473820`. Vanilla éclaire un bloc d'interface par deux lumières
  directionnelles (`gui_light: side`) ; notre moteur d'items applique les trois
  ombrages de face fixes (1,0 / 0,8 / 0,6). C'est l'essentiel de l'écart des
  pages pleines de blocs (Natural Blocks 69 %), et il est dans le rendu d'items,
  pas dans cet écran.
* **Le mélange des transparences** se fait en espace linéaire ici, en sRGB chez
  vanilla : le fond d'infobulle vaut `#363336` contre `#1b0c1b`, son cadre
  `#3f2a9d` contre `#2c0863`. Même cause pour le survol d'une case. Le calcul
  (`0,94·lin(0x10) + 0,06·lin(0xc6)`) donne exactement le `#33` mesuré.
* **Le texte** est blanc pur ici, `#fcfcfc` chez vanilla, qui multiplie par la
  couleur réelle de la feuille de police ; le titre `#404040` contre `#3f3f3f`.
  Un niveau : dans la tolérance de ±8.
* **Absents** : le joueur en miniature (le panneau noir de la page de survie :
  l'essentiel des 4,1 % restants), le coffre du bouton « Survival Inventory »,
  le reflet d'enchantement.

Captures (gitignorées) : `run/creative-parity/ours/` et
`run/creative-parity/vanilla/`, en entier et recadrées au panneau
(`*-crops/`), une par onglet plus les survols.

---

## 12. Ce que le serveur fait du paquet — nommé, pas corrigé

`server.cpp` (bloc `kSetCreativeSlot`) applique le paquet en créatif et
l'ignore en survie, comme vanilla. Mais :

* `net::parse_set_creative_slot` **ne lit pas le NBT** (`CreativeSlot` n'a que
  l'emplacement, l'id et le compte), et le serveur range la pile avec un NBT
  vide. **Une potion, un livre enchanté, une flèche à pointe prise dans notre
  écran arrivent sur notre serveur comme la version sans NBT.** Notre client
  envoie le NBT, octet pour octet, comme vanilla.
* L'emplacement **−1** (le lancer) est ignoré : rien n'apparaît au sol.

Les deux sont dans `ov_protocol` et `server.cpp`, hors du mandat de ce travail
(quatre agents y écrivent) : ils sont nommés ici pour le suivant.

---

## 6. Ce qui n'est pas fait, nommé

* **Le joueur en miniature** dans la page de survie (le panneau noir) : il faut
  rendre le modèle d'entité dans une passe d'interface ; ce n'est pas fait.
* **Le coffre** du bouton « Survival Inventory » : c'est un rendu d'entité de
  bloc, que nous n'avons pas.
* **Le reflet d'enchantement** (livres enchantés, pomme dorée enchantée) : il
  demande une texture animée et un mélange que le lot d'interface n'a pas.
* **Le mélange des transparences** (survol, infobulle, voile) se fait dans
  l'espace linéaire de la chaîne d'échange sRGB, vanilla le fait dans l'espace
  sRGB : le survol blanc à 50 % sur le gris d'emplacement donne `#cfcfcf` ici,
  `#c5c5c5` chez vanilla (calcul ; § 11 pour la mesure).
* **Le ragoût suspect** dans la recherche (§ 9).
* **L'italique** d'une ligne est un cisaillement d'un pixel, comme vanilla ;
  aucune autre différence de police n'est connue ici.
* Sans `creative_items.json` (pas d'instance vanilla), l'écran retombe sur les
  noms seuls, sans teintes, et les emplacements d'armure acceptent tout ; c'est
  écrit dans le journal.

---

## 7. Reproduire

```bash
# le catalogue (aussi fait par scripts/setup_vanilla.sh)
scripts/measure_creative_tabs.py --check

# le vrai client : nombres, gestes, infobulles, captures (ouvre une fenêtre)
scripts/measure_creative_screen.py            # lance son propre serveur vanilla
#   → data/vanilla/1.20.1/generated/creative-screen/client-faithful/{oracle,screenshots}
#   → installe data/vanilla/1.20.1/creative_items.json

# la parité de la recherche, sans fenêtre
build/macos-debug/bin/ov_voxel --creative-parity

# nos captures, onglet par onglet, pointeur posé
build/macos-debug/bin/ov_voxel --singleplayer --width=1280 --height=720 --gui-scale=3 \
    --frames=210 --open-creative=150 --creative-tab=minecraft:combat \
    --creative-pointer=17,26 --screenshot=run/creative-combat.ppm

# la comparaison pixel par pixel
scripts/compare_creative_screen.py <nos png> <png vanilla>
```

Les options de test du client : `--creative-tab=`, `--creative-search=`,
`--creative-pointer=x,y` (relatif au coin du panneau), `--creative-take=case,emplacement`
(clic milieu puis clic gauche), `--dump-creative`, `--operator-tab`,
`--creative-items=`, `--item-tags=`, `--hotbar-file=`.
