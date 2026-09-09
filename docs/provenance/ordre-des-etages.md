# L'ordre des étages du worldgen

Ce document trace pourquoi les étages de `src/ov_worldgen/src/chunk_generator.cpp` ont été
remis dans l'ordre du jeu, ce que ça change, ce que ça ne change pas, et — surtout — comment
la différence a été **mesurée**, puisqu'aucune des trois sondes existantes ne pouvait la voir.

Le résultat court, d'abord :

> **Sur les 763 cellules que le réordonnancement change réellement, le nouvel ordre met le bloc
> du vrai jeu dans 73,3 % des cas contre 8,9 % pour l'ancien.** L'accord sur la peau des
> grottes passe de **87,40 % à 89,12 %** (+1,72 point). Les trois sondes existantes ne bougent
> pas d'un chiffre, et c'est attendu : **aucune des trois n'exécute `ChunkGenerator::generate()`**.

---

## 1. Le problème

L'ordre du vrai jeu est :

```
bruit  →  surface  →  carvers  →  features
```

L'ordre qu'avait le dépôt après la fusion de deux branches parallèles était :

```
bruit + carvers fusionnés dans la même boucle  →  biomes  →  heightmaps  →  surface
```

Deux conséquences, opposées et toutes deux fausses :

1. **Les règles de surface voyaient un terrain déjà creusé.** Une colonne dont une grotte a
   mangé le sommet fait compter `stone_depth` depuis le *plafond de la grotte* au lieu de la
   vraie surface. Or `stone_depth` est la condition la plus utilisée de tout l'arbre de règles
   de l'overworld — 122 occurrences. La grotte fabriquait donc une fausse surface, avec sa
   couche d'herbe et ses trois blocs de terre, à l'intérieur du plafond.

2. **Les carvers ne pouvaient creuser que de la pierre**, puisque au moment où ils passaient
   il n'y avait rien d'autre. Le code disait explicitement « seule la pierre est creusée », et
   c'était vrai *par construction* — pas parce que le jeu fait ça.

3. **La règle « l'herbe au-dessus d'une grotte devient de la terre » était impossible.** Elle
   exige que l'herbe existe, donc que la surface soit posée avant le creusement. L'agent des
   carvers l'avait explicitement nommée comme bloquée par l'ordre
   (`docs/provenance/carvers.md` § 5).

---

## 2. Ce qui a changé

`ChunkGenerator::generate()` fait maintenant, dans cet ordre :

| # | étage | ce qu'il pose |
|---|---|---|
| 1 | bruit | pierre, eau, lave, air — **et rien n'est creusé** |
| 2 | biomes | la grille 4×4×4, depuis le même routeur |
| 3 | règles de surface | herbe, terre, sable, gravier, grès, bandes d'argile, bedrock |
| 4 | carvers | le masque, appliqué à ce que la surface a posé |
| 5 | heightmaps | recalculées **après** le creusement |

### 2.1 Le masque n'a pas bougé

Le masque de creusement est calculé exactement comme avant. Il ne dépend que de la graine, du
chunk et de la hauteur du monde — ni du terrain, ni des blocs — donc le déplacer dans le temps
ne peut pas le changer. C'est vérifié et pas seulement affirmé : `ov_carveparity` reste à
**1 200 / 1 200 chunks, 1 615 858 cellules, 100,000 %**, identique au chiffre d'avant.

### 2.2 Ce qu'un creusement remplace : le tag, pas une liste

Une fois la surface posée, le sommet d'une colonne est de l'herbe, de la terre, du sable ou du
gravier, et le jeu les creuse aussi. La liste est le tag
`minecraft:overworld_carver_replaceables`, et c'est **une donnée** :

```
data/vanilla/1.20.1/generated/data/minecraft/tags/blocks/overworld_carver_replaceables.json
```

Le fichier est un graphe — il inclut `#minecraft:base_stone_overworld`, `#minecraft:dirt`,
`#minecraft:sand`, `#minecraft:terracotta`, `#minecraft:iron_ores`, `#minecraft:copper_ores`
plus dix blocs nommés directement. `ov_registry` aplatit déjà ce graphe au moment de la
construction du pack, donc **rien n'est recopié à la main** : le générateur résout le tag une
fois, à l'attachement des carvers, et obtient **49 blocs**.

La résolution passe par les **noms**, pas par les identifiants : un membre du tag est un id
réseau du registre `minecraft:block`, converti en nom puis cherché dans `BlockRegistry`.
Supposer que l'index du registre de blocs et l'id réseau sont le même nombre est vrai en 1.20.1,
mais c'est une hypothèse invisible tant qu'elle tient et silencieuse quand elle cesse de tenir.
La résolution coûte 49 recherches par générateur, pas par bloc.

**Le tag est requis, pas optionnel.** `set_carvers()` prend maintenant *à la fois* l'étage de
carving et les `Registries`, et renvoie un `std::expected` qui refuse et **nomme** l'échec
(`NoBlockRegistry`, `NoReplaceablesTag`, `UnknownMember`). Il est impossible d'attacher les
carvers sans le tag, donc impossible de creuser en laissant silencieusement une croûte de terre
au-dessus de chaque grotte. C'est la règle du dépôt — un cas non implémenté est refusé et
nommé — appliquée à une dépendance de données.

### 2.3 La règle de l'herbe

Un bloc d'herbe dont le bloc en dessous vient d'être creusé devient de la terre.

Le balayage se fait **vers le haut**, et la direction porte le résultat : une cellule doit être
coupée avant que le bloc au-dessus soit interrogé ; et quand ce bloc au-dessus est lui-même
creusé, la terre que la passe vient d'y écrire est atteinte plus tard dans le même balayage et
retirée — ce qui est la bonne réponse, pas une coïncidence.

`grass_block` fait partie de `#minecraft:dirt`, donc du tag : une grotte qui traverse une
colline coupe bien l'herbe elle-même, et la règle ne s'applique qu'au bord.

### 2.4 Les heightmaps

Elles étaient calculées avant la surface. Elles le sont maintenant **en dernier**, après le
creusement : une cellule creusée peut être exactement le bloc que la heightmap désignait.

---

## 3. Le piège des sondes : aucune des trois ne voit l'ordre

C'est le fait le plus important de ce document, et il a failli faire passer un
réordonnancement pour un non-événement.

| sonde | ce qu'elle compare | voit-elle l'ordre ? |
|---|---|---|
| `ov_carveparity` | le **masque** contre celui du jeu | **non** — le masque ne dépend pas de l'ordre |
| `ov_surfparity` | les règles sur le terrain **du jeu** | **non** — elle ne voit jamais un carver |
| `ov_parity --terrain --carvers` | solide / non-solide | **non** — un mur de grotte est solide quel que soit le bloc |

Et surtout : **`ov_parity --terrain` n'appelle pas `generate()`**. Il interroge `is_solid()`
(la densité) et soustrait le masque directement. Les trois sondes sont donc, structurellement,
insensibles à ce changement.

Les chiffres ont quand même été pris des deux côtés, sur le même échantillon, avec le même
binaire reconstruit — parce qu'« ça ne peut pas bouger » est un raisonnement et que ce dépôt
demande une mesure :

| sonde | commande | avant | après |
|---|---|---|---|
| `ov_carveparity` | `--chunks=1200` | 1 200 / 1 200 — **100,000 %**<br>1 615 858 cellules, 0 en trop, 0 manquante | **identique**, au bit près |
| `ov_surfparity` | `--chunks=250 --per-region=4` | colonnes 58 378 / 64 000 — **91,216 %**<br>blocs 429 195 / 439 432 — **97,670 %** | **identique**, au bloc près |
| `ov_parity --terrain --carvers` | `--chunks=600` | 3 415 855 / 3 456 000 — **98,838 %**<br>pierre en trop 14 060, manquante 26 085 | **identique**, au bloc près |

Aucune ne baisse, aucune ne monte. Cette identité est un **contrôle de non-régression** — elle
prouve que le masque et les règles n'ont pas été abîmés — et **pas** une mesure de l'effet du
réordonnancement, qu'elles sont incapables de voir.

C'est pourquoi il a fallu une quatrième sonde.

---

## 4. La mesure qui manquait : `tools/ov_caveedge`

L'endroit où l'ordre se voit est la **peau des grottes**. La sonde prend, dans chaque chunk :

- les cellules **creusées** (l'intérieur) ;
- les cellules **non creusées mais adjacentes** à une creusée, en 6-connexité (le mur) ;

et compare notre bloc au bloc du vrai jeu, nom contre nom.

Elle génère les **deux ordres dans un seul processus, depuis un seul binaire, sur le même
échantillon** — la même discipline que `ov_parity --carvers`, et pour la même raison : prendre
l'avant dans une version du dépôt et l'après dans une autre est exactement comme ça qu'une
baisse passe inaperçue. L'ancien ordre est conservé dans le générateur comme **instrument de
mesure** (`set_carve_before_surface()`, ou `OV_CARVE_BEFORE_SURFACE`), pas comme mode de jeu ;
un test unitaire échoue si les deux ordres cessent de différer, faute de quoi tous les chiffres
ci-dessous deviendraient silencieusement une comparaison de quelque chose avec lui-même.

### 4.1 Le chiffre absolu, et pourquoi il ne suffit pas

Le taux d'accord brut sur les murs de grotte est plafonné par quelque chose qui n'a rien à voir
avec l'ordre : **notre bruit place sa surface 1 à 8 blocs à côté de celle du jeu dans neuf
colonnes sur dix** (mesuré : `ov_parity --terrain` donne 15,9 % de colonnes à la bonne hauteur).
Cette erreur est sous les deux bras de la comparaison et écrase tout.

### 4.2 Le chiffre qui compte : la comparaison appariée

La sonde compte donc, **uniquement sur les cellules de mur où les deux ordres produisent des
blocs différents**, lequel des deux est celui du jeu. Tout ce que les deux bras partagent —
l'erreur de hauteur en premier — s'annule, et il ne reste que l'ordre.

### 4.3 Les chiffres

40 chunks `minecraft:full`, `--per-region=2`, seed 1234567890 — 39 d'entre eux sont creusés,
31 243 cellules de mur et 46 135 cellules d'intérieur.

| | murs de grotte | intérieur de grotte |
|---|---|---|
| **avant** (carvers puis surface, pierre seule) | 27 305 / 31 243 — **87,396 %** | 32 395 / 46 135 — 70,218 % |
| **après** (surface puis carvers, le tag) | 27 796 / 31 191 — **89,115 %** | **33 910 / 46 135 — 73,502 %** |

**+1,72 point** sur les murs, **+3,28 points** à l'intérieur. (Le dénominateur des murs bouge de
52 cellules parce que le nombre de cellules attribuées à l'étage des minerais change avec le
bloc qu'on y met — elles sont tenues à l'écart des deux totaux, pas comptées comme erreurs.)

Le gain à l'intérieur se décompose : +0,31 point vient du réordonnancement lui-même, et
+2,97 points de la décision sur les fluides que le réordonnancement a rendue mesurable — voir
§ 5. Les deux sont dans la même colonne parce qu'ils arrivent dans le même commit, et ils sont
séparés ici parce qu'ils ne sont pas le même résultat.

Comparaison appariée, sur les **763** cellules de mur que le réordonnancement change :

| | cellules | part |
|---|---|---|
| seul le **nouvel** ordre correspond au jeu | **559** | **73,26 %** |
| seul l'**ancien** ordre correspond | 68 | 8,91 % |
| ni l'un ni l'autre | 136 | 17,82 % |

Le rapport est de **8,2 contre 1** en faveur du nouvel ordre.

Le désaccord que le réordonnancement fait disparaître est nommé et visible dans la liste des
confusions : **`minecraft:stone -> minecraft:grass_block`, 468 occurrences avant, 0 après.**
C'est exactement l'artefact prédit : de l'herbe posée par les règles de surface à l'intérieur
d'un plafond de grotte, là où le jeu a de la pierre. C'est la signature de l'ordre, et elle
disparaît complètement — ce n'est pas une amélioration graduelle, c'est un mode d'erreur qui
n'existe plus.

En sens inverse, `minecraft:grass_block -> minecraft:stone` apparaît à **67** dans le bras
« après » et n'existait pas dans le bras « avant ». Ce sont, à une cellule près, les 68 cas où
l'ancien ordre gagnait : des colonnes où notre erreur de hauteur de surface fait que la grotte
coupe notre herbe à un endroit où le jeu ne coupait pas la sienne. **Ce n'est pas un défaut de
l'ordre** — c'est l'erreur du bruit qui devient visible parce que l'ordre est maintenant
correct, et elle appartient à l'étage de densité. C'est le coût du changement, il est nommé, et
il est huit fois plus petit que le gain.

### 4.4 Ce qui n'est pas récupéré

Le plus gros désaccord de mur est identique dans les deux bras — **`minecraft:air ->
minecraft:water`, 1 535 occurrences, au bloc près dans les deux** — et n'a rien à voir avec
l'ordre : c'est **l'aquifère manquant**. Le jeu demande à `computeSubstance` ce que devient une
cellule creusée ; nous n'avons pas d'aquifère, donc l'eau du niveau de la mer reste là où le
jeu a mis de l'air. À lui seul il vaut 4,9 points sur les murs.

Le deuxième, `minecraft:dripstone_block -> minecraft:stone` (418, identique dans les deux bras
aussi), est une **feature** — les grottes de dripstone sont décorées après les carvers. Il
n'appartient ni à cet étage ni à cet ordre.

Un échantillon plus large (250 chunks) a été lancé et **abandonné** : le générateur en build
debug met plusieurs minutes par chunk quand il génère deux fois, et la mesure ne tenait pas
dans le temps de la session. Les 40 chunks ci-dessus couvrent 20 fichiers de région et
77 378 cellules comparées ; c'est dit ici pour que la taille de l'échantillon soit une donnée
connue et non une omission.

---

## 5. Le cas des fluides : le raisonnement disait non, la mesure a dit oui

`minecraft:water` **est** membre du tag : littéralement, un carver a le droit de vider une
cellule d'eau.

Le raisonnement disait de ne pas le faire. Le jeu ne vide pas l'eau n'importe où : il demande
d'abord à l'aquifère, qui répond « garde l'eau » sous le niveau de la mer. Sans aquifère,
appliquer le tag à la lettre devait vider des fonds marins que le jeu a laissés pleins. Le
premier jet du générateur épargnait donc les fluides, avec un commentaire expliquant pourquoi.

**La mesure a dit le contraire**, sur le même échantillon de 40 chunks :

| | murs de grotte | intérieur de grotte |
|---|---|---|
| fluides épargnés (`--keep-fluids`) | 27 796 / 31 191 — 89,115 % | 32 540 / 46 135 — 70,532 % |
| **fluides creusés** (défaut) | 27 796 / 31 191 — 89,115 % | **33 910 / 46 135 — 73,502 %** |

**+2,97 points** à l'intérieur des grottes, et les murs **identiques au bloc près** — ce qui
est cohérent : la décision ne porte que sur des cellules creusées, donc que sur l'intérieur.

L'explication est dans notre propre étage de bruit, pas dans le tag. Nous remplissons **toute**
cellule non solide sous le niveau de la mer avec de l'eau. Une grotte sèche sous une terre
sèche ressort donc inondée, et c'est un cas bien plus fréquent que la grotte sous l'océan que
le raisonnement redoutait. Creuser le fluide répare beaucoup plus de cellules qu'il n'en casse.

Le défaut est donc **fluides creusés**, c'est-à-dire le tag appliqué à la lettre, et
l'interrupteur reste (`OV_CARVE_FLUIDS=0`, ou `ov_caveedge --keep-fluids`) pour que la question
se re-tranche par la mesure quand l'aquifère arrivera et la retirera d'ici complètement.

C'est le deuxième endroit de ce travail où un raisonnement plausible a été démenti par un
oracle ; le premier est le § 3, où trois sondes ne mesuraient pas ce qu'on croyait.

---

## 6. Ce qui n'est pas fait

1. **L'aquifère.** C'est la plus grosse pièce manquante de cet étage et elle est chiffrée :
   1 535 cellules `air -> water` sur 40 chunks, identiques au bloc près dans les deux ordres,
   soit 4,9 points sur les murs de grotte. Tant qu'elle manque, le remplissage d'une cellule
   creusée reste « air, ou lave sous `min_y + 8` ».
2. **L'étage de décoration n'est pas câblé dans `generate()`, et ce n'est pas un oubli.**
   `Decorator::decorate()` prend un `FeatureLevel`, et `placement.hpp` dit explicitement
   pourquoi ce n'est pas un `world::Chunk` : une veine de minerai commencée dans la dernière
   colonne d'un chunk se termine dans le suivant, donc l'étage a besoin du **voisinage 3×3**.
   `ChunkGenerator::generate(world::Chunk&)` ne voit qu'un chunk et ne peut pas le fournir.

   Écrire ici un adaptateur `Chunk` → `FeatureLevel` **paraîtrait** marcher et jetterait
   silencieusement toute écriture franchissant un bord — précisément le bug contre lequel
   `FeatureLevel` a été conçu. La décoration appartient donc à l'étage au-dessus, celui qui
   possède plusieurs chunks (le gestionnaire de chunks du serveur), pas au générateur de
   chunk. C'est refusé et nommé plutôt que câblé de travers.

   L'ordre lui-même, en revanche, est acquis et c'est l'objet de ce document : quand la
   décoration sera câblée, sa place est **après les carvers**, en étage 5.

3. **Les features.** L'étage 5 du jeu n'existe pas encore ici. Les minerais, les variantes de
   pierre, les disques de gravier et les amas `ore_dirt` / `ore_gravel` sont posés *après* les
   carvers et expliquent une part des désaccords restants ; `ov_caveedge` les compte à part
   plutôt que de les fondre dans le total, comme `ov_surfparity` le fait déjà. **C'est une
   attribution, pas une preuve** : un vrai bug peut se cacher derrière chacune.
3. **La forme exacte de la règle de l'herbe.** Le jeu remplace le bloc au-dessus d'une cellule
   creusée par le **matériau de surface du biome** ; nous posons de la terre. C'est le bon
   résultat partout où le matériau de surface est de la terre, c'est-à-dire dans l'immense
   majorité des biomes qui ont de l'herbe, mais ce n'est pas la règle générale et ce n'est pas
   mesuré séparément.
4. **Le nether.** Sa liste de carvers et son tag de remplaçables diffèrent
   (`minecraft:nether_carver_replaceables`). Il est refusé par absence, pas traité comme un
   overworld.

---

## 7. Rejouer les mesures

```bash
cmake --build build/macos-debug --parallel 2

# la mesure qui voit l'ordre : les deux bras, un seul binaire, un seul échantillon.
# Compter en minutes, pas en secondes : chaque chunk est généré deux fois, en debug.
./build/macos-debug/bin/ov_caveedge --chunks=40 --per-region=2

# la même, en épargnant les fluides — la mesure du § 5
./build/macos-debug/bin/ov_caveedge --chunks=40 --per-region=2 --keep-fluids

# les trois contrôles de non-régression — ils doivent être identiques à l'avant
./build/macos-debug/bin/ov_carveparity --chunks=1200
./build/macos-debug/bin/ov_surfparity  --chunks=250 --per-region=4
./build/macos-debug/bin/ov_parity --terrain --carvers --chunks=600 \
    --world=run/reference-1234567890/world
```

## 8. Sources

- **`minecraft:overworld_carver_replaceables`** : `tags/blocks/` du data generator, régénéré
  localement, aplati par `ov_registry`. C'est la seule source de la liste des blocs creusables ;
  aucune liste n'est écrite en dur dans le code.
- **L'ordre des étages** (`noise → surface → carvers → features`) : la documentation de la
  génération de monde de `minecraft.wiki`, et il est déjà nommé comme l'ordre du jeu dans
  `docs/provenance/carvers.md` § 5, écrit avant ce travail.
- **Tous les chiffres** : mesurés contre `run/reference-1234567890`, généré par le vrai serveur
  1.20.1 à la graine 1234567890. Aucun ne vient d'un raisonnement.
