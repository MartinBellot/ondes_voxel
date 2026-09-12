# Le rendu des entités — les modèles du vrai client, et chaque espèce mesurée contre lui

*Deuxième version, 2026-09-11. La première (même fichier, historique git) dessinait
dix modèles tirés de la géométrie Bedrock ; celle-ci dessine les ~200 couches du
client Java 1.20.1 lui-même, avec leurs textures par métadonnées, et mesure chaque
espèce contre le vrai client.*

Ce document dit d'où vient chaque nombre, ce qui a été mesuré, de combien, ce que
la première version faisait de faux sans que rien ne le montre, et ce qui reste
**refusé et nommé**.

---

## 1. Un modèle d'entité n'est nulle part dans le pack — il est dans le client qui tourne

Un modèle de bloc est un JSON du resource pack. Un modèle d'entité Java ne l'est
pas : le client le **construit à l'exécution** (`LayerDefinitions.createRoots`) et
le cuit en arbres de `ModelPart`. La première version, faute de pouvoir lire le
code, avait pris la géométrie que Mojang publie pour **Bedrock**
(`bedrock-samples`, fichiers `.geo.json`) et l'avait confrontée à deux oracles.
Elle en tirait dix modèles.

### La source retenue : le client lui-même, pendant qu'il tourne

`scripts/entity_model_oracle.java` lance **le client 1.20.1 de l'utilisateur**
(instance PrismLauncher, sans serveur), attend que ses ressources soient chargées,
et imprime chaque couche telle que le jeu la cuit : pour chaque `ModelPart`, son
pivot, ses rotations, son échelle, sa visibilité, et **les polygones que le jeu
dessine** — quatre sommets, leurs coordonnées de texture, la normale. C'est la
technique des oracles du rendu, de l'inventaire créatif et du chat : les mappings
officiels ne servent qu'à *nommer* les classes et les champs lus par réflexion, et
**chaque nombre est produit par le bytecode du jeu en cours d'exécution**. Aucun
code décompilé n'est lu.

```
lockf /tmp/ov-vanilla.lock python3 scripts/measure_entity_render.py models
  → data/vanilla/1.20.1/entity_java_models.json   (2 070 363 o, 204 couches)
python3 scripts/measure_entity_models.py
  → data/vanilla/1.20.1/entity_models.json         (format 2, 204 modèles)
```

Les deux fichiers sont des **données Mojang**, dans la même catégorie que la
sortie du data generator : générés localement, exclus par `.gitignore`
(`/data/vanilla/*/*.json`), **jamais commités**.

Le même passage lit, du jeu qui tourne, trois choses que le code calcule plutôt
qu'il ne les lit :

* **la table des renderers** : pour chaque type, la classe de renderer, le modèle
  et les calques (`HumanoidArmorLayer`, `SheepFurLayer`, `EnderEyesLayer`,
  `CreeperPowerLayer`, `VillagerProfessionLayer`…) — c'est d'elle que viennent les
  couches de chaque espèce dans `entity_look.cpp` ;
* **les couleurs des teintures et des toisons** : une toison est la couleur de la
  teinture × 0,75, **sauf la blanche**, qui est un gris plat 0,902 (0xE6E6E6) et
  non le blanc 0xF9FFFE de la teinture ;
* **la texture d'overlay 16×16** : les rangées 0–7 sont le rouge d'une entité
  blessée, **(255, 0, 0, 178)** ; les rangées 8–15 le blanc d'un creeper qui va
  exploser, d'alpha 255 à 63 par pas de 12,75. Le shader du jeu fait
  `mix(overlay.rgb, couleur, overlay.a)` : blessée, une entité est tirée à 30,2 %
  vers le rouge, **avant** la lumière.

### Le format 2, et la conversion

Le modèle Java a +Y vers le **bas** (le renderer le retourne par
`scale(-1, -1, 1)`) ; ce projet a +Y vers le haut, −Z devant, +X à gauche. Le
passage est la réflexion F = diag(1, −1, 1). Une rotation conjuguée par F garde son
angle autour de Y et change de signe autour de X et de Z :

```
rotation (ce projet) = (−xRot, yRot, −zRot)   en degrés, composée Z·Y·X
```

— la composition du jeu (`Quaternionf.rotationZYX`). Le pivot d'une partie Java est
relatif à son parent, dans le repère du parent *avant* sa rotation : le pivot
absolu non tourné est la somme des translations, ce que ce projet appelle le pivot
d'un os. Les polygones sont gardés tels quels, dans le sens horaire vu de
l'extérieur (le passage modèle → monde est une réflexion, § 4), leur orientation
décidée par la normale que le jeu donne ; une face d'aire nulle — un cube plat en a
quatre — est jetée et comptée (8 sur le cheval, 24 sur l'arpenteur).

**L'origine n'est pas dans la couche.** Un mob vivant est dessiné 1,501 bloc
au-dessus de ses pieds (24,016 unités) ; un wagonnet et un bateau 0,375 bloc, et
tournent autour de ce point. Ce décalage appartient au renderer : la table des
espèces le porte (`EntityLook::origin`, `lift`), pas le fichier de modèles.

L'ancien format (1, Bedrock) reste lu : un fichier généré avant ce travail charge
et dessine ce que ses noms permettent, et le client le dit au démarrage.

---

## 2. Ce que la première version faisait de faux, et qu'aucune capture n'avait montré

### 2.1 Chaque vache, chaque cochon, chaque mouton se tenait debout sur ses pattes arrière

La géométrie Bedrock `cow.v1.8` décrit le corps **debout** (un pavé de 12×18×10)
avec `"bind_pose_rotation": [90, 0, 0]` : c'est l'animation qui le couche. Le
convertisseur ignorait ce champ. Le corps était donc dessiné vertical, et la table
de la première version le disait sans que personne le lise :

| modèle | hauteur, 1re version | hitbox | hauteur, client Java |
|---|---:|---:|---:|
| vache | **1,812** | 1,40 | 1,562 |
| mouton | **1,812** | 1,30 | 1,375 |
| cochon | 1,438 | 0,90 | 1,000 |

La capture `run/ent-close.png` de la première version montre la vache dressée. Le
dump Java porte la rotation au repos de chaque partie (`body` de la vache :
xRot = 90°) : le défaut ne peut plus revenir, et `test_entity_model.cpp` le tient
(« a rest rotation lays the body on its side »).

### 2.2 Chaque tête regardait en l'air quand celle du jeu regardait par terre

Les formules d'animation publiées (`cos(anim_time × 38.17) × 80`…) sont écrites
dans l'espace du jeu, y vers le bas. La première version les appliquait telles
quelles. Pour les pattes, qui balancent symétriquement, une erreur de signe est
un déphasage d'une demi-foulée : invisible. Pour la tête, c'est un tangage
inversé. `game_rotation()` convertit en un seul endroit, et le test « a
pitched-down head moves the face down » le vérifie par la géométrie plutôt que
par un nombre.

Les pattes d'araignée avaient la même erreur, visible cette fois : le roulis de
repos (±45°, ±33,3°) était du mauvais signe.

---

## 3. Les textures par métadonnées

Une entité arrive avec un type et une liste `(index, valeur)`. Quel index est la
toison d'un mouton et lequel la selle d'un cochon dépend du type ; la table est
`entity_look.cpp`, une fonction pure testée (`test_entity_look.cpp`).

**Les index** viennent de PrismarineJS/minecraft-data (MIT, source nommée par
CLAUDE.md), qui liste pour 1.20 les clés de métadonnées de chaque entité dans
l'ordre des index. Chaque index que ce dépôt avait déjà **mesuré** sur le vrai
serveur y tombe à sa place : toison 17, données de villageois 18, bloc affiché du
wagonnet 11, taille du slime 16, creeper chargé 17, bébé 16. Les autres (variante
du chat 19, collier du loup 20, variante du cheval 18, type du renard, du lapin,
du bateau 11, poses du porte-armure 16–21, cible du faisceau du cristal 8, phase
du dragon 16) viennent de la même liste et sont vérifiés par la mesure du § 9 :
les états que le vrai client a relevés pour chaque scène sont rejoués par notre
table.

| espèce | ce qui choisit la texture ou la couche |
|---|---|
| mouton | 17 : teinture (4 bits bas) → teinte de la toison ; 0x10 tondu → pas de toison |
| villageois | 18 : type (biome) → calque `type/`, métier → `profession/`, niveau → badge `profession_level/` ; bébé : ni métier ni badge ; nitwit : pas de badge ; sous un métier qui porte son chapeau (`.mcmeta` : `hat: full`, ou `partial` sur un type `full`), le calque de type perd **toute la tête** — tête, nez, chapeau, bord : 7 cubes sur 11 dans les sommets du jeu pour un bibliothécaire, et non 9 comme le cachait la première règle |
| villageois zombie | 20 : même chose dans `zombie_villager/` |
| chat | 19 : registre `cat_variant` → `cat/<variante>` ; apprivoisé (17, 0x04) → collier teint (22) |
| loup | apprivoisé → `wolf_tame` + collier teint (20) ; colère (21 > 0) → `wolf_angry` |
| renard | 17 : 1 → `snow_fox` |
| lapin | 17 : 0–5 → brun, blanc, noir, taché, doré, sel ; 99 → `caerbannog` ; nommé « Toast » → `toast` |
| cheval | 18 : couleur (8 bits bas) + marques (8 bits suivants), les marques en calque translucide ; adulte : pattes de poulain cachées ; selle (17, 0x04) |
| creeper | 17 chargé → calque d'énergie `creeper_armor`, additif, qui défile d'un centième de feuille par tick |
| enderman, araignées, dragon | calque des yeux, additif et sans lumière (`rendertype_eyes`) |
| slime | intérieur en découpe, enveloppe `slime#outer` translucide |
| noyé, vagabond | calque extérieur (`drowned#outer`, `stray#outer`) |
| cochon, arpenteur | selle en couche (`pig#saddle`, `strider#saddle`) ; arpenteur hors de la lave → `strider_cold` |
| ghast | 16 chargeant → `ghast_shooting` |
| humanoïdes | armure : casque, plastron, bottes sur `#outer_armor`, jambières sur `#inner_armor`, chaque pièce sur les seuls os qu'elle couvre ; cuir non teint 0xA06540 + son calque `_overlay` |
| wagonnets | 11–13 : bloc affiché personnalisé et son décalage ; sinon le bloc du type (four, TNT, entonnoir, générateur, bloc de commande) |
| bateau | 11 : essence → `boat/<essence>` |
| « Dinnerbone », « Grumm » | dessinés la tête en bas |

Un **bébé** est la moitié de son parent. Mesuré sur les sommets du jeu :

* **quadrupède** (bébé vache) : la tête garde la taille de celle de l'adulte et
  reste **où la moitié du modèle adulte la met** — décalage (0, 0, 0). Le
  `baby_transform` Bedrock (0, 4, 4) la plaçait 2 pixels trop haut et 2 trop en
  arrière (écart de boîte 0,125 bloc) ; avec (0, 0, 0), 0,0005 ;
* **humanoïde** (bébé zombie) : tête ×1,5 relativement au corps, écart 0,0039 ;
* **villageois** : moitié exacte de l'adulte **tête comprise** (hauteur 1,012
  contre 2,023) — pas de grosse tête, contrairement à Bedrock.

Les autres espèces (cochon, mouton, poulet, loup, chat, renard, cheval, lapin,
hoglin) ont chacune leur scène dans la passe A4 (§ 9).

---

## 4. La réflexion, et le test qui l'a trouvée

L'espace du modèle a −Z devant et +X à gauche ; le monde a yaw 0 vers +Z. La
transformation

```
monde = pieds + Ry(180° − yaw) · Rz(roulis) · Ry(yaw du modèle) · k · X · (m + origine) / 16
```

avec X = diag(−1, 1, 1), a un **déterminant de −1**. Un quad dans le sens direct vu
de l'extérieur dans l'espace du modèle sort dans le sens indirect dans le monde.
La première version avait trouvé que quatre faces sur six échouaient au premier
passage — un mob correct, entièrement invisible au culling. Le test « every emitted
face faces outwards » tient toujours, maintenant pour cinq yaws, deux roulis, une
échelle, une origine, et une face au format 2 sous une rotation de repos.

Le culling, lui, est **coupé** : le jeu dessine ses mobs avec
`entity_cutout_no_cull`, et l'intérieur d'un chapeau vu à travers ses texels
transparents fait partie de l'image. L'ordre des sommets reste juste ; il n'est
simplement plus requis.

---

## 5. La lumière d'une entité : trois couleurs, dans l'ordre du jeu

Le core shader `rendertype_entity_cutout` du pack (lu, jamais copié) fait, dans cet
ordre :

```
couleur = texture × couleur_du_sommet          # teinte × ombrage directionnel
couleur.rgb = mix(overlay.rgb, couleur.rgb, overlay.a)
couleur *= lightmap                              # texelFetch : le texel exact
```

Un seul produit ne peut pas porter ça : la rougeur d'une entité blessée serait
teintée par la lumière du bloc, ce que le jeu ne fait pas. Le sommet porte donc
**trois couleurs** (32 octets : position, uv, teinte × ombrage, overlay,
lumière).

**L'ombrage directionnel** n'est pas celui du terrain (1 / 0,8 / 0,6 / 0,5) :
`light.glsl` du pack donne `min(1, (max(0, n·L0) + max(0, n·L1)) × 0,6 + 0,4)`,
avec, dans le monde, L0 = (0,2, 1, −0,7) et L1 = (−0,2, 1, 0,7) normalisés. Une
face latérale d'entité vaut alors 0,74 (nord et sud) ou 0,50 (est et ouest), le
dessous 0,40 — pas 0,8 / 0,6 / 0,5.

Mesuré (ligne `light directions` des faits de la passe A,
`RenderSystem.shaderLightDirections`) : (−0,933 ; 0,263 ; −0,244) et
(−0,104 ; 0,977 ; 0,188), cosinus **0,307** — celui de L0 et L1. Mais ces deux
vecteurs sont **identiques** sous trois caméras différentes (lacet 0, tangage 0 ;
lacet −180, tangage 20 ; lacet 0, tangage 20) : lus à ce moment, ce ne sont pas les
lumières du monde dans le repère de la caméra, et la rotation qui porte L0, L1 sur
eux (91° autour de (−0,34 ; 0,88 ; 0,34)) n'est la vue d'aucune de ces caméras.
⚠ Les directions dans le monde ne sont donc confirmées que par **leur angle**, pas
par cette mesure ; les valeurs 0,74 / 0,50 / 0,40 en découlent et ne sont pas
vérifiées au pixel.

**Les calques additifs** (yeux, énergie) suivent `rendertype_eyes` : texture ×
couleur, sans lumière ni ombrage, **effacés** par le brouillard plutôt que colorés
par lui, ajoutés à ce qui est derrière, sans écrire la profondeur.

**La profondeur se compare « inférieur ou égal »**, comme dans le jeu : les
vêtements d'un villageois sont les mêmes triangles que sa peau, dessinés une
seconde fois par-dessus. Avec le test strict de la première version, aucun calque
de ce genre ne se serait jamais affiché.

---

## 6. Un seul atlas, quatre passes

La première version liait une image par texture et coupait un tirage par image —
tenable pour dix textures. Un village ne l'est pas : un bibliothécaire en porte
quatre, et sept biomes × quinze métiers × cinq niveaux, avec les chats, chevaux,
lapins, bateaux et armures, font **174 textures**. Elles sont empaquetées au
démarrage sur **une feuille** (`render::EntityAtlas`, rangées, sans marge : le
filtrage est au plus proche sans mips), téléversée une fois et **empruntée** par
les passes découpe, translucide et yeux. L'énergie a sa propre texture parce
qu'elle se répète en défilant. Une passe coûte un tirage quel que soit le mélange
d'espèces à l'écran. La feuille contient aussi le texel blanc de la plaque des
noms et les pages de police de l'ASCII imprimable.

---

## 7. Ce que le client fait des paquets

Le client lisait l'index 8 (la pile d'un objet au sol) et jetait le reste.
`ov_protocol/entity_metadata.hpp` lit maintenant **tous** les types de valeur à leur
largeur (un VarLong en VarLong : la version précédente le lisait comme un VarInt et
aurait décalé tout ce qui suit), testés aller-retour contre l'encodeur du serveur
avec une valeur témoin après chaque champ. Et : `Set Equipment` (0x55, drapeau de
continuation sur chaque entrée sauf la dernière), `Entity Event` 3 (la mort),
`Hurt Animation` et `Damage Event` (le flash), `Set Passengers` (0x59, la liste
entière à chaque fois), et l'id de sa propre entité (`Login (play)`).

* **Blessure** : 10 ticks de rouge (0,5 s) après un Damage Event.
* **Mort** : le corps bascule d'un quart de tour sur 20 ticks, rouge ; un serveur
  qui retire l'entité avant la fin voit la bascule finie quand même. ⚠ La forme de
  la courbe (linéaire ici) n'est pas mesurée.
* **Feu** (drapeau partagé 0x01, sauf les espèces que le jeu ne montre jamais en
  feu) : les deux sprites de feu en feuilles **debout**, tournées au seul lacet de
  la caméra, empilées sur la hauteur de la hitbox. Mesuré sur le zombie en feu :
  échelle = largeur × 1,4, feuille haute de 1,4 × échelle, montée de 0,45 ×
  échelle par feuille, la première à (0,3 − 0,02 × ⌊hauteur/échelle⌋) × échelle
  **vers la caméra**, chaque suivante 0,03 × échelle plus loin (0,218 ; 0,193 ; …
  0,092 bloc) ; largeur × 0,9 par feuille. La première version les tournait aussi au
  tangage et les poussait **derrière** l'entité : écart 0,127 bloc ; maintenant,
  sur la scène du zombie en feu, 0,0000 (boîte) et 0,0006 (plus proche voisin).
* **Nom** (index 2, visible si 3, toujours pour un joueur) : les glyphes de la
  police par défaut sur une plaque noire à 25 %, 0,025 bloc par pixel d'interface,
  1/2 bloc au-dessus de la hitbox.
* **Wagonnet** : le modèle, le bloc affiché à 3/4, tourné d'un quart, relevé du
  décalage ; ⚠ le coffre d'un wagonnet à coffre est un block entity dont ce
  cache de modèles n'a pas la géométrie : pas dessiné, nommé. **Passager** : si
  `Set Passengers` nomme notre joueur, la caméra suit le wagonnet (ou le bateau),
  l'œil à 1,62 au-dessus des pieds du passager. Mesuré sur le vrai client (directive
  `facts` de l'oracle, joueur assis) : les pieds du passager **0,35 sous** le
  wagonnet et la caméra **1,27 au-dessus** ; dans un bateau, 0,45 sous et 1,17
  au-dessus. `EntityWorld::riding_eye` prend ces deux nombres.
* **Dragon** : les ailes, les pattes et la mâchoire sur le battement, la tête, les
  cinq segments du cou et les douze de la queue qui suivent l'historique de vol —
  toutes les constantes sont celles que Mojang publie
  (`ender_dragon.entity.json`, scripts de pré-animation ; § pose_dragon). Le
  battement avance de 0,2 / (10·v + 1) × 2^vy par tick, 0,1 perché (phases 5–7) ;
  un dragon sans IA le garde à **0,5** (relevé par la directive `flap` de l'oracle),
  valeur de départ de notre client.

  Mesuré cube par cube contre les sommets du jeu (§ 9), quatre corrections :
  1. **la place** : tout le modèle 2 − h blocs plus haut (h, le « bob » du
     battement, 0,354 à 0,5) et 2 blocs vers son −z — chaque partie était
     décalée de (0 ; 1,648 ; −1,98), le même vecteur partout ;
  2. **l'inclinaison** : le corps tourne de 2h degrés autour de x par son origine —
     sans elle, l'écart de la queue croissait linéairement (0,05 → 0,13) ;
  3. **les ailes** : la gauche est le **miroir** de la droite (le `wing1` publié,
     battant sur −flap, les dessinait à 0,87 bloc d'écart en hauteur) et chacune
     est balayée de 14,32° vers la queue (les os des ailes 1,28 bloc trop en avant
     sans) ;
  4. **le cou en vol stationnaire** : sans phase envoyée, le jeu tient le dragon en
     phase 10 (hover), et son cou y descend de 7,5° par segment — la règle des
     phases perchées (montée = indice du segment). Notre client prend 10 par
     défaut et applique la règle à 5–7 et 10 : la tête était 0,90 bloc trop haut.

  Après quoi les 40 cubes du dragon que ce client dessine tombent à **0,001 bloc**
  des cubes du jeu. ⚠ L'inclinaison en vol (le jeu tourne le dragon de
  10 × (hauteur 5 ticks avant − hauteur 10 ticks avant) degrés autour de x) n'est
  pas mesurable sur une scène immobile : **non faite, nommée**.
* **Cristal de l'End** : à l'échelle 2, sa base de −1,0 à −0,5 sous ses pieds
  (index 9, montrée par défaut) ; puis **trois** parties emboîtées — un cube de
  verre, un second verre dedans à 0,875, le cube à 0,875 de celui-là —, chacune
  tournée encore de 60° autour de la diagonale (1, 0, 1) et de 3° par tick autour
  de y, le tout 2 + j blocs au-dessus des pieds, j = 0,4 (s² + s) − 1,4 avec
  s = ½ sin(0,2 t) + ½. Le modèle cuit n'a qu'une partie `glass` : elle est
  dessinée deux fois, chacune par sa propre application linéaire
  (`EntityPlacement::model_basis`). Mesuré : 96 sommets comme le jeu (72 avant),
  largeur ±0,75 et ±0,83 comme le jeu ; les centres du verre relevés (0,61 et
  ~1,35 bloc) tombent dans la plage de la loi. ⚠ La phase au tick près n'est pas
  vérifiée : l'horloge du cristal part d'une valeur **tirée au hasard** par le
  client, que la passe A4 n'enregistrait pas encore (l'oracle l'écrit maintenant,
  `ct=`). Le jeu ignore le lacet d'un cristal ; ce client aussi.
* **Faisceau du cristal** (index 8, position empaquetée) : un prisme de huit faces
  de `end_crystal_beam.png`, **0,75** de rayon côté cristal et **0,15** côté cible,
  du centre du verre (2 + j) à **deux blocs au-dessus** du centre du bloc visé —
  les deux extrémités relevées dans les sommets du jeu (0,61 et 6,5 pour une cible
  4 blocs plus haut et 3,5 plus loin). La première version : rayon 0,2 constant,
  d'un bloc au-dessus des pieds au centre du bloc.
* **Boule de feu du dragon** : sprite de 2 blocs face à la caméra, de 0,5 sous la
  position à 1,5 au-dessus le long du haut de la caméra (écart 0,0000 après
  correction ; la première version le posait sur les pieds).

---

## 8. La vérification hors ligne

Le vrai client ne tourne que sous le verrou commun, partagé par onze agents. Pour
itérer sur la géométrie sans lui, la passe A enregistre, pour chaque scène, l'état
de l'entité tel que le réseau l'aurait porté — type, position, angles, et les
valeurs synchronisées non par défaut (`SynchedEntityData.getNonDefaultValues`) —
et les **sommets que le jeu émet** pour elle : l'entité est rendue une fois de plus
par l'`EntityRenderDispatcher` du jeu dans un `MultiBufferSource` qui enregistre au
lieu de dessiner (un `java.lang.reflect.Proxy` sur l'interface `VertexConsumer`).

`ov_voxel --connect=… --entity-check=<dossier>` rejoue ces états dans le chemin
exact d'une frame — table des looks, pose, placement, émission — sans serveur ni
GPU, et écrit nos sommets ; `scripts/compare_entity_render.py --check` compare les
deux nuages (boîte englobante, plus proche voisin). C'est elle qui a trouvé presque
toutes les corrections des § 3, 7 et 11 : chacune a été faite, rejouée, et gardée
seulement si l'écart tombait. Les résultats : § 9.

---

## 9. La mesure : chaque espèce contre le vrai client

**Les scènes** (`scripts/entity_scenes.py`) : 68 cases à 32 blocs d'écart sur un
superflat vanilla, sol à y −60, un toit de verre à y −48 et l'heure 13 000 (piège
3 du § 12). Chaque espèce y est invoquée par `/summon` — sans IA, persistante,
face au sud — et une caméra se tient au sud, à une distance et une hauteur de
visée propres à la case. Même monde, même serveur **vanilla** pour les deux
clients :

* **passe A** (le vrai client, `lockf /tmp/ov-vanilla.lock python3
  scripts/measure_entity_render.py scenes --vanilla-only`) : pour chaque case, une
  capture vide, l'invocation, une capture pleine, et le relevé du § 8 — sommets
  émis et état de l'entité (données synchronisées, équipement, âge, horloge du
  cristal, caméra) ;
* **passe B** (notre client, `… scenes --ours-only`) : le même monde, les mêmes
  caméras (`--stand-at`, puis `/tp`), une capture vide (`--no-entities`) et une
  pleine, et nos sommets de la dernière image (`--entity-dump`).

**Les colonnes.** *Silhouette* : ce qui change entre la capture vide et la pleine
(seuil 24 sur 255), dans chaque client ; **IoU** des deux masques, part des
pixels du masque vanilla où nos couleurs tombent à ±8, écart moyen des couleurs
dans ce masque, hauteur de la silhouette en pixels (vanilla / nous). *Sommets* :
nombre de sommets (jeu / nous), écart des boîtes englobantes et, pour chaque
sommet, distance au plus proche de l'autre nuage — ces trois-là par la
vérification hors ligne (états du jeu rejoués), donc à caméra, âge et données
**identiques**, ce qu'aucune capture ne garantit (piège 14 du briefing : le jeu
contre lui-même).

Produit par `scripts/compare_entity_render.py --check` (captures des passes A et
B ; sommets du jeu relevés en passe A, les nôtres par la vérification hors ligne du
§ 8). « — » : pas de sommets à comparer (le bloc qui tombe et l'objet au sol n'en
ont pas dans la vérification hors ligne ; l'orbe d'expérience n'a rien laissé,
ni capture ni sommet, chez aucun des deux clients).

| espèce | IoU | ±8 dans la silhouette | écart moyen | hauteur px V/N | sommets V/N | boîte (blocs) | plus proche (blocs) |
|---|---:|---:|---:|---:|---:|---:|---:|
| zombie | 0.923 | 0.028 | 8.5 | 434/434 | 168/168 | 0.0006 | 0.0007 |
| zombie_armor | 0.827 | 0.040 | 17.3 | 538/530 | 896/488 | 0.0027 | 0.0524 |
| baby_zombie | 0.858 | 0.072 | 8.7 | 212/260 | 168/168 | 0.0005 | 0.0007 |
| named_zombie | 0.838 | 0.029 | 8.5 | 391/478 | 168/168 | 0.0006 | 0.0007 |
| dinnerbone | 0.958 | 0.022 | 9.0 | 395/445 | 168/168 | 0.0006 | 0.0007 |
| burning_zombie | 0.806 | 0.157 | 47.5 | 624/654 | 192/192 | 0.0000 | 0.0006 |
| husk | 0.658 | 0.469 | 8.9 | 467/468 | 168/168 | 0.0006 | 0.0008 |
| drowned | 0.702 | 0.083 | 10.0 | 441/441 | 336/336 | 0.0007 | 0.0007 |
| skeleton | 0.731 | 0.067 | 14.5 | 428/366 | 168/168 | 0.0112 | 0.0026 |
| stray | 0.592 | 0.108 | 12.8 | 433/433 | 336/336 | 0.0115 | 0.0028 |
| wither_skeleton | 0.887 | 0.956 | 3.9 | 447/447 | 168/168 | 0.0110 | 0.0029 |
| creeper | 0.670 | 0.241 | 10.2 | 391/391 | 144/144 | 0.0000 | 0.0000 |
| charged_creeper | 0.561 | 0.022 | 51.7 | 426/426 | 288/288 | 0.0000 | 0.0000 |
| spider | 0.536 | 0.985 | 3.6 | 226/226 | 528/528 | 0.0000 | 0.0000 |
| cave_spider | 0.726 | 0.993 | 2.5 | 233/233 | 528/528 | 0.0000 | 0.0000 |
| enderman | 0.788 | 0.831 | 6.9 | 432/432 | 336/336 | 0.0314 | 0.0063 |
| witch | 0.948 | 0.773 | 5.3 | 521/521 | 360/360 | 0.0000 | 0.0028 |
| slime | 0.553 | 0.000 | 12.7 | 184/263 | 120/120 | 0.0010 | 0.0010 |
| magma_cube | 0.977 | 0.207 | 9.5 | 278/277 | 216/216 | 0.0010 | 0.0009 |
| cow | 0.672 | 0.579 | 8.6 | 342/342 | 216/216 | 0.0000 | 0.0000 |
| baby_cow | 0.680 | 0.391 | 11.9 | 217/298 | 216/216 | 0.0005 | 0.0005 |
| pig | 0.979 | 0.022 | 15.6 | 247/247 | 336/336 | 0.0000 | 0.0000 |
| sheep_lime | 0.589 | 0.064 | 14.3 | 299/317 | 288/288 | 0.0000 | 0.0000 |
| sheep_sheared | 0.769 | 0.033 | 16.3 | 310/310 | 144/144 | 0.0000 | 0.0000 |
| chicken | 0.623 | 0.030 | 28.5 | 196/221 | 192/192 | 0.2473 | 0.0289 |
| wolf | 0.957 | 0.056 | 17.5 | 236/236 | 264/264 | 0.0000 | 0.0000 |
| cat | 0.852 | 0.090 | 17.5 | 185/198 | 264/264 | 0.0000 | 0.0000 |
| fox | 0.980 | 0.014 | 23.3 | 264/264 | 240/240 | 0.0000 | 0.0000 |
| rabbit | 0.912 | 0.156 | 18.4 | 160/159 | 288/288 | 0.0006 | 0.0011 |
| horse | 0.930 | 0.836 | 3.6 | 400/409 | 576/576 | 0.0000 | 0.0215 |
| villager | 0.818 | 0.274 | 11.0 | 439/439 | 960/960 | 0.0000 | 0.0000 |
| villager_desert | 0.783 | 0.530 | 8.6 | 439/439 | 1056/1056 | 0.0000 | 0.0000 |
| baby_villager | 0.454 | 0.387 | 9.6 | 252/252 | 528/528 | 0.0000 | 0.0000 |
| zombie_villager | 0.793 | 0.471 | 8.4 | 476/476 | 864/864 | 0.0006 | 0.0006 |
| piglin | 0.762 | 0.093 | 13.2 | 434/434 | 384/384 | 0.0138 | 0.0038 |
| zombified_piglin | 0.703 | 0.224 | 12.9 | 434/434 | 360/360 | 0.0007 | 0.0010 |
| hoglin | 0.937 | 0.021 | 17.6 | 273/273 | 264/264 | 0.0001 | 0.0114 |
| blaze | 0.598 | 0.008 | 54.4 | 300/298 | 312/312 | 0.0001 | 0.0000 |
| ghast | 0.902 | 0.000 | 20.0 | 295/679 | 240/240 | 0.0571 | 0.1464 |
| strider | 0.518 | 0.108 | 10.7 | 329/349 | 216/120 | 0.0625 | 0.0428 |
| iron_golem | 0.809 | 0.024 | 15.7 | 419/406 | 192/192 | 0.0000 | 0.0000 |
| minecart | 0.920 | 0.017 | 13.9 | 222/221 | 120/120 | 0.0018 | 0.0028 |
| chest_minecart | 0.800 | 0.018 | 14.5 | 284/222 | 192/120 | 0.3450 | 0.0602 |
| furnace_minecart | 0.855 | 0.104 | 12.7 | 286/287 | 144/144 | 0.0018 | 0.0018 |
| tnt_minecart | 0.957 | 0.233 | 11.5 | 287/287 | 144/144 | 0.0013 | 0.0013 |
| hopper_minecart | 0.892 | 0.021 | 13.7 | 237/237 | 248/248 | 0.0012 | 0.0016 |
| boat | 0.384 | 0.098 | 13.9 | 230/238 | 240/216 | 0.0000 | 0.2153 |
| chest_boat | 0.600 | 0.019 | 20.7 | 267/239 | 312/288 | 0.0000 | 0.2047 |
| armor_stand | 0.393 | 0.001 | 16.0 | 463/384 | 288/288 | 0.0001 | 0.0000 |
| end_crystal | 0.662 | 0.013 | 39.9 | 304/492 | 96/96 | 0.2976 | 0.1903 |
| end_crystal_beam | 0.497 | 0.050 | 62.8 | 645/659 | 104/104 | 0.4395 | 0.3031 |
| tnt | 0.000 | 0.000 | 59.9 | 321/272 | 24/24 | 0.0000 | 0.0000 |
| falling_block | 0.698 | 0.000 | 29.6 | 321/321 | —/— | — | — |
| experience_orb | — | — | — | 0/0 | —/— | — | — |
| item | 0.014 | 0.001 | 29.8 | 118/51 | —/— | — | — |
| snowball | 0.784 | 0.010 | 30.6 | 93/101 | 136/8 | 0.0865 | 0.1279 |
| dragon_fireball | 0.884 | 1.000 | 1.3 | 204/194 | 4/4 | 0.0000 | 0.0000 |
| ender_dragon | 0.089 | 0.865 | 3.8 | 180/953 | 3120/2992 | 0.0005 | 0.0003 |
| baby_pig | 0.970 | 0.021 | 17.3 | 220/220 | 168/168 | 0.0005 | 0.0005 |
| baby_sheep | 0.822 | 0.033 | 19.6 | 251/251 | 288/288 | 0.0005 | 0.0005 |
| baby_chicken | 0.852 | 0.074 | 18.0 | 154/154 | 192/192 | 0.1043 | 0.0125 |
| baby_wolf | 0.948 | 0.076 | 17.8 | 204/204 | 264/264 | 0.0005 | 0.0005 |
| baby_cat | 0.787 | 0.112 | 17.3 | 121/144 | 264/264 | 0.0004 | 0.0004 |
| baby_fox | 0.962 | 0.069 | 14.0 | 179/180 | 240/240 | 0.0008 | 0.0007 |
| baby_horse | 0.339 | 0.530 | 10.1 | 233/470 | 576/576 | 0.5107 | 0.1000 |
| baby_rabbit | 0.722 | 0.109 | 19.4 | 134/149 | 288/288 | 0.0007 | 0.0009 |
| baby_hoglin | 0.971 | 0.003 | 18.0 | 280/281 | 264/264 | 0.0008 | 0.0064 |
| skeleton_bow | 0.725 | 0.065 | 15.2 | 428/366 | 632/176 | 0.0273 | 0.0739 |

Ce que le tableau ne cache pas : les colonnes de **sommets** sont justes au
millième de bloc pour la plupart des espèces, et les écarts qui restent sont ceux
du § 11. Les colonnes de **pixels** sont faibles presque partout (±8 sous 10 % pour
la moitié des scènes) : elles mêlent la lumière (§ 5, non vérifiée au pixel), le
piège 14 et la différence des deux résolutions de texture ; elles ne jugent pas la
géométrie.

---

## 10. Ce que ça coûte

**Non mesuré.** `ov_voxel --frames=N` imprime le temps d'émission des entités par
image (ligne `ent p50/p99`) ; la mesure qui jugerait ce travail — 100 entités dans
un rayon de 12 blocs, p50 et p99, avant (main) et après cette branche — demande un
second build de main, une fenêtre et un serveur vivants, et n'a pas été faite. Le
coût reste à mesurer avant d'être affirmé.

---

## 11. Refusé et nommé

* **Les objets tenus** sont dessinés, dans la main que le jeu leur donne (la
  gauche chez un mob gaucher, drapeau 0x02 de l'index 15), placés par le
  `display.thirdperson_righthand` du modèle d'objet lu dans le pack — l'épée du
  zombie armé tombe à **0,01 bloc** du plan du jeu. Mais c'est l'**icône plate** :
  ni la plaque extrudée d'un seizième que le jeu construit, ni le modèle de bloc
  d'un objet-bloc (896 sommets chez le jeu pour cette scène, 488 chez nous). Même
  chose pour l'objet au sol et les projectiles lancés (boule de neige : 136
  sommets contre 8). Pas d'objet dans les bras croisés d'un villageois ni dans la
  main d'un golem de fer ou d'un enderman (bloc porté).
* **Le coffre d'un wagonnet à coffre** : un block entity, dont ce cache de modèles
  n'a pas la géométrie ; le wagonnet est dessiné sans (192 sommets contre 120).
* **Le poulet sans IA** : le client du jeu ne le voit jamais au sol et le fait
  battre des ailes sans fin ; ce client le dessine ailes au repos (écart de boîte
  0,25, le seul qui reste sur un animal adulte).
* **La phase du cristal de l'End** : la loi du bob et de la rotation est celle du
  jeu, les quatre cubes y sont (§ 7) ; la phase au tick près ne l'est pas — son
  horloge part au hasard, non enregistrée par la passe A4. Mesuré (§ 9) : écart de
  boîte **0,30** bloc pour le cristal et **0,44** pour son faisceau, qui part du
  centre du verre ; plus proche voisin 0,19 et 0,30.
* **La tête d'un poulain** : la tête du cheval est un groupe de parties (cou,
  crinière, museau) que le jeu met à l'échelle ensemble ; elle reste à la taille du
  parent — écart de boîte **0,51** bloc, plus proche voisin 0,10 (§ 9). (Le bébé
  hoglin, un temps nommé ici, tombe à 0,0008.)
* **Le ghast** : écart de boîte 0,057 bloc, plus proche voisin **0,146** — les
  sommets sont en même nombre (240) mais pas au même endroit ; cause non cherchée.
* **Le bateau et le bateau à coffre** : boîtes justes (0,0000), mais plus proche
  voisin **0,215** et **0,205**, et 24 sommets de moins que le jeu (240 / 216,
  312 / 288) ; cause non établie.
* **La TNT** : sommets identiques à ceux du jeu (0,0000 partout), mais silhouettes
  **disjointes** (IoU 0) ; non expliqué.
* **L'orbe d'expérience** : sa scène n'a rien laissé — aucune silhouette, aucun
  sommet, chez aucun des deux clients. Rien n'est mesuré pour elle.
* **L'inclinaison du dragon en vol** : § 7.
* **Les faces d'aire nulle** (les soies de l'arpenteur, la membrane des ailes du
  dragon) : le jeu les émet, ce client les jette — invisibles des deux côtés ; c'est
  toute la différence de nombre de sommets de l'arpenteur (216 / 120) et du dragon
  (3 120 / 2 992).
* **Le masque d'eau du bateau** : le jeu l'écrit dans la seule profondeur pour
  chasser l'eau de la coque ; pas dessiné ici, l'eau peut paraître dans un bateau à
  flot.
* **L'éclair** : une ramification aléatoire dessinée par le code, depuis une graine
  que le protocole ne porte pas. Suivi, pas dessiné ; le ciel s'éclaire (météo).
* **Les flèches, le trident, l'hameçon, la laisse, le tableau, les crocs
  d'évocateur, le projectile de shulker, la tête de wither, le crachat de lama, la
  fusée** : dessinés par le code du jeu, pas par une couche du dump. Nommés au
  démarrage, pas dessinés.
* **Le nuage d'effet** (`area_effect_cloud`, le souffle du dragon compris) : le jeu
  ne le dessine **que** par des particules ; ce client n'en émet pas pour lui.
* **Les cadres** : un modèle de bloc (`item_frame.json`) que le cache de modèles ne
  résout que par état de bloc ; pas dessinés.
* **La lumière d'une entité** : un échantillon par entité, pris toujours à
  lumière de bloc 0 et de ciel 15, comme dans la première version.

---

## 12. Pièges payés ici

1. **Une commande de joueur fait au plus 256 caractères.** Au-delà, le client
   refuse de l'encoder et **coupe sa propre connexion** (« String too big ») : la
   première passe a perdu toutes les scènes après le zombie armé (328
   caractères). `Scene.summon` l'affirme maintenant.
2. **`spawn-animals=false` et `spawn-npcs=false` jettent aussi les animaux et les
   villageois invoqués** : toutes les scènes d'animaux de la deuxième passe
   étaient vides.
3. **Le verre laisse passer la lumière du ciel à 15** : sous un toit de verre en
   plein jour, chaque mort-vivant brûlait. Heure 13 000 (les morts-vivants cessent
   de brûler à 12 542, wiki).
4. **Un filtre sur « text »** (pour écarter les étiquettes de nom) attrapait
   `texture[` et jetait toutes les couches du jeu. Le filtre porte sur le nom du
   type de rendu.
5. **Survoler la scène fait expulser le client** (« flying is not enabled ») :
   `allow-flight=true`, et plus de vol stationnaire.
6. **Cacher `root` cache le cou du dragon** : un segment se dessine en cachant
   tout sauf le cou — la racine comprise, aucun segment n'était dessiné (170 quads,
   exactement le corps seul).
7. **L'index 16 n'est pas « bébé » partout** : c'est la taille d'un slime, le
   gonflement d'un creeper, la phase du dragon. Lu comme bébé partout, chaque slime
   de taille 2 était dessiné au quart.
8. **Un dragon sans phase envoyée est en phase 10**, pas 0 — et son cou y suit la
   règle des phases perchées.
9. **Un `Optional<BlockPos>` encodé « présent » sans la position** a fait tomber
   le faisceau du cristal à 1 600 blocs de sa cible dans la vérification hors
   ligne : l'oracle écrit maintenant la position empaquetée.
10. **Un os caché au niveau du look l'est dans toutes les couches** : le chapeau du
    porte-armure nu, caché ainsi, emportait celui du casque. La règle est portée par
    la seule couche nue (`hide_bare_hat`).
11. **La vérification hors ligne sans caméra** tournait les sprites, les noms et le
    feu vers une caméra par défaut : chaque scène enregistre maintenant la sienne
    (ligne `camera` du fichier d'état), et l'âge de l'entité (`t=`), que lisent
    toutes les animations au temps — **plus un tick** : l'oracle rend à un tick
    partiel de 1 (le blaze est passé de 0,08 à 0,0001 bloc).
12. **Un bébé grandit même sans IA** (24 000 ticks, 20 minutes) et **la mèche d'une
    TNT finit par brûler** : entre la passe du vrai client et la nôtre, plus d'une
    heure plus tard, chaque bébé était devenu adulte — et notre client le dessinait,
    à juste titre, adulte. Ces scènes sont invoquées à nouveau juste avant notre
    passage.
13. **Notre client envoyait Client Information dès Login Success** : un serveur
    vanilla encore en état *login* (trois paquets) le lit comme le paquet 8 et
    déconnecte (« Index 8 out of bounds for length 3 »). Envoyé à Login (play),
    comme le client vanilla. Et sa commande `/tp` par le chat est refusée par le
    même serveur (accusé des derniers messages vus) : la passe B place notre joueur
    par la console. Deux défauts du client réseau, trouvés par cette mesure.
14. **Nos deux captures d'une scène (vide, pleine) ne mettent pas la caméra
    exactement au même endroit** : quand le toit de verre est dans le champ (dragon,
    ghast, cristal), tout le toit change entre elles et la silhouette déborde
    (953 pixels de haut pour un dragon de 180). Les colonnes de pixels de ces
    scènes ne mesurent pas l'entité ; celles des sommets, si.
