# L'interface — police, HUD, écrans de conteneur, rendu d'item

## Le problème qu'elle règle

Le serveur savait déjà faire mourir un joueur, gérer sa faim, son expérience et
ses conteneurs. **Un client vanilla voyait tout ça ; le nôtre ne voyait rien.**
`ov_voxel` affichait le terrain, la boîte du bloc visé et un réticule — le jeu
était jouable *avec le client de Mojang* et pas avec le nôtre.

Ce document dit ce qui a été établi, comment, et ce qui ne l'est pas.

---

## 1. La police se mesure, elle ne se déclare pas

`font/default.json` dit **où** est un glyphe, jamais **quelle largeur** il fait.
Le jeu balaie les pixels du glyphe, prend la dernière colonne dont l'alpha n'est
pas nul, ajoute une colonne d'espacement, et met le tout à l'échelle
`height / hauteur_de_cellule` :

```
avance = (int)(0,5 + colonnes_encrées × height / hauteur_de_cellule) + 1
```

C'est pour ça que `i` fait 2, `l` 3, `t` 4, `k` et `f` 5, `@` et `~` 7, et tout
le reste 6. Et c'est pour ça qu'il n'y a **aucune table de largeurs** dans le
code : une table serait la copie d'une réponse que les pixels donnent déjà, et
elle serait fausse dès qu'un pack redessine un glyphe.

### Le chiffre

`scripts/measure_font_widths.py` refait ce calcul de façon indépendante — un
décodeur PNG écrit dans le script, un parcours des `providers` écrit dans le
script — et compare, **glyphe par glyphe** :

| Comparaison | Glyphes communs | Désaccords |
|---|---|---|
| notre C++ contre l'oracle Python, même pack | 2414 | **0 (0,0000 %)** |
| jar vanilla 1.20.1 contre `run/assets` (Faithful 32x) | 2414 | **51 (2,11 %)** |

Les 51 désaccords sont **tous au-dessus de U+00FF** : zéro dans l'ASCII, zéro
dans le Latin-1. Ils viennent de glyphes que Faithful a redessinés un peu plus
larges ou plus étroits (U+026D 3→4, U+03B5 6→5, U+0492 7→6, U+049C 6→7…). Ce
n'est pas un défaut de notre lecture : c'est ce que le pack change, et le jeu
vanilla chargé avec ce pack afficherait exactement les mêmes largeurs que nous.

Reproduire :

```bash
scripts/measure_font_widths.py \
  --jar ~/Library/.../minecraft-1.20.1-client.jar
```

### Trois pièges payés

**1. `ascii.png` de Faithful fait 256×256, celui du jar 128×128.** Les cellules
font donc 16 pixels chez l'un et 8 chez l'autre. C'est exactement ce que le
facteur `height / hauteur_de_cellule` absorbe : les deux donnent la même avance.
Un lecteur qui ignore ce facteur double toutes les largeurs sur un pack HD.

**2. 1.20.1 n'a pas de CJK dans son jar.** `font/include/unifont.json` y est une
liste de providers **vide** ; les données unifont sont téléchargées dans le
magasin d'objets des assets, pas empaquetées. La police par défaut couvre donc
le latin, le grec, le cyrillique, l'hébreu, l'arménien et un tas de symboles —
2414 glyphes — et s'arrête là. Un codepoint sans glyphe est **signalé absent**,
jamais dessiné comme un blanc.

**3. `accented.png` déclare `ascent: 10, height: 12`.** Les autres pages font 8
de haut avec une ascendante de 7. Le placement vertical doit donc être
`haut = ligne_de_base − ascent` avec une ligne de base fixe à 7 : c'est ce qui
laisse les accents dépasser de trois pixels au-dessus de la ligne pendant que
les lettres restent dessus. Placer les glyphes par leur haut décale tout
`accented.png` de trois pixels.

---

## 2. Les sprites de l'interface, lus dans les textures

Les coordonnées d'un cœur ou d'un emplacement ne sont écrites nulle part : ce
sont des faits sur les pixels. `scripts/measure_gui_sprites.py` les mesure.

**`icons.png`, cellules de 9×9 à partir de (16, 0).** Les cinq familles de cœurs
ont la même forme et se distinguent uniquement par la couleur moyenne :

| x | texels opaques | couleur | ce que c'est |
|---|---|---|---|
| 16 | 54 | `#191919` | conteneur vide |
| 25 | 54 | `#777777` | conteneur, clignotement |
| 52 / 61 | 34 / 20 | `#F11818` | cœur plein / demi |
| 88 / 97 | 34 / 20 | `#897417` | **empoisonné** (olive) |
| 124 / 133 | 34 / 20 | `#332323` | **flétri** (presque noir) |
| 160 / 169 | 34 / 20 | `#D4AC2D` | **absorption** (or) |

Une disposition écrite de mémoire met l'absorption à 88 — c'est-à-dire dessine
des cœurs empoisonnés. Les autres rangées : armure vide/demi/plein en (16, 9),
(25, 9), (34, 9) ; bulles pleine/qui éclate en (16, 18), (25, 18) ; haunches
vide en (16, 27), pleine/demi en (52, 27), (61, 27) ; barre d'expérience en
(0, 64) pour le fond et (0, 69) pour le remplissage, 182×5.

**Les fonds de conteneur** sont balayés à la recherche des carrés 16×16 du gris
d'emplacement `#8B8B8B`. Le compte est la vérification :

| Texture | Carrés trouvés | Emplacements du protocole |
|---|---|---|
| `inventory.png` | 46 | 46 (fenêtre 0) |
| `generic_54.png` | 90 | 54 + 36 |
| `crafting_table.png` | 46 | 1 + 9 + 36 |
| `furnace.png` | 39 | 3 + 36 |

Les deux d'accord, c'est ce qui dit que la disposition et le protocole parlent
bien de la même fenêtre. Le test `test_ov_client` reprend ces coins un par un.

⚠️ **Deux emplacements de résultat sont encadrés à 26×26**, pas à 18×18 : le
détecteur y trouve un carré gris de 24×24 dont le coin est (120, 31) pour
l'établi et (112, 31) pour le four. L'emplacement de 16×16 est **centré dedans**
— (124, 35) et (116, 35), qui sont les valeurs du code.

**Les feuilles sont adressées comme si elles faisaient 256×256**, quelle que
soit leur résolution réelle. Faithful les livre en 512² ; diviser par la taille
*logique* et non par la vraie est ce qui fait qu'une seule disposition sert tous
les packs.

---

## 3. Le rendu d'item : deux choses qui n'ont rien en commun

Une épée est une **image** : sa chaîne de parents finit sur `builtin/generated`
et sa texture `layer0` remplit la cellule. Un bloc est un **modèle** :
`item/stone` n'est que `{"parent": "minecraft:block/stone"}`, et ce qui est
dessiné dans l'emplacement est le bloc lui-même, tourné par la transformation
`display.gui` qu'il hérite de `block/block` — 30° autour de X, 225° autour de Y,
échelle 0,625.

La projection est faite dans `ov_render` (`item_model.cpp`), sans GPU, donc
elle est testable. Trois affirmations vérifiées par `test_item_model.cpp` :

- **Trois faces visibles sur six.** Les faces dont la normale transformée pointe
  vers l'arrière sont éliminées par leur propre enroulement, pas par leur
  `facing` (qui est arrondi à l'axe le plus proche et garderait les deux moitiés
  d'un modèle en croix).
- **La silhouette tient dans la cellule.** Largeur `2·(√2/2)·0,625·16 = 14,142`
  px, hauteur `2·(½·cos30 + (√2/2)·sin30)·0,625·16 = 15,731` px, centrée sur
  (8, 8). La hauteur à un quart de pixel de la cellule explique le choix de
  0,625 plutôt qu'un nombre plus rond.
- **Les trois faces sont trois gris différents** : 0,6, 0,8 et 1,0.

⚠️ **La face du haut n'est pas la plus proche.** Son centre est plus haut mais
pas plus près : les deux faces latérales penchent vers l'observateur et leurs
centres sortent à 0,19 bloc contre 0,156 pour le haut. Sans conséquence pour un
cube, dont les trois faces ne se recouvrent jamais ; avec conséquence pour un
escalier ou une torche, qui sont plusieurs boîtes — c'est à ça que sert le tri.

### Ce qui n'est pas mesuré ici

- **L'éclairage.** Vanilla monte deux lumières directionnelles pour un item 3D
  dans une interface. Nous appliquons la teinte plate du terrain (haut 1,0,
  nord/sud 0,8, est/ouest 0,6, bas 0,5). Le cube se lit correctement et ce ne
  sont **pas les mêmes nombres**.
- **La teinte des items.** Vanilla échantillonne le colormap en (0,5 ; 1,0) pour
  un item ; nous prenons le vert de `plains`, qui est un autre point du même
  colormap, à quelques niveaux près.
- **Les `overrides`.** Un arc bandé, une boussole, une potion changent de modèle
  selon un prédicat. Non implémenté : l'item prend toujours son modèle de base.
- **La barre de durabilité.** Le serveur n'envoie pas de dégâts d'item.

---

## 4. Le serveur est autoritatif, et l'écran lui obéit

Un clic envoie `Click Container` et **rien ne bouge** tant que le serveur n'a
pas répondu. Pas de déplacement local, pas de réconciliation. Ce n'est pas de la
prudence : le serveur applique les six modes de façon autoritative et mesurée,
et un client qui les prédirait aurait raison presque toujours — la fois où il a
tort, un objet disparaît.

Les six modes sont câblés dans `ContainerScreen::click` : 0 ramasser (bouton 0
la pile, 1 la moitié), 1 clic-majuscule, 2 touche numérique (**l'index de la
barre d'action voyage dans le champ `button`**, pas un bouton de souris), 3 clic
molette, 4 jeter (slot −999 pour le curseur), 5 glissé, 6 double-clic. Les modes
5 et 6 sont **envoyés** tels quels quand ils arrivent mais l'interface n'a pas
encore de geste qui les produise — nommé plutôt que caché.

### L'aller-retour réel

Contre le banc de test, serveur `ov_dedicated --world=run/lab` :

```
run 1  --place-at=34,-60,180 --use-block=34,-60,180 --move-slots=54,0
       → window 1 "Chest" (63 slots, state 2): slot 0  minecraft:chest x64

run 2  (nouvelle connexion, même serveur)
       --use-block=34,-60,180 --dump-window
       → window 1 "Chest" (63 slots, state 1): slot 0  minecraft:chest x64

run 3  (le serveur a été arrêté et relancé entre-temps)
       --use-block=34,-60,180 --dump-window
       → window 1 "Chest" (63 slots, state 1): slot 0  minecraft:chest x64
```

La pile déplacée par un `Click Container` de **notre** client survit à la
déconnexion, à la reconnexion, et à un **redémarrage du processus serveur** :
elle est donc passée par l'inventaire autoritatif du serveur et par le disque.

⚠️ **Les coffres du banc ne s'ouvrent pas.** `ov_lab` écrit le *bloc* coffre et
aucune entité de bloc ; le serveur n'ouvre une fenêtre que s'il en trouve une.
Un aller-retour scripté pose donc son propre coffre d'abord (`--place-at`), ce
que le serveur accepte parce qu'il crée l'entité de bloc à la pose. C'est un
manque du banc, pas du client — et c'est aussi pour ça que les coffres du banc
sont invisibles à l'écran : un coffre est rendu par un *block entity renderer*,
que nous n'avons pas, exactement comme les panneaux.

⚠️ **Les clics dans l'inventaire du joueur (fenêtre 0) ne font rien.** Le
serveur les ignore : `kClickContainer` exige `player.window_open` et
`click->window_id == player.window_id`, et il n'ouvre jamais la fenêtre 0.
L'écran s'affiche, le survol s'allume, le paquet part — et le serveur le jette.
C'est un manque **côté serveur**, hors du périmètre de ce travail, et il fallait
le nommer plutôt que de simuler le déplacement côté client pour faire joli.

---

## 5. Ce que l'interface coûte par frame

Même scène, banc de test, rayon 12, 2560×1440, échelle d'interface 3, **sans
vsync** (sous FIFO chaque frame lit l'intervalle de rafraîchissement et le
chiffre ne dit rien), 900 frames dont 890 après échauffement, Apple M2 via
MoltenVK.

| | rec p50 | rec p99 | gpu p50 | gpu p99 | quads | appels |
|---|---|---|---|---|---|---|
| HUD masqué | 0,03 ms | 0,06 ms | 0,59 ms | 0,95 ms | 0 | 0 |
| HUD affiché | 0,05 ms | 0,11 ms | 0,60 ms | 0,96 ms | 10 | 4 |
| HUD + inventaire ouvert | 0,07 ms | 0,30 ms | 1,10 ms | 2,94 ms | 28 | 9 |

**Le HUD coûte +0,02 ms p50 / +0,05 ms p99 d'enregistrement CPU, et rien de
mesurable sur le GPU.** Sur un budget p99 de 17,77 ms avec vsync dont 0,41 ms
d'enregistrement, c'est 12 % de la part CPU et 0,3 % de la frame.

**Un écran ouvert coûte +0,50 ms p50 / +1,98 ms p99 de GPU.** Ce n'est pas
l'interface, c'est *un* quad : le fond assombri couvre les 3,7 mégapixels de la
fenêtre en alpha. Le reste — le fond du conteneur, les 46 emplacements, le
texte — tient dans les 0,04 ms de CPU supplémentaires.

Rien n'alloue en régime établi : le tampon de sommets est un anneau par frame en
vol, les lots et le scratch sont des membres, et `views_` est réservé une fois.
La pointe mesurée est de 306 sommets sur 73 728 disponibles.

---

## 6. Décisions à connaître

**Un seul pipeline, un tirage par changement de texture.** Le HUD complet fait
4 appels : la feuille `widgets`, la feuille `icons`, l'atlas des blocs pour
l'item tenu, une page de police pour le compte. Un écran de coffre en fait 9.

**Le y est retourné dans le shader d'interface, et il le fallait.** `ov_rhi`
pose un viewport à hauteur négative pour que le reste du moteur travaille en
+Y vers le haut ; le clip y = −1 est donc le *bas* du framebuffer. Un pixel
d'écran compte vers le bas. Les deux se contredisent, et la première capture
d'écran a montré exactement ça : la barre d'action en haut et tous les glyphes
à l'envers.

**Les teintes sont converties sRGB → linéaire avant d'atteindre le sommet.**
Le swapchain est sRGB et le GPU ré-encode à l'écriture ; sans la conversion, un
glyphe blanc teinté `0xFF5555` sortirait plus sombre que le nombre que vanilla
nomme. ⚠️ En revanche, le **mélange alpha** du fond assombri se fait alors en
linéaire alors que vanilla le fait en sRGB : mesuré, un pixel d'herbe à
`(114, 129, 90)` devient `(65, 69, 50)` chez nous et vaudrait ≈ `(40, ·, ·)`
chez vanilla. Notre écran est donc un peu moins sombre. C'est un choix que tout
le moteur a déjà fait — le calque translucide du terrain se mélange pareil — et
non un oubli.

**Le texte trace son ombre en premier, en une passe séparée.** Dessiner glyphe
et ombre ensemble met un glyphe sous l'ombre du suivant.

**Le gras est le même glyphe redessiné un pixel plus loin**, ce qui est aussi
pourquoi il coûte un pixel d'avance. L'italique est un cisaillement d'un pixel.
`§k` (obscurci) est **analysé et porté, jamais dessiné** : il remplace chaque
glyphe par un autre de même avance à chaque frame, ce qui demande un RNG par
frame que l'interface n'a pas, et une substitution figée serait un autre effet
portant son nom.

**Une forme de menu non implémentée est refusée et nommée.** Une enclume
dessinée comme un coffre mettrait ses trois emplacements là où est l'inventaire
du joueur, et chaque clic nommerait au serveur un slot qui veut dire autre
chose. Sont dessinés : `generic_9x1..9x6` (0–5), `crafting` (11), `furnace`,
`blast_furnace`, `smoker` (13, 9, 21). Tout le reste est refusé, journalisé, et
la fenêtre est fermée.

---

## 7. Ce qui n'est pas fait, nommé

- **L'absorption** (cœurs dorés) : dessinée, jamais alimentée. C'est la
  métadonnée d'entité 15 sur le joueur lui-même, et le serveur n'envoie pas de
  métadonnées pour le joueur à qui il parle.
- **L'oxygène** (bulles) : dessiné, jamais alimenté. Le serveur *calcule* la
  noyade (`survival_session.cpp`) et ne met jamais la valeur sur le fil.
- **L'armure** : dessinée, calculée **de ce côté** à partir des emplacements
  d'armure. La table des points par pièce est la seule chose de ce travail qui
  n'a **pas** été mesurée : le serveur n'envoie pas l'attribut `generic.armor`
  et le data generator n'émet pas les attributs d'item. C'est dit ici plutôt que
  présenté comme un fait établi.
- **Le chat, la liste des joueurs, le menu d'échappement, la barre de boss, les
  toasts, les effets de potion** : rien de tout ça n'existe.
- **Les modes de clic 5 (glissé) et 6 (double-clic)** : câblés jusqu'au paquet,
  aucun geste ne les produit encore.
- **Les composants de chat** : seul `{"text": …}` est rendu en texte. Un titre
  avec `translate` ou `extra` est laissé en JSON, visiblement faux plutôt que
  subtilement faux.
- **Le modèle du joueur** dans l'écran d'inventaire : le panneau noir de la
  texture est là où vanilla dessine le joueur, et ce rendu d'entité n'existe pas.

---

## 8. Reproduire

```bash
# le banc, sur un port à soi
build/macos-debug/bin/ov_dedicated --world=run/lab --port=25601 &

# le HUD en survie (cœurs, haunches, barre d'expérience)
build/macos-debug/bin/ov_voxel --connect=127.0.0.1:25601 --frames=500 \
    --stand-at=34.5,-60,182.5,180,15 --gui-scale=3 --screenshot=run/ui-hud.ppm

# l'inventaire du joueur
build/macos-debug/bin/ov_voxel --connect=127.0.0.1:25601 --frames=200 \
    --stand-at=34.5,-60,182.5,180,15 --open-inventory=150 \
    --hold=minecraft:oak_log --screenshot=run/ui-inventory.ppm

# l'aller-retour : poser un coffre, y déplacer une pile, relire
build/macos-debug/bin/ov_voxel --connect=127.0.0.1:25601 --frames=340 \
    --hold=minecraft:chest --stand-at=34.5,-60,182.5,180,25 \
    --place-at=34,-60,180 --use-block=34,-60,180 --move-slots=54,0 \
    --dump-window --close-at=320

# les oracles
scripts/measure_font_widths.py --jar <client-1.20.1.jar>
scripts/measure_gui_sprites.py
```

`--screenshot` écrit un PPM malgré l'extension ; `sips -s format png` le
convertit. Captures gardées dans `run/ui-*.png` (gitignoré).
