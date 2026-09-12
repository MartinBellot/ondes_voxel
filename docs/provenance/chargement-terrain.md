# Le chargement du terrain — du « Loading terrain » permanent à un terrain qui suit le joueur

Le rapport de départ, d'une session solo (`ov_voxel --singleplayer`), build **Debug** :

```
OndesVoxel asked to join; spawn area 0% ready — refused for now      (une fois par seconde)
Spawn area ready in 21.7 s
[ov-gen0..3/ERROR] [worldgen] eight tree positions collided in one hash bucket …
[ov-tick/WARN] can't keep up — is the server overloaded?              (toutes les quelques secondes)
autosave de 18 à 87 chunks toutes les 30 s
```

« Le chargement de la carte est injouable : en explorant, le client affiche Loading terrain en
permanence, les chunks autour du joueur ne sont pas chargés. »

> **En bref.** La génération passait **75 % de son temps** à rechercher, pour chaque chunk et
> chaque étage, des surfaces que le chunk voisin venait de rechercher. Mémoïsées, plus un cache
> de terrain partagé entre les blocs de génération : **436 ms par chunk au lieu de 1 535** en
> Release (Debug : 2,8 s au lieu de 11,2), **monde identique au bit près** — un condensé de
> chaque bloc, biome, heightmap et entité de bloc le prouve, et les quatre outils de parité
> rendent les mêmes chiffres. Avec l'ordonnancement au plus proche et les ouvriers passés de la
> classe UTILITY à USER_INITIATED : spawn prêt en **3,6 s** au lieu de 10,2, distance de vue remplie en
> **15 à 28 s** après un téléport de 1000 blocs au lieu de trois minutes, et **plus un seul
> chunk manquant sous le joueur au sprint ni en vol créatif**. La sauvegarde automatique a
> quitté le tick (1 372 ms → 16 ms de tick sur une session). **Pour jouer : le build Release**
> (§ 6) — le Debug génère le terrain quatre fois plus lentement, même après ce travail.

---

## 1. Où partait le temps

### 1.1 Le profil de la génération

`ov_gendet --serial-only --side=2` (64 chunks, graine 1234567890, un thread), binaire Release
d'avant, échantillonné 30 s par `sample` :

| où | échantillons | part |
|---|---:|---:|
| `AquiferSampler::compute` | 19 741 | **79 %** |
| … dont `preliminary_surface` | 18 827 | **75 %** |
| `generate_carvers` (un second aquifère, neuf) | 4 992 | 20 % |
| biomes | 1 305 | 5 % |
| règles de surface | 719 | 3 % |
| décoration, structures, arbres | ~370 | < 2 % |

Feuilles : `ImprovedNoise::noise` 16 401, `PerlinNoise::value` 3 364 — du bruit, échantillonné
pour trouver des surfaces. Le diagnostic des arbres (« eight tree positions collided ») coûte
11 échantillons sur 25 010 : négligeable en temps, mais pas en journal — 8 832 lignes ERROR en
quatre minutes de banc. Il appartient au chantier des features et n'est pas modifié ici.

**Le mécanisme.** Pour décider du fluide d'une cellule d'aquifère, le jeu lit la « surface
préliminaire » de treize colonnes autour de son centre (documentation de l'aquifère,
`docs/provenance/aquiferes.md`) — et chaque surface est une recherche vers le bas, de 8 en 8
blocs depuis le haut du monde, d'une fonction de densité lourde en splines. Notre
`AquiferSampler` mémoïsait ces surfaces **par échantillonneur**, et il en naît un par chunk et
par étage (le bruit, puis les carvers). Une cellule touche quatre chunks : la même colonne était
recherchée jusqu'à huit fois.

### 1.2 La référence « avant »

Binaires de `0b7f298` (plus le seul condensé), mis de côté avant toute modification
(`.scratch/before/`). `uptime` pris avant et après chaque série ; la machine est partagée par
une quinzaine d'agents.

| série | résultat | charge (1 min) |
|---|---|---|
| Release, 1 thread, 64 chunks | 98,3 s — **1 535 ms/chunk** — condensé `91441dd6c78f21b3` | 13 → 20 |
| Release, 144 chunks, 4 ouvriers UTILITY | 381 s — **×1,11** sur le bras série, 0,38 chunk/s | 27 |
| Debug, 1 thread, 16 chunks | 179 s — **11 208 ms/chunk** — condensé `be05d77e3959e2da` | 27 → 15 |

**Quatre ouvriers achetaient 11 %**, et **11 s par chunk en Debug** : les 21,7 s de
préparation du spawn du rapport sont ce qu'un build Debug coûte, avant même la machine.

---

## 2. Ce qui a été fait

### 2.1 La génération, sans changer un bloc

La règle : chaque optimisation laisse le monde **identique au bit près**, et la preuve est un
nombre. `ov_gendet` imprime désormais un **condensé FNV-1a** de tout ce que la génération
décide — chaque bloc, chaque cellule de biome, les quatre heightmaps stockées, les entités de
bloc — dans l'ordre des clés. Deux binaires qui génèrent le même monde impriment le même
condensé.

1. **Les surfaces et les statuts de l'aquifère, mémoïsés pour toute la pile**
   (`aquifer.hpp`). Fonctions pures de la graine et de la position ; gardées sur l'`Aquifer`
   (un par pile worldgen, donc un thread) au lieu de l'échantillonneur jetable. Bornées par
   effacement : une entrée effacée est recalculée à la même valeur. Le test « the answer does
   not depend on the order of the questions » compare maintenant un `Aquifer` froid, dans
   l'autre ordre, et l'`Aquifer` chaud : mêmes réponses.
2. **Le terrain partagé entre les blocs de génération** (`TerrainCache` dans `pipeline.hpp`,
   `terrain_cache.hpp`). Un bloc de 4×4 chunks décorés exige un 8×8 de terrain ; les trois
   quarts de l'anneau sont le terrain des blocs voisins, régénéré à l'identique quand leur tour
   vient. Jusqu'aux carvers inclus un chunk est une fonction pure de la graine et de sa
   position : un cache commun aux piles d'un monde change combien de fois le terrain est
   calculé, jamais ce qu'il est. La copie se fait **sous le verrou du cache**, parce que copier
   une section marque sa source partagée (copy-on-write) et que deux ouvriers copiant le même
   chunk écriraient ce drapeau ensemble. Le verrou garde le cache, pas le monde : aucun de ces
   chunks n'est visible d'un joueur.
3. **Les coins interpolés** (`density.cpp`, `Interpolated`) : une table à adressage direct au
   lieu d'une `unordered_map`, et les huit coins de la cellule précédente gardés — la passe de
   bruit monte chaque colonne, sept blocs sur huit demandent la cellule du bloc d'avant.

### 2.2 L'ordonnancement

- **Le plus proche d'abord, au moment où le travail commence.** Le tick remet à chaque tour la
  liste de ce que les tickets veulent et que la carte n'a pas, du plus proche au plus loin
  (`AsyncChunkSource::prioritise`) ; un ouvrier libre prend le premier bloc de cette liste que
  personne ne génère. Avant, chaque bloc était mis en file dans l'ordre où il avait été demandé
  la première fois et n'en sortait jamais : un joueur qui volait attendait derrière le terrain
  qu'il venait de quitter.
- **L'annulation gratuite** : un bloc qui n'est plus voulu n'est pas dans la liste suivante et
  n'est jamais commencé. Un bloc en cours se termine (on n'interrompt pas une décoration).
- **La classe d'ordonnancement des ouvriers** : `Generation`, USER_INITIATED, au lieu de
  `Worker`, UTILITY. Apple définit USER_INITIATED pour le travail « que l'utilisateur a lancé
  et qui exige un résultat immédiat […] nécessaire pour continuer l'interaction », et UTILITY
  pour celui qui « n'exige pas de résultat immédiat », en équilibrant performance et énergie
  (§ 8) : le terrain qu'un joueur attend est le premier cas. Ce qu'UTILITY coûte ici est
  **mesuré, pas déduit** : des ouvriers à ~2 % d'un cœur sous charge
  (`performance-tick.md` § 4.3), et les bras UTILITY / USER_INITIATED du § 3.1. Apple ne dit
  rien des cœurs d'efficacité, et ce fichier ne le prétend pas. `OV_WORKER_QOS=utility` rend
  l'ancienne classe pour la comparaison.
- `test_streaming.cpp` fixe les deux propriétés avec un générateur factice qui enregistre
  l'ordre : un ouvrier libéré prend le meilleur bloc de la **dernière** liste, un bloc retiré
  n'est jamais généré, aucun bloc n'est généré deux fois.

### 2.3 Le client

- **« Loading terrain » seulement à l'entrée d'un niveau** — la connexion, une réapparition, un
  changement de dimension (les paquets Login et Respawn portent la dimension) — jusqu'à
  l'arrivée du chunk sous le joueur. D'où vient cette règle, et ce qui n'en est pas vérifié :
  § 8.
  Jamais en explorant : le monde reste à l'écran, et le joueur ne bouge pas tant que le sol
  n'est pas là (le comportement d'avant, moins l'écran noir).
- **« Preparing spawn area: N % » par la réponse de statut.** Après l'unique refus de
  connexion, le client demande l'avancement par un ping de statut — clé `ondesSpawnProgress`
  de la réponse JSON, que le client du jeu ignore — quatre fois par seconde, sur une socket à
  lui, et ne frappe à la porte de la connexion qu'une fois le spawn prêt. Des octets, comme
  tout le reste (principe 2) ; le serveur ne journalise plus un refus par seconde. Choix
  documenté : garder la connexion en attente aurait demandé de sortir tout le gestionnaire de
  connexion de `server.cpp` pour le terminer plus tard, et un client du jeu aurait attendu sans
  voir d'avancement.

### 2.4 La sauvegarde automatique quitte le thread de tick

`save_world` encodait chaque chunk sale en NBT, le compressait, et relisait puis réécrivait
chaque fichier de région — sur le tick. Elle prend maintenant un **instantané** de chaque chunk
sale (`Chunk::snapshot`, un pointeur de section chacun) et une copie des deux files de ticks
**une fois** (l'ancien code les recopiait entières pour chaque chunk, au même tick), et remet
le tout à `ChunkSaver` (`chunk_saver.hpp`) : un thread, les mêmes octets regroupés par région
de la même façon, l'écriture atomique de région inchangée. Deux règles le gardent juste :

- **un seul thread, dans l'ordre** : deux sauvegardes d'un même chunk arrivent sur le disque
  dans l'ordre où elles ont été prises ;
- **épinglé jusqu'à l'écriture** : un chunk dont une sauvegarde est en vol n'est pas évincé
  (compteur par chunk) — évincé puis relu sur disque avant l'arrivée de ses octets, il
  reviendrait tel qu'avant la modification.

`/save-all` et l'arrêt attendent le thread ; une sauvegarde automatique jamais.

---

## 3. Avant / après

Première série, graine 1234567890 (`ov_gendet`) et 12345 (banc de streaming). La charge est
notée à côté de chaque chiffre : elle a varié de 6 à 62 pendant la série, et **les séries
« après » de génération ont tourné sous une charge plus haute que les « avant »** — la
comparaison joue contre les optimisations.

### 3.1 La génération

| | avant | après | charge (avant / après) |
|---|---:|---:|---|
| Release, 1 thread, ms/chunk | 1 535 | **436** (551 sans cache de terrain) | 13-20 / 40-62 |
| Debug, 1 thread, ms/chunk | 11 208 | **2 818** | 15-27 / 12-15 |
| condensé Release (64 chunks) | `91441dd6c78f21b3` | **`91441dd6c78f21b3`**, avec et sans cache | |
| condensé Debug (16 chunks) | `be05d77e3959e2da` | **`be05d77e3959e2da`** | |
| condensé, 144 chunks sur 4 et 6 ouvriers | `320ceab21b9f88a0` | **`320ceab21b9f88a0`**, 0 cellule différente du bras série | |

Les quatre outils de parité, mêmes arguments, binaires avant et après : **sorties identiques**
(`ov_carveparity --chunks=300`, `ov_surfparity --chunks=100 --per-region=4`,
`ov_features --chunks=48`, `ov_parity --terrain --carvers --chunks=200`).

Le cache de terrain, sur les 64 chunks : 112 copies, 144 générations (43,8 % du terrain copié).

### 3.2 Le streaming (`scripts/bench_stream.py`, Release)

| | avant | après |
|---|---:|---:|
| spawn prêt (journal du serveur) | 10,2 s | **3,6 s** |
| carré du spawn complet, rayons 4 / 8 | 39,6 / 106,5 s | **3,8 / 14,4 s** |
| téléport de 1000 blocs, rayon 8 rempli | 189,7 / 183,1 s / jamais (240 s) | **28,4 / 15,3 / 21,6 s** |
| chunks reçus par seconde pendant un remplissage | 1,2 à 1,6 | **10,2 à 18,9** |
| sprint 5,6 m/s : chunk sous le joueur manquant | 11,7 % du temps | **0 %** |
| sprint : rayon 4 complet | 50 % du temps | **100 %** |
| vol créatif 11 m/s : chunk sous le joueur manquant | 50 % | **0 %** |
| vol créatif : rayon 4 complet | 25 % | **100 %** |
| élytre 33 m/s : chunk sous le joueur manquant | 85 % | 73 % |
| « can't keep up », générations sur le tick | 0, 0 | 0, 0 |
| tick p50 / p99 / max | 0,6 / 5,9 / 135 ms | 1,8 / 15,9 / 190 ms |
| charge médiane | 9,3 | 10,3 |

Debug, entrée seule : spawn prêt en **24,4 s au lieu de 140,3 s** ; le serveur Debug d'avant a
fermé la connexion de la sonde avant le premier chunk.

Le tick p50 monte, et c'est le prix du débit : trois fois plus de monde arrive par seconde
(3 280 chunks en 3 min 20 contre 2 144 en 14 min), et avec lui la lumière de chaque chunk
publié (phase `relight`), l'apparition naturelle et les ticks aléatoires sur un monde chargé
plus vite. Le p99 reste à un tiers du budget de 50 ms.

### 3.3 La sauvegarde automatique

Même banc, trois binaires Release :

| | phase `autosave`, total sur la session | pire tick de sauvegarde | ticks lents dont elle est la cause |
|---|---:|---:|---:|
| avant | 1 372 ms | 134,7 ms | 14 |
| génération optimisée, sauvegarde sur le tick | 773 ms | 189,4 ms | 5 |
| **sauvegarde hors du tick** | **16 ms** | **4,1 ms** | **0** |

Le thread de sauvegarde écrit 146 à 283 chunks en 150 à 650 ms, hors du tick. La même série a
vu **un** tick de 915 ms — dans la phase des **ticks aléatoires** (867 ms), sans rapport avec la
sauvegarde ni avec ce travail ; il est nommé ici pour ne pas être caché.

---

## 4. Ce qui reste

1. **L'élytre va plus vite que la génération** : à 33 m/s le joueur traverse deux chunks par
   seconde, soit ~35 chunks neufs par seconde à l'avant de la vue, et quatre ouvriers en
   livrent 10 à 19. Six ouvriers (mesurés identiques au bit près) et la génération « en
   avance du mouvement » sont les deux leviers suivants ; non faits.
2. **Le Debug reste lent** : 2,8 s par chunk. Il est fait pour déboguer, pas pour jouer.
3. **Le plafond d'upload par frame** (`ROADMAP.md`) : le client borne le temps de maillage par
   frame (4 ms), pas les octets envoyés au GPU. Non mesuré, non fait.
4. **La distance de vue** reste plafonnée à 8 par le serveur.
5. **Le journal des arbres** : 8 832 lignes ERROR en quatre minutes. Le coût en temps est nul,
   le coût en lisibilité ne l'est pas ; signalé au chantier des features.
6. **Le reste de la sauvegarde** (entités, Nether, End, `level.dat`) écrit encore sur le tick ;
   ce sont les plus petits.

## 5. Rejouer

```bash
cmake --preset macos-release -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -gline-tables-only -DNDEBUG"
cmake --build --preset macos-release --target ov_gendet ov_dedicated

# le condensé et le temps d'un monde, un thread — le même condensé avant et après
./build/macos-release/bin/ov_gendet --serial-only --side=2
OV_TERRAIN_CACHE=0 ./build/macos-release/bin/ov_gendet --serial-only --side=2
# le même monde sur le pool, cache froid, dans les deux classes d'ordonnancement
./build/macos-release/bin/ov_gendet --side=3 --workers=4
OV_WORKER_QOS=utility ./build/macos-release/bin/ov_gendet --side=3 --workers=4
# le banc de streaming (port 25619, monde neuf sous .scratch/, effacé après)
python3 scripts/bench_stream.py --preset=macos-release --label=essai
```

## 6. Pour jouer vite

```bash
cmake --preset macos-release
cmake --build --preset macos-release --target ov_voxel
./build/macos-release/bin/ov_voxel --singleplayer
```

Le préréglage `macos-release` est RelWithDebInfo (`-O2`). `build/macos-debug` génère le terrain
quatre fois plus lentement : c'est le build qu'avait le rapport de départ.

## 7. Pièges

- **Un binaire Debug mesure le build, pas le jeu.** 11 s par chunk avant ce travail, 2,8 s après.
- **Le cache de terrain est partagé entre les bras d'`ov_gendet`** : un bras chronométré après
  un autre copierait tout. La première série l'a montré (144 chunks « en 0,9 s ») ;
  `GeneratedWorld::clear_terrain_cache` redonne un cache froid au bras parallèle.
- **Sans décorateur, l'étage des features ne creuse pas les voisins** : un test du pipeline
  sans décorateur doit creuser son trois-sur-trois lui-même.
- **Copier une section écrit sa source** (le drapeau copy-on-write) : deux threads qui copient
  le même chunk écrivent ensemble, d'où la copie sous le verrou du cache.
- **`ugrep` est le `grep` de cette machine** : un motif qui commence par `->` est lu comme une
  option (`grep -e`).

## 8. Sources

- **La surface préliminaire et les treize colonnes de l'aquifère** : `docs/provenance/aquiferes.md`
  (article *Cave* du wiki, explication de jacobsjo) — ce travail n'en change que la mémoïsation.
- **L'écran « Loading terrain » — une règle non sourcée, et dite comme telle.** Montré sur le
  chemin d'entrée dans un niveau (connexion, réapparition, changement de dimension) jusqu'à
  l'arrivée du chunk sous le joueur, jamais en explorant : c'est la description du
  comportement du jeu donnée par le mandat de ce travail. **Aucune des deux sources
  consultées ne la contient** — ni l'article *Loading screen* de minecraft.wiki (qui ne dit
  pas quand chaque écran s'ouvre ni se ferme), ni l'archive figée du protocole 763 (qui ne
  parle pas de cet écran aux paquets Login et Respawn) — et elle n'a pas été mesurée sur le
  vrai client ici. Ce qui en est acquis sans elle : le joueur ne bouge pas tant que son chunk
  n'est pas là (le comportement d'avant ce travail), et l'écran ne cache plus le monde en
  exploration. Aucun code du jeu lu.
- **Les classes QoS de macOS** : Apple, *Energy Efficiency Guide for Mac Apps*, page
  « Prioritize Work at the Task Level » (developer.apple.com, archive de documentation),
  consultée le 2026-09-12. USER_INITIATED : « Work that the user has initiated and requires
  immediate results […] The work is required in order to continue user interaction. Focuses on
  responsiveness and performance. » UTILITY : « Work that may take some time to complete and
  doesn't require an immediate result […] Focuses on providing a balance between
  responsiveness, performance, and energy efficiency. » **La page ne dit rien des cœurs
  d'efficacité ou de performance** ; tout ce que ce fichier affirme du coût d'UTILITY est
  mesuré (§ 3.1, `performance-tick.md` § 4.3).
- **Tous les chiffres** : mesurés sur cette machine avec les commandes du § 5.
