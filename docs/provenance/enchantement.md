# L'enchantement : la table, l'enclume, la meule, et les 39 effets

## Le nœud

Le registre `minecraft:enchantment` existait, trois systèmes lisaient déjà un niveau dans le
NBT d'un objet (Efficacité au cassage, Solidité/Tranchant/Recul au combat, Infinité et
Charge rapide aux projectiles), et **rien d'autre**. Aucune table, aucune enclume, aucune meule
ne s'ouvrait ; Protection, Chute amortie, Châtiment, Raccommodage ne faisaient rien.

Ce dossier livre :

- `src/ov_gameplay/{include/ov/gameplay/enchanting.hpp, src/enchanting.cpp}` — les 39, leurs
  poids, fenêtres de niveau, catégories et incompatibilités ; le tirage de la table ; l'enclume ;
  la meule ; l'EPF ; Raccommodage ; les bonus de Châtiment / Fléau / Empalement ; et une table
  qui dit, pour **chacun** des 39, où vit son effet — ou pourquoi il ne vit nulle part
  (`effect_status`).
- `src/ov_server/src/enchant_session.{hpp,cpp}` — les trois fenêtres, sur le modèle de
  `workbench.cpp` : des rappels plutôt qu'une référence au serveur.
- Des blocs courts `// ── enchanting ──` dans `server.cpp`, `combat.*`, `combat_session.*`,
  `damage.*`, `player_data.*`.

---

## 1. Sources

Le wiki documente la **version courante** — et depuis 1.21 les enchantements sont des données,
avec d'autres chiffres. Toutes les pages ont donc été lues dans leur **révision d'avant 1.21**
(API MediaWiki, `rvstart=2024-05-01`) :

| Page | Révision | Ce qu'elle a donné |
|---|---|---|
| *Enchanting table mechanics* | 2544409 (2024-04-30) | coût d'une case, étape 1 (modificateurs), étape 3 (tirage pondéré, boucle `(niveau+1)/50`), poids, incompatibilités, étagères, graine `XpSeed` |
| *Enchanting/Levels* | 2534509 (2024-04-21) **et** 2272716 (2023-05-31) | les fenêtres min/max par niveau ; les deux révisions (« 1.20.5 pre-1 » et « 1.14.4 ») donnent **les mêmes nombres** |
| *Anvil mechanics* | 2500853 (2024-03-31) | combinaison, réparation par matériau (25 %), réparation par deuxième objet (+12 %), renommage, pénalité `2^n − 1`, « Trop cher » à 40, multiplicateurs objet/livre, 12 % de dégradation |
| *Grindstone* | 2538159 (2024-04-24) | sortie, malédictions gardées, +5 % ; « quelle formule pour l'XP ? *Info needed* » |
| *Unbreaking* | 2485736 | `1/(n+1)` pour un outil, `60 % + 40 %/(n+1)` pour une armure |
| *Armor* | 2543310 | la table EPF, le plafond 20, `réduction = EPF/25` ; les durabilités d'armure |
| Protocole 763 (archive figée `oldid=2773082`) | — | *Click Container Button* 0x0A, *Rename Item* 0x23, les dix propriétés de la table, la propriété 0 de l'enclume |

Ce que ces pages ne disent pas, et qui a été **mesuré** au lieu d'être supposé, est au § 3.

---

## 2. Ce que le code fait, et pourquoi c'est ainsi

### La table

`table_offers(XpSeed, étagères, objet)` : un `LegacyRandomSource` (le `java.util.Random` du
dépôt) semé par la graine ; trois coûts, deux tirages chacun (`nextInt(8)`, puis
`nextInt(b+1)`) ; un coût inférieur à `case + 1` est annulé ; puis, pour chaque case vivante, le
générateur est **ressemé** à `graine + case` et la sélection est tirée ; l'indice affiché est un
`nextInt(taille)` **sur le même générateur, après** la sélection. Cliquer refait la même
sélection : ce qui est affiché est ce qui est appliqué.

Pièges C++ évités par construction : deux tirages ne sont jamais dans une même expression
(piège n° 2 du briefing) ; `graine + case` est une addition `int` de Java, qui **déborde** ;
`Math.round(float)` arrondit la moitié vers le haut sans le double arrondi de
`floor(x + 0.5f)`.

Le coût réel est **l'indice du bouton plus un** — en niveaux et en lapis — pas le nombre vert
affiché. La graine suivante vient du générateur propre au joueur.

`XpSeed` est porté par `PlayerRecord` (lu, écrit) : un joueur neuf a 0, comme chez vanilla.

La propriété 3 est la graine **sans masque** — l'archive du protocole dit `& 0xFFFFFFF0`, le vrai
serveur ne le fait pas (§ 5, piège n° 5) — et, comme toute *Container Property*, elle voyage en
**short** : ce sont ses 16 bits bas qui arrivent. Le test compare `(i16)seed`.

### Les étagères

Les 32 positions (deux blocs autour, à la hauteur de la table et au-dessus), le bloc
intermédiaire à `offset / 2` tronqué vers zéro, **à la hauteur de l'étagère**. Les deux tests
viennent des tags du data generator (`#enchantment_power_provider`,
`#enchantment_power_transmitter` = `#replaceable`), lus dans le pack, jamais en dur.

### L'enclume

`anvil_result` suit la prose du wiki : réparation par unités (un quart du maximum chacune), par
un second objet (le reste des deux plus 12 % du maximum), puis chaque enchantement du sacrifice :
niveau égal → +1, plus haut → le sien, plafonné au maximum ; incompatible → +1 au coût et refusé ;
coût `multiplicateur × niveau final`. Renommage +1. `cost ≥ 40` hors créatif : pas de sortie,
mais la propriété garde le coût (« Trop cher ! »). Un renommage seul est plafonné à 39. La
pénalité de sortie est `max(gauche, droite)` puis `2n + 1`, sauf renommage seul.

### La meule

Retire tout sauf les malédictions ; `RepairCost` repart de 0 puis `2n + 1` **par malédiction
restante** ; un livre enchanté vidé devient un livre, en gardant son nom ; deux objets identiques
se combinent à `+5 %`. L'XP d'une prise est tirée à la prise, depuis la somme des coûts
*minimaux* des enchantements retirés.

### Protection

`DamageMitigation::protection` porte l'EPF des pièces portées **par type de dégât**, recalculé
seulement quand une pièce change (aucun NBT n'est relu sur un tick calme), appliqué **après**
Résistance et avant les cœurs jaunes : `montant × (1 − min(EPF, 20)/25)`.

### Le reste des 39

`effect_status()` en fait la liste ; le tableau du § 4 la reprend.

---

## 3. Mesures contre le vrai serveur

`lockf /tmp/ov-vanilla.lock python3 scripts/measure_enchanting.py` contre
`tools/vanilla/server.jar` (SHA-1 `84194a2f286ef7c14ed7ce0090dba59902951553`), puis
`python3 scripts/check_enchanting.py` pour les chiffres et la table plate du test d'enclume.

### La table — **512 / 512 offres identiques, 57 / 57 enchantements identiques**

64 tours sur le vrai serveur ; à chaque tour, `XpSeed` lu par `/data get entity`, un nombre
d'étagères (0 à 15, les 16 valeurs), et huit objets posés l'un après l'autre — **512 offres**,
**56 graines**, **46 objets** (épées, outils, armures de chaque matériau, livre, arc, arbalète,
trident, canne, cisailles, bouclier, élytres, et des objets que la table refuse : bâton, boussole,
citrouille). 390 offres ont des coûts non nuls.

Rejouées par `test_ov_gameplay [enchanting][parity]` : **les dix propriétés identiques pour les
512** — les trois coûts, la graine, les trois indices d'enchantement et leurs trois niveaux. Même
graine, mêmes étagères, même objet → mêmes offres, tirage pour tirage.

À 57 de ces tours, un bouton a été pressé (20 en haut, 20 au milieu, 17 en bas) et la pile
enchantée relue : **57 / 57 listes d'enchantements identiques** à celles que tire notre
`table_enchantments` — dont 22 portent plusieurs enchantements, donc la boucle `(niveau+1)/50`, le
filtre des incompatibilités et la division du niveau sont exercés. **57 / 57** ont prélevé
exactement *l'indice du bouton plus un* en niveaux et en lapis, pas le coût affiché.

Avant la correction de la propriété 3 (§ 5, piège n° 5) : 24 / 512 — alors que coûts et indices
étaient déjà justes partout.

### La table sur notre propre serveur, contre le vrai

À `XpSeed` 0, sans étagère, le vrai serveur affiche les coûts **2 / 2 / 6** (et 2 / 3 / 6 avec une
étagère) ; notre `ov_dedicated`, piloté par la sonde de bout en bout, affiche **2 / 2 / 6** à la
même graine. Les trois coûts ne dépendent que de la graine et du nombre d'étagères, pas de
l'objet : la comparaison vaut malgré un livre d'un côté et une épée de l'autre. Avec quinze
étagères, le bouton du bas pose Solidité III et Butin II sur l'épée en diamant — l'énoncé du wiki
pour la graine 0 — et prend **3 niveaux** (30 → 27), l'indice plus un, pas les 30 affichés.

### Les étagères — 272 cellules, 0 ambiguë

Une étagère à chacune des 32 positions, un bloc entre elle et la table, et le verdict lu sur les
coûts d'une graine où 0 et 1 étagère diffèrent (graine 0 : 2/2/6 contre 2/3/6). **128 comptées,
144 bloquées, 0 ambiguë**, et chaque bloqueur se range d'un seul côté :

| au bloc intermédiaire, **à la hauteur de l'étagère** | verdict |
|---|---|
| air, fougère, herbe, neige, eau | l'étagère compte |
| pierre, verre, dalle de chêne, torche, tapis, toile d'araignée | l'étagère ne compte pas |
| pierre à l'intermédiaire de **l'autre** couche | sans effet |

C'est exactement `#enchantment_power_transmitter` (= `#replaceable`) au point `offset / 2`,
tronqué vers zéro, à la hauteur de l'étagère — la toile d'araignée, pourtant « traversable »,
n'y est pas et bloque.

### Chute amortie de bout en bout, sur notre serveur

Des bottes Chute amortie IV posées par la sonde, une chute de 13 blocs : **20 → 14,8**, soit 5,2,
exactement `10 × (1 − 12/25)` — les 10 d'une chute de 13 blocs sont ceux que `survie.md` a
mesurés contre vanilla (30 hauteurs sur 30).

### Protection — 46 coups, `/damage` sur une tête de joueur

Une tête de joueur n'a aucun point d'armure : ce qui reste d'un coup est l'enchantement seul.
Un coup de 10 laisse 10,4 / 10,8 / 11,2 / 11,6 / 12,0 à Protection I–V, exactement
`10 × (1 − EPF/25)` en float (10.400001, 11.599999 compris). Le plafond à 20 tient
(Protection X + Chute amortie IV sur une chute : 18,0 de santé, soit 80 %). Un boule de feu est à
la fois `#is_fire` et `#is_projectile` : Protection contre le feu IV et contre les projectiles IV y
valent chacune 8. `out_of_world` et `sonic_boom` passent entiers.

**Une correction de la règle, mesurée** : `starve` passe entier une tête Protection IV (10 sur
10), alors qu'il n'est pas dans `#bypasses_enchantments` — il est dans `#bypasses_effects`, et ce
tag saute l'étape des enchantements comme celle de la Résistance. `total_epf` rend 0 pour les
deux tags. Avant la correction : 45/46 ; après : voir le test `[enchanting][parity]`.

### Châtiment, Fléau, Empalement, Tranchant — un coup chargé d'épée en diamant (7)

| cas | dégâts mesurés | modèle |
|---|---|---|
| épée nue, vache / zombie | 7 / 7 | 7 |
| Tranchant V, zombie | 10 | 7 + 3 |
| Fléau V, araignée / vache | 19,5 / 7 | 7 + 12,5 / 7 |
| Châtiment V, vache | 7 | 7 |
| Châtiment V, III, zombie | **18,5 / 13,5** | 19,5 / 14,5 |
| Empalement V (trident, 9), gardien / vache | 21,5 / 9 | 9 + 12,5 / 9 |

L'écart d'un point sur le zombie n'est pas le bonus : les zombies **brûlaient** au soleil de midi
(1024 → 1023 avant même le coup). Un coup porté dans la fenêtre d'invulnérabilité ouverte par une
brûlure de 1 n'inflige que la différence : 19,5 − 1 et 14,5 − 1. Le +2,5 par niveau est donc
confirmé sur les trois enchantements.

### L'enclume — **93 / 93 combinaisons identiques**

Chaque combinaison du panneau — renommage, pénalités 3 à 60, réparation par 1 à 5 unités,
fusion de deux objets abîmés, les quatre exemples du wiki, livres dans les deux ordres, conflits
(trident, arbalète, protections), sur-niveau, malédictions, bouclier et citrouille, et les 37
paires « durabilité » — rejouée par `test_ov_server [anvil][parity]` : **même coût, même pile de
sortie, NBT compris** (`Enchantments` dans l'ordre, `RepairCost`, `display.Name`, `Damage`).

Deux corrections sont venues de la mesure, pas de la documentation :

- **`Damage:0` est toujours écrit.** La sortie porte `Damage` pour tout objet qui s'use, même
  quand rien n'a été réparé : vanilla ne garde jamais une telle pile sans la clé. Avant : 67/93 ;
  25 des 26 écarts étaient cette seule clé.
- **Une pile de gauche à plusieurs objets, avec un enchantement à poser, ne donne rien — coût 0.**
  Pas le « trop cher » à 40 qu'on lui prêtait. Le renommage d'une pile seul reste permis.

### L'enclume — ce que le panneau révèle d'autre

**La durabilité de 37 objets, lue par l'enclume.** Deux pièces identiques au même `Damage` *D*
donnent une sortie à `max − (2·(max − D) + ⌊max·12/100⌋)`, strictement décroissante en `max` :
chaque combinaison désigne **une seule** valeur. Les 37 tombent sur la table de
`enchant_max_damage` : les 24 pièces d'armure et la carapace de tortue (275), élytres 432,
bouclier 336, arc 384, **arbalète 465** (le tableau « durée de vie » du wiki Unbreaking écrit 464 —
c'est une durée de vie, pas le maximum), trident 250, canne à pêche, briquet et pinceau 64,
carotte sur un bâton 25, champignon biscornu 100, cisailles 238, pioche en fer 250.

**La dégradation** : 21 descentes d'un stade sur 150 usages, **0,140** (z = +0,75 contre 0,12).

**Les niveaux prélevés sont exactement le coût affiché** : renommage 1 (50 → 49), réparation de
deux unités 2 (→ 48), l'exemple « livre » du wiki 7 (→ 43), pénalités 7 + 3 : 11 (→ 39).

### Solidité — une houe qui laboure, comptée par la statistique `used`

| niveau | points perdus / usages | taux | attendu `1/(n+1)` | z |
|---|---|---|---|---|
| 0 | 271 / 271 | 1,000 | 1,000 | — |
| I | 148 / 279 | 0,530 | 0,500 | +1,02 |
| II | 94 / 290 | 0,324 | 0,333 | −0,33 |
| III | 66 / 261 | 0,253 | 0,250 | +0,11 |

**L'armure n'est pas mesurée.** La campagne a lu 0 point perdu sur un casque de cuir, niveau 0
(50 coups) comme niveau III (200 coups) : même le casque nu ne s'use pas, donc la méthode était
fausse — probablement la Résistance V qui gardait le bot en vie, ou le chemin de lecture du
`Damage`. Le chiffre du wiki (`60 % + 40 %/(n+1)`) reste celui de la documentation ; l'armure ne
s'use de toute façon pas dans ce serveur.

### La meule — douze cas

| cas | sortie | XP mesurée | modèle `ceil(b/2) … b` |
|---|---|---|---|
| épée Tranchant V, `Damage` 100 | épée, `Damage` 100, `RepairCost` 0 | 23..45 (n = 25) | b = 45 : 23..45 |
| livre Solidité I | **livre**, tag vide | 3..5 (n = 25) | b = 5 : 3..5 |
| pioche Raccommodage | pioche, `Damage` 0 | 15..22 (n = 10) | b = 25 : 13..25 |
| plastron Protection IV + Solidité III + Lien éternel, `RepairCost` 7 | plastron, **Lien gardé** | 28..54 (n = 10) | b = 55 : 28..55 |
| deux pioches, `Damage` 200 et 150 | pioche, `Damage` **88** | 0 | 250 − (50 + 100 + 12) = 88 |
| idem, celle du bas Efficacité II | pioche, `Damage` 88 | 6..9 (n = 5) | b = 11 : 6..11 |
| épée Disparition seule | épée, malédiction gardée | 0 | b = 0 |
| épée nue ; paire différente ; deux livres différents | **rien** | — | rien |
| épée seule en bas, Tranchant V | épée, `Damage` 0 | 27..38 (n = 3) | 23..45 |
| épée renommée, `RepairCost` 7, Tranchant I | épée, nom gardé, `RepairCost` 0 | 1 | b = 1 : 1 |

Un détail n'est pas expliqué : la sortie garde `Damage:0` dans tous les cas **sauf** l'épée
renommée, qui sort sans `Damage` du tout. Le code garde la clé du tas copié ; l'exception est
nommée, pas modélisée.

### Raccommodage — un orbe de valeur *v* sur une pioche à `Damage` 200

1 → 2, 2 → 4, 3 → 6, 5 → 10, 10 → 20, 60 → 120 : **2 × v**, exactement. L'orbe de 200 répare les
200 points et laisse **99** d'expérience au joueur, là où `v − réparé/2` en donne 100. La lecture
passe par `xp query … points`, qui rend `(int)(progression × coût du niveau)` en float — une
troncature qui peut perdre un point ; ce n'est pas établi, c'est nommé.

---

## 4. Les 39, un par un

| Enchantement | Où vit l'effet |
|---|---|
| Protection, Protection contre le feu / les explosions / les projectiles, Chute amortie | EPF dans le chemin des dégâts du joueur |
| Respiration | un tick sous l'eau garde son air avec la probabilité `n/(n+1)` : le serveur tire, `SurvivalSession` gèle l'air comme sous Respiration aquatique (non mesuré contre vanilla) |
| Affinité aquatique | posture de cassage, lue sur le casque (case 5) |
| Épines | **pas d'hôte** : aucun mob ne frappe un joueur dans ce serveur |
| Agilité aquatique, Vitesse des âmes, Furtivité | côté client (le mouvement du joueur est le sien) ; l'usure des bottes sur le sable des âmes n'est pas modélisée |
| Semelles givrantes | **pas d'hôte** : la glace givrée et ses ticks n'existent pas |
| Malédiction du lien éternel | **pas d'hôte** : l'écran du joueur ne refuse pas encore de retirer la pièce |
| Tranchant | combat (existant) |
| Châtiment, Fléau des arthropodes | combat : +2,5 par niveau contre le groupe du mob visé |
| Recul, Aura de feu, Butin, Affilage | combat / tables de butin (existants) ; Aura de feu calcule ses ticks, la combustion est au système du feu |
| Efficacité | cassage (existant) |
| Toucher de soie, Fortune | tables de butin de blocs (existantes) |
| Solidité | outils (existant) ; l'armure ne s'use pas dans ce serveur |
| Puissance, Frappe, Flamme, Infinité, Tir multiple, Perforation, Charge rapide, Impulsion | projectiles (existants) |
| Chance de la mer, Appât | **pas d'hôte** : pas de pêche |
| Loyauté | **pas d'hôte** : un trident lancé ne revient pas (`projectile.cpp` le dit) |
| Empalement | corps à corps seulement, +2,5 par niveau contre les mobs aquatiques ; pas sur le trident lancé |
| Canalisation | **pas d'hôte** : il faut un orage |
| Raccommodage | au ramassage d'un orbe : l'objet porté ou tenu, abîmé, tiré au hasard, `2 × valeur` |
| Malédiction de disparition | l'objet disparaît à la mort au lieu de tomber |

---

## 5. Les pièges payés ici

**1. Un tick de plus de 30 s ferme la connexion d'un client qui parle.** `listener.cpp` ferme une
connexion restée silencieuse **30 s** (`kHandshakeTimeout`). Mais les gestionnaires de paquets
tournent **sur le fil réseau** et prennent `players_mutex`, que le fil de tick tient pendant tout
le tick : un tick de 32,5 s (relevé dans le journal du serveur, sur une machine où tournaient neuf
agents) bloque le fil réseau dans un gestionnaire, plus rien n'est lu — les paquets de la sonde
attendent dans la socket —, le minuteur expire, et son gestionnaire ferme la socket dès que le fil
se libère. La sonde a perdu sa connexion **quatre fois au même endroit**, pendant la pose des
quinze étagères (chaque `setblock` y coûtait deux secondes de tick). Une sonde qui parle une fois
par seconde, comme un vrai client, supprime seulement le cas du silence ; elle ne peut rien contre
un tick de 32 s. **Ce n'est pas un défaut de l'enchantement**, et ce n'est pas corrigé ici : c'est
un serveur qui ne tient pas le tick, sur une machine saturée.

Un premier diagnostic accusait un keep-alive envoyé pendant qu'un autre attendait sa réponse
(notre serveur en envoie un toutes les 10 s même en attente, vanilla non ; une réponse traitée
après l'envoi du suivant serait refusée et couperait le client). La correction n'a **rien
changé** — la connexion tombait au même endroit —, elle a donc été retirée. Le risque reste réel
en théorie et n'a pas été observé : il est nommé ici, non corrigé.

**0. La graine suivante valait toujours 0 — un bogue de ce mandat, trouvé de bout en bout.** Le
générateur propre au joueur, celui qui tire le prochain `XpSeed`, était semé par
`entity_id × 0x5DEECE66D`. Or `setSeed` de Java fait un XOR avec **la même constante** : pour
l'entité 1 — le premier joueur d'un serveur neuf — l'état tombait à 0, et le premier `nextInt()`
d'un état 0 vaut exactement 0. La table remontrait donc les offres de la graine 0 après chaque
enchantement. Le banc unitaire ne pouvait pas le voir (il semait à 7) ; la sonde l'a vu : la
propriété 3 restait à 0 après le bouton. Le générateur est maintenant semé par l'UUID du joueur,
mélangé (`enchant_random_seed`), et un test fige la régression.

**1 bis. `Set Creative Slot` jetait le tag de l'objet.** `parse_set_creative_slot` lisait
l'identifiant et le compte, puis s'arrêtait ; le serveur reconstruisait la pile avec un NBT vide.
Un livre enchanté ou un outil enchanté pris dans l'inventaire créatif arrivait **nu** — sans que
rien ne le dise. Découvert quand la sonde a relu sa propre pioche à Raccommodage : `tag: None`, et
avec elle les bottes à Chute amortie et l'épée enchantée donnée à la meule. Corrigé : la case est
lue entière par `read_slot`, qui garde le tag tel qu'il est arrivé.

**2. Une sonde qui attend un temps fixe mesure la charge de la machine.** La première version
attendait 0,8 s après un clic : les dix propriétés valaient 0, le bouton partait sur des coûts
nuls, et tout ressemblait à une table cassée. Chaque étape attend maintenant un **fait** — la
réponse de la console dans le journal du serveur, un coût non nul, l'objet dans la case. La
campagne vanilla fait de même avec une barrière : les lignes de console passent en début de
tick, avant les paquets des joueurs, donc deux allers-retours par la console garantissent que le
clic précédent a été traité et ses propriétés envoyées.

**3. `tp` puis `fill` dans le même souffle place le `fill` dans le vide.** Les chunks de la
destination sont générés hors du thread de tick ; la commande suivante arrive avant eux.

**4. `gamemode creative` sur un joueur déjà créatif ne répond rien.** Une sonde qui attend la
phrase attend pour rien — le banc est créatif par défaut.

**5. La propriété 3 de la table n'est pas masquée — l'archive du protocole se trompe.** L'archive
écrit que la graine envoyée est `seed & 0xFFFFFFF0`. Mesuré sur 512 offres : le vrai serveur envoie
**les 16 bits bas de la graine, tels quels** — −11468 pour la graine 1338299188 (`0x4FC4D334`), là
où le masque donnerait −11472. Avec le masque, **24 offres sur 512** passaient (celles où la graine
finissait par zéro, et les objets refusés) alors que les coûts et les indices étaient déjà justes
partout ; sans lui, voir le § 3. Ce masque, s'il existe, est appliqué côté client.

---

## 6. Ce qui n'est pas fait, nommé

- **Les infobulles de notre client** : les enchantements ne s'affichent pas encore sous le nom
  d'un objet dans nos écrans. Le livre enchanté du créatif en a une ; le reste attend l'agent
  des écrans du client.
- **Les sons** de la table (`block.enchantment_table.use`) ; l'enclume et la meule envoient leurs
  *World Event* 1030 / 1029 / 1042.
- **Le glisser (mode 5) et le double-clic (mode 6)** dans les trois fenêtres : renvoyés tels
  quels, la fenêtre est resynchronisée.
- **Les orbes de la meule** partent en un seul orbe de la valeur totale ; vanilla les découpe en
  tailles standard. La quantité d'XP est la même.
- Une liste d'enchantements portant **deux fois** le même identifiant est lue au premier ;
  vanilla applique certains effets deux fois dans ce cas (bonus de dégâts).
- **Solidité sur l'armure n'est pas mesurée** (la campagne a lu 0 partout, méthode fausse) — et
  l'armure ne s'use pas dans ce serveur.
- **La meule** sort une épée renommée sans `Damage` là où tous les autres cas gardent `Damage:0` :
  mesuré, non expliqué, non modélisé.
- **Hors enchantement, trouvé en chemin** : notre code de survie n'enregistre pas toujours une
  seconde chute après un changement de mode de jeu (la sonde l'a vu deux fois) ; notre serveur
  envoie un keep-alive toutes les 10 s même quand le précédent attend sa réponse, ce qui
  couperait un client dont la réponse est traitée après l'envoi du suivant — possible, jamais
  observé (un avertissement le signale désormais dans le journal).

---

## Reproduire

```bash
lockf /tmp/ov-vanilla.lock python3 scripts/measure_enchanting.py   # l'oracle, ~30 min
python3 scripts/check_enchanting.py                                # les chiffres, la table plate
./build/macos-debug/bin/test_ov_gameplay "[enchanting]"            # parité table / Protection
./build/macos-debug/bin/test_ov_server "[enchanting]"              # parité enclume, fenêtres
python3 scripts/check_enchanting_e2e.py                            # notre serveur, de bout en bout
```
