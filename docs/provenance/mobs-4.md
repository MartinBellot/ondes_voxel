# Les mobs, quatrième vague — les effets de statut portés par un mob

> **État : branché, en partie mesuré.** Les campagnes `packets`, `splash`,
> `anvil` et les quatre espèces ont tourné sur le vrai serveur 1.20.1
> (`scripts/measure_hostile.py`, relevés dans `.scratch/hostile.json`, non
> suivi). `speed`, `strength` et `arrow` sont **à refaire** (§ 2.3). Ce qui
> n'est pas mesuré est marqué *en attente*.

Les règles des 33 effets existent depuis la vague des effets (`effets.md`,
`effects.hpp`) et un mob implémentait déjà `EffectTarget` dans les tests. Ce qui
manquait au serveur, c'était **une place où garder les effets d'un mob** entre
deux ticks. `effets.md` § 16 le disait : « les mobs ne portent pas d'effets côté
serveur ».

---

## 1. Ce qui est branché

### 1.1 Le corps d'une espèce (`mob_body.{hpp,cpp}`, `ov_gameplay`)

`effect_body(type)` range une espèce parmi les morts-vivants, les arthropodes
ou les ordinaires. Trois espèces sont **mesurées** par `effect give` des 31
effets non instantanés (`effets.md` § 7) : zombie et squelette refusent
régénération et poison, l'araignée refuse le poison. Les autres viennent des
catégories *Undead* et *Arthropod* du wiki (1.20.1).

### 1.2 La marche sous vitesse et lenteur

La loi de marche est `v = 2,15859 · s²` (`mobs-2.md` § 1), `s` l'attribut
`movement_speed` fois le modificateur du goal. Vitesse et lenteur agissent **sur
l'attribut** (modificateur `multiply_total`, `effets.md` § 3) : un mob dont
l'attribut vaut `k` fois sa base marche `k²` fois plus vite, quel que soit le
goal. `walk_factor(valeur, base) = (valeur / base)²` : vitesse I → 1,44,
vitesse II → 1,96, lenteur I → 0,7225. `MobBrain::effect_walk` le porte, et
`Mob::tick` le multiplie dans la vitesse de flânerie comme de poursuite.

**Mesuré** (campagne `speed`, cinq zombies par case, 110 s, effet caché infini
donné par `effect give`) :

| case | croisière (b/t) | rapport au témoin | prédit | `movement_speed` lu |
|---|---|---|---|---|
| témoin | 0,11413 | 1 | 1 | 0,23000000417 |
| vitesse I | 0,16432 | **1,4397** | 1,44 | 0,27600000569 |
| vitesse II | 0,22365 | **1,9596** | 1,96 | 0,32200000721 |
| lenteur I | 0,08249 | **0,7228** | 0,7225 | 0,19550000218 |
| vitesse I en NBT | 0,11416 | **1,0003** | — | 0,23000000417 |

Le carré tient à 0,04 % près. La dernière ligne prouve § 2.3 : un effet lu
depuis le NBT de `summon` ne porte pas son modificateur.

### 1.3 La table des effets d'un mob (`mob_effects.{hpp,cpp}`, `ov_server`)

Sur le modèle des fenêtres de `MobCombat` : une ligne par mob **qui porte un
effet**, indexée par l'identifiant réseau, supprimée quand le dernier effet
s'en va. Un mob sans effet ne coûte rien. La table est ordonnée (`std::map`) :
l'ordre des lignes est celui des coups mis en file, et les morts tirent leur
butin d'un même générateur dans cet ordre — une table de hachage l'aurait fait
dépendre de la bibliothèque standard.

Chaque ligne :

| quoi | comment |
|---|---|
| dégâts d'un effet (poison, wither, dégâts instantanés) | par la **fenêtre du mob** (`MobCombat`), résistance d'abord ; `MobCombat::hurt` prend désormais le type de dégât |
| soin | écrit sur l'entité, borné par le maximum des attributs |
| absorption | dans la fenêtre du mob, où les règles de dégâts la prennent |
| vigueur | `max_health` recopié sur l'entité ; bornage à la fin (18/18 → 10/10) |
| vitesse, lenteur | `effect_walk` dans le cerveau |
| force, faiblesse | `attack_damage` modifié, lu par `MobAttacks` à chaque coup |
| métadonnées | indices 10 (couleur), 11 (tous ambiants), 0 (bits 0x20 et 0x40), 9 (santé) à tous les clients, mesurés sur une vache (`effets.md` § 10) |
| sauvegarde | `ActiveEffects` dans le fichier d'entités, au format du fichier joueur (`effets.md` § 13) ; une liste périmée venue du disque est retirée |

`MobCombat::hurt` réensemence désormais la fenêtre depuis la santé de l'entité
**à chaque coup** : un mob soigné par la régénération depuis son dernier coup
aurait sinon retrouvé la santé d'avant le soin.

### 1.4 Qui atteint un mob

* **`/effect give` et `/effect clear`** sur un mob (`@e[type=cow]`), nommé par
  son type dans la réponse ; une entité qui ne porte pas d'effet (un objet, une
  flèche) n'est simplement pas comptée, comme en vanilla.
* **Potions jetables, nuages persistants, flèches trempées et spectrales** : la
  liste de `PotionHost::players` contient maintenant les mobs vivants du monde
  de l'overworld, chacun avec **sa** boîte (demi-largeur, hauteur), et `affect`
  résout l'identifiant vers un joueur ou un mob.
* **La mort par un effet** : le coup qui tue est mis en file ; le serveur envoie
  le Damage Event, puis la mort telle que `/kill` la joue (animation, cri, butin
  sans joueur, découpe du slime). Un coup qui ne tue pas : Damage Event et cri
  de douleur.
* **Les coups des joueurs** passent par la résistance du mob avant sa fenêtre.

---

## 2. Les campagnes (`scripts/measure_hostile.py`)

### 2.1 Ce que voient les observateurs d'un mob (`packets`) — mesuré

Une vache, une sonde qui la regarde, sept gestes, puis un second client qui
arrive :

| geste | paquets reçus pour la vache |
|---|---|
| vitesse II | métadonnée 10 = 0x33EBFF, puis **Update Attributes** (`movement_speed`) |
| poison caché | la santé seule (9, puis 8 : le premier coup tombe **tout de suite**, 600 étant multiple de 25) |
| vitesse retirée | Update Attributes, puis 11 = vrai et 10 = 0 (il ne reste qu'un effet caché) |
| tout retiré | 11 = faux |
| invisibilité | 0 = 0x20 et la couleur |
| surbrillance | 0 = 0x60 et la couleur mêlée |
| tout retiré | 0 = 0 et 10 = 0 |
| second client qui arrive | la couleur dans les métadonnées d'apparition, et Update Attributes avec le modificateur |

**Jamais d'Entity Effect ni de Remove Entity Effect** pour un mob, ni en
direct ni à l'arrivée. **Update Attributes, oui**, pour `movement_speed` : c'est
ce que `MobEffects` envoie désormais, en direct et dans `pairing`, après les
bases de l'apparition. Un poison seul n'en envoie pas : la ligne neuve efface
les drapeaux « sale » de ses attributs. L'ordre des champs dans une même
métadonnée (vanilla 11 avant 10) n'est pas reproduit ; le client ne le lit pas.

### 2.2 Le jet (`splash`) — mesuré

Poison I jeté à la verticale de vaches alignées tous les 0,5 bloc : durées
788 → 89 ticks de 0 à 3,5 blocs (lus ≈ 12 ticks après l'impact), rien à 3,9
et 4,4. La pente est de 0,25 par
bloc : `1 − r/4`, `r` la distance de l'impact aux **pieds** de la vache
(décalage vertical ≈ 0,44), et rien sous `kSplashMinimumTicks`.

Les instantanés, au point près dans les 40 cases, suivent
`(int)(proximité × base + 0,5)` avec `base = 4 << amp` pour soigner et
`6 << amp` pour blesser, mort-vivant inversé : dégâts instantanés sur des
zombies 14/13/13/12/12/11/11/10, soin instantané sur des zombies et dégâts
instantanés sur des vaches 5/5/6/6/7/8/9/9, dégâts instantanés II sur des
zombies 17/17/16/15/14/13/12/11. C'est ce que fait déjà `apply_instant`.

### 2.3 Le piège du NBT : `speed` refait, `strength` et `arrow` à refaire

Au premier passage, les effets de `speed` et `strength` avaient été donnés dans
le NBT de `summon` (`ActiveEffects`). Toutes les cases ont marché à
0,11417 b/t et frappé à 3,0, **comme le témoin**. Refait par `effect give`,
`speed` donne le carré (§ 1.2) ; la case gardée en NBT reste au témoin
(1,0003) avec un attribut **sans modificateur** : un effet lu depuis le NBT ne
rapporte pas son modificateur — vanilla le garde dans `Attributes`.

**Force et faiblesse** (refait par `effect give`, zombie contre une sonde en
survie, normal) : force I → **6,0** par coup, force II → **9,0**, témoin
**3,0** — `+3` par niveau sur `attack_damage` — et faiblesse I → **aucun coup**
en 14 s : `3 − 4` borné à 0, le coup n'a rien à porter. C'est ce que fait
`MobAttacks` avec l'attribut modifié : un coup à 0 n'est pas porté.

**Flèches** : toujours aucune vache touchée. La flèche finit ≈ 0,7 au-dessus
de sa tête, santé 10,0 : elle a **rebondi**, alors que la même chute blesse le
zombie (7,06, sans poison : mort-vivant). La cause n'est pas établie ; la
règle des flèches trempées sur un mob reste **non mesurée**, et notre code y
applique celle mesurée sur les joueurs (÷ 8).

La lecture d'attribut de `measure_mobs2.py` rendait `None` : la réponse dit
« for **entity** m1 », que son motif n'attendait pas. Corrigé ; les valeurs
ci-dessus viennent des réponses brutes gardées par la campagne.
`arrow` n'a touché **aucune** vache (santé 10,0) mais le zombie (7,06, sans
poison : mort-vivant) : elle note maintenant où la flèche et le mob ont fini.

Notre serveur relit `ActiveEffects` **avec** les modificateurs
(`load_effects`) : sur un monde sauvé par vanilla, qui porte aussi
`Attributes`, le résultat est le même ; seul un `summon` avec effets en NBT
diffère, et notre `/summon` refuse le NBT.

### 2.4 De bout en bout

Contre notre serveur, `scripts/check_hostile_e2e.py`, **4/4** :

* `effects` : vitesse II → indice 10 = 0x33EBFF ; invisibilité → bit 0x20 ;
  `effect clear` → 0 et 10 à 0.
* `poison` : une vache sous poison I 100 ticks passe par **9, 8, 7, 6** — la
  table périodique mesurée (`effets.md` § 4).
* `undead` : soin instantané IV tue le zombie (son `Remove Entities` arrive) ;
  l'autre passe à **14** sous soin instantané I puis **18** sous dégâts
  instantanés I.
* `anvil` : le monde sauvé par vanilla, servi par nous — la vache sous
  régénération II envoie 0xCD5CAB, celle sous vitesse III 0x33EBFF, le zombie
  sous force et résistance au feu leur mélange 0xFFB000, l'araignée sous
  invisibilité sans particules **rien** (aucun effet visible).

**L'aller-retour — mesuré.** Vanilla sauve un zoo sous effets, notre serveur le
relit, le sert, le réécrit (`save-all`), et vanilla relit ce que nous avons
écrit (`measure_hostile.py anvil_back`) :

| mob | sauvé par vanilla | relu par vanilla dans notre réécriture |
|---|---|---|
| vache | régénération II, 39 956 | régénération II, 39 321 |
| vache | vitesse I, 59 956 | vitesse I, 59 322 |
| zombie | résistance au feu 49 956 + force infinie | résistance au feu 49 323 + force infinie |
| araignée | invisibilité, 44 956 | invisibilité, 44 324 |

Identifiants, amplificateurs et durée infinie intacts ; chaque durée finie a
perdu ≈ 634 ticks, le temps que les effets ont couru chez nous entre les deux
lectures. La première relecture cherchait chaque mob à deux blocs de son point
d'apparition et n'en trouvait aucun : le zoo avait marché, notre serveur
ignorant `NoAI` (`mobs-3.md` § 6). Elle prend maintenant tous les mobs marqués.

---

## 3. Les quatre espèces suivantes — mesuré, non implémenté

Les nombres du wiki servaient d'hypothèses ; voici ce que le vrai serveur a
fait. Ils fixent ce que les prochaines vagues doivent atteindre.

* **Enderman.** Une sonde en survie qui regarde ses yeux à 8 blocs (lui dans
  une fosse de 2, pour qu'il ne sorte pas du regard) : `AngryAt` = la sonde dès
  la première lecture, dix fois sur dix ; regard détourné : jamais. Dans l'eau :
  **1** de dégâts, puis un téléport de ≈ 15 blocs, puis la marche — un seul
  téléport en 10 s.
* **Araignée.** Aucun coup à midi ; à minuit, 2,0 tous les **20** ticks
  (normal).
* **Slime.** Au contact, en normal : taille 0 → rien, taille 1 → **2,0** (le
  wiki dit 3), taille 3 → 4,0, coups espacés d'au moins 10 ticks (la fenêtre).
  Sans cible, les sauts partent tous les ≈ 32 à 70 ticks, les trois tailles
  pareil.
* **Sorcière.** À 2,5 blocs : poison d'abord, puis dégâts instantanés (6) ;
  empoisonnée par son propre jet, elle boit un soin. À 9 blocs : des coups de 2
  (des dégâts instantanés tombés court, proximité ≈ 0,3), une lenteur tard, et
  elle boit une vitesse tôt.

---

## 4. Ce qui n'est pas fait, ou pas mesuré

* **`max_health` et `attack_damage`** d'un mob ne partent pas en Update
  Attributes : seul `movement_speed` a été vu sur le fil (§ 2.1).
* **Les mobs du Nether** vivent dans un monde d'entités à part
  (`nether_mobs.cpp`) et ne portent pas d'effets.
* **L'âge d'un effet infini** se compte depuis la création de la ligne, pas de
  l'entité : la phase de l'intervalle d'une régénération ou d'un poison infinis
  peut différer de vanilla, pas son rythme.
* **La phase de la fenêtre** : les fenêtres des mobs avancent pendant la phase
  des joueurs, les effets des mobs agissent avant le tick des entités ; seul un
  effet qui frappe à moins de dix ticks d'un autre coup le verrait.
* **Le bit « en feu »** de l'indice 0 n'est pas transmis à `set_other_flags` :
  donner l'invisibilité à un mob qui brûle éteint un instant sa flamme chez le
  client, jusqu'au prochain envoi du feu.
* Les **attributs** d'un mob (`Attributes`) ne sont ni lus ni écrits ; les
  modificateurs des effets sont remis par `load_effects`, comme pour le joueur.
