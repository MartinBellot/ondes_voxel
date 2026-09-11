# Le combat contre l'Ender Dragon

*2026-09-11. Seed 1234567890. Oracle : le vrai serveur 1.20.1 (`tools/vanilla/server.jar`) et
`scripts/measure_dragon.py` — un client sonde en survie (résistance V, régénération, saturation)
qui enregistre, horodaté, chaque paquet du combat : le dragon (apparition, mouvements, métadonnées
9 et 16), les cristaux (métadonnées 8 et 9), les boules de feu, les nuages, les explosions (avec
leur puissance), les orbes, les événements de monde, la barre de boss, les blocs. Trois passages du
vrai serveur (`vanilla.json`, `vanilla2.json`, `vanilla_respawn.json`), un passage de notre serveur
(`--ours`). Les mondes sont effacés une fois lus.*

Suite de `docs/provenance/end.md` § 4, qui avait posé le début du combat (barre, cristaux qui
soignent, parties, mort en tests unitaires). Ce document dit ce qui a été établi, comment, avec
quels chiffres, et ce qui n'est **pas** fait.

---

## 1. Les sources

| sujet | source |
|---|---|
| phases, transitions, probabilités, anneaux de nœuds, règles de chemin, souffle, charge, décollage, mort, expérience | page « Dragon fight mechanics » du wiki de la communauté speedrun (`mcsr.miraheze.org`), prose ; image « Dragon Node Numbers » (numérotation et position des 24 nœuds) |
| dégâts (tête entière, reste d/4 + min(d, 1)), immunités, blocs `dragon_immune`, ailes 5 / tête 10, 0,5 s de répit après un coup, 12 000 puis 500 XP, 604 ticks de réinvocation | minecraft.wiki, « Ender Dragon » (Java 1.20) |
| cristal : puissance 6, 10 de dégâts au dragon, disparition sans explosion sous une explosion, feu dans l'End | minecraft.wiki, « End Crystal » |
| nuage : rayon, rayon par tick, attente, 3 / 6 PV par seconde | minecraft.wiki, « Lingering Potion » et « Ender Dragon » ; **mesuré** (§ 4) |
| tout le reste | **mesuré** sur le vrai serveur |

La page MCSR contient des extraits de code : ils n'ont pas été traduits. Les règles sont écrites
depuis la prose qui les décrit, dans la structure de ce dépôt ; les liaisons entre nœuds, que
aucune source lue ne donne, sont reconstruites par la géométrie (§ 2.2) et nommées comme telles.

---

## 2. Le vol

`ov_gameplay/dragon.{hpp,cpp}` : le graphe, les phases, l'intégrateur de vol, les dégâts. Aucune
dépendance au serveur : un test fait voler un dragon 24 000 ticks sans niveau.

### 2.1 Les phases (métadonnée 16)

0 holding_pattern · 1 strafe_player · 2 landing_approach · 3 landing · 4 takeoff · 5 sitting_flaming ·
6 sitting_scanning · 7 sitting_attacking · 8 charging_player · 9 dying · 10 hover — les numéros du tag
`DragonPhase`, vus sur le fil dans les trois passages.

- **holding_pattern** : à la fin d'un chemin, 1 chance sur (n + 3) d'aller se poser (n = compte des
  cristaux), puis une chance de strafe `1 − ((⌊d²/512⌋ + 1)/(⌊d²/512⌋ + 2)) · ((n + 1)/(n + 2))`
  (d = distance du joueur ciblable le plus proche au sommet de la fontaine) — tirée comme deux
  tirages, un par facteur ; sinon un nouveau chemin vers le nœud voisin du plus proche, sens
  inversé (et six nœuds plus loin) une fois sur huit ; la cible est le nœud suivant plus 0 à 20
  blocs de hauteur, renouvelée à moins de 10 ou plus de 150 blocs.
- **strafe_player** : un chemin vers le nœud le plus proche du joueur, puis le joueur lui-même ; à
  moins de 64 blocs et en vue, un compteur monte, et à 5 avec un cap à moins de 9,5° : la boule de
  feu, et retour au holding. Déclenché par un cristal détruit (le destructeur s'il est en survie,
  personne s'il est en créatif, sinon le joueur le plus proche du cristal).
- **landing_approach → landing → sitting_scanning** : un chemin vers le nœud à l'opposé du joueur
  (−40 · direction du joueur, y 105), puis la descente sur la fontaine, accélération verticale
  0,015 ; posé à moins d'un bloc.
- **sitting_scanning** : un joueur en vue dans une boîte de 20 × 10 × 20 : au 26ᵉ tick, le rugissement
  (sitting_attacking, 40 ticks) puis le souffle (sitting_flaming : le nuage au 10ᵉ tick, 200 ticks) ;
  quatre souffles par perchoir puis décollage ; personne dans la boîte : au 100ᵉ tick, charge sur le
  joueur en vue le plus proche à moins de 150 blocs, sinon décollage.
- **charging_player** : jusqu'à 10 ticks après avoir atteint la cible, accélération verticale 0,03.
- **takeoff** : un chemin vers le nœud devant la tête, puis holding à 10 blocs de la fontaine.
- **dying** : vers la fontaine ; santé 1 entre 10 et 150 blocs, 0 sinon — l'animation commence.
- **dégâts perché** : cumulés (arrondis à l'entier) ; au-delà de 50, décollage.

**La ligne de vue compte — mesuré.** Au premier passage, le dragon perché n'a pas chargé la sonde
restée sur la plateforme d'arrivée (100 blocs, 15 plus bas, derrière le rebord de l'île) : il a
décollé au 100ᵉ tick. La vue est un rayon de blocs (`DragonSurroundings::sees`, un `raycast_voxels`
sur le niveau de l'End côté serveur).

### 2.2 Les nœuds

24 nœuds sur trois anneaux, numérotés comme l'image MCSR : 0–11 sur le rayon 60, 12–19 sur 40,
20–23 sur 20, chaque anneau partant de +x et tournant vers +z (le nœud 3 en (0, 60), le 12 en
(40, 0)). Hauteur : `max(73, sol + 5)`, `+ 15` pour l'anneau du milieu. L'anneau extérieur est coupé
quand le compte des cristaux est nul. Les positions sont calculées en `float` : `60 × sin 30°` vaut 30
en float et 29,999… en double — l'image ne tranche pas un bloc, le choix est nommé.

**Les liaisons ne sont dans aucune source lue** : chaque nœud est relié à ses deux voisins
d'anneau, chaque nœud extérieur au(x) nœud(s) du milieu le(s) plus proche(s) en angle, chaque nœud du
milieu au(x) nœud(s) intérieur(s) le(s) plus proche(s). Chemin : Dijkstra sur les distances entre
centres. Tout nœud atteint tout autre (test).

### 2.3 L'intégrateur — ajusté

La documentation donne les plafonds d'accélération verticale de l'atterrissage (0,015) et de la
charge et de la mort (0,03). Le reste est ajusté à la mesure, pas pris d'une description du jeu :

| mesure (fenêtres de 10 ticks ; le jeu n'envoie un mouvement du dragon que tous les **3 ticks**) | vanilla, passage 1 | vanilla, passage 2 | nous |
|---|---|---|---|
| vitesse horizontale, holding : p10 / p50 / p90 | 0,54 / 0,89 / 1,14 | 0,50 / 0,78 / 1,07 | *§ 8* |
| vitesse verticale, holding : p10 / p90 | −0,058 / 0,055 | −0,053 / 0,058 | *§ 8* |
| vitesse verticale, landing (p50) | −0,166 | −0,166 | *§ 8* |
| montée de l'animation de mort | +0,100 / tick | +0,100 | +0,1 par construction |
| lacet du fil − direction du vol | 160 à 200° | 160 à 200° | 180° (`wire_yaw`) |

Le lacet que le jeu envoie est celui de la direction de vol **plus un demi-tour** : le modèle du
client regarde à l'envers de son `yRot`. Un dragon envoyé avec son cap de vol vole à reculons sur
l'écran.

**Le jeu contre lui-même** — la référence contre laquelle lire tout écart (piège 14 du briefing) :
les deux passages du vrai serveur, phase holding seulement, positions reconstituées tick par tick
depuis les paquets.

| holding pattern | passage 1 | passage 2 | écart jeu / jeu |
|---|---|---|---|
| rayon depuis le centre : p10 / p50 / p90 | 15,7 / 43,6 / 84,9 | 18,8 / 40,2 / 76,6 | 3 / 3 / 8 blocs |
| hauteur : p10 / p50 / p90 | 73,9 / 83,3 / 95,2 | 77,3 / 82,6 / 95,4 | 3 / 1 / 0 |
| vitesse horizontale (10 ticks) : p10 / p50 / p90 | 0,5 / 0,9 / 1,1 | 0,5 / 0,8 / 1,1 | 0 / 0,1 / 0 |

Restreint à la fenêtre **avec cristaux** (les 90 premières secondes, sonde sur la plateforme) —
celle que l'ajustement vise :

| holding, cristaux debout | rayon p10 / p50 / p90 | hauteur p10 / p50 / p90 | vitesse p10 / p50 / p90 |
|---|---|---|---|
| jeu, passage 1 | 24,7 / 44,1 / 78,5 | 85,3 / 91,1 / 117,1 | 0,49 / 0,86 / 1,11 |
| jeu, passage 2 | 27,8 / 57,1 / 83,8 | 78,7 / 85,3 / 104,7 | 0,50 / 0,78 / 1,08 |
| nous, premier modèle (virage fixe 5°, poussée constante), de bout en bout | 16,3 / 40,7 / 58,6 | 73,1 / 93,4 / 120,4 | 0,68 / 0,75 / 0,97 |
| nous, modèle retenu, simulé une heure (`test_ov_gameplay "[.fit]"`) | 19,6 / 47,7 / 65,4 | 73,2 / 82,6 / 87,5 | 0,55 / 0,91 / 1,16 |

Le modèle retenu : un **virage proportionnel** (un dixième de l'écart au cap voulu par tick,
10° au plus), une poussée de 0,12 contre une traînée de 0,9 (1,2 bloc/tick en ligne droite),
diminuée dans les virages (`× (1 − (1 − cos écart))`, un dixième au moins). Vitesse et rayon
médian tombent dans l'écart du jeu à lui-même ; les grandes embardées restent plus courtes (p90
du rayon 65 contre 78–84), la hauteur p90 aussi — la fenêtre du jeu commence à l'apparition, en
y 128, ce qu'une heure de simulation dilue. Nommé.

Deux pièges trouvés par l'ajustement, pas par la mesure :

- **Un virage à taux fixe fait tourner le dragon autour de sa cible pour toujours.** La cible
  n'est remplacée qu'à moins de 10 blocs ; à 0,9 bloc/tick et 3°/tick, le cercle de virage fait
  17 blocs de rayon : le dragon ne l'atteint jamais (hauteur et vitesse rigoureusement constantes
  pendant une heure simulée). D'où le virage proportionnel, dont le rayon descend sous 10 blocs.
- **Un strafe sans ligne de vue ne finit jamais** — la règle documentée n'a pas d'autre sortie que
  le tir. Une simulation qui cache le joueur partout passe 98 % du temps en strafe ; le jeu ne
  cache le joueur que depuis le perchoir, derrière le rebord de l'île (mesuré).

---

## 3. Les cristaux

| | le jeu (mesuré) | nous |
|---|---|---|
| explosion d'un cristal frappé | **puissance 6,0**, centre = pieds du cristal ((cx + 0,5 ; h + 1 ; cz + 0,5)), 1 341 à 1 430 enregistrements, **74** pour les deux cristaux en cage | puissance 6, même centre, blocs par `Explosions::collect_cells` (pas de butin : nommé) |
| entendue | par les joueurs à moins de 64 blocs : une sonde à 100 blocs n'en a entendu **aucune** sur dix | idem |
| le cristal qui soignait | le dragon perd **10** (200 → 190, deux fois) ; soin **+1 tous les 10 ticks** ensuite | idem (`Dragon::lose_crystal`, la tête, explosion) |
| phase après destruction | strafe **dans le tick**, dix fois sur dix | `crystal_destroyed` |
| cristal touché par une explosion | disparaît sans exploser (wiki) | idem |
| compte des cristaux | 0 au chargement, recompté tous les 100 ticks et à chaque destruction | idem |
| feu | le cristal garde un feu dans son bloc dans l'End (wiki ; le feu relevé sur cinq piliers, end.md § 1.5) | idem |

---

## 4. La boule de feu et les nuages

| | le jeu (mesuré) | nous |
|---|---|---|
| tir | événement de monde **1017** à la position du dragon, apparition de `dragon_fireball` (vitesse 0) | idem |
| vol | part à l'arrêt, poussée 0,1 / tick, traînée 0,95 : cette règle donne **53,4** blocs en 46 ticks, mesuré **53,6** | idem |
| impact | événement **2006** (donnée 1) ; le nuage aux **pieds** du joueur touché, au point d'impact sur un bloc | idem |
| nuage de boule de feu | couleur **11101546**, particule **8** (`dragon_breath`), attente 20 ticks (index 10), rayon 3 → +0,00667 / tick (index 8 envoyé chaque tick), retiré à **620** ticks | idem ; nocif : Instant Damage II à demi-force, 6 PV / s |
| nuage du souffle | rayon **5,0** constant (envoyé à l'apparition), même couleur et particule, attente 20, posé au sol devant la tête (y 64,0), retiré **190** ticks plus tard (fin de la phase) | idem ; 3 PV / s |
| fiole | — (non mesurée) | une fiole utilisée à moins de 2 blocs d'un des deux nuages : `dragon_breath`, le nuage perd 0,5 de rayon (wiki) |

---

## 5. La mort

| | le jeu (mesuré) | nous |
|---|---|---|
| coup mortel en vol | Entity Event **3**, puis `{pose 7, santé 1,0, phase 9}` dans une métadonnée | idem |
| vol vers la fontaine | 6,0 s au passage 2 (le dragon était loin), 0 au passage 1 (déjà posé) | phase `dying` |
| début de l'animation | `{santé 0,0}` et événement **1028** global dans le même tick | idem |
| expérience | 1ʳᵉ orbe à **154** ticks de l'animation, puis tous les 5 ticks : dix lots de 960 (617 + 307 + 17 + 17 + 1 + 1), le dernier avec 2 400 (1237 + 617 + 307 + 149 + 73 + 17) = **12 000** | dix lots à 155…200, total 12 000 ; 500 pour un dragon réinvoqué |
| fin | barre retirée et dragon retiré **200** ticks après le début de l'animation | idem |
| `/kill` | pas d'animation, pas d'orbe, portail ouvert tout de suite (end.md § 4.1) | idem (au tick suivant) |

---

## 6. La sauvegarde : `DragonFight`

Relu dans `level.dat` après une mort, au milieu d'une réinvocation et après :

| clé | le jeu | nous |
|---|---|---|
| `NeedsStateScanning` | octet 0 dès que le combat a commencé | idem ; 1 avant |
| `ExitPortalLocation` | **tableau d'entiers** [0, 63, 0] (pas un composé X/Y/Z) | idem ; la forme composée est aussi lue |
| `Gateways` | tableau d'entiers : les indices restants, le prochain à la fin — [10, 0, 18, …, 6] après une mort | idem, **identique** |
| `DragonKilled`, `PreviouslyKilled` | octets | idem |
| `Dragon` | UUID en tableau de 4 entiers, **gardé après la mort** ; remplacé à la réinvocation | idem |
| `IsRespawning` | **absent**, même huit secondes dans une réinvocation | absent |

L'ordre des clés est celui du jeu ; la comparaison se fait clé par clé (piège 25 du briefing). Une
sauvegarde écrite par le jeu se relit (test : le composé mesuré, relu, redonne le même `Dragon`).
Ce qui n'est pas sauvé : la santé et la position du dragon, les cristaux détruits (un redémarrage
avec un dragon vivant en redonne un neuf, à (0, 128, 0), avec son UUID).

---

## 7. La réinvocation

Mesurée au troisième passage (dragon retiré par `/kill`, quatre cristaux posés du sol) :

| t (ticks depuis le 4ᵉ cristal) | le jeu | nous |
|---|---|---|
| ~17 | les quatre visent (0, 128, 0) ; le portail redevient inactif (ses torches cassent, événements 2001) | tick 1 |
| 103 + 40 k | les quatre visent le bloc du cristal du pilier k (cx, h + 1, cz), dans l'ordre de `end_spikes` | idem |
| 141 + 40 k | le cristal du pilier disparaît, **explosion de puissance 5,0** en (cx + 0,5 ; h ; cz + 0,5), ~1 950 enregistrements ; un cristal réapparaît, faisceau sur (0, 128, 0) | idem ; pilier reconstruit (forme de la feature) |
| ~504 | les quatre visent (0, 128, 0) | 504 |
| **603** | le dragon (phase 0, 200 PV) ; les quatre explosent (puissance 6, **0** enregistrement : aucun bloc) ; tous les faisceaux s'éteignent ; le dragon passe en strafe | 604 |
| `DragonFight` | `DragonKilled` reste 1 pendant ; 0 après, nouvel UUID ; `Gateways` inchangé | idem |

Deux essais ratés avant : la sonde volait à y 70 (portée), puis restait en l'air (`allow-flight`)
— la pose d'un cristal exige l'œil à portée du bord du portail. Posés du sol, les quatre prennent.

---

## 8. De bout en bout, contre notre serveur

`scripts/measure_dragon.py --ours` : `ov_dedicated` (Debug, overworld superflat, donc un End de
graine 0 — ses positions ne se comparent pas au bloc près à celles de la graine 1234567890 : le
portail de sortie y est en (0, 62, 0)), la même sonde, le même scénario. La sonde entre par un
portail posé à la console, détruit les dix cristaux en les frappant depuis leur pilier, reste
deux minutes près de la fontaine, puis tue le dragon **en le frappant à la tête avec une épée
en netherite** — aucun dégât de commande —, puis pose les quatre cristaux de la réinvocation.

Deux passages. Le premier, poings nus, a mené le dragon de 200 à 24,2 PV en 180 s (1 PV par
coup, 263 coups) : les coups arrivent, la sonde était sous-armée. Le second, à l'épée :

| | le jeu (passage 2 sauf mention) | nous |
|---|---|---|
| cristaux frappés | dix explosions de puissance 6,0 ; 1 341 à 1 430 enregistrements, 74 pour les deux en cage | dix de puissance 6,0 ; 1 342 à 1 383, **75** pour les deux en cage |
| cristal qui soignait détruit | −10 PV (trois fois) | −10 PV (neuf fois : le dragon se soignait plus souvent au moment du coup) |
| après chaque destruction | strafe | strafe |
| boules de feu, nuages de souffle | 5 boules, 4 souffles de rayon 5 | 6 boules, 2 souffles de rayon 5 |
| mise à mort | `damage … 1000` (commande) | **25 coups d'épée** à la tête : `dying`, vol vers la fontaine, animation, retrait |
| expérience | 65 orbes vues sur 66, 11 383 points sur 12 000 (la sonde en perd hors de portée de suivi) | **12 000 points exactement** ; 60 orbes — le dernier lot sommé au 960 du même tick : corrigé ensuite, 66 (test) |
| animation → retrait | 200 ticks | 238 ticks d'horloge murale : notre serveur Debug ne tenait pas 20 TPS (« can't keep up ») ; 200 ticks de serveur par construction |
| `DragonFight` après la mort | `NeedsStateScanning` 0, `ExitPortalLocation` [0, 63, 0], `Gateways` sans le dernier indice, tués 1 / 1, UUID gardé | **mêmes clés, mêmes types** : [0, 62, 0] (graine 0), 19 passerelles, 1 / 1, UUID gardé |
| pendant la réinvocation | `DragonKilled` 1, pas d'`IsRespawning` | idem |
| après | `DragonKilled` 0, nouvel UUID, `Gateways` inchangé | idem |
| réinvocation | faisceaux → (0, 128, 0), puis vers le bloc du cristal de chaque pilier dans l'ordre de `end_spikes`, explosions de puissance **5,0** (~1 950 enregistrements), cristal réapparu visant (0, 128, 0) ; le dragon ; quatre explosions de puissance 6,0 à **0** enregistrement ; faisceaux éteints | la même suite, dans le même ordre, mêmes puissances, les quatre dernières à 0 enregistrement ; 36 s d'horloge murale (604 ticks de serveur) ; les explosions de pilier avaient **0** enregistrement — soufflées au tick suivant, après la reconstruction, depuis la bedrock : corrigé (vider, souffler, reconstruire), *§ 8.1* |

Le vol, fenêtre « avec cristaux » (§ 2.3) : rayon 21,7 / 40,0 / 52,5, hauteur 74,0 / 92,8 / 121,2 —
dans l'écart du jeu à lui-même sauf le p90 du rayon (52 contre 78–84) ; vitesse 0,52 / 0,58 / 1,02
**par tick d'horloge murale** : un serveur qui ne tient pas 20 TPS la sous-estime d'autant (×1,19
mesuré sur l'animation de mort, soit une médiane de l'ordre de 0,69). Le rayon et la hauteur ne
dépendent pas de l'horloge.

### 8.1 Après correction

Un passage `--respawn --ours` (dragon retiré par `/kill` — le portail s'ouvre, `DragonKilled` 1,
pas d'orbe —, puis les quatre cristaux) :

| explosions de pilier de la réinvocation | le jeu | nous, avant | nous, après |
|---|---|---|---|
| puissance | 5,0 | 5,0 | 5,0 |
| enregistrements | 1 908 à 1 970 | **0** (soufflées depuis la bedrock déjà reconstruite) | **1 739 à 1 915** |
| centre | (cx + 0,5 ; h ; cz + 0,5) | idem | idem |

Ce passage n'a vu que neuf piliers : notre serveur Debug, sur une machine chargée, avançait
un pilier toutes les 3 à 7 s au lieu de 2, et la fenêtre de 45 s de la sonde s'est refermée
avant le dragon. La suite complète (dragon revenu, faisceaux éteints, `DragonFight` après) est
celle du passage précédent, § 8. L'orbe : 66, par le test (`test_end_fight`), le passage
précédent en ayant compté 60 avant correction.

---

## 9. Ce qui n'est pas fait — nommé

| sujet | état |
|---|---|
| Rendu du dragon dans notre client | **non** : `apps/ov_voxel/src/entities.cpp` refuse par son nom tout type sans modèle, et `entity_models.json` n'a ni dragon ni cristal. Il faudrait importer la géométrie, écrire l'animation (ailes, cou et queue qui suivent les positions passées), le cristal, le faisceau, la boule de feu et les nuages : un chantier à part |
| Liaisons du graphe de nœuds | reconstruites par la géométrie (§ 2.2) |
| Intégrateur de vol | ajusté aux distributions mesurées, pas au tick près ; le RNG du dragon n'est pas celui du jeu (non reproductible de toute façon : il est semé au hasard) |
| Boîtes des parties | disposées depuis la taille documentée, non mesurées ; elles servent aux blocs cassés et aux ailes, pas aux coups (le client envoie l'id de la partie) |
| Poussée des ailes | 4 horizontal, 0,2 vertical : non mesurée |
| Suivi du dragon | vu dans un rayon de 192 blocs de (0, 128, 0) ; le jeu le suit à 160 blocs de chaque joueur |
| Butin des blocs cassés | aucun (le jeu vide les conteneurs) |
| Flèches et tridents | ne touchent ni le dragon ni un cristal (les projectiles vivent dans le monde d'entités de l'overworld) |
| Orbes de l'End | ne fusionnent pas, ne sont pas sauvées |
| Santé et position du dragon, cristaux détruits | non sauvés (§ 6) |

---

## 10. Pièges payés ici

1. **Le jeu n'envoie un mouvement du dragon que tous les trois ticks.** Une vitesse « par tick »
   calculée paquet à paquet est du bruit (p90 de 3 à 7 blocs) ; il faut des fenêtres.
2. **Le lacet du dragon est retourné** : +180° par rapport à son vol.
3. **Une explosion n'est envoyée qu'à moins de 64 blocs** : dix cristaux détruits, zéro paquet
   Explosion pour une sonde restée sur la plateforme.
4. **Une sonde qui ne simule pas sa chute se fait expulser** (« floating too long ») dès qu'une
   explosion la projette : `allow-flight=true` côté vanilla.
5. **Poser un objet exige l'œil à portée** : deux réinvocations ratées depuis y 70.
6. **`/tp` depuis la console atterrit dans le niveau de la console** (l'overworld) sans
   `execute in`.
7. **`sin(30°)` n'est pas 0,5 en double** : 60 × sin 30° se tronque à 29.
8. **`DragonFight` n'a pas d'`IsRespawning`** en 1.20.1, et ses positions sont des tableaux
   d'entiers, pas des composés.
