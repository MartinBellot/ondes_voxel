<div align="center">

# Ondes VOXEL

**Une réimplémentation en C++ de Minecraft Java Edition 1.20.1**

Compatible avec le format de sauvegarde Anvil et le protocole réseau 763.

</div>

---

## Ce que c'est

Ondes VOXEL n'essaie pas de « ressembler à Minecraft ». Il vise une parité
**vérifiable** : un client Minecraft vanilla 1.20.1 doit pouvoir se connecter à
un serveur Ondes VOXEL et y jouer, et une sauvegarde doit pouvoir passer d'un
jeu à l'autre sans corruption.

Cette contrainte change la nature du projet. « Est-ce fidèle ? » est une
question d'opinion ; « ce paquet est-il conforme, ce chunk est-il identique ? »
est une question à laquelle un test répond. Le client vanilla devient l'oracle
qui valide notre serveur — ce qui explique l'ordre des jalons ci-dessous.

## État

**M0 — Fondations.** Le squelette de build, les verrous d'architecture et la
boucle de tick tiennent. Le serveur dédié tourne à 20 TPS sur un monde vide.

L'état détaillé vit dans [`docs/PROGRESS.json`](docs/PROGRESS.json) ;
l'inventaire complet des features dans [`docs/ROADMAP.md`](docs/ROADMAP.md).

| Jalon | Preuve de fin | État |
|---|---|---|
| **M0** Fondations | le job `linux-server-only` refuse un include Vulkan planté | 🟡 en cours |
| **M1** Protocole 763 | le serveur apparaît dans la liste des serveurs du client vanilla | ⚪ |
| **M2** Registres | les 26 000 blockstates ont les IDs exacts de vanilla | ⚪ |
| **M3** Tranche verticale | **un client vanilla se connecte et on y joue — sans Vulkan** | ⚪ |
| **M4** Monde persistant | round-trip de sauvegarde croisé avec Minecraft | ⚪ |
| **M5** Client Ondes VOXEL | deux clients pour un serveur · p99 ≤ 20 ms à 12 chunks | ⚪ |
| **M6+** Contenu | worldgen, entités, redstone, interface, commandes | ⚪ |

## Démarrer

```bash
brew install cmake ninja ccache      # macOS
./scripts/bootstrap.sh               # vcpkg + cache binaire + hooks git

cmake --preset macos-debug
cmake --build --preset macos-debug
ctest --preset macos-debug

./build/macos-debug/bin/ov_dedicated --ticks=100
```

## Assets

**Ondes VOXEL ne distribue aucun contenu de jeu.** Il charge des resource packs
Minecraft réels, au format exact — vous pouvez donc importer le vôtre.

Placez un pack dans `ressourcepacks/`, puis lancez `ov-assetimport` : il
complète ce que le pack ne contient pas (modèles de blocs, blockstates, sons,
polices) depuis votre propre installation de Minecraft, détectée automatiquement
dans PrismLauncher, MultiMC ou le launcher officiel. Tout est écrit dans `run/`,
qui n'est pas versionné.

Le jeu démarre aussi sans aucun asset : un atlas généré par code sert de
substitut, ce qui permet aussi à la CI de tourner sans contenu propriétaire.

## Architecture

Le projet est stratifié, et le graphe ne remonte jamais :

```
ov_base → ov_math → ov_io → ov_nbt → ov_data → ov_registry → ov_world
  → ov_protocol → ov_entity → ov_gameplay → ov_worldgen → ov_sim
  → ov_server | ov_netclient
  ───────── frontière rendu ─────────
  → ov_rhi → ov_render | ov_audio → ov_client
```

Trois choses sont acquises au jour 1 parce qu'elles sont impossibles à
rattraper ensuite : **le solo est du multijoueur** (le canal intégré transporte
de vrais octets sérialisés), **le monde a un seul écrivain** (le thread de tick,
et aucun mutex), et **les IDs réseau sont ceux de Mojang** (le client vanilla les
code en dur et ne les reçoit jamais).

Le layering est tenu par trois verrous cumulés : une fonction CMake qui refuse
les arêtes interdites, un script qui lit les `#include` réels, et un job CI qui
compile le serveur sur une image **sans SDK Vulkan** — de sorte qu'un include
interdit ne peut littéralement pas compiler. `scripts/test_enforcement.sh`
plante chaque violation pour prouver que les verrous se déclenchent vraiment.

Détails : [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) · règles de
contribution : [`CLAUDE.md`](CLAUDE.md).

## Licence

Code sous [Apache-2.0](LICENSE). Voir [`NOTICE`](NOTICE) pour les attributions.

**Ondes VOXEL n'est pas un produit Minecraft officiel. Il n'est ni approuvé par
Mojang Studios ou Microsoft, ni associé à eux.** « Minecraft » est une marque de
Mojang Synergies AB. Le projet ne contient ni code, ni texture, ni son, ni
modèle issus de Minecraft, et **n'est monétisé sous aucune forme**.
