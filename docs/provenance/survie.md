# Survie — vie, dégâts, faim, mort, expérience

Ce qui suit est établi par **mesure contre le vrai serveur 1.20.1**
(`tools/vanilla/server.jar`, SHA-1 `84194a2f286ef7c14ed7ce0090dba59902951553`),
par `scripts/measure_survival.py`. Aucun nombre de ce document n'a été recopié
d'un résumé : chacun a une campagne, un protocole de mesure et un chiffre.

Les résultats bruts vont dans `data/vanilla/1.20.1/normalized/survival.json`
(gitignoré, régénérable). Les tables retenues vivent dans
`src/ov_gameplay/src/{damage,food,experience}.cpp` et sont rejouées par les
tests marqués `[parity]`.

---

## 0. Comment le serveur a été interrogé

Trois instruments, et le choix de l'instrument est la moitié du travail.

**La console.** `data get entity <cible> <chemin>` lit le NBT d'une entité et
c'est de la donnée documentée, pas du code. Toutes les lectures passent par là.

**Un datapack écrit par le script.** La console n'est pas exacte au tick : deux
commandes envoyées ensemble peuvent tomber dans le même tick ou dans deux. Pour
la fenêtre d'invulnérabilité, cette imprécision *est* la mesure. Le script écrit
donc un datapack `ovprobe` dans le monde avant son premier chargement : une
fonction exécute ses commandes dans un seul tick, et `schedule function … <n>t`
place la seconde frappe exactement `n` ticks plus tard.

**Un client sonde.** `scripts/capture_entity_packets.py` fournit déjà `Probe` ;
`Bot` l'étend avec le déplacement, le sprint, la minerie, l'usage d'objet et le
respawn. Tout ce qui concerne un *joueur* (courbe d'XP, valeurs des aliments,
famine, régénération) passe par lui, parce qu'aucune de ces choses n'existe sur
un mob.

### Le piège qui a coûté le plus de temps

**`/data merge entity` est refusé sur un joueur.** Le serveur vanilla répond
« impossible de modifier les données d'un joueur ». La première version du
script mettait en place chaque mesure avec
`data merge entity <bot> {foodLevel:10,Health:20.0f}` et lisait ensuite des
compteurs que rien n'avait bougés. La lecture, elle, est autorisée — c'est
l'asymétrie qui rend l'erreur silencieuse.

L'état d'un joueur se pose donc par **effets** :

| Ce qu'on veut | Comment |
|---|---|
| faim et saturation au plafond | `effect give <bot> minecraft:saturation 1 9 true`, **répété** : une application nourrit dix points, pas vingt |
| faim à un niveau choisi | `effect give <bot> minecraft:hunger 2 255 true`, qui facture 1,28 d'épuisement par tick ; la saturation part en premier, ce qui donne gratuitement la ligne de base « saturation nulle » dont toute mesure d'aliment a besoin |
| vie au plafond | `effect give <bot> minecraft:instant_health 1 5 true` — 128 points de soin |
| vie à une valeur choisie | plafond, puis un `damage` de la différence en `minecraft:generic`, dont l'épuisement propre vaut 0.0 et ne peut donc pas polluer une mesure de faim |

Deuxième piège de la même famille : **l'effet `hunger` vide la saturation avant
la faim**. Avec un amplificateur faible, vider une réserve pleine de vingt
points demande quatre-vingts d'épuisement, soit cinquante secondes — plus que le
délai d'attente. La campagne `regen` a d'abord rapporté « aucune régénération
nulle part » parce qu'elle mesurait un joueur qu'elle n'avait pas réussi à
amener au niveau demandé.

Troisième : **la sonde doit répondre aux keep-alive pendant les campagnes qui ne
l'utilisent pas**. La campagne de chute passe deux minutes à lâcher des vaches
sans toucher au socket ; le serveur expulse au bout de trente secondes, et la
campagne *suivante* meurt sur un EOF sans explication. Tous les `time.sleep` des
campagnes sont devenus des `nap`, qui pompent la sonde s'il y en a une.

---

## 1. Identifiants de paquets — dérivés, pas lus

Aucun identifiant clientbound n'a été écrit de mémoire. Le serveur est poussé à
changer **une chose à la fois** et le paquet qui porte ce changement est
identifié par une charge utile que rien d'autre ne pourrait produire.

| Paquet | Id | Comment il a été identifié |
|---|---|---|
| Set Health | `0x57` | flottant valant exactement 13.5 après un `damage 6.5` depuis le plafond |
| Set Experience | `0x56` | varint de niveau valant 7 avec une barre dans `]0,1[` |
| Damage Event | `0x18` | **différentiel** : deux frappes de types différents, le seul paquet qui change |
| Combat Death | `0x38` | seul paquet contenant `"translate"` et l'id du joueur |
| Respawn | `0x41` | seul paquet commençant par deux chaînes de dimension identiques |
| Spawn Experience Orb | `0x02` | varint + trois doubles + un short, à l'endroit de la mort |
| Client Command (serveur→) | `0x07` | vérifié par son effet : le respawn a lieu |

Deux résultats contredisent ce qu'une lecture de spécification aurait donné.

### Combat Death ne porte pas d'id de tueur

La capture fait **315 octets** : un varint d'id de joueur (1 octet), un varint
de longueur (2 octets) et 312 octets de composant de chat. Interprétée avec un
`Int` d'id de tueur entre les deux, la chaîne annoncée fait 116 octets et le
paquet en déborde de 193. Il n'y a pas de place pour ce champ.

### Il n'y a pas de Hurt Animation

Deux fenêtres capturées autour de deux frappes de types différents contenaient,
chacune : Damage Event (`0x18`), Set Entity Metadata (santé), Set Entity
Velocity (le recul) et Set Health. Aucune Hurt Animation. Le client 1.20.1
déduit le tressaillement, le flash rouge et la direction du recul de Damage
Event seul. L'encodeur de Hurt Animation existe dans `entity.hpp` parce que le
paquet existe dans le protocole ; le système de survie ne doit pas l'envoyer.

### Les ids de type de dégât sont les nôtres, et ils sont alphabétiques

`minecraft:damage_type` est l'un des six registres que le **serveur envoie** au
client dans le codec de login. Les ids nous appartiennent — mais le client s'en
sert pour choisir un message de mort, donc ils doivent correspondre au codec
octet pour octet. Mesuré sur le fil : `minecraft:cactus` arrive en 2,
`minecraft:lightning_bolt` en 23, `minecraft:generic_kill` en 17 — soit les
positions alphabétiques des noms parmi les 44 types du datapack, ce qui est
l'ordre de chargement d'un registre de datapack.

`test_damage.cpp` ouvre `registry_codec.nbt` et vérifie les **44 entrées** :
44/44 correspondent à l'ordre de l'énumération `DamageKind`.

---

## 2. Dégâts de chute — 30/30

**Protocole.** Une vache dont la barre de vie est portée à 200 points
(`attribute … generic.max_health base set 200` puis `data merge … {Health:200}`,
autorisé sur un mob) est invoquée à `y = sol + h` pour `h = 1..30`. Le sol est
mesuré d'abord, en lâchant une vache **invulnérable** de haut et en lisant où
elle s'arrête : `y = -60`.

> Piège : la première version lâchait une vache ordinaire pour trouver le sol.
> Quatre-vingts blocs valent soixante-dix-sept points de dégâts et une vache en
> a dix ; la boucle attendait une entité qui était de la viande depuis avant la
> première lecture, et le script s'arrêtait sur « aucune vache ne s'est posée ».

**Résultat.** La table mesurée n'est pas `h - 3`. Elle décroche à
`h = 12, 17, 20, 23, 25, 28, 30`, où elle vaut un point de moins.

| h | 1..3 | 4 | … | 11 | **12** | 13 | … | **17** | 18 | … | **20** | 21 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| dégâts | 0 | 1 | | 8 | **8** | 10 | | **13** | 15 | | **16** | 18 |

**Explication, et c'est le résultat.** La distance de chute n'est pas la hauteur
du saut : c'est ce qui a été *accumulé tick par tick*, et **le tick qui touche le
sol n'ajoute pas sa propre descente**. La vérification est un `if/else` : soit
l'entité est au sol et la chute est soldée, soit elle ne l'est pas et le pas du
tick compte. À vitesse élevée le pas jeté fait plus d'un bloc, d'où les
décrochages, de plus en plus fréquents avec la hauteur.

En rejouant la chute avec les constantes de `physics.hpp` (`g = 0.08`,
`d = 0.98`) et en appliquant `ceil(distance - 3)` :

| modèle | reproduit |
|---|---|
| `ceil(distanceAccumulée - 3)`, tick d'atterrissage exclu | **30 / 30** |
| `floor(distanceAccumulée - 3)`, tick d'atterrissage exclu | 3 / 30 |
| `ceil(hauteur - 3)`, la table naïve | 23 / 30 |

Deux conséquences pour le code :

1. **C'est un plafond, pas un plancher.** Le mandat de cette tâche annonçait
   `floor(distance - 3)` ; la mesure dit `ceil`. Le cas discriminant est
   `h = 12` : distance 10,8065, `floor` donne 7, `ceil` donne 8, le serveur dit
   8.
2. **L'ordre dans `accumulate_fall` est porteur.** Ajouter le pas puis tester le
   sol donne 23/30. Tester le sol d'abord donne 30/30.

---

## 3. Fenêtre d'invulnérabilité — 15 écarts, 15 reproduits

**Protocole.** Datapack. `ovprobe:gapN` frappe pour 4 points, puis
`schedule function ovprobe:hit_b Nt`. Pour `N = 0..14`, sur une vache à 200
points de vie.

| écart (ticks) | 0 | … | 10 | **11** | 12 | 13 | 14 |
|---|---|---|---|---|---|---|---|
| vie perdue | 4 | 4 | 4 | **8** | 8 | 8 | 8 |

La seconde frappe est avalée jusqu'à dix ticks inclus et passe à onze. Le modèle
retenu — `invulnerable_ticks` mis à 20 par une frappe, décrémenté d'un par tick,
régime « partiel » tant qu'il vaut au moins 10 — reproduit les quinze écarts.
C'est le seul couple (20, 10) qui place la frontière à cet endroit.

**« Un coup plus fort passe quand même. »** Deux fonctions, même tick :

| séquence | vie perdue |
|---|---|
| 4 puis 9 | **9** |
| 9 puis 4 | **9** |

Donc : à l'intérieur de la fenêtre, une frappe plus grande applique la
*différence* et devient la nouvelle référence ; une frappe plus petite ne fait
rien. Ce n'est pas « la plus grande gagne » — c'est un cumul plafonné par la
plus grande, et les deux se ressemblent jusqu'à la troisième frappe.

**Ce qui ignore la fenêtre.** Le tag `#bypasses_invulnerability` du datapack
contient exactement deux types : `minecraft:out_of_world` et
`minecraft:generic_kill`. Vérifié dans le code par un test qui les compte.

**Ordre d'appel.** `tick_health` doit tourner **avant** tout dégât du même tick.
L'écart entre deux frappes se compte dans ces appels ; appliquer les dégâts
d'abord vieillit chaque frappe d'un tick et déplace la frontière mesurée.

---

## 4. Courbe d'expérience — 41/41

**Protocole.** Pour chaque niveau `L` de 0 à 40 : `experience set <bot> 0
points`, `experience set <bot> L levels`, `experience add <bot> 1 points`, puis
`data get entity <bot> XpP`. La barre vaut alors exactement `1/coût`, et `XpP`
est un flottant qui garde largement assez de précision (1/249 ≈ 0,004016).

Les 41 coûts mesurés tombent tous sur trois droites :

| régime | coût de `L` à `L+1` | vérifié |
|---|---|---|
| `0 ≤ L < 16` | `2L + 7` | 16 / 16 |
| `16 ≤ L < 31` | `5L − 38` | 15 / 15 |
| `31 ≤ L` | `9L − 158` | 10 / 10 |

Les jointures sont nettes : 15→16 coûte 37 (`2L+7`), 16→17 coûte 42 (`5L−38`),
30→31 coûte 112, 31→32 coûte 121.

**Contre-épreuve.** Le total cumulé prédit pour chaque niveau est réinjecté en
points bruts et le niveau obtenu est relu : **39 des 41** reviennent exacts. Les
deux qui ne le sont pas sont les niveaux 12 et 13, où le serveur répond un
niveau en dessous avec une barre à 0,99999994.

C'est une **dérive de flottant du serveur vanilla**, pas une erreur de courbe :
vanilla accumule la barre en `float`, une division par point accordé, et mille
divisions ne se rassemblent pas en un. Notre implémentation garde le reste en
entier et ne calcule le flottant que pour le paquet. C'est une divergence
délibérée, nommée ici : le déterminisme (CLAUDE.md principe 5) vaut mieux que
la reproduction d'une erreur d'arrondi, et le nombre que le client dessine est
le même dans les deux cas.

---

## 5. Expérience lâchée à la mort — 12/12

**Protocole.** Le bot est mis à un niveau `L`, tué par
`damage <bot> 100 minecraft:generic_kill`, et la `Value` de chaque orbe présente
est sommée.

| niveau | 0 | 1 | 2 | 5 | 10 | 13 | 14 | **15** | 20 | 30 | 60 | 100 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| XP lâchée | 0 | 7 | 14 | 35 | 70 | 91 | 98 | **100** | 100 | 100 | 100 | 100 |

`min(7 × niveau, 100)`, sans exception. Le plafond mord à partir du niveau 15.

**Découpage en orbes.** Le nombre d'orbes observé (4 pour 100, 3 pour 91, 4 pour
98) correspond à un découpage glouton sur les dénominations
`2477, 1237, 617, 307, 149, 73, 37, 17, 7, 3, 1`. Une mort à 48 points a produit
exactement quatre orbes de 37, 7, 3 et 1.

---

## 6. Famine — 4/4 difficultés

**Protocole.** Le bot est amené à 12 points de vie et à zéro de faim, puis
laissé tranquille, sur chaque difficulté.

| difficulté | vie finale | est mort |
|---|---|---|
| peaceful | 20 (la faim est **remontée** à 20) | non |
| easy | 10 | non |
| normal | 1 | non |
| hard | 0 | **oui** |

La faim seule ne tue qu'en difficile. En paisible elle ne descend pas du tout.

Cadence mesurée : un point toutes les quatre secondes (80 ticks).

---

## 7. Oxygène et noyade

**Protocole.** Une vache à 200 points de vie, sans IA et sans gravité, placée
dans un `fill` d'eau ; `Air` et `Health` échantillonnés.

* `Air` part de **300** et descend d'un par tick (287 → 277 en une demi-seconde,
  soit dix ticks) ;
* à zéro il passe en négatif, et **chaque fois qu'il atteint −20 il repart de 0
  et deux points de vie partent** ;
* soit un cœur par seconde, une fois les quinze secondes d'apnée écoulées.

Trace : `200 → 198 → 196 → 194 …`, à une seconde d'intervalle exactement.

---

## 8. Épuisement, régénération, aliments

Les coûts d'épuisement des **types de dégâts** ne sont pas mesurés à la main :
ils sont déclarés par le datapack (`exhaustion` dans chaque
`data/minecraft/damage_type/*.json`), avec `message_id`, `scaling` et
`death_message_type`. Les 44 entrées et les 23 tags sont repris tels quels dans
`damage.cpp` et le test compare la table au codec.

Le reste de cette section est le point faible de ce travail et est décrit tel
quel dans le rapport de session : la campagne `food` (valeurs de faim et de
saturation de tous les aliments) et la campagne `exhaustion` (coût du sprint, de
la casse de bloc, du saut) sont écrites et exécutables
(`python3 scripts/measure_survival.py --only food,exhaustion`) mais leurs
résultats ne sont pas encore intégrés au moment de ce commit. Les valeurs
présentes dans `food.cpp` pour les constantes d'épuisement sont donc **non
mesurées** et le fichier le dit.

Ce qui *est* mesuré et intégré :

* **régénération à partir de 18 de faim**, et régénération rapide à 20 avec de
  la saturation — un joueur à 10 points de vie, faim 20, saturation ~19, est
  remonté à 20 en moins de la fenêtre d'échantillonnage, ce qui exclut la
  branche lente (un point toutes les 80 ticks aurait pris 40 s) ;
* **pas de régénération en dessous de 18** ;
* le seuil d'épuisement à **4,0**, dépensé d'abord en saturation puis en faim.

---

## 9. Ce qui n'a pas pu être mesuré

Nommé plutôt que passé sous silence, comme le veut la règle du dépôt.

* **XP du minage.** La campagne est écrite et tourne, mais le bot n'arrive pas à
  casser un bloc contre le serveur vanilla : les quinze minerais testés
  rapportent 0, y compris le diamant, ce qui est impossible. Le paquet Player
  Action est envoyé avec le bon id (0x1D, capturé par un travail antérieur du
  dépôt) et le bon empaquetage de position ; la cause n'est pas identifiée. La
  campagne vérifie maintenant si le bloc a réellement disparu, ce qui
  distinguera « rien cassé » de « cassé sans orbe ».
* **XP de la fonte.** Pas besoin de mesure : le champ `experience` est dans les
  recettes du data generator (`minecraft:smelting`, p. ex. 0,7 pour le lingot de
  fer). Non intégré faute de recettes dans ce module.
* **Épuisement du saut.** Un client ne « saute » pas du point de vue du serveur ;
  il envoie des positions. La campagne envoie une parabole en paquets de
  position et lit le compteur ; le résultat n'a pas encore été relevé.
* **Point de réapparition sur un lit ou une ancre.** Le serveur remet le joueur
  au spawn du monde. `SurvivalSession::SpawnPoint` porte déjà le drapeau
  `is_bed` mais rien ne le pose : il n'y a pas encore de bloc de lit utilisable.
* **Armure, résistance, enchantements de protection.** Les tags
  `#bypasses_armor` et `#bypasses_resistance` sont dans la table et lisibles ;
  rien ne les consulte encore, parce qu'il n'y a pas d'armure.
