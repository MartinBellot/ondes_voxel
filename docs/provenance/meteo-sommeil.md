# Météo, foudre, lits et sommeil — serveur et rendu client

Avant cette vague, `/weather` existait (un état côté serveur, envoyé en Game Events, persisté
dans `level.dat`, voir `commandes.md` § 4) mais **rien ne tombait** : pas de neige, pas de glace,
pas de foudre, la terre labourée ignorait la pluie, le client ne dessinait ni pluie ni neige et le
ciel restait clair sous l'orage. Aucun lit ne faisait dormir.

Ce dossier décrit ce qui est branché, **mesuré contre le vrai serveur 1.20.1**
(`tools/vanilla/server.jar`) et contre le monde de référence que le vrai jeu a généré
(`run/reference-1234567890`), et ce qui ne l'est pas.

| Mesure | Vanilla | Nous |
|---|---|---|
| Durées tirées par `/weather` (300 par type), uniformes sur les plages du wiki | pluie 12000..23976, orage 3617..15559, éclaircie 12341..179402 ; χ² p = 0,88 / 0,19 / 0,39 | mêmes plages, même tirage uniforme |
| Fin d'une éclaircie minutée | **pluie ET orage** à la fois, 60 / 60 | idem (même machine d'état) |
| Glace sur l'eau des océans gelés, cellules de biome uniformes | 11 285 colonnes | **11 285 / 11 285** (témoin décalé : 81,6 %) |
| Neige de génération, colonnes uniformes (froid, gelé, altitude, autres) | 22 491 colonnes | **99,85 %** (3099/3099, 10 745/10 752, 5515/5540) |
| Couches de neige en 2955 + 6006 ticks, 4096 colonnes | histogrammes `[964 1725 985 422]` et `[427 1165 1248 1256]` | modèle 1/16 × 1/256 par tick : χ² p = **0,986** et **0,271** |
| Gel d'étangs 5×5 (36), par anneau | bord 511/576, intérieur 239/288, centre 25/36 | règle « au bord seulement » : χ² = **1,85** (sans la règle : 24,5) |
| Foudre, un joueur, 208 chunks à moins de 128 blocs | 21 éclairs en 9601 ticks | 1 sur 100 000 par chunk prédit **20,0** |
| Paratonnerre à 40 blocs du joueur | 13 éclairs sur 17 sur le paratonnerre, **un bloc au-dessus** | idem au bout en bout (43 / 43 dans sa portée) |
| Terre labourée à ciel ouvert / sous verre, orage de 16 min | 216/225 humidité 7 / 0 | branché (`is_raining_at`) |
| Conversions par la foudre | cochon → piglin zombifié, villageois → sorcière, creeper chargé (métadonnée 17), mooshroom rouge ⇄ marron sans dégât, tortue tuée | cochon, villageois, creeper, tortue ; mooshroom **nommé** |
| Dégâts d'un éclair | 5 (vache 10 → 5), **un coup par flash** (zombie 20 → 15,06 → 10,14 → 5,22) | un coup par tick allumé, fenêtre d'invulnérabilité de `MobCombat` |
| Seuils jour/nuit d'un lit | 12541 / 12542, 23459 / 23460 ; pluie 12009 / 12010, 23991 / 23992 ; orage de midi : nuit | **identiques** (voir § 5.1) |
| Conditions d'un lit (portée, obstruction, monstres, occupé), pose, réveil, réapparition | 38 cas sur le vrai serveur | **38 / 38** même réponse (§ 5.4) ; l'explosion dans le Nether non mesurée |

Reproduire : `python3 scripts/measure_weather.py [draws sleep precip lightning strike]` (sous
`lockf /tmp/ov-vanilla.lock`), `python3 scripts/analyse_weather.py`,
`python3 scripts/measure_climate.py run/reference-1234567890/world climat.tsv` puis
`OV_CLIMATE_ORACLE=climat.tsv build/macos-debug/bin/test_ov_gameplay "the climate agrees*"`, et
`python3 scripts/check_weather_e2e.py` contre notre serveur.

---

## 1. Les sources

| Sujet | Source | Statut |
|---|---|---|
| Durées de la météo, cycle | Minecraft Wiki, *Weather* ; mesure `draws` | ✅ plages et uniformité mesurées |
| Tick de chunk : 1 chunk sur 16, une colonne, gel, neige, chaudrons | wiki *Snow*, *Ice*, *Cauldron*, *Rain* ; mesure `precip` | ✅ taux, couches, anneaux, chaudrons mesurés |
| Température, modificateur `frozen`, baisse avec l'altitude | wiki *Biome* ; bruit simplex de Gustavson (*Simplex noise demystified*, domaine public) ensemencé par `java.util.Random` comme les autres bruits du jeu | ✅ glace des océans gelés à 100 % ; ⚠️ la baisse d'altitude n'a **pas** d'oracle (§ 2.3) |
| Foudre : 1 sur 100 000, cible, paratonnerre (128 blocs), dégâts 5, conversions | wiki *Lightning*, *Lightning Rod* ; mesures `lightning`, `strike` | ✅ taux, paratonnerre, conversions, dégâts |
| Lits : conditions, messages, pose, saut de nuit, réveil | wiki *Bed*, *Sleep* ; mesure `sleep` | voir § 5 |
| Métadonnées : creeper chargé 17, mooshroom 17, pose 6, lit 14 | capture `strike` (17 booléen, 17 chaîne) ; `sleep` pour 6 et 14 | 17 ✅ ; 6 et 14 voir § 5 |
| Rendu de la pluie et de la neige | description du rendu vanilla (feuilles texturées par colonne, rayon 10 / 5) | ⚠️ non mesuré contre le client réel (§ 7) |

Aucun code de Paper / Spigot / Bukkit n'a été lu.

---

## 2. Le climat — `ov_gameplay/weather.{hpp,cpp}`

### 2.1 Ce qui est calculé

`ClimateNoise` porte les trois bruits à graine fixe que le climat lit : hauteur (1234, octave 0),
plaques gelées (3456, octaves −2..0), information de biome (2345, octave 0). Un bruit simplex 2-D
par octave, table de 256 mélangée et trois décalages tirés de `java.util.Random` dans l'ordre du
jeu ; les octaves non voulues consomment 262 entiers. Une octave positive (qu'aucun bruit du climat
n'utilise) est **refusée** (`supported()` faux) plutôt que devinée.

* **`temperature_at`** : la température du biome ; pour un biome `frozen` (océans et rivières
  gelés), 0,2 dans les plaques où `bruit_gelé × 7 + info(0,2) < 0,3` et `info(0,09) < 0,8` ; puis,
  au-dessus de `niveau de la mer + 17` (80), moins `(bruit_hauteur × 8 + y − 80) × 0,05 / 40`, en
  flottant, dans l'ordre de l'expression.
* **Pluie ou neige** : `température ≥ 0,15` → pluie, sinon neige ; rien si le biome n'a pas de
  précipitations.

### 2.2 L'oracle : le monde que le vrai jeu a généré

La dernière étape de décoration du jeu, `freeze_top_layer`, pose de la glace sur l'eau froide et
de la neige sur le sol froid au sommet de chaque colonne : **les deux mêmes règles** que le tick de
météo, sans la condition « au bord ». Le monde de référence est donc une carte de l'endroit où la
température du jeu est sous 0,15. `scripts/measure_climate.py` en extrait 316 990 colonnes (tous
les biomes froids, toutes les colonnes au-dessus de y 100, une sur seize des autres, chunks `full`
seulement — piège 4 du briefing) ; `test_climate_oracle.cpp` les rejoue dans **notre**
`ClimateNoise` et **nos** `PrecipitationRules`.

| Colonnes | Accord | Témoin (bruit lu 10 000 blocs plus loin) |
|---|---:|---:|
| glace, biome gelé, voisinage uniforme | **11 285 / 11 285** | 9 210 / 11 285 (81,6 %) |
| glace, autres biomes, uniforme | 22 342 / 22 342 | 22 342 / 22 342 |
| neige, biome gelé, uniforme | **3 099 / 3 099** | 3 018 / 3 099 (97,4 %) |
| neige, altitude > 80, uniforme | 10 745 / 10 752 | identique |
| neige, autres, uniforme | 5 515 / 5 540 | identique |
| glace, biome gelé, **bord de biome** | 37 887 / 38 193 (99,20 %) | — |
| neige, altitude, bord | 41 691 / 42 159 (98,89 %) | — |

* **Le bruit gelé est validé** : les plaques d'eau libre d'un océan gelé tombent exactement où le
  jeu les laisse ; décalé, le même bruit se trompe sur une colonne sur cinq.
* **Les bords ne sont pas le bruit** : le jeu lit le biome d'une position à travers un zoom semé
  (le « hashed seed » du paquet Login), que ce projet ne reproduit pas — nous lisons la cellule
  4×4×4 stockée. D'où la catégorie « bord » (au moins une des 27 cellules voisines d'un autre
  biome), séparée, et son écart d'environ 1 %. **Nommé, non corrigé.**
* **32 colonnes** de neige que le jeu n'a pas posée, toutes dans un seul amas de taïga enneigée
  (x ≈ 61 050, z ≈ 33 000). Cause non trouvée.

### 2.3 Ce que l'oracle ne peut pas dire

La baisse de température avec l'altitude n'est vérifiée que **là où elle ne décide rien** : aucune
colonne du monde de référence n'est assez près de la limite des neiges pour que les ±8 blocs du
bruit de hauteur changent le verdict (la catégorie « marge » du test est vide). Le témoin donne
donc le même accord que le bon bruit sur l'altitude. La formule est celle du wiki ; son **bruit**
n'a pas d'oracle ici. Un monde de référence avec des collines venteuses à y 110-130 le fermerait.

### 2.4 L'assombrissement du ciel

`gameplay::sky_darken(jour, pluie, orage)` : la courbe du jour (fraction décalée d'un quart, un
tiers vers un cosinus), **le cosinus de table** de `Mth` (65 536 flottants — piège 1 du
briefing), puis `1 − lumière × (1 − pluie × 5/16) × (1 − orage × pluie × 5/16)`, fois 11. Le serveur
l'utilise maintenant pour les plantes et **pour l'apparition des monstres** : un orage de midi
assombrit le ciel à 5, assez pour que des monstres apparaissent en plein jour, comme dans le jeu.

---

## 3. Le cycle et l'état — réutilisé, pas dupliqué

`cmd::WorldState` (`commands/world_state.*`) portait déjà la machine d'état du wiki : minuteries
`clearWeatherTime`, `rainTime`, `thunderTime`, tirages uniformes, niveaux qui bougent de 0,01 par
tick en flottant même sans `doWeatherCycle`, Game Events 1/2/7/8, persistance dans `level.dat`.
**Elle tournait déjà** : « il ne pleut jamais tout seul » n'était vrai que parce que rien ne
réagissait à la pluie. Cette vague ne l'a pas touchée ; elle l'a mesurée.

* **Les tirages** (`draws`) : `/weather rain|thunder|clear` sans durée, `save-all flush`,
  `level.dat` relu, 300 fois chacun. Minimum et maximum dans les plages du wiki, histogrammes à dix
  cases uniformes (χ² p = 0,884 / 0,187 / 0,390). `rain` écrit sa durée dans les **deux**
  minuteries, `clear` dans `clearWeatherTime` seule — ce que fait `set_weather`.
* **La fin d'une éclaircie** (`weather clear 1`, cycle actif) : **60 fois sur 60, pluie et orage
  commencent ensemble**, avec des durées tirées dans leurs plages (12 093..23 949,
  3 717..15 537). C'est la conséquence de la machine d'état (pendant l'éclaircie les minuteries
  valent 1, et au premier tick après elles basculent toutes deux) ; notre code la reproduit par
  construction.

---

## 4. Le tick de chunk — `ov_server/src/weather_session.{hpp,cpp}`

### 4.1 Où et quand

Les chunks sont ceux que le random tick sélectionne (à moins de 128 blocs d'un joueur, voir
`agriculture.md`), chunks dont les huit voisins sont résidents. Par chunk et par tick, dans l'ordre
du jeu : la foudre (sous un orage, `next_int(100 000) == 0`), puis la précipitation
(`next_int(16) == 0`, une colonne au hasard). La colonne est lue au sommet de MOTION_BLOCKING ; le
biome est celui du sommet ; chaque température à sa hauteur. Le générateur est un
`LegacyRandomSource` à graine fixe : la parité est statistique.

### 4.2 Gel, neige, chaudrons (`PrecipitationRules`)

* **Gel**, qu'il pleuve ou non : eau **source** du bloc eau (pas un escalier inondé, pas une algue),
  lumière de bloc < 10, froid, et **au moins un voisin horizontal qui n'est pas de l'eau**.
* **Neige**, s'il pleut et si `snowAccumulationHeight > 0` : air ou couche de neige, lumière < 10,
  froid, et une couche tiendrait (jamais sur glace, glace compacte, barrière ; toujours sur miel,
  sable des âmes, boue ; sinon face supérieure pleine ou neige de 8). Une couche de plus tant que
  `couches < min(gamerule, 8)`. Le bloc dessous avec `snowy` le prend.
* **Chaudrons** : un tirage flottant par chaudron qui peut se remplir (vide, eau, neige poudreuse),
  **avant** de regarder son niveau ; pluie 1 sur 20 → eau, neige 1 sur 10 → neige poudreuse,
  jusqu'à 3.

Mesures (`precip`, plaine enneigée plate, `randomTickSpeed 0`) :

| | vanilla | modèle / nous |
|---|---|---|
| couches 0..3 au milieu (2955 ticks à 1, puis 3003 à 3) | 964 / 1725 / 985 / 422 | 967,5 / 1721,4 / 990,2 / 417,0 — χ² p = 0,986 |
| couches 0..3 à la fin (+ 6006 à 3) | 427 / 1165 / 1248 / 1256 | 459,3 / 1159,4 / 1206,4 / 1270,8 — p = 0,271 |
| étangs, anneaux bord / intérieur / centre | 511/576, 239/288, 25/36 | 512,4 / 236,0 / 28,1 (z −0,19 / +0,47 / −1,26) |
| étangs sans la règle du bord (témoin) | | 512,4 / 256,9 / 31,9 — χ² 24,5 |
| 400 chaudrons sous la neige | 311 vides, 78 niveau 1, 11 niveau 2 | Poisson λ = 0,219 : 321 / 70 / 9 (χ² ≈ 1,8) |
| 400 chaudrons sous la pluie, ~19 200 ticks | 332 / 56 / 12 | λ = 0,234 : 316 / 74 / 10 (χ² ≈ 5,8, p ≈ 0,06 — limite, nommé) |

Le test d'intégration (`test_weather_session.cpp`) fait passer 3×3 chunks réels par
`WeatherSession::tick` : 198 colonnes enneigées sur 231 en 8192 ticks (modèle 199,7), 509 colonnes
tirées (512), 21 remplissages de chaudron (25,6), et l'étang gèle par les coins.

### 4.3 La pluie sur la terre labourée

`ServerPlantEnvironment::is_raining_at` répondait « jamais ». Il demande maintenant à la session :
il pleut (niveau > 0,2), la position voit le ciel (lumière du ciel 15), rien au-dessus dans
MOTION_BLOCKING, et ce qui tombe là est de la pluie. Vanilla, 16 minutes d'orage : 216 / 225
cellules de terre labourée à ciel ouvert à l'humidité 7 (9 reprises par l'herbe ou redevenues terre
avant la pluie), **0 / 225** sous du verre.

---

## 5. Les lits — `ov_gameplay/sleep.{hpp,cpp}` et la session

### 5.1 Le verdict

`judge_sleep`, dans l'ordre du jeu : dimension où les lits ne marchent pas (explosion de puissance
5 avec feu, centrée sur la tête) → occupé (`block.minecraft.bed.occupied`) → déjà couché ou mort →
dimension non naturelle → trop loin (`too_far_away` : à plus de 3 blocs horizontalement ou 2
verticalement du centre bas des **deux** moitiés) → obstrué (`obstructed` : un bloc qui étouffe
au-dessus de l'une ou l'autre moitié) → **le point de réapparition est posé** → jour (`no_sleep`) →
monstres (`not_safe`, sauf en créatif : une boîte de ±8 blocs, ±5 en hauteur, autour du centre bas
de la tête ; les types de la famille « Monster », sans le piglin zombifié qui n'est pas en colère).

« Jour » est `assombrissement < 4`. Nos seuils, en test unitaire : **12541 jour / 12542 nuit,
23459 nuit / 23460 jour ; sous la pluie 12009 / 12010 et 23991 / 23992 ; un orage de midi est la
nuit, la pluie seule non** — les nombres du wiki, qui ne tombent juste qu'avec le cosinus de table.

### 5.2 Couché, réveillé

* **Se coucher** : les deux moitiés `occupied=true`, le joueur à la tête + (0,5, 0,6875, 0,5),
  métadonnées pose (index 6) = SLEEPING (2) et position du lit (index 14).
* **Le compte** (serveur dédié seulement ; un solo non ouvert se tait, comme le jeu) : à chaque
  changement, `sleep.players_sleeping` [dormeurs, requis] ou `sleep.skipping_night`, **en barre
  d'action, à tous les joueurs** — capturé : deux joueurs, un couché, les deux reçoivent
  `{"translate":"sleep.players_sleeping","with":["1","2"]}` avec le drapeau overlay.
* **La nuit sautée** quand assez de joueurs dorment (`playersSleepingPercentage`, au moins un,
  `ceil(joueurs × % / 100)`) **et** autant dorment depuis 100 ticks : l'heure passe au multiple de
  24 000 suivant (si `doDaylightCycle`), tous se lèvent (animation 2), et si `doWeatherCycle` et
  qu'il pleut, la météo est remise à zéro. Capturé : à 50 %, un joueur couché sur deux →
  `sleep.skipping_night` aux deux, heure relue 41 (24 000 + le temps de la requête).
* **Se lever** : Player Command 2, le jour, ou le lit qui disparaît (sans animation). Debout sur la
  première des douze cases autour du lit qui a un sol et la place d'un joueur, du côté opposé au
  regard ; tourné vers le lit ; Synchronize Position au joueur.
* **Le point de réapparition** : celui de `/spawnpoint` (une seule case, comme le jeu), et le
  message `block.minecraft.set_spawn` — **dans le chat, pas en barre d'action**, capturé — seulement
  quand il change. À la mort : debout à côté du lit ; lit disparu → Game Event 0 (« pas de bloc de
  réapparition »), point oublié, point du monde.

### 5.3 De bout en bout, contre notre serveur

`scripts/check_weather_e2e.py` : `ov_dedicated` en Debug, superflat, piloté par sa console ; la
même sonde que les campagnes vanilla clique le lit en Use Item On.

| Étape | Ce que la sonde a reçu |
|---|---|
| `weather thunder` | Game Event 1 au passage de 0,2, puis 7 et 8 par pas de **0,01** (35 de chaque en 20 s) |
| lit de jour | `block.minecraft.set_spawn` (chat) puis `block.minecraft.bed.no_sleep` (barre d'action) — l'ordre de vanilla |
| lit de nuit | métadonnée 6 = 2 et 14 = (5, −60, 0) ; `sleep.skipping_night` (un joueur sur un) |
| se lever avant (Player Command 2) | pose 0, lit vidé ; debout en (5,5 ; −60 ; −0,5) ; `sleep.players_sleeping` 0/1 |
| 100 ticks couché | Update Time **18 121 → 24 018** ; animation 2 ; debout en (5,5 ; −60 ; −0,5) |
| orage, `OV_THUNDER_CHANCE=4000` | 17 éclairs au sol en 40 s, chacun suivi de son Remove Entities |
| + paratonnerre à 20 blocs | **40 / 40** éclairs à (20,5 ; −55 ; 20,5), le bloc au-dessus du paratonnerre |

**Non vérifié de bout en bout : la réapparition au lit.** Un `/kill` de la sonde par la console
renvoie bien le message de mort, mais ni Set Health ni Respawn ne suivent son Client Command dans
ce banc, avec ou sans lit — le chemin de mort et de réapparition est celui de la session de survie,
cause non établie ici. La décision de cette vague (debout à côté du lit, ou Game Event 0 et le point
du monde quand le lit a disparu) est couverte par `test_weather_session.cpp`, pas par la sonde.

### 5.4 Vanilla

Ce que la campagne `sleep` a établi sur le vrai serveur, par sa seconde sonde (la première perdait
ses clics, piège 2 du § 8) :

* `block.minecraft.set_spawn` arrive **dans le chat** (overlay faux), avant tout le reste ;
* vanilla ajoute alors l'annonce du progrès `adventure.sleep_in_bed` — **nommé, non fait** : ce
  serveur n'a pas de progrès ;
* deux joueurs, un couché : `sleep.players_sleeping` `["1","2"]` en barre d'action **aux deux** ;
* `playersSleepingPercentage 50`, un couché sur deux : `sleep.skipping_night` aux deux, et l'heure
  relue juste après le réveil vaut 41 — le multiple de 24 000 suivant, plus le temps de la requête.

Puis la campagne complète, troisième passe (les deux premières perdues au piège 2 ; celle-ci a
toutes ses téléportations confirmées). **Chaque cas donne la réponse que nos règles donnent** :

| Cas | Vanilla | Nous |
|---|---|---|
| ciel clair 1000 / 12541 / 12542 / 23459 / 23460 | jour / jour / **nuit** / nuit / jour | identique |
| pluie 12009 / 12010 / 23991 / 23992 / 6000 | jour / **nuit** / nuit / jour / jour | identique |
| orage 6000 et 1000 | nuit | identique |
| portée : 3,0 / 3,1 en x et en z ; 2,0 / 3,0 en y | couché / `too_far_away` | identique |
| pierre au-dessus de la tête ou du pied ; verre ; dalle | `obstructed` ; couché ; couché | identique |
| zombie à 8,2 / 8,4 (devant, de côté), 8,2 / 9,2 (derrière), +4,9 / +5,1 | `not_safe` / couché | identique (boîte ±8, ±5) |
| piglin zombifié à 2 blocs | couché | identique |
| creeper, araignée, enderman à 6 blocs | `not_safe` | identique |
| lit pris par l'autre sonde | `block.minecraft.bed.occupied` en barre d'action | identique |
| métadonnées couché / levé | 6 = 2 et 14 = (5, −60, 0) / 6 = 0 et 14 vide | identique |
| 100 ticks couché, cycle actif, pluie | réveil (animation 2), heure 18 021 → **61**, pluie arrêtée et nouvelle attente tirée (rainTime 70 554) | identique (24 000, météo remise à zéro puis retirée) |
| mort avec le lit | réapparu en **(5,5 ; −60 ; −0,5)** | notre position debout, au bloc près |
| mort sans le lit | Game Event **0**, puis le point du monde | identique |

Au premier coucher, vanilla envoie aussi l'annonce du progrès (`chat.type.advancement.task`) —
nommé, non fait. **Non mesuré** : l'explosion d'un lit dans le Nether — la sonde envoyée dans le
Nether n'a reçu aucun paquet Explosion ; la règle reste en test unitaire.

`python3 scripts/analyse_weather.py` réimprime ce tableau depuis `.scratch/weather-oracle.json`,
avec « téléportation confirmée ? » par cas : une passe où ce champ est faux n'a rien mesuré.

---

## 6. La foudre

### 6.1 Le taux et la cible

`lightning` : plaine plate sous un orage de 2 × 8 min, trois sondes qui devaient être à 512 blocs
l'une de l'autre. **Tous les éclairs sont tombés autour de la première** (21 puis 17). L'explication
est le piège 2 du § 8 : `tp` envoie des drapeaux relatifs, la sonde d'alors renvoyait sa position
d'arrivée, et le serveur a remis les trois sondes au point d'apparition — elles se tenaient dans les
**mêmes** 208 chunks. Sur ces 208 chunks : 1 sur 100 000 prédit **20,0** éclairs en 9601 ticks,
21 observés ; 19,9 en 9587, 17 observés.

Cible : un paratonnerre à moins de 128 blocs (distance au carré, le plus proche, et seulement s'il
est le bloc le plus haut de sa colonne) ; sinon une créature vivante dans la colonne (±3 blocs,
jusqu'au plafond) qui voit le ciel, au hasard ; sinon le sol. L'éclair ne tombe que s'il pleut à
cet endroit. Mesuré : avec un paratonnerre à 40 blocs, **13 éclairs sur 17 sont sur lui, à y −55,
le bloc au-dessus du paratonnerre** (y −56) ; les 4 autres au sol, à plus de 128 blocs de lui.

### 6.2 Ce qu'il fait

`strike` (`/summon lightning_bolt` sur des mobs NoAI, difficulté normale) :

| cible | vanilla | ici |
|---|---|---|
| cochon | → piglin zombifié | ✅ `convert_mob` (l'épée d'or n'est pas donnée — nommé) |
| villageois | → sorcière | ✅ |
| creeper | `powered: 1b`, métadonnée **17 booléen** vrai, 5 de dégâts | ✅ `TntGravity::charge_creeper` (explosion à 6) + métadonnée 17 |
| mooshroom | rouge ⇄ marron (métadonnée 17, chaîne), **aucun dégât** | pas de dégât ; **l'échange n'est pas fait** : ce serveur ne porte pas de variante de mooshroom |
| tortue | tuée | ✅ |
| vache | 10 → 5 | ✅ 5 |
| zombie | 20 → 19 → 15,06 → 10,14 → 5,22 | un coup de 5 par tick allumé, armure et fenêtre de `MobCombat` |

Le zombie montre qu'un éclair **frappe à chaque flash** (un à trois flashs, le premier au tick 1,
les suivants jusqu'à dix ticks plus tard) : trois coups de 4,92 (5 moins l'armure du zombie), plus
1 de brûlure. La première version de la session ne frappait qu'une fois par éclair ; corrigée.

La boîte : `±3` autour de l'éclair, `+9` en hauteur. Vertical : un zombie à +8,9 est touché, à
+9,1 non. Horizontal : à 3,2 touché, **mais un zombie à 3,4 en z l'a été et pas à 3,4 en x** — les
deux zombies à 3,2 et 3,4 en x se chevauchaient et ont pu se pousser ; la mesure horizontale n'est
pas concluante et c'est dit.

### 6.3 Nommé et non fait

* **Le feu** — *fait depuis la fusion du feu* (`docs/provenance/feu.md`) : en Normal et Difficile avec
  `doFireTick`, l'éclair allume le bloc frappé à chaque flash, et au premier flash jusqu'à quatre
  blocs de plus, chacun à un décalage aléatoire de −1..1 sur chaque axe (page *Lightning* du wiki).
  Le feu n'est posé que là où il peut tenir (`FireSession::ignite`). ⚠️ Non mesuré contre le vrai
  serveur, et le tirage des décalages vient du générateur de l'éclair, pas de celui du niveau : la
  position des feux secondaires n'est pas bit-exacte.
* **L'impulsion redstone du paratonnerre** (8 ticks) : le moteur ne connaît pas le paratonnerre
  comme source. Dit une fois.
* **Le piège du cheval squelette** (chance = difficulté locale × 1 %) : ni difficulté locale ni
  cheval squelette ici. Dit une fois.
* **La désoxydation du cuivre** frappé.
* Les **items au sol** sont brûlés (5 PV), par la même rafle que les explosions.

---

## 7. Le rendu client

* **Réseau** : `ov_netclient` lit enfin le Game Event (1, 2, 7, 8) — il ne le lisait pas.
* **Le ciel et le brouillard** : `render::sky_darken(jour, pluie, orage × pluie)` alimente le
  lightmap ; le brouillard est assombri par la pluie (rouge et vert × (1 − 0,5 p), bleu
  × (1 − 0,4 p)) et l'orage (× (1 − 0,5 o)). **Ces trois constantes ne sont pas mesurées** contre le
  client réel ; elles reproduisent la description du rendu, comme les deux constantes du lightmap
  déjà nommées dans `PROVENANCE.md`.
* **L'éclair** : une entité `lightning_bolt` que le client n'a pas encore vue illumine le ciel deux
  ticks (le lightmap à pleine lumière du ciel, le brouillard tiré vers un blanc bleuté).
  **L'éclair lui-même n'est pas dessiné** (pas de géométrie) ; le tonnerre est un son joué par le
  client, du domaine de l'audio — non fait ici.
* **La pluie et la neige** (`ov_render/precipitation.{hpp,cpp}`) : une feuille par colonne dans un
  carré de rayon 10 autour de la caméra, du sommet de MOTION_BLOCKING (les heightmaps du client
  sont recalculées à l'arrivée du chunk) jusqu'à 10 blocs au-dessus de l'œil, tournée en travers de
  la ligne vers la caméra ; texture `environment/rain.png` qui défile vite avec une phase par
  colonne, `environment/snow.png` lentement avec une dérive ; alpha qui s'efface vers le bord du
  carré et suit le niveau de pluie ; pluie ou neige selon **la même** `ClimateNoise` que le serveur.
  Dessinée par une passe translucide du moteur d'entités (`EntityPass::Translucent` : mélange alpha,
  deux faces, sans écrire la profondeur, échantillonneur répété, `weather.frag`).
* **Non fait** : les éclaboussures au sol, le son de la pluie, l'assombrissement des nuages (ce
  client n'a pas de nuages).

### 7.1 Ce que ça coûte, et à quoi ça ressemble

`ov_voxel --singleplayer --width=1280 --height=720 --frames=900`, superflat de plaine, Debug,
écran Retina (2560×1440 rendus), avec et sans `--chat=/weather thunder --chat-at=100` :

| | ciel clair | orage |
|---|---:|---:|
| enregistrement CPU, p50 / p99 | 0,35 / 1,30 ms | 0,75 / 2,30 ms |
| **passe météo seule** (colonnes, feuilles, soumission, dessin), p50 / p99 / max | — | **0,453 / 1,348 / 3,289 ms** |
| feuilles dessinées, dernière frame | — | 440 (441 colonnes moins celle sous la caméra) |
| GPU, p50 / p99 | 0,62 / 5,58 ms | 2,86 / 6,39 ms |

Le GPU prend 2,2 ms de plus à p50 : 440 feuilles translucides plein écran à 2560×1440, du
remplissage pur. C'est le poste à regarder si le rayon passe à 10 en « rapide » (vanilla : 5).

Les captures (locales, non commitées, `.scratch/weather-before.ppm` et `weather-after.ppm`) : le
ciel clair bleu pâle et l'herbe claire ; puis, sous l'orage, le ciel et le brouillard gris-bleu
sombre, l'herbe assombrie par le lightmap, et les traînées de pluie de `rain.png` sur tout le champ,
plus denses et plus opaques près de la caméra. **Pas de capture de neige** : le monde du solo est
une plaine ; la géométrie de la neige est couverte par `test_precipitation.cpp`, pas par une image.

Un bug trouvé par la capture elle-même : la première image d'orage n'avait **aucune** pluie. Le
client lisait bien les Game Events, mais `Client::poll` recopie les champs de sa boîte un par un, et
les deux nouveaux n'y étaient pas : paquets lus, puis jetés.

---

## 8. Pièges payés pendant cette vague

1. **Le pack de registres de `main` est passé au format 15** pendant la vague (fusion du son). Un
   worktree qui le lie par symlink refuse le pack et trois suites de tests plantent. Générer sa
   propre copie (`rm` du lien, `python3 tools/ov_datagen/ovpack.py`) — jamais à travers le lien.
2. **`tp x y z` garde la rotation : Synchronize Position arrive avec les drapeaux 0x18**, pas 0.
   Une sonde qui ne lit comme déplacement que les drapeaux 0 renvoie son ancienne position, le
   serveur la remet là, et **tous ses clics suivants sont hors de portée** : trente montages de lit
   lus « rien ne se passe » (première campagne `sleep`, et le premier bout-en-bout). Appliquer les
   bits 0x01/0x02/0x04 en relatif et tout le reste en absolu.
3. **Un vanilla ignore Use Item On tant qu'une téléportation n'est pas confirmée.** Attendre la
   confirmation avant de cliquer.
4. **Deux clés JSON de même nom** : la campagne `lightning` rangeait les positions des
   paratonnerres sous `rods` puis la phase `rods` écrasait la clé. Les positions sont fixées par
   construction et l'analyse les recalcule.
5. **Le nom d'un test Catch2 contenant une virgule** se coupe en deux filtres : utiliser un joker
   (`"ponds freeze*"`).
6. **Un serveur Debug chargé tourne loin de 20 ticks par seconde** : 11 ticks en 6 s pendant le
   premier bout-en-bout. Une attente en secondes n'est pas une attente en ticks.

---

## 9. Fichiers

| fichier | rôle |
|---|---|
| `src/ov_gameplay/{include/ov/gameplay,src}/weather.{hpp,cpp}` | bruit simplex, climat, assombrissement, gel/neige/chaudrons, horloge de l'éclair, conversions |
| `src/ov_gameplay/{include/ov/gameplay,src}/sleep.{hpp,cpp}` | verdict, lit, boîte des monstres, position debout, saut de nuit |
| `src/ov_gameplay/tests/test_weather.cpp` | règles, seuils, étangs contre vanilla |
| `src/ov_gameplay/tests/test_climate_oracle.cpp` | le monde de référence rejoué (`OV_CLIMATE_ORACLE`) |
| `src/ov_server/src/weather_session.{hpp,cpp}` | tick de chunk, éclairs, lits, respawn |
| `src/ov_server/tests/test_weather_session.cpp` | la session sur de vrais chunks |
| `src/ov_server/src/server.cpp` | blocs `// ── weather ──` : construction, hôte, tick, clic de lit, Player Command, déconnexion, arrivée, réapparition, assombrissement |
| `src/ov_server/src/agriculture.hpp` | `PlantHooks::is_raining_at` |
| `src/ov_server/src/tnt_gravity.hpp` | `charge_creeper` |
| `src/ov_server/src/commands/service.hpp` | `set_personal_spawn` / `clear_personal_spawn` |
| `src/ov_netclient/…/client.{hpp,cpp}` | Game Events 1/2/7/8 |
| `src/ov_render/{include/ov/render,src}/precipitation.{hpp,cpp}`, `environment.*` | feuilles de pluie/neige ; brouillard et ciel sous la pluie |
| `src/ov_client/…/entity_renderer.*`, `shaders/weather.frag` | passe translucide |
| `apps/ov_voxel/src/main.cpp` | blocs `// ── weather ──` |
| `scripts/measure_weather.py`, `analyse_weather.py`, `measure_climate.py`, `check_weather_e2e.py` | oracles et bout en bout |
