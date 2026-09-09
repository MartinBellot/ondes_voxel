# Le rendu des entités — modèles, marche, interpolation

## Le problème qu'il règle

Le serveur fait apparaître des mobs, les fait marcher, les fait poursuivre, et
leur A\* est mesuré identique à celui du jeu. **Notre client ne les dessinait
pas.** Ni les mobs, ni les joueurs, ni les objets au sol. Un client vanilla voit
tout ; le nôtre voyait un monde vide dans lequel des choses invisibles se
déplaçaient.

Ce document dit d'où vient la géométrie, comment on a vérifié qu'elle est la
bonne, ce que ça coûte, et ce qui reste **refusé et nommé**.

---

## 1. Un modèle d'entité n'est nulle part dans le pack

Un modèle de bloc est un JSON du resource pack : une chaîne de parents, des
`elements`, des faces qui nomment des sprites d'atlas. Un modèle **d'entité**
n'est rien de tout ça. C'est une hiérarchie d'os, chacun avec un pivot, portant
des boîtes qui ont un décalage, une taille et un coin d'UV sur **une** texture
dont la disposition est le patron déplié de ces boîtes.

Et en Java Edition, **il vit dans le code**. Vérifié plutôt qu'affirmé :

```
run/assets/assets/minecraft/models/block/chest.json    → { "textures": { "particle": … } }
run/assets/assets/minecraft/models/block/oak_sign.json → { "textures": { "particle": … } }
```

Aucune géométrie. Ce projet n'a pas le droit de lire le code décompilé de
Minecraft, donc la source de la géométrie devait être ailleurs.

### La source retenue

**Mojang publie la géométrie vanilla comme *données*, pour Bedrock Edition**,
dans `github.com/Mojang/bedrock-samples`, sous
`resource_pack/models/entity/<espèce>.geo.json` : le format documenté
`bones` → `pivot`, `cubes` → `origin`, `size`, `uv`, `inflate`, `mirror`. Les
animations sont publiées au même endroit, en Molang, en degrés.

Le `LICENSE.md` de ce dépôt dit :

> (c) Mojang AB. All rights reserved. By downloading the files in this
> repository, you agree to the Minecraft End User License Agreement…

C'est donc **exactement** la même catégorie que la sortie du data generator :
donnée Mojang, régénérée localement, **jamais commitée**. La sortie de
`scripts/measure_entity_models.py` va dans `data/vanilla/1.20.1/entity_models.json`,
que `.gitignore` exclut par le motif `/data/vanilla/*/*.json` déjà en place. Un
checkout qui n'a pas lancé le script ne dessine aucune entité et le dit une fois
au démarrage :

```
[WARN] no entity models (data/vanilla/1.20.1/entity_models.json): entity model
       file not found — run scripts/measure_entity_models.py --fetch
```

Le commit est **épinglé** dans le script (`736072450c26a7c67f07b1661f29d9a5ebaa14b1`).
« La branche `main` » n'est pas une source ; un commit en est une.

### Ce que la géométrie Bedrock ne prouve pas toute seule

Rien ne garantit *a priori* que la géométrie Bedrock soit celle de Java. Il
fallait donc la confronter à des oracles locaux — et il y en a deux.

---

## 2. Oracle nº 1 : le patron contre les pixels de la texture Java

Chaque cube déplie sur la feuille un patron de `2·(w+d) × (h+d)` texels. Si un
cube avait la mauvaise taille, le mauvais `uv` ou la mauvaise profondeur, son
patron **tomberait à côté du dessin**. La texture Java du pack est donc un
oracle indépendant, et `scripts/measure_entity_models.py` le mesure texel par
texel :

* **couverture** : part du patron qui tombe sur des texels encrés ;
* **orphelins** : part du dessin de la texture qu'*aucun* patron ne recouvre ;
* **hors** : nombre de faces dont le patron sort de la feuille.

| modèle | os | cubes | feuille | couverture | orphelins | hors |
|---|---:|---:|---:|---:|---:|---:|
| zombie | 10 | 7 | 64×64 | 81,0 % | **0,0 %** | 0 |
| skeleton | 10 | 7 | 64×32 | 59,3 % | **0,0 %** | 0 |
| creeper | 6 | 6 | 64×32 | 100,0 % | **0,0 %** | 0 |
| spider | 11 | 11 | 64×32 | 100,0 % | 1,2 % | 0 |
| cow | 6 | 9 | 64×32 | 99,0 % | **0,0 %** | 0 |
| pig | 6 | 7 | 64×32 | 99,3 % | **0,0 %** | 0 |
| sheep | 6 | 6 | 64×32 | 100,0 % | **0,0 %** | 0 |
| sheep_fur | 6 | 6 | 64×32 | 100,0 % | 1,3 % | 0 |
| chicken | 8 | 8 | 64×32 | 81,6 % | **0,0 %** | 0 |
| humanoid | 17 | 12 | 64×64 | 50,5 % | **0,0 %** | 0 |

**Zéro orphelin sur huit modèles sur dix.** Tout ce que la texture Java dessine
est recouvert par un patron de la géométrie Bedrock. C'est le résultat qui porte
la conclusion : les deux éditions décrivent la même bête.

Une couverture inférieure à 100 % est **attendue** et n'est pas un défaut : ce
sont les calques de surface. Le `hat` d'un zombie, et les **six** calques du
joueur — `hat` (gonflé de 0,5), `jacket`, `leftSleeve`, `rightSleeve`,
`leftPants`, `rightPants` (gonflés de 0,25) — sont des boîtes dont la texture
est presque entièrement transparente dans `steve.png`. Six cubes vides sur les
douze du modèle : le joueur tombe donc logiquement à 50,5 %.

Le squelette à 59,3 % a la même explication plus une seconde : ses membres font
2×12×2, donc leurs patrons sont étroits et ses os sont fins, et son `hat` est
vide.

### Trois désaccords que la mesure a attrapés

**1. La hauteur de feuille du zombie.** Bedrock déclare `textureheight: 32` ;
`zombie.png` de Java fait 64×64 (logiquement — le pack le livre en 128×128).
Croire Bedrock aurait divisé chaque `v` par 32 au lieu de 64 et **doublé toutes
les coordonnées verticales** : un zombie texturé avec le bas de sa propre
feuille. Le script prend donc la taille **logique** de la texture Java (largeur
déclarée par Bedrock, hauteur déduite du rapport d'aspect du fichier, ce qui est
indépendant de la résolution du pack) et signale l'écart :

```
zombie  ⚠ hauteur de feuille : bedrock dit 32, la texture Java dit 64x64
```

**2. La laine du mouton.** Le seul vrai désaccord Bedrock/Java. La laine Bedrock
est découpée dans une feuille de 64×64 (uv `0,32` · `28,40` · `0,48`) ;
`sheep_fur.png` de Java fait 64×32. Sans correction : **36 faces hors cadre** et
5,3 % d'orphelins. Retirer 32 de `v` ramène les trois origines exactement sur
celles du mouton tondu (`0,0` · `28,8` · `0,16`), ce qui est la disposition de
Java — et la mesure le confirme : **0 hors cadre, 100 % de couverture, 1,3 %
d'orphelins**. Le décalage est déclaré explicitement dans `SPECIES`, pas caché
dans une heuristique.

**3. L'héritage du mouton n'est pas une substitution.** `geometry.sheep.v1.8`
hérite de `geometry.sheep.sheared.v1.8` et redéclare `body`, `head`, `leg0..3`
avec les cubes de laine. Java rend deux modèles l'un sur l'autre
(peau puis laine, chacun sa texture). Notre générateur sort donc **deux**
modèles, et `draw_entity` en dessine deux, avec deux textures — ce qui se voit
dans les compteurs : `1 tracked, 2 drawn`.

Les 1,2 % d'orphelins de l'araignée et les 1,3 % du mouton laineux ne sont pas
expliqués et sont laissés tels quels plutôt que rabotés par un seuil.

---

## 3. Oracle nº 2 : la boîte rendue contre la boîte de collision mesurée

Le `.ovpack` porte la boîte de collision de 120 types, **mesurée sur le vrai
serveur**. `ov_voxel --entity-bounds` pose chaque modèle, appelle
`render::emit_entity` et `render::entity_bounds` — **le code exact qu'une frame
appelle**, ce qui est la seule façon qu'une sonde prouve quelque chose (piège
nº 13) — et compare :

```
model      bone  quads    width   hitbox   height   hitbox       y0
zombie       10     42    1.000    0.600    2.031    1.950    0.000   drift 0.000000
skeleton     10     42    0.750    0.600    2.031    1.990    0.000   drift 0.000000
creeper       6     36    0.750    0.600    1.625    1.700    0.000   drift 0.000000
spider       11     66    2.375    1.400    0.500    0.900    0.312   drift 0.000000
cow           6     54    1.438    0.900    1.812    1.400    0.000   drift 0.000000
pig           6     42    1.500    0.900    1.438    0.900    0.000   drift 0.000000
sheep         6     36    1.438    0.900    1.812    1.300    0.000   drift 0.000000
sheep_fur     6     36    1.381        ?    1.578        ?    0.344   drift 0.000000
chicken       8     48    0.688    0.400    0.938    0.700    0.000   drift 0.000000
humanoid     17     72    1.031        ?    2.047        ?   -0.016   drift 0.000000
```

Trois choses s'y lisent, et une quatrième s'y cache.

**`drift 0.000000` partout.** `entity_bounds` et la boîte recalculée depuis les
sommets réellement émis coïncident au bit près. Une boîte qui décrirait un
modèle que le renderer ne dessine pas serait un témoin absurde qui passe la
mesure (piège nº 14) ; ce chiffre l'exclut.

**`y0 = 0.000` sur tout ce qui marche au sol.** Les pieds du modèle sont
exactement à la position de l'entité. Les deux exceptions sont justes, pas des
défauts :
* l'araignée à **+0,312** — son corps flotte, `y = 5` unités = 5/16 = 0,3125
  exactement ;
* le joueur à **−0,016** — ses calques `leftPants`/`rightPants` sont gonflés de
  0,25 unité, soit 0,25/16 = 0,0156 sous le sol. Exactement.

**Les boîtes ne sont pas censées être égales**, et il fallait le dire plutôt que
de forcer la ressemblance. Chez Mojang aussi le modèle déborde de sa hitbox : un
zombie de 2,03 blocs de haut a une hitbox de 1,95 ; une vache mesure 1,44 bloc
de long pour une hitbox carrée de 0,9. Un creeper est plus **court** que sa
hitbox (1,625 contre 1,7). Ce qui serait un défaut, ce serait un facteur deux ou
un axe échangé, et il n'y en a pas.

⚠️ **La largeur est prise face au sud, pas à l'angle de la mesure de dérive.**
Une boîte alignée sur les axes autour d'un modèle tourné est plus grosse que le
modèle ; comparer *celle-là* à une hitbox flatterait ou condamnerait le modèle
par un angle que personne n'a choisi. La dérive, elle, est mesurée à yaw 143° et
position (100,5 ; −60 ; −37,25) volontairement : une boîte calculée à l'origine
face au sud cache à la fois une erreur de translation et un axe échangé.

**Le joueur n'a pas de hitbox mesurée** — c'est l'un des quatre types que le
serveur ne sait pas invoquer (`absent` dans `entities.json`), avec
`evoker_fangs`, `lightning_bolt` et `fishing_bobber`. La colonne est vide plutôt
que remplie avec le 0,6 × 1,8 du protocole, qui n'est pas une mesure de ce dépôt.

---

## 4. La réflexion, et pourquoi les mobs auraient pu être invisibles

L'espace du modèle a **−Z devant** et **+X à gauche** de l'entité. Le monde de
Minecraft a yaw 0 qui regarde vers +Z, et une entité à yaw 0 fait face à +Z. La
transformation est

```
monde = pieds + ( mx·cos y + mz·sin y ,  my ,  mx·sin y − mz·cos y ) / 16
```

et son **déterminant vaut −1**. C'est une *réflexion*, pas une rotation — la
même que vanilla écrit `scale(-1, -1, 1)` sur un modèle en y-descendant. Donc un
quad enroulé dans le sens direct vu de l'extérieur en espace modèle sort dans le
sens *inverse* dans le monde, et le culling arrière le supprime : un mob
correctement construit, entièrement **invisible**.

C'est arrivé. `test_entity_model.cpp` le tient maintenant par un produit
vectoriel plutôt que par une capture d'écran :

> `every emitted face faces outwards after the model-to-world reflection` —
> pour cinq yaws, les six faces d'un cube, la normale du premier triangle doit
> pointer à l'opposé du centre.

Au premier passage, **quatre faces sur six échouaient** (avant, arrière, haut,
bas), et rien à l'écran ne l'aurait dit : les deux faces justes suffisaient à
faire un mob qui « existe ».

---

## 5. La marche se mesure en distance, pas en temps

Le limb swing de vanilla n'est pas une horloge, c'est une **distance parcourue**.
C'est pour ça qu'un mob poussé par un piston ne pédale pas et que deux mobs de
vitesses différentes ne marchent jamais au pas.

Mojang publie les animations vanilla en données Bedrock, en degrés. Toutes les
formules de ce build en viennent, mot pour mot :

| espèce | fichier publié | expression |
|---|---|---|
| quadrupède | `quadruped.animation.json` | `leg0 = math.cos(anim_time × 38.17) × 80.0`, `leg1 = ×(−80)`, `leg2 = ×(−80)`, `leg3 = ×80` |
| humanoïde | `humanoid.animation.json` | `leftarm = tcos0`, `rightarm = −tcos0`, `leftleg = tcos0 × (−1,4)`, `rightleg = tcos0 × 1,4` |
| creeper | `creeper.animation.json` | pattes en `+ − − +`, comme le quadrupède |
| poule | `chicken.animation.json` | `leg0 = +80·cos`, `leg1 = −80·cos`, `body = 90° − this` |
| araignée | `spider.animation.json` | repos `yaw = ±45 / ±22,5`, `roll = ∓45 / ∓33,3` ; marche `|cos(t×76,34 + 90k)| × 22,92` en yaw et `|sin(t×38,17 + 90k)| × 22,92` en roll |

**38,17° = 0,6662 radian**, la constante qui apparaît partout, et
**80° = 1,4 rad**, **57,3° = 1 rad** : les mêmes nombres des deux côtés. C'est ce
qui rend crédible que les deux éditions décrivent la même animation. Un tour
complet de foulée fait donc `360 / 38,17 = 9,43` unités de swing.

⚠️ **Un désaccord possible, nommé.** Le terme en cosinus des pattes de
l'araignée est écrit `−math.abs(math.cos(…))` dans l'animation publiée par
Mojang, donc toujours négatif. Rien dans ce dépôt ne peut dire si Java prend la
valeur absolue au même endroit. La formule publiée est implémentée telle quelle
et l'incertitude est ici plutôt que dans un commentaire de code.

### Les deux constantes qui ne sont pas sourcées

L'accumulateur qui transforme « la distance parcourue en un tick » en swing :

```cpp
target = min(distance_horizontale × 4, 1)
amount += (target − amount) × 0,4
swing  += amount
```

Le **4** et le **0,4** ne sont donnés par aucune source que ce projet puisse
citer. Ils sont ici plutôt que présentés comme mesurés. Ce qu'on peut en dire de
vérifiable, et que `test_entity_model.cpp` vérifie :

> Un zombie a une vitesse de **0,23 bloc par tick** — mesurée, et dans
> `registry.ovpack` sous `generic.movement_speed`. Quatre fois cela vaut 0,92,
> sous le plafond de 1 : un zombie qui marche s'installe donc à **92 % de
> l'amplitude**, il ne sature pas. À 0,92 unité de swing par tick, une foulée de
> 9,43 unités prend **10,25 ticks**, soit 0,51 s à 20 Hz.

Si un jour quelqu'un mesure la cadence réelle d'un zombie vanilla, c'est ce
chiffre-là qu'il faut confronter.

---

## 6. L'interpolation : le serveur envoie des sauts

`ov_server` envoie **des deltas quantifiés au 1/4096 de bloc**, une fois par
tick, et rien entre deux. Dessiner un mob à la dernière position reçue le fait
sauter vingt fois par seconde dès que la frame dépasse 20 Hz. Le client garde
donc deux positions par entité et rend entre les deux, l'alpha étant l'âge du
dernier paquet divisé par un tick, **borné à 1** — extrapoler enverrait un mob à
travers un mur au premier hoquet réseau.

Mesuré, même serveur, même caméra, huit mobs, 900 frames sans vsync, en ne
changeant qu'un drapeau :

| | p50 | p99 | **max** | échantillons |
|---|---:|---:|---:|---:|
| `--no-entity-interpolation` | 0,00000 | 0,09996 | **0,15017** blocs | 7080 |
| interpolation (défaut) | 0,00000 | 0,02521 | **0,02921** blocs | 6984 |

**Le saut maximum entre deux frames consécutives passe de 0,150 à 0,029 bloc,
soit 5,1 fois moins**, et le p99 de 0,100 à 0,025, soit 4,0 fois moins.

Le p50 vaut zéro des deux côtés et c'est attendu : les mobs du banc errent, donc
la plupart des échantillons sont pris sur un mob immobile, pour lequel le
serveur n'envoie rien. C'est **le maximum** qui décrit ce qu'un œil voit comme
une saccade, et c'est pour ça qu'il est donné à côté de la médiane et pas à sa
place. 0,150 bloc est de l'ordre d'un tick entier de déplacement d'un mob qui
marche : sans lissage, tout le trajet du tick arrive sur une seule frame.

Reproduire :

```bash
build/macos-debug/bin/ov_dedicated --world=run/lab --port=25711 \
    --mobs=zombie,cow,creeper,sheep,pig,chicken,skeleton,spider &
build/macos-debug/bin/ov_voxel --connect=127.0.0.1:25711 --frames=900 \
    --stand-at=7.5,-60,16,180,8 --no-vsync --entity-stats
build/macos-debug/bin/ov_voxel --connect=127.0.0.1:25711 --frames=900 \
    --stand-at=7.5,-60,16,180,8 --no-vsync --entity-stats --no-entity-interpolation
```

---

## 7. Ce que le serveur n'envoie pas, et ce que le client en fait

Relevé sur `src/ov_server/src/server.cpp`, pas supposé :

* **Aucune rotation pour un mob.** Le serveur envoie Spawn Entity (0x01) une
  fois, puis seulement des deltas de position (0x2B) et des téléportations
  (0x68). Ni 0x2C, ni 0x2D, et Entity Head Rotation (0x42) **pour les joueurs
  seulement**. L'orientation d'un mob est donc **dérivée ici** de la direction
  dans laquelle il se déplace, avec un seuil de 0,0024 bloc — dix quanta, qu'un
  mob qui marche franchit en un dixième de tick et qu'un mob immobile ne
  franchit jamais. Elle tient sa dernière valeur à l'arrêt. Un serveur vanilla
  enverrait le vrai angle ; c'est un manque **côté serveur**, hors du périmètre
  de ce travail, et il fallait le nommer plutôt que de le maquiller.
* **Aucune tête pour un mob.** Même raison. La tête d'un mob est dessinée dans
  l'axe de son corps ; celle d'un joueur suit 0x42, que le serveur envoie.
* **Aucun drapeau bébé, aucune équipement, aucune pose.** Le serveur n'envoie
  qu'un seul index de métadonnées pour un mob : l'index 9, la vie. Donc pas de
  bras de zombie tendus (c'est le drapeau « agressif »), pas d'arc de squelette,
  pas de creeper qui gonfle, pas de bébés à l'échelle 0,5.
* **Aucune vélocité.** Les ailes d'une poule battent avec sa vitesse verticale
  dans vanilla ; ici elles sont **au repos**, pas animées depuis l'horloge.

---

## 8. Ce que les entités coûtent

Même banc (`run/lab`), même caméra, même rayon 12, `--no-vsync`, 900 frames dont
890 après échauffement, Apple M2 via MoltenVK, **build `macos-debug`** — c'est
la même configuration que la mesure de l'interface, donc les chiffres se
comparent entre eux.

| entités visibles | ent p50 | ent p99 | ent max | gpu p50 | gpu p99 | quads | tirages |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0,000 ms | 0,004 ms | 0,020 ms | 0,40 ms | 2,04 ms | 0 | 0 |
| 10 | 0,500 ms | 1,292 ms | 1,847 ms | 0,52 ms | 0,70 ms | 498 | 9 |
| 100 | 4,856 ms | 6,664 ms | 11,442 ms | 0,67 ms | 1,04 ms | 4 860 | **9** |

Sans les couches de validation Vulkan, les 100 entités tombent à **3,848 ms p50
/ 5,034 ms p99**.

Sur le budget p99 de 17,77 ms du projet dont 0,41 ms d'enregistrement CPU :
**dix mobs coûtent 0,50 ms d'enregistrement CPU, soit un peu plus que ce que
tout le terrain coûte**, et **cent en coûtent 3,8 à 4,9 ms, soit un quart de la
frame**. Le GPU, lui, ne bouge pas : 4 860 quads ne sont rien pour lui.

**Le coût est donc entièrement CPU, et c'est un choix assumé** : la géométrie est
reconstruite chaque frame dans `ov_render`, sans GPU, ce qui est exactement ce
qui permet aux quatorze assertions de `test_entity_model.cpp` d'exister. Un
palette de matrices d'os dans un storage buffer serait plusieurs fois plus
rapide et ne serait vérifiable que par capture d'écran. Le chiffre est donné
plutôt que caché, et il est mesuré en **debug** : rien ici n'est optimisé.

### ⚠️ Le piège qui a coûté un facteur douze en tirages

Une première version coupait un lot dès que la texture **soumise** changeait.
Pour cent mobs de huit espèces parcourus dans l'ordre d'une table de hachage,
cela fait **110 lots** — et le pool de descripteurs d'`ov_rhi` en tient **64 par
frame**. Résultat mesuré :

```
ent  100 tracked, 110 drawn last frame, 4914 quads in 110 draw(s)
[WARN] [rhi] descriptor pool exhausted this frame   (× beaucoup)
```

Passé soixante-quatre, `vkAllocateDescriptorSets` échoue, `rhi` prévient dans le
log et **le tirage se fait avec le dernier jeu lié** : des mobs texturés avec la
peau du voisin, silencieusement. Le coût par frame était 5,305 ms p50.

Le correctif : une soumission est classée dans un **seau par texture** et les
seaux sont concaténés au moment du tirage. Le nombre de tirages est donc borné
par le nombre de textures — dix — quel que soit le nombre d'entités :
**110 → 9**, et 5,305 → 4,856 ms p50. Le compteur est dans les statistiques pour
qu'une régression soit une ligne de sortie et pas une capture d'un zombie
déguisé en vache.

---

## 9. Refusé et nommé

### Le coffre et le panneau — **pas dessinés**

C'était la quatrième demande du mandat et elle n'est **pas** livrée. Voici
exactement pourquoi, parce qu'un manque nommé vaut mieux qu'un manque maquillé.

La géométrie du coffre et du panneau n'a **aucune source autorisée** :

1. **Java la code en dur.** `models/block/chest.json` et
   `models/block/oak_sign.json` ne portent qu'une texture de particule
   (vérifié, cité en § 1). Lire le code décompilé est interdit.
2. **Bedrock ne les publie pas.** Les 192 fichiers de
   `resource_pack/models/entity/` ont été énumérés : il n'y a **ni
   `chest.geo.json` ni `sign.geo.json`**. Bedrock aussi rend ces deux-là par du
   code. `resource_pack/models/` ne contient que `entity/` et `mobs.json`.
3. **Les projets tiers autorisés** — Cuberite, Valence, MCHPRS, Feather,
   Glowstone, PrismarineJS/minecraft-data — sont des **serveurs** ou des données
   de protocole. Aucun ne porte de géométrie d'entité. Et la règle d'or du
   projet interdit de spécifier depuis le code d'un autre projet de toute façon.

Écrire « la cuve fait 14×10×14 en (1, 0, 1) » de mémoire serait exactement
l'« approximé à l'œil » que le mandat interdit.

**Le chemin identifié pour la suite**, parce qu'il existe et qu'il est mesurable :
le patron de `entity/chest/normal.png` **détermine** les tailles et les `uv` des
boîtes. Un patron de boîte `(w, h, d)` en `(u, v)` a une signature très
contraignante — deux rectangles encrés de `w × d` dans la rangée haute, un
rectangle encré de `2w+2d × h` dans la rangée basse, et **deux coins vides** de
`d × d` de part et d'autre de la rangée haute. Chercher la décomposition qui
pave exactement l'encre de la feuille est le même genre d'oracle que
`measure_gui_sprites.py` applique aux fonds de conteneur. Le **placement** dans
le bloc se pince ensuite par la forme de collision du bloc, déjà dans le
registre (un coffre fait 1..15 en x et z, 0..14 en y), et la charnière du
couvercle par la contrainte qu'il tourne du côté opposé à la face qui porte le
loquet.

Ce n'est pas fait. Ce qui est fait, c'est la vérification que la donnée
nécessaire arrive bien jusqu'ici : `parse_chunk_data` remplit
`world::Chunk::block_entities()` et le texte des panneaux du banc est dans ce
NBT dès aujourd'hui. **Il ne manque que la géométrie.** La capture
`run/ent-containers.png` montre le trou : les note blocks, le jukebox, la
trémie et le tonneau sont là, et l'emplacement des coffres à z = 164 est vide.

### Les orbes d'expérience — **pas dessinés**

`Spawn Experience Orb` (0x02) est décodé et l'orbe est suivi ; il n'est pas
dessiné. Sa texture est une bande animée dans `entity/experience_orb.png` et sa
taille avance par paliers selon la valeur qu'il porte ; ni l'une ni l'autre n'a
été mesurée ici, et un carré vert serait une autre chose portant son nom.

### Les plaques de nom — **pas faites**

C'était la cinquième demande, « si le budget le permet ». Il ne l'a pas permis.
Il manque aussi la matière : le serveur n'envoie de nom pour aucun mob (index 2
des métadonnées jamais écrit) et `Player Info Update` (0x3A) n'est pas décodé
côté client, donc même le nom d'un autre joueur n'arrive pas.

### La lumière d'une entité — **plate, et dit ici**

Une entité est éclairée d'un seul échantillon de lightmap pour tout son corps,
ce que fait vanilla. Mais l'échantillon est pris à **lumière de bloc 0, lumière
du ciel 15** : `ov_world` ne porte pas encore de tableau de lumière du ciel lu
depuis le chunk, donc un mob dans une grotte est dessiné aussi clair qu'un mob
dans un champ. C'est un manque, pas un choix.

### L'éclairage directionnel — **différent, et dit ici**

Les faces prennent la teinte plate du terrain (haut 1,0 ; nord/sud 0,8 ;
est/ouest 0,6 ; bas 0,5). Vanilla monte deux lumières directionnelles pour une
entité. Le modèle se lit correctement et **ce ne sont pas les mêmes nombres** —
la même réserve que l'interface pose déjà pour ses items.

### Un objet au sol est dessiné plat

⚠️ Vanilla dessine une pile au sol comme une chose **solide** : un item-bloc est
le modèle du bloc, un outil est son sprite extrudé d'un seizième. Ici c'est la
**vignette d'inventaire** qui est dessinée — les mêmes quads, les mêmes rectangles
d'atlas, le même ombrage plat — sur un plan, tournée autour de Y et flottante.
La silhouette et les textures sont celles de l'objet ; l'épaisseur n'y est pas.
Mesuré : une bûche de chêne lâchée donne `1 tracked, 1 drawn, 6 quads` — les
trois faces visibles de la vignette, chacune en double face pour rester visible
de dos.

⚠️ **Le banc ne sait pas faire tomber un objet facilement.** En créatif casser
un bloc ne donne aucun butin ; en survie le serveur cadence la casse et refuse
un client qui envoie début et fin dans le même tick. Le seul chemin scripté qui
fonctionne est celui de l'interface : poser une pile dans un établi puis fermer
la fenêtre, ce qui fait tomber la pile. C'est un manque du **banc**, pas du
renderer.

---

## 10. Reproduire

```bash
# la géométrie et ses deux oracles
scripts/measure_entity_models.py --fetch

# la boîte rendue contre la boîte de collision, par le vrai chemin de code
build/macos-debug/bin/ov_voxel --entity-bounds

# le banc, sur un port à soi, avec des mobs
build/macos-debug/bin/ov_dedicated --world=run/lab --port=25711 \
    --mobs=zombie,cow,creeper,sheep &

# les mobs, de près
build/macos-debug/bin/ov_voxel --connect=127.0.0.1:25711 --frames=250 \
    --stand-at=3.5,-60,10,180,5 --no-vsync --no-hud --screenshot=run/ent-close.ppm

# ce que ça coûte, et le lissage
build/macos-debug/bin/ov_voxel --connect=127.0.0.1:25711 --frames=900 \
    --stand-at=7.5,-60,16,180,8 --no-vsync --entity-stats
build/macos-debug/bin/ov_voxel --connect=127.0.0.1:25711 --frames=900 \
    --stand-at=7.5,-60,16,180,8 --no-vsync --entity-stats --no-entity-interpolation

# une pile au sol, par le seul chemin que le banc offre
build/macos-debug/bin/ov_voxel --connect=127.0.0.1:25711 --frames=400 \
    --hold=minecraft:oak_log --stand-at=2.5,-60,167,180,30 \
    --use-block=2,-60,164 --move-slots=37,1 --close-at=300
```

`--screenshot` écrit un PPM malgré l'extension ; `sips -s format png` le
convertit. Captures gardées dans `run/ent-*.png` (gitignoré).
