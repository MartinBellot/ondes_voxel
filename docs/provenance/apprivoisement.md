# Apprivoisement, montures et espèces passives restantes

Ce dossier trace le loup, le chat, l'ocelot, le perroquet, le cheval, l'âne, la mule, le lama et le
lama marchand, et ce que le serveur garde du lapin, du renard, de la tortue, de l'abeille, de la
chèvre, du dromadaire et du renifleur. Les **mécanismes** sont ceux de la documentation
(minecraft.wiki : *Wolf*, *Cat*, *Ocelot*, *Parrot*, *Horse*, *Donkey*, *Llama*, *Tutorials/Horses*,
*Creeper*) ; **chaque probabilité, plage et indice ci-dessous a ensuite été mesuré contre le vrai
serveur 1.20.1** (`tools/vanilla/server.jar`) par `scripts/measure_tame.py`, avec la sonde de
`measure_husbandry.py` connectée. Les relevés bruts vont dans
`data/vanilla/1.20.1/normalized/tame.json` (gitignoré). Ce qui n'a pas été mesuré est dit à
l'endroit où c'est utilisé, et regroupé au § 9.

Le résultat court :

| mesure | vanilla | nous |
|---|---|---|
| os sur un loup, 150 loups | 150 apprivoisés en 435 essais (1/3 : 145 ± 10) | `next_int(3) == 0`, χ² géométrique p > 0,05, témoins 1/2 et 1/5 rejetés |
| morue sur un chat, 150 chats | 150 en 447 (et saumon, 30 en 87) | 1/3 |
| graines sur un perroquet, 210 perroquets | 210 en 2134 (1/10 : z = −0,25 ; 1/13 rejeté, z = +3,7) | `next_int(10) == 0` |
| loup apprivoisé | assis, santé 20 (6 → 20), max 8 → 20 | idem |
| colère d'un loup frappé | 414 à 775 lus 1–2 ticks après (38 coups) ; la meute entière (4/4, 6 fois) | 400 + `next_int(381)`, meute à 16 blocs |
| cheval apparu, 200 | santé 15..29 (moy. 21,9), vitesse 0,127..0,309 (σ 0,0389), saut 0,455..0,988 (σ 0,1016) | formules du wiki, moyennes à 2 σ |
| âne, mule, lama apparus, 280 | vitesse 0,175 et saut 0,5 **fixes**, santé tirée | idem |
| force d'un lama, 160 | 1/2/3/4/5 = 40/53/56/5/6 | **ajusté** : 1..5 une fois sur six (le wiki dit 1/25) |
| poulains, 2 × 60 | écarts-types au 1/60e près de la règle 1.20 | la règle 1.20, ± 18 % sur σ |
| mule d'un cheval et d'un âne, 12 | vitesse 0,178..0,234, saut 0,516..0,648 | tirés des parents, pas fixes |
| monter un cheval sauvage | jeté (statut 6), tempérament +5 à chaque fois ; cède au 3ᵉ–11ᵉ essai | `next_int(100) < tempérament` |
| monter un lama sauvage | cède à 5–20 : maximum 30 | `next_int(30) < tempérament` |
| loup laissé derrière | reste à 6 ; marche à 11 et s'arrête à 1,7 ; téléporté à 13 et 20 | suit au-delà de 10, s'arrête sous 2, téléporté dès 12 |
| morue sur un ocelot, 40 ocelots | **796 essais sans réponse** : non mesuré (§ 7) | 1/3 du wiki, statuts 41 / 40 |

---

## 1. Méthode

La sonde est celle de `measure_husbandry.py` (`Hand`), étendue (`Rider`) de `Set Passengers`
(0x59). Chaque mob est invoqué avec un UUID choisi `[I;0x4F56,0,0,n]`, ce qui relie l'invocation de
la console au `Spawn Entity` sur le fil. Le propriétaire est la sonde elle-même : son UUID hors
ligne (MD5 de `OfflinePlayer:ovhand`, version 3) écrit en `Owner:[I;…]`. La sonde est en survie,
Résistance V, sur un superplat, `doMobSpawning` coupé.

Piège payé : un `summon` **avec** NBT ne passe pas par l'apparition naturelle (déjà vu dans
`elevage.md` § 1.2) ; les statistiques de cheval se mesurent donc sur des invocations **sans** NBT,
relues par `attribute … base get` dans le même lot de commandes, avant que l'animal ne bouge.

---

## 2. La métadonnée (campagne `meta`)

Un champ NBT à la fois contre une référence de la même espèce, `NoAI` et `Silent` partout :

| espèce | indice | type | ce qu'il dit |
|---|---|---|---|
| loup, chat, perroquet | 17 | octet | 0x01 assis, 0x04 apprivoisé |
| loup, chat, perroquet | 18 | UUID optionnel | le propriétaire |
| loup | 20 | varint | collier ; **rouge (14) n'est pas envoyé** : c'est le défaut |
| loup | 21 | varint | `AngerTime`, **décompté sur le fil** : 400, 399, 398… une mise à jour par tick |
| chat | 19 | variante de chat (type 21) | `tabby` envoie 0 ; `black` n'envoie **rien** : noir est le défaut |
| chat | 22 | varint | collier |
| perroquet | 19 | varint | variante |
| ocelot | 17 | booléen | `Trusting` |
| cheval, âne, mule, lama, dromadaire | 17 | octet | 0x02 apprivoisé, 0x04 sellé, 0x08 `Bred`, 0x10 mange |
| cheval | 18 | varint | `Variant` (couleur \| motifs ≪ 8) |
| âne, mule, lama | 18 | booléen | coffre |
| lama | 19, 20, 21 | varint | force, couleur du tapis (−1 aucun), variante |
| lapin | 17 | varint | `RabbitType` ; le 99 porte en plus un nom `entity.minecraft.killer_bunny` |
| renard | 17 / 18 | varint / octet | type ; 0x01 assis, 0x04 accroupi, 0x20 endormi |
| tortue | 18 | booléen | `HasEgg` |
| abeille | 17 / 18 | octet / varint | 0x04 a piqué, 0x08 nectar ; colère |
| chèvre | 17, 18, 19 | booléens | hurleuse, corne gauche, corne droite |

Deux choses que la documentation du protocole ne dit pas clairement et que la mesure tranche :
**un cheval n'a pas de propriétaire sur le fil** (`Owner` sur un cheval ne change aucun indice), et
**son armure n'est pas une métadonnée** (c'est un équipement, emplacement poitrine). Les cornes de la
chèvre sont lues fausses sur une chèvre invoquée sans `HasLeftHorn` : leur défaut sur le fil est
vrai, et leur ordre (gauche 18, droite 19) est celui de l'archive du protocole — une chèvre à une
corne n'a pas été invoquée.

Constantes : `ov/protocol/entity.hpp`, bloc `── tame ──`, et `MetadataWriter::cat_variant_value`.

---

## 3. Apprivoiser (campagne `tame`)

Chaque animal reçoit un objet à la fois jusqu'aux cœurs ; chaque essai répond par un `Entity Event`,
**7** apprivoisé ou **6** refusé (aucun essai sans réponse sur 1750).

| espèce | objet | animaux | essais | essais par animal 1, 2, 3… |
|---|---|---|---|---|
| loup | os | 150 | 435 | 56 33 17 14 10 4 7 4 1 2 2 |
| chat | morue | 150 | 447 | 55 32 17 17 9 4 6 2 3 2 2 0 1 |
| chat | saumon | 30 | 87 | 11 4 5 4 2 1 3 |
| perroquet | graines de blé | 60 | 781 | de 1 à 48 essais, moyenne 13,0 |
| perroquet, campagne refaite | graines de blé | 150 | 1353 | 9 19 15 14 13 12 5 7 5 5 3 5 4 5 5… (jusqu'à 43), moyenne 9,0 |

`test_tame.cpp` passe les deux premiers histogrammes à un χ² contre une loi géométrique de 1/3
(p > 0,05) **et** contre les témoins 1/2 et 1/5, rejetés à p < 10⁻³ : le test sait dire non.

**Le perroquet a d'abord semblé tomber sous son 1/10 documenté** : 60 apprivoisés où 78,1 ± 8,4
étaient attendus (z = −2,2, p ≈ 0,03). La campagne refaite sur 150 perroquets (`measure_tame.py
parrot`) le tranche : 150 en 1353 essais (z = +1,33), soit **210 en 2134 à eux deux, 0,098 —
z = −0,25 contre 1/10**. Le premier lot était un écart de hasard. Le témoin 1/13, qu'il semblait
indiquer, est rejeté (z = +3,7 sur les deux lots ; χ² de l'histogramme refait 27,6 sur 14 degrés
contre 12,0 pour 1/10).

Ce que fait un essai réussi, relu par `data get` : `Owner` (la sonde), `Sitting: 1b`,
`Health: 20.0f` pour un loup invoqué à 6 — l'apprivoisement **remet la santé au maximum**, qui
passe de 8 à 20 (`attribute … generic.max_health`, lu 8,0 sauvage et 20,0 apprivoisé).

---

## 4. Le loup (campagnes `wolf`, `anger`)

| cas | vanilla | nous |
|---|---|---|
| main vide, puis bâton, os, morue, cookie, pain sur son loup | assis ↔ debout à chaque fois, rien de consommé | idem |
| viande sur un loup plein de vie, adulte | `InLove` 591 relu 9 ticks après, statut 18, viande consommée | amour 600 |
| viande sur un louveteau apprivoisé | `Age` −24 000 → −21 612, consommée | `feeding_growth` d'`elevage.md` |
| viande sur un loup sauvage | rien, gardée | idem |
| teinture rouge sur un collier rouge | gardée | idem |
| deux loups apprivoisés `InLove` | un louveteau 3,3 s après | naissance d'`elevage.md` |

**La guérison par la viande n'est pas mesurée** : chaque loup invoqué avec `Owner` et `Health:4f`
a été relu à 20 — l'`Owner` lu **après** la santé remet l'animal au maximum. La règle appliquée est
celle du wiki (autant de points de vie que la nourriture rend de faim).

**La colère.** 40 loups sauvages frappés une fois : 38 relus en colère, `AngerTime` de 414 à 775
une à deux ticks après le coup, `AngryAt` la sonde ; deux coups n'ont pas porté. Le wiki donne 20 à
39 secondes : `400 + next_int(381)`, dont les bornes sont exactement 400 et 780. **La meute** : six
fois quatre loups à moins de six blocs, un seul frappé — les quatre en colère une seconde plus tard,
les six fois. Au-delà de six blocs, non mesuré ; nous appelons la meute dans la portée de suivi du
loup, 16.

---

## 5. Les chevaux (campagnes `spawn`, `breed`, `temper`)

### 5.1 L'apparition

| type | n | santé | vitesse | saut |
|---|---|---|---|---|
| cheval | 200 | 15..29, moy. 21,88, σ 3,37 | 0,1269..0,3092, moy. 0,22470, σ 0,03894 | 0,4551..0,9879, moy. 0,7138, σ 0,1016 |
| âne | 80 | 15..29, moy. 22,01 | 0,175 fixe | 0,5 fixe |
| mule | 40 | 16..29, moy. 22,48 | 0,175 fixe | 0,5 fixe |
| lama | 120 | 15..30, moy. 22,29 | 0,175 fixe | 0,5 fixe |
| lama marchand | 40 | 18..30, moy. 22,83 | 0,175 fixe | 0,5 fixe |

Les formules du wiki : santé `15 + next_int(8) + next_int(9)`, vitesse `(0,45 + 0,3·(r₁+r₂+r₃))·0,25`
(moyenne 0,225, σ 0,0375), saut `0,4 + 0,2·(r₁+r₂+r₃)` (moyenne 0,7, σ 0,1). Vitesse et saut tombent
dedans. **La santé est un peu basse** : 22,13 sur les 480 animaux mis ensemble, pour 22,5 attendus
(z ≈ −2,3) — nommé, la formule est gardée. Couleurs du cheval 0..6 et motifs 0..4 tous vus.

**La force d'un lama ne suit pas le wiki.** Sur 160 : 1/2/3/4/5 = 40/53/56/5/6. « 1 à 5 une fois sur
25 » donnerait 2,6 au-dessus de 3 ; il y en a 11. La fréquence qui rend ces comptes est une fois sur
six (0,17) : **ajustée**, pas lue (`kLlamaWideStrength`).

### 5.2 Les poulains

Soixante paires apprivoisées, amoureuses (`InLove:600`), dans des enclos de 4 × 2, attributs posés
par NBT :

| paires | santé | vitesse | saut |
|---|---|---|---|
| (20 ; 0,2 ; 0,5) × (28 ; 0,3 ; 0,9) | 19,0..28,8, moy. 23,67, σ 2,224 | moy. 0,2533, σ 0,02575 | moy. 0,7075, σ 0,09473 |
| (24 ; 0,25 ; 0,7) × lui-même | 22,1..25,5, σ 0,7214 | σ 0,01028 | σ 0,02874 |
| cheval (22 ; 0,22 ; 0,6) × âne (20 ; 0,175 ; 0,5) | 12 **mules**, 19,4..23,7 | 0,178..0,234 | 0,516..0,648 |

La règle de 1.20 (wiki, *Horse § Breeding*) : moyenne des parents, plus un écart
`|a − b| + 0,3·(max − min)` fois `(r₁+r₂+r₃)/3 − ½`, replié aux bornes. Elle prédit σ = 2,08 / 0,0279 /
0,0967 pour la première paire et 0,75 / 0,0113 / 0,030 pour la seconde : **les six écarts-types
mesurés sont à moins de 12 % des prédits**, dans les ±18 % qu'autorisent 60 poulains. L'ancienne
règle (moyenne des deux parents et d'un tirage neuf) est le témoin, rejeté. Enfin **la mule tire sa
vitesse et son saut de ses parents** alors qu'une mule apparue les a fixes : l'héritage passe par les
plages du cheval quel que soit le petit.

### 5.3 Monter un cheval sauvage

La sonde, main vide, clique un cheval sauvage : `Set Passengers` la met en selle ; le cheval se
cabre, puis **la jette** (statut 6, `Set Passengers` vide) ou **cède** (statut 7). Chaque chute ajoute
**exactement 5** au `Temper` relu : 5, 10, 15, 20… Les chevaux domptés l'ont été au 3ᵉ, 5ᵉ, 5ᵉ, 6ᵉ,
8ᵉ et 11ᵉ essai, à des tempéraments de 10, 20, 20, 25, 35 et 50 — ce qu'on attend de
`next_int(100) < tempérament`. (Beaucoup de chevaux n'ont pas pu être remontés après la première
chute par la sonde, qui retombe hors d'atteinte : essais nommés `None` dans le relevé.)

Sur 43 animaux (25 chevaux, 8 ânes, 10 lamas), 14 domptés :

| type | domptés | au tempérament | décision après la montée (ticks) |
|---|---|---|---|
| cheval | 7 sur 25 | 10, 20, 20, 25, 30, 35, 50 (sur 100) | n = 63, médiane 60, moyenne 78 |
| âne | 2 sur 8 | 10, 25 | n = 15, médiane 40, moyenne 76 |
| lama | 5 sur 10 | 5, 5, 10, 15, 20 (sur **30**) | n = 22, médiane 20, moyenne 53 |

Les lamas cèdent à des tempéraments bien plus bas : c'est le maximum de 30 documenté, que ces
comptes confirment (au tempérament 5, un lama cède une fois sur six, un cheval une fois sur vingt).
Le délai avant la décision est lu par `time query gametime` interrogé toutes les secondes environ,
donc à une vingtaine de ticks près : une moyenne de 50 à 60 ticks réels, ce que donne un tirage de
1 sur 50 à chaque tick (`kTantrumOdds`, moyenne 50).

---

## 6. Suivre son maître (campagne `follow`)

Un loup apprivoisé, debout, et la sonde téléportée à une distance donnée ; la position du loup est
relue toutes les secondes pendant sept secondes :

| écart | ce que fait le loup |
|---|---|
| 6 blocs | ne bouge pas ; flâne au bout de 6 s |
| 9 blocs | ne bouge pas pendant 3 s, puis part vers la sonde |
| 11 blocs | part aussitôt, **s'arrête à 1,7 bloc** de la sonde |
| 13 blocs | **déjà à côté d'elle** au premier relevé (1 et 1,5 bloc) : téléporté |
| 20 blocs | déjà à 3 blocs d'elle au premier relevé : téléporté |

C'est la règle documentée : suivre au-delà de 10, s'arrêter sous 2, être téléporté à partir de 12,
sur une case libre à 3 blocs au plus du maître. Le cas des 9 blocs est ambigu — la flânerie tire sa
destination au hasard, et elle partait du bon côté ; une campagne plus fine le trancherait.

---

## 7. L'ocelot (campagne `ocelot`) — non mesuré

40 ocelots, 800 morues : **796 essais sans réponse**, 4 refus (statut 40), aucune confiance.
L'ocelot ne prend le poisson qu'à un joueur qu'il a lui-même approché en le tentant, et la sonde
immobile ne l'est jamais : la campagne ne mesure pas ce qu'elle voulait. La règle appliquée est
celle du wiki (1 sur 3, joueur à moins de 3 blocs), avec les statuts 41 et 40 vus sur le fil.

---

## 8. Bout en bout, et le zoo relu

### 8.1 Sur notre serveur (`scripts/check_tame_e2e.py`)

La sonde, en créatif, sur `ov_dedicated --mobs=wolf,horse`, jugée sur le fil seulement. **Le loup
passe, deux fois de suite à l'identique :**

| étape | vu sur le fil |
|---|---|
| des os jusqu'aux cœurs | un refus (6), puis les cœurs (7) au deuxième os ; indice 17 = 0x05 (apprivoisé, assis) ; indice 18 = l'UUID hors ligne de la sonde |
| la main vide | indice 17 = 0x04 : debout |
| la sonde à 16 blocs | le loup à côté d'elle **0,27 s** plus tard : téléporté |
| une teinture bleue | indice 20 = 11 |
| `stop`, redémarrage | le loup revient de `entities/` : 0x04, même propriétaire, collier 11 |

**Le cheval passe aussi** :

| étape | vu sur le fil |
|---|---|
| main vide sur le cheval sauvage | `Set Passengers` avec la sonde ; jetée (6) huit fois, puis les cœurs (7) à la neuvième montée ; indice 17 = 0x02 |
| descendre (`Player Input` 0x02), une selle | `Set Passengers` vide ; indice 17 = 0x06 |
| remonter, 40 `Move Vehicle` de 0,25 bloc | le cheval avance de **10,0 blocs** sur le fil ; descendu |
| `stop`, redémarrage | le cheval revient apprivoisé et sellé (0x06) |

**Mais pas à tous les coups.** Six exécutions : la première ne met pas la sonde en selle (la
version d'alors du script), trois passent, et **deux calent** — le cheval garde la sonde 279 puis
plus de 506 ticks sans décider, et les étapes suivantes échouent faute d'un cheval apprivoisé.
Le journal du serveur date chaque montée, chaque chute et chaque descente avec sa raison, et, tous
les 40 ticks, dit pour chaque animal monté depuis combien de ticks son cerveau le porte et quels
buts tournent. Sur les trois exécutions qui passent, **15 montées : 116, 159, 80, 33, 93, 153, 43,
83, 28, 96, 36, 36, 181, 30 et 191 ticks** de la montée à la décision, **90,5 en moyenne**.

Ce que le journal écarte : la sonde n'est jamais descendue avant la décision ; le cerveau du
cheval tourne à chaque tick du serveur (149 ticks portés au tick 400, 160 ticks après la montée,
sous surcharge) ; le but `tantrum` tourne à chaque relevé ; et aucune décision ne se perd — chaque
chute ajoute exactement 5 au tempérament, jamais sans chute. **Ce qu'il n'explique pas** : le même
tirage de 1 sur 50 par tick donne 50 ticks en moyenne sur 40 chevaux dans `test_tame.cpp`, et 90,5
sur notre serveur (3,1 écarts-types au-dessus) ; vanilla, lu à 20 ticks près, est à 78. Cet écart,
et les deux exécutions qui calent, sont ouverts (§ 9).

### 8.2 Le zoo du vrai serveur, relu par le nôtre

Seize mobs invoqués par `measure_tame.py zoo`, `NoAI`, chacun avec ses champs apprivoisés ; le
monde est copié après `save-all` et lu par `EntityStorage` dans `test_tame_server.cpp`. **Le vrai
serveur n'en a enregistré que quinze** : aucun lama marchand dans son `entities/`, et son
`data get` à l'emplacement du lama marchand a répondu par le chat. Les quinze sont relus avec
leurs champs : propriétaire, assis, collier, variante de chat par son nom, confiance de l'ocelot,
tempérament, selle, armure dorée, `Bred`, coffre et `Items` d'un âne reportés intacts, force et
tapis d'un lama, type de lapin, renard des neiges endormi, variante et propriétaire d'un
perroquet, œuf de tortue, nectar d'abeille, chèvre hurleuse à une corne, selle d'un dromadaire.
Les statistiques d'un cheval sont celles de sa liste `Attributes`, relues puis réécrites telles
quelles.

Puis **`ov_dedicated` sur une copie de ce monde** (`check_tame_e2e.py zoo`) : les quinze mobs
arrivent sur le fil avec leurs indices — chat 0x05, UUID de la sonde, variante 5 (calico), collier
3 ; lama 0x02, force 4, tapis 11 (bleu), variante 3 ; cheval 14 (0x02 | 0x04 | 0x08) et variante
515 ; âne 0x02 et coffre ; ocelot confiant ; lapin 3 ; renard des neiges (17 = 1) endormi (18 =
0x20) ; perroquet 0x05, variante 2 ; tortue à l'œuf ; abeille 0x08 ; chèvre hurleuse, corne droite
fausse ; dromadaire sellé (0x04). `save-all`, et le monde réécrit par notre serveur est gardé pour
`measure_tame.py zoo_back`.

Et **le vrai serveur relit ce monde réécrit** (`measure_tame.py zoo_back`) : **les 15 mobs
chargés, les 14 recherches (par type) retrouvent chacune tous les champs attendus** —
propriétaire, assis, collier, variante `minecraft:calico`, confiance, `Variant: 515`, `Tame`,
`Temper: 15`, selle, armure dorée, `Bred`, coffre et pomme de l'âne, force et tapis du lama, type
de lapin, renard des neiges endormi, variante et propriétaire du perroquet, œuf de tortue, nectar,
chèvre hurleuse à une corne, selle du dromadaire. L'aller-retour vanilla → nous → vanilla est
fermé. Deux faux départs de la mesure elle-même, corrigés et gardés dans l'historique : sa mise en
place tuait tout le zoo avant de le lire, et sa première recherche par position ne trouvait que
les mobs assis, les autres ayant marché (§ 9, `NoAI`).

---

## 9. Ce qui n'est pas fait, ou pas mesuré

**Non fait, et nommé.**

* **L'inventaire d'un cheval** (clic accroupi, `Open Horse Screen`) : ni la fenêtre ni ses cases.
  La selle, l'armure, le coffre et le tapis se posent d'un clic avec l'objet en main. Les `Items`
  d'un âne lus sur disque repartent intacts, sans jamais changer.
* **Monter** : pas de contrôle de mouvement sur `Move Vehicle` (vanilla refuse un déplacement trop
  grand ; nous le prenons), pas de saut du cheval côté serveur (`Player Command` 5 et 6 ignorés : le
  client simule son saut), pas de recherche d'une place où descendre (la sonde est posée à côté de
  l'animal, comme pour les wagonnets). Le lama se monte et s'apprivoise, ne se dirige pas ; le
  dromadaire se selle et ne se monte pas.
* **La mule née d'un cheval et d'un âne** : notre `BreedGoal` n'accouple que deux animaux du même
  type ; la règle d'héritage d'une mule est pourtant écrite et mesurée (§ 5.2).
* **Le loup** : ni la chasse des moutons, lapins et renards par un loup sauvage, ni le bond sur la
  cible, ni la mendicité (indice 19), ni la fuite devant un lama.
* **Le chat** : ni l'assise sur les lits, les coffres et les fours, ni le cadeau du matin, ni la
  fuite de l'ocelot devant un joueur qui ne lui a pas été présenté. Le creeper fuit bien chats et
  ocelots (6 blocs, 1,0 puis 1,2).
* **Le perroquet** marche : ni le vol, ni l'épaule, ni l'imitation des mobs, ni la danse, ni le
  cookie qui le tue.
* **Le lapin** glisse (ni les bonds, ni le lapin tueur, ni les types par biome : les quatre types
  tempérés sont tirés à parts égales) ; **le renard** ne dort pas au rythme du jour, ne porte rien, ne
  fait confiance à personne ; **la tortue** n'a ni plage natale, ni œufs, ni écaille ; **l'abeille**
  n'a ni ruche ni nid (le bloc et son entité ne sont pas faits), ni pollinisation, ni colère, ni dard ;
  **la chèvre** ne charge pas et ne perd pas ses cornes ; **le renifleur** ne creuse pas.
  Tous vivent, se nourrissent, se reproduisent et gardent leur type ou leur drapeau à la sauvegarde.
* **Le crachat du lama** (le projectile `llama_spit`) et les **caravanes** (la laisse) : non faits.
* **`NoAI` n'est pas respecté par notre serveur**, pour aucun mob : le drapeau est relu et
  réécrit tel quel, mais le cerveau tourne. Vu sur le zoo relu (§ 8.2) : pendant les 45 s où
  `ov_dedicated` l'avait chargé, les mobs debout ont quitté leur place, et seuls le loup, le chat
  et le perroquet assis y étaient encore. C'est un manque de tous les mobs, pas de
  l'apprivoisement ; il n'est pas corrigé ici.

**Non mesuré, et appliqué d'après la documentation.**

* Ce que la viande rend de santé à un loup, et le poisson à un chat (§ 4) ; les tables du cheval
  et du lama (sucre, blé, pomme, carotte dorée, pomme dorée, foin) : santé, croissance, tempérament.
* La portée de la meute au-delà de six blocs (nous : 16) ; le propriétaire d'un louveteau (celui du
  parent qui conclut).
* La robe d'un poulain (4/9 par parent, 1/9 neuve) et ses motifs (2/5, 2/5, 1/5) ; la force d'un
  lamaton (entre 1 et la plus forte des deux).
* Les modificateurs de marche des espèces ajoutées (âne, mule, lama : ceux du cheval, 0,7 et 1,2 ;
  les autres 1,0) ; les vitesses de tentation des six espèces ajoutées à l'élevage (1,0).
* L'ordre des deux cornes de la chèvre sur le fil (§ 2).

**Mesuré, et pas tout à fait conforme.**

* **Le délai de décision d'un cheval monté, sur notre serveur** : 90,5 ticks en moyenne sur 15
  montées de bout en bout, contre 50 pour la même règle dans `test_tame.cpp` et 78 chez vanilla
  (§ 5.3 et § 8.1) ; deux exécutions sur six calent au-delà de 279 et 506 ticks. Rider perdu, cerveau
  affamé, but bloqué et décision perdue sont écartés par le journal ; la cause ne l'est pas. La
  prochaine étape est de journaliser chaque tirage du but.
* **La santé d'un cheval apparu** : 22,13 sur 480 animaux pour 22,5 attendus (z ≈ −2,3, § 5.1).
* **La force d'un lama** : ajustée sur la mesure, pas lue dans une règle (§ 5.1).
