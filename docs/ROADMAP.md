# Ondes VOXEL — Roadmap

Inventaire complet de ce que doit contenir une réimplémentation de **Minecraft
Java Edition 1.20.1**. Une case ne se coche que si la [définition de
« terminé »](../CLAUDE.md#6-définition-de--terminé-) est intégralement remplie.

L'état résumé et lisible par machine vit dans [`PROGRESS.json`](PROGRESS.json) ;
un test CI vérifie que les deux fichiers concordent.

**Légende** — `[ ]` à faire · `[~]` en cours · `[x]` terminé · 🔒 décision
irrattrapable, à ne pas repousser · ⭐ critère de sortie du jalon.

---

## M0 — Fondations

### Build et outillage
- [x] `brew install cmake ninja ccache` — toolchain absente de la machine
- [x] `CMakeLists.txt` + `CMakePresets.json` (macOS, Linux, Windows, sanitizers)
- [x] `vcpkg.json` avec feature `client` isolant Vulkan/GLFW/VMA 🔒
- [x] `scripts/bootstrap.sh` — vcpkg, **cache binaire**, hooks git
- [x] `cmake/OvWarnings.cmake` — warnings-as-errors, `-ffp-contract=off` 🔒
- [x] `cmake/OvSanitizers.cmake` — ASan/UBSan/TSan
- [x] LTO désactivé par défaut (un link full-LTO dépasse 6 Go — risque R5) 🔒
- [x] `bench_headers.py` — poids préprocessé, **déterministe**, seuil à 10 % (risque R5) 🔒
- [x] `bench_build.sh` — temps mural, informatif (trop bruité pour un seuil)
- [ ] Brancher `bench_headers.py` en CI
      *(2026-09-11 : **volontairement pas branché**. Mesuré tel quel, la porte
      échoue d'emblée : +19,3 % par unité de compilation, et la base de
      référence date de 40 unités quand le projet en compte 445. Un seuil
      global sur une moyenne ne dit rien de l'en-tête qui a grossi. Avant
      d'activer : une comparaison **par fichier** et une référence
      rafraîchie — sinon la CI passe au rouge au premier commit et on apprend
      à l'ignorer)*
- [ ] En-têtes précompilées — **mesuré comme inutile à 30 TU**, à revoir vers 200

### Verrous d'architecture 🔒
- [x] `cmake/OvModule.cmake` — `ov_add_library`, couches, arêtes interdites
- [x] `scripts/check_layers.py` — lit les `#include` réels, pas les déclarations
- [x] `scripts/check_assets.py` — refuse tout asset de jeu dans le dépôt
- [x] `scripts/test_enforcement.sh` — **prouve que les verrous se déclenchent**
- [ ] Job CI `linux-server-only`, sans SDK Vulkan sur l'image ⭐🔒
- [x] Auto-test des headers publics : un TU par header, compilé seul
      *(dans CMake, donc actif dans tous les jobs et en local)*

### `ov_base` (L0)
- [x] `platform.hpp` — plateforme, compilateur, ligne de cache (128 o sur M2)
- [x] `types.hpp` — entiers de largeur fixe, `Id<Tag, Repr>` fortement typé
- [x] `assert.hpp` — `OV_ASSERT` / `OV_ENSURE` / `OV_UNREACHABLE`
- [x] `log.hpp` — catégories, niveaux, filtrage avant formatage
- [x] `thread.hpp` — nommage et **QoS macOS** (sinon le tick part sur E-core) 🔒
- [x] `time.hpp` — `TickClock` 20 Hz, anti-spirale de la mort
- [ ] `handle.hpp` — handles index + génération
- [ ] `hash.hpp` — xxHash, hachage stable
- [ ] Allocateurs : mimalloc global, pools par classe de taille, arènes par thread
- [x] `NoAllocScope` : hook d'allocation qui abandonne si le tick alloue 🔒
      *(actif dans la vraie boucle de tick ; 100 ticks, 0 violation)*

### `ov_math` (L1)
- [x] `vec.hpp` — `Vec3<T>`, `Direction` (numérotation du protocole)
- [x] `block_pos.hpp` — `BlockPos` / `ChunkPos` / `SectionPos`, division plancher
- [x] Index de section YZX vérifié par `static_assert` 🔒
- [ ] Matrices, quaternions, transformations
- [x] `AABB` : intersection, balayage, **clipping par axe** (Y puis X puis Z) 🔒
- [ ] `VoxelShape` — union de boîtes, pool dédupliqué
- [x] Raycast DDA (Amanatides & Woo) — bloc **et face** d'entrée 🔒

### `ov_io` (L2) et `ov_nbt` (L3)
- [x] Lecteur ZIP (jars, resource packs, datapacks) — deflate brut, sans dépendance
- [x] Anti-Zip-Slip : toute entrée `../` fait rejeter l'archive entière 🔒
- [x] Zip64, chiffrement et méthodes exotiques **refusés**, jamais mal gérés
- [ ] VFS : empilage dossier + zip selon la priorité des packs
- [x] Lecture de fichier bornée, écriture atomique (temp + rename)
- [x] NBT binaire : les 13 types (0-12), big-endian, gzip et zlib
- [x] **UTF-8 modifié** de Java : `\0` en C0 80, paires de substitution 🔒
- [x] Compounds à ordre préservé → round-trip **octet-identique** 🔒
- [x] Garde de profondeur (512) : un fichier hostile ne déborde pas la pile
- [x] Bornes sur les longueurs : rejet **avant** allocation
- [x] Plafond de décompression : refus des bombes zip
- [ ] SNBT (texte) : lecture et écriture
- [ ] Chemins NBT (`Inventory[0].id`) pour la commande `/data`
- [x] Fichiers région `.mca` : en-tête 8 KiB, 1024 offsets + timestamps
- [x] Les fichiers réels ne sont **pas** alignés sur le secteur (voir PROVENANCE) 🔒
- [x] Compression : 1 GZip, **2 Zlib (défaut)**, 3 aucune (pas de LZ4 en 1.20.1)
- [ ] Fichiers `.mcc` pour les chunks dépassant 1 Mo
- [x] Round-trip NBT sur de vrais fichiers vanilla ⭐
      *(2900 chunks / 795 277 tags, octet-identiques — voir `docs/PROVENANCE.md`)*
- [x] `ov-inspect nbt|region --verify`

### Assets
- [x] `ov-assetimport` : détection PrismLauncher / MultiMC / launcher officiel
- [x] Version lue dans le `version.json` du jar, jamais dans son nom de fichier 🔒
- [x] Message d'erreur actionnable si la version est absente
- [x] Extraction du jar client : `models/`, `blockstates/`, `font/`, `lang/`
- [x] Résolution via `assets/indexes/*.json` → `objects/<hash>` (142 langues)
- [x] Sons : `--sounds` implémenté, **activé par défaut** depuis que le client les joue
      *(**2026-09-11** : 152 Mo d'effets dans `run/assets/`, gitignoré ; `--no-sounds`
      les écarte, `--music` ajoute musique et disques, 432 Mo. Voir
      `docs/provenance/son-client.md`)*
- [x] Empilage des packs selon la priorité vanilla, dossiers **et** zips
- [x] Manifeste de provenance par fichier → `run/assets/PROVENANCE.tsv`
- [ ] `ov-assetgen` : atlas procédural (démarrage sans aucun asset externe)
- [ ] `scripts/setup_vanilla.sh` — guide l'installation de 1.20.1 et du server.jar

---

## M1 — Protocole 763

- [~] `data/protocol/763.json` — schéma des paquets (dérivé de minecraft-data, MIT) 🔒
      *(2026-09-11 : le **catalogue** — état, sens, id, noms des 176 paquets —
      dérivé de minecraft-data **et** de l'archive figée, qui s'accordent sur
      176/176 ids ; il sert de vérité à la matrice. Les **champs** n'y sont pas :
      tant qu'`ov-pktgen` n'existe pas, ils seraient une seconde copie de ce que
      le C++ spécifie et teste octet à octet)*
- [ ] `ov-pktgen` : **génération** des encodeurs, décodeurs, dumps de debug et
      harnais de fuzz. Écrire 250 paquets à la main est 2 mois de dette 🔒
- [x] VarInt (≤ 5 o) et VarLong (≤ 10 o) — table de la spec vérifiée 🔒
- [x] Chaînes UTF-8 validées, UUID, angle ; limites imposées avant allocation 🔒
- [x] Position empaquetée (26/26/12 bits), NBT réseau
      *(x sur 26 bits, z sur 26, **y sur les 12 de poids faible**, tous signés ;
      l'extension de signe est testée sur neuf positions dont les extrêmes du
      monde — la moitié des coordonnées sont négatives et le bug ne s'y voit
      que là)*
- [x] Framing par longueur, avec bascule de compression en cours de connexion 🔒
      *(le cas sous le seuil garde le framing compressé, longueur interne à 0)*
- [🚫] Chiffrement AES/CFB8, secret partagé 16 octets
      *(**hors périmètre** : le mode hors-ligne est une décision produit, pas un
      retard. Sans authentification Mojang il n'y a pas de secret partagé à
      établir, et chiffrer une session que personne n'authentifie ne protège
      rien)*
- [x] **Mode hors-ligne uniquement** — décision produit, voir `docs/ARCHITECTURE.md` § 8 🔒
- [x] UUID hors-ligne déterministe, vérifié contre `UUID.nameUUIDFromBytes` de Java
- [x] MD5 (requis par l'UUID v3), vecteurs RFC 1321
- [x] Machine à états par connexion : Handshake → Status ✓ / Login ✓
- [x] Login hors-ligne : Login Start, validation du nom, Disconnect explicatif
- [x] Login Success + passage à l'état Play
      *(des deux côtés : le serveur l'émet, et `ov_netclient` le reçoit et
      bascule)*
- [x] **Status : MOTD, nombre de joueurs, ping ⭐** — vérifié par un client écrit
      depuis la spec : JSON conforme, pong à écho correct
- [~] Les ~130 paquets Play, round-trip octet à octet
      *(44 identifiants implémentés — ceux dont la tranche verticale a besoin.
      Le reste arrive avec les entités, l'inventaire complet et le son)*
      *(2026-09-11 : **+14 paquets d'interface**, encodeur et décodeur, dans
      `ov/protocol/hud.hpp` — Boss Bar, les six paquets de bordure, Display
      Objective, Update Objectives, Update Teams, Update Score, Award
      Statistics, Select Advancements Tab, Seen Advancements. Octets attendus
      écrits à la main depuis l'archive ; la matrice passe à 125/176 paquets
      couverts, 33 en aller-retour. Pas encore émis par le serveur, et Update
      Advancements reste à faire)*
- [x] Métadonnées d'entité (index / type / valeur)
      *(`MetadataWriter` couvre les 28 types de valeur de 763, et la table
      d'indices est **dérivée** plutôt que recopiée : un champ NBT à la fois sur
      un zombie de référence, et l'index qui bouge est la réponse. Les indices
      que rien n'a fait bouger sont absents plutôt que devinés — voir
      `docs/PROVENANCE.md`)*
- [x] Format de chunk **réseau** — palette bit-packée, **distinct du disque** 🔒
      *(et **relu** : `parse_chunk_data` est le miroir exact de l'encodeur, testé
      sur 24 chunks réels comparés cellule par cellule — blocs, biomes, les deux
      lumières et les heightmaps)*
- [x] Cibles de fuzz sur le décodeur, aucun crash sur entrée malveillante
      *(2026-09-11 : `fuzz_ov_protocol`, **déterministe à graine fixe** — pas
      libFuzzer, que les trois OS de CI ne partagent pas. 72 points d'entrée
      (tous les décodeurs d'octets de socket, framer compris) nourris de préfixes,
      de mutations de paquets valides, d'octets aléatoires et de flux coupés au
      hasard. Vert sous ASan + UBSan ; le job CI *Sanitizers* le lance avec le
      reste. `OV_FUZZ_SEED` / `OV_FUZZ_ITERATIONS` pour une campagne longue —
      voir `docs/provenance/protocole-763.md`)*
- [x] Matrice de conformité `docs/protocol/763/`
      *(2026-09-11 : **générée depuis le code** par `scripts/protocol_matrix.py` —
      les 176 paquets, par état et par sens, avec constante, encodeur, décodeur,
      test, aller-retour, octets vanilla, usage serveur/client. Chaque constante
      d'id est comparée au catalogue ; `--check` en CI. Elle a trouvé **Set
      Cooldown envoyé en 0x16** (Chat Suggestions) au lieu de 0x15 — voir
      `docs/provenance/protocole-763.md`)*

---

## M2 — Registres et données

- [x] `ov-datagen` : `server.jar --all` → `data/vanilla/1.20.1/` (jamais commité)
- [x] Ordre des propriétés **déduit des IDs**, jamais lu dans `blocks.json` 🔒
      *(4 blocs y divergent — voir `docs/PROVENANCE.md`)*
- [x] `first_id` par registre (`mob_effect` est 1-based) 🔒
- [x] `MANIFEST.sha256` committé — CI vérifie sans redistribuer de données Mojang
- [x] `ResourceLocation` : validation stricte, forme canonique, ordre par namespace
- [ ] Interning des identifiants et hachage parfait
- [x] `Registries` : les 66 registres codés en dur, IDs exacts de Mojang 🔒
- [x] Test CI : égalité stricte contre `registries.json`, ordre inclus ⭐
      *(5067 IDs, `scripts/check_registry_parity.py`)*
- [x] `.ovpack` binaire, mmap-able, sans pointeur — 131 744 octets, déterministe
- [x] `BlockRegistry` : lookup bloc, propriétés, état par défaut, bloc d'un état
- [x] Opacité à la lumière du ciel, **mesurée** sur les 1003 blocs 🔒
- [x] Reste des flags : émission, formes de collision, couleurs
      *(émission mesurée sur 23282 états, 4327 formes de collision par état,
      et les effets de biome — climat, eau, brouillard, ciel — en format 11)*
- [x] **Arithmétique mixed-radix des propriétés** — un multiply, un add 🔒
- [x] Test : les **24 135 états** correspondent au rapport de Mojang ⭐
      *(`scripts/check_registry_parity.py`, relit le binaire depuis sa spec)*
- [x] `PalettedContainer` bit-exact : single-value, indirect 4-8 bits, direct 15 bits
- [x] Palette de biomes 4×4×4, 6 bits en direct
- [x] Tags des 10 registres codés en dur, résolution récursive à la compilation
- [ ] Tags des registres dynamiques *(avec `Registry Data`, M3)*
- [ ] Chargeur de datapack `pack_format` 15, empilable
- [~] `ov-datac` → cache `.ovpack` mmap, position-independent, zéro pointeur
      *(position-independent et sans pointeur : oui, c'est ce qui le rend
      lisible tel quel. **Pas encore mmap** — il est lu en mémoire d'un bloc,
      ce qui coûte 583 ko et rien d'autre pour l'instant)*
- [x] Invalidation par version de format + hash de contenu
      *(`FORMAT_VERSION` refusé s'il ne correspond pas — un cache périmé lu
      comme courant est bien pire que pas de cache — et `MANIFEST.sha256` pour
      les données sources)*
- [x] Test : rebuild depuis JSON == cache, **octet à octet** (déterminisme) 🔒
      *(l'émetteur reconstruit le pack une seconde fois et compare les octets ;
      `deterministic .. yes` à chaque génération)*
- [x] `LegacyRandomSource` bit-exact — vérifié contre une vraie JVM 🔒
- [ ] `log` fdlibm pour rendre `nextGaussian` bit-exact *(4 ulp d'écart mesuré)*
- [x] `XoroshiroRandomSource` bit-exact — vérifié contre le JDK, seeding compris 🔒
- [x] `PositionalRandomFactory`, hachage de seeds, vecteurs de référence ⭐
      *(le hachage de position, le fork positionnel, l'ensemencement par MD5
      d'un nom et le hash de chaîne de Java. Le piège est sur la première
      ligne du hachage : `x * 3129871` déborde en 32 bits avant d'être
      élargi, et le faire en 64 bits déplace chaque minerai passé ~686 blocs)*
- [x] `ov-inspect` : dump NBT, région, chunk, registre, paquet
      *(plus `column`, `heightmaps`, `state`, `loot`, `connects` et `stairs`,
      ajoutés au fur et à mesure que chaque mesure en a eu besoin)*

---

## M3 — Tranche verticale ⭐

> **Atteint le 2026-09-08** : un client Minecraft 1.20.1 non modifié se connecte,
> spawne dans un monde superflat éclairé, s'y déplace, casse et pose des blocs.
> Sans une ligne de Vulkan.

- [x] Boucle de tick 20 Hz avec budget et rapport de surcharge
- [x] `ChunkMap`, système de tickets, niveaux de chargement ⭐
      *(les tickets sont des **raisons** — joueur, forcé, transitoire — et le
      **niveau**, pas le ticket, décide si un chunk est tické, résident ou
      parti. La génération est tirée par les tickets et non par les paquets.
      **289 chunks sur 289 arrivent**, contre 153 ; `can't keep up` passe de 20
      à **0** ; le tick p99 de **6 686 738 µs à 424 µs**)*
- [x] Pipeline de chunks par statuts *(`empty → structure_starts → biomes →
      noise → surface → carvers → features → full`)*, avec le **rayon des
      voisins imposé** et non espéré : `features` pousse ses 8 voisins à
      `carvers`, `full` les pousse à `features`
      *(**27,97 % des écritures de features tombent hors du chunk décoré et
      sont conservées** — 350 762 sur 1 254 082. Un adaptateur mono-chunk
      aurait rapporté zéro et eu l'air sain. Statuts mis en cache : 48 chunks
      coûtent 484 terrains et 216 décorations, pas 48×9)*
- [~] **Ordonnancement par régions exclusives** — features déborde sur les voisins 🔒
      *(la règle est écrite et testée exhaustivement — deux chunks de même
      classe `(x mod 3, z mod 3)` ont des ensembles d'écriture disjoints, et
      les neuf classes servent — mais **rien ne tourne en parallèle**, avec
      l'argument mesuré : la décoration ne fait que la moitié du coût, les
      quatre étages de terrain ont un rayon nul, et paralléliser exigerait un
      cache de chunks partagé entre threads, exactement la structure mutable
      partagée que le principe 3 interdit. De la géométrie, jamais un mutex)*
- [ ] Sections copy-on-write en `shared_ptr<const>` 🔒
      *(pas encore nécessaire : rien ne demande d'instantané sans verrou. Le
      `chunk_mutex` du serveur protège toujours la carte — ce qui l'a quittée
      est la génération de 0,2 s, pas le verrou — et le retirer demande de
      router le réseau vers le thread de tick, c'est-à-dire `ov_sim`)*
- [x] Pool de jobs : `std::jthread` et une file, **pas enkiTS**
      *(rien ici n'a besoin de vol de travail ni de graphe de tâches, et une
      dépendance vcpkg coûte plus sur cette machine qu'elle ne rapporte. Ce qui
      porte le travail est qu'un job reçoit **son index de worker**, ce qui rend
      un état par worker atteignable sans verrou — indispensable parce que
      `NoiseRouter` et `SurfaceSystem` gardent des caches mémo `mutable` et
      qu'une pile de worldgen partagée corrompt la carte au lieu de rendre un
      nombre périmé. Déterminisme vérifié cellule par cellule : 6 291 456
      cellules, **0 différence**, sur 1 et 4 workers, et sous TSan)*
- [x] Générateur superflat
- [x] Lumière du ciel : sunlight direct + propagation par flood fill *(intra-chunk)*
- [x] Propagation de lumière inter-chunks *(voisinage 3×3 chargé)*
- [x] Lumière de bloc : émission mesurée par état, propagation depuis les sources
- [ ] Suppression incrémentale de la lumière (aujourd'hui le voisinage est refait en entier)
      *(**2026-09-11** : le recalcul n'est plus fait sur le thread réseau avant
      chaque Block Update mais sur le tick, une fois par chunk touché — un bloc
      cassé coûtait ~11-88 ms au thread qui écrit tous les paquets, ~10-40 µs
      maintenant ; casser un bloc sur un monde généré en Release passe de 5,2 s
      à **62 ms au p99**. Le voisinage est toujours refait en entier : ce qui
      manque ici reste l'incrémental. Voir `docs/provenance/performance-tick.md`)*
- [x] Tableaux de lumière nullables à valeur uniforme (divise l'empreinte par 2)
- [x] Heightmaps : stockage, packing 9 bits, sémantique vérifiée sur monde réel
- [x] `WORLD_SURFACE` calculé et maintenu incrémentalement *(air suffit)*
- [x] `MOTION_BLOCKING`, `MOTION_BLOCKING_NO_LEAVES` et `OCEAN_FLOOR` : prédicats mesurés sur 996 des 1003 blocs
- [x] Streaming de chunks selon la distance de vue *(envoi et déchargement)*
- [x] Collisions : boîtes par état, chevauchement et glissement axe par axe
- [x] Physique du joueur : marche, sprint, accroupissement, saut, chute — ajustés sur une trace du vrai client
- [~] Nage, échelles, glace et slime : glissance par bloc
      *(**l'eau et la lave sont faites** : traînée 0,8 (0,9 en nage sprintée),
      accélération 0,02, gravité au seizième dans l'eau et au quart dans la
      lave, poussée de 0,04 vers le haut, seuil de 0,4 qui distingue une flaque
      d'une piscine. Les six vitesses publiées sont reproduites. Échelles,
      glace et slime restent)*
- [x] Gestion des joueurs, keep-alive, liste des joueurs
- [x] Entités joueur : apparition, mouvement, rotation de tête, retrait
- [x] `ov_netclient` + `ClientLevel` (réplique séparée)
      *(module client **sans rendu** : poignée de main, login hors-ligne,
      compression, chunks, mises à jour de blocs, téléportations, keep-alive.
      Le paquet de chunk est **relu** par `parse_chunk_data`, miroir exact de
      l'encodeur et testé comme tel : 24 chunks réels encodés puis relus,
      blocs, biomes, lumière et heightmaps identiques)*
- [x] Le client Ondes VOXEL se connecte au serveur Ondes VOXEL et on y marche
      *(289 chunks reçus, maillage sous budget de 4 ms par frame, physique à
      20 Hz contre les formes de collision, position rapportée 20 fois par
      seconde, casse et pose par raycast)*
- [~] `LoopbackTransport` SPSC — **octets sérialisés même en solo** 🔒
      *(le solo passe aujourd'hui par une socket TCP sur la boucle locale, donc
      les octets sont bien sérialisés ; ce qui reste est de remplacer la socket
      par une file SPSC, ce qui ne change rien au-dessus du transport)*
- [x] **Un client vanilla 1.20.1 se connecte, marche, casse et pose un bloc** ⭐

---

## M4 — Monde persistant

> **Aller-retour croisé vérifié le 2026-09-08** : notre sauvegarde s'ouvre dans le
> vrai serveur Minecraft 1.20.1 sans une erreur, et une sauvegarde réécrite par
> lui se recharge chez nous avec les bons blocs.

- [x] Lecture Anvil : sections, palettes, biomes, heightmaps, block entities
- [x] Écriture Anvil : fichiers région, palettes par nom, écriture atomique : allocation de secteurs, compactage, timestamps
- [x] `level.dat` : version Anvil, générateur, spawn, bordure, DragonFight : gzip, racine `Data`, écriture sûre via `level.dat_old`
- [x] `DataVersion` : refus explicite, et jamais de réécriture d'un chunk illisible
- [x] Pose : dalles doubles, portes et lits à deux blocs, conventions mesurées
- [x] Formes de collision par état, et faces pleines : 23358/23358 faces mesurées reproduites
- [x] Connexions des clôtures, portillons, vitres et barreaux, dans les deux sens
- [x] Forme des escaliers : 256 cas relevés en entier, plus le bloc qui annule le coin
- [x] Murets : `low`, `tall` et le poteau, relevés avec le bloc du dessus
- [x] Durées de cassage et outils corrects : vérifiés tick pour tick sur 985 des 996 blocs
- [x] Tables de butin, Silk Touch et Fortune : tirages comparés à ceux du vrai serveur
- [x] Entités objet : les butins tombent au sol et se ramassent *(sans gravité)*
      *(**2026-09-11 — persistance** : objets et orbes **sauvés** dans le chunk où ils sont,
      par dimension (`Item`, `Age` court, `PickupDelay`, `Health` ; `Value` court, `Count`
      entier — types relevés sur le vrai serveur), relus avec leur âge : ils partent au même
      tick qu'avant. Avec eux : flèches plantées et ramassables, tridents, lancers en vol,
      TNT amorcée (`Fuse`), sable qui tombe (`BlockState`, `Time`), nuages, mobs du Nether
      (`DIM-1/entities`), dragon et cristaux (`DIM1/entities`), et le wagonnet du joueur
      (`RootVehicle`). Voir `docs/provenance/persistance-entites.md`)*
- [x] Conteneurs : ouverture, clic gauche et droit, hotbar
- [x] Shift-clic : fusion jusqu'à la taille de pile réelle, mesurée item par item
- [x] Glissés, touches numériques, lâcher d'objet, inventaire autoritatif en survie
- [x] Mode créatif, sélection d'items
- [x] Sauvegarde périodique et à l'arrêt *(asynchrone via COW : à venir)*
- [x] **Round-trip croisé avec Minecraft vanilla, sans perte** ⭐

---

## M5 — Client Ondes VOXEL

### `ov_rhi` (L13)
- [x] Instance, device, swapchain, `VK_KHR_dynamic_rendering`
- [x] Handles opaques index + génération, jamais de pointeur
- [x] Buffers, images, samplers, VMA
- [x] Pipelines, `VkPipelineCache` persisté, SPIR-V compilé **hors-ligne**
- [x] Command lists, **barrières explicites** (pas de state tracker automatique)
- [~] `FrameContext` : ring de 2, pool de descripteurs et arène de staging
      *(ring et pool faits ; l'arène de staging reste — les uploads passent par
      un buffer jetable, ce qui va pour le démarrage et pas pour une frame)*
- [x] Timestamps GPU dès le départ
- [ ] Handles de texture en `u32` — bindless **préparé, non implémenté** 🔒
- [x] `multiDrawIndirect` et `drawIndirectFirstInstance` demandés seulement si
      le pilote les annonce, et repli mesuré sur un draw par section

### `ov_render` (L14)
- [x] Triangle → quad texturé → cube + profondeur + caméra
- [x] **Pipeline blockstate → variant → model → parent → elements → faces →
      rotations → uvlock** *(1005 blockstates, 6081 références, 62227 quads,
      0 échec — `ov_modelbake run/assets`)*
- [x] Stitcher d'atlas, mips, animations `.mcmeta` *(animations **jouées**
      depuis le 2026-09-11 — eau, lave, feu, portail —, tous les niveaux de mip,
      copie par région ; phase non alignée sur vanilla, voir
      `docs/provenance/rendu-parite.md`)*
- [x] Mailleur **par face depuis le modèle**, occlusion ambiante par sommet
      *(pas greedy : le plan se trompait, voir PROVENANCE)*
- [x] Vertex packé **16 octets** *(12 d'abord ; le quatrième mot est la teinte
      de biome en RGB8, parce qu'une teinte n'est pas l'une de quatre couleurs
      — coût mesuré : +81 Mio dans l'arène à 12 chunks)*
- [x] **Couleurs de biome** : colormaps `grass.png` / `foliage.png` échantillonnées
      par climat, overrides et modificateurs, mélange 5×5 comme vanilla
      *(15 couleurs publiées reproduites exactement ; le climat est en f64 et
      c'est une exigence mesurée, pas de la prudence)*
- [x] Arène device-local 384 Mo, free-list en pages de **3 Ko**
      *(pas 4 Ko : le `vertexOffset` d'une commande indirecte compte des
      sommets, et 4096 n'est pas un multiple du sommet de 12 octets. 3072 l'est
      — 256 sommets, 64 quads. 259 Mo utilisés à 12 chunks)*
- [x] **Index buffer statique partagé** (supprime la mémoire d'index par section)
- [x] Culling frustum CPU → `drawIndexedIndirect`, **3 draws** pour le terrain
      *(3 et non 4 : la couche `cutout_mipped` est vide sur ce monde. 2491
      sections dessinées en 3 appels ; l'origine de section, qui ne peut plus
      être poussée, est lue dans un storage buffer indexé par le
      `firstInstance` de la commande)*
- [x] Passe translucide triée, index buffer mutable dédié *(les sections sont
      triées d'arrière en avant, et depuis le 2026-09-11 les quads translucides
      **à l'intérieur** de chaque section aussi — `render/translucent_sort`)*
- [ ] Plafond d'upload par frame (les spikes, pas le FPS moyen, sont le risque)
- [~] Ciel, soleil, lune, étoiles, nuages, brouillard, météo
      *(**2026-09-11 — la météo** : cycle naturel de pluie et d'orage (durées
      `/weather` uniformes, χ² p = 0,88/0,19/0,39), tick de chunk de
      précipitation — glace des océans gelés **11 285/11 285** colonnes du monde
      de référence, neige et chaudrons —, foudre (21 éclairs vanilla pour 20,0
      prédits sur 208 chunks, paratonnerre 40/40, conversions), lits : seuils
      de sommeil identiques au tick près au clair, sous la pluie et l'orage,
      nuit sautée, point de réapparition. Client : pluie et neige dessinées,
      ciel et brouillard assombris par les Game Events 7/8, flash d'éclair —
      0,45/1,35 ms p50/p99 d'enregistrement pour un orage. **Lits : 38/38 cas
      identiques au vrai serveur.** Le feu de la foudre est branché depuis la
      fusion du feu (non mesuré). Restent phantoms et insomnie, l'explosion du
      lit dans le Nether sur le fil. Voir `docs/provenance/meteo-sommeil.md`)*
      *(la **courbe de luminosité** (les 16 valeurs publiées à 1e-7), le
      **lightmap 16×16** reconstruit par frame, le **cycle du jour** (13670 et
      22331 au tick près) et le **brouillard cylindrique** avec sa couleur
      dérivée du biome sont faits. Soleil, lune, étoiles, nuages et météo
      restent — et deux constantes du lightmap ne sont documentées nulle part,
      voir PROVENANCE)*
      *(**2026-09-11 — parité de rendu**, mesurée scène par scène contre le vrai
      client : monde composé en valeurs stockées (cible UNORM, plus de sRGB
      caché), lightmap **12 800/12 800** texels exacts — les deux constantes
      manquantes mesurées (plancher du ciel 0,05, bleuissement 0,35) —, AO et
      lumière lissée du jeu, variante de modèle par position (459/459 graines),
      décalage des plantes 22/22, brouillard tiré vers le ciel et teinté au
      crépuscule, disque de ciel, bande de l'aube, soleil et lune avec phases.
      Scène `aolab` 23 → **98 %** des pixels à ±8, `desert` 95 %, `plains`
      84 %. Restent nuages, étoiles, mélange du ciel entre biomes, vue sous
      l'eau, et des pics d'image isolés à investiguer. Voir
      `docs/provenance/rendu-parite.md`)*
- [x] Courbe `f/(4-3f)` et lightmap : une torche à 7 rend 18 % et non 47 %

### `ov_client` (L15) et `ov_audio` (L14)
- [x] Surbrillance du bloc visé et réticule
      *(un seul pipeline en `LineList`, deux espaces de coordonnées : la boîte
      passe par la view-projection, le réticule est déjà en clip space. La visée
      ignore les fluides — un rayon lancé au-dessus d'un étang doit atteindre le
      fond, pas s'arrêter à la surface)*
- [x] Fenêtre GLFW, entrées, bindings de touches
- [x] Serveur intégré sur son propre thread
      *(le serveur a quitté `apps/` pour devenir `ov_server` ; `ov_voxel
      --singleplayer` l'héberge sur un thread, il ouvre une vraie socket, et le
      client s'y connecte comme à n'importe quelle autre. Une seule commande,
      un seul processus, et les octets restent ceux du protocole)*
- [ ] Prédiction de mouvement et réconciliation
- [ ] Interpolation d'entités
- [x] Mixeur audio, sons 3D atténués, catégories de volume
      *(**2026-09-11** : atténuation linéaire à `attenuation_distance` × max(1,
      volume), panoramique sur l'axe droit `regard × haut` de la tête —
      invariant au tangage, prouvé —, **8/8 positions** du modèle numérique
      retrouvées dans les échantillons du mixeur à 10⁻⁴ ; priorité des voix
      (le plus faible aux oreilles cède) : 200 sons/frame tiennent en **4,9 ms
      p99** par rappel de 10,7 ms, contre 16,0 ms sans plafond ; dix catégories
      et `showSubtitles` dans `options.txt` aux clés du vrai client. Bornes de
      voix et loi de panoramique : les nôtres, non comparées au vrai client.
      Voir `docs/provenance/son-client.md`)*
- [~] **Deux clients pour un serveur · p99 ≤ 20 ms à 12 chunks** ⭐
      *(le p99 est tenu : 17,77 ms avec vsync, dont 0,41 ms d'enregistrement
      CPU et 10,91 ms de GPU, à 12 chunks sur M2 en 2560×1440. Les deux
      clients pour un serveur restent — ils attendent le serveur intégré)*
- [ ] Golden images en CI sur **Linux + lavapipe** (MoltenVK n'est pas un oracle)

---

## M6+ — Contenu complet

### Génération du monde
- [x] Bruit : Perlin, octaves, `NormalNoise`
      *(`ImprovedNoise` → `PerlinNoise` → `NormalNoise`. Une amplitude nulle
      **saute** une octave sans décaler les autres, parce que chacune est
      ensemencée par le hash de son nom et non en séquence)*
- [x] **Interpréteur de `density_function`** — jamais un générateur ad hoc 🔒
      *(les **25 types atteignables** depuis les 15 entrées du routeur overworld
      sont implémentés et les 15 se construisent, `final_density` compris. Un
      type non implémenté est **refusé et nommé**, jamais traité comme zéro.
      L'amplitude de `old_blended_noise` a été mise en cause puis **disculpée
      par le Nether**, où la densité finale se réduit à ce seul bruit — voir
      `docs/provenance/amplitude-old-blended-noise.md`)*
- [~] `noise_router`, `noise_settings`, splines
      *(le routeur et les réglages sont lus depuis les JSON vanilla ; les
      splines sont évaluées **en float**, comme le jeu, parce que c'est d'elles
      que vient la forme à grande échelle du terrain)*
- [x] `old_blended_noise` — le bruit de terrain 1.17, dont dépend `final_density`
      *(trois piles d'octaves : deux limites et un sélecteur qui choisit entre
      elles, ce qui est ce qui donne ses surplombs au terrain 1.17. Ensemencées
      **en séquence** et non par nom — l'ancien schéma, et c'est pourquoi elles
      ne peuvent pas réutiliser `PerlinNoise::create`)*
- [x] `NoiseChunk` : échantillonnage sur la grille de cellules et interpolation
      *(cellules de 4 blocs de large et 8 de haut ; interpolation y puis x puis
      z, l'ordre de vanilla — en flottant ce n'est pas le même résultat qu'un
      autre ordre)*
- [~] **Les 15 entrées du routeur overworld se construisent** ; le remplissage
      des chunks donne **97,79 % d'accord solide/air** contre le monde de
      référence *(1,20 % de pierre en trop = grottes et aquifères non
      implémentés ; 1,01 % de pierre manquante, dont un décalage systématique
      de la surface d'environ deux blocs dont la cause n'est pas localisée)*
- [x] **Multi-noise biome source** : temperature, humidity, continentalness,
      erosion, depth, weirdness
      *(**613857 cellules de biome sur 614400 identiques au vrai jeu** à la
      seed 1234567890 — 99,912 %. Les 543 restantes sont des **égalités
      exactes** : deux boîtes à la même distance, départagées chez vanilla par
      l'ordre de parcours d'un R-tree et par un cache du résultat précédent.
      Le calcul du climat, lui, est identique)*
- [x] Départage des égalités : ordre de construction du R-tree et cache
      `lastResult` ⭐
      *(**7 821 312 / 7 821 312 cellules — 100,000 %**. Le R-tree seul ne
      récupère que 16 des 2197 écarts ; les 2181 autres viennent du cache, et
      seulement s'il est alimenté dans l'ordre de **remplissage** et non de
      stockage — c'est cette mesure qui a nommé l'imbrication des boucles)*
- [x] `surface_rules` — interpréteur des JSON vanilla, 4 types de règles et
      11 types de conditions, un type inconnu refusé et nommé
      *(colonnes dont les 8 couches du sommet correspondent : **4,07 % → 91,22 %** ;
      blocs individuels 64,92 % → **97,67 %** ; plancher de bedrock **320000/320000**.
      Restent les icebergs des océans gelés et les piliers des badlands érodées,
      qui sont des passes séparées et non des règles)*
- [x] Carvers : grottes et ravins
      *(**masques bit-exacts : 1200/1200 chunks, 1 615 858 cellules**, aucune
      chez nous seule, aucune chez le jeu seul. L'oracle est un chunk que le
      jeu n'a pas fini : il conserve ses `CarvingMasks`. Accord solide/air
      97,97 → 98,79 %. Cheese, spaghetti et noodle ne sont pas des carvers mais
      des termes de densité, et ils sont dans `final_density`)*
- [~] Aquifères, lave, niveaux d'eau
      *(**pas implémentés**, et l'oracle qui les décidera est construit : le
      masque de creusement du jeu croisé avec ses propres blocs dans le même
      chunk — 1 775 chunks, 1 490 251 cellules, dont le jeu laisse 69,2 %
      d'air, **23,5 % d'eau**, 2,8 % de lave et 4,5 % de solide, ce dernier
      chiffre étant la réponse de la barrière. Le **niveau de fluide est
      mesuré** — `40k + 20 + 3j`, décalage `3·⌊S·spread⌋` avec `spread` lu à
      l'**index de la grille** : 89,3 % contre 31,4 % au centre du bloc, et
      quatre témoins. La barrière, le cas noyé jusqu'au niveau de la mer et le
      départage eau/lave sont **refusés et nommés**. **2026-09-10 : implémentés**
      — grille décalée, barrière, cas noyé, lave profonde, dans l'étage de bruit
      et dans les carvers : l'oracle entier passe de **71,747 % à 99,954 %**
      cellule par cellule (eau 99,983, lave 99,998, air 99,946), contre 90,678 %
      pour un témoin au mauvais nom de hachage ; sur chunks `full` par
      `generate()`, intérieurs des grottes 76,2 → 97,6 %, déterminisme 0 écart
      sur 1 572 864 cellules. Restent : les fluides à réveiller — règle trouvée,
      98,3 % des marques `PostProcessing` — ne sont livrés à aucune file, donc
      une cascade générée ne coule pas ; un tiers de la lave des carvers sous
      −56 est marqué par une règle inconnue ; coût ≈ ×2,5 en debug. Voir
      `docs/provenance/aquiferes.md` § 10. **2026-09-11** : `aquifers_enabled`
      des réglages enfin lu — l'aquifère tournait aussi dans le Nether et l'End,
      qui n'en ont pas, et vidait la mer de lave du Nether : lave retrouvée
      25,2 → **99,99 %**, Nether 98,12 → **99,65 %** sur 200 chunks `carvers`,
      `docs/provenance/nether.md` § 1.6)*
- [x] Minerais par couche, distributions triangulaires
      *(**99,509 % des positions au bloc près** en rejouant sur le terrain du
      jeu, et **82,406 % sur notre propre terrain généré de bout en bout** —
      l'écart de 17,1 points est ce que coûte encore notre relief, et il est
      attribué : 68,2 % là où notre terrain a gardé sa pierre sans que rien
      n'y arrive, 20,0 % nos propres filons de granite arrivés les premiers,
      7,9 % un terrain qui n'est pas de la pierre)*
- [ ] **Les ~65 biomes** — plains, sunflower_plains, snowy_plains, ice_spikes,
      desert, swamp, mangrove_swamp, forest, flower_forest, birch_forest,
      old_growth_birch_forest, dark_forest, old_growth_pine_taiga,
      old_growth_spruce_taiga, taiga, snowy_taiga, savanna, savanna_plateau,
      windswept_hills, windswept_gravelly_hills, windswept_forest,
      windswept_savanna, jungle, sparse_jungle, bamboo_jungle, badlands,
      eroded_badlands, wooded_badlands, meadow, cherry_grove, grove,
      snowy_slopes, frozen_peaks, jagged_peaks, stony_peaks, river,
      frozen_river, beach, snowy_beach, stony_shore, warm_ocean,
      lukewarm_ocean, deep_lukewarm_ocean, ocean, deep_ocean, cold_ocean,
      deep_cold_ocean, frozen_ocean, deep_frozen_ocean, mushroom_fields,
      dripstone_caves, lush_caves, deep_dark, nether_wastes, warped_forest,
      crimson_forest, soul_sand_valley, basalt_deltas, the_end, end_highlands,
      end_midlands, small_end_islands, end_barrens
- [~] Features : arbres par essence, végétation, geodes d'améthyste, dripstone,
      lush caves, blocs sculk du deep dark, sources, disques, lacs
      *(le cadre est là et **l'ensemencement est trouvé** : `WorldgenRandom`
      enveloppe une source et ne redéfinit que `next(bits)`, donc l'état avance
      en Xoroshiro128++ pendant que les bits sortent à la mode legacy — 9712
      marqueurs sur 9712, sur quatre graines dont trois hors échantillon.
      **113 des 194 features configurées se chargent**, 134 placed features,
      le reste refusé par son nom. Les 9 trunk placers, les 11 foliage placers,
      les 6 décorateurs et la végétation sont écrits — mais la parité des
      arbres est à **40,5 %** de troncs au bon endroit et **43,1 %** de formes
      exactes parmi eux : les placeurs simples sont justes, les ramifiés
      (`fancy`, jungle, `giant`) ne le sont pas, et chacun est nommé)*
      *(**2026-09-11 — features-2** : **150/194** configurées, 183 placed, et
      les 20 placed de biome encore non construites sont toutes des refus
      nommés. Géode **100 %** des blocs, champignons géants 95 %, herbe marine,
      cornichons et magma 88,5 %, varech 90 %, herbe et fleurs des plaines
      91,8 % (témoins décalés 0–20 %). Arbres ramifiés : acacia 48 → 74,5 %,
      chêne noir 0 → 38 %, épicéa géant 0 → 37,5 %, mangrove 0 → 44 %, jungle
      géante 0 → 29 %, cerisier 0 %. Graine 987654321 hors échantillon :
      troncs au bon endroit 67 → 164 sur 364. Restent coraux (37–53 %),
      dripstone (59 %), cerisier, et `monster_room`, fossiles, icebergs, sculk.
      Voir `docs/provenance/features.md`)*
- [~] **Placement** des structures : les 19 `structure_set`, grille, spreads,
      réducteurs, tirage pondéré, tags de biome, filtre de dimension
      *(**rappel 36/36, 5/5 et 17/17** sur trois mondes de référence, et zéro
      faux négatif de grille sur les 18 ensembles à écartement. Le filtre de
      dimension n'est pas cosmétique : sans lui l'Overworld place **1286
      fossiles du Nether** sur 5092 chunks, avec une grille et un réducteur
      pourtant justes. L'ancre jigsaw est le seul désaccord de fond, et il est
      présenté comme tel : milieu et coin donnent des agrégats identiques et
      des erreurs opposées)*
- [~] **Structures**
      *(**2026-09-11 — les structures à gabarit** : lecteur de gabarits NBT lus
      à l'exécution dans le jar serveur (rien d'extrait ni de commité),
      rotation, miroir et 8 processeurs. Pièces du jeu posées par notre code :
      épaves **1 236/1 236** et 383/383 blocs, igloos **456/456** et 546/546,
      ruines océaniques 92 %, portails en ruine 94-97 % ; graines de butin des
      coffres 27/28 au bit près. Pièces tirées de la graine : **100/100 départs**
      identiques au jeu. **2026-09-11 : posées par le serveur** (Overworld,
      Nether, End) : étage attaché à chaque pile de génération, vidé avec le
      carré (déterminisme), `structures.starts` et `References` écrits au
      format du jeu — l'épave échouée du chunk (9, 5) identique champ pour
      champ, types compris ; portails et trésor refusés par le serveur tant
      que leur hauteur n'est pas réglée, tout refus nommé au journal
      (`structures.md` § 20). Temples du désert et
      de la jungle, cabane de sorcière : construits en code par le jeu,
      refusés par nom. Jigsaw (villages, avant-postes, bastions…) : autre
      mandat. Voir `docs/provenance/structures.md` §§ 12-20)* :
      villages ×5 (plains, desert, savanna, taiga, snowy),
      avant-poste pillard, mine abandonnée (+ mesa), forteresse (stronghold),
      pyramide du désert, temple de la jungle, igloo, cabane de sorcière,
      monument marin, ruines océaniques (froides et chaudes), épave (+ échouée),
      trésor enfoui, portail en ruine ×7, manoir, forteresse du Nether,
      bastion ×4, fossile du Nether, cité de l'End, **cité antique**,
      **ruines de sentier**, puits du désert, donjon, fossile, geode
- [ ] Jigsaw, pools de structures, ancrages
- [x] **Nether** : portails, allumage, ratio 1:8, liaison de portails
      *(**2026-09-11** : génération du Nether contre le vrai serveur à la graine
      1234567890 — biomes 3 071 999/3 072 000 cellules, masques de carvers
      166/166 chunks, blocs après bruit, surface et carvers 76,9 → **99,802 %**,
      chunks finis 99,913 % —, une seconde dimension dans le serveur (carte de
      chunks, génération asynchrone, `DIM-1/region`, dimension du joueur
      partout), portails de 2×3 à 21×21, 80 ticks en survie, recharge 300,
      recherche à 128/16 puis création : notre algorithme retrouve le portail
      du jeu **6/6** sur son terrain, et le portail construit par notre
      serveur est celui du jeu bloc pour bloc. Aller-retour de bout en bout
      11/11. Au passage, `old_blended_noise` lisait ses octaves à l'envers avec
      une graine de la mauvaise source : solide/air de l'Overworld 99,141 →
      **99,763 %** et le décalage de surface de ~2 blocs disparaît. Restent les
      mobs du Nether, 19 features et ses structures, et plusieurs gestes dans
      le Nether (conteneurs, TNT). Voir `docs/provenance/nether.md`)*
      *(**2026-09-11 — nether-2** : les neuf types de feature du Nether
      (pierre lumineuse **148/148**, basalte 10,8 → 94,8 %, forêts cramoisies
      et biscornues aux trois quarts pour les tiges, à moitié pour les
      chapeaux), le **fossile du Nether** tiré de la graine (**185/185** départs
      identiques au jeu, gabarit, rotation et position), et les **mobs du
      Nether** dans un monde d'entités à eux : apparition par les listes et les
      règles du Nether, onze espèces aux vitesses mesurées (piglin 0,6, hoglin
      0,4, strider 1,0 sur la lave et 0,66 froid…), troc des piglins (χ² 22,65
      à 16 ddl contre la table chez le vrai serveur ; 120 ticks chez nous),
      salves du blaze et charge du ghast mesurées, boules de feu qui allument et
      explosent. De bout en bout **8/8**. Restent la forteresse (pièces en code)
      et le bastion (jigsaw), les patchs de champignons, l'attache des
      structures par le serveur, le corps à corps des mobs. Voir
      `docs/provenance/nether-2.md`)*
- [x] **End** : îles principales, îles extérieures, passerelles, portail de sortie
      *(**2026-09-11** : bruit `end_islands`, règle de biomes, `end_spike`,
      `end_island`, `chorus_plant`, `end_gateway`. Seed 1234567890 : biomes
      **3 880 960/3 880 960**, blocs **99,9996 %** sur 300 chunks, chunks finis
      99,9888 %, piliers et cristaux 10/10, portail de sortie 20/20 cellules,
      ordre des passerelles 20/20. Troisième niveau dans le serveur (`DIM1`),
      portail de l'End ouvert au 12ᵉ œil, arrivée (100,5 ; 49 ; 0,5) mesurée.
      Restent le chorus à 92 % et une passerelle en trop, et la sortie par le
      portail non mesurée. Voir `docs/provenance/end.md`)*

### Entités et IA
- [x] Physique d'entité : gravité, traînée, collision par la boîte mesurée
      *(g et d **mesurés** dans le tag `Motion` d'un mob lâché de y = 300 :
      0,08 / 0,98 pour un mob, **0,04 / 0,98 pour une pile au sol** — la moitié
      de la gravité, ajustement exact. Une flèche ne suit pas ce modèle et
      c'est dit plutôt que caché. Un client vanilla voit huit types apparaître
      et tomber sur le sol au bloc près — `scripts/check_entities.py`)*
- [x] ECS EnTT : handles et stockage ; comportement polymorphe 🔒
      *(`ov_entity` L8. EnTT en `PRIVATE_DEP` derrière un PIMPL, aucun
      en-tête public ne le nomme. Le tick parcourt l'ordre d'insertion et
      pas une vue : l'ordre d'une vue est celui du stockage, et le
      swap-and-pop d'une destruction le change — voir `docs/PROVENANCE.md`)*
- [~] Attributs, modificateurs, équipement
      *(les valeurs de base des 13 attributs relevées sur un vrai serveur
      1.20.1 pour les 120 types mesurables — 622 valeurs — et une absence
      reste une absence. **Les modificateurs sont faits** : bornes des 13
      attributs 13/13, trois opérations bit pour bit, et l'ordre à l'intérieur
      d'une opération est celui des seaux de hachage de Java — 5/5, contre 2/5
      pour l'ordre d'insertion. L'équipement reste à faire)*
- [~] **33 effets de statut**
      *(**2026-09-10** : les 33 portés, envoyés et décomptés ; règles de
      remplacement et effet caché mesurés. Régénération, poison et wither
      **522/522** intervalles (amplificateurs 0-5), instantanés 24/24 morts-
      vivants inversés compris, résistance 36/36, couleur de particules 64/64,
      cassage sous haste / fatigue / conduit 30/32 au tick près. Branchés :
      manger, lait, miel, mort, cassage, mêlée, chute, souffle. **Restent** les
      effets sur les mobs, la persistance (le serveur n'écrit aucun fichier
      joueur), et les effets sans règle serveur — nausée, lévitation, dolphin's
      grace, bad omen, hero of the village. Voir `docs/provenance/effets.md`)* :
      speed, slowness, haste, mining_fatigue, strength,
      instant_health, instant_damage, jump_boost, nausea, regeneration,
      resistance, fire_resistance, water_breathing, invisibility, blindness,
      night_vision, hunger, weakness, poison, wither, health_boost, absorption,
      saturation, glowing, levitation, luck, unluck, slow_falling,
      conduit_power, dolphins_grace, bad_omen, hero_of_the_village, darkness
- [x] Pathfinding A* avec node evaluators (terrestre, aquatique, aérien)
      *(**le même labyrinthe donné au vrai serveur et à nous donne la même
      route** — ouvertures à −7, +7, −7, prises dans cet ordre une fois
      chacune. Vitesse de poursuite à 0,7 %, rayon d'acquisition encadré
      `]32, 40]`. Aquatique et aérien sont écrits, sans oracle et utilisés par
      aucune des 8 espèces)*
- [x] Système de **goals** (mobs classiques)
      *(file par priorité, drapeaux de contrôle et **éviction**. Six bugs sont
      sortis des mesures et aucun n'était visible autrement, dont un squelette
      et une araignée qui se prenaient mutuellement pour cible et restaient
      figés — trouvé en lançant le vrai serveur, pas par les tests)*
- [ ] Système de **brains / activities / memories** (villageois, piglins,
      axolotls, grenouilles, warden)
- [~] Règles de spawn : lumière, biome, hauteur, plafond, densité, structure
      *(**branché** : une boîte scellée et non éclairée rassemble 20 monstres en
      trois minutes, les vaches n'apparaissent que dehors sur l'herbe éclairée.
      Seuil de lumière identique au jeu — 26/35/27 apparitions à lumière 0 et
      **zéro** de 1 à 10 — et cap monstres à 70 contre un plateau vanilla à
      70,88. Le coût est nommé : 5 ticks sur 921 dépassent le budget, et c'est
      le spawner qui fait son travail. Le cap créatures n'est ni confirmé ni
      infirmé)*
      *(**2026-09-11 — mobs-2** : le type est tiré dans le biome de la position
      (739 entrées `spawners` sur 62 biomes), règles par type (sol par tag, ciel
      ouvert pour husk et stray, slimes des marais et des chunks à slime), et la
      lumière d'un monstre est le **tirage** documenté et non un seuil 0 — qui
      interdisait tout monstre en surface la nuit. Loi de marche
      `0,98·s²·(0,6/f)³/(1−0,91·f)` : 19 espèces à 0,2 % près sur notre serveur.
      Composition par biome non mesurée sur notre serveur. Voir
      `docs/provenance/mobs-2.md`)*
      *(**2026-09-11 — nether-2** : les règles du Nether — aucune porte de
      lumière par catégorie, piglin, hoglin et piglin zombifié partout sauf sur
      le bloc de verrue, ghast une tentative sur vingt, strider dans la lave
      sous l'air, obscurité du Nether pour l'enderman et le squelette, rien sur
      la bedrock, y tiré jusqu'au toit. Les `spawn_costs` (vallée des âmes,
      forêt biscornue) et les `spawn_overrides` des forteresses ne sont pas
      lus. Voir `docs/provenance/nether-2.md`)*
- [x] Despawn, persistance, cap de mobs par catégorie
      *(les caps et la persistance sont là ; `decide_despawn` est écrit,
      testé et **appelé par personne** — les mobs s'accumulent jusqu'au cap et
      y restent)*
      *(**2026-09-11 — mobs-3** : despawn **branché**, chaque mob chaque tick
      contre le joueur le plus proche. Vanilla mesuré : immédiat au-delà de
      128, rien sous 32, et entre les deux 600 ticks d'inactivité puis une
      décroissance — ajustée à 600 et 1/850 sur 32 zombies, contre 600 et 1/800
      appliqués ; **les vaches et les villageois ne partent jamais** et un
      `CustomName` seul **n'épingle pas**. Les mobs sont **sauvés** dans
      `entities/r.x.z.mca` au format 1.20.1 (lus au chargement d'un chunk,
      écrits au déchargement et à chaque sauvegarde), champs non modélisés et
      entités étrangères rendus intacts ; un zoo de 24 mobs écrit par le vrai
      serveur relu par le nôtre (24/24 sur le fil), réécrit, et relu par le
      vrai serveur (24/24). `NoAI` est rendu mais ignoré. Voir
      `docs/provenance/mobs-3.md`)*
      *(**2026-09-11 — persistance** : les mobs du **Nether** aussi, dans `DIM-1/entities`,
      par un stockage à eux — main principale et taille du cube de magma relues ; le
      **dragon** et les **cristaux** dans `DIM1/entities` : santé, phase et position
      revenues, un cristal détruit ne revient pas, le combat ne redémarre plus. Voir
      `docs/provenance/persistance-entites.md`)*
- [~] **Passifs (32)** : allay, axolotl, bat, camel, cat, chicken, cod, cow,
      donkey, fox, frog, glow_squid, horse, mooshroom, mule, ocelot, parrot,
      pig, pufferfish, rabbit, salmon, sheep, skeleton_horse, sniffer,
      snow_golem, squid, strider, tadpole, tropical_fish, turtle, villager,
      wandering_trader
      *(**2026-09-11 — apprivoisement et montures**, mesuré contre le vrai
      serveur : chat, ocelot, perroquet, cheval, âne, mule apprivoisables,
      montables ou dignes de confiance ; un essai sur trois apprivoise un chat
      (447 morues, χ² p > 0,05, témoins 1/2 et 1/5 rejetés) ; statistiques
      d'un cheval et de ses poulains à la règle 1.20 (écarts-types à 12 % près
      sur 120 poulains ; la mule hérite sa vitesse) ; tempérament +5 par chute ;
      un cheval dompté, sellé et conduit par `Move Vehicle` de bout en bout, et
      revenu de `entities/` — dans trois exécutions sur six : dans deux, le
      cheval monté ne décide pas (ouvert, journalisé). Lapin, renard, tortue,
      abeille, chèvre, dromadaire
      et renifleur vivent, se nourrissent, se reproduisent et gardent leur type
      à la sauvegarde. Restent le vol et l'épaule du perroquet, la ruche, la
      mule née d'un croisement, l'inventaire du cheval. Voir
      `docs/provenance/apprivoisement.md`)*
- [~] **Neutres (14)** : bee, cave_spider, dolphin, enderman, goat, iron_golem,
      llama, trader_llama, panda, piglin, polar_bear, spider, wolf,
      zombified_piglin
      *(**2026-09-11** : le loup — un os sur trois l'apprivoise (435 os), assis
      et à 20 de vie ; colère de 20 à 39 s et toute la meute ; il suit son maître
      au-delà de 10 blocs, est téléporté dès 12, défend son maître et ne mord
      jamais un creeper ; collier, propriétaire et colère sur le fil et sur
      disque. Le lama et le lama marchand se montent et s'apprivoisent
      (tempérament sur 30) ; la chèvre et l'abeille vivent sans leurs attaques.
      Voir `docs/provenance/apprivoisement.md`)*
- [ ] **Hostiles (29)** : blaze, creeper, drowned, elder_guardian, endermite,
      evoker, ghast, guardian, hoglin, husk, magma_cube, phantom, piglin_brute,
      pillager, ravager, shulker, silverfish, skeleton, slime, stray, vex,
      vindicator, **warden**, witch, wither_skeleton, zoglin, zombie,
      zombie_villager
- [ ] **Boss** : ender_dragon (phases, cristaux, combat complet), wither
      *(**2026-09-11 — le combat contre l'Ender Dragon, côté serveur** : les
      onze phases et leurs transitions documentées, les 24 nœuds, un vol ajusté
      aux trajectoires du vrai serveur ; cristaux qui explosent (puissance 6),
      −10 PV au dragon pour celui qui le soignait, strafe et boule de feu,
      nuages de souffle (fiole), corps qui casse tout sauf `#dragon_immune`,
      ailes et tête ; mort animée, **12 000 points** (500 ensuite) ;
      `DragonFight` lu et écrit aux clés et types du jeu ; réinvocation par
      quatre cristaux sur la chronologie mesurée (604 ticks). **Tué de bout en
      bout** contre notre serveur à l'épée, sans commande. Restent : le
      **rendu du dragon** dans notre client (aucun modèle : chantier à part), les
      grandes embardées du vol (p90 du rayon 52 contre 78–84), la santé et la
      position du dragon non sauvées, flèches et tridents qui ne le touchent pas ;
      et le **wither**. Voir `docs/provenance/dragon.md`)*
- [~] Projectiles
      *(**2026-09-10** : flèche, trident, boule de neige, œuf, perle, bouteille
      d'XP et potion jetable volent, se plantent et se ramassent ; arc,
      arbalète et squelettes branchés. Vol ajusté sur vanilla à **1,3·10⁻¹⁴**
      près — l'ordre est avancer, freiner puis tirer vers le bas, l'inverse des
      mobs, d'où le résidu de 2,6·10⁻³ noté dans `mobs.md` ; dégâts 7/7 =
      ⌈vitesse × base⌉, critiques couvrant exactement 6 à 10 ; œufs 5602/776/22
      sur 6400, soit 1/8 et 1/32 ; squelette 60 ticks, 40 en difficile.
      Restent perçage, multishot, Flamme, flèches à effet, Loyauté, Riptide,
      Canalisation, endermite et effets des potions jetables ; boules de feu,
      crachat de lama et balles de shulker n'existent pas. Voir
      `docs/provenance/projectiles.md`)* : arrow, spectral_arrow, trident, snowball, egg, ender_pearl,
      eye_of_ender, experience_bottle, potion, fireball, small_fireball,
      dragon_fireball, wither_skull, llama_spit, shulker_bullet, fishing_bobber,
      firework_rocket
- [ ] Autres non vivantes *(**tnt et falling_block faits** : les 25 blocs
      soumis à la gravité, départs de colonne à 2 ticks d'écart relevés, la
      poudre de béton qui durcit au contact de l'eau en tombant)* : item,
      experience_orb, falling_block, tnt, boat,
      chest_boat, minecart ×7, armor_stand, item_frame, glow_item_frame,
      painting, lightning_bolt, area_effect_cloud, end_crystal, leash_knot,
      marker, **block_display / item_display / text_display / interaction**

### Blocs et items
- [ ] Blocs naturels : pierre et variantes polies et ciselées, deepslate complet,
      tuff, calcite, dripstone, terres, argile, gravier, sables, grès ×4,
      terracotta ×16 + glazed ×16, obsidienne (+ pleurante), bedrock, magma,
      netherrack, sols et sables des âmes, basalte, blackstone, quartz du Nether,
      end stone, glaces ×3, neige, poudreuse
- [ ] Minerais : 8 + variantes deepslate + or du Nether + débris antiques +
      blocs bruts + **cuivre : 4 stades × ciré × coupé × escaliers × dalles**
- [ ] **11 essences de bois** × (bûche, bois, écorcés ×2, planches, escaliers,
      dalles, clôture, barrière, porte, trappe, plaque, bouton, panneau,
      panneau mural, panneau suspendu, bateau, bateau à coffre) + bambou
      (mosaïque, échafaudage)
- [ ] Végétation : herbes, fougères, ~20 fleurs, champignons et blocs, nylium,
      racines et vignes du Nether, vignes, lianes luisantes, mousse, azalée,
      dripleaf ×2, spore blossom, lichen, kelp, algues, 5 coraux × bloc /
      éventail / mort, nénuphar, cactus, canne, bambou, chorus, citrouille,
      melon, tiges, 7 cultures, baies ×2, 11 saplings, mangrove, boue
- [ ] Blocs fonctionnels : établi, four ×3, enclume ×3, table d'enchantement,
      chaudrons ×4, brassage, coffres ×3, tonneau, entonnoir, distributeur,
      dropper, observateur, jukebox, note block, table de cartes, chevalet,
      métier à tisser, table de forge, pierre de taille, meule, compostier,
      lutrin, cloche, ruche et nid, lanternes ×2, chaînes, torches ×2, feux de
      camp ×2, TNT, lits ×16, bannières ×16, panneaux, cadres ×2, porte-armure,
      têtes ×6, pot décoré, sable et gravier suspects, famille sculk complète,
      pistons, slime, miel, cible, paratonnerre, conduit, balise, ancre de
      résurrection, lodestone, cristal de l'End, œuf de dragon, portails,
      spawner, blocs de commande ×3, structure, jigsaw, barrier, light
- [ ] Blocs décoratifs : laine / tapis / béton / poudre / verre teinté / vitres
      ×16 chacun, shulker ×17, bougies ×17, gâteau, toutes les briques, ~20
      murs, toutes dalles et escaliers, sea lantern, glowstone, shroomlight,
      froglights ×3, améthyste + clusters ×4, dripstone pointue, foin, kelp
      séché, os, netherite, éponge ×2, toile, échelle, échafaudage, rails ×4,
      barreaux
- [ ] Outils 5 matériaux × 6 · armures 5 matériaux × 4 + turtle helmet + elytra
      + armures de cheval
- [ ] Arc, arbalète, flèches ×3, trident, bouclier, canne à pêche, briquet,
      seaux (eau, lave, lait, poudreuse, 6 poissons, têtard)
- [ ] ~40 aliments avec valeurs de faim et de saturation
- [~] Toutes les potions (normale / jet / persistante) et flèches trempées
      *(**2026-09-11 — alchimie** : l'alambic (400 ticks, 20 brassages par
      poudre, entonnoirs 13/13), **2709/2709** triplets de recettes et **43/43**
      potions bues identiques au vrai serveur, jetable (`1 − d/4`), persistante,
      flèches trempées (÷ 8), ragoût suspect. Restent les effets sur les mobs,
      la fabrication des flèches trempées, la couleur des dégâts instantanés.
      Voir `docs/provenance/alchimie.md`)*
- [ ] Livres ×4, cartes, boussoles ×2, horloge, longue-vue, loupe
- [ ] 20+ disques musicaux · ~70 œufs de spawn
- [ ] Matériaux : bâton, silex, cuir, fil, os et poudre, blaze, ghast tear,
      magma cream, slimeball, perle, œil, nether star, membrane, coquille de
      shulker, nautile, cœur de la mer, écaille, rayon de miel, encre ×2,
      **16 teintures**, netherite, améthyste, echo shard, fragments
- [ ] **Smithing templates** : netherite upgrade + **16 armor trims**
- [ ] **20 pottery sherds** · brush · **8 cornes de chèvre**

### Systèmes de jeu
- [~] **Redstone** : poussière et propagation, torches, blocs, leviers, boutons,
      plaques ×4, fil de détente, crochet, répéteur (+ verrouillage),
      comparateur (comparaison et soustraction, mesure de conteneur),
      observateur, pistons (+ quasi-connectivité, limite de 12, moving piston),
      distributeur (tous ses comportements), dropper, entonnoir (transferts,
      verrouillage), rails ×4 et wagonnets, note block, jukebox, cloche, cible,
      paratonnerre, **famille sculk** (capteur, calibré, shrieker, catalyst,
      veine), portes / trappes / portillons, lampe, TNT, ordre de mise à jour
      et block ticks
      *(le **modèle de puissance** est mesuré circuit par circuit : 19/19
      niveaux de fil, 4/4 délais et le verrouillage, 72/72 cellules de
      comparateur, 28/28 remplissages de conteneur, pistons 0-12 contre 13-14,
      quasi-connectivité confirmée réelle et **pistons seulement**. 830 blocs
      sondés pour la conductivité, **36 exceptions nommées** ; 979 pour la
      réaction au piston, **101 déclarés non lus**. Les **effets** des
      consommateurs sont mesurés : **note block 987/987 blocs** — il fallait une
      table, une heuristique par suffixe se trompe sur 51 blocs — entonnoir
      8,07 ticks/objet et 0 quand verrouillé, distributeur relevé sur 18 items,
      mèche de TNT 80 ticks. **Les boutons et plaques se relèvent** : 20 ticks
      pierre, 30 les onze bois, mesurés tick par tick et confirmés une seconde
      fois sans horloge réseau. Restent les rails, la cible, le transport
      d'objets par entonnoir dans le serveur, et l'algorithme d'explosion —
      refusé faute de résistance des blocs, absente de tout rapport)*
      *(**2026-09-11 — rails et wagonnets** : formes des rails **1536/1536**
      poses et 4608/4608 voisins sur le banc exhaustif, alimentation des
      propulseurs et activateurs (9 rails, 17 au milieu, pente), détecteur et
      comparateur sur wagonnet à coffre, et **14 trajectoires rejouées tick
      par tick à moins de 1e-9** du vrai serveur. Wagonnets posés, montés,
      sauvegardés, relus par vanilla. Restent le rendu dans notre client, les
      collisions, les fenêtres coffre/entonnoir, l'aspiration, le distributeur
      et le wagonnet TNT. Voir `docs/provenance/rails-wagonnets.md`)*
- [~] **Fluides** : écoulement, sources, mélanges (pierre / cobble / obsidienne),
      poussée d'entités, waterlogging, colonnes de bulles, éponge
      *(**branché au tick** : casser le bord d'un bassin fait un losange exact
      dont le niveau est la distance de Manhattan, 121/121 positions.
      **3172/3172 positions** identiques au vrai serveur, dont 1444 sur
      quatre labyrinthes à graine fixe jamais regardés pendant l'écriture des
      règles. La lave perd **2 niveaux par bloc** dans l'overworld et 1 dans le
      Nether ; le rayon de recherche du trou est exactement 5 ; l'eau qui coule
      ne waterlogue jamais. Restent les colonnes de bulles et la **magnitude**
      de la poussée, non mesurée — la direction l'est)*
- [~] **Fabrication et fonte** : recettes façonnées et informes, fours ×3,
      pierre de taille, forge, livre de recettes
      *(**1174 / 1174 recettes chargées**, 0 refusée, les 30 déclarées sans
      appariement nommées ; **2885 grilles posées, 2885 identiques** au vrai
      serveur — dont 120 arrangements qui doivent ne rien produire, le groupe
      qu'un apparieur trop gourmand rate. 248 combustibles mesurés sur 1254 ;
      trois tables de cuisson et non une avec un diviseur. Restent la fenêtre
      2×2 du joueur, les écrans de forge et de pierre de taille,
      `Place Recipe`, et un four qui ne tourne que pendant qu'on le regarde)*
- [~] **Agriculture et élevage** : toutes les cultures, terre labourée,
      hydratation, os, composteur, abeilles et pollinisation, mode amour,
      croissance, croisement de chevaux et lamas, apprivoisement, tonte, traite,
      pêche, sniffer et graines anciennes
      *(**2026-09-10 — le random tick existe** : `randomTickSpeed` tirages par
      section, chunks tickés à moins de 128 blocs d'un joueur. Cultures ×6,
      verrue, baies, cacao, tiges et fruits, varech, canne, cactus, herbe et
      mycélium, fonte, feuilles, pousses → arbres. Hydratation **676/676**,
      décomposition des feuilles **288/288**, croissance homogène à vanilla
      (χ² p = 0,58 à 0,82) quand un témoin à mauvaise probabilité est rejeté
      à p ≈ 0 ; poudre d'os mesurée — la betterave prend +1 dans 3/4 des cas,
      pas 2/3. La mesure a corrigé deux règles du wiki (torchflower, seuil de
      la glace). Et un bug de worldgen : **toutes les feuilles générées étaient
      à distance 7**, le premier random tick aurait rasé les forêts. Restent
      l'élevage entier, le composteur, les abeilles, la pêche, le sniffer,
      la météo — donc la terre qui s'humidifie sous la pluie — et le
      piétinement. Voir `docs/provenance/agriculture.md`)*
      *(**2026-09-10 — l'élevage des quatre espèces vivantes** : bébés (`Age`
      −24 000, métadonnée 16, boîte moitié et yeux mesurés), croissance par la
      nourriture, reproduction — veau 59-62 ticks après le repas contre 59-60,
      `InLove` 600, repos 6000, XP 1-7 (χ² p ≈ 0,69) —, tentation à 10 blocs,
      tonte 1-3 laines, teinture et couleurs héritées (256 paires), broutage
      qui refait la laine, ponte 6000-12 000 ticks, traite, selle, et l'œuf
      lancé qui fait éclore un poussin. Restent les autres espèces (lapin,
      équidés, loup, chat), l'apprivoisement, et la persistance des mobs.
      Voir `docs/provenance/elevage.md`)*
      *(**2026-09-11 — apprivoisement et croisement des chevaux et lamas** :
      loup, chat, perroquet, ocelot, cheval, âne, lama ; poulains à la règle
      1.20 mesurée sur 132 naissances, louveteaux à leur maître, et l'élevage
      des lapins, renards, chèvres (traite), tortues, abeilles et dromadaires.
      La force d'un lama, que le wiki donne à 1 sur 25, est ajustée sur 160
      mesures. Voir `docs/provenance/apprivoisement.md`)*
- [~] **Enchantement**
      *(**2026-09-11**, mesuré contre le vrai serveur : table **512/512 offres**
      (56 graines, 0-15 étagères, 46 objets) et **57/57 enchantements appliqués**,
      coût = rang du bouton + 1 niveaux et lapis ; occultation des étagères
      272/272 cellules ; enclume **93/93** combinaisons et 37/37 durabilités,
      dégradation 0,140 pour 12 % ; Protection 46/46 ; Solidité 0,530/0,324/0,253 ;
      Raccommodage exact ; meule. Trois erreurs de documentation relevées, dont
      l'archive du protocole qui masque à tort la propriété 3 de la table. Au
      passage : `Set Creative Slot` gardait enfin le NBT des objets. Manquent
      Épines, Loyauté, Canalisation, Semelles givrantes, pêche et Lien éternel,
      faute de système hôte. Voir `docs/provenance/enchantement.md`)* : table, coût XP, lapis, étagères, **39 enchantements**,
      enclume (combinaison, réparation, renommage, coûts, « trop cher »), meule,
      mending, livres enchantés
- [~] **Alchimie** : support de brassage, blaze powder, verrue du Nether, tous
      les ingrédients, redstone / glowstone / poudre à canon / œil d'araignée
      fermenté, 3 formes de potion, flèches trempées
      *(**2026-09-11** : alambic, 17 ingrédients, **2709/2709** recettes et
      **43/43** potions identiques au vrai serveur, jet et persistance. Restent
      la fabrication des flèches trempées et les effets sur les mobs. Voir
      `docs/provenance/alchimie.md`)*
- [ ] **Forge** : table, netherite upgrade, 16 trims × matériaux
- [~] **Villageois** : 13 professions, blocs de travail, niveaux, XP, offres,
      réapprovisionnement, gossip, popularité, reproduction, panique, golems de
      fer, cloche, zombification et guérison
      *(**2026-09-11** : métiers par bloc de travail (13/13 mesurés), écran de
      commerce (menu 18, Merchant Offers octet pour octet au NBT près),
      **4298/4298** offres échantillonnées sur 2319 villageois vanilla
      retrouvées dans nos lots, prix avec demande (4/4), seuils de niveau
      10/70/150/250, orbes, réapprovisionnement deux fois par jour, panique,
      lit la nuit. Restent ragots et popularité, reproduction, golems, cloche,
      zombification et guérison (mesurées, non branchées), type selon le
      biome, persistance. Voir `docs/provenance/villageois.md`)*
      *(**2026-09-11 — mobs-3** : **zombification branchée** (jamais en
      facile, une fois sur deux en normal, toujours en difficile) — le
      villageois zombie garde type, métier, niveau, XP et offres (indice 20) ;
      **guérison** par pomme d'or sous Faiblesse (potion jetable), 3600 +
      0..2400 ticks, accélérée par barreaux et lits, le même villageois au bout.
      La remise de prix n'est pas faite : pas de réputation. Villageois
      **sauvés** avec leurs offres. Voir `docs/provenance/mobs-3.md`)*
- [ ] **Raids** : mauvais présage, vagues, capitaines pillards, ravageurs,
      récompense héros du village
- [ ] **Structures interactives** : balise (pyramide, effets), conduit, table de
      cartes, chevalet, métier à tisser et motifs de bannière, pierre de taille,
      lutrin, coffre de l'Ender, shulker box, compostage, chaudrons, feux de
      camp, ruches
- [~] **Survie** : vie et dégâts, invulnérabilité, faim et épuisement,
      40 aliments, mort et réapparition, expérience, oxygène
      *(un chiffre par campagne, mesuré : 30/30 hauteurs de chute — les dégâts
      sont un **`ceil`** et non un `floor`, et le tick d'atterrissage ne compte
      pas —, 15/15 fenêtres d'invulnérabilité, 41/41 coûts de niveau, 12/12
      lâchers d'XP, 44/44 dans l'ordre du codec `damage_type`. Deux corrections
      de protocole : Combat Death **ne porte pas** l'id du tueur en 1.20.1, et
      il n'y a **pas** de Hurt Animation. Restent les sources de dégâts au
      corps à corps, projectile et feu, la réapparition au lit, l'armure, et
      l'XP de minage dont la sonde est intermittente)*
      *(**2026-09-11 — mobs-3** : **les mobs hostiles frappent** — ils ne
      visaient même pas le joueur, absent du monde des entités. Dégâts selon la
      difficulté et **l'armure** (points et robustesse, avant Résistance et
      Protection) : 22/22 cellules identiques au vrai serveur, puis 15/15 de
      bout en bout sur le nôtre ; Faim du husk 140/280, Poison de l'araignée
      venimeuse 140/300, flèche de stray Lenteur 600 ; la flèche de mob et
      l'explosion suivent la difficulté. Voir `docs/provenance/mobs-3.md`)*
- [~] **Combat et utilisation** : frapper, utiliser, manger, user un outil
      *(**jauge d'attaque 13 marches sur 13** et 5/5 pour une arme au
      refroidissement différent, `0,2 + 0,8·f²` ; dégâts et refroidissement de
      **34 objets sur 34** par deux routes indépendantes ; critique ×1,5 exact ;
      recul à deux impulsions dont la seconde suit le **regard** ; **91 tables
      de butin de mob sur 92** ; durabilité de 31 outils. **Branché** : le
      levier se tire et le fil s'allume 14/14, portes et trappes s'ouvrent au
      clic, cinq vaches lâchent 14 bœufs et 3 cuirs, et manger fait remonter la
      faim de 13 à 16. Le **seau** est refusé et nommé : il n'agit pas par
      `Use Item On` mais par `Use Item` avec un lancer de rayon serveur)*
- [x] **Explosions et résistance des blocs**
      *(**branché le 2026-09-10** : la TNT s'allume au briquet, au signal
      redstone et par une explosion voisine — mèche en chaîne 10 à 29, les 20
      valeurs relevées sur 384 —, vole avec la gravité 0,04 et la traînée 0,98
      sur trois axes, mesurées sur 3950 échantillons à 2,9e-9, et explose à
      80 ticks en deux phases, entités blessées entre les deux. Le creeper
      gonfle 30 ticks et rend 0,337 de ses blocs. Cratère **de bout en bout
      par notre serveur** : union 265/271, intersection 157/170, écart de
      fréquence 0,048 contre 0,269 pour un témoin décalé d'un bloc. Paquet
      Explosion en **0x1D, pas 0x1E** comme le dit l'archive. Restent le feu,
      le distributeur de TNT et le contenu des conteneurs détruits — voir
      `docs/provenance/tnt-et-gravite.md`)*
      *(mesure d'origine :)*
      *(la résistance au souffle des **987 blocs mesurée** — elle n'est dans
      aucun rapport du data generator : 959 blocs debout sur le banc, 947 dans
      leur propre classe, 1885 lectures sur 1900 en croisant deux bancs,
      **0 inversion** sur 31 valeurs, et les 15 écarts nommés. Le **plafond est
      démontré** : au-delà de R ≈ 30 aucune explosion du jeu ne sépare deux
      valeurs, donc obsidienne 1200 et bedrock 3,6 M sont indiscernables par
      construction. Cratère **97,5 % d'accord** cellule par cellule, tout le
      résidu sur le bord ; dégâts **10/10 exacts**, et le facteur 0,984 des PV
      relus est l'armure du zombie, ce qui prouve que la formule plancherie
      avant l'armure. **Rien n'est branché** : la TNT ne saute pas et le creeper
      ne siffle pas)*
- [ ] **Divers** : explosions et résistance des blocs, feu et propagation,
      foudre et conversions, météo, cycle jour / nuit, sommeil et phantoms,
      gel (poudreuse), noyade, gravité, **archéologie** (brosse, sable et
      gravier suspects, tessons, poteries), cornes de chèvre, cartes au trésor,
      boussole de récupération, bordure de monde, difficulté locale
      *(**2026-09-11 — le feu** : bloc de feu (âge, survie, propagation dans la
      boîte 3×3×6, pluie, biomes humides, `doFireTick`, feu des âmes), la lave
      qui allume (deux ticks aléatoires par tirage, mesuré), entités qui
      brûlent (dégâts toutes les 10 ticks, eau et pluie, Fire Resistance, Fire
      Aspect, Flame, zombies au soleil), feux de camp (600 ticks), et la
      foudre qui allume. Tables de combustion du wiki confirmées contre le vrai
      serveur. Voir `docs/provenance/feu.md`)*

### Interface et client
- [~] Interface du client Ondes VOXEL : police, HUD, inventaire, conteneurs
      *(**2414 glyphes**, avances validées glyphe par glyphe contre un oracle
      indépendant — 0 désaccord ; HUD complet alimenté par les paquets que le
      serveur envoyait déjà ; inventaire du joueur, coffres 9×1 à 9×6, établi
      et les trois fours, avec un geste pour chacun des sept modes de clic ;
      items en icône plate ou en modèle 3D. Coût : **+0,02 ms p50** et
      +0,05 ms p99 d'enregistrement CPU, 0,3 % de la frame. Le serveur reste
      autoritatif — une pile déposée survit à une reconnexion **et** à un
      redémarrage du processus). **L'inventaire créatif** est là aussi : les 14
      onglets de la 1.20.1 obtenus en **exécutant** `CreativeModeTabs` du jar
      serveur — 1689 cases, 1248 items distincts, et exactement 7 items du
      registre qui n'apparaissent dans aucun onglet, nommés. Recherche,
      défilement, et +0,27 ms d'enregistrement CPU : les 45 modèles d'items en
      3D n'ajoutent rien de mesurable au GPU. **2026-09-10 : il était
      inaccessible chez l'utilisateur** — `creative_tabs.json`, gitignoré, n'était
      produit par rien et l'écran retombait en silence sur l'inventaire de
      survie ; `setup_vanilla.sh` le génère désormais et le refus s'affiche.
      Géométrie remesurée **en faisant tourner le vrai client 1.20.1** : pas
      des onglets 27 et non 28, onglets de droite ancrés au bord, armure
      cliquable, onglet opérateur caché ; recherche **1557/1557** piles dans
      l'ordre du vrai client, 18/19 requêtes identiques ; tous les gestes
      mesurés (un clic donne 1 objet, pas une pile) ; barres sauvegardées
      `hotbar.nbt`. Pixels identiques 69 à 96 % selon l'onglet ; restent
      l'éclairage directionnel des blocs, le mélange sRGB, le joueur en
      miniature et le reflet d'enchantement. **Le chat aussi** (2026-09-10) :
      T et `/`, champ de texte partagé avec la recherche créative, historique,
      320 px et 10/20 lignes, fondu **mesuré sur le vrai client** — 9 s opaque
      puis une seconde, et non les 3 s du wiki —, composants traduits et
      stylés, barre d'action, titres, complétion des commandes par l'arbre
      `Commands` et Command Suggestions ; coïncidence au pixel avec le vrai
      client sur les boîtes, le champ et les suggestions ; +0,09 ms p50
      d'enregistrement pour 10 lignes. Restent la complétion des noms dans un
      message, les erreurs d'analyse côté client, les clics dans le chat*
- [~] Écrans *(**2026-09-11** : menu principal au lancement (7 widgets
      identiques au pixel au vrai client), liste des mondes et **Créer un monde
      par graine** (onglets et boutons mesurés sur le vrai client), menu pause
      qui arrête vraiment le serveur intégré (128 ticks mesurés pour 129
      attendus), écran de mort, options Vidéo/Sons/Contrôles/Touches/Langue
      persistées dans `options.txt` — un fichier écrit par le vrai client, 137
      lignes, relu et réécrit à l'octet —, F3. Non mesurés contre le vrai
      client : options, pause, mort, F3 ; l'écran de mort n'a pas été vu de bout
      en bout. Voir `docs/provenance/ecrans.md`)* : menu principal, création et sélection de monde, options
      (contrôles, vidéo, son, langue, accessibilité, packs), pause, mort,
      inventaire, établi, fours ×3, coffres ×4, entonnoir, distributeur,
      enclume, enchantement, brassage, balise, métier à tisser, pierre de
      taille, meule, table de cartes, table de forge, lutrin, commerce
      villageois, cheval et lama, wagonnet-coffre, blocs de commande, structure,
      jigsaw, chat et autocomplétion, liste de serveurs, téléchargement de
      terrain, statistiques, succès, livre de recettes
- [ ] HUD : barre d'action, cœurs (+ absorption, gel, monture), faim, XP,
      armure, oxygène, viseur, effets, boss bar, scoreboard, tab list, titres,
      sous-titres, **écran F3 complet**
- [ ] Rendu : entités et modèles animés, joueur et skins, **~90 types de
      particules**, entités d'affichage, cadres d'item, tableaux, bannières,
      cartes, texte et police, premier plan (main, blocs, armes), GUI 3D des
      items
      *(**2026-09-11 — le cassage** : fissures des 10 étapes sur le vrai modèle
      du bloc (couverture au pixel près contre le vrai client, 0,556 / 5,437 /
      18,158 % aux étapes 0 / 4 / 9), fissures des autres joueurs, particules
      de casse et de frappe (64 par bloc, texturées), sons, contour qui suit la
      forme, calendrier local identique au tick près (pierre à la main 151).
      Restent la main à la première personne et la prédiction locale du bloc
      cassé. Voir `docs/provenance/cassage-bloc.md`)*
- [~] Audio : tous les événements sonores, musique adaptative par biome et
      dimension, disques, sous-titres
      *(**2026-09-11** : musique par situation dans l'ordre du wiki (menu,
      crédits, dragon, End, sous l'eau, créatif, biome, jeu), dimension lue
      dans Login et Respawn, musique des **31 biomes** lue dans le codec de
      Login, barre du dragon par le drapeau 0x02 de Boss Bar ; disques par
      World Event **1010/1011 mesurés** sur le vrai serveur (id d'objet, reçus
      par l'acteur aussi), « Now Playing » en barre d'action, musique qui
      s'efface sous un disque ; sous-titres en bas à droite avec flèches et
      fondu ; clic des boutons des menus. Restent : éclaboussures, crédits,
      Update Tags (liste « sous l'eau » en table), arc-en-ciel de « Now
      Playing », sous-titres non comparés au pixel ; notre serveur n'émet pas
      encore 1010. Voir `docs/provenance/son-client.md`)*
- [ ] Resource packs empilables, i18n, options persistées, captures d'écran

### Commandes et progression
- [~] Parseur Brigadier-like : sélecteurs `@a @p @r @e @s` et tous leurs filtres,
      chemins NBT, coordonnées relatives et locales
      *(**2026-09-10** : arbre littéral / argument / redirection, permissions
      par nœud, erreurs `...<--[HERE]` et suggestions ; 21 types d'argument,
      sélecteurs et filtres, `~` et `^`, états de bloc, durées, texte JSON.
      L'arbre `Commands` envoyé au client, relu par un décodeur indépendant :
      **34/34**, ordre de la racine identique à vanilla. Restent les chemins
      NBT, qui attendent `/data`)*
- [~] **~75 commandes**
      *(**34 livrées** avec leurs alias, chat non signé et console du serveur
      dédié : **269/271 réponses identiques octet pour octet** au vrai serveur
      sur la même sonde — les deux écarts sont le succès « Diamonds! » et la
      liste de `/help`. Manquent notamment `execute`, `scoreboard`, `data`,
      `clone`, `particle`, `playsound`, `worldborder`, `ban`, `whitelist`,
      `trigger`. Voir `docs/provenance/commandes.md`)* : advancement, attribute, ban, ban-ip, banlist, bossbar,
      clear, clone, damage, data, datapack, debug, defaultgamemode, deop,
      difficulty, effect, enchant, **execute** (toutes les sous-commandes),
      experience, fill, fillbiome, forceload, function, gamemode, gamerule,
      give, help, item, kick, kill, list, locate, loot, me, msg, op, pardon,
      particle, place, playsound, publish, recipe, reload, return, ride,
      save-all, save-off, save-on, say, schedule, scoreboard, seed, setblock,
      setidletimeout, setworldspawn, spawnpoint, spectate, spreadplayers, stop,
      stopsound, summon, tag, team, teammsg, teleport, tellraw, time, title,
      trigger, weather, whitelist, worldborder, xp
- [~] **~45 gamerules**
      *(le catalogue est complet — **45/45** noms, types et défauts relevés sur
      le vrai serveur, persistés dans `level.dat` — mais seules celles dont le
      système existe agissent : doDaylightCycle, doMobSpawning, keepInventory,
      randomTickSpeed…)*
- [x] Fichiers joueur `playerdata/<uuid>.dat` au format vanilla
      *(**2026-09-10**. Avant, rien n'était écrit : la position vivait dans une
      table en mémoire et l'inventaire se perdait à chaque déconnexion. Un
      fichier vanilla relu par nous puis réécrit : **159/159 feuilles
      identiques**, et vanilla qui le relit ne change que ce qu'il change sur
      son propre fichier ; notre fichier ouvert par vanilla : position,
      PV, faim, piles et effets retrouvés, **0 feuille perdue**. Les clés que
      nous ne modélisons pas repartent intactes. Bout en bout 45/45 à travers
      un redémarrage. Le solo écrit aussi `level.dat` → `Data.Player`. Voir
      `docs/provenance/donnees-joueur.md`)*
- [ ] Scoreboard, équipes, objectifs, critères
- [ ] **Tous les succès** (story, nether, end, adventure, husbandry) et leurs
      déclencheurs
- [ ] **Toutes les statistiques** (custom, mined, crafted, used, broken,
      picked_up, dropped, killed, killed_by)
- [ ] Fonctions `.mcfunction`, `/function`, tags de fonction
- [ ] Loot tables : tous les prédicats, fonctions et conditions
- [ ] Prédicats, item modifiers
- [ ] Serveur dédié : `server.properties`, whitelist, ops, bans, RCON, query,
      MOTD et icône, permissions, sauvegarde automatique, arrêt propre,
      watchdog, console et complétion

---

## M7 — Distribution

- [ ] Packaging macOS (.app signé et notarisé)
- [ ] Packaging Windows (MSI)
- [ ] Packaging Linux (AppImage, Flatpak)
- [ ] Lanceur avec authentification Microsoft
- [ ] Releases automatisées
- [ ] Documentation utilisateur et contributeur
- [ ] Guide de portage de datapacks
