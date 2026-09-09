# Le pipeline de chunks, et l'étage de décoration enfin branché

Ce document trace la couche qui possède **plusieurs chunks**, pourquoi elle a dû exister avant
que la décoration puisse tourner, et ce que le monde généré vaut maintenant qu'elle existe.

Le résultat court, d'abord :

> **Sur un monde entièrement généré par nous — bruit, biomes, surface, carvers, features — les
> minerais du vrai jeu tombent au bon bloc dans 82,406 % des cas** (14 880 / 18 057, 48 chunks,
> graine 1234567890). Le même étage de décoration mesuré sur le terrain **du jeu** donne
> 99,509 %. L'écart de **17,1 points** est ce que notre relief coûte encore, et il est réparti
> plus bas.
>
> **27,970 % des écritures de la décoration tombent hors du chunk décoré et sont conservées**
> (350 762 sur 1 254 082). C'est la raison d'être de toute cette couche : un adaptateur
> mono-chunk aurait affiché zéro et aurait eu l'air parfaitement sain.

---

## 1. Pourquoi la décoration ne pouvait pas être branchée là où elle était réclamée

Deux agents successifs ont refusé de câbler `Decorator::decorate` dans `ChunkGenerator`, et
`docs/provenance/ordre-des-etages.md` § 6.2 comme `docs/provenance/features.md` le disent en
toutes lettres : `decorate` prend un `FeatureLevel` qui couvre le **voisinage 3×3**, parce
qu'une veine commencée dans la dernière colonne d'un chunk se termine dans le suivant.
`ChunkGenerator::generate(world::Chunk&)` ne voit qu'un chunk. Un adaptateur `Chunk` →
`FeatureLevel` aurait avalé en silence toute écriture franchissant un bord.

Le refus était juste, et la mesure le confirme : **plus d'un quart des écritures de la
décoration sortent du chunk décoré**. Un adaptateur mono-chunk n'aurait pas produit une petite
erreur, il aurait détruit 27,970 % du travail de l'étage sans un message.

La conclusion des deux refus était aussi la bonne : la décoration appartient à l'étage
au-dessus, celui qui possède neuf chunks. Ce document décrit cet étage.

---

## 2. Ce qui a été écrit

### 2.1 Les étages de `ChunkGenerator`, séparés

`generate()` faisait bruit → biomes → surface → carvers → heightmaps en un bloc. Les quatre
étages sont maintenant quatre méthodes (`generate_noise`, `generate_biomes`, `generate_surface`,
`generate_carvers`) et `generate()` les appelle dans le même ordre. **Aucun bloc n'a bougé** :
`test_pipeline.cpp` compare, cellule par cellule sur les 98 304 blocs et les 1 536 cellules de
biome d'un chunk, ce que produit `generate()` et ce que produit le pipeline en pilotant les
étages un par un. Zéro différence.

Aucun des quatre ne lit un voisin : ce sont des fonctions pures de la graine et de la position.
C'est ce qui donne au pipeline un **rayon de zéro** jusqu'aux carvers, et de **un** seulement à
l'étage des features.

### 2.2 `ChunkPipeline` (`src/ov_worldgen/{include/ov/worldgen,src}/pipeline.{hpp,cpp}`)

Un chunk monte par **statuts**, ceux du jeu, dans l'ordre du jeu :

```
empty → structure_starts → biomes → noise → surface → carvers → features → full
```

`structure_starts` existe et ne fait rien : c'est une marche que le chunk franchit réellement,
et la renommer pour cacher le trou rendrait le trou invisible au lieu de visible. Aucune
structure n'appartient à l'étape `underground_ores` (`docs/provenance/features.md`), donc les
minerais ne l'attendent pas.

Deux statuts ont un rayon, et le pipeline les **fait respecter** plutôt que de les espérer —
`promote()` pilote lui-même les voisins, et il n'existe aucune autre entrée vers l'étage des
features :

| statut | ce qu'il exige des 8 voisins | pourquoi |
|---|---|---|
| `features` | statut `carvers` | une veine qui déborde dans un voisin non creusé serait recoupée ensuite ; dans un voisin inexistant, elle serait perdue |
| `full` | statut `features` | c'est le moment où la **dernière** écriture qui peut atterrir dans ce chunk a atterri |

La deuxième ligne est celle qu'on oublie. Un chunk qui a décoré n'est pas fini : ses huit
voisins vont encore écrire dedans. `full` est le seul moment où il peut être distribué, et
c'est pour ça que `take()` peut le sortir du cache sans rien perdre.

L'ordre à l'intérieur du statut `carvers` est : creusement, **puis** `recompute_heightmaps()`.
La décoration vient après, à l'étage suivant — pas dans `recompute_heightmaps()`, après lui —
parce que le placement interroge la hauteur des colonnes à presque chaque tentative.

### 2.3 Le cache

Décorer un chunk exige ses 8 voisins creusés ; amener chacun d'eux au statut `features` exige
**leurs** 8 voisins creusés. Un chunk `full` isolé demande donc le **5×5** autour de lui, soit
25 chunks de terrain et 9 décorations. Sans cache ce serait 9 terrains par chunk décoré et
81 par chunk `full`.

Mesuré, sur 48 chunks comparés voisins les uns des autres :

```
chunks amenés à chaque statut : biomes 484, noise 484, surface 484, carvers 484,
                                features 216, full 48
décorations lancées : 216   (une par chunk, pas neuf)
```

484 terrains pour 48 chunks finis — 10,1 par chunk fini au démarrage à froid d'un lot, et
**1 en régime établi** quand on balaie une région : chaque chunk du voisinage est déjà là.
216 décorations pour 48 chunks, c'est 4,5, pas 9 : les voisinages se recouvrent et le cache
garde le recouvrement.

Le test unitaire vérifie le cas exact : un seul chunk mené à `full` amène **25** chunks au
statut `carvers`, pas 81.

### 2.4 L'empreinte

Peak 57 chunks résidents sur le lot de 48 ; après `trim`, 30 chunks pour **0,7 MiB** de
conteneurs palettés et de heightmaps. C'est une mesure des tableaux, pas du `sizeof` de l'objet
`world::Chunk` : `footprint_bytes()` somme les longs des conteneurs et les palettes, ce que le
commentaire de la fonction dit. Un chunk de terrain généré se palettise très bien — pierre,
deepslate, air, eau — d'où les ~20 Kio par chunk.

---

## 3. L'ordonnancement par régions exclusives — le verrou 🔒 de M3

Le verrou existe parce que deux chunks décorés en parallèle écrivent dans le même voisin.
`CLAUDE.md` § 2 principe 3 interdit le remède facile : **aucun mutex sur le monde, jamais**.

La règle correcte est **géométrique**, et elle est écrite et testée :

> Décorer un chunk écrit dans les neuf chunks autour de lui. Deux chunks peuvent donc être
> décorés en même temps exactement quand leurs deux ensembles de neuf sont disjoints,
> c'est-à-dire quand ils sont à trois chunks ou plus l'un de l'autre sur au moins un axe.
> Les classes `(x mod 3, z mod 3)` ont cette propriété **par construction** : deux chunks
> distincts d'une même classe diffèrent d'un multiple de trois sur un axe.

`ChunkPipeline::exclusive_class(x, z)` rend cette classe, et `test_pipeline.cpp` la vérifie de
façon exhaustive sur 13×13 chunks : pour toute paire de chunks distincts de même classe, les
deux voisinages 3×3 sont disjoints. Le test vérifie aussi que les neuf classes sont toutes
utilisées — une coloration dégénérée passerait la disjonction en laissant des classes vides.

**Rien ne tourne en parallèle pour autant, et c'est délibéré.** L'argument est mesuré :

| bras | temps par chunk comparé | ce qui tourne |
|---|---|---|
| terrain seul (`--no-features`) | *voir § 5.4* | bruit, biomes, surface, carvers |
| terrain + décoration | *voir § 5.4* | les cinq étages |

La décoration est la **moitié** du coût, pas la totalité : paralléliser la décoration seule
plafonne le gain à un facteur deux, et les quatre étages de terrain, eux, sont déjà de rayon
zéro et parallélisables sans aucune exclusion — ils n'ont pas besoin du verrou. Le verrou ne
paie donc que sur la moitié du coût, et il exige un cache de chunks partagé entre threads,
c'est-à-dire exactement la structure mutable partagée que le principe 3 cherche à éviter.
Le bon moment pour le poser est avec le `ChunkMap` et le pool de jobs, quand il y aura un
propriétaire pour ce cache ; poser aujourd'hui un verrou sans threads serait décoratif.

Ce qui est acquis maintenant : la **règle** est écrite, testée et disponible, et le prochain
agent n'aura pas à l'inventer.

---

## 4. La mesure : `tools/ov_genparity`

`ov_features` prend le terrain **du jeu** comme donné. C'est la bonne façon de mesurer le
placement, et c'est ce que fait son 99,509 %. Ça ne dit rien sur le monde que notre pipeline
produit : une veine posée à la coordonnée exacte du jeu tombe dans le vide si notre bruit a mis
de l'air là.

`ov_genparity` génère les chunks **de bout en bout** par `ChunkPipeline`, et compare les blocs
de minerai du monde obtenu à ceux du monde de référence, position par position. Trois arms sur
le même échantillon :

* défaut — les cinq étages ;
* `--no-features` — terrain seul, le **plancher** : combien des minerais du jeu notre monde
  contiendrait sans aucune décoration (zéro, par construction) et surtout **quel bloc notre
  terrain a** à ces positions, ce qui donne le plafond que notre relief impose ;
* `--seed`, `--world` pour une graine hors échantillon.

---

## 5. Les chiffres

Graine 1234567890, 48 chunks au statut `minecraft:full`, `--per-region=4`, build **debug**.

### 5.1 Les minerais dans un monde que nous avons généré

| minerai | jeu | nous | même bloc | % |
|---|---:|---:|---:|---:|
| coal_ore | 4079 | 3956 | 1993 | 48,860 |
| copper_ore | 4761 | 4694 | 3982 | 83,638 |
| deepslate_coal_ore | 65 | 66 | 63 | 96,923 |
| deepslate_copper_ore | 561 | 597 | 529 | 94,296 |
| deepslate_diamond_ore | 737 | 750 | 734 | 99,593 |
| deepslate_gold_ore | 1082 | 1106 | 1059 | 97,874 |
| deepslate_iron_ore | 1309 | 1243 | 1178 | 89,992 |
| deepslate_lapis_ore | 667 | 690 | 661 | 99,100 |
| deepslate_redstone_ore | 1661 | 1657 | 1656 | 99,699 |
| diamond_ore | 16 | 12 | 12 | 75,000 |
| gold_ore | 175 | 174 | 173 | 98,857 |
| iron_ore | 2299 | 2289 | 2209 | 96,085 |
| lapis_ore | 500 | 501 | 487 | 97,400 |
| redstone_ore | 145 | 145 | 144 | 99,310 |
| **tout** | **18 057** | **17 880** | **14 880** | **82,406** |

### 5.2 Les deux chiffres, et ce qui les sépare

| ce qui est mesuré | terrain | résultat |
|---|---|---|
| `ov_features --chunks=48` | celui du **jeu** | 13 184 / 13 249 — **99,509 %** |
| `ov_genparity --chunks=48` | **le nôtre**, généré de bout en bout | 14 880 / 18 057 — **82,406 %** |

**17,1 points d'écart, et c'est ce que notre relief vaut encore.** Le mot « relief » recouvre
deux choses distinctes, et le diagnostic de l'outil les sépare. Sur les **3 177** minerais du
jeu que nous ne reproduisons pas :

| ce que notre monde a à cette position | blocs | part de l'écart |
|---|---:|---:|
| `stone` (2017) ou `deepslate` (151) — **rien de nous n'y est arrivé** | 2168 | 68,2 % |
| une de **nos propres pierres décoratives** l'a atteint d'abord : granite 255, andésite 184, diorite 174, tuff 24 | 637 | 20,0 % |
| notre terrain **n'est pas de la pierre** là : dirt 82, water 78, air 63, gravel 29 | 252 | 7,9 % |
| un de **nos propres minerais** d'une autre espèce : coal_ore 43, deepslate_copper_ore 27 | 70 | 2,2 % |
| le reste, en queue | 50 | 1,6 % |

La deuxième ligne existe aussi dans `ov_features` — 33 blocs sur 65 y sont attribués à nos
propres blobs de diorite et d'andésite — donc elle n'est pas un effet du terrain. La troisième
l'est entièrement. La première est la grosse, et elle mérite d'être nommée précisément :

**ce n'est pas « nous n'avons pas placé la veine », c'est « le placement a refusé la
position ».** Le pipeline de placement lit le terrain : `height_range` s'ancre sur une
heightmap, `block_predicate_filter` demande si le bloc visé est remplaçable,
`discard_chance_on_air_exposure` regarde les six voisins. Un terrain différent donne des
réponses différentes, avec le même flux de tirages. Le minerai le plus touché le dit sans
ambiguïté : **`coal_ore` à 48,860 %**, et `ore_coal_lower` utilise `ore_coal_buried`, dont le
`discard_chance_on_air_exposure` vaut 0,5 — la veine saute les blocs voisins de l'air, et notre
air n'est pas le leur. C'est exactement le mécanisme que `docs/provenance/features.md` avait
déjà identifié à la graine 987654321, ici visible sur la graine de référence parce que le
terrain est le nôtre.

### 5.3 Le plafond que notre terrain impose

Le bras `--no-features` répond à la question « à ces 18 057 positions, qu'y a-t-il dans notre
terrain nu ? » :

| bloc | occurrences | part |
|---|---:|---:|
| `stone` | 11 499 | 63,68 % |
| `deepslate` | 6 323 | 35,02 % |
| dirt 80, water 78, air 63, gravel 10, sandstone 3, grass_block 1 | 235 | 1,30 % |

**98,70 % des positions où le jeu a un minerai sont de la pierre ou du deepslate chez nous.**
Le plafond brut du terrain est donc haut ; ce n'est pas la forme du relief en gros qui coûte
les 17 points, ce sont les décisions du placement que ce relief modifie au détail.

Un point que cette ligne **ne** tranche **pas** : la frontière pierre / deepslate. Les
proportions globales sont proches (le jeu a 11 975 minerais de la famille « pierre » et 6 082
de la famille « deepslate » ; notre terrain a 11 499 pierres et 6 323 deepslates aux mêmes
positions), mais l'accord **position par position** entre les deux frontières n'est pas mesuré
ici. C'est une observation agrégée, pas une preuve.

### 5.4 Le coût

Build debug. Les deux bras repris **seuls sur la machine**, rien d'autre en cours, sur le même
échantillon de 24 chunks — parce que les premières valeurs avaient été prises pendant qu'une
compilation tournait et qu'un chiffre de temps pris sous charge ne vaut rien :

| bras | secondes pour 24 chunks | par chunk comparé |
|---|---:|---:|
| terrain seul (`--no-features`) | 41,077 | **1,712 s** |
| terrain + décoration | 91,679 | **3,820 s** |

Les mêmes bras sur 48 chunks, sous charge, donnaient 1,745 s et 3,844 s — le rapport est stable.

**La décoration double le coût d'un chunk** : 1,71 s de terrain, 2,11 s de features. Le rapport,
pas les secondes, est le chiffre : en debug tout est dix à vingt fois trop lent, et rien ici ne
prétend être un budget de tick.

### 5.5 Les écritures à la frontière — la mesure qui justifie toute la couche

| | 48 chunks |
|---|---:|
| écritures de la décoration | 1 254 082 |
| dont **hors du chunk décoré, et conservées** | **350 762 — 27,970 %** |
| dont sorties du 3×3 et vraiment perdues | **0** |

Les deux lignes comptent.

La première est le chiffre à retenir : **plus d'un quart de ce que la décoration écrit ne va
pas dans le chunk qu'elle décore.** Un `FeatureLevel` mono-chunk aurait rendu `false` sur
chacune de ces 350 762 écritures — ce qui n'est même pas une erreur pour l'interface,
puisqu'une écriture hors zone est légitime — et le monde aurait eu des veines coupées net à
chaque bord, sans un message nulle part.

La seconde est une vérification, et elle est aussi une information : **zéro** écriture ne sort
du 3×3. Les features que nous savons construire — minerais, pierres décoratives, disques,
sources — ont un rayon assez petit pour qu'une origine placée dans le chunk central reste dans
le voisinage immédiat. Le 3×3 suffit **pour ces features-là** ; le compteur reste en place
parce que ce ne sera pas vrai des arbres et encore moins des structures, et le jour où il
cessera de valoir zéro on le saura par une ligne de sortie et non par un bug de terrain.

Un test unitaire vérifie la même chose autrement, sans faire confiance au compteur : deux
pipelines sur la même graine, l'un qui décore `(0,0)` et l'autre non, et on compare le chunk
`(1,0)` entre les deux. Il diffère. Ce que la décoration de `(0,0)` a écrit dans `(1,0)` y est
encore.

### 5.6 Les trois harnais existants — contrôle de non-régression

⚠️ **Aucun des trois n'exécute `generate()` ni le pipeline**, exactement comme
`docs/provenance/ordre-des-etages.md` § 3 l'avait établi, et je l'ai vérifié dans le code avant
de citer leurs chiffres :

* `ov_carveparity` appelle `CarverStage::carve_into` — jamais le générateur ;
* `ov_surfparity` appelle `SurfaceSystem::build` sur le terrain du jeu ;
* `ov_parity --terrain` n'appelle que `is_solid()` et `density_at()`.

Leurs chiffres ne sont donc **pas** une preuve que ce travail est bon ; ils sont une preuve
qu'il n'a **rien cassé** dans les étages qu'il a déplacés. Pris après :

| sonde | commande | avant (docs) | après |
|---|---|---|---|
| `ov_carveparity` | `--chunks=1200` | 1200 / 1200 — 100,000 %, 1 615 858 cellules | **identique** |
| `ov_surfparity` | `--chunks=250 --per-region=4` | colonnes 58 378 / 64 000 — 91,216 % · blocs 429 195 / 439 432 — 97,670 % | **identique** |
| `ov_features` | `--chunks=48` | 13 184 / 13 249 — 99,509 % | **identique** |
| `ov_parity --terrain --carvers` | `--chunks=600` | 3 415 855 / 3 456 000 — 98,838 %<br>pierre en trop 14 060, manquante 26 085 | **identique**, au bloc près |

`ov_features` **est** dans la liste et n'y était pas par hasard : il exécute la décoration, donc
il aurait vu un dégât dans `Decorator` ou dans `FeatureLevel`. Il ne bouge pas d'un bloc, ce qui
dit que rien de ce travail n'a touché au placement lui-même — ce qui est vrai, aucun des quatre
fichiers de l'étage n'a été modifié.

---

## 6. Le branchement dans le serveur

`src/ov_server/src/generated_world.{hpp,cpp}` : un fichier neuf qui possède toute la pile
worldgen et le pipeline. `server.cpp` reçoit un `#include`, **un bloc contigu** de vingt lignes
et la ligne qui choisit le générateur, rien d'autre — server.cpp est un fichier que plusieurs
agents éditent en même temps et un sous-système coincé au milieu est un conflit garanti.

Le chemin superflat est intact et reste le défaut. La génération s'allume par
`OV_WORLDGEN_SEED` :

```bash
OV_WORLDGEN_SEED=1234567890 ./build/macos-debug/bin/ov_dedicated --world=run/generated
```

Une variable d'environnement plutôt qu'un drapeau, et c'est un compromis assumé : ajouter un
drapeau touche la structure `Options`, l'analyseur d'arguments et le texte d'aide, soit trois
endroits de plus dans un fichier partagé. Le drapeau a sa place avec le `ChunkMap` et le système
de tickets, quand la génération cessera d'être une pièce isolée.

### Le piège trouvé au passage : deux numérotations de biomes

Le générateur écrit des **index du registre de blocs** (`BlockRegistry::find_biome`). Le paquet
de chunk lit des **identifiants du codec de registres**, l'ordre que le serveur envoie au client.
Rien ne garantit que les deux coïncident, et si elles divergent le monde s'affiche dans les
couleurs d'un autre monde sans qu'aucune ligne ne le dise.

`GeneratedWorld` construit la table de traduction **par nom**, une fois, et **refuse de démarrer
en nommant le biome fautif** si le codec ne connaît pas un biome que le registre nomme. Sur le
pack 1.20.1 les deux ordres coïncident — mais c'est une mesure, pas une hypothèse, et elle est
vérifiée à chaque démarrage.

### Ce qui n'est pas prouvé côté serveur

Le serveur charge le générateur et démarre (vérifié). La **génération à l'arrivée d'un joueur**
n'est pas prouvée de bout en bout : il n'existe pas de client sans rendu dans ce dépôt, et rien
ne génère de chunk avant qu'un joueur se connecte. Ce qui est prouvé, c'est l'appel exact que le
serveur fait — `ChunkPipeline::take()` — par un test unitaire qui vérifie qu'il rend un chunk
non vide, que le chunk quitte le cache, et que `trim()` oublie réellement ce qui est loin.

---

## 7. Ce qui n'est pas fait, et nommé

1. **Rien ne tourne en parallèle.** La règle d'exclusion géométrique est écrite et testée, le
   pipeline est mono-thread. Argument mesuré au § 3.
2. **L'ordre dans lequel les neuf chunks d'un voisinage sont décorés n'est pas celui du jeu, et
   n'est pas mesuré.** Notre pipeline décore un chunk quand on le lui demande ; le jeu décore
   selon son propre ordonnancement de statuts. Une veine qui déborde écrase, ou non, selon qui
   passe en premier. `docs/provenance/features.md` avait déjà nommé cette hypothèse pour ses
   65 blocs manquants ; elle vaut ici aussi, et elle n'est toujours pas tranchée.
3. **Les deux échantillons ne sont pas les mêmes.** `ov_features` compare 44 chunks (13 249
   minerais), `ov_genparity` 48 chunks (18 057). Les deux viennent du même monde de référence et
   de la même graine, mais les deux outils choisissent leurs chunks différemment. 99,509 % et
   82,406 % sont donc **deux taux**, pas une différence appariée. Le § 5.2 attribue l'écart avec
   le diagnostic pris sur le seul échantillon de `ov_genparity`, qui lui est cohérent.
4. **`structure_starts` ne fait rien**, et `initialize_light` / `light` / `spawn` n'existent pas
   du tout dans l'échelle. La lumière du serveur est calculée ailleurs (`relight_chunk`) et n'a
   pas été déplacée ici.
5. **114 placed features nommées par un biome ne se construisent pas** (`ov_features --missing`)
   — `would_survive`, `solid`, `carving_mask`, `BIOME_INFO_NOISE`. Inchangé par ce travail.
6. **Le cache du pipeline est borné par `trim`, pas par une politique.** Le serveur garde huit
   chunks de rayon autour du dernier chunk demandé. C'est un chiffre raisonnable, pas un chiffre
   mesuré.

---

## 8. Rejouer les mesures

```bash
cmake --preset macos-debug -DOV_BUILD_CLIENT=OFF
cmake --build --preset macos-debug --parallel 2

# les minerais dans un monde que nous générons nous-mêmes
./build/macos-debug/bin/ov_genparity --chunks=48 --per-region=4
# le bras de contrôle : le terrain seul, et le plafond qu'il impose
./build/macos-debug/bin/ov_genparity --chunks=48 --per-region=4 --no-features

# les mêmes minerais mesurés sur le terrain DU JEU, pour l'autre moitié de la comparaison
./build/macos-debug/bin/ov_features --chunks=48

# les contrôles de non-régression — ils n'exécutent pas le pipeline (§ 5.6)
./build/macos-debug/bin/ov_carveparity --chunks=1200
./build/macos-debug/bin/ov_surfparity  --chunks=250 --per-region=4
./build/macos-debug/bin/ov_parity --terrain --carvers --chunks=600 \
    --world=run/reference-1234567890/world

# le serveur, avec un vrai monde généré
OV_WORLDGEN_SEED=1234567890 ./build/macos-debug/bin/ov_dedicated --world=run/generated
```

## 9. Sources

* **L'échelle des statuts et leurs rayons** : la documentation de la génération de monde de
  `minecraft.wiki` (`Chunk format` / `Java Edition level format`, section *Status*), qui liste
  les statuts par lesquels un chunk passe et dit que `features` est celui où un chunk écrit chez
  ses voisins. Aucun code de jeu n'a été lu.
* **L'ordre des étages** (`bruit → surface → carvers → features`) :
  `docs/provenance/ordre-des-etages.md`, déjà établi et mesuré.
* **La graine de décoration, l'ordre partagé et le pipeline de placement** :
  `docs/provenance/features.md`, inchangés par ce travail.
* **Tous les chiffres de ce document** : mesurés contre `run/reference-1234567890`, généré par le
  vrai serveur 1.20.1 à la graine 1234567890. Aucun ne vient d'un raisonnement.

## 10. Pièges pour les autres agents

* **`Chunk::heightmap()` replie les deux cartes `_WG` sur la fente zéro**, c'est-à-dire sur
  `WORLD_SURFACE`. C'est juste pour `WORLD_SURFACE_WG` et **faux** pour `OCEAN_FLOOR_WG`, qui
  répondrait le sommet de l'eau au lieu du fond marin. Inoffensif là où il vit — rien ne stocke
  de carte `_WG` — et pas inoffensif dans un `FeatureLevel`, parce que l'ancre `heightmap` de
  `height_range` demande `OCEAN_FLOOR_WG` par son nom. `pipeline.cpp` résout les deux
  explicitement.
* **Les références d'un `std::unordered_map` survivent au réhachage, celles d'un `vector` non.**
  Le pipeline garde une référence sur un chunk pendant qu'il en fabrique d'autres ; c'est la
  raison du choix du conteneur et pas une préférence de style.
* **Un chunk qui vient de décorer n'est pas fini.** Ses huit voisins vont encore écrire dedans.
  C'est la différence entre `features` et `full`, et c'est ce qui rend `take()` sûr.
* **La numérotation des biomes du registre de blocs n'est pas celle du codec de registres.**
  Elles coïncident en 1.20.1, ce qui rend l'hypothèse invisible tant qu'elle tient. Traduire par
  le nom coûte une table de 64 entrées construite une fois.
* **`ov_features` et `ov_genparity` ne mesurent pas la même chose et ne doivent pas être
  confondus.** Le premier isole le placement en lui donnant le terrain du jeu ; le second mesure
  le monde. Le premier est le bon chiffre pour juger le placement, le second pour juger le monde.
