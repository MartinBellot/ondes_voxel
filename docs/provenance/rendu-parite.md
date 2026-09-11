# Le rendu du monde contre le vrai client 1.20.1

*Branche `rendu-parite`, 2026-09-11.*

L'utilisateur joue avec `ov_voxel` et demande un rendu « identique à
Minecraft ». Ce document dit comment on l'a mesuré, ce qui a changé, de combien,
et ce qui reste faux — chaque écart nommé avec son chiffre.

---

## 1. La méthode : deux clients, un serveur, les mêmes octets

Un rendu ne se compare pas à une description ; il se compare à une capture.
Le montage fait tourner **le vrai client 1.20.1 de l'utilisateur** (instance
PrismLauncher, Faithful 32x) et **le nôtre** contre **le même serveur — le
nôtre**, `ov_dedicated`, sur une copie de dix régions du monde de référence
(`run/reference-1234567890`, graine 1234567890) :

* les deux clients reçoivent **exactement les mêmes paquets** : mêmes blocs,
  mêmes biomes, **même lumière**. Tout écart entre les deux captures est un
  écart de rendu, pas de génération ni de moteur de lumière ;
* chaque scène est une position, une orientation, une heure (`/time set`,
  `doDaylightCycle false`), prise quand **les 289 chunks** du carré 17×17 sont
  arrivés, que plus rien n'a bougé depuis 3 s et que toutes les sections sont
  compilées (maillées chez nous) ;
* le vrai client est piloté par `scripts/render_parity_oracle.java` (même
  technique que les oracles de l'inventaire créatif et du chat : les mappings
  officiels ne servent qu'à *nommer*, chaque nombre est produit par le
  bytecode du jeu en cours d'exécution) ; il capture par son propre chemin
  (`Screenshot.grab`) et imprime les nombres de la frame : couleurs du ciel, du
  brouillard, du lever de soleil et des nuages, distances de brouillard des
  deux passes, luminosité des étoiles, phase de lune, ombrage par face
  (`ClientLevel.getShade`), et **les 256 texels du lightmap** avec le
  scintillement (`blockLightRedFlicker`) qui les a produits ; puis un balayage
  du lightmap sur 12 heures × 3 réglages de luminosité, et le lissage de la
  lumière (`AmbientOcclusionFace.calculate`) du jeu lui-même sur 25 faces
  choisies ;
* notre client (`ov_voxel --connect`) fait les mêmes scènes, une exécution
  par scène (`--stand-at`, `--settle-shot`, `--settle-chunks=289`) ;
* `scripts/compare_render_parity.py` compare les paires pixel à pixel.

Réglages communs : 854×480 (1708×960 en Retina), distance de rendu 8, FOV 70,
luminosité 0,5, graphismes « fancy », lissage de la lumière activé, mélange
de biomes 2, mipmaps 4, nuages activés — les réglages de l'instance de
l'utilisateur, sauf trois qui n'enlèvent que du mouvement : pas de balancement
de la vue, pas d'effet de FOV en vol, pas de pause quand la fenêtre perd le
focus. Interface masquée des deux côtés (F1 chez vanilla ; chez nous
`--settle-shot` coupe chat, réticule et contour du bloc visé).

### Les scènes

| scène | lieu | heure | ce qu'elle teste |
|---|---|---|---|
| `plains` | plaine, x = 48046 | 6000 | herbe, fleurs, eau, ciel |
| `aolab` | plateforme de pierre à y = 90 | 6000 | occlusion ambiante, ombrage par face |
| `cave` | salle de pierre fermée, deux torches | 6000 | lumière de bloc seule |
| `plains_sunset` | plaine, vers l'ouest | 12700 | coucher de soleil |
| `plains_night` | plaine, vers le zénith | 18000 | nuit, lune, étoiles |
| `sky_noon` / `sky_midnight` | zénith | 6000 / 18000 | soleil, lune |
| `sunrise` / `dusk_west` | horizon est / ouest | 23300 / 12300 | bande de l'aube |
| `beach` | plage du spawn | 6000 | eau, sable, océan tiède |
| `jungle` | lisière de jungle | 6000 | feuillage de jungle |
| `underwater` | fond de rivière | 6000 | brouillard sous l'eau |
| `desert` | désert contre badlands | 6000 | teintes sèches, terre cuite |

(Aucun marais dans les régions du monde de référence : la scène « biome
contrasté » est donc jungle + désert/badlands, et le marais reste non mesuré.)

---

## 2. Ce qui a été trouvé et corrigé

### 2.1 Le monde était composé en lumière linéaire — le jeu jamais

C'est l'écart le plus grand, et il était invisible dans le code : il est dans
les **formats**. La swapchain est `B8G8R8A8_SRGB` et l'atlas `R8G8B8A8_SRGB`.
Échantillonner un atlas sRGB décode vers la lumière linéaire, écrire dans une
cible sRGB ré-encode, et le mélange alpha d'une cible sRGB se fait en linéaire.
Minecraft 1.20.1 ne convertit **jamais** : il multiplie les octets stockés par
l'ombrage, le lightmap et le brouillard, et mélange l'eau sur ces octets.

Conséquence chez nous : une face ombrée à 0,6 sortait à 0,6^(1/2,2) ≈ 0,79 de
sa texture, un coin d'AO à 0,4 à 0,66, la teinte de l'herbe (multipliée à une
texture décodée puis ré-encodée) délavée en vert-de-gris. Le monde entier était
plus clair et plus terne que le jeu, et d'autant plus que la lumière baisse.

**Correction** : le monde est rendu dans une image **RGBA8 UNORM**
(`ov_client/scene_target`), où chaque multiplication et chaque mélange se fait
sur les nombres stockés ; les shaders du terrain et des entités ré-encodent la
texture échantillonnée pour retrouver ces nombres ; une passe plein écran
recopie l'image dans la swapchain à travers la fonction de transfert inverse,
que l'encodeur de la swapchain défait octet pour octet. L'interface (autres
agents) se dessine ensuite directement dans la swapchain, inchangée.

Écart résiduel nommé : le filtrage trilinéaire entre niveaux de mip se fait en
linéaire (atlas sRGB) et non sur les octets comme chez vanilla — visible
seulement au loin.

### 2.2 L'occlusion ambiante et la lumière lissée

*(chiffres de l'oracle `ao` et des captures `aolab` : section 3)*

L'ancienne règle comptait les voisins opaques sur quatre marches fixes
`0,2 / 0,467 / 0,733 / 1,0` et moyennait la lumière en **sautant** les voisins
opaques, arrondie au niveau entier, 4 bits par sommet. Ce n'est pas ce que fait
le jeu :

* la luminosité d'un coin est la **moyenne de quatre « shade brightness »** —
  0,2 pour un bloc dont la forme de collision est un cube plein, 1,0 sinon —
  soit 1,0 / 0,8 / 0,6 / 0,4 aux coins d'une face pleine : un coin rentrant est
  à 0,4 et non 0,2 ;
* un voisin **sans aucune lumière** (l'intérieur d'un bloc opaque) prête la
  lumière du centre au lieu d'être sauté ;
* la lumière est la **somme des quatre niveaux**, gardée en quarts de niveau
  (0..60) et non arrondie ;
* le verre et les feuilles, cubes pleins, assombrissent un coin comme la
  pierre ;
* la diagonale n'est masquée que si les blocs **un cran au-delà des deux
  côtés, le long de la normale**, arrêtent la vue — et elle est alors
  remplacée par **le côté opposé** du coin le long du premier axe de la face.

Tout cela est **mesuré sur l'`AmbientOcclusionFace` du jeu lui-même**
(directive `ao` de l'oracle, 27 faces) :

| configuration | le jeu | ancienne règle | nouvelle |
|---|---|---|---|
| coin ouvert | 1,0 | 1,0 | 1,0 |
| un bloc à côté | 0,8 | 0,733 | 0,8 |
| un côté + la diagonale | 0,6 | 0,467 | 0,6 |
| L de deux blocs, diagonale pleine | 0,4 | 0,2 | 0,4 |
| L de deux blocs, **diagonale vide** | **0,6** | 0,2 | 0,6 |
| coin de salle (murs de 5 à 7 de haut) | **0,6**, lumière 10,25 | 0,2 | 0,6 / 10,25 |
| même coin, un bloc posé du côté opposé | **0,4**, lumière 10,00 | 0,2 | 0,4 / 10,00 |
| dessous de plateforme | lumière 14,5 · 14,75 · 13,75 (quarts) | entiers | quarts |

La première version de ce document masquait la diagonale derrière deux côtés
opaques quels qu'ils soient (0,4 pour les deux L) : l'oracle a donné 0,6 au L
sans diagonale, et c'est ce qui a fait trouver le test « au-delà des côtés ».
Le coin de salle a ensuite départagé le remplaçant : le centre donnait la
bonne luminosité mais un quart de niveau de lumière en moins ; le côté opposé
tombe juste sur les deux configurations. Vérifié sur les faces du dessus
seulement — l'ordre des axes des autres faces suit le même code et n'est pas
mesuré séparément.

Le sommet passe de 4+4 bits de lumière et 2 bits d'AO à **6+6 bits et 8 bits**,
dans les bits libres : toujours 16 octets.

### 2.3 Le lightmap échantillonné entre deux texels

Les core shaders de 1.20.1 échantillonnent le lightmap à `uv / 256` avec une
lumière emballée en `niveau × 16`, bornée aux centres des texels 0 et 15, en
filtrage linéaire : le niveau 15 tombe **entre** les texels 14 et 15. Nous
visions le centre du texel. *(vérification : section 3)*

### 2.4 Le ciel

Le cadre était effacé à la couleur du brouillard et rien d'autre : tout le ciel
avait la couleur que le jeu n'atteint qu'à l'horizon. Le disque de ciel est
maintenant dessiné : un éventail de huit triangles à 16 blocs au-dessus de
l'œil, rayon 512, dans la couleur du ciel du biome, sous le brouillard de la
passe du ciel. Huit triangles et non un dôme lisse parce que la distance de
brouillard est interpolée par triangle : la forme de l'éventail se voit dans le
dégradé.

### 2.5 Les animations de textures

Lues et conservées depuis longtemps, jamais jouées : eau, lave, feu, portail,
magma, prismarine étaient figés. L'atlas garde maintenant toutes les cellules
de chaque bande animée (déjà mises à l'échelle de l'atlas),
`render::TextureAnimator` calcule à chaque tick client (20 Hz) quelles images
ont changé, recompose leur rectangle et **tous ses niveaux de mip**, et le
client les copie dans l'atlas par régions (`copy_buffer_to_image_region`,
nouveau dans `ov_rhi`). Interpolation `.mcmeta` comprise (RVB linéaire sur les
octets, alpha de l'image courante — non vérifié contre le jeu au-delà de
l'œil).

Écart nommé : l'horloge des animations part au chargement des ressources, chez
vanilla comme chez nous ; les deux phases ne sont donc pas alignées et une
comparaison pixel à pixel de l'eau ne peut pas être exacte.

### 2.6 Les quads translucides triés dans la section

Les sections translucides étaient triées d'arrière en avant ; les quads d'une
section, eux, sortaient dans l'ordre du maillage, si bien que la face lointaine
d'une vitre teintée ou d'une colonne d'eau passait devant la face proche vue
d'un côté sur deux. Chaque section translucide garde maintenant les centres de
ses quads et une plage dans un tampon d'indices par image en vol ; elle est
re-triée (centres du plus loin au plus proche, `render/translucent_sort`)
quand l'œil s'est déplacé d'un bloc depuis le dernier tri, **16 sections au
plus par image, les plus proches d'abord** — le budget du jeu est du même ordre
(quinze). Une image n'écrit que sa propre copie d'une plage, jamais celle que
le GPU lit peut-être encore. Si le tampon (16 Mio par image) est plein, la
couche retombe sur l'ordre du maillage pour l'image, sans rien perdre.

### 2.7 Le modèle que le jeu dessine à chaque position

La carte des écarts d'`aolab` (après les corrections de lumière) montrait le
sol en plein soleil **bruité texel par texel**, avec pourtant exactement les
mêmes niveaux de gris des deux côtés (97, 108, 118, 133) : les bonnes
couleurs sur les mauvais texels. La pierre a quatre variantes (normale et
miroir, chacune tournée de 0 ou 180°), l'herbe quatre rotations, la terre, le
sable… et le jeu en choisit une **par position**. Nous prenions toujours la
première.

L'oracle (`variants`) a imprimé, pour 459 positions, la graine de position du
jeu (`BlockState.getSeed`) et les coordonnées de texture du modèle qu'il a
tiré :

* **459 graines sur 459** égales à notre `render::position_seed` ;
* notre tirage (générateur congruentiel 48 bits du JDK, un `nextLong`, ses
  32 bits bas en valeur absolue, modulo le poids total) correspond **un pour
  un** au choix du jeu pour la pierre (121 positions), la terre (37) et le bloc
  d'herbe (43).

Effet sur la capture : `aolab` passe de 53,8 % à **94,1 %** de pixels
identiques (écart moyen 5,77 → 0,36), au niveau du témoin décalé d'un pixel.

### 2.8 Le décalage des plantes

Les fleurs, l'herbe, les fougères… sont décalées du centre de leur bloc par
position. Les 22 décalages imprimés par l'oracle pour l'herbe de la plaine sont
reproduits **exactement** (graine de position à y = 0 ; quartets 0 et 2 → ±0,25
en x et z, quartet 1 → −0,2..0 en y). La passe `offsets` de l'oracle a ensuite
parcouru **tous les blocs** : 33 ont un décalage — horizontal ±0,25 (±0,125
pour la stalactite), vertical jusqu'à −0,2 pour l'herbe et la fougère, −0,1
pour la petite grande-feuille. La table de `block_models.cpp` est celle-là.

### 2.9 Le crépuscule, le soleil et la lune

* **Couleur de la bande** : reproduit la valeur imprimée
  (`getSunriseColor`) à 12300, 12700 et 23300 à 1e-4 près.
* **Brouillard vers le soleil** : au crépuscule, le brouillard est d'abord
  mêlé à la couleur de la bande selon l'orientation du regard vers le soleil ×
  l'opacité de la bande, *puis* tiré vers le ciel — l'ordre qui redonne
  (187, 100, 71) regardant l'ouest à 12700, là où nous avions (110, 124, 150).
* **La bande** est un éventail de seize triangles sur l'horizon du soleil,
  dessiné après le disque de ciel.
* **Le soleil et la lune** : quadrilatères à 100 blocs, demi-côté 30 et 20,
  ajoutés au ciel (mélange additif, nouveau dans `ov_rhi`), la lune en huit
  phases. Tailles lues sur les captures vanilla : cœur blanc de 102 pixels pour
  le soleil au zénith, disque de 51 pour la lune, à 1708×960 — ce que ces
  demi-côtés donnent avec le FOV de 70° et les textures du pack.

---

## 3. Chiffres

### 3.1 Le coût : p99 de l'image à 12 chunks, sans vsync

Critère M5 (p99 < 20 ms). Visualiseur hors ligne sur la copie du monde, caméra
fixe au-dessus de la plaine, midi, 900 images (les premières jetées comme
échauffement), binaires debug, **A B A B** pour que la dérive d'une machine
partagée avec d'autres agents touche les deux pareil
(`measure_render_parity.py perf`). 2164 sections, 1 840 567 quads.

| binaire | CPU p50 | CPU p99 | CPU max | GPU p50 | GPU p99 |
|---|---:|---:|---:|---:|---:|
| avant, tour 1 | 2,26 | 15,82 | 22,17 | 2,22 | 5,76 |
| après, tour 1 | 2,40 | 16,02 | 17,27 | 2,34 | 6,67 |
| avant, tour 2 | 2,28 | 16,02 | 41,76 | 2,22 | 5,11 |
| après, tour 2 | 2,43 | 9,14 | 15,70 | 2,38 | 4,07 |

(ms). Le p99 reste sous 20 ms ; le coût propre est d'environ **+0,15 ms de
GPU médian** (la passe de présentation plein écran et le disque de ciel) et
+0,15 ms de CPU médian. Les p99 d'un tour à l'autre varient plus que l'écart
entre binaires : c'est la charge des autres agents, pas le rendu.

### 3.2 Ce que l'oracle a imprimé (premier passage : `plains`, `aolab`)

Midi, plaine, 8 chunks, luminosité 0,5 :

| grandeur | le jeu | nous, avant | verdict |
|---|---|---|---|
| couleur du ciel | 0,4706 0,6549 1,0 = **0x78A7FF** | 0x78A7FF | identique |
| couleur du brouillard | 0,7002 0,8112 1,0 = **(179, 207, 255)** | 0xC0D8FF (192, 216, 255) | corrigé : le brouillard est tiré vers le ciel de 1 − (0,25 + 0,75·8/32)^¼ = 0,1867 — les trois canaux tombent juste |
| brouillard du terrain | **115,2 → 128**, cylindrique | 117,76 → 128 (92 %) | corrigé : distance − clamp(distance/10, 4, 64) |
| brouillard du ciel | **0 → 128, cylindrique** | (pas de ciel) | appliqué au disque |
| ombrage par face | bas 0,5 · haut 1,0 · N/S 0,8 · E/O 0,6 | idem | identique |
| hauteur des nuages | 192 | — | nuages non faits |
| lightmap (bloc 0, ciel 15) / (0, 14) | **251 / 224** | 254 / 226 | corrigé (ci-dessous) |

**L'échantillonnage du lightmap entre deux texels est confirmé.** Le sol
plein soleil de la capture vanilla vaut 0,981 du nôtre. Si le jeu lisait le
centre du texel 15 (notre ancienne lecture), son sol serait 4,6 % plus clair
que le nôtre ; il est plus sombre, et la moyenne des texels 14 et 15,
(224 + 251)/2 = 237,5, rend compte de l'écart restant avec notre ancien
lightmap.

**Le lightmap, reconstruit sur les texels du jeu — 12 800 sur 12 800.** Le
balayage de l'oracle (12 heures × 3 réglages de luminosité, plus les frames
des scènes : 50 lightmaps, scintillement relevé à chaque fois) est reproduit
**exactement, texel pour texel**, par une seule structure : lumière de
bloc chaude sur deux polynômes (vert `b·((0,6b+0,4)·0,6+0,4)`, bleu
`b·(0,6b²+0,4)`), scintillement ajouté à 1,5, rappel vers 0,75 de 0,04
**avant et après** l'adoucissement de la luminosité, et **troncature** (arrondir
met 63 % des texels à une unité au-dessus). L'ancien lightmap ne tenait ni les
polynômes, ni le double rappel, ni la troncature. Les deux constantes qui ne
jouent qu'hors du plein jour, que les frames de midi ne départageaient pas,
sont fixées par le balayage nocturne : **plancher du ciel 0,05, bleuissement
0,35** (sans l'un ou l'autre, 46 à 51 % d'exacts seulement). Ce sont donc les
« deux constantes du lightmap non documentées » de la roadmap : mesurées.

### 3.3 Parité par scène

Pourcentage de pixels identiques, à ±8 niveaux sur chaque canal, et écart
moyen par canal (0..255), sur 1708×960. Témoin : la capture vanilla contre
elle-même décalée d'un pixel.

| scène | identiques avant → après | ±8 avant → après | écart moyen avant → après | témoin décalé (identiques / ±8) |
|---|---:|---:|---:|---:|
| `plains` | 0,00 → 1,43 % | 0,09 → 39,58 % | 32,91 → 8,51 | 74,17 / 85,38 % |
| `aolab` | 0,56 → 1,47 % | 23,11 → 47,68 % | 17,24 → 7,66 | 90,15 / 94,36 % |

(« après » = binaire *after3* : scène en espace des octets, lumière lissée,
disque de ciel, animations, tri translucide — **avant** les corrections de
brouillard et de lightmap ci-dessus, dont les chiffres suivent.)

Lecture de la carte des écarts d'`aolab` : **la géométrie coïncide au pixel**
(toutes les arêtes tombent juste — projection, FOV, hauteur de l'œil, ordre des
sommets) et les zones assombries par l'AO et l'ombrage sont dans les ±8 ; ce
qui dépasse, c'est le ciel entier (la couleur du brouillard, corrigée depuis)
et le sol en plein soleil, texel par texel (le lightmap, corrigé depuis). Le
témoin décalé fait beaucoup mieux que nous en pixels identiques parce que
l'image est faite de grands aplats : c'est la colonne ±8 et l'écart moyen qui
mesurent quelque chose ici.

---

## 4. Pièges payés

1. **Une cible de rendu sRGB est une conversion cachée.** Rien dans le shader ne
   le montre ; c'est le format de l'image. Toute arithmétique de couleur
   « comme vanilla » doit se faire sur une cible UNORM.
2. **Le chat reste à l'écran avec `--no-hud`.** Les premières captures
   montraient les réponses aux commandes par-dessus la scène : `--settle-shot`
   coupe toute l'interface pour la capture.
3. **Un serveur debug ne suit pas un carré de 17×17 chunks** (« can't keep up »,
   des chunks au compte-gouttes) : la première capture « stable depuis 3 s » en
   avait 56 sur 289. D'où `--settle-chunks=289`, et un `ov_dedicated` release
   (`--server=`) pour les captures.
4. **Le verrou commun du serveur vanilla est une file de plusieurs heures** avec
   onze agents : les captures de notre client se font sans lui, sur un monde et
   un port à elles (`world-ours`, 25672), pour ne jamais gêner la capture
   vanilla qui attend son tour (`world-vanilla`, 25671).
5. **`/kill @e[type=!minecraft:player]` sur notre serveur a déconnecté le
   joueur** (« write failed: Broken pipe ») : remplacé par
   `/difficulty peaceful` + `doMobSpawning false`.
6. **Le temps figé se perdait** : `ov_netclient` prenait la valeur absolue du
   temps du paquet Update Time et jetait le signe, qui dit
   `doDaylightCycle false`. Notre client faisait donc tourner le soleil pendant
   la capture ; le signe est maintenant transmis (`ClientEvents::time_frozen`).
7. **Le serveur ferme parfois la connexion en pleine scène** (« the server
   closed the connection », 20 à 40 s après l'arrivée, sans rien dans son
   journal) : environ une scène sur cinq sous charge. Le keep-alive est servi
   par le fil réseau du client, pas par la boucle de rendu, donc ce n'est pas
   une image trop lente qui l'affame. Non élucidé ; le script relance une scène
   sans capture jusqu'à trois fois et signale une capture manquante au lieu de
   la taire.
9. **Les deux mondes servis dérivent** : `world-vanilla` a tourné trois passes
   d'oracle, `world-ours` beaucoup plus de captures, et notre serveur fait
   couler les fluides. Au fond de la rivière de la scène `underwater`, le monde
   vanilla n'avait plus d'eau (herbes marines à l'air libre, brouillard d'air
   imprimé par l'oracle) quand le nôtre en avait : **cette scène ne compare pas
   le même contenu** et ses chiffres ne sont pas retenus. Le brouillard sous
   l'eau reste donc non mesuré.
10. **Une capture prise à une autre taille** (l'OS a donné 2940×1618 à une
    fenêtre demandée en 854×480) se compare mal sans le dire : le script la
    rejette et relance.
11. **Un corps de joueur resté d'une connexion coupée** se dessinait dans le
    champ : nos captures se font sans entités (les scènes vanilla n'en ont
    aucune).
12. **Notre serveur ne rallume pas une salle fermée par `/fill`** : la salle de la
   scène `cave` garde la lumière du ciel qu'elle avait à l'air libre. C'est un
   écart du serveur, pas du rendu — mais il fausse la scène : vanilla, qui
   recalcule la lumière côté client à chaque changement de bloc, la voit noire
   et orangée ; nous la voyons en plein jour. La salle creusée dans la
   deepslate (`cave_deep`), prévue pour y échapper, ne s'en sort pas mieux :
   moyenne vanilla (45, 39, 32), chaude et sombre, la nôtre (71, 71, 71), grise
   — une lumière de ciel que la roche n'a jamais eue. Les deux grottes ne
   mesurent donc pas le rendu ; **le rendu de la lumière de bloc est vérifié
   ailleurs** : les colonnes chaudes du lightmap (12 800 texels exacts) et les
   sommes de lumière lissée de la salle (10,25 / 10,00 / 4,25, identiques à
   l'oracle).

---

## 5. Ce qui reste, nommé

Chaque ligne est un écart connu, pas encore corrigé ; son chiffre est en
section 3 quand la mesure le donne.

| écart | où il se voit | pourquoi il reste |
|---|---|---|
| **Nuages** | la moitié du ciel de `sky_noon`, le haut de `plains`, `sunrise` | non faits. Hauteur mesurée (192) ; leur dérive suit un compteur de ticks *du client* : des nuages mal placés feraient un pire score que pas de nuages, il faut d'abord que l'oracle imprime ce compteur pour les aligner |
| **Étoiles** | `sky_midnight`, `plains_night`, `sunrise` (luminosité 0,25 à 0,5 imprimée) | non faites : la position des 1 500 étoiles vient d'un générateur dont la graine n'est documentée nulle part que ce projet puisse lire |
| Couleur du ciel mêlée entre biomes | `jungle`, `underwater` : 1 à 3 niveaux | le jeu mélange la couleur de ciel des biomes voisins ; nous prenons celle du biome sous l'œil |
| Brouillard et vue sous l'eau, eau vue de dessous | `underwater` | scène invalide (contenu différent, piège 9) ; distances du brouillard d'eau non mesurées |
| Forme exacte de la bande du crépuscule | `sunrise`, `dusk_west`, `plains_sunset` | couleur et brouillard mesurés ; l'éventail est jugé sur les captures seulement |
| Disque sombre sous l'horizon | œil sous y = 63 en regardant vers le bas au-delà du terrain | non fait (aucune scène ne le montre) |
| Main et objet tenus, particules, block entities (coffres, panneaux, lits) | — | non commencés dans cette branche |
| Marais en plaques | non mesurable ici (aucun marais dans les régions) | le bruit du modificateur `swamp` n'est documenté nulle part que ce projet puisse lire |
| Filtrage entre niveaux de mip | terrain lointain | fait en linéaire (atlas sRGB) et non sur les octets |
| Phase des animations | eau, lave, feu | l'horloge part au chargement des ressources, des deux côtés ; interpolation `.mcmeta` non vérifiée contre le jeu |
| Axes de l'AO des faces latérales | toute face verticale en coin fermé | la règle du remplaçant est mesurée sur les faces du dessus seulement |
| Multipart à pièces pondérées | aucun bloc de terrain | prend la première pièce |
| Salles fermées par `/fill` | `cave`, `cave_deep` | notre **serveur** ne rallume pas (piège 12) ; pas un écart de rendu |

---

## 6. Reproduire

```bash
# un ov_dedicated release suffit (le serveur ne fait que nourrir les deux clients)
S=…/build/macos-release/bin/ov_dedicated

# le vrai client, sous le verrou commun (≈ 10 min une fois le verrou obtenu)
lockf /tmp/ov-vanilla.lock python3 scripts/measure_render_parity.py vanilla --server=$S
#   → data/vanilla/1.20.1/generated/render-parity/client/{oracle/facts.txt,screenshots/}

# notre client, une exécution par scène (≈ 1 min par scène)
python3 scripts/measure_render_parity.py ours --tag=after --server=$S
#   → run/render-parity/ours-after/*.ppm

# la comparaison, et une carte des écarts par scène
python3 scripts/compare_render_parity.py --tag=after
```
