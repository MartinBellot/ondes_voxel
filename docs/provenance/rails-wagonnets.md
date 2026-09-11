# Rails et wagonnets

Ce qui suit est établi par **mesure contre le vrai serveur 1.20.1**
(`tools/vanilla/server.jar`, SHA-1 `84194a2f286ef7c14ed7ce0090dba59902951553`) par
`scripts/measure_rails.py` (six scénarios), puis rejoué contre notre code par les
tests `[rails]` et `[minecart]` de `test_ov_gameplay`, et contre notre serveur par
`scripts/check_rails_e2e.py`. Les relevés bruts vont dans
`data/vanilla/1.20.1/normalized/rails_*.json` (gitignorés, régénérables).

Sources documentaires : articles « Rail », « Powered Rail » et « Minecart » du wiki
(minecraft.wiki, consultés le 2026-09-11) pour les **énoncés** — la règle sud-est,
la portée de 8 rails, le 0,06 du propulseur, la vitesse maximale. Chaque chiffre
retenu a ensuite été **mesuré** ; ceux qui ne l'ont pas été sont nommés comme tels.
Le protocole est celui de l'archive figée (oldid=2773082), corrigé par la capture là
où elle diffère (§ 5).

| | |
|---|---|
| `src/ov_gameplay/{include/ov/gameplay,src}/rails.{hpp,cpp}` | la forme, la puissance, le support, le détecteur |
| `src/ov_gameplay/{include/ov/gameplay,src}/minecart.{hpp,cpp}` | le pas physique d'un wagonnet, sur et hors rail ; les sept wagonnets |
| `src/ov_server/src/rails_session.{hpp,cpp}` | le branchement : pose, apparition, paquets, monter/descendre, casse, détecteur, activateur, sauvegarde `entities/` |
| `src/ov_server/src/world_ticks.{hpp,cpp}` | un cinquième moteur de blocs (`set_rails_extension`) |
| `scripts/measure_rails.py` | les six bancs vanilla |
| `scripts/check_rails_e2e.py` | la sonde contre notre serveur |

---

## 0. La méthode

- **Les formes** sont lues dans la sauvegarde (palette des sections), pas par
  `execute if block` : une campagne entière tient en un `save-all` et une lecture.
  Le lecteur est dans le script (il décode la palette lui-même), il ne dépend pas
  d'un binaire compilé.
- **Les trajectoires** reprennent l'horloge de `tnt-et-gravite.md` § 0 : un TNT
  témoin `NoGravity` à `Fuse:30000` est lu dans la **même commande** que les
  wagonnets ; un échantillon n'est retenu que si le témoin a la même mèche avant et
  après la lecture des wagonnets (même tick). 6 960 échantillons sur 319 ticks
  distincts de 0 à 319, quatorze pistes.
- Une sonde (client) reste connectée pendant la campagne des wagonnets (piège 11).

---

## 1. La forme d'un rail — 1 536 poses, 4 608 voisins, 288 cellules « occupées »

### Le banc exhaustif (`shapes`)

Un rail posé au centre de quatre cellules voisines ; chacune est vide, un rail au
même niveau, un rail un bloc **au-dessus** (sur un bloc de pierre) ou un bloc
**en dessous**. 4⁴ = 256 arrangements × 6 variantes : centre `rail` ou
`powered_rail`, posé `north_south` ou `east_west`, à côté de rails simples ; et un
centre `rail` à côté de rails propulseurs. Les voisins sont posés d'abord, chacun
seul, puis le centre par `/setblock` — qui fait bien tourner la mise en forme du
rail (1 536 centres relus, **0** qui ne soit pas le rail posé).

La règle qui reproduit **1 536 / 1 536 centres et 4 608 / 4 608 voisins** :

1. Un voisin compte s'il y a un rail dans cette direction au même niveau, un
   au-dessus ou un en dessous, **et** qu'il a encore une extrémité libre (ou qu'il
   pointe déjà vers nous).
2. Deux extrémités exactement : droit ou courbe.
3. Trois ou quatre (rail simple) : **la dernière ligne gagne** dans l'ordre
   nord-ouest, nord-est, sud-ouest, sud-est — d'où la « règle sud-est » du wiki,
   mesurée dans chaque arrangement.
4. Un rail droit seulement (propulseur, détecteur, activateur) à qui l'on offre les
   deux axes **garde l'axe qu'il avait** : 225 des 256 arrangements d'un
   propulseur posé `north_south` démentent « est-ouest gagne », aucun ne dément
   celle-ci. Pour un joueur, cet axe initial est celui où il regarde (wiki, non
   mesuré).
5. Puis un rail droit **monte** vers un voisin un bloc au-dessus ; si les deux
   bouts montent, **sud** bat nord et **ouest** bat est (le dernier test gagne).
6. Les voisins raccordés se tournent vers lui ; celui qui est un bloc plus bas
   **monte** vers lui.

⚠ **Piège payé :** un rail seul un bloc sous le nouveau « pointe » déjà vers lui
(son extrémité sud trouve le nouveau rail au-dessus). Le premier `connect_to`
s'arrêtait là et 468 voisins restaient plats. Un voisin est toujours recalculé,
en gardant son autre extrémité raccordée.

### Voisins déjà raccordés (`busy`)

L'est est une ligne pleine de trois rails, une demi-ligne (un rail au nord ou au
sud de lui) ou un rail seul ; l'ouest et le nord varient. Rejoué **dans l'ordre
des poses** (chaque `/setblock` met en forme) : **288 / 288** cellules. Un voisin
plein refuse la connexion ; une demi-ligne se courbe vers le nouveau rail.

### Le support

Un rail tient sur une face supérieure qui couvre l'**anneau extérieur de deux
seizièmes** (wiki : « a rim around the edge ») : bloc plein, verre, entonnoir,
dalle haute ; pas une dalle basse. Calculé sur les boîtes de collision (grille de
1/32). Un rail en pente tombe aussi quand le bloc sous son extrémité haute part.
Le rail cassé laisse son objet (et l'eau s'il était `waterlogged`).

### Non mesuré, nommé

- L'aiguillage par la redstone d'un rail en T : la préférence **inversée**
  (nord-ouest en premier) quand il est alimenté vient du wiki. Le déclencheur
  retenu — « un composant de redstone est adjacent » — est une approximation du
  jeu, qui regarde le bloc qui a changé.
- Un propulseur à qui l'on offre un coin (rail existant) : droit vers le nouveau.
- `waterlogged` à la pose par un joueur : le serveur pose le rail avant de le
  mettre en forme et ne sait plus qu'il y avait de l'eau. Nommé.

---

## 2. La puissance — 10 lignes

Lignes de 24 propulseurs, puis d'activateurs, allumées par un bloc de redstone posé
**en dernier** (piège 9) :

| montage | alimentés |
|---|---|
| source au bout | **9** (0…8) |
| source au milieu | **17** (4…20) |
| en pente, source en bas | **9** |
| en pente, source en haut | **9** |
| deux sources à 20 d'écart | 0…8 et 12…23 — **9, 10, 11 éteints** |

Identique pour les deux rails. La règle : un rail est alimenté par un signal
voisin, ou par un rail **du même bloc** au plus 8 rails plus loin sur sa ligne,
chaque rail intermédiaire étant lui-même `powered`, qui reçoit un signal. La
recherche suit la forme (elle monte et descend les pentes, et ne passe pas d'un
axe à l'autre). Retirer la source éteint tout (relu). Le test « one source powers
seventeen rails » fige le cas du milieu.

Le serveur propage le changement comme le jeu : un rail qui change de puissance
réveille les voisins du bloc **dessous** (et **dessus** sur une pente), ce qui
atteint le rail suivant d'une pente.

---

## 3. Le wagonnet — 14 pistes, au tick près

Pistes relues dans la sauvegarde et rejouées avec **les mêmes états de bloc** que
vanilla a écrits ; comparaison tick par tick de `Pos` et `Motion` :

| piste | échantillons | écart position | écart vitesse |
|---|---|---|---|
| droite, lancé à 1,0 | 319 | < 1e-9 | 1,1e-16 |
| droite, 0,1 | 319 | < 1e-9 | 1,4e-17 |
| pente, départ arrêté | 319 | < 1e-9 | < 1e-9 |
| montée puis retour | 319 | < 1e-9 | < 1e-9 |
| propulseurs alimentés | 319 | < 1e-9 | 1,9e-16 |
| freinage (propulseurs éteints) | 319 | < 1e-9 | < 1e-9 |
| lancement depuis un bloc | 319 | < 1e-9 | 2,8e-17 |
| avec passager, 0,3 et 0,1 | 319 ×2 | < 1e-9 | < 1e-9 |
| hors rail, au sol | 319 | < 1e-9 | 0 |
| hors rail, en chute | 319 | < 1e-9 | 0 |
| courbe, lente et rapide | 319 ×2 | < 1e-9 | < 1e-17 |
| wagonnet à fourneau | 319 | < 1e-9 | < 1e-9 |
| deux wagonnets qui se heurtent | — | **non rejoué** | |

Ce que les pistes ont fixé, chacune à la douzième décimale :

- Sur un rail plat le wagonnet est à **y + 0,0625** ; `Motion.y` vaut **0**
  (la gravité 0,04 est appliquée puis jetée).
- Pas **plafonné à 0,4 par axe** (2,5 → 2,9 → 3,3 lancé à 1,0), vitesse gardée
  (0,96) ; la vitesse elle-même est plafonnée à **2,0** à la projection (le
  fourneau plafonne à 2,496 = (2 × 0,8 + 1) × 0,96).
- Friction **0,96** à vide, **0,997** avec passager ; avec passager le pas vaut
  **0,75 × la vitesse** (2,5 → 2,725 à 0,3).
- Pente : **1/128** vers le bas avant le pas, puis la différence de hauteur rend
  **0,05** de vitesse par bloc après la friction (tick 1 : 0,0078125 × 0,96 +
  0,00039 = 0,007890625).
- Propulseur : **+0,06** dans le sens de marche **après** la friction ; arrêté,
  **0,02** en s'éloignant d'un conducteur à un bout (tick 1 : 0,02, puis 0,0792,
  0,136032).
- Hors rail : chaque axe plafonné à 0,4, **× 0,5 au sol**, puis **× 0,95 en
  l'air** (tick 1 au sol : 0,3 inchangé ; tick 2 : 0,15).
- Fourneau : `v × 0,8 + poussée`, puis × 0,96 ; pas de 0,2 ; un charbon = 3 600.

⚠ **Deux pièges payés par le rejeu :**

1. **Un déplacement de moins de √1e-7 n'a pas lieu.** Toutes les pistes gèlent la
   position alors que `Motion` lit encore quelques millionièmes ; sans ce seuil le
   rejeu dérivait de 0,0077 bloc en fin de piste, les vitesses restant justes à
   1e-16.
2. **En quittant le bas d'une pente, le bloc de hauteur prêté pour le pas est
   rendu.** Sans quoi le rail suivant, un cran plus bas, n'est jamais trouvé : la
   pente se séparait de vanilla au tick 11 (y −42 contre −42,97), la montée au
   tick 29 en redescendant.

### Non compris, nommé

- **Un wagonnet portant un porte-armure est 0,1F plus haut** (−49,8375 au lieu
  de −49,9375), dès le premier tick et pour toujours. Avec un **joueur** dessus
  (capture), il est à −60,9375 pour un rail à −61 : pas de décalage. Le rejeu
  l'ajoute pour les pistes à porte-armure ; notre serveur ne l'ajoute pas.
- La poussée d'un joueur (entrée avant) : `RailsSession::kRiderPush = 0,0274` par
  unité d'entrée, **ajusté sur une seule capture** (face à l'est, avant pendant
  deux secondes : 0,0657 bloc/tick à l'arrivée). La règle n'est pas comprise.
- Les collisions wagonnet-wagonnet et wagonnet-entité (pousser, ramasser un mob) :
  la piste a été enregistrée, **rien n'est implémenté**.

---

## 4. Le rail détecteur et le comparateur

- Un wagonnet arrêté dessus : `powered` et la lampe voisine allumée à chaque relevé.
- Après `kill` : encore alimenté 7 ticks, éteint au **8ᵉ** — une vérification
  périodique. Le serveur revérifie toutes les **20** ticks tant qu'un wagonnet est
  dessus (le délai mesuré tombe dans une période ; la période elle-même n'est pas
  mesurée).
- Comparateur derrière un détecteur portant un wagonnet à coffre :

  | piles de 64 | 0 | 1 | 2 | 5 | 13 | 26 | 27 |
  |---|---|---|---|---|---|---|---|
  | signal | 0 | 1 | 2 | 3 | 7 | 14 | 15 |

  Exactement `container_reading` sur 27 cases. Notre serveur lit les `Items` du
  wagonnet ; toute pile est comptée comme se rangeant par 64 (nommé).
- La présence est « le rail sous le centre du wagonnet », pas l'intersection des
  boîtes du jeu. Nommé.

---

## 5. Les paquets — capture

Un client sonde a vu apparaître chaque wagonnet et changer chaque champ, un champ
NBT à la fois contre un wagonnet témoin :

| | valeur |
|---|---|
| types (`Spawn Entity`) | minecart **64**, chest 14, furnace 40, tnt 102, hopper 48, spawner 93, command block 17 |
| index 8 / 9 / 10 | secousse : durée (VarInt), sens (VarInt), dégâts (Float) — vus sur un wagonnet heurté |
| index 11 | bloc affiché (VarInt, id d'état : 1 pour la pierre) |
| index 12 | décalage du bloc (VarInt ; `DisplayOffset:3` → 3) |
| index 13 | bloc personnalisé (Boolean) — `DisplayOffset` seul n'envoie **rien** |
| index 14 | fourneau : a du combustible (Boolean) ; commande : la commande (String) |
| `Set Passengers` | **0x59**, trouvé par son contenu (id du wagonnet, 1, id de la sonde) |
| `Player Input` | **0x1F** : le drapeau 0x02 envoyé par la sonde l'a fait descendre (`Set Passengers` vide) |

---

## 6. Ce que fait notre serveur

- **Pose d'un rail** par un joueur : mise en forme au tick suivant, par la même
  règle ; puissance, support, aiguillage, détecteur par le drain des ticks.
- **Wagonnet** : l'objet utilisé sur un rail en pose un (hors rail : rien) ; les
  sept types apparaissent aussi par `/summon`. Paquets et métadonnées ci-dessus ;
  les déplacements passent par la diffusion commune des entités.
- **Monter** (clic droit sur un wagonnet simple), **pousser** (Player Input avant),
  **descendre** (0x02, ou un rail activateur alimenté). Le joueur est reposé à la
  position du wagonnet : la recherche de place du jeu n'est pas faite.
- **Casser** : un joueur en créatif l'enlève sans rien laisser ; en survie dix
  points par coup, cassé au-delà de 40, l'objet du wagonnet et le contenu d'un
  wagonnet à coffre ou entonnoir au sol. L'arme n'est pas lue : chaque coup vaut un
  poing.
- **Fourneau** : charbon ou charbon de bois au clic droit, +3 600, poussée opposée
  au joueur. **TNT** : amorcé par un activateur alimenté (80 ticks), l'explosion
  passe par le drain des TNT, puissance `4 + aléa × 1,5 × min(5, vitesse)` (wiki,
  non mesurée). **Entonnoir** : désactivé par un activateur alimenté.
- **Sauvegarde** : `entities/r.x.z.mca`, `DataVersion` 3465, les wagonnets avec
  `Pos`, `Motion`, `Rotation`, `UUID`, `Fuel`/`PushX`/`PushZ`, `TNTFuse`,
  `Enabled`, `Items`, `CustomDisplayTile`/`DisplayState`/`DisplayOffset`, et toute
  clé qu'on ne modélise pas relue telle quelle. **Les autres entités d'un chunk
  vanilla** (un cochon…) sont réécrites telles qu'elles étaient lues ; un chunk où
  l'on a écrit un wagonnet est réécrit à chaque sauvegarde, pour qu'un wagonnet
  parti ne reste pas derrière (testé).

### Ce qui manque, nommé

- **Notre client ne dessine pas les wagonnets** : la géométrie d'entité vient des
  modèles Bedrock (`rendu-entites.md`) et `entity_models.json` n'a pas de
  wagonnet ; ni siège, ni caméra embarquée.
- Aucune fenêtre pour le wagonnet à coffre ou à entonnoir ; l'entonnoir n'aspire
  rien. Le wagonnet à commande n'exécute rien ; le wagonnet à spawner ne fait
  rien apparaître (les deux apparaissent et se sauvegardent).
- Le distributeur ne pose pas de wagonnet ; un wagonnet TNT n'explose ni en tombant
  ni au feu ; aucun son de wagonnet.
- Les wagonnets ne vivent que dans l'Overworld.
- Le rejeu en rails de la vitesse dans l'eau (×0,2 sur la pente, cap 0,2) n'est
  ni mesuré ni écrit.

---

## 7. De bout en bout, contre notre serveur

`scripts/check_rails_e2e.py` fait tourner `ov_dedicated` (port 25625) et un client
sonde qui agit comme un joueur en créatif — `Set Creative Mode Slot`,
`Use Item On`, `Interact`, `Player Input`. Rien n'est appelé à côté du protocole.

| étape | ce que la sonde a lu |
|---|---|
| 14 rails posés sur l'herbe, un détecteur au milieu | les **12** rails du milieu reviennent `east_west` ; un rail posé à côté du bout le courbe en **`south_west`** |
| un wagonnet posé sur le rail x = 2 | `Spawn Entity` type **64**, à **y = −59,9375** (rail à −60 + 1/16) |
| clic droit dessus | `Set Passengers` **0x59** : le wagonnet, 1, la sonde |
| `Player Input` avant, face à l'est, 6 s | **7,67 blocs** vers l'est dans les paquets de position |
| le détecteur sous le wagonnet | `powered=true` puis `false` ; la lampe voisine `lit=true` puis `false` |
| `Player Input` drapeau 0x02 | `Set Passengers` vide |
| arrêt du serveur | `entities/r.0.0.mca` écrit |
| redémarrage | le wagonnet revient, à x = 11,58 où il s'était arrêté |
| un coup en créatif | `Remove Entities` |

⚠ **La première version de la sonde « échouait » sur le détecteur** : elle ne
gardait que la dernière mise à jour de chaque bloc, et le détecteur comme la lampe
s'éteignent une fois le wagonnet passé. Elle garde maintenant tout l'historique.

⚠ **Un défaut antérieur du serveur, trouvé ici : des Block Update perdues.**
Lors d'une exécution (sur trois), deux rails du milieu sont revenus `north_south`
côté client alors que le monde les tenait `east_west`. `flush_tick_writes`
vidait sa file **même quand** son `try_lock` sur `players_mutex` perdait contre
le fil réseau : toute écriture faite par le tick pendant ce tick-là — une forme
de rail, mais aussi un fil de redstone, un cratère — n'était jamais envoyée. La
file n'est plus vidée que lorsqu'elle est envoyée.

### Relu par vanilla

`OV_RAILS_KEEP=1 python3 scripts/check_rails_e2e.py` garde le monde que notre
serveur a sauvegardé après l'étape 8 ; `measure_rails.py readback` l'ouvre avec le
vrai serveur, sous le verrou, et lit les wagonnets : **1 wagonnet**, à
(12,61 ; −59,9375 ; 4,5), avec sa `Motion` et son `UUID`. Dans l'autre sens, le
test `test_rails_session` relit un chunk écrit à la manière de vanilla (un cochon
et un wagonnet à fourneau) et le réécrit avec le cochon intact.

---

## 8. Reproduire

```bash
lockf /tmp/ov-vanilla.lock python3 scripts/measure_rails.py shapes    # 1536 poses
lockf /tmp/ov-vanilla.lock python3 scripts/measure_rails.py busy      # voisins raccordés
lockf /tmp/ov-vanilla.lock python3 scripts/measure_rails.py power     # 10 lignes
lockf /tmp/ov-vanilla.lock python3 scripts/measure_rails.py carts     # 14 pistes
lockf /tmp/ov-vanilla.lock python3 scripts/measure_rails.py detector  # détecteur, comparateur
lockf /tmp/ov-vanilla.lock python3 scripts/measure_rails.py capture   # paquets
build/macos-debug/bin/test_ov_gameplay '[rails]'
build/macos-debug/bin/test_ov_gameplay '[minecart]'
python3 scripts/check_rails_e2e.py
```

Chaque scénario vanilla démarre son serveur sur le port 25623 (`OV_RAILS_PORT`) dans
`run/rails-oracle/<scénario>/` et **efface son monde** en partant.
