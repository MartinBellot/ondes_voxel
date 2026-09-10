# TNT, creeper et blocs soumis à la gravité

Ce qui suit est établi par **mesure contre le vrai serveur 1.20.1**
(`tools/vanilla/server.jar`, SHA-1 `84194a2f286ef7c14ed7ce0090dba59902951553`),
par `scripts/measure_tnt_gravity.py` (sept scénarios), puis rejoué **contre notre
propre serveur** par `scripts/check_tnt_e2e.py`. Les relevés bruts vont dans
`data/vanilla/1.20.1/normalized/tnt_*.json` (gitignorés, régénérables).

Le point de départ est la dernière ligne de `explosions.md` § 6 : « Rien n'est
branché dans le serveur. Une TNT amorcée ne saute toujours pas, un creeper
toujours ne siffle pas. » Les rayons, la résistance des 987 blocs et la formule
des dégâts étaient mesurés ; il manquait l'entité qui porte la charge, la loi de
la réaction en chaîne, les blocs qui tombent, et les paquets qu'un client reçoit
de tout cela.

Le code :

| | |
|---|---|
| `src/ov_gameplay/{include/ov/gameplay,src}/falling_block.{hpp,cpp}` | les blocs qui tombent, l'entité `falling_block`, le pas physique commun |
| `src/ov_gameplay/{include/ov/gameplay,src}/primed_tnt.{hpp,cpp}` | l'entité `tnt`, l'allumage, le creeper, l'explosion en deux phases |
| `src/ov_gameplay/src/explosion.cpp` | `collect_cells` : la même tirée, avec les cellules d'air |
| `src/ov_protocol/{include/ov/protocol,src}/blast.{hpp,cpp}` | le paquet Explosion, les indices de métadonnées |
| `src/ov_server/src/tnt_gravity.{hpp,cpp}` | le branchement : entités, joueurs, objets au sol, écritures |
| `src/ov_server/src/world_ticks.{hpp,cpp}` | un point d'extension : un troisième moteur de blocs |

---

## 0. La méthode : l'horloge est dans l'entité

Une console répond quand elle veut, et un relevé « par tick » lu depuis la
console mélange des ticks. Ce mandat ne lit donc jamais un tick sur l'horloge
de la console : il le lit **dans l'entité elle-même**.

- Une TNT amorcée porte `Fuse`, qui descend d'exactement un par tick. Lue dans
  la **même commande** que `Motion` et `Pos` (`execute as @e[type=tnt] run data
  get entity @s`), elle dit combien de ticks de mouvement l'échantillon a subis,
  à l'unité près, quelle que soit la latence de la console.
- Un bloc qui tombe porte `Time`, qui monte d'un par tick. Même usage.

C'est ce qui permet de comparer les trajectoires **à douze décimales** plutôt
que d'ajuster une courbe.

⚠ **`gametime − Time` est en retard d'un tick, par construction.** La console
exécute ses commandes avant que le niveau n'incrémente son horloge ; une entité
née au tick G+2 et lue au début du tick G' a été tickée `G' − G − 1` fois. Les
naissances relevées « 1, 3, 5… ticks après le retrait du support » sont donc
**2, 4, 6…** en temps de jeu. Le banc l'écrit tel quel et le test le traduit
une fois, au même endroit (`test_falling_block.cpp`).

---

## 1. La TNT amorcée

### Le lancer — capturé

Une TNT allumée par un bloc de redstone posé à côté a été annoncée au client
sonde avec une vitesse de **(104, 1600, 120)** en 1/8000ᵉ de bloc par tick :
**0,2 vers le haut et 0,02 à l'horizontale**, dans une direction tirée au sort.
Le fil **tronque** chaque composante à l'unité : (104, 120) est à 1,5·10⁻⁴ sous
0,02, et ne peut pas l'être davantage — le test le borne des deux côtés.

La composante verticale est le **flottant** `0.2F` élargi : la `Motion` un tick
plus tard vaut `0.1568000029206276`, soit `(0.2F − 0.04) × 0.98`. Avec le double
0,2 elle vaudrait `0.1568` exactement. Le dernier chiffre est dans la mesure.

La loi de la direction n'est **pas mesurée** : une capture ne dit pas qu'elle est
uniforme. Le code tire un angle uniforme, et c'est écrit dans l'en-tête.

### Le vol — 3 950 échantillons

Dix TNT amorcées à y = 100, cinq amorçages, chaque échantillon étiqueté par son
propre `Fuse`. Le modèle — gravité 0,04 avant le déplacement, puis ×0,98 sur les
trois axes après — prédit :

| | écart maximal sur 3 950 échantillons |
|---|---|
| vitesse verticale | **2,9·10⁻⁹** (le flottant 0,2F) |
| vitesse horizontale `0,02 × 0,98ⁿ` | **1,0·10⁻¹⁷** |
| position verticale | **1,2·10⁻⁷** |

La traînée horizontale est **0,98, pas 0,91** comme pour un mob, et l'ordre est
« déplacement puis traînée ». Ce n'est donc pas `step_entity` qui fait avancer
une TNT mais `step_block_entity`, écrit pour elle et pour les blocs qui tombent.

### Au sol — le saut, puis la glissade

La même TNT posée sur de la pierre ne reste pas au sol : elle **saute**.

| n | y | vy | ‖h‖ | au sol |
|---|---|---|---|---|
| 1 | −59,84 | 0,1568000029 | 0,0196 | non |
| 4 | −59,6158 | 0,0323152283 | 0,01844736 | non |
| 8 | −59,8824 | −0,1223518687 | 0,01701526 | non |
| **9** | **−60,0** | **0,0** | **0,01167247** | **oui** |
| 10 | −60,0 | 0,0 | 0,00800731 | oui |
| 14 | −60,0 | 0,0 | 0,00177330 | oui |

Trois faits en sortent, et chacun a coûté quelque chose à `step_block_entity` :

- **Au sol, l'horizontale est multipliée par 0,7 en plus du 0,98** :
  0,01701526 → 0,01167247, rapport 0,686 = 0,98 × 0,7.
- **La verticale au repos est exactement 0**, pas un rebond de +0,0196 : une
  collision verticale arrête l'axe avant la traînée.
- **Aucun seuil à 0,003.** À n = 14 l'horizontale vaut 0,00177 et descend
  encore ; le seuil des mobs l'aurait remise à zéro onze ticks plus tôt.

Les quatorze lignes sont figées dans `test_primed_tnt.cpp` et notre pas les
reproduit à 10⁻⁹ près sur la position et 10⁻¹² sur la vitesse.

### La mèche

**80 ticks**, mesurée par `redstone.md` § 14 et retrouvée ici : le plus grand
`Fuse` lu sur 7 900 échantillons est 79, un tick après l'amorçage. L'ordre est
« se déplacer, puis décompter » : l'échantillon lu à `Fuse = 79` a subi
exactement un tick de mouvement. La charge part au tick où le compte atteint 0,
de `y + 0,98F × 0,0625` — capturé à `−59.93874999880791` pour une TNT posée à −60.

### La réaction en chaîne — 384 mèches

Une charge (`Fuse:1`) au centre d'un anneau de 48 blocs de TNT, huit fois ; un
témoin (`Fuse:400`) invoqué dans la même commande compte les ticks écoulés.
**48 sur 48** allumés à chaque essai, et les 384 mèches initiales tombent **toutes
entre 10 et 29, les vingt valeurs présentes** :

```
10:17 11:20 12:26 13:14 14:16 15:24 16:30 17:15 18:20 19:21
20:17 21:16 22:25 23:19 24:20 25:15 26:19 27:15 28:19 29:16
```

C'est `fuse/8 + aléa(fuse/4)` pour une mèche de 80, ce qu'`explosions.md`
avait écrit d'après la documentation sans le mesurer. C'est mesuré.

### Allumer une TNT

| source | état |
|---|---|
| briquet ou boule de feu sur le bloc | **branché** (`ItemUse` le décidait déjà, le serveur l'engendre maintenant) |
| redstone | **branché** : tout voisin qui envoie un signal, la question de la lampe (`tnt_lit_by_signal`) |
| explosion voisine | **branché**, mèche 10–29 |
| feu qui se propage | **non** : ce serveur n'a pas de propagation du feu. Nommé |
| distributeur | **non** : `DispenseKind::PrimeTnt` reste refusé et nommé par `ItemTransport` |
| TNT `unstable=true` cassée | **non**, nommé |

⚠ La redstone **à la pose** passe par la notification de voisinage existante :
un joueur qui pose une TNT contre un bloc de redstone l'allume, comme vanilla.
Mais un monde chargé depuis le disque avec une TNT déjà contre une source n'est
pas réévalué — vanilla non plus (piège 12 du briefing).

---

## 2. L'explosion, branchée

Vanilla fait une explosion en **deux temps**, et l'ordre compte :

1. les rayons décident des cellules — rien n'est écrit ;
2. les entités sont blessées et repoussées, **pendant que les murs tiennent
   encore** : l'exposition se calcule contre un mur qui va disparaître ;
3. le butin est tiré, les TNT allumées, puis les cellules vidées.

`collect_detonation` et `destroy_detonation` sont ces deux phases ; le serveur
fait les entités entre les deux. Tous les états sont lus **avant** la première
écriture, pour qu'une plante de deux blocs dont l'autre moitié est aussi dans le
cratère tire encore sa propre table.

Les cellules sont vidées **à travers le `ServerLevel`** — le même `LevelWriter`
que les fluides et la redstone — puis `WorldTicks::settle_changes` notifie autour
de chaque cellule écrite. C'est ce qui fait couler l'eau dans un cratère et
tomber le sable qui le surplombait.

### Le butin, par source

| source | règle | mesure |
|---|---|---|
| TNT | tout ce qu'elle casse | **1754 / 1754** (`explosions.md` § 5) |
| creeper | un bloc sur `puissance` | **333 / 989 = 0,337** sur douze tirs (attendu 1/3) |

Le butin vient de la **table du bloc, main vide** — c'est pourquoi une charge dans
la pierre rend des pavés. `LootTables::drops` n'applique pas l'outil requis, ce
qui est la règle d'une explosion et pas une omission. Les conditions
`survives_explosion` et `explosion_decay` sont compilées « hors explosion » par
`tools/ov_datagen/loot.py` ; la décroissance du creeper est donc le tirage
`1/puissance` d'`Explosions::rolls_drop`, fait **par bloc**. Pour une table qui
porte `explosion_decay` sur le **nombre** d'objets (les minerais), c'est une
approximation, et elle est nommée.

### Les entités

- **Les entités du monde** (mobs, TNT, blocs qui tombent) : `hit_entity`, le
  recul ajouté à leur vitesse et diffusé ; un mob est blessé à travers
  `MobCombat`, avec un `Damage Event` de type `minecraft:explosion`. Une TNT et
  un bloc qui tombe sont poussés et jamais blessés. Un mob tué laisse son butin
  de mob, sans le « tué par un joueur ».
- **Les joueurs** : chacun reçoit **son** paquet Explosion, avec **son** recul.
  Un joueur en créatif est poussé et pas blessé ; un joueur en survie est blessé
  par `SurvivalSession::hurt(Explosion)`. Le rayon d'audience de 64 blocs vient
  de l'article du wiki et **n'est pas mesuré**. Un joueur créatif **qui vole**
  n'est pas poussé en vanilla ; ce serveur ne sait pas s'il vole, et le pousse.
- **Les objets au sol** disparaissent quand le coup vaut 5 ou plus (la santé
  d'un objet, d'après le wiki, non mesurée). Ils ne sont pas projetés : les
  objets de ce serveur n'ont pas de physique.
- **Le contenu d'un conteneur détruit** est perdu : vanilla le répand. Nommé.

---

## 3. Le paquet Explosion — deux corrections à l'archive

`scripts/measure_tnt_gravity.py capture` : un client sonde rejoint le jar, la
console fait exploser une TNT à côté de lui, et le paquet est trouvé **par son
contenu** — le premier dont les trois doubles de tête sont le centre de la
charge — plutôt que par un identifiant.

1. **L'identifiant est 0x1D.** L'archive figée du wiki (oldid=2773082) donne
   0x1E. `entity.hpp` le notait déjà : l'archive s'est trompée sur chaque
   identifiant que ce dépôt a vérifié.
2. **La liste porte les cellules d'air.** Une charge sur le sol du superflat
   envoie **676 enregistrements en moyenne** (32 charges, écart-type 11) en
   cassant moins d'une centaine de blocs : c'est toute cellule qu'un rayon a
   atteinte avec de l'énergie, pas toute cellule détruite. D'où
   `Explosions::collect_cells`, qui rend les deux listes d'un même tirage — la
   liste des blocs reste exactement celle de `collect_blocks`, et le test de
   parité du cratère d'`explosions.md` (541/555) n'a pas bougé.

   ⚠ Le premier paquet capturé en portait **827**, et ce chiffre ne se compare à
   rien : la charge est partie à côté du cratère qu'avait fait, une seconde plus
   tôt, la TNT sans `Fuse` de la ligne de base (piège 3 ci-dessous). Plus d'air
   autour, plus d'enregistrements. Le banc `records` a été écrit pour ça.

Le reste est conforme à l'archive : x, y, z en doubles, le rayon en flottant, un
VarInt, trois octets signés par enregistrement **relatifs au plancher du
centre**, puis trois flottants de recul. Les 12 octets de queue ont été comptés
sur la capture. `test_blast_packets.cpp` fige l'en-tête octet pour octet et fait
l'aller-retour.

### Les métadonnées — un champ à la fois

| entité | index | type | mesuré par |
|---|---|---|---|
| TNT : mèche | **8** | VarInt | `summon tnt {Fuse:37s}` → 8 = 37, puis 36, 35… **à chaque tick** |
| bloc qui tombe : départ | **8** | BlockPos | sable posé en l'air : x du bloc de départ |
| creeper : sens du gonflement | **16** | VarInt | `{ignited:1b}` → 16 = 1 au premier tick |
| creeper : chargé | **17** | Boolean | `{powered:1b}` |
| creeper : allumé | **18** | Boolean | `{ignited:1b}` |

La mèche par défaut (80) **n'est pas envoyée** : une TNT amorcée par redstone
arrive sans index 8, et sa première mise à jour vaut 79. Notre serveur fait de
même, et renvoie la mèche à chaque tick comme vanilla.

Le champ `data` de Spawn Entity d'un bloc qui tombe est **l'état de bloc** :
capturé **112** pour le sable et **117** pour le sable rouge — nos propres
identifiants d'état pour ces deux blocs, au chiffre près.

⚠ Un piège en passant : `summon tnt` **sans** `Fuse` part à la mèche 0 et
explose immédiatement, et la ligne de base de la capture a envoyé `8 = 0`. C'est
le jeu, pas un défaut du banc, mais c'est exactement l'allure d'un index mal lu.

---

## 4. Le creeper

Il existait comme mob (`MobKind`, l'une des huit espèces) et ne sifflait pas.

- **Le compte** est celui d'`explosions.md` § 5 : 30 ticks.
- **Le gonflement** commence à moins de **3** blocs d'une cible visible, continue
  tant qu'elle reste à **7** blocs ou moins et en vue, et redescend d'un par tick
  sinon. Ces deux rayons viennent de l'article « Creeper » du wiki et **ne sont
  pas mesurés** : la capture qui devait regarder un creeper gonfler contre une
  sonde en survie l'a vu devenir agressif (bit 0x04 de l'index 15, basculé
  toutes les secondes) sans jamais s'approcher à trois blocs, et n'a lu aucun
  index 16. Le montage est dans le script ; sa réussite ne l'est pas.
- **La cible** est le joueur en survie le plus proche : les joueurs ne sont pas
  des entités de `EntityWorld`, le module lit leurs positions à part. Un joueur
  en créatif n'est jamais une cible. La ligne de vue est le `clip` des collisions
  d'`Explosions`, qui est la ligne de vue « collisions », pas « visuelle ».
- **La charge** : puissance 3, 6 chargé, `DestroyWithDecay` (0,337 mesuré), du
  pied du creeper. Le creeper est retiré par sa propre explosion et ne lâche rien.

Ce qui manque, nommé : le creeper ne **s'arrête pas** pour gonfler (vanilla coupe
sa navigation) et ne **poursuit pas** de joueur (le but de poursuite ne voit que
les entités du monde) ; le briquet sur un creeper n'est pas branché
(`Interact` le reconnaît et ne le fait pas) ; un creeper chargé ne peut pas
naître, faute de foudre.

---

## 5. Les blocs qui tombent

Vingt-cinq blocs : sable, sable rouge, gravier, **les seize poudres de béton**,
les trois enclumes, l'œuf de dragon, le sable et le gravier suspects.

### Le délai et l'échelonnement — mesurés

Quatre colonnes de six sables tenues par une pierre ; la pierre retirée. Les six
blocs de chaque colonne naissent à **2, 4, 6, 8, 10, 12 ticks** (relevés 1, 3,
5…, voir § 0) et finissent en une colonne de six posée sur le sol, de −60 à
−55. Le bloc du bas tombe deux ticks après avoir appris que son support est
parti ; celui du dessus apprend que le bloc du bas est parti au moment où il
devient air, et attend deux ticks à son tour. Un gravier posé en l'air naît
aussi deux ticks après.

### La chute — à douze décimales

Le sable tombe sous la même loi que la TNT : 0,04, puis ×0,98 après le
déplacement. `Time = 1` : y −0,04, `Motion` −0,0392 ; `Time = 29` : y
−59,5484319382437 depuis −45. Sept points sur vingt-neuf sont figés dans le test,
à 10⁻¹² près.

### L'atterrissage

- **Sur une torche** : la torche reste, **un** objet sable au sol. Une cellule
  qui n'est pas dans `#minecraft:replaceable` refuse le bloc, qui tombe en objet.
  Un bloc suspect, lui, ne donne rien (sa récompense est un block entity que ce
  serveur ne porte pas).
- **Dans une piscine de quatre** : le sable **coule jusqu'au fond** et laisse
  l'eau au-dessus ; la poudre de béton **durcit dans la première cellule d'eau**
  qu'elle traverse et laisse les trois en dessous.
- **La poudre et l'eau, face par face** (l'eau posée en dernier) : dessus, nord,
  sud, ouest, est la durcissent **sur place**. Avec l'eau **dessous**, elle ne
  durcit pas sur place : elle **tombe dedans** et durcit une case plus bas.
  Confirmé par le banc `powder` sur quatre montages murés — eau posée sur
  pierre, sur air, sous une case d'air, et le sable témoin qui, lui, remplace
  simplement l'eau sur la pierre.

⚠ **Les deux premiers essais de ce banc étaient faux, et de la même façon.** La
poudre était posée sans support pendant les cinq ticks qu'on attendait avant de
poser l'eau : elle tombait **avant** que l'eau n'arrive, et l'eau posée ensuite
la remplaçait (« la poudre disparaît ») ou la touchait par le dessus (« le béton
est une case trop bas »). Deux résultats « inexpliqués » qui étaient le banc. Le
banc `powder` tient chaque bloc sur une pierre que l'eau remplace — c'est l'eau
qui retire le support, dans le même tick.

⚠ **Le premier bassin avait un couvercle.** Le verre montait d'une case de plus
que l'eau ; le sable et la poudre se sont posés dessus, et le banc lisait « ne
rentre pas dans l'eau ».

### Rien ne tombe au chargement

Vanilla ne réévalue pas un monde chargé : un pont de sable posé par `/setblock`
tient jusqu'à ce qu'un voisin change. Ce module n'est réveillé que par une
notification de voisinage ou par son propre tick planifié, jamais par un
chargement. Le test « nothing falls until something next to it changes » le
vérifie, et le banc `run/lab` est servi dix secondes par le contrôle de bout en
bout (§ 6).

Un écart assumé : vanilla planifie un tick à **chaque** changement de voisin et
laisse le tick décider ; ce module ne planifie que si la case du dessous est
libre. Le monde qui en sort est le même, et le sable d'un désert n'entre pas
dans la file à chaque fois que l'eau passe à côté.

Ce qui manque, nommé (`FallingBlocks::unimplemented()`) : une enclume ne blesse
pas ce sur quoi elle tombe et ne s'use pas ; l'œuf de dragon ne se téléporte pas ;
les blocs suspects perdent leur récompense. L'échafaudage et la stalactite, qui
tombent par d'autres règles, ne sont pas ici. Une couche de neige n'est
remplacée que si elle est seule (d'après le wiki, non mesuré).

---

## 6. De bout en bout, contre notre serveur

`scripts/check_tnt_e2e.py` fait tourner `ov_dedicated` et un client sonde. La
sonde agit comme un joueur en créatif : `Set Creative Mode Slot`, `Use Item On`,
`Player Action`. Rien n'est appelé à côté du protocole.

### `probe` — allumer une TNT, faire tomber du sable

Une TNT posée sur l'herbe, puis le briquet dessus. Ce que la sonde a lu :

| | vanilla (capture) | notre serveur |
|---|---|---|
| Spawn Entity, type | 101 | **101** |
| vitesse envoyée | (0,013, 0,2, 0,015) | **(−0,01525, 0,2, −0,012875)**, ‖h‖ = 0,0199 |
| mèche, index 8 | 79, 78, 77 … | **79, 78, 77 … 3, 2, 1 — 79 mises à jour** |
| Explosion | 0x1D, rayon 4, 12 octets de queue | **0x1D, rayon 4, 0 octet en trop** |
| centre y | −59,93874999880791 | **−59,93874999880791** |
| délai spawn → explosion | 80 ticks | **3,95 s** à l'horloge de la sonde |
| Block Update vers l'air | — | **91** |

Le centre en x et z (3,34 ; 3,37) n'est pas le milieu du bloc (3,5 ; 3,5) : c'est
la TNT qui a glissé de sa poussée initiale pendant quatre secondes, et c'est la
preuve que c'est l'entité qui a explosé et pas le bloc.

Puis une pierre, du sable dessus, la pierre cassée : **un** Spawn Entity de type
36 avec `data = 112` (l'état du sable), et le Block Update qui repose le sable
en **(8, −60, 8)**, une case plus bas.

### `lab` — rien ne tombe au chargement

Une copie de `run/lab` servie dix secondes à une sonde : **0 entité engendrée,
0 bloc qui tombe, 0 TNT, 0 Block Update.**

### `crater` — notre cratère contre le leur

Le monde-graine est celui que **vanilla** a écrit : seize boîtes de terre de
17 × 13 × 17 avec une TNT au centre, sauvées **avant** que vanilla ne les allume.
Vanilla les a ensuite allumées par un bloc de redstone aussitôt remplacé par de
la terre ; notre serveur les allume par le briquet de la sonde. Les deux mondes
sont relus par le **même** lecteur (`ov_inspect`), cellule par cellule.

| | cellules | d'accord | chez nous seulement | chez eux seulement |
|---|---|---|---|---|
| **union** des 16 tirs | 277 / 271 | **265** | 12 | 6 |
| **intersection** des 16 tirs | 167 / 170 | **157** | 10 | 13 |
| écart de fréquence moyen | | **0,048** | | |
| *témoin : notre cratère décalé d'un bloc en x* | | *226 (union)* | *51* | *45* |
| *témoin : écart de fréquence* | | *0,269* | | |

Le témoin est la vérification du piège 14 du briefing : la même comparaison,
contre un cratère décalé d'un seul bloc, perd 39 cellules d'union et fait un
écart de fréquence **5,6 fois** plus grand. La mesure discrimine.

Ce cratère n'est pas celui d'`explosions.md` § 3 (union 211, intersection 128
dans la terre) : là, la charge était invoquée **dans** un bloc de terre plein,
ici la TNT devient de l'air en s'amorçant. C'est pourquoi le banc a été refait
dans la géométrie qu'un joueur produit, et non comparé à l'ancien.

Les désaccords sont au bord, comme dans `explosions.md` : une union sur seize
tirs estime le rayon le plus chanceux, et son bord bouge encore des deux côtés.

### `records` — ce que porte le paquet

Le nombre d'enregistrements d'un paquet Explosion est l'observable qui dit si
la liste porte les cellules d'air : sans elles, il tomberait d'environ 580 à
quelques dizaines. Trente-deux TNT sur de l'herbe **intacte**, seize blocs l'une
de l'autre autour de la sonde, amorcées par redstone chez vanilla et par le
briquet chez nous :

| | charges | enregistrements, moyenne | écart-type | étendue |
|---|---|---|---|---|
| vanilla | 32 | **676,2** | 11,3 | 654 … 701 |
| notre serveur | 32 | **673,5** | 8,3 | |

L'écart vaut 2,7, soit **1,1 erreur standard** de la différence. Les deux listes
sont la même liste.

---

## 7. Pièges

1. **L'archive du wiki donne 0x1E pour Explosion ; c'est 0x1D.** Trouver un
   paquet par son contenu plutôt que par son identifiant est ce qui l'a montré.
2. **Le paquet Explosion porte l'air.** Un serveur qui n'envoie que les blocs
   détruits envoie un paquet valide et différent : 827 enregistrements contre
   quelques dizaines.
3. **`summon tnt` sans `Fuse` explose tout de suite.** La mèche par défaut de la
   commande n'est pas celle d'un amorçage.
4. **`gametime − Time` a un tick de retard** (§ 0). Un délai de 2 se lit 1.
5. **Un bloc qui tombe n'attend pas qu'on ait fini d'installer le banc.** Tout
   bloc soumis à la gravité doit être posé sur quelque chose, et le support
   retiré par le geste qu'on mesure.
6. **La traînée d'une TNT n'est pas celle d'un mob** : 0,98 sur les trois axes,
   sans le seuil de 0,003. `step_entity` aurait donné une TNT qui s'arrête onze
   ticks trop tôt et un saut trop court.
7. **Une explosion ne lit que les tronçons résidents.** Un cratère à cheval sur
   un tronçon que personne n'a chargé lirait une moitié d'air. Le contrôle de
   bout en bout se place au-dessus de chaque boîte avant d'allumer.

---

## 8. Reproduire

```bash
python3 scripts/measure_tnt_gravity.py capture     # paquets et indices
python3 scripts/measure_tnt_gravity.py tnt_motion  # 7900 échantillons
python3 scripts/measure_tnt_gravity.py chain       # 384 mèches
python3 scripts/measure_tnt_gravity.py sand        # colonnes, torche, bassins, faces
python3 scripts/measure_tnt_gravity.py powder      # la poudre au-dessus de l'eau
python3 scripts/measure_tnt_gravity.py creeper     # rendement du butin
python3 scripts/measure_tnt_gravity.py crater      # 16 boîtes, et le monde-graine
ctest --preset macos-debug -R test_ov_gameplay     # [falling] [tnt] [creeper]
python3 scripts/check_tnt_e2e.py probe             # notre serveur, un client sonde
python3 scripts/check_tnt_e2e.py lab               # rien ne tombe au chargement
python3 scripts/check_tnt_e2e.py crater            # notre cratère contre le leur
```

Chaque scénario vanilla démarre son propre serveur sur le port 25619
(`OV_TNT_PORT`), dans `run/tnt-oracle/<scénario>/`, et **efface son monde** en
partant — seul `crater-seed` reste, parce que le contrôle de bout en bout en a
besoin. Les contrôles de notre serveur utilisent le port 25621.
