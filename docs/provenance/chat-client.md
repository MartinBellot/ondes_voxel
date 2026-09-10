# Le chat de notre client — mesuré sur le vrai client 1.20.1

## Le problème

« Pourquoi le chat ou la console ne s'ouvrent pas avec la touche T ? » Le
serveur avait le chat et 34 commandes (`commandes.md`) et un client vanilla
s'en servait déjà ; **notre client n'avait aucun écran de chat**. Ce dossier
décrit ce qui a été construit, et surtout d'où viennent ses nombres : du
**vrai client 1.20.1 en fonctionnement**, pas de mémoire.

---

## 1. L'oracle : le vrai client, piloté

`scripts/measure_chat_screen.py` lance l'instance PrismLauncher 1.20.1 de
l'utilisateur, la fait rejoindre un serveur vanilla 1.20.1 (jar de
`tools/vanilla/`) **en opérateur** (un `ops.json` écrit avec l'UUID hors-ligne
du joueur), et `scripts/chat_screen_oracle.java` la pilote par des événements
GLFW injectés dans les gestionnaires clavier et souris du jeu lui-même
(même méthode que l'oracle de l'inventaire créatif, dont il reprend le lecteur
de mappings). Après chaque geste, le jeu donne ses nombres — champ de saisie,
taille du chat, lignes et tick d'arrivée, suggestions, minuteries des titres —
et prend une capture par son propre chemin de capture.

Rien n'est lu ni traduit du code de Mojang : les mappings officiels ne servent
qu'à **nommer** les champs et les méthodes (CLAUDE.md § 1). Une fonction comme
le fondu est mesurée **en boîte noire**, en l'échantillonnant. Rien n'est
commité : tout va sous `data/vanilla/1.20.1/generated/chat-screen/`.

Le deuxième passage regarde **le ciel à la verticale**, nuages coupés : un fond
presque uni derrière chaque capture, sur lequel une sonde de pixels
(`scripts/measure_chat_geometry.py`, et des balayages ligne/colonne) lit les
rectangles et leurs alphas.

Reproduire :

```bash
python3 scripts/measure_chat_screen.py            # ≈ 3 min, 25 captures + facts.txt
```

---

## 2. Ce que le client répond

### Ouvrir, fermer, envoyer

| Geste | Vrai client 1.20.1 | Notre client |
|---|---|---|
| T (avec son caractère `t`) | écran de chat, champ **vide** | idem : le caractère du même poll est jeté |
| `/` (avec son caractère) | champ **« / »**, pas « // » | idem |
| Entrée depuis le monde | **rien** ne s'ouvre | idem (le « T ou Entrée » du wiki est Bedrock) |
| Échap, liste de suggestions ouverte | **cache la liste**, garde l'écran | idem |
| Échap, sans liste | ferme | idem |
| Entrée | `/…` → Chat Command (sans `/`), sinon Chat Message | idem, non signés |
| « `  spaced    out   message  ` » envoyé | reçu **« spaced out message »** | idem (`normalize_chat_message`) |

### Le champ (EditBox du chat)

| Mesure | Valeur | Source |
|---|---|---|
| position, taille | (4, h−12), 850 × 12 à 854 × 480 | `input` |
| longueur max | **256** unités UTF-16 ; au-delà, coupé sans bruit | 300 lettres tapées → 256 |
| défilement | `displayPos` **104** après 256 lettres | idem |
| `§` | **refusé** (« é§x » donne « éx ») | |
| couleur du texte | `0xE0E0E0` | `DEFAULT_TEXT_COLOR` |
| curseur | `_` en fin de texte, sinon une barre de 1 px `0xFFD0D0D0` | `CURSOR_*` |
| fond du champ | (2, h−14) → (w−2, h−2), noir **alpha 0,5** | pixels |
| ←×3, Début, Fin, Retour arrière | curseur 8, 0, 11, « hello worl » | |

Les accords Ctrl/Cmd (tout sélectionner, mot à mot, copier/coller) **n'ont pas
pu être injectés** : `Screen.hasControlDown()` lit l'état physique de la
touche, pas le flux d'événements. Ils suivent la règle connue de l'EditBox
(Cmd sur macOS) et sont testés, mais **non mesurés**.

### L'historique

Deux envois (« hello world », « /time set day ») puis ↑, ↑, ↑, ↓ :
« /time set day », « hello world », « hello world », « /time set day » —
l'historique garde les commandes **avec** leur `/`, et le texte **normalisé**.
Une ligne rappelée n'affiche pas de liste de suggestions.

### Les messages

| Mesure | Valeur |
|---|---|
| historique | **100** messages (`MAX_CHAT_HISTORY`) |
| largeur | **320** px à l'échelle 1 (`getWidth`) |
| lignes | **10** fermé (hauteur 90), **20** ouvert (180), **9** px la ligne |
| marge basse | **40** px (`BOTTOM_MARGIN`) : la dernière ligne finit à h−40 |
| fond d'une ligne | x 0 → 332, noir alpha **0,5 × opacité** |
| barre à gauche | 2 px `0xD0D0D0` sur toute la hauteur de la ligne, sur **toutes** les lignes |
| texte | x = 4, y = haut de ligne + 1, alpha = opacité |
| molette | **7** lignes par cran (`MOUSE_SCROLL_SPEED`), 1 avec Maj |
| Page ↑ | **19** lignes (une page moins une) |

**Le fondu**, `getTimeFactor(âge)` échantillonné à 45 âges :

| âge (ticks) | 0…179 | 180 | 181 | 185 | 190 | 195 | 199 | ≥ 200 |
|---|---|---|---|---|---|---|---|---|
| facteur | 1 | 1 | 0,9025 | 0,5625 | 0,25 | 0,0625 | 0,0025 | 0 |

soit exactement `clamp((1 − âge/200)·10, 0, 1)²` : **10 s de vie, opaque 9 s,
fondu sur la dernière**. Le wiki (article *Chat*, version courante) dit
« 3 seconds » : ce n'est **pas** 1.20.1. Les captures confirment : la ligne
est intacte à 8,5 s, pâle à 9,3 s, absente à 10,2 s ; l'alpha lu sur le fond à
9,3 s vaut la moitié de celui du texte, comme la formule le veut.

⚠️ **Piège de mesure** : une capture est prise ≈ 6 ticks après le tick demandé
(le script attend 300 ms avant de capturer) ; l'alpha lu à « 9,3 s » correspond
à l'âge 192, pas 186. Sans cette correction la mesure semblait contredire la
formule.

**Le retour à la ligne**, `StringSplitter.splitLines` à 320, même pack :

```
"The quick brown fox jumps over the lazy dog, and then it"          291 px
"keeps running because the lazy dog finally woke up and is now"     318 px
"very, very angry."                                                  90 px
```

- coupure **à une espace**, l'espace disparaît ;
- un mot plus long que la ligne est **coupé au caractère** (« …docio » / « us… ») ;
- `\n` coupe ; les espaces de tête et de queue restent ;
- dans le chat, **chaque ligne de continuation commence par une espace**
  (« ␣ious »), non comptée dans la largeur de coupe.

Notre `wrap_runs` rend **ces trois découpes à l'identique** avec la police du
pack (test `lines break where the running client breaks them`).

### La complétion

| Saisie | Vrai client | Notre client |
|---|---|---|
| `/` | **aucune liste** | idem |
| `/` puis Tab | liste de 79, rien remplacé | liste de **34** (nos commandes), idem |
| Tab à nouveau | « /advancement » (la 1ʳᵉ) | « /clear » (notre 1ʳᵉ) |
| `/ti` | `time`, `title` ; « me » gris après le curseur | idem, local |
| `/ti` Tab | « /time », la liste reste | idem ; Tab suivant → « /title » |
| `/time set ` | `day midnight night noon`, fantôme « day » | idem, **demandé au serveur** |
| `/gamemode ` | `adventure creative spectator survival` | demandé au serveur |
| usage | « <time> », « <gamemode> [<target>] » | idem, depuis l'arbre |

Géométrie : lignes de 12 px, texte à (x+1, ligne+2), fond `0xD0000000`,
sélection `0xFFFF00`, autres `0xAAAAAA`, fantôme `0x808080`, bas de la liste à
h−15, x = début du mot − 1, largeur = plus long + 1 ; au-delà de 10 entrées la
liste déborde d'un pixel à chaque bout et le bord qui cache des entrées est
**pointillé blanc**, un pixel sur deux.

Couleurs de la commande tapée : `/` et littéraux gris `0xAAAAAA`, arguments
cyan, jaune, vert, magenta, or à tour de rôle, ce qui ne s'analyse pas rouge
(« /ti » : « ti » rouge ; « /tp @s 0 64 0 zz » : `@s` cyan puis rouge après
l'erreur).

### Titres et barre d'action

| Mesure | Valeur |
|---|---|
| temps par défaut | fondu 10, maintien 70, fondu 20 ticks |
| barre d'action | **60** ticks (41 lus 19 ticks après) ; fondu sur les 20 derniers |
| titre | ×4, centré, haut à h/2 − 40 |
| sous-titre | ×2, centré, haut à h/2 + 10 |
| barre d'action | centrée, haut à **h − 70** (mesuré en créatif) |

---

## 3. Ce qui est construit

| Pièce | Où | Rôle |
|---|---|---|
| `TextField` | `ov_client/text_field.{hpp,cpp}` | le champ, partagé avec la **recherche créative** (même widget chez vanilla) |
| `KeyEvent`, presse-papiers | `ov_client/window.{hpp,cpp}` | touches d'édition **avec répétition** (rappel clavier GLFW), Cmd = Ctrl sur macOS |
| `ChatLog`, `ChatInput`, `TitleOverlay` | `ov_client/chat.{hpp,cpp}` | messages, champ + complétion, titres |
| `CommandTree` | `ov_client/command_suggestions.{hpp,cpp}` | complétion locale depuis le paquet Commands |
| `component_runs` | `ov_render/text_component.{hpp,cpp}` | composants → segments stylés, **couleur exacte** (`#rrggbb`) |
| `chat_types_from_codec` | `ov_protocol/chat_types.{hpp,cpp}` | les types de chat, lus dans le codec de Login (play) |
| paquets | `ov_netclient/client.{hpp,cpp}` | System/Player/Disguised Chat, titres, Commands, suggestions ; Chat Message, Chat Command, Suggestions Request |
| câblage | `apps/ov_voxel/src/chat.{hpp,cpp}` | décore, envoie, relie |

**Les composants** passent par la même marche que les info-bulles créatives
(`text_component`), avec une seconde sortie en segments pour garder la couleur
exacte : `flatten_component` ne sait dire que 16 couleurs, et `/tellraw` envoie
`#3080ff`. Les octets de `flatten_component` ne bougent pas (ses tests
passent inchangés).

**Player Chat Message** porte un index dans le registre `minecraft:chat_type`
**du codec**. Le client lit le codec de Login (play), prend la décoration
(`chat.type.text` et ses paramètres `sender`, `content`, son style) et bâtit le
composant traduisible. Un index absent du codec est **refusé et nommé** (le
message s'affiche nu, un avertissement le dit), jamais décoré d'une mise en
page inventée. Contenu : `unsigned_content` s'il est présent, sinon le corps
en littéral.

**Les trois paquets de chat reçus, vérifiés contre ce que notre serveur
envoie** : System (retours de commandes, `/tellraw`, messages de mort),
Player (chat, `/me`, `/msg`, `/say` d'un joueur), Disguised (`/say` de la
console) — tous passés par les parseurs d'`ov_protocol` que le serveur
aller-retourne octet pour octet.

---

## 4. Notre client contre le vrai, au pixel

Mêmes conditions des deux côtés : 2560 × 1440, échelle d'interface 3
(854 × 480 pixels d'interface), regard vers le haut, pack Faithful 32x.
Balayages ligne/colonne des captures (`.scratch/probe_pixels.py`, non commité ;
`scripts/measure_chat_geometry.py` pour la version générale).

| Élément | Vrai client | Notre client |
|---|---|---|
| boîte d'une ligne fermée | x 0 → 332, bas à 440 | **x 0 → 332, bas à 440** |
| barre grise | x 0 → 2, `d0d0d0` | **identique** |
| premier pixel du texte | x 4,33 | **x 4,33** |
| fond d'une ligne (rapport au ciel derrière) | 0,502 | **0,498 – 0,502** |
| champ | x 2 → 852, y 466 → 478, rapport 0,50 | **identique** |
| liste « /time set » | x 51 → 90, y 417 → 465 | **identique** |
| fond du champ | 0,50 | **0,493 – 0,498** |
| fond de la liste | 0,185 | **0,177 – 0,180** (0,16 – 0,165 avec l'approximation en puissance 2,2 ; voir ci-dessous) |

| fondu à l'âge **190** (facteur 0,25) | fond 0,875 attendu | **0,871 – 0,875** ; texte `d5dce7` là où un mélange sRGB donnerait ≈ `d2` (le texte n'a pas la correction) |
| titre ×4 | encre du « T » à y 200, 4 px blancs puis ombre `3e3e3e` | **y 200, 4 px, ombre `404040`** |
| sous-titre ×2 jaune | encre 250 → 264 | **250 → 264** |
| barre d'action | encre à y 410 | 412 à la première capture → origine corrigée à **h − 72** |
| styles `/tellraw` | gras hérité par tout le tableau, `#3080ff`, or souligné, barré, vert traduit | **identique à l'œil** (captures 17 et `chat-styles`) |

⚠️ **Le mélange alpha, piège payé.** La première capture de notre chat avait un
fond bien plus clair (ciel `0xE0` → `0xA4` au lieu de `0x70`) : le RHI mélange
en lumière linéaire, vanilla en sRGB. Pour un fond **noir**, demander
a′ = 1 − lin(1 − a) au lieu de a donne lin(s)·lin(1 − a) ≈ lin(s·(1 − a)) — le
pixel de vanilla, quel que soit le fond. Appliqué aux seuls fonds noirs du
chat (`black()` dans `chat.cpp`) ; le reste de l'interface garde l'écart décrit
dans `interface.md` § 6.

---

## 5. De bout en bout

Contre `ov_dedicated --world=<copie de run/lab>` (une copie : `/time set`
écrit `level.dat`), un `ops.json` à côté, joueur `OvChat` :

```
ov_voxel --connect=127.0.0.1:25612 --username=OvChat --gui-scale=3 --frame-ms=16 \
    --frames=420 "--chat=/time set day" "--chat=hello world" --chat-at=100 --dump-chat \
    --screenshot=chat-closed.ppm
→ [chat] chat: 7 chat types from the codec
→ [chat] chat: command tree of 242 nodes
→ [chat] [CHAT] Set the time to 1000
→ [chat] [CHAT] <OvChat> hello world
```

Le serveur a exécuté la commande (Chat Command, sans signature), répondu par
un System Chat traduisible que le client a résolu avec `en_us.json` ; le
message est revenu en Player Chat, décoré par le type `minecraft:chat` du
codec (`<%s> %s`).

Options de script ajoutées à `ov_voxel` (toutes passent par les chemins des
touches : `submit` appuie sur Entrée, `type` tape) : `--chat=…` (répétable),
`--chat-file=` (une ligne par message, sans guillemets de shell), `--chat-at=`,
`--chat-open=`, `--chat-type=`, `--dump-chat`, `--dump-commands=`,
`--frame-ms=`, `--chat-shot-age=`.

⚠️ **Piège payé : un script de chat compté en images court plus vite que le
serveur.** Une fenêtre masquée n'est pas limitée par la synchro verticale
(0,49 ms par image) : 420 images passaient en une demi-seconde, `/time set day`
partait avant la connexion, et la capture ne montrait rien. Le script attend
désormais l'arbre des commandes (le jeu a commencé) et compte à partir de là ;
`--frame-ms` impose une durée d'image minimale aux captures ; et la capture du
fondu s'arrête sur **l'âge** voulu (`--chat-shot-age`), qu'un compte d'images
ne sait pas viser.

**Tests** : `test_ov_client` 42 cas / 453 assertions (dont le nouveau
`test_chat.cpp` : champ, historique, Unicode, retour à la ligne contre les
découpes mesurées, fondu contre les 45 échantillons, traductions `%s` / `%1$s`
/ imbriquées, types de chat du codec réel, complétion depuis **le paquet
Commands de notre serveur capturé par notre client**
(`src/ov_client/tests/data/commands_owner.bin`, 3330 octets, 242 nœuds)) ;
`test_ov_render` 171 / 1862 et `test_ov_protocol` 906 assertions, inchangés et
verts. `check_layers` et `check_assets` passent.

---

## 6. Ce que le chat coûte par image

Même scène (banc, regard vers le haut), 2560 × 1440, échelle 3, **sans
vsync**, 900 images dont 890 après échauffement, Apple M2 via MoltenVK,
build Debug. « rec » est l'enregistrement CPU de la frame entière.

| | rec p50 | rec p99 | gpu p50 | gpu p99 | quads | appels |
|---|---|---|---|---|---|---|
| chat fermé, vide (référence) | 0,08 ms | 0,13 ms | 0,39 ms | 0,47 ms | 10 | 4 |
| chat fermé, 10 lignes | 0,17 ms | 0,29 ms | 0,43 ms | 1,12 ms | 232 | **6** |
| chat ouvert, « /time set » et 4 suggestions | 0,14 ms | 0,51 ms | 0,41 ms | 0,51 ms | 76 | 8 |

**Dix lignes coûtent +0,09 ms p50 / +0,16 ms p99 d'enregistrement**, le champ
ouvert avec sa liste +0,06 / +0,38 ms ; le GPU ne bouge pas de façon
mesurable au p50. Le chat vide ne dessine rien : son coût est la
mise à jour d'un état, pas un quad.

⚠️ **Mesuré, puis corrigé** : la première version dessinait chaque ligne fond
puis texte, et le GUI coupe un lot à chaque changement de texture — dix lignes
faisaient **24 appels** (0,24 / 0,47 ms). Tous les fonds d'abord, tous les
textes ensuite : **6 appels**, 0,17 / 0,29 ms.

Rien n'alloue par image une fois les lignes reçues : le style est codé dans
le texte (`§l`, `§o`…) **quand la ligne est rangée**, pas quand elle est
dessinée ; seule l'arrivée d'un message ou une frappe allouent.

La « référence » est ce binaire, chat fermé et vide — pas le binaire d'avant
ce travail, qui n'a pas été reconstruit (disque partagé) ; le chemin
supplémentaire du chat vide est une mise à jour d'état sans dessin.

---

## 7. Ce qui n'est pas fait, nommé

- **Complétion des noms de joueurs** dans un message qui n'est pas une
  commande (Tab sur du texte simple) : non fait.
- **Complétion locale des arguments** : vanilla complète certains types
  d'argument lui-même et ne demande au serveur que les nœuds `ask_server` ;
  nous demandons au serveur **dès qu'un argument est possible**. Le résultat
  affiché est le même (notre serveur répond comme vanilla sur 21/22 sondes),
  le trafic ne l'est pas.
- **Les messages d'erreur d'analyse** au-dessus du champ (« Incorrect argument
  for command at position 7 … ») : vanilla analyse la commande localement avec
  Brigadier ; nous n'avons pas d'analyseur client. La coloration après une
  erreur est une **approximation** (chaque mot d'argument prend la couleur
  suivante au lieu du rouge de vanilla).
- **Clics et survols** dans le chat (`clickEvent`, `hoverEvent`) : non faits.
- **La sélection** est une boîte bleue translucide ; vanilla inverse les
  couleurs, un mode de mélange que le RHI n'a pas.
- **La barre de défilement** : seule sa colonne a été mesurée (x 332–335, à
  peine plus sombre que le ciel) ; sa longueur et sa course ne sont pas
  reproduites exactement.
- **Le mélange alpha** se fait en linéaire chez nous, en sRGB chez vanilla
  (`interface.md` § 6) : un fond noir à 0,5 sort un peu moins sombre.
- **La hauteur de la barre d'action en survie** : mesurée en créatif seulement.
- **La langue active** : le client prend `--lang` (défaut `en_us`) ; aucun
  écran de langue n'existe.
- **Le bouton d'indicateur** (icône « non sécurisé » au survol de la barre
  grise) : non dessiné.
- **Le texte qui s'efface** n'a pas la correction gamma des fonds : à mi-fondu
  il sort de deux ou trois niveaux plus clair que chez vanilla.

---

## 8. Reproduire

```bash
# l'oracle : le vrai client, ≈ 3 min (PrismLauncher, Java 17, server.jar)
python3 scripts/measure_chat_screen.py
#   → data/vanilla/1.20.1/generated/chat-screen/client-faithful/{oracle/facts.txt,screenshots/}

# un serveur à nous, sur une COPIE du banc (/time écrit level.dat), un ops.json à côté
cp -R run/lab /tmp/lab && build/macos-debug/bin/ov_dedicated --world=/tmp/lab --port=25612 &

# notre client : fermé, ouvert avec suggestions, fondu à 190 ticks
ov_voxel --connect=127.0.0.1:25612 --username=OvChat --stand-at=34.5,-60,182.5,180,-90 \
    --gui-scale=3 --frame-ms=16 --frames=420 "--chat=/time set day" "--chat=hello world" \
    --chat-at=100 --dump-chat --screenshot=run/chat-closed.ppm
ov_voxel … --frames=330 "--chat=/time set day" --chat-at=60 --chat-open=150 \
    "--chat-type=/time set " --screenshot=run/chat-open.ppm
ov_voxel … --frames=3000 "--chat=fading line" --chat-at=60 --chat-shot-age=190 \
    --screenshot=run/chat-fade.ppm

# la géométrie, des deux côtés
scripts/measure_chat_geometry.py --scale 3 --sky --region 0,300,860,480 <captures…>

# le fixture de la complétion : le paquet Commands de notre serveur, par notre client
ov_voxel --connect=… --username=OvChat --dump-commands=src/ov_client/tests/data/commands_owner.bin
```

Captures gardées dans `run/chat-{closed,open,open-ti,fade-190ticks,title,styles}.png`
(gitignoré), à comparer à `data/vanilla/1.20.1/generated/chat-screen/client-faithful/screenshots/`
(`12-time-set`, `10-suggest-argument`, `08-suggest-ti`, `05-fade-9.3s`,
`19-title`, `17-styles`).
