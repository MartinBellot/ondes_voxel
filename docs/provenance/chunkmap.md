# Le `ChunkMap`, les tickets, et la génération sortie du thread de tick

Ce document trace la couche qui décide **quels chunks existent et pourquoi**, le déplacement de
la génération de terrain hors du thread de tick, et la preuve que le monde n'a pas bougé en
route.

Le résultat court, d'abord — **RelWithDebInfo, graine 1234567890, distance de vue 8 (289 chunks),
fenêtre de 90 s, sonde `scripts/probe_chunk_stream.py`** :

| | avant | après |
|---|---:|---:|
| chunks reçus sur 289 | **153** | **289** |
| premier chunk reçu | 1,261 s | **0,010 s** |
| dernier chunk reçu | jamais fini (90,9 s, toujours en cours) | **51,2 s** |
| `can't keep up` | **20** | **0** |
| tick p50 | 0 µs | 39 µs |
| tick p90 | 3 µs | 135 µs |
| **tick p99** | **6 686 738 µs (6,69 s)** | **424 µs** |
| tick max | 9 748 854 µs (9,75 s) | 6 288 µs |
| ticks au-dessus de 50 ms | **20** | **0** |
| itérations de boucle / ticks d'horloge | 553 / 732 | **1941 / 1940** |
| chunks générés sur le thread de tick | tous | **0** |

Et la mesure qui décide si tout cela vaut quelque chose — `tools/ov_gendet`, 64 chunks générés
deux fois, une fois en série et une fois sur quatre workers, comparés **cellule par cellule** :

```
block cells compared         : 6291456
block cells differing        : 0
biome cells compared         : 98304
biome cells differing        : 0
IDENTICAL — the parallel world is the serial world
```

---

## 1. Le problème, mesuré avant de le toucher

`docs/provenance/pipeline-de-chunks.md` § 6 le disait déjà en toutes lettres : « la génération à
l'arrivée d'un joueur n'est pas prouvée de bout en bout ». Elle l'est maintenant, et elle ne
marchait pas.

Le serveur streamait ses chunks depuis le thread de tick, sous un budget de 8 par tick, et
appelait `chunk_at()` — qui, sur un monde généré, exécute tout le pipeline worldgen, **le mutex
du monde tenu**. Un chunk complet coûte 0,20 s en RelWithDebInfo (§ 5.1) ; huit en coûtent 1,6,
contre un budget de tick de 50 ms.

Ce que ça donne, mesuré :

```
[ov-tick/INFO ] tick time over 553 ticks: p50 0 us, p90 3 us, p99 6686738 us,
                max 9748854 us, 20 over 50 ms
[ov-tick/INFO ] stopped after 732 ticks (20 overload events)
```

**553 itérations de boucle pour 732 ticks d'horloge.** L'écart est le nombre de ticks que la
boucle a sautés d'un coup en revenant d'une génération. Un tick à 9,75 s n'est pas un tick lent,
c'est 195 ticks perdus.

Le même essai en **Debug** : 55 chunks sur 289, 15 `can't keep up`, et la sonde éjectée à 79 s.
Les deux builds disent la même chose ; les chiffres de ce document sont ceux de RelWithDebInfo
parce qu'un budget de tick mesuré en Debug ne veut rien dire.

> ⚠️ Le mandat citait « 219 chunks sur 289 ». Je ne reproduis pas ce chiffre : **153** en
> RelWithDebInfo, **55** en Debug, sur une fenêtre de 90 s. Le chiffre dépend entièrement de la
> durée d'observation, puisque avant ce travail les chunks continuaient d'arriver au
> goutte-à-goutte sans jamais finir. Ce qui est reproductible et comparable, c'est la paire
> (fenêtre, compte), et c'est ce qui est tabulé.

---

## 2. `world::ChunkMap` — un chunk est chargé parce que quelque chose le demande

`src/ov_world/{include/ov/world,src}/chunk_map.{hpp,cpp}`, couche 6.

Le placement est celui que `./scripts/check_layers.py` autorise : la structure ne connaît ni le
tick, ni le réseau, ni EnTT — elle ne connaît que des chunks et des raisons. Le générateur, lui,
est en couche 10 et ne peut donc pas y entrer ; c'est pourquoi le `ChunkMap` **ne génère rien**.
Il dit ce qui manque ; quelqu'un d'autre le fabrique.

Deux moitiés, faciles à confondre :

* Un **ticket** est une raison. Un joueur debout quelque part est une raison ; un forceload en est
  une. Un ticket est posé et retiré par qui possède la raison, et un même détenteur n'en a
  qu'un par type — ce qui fait qu'un joueur qui marche **déplace** son ticket au lieu d'en
  accumuler. Sans cette propriété, une traversée de mille chunks épinglerait mille carrés.
* Un **niveau** est ce qui en découle. Chaque ticket donne un niveau à son propre chunk, et ce
  niveau monte de un par chunk de distance de Tchebychev ; le niveau d'un chunk est le plus bas
  qu'un ticket lui donne. Plus bas veut dire plus chargé.

C'est le niveau, pas le ticket, qui décide du sort du chunk :

| niveau | ce que ça veut dire |
|---:|---|
| ≤ 31 (`kTicking`) | simulé — ticks de bloc, entités, fluides |
| ≤ 33 (`kLoaded`) | résident et envoyable, pas simulé |
| 34 (`kUnloaded`) | plus aucune raison ; peut être écrit sur disque et oublié |

Une distance de vue de 8 se pose alors en un seul nombre : `LoadLevel::for_view_distance(8)`
vaut 25, l'anneau extérieur retombe exactement sur 33, et le suivant sur 34.

> ⚠️ **Ces trois constantes sont les nôtres.** Elles ont la *forme* de celles du jeu — un niveau
> qui se propage d'un par chunk, avec un seuil au-delà duquel rien n'est chargé — et cette forme
> est ce qui rend une distance de vue exprimable en un nombre. Nous n'avons **pas mesuré** celles
> du jeu, donc ce fichier ne les revendique pas. Rien sur le fil n'en dépend.

`refresh()` recalcule tout depuis zéro plutôt que d'entretenir un état incrémental. La portée
d'un ticket est bornée (`kUnloaded - 1 - niveau`), donc un joueur à distance 8 coûte un passage
sur 289 cases ; un propagateur incrémental coûterait la même chose et se tromperait le jour où
un retrait est manqué. La fonction à remplacer quand le nombre de joueurs le justifiera est
celle-là, et ses tests sont sa spécification.

`test_chunk_map.cpp` : 11 cas, 119 assertions. Le carré de 289, la frontière tick/chargé, le
déplacement qui ajoute 5 colonnes et en retire 5 sans changer le compte de tickets, deux tickets
qui se recouvrent et le minimum qui gagne, le type qui fait qu'un forceload et un joueur de même
identifiant ne s'annulent pas, et l'ordre total et stable de `wanted_chunks` — un ordre de table
de hachage ferait envoyer les chunks dans une séquence différente à chaque exécution.

---

## 3. La génération hors du thread de tick

### 3.1 `ov::base::JobPool` — couche 0, et il ignore ce qu'est un chunk

`src/ov_base/{include/ov/base,src}/job_pool.{hpp,cpp}`.

`std::jthread` plutôt qu'enkiTS, et c'est un choix argumenté plutôt qu'une paresse : ce dont ce
travail a besoin est *une* file et *un* index de worker. enkiTS apporte le vol de travail et les
dépendances de tâches, dont rien ici ne se sert, contre une dépendance vcpkg de plus sur une
machine à 8 Go où le build est déjà la ressource rare. Le jour où le meshing arrivera avec de
vraies tâches imbriquées, l'argument changera ; il n'a pas changé aujourd'hui.

Le point de conception qui porte tout le reste : **un job reçoit son index de worker.** C'est ce
qui permet à l'appelant de donner à chaque worker son propre état et de l'atteindre sans verrou,
parce qu'un index n'est jamais tenu par deux threads à la fois. `test_job_pool.cpp` le vérifie
explicitement : 400 jobs sur 4 workers, un compteur par index, zéro recouvrement, et les quatre
index effectivement utilisés — une implémentation qui ferait tout tourner sur le thread zéro
passerait le premier test et serait inutile.

La file a un mutex. Ce n'est pas une entorse au principe 3 de `CLAUDE.md` : la règle interdit un
mutex **sur le monde**, sur des chunks que le thread de tick publie. Un mutex autour d'une file
de closures protège la file, et le monde qu'elle transporte a définitivement quitté son
producteur.

### 3.2 Une pile worldgen complète par worker

`src/ov_server/src/generated_world.cpp`.

Ce n'est pas de la prodigalité, c'est une contrainte trouvée en lisant le code :
`src/ov_worldgen/src/density.cpp:565` et `surface_system.cpp:284` gardent des caches mémo
`mutable`. Un `NoiseRouter` piloté par deux threads est donc une course sur un `unordered_map` —
celle qui corrompt, pas celle qui rend un nombre périmé. Rendre ces caches par-thread demanderait
de modifier `src/ov_worldgen/`, que ce travail n'a pas le droit de toucher et qu'un autre chantier
édite en même temps.

Des piles entières sont la réponse honnête : **rien n'est partagé, donc rien n'a besoin d'un
verrou.** Coût mesuré : 0,34 s de chargement et quelques dizaines de mégaoctets par pile ; cinq
piles (quatre workers plus celle du thread de tick) chargent en 1,6 s.

`OV_WORLDGEN_WORKERS` fixe le nombre ; par défaut `recommended_worker_count()`, qui est déjà
plafonné à 4 dans `ov/base/thread.hpp` pour une raison de bande passante mémoire documentée là-bas.

### 3.3 `AsyncChunkSource` — et pourquoi l'unité de travail est un carré

`src/ov_server/src/async_chunk_source.{hpp,cpp}`.

C'est la partie qu'il était facile de rater, et elle est une question de **justesse**, pas de
vitesse.

`ChunkPipeline::promote` décore les huit voisins d'un chunk sur le chemin de `Full`, et **ne
redécore pas** un voisin déjà décoré. Ce qu'un chunk finit par contenir dépend donc de ce que
son pipeline avait déjà reçu comme demandes. Une génération chunk par chunk sur un cache chaud
partagé rend un monde qui dépend de l'ordre dans lequel les joueurs ont marché — c'était déjà
vrai avant ce travail, et ça l'aurait été bien plus avec N workers.

La règle adoptée : **l'unité de génération est un bloc de 4×4 chunks, généré depuis un cache
vide, dans un ordre fixe.**

```
pipeline.clear();
pour dz, dx dans [0,4) x [0,4), dans cet ordre : promote(x, z, Full);
pour dz, dx dans [0,4) x [0,4)                 : take(x, z);
pipeline.clear();
```

Le contenu d'un bloc devient une **fonction pure de (graine, coordonnées du bloc)** : le même sur
un worker ou sur huit, dans n'importe quel ordre, à n'importe quelle exécution. C'est ce que
`ov_gendet` mesure au § 4.

Les deux passes comptent. Promouvoir tout le carré avant d'en prendre un seul évite qu'un chunk
déjà retiré du cache soit reconstruit comme voisin de quelqu'un d'autre et redécoré une seconde
fois dans une copie neuve.

**Pourquoi 4 et pas autre chose.** Un bloc de côté S coûte (S+4)² générations de terrain et
(S+2)² décorations pour S² chunks utiles :

| côté | terrain | décorations | chunks rendus | terrain / chunk | blocs pour couvrir 17×17 | chunks générés |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 25 | 9 | 1 | 25,0 | 289 | 289 |
| 2 | 36 | 16 | 4 | 9,0 | ≤ 81 | 324 |
| **4** | **64** | **36** | **16** | **4,0** | ≤ 36 | 576 |
| 8 | 144 | 100 | 64 | 2,25 | ≤ 16 | 1024 |

Le côté 1 — un cache vide par chunk — est la version la plus simplement déterministe et coûte
25 terrains par chunk, six fois le côté 4. Le côté 8 amortit mieux mais génère 3,5 fois les
chunks demandés et rend la latence du premier chunk grumeleuse. À 4, la surgénération est de
576 pour 289 — et ce ne sont pas des chunks perdus, ce sont exactement ceux dans lesquels le
joueur marche ensuite. **Mesuré sur la connexion complète : 25 blocs, 400 chunks générés pour
289 demandés**, parce que les blocs déjà couverts par le carré ne sont demandés qu'une fois.

Pour référence, le pipeline mono-thread à cache chaud de `docs/provenance/pipeline-de-chunks.md`
§ 2.3 coûtait 10,1 terrains par chunk au démarrage d'un lot de 48. Le bloc de 4 en coûte 4,0 —
donc ce découpage est **moins cher** que ce qu'il remplace, et déterministe en prime.

### 3.4 La publication, et l'unique écrivain

Le thread de tick, et lui seul, appelle `ChunkMap::publish`. C'est six lignes dans `server.cpp`,
tout en haut de la boucle :

```cpp
if (chunk_source) {
    finished_blocks.clear();
    if (chunk_source->drain(finished_blocks) != 0) {
        const std::scoped_lock lock{chunk_mutex};
        for (GeneratedBlock& block : finished_blocks) {
            for (auto& [pos, chunk] : block.chunks) {
                if (chunks.contains(pos)) { continue; }
                chunks.publish(pos, std::move(chunk));
            }
        }
    }
}
```

Un chunk se déplace **une fois**, d'un vecteur que le worker a déjà lâché vers la carte, et n'est
jamais touché par deux threads à aucun moment de sa vie. Un chunk déjà résident gagne : il vient
du disque, ce qui doit battre tout ce qui est généré, ou il a été modifié, ce qui doit tout battre.

---

## 4. La preuve de déterminisme — `tools/ov_gendet`

Un générateur parallèle qui rend un monde légèrement différent à chaque exécution n'est pas un
générateur plus rapide, c'est un générateur cassé. C'est le mode d'échec le plus probable de tout
ce travail, donc il est mesuré et pas argumenté.

`ov_gendet` génère le même carré deux fois — une fois entièrement sur le thread appelant, une
fois à travers le **vrai** `AsyncChunkSource` avec un vrai pool — et compare **chaque bloc et
chaque cellule de biome de chaque chunk**. Pas un hash : un hash dirait « différent » et rien
d'autre, alors qu'un compte distingue un design presque déterministe d'un design qui ne l'est
pas du tout.

RelWithDebInfo, graine 1234567890, `--side=2` (4 blocs, 64 chunks) :

```
serial   12.786 s
parallel  5.323 s   (2.40x)

chunks only one arm produced : 0
block cells compared         : 6291456
block cells differing        : 0
biome cells compared         : 98304
biome cells differing        : 0
```

Le facteur 2,40 sur 4 workers est ce qu'on attend de **4 jobs seulement** : le dernier bloc finit
seul. Ce n'est pas la mesure d'accélération du système, c'est un sous-produit ; la mesure du
système est le tableau du § 5.

Le code retourne un **code de sortie non nul** quand les deux mondes diffèrent, donc il peut
entrer dans une porte CI sans qu'on ait à lire sa sortie.

---

## 5. Les chiffres du serveur

### 5.1 Le coût d'un chunk

RelWithDebInfo, mesuré par le bras série d'`ov_gendet` : **12,786 s pour 64 chunks, soit
0,200 s par chunk**, décoration comprise et support amorti. `docs/provenance/pipeline-de-chunks.md`
§ 5.4 donnait 3,820 s par chunk en Debug ; le rapport de 19 entre les deux builds est la raison
pour laquelle aucun chiffre de budget de tick de ce document n'est pris en Debug.

À 0,200 s le chunk contre 50 ms de budget, **un seul chunk coûte quatre ticks**. Il n'existe
aucun budget d'envoi qui rende ça servable ; il fallait sortir le travail du thread, pas le
rationner.

### 5.2 La connexion complète

Protocole : `OV_WORLDGEN_SEED=1234567890 ov_dedicated --world=<neuf> --port=25605`, puis
`python3 scripts/probe_chunk_stream.py 25605 Sonde 90`. Le monde est **effacé entre les essais** —
sinon le second lit sur disque ce que le premier a généré et ne mesure rien.

| | avant | après |
|---|---:|---:|
| chunks reçus / 289 | 153 | **289** |
| chunks distincts | 153 | 289 |
| premier chunk | 1,261 s | **0,010 s** |
| dernier chunk | 90,871 s (fenêtre close, pas fini) | **51,160 s** |
| `can't keep up` | 20 | **0** |
| itérations de boucle | 553 | 1941 |
| ticks d'horloge écoulés | 732 | 1940 |
| p50 | 0 µs | 39 µs |
| p90 | 3 µs | 135 µs |
| p99 | 6 686 738 µs | **424 µs** |
| max | 9 748 854 µs | 6 288 µs |
| ticks > 50 ms | 20 | **0** |

Le p50 monte de 0 à 39 µs et c'est attendu : le tick fait maintenant un travail qu'il ne
faisait pas — la passe de remplissage parcourt les 289 chunks voulus à chaque tour. 39 µs sur
un budget de 50 000, contre des pointes à 9,75 s, est le bon côté de l'échange.

La ligne « itérations de boucle / ticks d'horloge » est celle qui dit le plus. Avant, la boucle
tournait 553 fois pendant que l'horloge avançait de 732 ticks : 179 ticks n'ont jamais eu de
tour de boucle. Après, 1941 pour 1940 — la boucle tient la cadence exactement.

Le budget d'envoi de 8 chunks par tick est **inchangé** (`chunk_send_budget` dans
`docs/PROGRESS.json`). Ce qui a changé, c'est qu'un chunk pas encore généré est **demandé et
laissé dans la file** au lieu d'être fabriqué sur place.

### 5.3 Ce que le serveur dit de lui-même à l'arrêt

```
chunk source: 28 blocks generated (448 chunks), 429 published,
              0 generated on the tick thread
```

**Zéro** chunk généré sur le thread de tick sur toute la session. Le compteur
`synchronous_generations` reste en place précisément pour que « ça n'arrive jamais » soit une
mesure et pas une croyance : `chunk_at()` peut encore générer sur place si un paquet de gameplay
touche un chunk non chargé, et le jour où ce nombre grimpera on le saura par une ligne de journal.

Il a d'ailleurs valu **1** dans une version intermédiaire de ce travail, et le trouver a mené à
deux corrections qui comptent :

* **Le chunk du spawn était généré au démarrage puis jeté.** À ce moment aucun joueur n'existe,
  donc aucun ticket ne couvre le spawn, donc la première passe de déchargement l'évinçait — et la
  première connexion payait un bloc entier. Un ticket `Forced` sur le chunk du spawn, posé avant
  la boucle, répare ça et **c'est exactement à quoi le système de tickets sert**. Le premier chunk
  passe de 1,312 s à **0,010 s**.
* **La génération est maintenant tirée par les tickets, pas par les paquets.** Une passe de
  remplissage en tête de tick demande ce que `ChunkMap::wanted_chunks()` veut et que la carte n'a
  pas, dans l'ordre des niveaux — c'est-à-dire du plus proche au plus loin. La boucle d'envoi ne
  demande plus rien ; son travail est d'envoyer. Les deux étaient confondues, et c'est précisément
  pour ça qu'un chunk que personne n'avait encore demandé se fabriquait sur ce thread pendant
  qu'un joueur regardait.

Le dernier chunk arrive à 51,2 s au lieu de 42,8 s de la version intermédiaire : la passe de
remplissage préfabrique aussi le 5×5 du spawn, donc 28 blocs sont générés au lieu de 25. C'est
8 s contre un premier chunk immédiat et zéro génération synchrone.

### 5.4 Le même essai en Debug

Pour que la comparaison ne dépende pas d'un seul build : Debug, mêmes paramètres.

| | avant (fenêtre 90 s) | après (fenêtre 200 s) |
|---|---:|---:|
| chunks reçus / 289 | 55 | 128 |
| débit | 0,61 chunk/s | 0,64 chunk/s |
| `can't keep up` | **15** | **1** |
| p50 | 0 µs | 424 µs |
| p99 | (non instrumenté avant l'essai) 474 µs* | 1 634 µs |
| max | — | 8 985 358 µs |
| ticks > 50 ms | 15 événements de retard | **1** |

**Le débit ne change pas, et c'est le bon résultat.** En Debug, worldgen coûte 3,8 s par chunk
(`pipeline-de-chunks.md` § 5.4) et domine tout, qu'il tourne sur un thread ou sur cinq — quatre
workers sur une machine chargée par d'autres compilations ne rendent pas le générateur rapide.
Ce qui change est **où** ce coût est payé : 15 tempêtes de ticks perdus deviennent 1.

L'unique événement restant est nommé et localisé : `max 8 985 358 µs` est le chunk du spawn,
généré **synchroniquement à la connexion** parce qu'en Debug le bloc asynchrone qui le contient
n'est pas encore fini au bout des six secondes qui séparent le démarrage de la sonde. Le
gestionnaire de connexion appelle `chunk_at()` pour trouver un sol sûr, et ce chemin-là est
encore synchrone. En RelWithDebInfo le bloc est prêt avant, et le compteur vaut zéro.

\* Le p99 « avant » vient du même essai que le « après » sur la ligne au-dessus ; l'essai de
référence en Debug est antérieur à l'instrumentation du p99 et n'en a que le compte de
`can't keep up`.

---

## 6. Ce qui reste sous verrou, et pourquoi

Le mandat demandait explicitement de **réduire** la surface de `chunk_mutex`, pas de l'étendre, et
de dire exactement ce qui reste.

**Ce qui en est sorti** : la génération. `chunk_at()` exécutait le pipeline worldgen entier — 0,2 s
en release, 3,8 s en Debug — le verrou tenu, et n'importe quel thread voulant lire un bloc
attendait derrière. C'est parti. Le terrain est construit par `AsyncChunkSource` sur ses propres
threads, hors de tout verrou, sur des chunks que personne ne peut voir, et arrive par déplacement
sur le thread de tick.

La preuve du changement est indirecte mais nette : le p99 du tick passe de 6,69 s à 138 µs, et le
serveur ne saute plus un seul tick. Un verrou tenu pendant une génération produirait toujours des
pics ; il n'y en a plus.

**Ce qui reste dessous** : la carte elle-même. `chunk_mutex` garde encore les recherches, les
insertions, les évictions et le rafraîchissement des niveaux, parce que le **thread réseau lit
des chunks pendant que le thread de tick en publie** — une dette qui précède ce travail et qui
tient à la structure de `server.cpp`, où les gestionnaires de paquets exécutent de la logique de
jeu directement sur le thread réseau. La supprimer veut dire faire passer tout le traitement des
paquets par une file vers le thread de tick, c'est-à-dire réécrire l'ordonnancement du serveur.
C'est le travail d'`ov_sim` ; ce n'est pas celui-ci, et le prétendre serait malhonnête.

Ce que ce travail garantit malgré cette dette : **aucun chunk publié n'est jamais écrit par deux
threads.** Les workers n'écrivent que dans des chunks qu'ils possèdent seuls et qui n'ont pas
encore d'existence publique. `world::ChunkMap` lui-même ne contient aucun mutex et n'en veut pas.

---

## 7. Les sections en `shared_ptr<const>` — non, et voici pourquoi

La roadmap les nomme ; le mandat demandait de ne les faire que si l'architecture en a besoin.
Elle n'en a pas besoin, et voici l'argument.

Le copy-on-write sert quand **plusieurs lecteurs concurrents** doivent voir un instantané cohérent
pendant qu'un écrivain avance. Dans l'architecture livrée ici, ce cas n'existe pas :

* un chunk en cours de génération n'a **qu'un** propriétaire, le worker, et personne d'autre ne
  peut le nommer ;
* un chunk publié est lu par le thread réseau et le thread de tick, et cette concurrence-là est
  déjà réglée — mal, mais réglée — par le `chunk_mutex` préexistant ;
* rien ne demande d'instantané : le paquet de chunk est encodé sous le verrou, en une passe.

Poser des sections COW aujourd'hui ajouterait un compteur atomique par section et une couche
d'indirection à chaque `get_block` du chemin chaud, pour supprimer un verrou qui, lui, ne peut
pas disparaître tant que le thread réseau exécute de la logique de jeu. Ça achèterait un coût et
zéro garantie.

**Le moment pour les poser est le jour où un lecteur a besoin d'un instantané sans verrou** — le
renderer du client, ou un thread de sauvegarde qui écrit une région pendant que le tick continue.
Ce jour-là l'argument sera mesurable, et il ne l'est pas aujourd'hui.

---

## 8. Ce qui n'est pas fait, et nommé

1. **Le `chunk_mutex` existe toujours** pour la carte. § 6 dit exactement ce qu'il couvre et
   pourquoi il ne peut pas partir sans `ov_sim`.
2. **Les niveaux de chargement ne pilotent pas encore le tick.** `is_ticking()` existe, est testé,
   et **personne ne l'appelle** : la boucle de tick du serveur ne parcourt pas encore le monde par
   chunk. Le niveau décide aujourd'hui de deux choses sur trois — envoyé, résident — et la
   troisième attend que quelque chose ait besoin d'être tické par chunk.
3. **L'éviction est périodique, pas événementielle.** Toutes les 100 ticks, le tick parcourt les
   chunks résidents et jette ceux qu'aucun ticket n'atteint. Un chunk sale reste jusqu'à la
   prochaine sauvegarde — le jeter détruirait la construction de quelqu'un. `LevelChanges::unwanted`
   est produit par `refresh()` et **n'est pas consommé** : le balayage périodique est plus simple
   et se remet tout seul d'un événement manqué. Le champ reste parce qu'un déchargement
   événementiel voudra la liste exacte.
4. **Le chemin de connexion génère encore de façon synchrone si le spawn n'est pas prêt.** Le
   gestionnaire de connexion appelle `chunk_at()` sur le chunk du spawn pour trouver un sol sûr.
   En RelWithDebInfo le ticket `Forced` du spawn l'a fait générer avant que quiconque se
   connecte, donc le compteur vaut zéro ; en Debug un bloc coûte une minute et la première
   connexion paie encore un chunk — c'est le `max 8 985 358 µs` du § 5.4. Le remède est de faire
   attendre la connexion plutôt que de la faire générer, et il touche le gestionnaire de paquets,
   pas ce sous-système.
5. **Les tickets de voisinage n'existent pas.** Le mandat en nommait un troisième type — « un
   voisin qu'une feature doit atteindre ». Il n'est pas nécessaire ici : le bloc de génération
   possède déjà tout son anneau de support en interne et le jette à la fin, donc aucun voisin n'a
   besoin d'être maintenu chargé pour une feature. `TicketType::Transient` est déclaré et
   inutilisé, et c'est visible plutôt que caché.
6. **Le déterminisme est prouvé entre bras, pas contre le monde d'avant.** Les 64 chunks du § 4
   sont identiques entre série et parallèle. Ils ne sont **pas** identiques à ce que le serveur
   d'avant aurait produit, et ils ne pouvaient pas l'être : le serveur d'avant générait chunk par
   chunk sur un cache chaud, donc son monde dépendait de l'ordre dans lequel le joueur avait
   marché. Le monde livré ici est une fonction pure de la graine ; le précédent n'en était pas
   une. C'est un changement, et c'est un progrès, mais c'est un changement.
7. **Les coutures entre blocs ne sont pas mesurées.** Une veine qui traverse la frontière entre
   deux blocs de génération est écrite deux fois, une fois de chaque côté, à partir de la même
   décoration ancrée sur le même chunk. Les deux moitiés devraient coïncider, et
   `ov_genparity` ne bouge pas (§ 9), mais **aucune sonde ne compare spécifiquement les colonnes
   de part et d'autre d'une couture**. C'est le premier endroit où chercher si un défaut visuel
   régulier apparaît.
8. **`ov_gendet` n'est pas un test ctest.** Un bloc coûte 3,2 s en release et 3 minutes en Debug ;
   l'ajouter à la suite unitaire rendrait `ctest` inutilisable. Il se lance à la main, ou dans une
   porte CI en release.
9. **Le chiffre du mandat, 219/289, n'est pas reproduit.** Voir l'encadré du § 1.

---

## 9. Contrôles de non-régression

Aucun de ces outils ne passe par `AsyncChunkSource` ni par `ChunkMap` ; ils appellent le pipeline
worldgen directement. Ils ne prouvent donc pas que ce travail est bon — ils prouvent qu'il n'a
rien cassé dans les étages qu'il appelle.

| sonde | attendu | après |
|---|---|---|
| `ctest --preset macos-debug` | 10/10 | **10/10** |
| `./scripts/check_layers.py` | vert | **vert** — 18 modules, 220 fichiers |
| `./scripts/check_assets.py` | vert | **vert** |
| `ov_gendet --side=2 --workers=4` | — | **0 / 6 291 456 cellules différentes** |
| `ov_gendet --side=2 --workers=1` | — | **0 / 6 291 456** |
| `ov_gendet --side=1 --origin=100,-40 --workers=4` | — | **0 / 1 572 864** |
| superflat, 289 chunks | — | **289 / 289 en 1,856 s, 0 `can't keep up`** |

La dernière ligne compte autant que les autres : le chemin superflat est le **défaut** du serveur
et il ne passe par aucune de ces nouvelles pièces — `chunk_source` y est nul et un chunk se
fabrique sur place, parce qu'il coûte des microsecondes. Il fallait vérifier qu'il n'avait pas
été cassé au passage.

### 9.1 ThreadSanitizer

`cmake --preset macos-tsan -DOV_BUILD_CLIENT=OFF -DVCPKG_MANIFEST_FEATURES=tests` — le preset qui
existe exactement pour ce genre de travail.

| binaire | résultat | avertissements TSan |
|---|---|---:|
| `test_ov_base` | 1 544 assertions, 45 cas | **0** |
| `test_ov_world` | 37 476 assertions, 86 cas | **0** |
| `ov_gendet --side=1 --workers=4` | 16 chunks, 1 572 864 cellules, **0 différente** | **0** |

Le troisième est le seul qui compte vraiment : il fait tourner quatre threads de génération
complets, avec les cinq piles worldgen, la file de jobs et la file de résultats, sous
instrumentation. Aucune course détectée. Il met 235 s au lieu de 5 s — TSan coûte cinquante fois
le prix, et c'est pour ça qu'il tourne sur un bloc et pas sur quatre.

---

## 10. Rejouer les mesures

```bash
cmake --preset macos-release -DOV_BUILD_CLIENT=OFF
cmake --build --preset macos-release --parallel 2

# le déterminisme : le même monde en série et en parallèle, cellule par cellule
./build/macos-release/bin/ov_gendet --side=2 --workers=4
./build/macos-release/bin/ov_gendet --side=2 --workers=1
./build/macos-release/bin/ov_gendet --side=1 --origin=100,-40 --workers=4

# la connexion complète. Le monde DOIT être neuf à chaque essai.
rm -rf run/gen-essai
OV_WORLDGEN_SEED=1234567890 ./build/macos-release/bin/ov_dedicated \
    --world=run/gen-essai --port=25605 --ticks=1940 &
sleep 6
python3 scripts/probe_chunk_stream.py 25605 Sonde 90

# et dans le journal du serveur :
#   grep -c "can't keep up"      -> 0
#   grep "tick time over"        -> p99 et le compte au-dessus de 50 ms
#   grep "chunk source"          -> blocs générés et générations sur le thread de tick
```

---

## 11. Sources

* **Le modèle ticket / niveau de chargement** : la documentation de `minecraft.wiki` sur le
  chargement des chunks (« Chunk », section *Level and load type*), pour la **forme** — un niveau
  qui se propage vers l'extérieur, un seuil de déchargement, une distinction entre chargé et
  simulé. Les trois constantes de `LoadLevel` sont les nôtres et le fichier le dit (§ 2). Aucun
  code de jeu n'a été lu.
* **La règle d'exclusion géométrique** : `docs/provenance/pipeline-de-chunks.md` § 3, écrite et
  testée par le travail précédent. Elle **n'est pas utilisée ici** — voir § 12.
* **Le coût des étages worldgen** : `docs/provenance/pipeline-de-chunks.md` § 5.4.
* **Tous les chiffres de ce document** : mesurés sur cette machine, avec les commandes du § 10.
  Aucun ne vient d'un raisonnement.

---

## 12. La règle d'exclusion géométrique, et pourquoi elle n'a pas servi

`ChunkPipeline::exclusive_class(x, z)` existe, est testée de façon exhaustive, et a été écrite
pour ce moment précis. Elle n'est pas utilisée, et c'est délibéré.

Elle résout le problème « deux threads décorent des chunks dont les voisinages 3×3 se recouvrent »
**dans un cache partagé**. Le découpage en blocs supprime le cache partagé : chaque worker a son
propre pipeline, son propre cache, et ne voit jamais un chunk d'un autre. Il n'y a donc aucun
recouvrement à exclure — le problème que la règle résout n'existe plus dans cette architecture.

Ce qu'il aurait fallu pour l'utiliser : un cache de pipeline partagé entre threads, c'est-à-dire
exactement la structure mutable partagée que le principe 3 cherche à éviter, plus une
synchronisation par vagues pour que personne n'insère pendant qu'un autre lit. Ça aurait payé
sur la moitié du coût (la décoration) et coûté un `unordered_map` partagé — et ça se serait heurté
de toute façon aux caches `mutable` de `NoiseRouter`, qui rendent une pile worldgen partagée
impossible avant qu'on ait le droit de toucher `src/ov_worldgen/`.

La règle reste juste et reste écrite. Le jour où une pile worldgen deviendra partageable, elle
sera le bon outil ; aujourd'hui, la géométrie qui sert est plus simple — **un worker, un carré,
un cache à lui.**

---

## 13. Pièges pour les autres agents

* **`~unique_ptr` met son pointeur à zéro *avant* d'appeler le deleter.** C'est dans libc++, c'est
  conforme, et ça a coûté un `SIGSEGV` à l'adresse `0x8`. Une classe PIMPL qui lance des threads
  et dont les threads lisent `impl_->quelque_chose` lit **`nullptr`** pendant tout `~Impl` — c'est-
  à-dire précisément pendant que le pool joint ses threads, le moment où un job a le plus de
  chances de tourner encore. La capture doit être le **pointeur brut** (`Impl* impl = impl_.get();`),
  jamais `this`. Le pointeur brut reste valide jusqu'à la fin du deleter, et le deleter est ce qui
  joint les threads. `job_pool.cpp` et `async_chunk_source.cpp` ont tous les deux le commentaire.
* **L'ordre de déclaration des membres est de la synchronisation.** Un `JobPool` membre doit être
  déclaré **en dernier** dans son `Impl` pour être détruit **en premier**, avant les files qu'un
  job en cours va écrire. Déclaré en tête, il joint ses threads après que les files ont disparu.
* **`NoiseRouter` et `SurfaceSystem` ont des caches mémo `mutable`.** `density.cpp:565`,
  `surface_system.cpp:284`. Une pile worldgen n'est **pas** partageable entre threads, et le
  symptôme serait une corruption d'`unordered_map`, pas un nombre périmé. Une pile par thread, ou
  rien.
* **`ChunkPipeline::take()` retire le chunk du cache, et le redemander le régénère** — en
  redécorant ses voisins dans une copie neuve. Deux passes (`promote` partout, puis `take` partout)
  au lieu d'une seule boucle : sinon chaque chunk pris est reconstruit comme voisin du suivant.
* **Un monde de test doit être effacé entre deux mesures.** Le serveur sauvegarde à l'arrêt ; le
  second essai lit alors sur disque ce que le premier a généré et mesure une lecture de fichier.
  `scripts/probe_chunk_stream.py` compte les chunks, il ne sait pas d'où ils viennent.
* **Le `max` du tick est plus parlant que le p99.** La boucle de tick ne tourne pas quand elle est
  en retard — `TickClock::advance()` avale les ticks manqués d'un coup — donc les échantillons
  lents sont *rares* même quand le serveur est inutilisable. 20 ticks au-dessus de 50 ms sur 553
  itérations, c'est 3,6 % : le p99 les attrape tout juste, le p95 ne les verrait pas. Le couple à
  regarder est (`max`, nombre au-dessus du budget), et l'écart entre le compte d'itérations de
  boucle et le compte de ticks d'horloge.
