# Les conteneurs : l'inventaire de block entity, et les machines qui s'en servent

## Le nœud

Trois jeux de règles finis et injoignables dormaient dans `ov_gameplay` :

- `HopperRules`, neuf tests verts et une recharge **mesurée à 8 ticks** ;
- `Dispenser`, une table de dix-huit objets lue sur un vrai serveur ;
- les drapeaux `enabled` et `triggered`, pilotés correctement par le moteur de redstone
  depuis des semaines, avec **rien** au bout.

Ce qui manquait n'était aucune de ces règles. C'était l'objet qu'on leur passe : le serveur
connaissait un seul conteneur, le coffre, et le connaissait **trois fois** — un écran de 27
cases, un gestionnaire de clic où le nombre 27 apparaît cinq fois, et une lecture de
comparateur. Un tonneau n'ouvrait rien, un entonnoir ne pouvait pas être ouvert du tout, et
`gameplay::HopperRules` n'avait aucun conteneur à recevoir.

`src/ov_server/src/block_container.{hpp,cpp}` décrit un conteneur **une fois** : le bloc,
le type de block entity, le nombre de cases, le menu, le titre, et quelles faces admettent
quelles cases. `item_transport.{hpp,cpp}` est la passe qui fait tourner les machines.

---

## 1. L'accès par face d'un four — mesuré, pas récité

`python3 scripts/measure_containers.py furnace-faces`, contre
`tools/vanilla/server.jar` (SHA-1 `84194a2f286ef7c14ed7ce0090dba59902951553`).

Le montage : un four, **un** entonnoir tourné vers lui, un objet dans l'entonnoir, et on lit
dans quelle case du four il atterrit. Une cellule par face, et **deux objets** — c'est le
second qui porte tout le résultat.

| face de l'entonnoir | `minecraft:coal` | `minecraft:iron_ore` |
|---|---|---|
| au-dessus (face `up`) | case **0** (entrée) | case **0** (entrée) |
| à côté (face latérale) | case **1** (combustible) | **refusé** |
| en dessous (face `down`) | **refusé** | **refusé** |

Et la sortie, un entonnoir sous un four dont la case 2 tient trois lingots et la case 1 deux
charbons :

| lu dans l'entonnoir du dessous | valeur |
|---|---|
| lingots de fer tirés | **3** |
| charbons tirés | **0** |
| charbons restés dans la case combustible | **2** |

> **Une seule ligne de ce tableau vaut la campagne entière.** Avec le charbon seul, la règle
> se lit « le côté prend tout » — et une batterie de fours alimentée par entonnoir bourre sa
> propre ligne de combustible avec du minerai, se bloque, et rien ne dit pourquoi. Le
> minerai refusé par la même face et le même entonnoir est ce qui nomme la vraie règle : le
> côté admet la case combustible **et seulement ce qui brûle**.

`ContainerBridge` a donc besoin du `RecipeBook` pour répondre, et **refuse** la case
combustible quand il ne l'a pas. Refuser un combustible légal se voit et se corrige ;
accepter du pavé comme combustible ne se voit pas.

---

## 2. Le distributeur n'est pas un dropper — mesuré

`python3 scripts/measure_containers.py into-container`. Un distributeur et un dropper, la
même pile de cinq pavés, chacun face à un coffre, vingt-quatre blocs d'écart, déclenchés par
un levier posé **en dernier** et **adjacent** (§1 de `redstone.md` : `/setblock` ne prévient
que six voisins).

| machine | dans le coffre | resté dans la machine | par terre |
|---|---|---|---|
| distributeur | **0** | 4 | **oui** |
| dropper | **1** | 4 | non |

⚠ **`src/ov_gameplay/include/ov/gameplay/dispenser.hpp` dit le contraire** dans le
commentaire de `DispenseKind::IntoContainer` : « ce que font **les deux** machines quand
elles font face à un coffre ou un entonnoir ». C'est faux, et ça n'avait pas été mesuré — la
campagne §13 de `redstone.md` mesurait ce qui **sort**, pas où ça va. Le fichier n'était pas
dans le périmètre de ce mandat ; le commentaire reste à corriger.

`item_transport.cpp` implémente le tableau : seul le dropper insère.

---

## 3. La shulker box refuse la shulker box — mesuré

`python3 scripts/measure_containers.py shulker`. Un entonnoir au-dessus d'une shulker box,
une shulker box rouge en case 0 et un pavé en case 1.

| lu | valeur |
|---|---|
| shulker box entrée dans la boîte | **0** |
| pavé entré dans la boîte | **1** |
| shulker box restée dans l'entonnoir | **1** |

La récursion est coupée par le conteneur, pas par l'entonnoir : l'entonnoir a bien essayé,
et il est passé à la case suivante.

---

## 4. Chez nous : ce que la sonde de bout en bout mesure

`python3 scripts/check_containers_e2e.py`, sur une copie du banc (`tools/ov_lab`), avec un
client qui parle le protocole 763 exact et n'a pas d'écran.

| ce qui est mesuré | résultat | oracle |
|---|---|---|
| taille des fenêtres | coffre 27, tonneau 27, shulker 27, entonnoir 5, distributeur 9 | le jeu |
| **cadence d'un entonnoir** | **8 ticks/objet**, 23 intervalles sur 28 valent exactement 8, médiane 8 | **8,07** mesuré sur le vrai serveur (§12 de `redstone.md`) |
| chaîne de cinq entonnoirs | 36 objets livrés au coffre de queue | — |
| **verrou par signal** | **0 objet** en 189 ticks serveur | **0 en 242 ticks** |
| distributeur sur front montant | 1 objet éjecté, une fois | le jeu |
| dropper sur front montant | 1 objet éjecté, une fois | le jeu |
| remplissage du coffre | 0, 1, 5, 14 et 27 piles exactes | — |
| sauvegarde / rechargement | tonneau 17, shulker 5, entonnoir 3 | — |

Le comparateur sur conteneur est vérifié en test unitaire sur **28 niveaux de remplissage**
(`test_ov_server`, `[container][comparator]`) : `BlockInventory::comparator_reading()` rend
`floor(14n/27)+1` pour les 27 remplissages non vides et 0 pour le vide, en passant par le
vrai inventaire — la plénitude d'une pile pleine sur 27 cases est 1/27 du conteneur, pas
1/27 d'une case, et c'est l'erreur que ce test attrape.

---

## 5. Le round-trip croisé : le vrai jeu ouvre notre sauvegarde

Le format n'est pas prouvé par notre lecteur qui relit notre écrivain. Il est prouvé par le
vrai serveur 1.20.1 lancé sur une copie de notre monde.

Déposé chez nous, puis lu **par le vrai serveur** avec `data get block` :

```
43, -60, 164   {Items: [{Slot: 0b, id: "minecraft:cobblestone", Count: 17b}], id: "minecraft:barrel"}
45, -60, 164   {Items: [{Slot: 0b, id: "minecraft:diamond",     Count:  5b}], id: "minecraft:shulker_box"}
34, -60, 168   {TransferCooldown: 0, Items: [{Slot: 0b, id: "minecraft:stick", Count: 3b}], id: "minecraft:hopper"}
226, -58, 7    {Items: [{Slot: 0b, …, Count: 64b}, {Slot: 1b, …, Count: 6b}], id: "minecraft:chest"}
```

Aucune exception, aucune erreur. Il a ensuite **réécrit** les régions, et nos six conteneurs
relus depuis sa réécriture portent exactement les mêmes objets. `TransferCooldown` est lu et
compris par le vrai jeu : c'est le nom de vanilla, pas le nôtre, et un entonnoir rechargé
sans lui bougerait un objet sur le tick où il se charge.

---

## 6. Les pièges payés ici

**1. `Chunk::set_block` jette la block entity à chaque écriture — et c'est ce qui vide un
conteneur.** Le commentaire du fichier a raison pour un bloc *remplacé* : une block entity
qui survit à son bloc est un coffre qu'on ne peut ni ouvrir ni enlever. Mais un entonnoir
qui passe `enabled=false`, un four qui passe `lit=true`, un distributeur qui passe
`triggered=true` **ne sont pas des blocs neufs**, et l'écriture les vide. Le symptôme n'est
pas une erreur : l'entonnoir est toujours un entonnoir, avec son orientation, il refuse
simplement de s'ouvrir, et ses objets ont disparu de la sauvegarde. **La passe des fours
avait le même trou** : un four perdait son minerai, son combustible et son expérience sur le
tick où il s'allumait. `write_block` dans `server.cpp` garde la block entity quand l'id du
bloc ne change pas — ce que fait `Level.setBlock` de vanilla — et les trois chemins
d'écriture du serveur y passent.

**2. L'âge du monde du paquet `Update Time` n'est pas un compteur de ticks.** Mesuré sur
notre serveur : **21,3 Hz** sur 30 secondes. Une cadence divisée par lui est fausse de 6 %,
ce qui a donné 9,11 ticks/objet pour un entonnoir qui transfère très exactement toutes les 8
ticks.

**3. Et `clock.tick_count()` non plus, dès que la machine est chargée.** Il avance avec le
temps mural pendant que le serveur saute des ticks (piège n° 22 du briefing). Une cadence
lue comme « objets arrivés ÷ ticks écoulés » mesure la charge de la machine. Ce qui se
mesure, c'est l'**écart entre deux transferts du même entonnoir**, et il faut donc **un
seul** entonnoir chargé : la ligne de journal est écrite une fois par tick pour toute la
passe, pas une par entonnoir, et cinq entonnoirs déphasés la font paraître à presque chaque
tick.

**4. En Python, `v << 52 >> 52` n'étend pas le signe** — les entiers y sont de précision
arbitraire, donc l'expression rend `v` inchangé. Une `Position` du protocole décodée comme
ça donne un `y` à dix-huit chiffres ; la sonde ne retrouve jamais le bloc qu'elle vient de
faire poser et conclut que la pose a échoué **alors qu'elle a parfaitement réussi**. Une
heure perdue à chercher un bug de serveur qui n'existait pas.

**5. Le paquet serverbound *Player Command* est `0x1E` en 763**, pas `0x1F`. Sans lui, le
joueur ne s'accroupit jamais, et **rien ne peut être posé sur un conteneur** : chaque clic
l'ouvre. `player.sneaking` était suivi et inutilisé dans le chemin d'ouverture ; il est
maintenant respecté, comme dans le vrai jeu.

**6. Les deux montages de comparateur du banc sont à l'envers.** Dans
`tools/ov_lab/src/plots.cpp`, `redstone_comparators` pose le bloc de redstone (ou le
conteneur) du côté `facing` du comparateur — c'est-à-dire du côté de sa **sortie** — et le
fil et la lampe du côté de son entrée. `facing` d'un comparateur est la direction où il
émet. C'est pour ça qu'aucune mesure de comparateur sur conteneur n'a pu être faite sur le
banc ; le fichier n'est pas dans le périmètre de ce mandat.

**7. Un serveur vanilla resté vivant sur son port fait mourir la campagne suivante avec une
`NullPointerException` qui ne parle pas de port** (`Cannot invoke "aif.w_()" because "$$5"
is null`, pendant l'arrêt). Deux campagnes perdues. `pkill -f "java.*server.jar"` avant de
relancer.

---

## 7. Ce qui n'est pas fait, nommé

- **Les comportements de distributeur autres que l'éjection** — seau, briquet, TNT, œuf
  d'apparition, projectile — sont **nommés et refusés**, jamais éjectés par défaut.
  `ItemTransport::unsupported()` les liste, `TransportStats::unsupported` les compte, et
  l'objet reste dans la machine. Un distributeur qui lâcherait un seau d'eau par terre au
  lieu de poser de l'eau est indiscernable d'un distributeur qui marche.
- **Le dropper face à un conteneur y insère** (mesuré, §2), et c'est implémenté ; la
  parcelle du banc n'a pas de conteneur devant ses machines, donc la sonde de bout en bout
  n'exerce que l'éjection.
- **Le distributeur tire la première case pleine, pas une au hasard.** Vanilla tire au sort
  parmi les cases non vides ; reproduire le tirage demande le flux RNG de la machine, que ce
  projet ne porte pas encore sur une block entity. Déjà dit dans `dispenser.hpp`.
- **La case combustible n'est pas atteignable par le dessous**, même pour reprendre un seau
  vide. Vanilla le permet ; ce serveur ne produit pas encore de seau vide dans un four, donc
  la face est refusée plutôt qu'à moitié modélisée.
- **Conteneurs non modélisés**, nommés dans `unmodelled_containers()` : alambic, coffre de
  l'Ender, lutrin, étagère ciselée, juke-box. Le coffre de l'Ender est là parce que ce n'est
  pas un inventaire de bloc du tout — son contenu appartient au joueur qui l'ouvre, et le
  modéliser comme 27 cases sur le bloc donnerait le même coffre à tout le monde.
- **Le comparateur derrière un conteneur n'a pas de mesure de bout en bout** — voir le piège
  n° 6. La règle est vérifiée en test unitaire sur 28 remplissages, et le crochet
  `container_signal` du serveur lit désormais **tout** conteneur au lieu du seul coffre.
- **Le double coffre est un simple coffre de 27 cases.** Les moitiés `left` et `right` du
  banc s'ouvrent chacune sur son propre inventaire, pas sur les 54 cases partagées.

---

## Reproduire

```bash
pkill -f "java.*server.jar"                       # un serveur resté sur son port tue la campagne
python3 scripts/measure_containers.py all         # l'oracle vanilla : faces, machines, shulker
python3 scripts/check_containers_e2e.py           # notre serveur, de bout en bout
```
