# Architecture

Décisions structurantes d'Ondes VOXEL, et pourquoi elles sont ce qu'elles sont.
Les règles opérationnelles vivent dans [`CLAUDE.md`](../CLAUDE.md) ; ce document
explique les raisons.

---

## 1. Le choix qui commande tous les autres

Ondes VOXEL vise la compatibilité avec le **protocole réseau 763** et le **format
de sauvegarde Anvil**. Ce n'est pas un objectif de plus dans la liste : c'est ce
qui rend le projet testable.

« Est-ce fidèle à Minecraft ? » est une question d'opinion, et un projet qui
répond à des questions d'opinion dérive. « Ce paquet est-il conforme, ce chunk
est-il identique ? » est une question à laquelle une machine répond.

D'où l'ordre des jalons : **le client Minecraft vanilla est notre oracle
principal**, et la première tranche verticale (M3) est un serveur auquel un
client vanilla se connecte, **sans une ligne de Vulkan**. Toute erreur d'ID, de
palette, de heightmap ou de lumière apparaît immédiatement à l'écran d'un
programme que nous n'avons pas écrit. Aucune suite de tests unitaires n'offre
cette densité de vérification par ligne de code — et écrire d'abord un renderer
retarderait de plusieurs mois la découverte de ces erreurs.

Corollaire gratuit : à M5, nous aurons **deux clients pour le même serveur**, et
toute divergence devient localisable par différence.

---

## 2. Les couches

```
ov_base(0) → ov_math(1) → ov_io(2) → ov_nbt(3) → ov_data(4) → ov_registry(5)
  → ov_world(6) → ov_protocol(7) → ov_entity(8) → ov_gameplay(9)
  → ov_worldgen(10) → ov_sim(11) → ov_server(12) | ov_netclient(12)
  ───────────────────── frontière rendu ─────────────────────
  → ov_rhi(13) → ov_render(14) | ov_audio(14) → ov_client(15)
```

Des modules de même numéro sont des frères : ils ne dépendent jamais l'un de
l'autre, et la règle « strictement inférieur » suffit à l'interdire.

Trois règles que la numérotation seule n'exprime pas, encodées dans les
ensembles `OV_FORBID_*` :

- **`ov_world` ignore le tick, le réseau et EnTT.** Il expose `LevelView` et
  `LevelWriter` abstraits. C'est ce qui permet à `ov_sim` (monde autoritaire) et
  `ov_netclient` (réplique client) de **partager le même code de stockage de
  chunks** au lieu d'en maintenir deux.
- **`ov_gameplay` ne connaît ni `ov_sim` ni `ov_server`.** Un comportement de
  bloc reçoit un `LevelWriter&`, jamais un `ServerLevel&`. Sans cette
  discipline, la prédiction côté client devient impossible à ajouter.
- **`ov_netclient` est un module client sans rendu.** C'est le point de
  vigilance : c'est là que la tentation d'un `#include <vulkan/...>` apparaîtra.

### Pourquoi trois verrous et pas une convention

Une convention de layering tient trois mois sur un projet de cette taille. Les
trois verrous sont cumulatifs et de force croissante :

1. **`cmake/OvModule.cmake`** — `ov_add_library` refuse les arêtes interdites, et
   `target_link_libraries` direct sous `src/` est banni pour qu'on ne puisse pas
   la contourner. Attrape l'erreur honnête dans un CMakeLists.
2. **`scripts/check_layers.py`** — lit les `#include` **réels**. Attrape
   l'include ajouté dans un `.cpp` qui lie quand même par transitivité.
3. **Le job CI `linux-server-only`** — compile le serveur sur une image **sans
   SDK Vulkan**. Ici un include interdit ne rate pas un contrôle de politique :
   il ne compile pas. C'est le seul verrou qu'on ne contourne pas par
   distraction. Le job vérifie d'abord qu'aucun SDK Vulkan n'est présent, sinon
   il passerait au vert en ne testant plus rien.

`scripts/test_enforcement.sh` plante chaque violation et vérifie qu'elle est
rejetée. **Un garde-fou qu'on n'a jamais vu se déclencher n'est pas un
garde-fou** : c'est un script qui affiche « ok » et fabrique de la confiance sans
protection.

---

## 3. Les trois décisions irrattrapables

### 3.1 Le solo est du multijoueur

Même en partie solo, le serveur tourne sur son propre thread et le canal intégré
transporte des **octets sérialisés**. Le coût mesuré est inférieur à 0,5 % d'un
cœur ; le bénéfice est que chaque partie solo est un test d'intégration du
protocole.

Une seule exception « objets en mémoire pour aller plus vite » annule tout : le
chemin réseau cesse d'être exercé, casse sans qu'on le sache, et se découvre six
mois plus tard quand quelqu'un lance un serveur dédié — qui est alors devenu un
fork.

### 3.2 Un seul écrivain, aucun mutex sur le monde

Le thread de tick est l'unique écrivain de tout chunk publié. Un chunk est dans
exactement un de trois états : **en construction** (privé à un worker),
**publié** (thread de tick seul), **snapshot immuable** (partagé librement).

Le stockage de blocs d'une section est un `shared_ptr<const PalettedContainer>`
en copy-on-write : une écriture clone si le compteur dépasse 1, pour environ
2,5 Ko. Les bénéfices arrivent en cascade — le mesher obtient un voisinage
3×3×3 cohérent en copiant **27 `shared_ptr` (~200 ns)** sans verrou ni copie de
données ; l'envoi réseau et la sauvegarde asynchrone deviennent triviaux.

La génération pose un problème particulier : le statut `features` **écrit dans
les chunks voisins** (les arbres débordent), donc deux jobs adjacents se
marchent dessus. La réponse est un **ordonnancement par régions exclusives** —
un bitset de chunks réservés, un dispatch seulement si le disque de rayon `r(S)`
ne chevauche rien d'actif. Parallélisme en damier, zéro synchronisation dans le
code de génération, déterminisme conservé. L'alternative du verrou par chunk
fonctionne six mois puis devient un cauchemar de deadlocks dès l'arrivée des
structures inter-chunks.

### 3.3 Les IDs réseau sont ceux de Mojang

En 1.20.1, le client vanilla **code en dur** les IDs de la plupart des registres
— `block`, `block_state`, `item`, `entity_type`, `fluid`, `particle_type`,
`menu`, `sound_event`, `mob_effect`, `enchantment` et les autres. Ils ne sont
**jamais transmis**. Nous n'avons donc aucune liberté : nos IDs doivent être
bit-à-bit ceux de Mojang, y compris l'ordre mixed-radix des propriétés à
l'intérieur de chaque bloc.

Seuls six registres sont réellement dynamiques et envoyés en NBT :
`dimension_type`, `worldgen/biome`, `chat_type`, `damage_type`, `trim_material`,
`trim_pattern`.

Si c'est raté, le client vanilla se connecte puis tout est décalé d'un type de
bloc, ou il crashe sur un `entity_type` inconnu. Le diagnostic prend des
semaines. D'où la source de vérité unique — les rapports du data generator
officiel — et un test CI d'égalité stricte, entrée par entrée, ordre compris.

Point rassurant en contrepartie : **Anvil est name-based depuis 1.13**. La
palette stocke `{Name, Properties}`, donc la compatibilité des sauvegardes ne
dépend d'aucun ID numérique. Le seul endroit où ils fuient vers l'extérieur est
le réseau.

---

## 4. Ce qui est data-driven, et ce qui ne l'est pas

Correction fréquente à faire par rapport à l'intuition « tout est donnée » : **en
vanilla, le comportement des blocs est du code Java, pas des données.** Il ne
faut pas concevoir un DSL de comportement de blocs.

| Vraiment data-driven (JSON) | Code C++ |
|---|---|
| recettes, loot tables, tags, advancements, prédicats, item modifiers | comportement des blocs — 1003 types, mais **80 % dérivent de ~20 archétypes** |
| worldgen : `density_function`, `noise_router`, `surface_rules`, `multi_noise` — un **interpréteur d'expressions**, à implémenter comme tel | IA et logique d'entités |
| modèles et blockstates (assets **client**, pas datapack) | redstone, fluides, physique |

Deux pipelines à ne pas confondre : **`data/`** (datapack, `pack_format` 15) est
**serveur** ; **`run/assets/`** (blockstates, models, textures, fonts) est
**client** et vient du jar client, pas d'un datapack.

---

## 5. Entités : EnTT comme stockage, pas comme dogme

Les entités Minecraft sont profondément OO (`LivingEntity → Mob →
PathfinderMob`), il y a ~100 types, et seulement quelques **milliers**
d'instances. L'ECS pur n'apporte quasiment rien en performance à cette échelle,
et coûte cher en friction : churn de composants, changement d'archétype à chaque
changement d'état.

Retenu : EnTT pour les handles, le stockage et les relations ; le comportement
reste polymorphe dans un composant `std::unique_ptr<IEntityLogic>`. L'itération
ECS est réservée à ce qui est réellement data-parallèle — intégration des
transforms, broadphase AABB, interest management réseau. Décomposer l'IA d'un
zombie en 40 composants coûte un an et ne rapporte rien.

⚠️ EnTT n'est pas thread-safe, et `registry.view<>()` / `group<>()` **mutent
l'état interne** même en lecture seule. Le registry vit sur le thread de tick, et
toutes les vues sont pré-créées au démarrage.

---

## 6. Threading

| Thread | Rôle | QoS macOS |
|---|---|---|
| 1× Main/Render | fenêtre, entrées, soumission Vulkan | `USER_INTERACTIVE` |
| 1× Tick serveur | unique écrivain de l'état monde, 20 TPS | `USER_INTERACTIVE` |
| 4× Workers | worldgen, lumière bulk, meshing, décompression | `UTILITY` |
| 1× IO disque | fichiers région (bloquant) | `UTILITY` |
| 1× Réseau | asio — serveur dédié uniquement | `USER_INITIATED` |

**Le piège macOS.** Sans appel explicite à `pthread_set_qos_class_self_np()`, le
scheduler d'Apple Silicon migre un thread non classé vers un E-core sous charge.
Pour le thread de tick, cela se manifeste par 14-17 TPS sans aucun point chaud
dans un profil : le travail tourne simplement sur des cœurs plus lents. Ce n'est
pas une optimisation, c'est une condition de la garantie 20 Hz — d'où
`set_thread_role()` en première instruction de chaque thread durable.

**Pas plus de 4 workers.** Sur mémoire unifiée, la ressource rare est la bande
passante, pas les cœurs : au-delà de quatre, le débit de meshing **baisse**.
Relever ce plafond demande un benchmark, pas une opinion.

**`enkiTS`, pas un pool maison.** Il apporte les *pinned tasks* (« exécute ceci
sur le thread de tick ») et les priorités (une tâche tick-critique ne doit pas
attendre derrière 200 jobs de meshing), tous deux indispensables. Écrire son
propre pool work-stealing prend environ trois semaines, et la partie sleep/wake
est presque toujours ratée — soit on brûle un cœur en spin, soit on ajoute
200 µs de latence de réveil.

---

## 7. Risques

### R1 — Parité des IDs et du format fil 🔒 M2
Voir § 3.3. Décision associée : **générer les codecs de paquets** depuis un
schéma plutôt qu'écrire 250 structures à la main. Le faire à la main fonctionne
jusqu'au soixantième paquet, puis c'est deux mois de réécriture pour ajouter la
validation, le fuzzing et les tests de round-trip. Le codegen les donne
gratuitement.

### R2 — Couplage client/serveur 🔒 M0
Voir § 3.1 et § 3.2. Neutralisé par trois choses ensemble : `ov_netclient` ne lie
ni `ov_sim` ni `ov_server`, le canal intégré sérialise toujours, et les sections
sont en COW. Aucune ne suffit seule.

### R3 — Déterminisme numérique 🔒 M2
La worldgen 1.18+ est un graphe de fonctions de densité en `double`. **Une seule
contraction FMA, un ordre d'opérations différent ou un appel RNG de trop, et le
terrain diverge totalement** — de façon indétectable jusqu'à comparaison
visuelle. D'où `-ffp-contract=off` partout, `-ffast-math` et `-Ofast` rejetés à
la configuration, et les RNG vanilla testés contre des vecteurs de référence dès
M2. Même exigence pour la physique d'entités (frictions 0.6 / 0.91, drag 0.98,
`float` ou `double` selon le champ) : l'écart s'y manifeste en désynchronisation
de position.

### R4 — Ampleur : worldgen, redstone et IA
Trois projets de plusieurs mois chacun. La worldgen 1.18+ **est déjà data-driven
en vanilla** : implémenter l'interpréteur, jamais un générateur ad hoc qu'il
faudrait jeter. Redstone et fluides dépendent entièrement des décisions de
représentation prises en M2 (mixed-radix, flags SoA).

### R5 — Vélocité de build sur 8 Go 🔒 M0
Le risque que tout le monde juge cosmétique et qui tue le plus de projets de
cette taille. À M6, si modifier une ligne de `world.h` coûte quatre minutes, le
projet meurt d'attrition.

Contre-mesures, toutes acquises dès M0 : aucun template lourd dans un header
public (EnTT, simdjson, asio, Vulkan restent **privés**, PIMPL aux frontières) ;
headers `fwd.hpp` ; `ccache` ; PCH par bibliothèque ; **LTO désactivé en
développement** (un link full-LTO dépasse 6 Go et fait swapper une machine de
8 Go) ; **cache binaire vcpkg dès le premier jour** ; dépendances ajoutées au
jalon qui les utilise, jamais d'avance ; et un seuil CI sur le temps de rebuild.

### R6 — MoltenVK
`descriptorIndexing` passe par les argument buffers Metal et a un historique
instable ; `drawIndirectCount` est inégalement supporté ; l'outillage de capture
est inexistant sur macOS. Contourné par : dynamic rendering plutôt que
`VkRenderPass`, bindless **préparé mais non implémenté** (les handles de texture
sont des `u32` dès le jour 1, câblés sur un descriptor set fixe),
`drawIndexedIndirect` simple, et **golden images sur Linux + lavapipe** — MoltenVK
n'est pas un oracle de conformance, il tolère ce que Vulkan natif rejette et
inversement.

---

## 8. Ce qui a été écarté

**Toute API de plugin ou de scripting, pendant au moins deux ans.** Une API de
plugin fige les frontières internes avant qu'elles ne soient bonnes. C'est un
pur accélérateur de dette sur un projet dont l'architecture est encore en train
de se découvrir.

**La parité de seed bit-exacte.** Reproduire le terrain de vanilla bloc pour bloc
pour une même seed imposerait de réimplémenter exactement chaque appel RNG et
chaque ordre d'opération de la worldgen. Le worldgen est donc comparé
**statistiquement** — distribution des biomes, densité des minerais par couche,
taux d'apparition des structures sur 1000 seeds. C'est un écart de parité
assumé, et il est documenté dans `PARITY.md`.

**Le rendu identique à vanilla.** Nous ne distribuons aucun asset ; l'apparence
dépend du resource pack fourni par le joueur. C'est le second écart assumé.

**L'authentification Mojang.** Le serveur tourne en **mode hors-ligne**, comme
un `online-mode=false` vanilla. Concrètement : pas de chiffrement AES/RSA de la
session, pas d'appel HTTPS à `sessionserver.mojang.com`, et donc pas de
dépendance OpenSSL.

Un client vanilla se connecte sans difficulté à un serveur en mode hors-ligne —
le critère de sortie de M3 reste donc entièrement atteignable. L'identité d'un
joueur est son **UUID hors-ligne**, un UUID v3 sur `OfflinePlayer:<nom>`,
identique à ce que calcule vanilla. C'est sous cet identifiant que sont classées
ses données, donc il doit correspondre exactement, et il est vérifié contre la
sortie de `UUID.nameUUIDFromBytes` du JDK.

Conséquence à énoncer clairement : **n'importe qui peut se connecter sous
n'importe quel nom.** C'est le comportement attendu d'un serveur hors-ligne, et
c'est adapté à un usage local ou en réseau de confiance ; ça ne l'est pas pour
un serveur public. Si cela change un jour, l'authentification s'ajoute dans la
phase de login sans toucher au reste du protocole.
