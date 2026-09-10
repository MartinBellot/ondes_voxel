# Random ticks et agriculture

Ce dossier trace le **random tick** — la seule horloge qui fait pousser quelque chose dans
Minecraft — et les comportements qui en dépendent : cultures, tiges, terre labourée, canne et
cactus, pousses, herbe et mycélium, feuilles, glace et neige ; plus l'agriculture active (poudre
d'os, plantation). Chaque règle a été spécifiée depuis le wiki, puis **mesurée** contre le vrai
serveur 1.20.1 (`tools/vanilla/server.jar`) partout où un oracle existe. Deux règles écrites
d'après la documentation ont été **corrigées par la mesure** (§ 4.4 et § 7.2), un seuil du wiki
est faux d'un cran (§ 6), et un chiffre du wiki qu'un banc mal trié semblait contredire tient
(§ 7.3).

Le résultat court :

| mesure | vanilla | nous |
|---|---|---|
| hydratation de la terre labourée, 4 × 169 cellules | carré 9 × 9, eau à dy 0 ou +1 | **676 / 676** |
| décomposition des feuilles, 10 montages + une nappe de 288 | distances 1..6 gardées, 7 tombe | **règle identique**, 288 / 288 sur la nappe |
| croissance, 4 dispositions, χ² d'homogénéité avec vanilla | — | p = **0,745 / 0,578 / 0,816 / 0,736** |
| … témoin (mauvaise probabilité) | — | p = 0 / 0 / 7,5·10⁻⁹⁷ (rejeté) |
| fonte, 32 échantillons | glace > 10, neige > 11 (lumière de bloc de la cellule) | identique |
| poudre d'os sur l'herbe, 64 parcelles | 14,45 herbes (σ 2,81) | 14,86 |
| poudre d'os sur la betterave, 1 500 vérifiées | +1 : 1 111 (0,741), jamais +2 | 3/4 |
| feuilles des arbres générés | distances 1..6 | **7 partout avant**, 1..6 après |

---

## 1. Méthode : un joueur, et des distributions

### 1.1 Pas de joueur, pas de pousse

Un random tick n'a lieu que dans un chunk dont le centre est à moins de 128 blocs
(horizontalement) d'un joueur qui n'est pas spectateur — la même porte que l'apparition des
mobs. Un serveur vanilla sans client ne fait **rien** pousser, à aucune vitesse, et lit « aucun
effet » partout. La campagne garde donc `scripts/keepalive_client.py` connecté au spawn pendant
toute la mesure (piège 11 du briefing, payé ici d'avance).

### 1.2 Le temps compté en ticks, pas en secondes

`scripts/measure_agriculture.py` pose `randomTickSpeed` à K par la console, dort, puis la remet à
0 dans la même ligne que `time query gametime`. La console s'exécute en début de tick, dans
l'ordre : la différence des deux `gametime` (moins un) est le nombre **exact** de ticks passés à
la vitesse K. Les deux campagnes de croissance ont duré 303 ticks à K = 200 et 604 ticks à
K = 100.

### 1.3 Ce qu'on compare

Le rythme de pousse est aléatoire. Un bloc reçoit Binomiale(K·T, 1/4096) random ticks — K tirages
**avec remise** par section de 4096 blocs et par tick — et pousse à chacun avec la probabilité p.
L'âge final suit donc `min(max, Binomiale(K·T, p/4096))`. On compare l'**histogramme des âges**
(un χ², cases regroupées jusqu'à un attendu ≥ 5), jamais un tirage individuel, et chaque test est
doublé d'un **témoin** — la même mesure contre une valeur de p fausse, qui doit être rejetée.
Sans témoin, un « p = 0,7 » ne prouverait rien (piège 14 du briefing).

### 1.4 Deux pièges de banc payés pendant la campagne

* **`/fill` refuse en silence au-delà de 32 768 blocs.** `clear(-60,-60,60,60)` sur quatre couches
  en demande 58 564 : la commande échoue, la zone garde tout ce que les campagnes précédentes y
  avaient posé. La première mesure de poudre sur l'herbe était pleine de distributeurs et d'eau
  d'une autre campagne ; elle a été jetée et refaite après découpage en bandes. Les campagnes
  antérieures construisaient leurs parcelles par leurs propres `fill`, petits, et n'en sont pas
  affectées — les deux campagnes de croissance, faites avant et après, s'accordent.
* **Un distributeur rate une fois sur cent environ.** Dans la seconde campagne de poudre, chaque
  rangée de 100 avait exactement un plant resté à l'âge 0 — impossible pour du blé, qui prend au
  moins deux stades. Pour la betterave, où « +0 » est un vrai résultat, l'inventaire de chaque
  distributeur est donc relu (`data get block`) avant de compter.

La première campagne de croissance a aussi perdu 9 blés denses (air à la relecture, 0 dans la
seconde) : un lot de `setblock` non exécuté au montage, pas une règle du jeu. Exclus et comptés
(« hors culture »).

---

## 2. Le random tick (`ov_server/src/agriculture.{hpp,cpp}`)

`RandomTicks` choisit, chaque tick, `randomTickSpeed` positions par section **non vide** de chaque
chunk retenu, avec remise, et les tend à `gameplay::Plants`. Trois tirages `next_int(16)` en
variables nommées, dans l'ordre (piège 2 du briefing).

* **Quels chunks.** Tickés par leur niveau de ticket (`ChunkMap::is_ticking`, voir
  `chunkmap.md`) **et** à moins de 128 blocs d'un joueur, centre du chunk contre position du
  joueur. L'ensemble est reconstruit une fois par seconde et trié — l'ordre ne dépend pas d'une
  table de hachage. Ce serveur n'a pas de spectateur ; si un jour il en a, `select` ne doit pas
  les recevoir.
* **Pas d'écriture hors du monde chargé.** Un chunk dont les huit voisins ne sont pas tous
  résidents est sauté pour ce tick : une tige qui pose un melon, ou de l'herbe qui s'étend, un bloc
  au-delà du bord écrirait sinon dans un chunk absent — et sur ce serveur, écrire là le **génère**
  sur le thread de tick.
* **RNG.** Un `XoroshiroRandomSource` à graine fixe (principe 5). Le générateur du jeu est semé
  depuis l'horloge et n'est reproductible par personne : la parité est statistique par nature.
* **Réglable.** `RandomTicks::set_speed` est le setter qu'appellera `/gamerule randomTickSpeed`
  (écrit en parallèle par un autre agent). En attendant, `OV_RANDOM_TICK_SPEED` au démarrage.
* **Ordre.** Après la vidange des block ticks planifiés, comme dans le jeu ; les écritures sont
  réglées par la même onde de notifications (`WorldTicks::settle_writes`) que celles des fluides
  et de la redstone.
* **Coût de démarrage.** `TreeGrower` charge sa propre copie du registre de features
  (`ov_worldgen` en garde une par worker, et elles ne sont pas partageables — piège 17) :
  **0,16 s** en Debug, la sixième ligne « 113 of 194 configured features » du journal.
* **Allocation.** Aucune dans `run` et `tick_section` ; `select` n'alloue qu'au-delà de sa réserve
  de 1024 chunks. Le crochet `set_block` du serveur insère encore dans `tick_relight`
  (`unordered_set`) — c'est vrai de toute écriture de règle, pas propre aux plantes, et c'est
  nommé ici.

**Nommés et non faits :** le random tick des **fluides** (la lave qui allume du feu), la
précipitation du tick de chunk (eau qui gèle et neige qui tombe dans les biomes froids), la
foudre. `Plants::unanswered()` liste un par un les blocs à random tick que ce projet ne traite pas
(bambou, lianes, cave vines, chorus, œufs de tortue, dripstone, améthyste, champignons, nylium,
glace givrée, minerai de redstone allumé, portail, **cuivre**) : un monde de lianes immobiles
ressemble exactement à un monde de lianes lentes.

---

## 3. L'hydratation de la terre labourée — exacte

Quatre parcelles de 13 × 13 de terre labourée (`moisture=0`), une source d'eau au centre à
dy = −1, 0, +1, +2 de la couche labourée (à +1 et +2 posée sur une colonne de verre et entourée de
verre pour qu'elle ne coule pas). 403 ticks à K = 100, puis relecture :

```
dy = 0 et dy = +1 :  carré 9 × 9 exact autour de l'eau, moisture 7 ; tout le reste redevenu terre
dy = -1 et dy = +2 :  aucune cellule hydratée
```

(À dy = +1, les cinq cellules sous le verre sont redevenues terre — la terre labourée sous un bloc
solide — et sont exclues.) Autrement dit : eau à ≤ 4 blocs horizontalement, **diagonales
comprises**, au niveau de la terre ou un au-dessus. C'est la phrase du wiki (Farmland), et c'est
`Plants::farmland_near_water`, testé cellule par cellule sur les quatre cartes : **676 / 676**.

Le reste de la règle, du wiki : sans eau et sans pluie, `moisture` descend d'un cran par random
tick ; à 0, sans culture dessus (`#maintains_farmland`), la terre redevient terre ; avec de l'eau,
elle passe directement à 7. Vérifié par les cartes elles-mêmes : les cellules sèches sans culture
sont redevenues terre dès leurs premiers random ticks, puis l'herbe voisine les a reprises.

**Nommés :** ce serveur n'a **pas de météo** — `is_raining_at` répond non, et c'est écrit dans
`ServerPlantEnvironment`. Le **piétinement** (un saut sur la terre labourée, probabilité
« distance − 0,5 », entités de plus de 0,512 bloc³) n'est pas branché : il demande l'événement de
chute d'une entité, qui vit dans la survie et non ici. La terre sous un bloc solide redevient terre
au tick suivant (block tick planifié) ; « solide » y est le drapeau motion-blocking mesuré, qui
tient lieu du drapeau *legacy solid* du jeu, présent dans aucun rapport.

---

## 4. La croissance

### 4.1 La formule (wiki, Tutorials/Crop farming)

Points de vitesse : la terre sous la culture vaut 2 sèche, 4 humide ; chacune des huit autour
ajoute 0,25 sèche, 0,75 humide. Divisé par deux si la même culture est sur une diagonale, ou dans
les deux axes à la fois. Probabilité par random tick : `1 / (⌊25 / points⌋ + 1)`, avec lumière
≥ 9 sur la culture. La division est en `float` et tronquée : 9,25 points donnent ⌊2,70⌋ = 2, la
même chance d'un sur trois que 10.

« Humide » est `moisture > 0` dans notre code : le wiki dit « hydratée », et les campagnes
n'utilisent que 0 et 7, elles ne distinguent donc pas les deux lectures. **Non mesuré.**

### 4.2 Les dispositions

Tuiles de 9 × 9 humides (eau au centre) et de 12 × 12 sèches, construites par
`measure_agriculture.py growth` et **reproduites bloc pour bloc** par le test
`test_agriculture.cpp` :

| groupe | disposition | points | p attendu | témoin |
|---|---|---|---|---|
| A | blé épars sur tuile humide, aucun blé parmi les 8 voisins | 10 ou 9,25 | 1/3 | 1/6 |
| C | blé dense sur tuile humide | 5 ou 4,625 | 1/6 | 1/3 |
| R | rangs de blé et de carottes alternés, terre sèche | 4 | 1/7 | 1/13 |
| B | betterave éparse, humide | 10 | 2/3 × 1/3 | 1/3 |

### 4.3 Vanilla contre le modèle — deux campagnes, K·T = 60 600 et 60 400

```
A  observé [5, 14, 26, 60, 60, 72, 55, 92]      χ²=4,17 ddl 6 p=0,653   témoin p=0
C  observé [169, 425, 503, 394, 231, 119, 40, 30] χ²=6,67 ddl 7 p=0,464  témoin p=0
R  observé [93, 195, 223, 160, 90, 26, 8, 5]    χ²=5,13 ddl 6 p=0,527   témoin p=3,4·10⁻¹⁷²
B  observé [13, 50, 78, 243]                    χ²=0,30 ddl 3 p=0,959   témoin p=6,7·10⁻⁴⁷
N  verrue du Nether, 1/10                       χ²=3,02 ddl 3 p=0,389   témoin (1/5) p=3,7·10⁻²⁸
S  baies sucrées, 1/5                           χ²=1,75 ddl 3 p=0,626   témoin (1/10) p=2·10⁻²¹
O  cacao, 1/5                                   χ²=0,67 ddl 2 p=0,715   témoin (1/10) p=1,2·10⁻¹²
P  pitcher, 1/3                                 χ²=2,25 ddl 2 p=0,325   témoin (2/9) p=5,6·10⁻⁹
```

La betterave a **un tirage de plus** : son random tick n'atteint la règle des cultures que deux
fois sur trois. Sans ce tirage (p = 1/3), le rejet est à 10⁻⁴⁷ ; avec deux sur trois « pour rien »
(p = 1/9), il serait pire encore.

La canne à sucre et le cactus prennent un âge par random tick et poussent au seizième : la
fraction qui a poussé en 303 ticks à K = 200 est P(Poisson(14,8) ≥ 16) = 0,39, soit 23 sur 59
attendus — **22** cannes et **23** cactus observés.

### 4.4 La torchflower, corrigée par la mesure

Écrite d'abord comme la betterave (tirage 1 sur 3, p = 2/9) — c'est ce que suggère sa parenté de
code avec elle. Rejeté : **p = 0,008** contre 2/9, **p = 1,0** contre 1/3. La torchflower suit la
règle ordinaire. L'âge 2 n'existe pas sur le bloc : la culture qui y arrive **devient** le bloc
`torchflower` (36 sur 48 dans la première campagne).

Le **pitcher** : même règle ordinaire à 1/3, cinq stades, et à partir de l'âge 3 un second bloc
`half=upper` au-dessus portant le même âge (34 pitchers à 4 et 10 à 3 ont tous leur moitié haute,
les 4 à l'âge 2 n'en ont pas). S'il faut de l'air au-dessus pour grandir n'est **pas mesuré** :
tous les pitchers échantillonnés en avaient.

### 4.5 Notre serveur contre vanilla

`test_agriculture.cpp` construit les mêmes tuiles dans de vraies `ChunkSection`s, les fait
passer par `RandomTicks::tick_section` — le code du serveur — à K = 100 pendant 605 ticks, cinq
champs indépendants, et compare les histogrammes à ceux de vanilla (χ² d'homogéneité) :

| groupe | p (homogénéité avec vanilla) | témoin (même histogramme contre un p faux) |
|---|---:|---:|
| A — blé épars humide | **0,745** | 0 (p = 1/6) |
| C — blé dense humide | **0,578** | 0 (p = 1/3) |
| R — rangs secs | **0,816** | — |
| B — betterave | **0,736** | 7,5·10⁻⁹⁷ (p = 1/3) |

Les p-values sont imprimées à chaque exécution (`WARN`), pas seulement en cas d'échec.

---

## 5. Les feuilles — exactes

### 5.1 Ce qui tient une feuille

`measure_agriculture.py leaves` : une source, puis une ligne de dix feuilles posées **une par
une** par `setblock` depuis la source (`setblock` recalcule la forme du bloc posé depuis ses
voisins, donc chaque feuille reçoit la distance de la précédente). Relecture à vitesse 0, puis
après 603 ticks à K = 200 :

| source | distances lues | après |
|---|---|---|
| `oak_log` | 1 2 3 4 5 6 7 7 7 7 | les six premières restent, les quatre suivantes tombent |
| `stripped_oak_log` | idem | idem |
| `oak_wood` | idem | idem |
| `crimson_stem` | idem | idem |
| dix essences de feuilles en chaîne | idem | idem — la distance traverse n'importe quelle feuille |
| `mangrove_roots` | 7 partout | tout tombe |
| `oak_planks` | 7 partout | tout tombe |
| feuille touchant le tronc **par un coin** seulement | 7 | tombe |
| feuilles `persistent=true` isolées | 7 | **restent** |

La source est exactement `#minecraft:logs` (qui contient les tiges du Nether et pas les racines de
palétuvier). ⚠️ Le wiki courant parle d'un tag `prevents_nearby_leaf_decay` : **il n'existe pas en
1.20.1** (absent du datapack généré) — c'est la documentation d'une version plus récente.

Puis une nappe de 17 × 17 autour d'un tronc, posée anneau par anneau : **288 / 288** cellules ont
pour distance `min(Manhattan, 7)`, et après les random ticks exactement celles de distance ≤ 6
restent — un losange.

### 5.2 Le chemin dans le serveur

Une feuille notifiée (voisin changé, ou elle-même écrite) compare la distance que lui donnent ses
six voisins à la sienne ; si elles diffèrent, elle demande un block tick à 1, qui réécrit la
distance, qui notifie ses voisins. Une feuille à 7 non persistante tombe à son random tick, avec
son butin (la table de butin du bloc, conditions comprises). Le test d'intégration fait passer tout
cela par `ServerLevel` et `WorldTicks` : huit feuilles posées à 7 prennent 1..7 par la vidange ; le
tronc retiré, elles remontent à 7 d'un bloc par tick ; les random ticks les font tomber, 8 butins.

Les feuilles **posées par un joueur** sont `persistent=true` (`placed_state`, une ligne) : sans
cela elles arrivaient à distance 7 et le premier random tick les prenait.

### 5.3 Les arbres générés portaient tous distance 7

Le jeu fait une dernière passe sur un arbre fini : chaque feuille de la boîte de l'arbre reçoit sa
distance au `#logs` le plus proche, à travers les feuilles, jusqu'à six. `features.md` le notait :
« Cette passe n'est **pas** implémentée : nos feuilles sortent avec la `distance` du
fournisseur » — c'est-à-dire 7, le fournisseur de `oak.json` écrit `distance: "7"`. Sans random
tick c'était invisible ; **avec**, le premier passage aurait fait tomber toutes les forêts.

Mesuré sur le monde de référence (graine 1234567890, 48 × 48 colonnes, y 60..119) :

| | distance 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---:|---:|---:|---:|---:|---:|---:|
| `oak_leaves` | 505 | 864 | 762 | 377 | 193 | 42 | 0 |
| `jungle_leaves` | 522 | 837 | 590 | 327 | 223 | 76 | 1 |

La passe est maintenant dans `TreeFeature::place` (`ov_worldgen`, un bloc à la fin) : un parcours
en largeur depuis les `#logs` de la boîte, six couches. Elle ne tire rien, donc ne change aucune
forme ni aucun flux aléatoire : les 73 tests de `test_ov_worldgen` passent inchangés.

---

## 6. La fonte — une de moins que le wiki pour la glace

`measure_agriculture.py melt` : un bloc `light[level=N]` (invisible, sans collision) pour
N = 8..15, et contre lui de la glace sur pierre, de la glace au-dessus du vide, une couche de
neige, et de la glace à deux blocs. La lumière **de bloc stockée dans la cellule** est relue par
`ov_inspect light` avant la fonte ; 603 ticks à K = 200.

| lumière dans la cellule | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 |
|---|---|---|---|---|---|---|---|---|
| glace | reste | reste | reste | **reste** | **fond** | fond | fond | fond |
| neige (couche) | reste | reste | reste | reste | **reste** | **fond** | fond | fond |

La glace fond **au-dessus de 10**, la neige au-dessus de 11. Le wiki donne « au-dessus de 11 »
pour les deux ; la glace prend un niveau à la lumière qui y entre, et le seuil est compté après.
La glace au-dessus du vide devient de l'eau, comme celle posée sur pierre (l'eau coule ensuite) ;
dans une dimension *ultrawarm* elle disparaît (wiki, non mesuré).

---

## 7. La poudre d'os

### 7.1 Distributions (distributeurs, une poudre chacun, random ticks coupés)

| cible | vanilla | règle |
|---|---|---|
| blé âge 0, 2 × 100 | +2 / +3 / +4 / +5 : 54 / 52 / 47 / 46 | `2 + next_int(4)`, uniforme |
| tige de melon âge 0, 2 × 100 | 49 / 45 / 46 / 59 | idem |
| pousse de chêne, 2 × 100 | stade 1 : **90 / 199** (0,452) | 45 % |
| baies sucrées, carottes âge 6 | +1, 199 / 199 | +1 |
| tige âge 5 | 100 / 100 à l'âge 7, **7** melons posés sur-le-champ | voir ci-dessous |
| betterave | voir § 7.3 | |

La tige menée à 7 par la poudre prend **un random tick sur-le-champ** : 7 melons sur 100, tous
sur le seul côté libre du montage, là où un tirage ordinaire donne 1/5 (points 5,5 ici) × 1/4
(le côté) = 5 %. `bone_meal` le fait.

### 7.2 L'herbe — corrigée par la mesure

64 parcelles d'herbe plate, une poudre chacune, tout ce qui pousse relevé dans un carré de 15 :
**14,45** herbes par parcelle (σ 2,81, de 7 à 20), 2,0 hautes herbes, 2,7 fleurs (pissenlit,
bleuet, houstonie, marguerite, coquelicot). Par anneau de Tchebychev, fraction des cellules en
herbe ou haute herbe (fleurs exclues) : 0,92 / 0,72 / 0,36 / 0,11 / 0,03.

L'algorithme (128 marches aléatoires depuis le bloc au-dessus, d'autant plus longues qu'elles
sont tardives, qui s'arrêtent sur tout ce qui n'est pas de l'herbe) a d'abord donné **18,8**
herbes. Deux causes, trouvées une par une :

1. **le montage** — le distributeur et le bloc de redstone sont enfoncés dans le sol juste au sud
   de la cible ; rien ne pousse dessus et une marche qui y passe s'arrête. Le test les pose
   exactement là ;
2. **les fleurs** — dans le jeu une fleur occupe sa cellule, et les marches suivantes qui y
   tombent ne font rien. Laissée vide, notre cellule était remplie d'herbe par une marche
   ultérieure. Elle est maintenant retenue vide.

Après : **14,86** herbes, 1,85 hautes herbes, anneaux 0,86 / 0,70 / 0,37 / 0,12. **Nommé :** les
fleurs elles-mêmes ne sont pas posées (elles viennent de la feature de fleurs du biome, et un
biome n'est pas sur l'interface de niveau) ; l'issue le dit (`unsupported`).

### 7.3 La betterave : le wiki a raison, et le banc a failli dire le contraire

Le wiki : « 75 % de chance d'avancer d'un stade ». La règle écrite est le tirage des autres
cultures divisé par trois, `(2 + next_int(4)) / 3` : +0 une fois sur quatre, +1 trois fois sur
quatre, jamais +2.

Trois mesures, et pourquoi il en a fallu trois :

| campagne | avancées d'un stade | fraction | contre 3/4 | contre 2/3 |
|---|---:|---:|---:|---:|
| 2 × 100 + 100, ratés non triés | 207 / 298 | 0,695 | z = −2,2 | z = +1,0 |
| 1 500, ratés non triés | 1 092 / 1 500 | 0,728 | z = −2,0 | z = +5,0 |
| 1 500, **chaque distributeur vérifié** | **1 111 / 1 500** | **0,741** | **z = −0,8** | z = +6,1 |

Jamais un seul +2 sur 3 298 betteraves. Les deux premières campagnes penchaient vers 2/3 : un
distributeur qui ne tire pas laisse la betterave à l'âge 0, exactement comme une poudre qui fait
+0. Une fois l'inventaire de chaque distributeur relu, les 1 500 avaient tiré, et 3/4 tient là où
2/3 est rejeté à six écarts-types. C'est le seul endroit de ce dossier où le témoin d'un banc
(le blé, qui ne peut pas rester à 0) a dû être importé d'une autre rangée pour lire la bonne.

### 7.4 Ce qui ne prend pas la poudre, ou n'est pas fait

La verrue du Nether, la canne à sucre et le cactus ne la prennent pas (wiki) — `bone_meal` rend
Pass et rien n'est consommé. **Nommés et non faits** (`unsupported`) : varech, pitcher, bambou,
mousse, champignons, cave vines, lichen, dripleaf, cornichons de mer, herbes marines, lianes du
Nether, nylium, terre enracinée, feuilles de palétuvier, pétales, fleurs.

---

## 8. Le reste, spécifié depuis le wiki

Non mesuré ici, et dit comme tel :

* **Tiges** : même formule que les cultures ; à l'âge 7, un côté horizontal au hasard, air
  au-dessus de terre labourée ou `#dirt` → le fruit, et la tige devient `attached_*_stem` tournée
  vers lui ; le fruit retiré, elle redevient tige d'âge 7.
* **Canne à sucre** : sur `#dirt` ou `#sand` avec de l'eau (ou de la glace givrée) contre le bloc
  support, jusqu'à trois de haut ; déracinée par la perte de l'eau (random tick ou block tick).
* **Cactus** : sur `#sand` ou cactus, rien de solide ni de lave sur les côtés, pas de liquide
  au-dessus ; le nouveau bloc qui ne peut pas tenir se casse au tick suivant.
* **Pousses** : lumière locale ≥ 9 au-dessus, un sur sept ; stade 0 → 1, puis l'arbre. Les arbres
  sont les `configured_feature` du datapack (`oak` avec 10 % de `fancy_oak`, `birch`, `spruce` /
  `mega_spruce` ou `mega_pine` en 2 × 2, `jungle_tree_no_vine` / `mega_jungle_tree`, `acacia`,
  `cherry`, `dark_oak` en 2 × 2 seulement, `mangrove` / `tall_mangrove`, `azalea_tree`), placés par
  l'interpréteur de `ov_worldgen` — côté serveur (`TreeGrower`, couche 12), puisque `ov_gameplay`
  (couche 9) ne peut pas l'inclure. **Non faits :** les variantes à ruches près des fleurs, les
  propagules suspendues.
* **Herbe et mycélium** : meurent sous un bloc opaque ou une eau pleine ; s'étendent sur la terre
  à lumière locale ≥ 9, quatre essais par tick dans un pavé 3 × 5 × 3. L'opacité est celle, par
  bloc, mesurée avec la lumière du ciel : une dalle basse compte comme couvercle ici, alors que le
  jeu interroge la face — nommé. Le drapeau `snowy` suit le bloc au-dessus.
* **Verrue du Nether** (1/10, mesuré § 4.3), **baies** (1/5, mesuré), **cacao** (1/5, mesuré),
  **varech** (14 % par random tick, non mesuré).
* **Plantation** : graines sur terre labourée, verrue sur sable des âmes, baies et pousses sur
  `#dirt` ou terre labourée, cacao sur le flanc d'une bûche d'acajou tourné vers elle, canne et
  cactus selon leur survie. Une touffe d'herbe cliquée est remplacée. Rien ne tombe jamais dans le
  chemin de pose générique, qui mettait une pousse sur de la pierre.
* **Butin** : les tables de butin existantes, conditions d'état comprises — le test de
  `test_loot.cpp` couvre le blé mûr et pas mûr.

**Houe** : déjà mesurée dans `item_use.cpp` (terre, herbe, chemin → terre labourée ; terre
grossière et terre enracinée → terre). **Non fait :** les racines suspendues que lâche la terre
enracinée labourée.

---

## 9. De bout en bout, sur notre serveur

`ov_dedicated` en Debug, monde généré (graine 1234567890), un client sonde au spawn, 3000 ticks,
deux fois : `OV_RANDOM_TICK_SPEED=0` puis `200`.

* **Le client doit frapper plusieurs fois.** En Debug, le chunk du spawn met plus que les 20 s
  que le serveur accorde à une connexion : la sonde a été refusée deux fois (trois pour la
  seconde manche) avant d'entrer, 83 s après le démarrage. Un banc qui ne réessaie pas n'a
  **aucun joueur**, donc aucun random tick — et mesure un serveur qui ne fait rien pousser.
* **Le monde à vitesse 0 n'a rien écrit** sur le disque : ce serveur ne sauve que les chunks
  modifiés, et rien ne les a modifiés. **À vitesse 200, 25 à 29 chunks par sauvegarde** : ceux
  que les random ticks ont écrits. C'est la preuve la plus directe que le tirage passe bien par le
  vrai serveur — et c'est aussi ce qui a rendu la comparaison des deux mondes impossible telle
  qu'elle était prévue, puisque le premier est vide.
* **Coût.** 7 « can't keep up » sur 3000 ticks à vitesse 0, **31** à vitesse 200 — en Debug, et
  à une vitesse soixante-sept fois celle du jeu. Chaque écriture d'une règle rallume le voisinage
  3 × 3 de son chunk (`flush_tick_writes`) ; c'est ce coût-là, et non le tirage, qu'une vitesse
  élevée multiplie. À la vitesse par défaut il n'a pas été mesuré séparément.

**Les forêts tiennent.** Relu dans les 29 chunks sauvés du monde à vitesse 200 (environ 146
random ticks par bloc sur la durée, en supposant la sonde présente) : 8 184 feuilles de chêne,
toutes à distance 1..6 — **aucune à 7** ; 9 347 feuilles d'acajou dont 445 à 7. Avant la passe
de distance de `TreeFeature`, toutes auraient été à 7 et tombées.

Pour savoir si les distances stockées sont **vraies**, `scripts/locate_leaves.py` (lecture directe
des régions par `scripts/anvil_read.py`, quelques secondes au lieu de 25 minutes d'`ov_inspect`)
refait un parcours en largeur depuis toutes les bûches à travers les feuilles, sur les seuls
blocs sauvés. Au bord de la zone sauvée ce parcours ne voit pas les bûches des chunks non sauvés ;
on ne lit donc que les **11 chunks intérieurs**, ceux dont les huit voisins sont sauvés :

| feuilles non persistantes, chunks intérieurs | nombre |
|---|---:|
| distance stockée = distance vraie | **6 746 / 7 447** (90,6 %) |
| stockée à 7 alors qu'un chemin ≤ 6 existe — tomberait à tort | **0** |
| stockée **plus haut** que la vraie (ex. 3 pour 2 : 139) | 590 |
| stockée plus bas que la vraie | 111, dont **71** « tenues » sans aucun chemin |

* Les 590 trop hautes sont sans effet sur la décomposition. Elles s'expliquent par la boîte : la
  passe d'un arbre ne voit pas la bûche d'un arbre posé après lui. Si le jeu fait pareil **n'est
  pas mesuré**.
* Les **71 feuilles tenues sans chemin** sont un écart nommé : le jeu les ferait tomber, nous non.
  Cause non trouvée — une écriture postérieure à la passe (une autre feature) est le candidat,
  non vérifié.
* 440 des 445 feuilles à 7 sont dans les chunks du bord de la zone sauvée. Qu'elles n'aient pas
  été décomposées s'explique le plus probablement par la règle du tirage : un chunk dont les
  huit voisins ne sont pas résidents n'est pas tické (§ 2). **Probable, non prouvé** : la sonde n'a
  été présente que 65 s d'horloge sur les 3000 ticks, pendant lesquels le serveur en Debug a
  sauté des ticks (31 surcharges, piège 22 du briefing), et le journal ne dit pas quels chunks
  ont été tirés.

---

## 10. Fichiers

| fichier | rôle |
|---|---|
| `src/ov_gameplay/{include/ov/gameplay,src}/plants.{hpp,cpp}` | les règles, sur un `LevelWriter&` |
| `src/ov_gameplay/tests/test_plants.cpp` | règles, cartes mesurées, poudre, herbe |
| `src/ov_server/src/agriculture.{hpp,cpp}` | le tirage, les arbres, l'environnement |
| `src/ov_server/tests/test_agriculture.cpp` | parité statistique, chemin serveur des feuilles |
| `src/ov_server/src/world_ticks.{hpp,cpp}` | les plantes dans la vidange et les notifications |
| `src/ov_server/src/server.cpp` | trois blocs `// ── agriculture ──` et une ligne dans `placed_state` |
| `src/ov_worldgen/src/tree_feature.cpp` | la passe de distance des feuilles |
| `scripts/measure_agriculture.py` | l'oracle et l'analyse (`analyse <json…>`) |
| `scripts/locate_leaves.py` | distance stockée contre distance vraie des feuilles d'un monde sauvé |
