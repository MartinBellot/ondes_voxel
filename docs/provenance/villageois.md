# Villageois — métiers, commerce, niveaux

Ce dossier trace les villageois : leur identité (type, métier, niveau), la prise et la perte
d'un métier au bloc de travail, l'écran de commerce, les offres de chaque métier à chaque niveau,
les prix, l'expérience et la montée de niveau, le réapprovisionnement, et un comportement minimal
(travail, sommeil, fuite). **Tout chiffre ci-dessous a été mesuré contre le vrai serveur 1.20.1**
(`tools/vanilla/server.jar`) par `scripts/measure_villagers.py`, avec un client sonde connecté ;
les relevés bruts vont dans `.scratch/villagers.json` (non suivi). Ce qui n'a pas été mesuré est
dit à l'endroit où c'est utilisé, et regroupé au § 12.

**Pas de cerveau à la vanilla.** Le jeu mène un villageois par un « brain » de mémoires, de
capteurs et d'activités planifiées. Ce serveur le mène par le système de buts de tous les autres
mobs, avec cinq buts propres (fuite, commerce, sommeil, prise de métier, travail) qui reproduisent
ce qu'un joueur voit et ce qui a été mesuré. Le reste du cerveau est nommé au § 12.

Le résultat court :

| mesure | vanilla | nous |
|---|---|---|
| indice VillagerData | 18, type 18, envoyé même à plains / none / 1 | 18, octets identiques |
| octets VillagerData | `type, métier, niveau` (varints), 4/4 relevés | idem |
| blocs de travail | 13/13 donnent leur métier à 3 blocs | table des 13 (+ 3 chaudrons) |
| embauche à 3 blocs | ≤ 35 ticks (chaudron 135) | < 200 ticks dans le test |
| perte du métier | 3 ticks après le pupitre cassé, si jamais échangé ; gardé avec Xp 5 | idem |
| offres tirées | 4298 offres de 2319 villageois | **4298 / 4298** retrouvées dans nos lots |
| ordre des deux offres | celui d'un `HashSet<Integer>` (indice modulo 16) | idem, 0 villageois hors ordre |
| livres enchantés | 37 enchantements sur 248 livres, prix dans leur plage | idem |
| armure teinte | 65 couleurs, toutes expliquées par la règle du wiki | règle en flottant |
| prix avec demande | 4/4 offres : n − 1 refusé, n accepté | `cost_a_count` |
| seuils de niveau | 10 / 70 / 150 / 250, encadrés à un point | idem |
| XP par échange | le `xp` de l'offre | idem |
| orbe par échange | 3–6 ; 8–11 sur l'échange qui fait monter | `3 + next_int(4)`, +5 |
| montée de niveau | 40 ticks après la fermeture (lu 1 à 1,87 s, 2 à 2,13 s) | 40 ticks |
| réapprovisionnement | 2 par jour, 2437 ticks entre les deux, demande `d + u − (max − u)` | idem |
| Merchant Offers | 0x2A, 144 octets décodés champ par champ | même encodeur |
| Select Trade | 0x26 : la case de paiement se remplit (64 papiers) | idem |
| fuite devant un zombie | nette à 5 et 7 blocs, à 0,21–0,24 bloc/tick | 8 blocs, 0,225 |
| zombification | facile 0/10, normal 37/90, difficile 10/10 | **non faite**, nommée |
| Merchant Offers, octet par octet | 144 octets capturés | identiques hors ordre des clés NBT, NBT égal tag par tag |
| de bout en bout (§ 11) | — | bibliothécaire en 13,1 s ; livre à 8 émeraudes, payé 8 ; niveaux 2 puis 3 |

---

## 1. Méthode

### 1.1 Les offres se tirent quand on les demande

Un villageois tire ses offres **la première fois que quelque chose les demande**, et sauver
l'entité les demande. Donc `data get entity … Offers` sur un villageois fraîchement invoqué avec
un métier et un niveau, sans `Offers`, rend un tirage frais. C'est ce qui a permis d'échantillonner
les tables sans rapport du data generator (qui n'en a pas : dans le jeu ce sont du code) :

* 30 villageois par métier et par niveau (13 × 5), 100 pour les bibliothécaires des niveaux 1 à 4,
  12 pêcheurs maîtres de chacun des sept types, 5 sans-emploi et 5 nitwits : **2319 villageois,
  4298 offres**.
* Un villageois invoqué au niveau 3 sans offres a **deux** offres, toutes deux du lot du niveau 3 :
  le tirage paresseux ne prend que le niveau courant. `ensure_offers` fait de même.

La structure des tables — un lot par métier et niveau, deux offres tirées, les sortes d'entrée —
vient de la page « Trading » du wiki. Son contenu, lui, vient de la mesure : le résumé du wiki que
j'ai lu mêlait des versions (une carte des épreuves de 1.21, un armurier dans un autre ordre) et
aurait donné une table fausse à une douzaine d'endroits. `scripts/check_trades.py` relit les lots
**directement dans `trading.cpp`** et les confronte aux échantillons, ligne par ligne.

### 1.2 La sonde

`Trader` (dans `measure_villagers.py`) est la sonde de `measure_husbandry.py` avec ce qu'un écran
de commerce demande : l'id de fenêtre, l'id d'état du conteneur, et un enregistreur de tout ce qui
arrive. Elle ouvre l'écran par `Interact`, choisit une offre par `Select Trade`, clique la case
résultat par `Click Container` et ferme par `Close Container`.

### 1.3 Deux campagnes

La première (`meta offers packet trade claim flee zombify cure`, 25 min) a tout livré sauf les
octets de VillagerData — le décodeur de la sonde s'arrêtait au type 18 — et une zombification
normale trop maigre (10/30). La seconde (`meta2 zombify_normal`) les a comblés.

---

## 2. La métadonnée (campagnes `meta`, `meta2`)

Un champ NBT à la fois contre une référence du même type, comme la table du zombie :

| NBT | indice | type | valeur |
|---|---|---|---|
| `VillagerData` (villageois) | 18 | VillagerData | envoyé à **chaque** apparition, même plains / none / 1 |
| `VillagerData:{type desert, librarian, 3}` | 18 | | `00 09 03` |
| `VillagerData:{taiga, farmer, 5}` | 18 | | `06 05 05` |
| `Age:-24000` | 16 | booléen | vrai (le même indice que les animaux) |
| `SleepingX/Y/Z` | 6 et 14 | pose, position de bloc optionnelle | pose 2 (dormir) |
| clic sur un sans-emploi, sur un bébé | 17 | VarInt | 40, puis 39, 38 … à chaque tick, **aucun** Entity Event |
| zombie villageois : `VillagerData` | 20 | VillagerData | |
| zombie villageois : `ConversionTime:2000` | 19 | booléen | vrai |

Les types et les métiers se numérotent dans l'ordre des registres `villager_type` et
`villager_profession` ; `test_villager.cpp` le vérifie contre le registre, entrée par entrée.

---

## 3. Le métier (campagne `claim`)

### 3.1 Les treize blocs

Treize villageois sans métier, chacun à trois blocs de son bloc, sur de l'herbe nue :

| bloc | métier | bloc | métier |
|---|---|---|---|
| haut fourneau | armurier | pupitre | bibliothécaire |
| fumoir | boucher | tailleur de pierre | maçon |
| table de cartographie | cartographe | métier à tisser | berger |
| alambic | clerc | table de forgeron | forgeron d'outils |
| composteur | fermier | meule | forgeron d'armes |
| tonneau | pêcheur | chaudron | tanneur |
| table d'archer | archer | | |

**13 sur 13.** Douze ont pris leur métier au premier relevé (35 ticks), le tanneur à 135 — le
chaudron est pris une fois le villageois arrivé à côté. Au moment du métier, chacun était à 0,9 à
1,7 bloc du centre de son bloc : `kJobSiteReach` = 2. Les trois autres chaudrons (d'eau, de lave, de
neige poudreuse) donnent aussi le tanneur d'après le wiki ; **non mesurés**.

### 3.2 Le rayon

Des pupitres à 16, 32, 44, 47, 49 et 52 blocs, un villageois chacun, 100 s :

| pupitre à | revendiqué | distance du villageois au relevé |
|---|---|---|
| 16, 32, 44 | au premier relevé | 13,1 · 28,8 · 40,1 (il marchait déjà) |
| 47 | **jamais** | il s'est éloigné : 47,0 puis 49,3, 50,6 … |
| 49 | à 1431 ticks | 42,9 |
| 52 | à 1079 ticks | 46,7 |

Le rayon n'est **pas encadré** finement : les revendications de 49 et 52 se sont faites quand le
villageois, en errant, s'était rapproché, et chaque relevé tombe jusqu'à 1,5 s après, pendant qu'il
marche vers le bloc. Rien ne contredit les 48 blocs du wiki, que `kJobSearchRadius` garde.

Chez nous la recherche est un **balayage incrémental** de la sphère, 4096 blocs par tick, plus
proche d'abord, en ignorant ce qu'un autre villageois tient déjà ; sa hauteur est bornée à ±8
(`kScanHalfHeight`, nommé : la campagne avait tout à la même hauteur).

### 3.3 La marche

Vers un pupitre lointain, 0,126 à 0,14 bloc/tick. C'est la loi de marche d'`elevage.md`
(`v = 2,1586 · s²`) avec l'attribut 0,5 et le modificateur 0,5 : 0,1349.

### 3.4 La perte

Deux bibliothécaires, fraîchement embauchés à trois blocs ; B reçoit `Xp:5` ; les deux pupitres
sont cassés. **A n'a plus de métier au premier relevé, 3 ticks après ; B garde le sien** pendant
la minute observée. La règle : un métier sans bloc, sans échange (`Xp` 0, niveau 1), se perd.

---

## 4. L'écran (campagne `packet`)

Un bibliothécaire aux offres écrites à la main, pour que chaque champ ait une valeur connue. Ce qui
arrive, dans l'ordre : **Open Screen** (0x30, fenêtre 1, menu 18 `merchant`), **Set Container
Content** (0x12), **Merchant Offers** (0x2A, identifié par son contenu — piège 24 du briefing).
Les 144 octets se décodent entièrement :

```
fenêtre (VarInt) · nombre (VarInt)
par offre : case A · case résultat · case B · désactivée (bool) · utilisations (int)
            · maximum (int) · xp (int) · prix spécial (int) · multiplicateur (float) · demande (int)
niveau (VarInt) · expérience (VarInt) · villageois ordinaire (bool) · réapprovisionne (bool)
```

* La case A porte le **prix de base** (17 émeraudes pour une offre à demande 4) : le client ajoute
  la demande lui-même. Une offre épuisée part `désactivée`.
* Le NBT d'une case a une racine **sans nom** (`0a 00 00 …`), que nous écrivons de même.
* Le titre est le nom de l'entité : `{"insertion":<uuid>,"hoverEvent":{"action":"show_entity",…},
  "translate":"entity.minecraft.villager.librarian"}`. `merchant_title` le reproduit.
* **Pendant un échange, les offres ne sont pas renvoyées** : aucun 0x2A après le clic sur la case
  résultat. Le client tient le compte des utilisations.

**Select Trade** (0x26, l'archive) est confirmé par la réponse du serveur : la case de paiement se
remplit depuis l'inventaire — d'abord l'inventaire principal, puis la barre — **jusqu'à une pile
entière** (64 papiers pour une offre de 24), et la case résultat affiche le produit.

---

## 5. Les offres (campagne `offers`)

### 5.1 Les lots

`check_trades.py` : **4298 / 4298** offres échantillonnées correspondent à une ligne de nos lots —
objet, nombre, prix, utilisations, expérience, multiplicateur. Les lignes jamais vues sont toutes
dans les grands lots de couleurs, et leur nombre est celui que le tirage prévoit : maçon 4, 3
contre 5,1 attendues ; berger 2, 6 contre 7,0 ; berger 3, 1 contre 1,0 ; berger 4, 1 contre 1,3.
Aucune ligne qui avait moins de 1 % de chances de passer inaperçue ne l'a été.

Ce que la mesure a corrigé par rapport au wiki que j'avais lu :

* l'armurier novice : charbon, **jambières, bottes, casque, plastron**, dans cet ordre ;
* l'arc et l'arbalète enchantés de l'archer, l'épée de fer enchantée du forgeron d'armes :
  multiplicateur **0,05**, pas 0,2 ;
* le plastron du tanneur compagnon : **1** d'expérience, pas 10 ;
* l'armure de cheval en cuir du tanneur est vendue **sans teinture** ;
* le bateau du pêcheur maître selon le type : jungle pour désert et jungle, chêne pour plaines,
  acacia pour savane, sapin pour neige et taïga, chêne noir pour marais (12 de chaque type).

### 5.2 L'ordre des deux offres

Un niveau ajoute deux entrées distinctes de son lot. L'ordre où elles s'affichent n'est **pas**
l'ordre du lot : 9 à 16 villageois sur 30 le contredisaient dans les lots de plus de seize entrées,
et l'armurier. Les indices tirés vivent dans un `HashSet<Integer>`, qui itère par seau — l'indice
modulo 16 — puis dans l'ordre d'insertion. Avec cette règle (`listed_before`), **zéro** villageois
n'est hors ordre sur les 2319. C'est un détail de l'implémentation Java, et c'est ce que le joueur
voit.

### 5.3 Ce qui est tiré au hasard

* **Livres enchantés** (bibliothécaire, niveaux 1 à 4) : 248 livres, **les 37** enchantements
  échangeables, jamais Vitesse des âmes ni Furtivité rapide ; chaque prix dans
  `[2 + 3·niv, 6 + 13·niv]` (doublé pour un trésor), plafonné à 64. `draw_enchanted_book`.
* **Objets enchantés** : prix `base + niveau` où l'objet est enchanté à `5 + next_int(15)` niveaux
  par la sélection de la table d'enchantement (`select_enchantments`, déjà mesurée).
* **Armure teinte** : une à trois teintures au hasard, mélangées par la règle du wiki (moyenne des
  couleurs, ramenée à la moyenne des canaux les plus clairs), calculée en flottant comme le jeu.
  **Les 65 couleurs** vues s'expliquent toutes par cette règle — qui n'en atteint que 936 sur
  16,7 millions : l'accord n'est pas trivial. Trois mélanges vendus sont épinglés dans le test.
* **Flèches à effet** (archer maître) : une potion au hasard parmi les 37 brassables à effet ;
  13 sont apparues sur 26 flèches, toutes dans la liste.
* **Soupe suspecte** (fermier expert) : six entrées fixes, une par effet.
* **Cartes d'exploration** (cartographe 2 et 3) : **refusées**. Elles demandent une recherche de
  structure ; le monde d'échantillonnage n'avait pas de structures et le jeu a lui aussi sauté
  l'entrée — ces cartographes n'avaient qu'une offre, jamais deux.

---

## 6. Le prix (campagne `trade`)

Une offre de 10 (ou 9) émeraudes, une demande et un multiplicateur choisis ; la sonde a n − 1 puis n
émeraudes et choisit l'offre :

| base | multiplicateur | demande | prix demandé | n − 1 | n |
|---|---|---|---|---|---|
| 10 | 0,2 | 5 | 20 | refusé | accepté |
| 10 | 0,05 | 3 | 11 | refusé | accepté |
| 10 | 0,2 | −4 | 10 | refusé | accepté |
| 9 | 0,05 | 11 | **13** | refusé | accepté |

`prix = base + max(0, ⌊base · demande · multiplicateur⌋) + prix spécial`, borné à [1, pile], le
produit **en flottant** : 9 × 11 × 0,05F = 4,95, donc 4. Le prix spécial (réputation, héros du
village) vaut toujours 0 ici — § 12.

---

## 7. L'expérience et le niveau (campagne `trade`)

* **L'expérience du villageois** monte du `xp` de l'offre à chaque échange : 1 → 3 → 5 → 7 → 9 → 11
  pour une offre à 2.
* **Les seuils** : un échange de 2 depuis `t − 3` ne fait pas monter, depuis `t − 2` si. Pour 10,
  70, 150 et 250, les huit encadrements ; un maître à 998 reste maître.
* **L'orbe** du joueur : 6, 3, 5, 4 sur des échanges ordinaires ; 11, 8, 9, 10, 9 sur ceux qui
  franchissent un seuil — `3 + next_int(4)`, plus 5.
* **La montée** a lieu 40 ticks après la fermeture de l'écran (le niveau se lisait 1 à 1,87 s et 2
  à 2,13 s ; elle attend tant que quelqu'un commerce) ; elle ajoute deux offres du nouveau lot
  (1 offre avant, 3 après) et donne 200 ticks de Régénération — **non appliquée ici**, § 12.

---

## 8. Le réapprovisionnement (campagne `claim`)

Le bibliothécaire de la campagne, à son pupitre, en plein jour (heure de travail) :

| phase | utilisations posées (sur 12) | restock | demande après |
|---|---|---|---|
| 1 | 0, 5, 12 | au tick 82 | **−12, −2, 12** |
| 2 | 12, 12, 0 | au tick 2519 | **0, 10, 0** |
| 3 | 12, 12, 12 | **aucun** en 3883 ticks | 0, 10, 0 |

`demande ← demande + utilisations − (maximum − utilisations)`, puis les utilisations à 0 ; deux
restocks par jour au plus, le second **plus de** 2400 ticks après le premier (2437 mesurés, à
40 ticks près). Chez nous, le villageois en heure de travail va à son bloc (au plus toutes les 300
ticks) et y réapprovisionne si ces conditions tiennent.

---

## 9. Le comportement (campagne `flee`)

* **Un zombie immobile** à 5 et 7 blocs : le villageois part aussitôt, droit à l'opposé (12 et 4
  blocs). À 7,8, 8,6, 10 et 12 : des mouvements, mais qu'une promenade explique aussi bien — un
  essai par distance ne suffit pas. Nous gardons les 8 blocs du wiki pour le zombie (et sa table
  pour les autres hostiles).
* **La vitesse de fuite** : 0,21 à 0,24 bloc/tick, médiane 0,225, douze intervalles ; portée telle
  quelle (`kPanicSpeed`) : c'est au-delà des vitesses sur lesquelles la loi de marche a été ajustée.
* **Un coup** (dégât générique, ou par le joueur) : 9 blocs en 6 s, au pas de fuite jusqu'à 2,07 s,
  au pas de marche dès 4,12 s. Chez nous, 60 ticks de fuite (dans l'encadrement). Aucune partie du
  serveur n'appelle `Mob::frighten` : c'est le module des villageois qui voit la vie baisser.
* **Le jour et la nuit** : le planning du wiki (travail de 2000 à 9000, repos de 12000 à 10 le
  lendemain). La nuit, un villageois qui a revendiqué un lit y va et s'y couche (pose 2, indice 14).
  **Non mesuré** : ni le planning, ni le coucher ; le lit n'est pas marqué `occupied`.

---

## 10. La zombification et la guérison (campagnes `zombify`, `zombify_normal`, `cure`)

Mesurées, **non implémentées** :

* un zombie tue un villageois : **0/10** converti en facile, **37/90** en normal (10/30 puis
  27/60 ; la moitié du wiki donne p ≈ 0,11 en bilatéral), **10/10** en difficile ;
* le zombie villageois garde `VillagerData`, `Xp` et les `Offers` ;
* une pomme d'or sur un zombie villageois affaibli : `ConversionTime` de 3734 à 5974 sur 23
  essais (le wiki : 3600 à 6000) ;
* guéri, il redevient le même bibliothécaire de niveau 3 avec ses offres et son `Xp`.

Les indices 19 (conversion) et 20 (VillagerData) du zombie villageois sont relevés (§ 2) pour qui
fera la suite.

---

## 11. De bout en bout, sur notre serveur

`scripts/check_villagers_e2e.py` : `ov_dedicated --mobs=villager` sur un monde neuf, une sonde en
créatif, jugée sur le fil seulement :

```
villageois à l'apparition   VillagerData (plaines 2, sans métier 0, niveau 1)
bibliothécaire              oui, 13,1 s après la pose du pupitre, VillagerData (2, 9, 1)
écran                       fenêtre 4, menu 18, titre « entity.minecraft.villager.librarian »
offres, niveau 1            24 papiers → émeraude · 9 émeraudes → bibliothèque
niveau 2                    oui, métadonnée 18 = (2, 9, 2) ; orbes 8, 11, 10
offres, niveau 2            + 8 émeraudes → livre enchanté · 1 émeraude → lanterne
livre acheté                Affinité aquatique, prix affiché 8 émeraudes, payé 8
niveau 3                    oui, métadonnée 18 = (2, 9, 3), après l'achat
```

Le novice de ce tirage n'avait pas de livre (une chance sur trois) : la sonde a monté d'un niveau
en échangeant, puis acheté le livre au niveau 2, puis continué jusqu'au niveau 3. Les orbes de 8 à
11 arrivent sur **chaque** échange fait entre le franchissement du seuil et la montée effective : la
règle ajoute 5 tant que le niveau n'a pas monté. Mesuré sur le vrai serveur pour l'échange qui
franchit (§ 7) ; les suivants suivent la même règle, **non mesurés** un par un.

---

## 12. Ce qui n'est pas fait, ou pas mesuré

* **Le cerveau** : ni ragots, ni réputation (le prix spécial vaut 0), ni héros du village, ni
  golems de fer, ni cloche, ni rassemblement, ni reproduction des villageois, ni récolte des
  fermiers, ni raids, ni marchand ambulant.
* **Zombification et guérison** : mesurées (§ 10), pas faites — il faudrait brancher la mort d'un
  villageois tué par un zombie, dans le combat des mobs, qui n'est pas de ce mandat.
* **Les cartes d'exploration** : refusées (§ 5.3).
* **La Régénération** donnée à la montée de niveau : non appliquée (les effets de mob ne sont pas
  branchés ici).
* **Le type selon le biome** : un villageois de `--mobs` ou de `/summon` est de plaines ; le jeu
  tire le type du biome à l'apparition.
* **La persistance** : comme les autres mobs, un villageois n'est pas sauvé ; son métier, ses
  offres et son expérience vivent en mémoire.
* **Non mesurés** : le rayon de 48 (non contredit, pas encadré), la portée d'interaction de 6
  blocs, le planning de la journée, le coucher, la distance de fuite des hostiles autres que le
  zombie, la durée exacte de la fuite après un coup, la distance à laquelle un écran se ferme
  (16 ici), les trois autres chaudrons.
* **Le balayage des blocs de travail** est borné à ±8 en hauteur : un bloc de travail plus haut ou
  plus bas n'est pas trouvé.
* **Les clics** `clone`, `drag` et `double-clic` sur l'écran de commerce sont refusés et nommés ;
  l'écran est renvoyé tel quel.

---

## 13. Pièges payés

1. **Deviner un identifiant d'entité**, c'est perdre une campagne. Le script vérifiait au démarrage
   que `villager` et `zombie_villager` valent ce qu'il croyait : 120 et 119 écrits de mémoire,
   **108 et 120** en vrai. L'assertion l'a dit avant que le serveur ne démarre.
2. **L'ordre d'un `HashSet` Java se voit.** Deux offres d'un niveau s'affichent par indice modulo
   16, pas dans l'ordre du lot (§ 5.2). Sans ça, un quart des villageois des grands lots avaient
   leurs offres « à l'envers ».
3. **Toute pile qui s'use porte `Damage:0`** dès sa création. Un comparateur qui la prend pour une
   étiquette de commerce rejette les outils, les armures et les cisailles de tous les métiers.
4. **Un lot d'une ligne s'écrit sur une ligne** : l'expression régulière qui cherchait `\n};`
   comparait douze cellules au mauvais lot. Le vérificateur a été vérifié en voyant ses faux
   positifs disparaître un par un, pas en le croyant.
5. **Personne n'appelle `Mob::frighten`** : aucune créature de ce serveur ne fuit quand on la frappe.
   Les villageois surveillent leur propre vie ; les vaches, elles, ne fuient toujours pas — nommé,
   hors mandat.
6. **Un `cd` dans une commande reste** : le répertoire courant persiste d'un appel à l'autre, et un
   script lancé ensuite avec un chemin relatif ne trouve plus rien.

---

## 14. Fichiers

| fichier | rôle |
|---|---|
| `src/ov_gameplay/include/ov/gameplay/villager_state.hpp` | l'état : type, métier, niveau, Xp, offres, revendications |
| `src/ov_gameplay/{include/ov/gameplay,src}/villager.{hpp,cpp}` | identité, blocs de travail, planning, balayage, buts |
| `src/ov_gameplay/{include/ov/gameplay,src}/trading.{hpp,cpp}` | les lots mesurés, le tirage, le prix, l'échange, le niveau, le restock |
| `src/ov_gameplay/include/ov/gameplay/goals.hpp` | `MobBrain::villager`, `GoalContext::villagers` (blocs `villagers`) |
| `src/ov_gameplay/{include/ov/gameplay/mob_logic.hpp,src/mob_logic.cpp}` | le type, ses buts, son tick (blocs `villagers`) |
| `src/ov_server/src/merchant_session.{hpp,cpp}` | l'écran, les paquets, la métadonnée, les clics |
| `src/ov_server/src/server.cpp` | blocs `// ── villagers ──` |
| `src/ov_gameplay/tests/test_{villager,trading}.cpp` | 20 cas, dont les mesures `[parity]` |
| `src/ov_server/tests/test_merchant_session.cpp` | 4 cas : Merchant Offers contre les 144 octets capturés, le titre, Select Trade, les étiquettes |
| `scripts/measure_villagers.py` | l'oracle (10 campagnes) |
| `scripts/check_trades.py` | nos lots contre les 4298 offres |
| `scripts/check_villagers_e2e.py` | la preuve de bout en bout |

```bash
lockf /tmp/ov-vanilla.lock python3 scripts/measure_villagers.py      # ~25 min
python3 scripts/check_trades.py
python3 scripts/check_villagers_e2e.py
```
