# Lumière incrémentale — un bloc change, la lumière se répare autour de lui

Jalon M3, ligne « Suppression incrémentale de la lumière ». Le code vit dans
`src/ov_world/include/ov/world/light_engine.hpp` et `src/ov_world/src/light_engine.cpp` ;
le serveur l'appelle depuis `server.cpp` (balises `── light ──`), le client
`ov_voxel` depuis `apps/ov_voxel/src/session.cpp`.

---

## 1. Ce qui existait

Le moteur précédent (`src/ov_server/src/relight.cpp`, toujours là pour le Nether, l'End
et le superflat) recalculait **tout** :

- chaque chunk touché par une écriture était mis dans un ensemble, et le tick refaisait
  la lumière du ciel sur le 3×3 autour de lui puis la lumière de bloc de ses neuf
  chunks, depuis zéro ;
- la lumière de bloc **s'arrêtait au bord du chunk** : une torche contre une frontière
  n'éclairait pas le voisin ;
- les chunks **générés** arrivaient **sans aucune lumière** (ni ciel ni bloc) — seul un
  geste du joueur à moins d'un chunk leur en donnait, par le 3×3.

`performance-tick.md` avait déjà sorti ce recalcul du thread réseau ; restait qu'un bloc
cassé coûtait un flood fill de neuf chunks.

## 2. Les règles, et d'où elles viennent

Source documentaire : la page **« Light »** du Minecraft Wiki
(`https://minecraft.wiki/w/Light`, section Java Edition), lue le 2026-09-11. Ce qu'elle
dit et que le moteur applique :

1. *« The block light level decreases by one for each meter (block) of taxicab distance
   from the light source »* — un niveau perdu par pas, en six directions.
2. *« When sky light of a level of 15 spreads down through a transparent block, the level
   remains unchanged. When it spreads horizontally or upwards, it reduces its level by
   1. »* — le ciel à 15 tombe sans perte ; c'est la seule exception.
3. Les blocs **filtrants** (eau, glace, feuilles, toile, slime, miel, spawner, lave,
   balise…) *« decrease sky light by 1 level (but do not affect block light) »*. Dans le
   moteur : un tel bloc **interrompt la chute sans perte** du ciel à 15 ; ailleurs il
   perd un niveau comme n'importe quel bloc, ce qui est exactement « ne change rien à la
   lumière de bloc ». C'est l'option `LightRules::filtering_dims_sky` ; son réglage est
   décidé par la mesure contre vanilla (§ 6.3).
4. Les formes (dalles, escaliers, pistons, terre labourée…) qui ne laissent passer la
   lumière que dans certaines directions : **non modélisées**, nommées au § 7.

Ce que le moteur **ne suppose pas** : quel bloc arrête la lumière et combien chaque état
en émet. Ce sont les deux tables **mesurées** du registre (`light_opacity`, 1003 blocs ;
`light_emission`, 23 282 états), lues une fois dans une table d'un octet par état.

L'algorithme de suppression à deux files est un classique du voxel, décrit publiquement
depuis longtemps (par exemple « Fast Flood Fill Lighting in a Blocky Voxel Game », Seed of
Andromeda, 2014). **Aucun code tiers n'a été lu** : en particulier pas Starlight, dont la
licence n'est pas sur la liste sûre du `CLAUDE.md`.

## 3. L'algorithme

La lumière est un **point fixe** : chaque case vaut le maximum de ce qu'elle émet et de ce
que chaque voisine lui donne. Une modification ne dérange ce point fixe que près
d'elle ; le moteur le répare là.

- **Augmentation.** Un parcours en largeur depuis les cases qui peuvent donner plus que
  leurs voisines ne tiennent. Une case ne reçoit que si elle n'arrête pas la lumière.
- **Diminution (deux files).** La case modifiée est éteinte et retient ce qu'elle
  valait. Toute voisine dont le niveau *a pu venir d'elle* (niveau ≤ ce qu'elle
  donnait) est éteinte à son tour et entre dans la file de suppression ; une voisine qui
  tient **plus** que ce qu'elle a pu recevoir est éclairée par autre chose et entre dans la
  file d'augmentation, pour reboucher le trou depuis l'extérieur. Pour le ciel, « a pu
  venir d'elle » inclut la chute sans perte : une colonne coiffée s'éteint jusqu'au sol en
  une seule vague.
- **Réensemencement.** Une case éteinte qui émet (une torche traversée par la vague) ou
  qui est au sommet du monde (le ciel au-dessus lui donne 15) récupère sa propre valeur
  après la suppression, avant l'augmentation.
- **Par lots.** `block_changed` ne fait qu'empiler ; `propagate` traite tout ce qui s'est
  accumulé depuis le dernier appel en une suppression puis une augmentation. Un `/fill`
  de 32 768 blocs est un lot, pas 32 768 inondations.

Pourquoi le résultat est exact : après la suppression, **aucune case ne dépasse sa
valeur finale** — une case qui doit baisser tenait sa lumière d'une chaîne remontant à
une case modifiée, et la vague suit exactement cette chaîne ; et **toute case qui doit
monter** est atteignable depuis une case de la file d'augmentation (une voisine gardée,
une source réensemencée, ou une voisine de la case modifiée). La preuve expérimentale est
au § 6.1.

Aucune allocation en régime établi : les files vivent dans le moteur et gardent leur
capacité d'un appel à l'autre. La recherche de chunk passe par un cache direct de 64
entrées, vidé à chaque appel parce qu'un chunk peut être déchargé entre deux.

## 4. L'arrivée d'un chunk : seul, puis cousu

Le moteur incrémental suppose que la lumière **est déjà** au point fixe avant le geste.
Un chunk qui arrive doit donc être éclairé correctement, sans quoi la première réparation
part d'un état faux :

1. `light_chunk` : le chunk **seul** — émetteurs, puis ciel direct colonne par colonne
   (15 jusqu'au premier bloc opaque ou filtrant, puis la « queue » filtrée), puis
   l'inondation à l'intérieur du chunk. Seules les cases à 15 à côté d'une colonne dont le
   15 s'arrête plus haut sont semées : une case à 15 entourée de 15 ne donne rien.
2. `stitch` : les quatre frontières avec les voisins **chargés**, dans les deux sens. Une
   paire de cases de part et d'autre n'est semée que si l'une dépasse l'autre de plus
   d'un ; deux sections uniformes proches sont sautées d'un coup.

Pourquoi deux temps suffisent : **ajouter un chunk ne peut qu'ajouter de la lumière**. Il
remplace de l'absent — qui ne donne ni ne prend — par des cases réelles ; aucune case
voisine ne peut y perdre. Partant de deux points fixes partiels, une seule augmentation
depuis la frontière atteint le point fixe de l'union.

Un chunk **déchargé** laisse chez ses voisins la lumière qu'il leur donnait, comme vanilla
qui la sauvegarde. Le moteur ne l'efface pas ; nommé au § 7.

## 5. Le branchement

**Serveur** (`server.cpp`, Overworld) :

- un `world::LightEngine` à côté de la `ChunkMap`, utilisé seulement sous `chunk_mutex`,
  comme la carte qu'il éclaire ;
- `light_arrived(pos, keep_sky)` après chacune des trois publications : lecture disque
  (les deux lumières recalculées — pourquoi le ciel écrit par vanilla n'est plus gardé :
  § 6.3), génération synchrone et chunks générés par les ouvriers. Les modifications en attente
  passent d'abord, pour que la couture ne propage jamais une lumière déjà périmée ;
- les quatre écrivains (geste du joueur, drain du tick, `/fill`, `apply_block_change`)
  appellent `block_changed` ; `flush_tick_writes` appelle `propagate` une fois par tick,
  dans la phase `relight` du profil ;
- le Nether et l'End ont chacun leur moteur, **sans ciel** (`kNoSkyLight`) : un chunk qui
  arrive est éclairé seul puis cousu, un geste du joueur est réparé sur-le-champ, le drain
  d'un tick en un seul lot. Leur lumière de bloc traverse enfin les frontières de chunk ;
- **`OV_LIGHT_FULL=1`**, lu une fois au démarrage comme `OV_NETHER`, sert **uniquement à
  la mesure** : la phase `relight` refait alors le 3×3 de chaque chunk touché depuis zéro,
  comme avant ce moteur. Un seul binaire mesure ainsi les deux côtés du banc (§ 6.5) — la
  première idée, reconstruire l'ancien `server.cpp` à côté, demandait d'écraser des
  fichiers suivis de l'arbre de travail.

**Client** (`apps/ov_voxel/src/session.cpp`) : le serveur n'envoie aucune lumière après
un geste — le vrai client éclaire lui-même ses modifications. Le nôtre ne le faisait pas :
une torche posée restait noire jusqu'au prochain envoi du chunk. Il passe maintenant chaque
`Block Update` au même moteur, et remaille les sections dont la lumière a changé
(`changed_sections()`) et leurs six voisines.

## 6. Les preuves

### 6.1 Équivalence au recalcul complet

`test_ov_world "[light]"` (`src/ov_world/tests/test_light_engine.cpp`). La référence
est écrite **sans rien partager avec le moteur** : des tableaux denses sur toute la
région, toutes les sources semées, et un remplissage par seaux du niveau 15 au niveau 1
(une case est définitive la première fois qu'elle sort à son niveau). Elle ne connaît que
les règles du § 2 — ni colonnes, ni vagues de suppression, ni frontières de chunk. Elle
compare **chaque demi-octet** des deux tableaux de chaque section, et compte aussi comme
différence un tableau resté matérialisé alors qu'il est uniforme (ce serait un autre
paquet sur le fil).

Les gestes sont tirés au hasard autour de la surface, dans des lots d'un à une vingtaine
de blocs : casser, poser de la pierre, du verre, des feuilles, de l'eau, de la glace,
une barrière, une torche, une torche de redstone, une pierre lumineuse, une lanterne
aquatique, de la lave, un bloc de magma ; 8 % des lots creusent ou bouchent un puits de
3 à 22 blocs (les ouvertures de ciel), 8 % dispersent jusqu'à treize gestes dans un
carré de 9×9.

| Terrain | Lots | Comparaisons | Différences |
|---|---|---|---|
| plat 3×3 chunks, ciel filtré puis non filtré | 2 × 600 | après **chaque** lot | **0** |
| `run/lab`, 4×4 chunks, deux réglages | 2 × 2 000 | tous les 200 lots | **0** |
| `run/saves/New World` (vrai monde 1.20.1), 4×4 chunks, deux réglages | 2 × 2 000 | tous les 200 lots | **0** |
| **témoin** : les mêmes gestes, passe de suppression coupée (plat, lab, vrai monde) | 600 / 400 / 400 | fin | **> 0**, comme il se doit |

Plus quatre cas nommés : une torche contre une frontière de chunk (14 dans sa case, 13
dans le chunk voisin, 0 après l'avoir cassée) ; un puits ouvert, coiffé d'eau puis de
pierre, et le sommet du monde lui-même ; neuf chunks arrivant un à un dans un ordre qui
laisse des trous, éclairés seuls puis cousus, identiques au recalcul de la région après
**chaque** arrivée ; une dimension sans ciel. Total : 8 526 assertions, toutes vertes.

### 6.2 Après un geste, contre le vrai serveur

`scripts/measure_light_edits.py` (voie java, port 25602) copie `run/saves/New World`
sous `.scratch/light-lab/`, le fait charger par le vrai serveur 1.20.1 — qui l'éclaire :
les 64 chunks relus portent alors `isLightOn` —, y construit des scènes (une boîte creuse
à cheval sur x = 16 avec une torche, une salle souterraine à la pierre lumineuse, un bloc
de pierre à percer, une canopée de feuilles persistantes, un bassin entre quatre vitres),
sauvegarde (`before/`), fait les gestes (toits ouverts, torche déplacée, pierre lumineuse
cassée, lanterne posée, puits percé jusqu'au ciel, canopée recouverte, bassin couvert),
sauvegarde encore (`after/`). Toutes les commandes ont été acceptées par le jeu.

`test_ov_world "[.light-vanilla-edits]"` éclaire `before/` avec notre moteur, retrouve
**515 blocs changés** en comparant les deux sauvegardes, les applique en incrémental, puis
compare, dans les 36 chunks dont les huit voisins sont chargés, **chaque case dont vanilla
a changé la lumière** entre les deux sauvegardes. Le témoin est notre lumière d'avant les
gestes.

| Cases changées par vanilla | Lumière de bloc (1 497) | Lumière du ciel (1 241) |
|---|---|---|
| filtrage du ciel coupé (`filtering_dims_sky=0`) | **1 497/1 497** (témoin 9) | 844/1 241 (témoin 5) |
| filtrage du ciel actif (`filtering_dims_sky=1`) | **1 497/1 497** (témoin 9) | **1 182/1 241** (témoin 319) |

Relu case par case dans les sauvegardes du jeu, le bassin posé sous le ciel vaut eau 14,
13, 12 (puis 12, par les vitres) et la canopée posée feuille 14, air 13, 12, 11 : pour des
blocs **posés**, le jeu filtre exactement comme le dit le wiki.

### 6.3 Contre la lumière que vanilla a écrite

**Un faux oracle d'abord.** La première comparaison a porté sur `run/saves/New World`,
un vrai monde 1.20.1 (`DataVersion` 3465). Le résultat semblait parler — lumière de bloc
1 966 080/1 966 080 identique, lumière du ciel 148 733/172 032 — mais **aucun chunk de ce
monde ne porte `isLightOn`** : les 64 chunks relus n'ont même pas la clé. Le jeu ne
l'écrit que lorsque la lumière du chunk est terminée ; sans elle, il la recalcule au
chargement. Ce que le fichier contient n'est donc pas la lumière du jeu mais un état
intermédiaire (du ciel stocké dans deux sections par chunk, `y3` et `y4`, et rien
ailleurs), et une égalité de lumière de bloc sur un tel fichier peut n'être que deux
tableaux vides. Écarté.

**Le vrai oracle** est le même terrain une fois chargé, éclairé et sauvegardé par le
vrai serveur : `.scratch/light-lab/before` (§ 6.2), relu par
`test_ov_world "[.light-vanilla]"`. Notre moteur éclaire une copie depuis les seuls blocs,
et chaque case des 36 chunks intérieurs est comparée à ce que le jeu a écrit :

| 36 chunks intérieurs | Ciel, dans les sections où vanilla l'a stocké | Bloc |
|---|---|---|
| filtrage coupé | **741 427/749 568 (98,9 %)** | **3 534 214/3 538 944 (99,87 %)** |
| filtrage actif | 726 868/749 568 (97,0 %) | idem |

**Deux régimes chez vanilla.** Le terrain naturel que le jeu a éclairé au chargement
laisse passer le ciel à 15 à travers deux à quatre feuilles de chêne et dans une mare d'un
bloc — le filtrage coupé ; ses **mises à jour** après un geste filtrent (§ 6.2). La lumière
que le jeu stocke n'est donc pas un point fixe unique : elle dépend de l'histoire du chunk.
Notre moteur n'en a qu'un, par construction. Le réglage retenu est **coupé** : il colle au
terrain que tout joueur voit (98,9 % contre 97,0 %), au prix des cases proches d'un geste
près de l'eau ou des feuilles (844 contre 1 182 sur 1 241). L'écart vers le fond des océans
(nous plus clairs de +1 à +15 sur 5 110 cases d'eau) dit que l'éclairage initial filtre
quand même en profondeur — la règle exacte n'est pas établie.

La lumière de bloc diffère sur 4 730 cases (air 3 788, eau 729, lave 193, lichen
lumineux 18) : non analysées.

**Ce que le serveur en tire au chargement.** Le jeu ne stocke le ciel que dans **297 des
1 536 sections** d'un monde qu'il vient d'éclairer ; une section sans tableau veut dire
« comme au-dessus », que ce format lit comme noir (681 sections des chunks comparés, où
nous éclairons 1,77 million de cases). L'ancienne règle — garder le ciel d'une sauvegarde
vanilla s'il en porte — servait donc de l'air noir au-dessus du relief. Le serveur
recalcule maintenant les deux lumières de tout chunk lu, puis le coud : 98,9 % d'accord
avec le jeu là où le jeu a écrit, un état dont la réparation incrémentale peut partir, et
plus de sections noires.

### 6.4 Le coût d'un geste

`test_ov_server "[.relight-bench-incremental]"` (`tests/test_relight.cpp`) : deux copies
d'un carré de 5×5 chunks lu sur disque, la même suite de gestes aléatoires près de la
surface (casser, poser de la pierre, une torche, une pierre lumineuse) dans le 3×3 du
milieu — le cas pour lequel l'ancien recalcul avait tout son voisinage. L'ancien moteur
refait `relight_after_edit` sur le chunk touché, le nouveau appelle `block_changed` puis
`propagate`. Build Debug, machine partagée avec d'autres compilations :

| Monde | Recalcul 3×3 (avant) | Incrémental (après) |
|---|---|---|
| `run/lab` (banc `ov_lab`, 25 chunks, 300 gestes) | p50 **40,6 ms**, p99 275,8 ms, max 411,2 ms | p50 **30 µs**, p99 14,2 ms, max 35,6 ms |
| vrai monde 1.20.1 (`run/saves/New World`, 21 chunks, 222 gestes) | p50 **58,3 ms**, p99 259,5 ms, max 351,0 ms | p50 **339 µs**, p99 12,7 ms, max 26,3 ms |

Le p50 gagne trois ordres de grandeur sur le banc et deux sur le vrai monde. Le p99 reste
à une dizaine de millisecondes en Debug : ce sont les gestes qui ouvrent ou ferment une
colonne de ciel, dont la vague descend jusqu'au fond de la colonne et s'étale de quinze
blocs à chaque étage — le travail est proportionnel aux cases qui changent vraiment, et
une colonne entière en change des milliers.

### 6.5 Le tick, avec un vrai client qui casse et pose

`scripts/bench_play.py --world=lab --seconds=60 --edit-rate=4` : le banc `ov_lab`, une
sonde protocole 763 qui marche et casse puis pose un bloc quatre fois par seconde. Le
même binaire Debug des deux côtés, l'ancien recalcul obtenu par `OV_LIGHT_FULL=1`, et les
séries **alternées** (ancien, nouveau, ancien, nouveau) pour que la charge de la machine
— 11 à 17, quatorze autres agents — pèse sur les deux. Une première série, lancée pendant
que le binaire se reconstruisait, avait mesuré deux fois le nouveau moteur ; elle est
écartée (sa phase `relight` « avant » tournait à 32 µs, ce qui l'a trahie).

| Phase `relight` du tick, 60 s | Recalcul 3×3 (`OV_LIGHT_FULL=1`) | Incrémental |
|---|---|---|
| série 1 | 374 passages, p50 **9 472 µs**, p99 32 768 µs, max 63 ms, total 4 197 ms | 347 passages, p50 **31 µs**, p99 656 µs, max 2,3 ms, total 23 ms |
| série 2 | 366 passages, p50 **10 240 µs**, p99 38 912 µs, max 88 ms, total 4 743 ms | 349 passages, p50 **28 µs**, p99 208 µs, max 1,9 ms, total 16 ms |
| ticks lents dont elle est la plus lourde | 3 sur 61, 4 sur 65 (deux au-dessus de 50 ms) | 0 sur 76, 0 sur 52 |

La lumière coûtait **4,2 à 4,7 s de tick par minute** de jeu à ce rythme ; elle en coûte
**16 à 23 ms**. Ce que le banc ne montre **pas** : un p99 du tick entier plus bas (467 et
492 ms avant, 606 et 754 ms après) ni un cassage plus rapide (p50 0,9 et 1,8 ms avant, 3,0
et 4,3 ms après). À cette charge, le p99 du tick est fait par les autres phases —
apparition naturelle, sauvegarde, entités — et par la machine ; et le `Block Update` part
du thread réseau **avant** toute lumière depuis `performance-tick.md`, si bien que le
cassage n'a jamais attendu ce moteur. Les deux écarts vont dans le sens contraire d'une
série à l'autre de la charge et ne sont pas attribués.

## 7. Ce qui reste, nommé

- **Les formes directionnelles** : dalles, escaliers, pistons, capteurs de lumière,
  tables d'enchantement, terre labourée, chemins, neige, cadres de portail de l'End, qui
  ne laissent passer la lumière que par certaines faces (wiki « Light »). Le moteur ne
  connaît qu'« arrête » ou « laisse passer », par bloc — c'est la table mesurée du
  registre, et c'est là que la prochaine mesure contre vanilla trouvera ses écarts.
- **Le filtrage du ciel par l'eau et les feuilles** (§ 6.3) : le jeu ne filtre pas au
  premier éclairage d'un chunk et filtre dans ses mises à jour. Reproduire les deux
  demanderait de garder l'histoire d'une case ; le moteur garde un point fixe, réglé sur
  le terrain naturel. La profondeur exacte à laquelle l'éclairage initial se met à
  perdre de la lumière sous l'eau est la prochaine mesure à faire.
- **Un chunk déchargé** laisse chez ses voisins la lumière qu'il leur donnait, comme le
  jeu qui la sauvegarde. Le recalcul de l'ensemble chargé l'enlèverait ; le moteur ne le
  fait pas, et une réparation ultérieure près de cette lumière la traite comme n'importe
  quelle autre.
- **Le client `ov_voxel`** applique les règles de l'Overworld : il ne connaît pas encore
  d'autre dimension (ni `ov_netclient` ni la session n'en portent une).
- **Le superflat** garde son ancien calcul dans sa génération, refait ensuite par
  l'arrivée du chunk : du travail en double, sur un terrain trivial. `relight.cpp` reste
  aussi pour les bancs de `test_relight.cpp`, dont il est le « avant ».
- **La mesure complète** du § 6.1 (2 000 lots par vrai monde et par réglage, 600 lots
  sur terrain plat comparés à chacun) prend plus de vingt minutes en Debug : elle est
  cachée derrière `[.light-real-thousands]`. La suite unitaire que tout le monde lance —
  et que la CI lance — en garde une version courte : 200 lots par vrai monde comparés
  tous les 100, 150 sur terrain plat comparés tous les 3, et les trois témoins.
