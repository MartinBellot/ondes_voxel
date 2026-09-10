# Projectiles — flèches, arc, arbalète, trident, lancers, squelette

Ce qui suit est établi par **mesure contre le vrai serveur 1.20.1**
(`tools/vanilla/server.jar`, SHA-1 `84194a2f286ef7c14ed7ce0090dba59902951553`),
par `scripts/measure_projectiles.py` (neuf campagnes), puis rejoué **contre notre
propre serveur** par `scripts/check_projectiles_e2e.py`. Les relevés bruts vont dans
`data/vanilla/1.20.1/normalized/projectile_*.json` (gitignorés, régénérables).

Point de départ : `mobs.md` et `entity_physics.hpp` disaient qu'une flèche ne suit pas
le modèle des mobs (résidu 2,6·10⁻³ contre 1,6·10⁻⁶) et qu'aucun projectile n'existait.

| | |
|---|---|
| `src/ov_gameplay/{include/ov/gameplay,src}/projectile.{hpp,cpp}` | le pas physique, les lancers de rayon, les dégâts, l'arc, l'arbalète, le squelette, l'œuf |
| `src/ov_gameplay/tests/test_projectile.cpp` | les transitions mesurées, figées au dernier chiffre |
| `src/ov_server/src/projectiles.{hpp,cpp}` | le branchement : tirer, consommer, faire apparaître, blesser, ramasser |
| `src/ov_server/src/server.cpp` | onze blocs courts `// ── projectiles ──` |

---

## 0. La méthode : un témoin lu dans la même commande

Une flèche en vol n'a pas d'horloge dans son NBT (`life` ne compte qu'au sol). Chaque
campagne invoque donc un **témoin** — une TNT `{Fuse:30000s,NoGravity:1b}` portant le
même tag que les projectiles — et lit tout par **une seule** commande
`execute as @e[tag=m] run data get entity @s`. Le `Fuse` du témoin étiquette chaque
échantillon avec le tick où il a été pris.

On n'ajuste **pas** une trajectoire entière : on ne garde que les paires d'échantillons
**consécutifs** (âge n et n+1), qui donnent directement les transitions
`v_n → v_{n+1}` et `p_n → p_{n+1}`. La naissance du projectile n'entre jamais dans le
calcul, ni la latence de la console.

⚠ **Piège payé** : la première version ne lisait rien — l'expression régulière attendait
un nom sans espace avant « has the following entity data », et le témoin s'appelle
« Primed TNT ». Zéro échantillon, et la campagne se terminait « avec succès ».

---

## 1. Le vol — dix projectiles, jusqu'au dernier chiffre

v₀ = (0,6 ; 0,4 ; −0,3) à y = 150 ; dans l'eau, un bassin de 43 × 11 × 13 muré de
verre. Entre 17 et 106 transitions par projectile.

| projectile | traînée (air) | gravité | traînée (eau) | résidu vertical |
|---|---|---|---|---|
| flèche | 0,9900000095367432 | 0,05000000074505806 | 0,6000000238418579 | 4·10⁻¹⁶ |
| trident | idem | idem | **0,9900000095367432** | 0 |
| boule de neige, œuf, perle | idem | 0,029999999329447857 | 0,800000011920929 (neige) | 2·10⁻¹⁶ |
| bouteille d'XP | idem | 0,07000000029802322 | — | 0 |
| potion jetable | idem | 0,05000000074505806 | — | 4·10⁻¹⁶ |

Trois faits, et chacun change la trajectoire :

1. **Chaque constante est un `float` Java élargi** : `0.99F`, `0.05F`, `0.03F`, `0.07F`,
   `0.6F`, `0.8F`. Avec les doubles 0,99 et 0,05, la première transition est déjà fausse
   au huitième chiffre.
2. **La position avance de l'ancienne vitesse** : résidu 1,3·10⁻¹⁴ contre 0,053 avec la
   nouvelle.
3. **La gravité vient après la traînée** : la régression `v'_y = a·v_y + b` donne
   `b = −g` et non `−a·g`. C'est l'inverse du mob (gravité avant la traînée), et c'est
   exactement ce qui faisait le résidu de 2,6·10⁻³ de `mobs.md`.

L'ordre est donc : *déplacer, traîner, tirer vers le bas*. Le trident traîne comme une
flèche dans l'air mais **0,99 dans l'eau** — c'est pourquoi il nage.

## 2. Au sol — planté, puis 1200 ticks

Une flèche lâchée sur l'herbe et une autre tirée à plat dans un mur de pierre :

| | position au repos | `Motion` gardé |
|---|---|---|
| sol, y = −60 | **−59,94999999925494** = −60 + `0.05F` | (0 ; −1,0294762709270207 ; 0) |
| mur, x = 10 | **9,950088916881048** | (1,1058299946022032 ; −0,11603773069454587 ; 0) |

Le recul est `0.05F` le long de la dernière course (et non 0,05 : l'écart de 7,4·10⁻¹⁰
est dans la mesure). Le `Motion` gardé est **la course du dernier tick, traînée puis
tirée vers le bas** comme n'importe quel tick — notre simulation retombe sur les deux
triplets au dernier chiffre. Il ne change plus ensuite, tick après tick.

`life` vaut 0 au tick de l'impact et monte d'un par tick ; la flèche a disparu entre les
âges 1202 et 1207 alors que le dernier `life` lu était 1198–1199 : **retrait à 1200**, à
l'écart d'échantillonnage près. Le trident fait comme la flèche ; la boule de neige
casse au mur (disparue au tick 3).

## 3. Les dégâts — un tick, une vitesse, une vache neuve

Chaque flèche est invoquée à un dixième de bloc de la boîte (grossie de 0,3) d'une
vache neuve `{NoAI, 1024 PV}`, donc touche **au premier tick à vitesse connue**.

| vitesse | base | PV perdus | `ceil(v × base)` |
|---|---|---|---|
| 1,0 | 2 | 2 | 2 |
| 1,3 | 2 | 3 | 3 |
| 1,9 | 2 | 4 | 4 |
| 2,6 | 2 | 6 | 6 |
| 3,0 | 2 | 6 | 6 |
| 1,3 | 3,5 | 5 | 5 |
| 0,7 | 0,5 | 1 | 1 |

**7 sur 7.** 24 flèches critiques à 3,0 : **6 à 10, les cinq valeurs présentes** (7, 3,
6, 4, 4 fois) — `6 + nextInt(6/2 + 2)`. Le trident : **8** à 2,5 comme à 0,5 (la vitesse
n'entre pas), et il reste en vie après le coup (il rebondit). Boule de neige : **0** à
une vache et elle ne bouge pas (pas de recul) ; **3** à un blaze. Œuf : 0.

## 4. L'arc — un joueur sonde qui tire

Le client sonde (`Archer`) envoie Use Item (0x32), attend t ticks, puis Player Action
statut 5. Vitesse lue sur le **Spawn Entity** (le NBT est lu quelques ticks trop tard et
porte déjà la traînée) :

* **26 tirs de t = 3 à 30 : tous à `3 × puissance(t)` à 1,6 % près** au pire, sous le
  ±1,7 % que la dispersion permet. `puissance = (f² + 2f)/3`, `f = t/20`, bornée à 1, en
  `float`. **t = 2 ne tire pas** (0,07 < 0,1).
* **Critique** à partir de t = 20, jamais avant. `damage: 2.0d` toujours.
* **Départ** à `yeux − 0.1F` : y = −58,479999996721745 pour des pieds à −60.
* **Consommation** : 64 → 63 → … une flèche par tir en survie ; Infinité : 5 avant, 5
  après, `pickup: 2b` ; créatif : 5 avant, 5 après, `pickup: 2b`, et **tire même sans
  flèche** ; survie sans flèche : **rien**. Survie normale : `pickup: 1b`.
* **Portée** à tangage 0, pleine charge, depuis 1,52 au-dessus du sol : **22,65 blocs**.
* **Ramassage** : la sonde posée sur la flèche plantée → Take Item Entity
  `[flèche, joueur, 1]`, inventaire 45 → 46.
* **Contre une vache à 2,5 blocs** : t = 5 → 2, t = 10 → 3, t = 15 → 5, t = 20 → 9, 11,
  7, t = 25 → 8, 10, 8. Le 11 demande |v₀| > 3 : la dispersion le permet (6 → 7 avant le
  critique).

**Dispersion.** 40 tirs pleine charge, écart transverse `vz/vx` (que la traînée ne
modifie pas) : max 0,0161, écart-type 0,0079. Compatible avec le tirage triangulaire de
demi-largeur 0,0172275 que le code implémente (écart-type 0,0070, borne jamais
franchie) ; une gaussienne d'écart-type 0,0075 franchirait la borne dans 2 % des tirs,
0,8 tir attendu sur 40. **Indicatif, pas décisif.**

### ⚠ Le champ `data` de Spawn Entity est l'id du tireur, pas id + 1

Le mandat, d'après la documentation, annonçait « id du tireur + 1 ». Mesuré : la flèche
du premier joueur arrive avec `data = 1`, et le Take Item Entity de la même campagne
nomme ce joueur **1**. Dans la campagne arbalète, `data = 37` pour un joueur d'id 37.
Une flèche invoquée sans tireur : `data = 0`. C'est ce que notre serveur envoie.

## 5. L'arbalète

Relâchée à t = 20 à 24 : **non chargée** ; à t = 25 à 35 : **chargée** (deux essais
par valeur). Seuil : **25 ticks**. Le NBT, lu tel quel :
`{ChargedProjectiles: [{id: "minecraft:arrow", Count: 1b}], Damage: 0, Charged: 1b}`, et
après le tir `ChargedProjectiles: [], Charged: 0b`. Tir : |v| ≈ 3,18 sur le Spawn
Entity (3,15 + dispersion), **toujours critique**, `ShotFromCrossbow`, `pickup: 1b`,
index 8 = **5** (0x01 | 0x04). Usure : `Damage: 4` après quatre tirs.

## 6. Œufs, perle

* **6400 œufs**, un par cellule de verre, poussins comptés par cellule : **5602 × 0,
  776 × 1, 22 × 4, jamais 2 ni 3**. Éclosion 798/6400 = 0,1247 (1/8) ; quatre parmi les
  éclosions 22/798 = 0,028 (1/32 = 0,031, espérance 24,9 — à 0,6 écart-type).
* **Perle** : trois lancers, **20 → 15** trois fois, téléporté à ~42 blocs.
  ⚠ La première campagne lisait 17,5 / 15,8 / 15,2 : la régénération naturelle d'un
  joueur rassasié soignait pendant les quatre secondes d'attente. Refaite avec
  `naturalRegeneration false`. Aucune endermite sur six lancers — le 5 % n'est pas
  mesuré.

## 7. Le squelette

Un squelette à 11 blocs de la sonde en survie, 20 s par difficulté, arcs comptés sur les
Spawn Entity :

| difficulté | flèches | intervalle (ticks) | vitesses au départ |
|---|---|---|---|
| facile | 7 | 60, 60, 60, 60, 60, 60 | 1,26 … 1,71 |
| normal | 7 | **60** × 6 | 1,54 … 1,68 |
| difficile | 10 | **40** × 9 | 1,57 … 1,65 |

Intervalle 60 (facile, normal), 40 (difficile). L'étalement des vitesses rétrécit avec
la difficulté comme l'imprécision `14 − 4 × difficulté` le prévoit autour de 1,6.

## 8. Les métadonnées — un champ à la fois

| entité | index | type | NBT qui le fait bouger |
|---|---|---|---|
| flèche | **8** | octet, bit 0x01 | `crit:1b` |
| flèche | **8** | octet, bit 0x04 | `ShotFromCrossbow:1b` |
| flèche | **9** | octet | `PierceLevel:3b` → 3 |
| flèche | **10** | VarInt | `Color` → 16711680 ; `Potion:"poison"` → 8889187 |
| trident | **10** | octet | Loyauté III → 3 |
| tous | 5 | booléen | `NoGravity:1b` |

Rien d'autre n'est envoyé pour une flèche, une boule de neige, un œuf, une perle, une
bouteille ou une flèche spectrale de base. Un trident invoqué avec un objet enchanté
(Tranchant) **n'envoie pas** d'index 11 : l'éclat ne vient pas du NBT invoqué — non
tranché ici.

---

## 9. Ce que fait notre serveur, et ce qu'il ne fait pas

**Fait** : physique des huit sortes, rayon contre les formes de collision par état et
contre les boîtes des entités et des joueurs (grossies de 0,3), flèche plantée, 1200
ticks, ramassage (`pickup` 0/1/2, survie oui, créatif « créatif seulement »), arc (charge,
puissance, critique, dispersion triangulaire, consommation, Infinité, usure), arbalète
(25 ticks, Quick Charge, NBT `Charged`/`ChargedProjectiles`, tir à 3,15 critique),
trident (10 ticks, 2,5, 8 dégâts, retombée), boule de neige (3 au blaze), œuf (1/8, 1/32
de quatre), perle (téléportation + 5), bouteille (orbe), squelette et stray (intervalle
60/40, imprécision selon la difficulté), paquets : Spawn Entity (`data` = tireur),
index 8/9, Damage Event (`minecraft:arrow`/`trident`, cause = tireur, direct = flèche),
Take Item Entity.

**Nommé, non fait** (`gameplay::projectile_gaps()`, journalisé au démarrage) :

* perçage (la flèche s'arrête au premier coup), multishot (une flèche), Flamme et flèches
  enflammées (pas de feu sur les entités), effets des flèches à pointe et de la flèche
  spectrale (le NBT de potion n'est pas relu ; la spectrale vole et ne fait pas briller),
  Loyauté, Impulsion, Canalisation, endermite de la perle, effets des potions jetables,
  une flèche dont le bloc disparaît ne retombe pas, les flèches plantées dans un mob ne
  s'affichent pas ;
* l'œuf fait éclore un poulet **adulte** (l'indice « bébé » n'est pas mesuré) ;
* la bouteille lâche **une** orbe de 3 + 0..4 + 0..4 (wiki, non mesuré) ;
* le serveur n'a pas de réglage de difficulté : ses squelettes tirent en **normal** ;
  leur première flèche après 20 ticks de vue et la portée de 15 viennent du wiki ; leur
  arc n'est pas montré en main ; leurs flèches portent la base 2,0 (vanilla y ajoute un
  terme de difficulté, non mesuré) ;
* le recul d'une flèche reprend l'impulsion 0,4 du corps à corps le long de la course,
  **non mesuré** pour une flèche ; Frappe (0,6 par niveau) et Puissance (0,5 × niv +
  0,5) viennent du wiki ;
* le « shake » de 7 ticks avant ramassage, les durabilités max de l'arc (384), de
  l'arbalète (465) et du trident (250) viennent du wiki ;
* le temps de recharge de la perle (20 ticks) est envoyé au client mais pas imposé ;
* un tireur accroupi tire depuis la hauteur des yeux debout.

## 10. De bout en bout, contre notre serveur

`scripts/check_projectiles_e2e.py` : `ov_dedicated`, une vraie connexion, rien d'appelé à
côté du protocole — la sonde envoie Use Item puis Player Action 5 comme un client.

**Avant** (le binaire de `main`, même script) : **0** flèche, 0 boule de neige, 0 œuf,
arbalète jamais chargée, 0 trident, pas de téléportation. **Après** :

| scénario | ce que la sonde a lu |
|---|---|
| créatif, arc plein vers la vache à 3 blocs | Spawn Entity `arrow`, `data = 1` (la sonde), |v| = 3,010, départ y = −58,479999996721745 (celui de vanilla au dernier chiffre), index 8 = 1 ; Damage Event type `arrow`, cause 1, direct = la flèche ; vache 10 → 3 PV ; flèche retirée |
| créatif, flèche plantée à ses pieds | Take Item Entity `[flèche, 1, 1]`, retirée (« créatif seulement ») |
| boule de neige | |v| = 1,483, cassée au sol |
| 32 œufs | 32 lancés, 10 poussins (espérance 4,2 ; au lancer précédent, 6) |
| arbalète | Set Container Slot avec `Charged` après 30 ticks ; tir à 3,171, index 8 = 5, puis 4 à l'impact |
| trident | 2,515, ramassé |
| perle vers +x | Synchronize Player Position à x = 44,4 |
| survie, arc + 8 flèches écrits dans `playerdata/<uuid>.dat` | 8 → **7** après le tir, `Damage: 1` sur l'arc, vache blessée ; flèche plantée : **6 → 7** au ramassage ; huit tentatives avec sept flèches : **7** tirs |
| survie, un squelette à 3 blocs | 6 flèches en 16 s, intervalles **59,9 · 59,9 · 60,1 · 60,0 · 60,1** ticks (60 mesuré en normal), vitesses 1,53 … 1,63, `data` = le squelette ; santé de la sonde 20 → 16 → … → 8,3 |

Les 10 poussins sur 32 œufs sont hauts : espérance 4,25, écart-type 2,3, donc 2,5
écarts-types ; sur les deux passages, 16 pour 64 œufs, 2,3 écarts-types. Le serveur tire
d'une graine fixe, donc ce sont toujours à peu près les mêmes tirages. **La loi elle-même
n'est pas jugée sur ce banc** : elle est vérifiée sur 256 000 tirages dans
`test_projectile.cpp` (7/8 et 1/256 à 0,5 % et 0,08 % près) — un écart de ce banc serait
à reprendre avec plus d'œufs, pas à expliquer.

⚠ **Deux défauts trouvés ici, invisibles aux tests unitaires.**

1. **Le squelette ne tirait jamais.** La liste des cibles n'était construite que s'il
   existait déjà un projectile — et c'est dans cette liste que le squelette cherche ses
   joueurs. Aucune flèche, donc aucune cible, donc aucune flèche : 0 en 16 s. Corrigé.
2. **La flèche « plantée » n'était pas ramassée** au premier passage : la vache blessée
   par le premier tir paniquait et courait devant la sonde, et le second tir, vers le sol
   devant elle, la touchait. Le journal du serveur disait `hit 1, stuck 0`. Le banc tire
   maintenant vers l'arrière. Et le passage « avant » écrasait le journal du passage
   « après » (même nom de monde) : il porte désormais un suffixe.

## 11. Pièges

1. **Le nom d'une entité a des espaces** (« Primed TNT », « Thrown Ender Pearl ») : une
   regex `\S+` ne lit rien et la campagne finit « avec succès ».
2. **La vitesse d'un tir se lit sur le Spawn Entity**, pas sur le NBT relu : 0,15 s plus
   tard, trois ticks de traînée l'ont déjà ramenée de 3,0 à 2,86.
3. **La régénération ment sur les dégâts** d'un joueur rassasié lus en différé.
4. **Une regex d'inventaire compte aussi `ChargedProjectiles`** : l'arbalète chargée
   semblait n'avoir consommé aucune flèche.
5. **`data` = tireur, pas tireur + 1.**

## 12. Reproduire

```bash
python3 scripts/measure_projectiles.py flight ground damage metadata
python3 scripts/measure_projectiles.py bow crossbow pearl skeleton eggs 16
ctest --preset macos-debug -R test_ov_gameplay       # [projectile]
python3 scripts/check_projectiles_e2e.py creative survival skeleton
```

Port 25631 pour vanilla (`OV_PROJ_PORT`), 25633 pour notre serveur
(`OV_PROJ_E2E_PORT`). Chaque campagne efface son monde en partant.
