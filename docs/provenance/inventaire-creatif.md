# L'inventaire créatif — les onglets, leur contenu, et leur géométrie

## Le problème qu'il règle

Le banc de test (`scripts/lab.sh`) sert `run/lab` **en créatif par défaut**, et
notre client n'avait aucun moyen de prendre un bloc : `--hold=` en donnait un
depuis la ligne de commande, et c'était tout. Le mandat demandait l'inventaire
créatif du jeu, pas « un sélecteur d'items ».

Il se sépare en deux moitiés qui n'ont **rien** en commun, et les confondre est
la seule vraie erreur possible ici :

* **le contenu** — quels items, dans quel ordre, dans quel onglet — n'a pas
  d'oracle réseau ;
* **la géométrie** — combien de cases, où, quelle taille — est écrite dans les
  pixels du pack et se compte.

---

## 1. Le contenu : il n'y a pas d'oracle réseau, mais il y a un oracle

En 1.20.1 les onglets créatifs sont construits par `CreativeModeTabs`. Le client
vanilla ne les reçoit **jamais** du serveur : il les fabrique lui-même au
démarrage, puis à chaque rechargement de tags. Il n'y a donc rien à capturer sur
le fil, et l'ordre d'un onglet ne peut pas se déduire du registre d'items —
`data/vanilla/1.20.1/generated` liste 1255 items dans l'ordre de leurs **ids
réseau**, qui n'est pas l'ordre créatif.

### Ce qui a été cherché d'abord

`PrismarineJS/minecraft-data` est MIT et explicitement autorisé par `CLAUDE.md`.
Il ne porte **pas** les onglets créatifs : ses jeux de données pour la 1.20.1
sont blocs, items, biomes, recettes, entités, protocole, langues, fenêtres,
formes de collision, enchantements, sons. Rien pour `creative`.

### Ce qui a été fait

`CreativeModeTabs` est aussi dans le **jar serveur**. On peut donc *le faire
tourner* :

```
scripts/creative_tabs_oracle.java   amorce le jeu, appelle tryRebuildTabContents,
                                    et imprime les piles dans l'ordre où le jeu
                                    les a mises
scripts/measure_creative_tabs.py    fournit le classpath, les mappings, et écrit
                                    data/vanilla/1.20.1/creative_tabs.json
```

C'est exactement la méthode de `tools/ov_datagen` : **exécuter le jar, jamais le
lire**. Les mappings officiels de Mojang (`server.txt`,
SHA-1 `0b4dba04…`) n'y servent qu'à **nommer** des classes, des champs et des
méthodes — l'usage que `CLAUDE.md` § 1 autorise explicitement — et chaque fait
imprimé sort du bytecode du jeu, pas d'une lecture de code.

Rien n'est commité : la sortie va dans `data/vanilla/1.20.1/creative_tabs.json`,
gitignoré comme le reste des données dérivées de Mojang. Seul le **SHA-256** est
dans le dépôt, dans `EXPECTED_SHA256` du script, et `--check` le vérifie :

```
sha256 e53d702cacaec6f85995c22b1ba98000a916c841b0ac918b608f35d0302f80c4
       147 558 octets, 14 onglets
```

### Le chiffre

| onglet | rangée | col | type | icône | cases | items distincts | cases à NBT |
|---|---|---|---|---|---|---|---|
| `building_blocks` | haut | 0 | catégorie | `bricks` | 348 | 348 | 0 |
| `colored_blocks` | haut | 1 | catégorie | `cyan_wool` | 198 | 198 | 0 |
| `natural_blocks` | haut | 2 | catégorie | `grass_block` | 226 | 226 | 0 |
| `functional_blocks` | haut | 3 | catégorie | `oak_sign` | 196 | 169 | 27 |
| `redstone_blocks` | haut | 4 | catégorie | `redstone` | 62 | 62 | 0 |
| `hotbar` | haut | 5 | barres d'action | `bookshelf` | 0 | 0 | 0 |
| `search` | haut | 6 | recherche | `compass` | 1587 | 1248 | 413 |
| `tools_and_utilities` | bas | 0 | catégorie | `diamond_pickaxe` | 110 | 101 | 42 |
| `combat` | bas | 1 | catégorie | `netherite_sword` | 97 | 54 | 86 |
| `food_and_drinks` | bas | 2 | catégorie | `golden_apple` | 176 | 45 | 135 |
| `ingredients` | bas | 3 | catégorie | `iron_ingot` | 171 | 133 | 40 |
| `spawn_eggs` | bas | 4 | catégorie | `pig_spawn_egg` | 76 | 76 | 0 |
| `op_blocks` | bas | 5 | catégorie | `command_block` | 29 | 11 | 19 |
| `inventory` | bas | 6 | inventaire de survie | `chest` | 0 | 0 | 0 |

**1689 cases** dans les onglets de catégorie, pour **1248 items distincts**.
L'écart de 441 vient des piles qui portent un NBT : 42 potions × 3 formes, 42
flèches teintées, 39 livres enchantés, 27 tableaux, 16 blocs de lumière, 9
soupes suspectes, 8 cornes de chèvre.

**Sept items du registre n'apparaissent dans aucun onglet** — et ce sont
exactement ceux que le jeu ne donne pas en créatif :
`air`, `petrified_oak_slab`, `filled_map`, `ender_dragon_spawn_egg`,
`wither_spawn_egg`, `written_book`, `knowledge_book`.

⚠️ **Le mandat décrivait les onglets d'avant la 1.19.3** — « décoration,
transport, divers, brassage, matériaux ». Ce jeu d'onglets n'existe plus en
1.20.1 : la 1.19.3 les a remplacés par les quatorze ci-dessus. Leurs noms
viennent des clés `itemGroup.*` de `lang/en_us.json` du jar, lues par
`ov_render::Language` : « Building Blocks », « Colored Blocks », « Natural
Blocks », « Functional Blocks », « Redstone Blocks », « Saved Hotbars »,
« Search Items », « Tools & Utilities », « Combat », « Food & Drinks »,
« Ingredients », « Spawn Eggs », « Operator Utilities », « Survival Inventory ».

### Le piège, et il a coûté cher

**Le premier tour de l'oracle était faux et ne disait rien.** Il rendait 1681
cases au lieu de 1689, avec **une** peinture au lieu de vingt-sept et **aucune**
corne de chèvre — et goat_horn ressortait alors dans la liste des « items dans
aucun onglet », ce qui ressemblait à un fait sur le jeu.

La cause : trois des générateurs demandent un **tag** au registre —
`#minecraft:placeable` pour les peintures, `#minecraft:goat_horns` pour les
cornes — et un registre dont les tags n'ont jamais été liés répond « vide »
au lieu d'échouer. `VanillaRegistries.createLookup()` construit les registres
mais **pas** leurs tags : ceux-ci viennent du datapack.

Le correctif est de charger le datapack vanilla par `TagManager` avant de
construire le lookup. `ReloadableServerResources.loadResources` ferait le même
travail mais construit aussi `Commands`, qui réclame les registres dynamiques de
worldgen que ce `RegistryAccess` ne porte pas — l'erreur exacte est
`Missing registry: minecraft:worldgen/biome`. `TagManager` seul n'en a pas
besoin ; la barrière de préparation qu'il exige est fournie par un `Proxy`
dynamique de trois lignes.

**Le compte est le contrôle** : 27 peintures posables + 4 non posables = les 31
variantes qui existent en 1.20.1, et 8 cornes = les 8 instruments. C'est le
piège n° 8 du briefing sous une autre forme — un jeu de données partiel résout
quand même.

### Le témoin absurde

Un oracle qui n'exécute pas le code mesuré donne le même chiffre quoi qu'on
fasse (piège n° 13). Ici la vérification est directe : avec
`--no-op` (`hasPermissions = false`), l'onglet opérateur passe de 29 cases à
**0** et le nombre d'items couverts par aucun onglet passe de 7 à 17. Le
paramètre atteint donc bien le code du jeu.

---

## 2. La géométrie : elle se compte dans les pixels

Même technique que `scripts/measure_gui_sprites.py` sur `inventory.png` : on
cherche les carrés 16×16 du gris d'emplacement `#8B8B8B`.
`scripts/measure_creative_tabs.py --geometry` les compte, et **le jar vanilla et
Faithful 32x donnent exactement les mêmes nombres** (Faithful livre ses feuilles
en 512² ; l'adressage reste en 256, ce que le facteur d'échelle absorbe).

| texture | carrés | ce que c'est |
|---|---|---|
| `tab_items.png` | **54** | 45 cases d'items (9×5) + 9 de barre d'action |
| `tab_item_search.png` | **55** | les 54 mêmes + le champ de saisie, dessiné dans le même gris |
| `tab_inventory.png` | **41** (+1 rose) | 4 armures, 1 main gauche, 27 d'inventaire, 9 de barre d'action, et l'emplacement de destruction |

**Le nombre d'emplacements par page est donc 45**, en 9 colonnes et 5 rangées,
coins en `(9 + 18c, 18 + 18r)`. La barre d'action est à `y = 112`, même pas de
18. Le champ de recherche occupe `(82, 6)` sur 80×16 — le détecteur n'en voit
qu'un carré de 16, à `(81, 5)`, parce que le champ est plus large que haut ;
c'est nommé, pas corrigé.

⚠️ **L'emplacement de destruction n'est pas gris, il est rose**, et le
détecteur de carrés gris ne peut pas le voir. Il est cherché par sa couleur :
`(173, 112)`, 16×16. Une page de survie sans lui est une page à laquelle il
manque la seule case dont l'effet est irréversible.

### `tabs.png`

* **Sept boutons par bande, pas de 26, largeur 26.** La seule ligne où les sept
  formes ne se touchent pas est celle de leur biseau d'arrondi — `y = 2` et
  `y = 34` pour la rangée du haut (arrondi en haut), `y = 90` et `y = 126` pour
  celle du bas (arrondi en bas) — et c'est de là que sort le pas.
* **Quatre bandes de 32, à `y = 0 / 32 / 64 / 96`** : haut non sélectionné, haut
  sélectionné, bas non sélectionné, bas sélectionné. Ce ne sont pas les bornes
  d'opacité qui les séparent (les bandes 1 à 3 se touchent) mais la **couleur** :
  la colonne `x = 13` vaut `#8B8B8B` sur les bandes non sélectionnées et
  `#C6C6C6` sur les sélectionnées, avec les transitions à 32, 64 et 96.
* **L'ascenseur** : deux poignées de 12×15 côte à côte à `(232, 0)` et
  `(244, 0)`, l'active puis la grisée. Sa glissière est dans `tab_items.png` :
  le gris d'emplacement y court de `x = 175` à `186` et de `y = 18` à `127`,
  donc **110 de haut pour une poignée de 15 — 95 de course**.
* ⚠️ **La feuille contient une légende que le jeu ne dessine jamais** : à
  `x = 182..207`, sur les quatre bandes, deux petits boutons rouges et bleus
  portant les mots « UNSEL » et « SEL ». C'est une note laissée par le
  graphiste. Un lecteur qui compte huit colonnes de boutons dessine un huitième
  onglet qui n'existe pas.

### Deux nombres *dérivés*, et ils sont signalés comme tels

Le pas des boutons **à l'écran** n'est pas dans les pixels de la feuille. Sept
boutons de 26 doivent couvrir un panneau de 195 : `(195 − 26) / 6 = 28,17`,
donc **28**. Le dernier bouton finit alors à 194, un pixel à l'intérieur du
panneau, et six intervalles de deux pixels séparent les boutons — ce qui est ce
que montre une capture. Un pas de 26 laisserait treize pixels de panneau nus à
droite.

Le **recouvrement de 4 pixels** entre un bouton et le panneau vient de la même
lecture : la bande « bas, non sélectionné » ne remplit que 28 des 32 lignes de
sa cellule, et la sélectionnée les 32.

Ces deux valeurs sont marquées **« Derived, not read »** dans
`creative_screen.hpp`. Elles sont les seules du travail à ne pas sortir d'une
mesure directe.

---

## 3. Ce qui est branché au serveur

`Set Creative Slot` est le **seul paquet du jeu dont le client est
autoritatif** — le serveur l'applique en créatif et *l'ignore en survie*, ce
qui est le comportement vanilla déjà vérifié dans `docs/provenance/survie.md`.
L'écran créatif est donc la seule fenêtre de ce client qui déplace une pile
elle-même ; tout le reste passe toujours par `Click Container` et attend.

L'écran refuse de s'ouvrir en survie plutôt que de proposer un catalogue qui ne
donnerait rien.

### ⚠️ Un bug trouvé en chemin : `Set Creative Slot` était tronqué

`Client::send_creative_slot` encodait le Slot à la main : drapeau de présence,
varint de l'id, octet du compte — **et s'arrêtait là**. Un Slot se termine par
son NBT, et un tag absent est un octet `TAG_End`, pas rien. Un serveur vanilla
aurait lu le premier octet du paquet suivant comme un type de tag et n'aurait
jamais retrouvé le fil. Notre serveur ne s'en plaignait pas, ce qui est
exactement pourquoi personne ne l'avait vu.

Corrigé : le paquet passe par `net::write_slot`, et porte le NBT de la case.

### L'aller-retour réel

Contre le banc, `ov_dedicated --world=run/lab` :

```
run 1  --open-creative=60 --creative-search="Block of Gold" --creative-take=0,36
       --place-at=33,-60,180
       → creative: cell 0 -> slot 36
       → creative tab minecraft:search "Search Items" (1 cells)
           cell 0  minecraft:gold_block

run 2  (nouvelle connexion, même serveur, --hold= vide)
       → le bloc d'or est là

run 3  (le processus serveur a été arrêté et relancé entre-temps)
       → le bloc d'or est toujours là
```

Le bloc a donc été **choisi dans l'écran créatif**, envoyé par `Set Creative
Slot`, accepté par le serveur, posé par une pose que le serveur a appliquée, et
écrit sur le disque. Aucune de ces étapes n'est simulée côté client.

---

## 4. Ce que l'écran coûte par frame

Même scène (banc, rayon 12, 2560×1440, échelle d'interface 3), **sans vsync**,
900 frames, Apple M2 via MoltenVK. La machine partageait son processeur avec
deux autres agents qui compilaient, donc les deux mesures ont été **alternées
trois fois** plutôt que faites l'une après l'autre : c'est l'écart d'une paire,
pas la valeur absolue, qui porte la conclusion.

Deux séries : trois paires *fermé / créatif*, puis deux triplets *fermé /
inventaire / créatif*. Les intervalles sont les minimum et maximum observés.

| | rec p50 | rec p99 | gpu p50 | gpu p99 | quads | appels |
|---|---|---|---|---|---|---|
| HUD seul, écran fermé | 0,10–0,17 ms | 0,14–0,31 ms | 0,64–0,74 ms | 0,81–0,90 ms | 10 | 4 |
| inventaire du joueur ouvert | 0,19–0,21 ms | 0,27–0,29 ms | 1,26–1,35 ms | 1,55 ms | 19 | 8 |
| **créatif ouvert** | **0,32–0,45 ms** | **0,40–0,55 ms** | **1,30–1,40 ms** | **1,53–1,65 ms** | **317** | **13** |

**L'écran créatif coûte +0,27 ms p50 / +0,29 ms p99 d'enregistrement CPU, et
+0,66 ms p50 de GPU**, par rapport au même plan écran fermé.

Le chiffre intéressant est la comparaison avec l'inventaire du joueur, qui
dessine **19 quads** contre 317 : leurs coûts GPU sont les mêmes à 0,03 ms près.
Le GPU d'un écran ouvert est **entièrement** le quad de fond assombri, qui
couvre les 3,7 mégapixels de la fenêtre en alpha — la référence de
`interface.md` (« +0,50 ms de GPU pour un seul quad ») tient donc aussi ici, et
les 45 modèles 3D d'items n'y ajoutent rien de mesurable. C'est côté **CPU** que
la différence se voit : +0,20 ms d'enregistrement de plus que l'inventaire, pour
300 quads de plus.

### Treize appels, pas trente-sept

La première version en faisait **37**. Le lot est coupé à chaque changement de
texture, et dessiner un bouton d'onglet puis son icône alterne la feuille
`tabs.png` avec l'atlas de blocs quatorze fois. En deux passes — tous les
boutons non sélectionnés, le panneau, le bouton sélectionné, puis toutes les
icônes — il en reste **13**, et le p99 d'enregistrement est passé de 1,53 ms à
0,74 ms sur la même page.

Rien n'alloue en régime établi : la page filtrée est reconstruite au changement
d'onglet ou de frappe, jamais dans `draw()`, et les noms traduits sont pliés en
minuscules **une fois par onglet** — la page de recherche en porte 1587, et les
traduire à chaque caractère ferait un million de recherches pour taper
« stone ».

---

## 5. Les gestes

| geste | effet |
|---|---|
| `E` | ouvre l'inventaire créatif en créatif, celui du joueur sinon |
| molette | fait défiler la page, par rangées de neuf |
| glisser la poignée | même chose, à la souris ; le glissé tient même quand le pointeur quitte la glissière |
| clic sur un onglet | change de page et remet le défilement à zéro, comme vanilla |
| clic sur une case | prend une **pile complète** — la limite propre à l'item, donc 16 pour un seau et 1 pour une épée — sur le curseur |
| maj + clic sur une case | va au premier emplacement libre : la barre d'action d'abord, puis les trois rangées |
| clic sur un emplacement du joueur | y dépose le curseur, ou reprend ce qui s'y trouve |
| clic sur l'emplacement de destruction | vide le curseur |
| clic hors de tout | vide le curseur |
| frappe (onglet loupe) | filtre sur le **nom traduit**, sans casse |
| retour arrière | efface une séquence UTF-8 entière, pas un octet |
| `Échap` | ferme |

La recherche filtre sur `Language::item_name`, c'est-à-dire les 6217 entrées de
`lang/en_us.json` déjà chargées : « diamond » rend 14 cases (bloc, minerai,
minerai des abysses, pelle, pioche, hache, houe, épée, casque, plastron,
jambières, bottes, armure de cheval, et le diamant).

---

## 6. Ce qui n'est pas fait, nommé

* **Les barres d'action sauvegardées** (`minecraft:hotbar`). Refusé et écrit à
  l'écran : elles vivent dans le fichier d'options du client vanilla, pas dans
  le jeu et pas sur le fil. Neuf rangées vides sous ce bouton promettraient une
  fonction qui n'existe pas ici.
* **Les quatre emplacements d'armure et la main gauche de la page de survie**
  sont dessinés mais **pas cliquables**, et l'ordre dans lequel ils
  correspondent aux emplacements 5 à 8 du protocole **n'a pas pu être mesuré** :
  les pixels donnent quatre carrés à `(54,6)`, `(108,6)`, `(54,33)`, `(108,33)`
  et ne disent pas lequel est le casque. C'est la seule chose de ce travail qui
  serait une supposition, et elle n'est donc pas branchée.
* **Le modèle du joueur** dans le panneau noir de la page de survie : ce rendu
  d'entité n'existe pas, comme dans l'écran d'inventaire ordinaire.
* **Les œufs d'apparition** sont tous dessinés du même gris. Vanilla les teinte
  par type d'entité à travers un gestionnaire de couleur d'item ; nous n'avons
  pas les `overrides` ni les teintes d'item (déjà nommé dans `interface.md`).
* **Le coffre** de l'onglet « Survival Inventory » ne s'affiche pas sur son
  bouton : un coffre est rendu par un *block entity renderer* que nous n'avons
  pas — même manque que les coffres invisibles du banc.
* **Les potions ne portent pas leur nom.** Le NBT est transporté jusqu'au
  serveur intact, mais le nom affiché vient de `item_name(minecraft:potion)`,
  donc les 42 variantes s'appellent toutes « Potion ». Les nommer demande de
  lire `Potion:` dans le tag et de traduire
  `item.minecraft.potion.effect.<effet>` ; ce n'est pas fait, et la recherche
  ne les distingue donc pas.
* **Le curseur de la barre de recherche ne clignote pas.** Un clignotement
  demande une horloge que l'interface n'a pas.
* **Le double-clic qui rassemble les piles** (mode 6) n'a pas d'équivalent ici :
  en créatif il n'y a rien à rassembler.

---

## 7. Reproduire

```bash
# l'oracle : les onglets et leur contenu, demandés au jeu
scripts/measure_creative_tabs.py --contents      # écrit le json + son sha256
scripts/measure_creative_tabs.py --check         # et le compare à celui du dépôt
scripts/measure_creative_tabs.py --no-op         # le témoin : l'onglet opérateur se vide

# la géométrie, dans le pack et dans le jar
scripts/measure_creative_tabs.py --geometry \
    --jar ~/Library/.../minecraft-1.20.1-client.jar

# une capture par onglet, contre le banc
build/macos-debug/bin/ov_dedicated --world=run/lab --port=25611 &
build/macos-debug/bin/ov_voxel --connect=127.0.0.1:25611 --frames=200 \
    --gui-scale=3 --stand-at=34.5,-60,182.5,180,15 \
    --open-creative=150 --creative-tab=minecraft:redstone_blocks \
    --dump-creative --screenshot=run/creative-redstone.ppm

# la recherche
build/macos-debug/bin/ov_voxel --connect=127.0.0.1:25611 --frames=200 \
    --open-creative=150 --creative-search=diamond --dump-creative

# l'aller-retour
build/macos-debug/bin/ov_voxel --connect=127.0.0.1:25611 --frames=220 --hold= \
    --stand-at=33.5,-60,182.5,180,25 --open-creative=60 \
    --creative-search="Block of Gold" --creative-take=0,36 --place-at=33,-60,180
```

`--screenshot` écrit un PPM malgré l'extension ; `sips -s format png` le
convertit. Les captures restent dans `run/` (gitignoré).
