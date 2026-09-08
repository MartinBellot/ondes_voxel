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
- [ ] Sons : `--sounds` implémenté, non activé par défaut (inutile avant M9)
- [x] Empilage des packs selon la priorité vanilla, dossiers **et** zips
- [x] Manifeste de provenance par fichier → `run/assets/PROVENANCE.tsv`
- [ ] `ov-assetgen` : atlas procédural (démarrage sans aucun asset externe)
- [ ] `scripts/setup_vanilla.sh` — guide l'installation de 1.20.1 et du server.jar

---

## M1 — Protocole 763

- [ ] `data/protocol/763.json` — schéma des paquets (dérivé de minecraft-data, MIT) 🔒
- [ ] `ov-pktgen` : **génération** des encodeurs, décodeurs, dumps de debug et
      harnais de fuzz. Écrire 250 paquets à la main est 2 mois de dette 🔒
- [x] VarInt (≤ 5 o) et VarLong (≤ 10 o) — table de la spec vérifiée 🔒
- [x] Chaînes UTF-8 validées, UUID, angle ; limites imposées avant allocation 🔒
- [ ] Position empaquetée (26/26/12 bits), NBT réseau
- [x] Framing par longueur, avec bascule de compression en cours de connexion 🔒
      *(le cas sous le seuil garde le framing compressé, longueur interne à 0)*
- [ ] Chiffrement AES/CFB8, secret partagé 16 octets
- [x] **Mode hors-ligne uniquement** — décision produit, voir `docs/ARCHITECTURE.md` § 8 🔒
- [x] UUID hors-ligne déterministe, vérifié contre `UUID.nameUUIDFromBytes` de Java
- [x] MD5 (requis par l'UUID v3), vecteurs RFC 1321
- [x] Machine à états par connexion : Handshake → Status ✓ / Login ✓
- [x] Login hors-ligne : Login Start, validation du nom, Disconnect explicatif
- [ ] Login Success + passage à l'état Play *(attend le monde, M3)*
- [x] **Status : MOTD, nombre de joueurs, ping ⭐** — vérifié par un client écrit
      depuis la spec : JSON conforme, pong à écho correct
- [ ] Les ~130 paquets Play, round-trip octet à octet
- [ ] Métadonnées d'entité (index / type / valeur)
- [ ] Format de chunk **réseau** — palette bit-packée, **distinct du disque** 🔒
- [ ] Cibles de fuzz sur le décodeur, aucun crash sur entrée malveillante
- [ ] Matrice de conformité `docs/protocol/763/`

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
- [ ] Reste des flags : émission, formes de collision, couleurs
- [x] **Arithmétique mixed-radix des propriétés** — un multiply, un add 🔒
- [x] Test : les **24 135 états** correspondent au rapport de Mojang ⭐
      *(`scripts/check_registry_parity.py`, relit le binaire depuis sa spec)*
- [x] `PalettedContainer` bit-exact : single-value, indirect 4-8 bits, direct 15 bits
- [x] Palette de biomes 4×4×4, 6 bits en direct
- [x] Tags des 10 registres codés en dur, résolution récursive à la compilation
- [ ] Tags des registres dynamiques *(avec `Registry Data`, M3)*
- [ ] Chargeur de datapack `pack_format` 15, empilable
- [ ] `ov-datac` → cache `.ovpack` mmap, position-independent, zéro pointeur
- [ ] Invalidation par version de format + hash de contenu
- [ ] Test : rebuild depuis JSON == cache, **octet à octet** (déterminisme) 🔒
- [x] `LegacyRandomSource` bit-exact — vérifié contre une vraie JVM 🔒
- [ ] `log` fdlibm pour rendre `nextGaussian` bit-exact *(4 ulp d'écart mesuré)*
- [x] `XoroshiroRandomSource` bit-exact — vérifié contre le JDK, seeding compris 🔒
- [ ] `PositionalRandomFactory`, hachage de seeds, vecteurs de référence ⭐
- [ ] `ov-inspect` : dump NBT, région, chunk, registre, paquet

---

## M3 — Tranche verticale ⭐

> **Atteint le 2026-09-08** : un client Minecraft 1.20.1 non modifié se connecte,
> spawne dans un monde superflat éclairé, s'y déplace, casse et pose des blocs.
> Sans une ligne de Vulkan.

- [x] Boucle de tick 20 Hz avec budget et rapport de surcharge
- [ ] `ChunkMap`, système de tickets, niveaux de chargement
- [ ] **Ordonnancement par régions exclusives** — features déborde sur les voisins 🔒
- [ ] Sections copy-on-write en `shared_ptr<const>` 🔒
- [ ] Pool de jobs enkiTS : pinned tasks + priorités
- [x] Générateur superflat
- [x] Lumière du ciel : sunlight direct + propagation par flood fill *(intra-chunk)*
- [x] Propagation de lumière inter-chunks *(voisinage 3×3 chargé)*
- [ ] Lumière de bloc et suppression incrémentale
- [x] Tableaux de lumière nullables à valeur uniforme (divise l'empreinte par 2)
- [x] Heightmaps : stockage, packing 9 bits, sémantique vérifiée sur monde réel
- [x] `WORLD_SURFACE` calculé et maintenu incrémentalement *(air suffit)*
- [x] `MOTION_BLOCKING`, `MOTION_BLOCKING_NO_LEAVES` et `OCEAN_FLOOR` : prédicats mesurés sur 996 des 1003 blocs
- [x] Streaming de chunks selon la distance de vue *(envoi et déchargement)*
- [ ] Physique du joueur : AABB, marche, saut, sprint, accroupissement, nage
- [x] Gestion des joueurs, keep-alive, liste des joueurs
- [x] Entités joueur : apparition, mouvement, rotation de tête, retrait
- [ ] `ov_netclient` + `ClientLevel` (réplique séparée)
- [ ] `LoopbackTransport` SPSC — **octets sérialisés même en solo** 🔒
- [x] **Un client vanilla 1.20.1 se connecte, marche, casse et pose un bloc** ⭐

---

## M4 — Monde persistant

> **Aller-retour croisé vérifié le 2026-09-08** : notre sauvegarde s'ouvre dans le
> vrai serveur Minecraft 1.20.1 sans une erreur, et une sauvegarde réécrite par
> lui se recharge chez nous avec les bons blocs.

- [x] Lecture Anvil : sections, palettes, biomes, heightmaps, block entities
- [x] Écriture Anvil : fichiers région, palettes par nom, écriture atomique : allocation de secteurs, compactage, timestamps
- [x] `level.dat` : version Anvil, générateur, spawn, bordure, DragonFight : gzip, racine `Data`, écriture sûre via `level.dat_old`
- [ ] `DataVersion`, refus explicite des versions non supportées
- [x] Pose : dalles doubles, portes et lits à deux blocs, conventions mesurées
- [ ] Connexions (clôtures, vitres, murets) et forme des escaliers : demandent la face pleine, par état
- [x] Durées de cassage et outils corrects : vérifiés tick pour tick sur 985 des 996 blocs
- [x] Tables de butin, Silk Touch et Fortune : tirages comparés à ceux du vrai serveur
- [x] Entités objet : les butins tombent au sol et se ramassent *(sans gravité ni sauvegarde)*
- [x] Conteneurs : ouverture, clic gauche et droit, hotbar
- [x] Shift-clic : fusion jusqu'à la taille de pile réelle, mesurée item par item
- [ ] Glissés, touches numériques, inventaire du joueur autoritatif
- [x] Mode créatif, sélection d'items
- [x] Sauvegarde périodique et à l'arrêt *(asynchrone via COW : à venir)*
- [x] **Round-trip croisé avec Minecraft vanilla, sans perte** ⭐

---

## M5 — Client Ondes VOXEL

### `ov_rhi` (L13)
- [ ] Instance, device, swapchain, `VK_KHR_dynamic_rendering`
- [ ] Handles opaques index + génération, jamais de pointeur
- [ ] Buffers, images, samplers, VMA
- [ ] Pipelines, `VkPipelineCache` persisté, SPIR-V compilé **hors-ligne**
- [ ] Command lists, **barrières explicites** (pas de state tracker automatique)
- [ ] `FrameContext` : ring de 2, pool de descripteurs et arène de staging
- [ ] Timestamps GPU dès le départ
- [ ] Handles de texture en `u32` — bindless **préparé, non implémenté** 🔒

### `ov_render` (L14)
- [ ] Triangle → quad texturé → cube + profondeur + caméra
- [x] **Pipeline blockstate → variant → model → parent → elements → faces →
      rotations → uvlock** *(1005 blockstates, 6081 références, 62227 quads,
      0 échec — `ov_modelbake run/assets`)*
- [ ] Stitcher d'atlas, mips, animations `.mcmeta`
- [ ] Mailleur **par face depuis le modèle**, occlusion ambiante par sommet
      *(pas greedy : le plan se trompait, voir PROVENANCE — la règle AO et la
      lumière lissée sont écrites et testées, le mailleur reste à faire)*
- [x] Vertex packé 8 octets
- [ ] Arène device-local 384 Mo, free-list en pages de 4 Ko
- [ ] **Index buffer statique partagé** (supprime la mémoire d'index par section)
- [ ] Culling frustum CPU → `drawIndexedIndirect`, 4 draws pour le terrain
- [ ] Passe translucide triée, index buffer mutable dédié
- [ ] Plafond d'upload par frame (les spikes, pas le FPS moyen, sont le risque)
- [ ] Ciel, soleil, lune, étoiles, nuages, brouillard, météo

### `ov_client` (L15) et `ov_audio` (L14)
- [ ] Fenêtre GLFW, entrées, bindings de touches
- [ ] Serveur intégré sur son propre thread
- [ ] Prédiction de mouvement et réconciliation
- [ ] Interpolation d'entités
- [ ] Mixeur audio, sons 3D atténués, catégories de volume
- [ ] **Deux clients pour un serveur · p99 ≤ 20 ms à 12 chunks** ⭐
- [ ] Golden images en CI sur **Linux + lavapipe** (MoltenVK n'est pas un oracle)

---

## M6+ — Contenu complet

### Génération du monde
- [ ] Bruit : Perlin, Simplex, octaves, `NormalNoise`
- [ ] **Interpréteur de `density_function`** — jamais un générateur ad hoc 🔒
- [ ] `noise_router`, `noise_settings`, splines
- [ ] **Multi-noise biome source** : temperature, humidity, continentalness,
      erosion, depth, weirdness
- [ ] `surface_rules`
- [ ] Carvers : grottes, ravins, cheese / spaghetti / noodle
- [ ] Aquifères, lave, niveaux d'eau
- [ ] Minerais par couche, distributions triangulaires
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
- [ ] Features : arbres par essence, végétation, geodes d'améthyste, dripstone,
      lush caves, blocs sculk du deep dark, sources, disques, lacs
- [ ] **Structures** : villages ×5 (plains, desert, savanna, taiga, snowy),
      avant-poste pillard, mine abandonnée (+ mesa), forteresse (stronghold),
      pyramide du désert, temple de la jungle, igloo, cabane de sorcière,
      monument marin, ruines océaniques (froides et chaudes), épave (+ échouée),
      trésor enfoui, portail en ruine ×7, manoir, forteresse du Nether,
      bastion ×4, fossile du Nether, cité de l'End, **cité antique**,
      **ruines de sentier**, puits du désert, donjon, fossile, geode
- [ ] Jigsaw, pools de structures, ancrages
- [ ] **Nether** : portails, allumage, ratio 1:8, liaison de portails
- [ ] **End** : îles principales, îles extérieures, passerelles, portail de sortie

### Entités et IA
- [ ] ECS EnTT : handles et stockage ; comportement polymorphe 🔒
- [ ] Attributs, modificateurs, équipement
- [ ] **33 effets de statut** : speed, slowness, haste, mining_fatigue, strength,
      instant_health, instant_damage, jump_boost, nausea, regeneration,
      resistance, fire_resistance, water_breathing, invisibility, blindness,
      night_vision, hunger, weakness, poison, wither, health_boost, absorption,
      saturation, glowing, levitation, luck, unluck, slow_falling,
      conduit_power, dolphins_grace, bad_omen, hero_of_the_village, darkness
- [ ] Pathfinding A* avec node evaluators (terrestre, aquatique, aérien)
- [ ] Système de **goals** (mobs classiques)
- [ ] Système de **brains / activities / memories** (villageois, piglins,
      axolotls, grenouilles, warden)
- [ ] Règles de spawn : lumière, biome, hauteur, plafond, densité, structure
- [ ] Despawn, persistance, cap de mobs par catégorie
- [ ] **Passifs (32)** : allay, axolotl, bat, camel, cat, chicken, cod, cow,
      donkey, fox, frog, glow_squid, horse, mooshroom, mule, ocelot, parrot,
      pig, pufferfish, rabbit, salmon, sheep, skeleton_horse, sniffer,
      snow_golem, squid, strider, tadpole, tropical_fish, turtle, villager,
      wandering_trader
- [ ] **Neutres (14)** : bee, cave_spider, dolphin, enderman, goat, iron_golem,
      llama, trader_llama, panda, piglin, polar_bear, spider, wolf,
      zombified_piglin
- [ ] **Hostiles (29)** : blaze, creeper, drowned, elder_guardian, endermite,
      evoker, ghast, guardian, hoglin, husk, magma_cube, phantom, piglin_brute,
      pillager, ravager, shulker, silverfish, skeleton, slime, stray, vex,
      vindicator, **warden**, witch, wither_skeleton, zoglin, zombie,
      zombie_villager
- [ ] **Boss** : ender_dragon (phases, cristaux, combat complet), wither
- [ ] Projectiles : arrow, spectral_arrow, trident, snowball, egg, ender_pearl,
      eye_of_ender, experience_bottle, potion, fireball, small_fireball,
      dragon_fireball, wither_skull, llama_spit, shulker_bullet, fishing_bobber,
      firework_rocket
- [ ] Autres non vivantes : item, experience_orb, falling_block, tnt, boat,
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
- [ ] Toutes les potions (normale / jet / persistante) et flèches trempées
- [ ] Livres ×4, cartes, boussoles ×2, horloge, longue-vue, loupe
- [ ] 20+ disques musicaux · ~70 œufs de spawn
- [ ] Matériaux : bâton, silex, cuir, fil, os et poudre, blaze, ghast tear,
      magma cream, slimeball, perle, œil, nether star, membrane, coquille de
      shulker, nautile, cœur de la mer, écaille, rayon de miel, encre ×2,
      **16 teintures**, netherite, améthyste, echo shard, fragments
- [ ] **Smithing templates** : netherite upgrade + **16 armor trims**
- [ ] **20 pottery sherds** · brush · **8 cornes de chèvre**

### Systèmes de jeu
- [ ] **Redstone** : poussière et propagation, torches, blocs, leviers, boutons,
      plaques ×4, fil de détente, crochet, répéteur (+ verrouillage),
      comparateur (comparaison et soustraction, mesure de conteneur),
      observateur, pistons (+ quasi-connectivité, limite de 12, moving piston),
      distributeur (tous ses comportements), dropper, entonnoir (transferts,
      verrouillage), rails ×4 et wagonnets, note block, jukebox, cloche, cible,
      paratonnerre, **famille sculk** (capteur, calibré, shrieker, catalyst,
      veine), portes / trappes / portillons, lampe, TNT, ordre de mise à jour
      et block ticks
- [ ] **Fluides** : écoulement, sources, mélanges (pierre / cobble / obsidienne),
      poussée d'entités, waterlogging, colonnes de bulles, éponge
- [ ] **Agriculture et élevage** : toutes les cultures, terre labourée,
      hydratation, os, composteur, abeilles et pollinisation, mode amour,
      croissance, croisement de chevaux et lamas, apprivoisement, tonte, traite,
      pêche, sniffer et graines anciennes
- [ ] **Enchantement** : table, coût XP, lapis, étagères, **39 enchantements**,
      enclume (combinaison, réparation, renommage, coûts, « trop cher »), meule,
      mending, livres enchantés
- [ ] **Alchimie** : support de brassage, blaze powder, verrue du Nether, tous
      les ingrédients, redstone / glowstone / poudre à canon / œil d'araignée
      fermenté, 3 formes de potion, flèches trempées
- [ ] **Forge** : table, netherite upgrade, 16 trims × matériaux
- [ ] **Villageois** : 13 professions, blocs de travail, niveaux, XP, offres,
      réapprovisionnement, gossip, popularité, reproduction, panique, golems de
      fer, cloche, zombification et guérison
- [ ] **Raids** : mauvais présage, vagues, capitaines pillards, ravageurs,
      récompense héros du village
- [ ] **Structures interactives** : balise (pyramide, effets), conduit, table de
      cartes, chevalet, métier à tisser et motifs de bannière, pierre de taille,
      lutrin, coffre de l'Ender, shulker box, compostage, chaudrons, feux de
      camp, ruches
- [ ] **Divers** : explosions et résistance des blocs, feu et propagation,
      foudre et conversions, météo, cycle jour / nuit, sommeil et phantoms,
      gel (poudreuse), noyade, gravité, **archéologie** (brosse, sable et
      gravier suspects, tessons, poteries), cornes de chèvre, cartes au trésor,
      boussole de récupération, bordure de monde, difficulté locale

### Interface et client
- [ ] Écrans : menu principal, création et sélection de monde, options
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
- [ ] Audio : tous les événements sonores, musique adaptative par biome et
      dimension, disques, sous-titres
- [ ] Resource packs empilables, i18n, options persistées, captures d'écran

### Commandes et progression
- [ ] Parseur Brigadier-like : sélecteurs `@a @p @r @e @s` et tous leurs filtres,
      chemins NBT, coordonnées relatives et locales
- [ ] **~75 commandes** : advancement, attribute, ban, ban-ip, banlist, bossbar,
      clear, clone, damage, data, datapack, debug, defaultgamemode, deop,
      difficulty, effect, enchant, **execute** (toutes les sous-commandes),
      experience, fill, fillbiome, forceload, function, gamemode, gamerule,
      give, help, item, kick, kill, list, locate, loot, me, msg, op, pardon,
      particle, place, playsound, publish, recipe, reload, return, ride,
      save-all, save-off, save-on, say, schedule, scoreboard, seed, setblock,
      setidletimeout, setworldspawn, spawnpoint, spectate, spreadplayers, stop,
      stopsound, summon, tag, team, teammsg, teleport, tellraw, time, title,
      trigger, weather, whitelist, worldborder, xp
- [ ] **~45 gamerules**
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
