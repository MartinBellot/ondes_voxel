# Ondes VOXEL — Règles du projet

Réimplémentation C++ de **Minecraft Java Edition 1.20.1** (protocole **763**, `pack_format` 15),
à parité de gameplay, **compatible sauvegarde Anvil/NBT et protocole réseau vanilla**.

Ce fichier contient les règles **invariantes**. Il n'est pas la roadmap.
→ Plan complet : `docs/ARCHITECTURE.md` · Features : `docs/ROADMAP.md` · État : `docs/PROGRESS.json`

---

## 1. Règles juridiques — ABSOLUES, non négociables

Le projet est sous **Apache-2.0**, dépôt **public**. Les règles suivantes le maintiennent légal.

### Interdit dans le dépôt, sans exception
- **Aucun asset Mojang** : texture, son, musique, police, modèle, `.jar`. Ils vivent dans
  `ressourcepacks/` et `run/`, tous deux **gitignorés**.
- **Aucune donnée du data generator commitée brute** — elle est régénérée localement dans
  `data/vanilla/1.20.1/generated/` (gitignoré). Seuls les **hashes** sont commités.
- **Aucun code décompilé de Minecraft.** Les mappings officiels servent à *nommer* des concepts,
  jamais à traduire du code ligne à ligne.

### Lecture de code tiers
| Verdict | Projets |
|---|---|
| ✅ Sûr à lire et à s'inspirer | **Cuberite** (Apache-2.0) · **Valence** (MIT) · **MCHPRS** (MIT) · **Feather** (Apache-2.0) · **Glowstone cœur** (MIT) · **PrismarineJS/minecraft-data** (MIT) |
| ⚠️ Architecture seulement, jamais de copier-collé | **Luanti/Minetest** (LGPL-2.1+) · **Glowkit** (GPL) |
| 🚫 **NE PAS LIRE** | **Paper · Purpur · Spigot · Bukkit · CraftBukkit** (GPLv3) — incompatible avec Apache-2.0 |

> **Règle d'or** : un système se spécifie depuis la **documentation**, jamais depuis le code source
> d'un autre projet. Toute source documentaire non triviale est tracée dans `docs/PROVENANCE.md`.

### Resource packs
Le pack par défaut est **Faithful 32x**, fourni par l'utilisateur dans `ressourcepacks/`.
Sa licence (Faithful License v3) autorise l'usage comme placeholder, comme base, et pour les
resource packs de serveur, **à condition** de :
1. donner un **crédit clair et spécifique** (« GUI originally from Faithful 32x », pas « des
   textures de Faithful ») ;
2. lier `https://faithfulpack.net/` ;
3. inclure son `LICENSE.txt` **non modifié** avec tout contenu qui en dérive ;
4. **ne JAMAIS monétiser** quoi que ce soit contenant leur travail — pas de paywall, pas de lien de
   téléchargement monétisé. Les dons volontaires restent permis ;
5. ne pas faire passer le projet pour officiel.

Ces obligations sont satisfaites par `NOTICE`. **Le projet ne doit jamais être monétisé.**

### Documentation du protocole
⚠️ `https://minecraft.wiki/w/Java_Edition_protocol` documente la version **courante**, pas la 763.
Utiliser l'**archive figée** :
`https://minecraft.wiki/w/Minecraft_Wiki:Projects/wiki.vg_merge/Protocol?oldid=2773082`
Miroir de secours : `https://c4k3.github.io/wiki.vg/`

---

## 2. Six principes non négociables

1. **Séparation stricte logique / rendu.** Le serveur headless compile avec **zéro** dépendance de
   rendu, garanti par le job CI `linux-server-only` qui n'a pas le SDK Vulkan.
2. **Le solo est du multijoueur.** Le canal intégré transporte des **octets sérialisés**, toujours.
   Une seule exception « objets en mémoire pour aller vite » annule tout le bénéfice.
3. **Un seul écrivain.** Le thread de tick est l'unique écrivain de tout chunk publié.
   **Aucun mutex sur le monde, jamais.** Le partage passe par `shared_ptr<const>` (copy-on-write).
4. **Les IDs réseau sont ceux de Mojang**, pas les nôtres. Le client vanilla les code en dur et ne
   les reçoit jamais. Source de vérité : `data/vanilla/1.20.1/{registries,blocks}.json`.
5. **Déterminisme.** Tick fixe 20 Hz · RNG explicites bit-exacts · `-ffp-contract=off` ·
   `-ffast-math` et `-Ofast` **interdits** · jamais d'horloge murale dans la logique.
6. **Pas de singleton mutable global.** Tout contexte est passé explicitement → N serveurs par
   processus → tests d'intégration parallèles.

---

## 3. Couches — le graphe ne remonte jamais

```
ov_base(0) → ov_math(1) → ov_io(2) → ov_nbt(3) → ov_data(4) → ov_registry(5) → ov_world(6)
  → ov_protocol(7) → ov_entity(8) → ov_gameplay(9) → ov_worldgen(10) → ov_sim(11)
  → ov_server(12) | ov_netclient(12)
  ───────── frontière rendu ─────────
  → ov_rhi(13) → ov_render(14) | ov_audio(14) → ov_client(15)
```

Un module ne peut dépendre que de modules de couche **strictement inférieure**, **et** hors de sa
liste d'interdits (`OV_FORBID_*` dans `cmake/OvModule.cmake`).

**Règles au-delà de la numérotation :**
- `ov_world` ne connaît **ni** le tick, **ni** le réseau, **ni** EnTT. Il expose `LevelView` /
  `LevelWriter` abstraits — c'est ce qui permet à `ov_netclient` et `ov_sim` de partager le même
  stockage de chunks.
- `ov_gameplay` ne dépend **jamais** de `ov_sim` ni `ov_server`. Un comportement de bloc reçoit un
  `LevelWriter&`, pas un `ServerLevel&` — sinon la prédiction client devient impossible.
- `ov_netclient` est un module **client sans rendu**. Un `#include <vulkan/...>` ici et tout est mort.
- `ov_rhi` ignore le jeu. `ov_render` ignore fenêtre et input.

**Interdit** : appeler `target_link_libraries` directement dans `src/`. Utiliser `ov_add_library`.

---

## 4. Commandes

```bash
./scripts/bootstrap.sh            # vcpkg + binary cache + SDK Vulkan + hooks git
./scripts/setup_vanilla.sh        # vérifie l'instance 1.20.1 et le server.jar
./build/tools/ov-assetimport      # ressourcepacks/ + jar client → run/assets/

cmake --preset macos-debug && cmake --build --preset macos-debug
ctest --preset macos-debug        # tests unitaires + intégration
ctest -L bench                    # benchmarks non-régression

./scripts/check_layers.py         # graphe de dépendances (verrou #2)
./scripts/check_assets.py         # aucun asset commité
./scripts/check_progress.py       # ROADMAP.md et PROGRESS.json concordent
./scripts/test_enforcement.sh     # prouve que les garde-fous se déclenchent
```

À venir : `scripts/run_parity.sh` (parité golden vs vanilla) arrive en M2, avec
`tools/ov_parity` — il nécessite `tools/vanilla/server-1.20.1.jar`.

---

## 5. Conventions de code

- **C++20.** Namespace `ov::` + sous-namespace par module (`ov::world`, `ov::net`…).
- Fichiers en `snake_case.hpp` / `.cpp` · types en `PascalCase` · fonctions et variables en
  `snake_case` · membres privés suffixés `_`.
- **Code, identifiants, commentaires et messages de commit en anglais.** Docs en français.
- `std::expected<T, E>` pour les erreurs récupérables. Exceptions réservées aux erreurs fatales,
  **jamais dans le chemin chaud**.
- RAII partout. Pas de `new`/`delete` nus.
- **Aucune allocation dans le tick en régime établi** — vérifié par un hook qui `assert` en debug.
- **Aucun template lourd dans un header public** (EnTT, simdjson, asio, Vulkan restent privés à leur
  bibliothèque ; PIMPL aux frontières). C'est ce qui garde le build utilisable sur 8 Go.
- Chaque header public doit être **self-contained** (job CI `-fsyntax-only`).

---

## 6. Définition de « terminé »

Une tâche n'est terminée que si **tout** ce qui suit est vrai :
1. le code compile sans warning sur les 3 OS ;
2. tests unitaires écrits et verts ;
3. test d'intégration `world_test` si le système a un comportement observable ;
4. **entrée de parité** ajoutée si le système a un oracle vanilla ;
5. documentation à jour (`docs/`, et `docs/PROVENANCE.md` si source documentaire non triviale) ;
6. case cochée dans `docs/ROADMAP.md` ;
7. `docs/PROGRESS.json` mis à jour ;
8. commit atomique avec message en anglais.

## 7. Workflow de session

1. Lire `docs/PROGRESS.json` → identifier le jalon et la tâche en cours.
2. Lire la section correspondante de `docs/ROADMAP.md`.
3. Implémenter → tester → vérifier les gates (`check_layers`, `check_assets`, bench).
4. Mettre à jour `ROADMAP.md` + `PROGRESS.json`.
5. Commit atomique.
