# Le HUD complet, et les écrans qui manquaient — mesurés sur le vrai client

## Le problème

Le HUD de notre client dessinait les cœurs, les haunches, l'armure et la
barre d'expérience, sans aucune de leurs variantes, et une partie des nombres
n'arrivait jamais : l'absorption posée *sur* les cœurs au lieu d'après, pas
d'air, pas de gel, pas d'icônes d'effets, pas de barre de boss, pas de liste
des joueurs, pas de cœurs de monture. Et la moitié des fenêtres que notre
serveur ouvre (trémie, distributeur, boîte de Shulker, enclume, meule, table
d'enchantement, alambic) étaient refusées par le client.

Ce document dit comment chaque élément a été **mesuré sur le vrai client
1.20.1**, ce qui est identique, et ce qui ne l'est pas.

---

## 1. La méthode : les deux clients, le même serveur, les mêmes scènes

`scripts/hud_scenes.txt` est **la** liste des scènes : des commandes, des
attentes en ticks, des captures, des touches. Les deux clients la jouent contre
un serveur vanilla 1.20.1 **neuf** (monde plat, survie, opérateur), lancé par
`scripts/measure_hud.py` :

- le vrai client (PrismLauncher, Faithful 32x) par `scripts/hud_oracle.java`,
  qui envoie les commandes par `ClientPacketListener.sendCommand` et les touches
  par les gestionnaires GLFW du jeu, comme les oracles du chat et des écrans ;
- notre client par `ov_voxel --hud-script=…` (`apps/ov_voxel/src/hud_script.cpp`),
  qui passe par les mêmes chemins qu'une main : l'Entrée du chat, Tab, F3, E.

Les deux reçoivent donc **les mêmes octets** : Set Health, les métadonnées de
l'air (1), du gel (7) et de l'absorption (15), Entity Effect, Update
Attributes, Boss Bar, Player Info Update. Tout écart est un écart de *notre*
HUD — ou de notre décodage, ce qui est la seconde chose mesurée.

**Chaque capture est double** : l'image avec l'interface, puis la même sans
(F1 : `Options.hideGui` chez vanilla, `set_hud_visible(false)` chez nous). La
différence des deux est l'**encre** de l'interface, exacte quel que soit le
fond — ciel, eau, neige poudreuse. `scripts/compare_hud.py` compare les masques
d'encre des deux clients zone par zone (IoU) et la couleur de chaque pixel
d'encre (exacte, et à ±8 niveaux).

À chaque capture, le vrai client donne aussi ses nombres (`facts.txt`) : santé,
absorption, air, gel, armure, `lastHealth`, `displayHealth`,
`healthBlinkTime`, les effets et leur durée, les barres de boss, la liste,
les titres, les lignes du F3 — lus par réflexion, nommés par les mappings
officiels. **Rien n'est lu ni traduit du code de Mojang** (CLAUDE.md § 1) : ce
qui est mesuré est ce que le jeu *dessine* et ce qu'il *vaut*, en boîte noire.

Rien n'est commité : tout va sous `data/vanilla/1.20.1/generated/hud/`.

```bash
ovlane.sh java python3 scripts/measure_hud.py vanilla            # le vrai client
ovlane.sh java python3 scripts/measure_hud.py ours               # le nôtre
scripts/compare_hud.py compare data/vanilla/1.20.1/generated/hud/{vanilla,ours}
scripts/compare_hud.py grid <capture.png> --region x,y,w,h       # lire une zone
```

Même conditions partout : fenêtre 1280×720 (tampon 2560×1440), échelle
d'interface 3, soit 854×480 pixels d'interface ; regard à la verticale vers
un ciel de midi sans nuages.

---

## 2. Les cœurs

### Ce que le vrai client a dessiné

Lu sur les captures (`compare_hud.py grid`), en pixels d'interface :

| scène | mesure |
|---|---|
| 04-hurt (13/20) | cœurs à y = h − 39 = 441, à partir de x = w/2 − 91 = 336, pas de 8 ; 6 pleins, 1 demi |
| 06-absorption (8) | **4 cœurs dorés sur une seconde rangée, 10 px plus haut (y 431)**, *après* les cœurs de santé ; l'armure monte d'autant (y 421) |
| 05-armour (10) | armure à y 431, cinq pleines sur dix |
| 02-xp (30) | niveau « 30 » à y = h − 35 = 445, contour noir, vert `#80FF20` |

L'absorption **n'est pas posée sur les cœurs** : elle les continue. C'était
l'erreur de la version précédente. Le nombre de rangées est
`ceil((max(santé max, affichée) + ceil(absorption)) / 2 / 10)` et l'écart entre
deux rangées `max(10 − (rangées − 2), 3)` — 10, puis 9 à trois rangées, jamais
moins de 3.

### Les familles, et le hardcore

Les cellules d'`icons.png` (scan de `measure_gui_sprites.py`) : conteneur 16
(clignotant 25), normal 52, empoisonné 88, flétri 124, doré 160, **gelé 178** ;
demi +9 ; clignotant +18 sauf pour le doré et le gelé, qui n'ont pas de
cellule clignotante. Le hardcore déplace **toutes** les cellules à la rangée
y = 45 de la feuille. Le gelé vient de 140 ticks de gel (le « fully frozen » du
jeu), mesuré : `frozen 140` dans les nombres de la scène 12-frozen.

### Le clignotement

Mesuré sur les minuteries du vrai client : sept dégâts au tick 566 donnent
`healthBlinkTime` 586 (+20), et la santé *affichée* reste à 20 pendant une
seconde de temps réel (`displayHealth 20`, 325 ms après). Les cœurs clignotent
quand `(fin − tick) / 3` est impair. Une hausse fait +10.

### Les cœurs qui tremblent : dix sur dix

Sous 4 points (santé + absorption), chaque cœur descend d'un pixel ou non,
chaque tick. **Hypothèse** : un `java.util.Random` resemé à chaque image par
`tick × 312871`, un tirage `nextInt(2)` par cœur, du dernier au premier.
**Vérifiée** sur la capture 20-low-a (tick 2236) : les cœurs 1, 2, 3, 5 et 8
sont un pixel plus bas, 0, 4, 6, 7 et 9 non — exactement les dix tirages que
le JDK lui-même donne pour la graine 699579556 (0,1,0,0,1,0,1,1,1,0 du cœur 9
au cœur 0). `java.util.Random` est l'algorithme que la documentation du JDK
spécifie ; `test_hud.cpp` le tient contre des valeurs produites par le JDK.

### La vague de régénération

Le cœur `tick mod ceil(santé max + 5)` monte de deux pixels. Échantillonné aux
ticks 923, 937, 950 et 964 (10-regen-a..d) : seul 950 donne un indice sous 10
(le cœur 0).

---

### Les montures

Lu sur les captures de `hud_scenes_mount.txt` (le vrai client, le même
serveur) :

| monture | à droite | barre du bas | fenêtre (E) |
|---|---|---|---|
| cheval 23/30 | 15 cœurs orange, 10 à y 441 et 5 à y 431, alignés à droite comme les haunches | **barre de saut**, pas de niveau | selle (8, 18), armure (8, 36), pas de coffre |
| âne à coffre | ses cœurs | barre de saut | selle, **pas d'armure**, coffre de 5 colonnes |
| lama force 3 | ses cœurs (53 → 27, trois rangées) | **barre d'expérience et niveau** | **pas de selle**, tapis (8, 36), coffre de 3 colonnes |
| cochon sellé 7/10 | 5 cœurs | barre d'expérience et niveau | — |

Un lama ne se dirige pas : il garde la barre d'expérience. Les cœurs du joueur
restent à gauche. Et le vrai client écrit **de lui-même**, au moment où la
monture commence, « Press Left Shift to Dismount » dans la barre d'action
(`mount.onboard`) — le nôtre aussi désormais.

Les cadres d'emplacement de la fenêtre du cheval sont sous la fenêtre dans
`horse.png`, rangée y 220 : armure 0, selle 18, tapis de lama 36 ; la grille
du coffre en (0, 166), 18 pixels par colonne.

## 3. Ce que le serveur envoie, et que notre client lit maintenant

| élément | paquet | champ |
|---|---|---|
| absorption | Set Entity Metadata (0x52) du joueur lui-même | index 15, Float |
| air | idem | index 1, VarInt |
| gel | idem | index 7, VarInt |
| armure | Update Attributes (0x6A) | `minecraft:generic.armor`, base + modificateurs |
| santé max | idem | `minecraft:generic.max_health` |
| monture | Set Passengers + métadonnée 9 (santé) + attribut de santé max du véhicule | |
| effets | Entity Effect (0x6C) : durée et drapeaux (0x01 ambiant, 0x04 icône) | décomptés par le client tick par tick |
| barres de boss | Boss Bar (0x0B), les six actions | |
| liste | Player Info Update (0x3A), Player Info Remove (0x39), Set Tab List Header And Footer (0x65) | |
| hardcore | Login (play) | |

Les trois paquets de la liste et Open Horse Screen (0x20) ont leur décodeur
dans `ov/protocol/tab_list.hpp`, écrits depuis la page figée (oldid=2773082),
testés sur des octets écrits à la main depuis ses tables et ajoutés au fuzz.

⚠️ **Set Tab List Header And Footer n'est jamais envoyé par un serveur
vanilla** (la page le dit, et aucune commande ne le fait). La scène
`tabfoot` le fait arriver : chez vanilla par ses propres setters, chez nous
par **notre décodeur**, les octets étant ceux de notre encodeur.

⚠️ La musique des boss lisait déjà Boss Bar. Les actions 2–4 (santé, titre,
style) n'ont pas de drapeaux ; les transmettre sans précaution aurait retiré la
barre du dragon de la liste de la musique. `sound_director.cpp` les ignore.

---

## 4. Les icônes d'effets

Lu sur 11-effects : cadres 24×24 de `inventory.png` (141, 166), (165, 166)
pour un effet ambiant, tous les **25 px depuis le bord droit** (829, 804, 779,
754 pour 854 de large) ; bénéfiques à y 1, les autres à y 27 ; l'icône 18×18 à
3 px du coin. Un effet donné avec `hideParticles` n'a pas le drapeau d'icône et
n'est **pas dessiné du tout** (résistance, dans les nombres : `icon false`).

**L'ordre**, lu sur la même capture (chaque icône comparée, par sa couleur
moyenne, à la texture de chaque effet candidat composée sur la couleur
intérieure du cadre) : de droite à gauche, chance
(infinie), force (568 ticks), vitesse (567), vision nocturne (150) ;
faiblesse (574), lenteur (572). Le temps restant décroissant depuis le bord,
et **un effet infini compte comme le plus long** — l'hypothèse de départ
le mettait en dernier, et c'était faux. La vision nocturne clignotait
(opacité ≈ 0,58 à ce tick) : c'est la seule que la couleur moyenne ne
reconnaît pas, par élimination. Non mesurés : un effet ambiant après les
autres, la couleur du tourbillon comme départage.

**Le clignotement** des dix dernières secondes, lui, **n'est pas mesuré**.
L'hypothèse (`effect_icon_alpha`) donne 0,575 / 0,400 / 0,601 / 0,461 aux
durées 150, 135, 119 et 103 de la vision nocturne (11-effects a..d). L'opacité
lue sur les captures — un ajustement « icône × a + cadre × (1 − a) » aux
moindres carrés — donne 0,43 / 0,32 / 0,46 / 0,30 : la même alternance
haut-bas-haut-bas, mais un niveau que la méthode ne sait pas lire. Étalonnée
sur des icônes qui ne clignotent pas (opacité 1), elle rend 0,78 à 0,95 selon
l'icône et le texel échantillonné : une erreur de ±0,2, trop grande pour
trancher. La phase est cohérente ; la formule reste une hypothèse.

La catégorie « bénéfique » vient du tableau « Effect » du wiki (1.20.1) ;
confirmée par les nombres du vrai client pour les huit effets des scènes
(`beneficial true/false`). Les neutres (lueur, mauvais présage) vont dans la
rangée des autres.

Toutes les icônes sont **une seule texture** assemblée au chargement : six
icônes coûtent un changement de texture, pas six (la réserve de descripteurs
est de 64 jeux par image pour tout le client — `ecrans.md` § 4).

---

## 5. Les barres de boss

Les sept couleurs et les cinq styles, à l'écran ensemble (15-bossbars) : 182
de large depuis x 336, première barre à y 12 puis **19 px** plus bas chaque
fois, titre centré 9 px au-dessus, **dans l'ordre d'ajout**, aucune barre au
tiers de l'écran ou plus bas. `bars.png` : couleur c aux rangées 10c (vide) et
10c + 5 (plein), style s aux rangées 80 + 10(s − 1). Le glissement d'une barre
dure `LerpingBossEvent.LERP_MILLISECONDS` = **100 ms** de temps réel, lu sur le
jeu en marche.

---

## 6. La liste des joueurs

Lu sur 18-tab-score : le bloc de x 380 à 473, à partir de y 9 ; une case de 92
à x 381 ; nom `[R] OvOracle` en rouge d'équipe ; score « 42 » en jaune après le
nom le plus large ; les barres de latence finissent à x 471. La largeur d'une
colonne est `nom + score + 13`, au plus 20 rangées par colonne
(`PlayerTabOverlay.MAX_ROWS_PER_COL` = 20 lu sur le jeu), fond noir à 50 %,
cases blanches à 1/8. **Pas de têtes** : le vrai client ne les dessine que pour
un serveur intégré ou une connexion chiffrée, et un serveur hors ligne n'est ni
l'un ni l'autre.

---

## 7. Les écrans ajoutés

Emplacements lus dans les textures du pack (carrés 16×16 du gris
d'emplacement), chaque compte tenu contre le protocole :

| écran | menu | emplacements (texture) |
|---|---|---|
| trémie | 15 | 5 à y 20 depuis x 44, joueur à 51 et 109, fenêtre de **133** ✓ 41 |
| distributeur, dropper | 6 | 3×3 depuis (62, 17) ✓ 45 |
| boîte de Shulker | 19 | 3×9 depuis (8, 18) ✓ 63 |
| enclume | 7 | (27, 47) (76, 47) (134, 47) ✓ 39 |
| meule | 14 | (49, 19) (49, 40) (129, 34) — le détecteur y voit aussi un cadre |
| table d'enchantement | 12 | (15, 47) (35, 47) ✓ 38 |
| alambic | 10 | fioles (56, 51) (79, 58) (102, 51), ingrédient (79, 17), poudre (17, 17) |
| cheval, âne, lama | Open Horse Screen | selle (8, 18), armure (8, 36), coffre à (80, 18) sur `nb/3` colonnes |

Les titres, lus dans les fenêtres du vrai client (encre gris foncé dans la
rangée du titre, bordure exclue) : « Item Hopper » et « Repair &
Disenchant » à x 8 comme toutes les fenêtres ; « Dispenser » de 63 à 111,
**centré** (176 − 50) / 2 ; « Repair & Name » à **x 60**, après le marteau
que la texture de l'enclume dessine de 25 à 40. Nos deux exceptions (titre
centré du distributeur, x 60 de l'enclume) sont celles-là.

Tailleur de pierre, table de forge, métier à tisser, table de cartographie,
balise et pupitre **ne sont pas ouverts par notre serveur** : leurs écrans
restent refusés et nommés, comme avant.

---

## 7 bis. Pièges payés

- **La dernière capture d'une passe était vide (0 octet).** Le jeu écrit une
  capture sur son pool d'entrées-sorties, *après* le retour de
  `Screenshot.grab` ; l'oracle arrêtait la JVM aussitôt la dernière scène
  jouée. Deux captures perdues (`20-low-d-nohud`, `14-mount-pig-nohud`) : la
  seconde de chaque passe. L'oracle attend désormais 3 s avant de s'arrêter,
  et `compare_hud.py` nomme une capture vide au lieu de trébucher dessus.
- **Deux passes, un port.** Les passes « hardcore » et « écrans » ont tourné
  en même temps dans les deux emplacements de la voie java, toutes deux sur
  25621 : le second serveur n'a pas pu ouvrir le port et le client « écrans »
  est resté à « Connecting to 127.0.0.1, 25621 » jusqu'au délai. La passe
  « écrans » tourne maintenant sur 25721.
- **Échap injecté n'a pas fermé la fenêtre de la trémie.** Toutes les scènes
  suivantes ont photographié la même trémie : chaque `use` tombait sur
  l'écran resté ouvert, pas sur le nouveau bloc. Les nombres du jeu le
  disaient sans le dire — les commandes passaient, aucune erreur. L'oracle
  appelle désormais `LocalPlayer.closeContainer()` (ce qu'Échap appelle sur un
  conteneur) quand un écran est encore ouvert, et note l'écran ouvert après
  chaque `use` et chaque touche.
- **`spawn-animals=false` refuse aussi un cheval `/summon`é** : la première
  scène de monture n'avait aucun cheval (« No entity was found »).
  `doMobSpawning false` dans les scènes suffit à garder le monde vide.
- **Toute notre interface était un pixel d'écran trop haut.** Colonne par
  colonne, à la résolution du tampon, à travers un cœur et la barre
  d'expérience : chaque bord horizontal des nôtres venait une rangée plus tôt
  que chez le vrai client (rouge du cœur à 1327 contre 1328, bord de la barre
  à 1354 contre 1355), chaque bord vertical sur la même colonne. Le calcul du
  viewport est exact (y + h, hauteur −h) : c'est une convention de
  rastérisation entre les deux pipelines. Avec un pack 2× à l'échelle 3, un
  texel fait 1,5 pixel d'écran, et ce décalage d'un pixel suffit à changer le
  texel de plusieurs rangées — c'est ce qui laissait les cœurs à 82–92 %
  d'identiques pour des masques d'encre à plus de 90 %. `Gui` ajoute
  désormais une rangée à chaque sommet (`kRowOffset`), vérifié sur les
  captures suivantes.
- **F1 ne cache pas un écran ouvert.** `hideGui` retire le HUD mais laisse la
  fenêtre de conteneur : la capture « sans HUD » d'un écran le montre encore,
  et la différence d'encre y est vide. Les fenêtres sont donc comparées
  directement, pixel contre pixel dans leur rectangle de 176 de large —
  opaque sur le monde assombri, il n'y a rien à soustraire.
- **Le coffre de la passe « écrans » ne s'est pas ouvert** chez le vrai
  client (« screen none » après le clic). Les huit autres fenêtres oui ; le
  coffre, déjà mesuré (`interface.md`), sort de cette comparaison.
- **Le disque.** Une capture PPM de notre client pèse 11 Mo, deux par scène ;
  un BMP intermédiaire 14,7 Mo. Nos captures sont récrites en PNG sans perte
  à la fin de la passe, et les BMP de comparaison lus puis effacés aussitôt.

## 8. Ce qui n'est pas fait, nommé

- **Le réticule n'inverse pas** ce qui est dessous : blanc aux trois quarts,
  faute d'un mode de mélange dans le RHI (déjà nommé dans `interface.md`).
- **Le clignotement d'une hausse de santé** : +10 ticks à chaque hausse chez
  nous ; vanilla l'exige « pendant l'invulnérabilité », que ce client ne tient
  pas. Une régénération lente clignote donc chez nous et pas chez lui.
- **Les têtes de la liste** : jamais dessinées (un serveur intégré en aurait).
  Les objectifs de liste en cœurs (`render type hearts`) ne sont pas dessinés.
- **Les drapeaux de barre de boss** « assombrir le ciel » et « brouillard » :
  lus, pas rendus.
- **Le F3** : les lignes sans valeur ici restent vides à leur numéro — serveur
  et débit (tx/rx), P:, Chunks[C], FC, CH/SH, Local Difficulty, Sounds, et à
  droite Java, allocations, version GL. La mémoire est celle du processus.
- **Dans les écrans ajoutés**, seuls le fond, les emplacements et les titres
  sont dessinés : ni le champ de nom de l'enclume, ni les trois boutons de la
  table d'enchantement, ni les jauges de l'alambic (propriétés de conteneur),
  ni le **modèle de l'animal** dans le cadre noir de la fenêtre du cheval.
- **Tailleur de pierre, table de forge, métier à tisser, table de
  cartographie, balise, pupitre** : notre serveur ne les ouvre pas ; ils
  restent refusés.
