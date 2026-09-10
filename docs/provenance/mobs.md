# Peupler le monde — apparition, pathfinding, comportements

Ce qui suit est établi par **mesure contre le vrai serveur 1.20.1**
(`tools/vanilla/server.jar`, SHA-1 `84194a2f286ef7c14ed7ce0090dba59902951553`),
par `scripts/measure_mobs.py`. Cinq campagnes : `maze`, `speed`, `acquire`,
`caps`, `light`.

Les relevés bruts vont dans `data/vanilla/1.20.1/normalized/mob_spawning.json`
(gitignoré, régénérable). Le code vit dans
`src/ov_gameplay/{src,include/ov/gameplay}/{pathfinding,goals,spawning,mob_logic}`
et les tests marqués `[parity]` rejouent les mesures.

**Ce document dit aussi ce qui n'a pas été mesuré.** Une table qui a l'air
uniforme est exactement la façon dont un nombre dérivé se fait passer pour un
nombre mesuré ; les sections 6 et 7 listent ce qui est de nous.

---

## 0. Le piège qui a coûté quatre montages

**`Invulnerable:1b` rend une entité impossible à prendre pour cible.** Vanilla
refuse purement et simplement une entité invulnérable comme cible.

C'est la cause racine de toute l'histoire de la campagne `maze`, et elle a été
diagnostiquée en dernier. Le premier montage plaçait un golem de fer
`{NoAI:1b,NoGravity:1b,Invulnerable:1b}` au bout du labyrinthe — l'invulnérabilité
paraissant évidemment souhaitable pour que l'expérience ne se termine pas par la
mort de la cible. Le zombie l'a ignoré, y compris à trois blocs de distance.

Chaque contournement construit ensuite visait le mauvais symptôme :

1. **Frapper le zombie pour lui imposer une cible.** `/damage <zombie> 1
   minecraft:mob_attack by <golem>` fonctionne : être blessé ne demande pas de
   ligne de vue. Résultat : le zombie avançait deux ticks puis s'arrêtait
   trente-huit.
2. **Frapper à chaque tick.** Une cible posée par `HurtByTargetGoal` est
   abandonnée dès le premier tick sans ligne de vue, et un mur de labyrinthe en
   est précisément la négation. Le zombie oscillait alors entre poursuite et
   errance.
3. **Retirer `PersistenceRequired`.** Un mob persistant voit son `noActionTime`
   remis à zéro à chaque contrôle de despawn, et le but d'errance aléatoire de
   vanilla ne se tait qu'une fois `noActionTime` passé 100 ; épingler le mob
   contre le despawn redonne donc au but d'errance une chance à chaque tick. Le
   retirer a bien éteint l'errance — et le mob s'est mis à ne plus bouger du
   tout, faute de cible.
4. **Prendre un villageois plutôt qu'un golem.** Un zombie acquiert un joueur ou
   un golem *avec* ligne de vue, et un villageois *sans*. Toujours rien : le
   villageois était lui aussi `Invulnerable`.

C'est seulement en enlevant `Invulnerable` que le montage a fonctionné du
premier coup, sans aucun des trois contournements précédents. La cible est
maintenue en vie par `max_health 1024` et Résistance V, ce qui la laisse
attaquable.

Deux autres pièges, plus petits, payés en route :

* **Un serveur vanilla sans joueur connecté ne fait apparaître strictement
  rien.** Le générateur naturel parcourt les chunks qu'un ticket de joueur
  atteint ; sans joueur il n'y en a aucun. Une campagne d'apparition sans client
  sonde lit zéro partout, ce qui ressemble exactement à un seuil de zéro. Toutes
  les campagnes d'apparition maintiennent donc un `Probe` connecté.

* **La géométrie de l'arène était fausse deux fois.** Le premier labyrinthe avait
  une enceinte deux blocs plus large que ses murs transversaux : le zombie l'a
  résolu en contournant le bout de chaque mur. Le second faisait apparaître le
  zombie *dans* le mur d'enceinte, et la résolution de collision l'a éjecté à 25
  blocs hors de l'arène.

---

## 1. Le labyrinthe — la mesure qui juge l'A\*

C'est la mesure centrale du mandat : un mob qui atteint le but par un autre
chemin n'est pas le même mob.

**Géométrie.** Un carré de 17 × 17 (`MAZE_HALF = 8`) entouré de pierre. Trois
murs transversaux en `z = -4, 0, +4`, chacun percé d'une seule ouverture, du
côté ouest puis est puis ouest : `x = -7, +7, -7`. Départ `(0.5, -7.5)`, arrivée
`(0.5, +7.5)`. Superflat, sol de pierre en `y = -61`, mobs en `y = -60`.

**Protocole.** Un zombie au départ, un villageois à l'arrivée
(`NoAI`, `NoGravity`, 1024 PV, Résistance V), `follow_range` porté à 128. Rien
d'autre n'est fourni de l'extérieur. `Pos` et `time query gametime` sont
échantillonnés ensemble, à environ un échantillon par tick.

**Relevé vanilla** (1009 échantillons, 984 ticks, arrivée atteinte) :

| mur | x du franchissement | ouverture |
|---|---|---|
| `z = -4` | −6.42 | −7 |
| `z = 0`  | +7.42 | +7 |
| `z = +4` | −6.42 | −7 |

Trois franchissements, un par mur, chacun par l'unique ouverture, aucun retour
en arrière. Le `.42` est la position d'un corps de 0,6 de large debout dans la
colonne de l'ouverture.

**Notre A\*** (`test_pathfinding.cpp`, `[parity]`) rejoue exactement la même
géométrie : franchissements à `x = -7, +7, -7`, dans cet ordre, une fois chacun.
Longueur de route 40 à 60 nœuds.

**La comparaison porte sur la suite des ouvertures, pas sur la suite des
positions**, et c'est délibéré : les deux ne sont pas comparables à cette
résolution. Un mob vanilla oscille à l'intérieur de son bloc et recalcule son
chemin plusieurs fois par seconde, là où un A\* émet des centres de blocs. Ce
qui n'oscille pas, c'est par où il est passé.

---

## 2. La vitesse d'un mob qui poursuit

**Protocole.** Sol dégagé de 45 × 17, zombie et villageois à 30 blocs l'un de
l'autre, trace échantillonnée avec le `gametime` du jeu comme dénominateur —
jamais l'horloge de la machine.

**Résultat : 0,11419 bloc par tick**, mesuré sur 271 échantillons. La valeur est
identique du 25ᵉ au 100ᵉ centile des pas non nuls : un zombie qui marche marche
à vitesse constante.

**L'attribut `movement_speed` mesuré d'un zombie est 0,23.** Le rapport vaut
0,4965. **L'attribut n'est donc pas une vitesse en blocs par tick**, et l'utiliser
tel quel donnerait des mobs deux fois trop rapides.

La relation retenue est `blocs/tick = movement_speed / 2`, qui prédit 0,115
contre 0,11419 mesuré — un écart de 0,7 %.

> **Remplacé** par `mobs-2.md` § 1 : la vraie loi est `v = 0,98·s²·(0,6/f)³ / (1 − 0,91·f)`,
> soit `2,15859·s²` sur l'herbe, vérifiée sur 19 espèces et trois sols. La moitié n'est juste que
> près de 0,23 ; une vache qui erre allait 16 % trop vite, un creeper 45 %.

**Un deuxième piège, du côté de notre code.** `step_entity` applique la traînée
horizontale *après* que le but a posé la vitesse. Un mob poussé à la vitesse
mesurée ne parcourait que 0,546 de celle-ci, soit 0,062 bloc par tick : 45 % trop
lent, avec la constante de la table restant parfaitement correcte à la lecture.
La vitesse est donc divisée par la friction au moment où elle est posée. Le test
`[parity]` de `test_mob_logic.cpp` vérifie le déplacement effectif, pas la
constante.

---

## 3. À quelle distance un mob commence-t-il à poursuivre

Campagne écrite parce que la campagne `speed` s'est heurtée à la question plutôt
que de la contourner : à soixante blocs, le zombie ne bougeait pas du tout.

**Protocole.** Une arène par écart, un zombie et un villageois séparés de cet
écart, soixante ticks. Le verdict est le déplacement vers la cible.

| écart (blocs) | 8 | 16 | 24 | 32 | 40 | 48 | 56 | 64 |
|---|---|---|---|---|---|---|---|---|
| `follow_range` par défaut (35) | +4,66 | +4,54 | +1,68 | +4,54 | 0,00 | 0,00 | −2,51 | 0,00 |
| `follow_range` porté à 128 | +4,54 | +4,77 | +4,66 | +4,66 | 0,00 | +3,81 | 0,00 | 0,00 |

**Avec l'attribut par défaut, l'acquisition est franche jusqu'à 32 blocs et
absente à partir de 40.** Le rayon est donc encadré par `]32, 40]`, ce qui est
cohérent avec la valeur mesurée de `follow_range` pour un zombie : **35**.

**La deuxième ligne n'est pas concluante et n'est pas présentée comme telle.**
Elle acquiert à 48 et échoue à 40 et 56. La fenêtre de soixante ticks est
probablement trop courte à ces distances — un zombie parcourt 6,8 blocs en
soixante ticks, et les déplacements observés valent 4,5 à 4,8, donc l'acquisition
consomme déjà plusieurs ticks. Conclure que le rayon vaut `follow_range` demande
une campagne plus longue qui n'a pas été faite.

`NearestAttackableTargetGoal` reçoit 35 dans `install_goals`, ce qui est la
valeur mesurée de l'attribut et est cohérent avec l'encadrement.

---

## 4. Les caps par catégorie

**Protocole.** Superflat ordinaire, un joueur connecté immobile au point
d'apparition, `simulation-distance = 10`, minuit, difficulté difficile,
`doMobSpawning` activé. Comptage au tableau de score — `execute store result
score <nom> <objectif> if entity <sélecteur>` est la seule façon d'extraire un
nombre d'un sélecteur depuis une console — toutes les dix secondes pendant
quatre minutes.

**Plateau mesuré**, 24 échantillons sur quatre minutes :

| catégorie | série | moyenne du dernier tiers | pic |
|---|---|---|---|
| monstre | 69 à 73, sans tendance | **70,88** | 73 |
| créature | 0 puis 12, plat | 12,00 | 12 |
| ambiant | 0 | 0 | 0 |
| créature aquatique | 0 | 0 | 0 |
| ambiant aquatique | 0 | 0 | 0 |

**Monstres : 70,88 mesuré contre 70 dans notre table**, et 70 est à l'intérieur
de la plage observée (69–73). La dispersion vient de ce que le cap est vérifié
une fois par passe et non par mob.

**Créatures : la mesure ne teste pas le cap.** La série passe de 0 à 12 entre le
premier et le deuxième échantillon puis ne bouge plus d'un seul mob pendant
quatre minutes. Ces douze animaux sont ceux posés à la **génération des chunks**,
et l'apparition naturelle de créatures n'en a ajouté aucun sur un superflat déjà
généré. Notre cap de 10 n'est donc **ni confirmé ni infirmé** par cette
campagne ; il est présenté comme non mesuré.

Les trois zéros sont attendus et ne sont pas non plus des mesures de cap : un
superflat d'herbe n'a ni grotte ni eau, donc ni chauve-souris ni poisson.

**Notre mise à l'échelle** (`effective_cap` dans `spawning.cpp`) :
`cap × chunks tickés / 289`, 289 étant les chunks d'un carré d'apparition de
17 × 17 — ce qui rend le cap complet pour la `simulation-distance = 10` du
montage, et c'est bien à cette distance que 70,88 a été relevé.

**La multiplication vient avant la division**, et c'est testé : diviser d'abord
arrondit à zéro le cap d'un petit serveur, qui n'a alors plus aucun mob — un bug
qui ressemble exactement à « l'apparition est cassée ».

---

## 5. Les seuils de lumière

**Protocole.** Un monde **vide** (`generator-settings` d'une seule couche d'air),
de sorte que les seules surfaces d'apparition à cent blocs à la ronde soient
celles que le montage construit. Une rangée de salles 7 × 7, chacune éclairée à
un niveau différent. Trois passages : scellé à minuit, ouvert au ciel à minuit,
ouvert au ciel à midi. Un client sonde parqué à 48 blocs de la rangée, hors du
rayon de refus de 24 blocs et dans le rayon d'offre de 128.

### Résultat : le seuil vaut **0**

Montage corrigé (voir plus bas), trois passages, comptes par niveau de lumière
**au sol** :

| lumière au sol | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| scellé, minuit | **26** | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 |
| toit ouvert, minuit | **35** | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| toit ouvert, midi | **27** | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

**Un monstre n'apparaît qu'à une lumière strictement nulle.** Le seuil est 0, et
non « inférieur ou égal à 7 » : à 1 il ne se passe déjà plus rien. Le seul 1 de
la ligne du haut est un mob qui a marché d'une salle voisine, pas une apparition.

C'est la valeur que porte `rules_for(MobCategory::Monster).max_spawn_light`.

### La lumière du ciel participe

Établi par le **premier** montage, et par une comparaison appariée qui reste
valable malgré ses défauts de calibration : à géométrie et éclairage rigoureusement
identiques, seule l'heure changeant, minuit donnait des apparitions dans les
salles sombres (10, 18, 27, …) et midi **zéro partout**. Le niveau de lumière de
chaque salle était alors mal maîtrisé, mais le contraste minuit/midi ne dépend
pas de cette calibration.

La formule de combinaison retenue —
`max(lumière de bloc, lumière du ciel − assombrissement)`, dans
`LightSource::effective_light` — est cohérente avec ce contraste et testée
unitairement, mais n'a pas été mesurée terme à terme.

### Ce que les trois passages corrigés mesurent réellement

**Ils mesurent trois salles scellées, pas une scellée et deux ouvertes.** La
couche de dalles qui règle le problème des toits (point 2 ci-dessous) scelle
aussi les salles : une dalle inférieure ne laisse pas descendre la lumière du
ciel. Les variantes « toit ouvert » ne sont donc pas ouvertes.

Cela ne compromet pas le seuil — les trois passages donnent la même réponse, ce
qui en fait un contrôle de reproductibilité — mais cela veut dire que **la
moitié « ciel » de la question n'a pas été remesurée par le montage corrigé**,
et repose entièrement sur la comparaison appariée du paragraphe précédent.

### Les trois défauts du premier montage

Le premier passage produisait des comptes non monotones — 10, 18, 27, 1, 0, 0,
0, 5, 11 en scellé à minuit — c'est-à-dire pas un seuil du tout. Trois défauts,
chacun rendant le résultat faux d'une manière différente et plausible :

1. **Une lumière d'angle n'éclaire pas un sol.** Le bloc `minecraft:light` était
   posé dans deux angles du plafond, à six ou sept blocs du milieu du sol ; la
   lumière de bloc décroît de 1 par bloc, donc une salle « au niveau 8 » avait un
   sol éclairé entre 2 et 0 selon l'endroit. La salle avait un dégradé, pas un
   niveau. Le montage corrigé remplit toute la couche située deux blocs au-dessus
   du sol, qui vaut donc uniformément `niveau − 2` — et les deux blocs de corps
   du mob restent dégagés.

2. **Les mobs apparaissaient sur les toits.** Dans un monde vide, les toits de
   pierre des salles étaient la seule autre surface, et un toit à ciel ouvert lit
   4 à minuit ; ils consommaient le cap que les salles se disputaient. Ils sont
   coiffés de dalles inférieures, dont la face supérieure n'est pas *sturdy* et
   sur lesquelles rien ne se pose.

3. **La salle *n* était comptée dans la salle *n + 1*.** Le sélecteur était une
   sphère de rayon 7 autour d'une salle large de 7 espacée de 10, donc chaque
   compte incluait les bords de ses deux voisines — ce qui est la raison pour
   laquelle les comptes *remontaient* du côté éclairé. C'est une boîte
   maintenant, exactement l'intérieur.

### Ce qui reste non mesuré dans cette campagne

Le seuil de 3 des **ambiants** dans `rules_for`. Aucune chauve-souris n'est
apparue : elles demandent une grotte, et le monde de la campagne est vide.

---

## 6. Ce qui est de nous, et n'est pas mesuré

Nommé ici parce qu'une table qui a l'air uniforme est exactement la façon dont un
nombre dérivé se fait passer pour un nombre mesuré.

* **Les malus de nœud** de `path_malus` — eau 8, bord d'eau 4, feu 16, danger 8,
  porte de bois fermée 4, miel 8, plaque 0,5. La *forme* est celle du jeu (un
  malus additionné à la longueur du pas, un malus négatif valant infranchissable)
  mais aucune de ces valeurs n'a été relevée. Ce qui est vérifié, c'est un
  ordonnancement, unitairement : un mob préfère un détour sec court à quatre
  blocs d'eau, et traverse l'eau quand il n'y a pas de détour.

* **Les vitesses de marche des sept autres espèces.** Seul le zombie a été
  chronométré. Les sept autres valent leur attribut `movement_speed` mesuré
  divisé par deux, par application de la relation de la section 2.

* **Le taux de despawn aléatoire** (`kRandomDespawnOdds = 800`) et le délai
  d'inactivité (`kIdleTicksBeforeDespawn = 600`). Ce qui est établi, c'est qu'un
  mob vanilla entre les deux distances finit par disparaître seul et qu'un mob
  persistant ne disparaît pas ; le taux, lui, est choisi.

* **L'impulsion de saut de 0,42**, qui est la valeur mesurée pour un joueur et
  n'a pas été mesurée pour un mob. Ce qu'elle doit faire est franchir un bloc,
  et elle le fait.

* **Le rayon de 35 blocs** donné à `NearestAttackableTargetGoal` : c'est la
  valeur mesurée de l'attribut `follow_range` d'un zombie, cohérente avec
  l'encadrement `]32, 40]` de la section 3, mais l'encadrement ne prouve pas que
  le rayon *soit* cet attribut.

* **Les priorités de buts** de `install_goals`. Elles reproduisent l'ordre
  observable du jeu (paniquer avant errer, poursuivre avant errer) mais ne sont
  la copie d'aucun relevé.

## 7. Ce qui n'est pas implémenté, et le dit

* **Le seuil de lumière des ambiants** (3 dans `rules_for`) — voir la fin de la
  section 5.

* **Les listes d'apparition par biome.** Vanilla les lit dans les `spawners` du
  datapack ; nous ne parsons pas encore ce champ. Plutôt que d'inventer une
  distribution plausible, `NaturalSpawner` reçoit ses entrées de l'appelant, et
  une catégorie sans entrées ne fait rien apparaître.

* **Le peuplement à la génération de chunk.** Voir l'écart de 2 sur les créatures
  en section 4.

* **`SwimNodeEvaluator` et `FlyNodeEvaluator` n'ont pas d'oracle.** Ils sont
  écrits et compilés, la campagne `maze` ne couvre que le terrestre. Aucun des
  huit mobs livrés ne les utilise.

* **Un type d'entité absent de la table des catégories est `Misc`**, et `Misc`
  n'apparaît jamais naturellement — refusé et nommé plutôt que doté d'une
  catégorie plausible.

---

## 8. Contrôle de bout en bout

`scripts/check_entities.py` fait tourner notre serveur et vérifie que huit mobs
apparaissent avec les bons identifiants de type, la bonne santé et la bonne
hauteur de repos. Un contrôle de plus a été fait à la main sur ce mandat, et il
a trouvé un bug que les tests unitaires ne pouvaient pas voir : sur 1600 ticks
avec huit mobs, **six sur huit marchaient et deux ne bougeaient pas d'un
millimètre**.

Le squelette et l'araignée étaient à deux blocs l'un de l'autre, et
`NearestAttackableTargetGoal` avait reçu -1 — « n'importe quoi ». Ils se sont
donc pris mutuellement pour cible, sont restés à portée de corps à corps, et le
but d'attaque, qui à portée relâche le déplacement, les a figés. Le
comportement était exactement celui que la liste de buts décrivait ; c'est la
liste qui était fausse.

`kNoQuarry` distingue désormais « n'importe quoi » de « rien », et un mob
hostile ne chasse que le type que l'appelant nomme. Après correction :
**huit sur huit**, de 17,7 à 69,5 blocs parcourus en quatre-vingts secondes.

Aucun test unitaire n'aurait attrapé cela : chaque but faisait ce qu'il
annonçait.

---

## 9. Reproduire

```bash
python3 scripts/measure_mobs.py maze speed acquire caps light
ctest --preset macos-debug -R test_ov_gameplay
```

Chaque campagne démarre et arrête son propre serveur, sur son propre port et
dans son propre monde — `light` a besoin d'un monde vide et les autres d'un
superflat ordinaire, et le générateur d'un monde ne se change pas après coup.
