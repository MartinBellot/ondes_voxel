# Brancher les moteurs — ce qui a été établi, et comment

Ce fichier ne décrit pas un moteur. Il décrit **le câblage** de moteurs qui
étaient déjà écrits, déjà mesurés contre le vrai serveur, déjà couverts par des
tests unitaires — et appelés par personne.

État avant cette vague, vérifié par `grep -rn` sur `src/`, `apps/` et `tools/` :

| moteur | fichier | seuls appelants |
|---|---|---|
| `BlockTickScheduler` | `src/ov_world/src/block_ticks.cpp` | ses propres tests |
| `ticks_to_nbt` / `ticks_from_nbt` | idem | ses propres tests |
| `FluidRules` | `src/ov_gameplay/src/fluid.cpp` | `test_fluid.cpp` |
| `Redstone` | `src/ov_gameplay/src/redstone.cpp` | `test_redstone.cpp` |
| `NaturalSpawner` | `src/ov_gameplay/src/spawning.cpp` | `test_spawning.cpp` |

Conséquence observable : de l'eau posée au seau restait un cube d'un bloc, un
levier n'allumait rien, et le monde ne contenait que les mobs que `--mobs=` y
avait mis à la main. Rien de tout cela n'était une règle manquante.

---

## 1. Ce que le câblage a demandé

### Le corps de tick

`world_ticks.{hpp,cpp}` fait trois choses, dans cet ordre :

1. **vider la file fluide** dans `FluidRules::tick`, puis la file bloc dans
   `Redstone::scheduled_tick`. Les deux files sont séparées parce qu'Anvil les
   stocke séparément et que vanilla les vide séparément ;
2. **notifier les voisins** de chaque position écrite ;
3. **recommencer** tant que la notification produit des écritures, avec un
   plafond de 512 vagues.

Le point 2 est celui qu'on oublie et qu'on ne peut pas contourner. Le
`Level.setBlock` de vanilla notifie les six voisins dans le cadre de
l'écriture ; les deux moteurs sont écrits en le supposant —
`FluidRules::on_neighbour_changed` et `Redstone::neighbour_changed` sont publics
pour exactement cet appelant.

**La notification est une file de vagues, pas une récursion.** Vanilla récurse ;
sur un fil long c'est une pile de mille cadres, et `update_wire` avait déjà
refusé de faire ainsi pour la même raison. Une écriture *enregistre* sa
position, et le pilote vide l'enregistrement par vagues après le retour de la
règle. La vague en cours est **déplacée hors** du niveau avant d'être parcourue :
une écriture faite pendant la notification doit atterrir dans la vague suivante,
sinon une vague poursuit sa propre queue aussi longtemps que le circuit bouge.

### Le verrou

Les crochets du niveau ne prennent **jamais** `chunk_mutex`. Le drain le prend
**une fois**, autour de tout. Deux raisons, et la première est fatale :

- `chunk_mutex` n'est pas récursif. Une règle qui écrirait par
  `set_block_and_broadcast` — qui le prend lui-même — bloque le thread de tick à
  la première goutte d'eau. Le même piège avait déjà été payé par le four
  (commentaire de `host.set_lit` dans `server.cpp`) ;
- une flaque qui se pose fait quelques centaines d'écritures, soit autant
  d'allers-retours de verrou dans un seul tick.

D'où l'extraction de `apply_block_change` : le corps de
`set_block_and_broadcast` moins le verrou. Les appelants existants ne changent
pas.

### L'éclairage différé

`set_block_and_broadcast` rallume un voisinage 3×3 par bloc écrit. Pour un joueur
qui pose un bloc c'est correct ; pour une flaque de 121 blocs c'est 1089
rallumages de chunk dans un tick. Le drain écrit donc avec `relight=false`,
accumule les chunks touchés, et rallume **une fois** à la fin.

---

## 2. Parité mesurée, de bout en bout

Pas des tests unitaires : un serveur réel sur une socket réelle, le monde
`ov_lab`, et un client protocole 763 qui joue un geste et lit ce que le serveur
renvoie. Le client est
`scripts/`-style mais vit hors du dépôt (sonde jetable) ; ce qu'il fait tient en
trois paquets : `Player Action`, `Click Container`, et la lecture des
`Block Update` / `Set Container Content` qui reviennent.

### Redstone — 14 / 14 positions

Parcelle « dust » (x 0, z 0). `ov_lab` écrit des états et n'évalue rien, donc
chaque fil est sauvegardé à `power=0` — exactement ce que fait vanilla au
chargement, et la raison pour laquelle un circuit doit être **poussé** avant
d'être mesuré (piège 12 du briefing).

La poussée est un `dig` sur **l'air** au-dessus du premier fil : écrire de l'air
sur de l'air notifie quand même les six voisins, et c'est tout le chemin de mise
à jour qui est sous test. Rien n'est détruit.

Relevé après sauvegarde, avec `ov_inspect state` :

| x | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| bloc | source | fil | fil | fil | fil | fil | fil | fil | fil | fil | fil | fil | fil | fil | lampe |
| `power` | — | **15** | 14 | 13 | 12 | 11 | 10 | 9 | 8 | 7 | 6 | 5 | 4 | **3** | allumée |

C'est la loi mesurée de `docs/provenance/redstone.md` (« un fil perd un par
bloc »), et la lampe passe à `lit=true`. **13 fils sur 13, plus la lampe : 14 sur
14.** Les formes (`east=side`, `west=side`) sont correctes aussi : le fil se
tourne vers ses voisins.

### Fluides — 121 / 121 positions

Parcelle « basin » (x 0, z 32) : sol de pierre à y = −60, murs à −59, une source
d'eau en (8, −59, 40) jamais évaluée. Même poussée, puis 33 secondes.

Comparaison case par case de l'intérieur 11×11 à la loi mesurée dans
`docs/provenance/fluides.md` — « une source sur un sol plat fait un losange dont
le niveau est la distance de Manhattan » :

```
  z= 35  ...76567...
  z= 36  ..7654567..
  z= 37  .765434567.
  z= 38  76543234567
  z= 39  65432123456
  z= 40  54321S12345
  z= 41  65432123456
  z= 42  76543234567
  z= 43  .765434567.
  z= 44  ..7654567..
  z= 45  ...76567...
```

**121 / 121.** Le losange est exact, le niveau est la distance de Manhattan, et
les quatre coins à distance 8 et plus restent secs — la coupure à 7 est là.

### Apparition — 20 mobs dans la boîte noire en 3 minutes

Parcelle « dark box » (x 0, z 192) : coque de pierre scellée, intérieur 13×13 à
y = −59, aucune lumière. Sonde garée en (8, −59, 168), à 26 blocs du bord — hors
du rayon de refus de 24 blocs, dans le rayon d'offre de 128, et à deux chunks,
donc dans les chunks tickés (un ticket joueur vaut le niveau 25, `kTicking` est
31).

55 paquets `Spawn Entity` en 180 s, dont **20 à l'intérieur de la boîte**. Les
types, relus dans `registries.json` :

| dans la boîte scellée | creeper, zombie, squelette, araignée, slime, sorcière, villageois-zombie, chauve-souris |
| dehors, sur l'herbe éclairée | vache, calmar luminescent |

C'est le comportement mesuré dans `docs/provenance/mobs.md` : le seuil de lumière
d'un monstre vaut **strictement 0**, et aucune vache n'apparaît dans le noir.
Le cap n'est **pas** mesuré ici : 3 minutes et un seul joueur ne suffisent pas à
atteindre le plateau de 70 relevé sur le vrai serveur, et le dire vaut mieux que
de présenter 20 comme une mesure de cap.

### Inventaire — la fenêtre 0, depuis un client protocole

`Set Creative Slot` met 64 pierres dans l'emplacement 36, puis deux
`Click Container` sur la **fenêtre 0**. Ce que le serveur renvoie :

```
t= 9036ms window=0 state=2 slots=46 carried=(1, 64)   non-empty: {}
t=11036ms window=0 state=3 slots=46 carried=None      non-empty: {9: (1, 64)}
```

La pile est prise sur le curseur, puis posée dans le sac. Avant ce câblage les
deux clics étaient jetés (`!player.window_open` était toujours vrai pour la
fenêtre 0, que le client ouvre seul et dont le serveur n'est jamais informé).

**Non vérifié : le client vanilla 1.20.1 non modifié.** Le mandat le demandait ;
cet agent n'a pas d'affichage. Le geste est joué au niveau du protocole, avec les
octets exacts qu'un client vanilla enverrait, mais ce n'est pas la même preuve.

---

## 3. Deux bugs trouvés *par* le câblage

Ni l'un ni l'autre n'aurait été trouvé par relecture.

### `sky_darken` : le temps du jour n'est pas la fraction du jour

Première version : fraction du jour → un cosinus → borné → ×11. Plausible, et
faux à toute heure sauf deux. Elle donnait **5 au tick 0**, là où le jeu donne 0
— un serveur qui vient de démarrer faisait donc apparaître des monstres sur de
l'herbe éclairée.

L'overworld passe son soleil par une courbe de mise en forme **avant** :

```
d0        = frac(temps / 24000 − 0,25)
d1        = 0,5 − cos(d0 × π) / 2
timeOfDay = (d0 × 2 + d1) / 3
darken    = (int) clamp(1 − (cos(timeOfDay × 2π) × 2 + 0,5), 0, 1) × 11
```

Vérifié aux trois heures qui épinglent la forme : 0 → 0 (lever), 6000 → 0 (midi),
18000 → 11 (minuit). Le crépuscule est bref : 0 à 12000, 11 à 14000.

### Le carré d'apparition n'est pas l'ensemble des chunks tickés

Premier câblage : passer à `spawn_tick` **tous** les chunks tickés. C'est faux, et
pas seulement lent. `effective_cap` vaut `cap × chunks / 289`, et 289 est 17×17 —
le carré autour d'**un** joueur. Lui donner tous les chunks tickés gonfle le
dénominateur : un monde avec beaucoup de chunks chargés autoriserait plusieurs
fois le cap réel. Le carré est maintenant parcouru depuis chaque joueur.

---

## 4. Ce que ça coûte au tick — mesuré, pas estimé

Même scénario partout : monde `ov_lab` (441 chunks), une sonde qui se connecte et
ne fait rien, 35 s. Attention au piège 22 du briefing — le `max` bat le p99.

### Debug (`macos-debug`, `-O0`)

| | p50 | p90 | p99 | max | > 50 ms |
|---|---|---|---|---|---|
| `main` | 4 µs | 26 µs | 165 ms | 203 ms | 36 / 817 |
| après, apparition **coupée** | 7 µs | 42 µs | 165 ms | 201 ms | 38 / 825 |
| après, tout branché | 2561 µs | 2961 µs | 192 ms | 554 ms | 49 / 776 |

Lecture : **tout sauf l'apparition coûte 3 µs au p50 et deux ticks hors budget.**
Le drain des files, les notifications, le four sans spectateur et l'attente du
chunk de spawn ne se voient pas. La totalité de l'écart est le
`NaturalSpawner` : ~2,55 ms par tick, soit 289 chunks × 3 tentatives = 867
positions candidates par tick, chacune avec ses lectures de bloc et de lumière à
travers des `std::function`, à `-O0`.

**La longue queue (p99 ≈ 165 ms, 36 ticks hors budget) est antérieure à ce
travail** : c'est le streaming de chunks à la connexion, 8 chunks par tick lus
depuis le disque et rallumés sur le thread de tick. `main` la montre à
l'identique.

### Release (`macos-release`)

| | p50 | p90 | p99 | max | > 50 ms |
|---|---|---|---|---|---|
| `main` | 1 µs | 5 µs | 13,4 ms | 21 ms | **0** / 930 |
| après | 379 µs | 1860 µs | 29,0 ms | 134 ms | **5** / 921 |

C'est le chiffre qui compte. L'apparition coûte ~0,38 ms par tick, soit 0,7 % du
budget de 50 ms. **Mais le régression est réelle et nommée : 5 ticks sur 921
dépassent 50 ms là où `main` n'en dépassait aucun, et le `max` passe de 21 ms à
134 ms.** Les cinq se produisent pendant la connexion, quand la première passe
d'apparition tombe sur le streaming de chunks.

Une correction appliquée en chemin, qui relève de la règle plutôt que de la
vitesse : `ChunkLight` porte trois `std::function` sur des lambdas capturantes,
donc le construire alloue. Il était construit **dans** le corps de tick — droit à
travers l'interdiction d'allouer là, celle que garde `NoAllocScope`. Il est
maintenant construit une fois.

---

## 5. Ce qui n'est pas fait, et qui doit être su

- **Aucun handler d'interaction de bloc.** Le mandat demandait de « pousser le
  levier » ; le serveur n'a aucun chemin pour cela. `Use Item On` ouvre un
  établi, ouvre un coffre, ou pose un bloc — un levier, un bouton, une porte, une
  trappe ne réagissent à rien. C'est la raison pour laquelle la preuve redstone
  passe par un `dig` sur de l'air plutôt que par le levier. **C'est le trou le
  plus visible qui reste** : la moitié des parcelles redstone du banc ne peut pas
  être actionnée par un joueur.
- **Une seule liste de mobs pour tout le monde.** `load_biome_spawners` lit un
  fichier de biome (`plains`) et l'applique partout, parce que le tableau de
  biomes du chunk n'est pas encore consulté à chaque tentative. Le biome utilisé
  est écrit dans le journal au démarrage, pour que ce qui tourne ne soit jamais
  une supposition.
- **Aucun dépeuplement.** `decide_despawn` existe dans `ov_gameplay` et n'est pas
  appelé : les mobs s'accumulent jusqu'au cap et y restent. Le cap tient, donc le
  monde ne déborde pas, mais un joueur qui s'éloigne laisse une foule derrière
  lui.
- **La file bloc ne sert qu'à la redstone.** Rien d'autre ne programme de tick de
  bloc aujourd'hui — ni la décomposition des feuilles, ni la croissance des
  cultures, ni le feu. Un nom que le registre ne connaît pas est **refusé et
  nommé** dans le journal, jamais traité comme un défaut.
- **L'index des fours est reconstruit une fois par seconde.** Un four posé
  commence donc à cuire au plus une seconde plus tard. Invisible devant une
  cuisson de 200 ticks, mais c'est une latence et elle est dite.
- **`Set Creative Slot` en survie : vérifié, rien à changer.** Le mandat
  demandait de regarder ce que fait le vrai serveur. Vanilla teste
  `player.gameMode.isCreative()` en tête de son handler et ignore le paquet
  sinon ; c'est déjà ce que fait `server.cpp`, avec le commentaire qui le dit. Le
  point 5 du mandat était un faux positif.
