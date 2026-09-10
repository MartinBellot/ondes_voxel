# Explosions — les rayons, la résistance, le cratère

Ce qui suit est établi par **mesure contre le vrai serveur 1.20.1**
(`tools/vanilla/server.jar`, SHA-1 `84194a2f286ef7c14ed7ce0090dba59902951553`),
par `scripts/measure_blast.py`. Les résultats bruts vont dans
`data/vanilla/1.20.1/normalized/blast_*.json` (gitignorés, régénérables) ; la
table retenue entre dans le `.ovpack` et est relue par
`registry::BlockRegistry::blast_resistance`. Les règles vivent dans
`src/ov_gameplay/{include/ov/gameplay,src}/explosion.{hpp,cpp}` et sont rejouées
par `src/ov_gameplay/tests/test_explosion.cpp`.

Le point de départ de ce mandat est un refus, écrit dans `redstone.md` § 14 par
l'agent qui avait mesuré la mèche de la TNT :

> « la résistance au souffle n'est dans aucun rapport du data generator, et la
> déduire des formes demanderait une boîte par bloc, soit 987 boîtes. »

Le refus était juste. Ce document est la campagne qui le lève.

---

## 0. Ce qu'un oracle peut et ne peut pas dire ici

Une explosion **tire au sort**. L'énergie de chacun de ses 1352 rayons vaut
`puissance × (0,7 + 0,6 × aléa)`, tiré par rayon. Deux charges identiques au
même endroit dans la même boîte ne font pas le même cratère — ni chez eux, ni
chez nous. **Aucune comparaison coup par coup n'a de sens.**

Ce qui se compare, et ce que ce document compare :

- la **fréquence** de destruction de chaque cellule, sur K tirs ;
- l'**union** des K tirs : toute cellule qu'un tir a prise ;
- l'**intersection** : les cellules que les K tirs ont toutes prises.

L'union et l'intersection convergent, quand K grandit, vers deux formes nettes —
le cratère du rayon le plus chanceux et celui du moins chanceux. Ce sont elles
qui font une preuve cellule par cellule.

⚠ **Le piège du `/fill` sous le monde.** Un `fill` dont un coin descend sous
`y = −64` échoue **en entier**, sur une ligne de console que personne ne lit, et
chaque cellule se relit alors « pas le matériau » — ce qui est exactement
l'apparence d'une destruction totale. Ce piège a déjà coûté une campagne ici
(voir `redstone.md` § 14). Tout `y` de `measure_blast.py` passe par `check_y()`,
qui refuse et dit pourquoi ; le banc travaille à `y = −40`, vingt et un blocs
au-dessus du sol du superflat, donc rien de ce qui est mesuré ne touche le
terrain d'origine.

---

## 1. Le banc de résistance : une rangée, pas une boîte

L'idée qui rend la campagne faisable. La résistance décide de **la profondeur à
laquelle un rayon s'arrête**. Il suffit donc, par bloc, d'une **rangée** :

```
     y        [charge]  B B B B B B B B B        <- neuf cellules du bloc
     y−1      sol       sol …                    <- barrière, ou le substrat
     y−2      barrière  barrière …               <- tient les sols qui tombent
```

La rangée est **radiale** : pour atteindre la k-ième cellule, un rayon doit
traverser les k−1 précédentes, parce qu'un rayon parti du centre et penché sort
de la rangée et n'y revient jamais. Ce qui remonte est donc une profondeur de
pénétration pure, et rien d'autre.

La charge est une TNT amorcée invoquée avec `{Fuse:0,NoGravity:1b,Motion:[0,0,0]}` :
son centre est un point fixe, pas l'endroit où une entité a fini par tomber.

**987 cellules dans un seul monde**, 46 colonnes de 20 blocs, 22 rangées de 12,
soit 812 chunks forceloadés. Une passe = reconstruire les 959 rangées, invoquer
959 charges, attendre l'horloge **du serveur** (pas la nôtre : mille explosions
dans un tick font un tick de plusieurs secondes), sauver, relire 8 631 cellules.
Seize passes.

### Ce que la rangée refuse

| | blocs | pourquoi |
|---|---|---|
| se sont tenus debout sur barrière | 910 | |
| + après reprise sur terre | 959 | |
| **n'ont jamais tenu** | **28** | nommés ci-dessous |

Les 28 : `attached_melon_stem`, `attached_pumpkin_stem`, `big_dripleaf_stem`,
`brain_coral`, `brain_coral_block`, `brain_coral_fan`, `brain_coral_wall_fan`,
`cactus`, `cave_vines`, `cave_vines_plant`, `fire_coral`, `fire_coral_block`,
`fire_coral_fan`, `fire_coral_wall_fan`, `frogspawn`, `frosted_ice`,
`glow_lichen`, `lily_pad`, `sculk_vein`, `sugar_cane`, `tube_coral`,
`tube_coral_block`, `tube_coral_fan`, `tube_coral_wall_fan`,
`twisting_vines_plant`, `vine`, `weeping_vines`, `weeping_vines_plant`.

Trois familles, et aucune n'est une surprise une fois nommée : ce qui pousse sur
un support précis (tiges, cactus, canne, nénuphar), ce qui s'accroche à un mur
(lichen, vigne, veine de sculk), et **les coraux vivants, qui meurent hors de
l'eau**. Le piège du corail est déjà écrit dans `redstone.md` § 2 ; il est repayé
ici sur les **blocs** de corail, que la campagne de conduction n'avait pas
touchés.

### Ce que la rangée lit à côté de la résistance — 21 blocs

Nommés plutôt que jetés en silence, parce que chacun est un mécanisme du jeu et
pas du bruit :

- **les 20 coraux vivants** (`tube`, `brain`, `bubble`, `fire`, `horn` × bloc,
  corail, éventail, éventail mural) : ceux qui ont tenu à la première lecture
  sont morts pendant les passes. `bubble_coral_block` donne un score de
  **3,94** là où sa classe (résistance 6) donne **1,00** : le banc a mesuré une
  mort, pas un souffle ;
- **`minecraft:tnt`** : score **8,31** sur 9, profil `[16,16,16,16,16,16,14,13,10]`.
  La rangée s'est **allumée elle-même**. C'est le témoin le plus fort que le banc
  fonctionne : la détonation en chaîne est un vrai mécanisme du jeu et il est
  visible dans les chiffres bruts.

### Le résultat

Le score `S` d'un bloc est la somme, sur les neuf cellules, de la fréquence de
destruction. Il est **monotone en résistance et en rien d'autre**. Confronté au
candidat de PrismarineJS/minecraft-data (MIT, `pc/1.20`) — la même source, prise
de la même façon, que la dureté (`measure_hardness.py`) :

| candidat | blocs | S médian | | candidat | blocs | S médian |
|---|---|---|---|---|---|---|
| 0,0 | 131 | 4,188 | | 1,0 | 100 | 2,125 |
| 0,1 | 37 | 3,938 | | 1,4 | 16 | 2,000 |
| 0,2 | 32 | 3,844 | | 1,5 | 13 | 2,000 |
| 0,25 | 3 | 3,688 | | 1,8 | 16 | 2,000 |
| 0,3 | 42 | 3,625 | | 2,0 | 66 | 2,000 |
| 0,4 | 6 | 3,250 | | 2,5 … 9,0 | 340 | 1,000 |
| 0,5 | 80 | 3,000 | | 600 … 3,6 M | 20 | 0,000 |
| 0,6 … 0,8 | 57 | 2,875 | | | | |

**Aucune inversion sur 31 valeurs distinctes.** C'est le contrôle : une campagne
qui donnerait l'obsidienne plus fragile que le verre serait fausse quel que soit
le nombre.

- **959 blocs** passés devant une charge ;
- **947** tombent dans leur propre classe de résistance ;
- **3 écarts**, tous expliqués et nommés : `bamboo` et `bamboo_sapling`
  (S = 3,00 contre 2,125 pour la classe 1,0) reposent sur un sol de **terre**,
  qui est détruit sous eux et les fait tomber une cellule trop loin ;
  `spruce_wall_hanging_sign` (2,69), profil `[16,16,5,1,1,1,1,1,1]`, s'est
  décroché une fois sur seize.

### Ce que le banc **ne sépare pas**, et pourquoi

Un rayon de puissance 4 porte au plus `4 × 1,3 = 5,2` d'énergie ; un bloc de
résistance R lui en coûte `(R + 0,3) × 0,3` par pas. Passé
`R ≈ (5,2 − 0,09) / 0,3 ≈ 17`, **aucune explosion du jeu ne fait la différence**
entre deux résistances : la plus grosse charge que le jeu produise est celle du
Wither, puissance 7, soit un plafond à `R ≈ 30`. L'obsidienne à 1200, le
bedrock à 3,6 millions et l'ender chest à 600 sont **indiscernables par
construction**, et le seraient encore avec dix mille tirs.

Bandes que le banc à puissance 4 et sans écart ne sépare pas :

`[0,1 · 0,2]` · `[0,25 · 0,3]` · `[0,5 · 0,6 · 0,65 · 0,7 · 0,75]` ·
`[1,4 · 1,5 · 1,8 · 2,0]` · `[2,5 … 9,0]` · `[600 … 3,6 M]`

Les quatre premières bandes sont un problème de résolution, pas de principe : le
banc a **un écart d'air réglable** (`measure_blast.py resistance_gap`) qui mange
l'énergie avant la rangée et remet la bande haute dans la zone où une cellule
casse *parfois* — et une fréquence mesure là où une certitude ne mesure rien. La
dernière bande, elle, est un plafond du jeu.

---

## 2. L'algorithme

Spécifié depuis la documentation (`https://minecraft.wiki/w/Explosion`, article
« Explosion », section « Java Edition »), jamais depuis du code, et chaque
constante confrontée au cratère mesuré.

1. **1352 rayons**, vers les cellules **extérieures** d'une grille 16×16×16 :
   `16³ − 14³`. Prendre les 4096 tirerait les directions intérieures plusieurs
   fois et épaissirait le cratère le long des axes.
2. Direction `(j/15 × 2 − 1, k/15 × 2 − 1, l/15 × 2 − 1)`, **calculée en
   `float` puis élargie**, puis normalisée en `double`.
3. Énergie `puissance × (0,7 + 0,6 × aléa)`, **un tirage par rayon**.
   ⚠ Un seul tirage nommé, dans l'ordre : C++ ne séquence pas les opérandes
   d'une addition, Java si.
4. Pas de **0,3 bloc**, coûtant **0,22500001** — pas 0,225. Le dernier bit est
   dans la constante du jeu et il fait entrer ou sortir la dernière cellule d'un
   rayon.
5. Un bloc non-air coûte `(résistance + 0,3) × 0,3` **par pas passé dedans**.
   L'air pur ne coûte **rien du tout** — pas « une résistance nulle », rien : le
   biais de 0,3 n'est pas payé. Un bloc fait un peu plus de trois pas, donc un
   bloc de résistance R coûte à peu près `R + 1,05`.
6. La cellule est prise si l'énergie est **encore strictement positive après**
   la soustraction.
7. Un fluide répond **100**, et un bloc noyé répond le **plus grand** des deux.
   C'est ce qui fait qu'un couloir inondé survit à une charge qui aplatit le
   même couloir à sec, et c'est la raison pour laquelle tout bloc du banc a été
   posé `waterlogged=false`.

Le RNG à passer est `math::LegacyRandomSource` — `java.util.Random`, ce que le
niveau 1.20.1 utilise pour ses explosions. Notre ordre de parcours de la grille
est `j, k, l` et il consomme exactement un `next_float()` par rayon, donc deux
appels avec le même état donnent le même cratère : le test
`« the same seed makes the same crater »` le vérifie dans les deux sens.

Vanilla garde les positions dans un `HashSet` puis les **mélange**. Cet ordre
n'est reproductible par rien, le nôtre est trié — l'ensemble est le même, et le
choix est écrit dans le code plutôt que subi.

---

## 3. Le cratère, cellule par cellule

*(section remplie par `measure_blast.py crater` ; voir le test de parité
`« the crater a real server made, cell by cell »`.)*

---

## 4. Les dégâts et le recul

*(section remplie par `measure_blast.py damage`.)*

---

## 5. Les sources

*(section remplie par `measure_blast.py sources` et `drops`.)*

---

## 6. Reproduire

```bash
python3 scripts/measure_blast.py resistance 16    # ~25 min, 987 blocs
python3 scripts/measure_blast.py resistance_gap 16
python3 scripts/measure_blast.py crater 24        # la forme, cellule par cellule
python3 scripts/measure_blast.py damage
python3 scripts/measure_blast.py drops
python3 scripts/measure_blast.py sources
python3 scripts/measure_blast.py table            # la confrontation, puis la table
python3 tools/ov_datagen/ovpack.py                # FORMAT_VERSION 14
```

Chaque scénario démarre et arrête son propre serveur, sur le port 25613, dans
`run/blast-oracle/<scénario>/`. Aucun ne dépend d'un autre.

⚠ `tools/ov_datagen/ovpack.py` est passé de **13 à 14** : le pack porte
maintenant un flottant de résistance par bloc, dans le `u32` que l'en-tête
gardait en réserve. Un pack de format 13 est refusé au chargement plutôt que
mal relu.
