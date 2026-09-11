# L'End : génération, troisième niveau, portails, combat

*2026-09-11. Seed 1234567890. Oracles : `.scratch/reference-end-1234567890` (généré par le vrai
serveur 1.20.1 via `scripts/reference_end.sh`, supprimé une fois lu — le script le régénère à
l'identique), `scripts/measure_end_portal.py` (le vrai serveur, un client sonde qui pose douze
yeux, entre, meurt, revient, tue le dragon, sort), le même script `--ours` contre notre serveur.*

Ce document dit ce qui a été établi, comment, avec quels chiffres, et ce qui n'est **pas** fait.

---

## 1. Génération

### 1.1 `end_islands` : un nœud sans paramètres

`noise_settings/end.json` nomme `minecraft:end_islands` trois fois (l'érosion, `sloped_cheese`,
la densité initiale) et ce nœud n'a **aucun** champ : l'algorithme est le nœud. Il est écrit dans
`ov/worldgen/end.hpp` depuis sa description technique, pas depuis un code :

- un bruit simplex 2D (`SimplexNoise`), semé comme une octave d'`ImprovedNoise` — trois décalages
  tirés puis le même Fisher-Yates partiel — depuis `new LegacyRandomSource(seed)` **après 17 292
  tirages jetés** (66 × 262 : les octaves que l'ancien générateur de l'End créait avant) ;
- une hauteur par point du plan au huitième (x / 8, z / 8, **division tronquée**) : le cône
  `100 − 8 · distance` de l'île principale, borné à [−100, 80] ; puis, sur une grille d'un point
  sur deux de ce plan, à plus de 64 de l'origine (`o² + p² > 4096`, en `long`), une petite île
  partout où le simplex descend sous **−0,9F** (le float élargi, pas −0,9) — sa raideur est un
  hachage flottant de ses propres coordonnées, `(|o|·3439 + |p|·147) mod 13 + 9` ;
- la densité vaut `(hauteur − 8) / 128`, dans [−0,84375 ; 0,5625] — les bornes que le routeur lit
  pour élaguer (piège 3 du briefing).

Tous les calculs de hauteur sont en `float` Java, les restes et divisions tronqués comme en Java.

### 1.2 La source de biomes : une règle

`TheEndBiomeSource` n'est pas multi-bruit et n'a pas de table exportée. `BiomeSource::load(root,
"end")` rend la règle ; "the_end" échoue toujours (pas de fichier de ce nom, et jamais une table
vide). Dans un rayon de 64 chunks de l'origine (`cx² + cz² ≤ 4096`), toute cellule est `the_end` ;
au-delà, l'entrée `erosion` du routeur (`cache_2d(end_islands)`) lue **au centre du chunk**
(`(section · 2 + 1) · 8`, y = 0) décide : > 0,25 `end_highlands`, ≥ −0,0625 `end_midlands`,
< −0,21875 `small_end_islands`, sinon `end_barrens`. Une seule réponse par chunk, pour tous les y.
L'ordre des cinq biomes de la source (`the_end`, `end_highlands`, `end_midlands`,
`small_end_islands`, `end_barrens`) est celui que le trieur de features lit pour ses égalités.

### 1.3 Les features

| type | ce qui est construit | ce qui ne l'est pas |
|---|---|---|
| `end_spike` | dix piliers d'obsidienne sur un cercle de rayon 42 (`floor(42 cos θ)`, `floor(42 sin θ)`, θ = 2(−π + iπ/10)), rayon `2 + k/3`, hauteur `76 + 3k` où k vient d'un mélange de 0..9 semé par les 16 bits bas du premier `long` de `new Random(seed)` ; disque d'obsidienne (distance au coin bas ≤ r² + 1), air au-dessus de y = 65 dans la boîte, bedrock au sommet, cage de barreaux de fer pour k = 1 et 2 | le cristal (une entité : c'est le combat du serveur qui le pose) |
| `end_island` | disques empilés vers le bas, rayon 4 à 6 diminuant de 0,5 ou 1,5 | — |
| `chorus_plant` | la croissance récursive (`generatePlant`, profondeur 4, écart 8), états joints | — |
| `end_gateway` | le bloc et sa gaine de bedrock | la sortie (`exit`, donnée d'entité de bloc) |

Le sin(−π) de `Math.sin` vaut −1,2·10⁻¹⁶ : le pilier « à −42, 0 » est en **(−42, −1)**.

### 1.4 La mesure

`scripts/reference_end.sh` : le vrai serveur, `execute in minecraft:the_end run forceload`, quatre
carrés de 10 × 10 chunks sur l'île principale et dix carrés au-delà de 1 000 blocs (44 fichiers de
région, 159 Mo, 9 216 chunks sur disque dont 1 786 `full`). Personne n'entre dans l'End : pas de
combat, pas de portail de sortie ni de dragon — ce qui est sur le disque est la génération, plus le
feu que cinq cristaux ont posé sur leur pilier pendant leurs premiers ticks (§ 1.5).
`tools/ov_endparity` lit ce monde :

| question | échantillon | accord |
|---|---|---|
| biomes, cellule 4 × 4 × 4 | 3 790 chunks (statut ≥ `biomes`), 3 880 960 cellules | **3 880 960 (100,0000 %)** — les cinq biomes, chacun à 100 % |
| blocs par `generate()` | 300 chunks laissés avant les features, 19 660 800 blocs | **19 660 715 (99,9996 %)** |
| blocs finis, pipeline + décorateur | 150 chunks `full` pris dans l'ordre des fichiers, 9 830 400 blocs | **100,0000 %** — mais le vide : air et pierre de l'End seulement |
| blocs finis, **chunks où le jeu a autre chose** (`--interesting`) | 120 chunks `full`, 7 864 320 blocs | **7 863 442 (99,9888 %)** — obsidienne 40 499 / 40 499, barreaux 146 / 146, bedrock 10 / 10, pierre de l'End 100 % ; `chorus_plant` 92,22 %, `chorus_flower` 92,39 % ; mêmes propriétés pour 45 528 des 45 537 blocs identiques (99,98 %) |
| piliers : centre, rayon, hauteur, bedrock, cage | 10 | **10 / 10** |
| cristaux (entités du jeu, `DIM1/entities`) | 10 | **10 / 10** en (x + 0,5 ; h + 1 ; z + 0,5) |
| colonne (0, 0) : premier bloc libre | 1 | **64 et 64** — l'anneau du portail de sortie en y = 63 |

Les 85 blocs de différence sur les chunks d'avant les features sont tous des écritures de features
**voisines** : 68 `chorus_plant`, 10 `chorus_flower` et 7 `end_stone` qu'un chunk voisin décoré a
posés dans un chunk que le jeu n'a pas fini (le 3 × 3 d'une décoration déborde, cf.
`feature_border_writes`). `generate()` ne les écrit pas par construction ; ce n'est pas un écart de
génération.

### 1.5 Ce qui reste, nommé

- **Le feu sur les piliers** : cinq des dix portent un bloc de `fire` au-dessus de la bedrock. Ce
  n'est pas la feature : c'est le cristal qui l'allume à son premier tick dans un chunk qui tique ;
  les cinq autres étaient dans des chunks qui n'ont pas tiqué. Notre génération ne le pose pas (le
  cristal du serveur, § 4, non plus).
- **La sortie d'une passerelle** (`exit`, `exact`) n'est pas enregistrée : donnée d'entité de bloc.
- **Le chorus à 92 %.** La plupart des plantes sont identiques au bloc et à l'état près — une graine
  fausse n'en laisserait aucune —, 367 blocs de plante du jeu nous manquent et nous en avons 401 de
  trop. Hypothèse, non vérifiée : une plante grandit dans le chunk voisin, où la plante du voisin
  est ou n'est pas encore là selon l'ordre dans lequel les chunks sont décorés — le nôtre (le
  pipeline) n'est pas celui d'un serveur qui charge au gré des tickets.
- **Une passerelle de trop** : sur les 120 chunks, un `end_gateway` (et ses 10 bedrock visibles)
  chez nous là où le jeu a de l'air. Non localisé.

---

## 2. Le troisième niveau du serveur

L'End réutilise le stockage du Nether : `NetherWorld::open(..., DimensionId::End)` ouvre les
réglages `end` et `DIM1/region`. `server.cpp` tient `end_world` et `end_level` à côté de
`nether` / `nether_level`, dans des blocs `// ── end ──` ; les helpers `_in(dimension, …)` du
Nether prennent l'End par un paramètre (`other_world(dimension)`) plutôt qu'en double.

| | End |
|---|---|
| génération | `GeneratedWorld("end")` : pas de carver, chunk de 256, **2 workers** (chaque pile est construite sur le thread de tick ; cinq l'ont tenu plusieurs secondes en Debug) |
| disque | `DIM1/region`, comme vanilla |
| niveau | `end_level` (`ultrawarm` faux, `natural` faux), son drain de fluides et de redstone |
| clic droit | `end_player_level`, le même chemin que le Nether |
| fichier joueur | `minecraft:the_end` est lu ; un joueur sauvegardé dans l'End y revient (ou est refusé avec la raison si `OV_END=0`) |
| mort | réapparition au point d'apparition, dans l'overworld (le chemin du Nether) |

---

## 3. Le portail de l'End

`ov_gameplay/end_portal.{hpp,cpp}`, depuis la page « End portal » du wiki (Java 1.20) :

- douze cadres autour d'un carré 3 × 3, coins vides, chacun **tourné vers l'intérieur** (le côté
  nord regarde au sud…), chacun avec son œil ; ce que contient le carré ne compte pas ;
- un œil utilisé sur un cadre vide y entre (`eye=true`), consommé hors créatif, événement 1503 ;
  s'il complète l'anneau, le 3 × 3 devient `end_portal` à la hauteur des cadres, événement 1038
  (global) ;
- une entité dont la boîte touche la tranche 6/16 → 12/16 d'un bloc de portail part aussitôt :
  pas d'attente, pas de recharge ;
- elle arrive en **(100,5 ; 49 ; 0,5)**, lacet 90, sur une plateforme 5 × 5 d'obsidienne à
  **y = 48** dont les trois couches au-dessus (49 à 51) sont vidées — reconstruite à chaque
  arrivée ; les deux mesurés (§ 4.1) ;
- le portail de sortie (après le dragon) envoie le Game Event 4 (« win game », 1 la première
  fois : le générique) ; le Client Command 0 qui suit ramène le joueur au point d'apparition en
  gardant tout.

L'œil lancé en l'air ne part pas : il volerait vers le fort le plus proche, et ce serveur ne
place aucun fort (mandat des structures). Vanilla ne le lance pas non plus quand il ne trouve pas
de fort ; la différence est qu'un monde vanilla en a toujours.

### 3.1 De bout en bout, contre notre serveur

`scripts/measure_end_portal.py --ours` : `ov_dedicated` (overworld superflat, `--survival` — un
overworld généré en Debug a mis plus de cinq minutes à préparer son point d'apparition sur une
machine à charge 35 ; l'End, lui, est généré depuis la graine quel que soit l'overworld), un client
sonde qui fait tout par le protocole : les douze cadres posés par la console, douze Use Item On
avec un œil, l'entrée, la mort, le retour, une seconde entrée.

| étape | mesuré sur notre serveur |
|---|---|
| chaque œil | le cadre passe à `eye=true`, sa `facing` gardée : états **7408, 7407, 7410, 7409** ×3 — **12 / 12 ceux du jeu** ; un événement 1503 chacun, comme lui |
| le portail | **au douzième œil, pas avant** : 9 blocs `end_portal` (état 7406), un événement 1038 global — le jeu : idem |
| la traversée | Respawn `minecraft:the_end` / `minecraft:the_end` ; première : **42 à 128 s** selon les passages (la pile de l'End construite, puis les deux blocs de génération de la plateforme par les workers, Debug sur machine chargée) ; seconde : **0,1 s** |
| l'arrivée | **(100,5 ; 49,0 ; 0,5), lacet 90, tangage 0**, les deux fois — **celle du jeu** (§ 4.1) ; (100,5 ; 50,0 ; 0,5) avant la correction |
| la plateforme (relue dans `DIM1/region`) | **25 obsidiennes à y = 48** — celles du jeu ; 25 à y = 49 avant la correction |
| la mort dans l'End | Combat Death, Client Command 0, Respawn `minecraft:overworld` au point d'apparition ; le lieu de la mort nomme **`minecraft:the_end`** et sa position, **octet pour octet ceux du jeu** — il nommait l'overworld quel que soit le niveau (corrigé : `SurvivalSession::death_dimension`) |

Il a fallu trois corrections dans le serveur pour que ce passage aille au bout — les pièges 1, 2
et 5 du § 6 — et trois dans la sonde (pièges 6 et 7, et l'attente de chaque œil).

---

## 4. Le combat

`ov_server/src/end_fight.{hpp,cpp}`. Ce qui est fait, et ce qui ne l'est pas, est écrit en tête
du fichier ; en bref :

- **fait** : dès qu'un joueur est dans l'End **et** que l'arène est chargée (les quatre chunks
  autour de l'origine, amenés par un ticket `Transient` — le jeu attend lui aussi son arène), le
  portail de sortie (`end_podium`, inactif) au sommet de la colonne (0, 0), le dragon en
  (0, 128, 0) avec 200 PV, un cristal sur chaque pilier. La traversée, elle, n'attend que les deux
  chunks de la plateforme (un ticket de rayon 1 : deux blocs de génération 4 × 4) ; la barre de
  boss (rose, barre pleine, musique et brouillard) pour les joueurs à moins de 192 blocs de
  (0, 128, 0), mise à jour à chaque changement de vie ; le cristal le plus proche à 32 blocs de
  la boîte du dragon le soigne d'un point tous les dix ticks ; un coup détruit un cristal ; le
  dragon est touché par ses huit parties (ids réseau qui suivent le sien) — la tête en entier, le
  reste à dégâts / 4 + min(dégâts, 1) ; à zéro PV il meurt en 200 ticks, puis la passerelle
  suivante (`end_gateway_order` : vingt positions sur un cercle de rayon 96 à y = 75, mélangées
  par `new Random(seed)`), le portail ouvert, l'œuf au premier kill ;
- **pas le jeu** : le vol (un cercle fixe de rayon 60 à y = 90, aucune des phases de frappe, de
  perchoir ou de souffle), l'explosion du cristal, la phase `dying` qui ramène le dragon au
  portail, le délai d'invulnérabilité, l'expérience (12 000 puis 500 : les orbes de ce serveur
  vivent dans la liste de l'overworld), la sauvegarde du combat (`DragonFight` de level.dat reste
  celui de la création du monde), la réinvocation par quatre cristaux.

### 4.1 Ce que le vrai serveur a laissé

Premier passage de `scripts/measure_end_portal.py` sur le vrai serveur (le joueur entre dans l'End ;
le combat démarre), relu dans `DIM1/region` et `level.dat` :

| | le jeu | nous |
|---|---|---|
| plateforme d'arrivée | **25 obsidiennes à y = 48** | 25 à y = 49 au premier passage — **corrigé** : le point d'apparition est (100, 49, 0), l'obsidienne un sous lui |
| portail de sortie : `ExitPortalLocation` | **(0, 63, 0)** | le premier bloc libre de la colonne (0, 0) est 64 chez nous comme chez lui (§ 1.4) : (0, 63, 0) |
| portail de sortie, inactif | bedrock : 21 à y = 62, 17 à y = 63 (l'anneau et le pilier), pilier jusqu'à y = 66, 4 torches murales à y = 65 tournées vers l'extérieur | la même forme par construction (`build_exit_portal`, distances au coin, y compris) — comparée cellule par cellule dans `test_end_portal` |
| ordre des passerelles (`Gateways`) | [10, 0, 18, 13, 8, 9, 5, 4, 12, 3, 1, 7, 2, 14, 16, 15, 11, 19, 6, 17] | **identique, 20 / 20** (`end_gateway_order`, mélange de `new Random(seed)`, prises depuis la fin : la 17 d'abord) |
| `DragonFight` | `NeedsStateScanning` 0, `DragonKilled` 0, `PreviouslyKilled` 0, `Dragon` (UUID), `ExitPortalLocation`, `Gateways` | **non écrit** : notre level.dat garde celui de la création du monde (nommé en tête d'`end_fight.hpp`) |
| barre de boss (Boss Bar, 0x0B) | ajout : `{"translate":"entity.minecraft.ender_dragon"}`, santé 1,0, couleur 0 (rose), division 0, drapeaux **6** (musique, brouillard) ; retrait à la mort | **les mêmes octets** après l'UUID (`EndFight::boss_bar_add`) |

Second passage, le dragon tué depuis la console (`kill @e[type=minecraft:ender_dragon]`), les
blocs changés relus dans les paquets reçus :

| | le jeu | nous (`finish_death`, tenu par `test_end_fight`) |
|---|---|---|
| portail de sortie ouvert | **20 `end_portal` à y = 63**, le disque d² < 6,25 autour de (0, 63, 0) moins le pilier | la même forme (`build_exit_portal(…, true)`) |
| œuf | **(0, 67, 0)** : l'anneau + 4, sur le pilier | l'anneau + 4 |
| première passerelle | **(56, 75, −78)** — la 17, prise à la fin de la liste — et ses **12 bedrock** | `end_gateway_order(seed)[0]` = (56, 75, −78), 12 bedrock |
| `DragonFight` après | `DragonKilled` 1, `PreviouslyKilled` 1, `Gateways` sans la 17 | non écrit |
| expérience | **aucune orbe** : `/kill` retire le dragon sans son agonie de 200 ticks, donc sans les 12 000 points | non versée (nommé) |

Un troisième passage du jeu, la sonde corrigée, a relu les vingt blocs du portail ouvert avec leurs
coordonnées : (−2, 63, −1..1), (−1, 63, −2..2), (0, 63, ±1 et ±2), (1, 63, −2..2), (2, 63, −1..1)
— **20 / 20 cellules de notre `build_exit_portal(…, true)`**. Il n'a pas mesuré la **sortie** : la
sonde a sauté d'un coup d'une centaine de blocs, de la plateforme au portail, et ce pas n'a produit
aucun Game Event 4 (vraisemblablement refusé comme mouvement trop rapide — non vérifié). Le
générique, le Client Command qui suit et le retour « en gardant tout » (`data_kept` 3 chez nous)
sont codés et **non mesurés** ; il faudrait téléporter la sonde près du portail de sortie avant le
pas.

Le premier passage du jeu a aussi pris la sonde **avant** qu'elle ne se place dans le portail
(0,05 s après le début du pas, quand elle se tenait encore au-dessus, en y = 101) ; notre règle de
la tranche 6/16 → 12/16 ne la prend pas là. L'arrivée, elle, est la même au dixième de bloc près :
**(100,5 ; 49,0 ; 0,5)**, lacet 90, tangage 0, aux deux passages — et la nôtre était en 50,0 :
corrigé (§ 3).

---

## 5. Ce qui n'est pas fait — nommé

| sujet | état |
|---|---|
| Œil d'Ender lancé | ne part pas : pas de fort dans ce serveur (mandat des structures) ; vanilla ne le lance pas non plus sans fort trouvé |
| Combat : vol du dragon | un cercle de rayon 60 à y = 90 ; ni frappe, ni perchoir, ni souffle, ni phase `dying` |
| Combat : explosion du cristal, délai d'invulnérabilité | non |
| Combat : expérience (12 000 puis 500) | non versée : les orbes de ce serveur vivent dans la liste de l'overworld |
| Combat : sauvegarde (`DragonFight`) | non écrite ; un redémarrage recommence le combat |
| Combat : réinvocation par quatre cristaux | non |
| Combat de bout en bout contre notre serveur | le dragon n'y a pas été tué : le démarrage (arène chargée), la barre, les parties, la mort, le portail ouvert, l'œuf et la passerelle sont tenus par `test_end_fight` ; la sonde ne frappe pas le dragon |
| Mobs dans l'End (endermen), cités de l'End, élytres | non : `EntityWorld` est celui de l'overworld ; les structures sont un autre mandat |
| Portail de sortie vers l'overworld par le générique | codé (Game Event 4, Client Command 0, retour en gardant tout) ; **non mesuré**, ni chez le jeu (la sonde n'a pas atteint le portail, § 4.1) ni chez nous (le dragon n'y est pas tué) |
| Chorus à 92 %, une passerelle de trop | § 1.5 |
| Première traversée d'une vie de serveur | construit la pile de l'End sur le thread de tick (quelques secondes en Debug) |

---

## 6. Pièges payés ici

1. **Un gestionnaire lent ferme la connexion sans un mot.** Le minuteur d'inactivité de
   `AsioConnection` (30 s) est réarmé à chaque *lecture*, pas à chaque paquet traité. Un lot de
   paquets lu d'un coup — douze Use Item On envoyés en rafale — dont le traitement dépasse trente
   secondes (un Debug qui ré-éclaire 3 × 3 chunks après chaque écriture, sur une machine à charge
   35) déclenche le minuteur juste après : socket fermée, aucune ligne de journal. La sonde attend
   maintenant le Block Update de chaque cadre avant le suivant.
2. **Le serveur renvoyait un keep-alive alors qu'un autre attendait sa réponse.** Avec des paquets
   traités plus de dix secondes en retard, la réponse au premier arrivait après que le second eut
   remplacé son id, et la règle « mauvais id » coupait le joueur, en silence. Vanilla n'envoie
   jamais un second défi tant que le premier est en attente ; le nôtre non plus désormais.
3. **`sin(−π)` n'est pas 0.** Un pilier annoncé « en (−42, 0) » par toutes les descriptions est en
   (−42, −1) : `floor(42 · −1,2·10⁻¹⁶)`.
4. **`end_islands` se lit à x / 8 tronqué, pas décalé** : les blocs −7..7 lisent la même colonne,
   et un `>> 3` déplacerait toute l'île d'un pas vers le négatif.
5. **`/kill` sur un joueur ne tuait personne.** La commande blesse par la session de survie hors du
   tick ; le tick suivant sortait tôt sur `health.dead` et n'atteignait jamais la mort : pas de
   Combat Death, pas d'`awaiting_respawn`, et le Client Command qui suivait ne ressuscitait rien —
   le joueur restait « dans l'End » pour la sonde et « dans l'overworld » pour le serveur. Vrai
   aussi dans l'overworld ; trouvé ici. Corrigé dans `survival_session.cpp`, tenu par un test.
6. **Une synchronisation de position tardive défait le pas dans le portail.** Sur un serveur lent,
   le Synchronize Player Position d'un `/tp` antérieur arrive après que la sonde s'est placée dans
   le portail et la remet dessus, pour de bon. La sonde tient désormais sa position tant qu'elle
   n'a pas traversé.
7. **Le jeu envoie les neuf blocs du portail en un Update Section Blocks**, pas en neuf Block
   Update : une sonde qui ne lit que ces derniers ne voit jamais le portail s'ouvrir.
8. **Le point d'apparition de l'End est (100, 49, 0), pas (100, 50, 0).** Le jeu y pose le joueur,
   en (100,5 ; 49,0 ; 0,5), sur 25 obsidiennes à y = 48 ; la valeur répandue le mettait un bloc
   plus haut, et notre première correction — n'ayant que la plateforme — avait déplacé
   l'obsidienne au lieu du joueur. Il a fallu l'arrivée mesurée pour trancher.
