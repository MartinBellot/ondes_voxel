# Effets de statut et modificateurs d'attributs

Ce qui suit est établi par **mesure contre le vrai serveur 1.20.1**
(`tools/vanilla/server.jar`, SHA-1 `84194a2f286ef7c14ed7ce0090dba59902951553`),
par `scripts/measure_effects.py`. Le registre `minecraft:mob_effect` donne trente-trois
noms et leurs identifiants (**à partir de 1**) ; le registre `minecraft:attribute` en donne
treize. Tout le reste — l'intervalle de la régénération, le modificateur de la vitesse, la
couleur des particules, la règle qui cache un effet faible sous un fort — est du code Java
et n'apparaît dans aucun rapport. Aucun nombre de ce document n'a été recopié de mémoire :
chacun a sa campagne.

Les résultats bruts vont dans `data/vanilla/1.20.1/normalized/effects.json` (gitignoré,
régénérable). Les tables retenues vivent dans `src/ov_gameplay/src/{attributes,effects}.cpp`
et sont rejouées par les tests marqués `[parity]` de `test_attributes.cpp`,
`test_effects.cpp`, `test_breaking.cpp` et `test_effect_packets.cpp`.

---

## 0. Comment le serveur a été interrogé

Mêmes instruments que `survie.md` — la console, le `Server` de `measure_entities.py`, le
`Bot` de `measure_survival.py` — plus un second client, le `Miner` de `vanilla_miner.py`,
pour le cassage. Trois choix de méthode portent tout le reste :

* **Les durées exactes passent par la NBT, pas par la commande.** `effect give` ne connaît
  que les secondes. Une vache invoquée avec `ActiveEffects:[{Id:10,Amplifier:1b,Duration:137}]`
  porte exactement 137 ticks. On ne lit pas la vache pendant l'effet : on attend qu'il soit
  fini et on compte ce qu'il a fait **en tout**. Ce total ne dépend d'aucun minutage de la
  console.
* **Les lectures de doubles sont exactes.** La console imprime les doubles par
  `Double.toString`, qui fait l'aller-retour. `attribute … get` et `data get entity …
  Attributes` sont donc comparés **bit pour bit**, pas à une tolérance.
* **Les attentes se comptent en ticks du serveur.** `time query gametime` avant et après.
  Voir le piège 3.

### Les pièges payés pendant cette campagne

1. **Un zombie à midi brûle.** Les effets instantanés sur un mort-vivant sont revenus faux
   d'un point à quatre amplificateurs sur six (−7 au lieu de −6, +3 au lieu de +4). Ce
   n'était pas l'effet : c'était le soleil. Refait à minuit, **12/12**. La première campagne
   de métadonnées est morte de la même cause — le zombie a brûlé jusqu'à zéro au milieu de la
   liste, et toutes les couleurs après le poison sont revenues vides. Elle a été refaite sur
   une **vache** à mille points de vie, qui n'est pas mort-vivant et accepte donc aussi
   régénération et poison.
2. **Un argument par défaut Python est lié à la définition.** `read_player(server, name=BOT)`
   de `measure_survival.py` garde le `BOT` de *son* module même quand on réaffecte
   `ms.BOT` : toute la campagne de saturation a lu un joueur inexistant et rapporté des
   `None`. Passer le nom explicitement.
3. **Le serveur de mesure peut tomber à 2 TPS.** Pendant une exécution, avec trois JVM et un
   compilateur sur la machine, une attente de six secondes a duré **onze** ticks. La campagne
   de remplacement n'a alors jamais vu un effet caché remonter, et la campagne d'absorption a
   trouvé trois points de trop — une seconde frappe tombée *dans* la fenêtre
   d'invulnérabilité qu'on croyait fermée. Refaites avec `wait_ticks` sur l'horloge du
   serveur, les deux sont conformes (§ 6, § 8).
4. **Le bot peut être déconnecté en silence.** `pump` note la perte et rend la main ; les
   campagnes suivantes tournent contre un socket mort et rapportent des listes vides.
   La première exécution l'a perdu avant `metadata` et trois campagnes n'ont rien mesuré.
   Le script vérifie maintenant le bot avant chaque campagne, le remplace, et garde la raison
   donnée par le serveur.
5. **Une écriture de notre propre banc a été prise pour une mesure.** Le premier Update
   Attributes capturé portait une vitesse de base de `0.1` pile — parce qu'un utilitaire du
   script venait de faire `attribute … base set 0.1`. La vraie base du joueur, lue avant tout
   contact, est **0.10000000149011612** (un flottant 0,1). Ce paquet reste dans les tests
   parce que ses octets sont justes, et le commentaire dit d'où vient son 0,1.

---

## 1. Attributs

### Les bornes — 13/13

La base est posée à +10⁹ puis −10⁹ sur une entité qui possède l'attribut (un zombie pour la
plupart, le joueur pour la vitesse d'attaque et la chance, un cheval et un perroquet pour les
deux qui leur sont propres), et `attribute … get` est relu.

| attribut | min | max |
|---|---|---|
| `generic.max_health` | 1 | 1024 |
| `generic.follow_range` | 0 | 2048 |
| `generic.knockback_resistance` | 0 | 1 |
| `generic.movement_speed` | 0 | 1024 |
| `generic.flying_speed` | 0 | 1024 |
| `generic.attack_damage` | 0 | 2048 |
| `generic.attack_knockback` | 0 | 5 |
| `generic.attack_speed` | 0 | 1024 |
| `generic.armor` | 0 | 30 |
| `generic.armor_toughness` | 0 | 20 |
| `generic.luck` | −1024 | 1024 |
| `zombie.spawn_reinforcements` | 0 | 1 |
| `horse.jump_strength` | 0 | 2 |

**La base est stockée non bornée** : après `base set 1000000000`, `base get` répond un
milliard et `get` répond la borne. Seule la valeur calculée est bornée.

### Les trois opérations — 6/6, bit pour bit

Sur la vitesse d'un zombie (base 0.23000000417232513), six modificateurs ajoutés un à un par
`attribute … modifier add`, le total relu après chacun :

| ajout | total mesuré |
|---|---|
| +0.1 `add` | 0.3300000041723251 |
| +0.5 `multiply_base` | 0.49500000625848767 |
| +0.25 `multiply` | 0.6187500078231096 |
| −0.5 `multiply` | 0.3093750039115548 |
| +0.07 `add` | 0.3750000039115548 |
| −0.3 `multiply_base` | 0.30000000312924385 |

Ordre : les additions sur la base, puis `total += base' × a` pour chaque `multiply_base`,
puis `total ×= 1 + a` pour chaque `multiply`. La console nomme les opérations `add`,
`multiply_base`, `multiply` ; la NBT et le fil portent 0, 1, 2. Un UUID déjà présent est
refusé (« Modifier … is already present »).

### L'ordre *dans* une opération — 5/5 contre 2/5

L'addition de doubles n'est pas associative : `base + 0,1 + 0,07` et `base + 0,07 + 0,1`
diffèrent d'un ulp. Les UUID sont choisis pour que leur seau de `HashMap` Java (…a009 dans le
seau 9, …a002 dans le seau 2) aille **contre** l'ordre d'insertion.

| cas | mesuré | ordre des seaux | ordre d'insertion |
|---|---|---|---|
| a009:+0,1 puis a002:+0,07 | …252 | …252 ✓ | …251 ✗ |
| a002:+0,07 puis a009:+0,1 | …252 | …252 ✓ | …252 ✓ |
| a009:+0,07 puis a002:+0,1 | …251 | …251 ✓ | …252 ✗ |
| b009:×1,1 puis b002:×1,07 | …267 | …267 ✓ | …2675 ✗ |
| b002:×1,07 puis b009:×1,1 | …267 | …267 ✓ | …267 ✓ |

**Ordre des seaux, 5/5 ; ordre d'insertion, 2/5.** `AttributeInstance::value` trie donc les
modificateurs d'une opération par `java_hash_bucket(uuid, table)` (hash du UUID replié,
`h ^ (h >>> 16)`, masqué par la taille de table), stable sur l'insertion. Approximation
nommée : un `HashSet` Java ne rétrécit jamais, le nôtre recalcule la table depuis le nombre
courant — la différence n'existe qu'au-delà de douze modificateurs d'une même opération.

### Les bases du joueur

Un joueur ne s'invoque pas : le balayage de `measure_entities.py` ne l'a jamais vu. Lues sur
le bot avant tout contact (campagne `player_bases`) : max_health 20, knockback_resistance 0,
movement_speed **0.10000000149011612**, attack_damage 1, attack_speed 4, armor 0,
armor_toughness 0, luck 0. Pas de follow_range : **absent n'est pas zéro**.

---

## 2. Les paquets

Chaque paquet est identifié par sa **charge** — l'identifiant d'entité du bot suivi de
l'identifiant d'effet qu'on vient de nommer — jamais par l'identifiant qu'une table
annonce. Les constantes `0x6C`, `0x3F` et `0x6A` du dépôt sont ainsi **confirmées**.

| paquet | disposition mesurée |
|---|---|
| Entity Effect `0x6C` | varint entité · varint effet (**1-based**) · octet amplificateur · varint durée · octet drapeaux · bool + NBT |
| Remove Entity Effect `0x3F` | varint entité · varint effet |
| Update Attributes `0x6A` | varint entité · varint n · (chaîne, f64 **base**, varint m · (UUID, f64, octet op)) |

Ce que la capture a établi et qu'une lecture de la spécification n'aurait pas donné :

* **Masquer les particules masque aussi l'icône.** `effect give … true` produit l'octet de
  drapeaux `0x00`, pas `0x04`. La commande lie l'icône aux particules.
* **L'infini est la varint de −1**, cinq octets `ffffffff0f`.
* **L'obscurité porte son état de fondu** en NBT réseau **nommée** (`0a 0000` : racine avec
  nom vide — 1.20.1 est d'avant la NBT anonyme de 1.20.2), clés dans l'ordre du jeu.
* **Update Attributes porte la base, pas le total**, et **seulement les attributs qui ont
  changé** : donner la vitesse a envoyé une propriété, pas les huit du joueur.
* Au don répété d'un même effet plus long, `0x6C` est renvoyé.

Les charges capturées sont figées dans `test_effect_packets.cpp` et nos encodeurs les
reproduisent octet pour octet ; les décodeurs refusent ce qui déborde.

---

## 3. Les modificateurs portés par les effets — 9 effets, 22 totaux bit pour bit

Chaque effet donné aux amplificateurs 0, 1 et 4, sur un zombie et sur le bot, la liste
`Attributes` relue en SNBT. **Exactement neuf** effets portent un modificateur ; les
vingt-quatre autres n'en portent sur aucune des deux cibles.

| effet | attribut | UUID | montant niveau I | op |
|---|---|---|---|---|
| speed | movement_speed | `91aeaa56-376b-4498-935b-2f7f68070635` | 0.20000000298023224 | 2 |
| slowness | movement_speed | `7107de5e-7ce8-4030-940e-514c1f160890` | −0.15000000596046448 | 2 |
| haste | attack_speed | `af8b6e3f-3328-4c0a-aa36-5ba2bb9dbef3` | 0.10000000149011612 | 2 |
| mining_fatigue | attack_speed | `55fced67-e92a-486e-9800-b47f202c4386` | −0.10000000149011612 | 2 |
| strength | attack_damage | `648d7064-6a60-4f59-8abe-c2c23a6dd7a9` | 3 | 0 |
| weakness | attack_damage | `22653b89-116e-49dc-9b6b-9971489b5be5` | −4 | 0 |
| health_boost | max_health | `5d6f0ba2-1186-46ac-b896-c61c5cee99cc` | 4 | 0 |
| luck | luck | `03c3c89d-7037-4b42-869f-b146bcb64d2e` | 1 | 0 |
| unluck | luck | `cc5af142-2bd2-4215-b636-2605aed11727` | −1 | 0 |

Les quatre montants en pourcentage sont des **flottants** promus en double, et le montant au
niveau `a` est ce double multiplié par `a + 1` **en double** : la vitesse V vaut
1.0000000149011612, que seul ce produit donne. Nom du modificateur :
`effect.minecraft.speed 2` (identifiant de description, espace, amplificateur).

La faiblesse emmène les dégâts d'attaque à la borne : zombie 3 − 4 → **0**. L'absorption ne
porte **pas** de modificateur en 1.20.1 (l'attribut `max_absorption` est postérieur).

---

## 4. Les effets périodiques — 522 vaches sur 522

Pour chaque effet, chaque amplificateur 0..5 et 29 durées D choisies autour des multiples
utiles (1 à 7, 9 à 13, 19 à 26, 39 à 51, 99 à 101, 150, 151), une vache à 500 PV sur 1000,
l'effet dans sa NBT. Compté une fois l'effet fini.

**Régénération** : `floor(D / k)` soins d'un point, `k = 50 >> amp`, chaque tick quand k
vaut 0. Les 174 vaches, sans exception. Le premier soin tombe sur le premier tick où la
durée restante est un multiple de k — D compris.

**Poison** (`k = 25 >> amp`) et **flétrissement** (`k = 40 >> amp`) : même règle, **amincie
par la fenêtre d'invulnérabilité**. Poison III (k = 6) sur D = 12 : une seule frappe, pas
deux — la seconde tombe six ticks après la première. Le poison V et VI (k = 1) donne une
frappe jusqu'à D = 10 et deux à D = 11 : **l'écart minimal entre deux frappes d'effet est de
dix ticks**.

### La fenêtre : dix ticks ici, onze dans `survie.md`

`survie.md` § 3 a mesuré onze ticks entre deux frappes de console. Ce n'est pas une
contradiction mais une **phase** : une commande frappe avant le tick où le compteur de la
victime est décrémenté, un effet frappe *dans* ce tick, après. `damage.cpp` encode la
mesure de console avec « partiel tant que le compteur vaut au moins 10 » ; les effets
utilisent `effect_damage_constants`, « au moins 11 ». Les deux campagnes sont reproduites
chacune avec ses constantes, et le modèle de survie n'a pas été touché.

**Planchers** : trois vaches à 3 PV sous poison I, IV et VI pendant 300 ticks finissent
**toutes à 1,0**. La vache à 3 PV sous flétrissement IV **meurt**.

**Effet infini** : il n'a pas de durée à décompter ; son intervalle se lit contre l'âge du
porteur. Quatre vaches sous régénération I infinie ont gagné **8** points en 20,05 s, le taux
fini d'un soin par cinquante ticks.

**Faim** : `0,005 × (amp + 1)` d'épuisement **par tick**. Mesuré 0,50001 (I, 5 s),
0,99999 (II, 5 s), 2,5000 (V, 5 s), 1,99999 (X, 2 s).

**Saturation** : **une application toutes les vingt ticks**, `amp + 1` de nourriture et
deux fois autant de saturation (bornée par la nourriture). 1 s → une application, 2 s → deux,
3 s → trois, 5 s → cinq, aux amplificateurs 0, 1, 2, 3 et 9 : l'amplificateur ne raccourcit
pas l'intervalle. Ce que la mesure ne tranche pas : la commande ne donne que des secondes,
donc « au tick où la durée est multiple de 20 » et « au premier tick puis tous les 20 »
prédisent la même chose. Le premier est retenu.

---

## 5. Les effets instantanés — 24/24

Vaches à 500/1000 et zombies (à minuit, voir piège 1), amplificateurs 0..5 :

| | vache | zombie |
|---|---|---|
| soin instantané | +4 << amp | −6 << amp |
| dégâts instantanés | −6 << amp | +4 << amp |

Appliqués **une fois** à l'arrivée, jamais stockés — une seconde de soin instantané V a donné
128, pas 20 × 128. Leur couleur n'est donc jamais visible et la table la met à 0 plutôt que
d'y écrire une valeur de mémoire.

---

## 6. Remplacement, effet caché, promotion — 11 + 3 + 3 cas

Deux `effect give` de vitesse dans un même lot, l'`ActiveEffects` relu :

| cas | résultat |
|---|---|
| III 5 s sur I 30 s | III 5 s, **I caché** 30 s |
| I 30 s sur III 5 s | III 5 s, **I caché** 30 s |
| III 30 s sur I 5 s | III 30 s, rien de caché |
| I 5 s sur III 30 s | inchangé |
| II 30 s sur II 5 s | II 30 s |
| II 5 s sur II 30 s | inchangé |
| III 5 s sur I infini | III 5 s, I infini caché |
| I 30 s sur III infini | inchangé |
| II infini sur II 30 s | II infini |
| III 5 s, II 10 s, I 30 s | **chaîne de trois** |
| I 30 s, particules masquées | `ShowParticles:0b`, `ShowIcon:0b` |

**Promotion**, sur l'horloge du serveur : la chaîne 98 / 198 / 598 lisait 87 / 487 après
111 ticks et 385 après 213. **Tous les niveaux décomptent ensemble**, et l'effet caché
revient avec ce qui lui reste. Sous un infini, le fini expire et l'infini revient.

**Drapeaux** : un second don qui ne change rien d'autre impose quand même ses particules et
son icône — masquer après un visible masque, montrer après un masqué montre, et même un don
plus faible qui part se cacher change les drapeaux de l'effet visible.

**Rafraîchir la vigueur** : une vache pleine sous vigueur II (18/18) à qui l'on redonne
vigueur II plus longue retombe à **10** — le modificateur est retiré, la santé bornée au
maximum de base, *puis* le modificateur remis. Pareil vers vigueur III. Un don plus court ne
change rien et laisse 18. `ActiveEffects::add` rafraîchit en `ended` puis `started`, qui
reproduit exactement cela.

---

## 7. Qui refuse quoi

`effect give` des 31 effets non instantanés à un zombie, un squelette, une araignée et une
vache. Zombie et squelette refusent **régénération et poison** ; l'araignée refuse
**poison** ; la vache ne refuse **rien**.

---

## 8. Résistance, absorption, vigueur

**Résistance — 36/36.** Une vache par amplificateur 0..5 et par frappe :
`frappe × (25 − 5 × (amp + 1)) / 25` en flottant, à partir de V plus rien ne passe.
`out_of_world` (`#bypasses_resistance`) et `starve` (`#bypasses_effects`) passent entiers à
tous les niveaux ; `magic` est réduit comme `generic`.

**Absorption — 4 cas sur l'horloge du serveur.** 4 × (amp + 1) points. Une vache à 50/100
sous absorption II (8) :

| frappes | sans absorption | avec |
|---|---|---|
| 3 puis 30, 30 ticks plus tard | 17 | 25 |
| 3 puis 30, 6 ticks plus tard (fenêtre) | 20 | 28 |

La fenêtre compare les frappes **brutes** ; ce qui passe est ensuite pris sur l'absorption
avant la santé. Retirée, l'absorption tombe à 0 (soustraction bornée).

**Vigueur.** +4 × (amp + 1) au maximum, **sans** soin. À la fin, la santé est bornée : 18/18
→ 10/10.

---

## 9. Les autres effets observables par le serveur

* **Saut amélioré — 15/15 chutes.** Des vaches lâchées de 6, 10 et 20 blocs sous saut I, II
  et V : `ceil(distance − 3) − (amp + 1)`, borné à 0 (3 → 2, 1, 0 ; 7 → 6, 5, 2 ;
  16 → 15, 14, 11).
* **Chute lente** : aucun dégât, des trois hauteurs.
* **Respiration aquatique et puissance de conduit** : sous l'eau, l'air est **figé** — 299
  après cinq secondes, là où la vache témoin est à 193. Ni vidé ni rempli. La **grâce du
  dauphin** ne fait rien à l'air (195).
* **Invisibilité** met le bit `0x20` de l'index 0, **surbrillance** le bit `0x40`.

---

## 10. Les métadonnées et la couleur — 64/64

| index | type | quoi |
|---|---|---|
| 0 | Byte | `0x20` invisibilité, `0x40` surbrillance |
| 10 | VarInt | couleur des particules, 0 sans effet visible |
| 11 | Boolean | tous les effets **visibles** sont ambiants (vrai s'il n'y en a aucun de visible, faux s'il n'y a aucun effet) |
| 15 (joueur) | Float | absorption |

Les 31 couleurs non instantanées ont été mesurées une par une sur une vache. **Trente**
coïncidaient avec ce que j'avais tapé de mémoire en écrivant la table ; **une non** : la
chute lente est `0xF3CFB9`, pas `0xFEFFDF`. C'est exactement la raison pour laquelle la table
a été entièrement remplacée par la mesure.

**La formule du mélange.** Une moyenne des couleurs visibles pondérée par `amp + 1`, en
flottant. Quatre ordres d'évaluation de cette même moyenne ont été confrontés à 64 cas
mesurés — les 31 couleurs seules, 27 effets seuls aux amplificateurs 2, 4 et 6, six
mélanges :

| ordre | échecs |
|---|---|
| `(float)(w × c) / 255`, somme, `/ total × 255`, tronqué | **0** |
| idem, `× 255 / total` | 20 |
| `(c / 255) × w`, `/ total × 255` | 15 |
| `(c / 255) × w`, `× 255 / total` | 20 |

Le seul survivant fait **dériver** la couleur d'un effet seul : la vitesse V envoie
`0x33EAFF`, pas `0x33EBFF`, la nausée V `0x541D4A`. Le jeu le fait, et nous aussi.

---

## 11. Le cassage sous effets — 16 cas, 30 comptes exacts sur 32

La méthode de `PROVENANCE.md` (le serveur casse au tick exact quand la déclaration du client
arrive trop tôt), deux essais par cas :

| cas | ticks |
|---|---|
| pierre, main nue | 150 |
| … hâte I / II / III / V | 125 / 108 / 94 / 75 |
| … puissance de conduit I | 125 |
| … conduit II + hâte I | 108 |
| pierre, pioche en bois, hâte II | 17 (un essai à 16) |
| obsidienne, pioche en netherite, hâte II | 120 |
| terre, main nue | 15 (un essai à 14) |
| … fatigue I / II | 50 / 167 |
| terre, pelle en netherite, fatigue III | 618 |
| … efficacité V, fatigue IV | 530 |
| terre, hâte II + fatigue I | 36 |
| planches, hâte III | 38 |

La table de `breaking.cpp` (hâte `1 + 0,2 × (amp + 1)`, fatigue 0,3 / 0,09 / 0,0027 /
0,00081) donnait déjà chacun de ces nombres ; ce qui manquait était l'effet lui-même, et une
chose que la table ne disait pas : **la puissance de conduit creuse comme la hâte**, et avec
les deux c'est **le plus grand** amplificateur qui compte. Les deux comptes à un tick près
sont la gigue de départ du mineur, que l'autre essai du même cas ne montre pas.

---

## 12. Les aliments

Le bot mange, son `ActiveEffects` est relu. Toutes les durées sont revenues **exactement 17
ticks** sous un nombre rond — 83, 2383, 5983, 283, 583, 1183 — le même écart pour chaque
aliment, qui est le délai entre la fin du repas et la lecture : les durées sont donc
épinglées au tick les unes par rapport aux autres.

| aliment | effets |
|---|---|
| pomme dorée | régénération II 100, absorption I 2400 |
| pomme dorée enchantée | régénération II 400, résistance I 6000, résistance au feu I 6000, absorption IV 2400 |
| poisson-globe | poison II 1200, faim III 300, nausée I 300 |
| œil d'araignée | poison I 100 |
| chair putréfiée | faim I 600, **49/60** |
| poulet cru | faim I 600, **18/84** |
| pomme de terre empoisonnée | poison I 100, **46/84** |
| ragoût suspect (sans NBT) | rien |
| fiole de miel | retire le poison, **et seulement lui** |
| seau de lait | retire **tout** |

**Les chances ne sont pas épinglées.** Les valeurs retenues, 0,8 / 0,3 / 0,6, sont dans les
intervalles de Wilson à 95 % des fréquences (chair [0,70 ; 0,89], poulet [0,14 ; 0,31], pomme
de terre [0,44 ; 0,65]) ; une campagne ne distingue pas 0,8 de 0,79, et le poulet est au bord
de son intervalle. Nommé plutôt qu'affirmé.

---

## 13. Le format de sauvegarde, sur un vrai fichier joueur

Le bot sous vitesse III (sur vitesse I cachée), obscurité, régénération infinie à particules
masquées et chance V ; `save-all flush` ; `world/playerdata/<uuid>.dat` lu par un lecteur NBT
typé.

* `ActiveEffects` : liste de composés `Id` **int**, `Amplifier` **byte**, `Duration` **int**,
  `Ambient` / `ShowParticles` / `ShowIcon` **byte**, `HiddenEffect` composé imbriqué,
  `FactorCalculationData` pour l'obscurité.
* **Sans effet, la clé est absente** — pas de liste vide.
* `Attributes` contient les **modificateurs des effets** (`effect.minecraft.speed 2`,
  `Operation` int, `UUID` en tableau d'int).
* L'amplificateur est un octet : 255 est écrit −1.

`save_effects` / `load_effects` écrivent et relisent ce format (test d'aller-retour, types
vérifiés). **Mais ce serveur n'écrit aucun fichier joueur** — voir § 16.

---

## 14. La mort

Au moment de la mort vanilla : Damage Event, Combat Death, métadonnées, vitesse —
**aucun Remove Entity Effect**. Après le respawn, plus aucun effet. Le client l'apprend par
Respawn (`data_kept = 0`). `EffectSession::on_death` oublie donc tout, en silence.

---

## 15. Le branchement serveur

`src/ov_server/src/effect_session.{hpp,cpp}`, sur le modèle de `survival_session` : les
règles sont dans `ov_gameplay`, l'état entre deux paquets et les paquets sont ici.

**API pour `/effect`** (un autre agent) : `who.effects.apply(instance, who.survival, io,
bearer)`, `remove(effect, …)`, `clear(…)`. `effect give <cible> <effet> <s> <amp> <masquer>`
se traduit par `duration = s × 20` (ou `kInfiniteDuration`), `visible = show_icon =
!masquer`, `ambient = false`. `AddResult::Immune` et `AddResult::Unchanged` sont les deux
« Unable to apply ». `parse_effect_spec` lit `nom[:amp[:ticks]]`.

Ce qui est branché, chaque bloc délimité `// ── effects ──` dans `server.cpp` :

| où | quoi |
|---|---|
| boucle de tick, avant la survie | chaque joueur confirmé : tick des effets, paquets |
| repas terminé | effets de l'aliment, miel ; lait (qui n'est pas un aliment) vide tout et rend un seau |
| mort | `on_death` |
| progression de cassage | `dig_stance` : hâte / conduit / fatigue |
| vue de combat | cécité (critique), force et faiblesse (`CombatPlayer`, puis `AttackerState`) |
| tick de survie | respiration aquatique (air figé), saut amélioré et chute lente (chute) |
| dégâts du joueur | résistance (`SurvivalSession::mitigation`), absorption (`HealthState`) |

Paquets : `0x6C` / `0x3F` au joueur seul (vanilla ne dit rien aux autres) ; `0x6A` et
métadonnées (index 0, 10, 11, 15) au joueur **et** aux observateurs.

`--effect=nom:amp:ticks,…` donne des effets à chaque joueur qui entre : un point d'entrée de
test en attendant `/effect`, nommé comme tel.

### De bout en bout — contre notre serveur

`scripts/check_effects_e2e.py` branche la sonde des autres vérifications sur
`ov_dedicated --survival --effect=…` et lit le socket.

**Vitesse** (`--effect=speed:1:60`) : Entity Effect vitesse II, 60 ticks, drapeaux `0x06` ;
Update Attributes avec la base mesurée du joueur **0.10000000149011612** et le modificateur
`91aeaa56-…` de **0.4000000059604645**, opération 2 ; puis Remove Entity Effect **58,7 ticks**
plus tard (60 à l'interpolation près), et l'attribut renvoyé sans modificateur.

**Régénération** (`--effect=hunger:255:40,regeneration:1:1200`) : la faim 256 pendant deux
secondes laisse 13 de nourriture, sous le seuil de 18 de la régénération naturelle ; la sonde
tombe de dix blocs (20 → 13) ; puis **sept soins de exactement +1,0**, espacés de **24,8 à
25,2 ticks** sur l'horloge du serveur — la régénération II, `50 >> 1`.

**Le piège, et il m'a d'abord trompé.** La première mesure de la vitesse donnait 131 ticks,
puis 94 après avoir interpolé entre deux Update Time. J'ai d'abord accusé le verrou
`players_mutex` pris sans attendre (un tick d'effet sauté quand le fil réseau le tient) et
écrit un rattrapage : **95**, rien n'avait changé. L'hypothèse était fausse et le rattrapage
a été retiré. La vraie cause est le piège 22 du briefing : `TickClock::advance()` avale les
ticks qu'il a en retard, donc l'âge du monde envoyé par Update Time suit l'horloge murale même
quand la boucle ne tient pas vingt itérations par seconde — ce qui est le cas pendant la
première connexion d'un build de debug, qui génère ses chunks. L'effet comptait bien 60
itérations ; la sonde en mesurait 95 d'horloge. Une connexion d'échauffement d'abord, puis la
mesure : **58,7**. À retenir pour quiconque chronomètre ce serveur au tick près : mesurer
une fois le monde chaud, jamais pendant une connexion qui génère.

---

## 16. Ce qui n'est pas fait

* **Les mobs ne portent pas d'effets côté serveur.** Les règles le permettent
  (`EffectTarget` est implémenté par un mob dans les tests), mais le serveur n'a ni table
  d'effets par mob ni table des corps (mort-vivant, arthropode) au-delà des trois types
  mesurés.
* **Pas de persistance.** Ce serveur n'écrit aucun `playerdata` ; les effets d'un joueur
  qui se reconnecte sont perdus, là où vanilla les garde. Le codec NBT est prêt et vérifié.
* **Le renvoi périodique de `0x6C`** que vanilla fait en cours d'effet n'est ni mesuré ni
  reproduit.
* **L'ordre d'itération des effets** : ordre des identifiants ici, table de hachage par
  identité en vanilla. Ne compte que si deux effets agissent sur la même valeur au même
  tick.
* **Effets sans règle serveur** : nausée, vision nocturne, cécité (hors critique),
  obscurité (le fondu n'est pas simulé, l'état envoyé est celui du départ), lévitation,
  grâce du dauphin (physique client), mauvais présage et héros du village (pas de raids),
  résistance au feu (aucune source de feu dans ce serveur), surbrillance (le bit est
  envoyé, rien d'autre). Ils sont portés, affichés, décomptés.
* **La fiole de miel** ne rend pas de fiole vide : lacune préexistante du chemin de repas.
* **Le ragoût suspect** porte ses effets dans sa NBT ; ils ne sont pas lus.
* **L'amplificateur 255 relu** vaut −1 en vanilla (octet signé) et 255 ici ; cela change la
  résistance à ce seul niveau.
* **Potions brassées et balises** : autres mandats.
