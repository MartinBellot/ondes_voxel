# Les mobs, deuxième vague — la marche, les biomes, douze espèces

Ce dossier reprend trois choses que `mobs.md` et `elevage.md` avaient laissées ouvertes : la
vitesse de marche (qui n'est pas « l'attribut divisé par deux »), la liste de mobs d'un seul biome
appliquée partout, et les espèces qui manquaient. **Tout chiffre ci-dessous a été mesuré contre le
vrai serveur 1.20.1** (`tools/vanilla/server.jar`) par `scripts/measure_mobs2.py`, et les preuves de
bout en bout sur notre serveur par `scripts/check_mobs2_e2e.py`. Les relevés bruts vont dans
`data/vanilla/1.20.1/normalized/mobs2_speed.json` et `mobs2_e2e.json` (gitignorés). Ce qui n'est pas
mesuré est dit là où c'est utilisé, et regroupé au § 8.

Le résultat court :

| mesure | vanilla | avant | après |
|---|---|---|---|
| loi de marche | `v = 0,98·s²·(0,6/f)³ / (1 − 0,91·f)` | `v = attribut / 2` | la loi, `walk_speed.hpp` |
| zombie qui poursuit, herbe | 0,1141894 b/t | 0,115 (+0,7 %) | 0,1141894 |
| zombie qui poursuit, glace compacte | 0,1099586 | 0,115 (+4,6 %) | la loi donne 0,1099586 (non appliquée, § 8) |
| zombie qui poursuit, glace bleue | 0,1157442 | 0,115 (−0,6 %) | la loi donne 0,1157442 (non appliquée, § 8) |
| vache qui erre | 0,08632 | 0,100 (+16 %) | 0,08634 |
| creeper qui erre | 0,08633 | 0,125 (+45 %) | 0,08635 |
| araignée qui erre | 0,12429 | 0,150 (+21 %) | 0,12433 |
| vache qui panique | 0,3415 | 0,125 (−63 %) | 0,3453 |
| espèces vivantes | — | 8 | 20 |
| biome des apparitions | celui de la **position** | plains partout | celui de la position |
| monstre en surface, minuit | ≈ 70 au plafond (mobs.md § 4) | **aucun** (seuil « lumière 0 ») | tirage documenté, § 3 |

---

## 1. La vitesse de marche (campagnes `stroll`, `panic`, `chase`, `ice`)

### 1.1 Méthode

Des champs de mobs `PersistenceRequired:1b` — sinon le délai d'inactivité passé 100 éteint le but
d'errance (`mobs.md` § 0) — sur un superplat d'herbe, chacun nommé `CustomName:'"mNN"'` pour que la
réponse d'un `data get` dise de qui elle parle. Un lot de console par échantillon : `time query
gametime` puis la position de chaque mob, **dans le même tick** ; le dénominateur est le temps du jeu.
La vitesse de croisière est le plateau haut des pas : la médiane des pas à moins de 4 % du 90ᵉ
centile. Une errance démarre, tourne et s'arrête, et les trois sont plus lents que la croisière.

* `stroll` : 30 passifs (vache, cochon, mouton, poulet, lapin, cheval), 40 hostiles (zombie, husk,
  noyé, squelette, stray, creeper, araignée, araignée venimeuse, sorcière, enderman), loup + chat,
  renard ; 100 à 110 s chacun, de nuit.
* `panic` : les mêmes passifs, chacun blessé par la sonde toutes les 4 s
  (`damage … by ovprobe` pose le dernier attaquant, ce qui déclenche la panique) et soigné.
* `chase` : quatre mobs d'une espèce à 30 blocs d'une sonde en survie (Résistance V),
  `follow_range` porté à 64 ; loup et enderman provoqués d'un coup de la sonde.
* `ice` : la poursuite du zombie rejouée sur glace compacte, glace bleue, herbe.

### 1.2 La loi

La documentation donne les pièces (minecraft.wiki, *Movement*, *Ice*) : le contrôle de déplacement
d'un mob demande une vitesse `s` = attribut × modificateur du but, et s'en sert **deux fois** — comme
entrée avant du corps et comme accélération qui la multiplie ; l'entrée est amortie de 0,98 par tick
comme celle de toute entité vivante ; le sol de glissance `f` multiplie l'accélération par
`(0,6/f)³` et la vitesse par `0,91·f` à chaque tick. D'où, en régime :

```
a = 0,98 · s² · (0,6 / f)³        v = a / (1 − 0,91 · f)
```

soit `2,15859 · s²` sur un sol ordinaire (`f` = 0,6). Le 2,1586 que l'élevage avait **ajusté** sur
sept vitesses est ce coefficient à 0,002 % près. La moitié de `mobs.md` est la tangente de cette
parabole près de 0,23 : exacte pour le zombie sur lequel elle avait été calée, fausse ailleurs.

**Le témoin qui tranche** — piège 14 du briefing : un bon ajustement sur un seul sol ne dit pas
*pourquoi* la loi est quadratique. La glace, si : la loi prédit une dépendance au sol que « l'attribut
divisé par deux » ne prédit pas du tout.

| sol (`f`) | loi | vanilla (plateau, n) |
|---|---|---|
| herbe (0,6) | 0,1141894 | 0,1141894 (525) |
| glace compacte (0,98) | 0,1099586 | 0,1099586 (568) |
| glace bleue (0,989) | 0,1157442 | 0,1157451 (494) |

Trois sols, trois accords au dix-millième. `walk_speed.hpp` porte la loi, avec la glissance en
paramètre (`walk_blocks_per_tick_on`) et sa forme sur sol ordinaire (`walk_blocks_per_tick`).

### 1.3 Les modificateurs, lus sur le jeu

`s = √(v / 2,15859)`, divisé par l'attribut mesuré (`entities.json`) :

| espèce | attribut | errance (b/t → modif.) | panique | poursuite |
|---|---|---|---|---|
| zombie | 0,23 | 0,11417 → **1,0** | — | 0,11419 → **1,0** |
| husk | 0,23 | 0,11417 → 1,0 | — | 0,11419 → 1,0 |
| noyé | 0,23 | 0,11416 → 1,0 | — | 0,11419 → 1,0 |
| squelette | 0,25 | 0,13488 → 1,0 | — | sans arc : 0,19427 → 1,2 (§ 1.4) |
| stray | 0,25 | 0,13485 → 1,0 | — | sans arc : 0,19407 → 1,2 |
| sorcière | 0,25 | 0,13484 → 1,0 | — | non mesurée (§ 1.4) |
| creeper | 0,25 | 0,08633 → **0,8** | — | non mesurée |
| araignée | 0,3 | 0,12429 → **0,8** | — | non mesurée |
| araignée venimeuse | 0,3 | 0,12426 → 0,8 | — | non mesurée |
| enderman | 0,3 | 0,19412 → 1,0 | — | provoqué : 0,43708 → s = 0,45 |
| vache | 0,2 | 0,08632 → 1,0 | 0,3415 → **2,0** | — |
| cochon | 0,25 | 0,13482 → 1,0 | 0,2094 → 1,25 | — |
| mouton | 0,23 | 0,11415 → 1,0 | 0,1777 → 1,25 | — |
| poulet | 0,25 | 0,13485 → 1,0 | 0,2617 → 1,4 | — |
| cheval | 0,225 | 0,05354 → **0,7** | 0,1570 → 1,2 | — |
| chat | 0,3 | 0,12432 → 0,8 | 0,4293 → 1,5 | — |
| loup | 0,3 | 0,19416 → 1,0 | — | provoqué : 0,19427 → 1,0 |
| renard | 0,3 | 0,19392 → 1,0 | 0,859 → 2,1 (ajusté) | — |
| lapin | 0,3 | 0,13175 → 0,824 (ajusté) | 0,380 → 1,40 (ajusté) | — |

Les modificateurs tombent ronds partout où le mouvement est une marche : 1,0, 0,8, 0,7, 1,25, 1,4, 2,0.
**L'enderman provoqué** court à `s` = 0,45 exactement : son attribut 0,3 plus les +0,15 de son
élan d'attaque, au modificateur 1,0. **Le lapin saute** et **le renard bondit** en panique : leurs
chiffres sont des ajustements à travers la loi, pas des modificateurs de but, et sont donnés comme
tels dans `mob_species.cpp`. La panique est mesurée sous recul (les coups poussent le mob) : tolérance
de 2 % dans le test, contre 0,5 % pour l'errance.

### 1.4 Ce que la poursuite n'a pas pu mesurer

Les traces le disent (`chase`, position de départ → d'arrivée) : zombie, husk, noyé, loup et
enderman ont couru droit sur la sonde ; **creeper, araignée, araignée venimeuse et sorcière ne se
sont pas approchés** — leurs « vitesses de poursuite » sont des errances, et ne sont pas retenues.
Pourquoi ils n'ont pas acquis la sonde à 30 blocs malgré `follow_range` 64 n'est pas élucidé.

**Squelette et stray : piège 32 encore.** Un `/summon` avec NBT saute `finalizeSpawn`, qui est ce
qui leur donne leur arc ; sans arc, le squelette se bat au corps à corps, et un sur quatre a couru
jusqu'à 0,8 bloc de la sonde, à 0,1943 b/t — `s` = 0,3, le modificateur 1,2 du corps à corps. Nos
squelettes tirent (`projectiles.cpp`) : ils gardent le modificateur 1,0 et s'arrêtent à 15 blocs
(`hold_at`), portée du tir déjà écrite là-bas ; **ni l'un ni l'autre n'est mesuré ici**.

### 1.5 Avant / après, les huit espèces de M2

Vitesse de croisière sur herbe, blocs par tick :

| espèce | vanilla | avant (`attribut / 2`) | après (loi × modificateur) |
|---|---|---|---|
| zombie, errance et poursuite | 0,11417 / 0,11419 | 0,115 (+0,7 %) | 0,11419 |
| squelette, errance | 0,13488 | 0,125 (−7 %) | 0,13491 |
| creeper, errance | 0,08633 | 0,125 (+45 %) | 0,08635 |
| araignée, errance | 0,12429 | 0,150 (+21 %) | 0,12433 |
| vache, errance / panique | 0,08632 / 0,3415 | 0,100 / 0,125 | 0,08634 / 0,3453 |
| cochon, errance / panique | 0,13482 / 0,2094 | 0,125 / 0,156 | 0,13491 / 0,2108 |
| mouton, errance / panique | 0,11415 / 0,1777 | 0,115 / 0,144 | 0,11419 / 0,1784 |
| poulet, errance / panique | 0,13485 / 0,2617 | 0,125 / 0,156 | 0,13491 / 0,2644 |

### 1.6 De bout en bout, sur notre serveur

`check_mobs2_e2e.py speed` : quatre mobs de chaque espèce invoqués à la console d'`ov_dedicated`
sur son superplat, 90 s, la sonde lisant les `Update Entity Position` (pas quantifiés à 1/4096) —
même statistique de plateau que la campagne vanilla :

| espèce | vanilla | notre serveur | écart |
|---|---|---|---|
| zombie | 0,11417 | 0,11421 | +0,03 % |
| husk | 0,11417 | 0,11422 | +0,04 % |
| noyé | 0,11416 | 0,11423 | +0,06 % |
| squelette | 0,13488 | 0,13491 | +0,02 % |
| stray | 0,13485 | 0,13491 | +0,04 % |
| sorcière | 0,13484 | 0,13490 | +0,04 % |
| creeper | 0,08633 | 0,08632 | −0,01 % |
| araignée | 0,12429 | 0,12432 | +0,02 % |
| araignée venimeuse | 0,12426 | 0,12430 | +0,03 % |
| enderman | 0,19412 | 0,19432 | +0,10 % |
| vache | 0,08632 | 0,08633 | +0,01 % |
| cochon | 0,13482 | 0,13493 | +0,08 % |
| mouton | 0,11415 | 0,11421 | +0,05 % |
| poulet | 0,13485 | 0,13490 | +0,04 % |
| lapin | 0,13175 | 0,13189 | +0,11 % (glisse, ne saute pas) |
| loup | 0,19416 | 0,19430 | +0,07 % |
| renard | 0,19392 | 0,19431 | +0,20 % |
| chat | 0,12432 | 0,12430 | −0,02 % |
| cheval | 0,05354 | 0,05352 | −0,04 % |

Dix-neuf espèces sur dix-neuf à 0,2 % près, par le vrai chemin de code (buts, `Mob::tick`,
`step_entity`, paquets de mouvement). Avant cette vague le creeper allait à 0,125 (+45 %).

**Le labyrinthe et la poursuite ne bougent pas.** Le labyrinthe juge l'A\* (`test_pathfinding.cpp`,
suite des ouvertures), que la vitesse ne touche pas. La poursuite du zombie passe de 0,115 à
0,11419 : c'est la valeur mesurée par `mobs.md`, désormais **prédite** plutôt que recopiée.

---

## 2. Apparition par biome (campagne `biomes`)

### 2.1 Ce qui est branché

`load_all_biome_spawners` (`natural_spawning.cpp`) lit **chaque** fichier
`worldgen/biome/<nom>.json` du data generator, dans l'ordre du codec de registre — celui des
indices que stocke le chunk —, et les sept catégories de `spawners` avec leurs poids et leurs
tailles de groupe. Au démarrage de notre serveur :

```
natural spawning: 739 entries over 62 biomes, drawn from the biome of each position;
2 surface-slime biomes
```

62 biomes portent au moins une liste (sur 64 dans le codec) ; les deux biomes à slimes de surface
sont ceux du tag `#allows_surface_slime_spawns` (marais, marais de mangroves). `ChunkBiomes` lit
le biome de la position tirée dans le chunk, et le spawner tire le type **dans la liste de ce
biome** (`NaturalSpawner::entries_at`). Un biome sans fichier ne fait rien apparaître : refusé, pas
doté de la liste d'un autre. Les `spawn_costs` ne concernent que deux biomes du Nether
(`soul_sand_valley`, `warped_forest`) : non lus, et hors de l'Overworld que ce serveur fait vivre.

### 2.2 La composition, contre le vrai serveur

*(§ rempli à la mesure.)*

---

## 3. La lumière d'un monstre : un tirage, pas un seuil

`mobs.md` § 5 a mesuré qu'un monstre n'apparaît pas sous une lumière de bloc de 1 : c'est vrai, et
c'est la limite `monster_spawn_block_light_limit` = 0 du type de dimension. Mais le code en avait tiré
« lumière effective ≤ 0 », et à minuit la surface lit 15 − 11 = 4 : **notre serveur ne faisait
apparaître aucun monstre en surface la nuit**, là où la campagne `caps` du vrai serveur en compte
70 sur un superplat. Les husks et les strays, qui exigent le ciel, ne pouvaient donc jamais apparaître.

La règle documentée (minecraft.wiki, *Mob spawning*), avec les constantes du data generator
(`dimension_type/overworld.json` : `monster_spawn_light_level` uniforme 0..7,
`monster_spawn_block_light_limit` 0), dans l'ordre des tirages :

1. la lumière du ciel stockée ne doit pas dépasser un tirage dans 0..31 ;
2. la lumière de bloc ne doit pas dépasser 0 ;
3. la lumière effective ne doit pas dépasser un tirage dans 0..7.

En surface à minuit : 17/32 × 4/8 ≈ 0,27 des tentatives passent ; dans le noir scellé : toutes ; à
midi ou sous une torche : aucune. `spawn_rules.cpp`, `monster_dark_enough` ; la partie que nul tirage
ne sauve (`monster_light_possible`) reste testée **avant** le tirage du type, comme avant.

---

## 4. Ce qu'un type demande à sa position

`spawn_rules.cpp`. Après le tirage du type, dans le biome de la position :

| type | règle | source |
|---|---|---|
| monstres (défaut) | tirage de lumière du § 3 | dimension type |
| husk, stray | tirage, **et le ciel ouvert** | wiki |
| slime | marais (`#allows_surface_slime_spawns`) entre y 51 et 69, ½ × lune, lumière ≤ tirage 0..7 ; ou chunk à slime sous y 40, une fois sur dix | wiki, *Slime* |
| animaux (défaut) | sol dans `#animals_spawnable_on` (herbe), lumière brute > 8 | tags |
| lapin, loup, renard, grenouille, chèvre, champimeuh, perroquet, axolotl | sol dans `#<type>s_spawnable_on` | tags du data generator |

Le « ciel ouvert » est approché par une lumière du ciel de 15 : une vitre laisse passer 15 et le jeu,
qui teste la carte de hauteur, dirait non. Nommé.

Le chunk à slime est la formule documentée (un `Random` Java semé par la graine et la position du
chunk, `nextInt(10) == 0`) ; aucune valeur de référence du jeu n'a été relevée pour la vérifier, et
le test vérifie seulement la fréquence (1000 ± 90 sur 10 000 chunks).

---

## 5. Le slime (campagne `slime`)

Les règles documentées (minecraft.wiki, *Slime*) : taille 1, 2 ou 4 à l'apparition (l'exposant tiré
dans 0..2, une chance de plus en difficile — le multiplicateur spécial, pris à 0) ; boîte
0,5202 × taille (0,5202 mesuré à la taille 1), santé taille², vitesse 0,2 + 0,1 × taille ; à la mort,
une taille > 1 laisse `2 + next_int(3)` slimes de la moitié de sa taille, sur une grille 2 × 2 d'un
quart de la taille. `slime.cpp` porte les règles, `slimes.cpp` la taille de chaque slime par
identifiant réseau, l'indice de métadonnée 16 (entier) et la division ; les enfants naissent au tick
suivant la mort, dans le bloc `mobs-2` de `server.cpp`.

**De bout en bout, notre serveur** (`check_mobs2_e2e.py slime`, 40 slimes invoqués à la console,
tués à la console, les `Spawn Entity` de slime comptés et leur indice 16 lu sur le fil) :

| taille du parent (lue sur le fil) | parents | enfants 2 / 3 / 4 | taille des enfants |
|---|---|---|---|
| 1 | 20 | aucun (20 fois 0) | — |
| 2 | 8 | 3 / 1 / 4 | 1 |
| 4 | 12 | 5 / 3 / 4 | 2 |

Tailles tirées : 20 / 8 / 12 sur 40 (un tiers chacune attendu, 13,3 ; χ² = 4,4 à 2 ddl, p ≈ 0,11 —
le petit échantillon ne distingue rien). L'indice 16 **n'est pas relevé sur un vrai serveur** : il est
pris à la place que le protocole de Mob laisse au premier champ de Slime.

*(Mesure vanilla de la division : § rempli à la mesure.)*

---

## 6. Les espèces

Vingt espèces ont un cerveau (`mob_species.cpp`), contre huit : les huit de M2 et **husk, stray,
noyé, araignée venimeuse, sorcière, enderman, slime, lapin, loup, renard, chat, cheval**. Chacune a
son attribut mesuré (`entities.json`), sa boîte (le registre), ses modificateurs mesurés (§ 1.3),
sa catégorie d'apparition, et ses tables de butin existantes (`entity_loot.ovpack`, déjà branché
par `mob_combat`). Ce que chacune fait, et ce qu'elle ne fait pas encore :

| espèce | fait | chiffre de parité | nommé, pas fait |
|---|---|---|---|
| husk | zombie qui ne brûle pas ; apparaît sous le ciel ouvert | errance 0,11417, poursuite 0,11419 | la Faim au coup |
| stray | squelette ; tire (déjà dans `projectiles.cpp`) | errance 0,13485 | la flèche de Lenteur |
| noyé | zombie qui brûle ; **naît d'un zombie noyé** (§ 6.1) | errance 0,11416, poursuite 0,11419 | l'apparition dans l'eau (règle de l'eau non écrite : il n'apparaît jamais naturellement), le trident |
| araignée venimeuse | araignée | errance 0,12426 | le Poison au coup |
| sorcière | hostile à distance, s'arrête à 10 blocs | errance 0,13484 | les potions |
| enderman | neutre, erre | errance 0,19412 ; provoqué 0,43708 (= s 0,45) | la téléportation, le regard, les blocs portés, la colère |
| slime | tailles 1/2/4, division | § 5 | le déplacement par bonds (le marcheur le fait glisser) |
| lapin | panique | errance 0,13175, panique 0,380 (ajustés) | les bonds, la reproduction, les variantes |
| loup | neutre, erre | errance 0,19416 ; provoqué 0,19427 | l'apprivoisement, la meute, la chasse aux moutons |
| renard | panique | errance 0,19392, panique 0,859 (ajusté) | le sommeil, la chasse, les variantes |
| chat | panique | errance 0,12432, panique 0,4293 | l'apprivoisement, les variantes |
| cheval | panique | errance 0,05354, panique 0,1570 | la monture, l'apprivoisement, les attributs tirés à l'apparition |

### 6.1 Le zombie qui se noie (campagne `drown`)

La règle documentée (minecraft.wiki, *Zombie*) : les yeux dans l'eau pendant 30 s, le zombie se met
à trembler, et 15 s plus tard c'est un noyé ; un husk devient de même un zombie. Le compteur repart
de zéro si les yeux sortent avant les 30 s ; une fois lancée, la conversion va à son terme.
`conversion.cpp` porte la règle (601ᵉ tick : début, 902ᵉ : remplacement, testé), `drowning.cpp` le
compteur par mob, et le bloc `mobs-2` de `server.cpp` remplace le mob à la même place.

*(Mesure vanilla : § rempli à la mesure.)*

Non fait : l'indice de métadonnée « en conversion » qui fait trembler le zombie chez le client (non
relevé sur le fil), la conservation de l'équipement et de la santé à la conversion.

**Un mob hostile ne blesse aucun joueur au corps à corps dans ce serveur** — c'était déjà vrai des
zombies de M2 : `MeleeAttackGoal` compte son temps de recharge, et rien ne transforme le coup en
dégâts. C'est la raison pour laquelle la Faim du husk et le Poison de l'araignée venimeuse ne sont pas
faits : ils n'ont pas de coup où s'accrocher. Nommé ici, pas corrigé.

**Le rendu.** Le client dessine un modèle avec **une** texture par nom de modèle
(`apps/ov_voxel/src/entities.cpp`) ; un husk posé sur le modèle du zombie porterait la peau du
zombie. Les douze nouvelles espèces ne sont donc pas dessinées par notre client — refusées par nom
comme l'enderman l'était déjà (`test_entity_model.cpp`) —, et un client vanilla les voit, puisqu'il
ne reçoit que le type.

---

## 7. Les pièges payés

* **Main a changé le format de `registry.ovpack` pendant la vague** (format 15). Un worktree dont
  le pack est un lien vers main lit un fichier d'un autre format : `test_ov_gameplay` et
  `test_ov_server` plantent au chargement. Régénérer le sien (`python3 tools/ov_datagen/ovpack.py`).
* **`execute if biome` ne répond que sur une position chargée** ; sur une autre il écrit une erreur,
  et une liste qui ne ramasse que les verdicts décale silencieusement toutes les réponses suivantes.
  Première lecture : pureté 0,00 pour les plaines. Chaque test porte maintenant un marqueur.
* **Un joueur spectateur ne compte pas pour l'apparition** ; la sonde des biomes est en créatif.
* **Le message de `/attribute … get`** est « Value of attribute Movement Speed for m3 is 0.2 » : le
  nom de l'attribut contient des espaces.
* **Un port peut être celui d'un autre agent** : un serveur qui ne peut pas se lier attend
  « Done ( » jusqu'à l'échéance, et le script a l'air bloqué.
* **Le piège 32, encore** : un squelette invoqué avec NBT n'a pas d'arc, et poursuit au corps à corps
  à 1,2.

---

## 8. Ce qui n'est pas fait, ou pas mesuré

* **La glissance par bloc** : la loi la porte (`walk_blocks_per_tick_on`), mais `step_entity`
  n'applique que la glissance par défaut ; nos mobs vont sur la glace comme sur l'herbe.
* **La poursuite** du creeper, de l'araignée, de l'araignée venimeuse, de la sorcière, et d'un
  squelette armé : non mesurées (§ 1.4) ; modificateur 1,0 et arrêt à 15 / 10 blocs, documentaires.
* **`follow_parent` (1,1) et `avoid_sun` (1,0)** : non mesurés.
* **Le peuplement à la génération** (`creature_spawn_probability`, les animaux posés quand un chunk
  naît) : non fait, comme avant ; c'est ce qui peuple un monde vanilla d'animaux.
* **La hauteur tirée** : vanilla tire y entre le fond et la carte de hauteur + 1 ; nous tirons sur
  toute la hauteur du monde. La composition dans un biome n'en dépend pas au premier ordre, le
  rythme si.
* **Les espèces qu'une liste de biome nomme sans cerveau ici** (âne, lama, chèvre, panda, perroquet,
  ours polaire, grenouille, champimeuh, tortue, ocelot, villageois zombie…) apparaissent et tombent
  (`FallingMob`), comme le cheval et l'âne des plaines avant cette vague.
* **Le multiplicateur spécial de difficulté** (taille des slimes en difficile) : pris à 0.
* **Le chunk à slime** : formule documentée, aucune valeur de référence du jeu relevée.
