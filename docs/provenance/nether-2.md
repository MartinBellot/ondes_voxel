# Le Nether, deuxième vague — ses features, ses mobs, ce qui reste de ses structures

*2026-09-11. Graine 1234567890. Oracles : `.scratch/ref-nether` (généré par le vrai serveur 1.20.1
avec `scripts/reference_nether.sh 1234567890 .scratch/ref-nether` ; 1 331 chunks `full`, 44
fichiers de région, jeté une fois lu — le script le régénère), `scripts/measure_nether_mobs.py`
(le vrai serveur, sous `lockf /tmp/ov-vanilla.lock`), `scripts/check_nether_mobs_e2e.py` (notre
serveur, un client sonde).*

`docs/provenance/nether.md` finissait sur « restent les mobs du Nether, 19 features et ses
structures ». Ce document dit ce qui en est fait, avec quels chiffres, et ce qui ne l'est pas.

---

## 1. Les features du Nether

### 1.1 Neuf types, un fichier

`src/ov_worldgen/src/nether_feature.cpp` porte les neuf types de feature configurée que seules les
biomes du Nether nomment ; `feature.cpp` n'y gagne qu'un appel, marqué `nether-2`, à côté de celui
de l'End.

| type | features du jeu qui l'emploient | ce qu'il fait |
|---|---|---|
| `glowstone_blob` | `glowstone_extra` (placées `glowstone`, `glowstone_extra`) | l'origine sous un plafond de netherrack, basalte ou pierre noire, puis 1 500 essais vers le bas, gardés où exactement un des six voisins est déjà de la pierre lumineuse |
| `weeping_vines` | `weeping_vines` | une plaque de bloc de verrue (200 essais) puis 100 colonnes de lianes pendantes |
| `twisting_vines` | `twisting_vines` | `spread_width²` essais, chacun descendu jusqu'au sol, une colonne montante |
| `nether_forest_vegetation` | `crimson/warped_forest_vegetation`, `nether_sprouts` | `spread_width²` essais, l'état tiré **avant** le test de la position |
| `huge_fungus` | `crimson_fungus`, `warped_fungus` | le champignon géant : tige 4 à 13 (doublée une fois sur douze), une fois sur ~16 « énorme » 3×3, chapeau, lumichampignons, lianes sous le chapeau cramoisi |
| `basalt_columns` | `small/large_basalt_columns` | colonnes de basalte autour d'un point de sol, jusque dans l'océan de lave sous y 32 |
| `basalt_pillar` | `basalt_pillar` | un pilier du plafond au sol, sa jupe irrégulière, sa flaque au pied |
| `delta_feature` | `delta` | le delta : bord de magma, lave à l'intérieur, sur un losange de Manhattan |
| `netherrack_replace_blobs` | `basalt_blobs`, `blackstone_blobs` | un losange de Manhattan de netherrack remplacé |

Sources : les pages du Minecraft Wiki de chaque bloc et biome pour les formes et les
probabilités ; l'ordre des tirages et les bornes de boucle, que les pages ne donnent pas, sont
ceux que la mesure du § 1.3 retient. Aucun code tiers ni du jeu.

Deux pièces partagées :

* **`ManhattanWalk`** (`ov/worldgen/nether_feature.hpp`) : `BlockPos.withinManhattan` — anneaux
  de distance croissante, x croissant, puis y, chaque z ≠ 0 suivi aussitôt de son miroir. Le delta
  et les amas s'arrêtent au premier point au-delà de leur portée : l'ordre fait partie de la graine.
  Testé (`test_nether_feature.cpp`) : chaque position une fois, anneau par anneau, le premier anneau
  dans l'ordre exact.
* **La colonne de lianes**, commune aux lianes pendantes, tordues et à celles du champignon
  cramoisi ; l'âge de la tête (17–25, ou 23–25 sous un chapeau) est tiré à la pose de la tête.

Refusé et nommé : `crimson_fungus_planted` / `warped_fungus_planted` (le champignon de la poudre
d'os, qui casse des blocs avec leurs drops — une écriture que `FeatureLevel` ne sait pas faire),
et, toujours, les quatre patchs de champignons (leur survie lit la lumière).

`OV_NETHER_FEATURES=0` refuse à nouveau les neuf types, par leur nom : c'est le bras « avant » de
la mesure, dans le même binaire.

| | avant | après |
|---|---:|---:|
| configured features construites (`FeatureRegistry`) | 150 de 194 | **177** de 194 |
| placed features construites | 183 | **207** |
| placed features d'une biome du Nether nommées et non construites | 19 | **4** (les champignons) |

### 1.2 La mesure

`tools/ov_netherparity --full=100 --full-stride=5` : un chunk `full` sur cinq du monde de
référence, dans l'ordre des fichiers (l'option `--full-stride` est de cette vague : les cent
premiers chunks d'un fichier sont un seul patch, une seule biome), passés par le pipeline complet et
le décorateur du Nether, comparés bloc à bloc. Le tableau donne le rappel par bloc du jeu, et
combien de ce bloc nous avons posé en tout.

### 1.3 Avant / après, même binaire, même échantillon

Cent chunks `full` (un sur cinq), graine 1234567890 ; « avant » est `OV_NETHER_FEATURES=0`.

| bloc du jeu (compte du jeu) | avant | après |
|---|---:|---:|
| **tous les blocs** (6 553 600) | 97,616 % | **97,812 %** (+12 787 blocs) |
| `glowstone` (148) | 0,0 % | **100,000 %** |
| `basalt` (14 428) | 10,8 % | **94,8 %** |
| `blackstone` (16 780) | 88,2 % | **93,2 %** |
| `crimson_stem` (978) | 0,0 % | **73,4 %** |
| `warped_stem` (27) | 0,0 % | 63,0 % |
| `nether_wart_block` (5 812) | 0,0 % | 49,3 % |
| `warped_wart_block` (128) | 0,0 % | 40,6 % |
| `shroomlight` (174) | 0,0 % | 27,6 % |
| `crimson_roots` (552) | 1,4 % | 52,5 % |
| `warped_roots` (18) | 0,0 % | 77,8 % |
| `nether_sprouts` (14) | 0,0 % | 64,3 % |
| `crimson_fungus` (72) | 0,0 % | 40,3 % |
| `weeping_vines` / `_plant` (184 / 412) | 0,0 % / 0,0 % | 16,3 % / 29,4 % |
| `magma_block` (18 548) | 99,77 % | 99,76 % |
| `air` (4 015 039) | 99,887 % | 99,778 % |

La colonne « après » est celle des lianes pendantes de `n` blocs ; le code final les fait de
`n + 1` (ci-dessous), ce qui donne 97,811 % avec l'étage de structures : têtes 29,3 %, tiges
35,7 %, le reste inchangé à quelques blocs près.

Ce qui est établi :

* **La pierre lumineuse est exacte** : 148 blocs sur 148, là où chaque grappe fait 1 500 tirages
  dont chacun dépend de ce que les précédents ont posé. Une erreur d'un seul tirage déplacerait
  toute la grappe.
* **Le basalte** (colonnes, pilier, amas) passe de 10,8 % à 94,8 %, **la pierre noire** (amas) de
  88,2 à 93,2 % ; le magma du delta ne bouge pas parce que le delta pose surtout de la lave, et que
  la lave des chunks `full` est dominée par ce qui a coulé (ci-dessous).
* **La forêt cramoisie est à moitié** : les tiges sont justes aux trois quarts, les chapeaux à
  moitié, les lianes à un quart. `game air / ours nether_wart_block` (3 104) et `game
  nether_wart_block / ours air` (2 753) sont presque symétriques : des chapeaux posés au bon endroit
  avec une autre forme, ou un peu décalés. La forme du chapeau, sa décoration et ses lianes sont
  écrites depuis la documentation ; ce qui diverge n'est **pas localisé**.

**La longueur d'une colonne de lianes pendantes, tranchée par la mesure.** « Une colonne de n
blocs » se lit de deux façons, et la page ne tranche pas. Avec `n` blocs, nous posions **plus de
têtes que le jeu** (211 contre 184) et **moins de tiges** (369 contre 412) : des colonnes trop
courtes. Les deux bras, même binaire (le second arm était une variable d'environnement le temps de
la mesure), même échantillon, étage de structures compris :

| lianes pendantes | colonne de n | colonne de n + 1 |
|---|---:|---:|
| têtes `weeping_vines` justes (184) | 30 (16,3 %) | **54 (29,3 %)** |
| tiges `weeping_vines_plant` justes (412) | 121 (29,4 %) | **147 (35,7 %)** |
| tiges posées par nous | 369 | 464 |
| tous les blocs | 97,813 % | 97,811 % |

L'agrégat ne bouge pas (−0,002 point : quelques verrues voisines changent parce qu'une liane de
plus occupe une case), mais la **position de la tête** — la seule chose qui dise où la colonne
s'arrête — est retrouvée presque deux fois plus souvent : c'est le piège 7 du dépôt, et c'est le
discriminant retenu. Les colonnes pendantes (la feature et celles des chapeaux cramoisis) font
donc `n + 1` blocs ; les lianes tordues, absentes de l'échantillon, gardent `n`. Le surplus de
tiges (464 contre 412) dit que tout n'est pas là : non localisé.

Ce que le chiffre global ne dit pas, nommé :

* **La lave** : 29,7 % des blocs de lave du jeu, avant comme après — `game lava / ours air`,
  105 291 blocs. Deux causes, aucune dans ce mandat : la lave des sources a coulé dans les chunks
  `full` du jeu (`nether.md` § 1.5), et, dans ce monde de référence régénéré, les cellules que le
  carver a creusées sous y 32 sont de la lave chez le jeu et de l'air chez nous (97,875 % sur 200
  chunks `carvers`, contre 99,802 % mesuré par `nether.md` sur le monde d'alors ; le code du carver
  n'a pas changé depuis). **Élucidé depuis** (`nether.md` § 1.6) : ce n'était pas le carver mais
  l'aquifère de l'overworld, fusionné après la mesure du Nether et jamais coupé par
  `aquifers_enabled: false` ; il laissait de l'air dans la mer de lave. Lave 25,198 % → 99,986 %.
* **Les briques du Nether** (11 712) et les **briques de pierre noire polie** (886) : la forteresse
  et le bastion, non construits (§ 2).

---

## 2. Les structures du Nether

Le monde de référence porte, dans ses 44 fichiers de région, **185** départs de fossile du Nether,
9 forteresses, 5 bastions et 8 portails en ruine du Nether (`structures.starts`, lus sans bloc).

### 2.1 Le fossile du Nether — 185 départs sur 185

Le fossile est un gabarit (`nether_fossils/fossil_1` à `_14` du jar serveur, lus à l'exécution par
`TemplateLibrary`, jamais commités), posé tel quel, sans son air ni ses blocs de structure. Sa
**génération** n'est pas une ancre de chunk : la page *Nether Fossil* du wiki le décrit posé sur un
sol de la vallée des âmes à une hauteur tirée ; l'ordre exact des tirages a été fixé par l'oracle
ci-dessous, qui ne laisse aucune place à un tirage de trop ou de moins :

1. aléa de structure du chunk (`setLargeFeatureSeed`, cœur legacy, comme les autres départs) ;
2. x = coin du chunk + `nextInt(16)`, z de même ;
3. une hauteur uniforme entre 32 et 125 (`below_top 2` d'un générateur profond de 128) ;
4. descente dans la **colonne de base** — le bruit seul, avant règles de surface et carvers — tant
   que y > 32, jusqu'au premier air posé sur un bloc plein ; l'origine est à la hauteur du bloc
   plein ; aucun sol trouvé : pas de fossile ;
5. le biome **à ce point** (le tag `#has_structure/nether_fossil`, la vallée des âmes) — et non à
   une ancre de colonne : le Nether a des biomes en trois dimensions, et le placeur ne pouvait pas
   connaître ce point ; le fossile y est désormais sans ancre (`GenerationAnchor::None`) et c'est le
   constructeur qui refuse ;
6. rotation `nextInt(4)`, puis gabarit `nextInt(14)` dans l'ordre des numéros.

`ov_netherparity --fossils` rejoue chaque départ que le jeu a stocké avec **notre** constructeur et
**notre** colonne de base (`ChunkGenerator::is_solid`), et compare le gabarit, la rotation et
l'origine :

| | départs | identiques |
|---|---:|---:|
| gabarit | 185 | **185** |
| rotation | 185 | **185** |
| origine x, z | 185 | **185** |
| **départ entier (gabarit, rotation, x, y, z)** | 185 | **185** |

Le y exact sur 185 départs dit deux choses à la fois : l'ordre des tirages est le bon, et notre
colonne de base du Nether est celle du jeu à chaque colonne interrogée.

### 2.2 Les blocs posés

`ov_netherparity --full` attache désormais au pipeline du Nether l'étage de structures des
gabarits (`StructureStage`, `structures.md` § 14) avec un placeur restreint aux cinq biomes du
Nether ; `--no-structures` le retire. Même échantillon qu'au § 1.3 :

| | sans l'étage | avec l'étage |
|---|---:|---:|
| **`bone_block`** (90 blocs du jeu) | 0,0 % | **100,000 %** (90 posés, 90 justes) |
| tous les blocs | 97,812 % | 97,813 % |

L'étage a construit 120 départs de fossile dans le voisinage des cent chunks (60 poses), et refusé,
chacun par sa raison : 611 départs de fossile sans sol dans leur colonne (le jeu n'en fait pas non
plus : la grille propose un chunk sur deux, la recherche en garde peu), 1 forteresse (`fortress is
not built here`), 5 bastions (`jigsaw is not built here`). Les fossiles sont posés *avant* la
décoration, là où le jeu les pose à l'étape `underground_decoration` : les minerais d'avant ne
remplacent que le netherrack, jamais l'os, et l'écart d'ordre de `structures.md` § 14 n'a pas de
prise ici.

### 2.3 La forteresse et le bastion — non construits, et pourquoi

* **La forteresse** (`nether_bridge`, 9 départs dans le monde de référence, jusqu'à 171 pièces
  pour un seul) est écrite par le jeu **en code**, pièce par pièce — pont, croisements, escaliers,
  salle du générateur de blazes, cour des verrues —, pas en gabarits. La page *Nether Fortress* du
  wiki nomme les pièces et leur rôle, pas leur géométrie bloc à bloc ni l'algorithme qui les
  enchaîne ; les réécrire de mémoire serait traduire du code, ce que ce dépôt s'interdit
  (`structures.md` § 18 a refusé les temples du désert et de la jungle pour la même raison). Refusée
  par nom (`fortress is not built here`).
* **Le bastion** (`bastion_remnant`, 5 départs) est une structure **jigsaw** : 167 gabarits
  `bastion/`, des pools `template_pool/bastion/`, des connecteurs, une profondeur, des processeurs
  de dégradation. Le système jigsaw n'est pas commencé dans ce dépôt (`structures.md` § 10) ; il
  porte aussi les villages, les avant-postes, les cités antiques. Refusé par nom (`jigsaw is not
  built here`).

Leur prix se lit au § 1.3 : 11 712 blocs de briques du Nether et 886 de briques de pierre noire
polie sur l'échantillon, et, côté mobs, ni blazes ni wither squelettes naturels — ils n'apparaissent
que dans la boîte d'une forteresse (`spawn_overrides`).

---

## 3. Les mobs du Nether

### 3.1 La marche, mesurée

`measure_nether_mobs.py stroll` : cinq mobs persistants d'une espèce à la fois sur un superplat
d'herbe, soixante secondes, la statistique de plateau de `measure_mobs2.py` (médiane des pas à
moins de 4 % du 90ᵉ centile). Les piglins, brutes et hoglins portent `IsImmuneToZombification:1b` :
dans l'Overworld ils seraient devenus zombifiés au 300ᵉ tick, au milieu de la mesure. Le
modificateur est `√(v / 2,15859) / attribut` (la loi de `mobs-2.md` § 1.2).

| espèce | attribut | croisière (b/t) | modificateur | n |
|---|---:|---:|---:|---:|
| piglin | 0,35 | 0,095172 | **0,600** | 3 785 |
| piglin brute | 0,35 | 0,095160 | **0,600** | 3 834 |
| hoglin | 0,3 | 0,031083 | **0,400** | 4 363 |
| zoglin | 0,3 | 0,031083 | **0,400** | 4 666 |
| piglin zombifié | 0,23 | 0,114143 | **1,000** | 2 612 |
| wither squelette | 0,25 | 0,134846 | **1,000** | 1 884 |
| strider, sur la lave | 0,175 | 0,066099 | **1,000** | 2 847 |
| strider, froid, sur l'herbe | 0,175 | 0,028796 | **0,660** | 4 367 |
| cube de magma (taille 1) | 0,3 | 0,0857 (bonds) | — | 628 |
| blaze | 0,23 | 0,0494 (vol stationnaire) | — | 542 |
| ghast | 0,7 | 0,289 horizontal, 0,324 en 3D (vol) | — | 5 992 |

Des modificateurs ronds partout où c'est une marche : 0,6, 0,4, 1,0, et 0,66 pour le strider froid
— le « −34 % » que la page *Strider* du wiki donne pour un strider hors de la lave, retrouvé. Le
cube de magma bondit et le blaze plane : leurs chiffres ne sont pas des marches et ne sont pas
convertis.

### 3.2 Le troc des piglins, mesuré

`measure_nether_mobs.py barter` : dix piglins, chacun dans un enclos de verre, un lingot d'or posé
à leurs pieds par tour (`PickupDelay:0`), 24 tours ; chaque objet que le tour laisse est lu puis
retiré à la console. **Deux conditions sans lesquelles le vrai serveur ne troque pas du tout** — le
premier passage a lu zéro objet sur 240 lingots : `mobGriefing` doit être vrai (un piglin ne
ramasse rien sinon), et un piglin invoqué avec NBT n'a pas le droit de ramasser (`CanPickUpLoot:1b`,
piège 32 encore).

240 lingots, 233 objets (7 lingots non ramassés à temps). Comparés aux poids de
`loot_tables/gameplay/piglin_bartering.json` (total 459) :

| objet | poids | vu | attendu | quantités vues |
|---|---:|---:|---:|---|
| obsidienne | 40 | 15 | 20,3 | 1 |
| obsidienne pleureuse | 40 | 25 | 20,3 | 1–3 |
| boule de feu | 40 | 22 | 20,3 | 1 |
| cuir | 40 | 14 | 20,3 | 2–4 |
| sable des âmes | 40 | 24 | 20,3 | 2–8 |
| brique du Nether | 40 | 21 | 20,3 | 2–8 |
| flèche spectrale | 40 | 16 | 20,3 | 6–12 |
| gravier | 40 | 15 | 20,3 | 8–16 |
| pierre noire | 40 | 18 | 20,3 | 8–16 |
| ficelle | 20 | 14 | 10,2 | 3–9 |
| quartz | 20 | 17 | 10,2 | 5–12 |
| potion (résistance au feu, eau) | 18 | 10 | 9,1 | 1 |
| pépite de fer | 10 | 8 | 5,1 | 22–36 |
| perle de l'Ender | 10 | 6 | 5,1 | 2–4 |
| potion jetable de résistance au feu | 8 | 5 | 4,1 | 1 |
| livre (Agilité des âmes) | 5 | 3 (livres enchantés) | 2,5 | 1 |
| bottes de fer (Agilité des âmes) | 8 | 0 | 4,1 | — |

χ² = 22,65 à 16 degrés de liberté (p ≈ 0,12) : la distribution est celle du tableau, chaque
quantité tombe dans l'intervalle de son `set_count`, et le livre sort **enchanté** (le livre
d'Agilité des âmes est un `enchanted_book` sur le fil). Les bottes de fer manquent : un piglin
ramasse une armure qu'on lui lance et la porte — dans un enclos d'un bloc, il ramasse ses propres
bottes. Artefact du montage, nommé, pas mesuré autrement.

Le délai : médiane 6,17 s entre le lingot posé et l'objet apparu, ramassage compris — les 6 s
(120 ticks) de la page *Piglin*.

`gameplay::BarterTable` (`nether_mobs.hpp`) : une entrée pondérée (`nextInt(459)`), puis sa
quantité, puis le niveau d'Agilité des âmes (1 à 3) — lue à l'exécution dans le JSON du data
generator par la session, qui refuse la table entière si elle contient une fonction qu'elle ne
modélise pas (`set_count`, `set_potion` et `enchant_randomly` restreint à Soul Speed, rien
d'autre). Test : 73 000 tirages sur une table jouet tombent à moins de 4 à 6 % de leurs poids.

### 3.3 Les tirs, mesurés

`measure_nether_mobs.py fire` : la sonde en survie (Résistance V, Résistance au feu, soins
instantanés chaque seconde), un blaze à 8 blocs puis un ghast à 20, une minute chacun, chaque
`Spawn Entity` de `small_fireball` / `fireball` reçu noté.

* **Blaze** : 13 petites boules de feu en 60 s, par salves de trois ; dans une salve, 0,28 à 0,35 s
  entre deux tirs (**6 ticks**) ; d'une salve à la suivante 7,8 à 9,9 s. Notre `BlazeVolley` :
  charge 60 ticks, trois tirs espacés de 6, repos 100 — une salve toutes les 178 ticks (8,9 s).
* **Ghast** : 19 boules de feu en 60 s ; l'écart modal est **3,0 s** (2,96 ; 2,98 ; 2,99 ; 3,01 ;
  3,01 ; 3,01), avec des écarts plus longs quand la cible sort de sa vue. Notre `GhastCharge` :
  la charge monte tant qu'il voit sa cible à moins de 64 blocs, tir à 20, reprise à −40 — **60
  ticks**. Le visage « bouche ouverte » (métadonnée 16) dès la charge 11.

La boule de feu elle-même (page *Fireball*) : poussée à chaque tick par son accélération
(direction × 0,1), ralentie de 5 % (20 % dans l'eau) ; la petite fait 5 de dégâts et allume le bloc
qu'elle touche (feu des âmes sur le sable ou la terre des âmes), la grande fait 6 et explose en
puissance 1 avec du feu (`gameplay::Explosions`, le moteur de la TNT). Test : la vitesse limite
vaut 19 × l'accélération, 1,9 b/t pour une poussée de 0,1.

### 3.4 Dans le serveur : un monde d'entités à part

L'`EntityWorld` du serveur est celui de l'Overworld, et chaque système qui le touche — apparition,
noyade, élevage, projectiles, diffusion de chaque mouvement — a été écrit pour ce seul niveau.
Plutôt que d'apprendre une seconde dimension à chacun, le Nether reçoit le sien :
`src/ov_server/src/nether_mobs.{hpp,cpp}`, une session à rappels (comme `tnt_gravity` et
`projectiles`), branchée par huit blocs courts `// ── nether-2 ──` dans `server.cpp` :

| bloc | ce qu'il fait |
|---|---|
| construction | la session et son hôte (`NetherMobHost`), au démarrage si le Nether est permis |
| tick | après les mobs de l'Overworld, sous `players_mutex` et `chunk_mutex` |
| connexion, traversée | les entités du Nether envoyées au joueur qui s'y trouve ou y arrive |
| `hurt_mob` | un coup sur un identifiant réseau du Nether (trois millions et plus) va à la session |
| Interact | un clic droit sur un mob du Nether est mis en file pour le tick (le troc) |
| `/summon` | un joueur debout dans le Nether invoque dans le Nether (`CommandHost::summon_by`) |
| hôte | lecture des blocs, biomes et lumière de bloc du Nether, joueurs du Nether, objets au sol, dégâts |

Ce que fait la session, tick par tick : l'apparition naturelle (`NaturalSpawner` et les listes des
biomes du Nether, `SpawnEnvironment::nether`), les cerveaux (`gameplay::Mob`, les mêmes que ceux
de l'Overworld, sur une `LevelView` du Nether), le troc, les salves et les charges, les boules de
feu, le frisson du strider, la division du cube de magma, la colère, puis la diffusion des
mouvements et des retraits aux seuls joueurs du Nether.

**Les joueurs vus par les cerveaux.** Un but de poursuite cherche sa proie dans le monde
d'entités ; les joueurs n'y sont pas. La session y tient, par joueur du Nether vivant et non
créatif, deux mandataires invisibles (jamais diffusés, hors plafonds) : un de type `player`, proie de
tout hostile, et un de type `villager` — que le Nether ne fait jamais apparaître — pour les seuls
joueurs **sans armure en or**, qui est la proie des piglins et des brutes. Le piglin qui voit un
joueur en armure d'or ne le voit pas comme une proie, sans une ligne de code propre au piglin
dans les buts. Le piglin zombifié n'a aucune proie : sa colère lui en donne une.

**L'horloge de la session** est le nombre de ticks qu'elle a courus, pas l'horloge du serveur :
`TickClock` avale les ticks qu'un serveur lent manque (piège 22), et un piglin admire pendant 120
de *ses* ticks. Mesuré avant ce choix, de bout en bout : 65 et 74 ticks d'âge du monde entre le
lingot et l'objet sur un serveur Debug en retard.

### 3.5 De bout en bout, contre notre serveur

`scripts/check_nether_mobs_e2e.py` : notre `ov_dedicated` (superplat, `--survival`), un client
sonde qui fait tout par le protocole — un portail construit et allumé, la traversée, un opérateur
qui invoque par une commande de joueur, huit lingots donnés d'un clic droit, un blaze et un ghast
invoqués sur le toit de bedrock du Nether (plat, ciel ouvert : leur vue ne dépend pas de la grotte
du portail), et chaque apparition naturelle comptée pendant le séjour.

**8 / 8 contrôles passent** sur le dernier passage (Debug, machine partagée ; le serveur a couru à
16,6 ticks par seconde en moyenne sur le séjour, d'après ses propres paquets Update Time) :

| étape | mesuré |
|---|---|
| traversée | Respawn vers `minecraft:the_nether`, arrivée au portail créé |
| `/summon minecraft:piglin` par le joueur dans le Nether | un `Spawn Entity` de piglin, identifiant 3 000 041 |
| huit lingots d'un clic droit | **8 / 8** montrés en main secondaire (Set Equipment), **8 piles** payées |
| délai du troc | **121, 120, 116 ticks** d'âge du monde entre la main secondaire et l'objet (les trois où deux paquets Update Time encadrent l'événement) ; 6,5 à 8,0 s de mur |
| ce qui est payé | obsidienne pleureuse, pierre noire, brique du Nether, flèche spectrale, obsidienne, ficelle, boule de feu, quartz — tous sur la table |
| blaze à 7 blocs, toit du Nether | 6 petites boules de feu en 14 s : **deux salves de trois**, 0,30 à 0,40 s entre deux tirs, 9,6 s d'une salve à l'autre |
| ghast à 16 blocs | 3 boules de feu en 10 s, **2,98 et 3,01 s** d'écart |
| apparition naturelle, 105 s autour du portail d'arrivée (biome des déchets du Nether) | piglin zombifié 22, piglin 10, strider 5, hoglin 2, ghast 1 |

Le premier passage de ce montage avait échoué sur le délai : 65 et 74 ticks — c'est lui qui a
fait compter la session en ses propres ticks (§ 3.4). Le second a échoué sur la traversée (le
Respawn n'était pas venu en 60 s sur une machine qui générait aussi deux Nether de mesure) :
l'échéance est de 240 s, et elle est dite.

**Composition par biome sur notre serveur** : mesurée **dans un seul biome** (les déchets du
Nether autour du portail de la graine 0 du superplat), pas biome par biome aux positions du vrai
serveur ; la comparaison avec le § 3.6 est qualitative.

### 3.6 Qui apparaît où, chez le vrai serveur

`measure_nether_mobs.py biomes` : un monde généré par le vrai serveur à la graine 1234567890 (jeté
après) ; pour chaque biome du Nether, `execute in minecraft:the_nether run locate biome`, la sonde
posée sur le sol par `spreadplayers … under 100`, tout tué, puis chaque `Spawn Entity` reçu pendant
120 s, le biome de chaque position testé un par un (`execute in the_nether if biome`, un marqueur
par test). Apparitions **dans le biome** (sur toutes celles reçues) :

| biome | apparu dans le biome | total reçu |
|---|---|---:|
| déchets du Nether | piglin zombifié 86, piglin 20, cube de magma 5, strider 4, enderman 2, ghast 1, poulet 1 | 179 |
| forêt cramoisie | piglin 45, hoglin 30, piglin zombifié 3, strider 1 | 183 |
| forêt biscornue | enderman 23, strider 9 | 209 |
| vallée des âmes | squelette 11, enderman 1 | 175 |
| deltas de basalte | cube de magma 15, strider 4 | 188 |

Ce que ces chiffres disent de nos règles :

* **Le ghast est rare partout** où son poids est pourtant fort (50 aux déchets, 50 à la vallée,
  40 aux deltas) : un seul en 120 s aux déchets, aucun ailleurs. C'est l'essai sur vingt de sa
  règle, et le fait qu'un ghast de 4×4×4 trouve rarement la place — les deux sont dans la nôtre.
* **Aux déchets, le piglin zombifié domine** (poids 100, groupes de 4) et le piglin suit (15) ; aux
  **forêts cramoisies**, piglin et hoglin (5 et 9, groupes de 3 à 4). Notre serveur, aux déchets
  (§ 3.5) : piglin zombifié 22, piglin 10, strider 5, hoglin 2, ghast 1 — le même ordre ; les deux
  hoglins viennent d'une forêt cramoisie voisine, que notre comptage d'une minute et demie ne
  sépare pas par biome.
* **La vallée des âmes ne montre que des squelettes** (poids 20, contre 50 au ghast et 1 à
  l'enderman) : l'essai sur vingt du ghast, et les `spawn_costs` du biome, que nous ne lisons pas.
* **Le poulet** aux déchets est le jockey d'un bébé piglin zombifié : ni bébés ni jockeys ici.
* Le strider apparaît partout (poids 60, une passe toutes les 400 ticks) et **toujours sur la
  lave** ; sa règle chez nous est la même.

Ce qui n'est pas mesuré, et nommé : notre serveur **biome par biome** aux mêmes positions (le
Nether de notre serveur Debug met des minutes à générer autour d'un portail, et une campagne à
cinq biomes y aurait tenu une heure de plus) ; les proportions fines, que 120 s par biome ne
permettent pas.

---

## 4. Ce qui n'est pas fait — nommé

| sujet | état |
|---|---|
| **Forteresse du Nether** | non construite. Ses pièces sont écrites **en code** par le jeu, pas en gabarits ; les reconstruire bloc par bloc de mémoire serait traduire du code, et la documentation ne donne pas la géométrie des pièces. Refusée par nom (`fortress is not built here`). 9 départs dans le monde de référence, 11 712 blocs de briques du Nether dans l'échantillon du § 1.3. Sans elle : ni blazes ni wither squelettes naturels (les `spawn_overrides` de la forteresse), ni verrues, ni ses coffres |
| **Bastion** | non construit : c'est une structure **jigsaw** (167 gabarits `bastion/`, les pools `template_pool/bastion/`), et le système jigsaw n'est pas commencé dans ce dépôt (`structures.md` § 10). Refusé par nom (`jigsaw is not built here`) |
| Portail en ruine du Nether | le départ est tiré (les sept variantes, `structures.md` § 13) ; sa hauteur, le basalte et la pierre noire du Nether ne sont pas faits, comme dans l'Overworld |
| Structures dans **notre serveur** | la `StructureStage` n'est attachée par le serveur ni dans l'Overworld ni dans le Nether (`structures.md` § 18) : le fossile est posé par le pipeline de mesure, pas encore par `ov_dedicated` |
| Patchs de champignons du Nether | refusés : leur survie lit la lumière, que la génération ne calcule pas |
| Champignon géant **planté** | refusé : il casse des blocs avec leurs drops |
| Forme du chapeau et des lianes du champignon cramoisi | la moitié des blocs de verrue, un quart des lianes (§ 1.3) : écart non localisé |
| `spawn_costs` | la vallée des âmes et la forêt biscornue limitent leurs apparitions par une « charge » ; non lue |
| Attaques au corps à corps | aucun mob de ce serveur ne blesse un joueur au contact (mandat parallèle) : la brute, le hoglin, le zoglin, le cube de magma, le wither squelette (et son Wither 10 s) poursuivent sans frapper |
| Troc par un lingot **jeté** | seul le lingot donné d'un clic droit est admiré ; un piglin ne ramasse pas les objets au sol ici |
| Colère des piglins | coffre ouvert, or miné près d'eux : non |
| Zombification | un piglin ou un hoglin devient zombifié dans l'Overworld après 15 s ; aucun mob ne traverse de portail dans ce serveur, donc jamais. Le zoglin existe (invocation) |
| Vol du blaze | il marche (modificateur 1,0) : sa chute lente et sa montée vers sa cible ne sont pas modélisées ; ses tirs le sont |
| Vol du ghast | la vérification du trajet avant chaque poussée (`canReach`) est approchée par « la destination est libre » |
| Renvoi d'une boule de feu | frapper une boule de feu la traverse, ne la renvoie pas |
| Mise à feu d'un joueur | la petite boule de feu fait ses 5 de dégâts ; le joueur ne brûle pas ensuite |
| Selle, champignon tordu sur bâton, reproduction du strider | non |
| Bonds du cube de magma | il glisse comme le slime (`mobs-2.md`) |
| Équipement tiré à l'apparition | la main principale seulement (épée d'or ou arbalète du piglin, hache d'or de la brute, épée d'or du zombifié, épée de pierre du wither squelette) ; l'armure, non |
| Rendu dans notre client | aucun mob du Nether n'est dessiné : la table d'espèces du rendu (`ov_render/src/entity_pose.cpp`, `species_of`) ne les nomme pas, `entity_model_name` rend vide et le client ne dessine rien plutôt qu'une boîte par défaut — la même règle qui laisse l'enderman et les douze espèces de `mobs-2` sans modèle. Un client vanilla les voit, puisqu'il ne reçoit que le type |
| Indices de métadonnée | 16 (blaze : drapeaux ; ghast : attaque ; cube : taille) et 18 (strider : frisson) sont pris à la place que le protocole 763 leur donne ; **non relevés sur le fil d'un vrai serveur** |
| `/kill`, `/tp` d'un mob du Nether | les commandes d'entité ne voient que les mobs de l'Overworld |
| Sauvegarde des mobs du Nether | **faite** le 2026-09-11 (`persistance-entites.md`) : `DIM-1/entities`, par un stockage à lui qui lit dans le monde d'entités du Nether ; main principale et taille du cube relues, doublures des joueurs jamais écrites. Restent la colère d'un zombifié (`AngerTime`, `AngryAt` non relus), l'armure, et les boules de feu, non sauvées |

---

## 5. Pièges payés ici

1. **Un piglin invoqué ne troque pas.** Avec `mobGriefing` à faux il ne ramasse rien, et invoqué
   avec du NBT il n'a pas le droit de ramasser (`CanPickUpLoot:1b`, le piège 32 encore) : le
   premier passage de la campagne a lu zéro objet sur 240 lingots, et zéro ressemble à un
   troc cassé.
2. **Un piglin, un hoglin ou une brute invoqués dans l'Overworld deviennent zombifiés au 300ᵉ
   tick**, au milieu de la mesure : `IsImmuneToZombification:1b`.
3. **Un piglin porte l'armure qu'on lui lance** : dans un enclos d'un bloc, il ramasse ses propres
   bottes de fer troquées ; les bottes manquent au comptage.
4. **`TickClock` avale les ticks d'un serveur lent** : une minuterie de mob lue sur l'horloge du
   serveur finit trop tôt en ticks réellement joués. Une session qui compte ses propres ticks ne
   triche pas.
5. **Le biome du Nether est en trois dimensions.** Un fossile lit son biome au point que sa
   recherche trouve ; une ancre de colonne (le toit, ici) aurait lu autre chose. Le placeur ne
   pouvait pas connaître ce point : le constructeur refuse, le placeur laisse passer.
6. **Le bras « avant » d'une mesure doit venir du même binaire** : `OV_NETHER_FEATURES=0` et
   `--no-structures` plutôt qu'un vieux binaire, dont l'arbre de sources a bougé entre-temps.
7. **Les cent premiers chunks `full` d'un monde sont un seul patch** : `--full-stride` est ce qui
   fait voir toutes les biomes du Nether à un échantillon de cent chunks.
8. **Le bac à sable refuse les heredocs** (piège 27) : un test ajouté à un fichier passe par
   l'éditeur, pas par `cat >>`.

---

## 6. Rejouer

```bash
# le Nether de référence (le vrai serveur, sous le verrou JVM de la machine)
lockf /tmp/ov-vanilla.lock scripts/reference_nether.sh 1234567890 .scratch/ref-nether

# les features : après, puis le bras « avant » du même binaire
./build/macos-debug/bin/ov_netherparity --world=.scratch/ref-nether/world/DIM-1 \
    --chunks=0 --biome-chunks=0 --full=100 --full-stride=5
OV_NETHER_FEATURES=0 ./build/macos-debug/bin/ov_netherparity ... --no-structures

# les fossiles : chaque départ du jeu contre le nôtre
./build/macos-debug/bin/ov_netherparity --world=.scratch/ref-nether/world/DIM-1 \
    --chunks=0 --biome-chunks=0 --fossils

# les mobs chez le vrai serveur (marche, troc, tirs, biomes)
lockf /tmp/ov-vanilla.lock python3 scripts/measure_nether_mobs.py stroll barter fire biomes

# de bout en bout contre notre serveur
python3 scripts/check_nether_mobs_e2e.py

# les tests
./build/macos-debug/bin/test_ov_worldgen "[nether]"
./build/macos-debug/bin/test_ov_gameplay "[nether]"
./build/macos-debug/bin/test_ov_server "[nether]"
```

Sources : les JSON du data generator (`worldgen/configured_feature`, `placed_feature`, `biome`,
`structure`, `structure_set`, `loot_tables/gameplay/piglin_bartering.json`,
`dimension_type/the_nether.json`, `normalized/entities.json`), les gabarits `nether_fossils/` du
jar serveur (lus, jamais copiés), le NBT des chunks du monde de référence (`structures.starts`,
blocs), le vrai serveur 1.20.1 pour chaque chiffre de mob, et les pages du Minecraft Wiki nommées
à chaque section. Aucun code tiers, aucun code du jeu.
