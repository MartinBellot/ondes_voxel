# Performance du tick — pourquoi le serveur rame, et pourquoi casser un bloc prend 2 s

Le rapport de départ, d'un joueur sur un monde généré (`scripts/lab.sh --seed=… --client`),
serveur et client dans le même terminal, pendant que huit agents compilaient :

```
[server] tick time over 303 ticks: p50 62 us, p90 609871 us, p99 3220246 us, max 5398346 us, 37 over 50 ms
[server] stopped after 620 ticks (35 overload events)
client: cpu p50 16.48 ms  p99 51.64 ms  max 733.70 ms
```

« Quand je casse un bloc il y a 2 s de délai et tout est super lent. » Deux causes
superposées étaient candidates : **ce que le serveur fait lui-même**, et **la machine** —
8 Go de RAM, neuf agents, des compilateurs et des JVM. Ce fichier les sépare, chiffres à
l'appui, et dit ce qui a été corrigé.

> **En bref.** Deux causes propres au serveur, mesurées et corrigées : l'éclairage recalculé
> en 3×3 **sur le thread réseau avant chaque `Block Update`** (§ 3, § 5.1–5.3), puis la
> **génération de chunks sur le thread de tick** par la phase des entités (§ 5.4). Sur un
> monde généré, en Release, le cassage passe de **5,2 s à 62 ms au p99** ; à charge égale sur
> le banc Debug, de **205 à 6 ms au p50**, et le serveur tourne **2,5 fois plus vite**. La
> contention de la machine est l'autre moitié, chiffrée au § 4 : elle double les coûts, et
> elle décide seule du temps d'entrée sur un monde neuf (ouvriers en QoS `UTILITY` à ~2 %
> d'un cœur). **Non corrigé et nommé** : une troisième cause (génération synchrone sur le
> thread réseau, § 5.4, § 7.7), l'envoi des chunks sous le verrou joueurs (§ 7.6), la QoS des
> ouvriers (§ 7.2), le maillage côté client (§ 6).

---

## 1. Les instruments

### 1.1 Le profil par phase (`src/ov_server/src/tick_profile.{hpp,cpp}`)

Le serveur imprimait un histogramme pour le tick entier et un pour le spawner. Un tick lent
n'y disait pas *pourquoi*. Chaque phase de la boucle a maintenant sa ligne, imprimée à
l'arrêt, **la plus lourde en premier** :

```
tick profile: 37 loop iterations, 339 clock ticks, 41.0 s: 0.90 iterations/s (…)
tick profile: whole tick p50 … us, p90 …, p99 …, max …, mean …, N over 50 ms, tick thread on cpu X% of wall
tick phase relight: runs, p50, p90, p99, max, mean, total ms, cpu %, over 50 ms, heaviest in K of S slow ticks
network packet / player lock wait / block edit: …
process: … involuntary and … voluntary context switches, … major and … minor page faults since start
```

Trois choix, chacun pour une raison :

- **Deux horloges par phase** : le temps mur (ce que le joueur attend) et le temps CPU du
  thread de tick (`CLOCK_THREAD_CPUTIME_ID`, ce que la phase *coûte*). Une phase à 100 %
  de CPU est lente parce qu'elle travaille ; une phase à 20 % attendait — un verrou tenu
  par le thread réseau, un cœur donné à un compilateur, une page revenant du swap.
  Optimiser son code n'y changerait rien. **Ce rapport est tout l'intérêt du fichier.**
- **« heaviest in K of S slow ticks »** : pour chaque tick au-dessus de 50 ms, la phase la
  plus coûteuse est comptée. C'est la réponse directe à « qu'est-ce qui a rendu ce tick
  lent ».
- **Des histogrammes à seaux fixes**, pas des vecteurs d'échantillons : 64 seaux exacts
  puis 32 par puissance de deux, donc un percentile relu est au plus **1/32 (3,1 %)**
  sous l'échantillon vrai ; `max`, `total`, `count` et le nombre au-dessus de 50 ms sont
  exacts. Aucune allocation après construction (vérifié sous `NoAllocScope` par
  `test_tick_profile.cpp`), et un serveur peut tourner un mois sans que le profil
  grossisse — les anciens vecteurs `tick_micros`, réservés à 64 K échantillons,
  réallouaient au-delà.

Côté réseau, trois histogrammes remplis depuis n'importe quel thread : le traitement d'un
paquet *Play* entier, **l'attente du verrou joueurs** avant de le traiter (le temps pendant
lequel le tick le tenait), et `set_block_and_broadcast`.

Enfin la ligne `process:` relève `getrusage` : commutations de contexte involontaires
(le noyau a repris le CPU) et **défauts de page majeurs** (lectures depuis le swap).

**Piège 22 du briefing, confirmé et désormais affiché** : la boucle exécute le corps **une
fois par tour**, quel que soit le nombre de ticks que `TickClock::advance()` rend. Le
monde avance donc au rythme des *tours de boucle*, pas des ticks d'horloge. Le rapport de
départ (303 tours pour 620 ticks) décrit un monde qui a avancé 303 fois, pas 620. Le
profil imprime les deux et le rapport des deux.

### 1.2 Le banc (`scripts/bench_play.py`)

Notre serveur (Debug ou Release), un monde généré à graine fixe ou le banc `ov_lab`, et
une sonde protocole 763 (`vanilla_miner.Miner`) qui :

- lit les heightmaps `MOTION_BLOCKING` des paquets `Chunk Data` pour savoir où est le sol ;
- marche en ligne droite (`--walk`, blocs/s) en annonçant sa position vingt fois par seconde ;
- à `--edit-rate` gestes par seconde, casse le bloc du dessus d'une colonne à deux blocs
  devant elle, puis en pose un (mode créatif : le cassage est instantané, tout le délai
  est celui du serveur) ;
- chronomètre **`Player Action` → `Block Update`** de la même position, et à part
  **l'accusé (`Acknowledge Block Change`)**, que le thread réseau envoie *avant*
  d'écrire : l'écart entre les deux est le coût de l'écriture elle-même.

Pendant la mesure, toutes les deux secondes : charge (`vm.loadavg`), swap
(`vm.swapusage`), `vm_stat` (pages sorties, swap-ins, swap-outs) et `ps` du serveur (CPU,
mémoire résidente). Options de séparation : `--workers` (`OV_WORLDGEN_WORKERS`), `--nice`,
`--burn=N` (N processus `yes` à côté, pour une série de contention *contrôlée*).

Ce que le banc a dû apprendre en chemin, chaque point payé par une mesure fausse :

- **`--binary=`** : un serveur mis de côté. Les binaires « avant » (Debug et Release) ont
  été copiés sous `.scratch/` avant la reconstruction avec le correctif, et chaque série
  désigne explicitement celui qu'elle mesure — sinon une reconstruction intermédiaire
  aurait fait mesurer deux fois le même code.
- **Fenêtre de 30 s** avant de compter un geste perdu : 10 s au départ, et le serveur Debug
  non corrigé répondait à la plupart des gestes à 4/s *plus tard* que ça. Une latence de
  vingt secondes est un résultat, pas une perte.
- **L'accusé est le paquet 0x06** (`kAcknowledgeDig` dans `play.hpp`), pas 0x05 : la
  première version n'en voyait aucun alors que les `Block Update`, envoyés *après*,
  arrivaient.
- **Un échec d'entrée est un résultat** : la sonde qui ne peut pas entrer (spawn jamais
  prêt) enregistre le délai et l'erreur, et le rapport d'arrêt du serveur est quand même
  lu.
- **`--burn` n'a pas été utilisé.** Faire tourner des processus `yes` à côté du serveur
  ralentit aussi les huit autres agents de la machine pour démontrer un point. La
  séparation passe par `--workers=1` et `--nice=10`, qui ne coûtent rien à personne.

Les mondes vivent sous `.scratch/bench-play/` et sont supprimés après lecture. Aucun
chemin sous `run/` (c'est un lien vers le dépôt principal).

---

## 2. Ce que le binaire d'origine fait, avant tout changement

Binaire `macos-debug` de `bc6aa3a`, **sans** instrumentation, banc `ov_lab`, sonde à
4 gestes/s et 1 bloc/s, 45 s. Machine : charge 19–24, swap utilisé 6,2 → 6,5 Go.

| série | tours de boucle | ticks d'horloge | tick p50 | tick p99 | > 50 ms | accusés reçus | chunks reçus |
|---|---|---|---|---|---|---|---|
| gestes à 4/s | **37** en ~45 s | 339 | 866 ms | 3,6 s | 37 / 37 | **0** | 8 |
| sonde inactive (témoin) | **28** en ~40 s | 254 | **1 414 ms** | 3,0 s | 28 / 28 | — | — |

Trois lectures, dans l'ordre où elles contraignent l'interprétation :

1. **Le symptôme se reproduit, sans monde généré.** Sur le banc plat de 441 chunks, le
   monde avance moins d'une fois par seconde.
2. **Les gestes ne sont pas la cause *dans ces conditions*.** L'accusé de réception est la
   première chose que le thread réseau envoie en traitant un `Player Action` : **zéro**
   accusé en 45 s veut dire qu'aucun cassage n'a même été traité. Et le témoin sans aucun
   geste est *plus* lent (p50 1,4 s). La vague précédente a mesuré exactement ce témoin —
   même banc, sonde inactive, Debug — à **2,5 ms** au p50 (`branchement.md` § 4) : un
   facteur **~500**, sur un serveur qui ne fait rien.
3. **Le serveur n'était pas en train de calculer.** CPU du processus 24 à 42 %, mémoire
   résidente **11 à 16 Mo** pour un serveur Debug, et **5 à 6 Go de swap-ins** sur la
   machine pendant chacune des deux mesures. Un processus dont les pages sont en swap
   passe son tick en défauts de page ; c'est ce que ces trois chiffres décrivent.

**Correction, après mesure** : le point 3 était une *déduction* à partir de mesures
machine, et le profil (§ 3) l'a **démentie** pour la série avec gestes : le serveur y
calculait — de l'éclairage — et son thread de tick attendait des verrous, pas des pages
(69 défauts majeurs en 42 s). Le point 2 tient pour ces deux exécutions, mais pour une
autre raison que celle écrite : **la sonde elle-même était cassée** (§ 3.3) et ne recevait
plus rien après quelques paquets, si bien que « zéro accusé » mesurait la sonde et non le
serveur. La déduction est laissée ici, barrée par ce paragraphe, parce qu'elle montre
exactement pourquoi le dépôt ne se contente pas de déductions.

### 2.1 Une déconnexion silencieuse, trouvée en chemin

La première exécution du banc a perdu la connexion sans aucune ligne dans le journal du
serveur. Le mécanisme, lu dans le code :

- le serveur envoie un keep-alive toutes les 10 s et **ferme la connexion si la réponse
  porte un autre identifiant que le dernier envoyé** (`kKeepAlive` rend `false`) ;
- le listener ferme alors la socket sans journaliser ; `on_disconnect` ne l'écrit qu'au
  niveau DEBUG.

Un thread réseau en retard de plus de 10 s traite la réponse au keep-alive *n* après que
le tick a envoyé le *n+1* : le joueur est éjecté, en silence. Sur une machine saturée,
« tout est lent » finit donc en déconnexion que personne ne peut expliquer. **Non corrigé**
(vanilla aussi déconnecte sur un keep-alive qui ne correspond pas) ; nommé ici parce que
c'est la fin logique du symptôme rapporté.

---

## 3. Le coût propre du serveur

### 3.1 Le profil par phase, binaire instrumenté, avant correction

`macos-debug`, instrumentation seule (aucun changement de comportement), banc `ov_lab`,
sonde à 4 gestes/s, 1 bloc/s, 90 s demandées (42 s de tick relevées avant la
déconnexion). Machine : charge **44–49**, swap 6,9 → 6,6 Go.

`1,37 tour de boucle par seconde`, tick p50 **508 ms**, p99 2,29 s, 58 tours sur 58
au-dessus de 50 ms, **thread de tick sur le CPU 16 % du temps mur**.

| phase | total | p99 | **CPU** | la plus lourde dans |
|---|---|---|---|---|
| containers (entonnoirs, droppers) | 16,8 s | 934 ms | **0 %** | 31 des 58 ticks lents |
| scheduled ticks (fluides, redstone) | 10,5 s | 606 ms | **0 %** | 18 / 58 |
| chunk send | 9,7 s | 1,54 s | 47 % | 8 / 58 |
| relight (sur le tick) | 1,5 s | 242 ms | 79 % | 0 / 58 |
| autosave | 1,2 s | — | 37 % | 1 / 58 |
| natural spawning | 0,2 s | 3,8 ms | 49 % | 0 / 58 |
| tout le reste | < 0,1 s | | | |

Côté réseau, sur le même intervalle :

| | échantillons | p50 | p99 | max | > 50 ms |
|---|---|---|---|---|---|
| `block edit` (`set_block_and_broadcast`) | 153 | **168 ms** | 475 ms | 493 ms | **153 / 153** |
| paquet *Play* entier | 944 | 11 µs | 492 ms | 2,02 s | 164 |
| attente du verrou joueurs | 944 | 0 µs | 1,8 ms | 1,54 s | 10 |

Lecture :

- **Les deux phases les plus lourdes du tick ne font rien.** `containers` et `scheduled
  ticks` sont à 0 % de CPU : elles prennent `chunk_mutex` (et `players_mutex` pour la
  première) par un verrou **bloquant**, et attendent.
- **Qui tient le verrou** : le thread réseau, dans `set_block_and_broadcast`, **168 ms au
  p50 par bloc écrit**, les 153 au-dessus du budget d'un tick. Soit ~30 s de rallumage,
  verrous tenus, dans une fenêtre de 42 s. Un cassage en créatif écrit le bloc *et* ses
  voisins remodelés (`set_block_connected`), chacun avec son voisinage 3×3.
- **L'échantillonneur confirme sans le profil** : `sample` sur 15 s du même processus —
  le seul calcul visible est le moteur de lumière (`PalettedContainer::get`,
  `Chunk::get_block`, `relight_blocks`, `light_emission`, `section_for_y`,
  `LightArray::set`, et la recherche linéaire `Loaded` de l'ancien
  `relight_neighbourhood`), à côté de **11 535 échantillons dans
  `__psynch_mutexwait`**.

C'est le mécanisme du « 2 s de délai » : les gestes s'enfilent sur le thread réseau, chaque
`Block Update` attend les rallumages de tous les gestes précédents, et le tick, bloqué
derrière les mêmes verrous, cesse d'avancer — donc « tout est lent ». Et comme l'unique
thread réseau fait aussi *toutes* les écritures de socket (`Connection::send` poste sur
son exécuteur asio), le monde entier attend avec lui.

### 3.2 La série de référence « avant », sonde réparée

Même binaire, même banc, **1 geste/s** (un rythme humain), 60 s, fenêtre de 30 s avant de
compter un geste perdu. Machine : charge 41–44, 4 Go de swap-ins pendant la mesure.

| | n | p50 | p90 | p99 | max | perdus |
|---|---|---|---|---|---|---|
| cassage → `Block Update` | 30 | **416 ms** | 1 389 ms | 3 372 ms | **5 365 ms** | 0 |
| pose → `Block Update` | 30 | 477 ms | 1 293 ms | 2 143 ms | 4 401 ms | 0 |
| accusé (envoyé *avant* l'écriture) | 60 | 423 ms | 1 389 ms | — | 5 364 ms | — |

Serveur : **2,10 tours/s**, tick p50 225 ms, 92 tours au-dessus de 50 ms. Phases :

| phase | total | CPU | la plus lourde dans |
|---|---|---|---|
| chunk send | 29,4 s | 53 % | 40 des 92 ticks lents |
| relight (sur le tick) | 19,6 s | 53 % | 42 / 92 |
| scheduled ticks | 6,4 s | 1 % | 8 / 92 |
| autosave | 2,8 s | 43 % | 2 / 92 |
| containers | 2,0 s | 1 % | 0 / 92 |

Réseau : `block edit` p50 125 ms (max 1,6 s) ; **attente du verrou joueurs p99 754 ms,
max 1,65 s**.

Ce que cette série ajoute à la précédente :

- **L'accusé arrive aussi tard que le `Block Update`.** À ce rythme, le délai d'un
  cassage se passe presque entièrement *avant* que le serveur traite le paquet : dans la
  file du thread réseau, derrière des attentes de verrou. Le coût de l'écriture elle-même
  (l'écart accusé → `Block Update`) est ici faible devant l'attente.
- **Trois causes propres au serveur**, toutes sous verrou :
  1. l'**envoi des chunks** (8 par tick) encode chaque `Chunk Data` en tenant
     `players_mutex` *et* `chunk_mutex` — le thread réseau attend ce verrou pour chaque
     paquet. Pas de compression : `encode_packet` est appelé sans seuil
     (`kNoCompression`) et `Connection::send` ne fait que copier la trame ; le
     `deflate_compress_lazy` vu par `sample` est celui de la sauvegarde des régions ;
  2. le **rallumage sur le tick**, alimenté par le drain (l'eau et le sable du banc que la
     sonde dérange) : un voisinage 3×3 par chunk touché et par tick ;
  3. le **rallumage par bloc écrit** sur le thread réseau (125 ms au p50).
- **La contention double tout** : les phases qui calculent n'ont eu que 45 à 53 % d'un
  cœur, 157 000 commutations involontaires en une minute. Elle multiplie les coûts ; elle
  ne les crée pas.

**Reprise sans compilation.** Cette série tournait pendant une construction Release de cet
agent (voir la note de méthode, § 7). Reprise avec le même binaire mis de côté, les mêmes
paramètres que la série « après », et **aucune** compilation de cet agent en parallèle :

| | cassage p50 | p90 | p99 | max | tours/s |
|---|---|---|---|---|---|
| série ci-dessus (compilation en parallèle) | 416 ms | 1 389 ms | 3 372 ms | 5 365 ms | 2,10 |
| **reprise** | **199 ms** | 676 ms | 1 963 ms | 3 951 ms | **5,42** |

Une unité de charge de plus sur la machine a doublé le p50 : c'est la sensibilité à la
contention, mesurée sur le même code. Le classement des phases, lui, ne change pas —
rallumage sur le tick 27,1 s, le plus lourd dans **148 des 204** ticks lents, à 80 % de
CPU (du travail, pas de l'attente) ; envoi des chunks 18,0 s ; `block edit` p50 82 ms.
C'est la reprise qui sert de référence « avant » au § 5.3.

### 3.3 Pourquoi ce rallumage coûte si cher

Par bloc écrit, l'ancien code faisait :

1. le ciel sur les 9 chunks : remise à zéro de toute la bande occupée, puis **chaque
   case directement éclairée poussée comme graine** du remplissage — des centaines de
   milliers de cases, alors qu'une case à 15 entourée de cases à 15 ne propage rien ;
2. chaque lecture et écriture de lumière retrouvait son chunk par une **recherche
   linéaire** dans une liste de neuf, à travers un lambda ;
3. la lumière des blocs, **relue case par case dans les 9 chunks** (9 × 98 304 lectures de
   `PalettedContainer` + `light_emission`), même pour une section sans une seule source.

### 3.3 La sonde, cassée deux fois, et ce que ça a coûté

`Miner.read` (de `vanilla_miner.py`) consomme la longueur de trame **octet par octet**
depuis la socket. Avec les délais de 1 ms dont la boucle de la sonde a besoin, un délai
qui tombe entre deux octets de la longueur jette les premiers : le flux est décalé pour
toujours, chaque « paquet » suivant est du bruit. Les deux premières exécutions ont reçu
8 puis 40 chunks et **aucun** `Block Update` exploitable. `bench_play.Player.read` lit
maintenant la trame dans le tampon et ne coupe le tampon qu'une fois la trame complète.
Les autres sondes du dépôt n'appellent `Miner.read` qu'avec des délais longs et ne sont
pas touchées — mais **le piège vaut pour toute sonde à délai court**.

## 4. La part de la contention

### 4.1 L'état de la machine pendant ces mesures

Relevé à 01:45 le 2026-09-11, pendant les mesures du § 2, avec `vm_stat` et `sysctl` :

| | |
|---|---|
| pages **stockées** dans le compresseur | **19 088 Mo** |
| RAM occupée par le compresseur | 3 176 Mo |
| RAM câblée (*wired*) | 2 298 Mo |
| RAM libre | **56 Mo** |
| swap utilisé | **7 280 Mo sur 8 192** |
| charge (1 min) | 19 à 34 selon le relevé, sur 8 cœurs |

Environ **27 Go de mémoire vivante tenus dans 8 Go**. Recensement des processus par taille
résidente au même moment : l'ensemble de tous les processus n'est résident qu'à hauteur
d'environ 2 Go ; un binaire de test Debug du projet l'est à **2,4 Mo**, notre serveur Debug
à **11–16 Mo**. Tout le reste est dans le compresseur ou le swap, et chaque accès à une
page qui y dort coûte une décompression ou une lecture disque.

Deux effets directement lisibles dans nos chiffres :

- `test_ov_worldgen`, qui ne fait que calculer, a tourné à **26 % de CPU** (3 min 35 s de
  CPU pour 9 min 26 s de temps mur) : même un calcul pur passe les trois quarts de son
  temps à attendre ses pages ;
- le serveur témoin du § 2, sans rien à faire, à **24 % de CPU** et 1,4 s par tick.

### 4.2 Un piège pour tous les agents : le swap mange le disque

Sur macOS, le swap est une suite de fichiers sur le **volume de données** — le même que
celui des `build/`. Pendant ces dix minutes l'espace libre est passé de **4,9 à 3,4 Go**
sans qu'aucune compilation de cet agent ne l'explique (son `build/macos-release` pesait
alors 167 Mo). Le plancher de 1,5 Go du briefing peut donc être franchi par la seule
croissance du swap : **lancer une JVM ou un build de plus sur une machine qui swappe coûte
aussi du disque**.

### 4.3 Monde généré en Debug : la génération affamée par l'ordonnanceur

Serveur Debug, graine 12345, monde neuf, quatre ouvriers de génération (le plafond de
`recommended_worker_count`). Après ~3 min : **« Preparing spawn area: 0% »**, aucun des
neuf chunks du spawn terminé, la sonde ne peut pas entrer. `ps -M` sur le processus :

| thread | priorité | état | CPU utilisateur en ~3 min | %CPU instantané |
|---|---|---|---|---|
| principal (le tick : `run()` tourne sur le thread principal) | 31 | S | 3,89 s | 0,2 |
| ouvrier 1 | **20** | **R** | 4,22 s | 1,8 |
| ouvrier 2 | 20 | R | 4,26 s | 2,6 |
| ouvrier 3 | 20 | R | 4,15 s | 1,0 |
| ouvrier 4 | 20 | R | 4,34 s | 1,3 |

Les quatre ouvriers sont **exécutables (R) et attendent un cœur**. Priorité 20 : c'est
`QOS_CLASS_UTILITY`, ce que `ov_base/thread.cpp` donne au rôle `Worker`, sous le 31 par
défaut des compilateurs des autres agents.

Un taux, pas un instantané : deux relevés `ps -M` à **85 s** d'intervalle (âge du
processus 2:38 puis 4:03). Chaque ouvrier y a gagné ~1,5 s de CPU (4,22 → 5,74 s, 4,26 →
5,76 s, 4,15 → 5,71 s, 4,34 → 5,79 s) : **1,8 % d'un cœur chacun**. Le processus entier
est passé de 25,1 à 39,6 s de CPU : **17 % d'un seul cœur** pour quatre ouvriers et le
tick, là où quatre ouvriers libres en brûleraient 400 %. Le spawn est resté à 0 % pendant
tout ce temps.

**Même mesure sur le binaire Release** (non corrigé, même graine, même machine), deux
relevés à **75 s** d'intervalle (âge 1:46 puis 3:01) :

| ouvrier | relevé A | relevé B | gagné | part d'un cœur |
|---|---|---|---|---|
| 1 | 2,74 s | 4,29 s | 1,55 s | 2,1 % |
| 2 | 2,94 s | 4,58 s | 1,64 s | 2,2 % |
| 3 | 2,94 s | 4,57 s | 1,63 s | 2,2 % |
| 4 | 2,72 s | 4,23 s | 1,51 s | 2,0 % |
| processus entier | 15,93 s | 25,22 s | 9,29 s | **12,4 % d'un cœur** |

Les quatre toujours en priorité 20, état R ; spawn toujours à 0 %. **Le type de build ne
change pas la part de CPU que reçoivent les ouvriers** : 1,8 % en Debug, ~2 % en Release.
Un code dix fois plus rapide n'avance pas quand il ne tourne pas.

Le rapport d'arrêt de ce serveur, après **309 s** :

```
chunk source: 0 blocks generated (0 chunks), 0 published, 0 generated on the tick thread
tick profile: 6083 loop iterations, 6175 clock ticks, 309.2 s: 19.67 iterations/s
tick profile: whole tick p50 144 us, p99 6272 us, max 334513 us, 13 over 50 ms, tick thread on cpu 21% of wall
process: 331996 involuntary and 575 voluntary context switches, 1482 major and 34681 minor page faults
```

**Zéro chunk généré en cinq minutes.** La sonde a frappé quatre fois (une tentative toutes
les ~31 s), a été renvoyée à chaque fois avec `menu.preparingSpawn`, et a abandonné au bout
de 300 s. Le tick, lui, allait bien — 19,67 tours/s, rien à faire sans joueur — mais même
ce thread à priorité 31 n'a été sur le CPU que 21 % du temps de ses propres ticks, et le
processus a subi **332 000 commutations involontaires** en cinq minutes.

L'arrêt lui-même a traîné : le journal écrit son rapport final, puis le processus reste en
vie environ deux minutes, jusqu'à ce que le banc le tue (délai de 120 s après SIGINT).
L'explication cohérente avec l'ordre des événements — non vérifiée par un échantillonnage —
est que la destruction du pool attend que chaque ouvrier finisse le bloc de chunks
commencé, à 1,8 % d'un cœur.

**La même mesure, reprise plus tard sur une machine un peu moins chargée** — même binaire
Debug mis de côté, même graine, mêmes quatre ouvriers :

| | charge | chunks générés | spawn prêt | CPU du serveur | commutations involontaires |
|---|---|---|---|---|---|
| première mesure | ~40 | **0 en 309 s** | jamais | ~17 % d'un cœur | 332 000 |
| reprise | ~30 (20–37) | **64 en ~290 s** | **290,4 s** | ~101 % | 2 394 000 |

Dix points de charge en moins, et la génération passe de *rien du tout* à un spawn complet
en cinq minutes. Le code est identique ; seule la part de CPU laissée par les autres
processus a changé. En Release, spawn prêt à 292,6 s à une charge ~38 (§ 5.3) : le type de
build pèse moins que la machine.

Conséquence directe pour le joueur : **sur une machine occupée, la QoS `UTILITY` des
ouvriers décide combien de temps il attend avant de pouvoir entrer**, et combien de temps
le terrain met à apparaître quand il marche. Ce n'est pas le code de génération, et ce
n'est pas corrigeable dans le serveur sans changer ce qu'il demande à l'ordonnanceur (§ 7).

### 4.4 La séparation, chiffrée

**Un seul ouvrier au lieu de quatre** (`--workers=1`), binaire Release corrigé (correctif 1),
graine 12345, 1 geste/s :

| | 4 ouvriers (ligne « après » du § 5.3) | **1 ouvrier** |
|---|---|---|
| spawn prêt | 36,1 s | **99,0 s** |
| cassage p50 / p90 / max | 52,6 / 3 683 / 5 683 ms | **740 / 4 105 / 7 910 ms** |
| générations synchrones (compteur d'arrêt) | 31 | **40** |
| phase `entities` : total / pire tick | 33,9 s / 7,33 s | **41,4 s / 9,11 s** |

Moins d'ouvriers, **c'est pire**, et par un mécanisme que le compteur mesure : avec un seul
ouvrier, moins de chunks sont prêts quand les mobs atteignent le bord de la zone chargée,
donc davantage sont générés de façon synchrone sur le tick — le défaut du § 5.4. Les deux
lignes n'ont pas tourné au même moment (réserve de charge habituelle), mais le sens de
l'écart passe par un compteur, pas par une impression. Tant que le correctif 2 n'est pas
en place, affamer le pool d'ouvriers *pousse* la génération sur le thread de tick.

**Le serveur sous `nice -n 10`**, binaire Release corrigé (correctif 1), banc, 1 geste/s —
à la **même** charge médiane (24,3) que sa ligne de référence :

| | nice 0 | **nice 10** |
|---|---|---|
| cassage p50 / p90 / p99 / max | 1,2 / 5,3 / 19,4 / 20,0 ms | **0,3 / 9,1 / 14,9 / 333 ms** |
| tours de boucle / s | 19,01 | 19,10 |
| `block edit` p50 / max | 10 µs / 0,92 ms | 7 µs / 0,13 ms |

À cette charge, `nice 10` ne change rien de mesurable, un pic isolé à 333 ms mis à part.
Deux explications, que ces données ne départagent pas : (1) **corrigé, le serveur demande
peu de CPU** sur ce banc — il reste trop peu de travail pour qu'une priorité plus basse se
voie ; (2) **macOS ordonnance d'abord par classe QoS**, et chaque thread du serveur fixe la
sienne (`set_thread_role`) : un `nice` au niveau du processus les déplace peut-être à peine.
Conséquence pratique, favorable : baisser la priorité des *compilations* (§ 7.1) ne demande
pas d'élever le serveur. Ce qui **n'est pas mesuré** : l'effet réel d'un `nice` sur les
threads des compilateurs eux-mêmes, qui ne fixent pas de QoS.

## 5. Ce qui a été corrigé

### 5.1 Le moteur de lumière, réécrit à résultat identique (`relight.{hpp,cpp}`)

Sorti de `server.cpp`, avec le registre passé explicitement (le global mutable
`light_blocks` disparaît, principe 6). Trois changements, aucun sur *ce* qui est calculé :

1. **Graines du remplissage du ciel** : seules les cases éclairées dont une colonne
   voisine est plus sombre à cette hauteur (ou juste au-dessus du plancher de leur propre
   colonne). Le remplissage converge vers le même point fixe depuis toute graine qui
   contient chaque case capable d'élever une voisine : une case à 15 entourée de 15 n'élève
   personne ;
2. **Le 3×3 en grille** : le chunk d'une case se trouve par deux décalages et un indice,
   au lieu d'une recherche linéaire dans une liste à chaque lecture et écriture ;
3. **Lumière des blocs** : une section dont la palette ne nomme aucun état émetteur n'est
   pas relue case par case (une palette ne rétrécit jamais : au pire elle fait lire une
   section pour rien, jamais sauter une section utile). Seules les sections en mode direct,
   sans palette, sont toujours relues.

**Preuve** : `tests/test_relight.cpp` garde l'ancien moteur tel qu'il était et compare,
sur trois terrains aléatoires (colonnes de hauteurs variées, toits, verre, feuilles,
poches de grottes avec pierre lumineuse et lave, torches), avec et sans voisin manquant,
avant et après une édition (puits creusé, toit posé, torche au fond, bloc contre la
frontière) : **chaque quartet de chaque tableau de chaque section**, et le fait qu'il soit
matérialisé ou uniforme. 244 assertions, zéro écart.

Coût d'une édition (voisinage 3×3 complet), terrain du test, Debug
(`test_ov_server "[.relight-bench]"`, 20 tours) :

| | charge ~45 | charge ~30 (avec le détail) |
|---|---|---|
| édition entière, ancien → nouveau | 626 → 287 ms (×2,2) | **393 → 135 ms (×2,9)** |
| ciel sur le 3×3 | — | 238 → 91 ms (×2,6) |
| lumière des blocs, 9 chunks | — | 182 → 77 ms (×2,4) |

Les deux moitiés gagnent à peu près autant. Ce qui reste de la seconde est surtout pour
huit chunks voisins dont les blocs n'ont pas changé : un indicateur « lumière des blocs à
jour » par chunk en retirerait ~8/9. **Non fait** : les chunks générés arrivent sans aucune
lumière de bloc, et ces passes voisines sont aujourd'hui ce qui les éclaire — l'indicateur
devrait naître avec un vrai calcul de lumière à la génération. **Non mesuré en
Release** : seul `ov_dedicated` a été construit dans ce préréglage (`--target
ov_dedicated`), pour épargner une heure de compilation et ~1 Go de disque sur une machine
qui en manquait ; le binaire de test Release n'existe pas. L'effet en Release est mesuré
de bout en bout, sur le banc (§ 5.3), pas isolément.

### 5.2 Le rallumage quitte le chemin du `Block Update`

`set_block_and_broadcast` rallumait le 3×3 **avant** d'envoyer le `Block Update`, sur le
thread réseau, verrous tenus. Il ajoute maintenant le chunk à `tick_relight` — l'ensemble
que le drain des fluides remplissait déjà — et rend la main. Le tick rallume chaque chunk
de l'ensemble **une fois** par tour (`flush_tick_writes`), quel que soit le nombre de
blocs écrits dedans : un cassage qui remodèle cinq voisins dans le même chunk coûtait six
voisinages 3×3, il en coûte un. Le remplissage d'une commande (`/fill`) passe par le même
ensemble.

Ce qui change, dit franchement : **la lumière stockée d'un chunk édité a au plus un tour de
boucle de retard**. Le client éclaire lui-même ses propres modifications (c'est déjà ce
que disait le code) et un rallumage n'a jamais été renvoyé au client ; seul un chunk
*envoyé* entre l'édition et le rallumage suivant — à un autre joueur qui arrive — porte la
lumière d'avant sur ce bloc. Le spawner et les plantes lisent la lumière stockée : ils
voient la même au tour suivant.

Le test `if (!tick_relight.empty())` est passé **sous** `chunk_mutex` : le thread réseau
alimente maintenant l'ensemble, et le lire sans le verrou aurait été une course.

### 5.3 Avant / après sur le banc

Cassage → `Block Update`, en ms (p50 / p99 / max), et tours de boucle par seconde. Même
sonde, mêmes paramètres des deux côtés (`.scratch/series.sh`) ; « avant » = instrumentation
seule, « après » = instrumentation + correctif.

| série | avant : cassage | avant : tours/s | après : cassage | après : tours/s |
|---|---|---|---|---|
| Debug, banc, 1 geste/s | 199 / 1 963 / 3 951 | 5,42 | **20,3 / 817 / 2 413** | 10,37 |
| Debug, banc, 4 gestes/s | 205 / 3 965 / 4 388 | 4,25 | **6,0 / 1 473 / 2 006** | **10,66** |
| Debug, graine 12345, 1 geste/s | **impossible d'entrer** en 300 s : spawn prêt à **290,4 s** (64 chunks) | 19,56 (sans joueur) | spawn prêt à 187 s ; cassage **13 072 / 25 037 / 27 078**, 10 perdus sur 24 (§ 5.4) | 12,13 |
| Release, banc, 1 geste/s | 12,2 / 63,1 / 316 | 17,36 | **1,2 / 19,4 / 20,0** | 19,01 |
| Release, banc, 4 gestes/s | 11,7 / 648 / 897 | 18,58 | **1,1 / 41,9 / 208** | 18,00 |
| Release, graine 12345, 1 geste/s | **impossible d'entrer** en 300 s : spawn prêt à **292,6 s** (64 chunks en 311 s) | 19,53 (sans joueur) | spawn prêt à **36,1 s** ; cassage **52,6 / 5 203 / 5 683**, p90 **3 683** | 14,84 |

**Release, banc, 1 geste/s — ce qui porte la conclusion et ce qui ne la porte pas.** La
série « après » a tourné à une charge médiane de **24**, contre **35** pour l'« avant » : une
partie du gain de bout en bout (×10 au p50) revient à une machine plus calme, et ce facteur
seul ne prouve rien. Deux chiffres, eux, ne dépendent presque pas de la charge parce qu'ils
mesurent *combien de travail* est fait, pas combien de temps il attend :

| | avant | après |
|---|---|---|
| `block edit` sur le thread réseau, p50 / max | 11,0 ms / 64 ms | **10 µs / 0,92 ms** (~×1 000) |
| rallumage sur le tick, total sur la série | 10,1 s | **1,48 s** (×6,8) |
| … le plus lourd dans | 38 des 72 ticks lents | 4 des 26 |

**Release, banc, 4 gestes/s** — même forme, même réserve (charge médiane 28 contre 36) :
`block edit` p50 10,5 ms → **7 µs** (max 535 → 21,6 ms), rallumage sur le tick 7,16 s →
**1,40 s** (×5,1). C'est dans la queue que le gain se voit le mieux : **p99 648 → 42 ms**.
À ce rythme les gestes ne s'enfilent plus derrière les rallumages des précédents. Les tours
de boucle par seconde, eux, ne bougent pas (18,58 → 18,00) et aucun gain n'est revendiqué
là : le tick reste borné par l'envoi des chunks et les random ticks (dont un pic de 1,2 s à
8 % de CPU — une attente, pas un calcul).

**Debug, banc, 1 geste/s** — même réserve (charge médiane 27 contre 37). Ce qui ne dépend
pas de la charge : `block edit` p50 82 ms → **40 µs** (max 614 → 4,1 ms, ~×2 000),
rallumage sur le tick 27,1 s → **6,75 s** (×4), le plus lourd dans 33 des 98 ticks lents
au lieu de 148 des 204. Ce qui reste dans la queue (p90 540 ms) a un nom : **l'envoi des
chunks**, désormais la phase la plus lourde (22,4 s, la plus coûteuse dans 45 des 98 ticks
lents), qui tient `players_mutex` — l'attente de ce verrou côté réseau monte encore à
p99 508 ms. C'est le point 6 du § 7.

**Debug, banc, 4 gestes/s — la comparaison la plus propre de l'étude**, parce que la charge
y est la même des deux côtés (médiane **29,2** avant, **28,4** après) : la réserve des
autres lignes ne s'applique pas ici.

| | avant | après |
|---|---|---|
| cassage p50 / p90 / p99 / max | 205 / 1 918 / 3 965 / 4 388 ms | **6,0 / 380 / 1 473 / 2 006 ms** |
| `block edit` p50 / max | 88 ms / 1 284 ms | **28 µs / 5,3 ms** |
| rallumage sur le tick | 11,2 s | 5,8 s |
| tours de boucle / s | 4,25 | **10,66** (×2,5) |

À charge égale, le correctif divise le cassage médian par 34, le p99 par 2,7, et **multiplie
par 2,5 le rythme réel du serveur**. Ce qui reste de la queue a une signature nette : un
envoi de chunks a tenu `players_mutex` **4,9 s** d'un coup, et l'attente la plus longue du
thread réseau sur ce verrou vaut… **4,9 s**. C'est la preuve la plus directe du point 6 du
§ 7.

Le thread réseau ne rallume plus rien : c'est ce qui fait que l'accusé et le `Block Update`
partent en ~1 ms. Le tick rallume moins (un voisinage par chunk touché et par tour, avec un
moteur plus rapide). **La phase la plus lourde est désormais l'envoi des chunks** (le plus
lourd dans 17 des 26 ticks lents) — le prochain chantier, nommé au § 7.

Pour les lignes « graine », le chiffre qui compte n'est pas la latence de cassage — la
sonde n'est jamais entrée — mais **le temps de préparation du spawn**, et il mesure la
génération, que le correctif ne touche pas. En Release, 64 chunks ont pris ~293 s ; le
commentaire de `scripts/lab.sh` en relève 64 dans la première minute sur une machine plus
calme. Même binaire, cinq fois plus lent : c'est la part d'un cœur que reçoivent les
ouvriers (~2 %, § 4.3), pendant une mesure où la machine a fait **15 Go de swap-ins**.

**Les deux binaires Release comparés sont bien deux codes différents**, vérifié et non
supposé : le binaire « avant » a été compilé depuis `server.cpp` non corrigé (étape 3 de
sa construction, avant l'application du correctif) puis mis de côté
(`.scratch/ov_dedicated-release-before`), l'« après » reconstruit ensuite. Tailles
4 719 072 et 4 713 360 octets, `cmp` : différents ; le symbole `relight_after_edit`
apparaît **0 fois** dans l'« avant » et **1 fois** dans l'« après » (`nm -C`). Sans ce
contrôle, une reconstruction qui n'aurait pas repris le correctif aurait mesuré deux fois
le même code et affiché un gain nul — ou pire, un gain de bruit.

### 5.4 La seconde cause, que le banc plat ne pouvait pas montrer : la génération sur le tick

La série Release « après » sur la graine 12345 est la première où la sonde est entrée sur un
monde généré (la machine s'était calmée : spawn prêt en 36,1 s). Avec le correctif de
l'éclairage déjà en place, **le « 2 s » du rapport d'origine réapparaît** :

| Release, graine 12345, 1 geste/s, correctif 1 | p50 | p90 | p99 | max |
|---|---|---|---|---|
| cassage → `Block Update` | 52,6 ms | **3 683 ms** | 5 203 ms | 5 683 ms |
| pose → `Block Update` | 301,8 ms | 4 271 ms | 4 989 ms | 7 003 ms |

Le profil nomme la cause sans ambiguïté :

- phase **`entities`** : 33,9 s au total, **7,33 s** pour un seul tick, la plus lourde dans
  **9 des 9** ticks lents, à 76 % de CPU — du travail, pas une attente ;
- `chunk source: … 31 generated on the tick thread` — le compteur qui existe précisément
  pour rester à zéro.

Le chemin, lu dans le code : la phase des entités lit les blocs (collisions, recherche de
chemin) par `block_at`, qui appelle **`chunk_at`**, qui, pour un chunk absent d'un monde
généré, **lance la génération sur le thread appelant** et incrémente
`synchronous_generations`. Un mob au bord de la zone chargée fait donc générer des chunks
au thread de tick, **sous `chunk_mutex`** — le verrou que chaque cassage attend sur le
thread réseau avant d'écrire. Les accroches des fluides évitent déjà exactement ce piège
(`chunk_if_resident`, « air, et non chargé ») ; la phase des entités ne le faisait pas.

**Correctif 2** : les deux lecteurs de la phase des entités (`WorldView` pour les
collisions, `MobLevel` pour les chemins) passent par une lecture *résidente seulement* —
un chunk absent se lit comme de l'air et n'est jamais généré — et `MobLevel::is_loaded`
dit la vérité au lieu de `true`, pour qu'aucun chemin ne soit planifié dans un terrain qui
n'existe pas encore. Changement de comportement, dit : un mob au bord extrême de la zone
chargée (à huit chunks de tout joueur) voit de l'air au lieu de déclencher la génération.

**La même chose en Debug, pire.** Série Debug « après » (correctif 1 seul) sur la même
graine : la sonde entre (spawn prêt en 187 s), et un cassage met **13,1 s au p50, 27,1 s au
pire**, 10 gestes sur 24 perdus au-delà de 30 s. Un tick de la phase `entities` a duré
**34,2 s à 85 % de CPU** (8 générations synchrones, en Debug, sur une machine affamée) ; un
tick de `chunk requests` a attendu **36,1 s à 1 % de CPU** — le tick bloqué sur
`chunk_mutex` pendant que quelqu'un d'autre le tenait.

**Une imprécision de l'instrument, trouvée là.** La ligne d'arrêt dit « N generated on the
*tick* thread », mais le compteur de `chunk_at` s'incrémente sur **n'importe quel** thread.
L'attente de 36 s du tick s'explique naturellement par une génération synchrone sur le
thread **réseau** — un cassage ou une pose qui lit un bloc voisin dans un chunk non résident
(`set_block_connected` → `block_at` → `chunk_at`), sous le verrou — mais le compteur ne
permet pas de l'affirmer. Il est donc séparé par thread (« N generated on the tick thread,
M on other threads ») avant la mesure du correctif 2, pour que cette mesure dise si le
correctif supprime *toutes* les générations synchrones ou seulement celles du tick.

Contrôles avant de mesurer le correctif 2 : `test_ov_server` vert avec le correctif ;
reconstruction Debug vérifiée (le journal recompile bien `server.cpp.o`) ; le binaire
Release mis de côté pour cette mesure (`.scratch/ov_dedicated-release-after2`) contient la
nouvelle chaîne de rapport « on other threads » **1 fois**, le binaire du correctif 1
**0 fois** (`strings`) — les deux mesures portent bien sur deux codes différents.

**Après correctif 2** — Release, graine 12345, 1 geste/s, charges médianes **proches**
(26,9 pour le correctif 1 seul, 23,9 avec le correctif 2) :

| | correctif 1 seul | **correctifs 1 + 2** |
|---|---|---|
| cassage p50 / p90 / p99 / max | 52,6 / 3 683 / 5 203 / 5 683 ms | **2,9 / 13,9 / 62,4 / 284 ms** |
| pose p99 / max | 4 989 / 7 003 ms | **48,1 / 62,3 ms** |
| générations synchrones **sur le tick** | 31 | **0** |
| générations synchrones **sur un autre thread** | (non compté alors) | **2** |
| phase `entities` : total / pire tick | 33,9 s / 7,33 s | **3,1 s / 0,71 s** |

Ce que le correctif 2 a supprimé est exactement ce qu'il visait : **plus aucune génération
sur le tick**, plus de tick de sept secondes dans les entités, et le cassage sur monde généré
repasse de plusieurs secondes à quelques millisecondes (p99 ÷83).

**La troisième cause, révélée par le compteur séparé.** Les 2 générations « sur un autre
thread » ne peuvent venir que du thread réseau, et elles laissent une trace précise dans la
même série : un paquet *Play* de **19,73 s** (`network packet` max) et, sur le tick, une
attente de **19,7 s à 0 % de CPU** dans `chunk requests` — le tick bloqué sur `chunk_mutex`
pendant que le thread réseau générait sous ce verrou. 1 cassage et 1 pose perdus dans cette
fenêtre. Ce n'est **pas** `set_block_and_broadcast` (`block edit` max 12,7 ms) : c'est une
lecture faite *ailleurs* dans un gestionnaire de paquet, sous le verrou — le candidat le plus
probable est la lecture des voisins par `set_block_connected` (`neighbours_of` → `block_at` →
`chunk_at`), qui déborde d'un bloc dans le chunk suivant. La série « correctif 1 » la montrait
déjà sans pouvoir la nommer (`network packet` max 7,3 s), cachée dans l'ancien compteur.

**Pas de régression là où le correctif 2 n'a rien à faire** — Release, banc plat, 1 geste/s,
charges comparables (24,3 avec le correctif 1 seul, 23,5 avec les deux) : cassage p50 / p99
/ max **1,2 / 19,4 / 20,0 ms → 0,6 / 19,4 / 560 ms**, 19,01 → 18,31 tours/s. Médiane et p99
identiques ; le pic isolé à 560 ms tombe sur un tick d'**envoi de chunks** de 1,82 s à 18 %
de CPU — une attente, et encore le point 6 du § 7 (l'envoi de chunks y est la phase la plus
lourde dans 16 des 27 ticks lents).

**Nommée, non corrigée ici** : plusieurs gestionnaires lisent par `block_at` (placement,
remodelage, bruit de pas), et le compteur dit *quel thread*, pas *quel appel*. Changer ces
lectures partagées sur une supposition serait exactement ce que ce dépôt refuse. Le pas
suivant est au § 7 (point 7).

### 5.5 Les paquets passent sur le thread de tick ; les verrous du monde deviennent des preuves

Le § 5.4 laissait une cause nommée (§ 7, point 7) posée sur un défaut de structure (§ 7,
point 5) : les gestionnaires de paquets tournaient sur le thread réseau et entraient dans le
monde sous `chunk_mutex` et `players_mutex`. Tant que c'est le cas, une lecture lente dans un
gestionnaire bloque le tick, et un tick lent bloque tous les paquets de tous les joueurs.
Plutôt que de corriger chaque site, le correctif retire la cause :

- **le thread réseau ne fait plus que découper des octets.** Chaque paquet *Play* décodé est
  rangé dans une file (`src/ov_server/src/inbound_queue.{hpp,cpp}`) ; le thread de tick la
  vide au début de chaque tour **et pendant qu'il attend le tick suivant** — un cassage
  n'attend donc pas le prochain tick, il est traité dès que le tour en cours se termine ;
- **`chunk_mutex` et `players_mutex` ne sont plus des mutex**
  (`src/ov_server/src/tick_thread_lock.{hpp,cpp}`). `lock()` ne prend rien : il vérifie que
  l'appelant est le thread de tick. En Debug, le premier accès d'un autre thread arrête le
  processus en le nommant ; en Release, il est compté pour le rapport d'arrêt. Les ~150
  `scoped_lock` restent en place — ils compilent toujours, et ce qu'ils affirment est
  désormais vérifié au lieu d'être supposé ;
- **un gestionnaire de paquet ne génère plus jamais de chunk.** Sur un monde généré, une
  lecture de bloc dans un chunk absent rend de l'air, une écriture y est refusée, et les deux
  sont comptées ; toute génération à la volée qui resterait est journalisée avec le paquet
  qui l'a causée. C'est le point 7 du § 7, dans l'ordre qu'il donnait : attribuer, puis
  appliquer le traitement du correctif 2 ;
- l'entrée d'un joueur n'attend plus le chunk du spawn : elle le cherche une fois ;
- un paquet refusé par son gestionnaire ferme proprement la connexion. Avant, le joueur
  restait dans la table des joueurs jusqu'à la fin du processus.

**Changement de comportement, dit** : un gestionnaire qui lit un bloc au-delà de la zone
générée (la connexion d'une barrière posée au bord extrême, par exemple) y voit de l'air
jusqu'à ce que le chunk arrive par les ouvriers.

Mesure — **Debug**, graine 12345, 60 s demandées, 1 geste/s
(`scripts/bench_play.py --world=seed:12345 --seconds=60 --edit-rate=1`). « — » : non relevé
dans la série « avant ».

| Debug, graine 12345, 1 geste/s | avant | **après** |
|---|---|---|
| cassage → `Block Update` p50 / p90 / p99 / max | 4,2 s / — / — / 16,2 s | **1,8 / 15,5 / 31,2 / 124 ms**, 0 perdu sur 26 |
| pose → `Block Update` p50 / p99 / max | — | 4,0 / 43,7 / 110 ms, 0 perdue sur 26 |
| pire tick | 18,19 s (`entities` : 18,18 s) | 557 ms (sauvegarde automatique : 533 ms) |
| pire traitement d'un paquet | 18,15 s | 25,0 ms |
| attente d'un paquet dans la file p50 / p99 / max | (la file n'existait pas) | 0,6 / 123 / 553 ms |
| « can't keep up » | 4 | 1 |
| générations synchrones : tick / autres threads | 3 / 2 | **0 / 0** |
| charge médiane de la machine | 23,6 | **53,2** |

Rapport d'arrêt de la série « après » : **1 229 paquets traités sur le thread de tick**
(0 refusé), **0 verrou du monde pris hors du thread de tick**, 0 chunk généré pour un paquet,
1 396 lectures et 0 écriture de chunks pas encore générés répondues sans générer. Le serveur
Debug s'arrête au premier accès étranger : qu'il ait tourné jusqu'au bout est la même
affirmation, faite par le programme.

Lecture :

- **Le cassage passe de plusieurs secondes à quelques millisecondes, sous une charge plus de
  deux fois plus haute** (53,2 contre 23,6). La comparaison joue *contre* le correctif ;
  aucune compilation de cet agent ne tournait pendant la série « après ».
- **Le pire paquet n'attend plus un verrou, il attend un tour de boucle.** Le max de la file
  (553 ms) tombe sur le tick de la sauvegarde automatique (533 ms) : la sauvegarde est encore
  synchrone sur le tick (`ROADMAP.md`, « asynchrone via COW : à venir »).
- **Ce qui reste des ticks lents est du calcul, pas de l'attente** : 152 ticks au-delà de
  50 ms, où la phase la plus lourde est le rallumage dans 81 cas et les entités dans 66, les
  deux à ~78 % de CPU. Plus aucune phase n'attend à 0 % de CPU.
- **L'envoi de chunks n'apparaît plus** : 285 ms au total sur la série, 17 ms au pire,
  phase la plus lourde dans **0** des 152 ticks lents — il pesait dans le § 5.3 parce qu'il
  tenait les deux verrous, et les verrous n'excluent plus personne.

### 5.6 Sections copy-on-write : le `Chunk Data` s'encode sur le thread réseau

Le § 5.5 a retiré l'attente ; l'encodage, lui, restait sur le tick — jusqu'à 8 `Chunk Data`
par joueur et par tick. Le principe 3 de `CLAUDE.md` dit comment le partager : par
`shared_ptr<const>`, en copy-on-write. Ce qui est en place :

- **une section garde blocs, biomes et lumière dans un stockage partagé**
  (`src/ov_world/include/ov/world/chunk_section.hpp`). Copier une section copie un
  pointeur et **marque les deux côtés** ; le premier qui écrit prend sa propre copie. La règle
  est volontairement brutale — *un stockage vu par deux sections n'est plus jamais écrit* —
  parce que l'alternative, lire `use_count()` sur le tick pendant que le réseau relâche sa
  référence, demanderait une barrière isolée pour être correcte, et TSan ne les modélise pas ;
- **`Chunk::snapshot()`** rend un `shared_ptr<const Chunk>` : 24 pointeurs de sections pour
  l'Overworld, plus les heightmaps, les entités de bloc et les départs de structures, copiés
  parce qu'ils sont petits ;
- **`Connection::send_chunk`** (`src/ov_protocol/include/ov/protocol/listener.hpp`) : la
  connexion TCP encode l'instantané **sur son propre thread, dans la même file que `send`** —
  un `Block Update` envoyé après le chunk ne peut pas le doubler. La version par défaut
  encode sur place et appelle `send`, octet pour octet ce que le serveur envoyait avant ;
- **le piège évité** : le rallumage prend `section->blocks()` par référence puis écrit la
  lumière de la même section. Si la copie avait lieu à cette écriture, la référence
  pointerait dans un stockage que seul l'instantané possède — libéré par le thread réseau
  quand il veut. `Chunk::section_for_y` en écriture **désolidarise donc avant de rendre la
  section**, et le test « a reference from a writable section outlives the snapshot » fixe ce
  comportement.

Vérifié :

| | Debug | TSan |
|---|---|---|
| `test_ov_world` (dont 6 cas `[snapshot]`) | 97 cas verts | 97 cas verts, 0 rapport |
| `test_ov_protocol` (dont le test sur socket réel) | 150 verts, 1 sauté | 150 verts, 1 sauté, 0 rapport ; **20 passes sur 20** |
| `test_ov_server` | 125 cas verts | 124 verts, 1 sauté, 0 rapport |

Le cas sauté de `test_ov_protocol` demande un monde dans `run/world/region`, absent de ce
worktree ; il l'était déjà avant. Le test sur socket réel envoie un paquet, un instantané,
puis un autre paquet, **pendant que le thread du test réécrit le chunk** : les trois arrivent
dans l'ordre, et le chunk reçu est celui d'avant la réécriture.

**Une première mesure ratée, et ce qu'elle montre.** La série « après copy-on-write » lancée
juste après ces tests n'a jamais fait entrer la sonde : **0 bloc généré (0 chunk) en 3,6 min
avec 4 ouvriers**, zone de spawn à 0 %, 7 connexions refusées « refused for now » puis fermées
après 30 s de silence. Le serveur n'avait pas de problème de tick (20 tours/s, aucun au-delà
de 50 ms) : il n'avait simplement rien à envoyer. Sa part de CPU était de **42 %** contre 190 %
sur la série du § 5.5, avec **7,7 Go de swap relus** pendant la mesure — les ouvriers de
génération affamés, exactement le § 4.3. Le code des ouvriers ne copie jamais de chunk (le
pipeline déplace, `pipeline.cpp:390`) : le copy-on-write n'y ajoute aucune copie.

**La seconde tentative réfute la famine comme explication suffisante.** Relancée après la suite
complète (16 exécutables verts, `test_ov_worldgen` en 352 s contre 512 s avant le changement,
0 avertissement), sur une machine plus calme — charge médiane **16**, **300 Mo** de swap relus,
**148 %** de CPU pour le serveur —, la sonde n'entre toujours pas : 0 bloc généré en 4 min.
Les quatre ouvriers ne sont pas bloqués pour autant : chacun écrit dans le journal du début à
l'arrêt du serveur (dernières lignes 24 à 53 s *après* le rapport d'arrêt), ils travaillent.
Le chiffre qui recadre tout est dans la série du § 5.5, faite *avant* le copy-on-write : la
sonde y est entrée à **195,3 s**, pour une patience de 240 s. En Debug sur cette machine, la
préparation du spawn consommait déjà 80 % de la patience ; deux échecs à 254–256 s restent dans
cette marge.

**Une patience de 600 s ne suffit pas non plus** : troisième tentative, 0 bloc en 620 s, charge
médiane 25 (17,4 à 57,8), **26 %** d'un cœur pour le serveur. Des ouvriers qui n'obtiennent
qu'un quart de cœur à eux quatre ressemblent à la famine du § 4.3 — mais c'est aussi ce que
montrerait un ouvrier ralenti par le changement. Une seule expérience sépare les deux.

**A/B à conditions identiques : les deux binaires lancés en même temps.** Le serveur d'avant le
copy-on-write (sources de `fa3b709` recompilées — `chunk.cpp`, `chunk_section.cpp`,
`listener.cpp` et `server.cpp` dans le journal de construction) et celui d'après, deux mondes
et deux ports distincts, même graine, même sonde, patience 600 s :

| | avant copy-on-write | **après copy-on-write** |
|---|---|---|
| blocs générés en ~600 s | 0 | 0 |
| entrée de la sonde | refusée, abandon à 604,7 s | refusée, abandon à 605,9 s |
| CPU du serveur | 13 % d'un cœur | 13 % d'un cœur |
| tours de boucle du tick | 19,96 /s | 19,96 /s |

Charge pendant la paire : `uptime` **29,21 / 37,27 / 32,22** avant, **51,68 / 50,32 / 41,38**
après ; médiane 37,9 (27,2 à 64,1), 1,5 Go de swap relus. Selon l'agent coordinateur, la
machine faisait tourner au même moment des outils de parité, plusieurs suites de tests et deux
JVM.

**Verdict** : le binaire d'avant échoue exactement comme celui d'après. Ce n'est pas le
copy-on-write qui empêche la génération ; ce sont des ouvriers en QoS `UTILITY` sans temps de
CPU sur une machine à 40–50 de charge — le point 2 du § 7, qui reste la prochaine correction.

**Non mesuré, et dit comme tel.** Aucune de ces séries n'a fait entrer un joueur : il n'y a donc
**pas de chiffre** de latence de cassage ni d'envoi de chunks après le copy-on-write, et ceux
qu'une série contendue aurait donnés ne seraient pas comparables au § 5.5. La série qui les
fournira doit passer par la voie de construction partagée des agents (aucune compilation à
côté), avec `uptime` avant et après — et une charge encore bien au-dessus de ~10 notée à côté
des chiffres.

## 6. Côté client

Le client (`ov_voxel`) imprimait déjà `cpu`, `rec` et `gpu` ; un `max` à 733 ms n'y disait
pas *où*. Deux lignes de plus à l'arrêt, chronométrées dans la boucle de frame :

- `apply` : `session->apply` + `entity_world.apply` — les chunks, blocs et entités reçus
  du réseau, rangés dans le monde du client ;
- `mesh` : `session->mesh_pending(4.0)` — le maillage sous budget.

Mesure « avant » : client Debug, 1 800 frames sans vsync, `--walk --dig`, contre le
serveur Debug non corrigé sur le banc `ov_lab` (`.scratch/client_run.sh`), charge ~40 :

| | p50 | p99 | max |
|---|---|---|---|
| cpu (frame entière) | 5,05 ms | 36,1 ms | **457,7 ms** |
| **mesh** | 0,00 ms | **18,3 ms** | **160,5 ms** |
| apply | 0,00 ms | 0,09 ms | 1,41 ms |
| rec | 0,25 ms | 0,76 ms | 4,47 ms |
| gpu | 0,56 ms | 5,58 ms | 10,52 ms |
| snd | 0,004 ms | 0,018 ms | 8,93 ms |

Lecture :

- **Le maillage est la plus grosse part mesurée du p99** (~18 des 36 ms). Son budget de
  4 ms n'est vérifié *qu'après* chaque section, pour garantir un progrès : une section qui
  coûte 156 ms en Debug sur une machine chargée dépasse le budget de 156 ms. Le budget
  borne le nombre de sections par frame, pas le coût d'une section.
- **Une hypothèse réfutée** : `Session::mark_dirty` fait une recherche linéaire dans la
  file des sections sales, 120 fois par chunk reçu — quadratique en théorie. Mesuré :
  `apply` ne dépasse pas **1,41 ms** avec 89 chunks. Pas là.
- **~300 ms du pire frame ne sont dans aucun segment chronométré** (cpu max 458 ms, mesh
  max 160 ms, le reste < 11 ms chacun). Restent la lecture de la fenêtre, la
  présentation, la boucle de physique qui rattrape jusqu'à 250 ms de retard, ou la simple
  préemption à une charge de 40 sur 8 cœurs. **Non attribué**, et dit comme tel.
- **Non corrigé côté client.** Le correctif qui compte — mailler sur un thread de travail
  — n'est pas « simple » : `mesh_one` lit le monde du client et écrit dans l'arène du
  rendu. Nommé comme le prochain chantier du client.

## 7. Réglages proposés — non appliqués

Rien de ce qui suit n'a été appliqué aux autres agents ni à la machine : ce sont des
propositions, chacune adossée à une mesure de ce fichier.

1. **Les compilations des agents en `nice` / QoS basse.** Sur cette machine, les phases
   du tick qui calculent n'ont obtenu que **45 à 79 % d'un cœur**, avec 83 000 à 157 000
   commutations de contexte involontaires par minute de mesure (§ 3). Les compilateurs sont
   des processus de QoS par défaut ; les lancer sous `nice -n 10` (ou `taskpolicy -b`) les
   ferait passer derrière le serveur et le client d'un joueur, sans changer leur débit
   quand la machine est libre. C'est le réglage qui toucherait le plus de choses d'un coup.
2. **Les ouvriers de génération ne devraient pas être en `UTILITY` quand un joueur attend
   un chunk.** Mesuré au § 4.3 : 1,8 % d'un cœur par ouvrier, spawn à 0 % pendant plus de
   quatre minutes. `USER_INITIATED` pour les requêtes près d'un joueur (et `UTILITY` pour
   le reste) serait la séparation honnête ; c'est un changement de `ov_base/thread.cpp` et
   de `AsyncChunkSource`, qui mérite sa propre mesure avant d'être fait.
3. **Ne pas réduire `OV_WORLDGEN_WORKERS`** pour soulager la machine. L'intuition — « sur
   une machine saturée, plus d'ouvriers n'achète rien, ils attendent tous le même cœur » —
   a été écrite ici avant d'être mesurée, et **la mesure la contredit** (§ 4.4) : avec un
   seul ouvrier, le spawn met 99 s au lieu de 36, il y a davantage de générations
   synchrones sur le tick, et le cassage passe de 53 à 740 ms au p50.
4. **Le swap et le disque** (§ 4.2) : chaque JVM ou build supplémentaire sur une machine qui
   swappe coûte aussi de l'espace disque ; le plancher de 1,5 Go du briefing peut être
   franchi par le seul swap.
5. **Deux défauts du serveur, nommés et non corrigés ici** :
   - un joueur dont la réponse au keep-alive arrive après le keep-alive suivant est déconnecté
     **sans une ligne au niveau INFO** (§ 2.1) — au minimum, le journaliser ;
   - l'unique thread réseau fait *toutes* les écritures de socket : tout ce qui le retient
     retient le monde entier. Tant que des paquets de jeu sont traités sur ce thread sous
     les verrous du monde, c'est la prochaine source de latence. **Appliqué au § 5.5** : les
     paquets de jeu sont traités sur le thread de tick, et le thread réseau ne fait plus
     que lire et écrire des octets.
6. **Le prochain correctif côté serveur : l'envoi des chunks.** Mesuré : il encode jusqu'à
   8 `Chunk Data` par joueur et par tick **en tenant `players_mutex` et `chunk_mutex`** ; en
   Debug il a porté l'attente du verrou joueurs côté réseau jusqu'à p99 754 ms (§ 3.2), et
   après le correctif de l'éclairage c'est **la phase la plus lourde** en Release (la plus
   coûteuse dans 17 des 26 ticks lents, § 5.3). Le remède proposé, non mesuré : relever sous
   `players_mutex` seulement *quels* chunks envoyer à *qui*, encoder sous `chunk_mutex`
   seul, envoyer sans aucun verrou (`Connection::send` poste déjà sur l'exécuteur réseau).
   Non fait ici parce qu'il restructure un bloc de `server.cpp` que l'agent des écrans
   modifie en parallèle (fours et établis vivent dans la même section verrouillée) : une
   fusion à trois voies y serait un risque réel pour un gain qui mérite sa propre mesure.
   **Appliqué dans le code aux § 5.5–5.6**, sous une autre forme que celle proposée ici : il
   n'y a plus de verrou à tenir (§ 5.5), et l'encodage a quitté le tick — le tick prend un
   instantané (`Chunk::snapshot`), le thread réseau encode (`Connection::send_chunk`). Le
   bloc de `server.cpp` n'a changé que sur ces quelques lignes. **Le gain n'est pas encore
   mesuré** (§ 5.6, fin).
7. **La troisième cause : la génération synchrone sur le thread réseau.** Mesurée (§ 5.4) :
   2 chunks générés hors du tick en une minute, un paquet de 19,7 s, et le tick bloqué
   autant de temps derrière `chunk_mutex`. Deux pas, dans l'ordre : (1) **attribuer** —
   quand `chunk_at` génère hors du tick, retenir l'identifiant du paquet *Play* en cours de
   traitement (une variable propre au thread réseau suffit) et l'imprimer à l'arrêt avec le
   compteur ; (2) **appliquer** au site nommé le traitement du correctif 2 : lecture des
   chunks résidents seulement, un chunk absent se lit comme de l'air et n'est jamais
   généré depuis un gestionnaire de paquet. La génération n'a qu'une place : les ouvriers,
   à la demande d'un ticket. **Appliqué au § 5.5**, les deux pas, pour tous les
   gestionnaires à la fois : 0 génération synchrone sur la série « après ».

### Note de méthode : le bruit que cet agent a ajouté

Pour ne pas payer une heure de plus par série, une partie des séries a tourné pendant que
cet agent compilait (un seul job, `--parallel 1`) : une unité de charge de plus sur une
machine à 25–49. Chaque ligne de mesure porte la charge relevée pendant son exécution, et
voici, série par série, si une compilation de cet agent tournait :

| série | compilation de cet agent pendant la mesure |
|---|---|
| Debug « avant », banc, 4 gestes/s (§ 3.1) | non |
| Debug « avant », banc, 1 geste/s (§ 3.2, première série) | **oui** (construction Release) |
| Debug « avant », reprise sur binaire mis de côté (§ 3.2, § 5.3) | non |
| Debug « avant », graine 12345 (§ 4.3) | **oui** (construction Release « après ») |
| Release « avant » puis « après » | non — lancées l'une après l'autre, sans compilation |
| Debug « après » | non |
| Debug « après », paquets sur le thread de tick, graine 12345 (§ 5.5) | non |

La première série Debug à 1 geste/s avait une unité de charge de plus que la série
« après » ne l'aura : une comparaison avec elle aurait joué **en faveur** du correctif
(l'« avant » y paraît pire qu'il n'était). Elle n'est donc **pas** la référence : le § 5.3
compare à la reprise, sans compilation, sur le binaire « avant » mis de côté. La première
série reste au § 3.2 pour ce qu'elle mesure bien — la sensibilité du même code à une unité
de charge de plus (p50 ×2,1).
