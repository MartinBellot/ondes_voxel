# Cerveaux — la vie des villageois : horaire, ragots, reproduction, golems

Ce dossier trace le **système de cerveaux** (mémoires, capteurs, activités, comportements) et ce
qu'il fait vivre chez les villageois : l'horaire de la journée (travail, rassemblement à la cloche,
sommeil), les ragots et la réputation (et les prix qu'elle fait), la reproduction, la récolte des
fermiers, l'invocation des golems de fer, le type donné par le biome, et les tables du marchand
ambulant. Il prolonge `villageois.md` (métiers, commerce) et `mobs-3.md` (zombification, guérison).

**Tout chiffre ci-dessous a été mesuré contre le vrai serveur 1.20.1** (`tools/vanilla/server.jar`)
par `scripts/measure_villager_life.py`, avec un client sonde connecté ; les relevés bruts vont dans
`.scratch/villager_life.json` (non suivi). Chaque campagne garde un **témoin qui doit échouer** à
côté de ce qu'elle mesure. Ce qui n'a pas été mesuré est dit à l'endroit où c'est utilisé, et
regroupé au § 11.

| mesure | vanilla | nous |
|---|---|---|
| prix sous réputation et Héros du village | 18 villageois × 5 offres | **90 / 90** égaux (`[parity]`) |
| ragot par échange, par coup | +2 commerce, +25 petit négatif | idem |
| témoins d'un meurtre | 4 et 12 blocs : grand négatif 25 ; 20 blocs : rien | rayon 16 |
| oubli quotidien | −10 / −20 / −1 / 0 / −2, à `LastGossipDecay + 24000` pile | idem |
| seuil d'oubli | 2 gardé, 1 perdu (3 types) | `kGossipKeepAtLeast` = 2 |
| `LastGossipDecay: 0` | devient l'heure courante, rien d'oublié | idem |
| ragots transmis à la cloche | 80 (100 − 20) et 30 (40 − 10) en 4 s ; commerce 20 et grand positif jamais ; rien sans cloche | idem (`[parity]` sur 64 graines) |
| horaire | `last_slept` au jour 12070, `last_woken` au jour 24019 | repos à 12000, réveil ≤ 21 ticks après 10 |
| reproduction | 12 points de nourriture et un lit libre ; 8 points : rien ; 2 lits pour 2 : rien, mais la nourriture est mangée | idem, témoins compris |
| parents après | `Age` 6000, pain 6 → 3, pommes de terre 12 + betteraves 12 → betteraves 12 | idem |
| type du bébé (parents désert, plaines) | 20 désert / 36 | moitié le biome, moitié un parent |
| golem contre un joueur | −500 et −100 : 13 coups en 12 s ; −99 et −50 : aucun | `≤ −100` |
| fermier | récolte et replante ; 22 blés et +38 graines en 113 s | récolte ±1 autour de lui, replante |
| golem, panique | 3 dormeurs + zombie : 1 golem ; 3 non-dormeurs, 2 dormeurs, golem déjà là, pas de zombie : 0 | idem (5 / 5) |
| golem, rassemblement | 5 dormeurs à la cloche : 1 golem ; 5 non-dormeurs : 0 | idem |
| type par biome | 53 biomes | **53 / 53** |
| marchand ambulant | 64 offres ordinaires (5 distinctes par marchand) + 6 rares (1), 200 marchands | tables § 9 |
| `DespawnDelay` | −1 par tick, parti à 0 | idem |

---

## 1. Méthode

La sonde et les aides de console sont celles de `measure_villagers.py` (`Trader`, `Rig`) ; la
sonde garde en plus l'heure du jour de chaque *Update Time* (`Clocked`). Douze campagnes :

```
price events decay share schedule breed breed_type farm golem golem_meet anger trader biome
```

Chacune prend quelques minutes ; elles passent par la voie `java` de la machine, cinq ou six à la
fois (moins de 10 minutes chacune). Les pièges payés en route sont au § 12 — plusieurs premiers
passages ne mesuraient rien, et chacun l'a dit par son témoin.

---

## 2. Le cadre : mémoires, capteurs, activités, comportements

La forme est celle que décrit le Minecraft Wiki (page « Brain », et « Villager » pour le contenu) :

1. **les mémoires** oublient ce qui a expiré (`Memories::tick`) : chaque mémoire peut porter un
   temps de vie (`ttl`), compté à rebours, oubliée à zéro ;
2. **les capteurs** dont le compte à rebours est écoulé captent, puis attendent leur intervalle ;
   leur première phase est tirée dans `[0, intervalle)` pour que cent villageois ne scannent pas le
   même tick ;
3. **les comportements arrêtés** des activités actives, par priorité, se voient offrir un départ :
   leurs mémoires requises d'abord (présentes ou absentes), puis leurs conditions propres ; ils
   courent une durée tirée dans `[min, max]` ;
4. **les comportements en cours** tiquent, ou s'arrêtent quand leur temps est écoulé ou qu'ils ne
   peuvent plus servir.

Les activités *core* sont toujours actives ; des autres, une seule, choisie par l'horaire (au plus
une fois tous les **20 ticks**, `kScheduleUpdateInterval`) ou forcée (la panique). Une activité peut
exiger des mémoires — le travail un lieu de travail, le rassemblement un point de rencontre — et
retombe sinon sur l'activité par défaut (`idle`).

**Code.** `brain/memory.hpp` (les mémoires, un tableau fixe, aucune allocation), `brain/brain.hpp`
(capteurs, horaire, activités, comportements, `BrainGoal` qui donne le corps du mob à son cerveau :
un mob à cerveau passe toujours par `Mob::tick` et son suiveur de chemin). Générique : un piglin,
un axolotl ou une grenouille sera une autre table de capteurs et de comportements ; seul le
villageois en a une ici.

**Ce qui diffère, nommé.**

* Les durées et les phases des capteurs tirent du générateur **du mob**, pas de celui du niveau :
  un générateur partagé fait dépendre le monde de l'ordre des mobs (déterminisme, CLAUDE.md § 2).
* Quand l'activité change, les comportements de celle qu'on quitte sont **arrêtés aussitôt** ;
  vanilla les laisse s'arrêter sur leurs propres conditions.
* Pas d'index de POI : les revendications (lit, lieu de travail, cloche) restent le balayage
  incrémental de `villageois.md` § 3.2, recopié dans les mémoires à chaque tick. Une cloche est
  partagée (32 tickets en vanilla), jamais « revendiquée » contre un autre.

---

## 3. L'horaire (campagne `schedule`)

Un bibliothécaire enfermé avec un lit, un pupitre et une cloche ; le cycle du jour allumé depuis
1900, 8900, 10900, 11900 et 23900 ; ses mémoires relues toutes les demi-secondes, l'heure lue sur
le fil.

* **Revendication** : lit, pupitre et cloche au premier relevé — `home`, `job_site`,
  `meeting_point`, et `last_worked_at_poi`.
* **Coucher** : `last_slept` apparaît au jour **12070** (repos à 12000, la marche jusqu'au lit).
* **Réveil** : `last_woken` au jour **24019**, soit 19 après minuit : l'activité `idle` commence à
  10, et l'horaire n'est consulté qu'une fois tous les 20 ticks.
* `meeting_point` disparaît au jour 10979 et revient à 10999 — une relecture à la sortie du
  rassemblement ; **non reproduit**, nommé.

Chez nous : l'horaire du wiki (`idle` 10, `work` 2000, `meet` 9000, `idle` 11000, `rest` 12000 ;
le bébé : `play` 10, `idle` 3000, `play` 6000, `idle` 10000, `rest` 12000), la porte des 20 ticks,
et le test `a villager claims a bed and a bell, sleeps at rest and wakes after day 10` : endormi
après 12000, réveillé entre 10 et 31.

**Le format sauvé** (lu tel quel sur le vrai serveur) :

```
Brain: {memories: {
  "minecraft:home":        {value: {pos: [I; 301, -60, 2], dimension: "minecraft:overworld"}},
  "minecraft:last_worked_at_poi": {value: 968L},
  "minecraft:golem_detected_recently": {value: 1b, ttl: 561L}}}
```

---

## 4. Ragots et réputation

### 4.1 Les types

Le tableau du wiki (« Villager », Gossiping), confirmé par les mesures ci-dessous :

| type | poids | max | oubli par jour | perte à la transmission |
|---|---|---|---|---|
| `major_negative` | −5 | 100 | 10 | 10 |
| `minor_negative` | −1 | 200 | 20 | 20 |
| `minor_positive` | 1 | 200 | 1 | 5 |
| `major_positive` | 5 | 100 | 0 | 100 |
| `trading` | 1 | 25 | 2 | 20 |

La réputation d'un joueur auprès d'un villageois est la somme des valeurs × poids.

### 4.2 Les prix (campagne `price`)

Un bibliothécaire à cinq offres (24 papiers à 0,05 ; 10 émeraudes à 0,2 ; 1 à 0,05 ; 5 à 0,05 ;
20 à 0,2), des ragots sur la sonde écrits au `summon` ; l'écran ouvert, *Merchant Offers* décodé.
**Les 90 prix spéciaux** (18 villageois × 5) tombent sur :

```
spécial = −floor(réputation × multiplicateur)            (produit en flottant)
        + −max(1, floor((0,3 + 0,0625 × amplificateur) × base))   si Héros du village
```

Cellules : chaque type seul (10, 100, 200, **250** — une valeur lue au-dessus du maximum compte
entière), guéri (grand positif 20 + petit positif 25), les cinq mêlés, Héros I, II et V, Héros +
ragots ; **témoin** : les mêmes ragots sur un autre joueur ne changent rien. Le prix revient à 0 à
la fermeture (17 villageois sur 18 ; le 18ᵉ a été relu avant que la fermeture n'arrive).
`test_brain.cpp` rejoue les 90 ; `test_villager_life_server.cpp` passe par `apply_special_prices`.

### 4.3 Ce qui écrit un ragot (campagne `events`)

* un échange : **commerce +2** (2, puis 4) ;
* un coup du joueur : **petit négatif +25** (25, puis 50) ;
* un meurtre : les villageois à **4 et 12 blocs** entendent **grand négatif 25**, celui à **20**
  rien. Ce sont ceux que *la victime* voyait (les deux premiers passages ne relevaient rien : la
  victime, puis les témoins, étaient `NoAI`, donc sans capteurs — § 12). Chez nous : les villageois
  à 16 blocs de la victime, la vue n'est pas modélisée (nommé) ;
* une guérison : grand positif 20 et petit positif 25 au joueur de la pomme (le wiki ; **non
  remesuré ici**, la guérison elle-même l'est dans `mobs-3.md`).

### 4.4 L'oubli (campagne `decay`)

Villageois invoqués avec des ragots et un `LastGossipDecay` choisi :

| cellule | résultat |
|---|---|
| dû (`maintenant − 24000`) | −10 / −20 / −1 / 0 / −2, `LastGossipDecay` ← maintenant |
| dû dans 200 ticks | rien, puis l'oubli **200 ticks plus tard**, pile |
| très en retard (`− 100000`) | **un seul** jour d'oubli |
| `0` | devient l'heure courante, **rien** d'oublié |
| seuil : 2→… | petit positif 3 → 2 **gardé**, 2 → 1 **perdu** ; commerce 4 → 2 gardé, 3 → 1 perdu ; petit négatif 22 → 2 gardé, 21 → 1 perdu |

Le premier passage ne mesurait que la cellule « 0 » : le script bornait à 0 les longs négatifs dont
un monde jeune a besoin (§ 12).

### 4.5 La transmission (campagne `share`)

A (petit négatif 100, commerce 20, grand positif 50, grand négatif 40, petit positif 60) et B, dans
une pièce avec une cloche, en heure de rassemblement : B lit **petit négatif 80 et grand négatif 30
dès 4 s**, puis plus rien de nouveau en 100 s ; commerce (20 − 20 = 0) et grand positif (perte 100)
n'arrivent jamais ; petit positif (60 − 5 = 55) n'a pas été tiré. **Témoin** : sans cloche, B
n'apprend rien.

Chez nous (`transfer_from`) : jusqu'à 10 tirages avec remise, chacun proportionnel à
|valeur × poids| ; un ragot tiré arrive diminué de sa perte, gardé s'il reste ≥ 2, sans jamais
baisser ce que l'autre savait déjà ; deux villageois ne bavardent qu'une fois par 1200 ticks. Le
comportement `gossip` le fait quand deux villageois se parlent à moins de √5 bloc (`idle`, et
`meet` à la cloche).

---

## 5. La reproduction (campagnes `breed`, `breed_type`)

Six pièces de 9 × 9 à 120 blocs l'une de l'autre (un villageois cherche un lit à 48), deux
villageois sans métier par pièce :

| cellule | bébé | après |
|---|---|---|
| 3 pains chacun (12 points), 3 lits | oui, vers le tick 340 à 448 | pain mangé, parents `Age` 6000 |
| 12 carottes chacun, 3 lits | oui | carottes mangées |
| **2 pains chacun (8 points)**, 3 lits | **non** (témoin) | pain gardé |
| 3 pains, **2 lits pour 2** | **non** (témoin) | **pain mangé quand même** |
| parents désert, 6 pains | oui, bébé désert | 3 pains restants chacun |
| 12 pommes de terre + 12 betteraves | oui | les betteraves restent |

Points de nourriture : pain 4, carotte, pomme de terre, betterave 1 ; il en faut 12
(`food_level` + poches). La naissance mange 12 points à chacun **avant** de chercher le lit libre
(d'où le pain mangé sans bébé), en vidant les poches dans l'ordre ; le bébé naît `Age` −24000 et
prend ce lit pour maison. Un couple, un bébé : chez nous, celui des deux dont l'identifiant est le
plus petit porte la naissance.

**Le type du bébé** (campagne `breed_type`). Des couples de parents désert dans des plaines :
un premier passage de 12 a donné 9 désert, 3 plaines — que la règle du wiki n'atteint que 7 % du
temps ; un second de 24 a donné 11 désert, 13 plaines. **Ensemble : 20 désert sur 36.** La règle du
wiki — une fois sur deux le type du biome, sinon celui d'un parent ou de l'autre — prévoit 18 ; la
règle « un parent trois fois sur quatre » en prévoirait 27, et n'en donne 20 ou moins qu'une fois
sur cent. Le code suit le wiki (`VillagerLife`, naissance). Le premier passage, seul, aurait fait
choisir la mauvaise règle.

---

## 6. Le fermier (campagne `farm`)

Un fermier à son composteur, un champ de 35 blés mûrs, 4 graines en poche : en 113 s, **22 blés
et +38 graines** dans ses poches. C'est la table de butin du blé (1 blé, 1 + Bin(3, 4/7) graines)
moins une graine replantée par récolte : 22 × 1,71 = 37,7. Chez nous : le comportement `harvest`
(métier fermier, ±1 bloc autour des pieds comme le wiki), un `VillagerEvent` que le serveur finit —
le bloc cassé par la vraie table de butin, les objets **mis directement en poche** (vanilla les
laisse tomber et le villageois les ramasse ; le ramassage n'est pas fait, nommé), puis la graine
plantée sur la terre labourée nue.

---

## 7. Les golems de fer (campagnes `golem`, `golem_meet`, `anger`)

**L'invocation.** Un villageois veut un golem s'il a dormi depuis moins de 24000 ticks
(`last_slept`) et n'a pas vu de golem récemment (`golem_detected_recently`, 600 ticks). Paniqué, il
tente l'invocation une fois sur cent par tick, et il en faut **3** qui veulent dans la boîte de ±10 ;
en bavardant, **5**.

| cellule | golems |
|---|---|
| 3 dormeurs derrière des barrières, un zombie `NoAI` au milieu | **1**, entre 65 et 130 ticks |
| 3 non-dormeurs (témoin) | 0 |
| 2 dormeurs (témoin) | 0 |
| 3 dormeurs, un golem déjà là (témoin) | 0 de plus ; leur `golem_detected_recently` relu, `ttl` 443 |
| 3 dormeurs, pas de zombie (témoin) | 0 |
| 5 dormeurs à la cloche, heure du rassemblement | **1**, entre 322 et 423 ticks |
| 5 non-dormeurs à la cloche (témoin) | 0 |

Après l'invocation, chaque villageois de la boîte garde `golem_detected_recently` (`ttl` 561 relu),
et le capteur du golem le rafraîchit tant que le golem est à 16 blocs.

**La place du golem** : dix colonnes tirées à ±8, chacune descendue de +6 à −6 jusqu'à un sol
solide (ni verre, ni feuilles, ni glace…) sous trois blocs libres — le wiki ; **non mesuré** (le
golem marche aussitôt, sa position relue ne dit rien de sa naissance).

**Le golem** (`village_mobs.hpp`) : il frappe les ennemis (tout `Enemy` sauf le creeper) à 16 blocs
sans avoir besoin de les voir, se promène, regarde les joueurs, et **défend le village** contre un
joueur dont la réputation auprès d'un villageois proche est **≤ −100**. Campagne `anger` (en
Facile — le premier passage tournait en Paisible, où aucun mob n'attaque un joueur, § 12) : un
golem libre dans une pièce avec la sonde en survie et un villageois `NoAI` qui a des ragots sur
elle ; les coups comptés sur le fil (événement d'entité 4) pendant 12 s :

| réputation du villageois | coups du golem |
|---|---|
| −500 (grand négatif 100) | 13 |
| −100 (petit négatif 100) | 13 |
| **−99** (petit négatif 99), témoin | **0** |
| −50 (petit négatif 50), témoin | 0 |

Le seuil est **inclusif** et tombe entre −99 et −100 : `kDefendReputation` = −100, comparé par
`≤`.
Les dégâts d'un golem sont l'attribut `attack_damage` (15) passé par `MobAttacks` ; le tirage
`7,5 + next_int(15)` de vanilla et la projection vers le haut ne sont **pas faits**, nommés.
L'offre du coquelicot n'est pas faite.

---

## 8. Le type par biome (campagne `biome`)

Un villageois invoqué **sans NBT** (avec, `summon` saute `finalizeSpawn` et le type n'est jamais
tiré — § 12) dans chacun de 53 biomes posés par `fillbiome` :

| type | biomes |
|---|---|
| désert | desert, badlands, eroded_badlands, wooded_badlands |
| jungle | jungle, sparse_jungle, bamboo_jungle |
| savane | savanna, savanna_plateau, windswept_savanna |
| neige | snowy_plains, ice_spikes, snowy_taiga, grove, snowy_slopes, frozen_peaks, jagged_peaks, frozen_river, snowy_beach, frozen_ocean, deep_frozen_ocean |
| marais | swamp, mangrove_swamp |
| taïga | taiga, old_growth_pine_taiga, old_growth_spruce_taiga, windswept_hills, windswept_gravelly_hills, windswept_forest |
| plaines | tout le reste (24 relevés, dont cherry_grove, meadow, les océans non gelés, deep_dark, mushroom_fields) |

Chez nous (`villager_type_for_biome`) : tout villageois dont le type n'a pas été décidé (lu sur
disque, gardé par une guérison, tiré à la naissance) prend celui du biome sous ses pieds au premier
tick — `/summon`, `--mobs=`, et les villageois des villages générés.

---

## 9. Le marchand ambulant (campagne `trader`)

200 marchands invoqués, leurs `Offers` relues : **toujours 6 offres**, les 5 premières distinctes
dans un lot de **64** (≈ 15,6 tirages chacune, uniformes), la sixième dans un lot de **6** rares.
Toutes : `xp` 1, multiplicateur 0,05, `rewardExp` 1.

| lot | offres (émeraudes → objet, utilisations max) |
|---|---|
| ordinaire | 5 → pousse d'acacia, bouleau, cerisier, chêne noir, jungle, chêne, sapin, propagule de palétuvier (8) ; 3 → cactus (8), blocs de corail ×5 (8), varech (12) ; 2 → pierre lumineuse (5), cornichon de mer (5) ; 4 → boule de slime (5) ; 5 → coquille de nautile (5) ; 1 → 1 fleur de chaque (allium, bleuet azur, orchidée bleue 8, bleuet, pissenlit, fougère, muguet 7, tulipes ×4, marguerite, coquelicot : 12), graines de betterave, melon, citrouille, blé (12), champignons ×2 (12), citrouille (4), canne à sucre (8), lianes (12) ; 1 → 2 nénuphars, 2 blocs de mousse, 2 spéléothèmes, 2 terre racinée, 2 petites feuilles-plateaux (5) ; 1 → 3 teintures ×16 (12) ; 1 → 4 sable rouge (6) ; 1 → 8 sable (8) |
| rare | 6 → glace bleue (6) ; 1 → poudre à canon (8) ; 3 → glace compactée (6) ; 5 → seau de poisson-globe (4) ; 5 → seau de poisson tropical (4) ; 3 → 3 podzol (6) |

`DespawnDelay:100` : relu 97, 85, 73 … 4, puis le marchand est parti — un tick de moins par tick,
parti à 0. Un marchand invoqué sans ce champ ne part jamais.

**L'ordre des cinq offres n'est pas établi.** Chez le villageois, deux offres tirées s'affichent
dans l'ordre d'un `HashSet` (indice modulo 16, `villageois.md` § 5.2). Si le marchand faisait de
même, deux objets du même seau devraient apparaître dans les deux ordres et deux objets de seaux
différents dans un seul — 16 classes. Les 200 marchands en donnent 49, avec 6 contradictions :
avec 5 offres parmi 64, deux objets ne se croisent qu'une fois en moyenne, et l'échantillon est trop
maigre pour trancher. Nommé, non tranché.

---

## 10. La persistance

Aux clés vanilla (`entity_storage.cpp`) : `Gossips` (`{Type, Target:[I;…], Value}`, le type sans
espace de noms), `LastGossipDecay`, `Inventory` (`{id, Count}`), `FoodLevel`, et
`Brain.memories` (les huit mémoires qu'un villageois sauve, au format du § 3). À la lecture, les
lieux mémorisés redeviennent les revendications du balayage.

* **Nous → nous** (`test_villager_life_server.cpp`) : ragots, mémoires (dont un `ttl` de 300),
  poches, nourriture, jour d'oubli, écrits puis relus par un deuxième monde ; une mémoire jamais
  sauvée (`walk_target`) ne l'est pas.
* **Vanilla → nous** : le bibliothécaire de la campagne `schedule` et le villageois de `golem`,
  leur `Brain` et leurs `Gossips` tels que le vrai serveur les a écrits, relus : maison, pupitre,
  cloche, `last_worked_at_poi` 968, `ttl` 561, commerce 4 sur la sonde.

### 10.1 De bout en bout, sur notre serveur

`scripts/check_villager_life_e2e.py` : `ov_dedicated` sur un monde neuf (port 25624), mené par sa
console ; un enclos de verre, trois lits, un pupitre, un composteur, un tonneau et une cloche à
l'autre bout ; trois villageois invoqués ; une sonde protocole 763 jugée **sur le fil seulement** :

```
villageois          3 arrivés sur le fil
métiers (midi)      [5, 6, 9] en 15,0 s (fermier, pêcheur, bibliothécaire), types [2, 2, 2]
couchés (jour)      12017, 12077, 12116          (pose 2, jour lu sur Update Time)
levés (jour)        13, 13, 13                   (vanilla : last_woken au jour 19)
cloche (distance)   avant 9000 : 18,1 · 21,1 · 18,6 ; au jour 9105 : 4,7 · 5,9 · 4,8
marchand ambulant   6 offres, titre « entity.minecraft.wandering_trader »
entities/           Brain.memories des trois : home, job_site, meeting_point,
                    last_slept, last_woken (et last_worked_at_poi pour deux)
Gossips             minor_negative 25 sur la sonde, après un coup
```

**Relu par le vrai serveur** (`check_villager_life_e2e.py readback`, voie java, port 25724) : le
jar 1.20.1 lancé sur une copie du monde que notre serveur a sauvé relit **les trois villageois
avec maison, travail, cloche, `last_slept` et `last_woken`** dans `Brain.memories`, le ragot
`{Target: [I; …], Type: "minor_negative", Value: 25}` sur la sonde, et le marchand ambulant avec
ses **6 offres**. L'aller-retour nous → vanilla tient.

Le lever tombe au jour 13 chez nous, 19 chez vanilla : les deux sont dans les 21 ticks que la
porte de l'horaire laisse après 10 ; la phase de cette porte dépend du tick où chacun a consulté
son horaire la dernière fois, et n'est pas reproduite au tick près — nommé.

---

## 11. Ce qui n'est pas fait, ou pas mesuré

* **Le marchand ambulant** : ses tables et son `DespawnDelay` sont mesurés (§ 9) ; son
  **apparition** suit le wiki (tous les 24000 ticks comptés par 1200, chance 25 → 50 → 75 %, puis
  un sur dix, dix colonnes à 48 blocs d'un joueur, le gamerule `doTraderSpawning`) — **non
  mesurée** (aléatoire) ; ses deux lamas de marchand apparaissent à côté de lui **sans laisse** ;
  il ne marche pas vers un but choisi ; les clés `WanderingTraderSpawnDelay` /
  `WanderingTraderSpawnChance` de `level.dat` ne sont pas sauvées ; les deux octets de fin de
  *Merchant Offers* d'un marchand (barre de niveau, réapprovisionnement) sont mis à 0 sans capture.
* **Le golem de neige** : laissé aux mobs hostiles.
* **Raids** (`pre_raid`, `raid`, `hide`, la cloche qui sonne) : activités nommées, vides.
* **Non mesurés** : la place exacte d'un golem invoqué, le rythme des bavardages (1200 ticks, le
  wiki), la guérison comme ragot, le partage de nourriture entre fermiers, le ramassage d'objets
  par un villageois, les capteurs et leurs intervalles (20, 1 et 200 ticks ici), la compétition pour
  un lieu de travail (`PoiCompetitorScan`), la chance du coquelicot.
* **La vue** n'est pas modélisée pour les témoins d'un meurtre et les hostiles (distance seule).

---

## 12. Pièges payés

1. **Un bibliothécaire à `Xp:0` sans pupitre perd son métier en 3 ticks** (la règle mesurée de
   `villageois.md` § 3.4) : les 18 écrans de la première campagne `price` ne se sont jamais ouverts.
   `Xp:1` partout.
2. **`summon` avec du NBT saute `finalizeSpawn`** (déjà le piège 32 de `mobs-3.md`) : 53 biomes,
   53 plaines. Et un villageois invoqué nu marche : la moitié des sélecteurs à 2 blocs ne le
   retrouvaient plus.
3. **Un villageois `NoAI` n'a pas de capteurs** : ni la victime ni les témoins d'un meurtre ne
   « voient » quoi que ce soit, et personne n'entend rien — deux passages pour le comprendre.
4. **Un monde jeune a besoin de longs négatifs** : `LastGossipDecay` = maintenant − 24000 est
   négatif avant le tick 24000 ; le script les bornait à 0 et mesurait autre chose.
5. **En Paisible, aucun mob n'attaque un joueur** — un golem face à une réputation de −500 ne
   bouge pas. La campagne précédente remettait Paisible en partant.
6. **Le cycle du jour coupé envoie une heure négative** dans *Update Time* : l'heure lue est la
   valeur absolue.
7. **Une sonde à 300 blocs ne voit rien** : la pose du dormeur n'a pas été relue ; les mémoires
   `last_slept` / `last_woken` en tiennent lieu.
8. **Une cellule ajoutée n'est pas une cellule relue** : la cellule du seuil d'oubli a été invoquée
   un passage entier sans que la boucle de relevé la connaisse.
9. **Une construction qui démarre pendant qu'on ajoute des sources** régénère `build.ninja` sur un
   fichier pas encore écrit, et meurt.

---

## 13. Fichiers

| fichier | rôle |
|---|---|
| `src/ov_gameplay/{include/ov/gameplay,src}/brain/memory.{hpp,cpp}` | mémoires à durée de vie, noms vanilla |
| `src/ov_gameplay/{include/ov/gameplay,src}/brain/brain.{hpp,cpp}` | capteurs, horaire, activités, comportements, `BrainGoal` |
| `src/ov_gameplay/{include/ov/gameplay,src}/brain/gossip.{hpp,cpp}` | ragots, réputation, oubli, transmission, prix |
| `src/ov_gameplay/{include/ov/gameplay,src}/brain/villager_brain.{hpp,cpp}` | le cerveau du villageois, la reproduction, les poches, l'invocation du golem |
| `src/ov_gameplay/{include/ov/gameplay,src}/village_mobs.{hpp,cpp}` | le golem de fer |
| `src/ov_server/src/villager_life.{hpp,cpp}` | naissances, golems, récoltes, type par biome, oubli, ragots du combat |
| `src/ov_server/src/merchant_session.{hpp,cpp}` | prix pour le joueur, ragot d'échange (blocs `brains`) |
| `src/ov_server/src/entity_storage.cpp` | `Gossips`, `Brain`, `Inventory`, `FoodLevel`, `LastGossipDecay` |
| `src/ov_server/src/server.cpp`, `zombie_villagers.*` | blocs `// ── brains ──` |
| `src/ov_gameplay/tests/test_{brain,villager_life}.cpp`, `src/ov_server/tests/test_villager_life_server.cpp` | les mesures `[parity]`, les comportements, la persistance |
| `scripts/measure_villager_life.py` | l'oracle (12 campagnes) |

```bash
python3 scripts/measure_villager_life.py price events decay biome trader anger   # ~6 min
python3 scripts/measure_villager_life.py schedule breed breed_type share           # ~8 min
python3 scripts/measure_villager_life.py golem golem_meet farm                     # ~5 min
```
