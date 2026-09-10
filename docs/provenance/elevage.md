# Élevage — bébés, croissance, reproduction, tonte, traite

Ce dossier trace l'élevage des quatre espèces que le serveur fait vivre (vache, mouton, cochon,
poulet) : l'âge et la croissance, la nourriture, l'amour et la naissance, la tonte, la teinture, la
traite, la selle, la ponte, l'éclosion, la tentation. **Tout chiffre ci-dessous a été mesuré contre
le vrai serveur 1.20.1** (`tools/vanilla/server.jar`) par `scripts/measure_husbandry.py`, avec un
client sonde connecté — c'est la seule façon d'envoyer un `Interact` (0x10) et de voir ce que le fil
dit d'une entité. Les relevés bruts vont dans `data/vanilla/1.20.1/normalized/husbandry.json`
(gitignoré). Ce qui n'a pas été mesuré est dit à l'endroit où c'est utilisé, et regroupé au § 12.

Le résultat court :

| mesure | vanilla | nous |
|---|---|---|
| indice « bébé » | 16, booléen, sur les quatre espèces | 16 |
| boîte d'un bébé | exactement la moitié, quatre espèces | la moitié |
| hauteur des yeux d'un bébé | **pas** la moitié pour la vache (0,665) et le poulet (0,2975) | table mesurée |
| âge d'un nouveau-né (veau, poussin éclos) | −24 000 | −24 000 |
| croissance sous nourriture, 20 âges | `⌊−âge / 200⌋ × 20` ticks | idem, 9 cas `[parity]` |
| amour après le repas | `InLove` 600, Entity Event 18 | idem |
| du repas au veau (paire côte à côte) | 59–60 ticks, 5 essais | 59–62 dans le test |
| âge des parents après | 6000 | 6000 |
| orbe d'XP d'une naissance, 60 naissances | 1..7, χ² uniforme p ≈ 0,69 | `1 + next_int(7)` |
| laine par tonte, 150 moutons | 1 / 2 / 3 : 55 / 44 / 51 (χ² p ≈ 0,54) | `1 + next_int(3)` |
| couleur de l'agneau, 256 paires ordonnées | les 9 mélanges de teinture, sinon un parent (113 / 109) | idem |
| rayon de recherche du partenaire | acquis à 9,0 blocs (2/2), jamais à 9,5 (0/4) | boîte + 8 ; 8,5 oui, 9,5 non |
| ponte | `EggLayTime` 6004..11992, moyenne 8972 (n = 200) | `6000 + next_int(6000)` |
| tentation | 10 blocs oui, 11 non ; arrêt à ≈ 2,2 blocs | 10, arrêt à 2,5 |
| vitesse tentée (blocs/tick) | vache 0,1347, mouton 0,1380, cochon 0,1936, poulet 0,1349 | loi `2,1586·s²`, ≤ 2 % |
| herbe mangée, 40 adultes tondus | 1 sur **2060** par tick (max. de vraisemblance) | 1/1000 un tick sur deux |
| de bout en bout sur notre serveur | — | cœurs 2/2, veau (16 = vrai), veau adulte, 3 laines, lait |

---

## 1. Méthode

### 1.1 Une sonde qui clique

`Hand` (dans `measure_husbandry.py`) est le client sonde de `capture_entity_packets.py`, lu sur un
fil à lui : il répond aux keep-alive et aux téléportations pendant que la campagne envoie des
`Interact` depuis le fil principal (les envois passent par un verrou). Il enregistre `Spawn Entity`,
`Spawn Experience Orb`, `Set Entity Metadata` et `Entity Event`.

Pour relier une entité invoquée depuis la console à son identifiant réseau, chaque `summon` porte
un UUID choisi, `[I;0x4F56,0,0,n]` : le `Spawn Entity` le contient, et le même UUID écrit
`00004f56-0000-0000-0000-…` sert de sélecteur à la console (`data get entity <uuid> Age`).

L'objet en main est posé par `item replace entity ovhand weapon.mainhand with …` ; « consommé »
se lit sur `SelectedItem.Count`. La sonde est en survie.

### 1.2 Un `summon` avec NBT ne passe pas par `finalizeSpawn`

Un mouton invoqué avec un NBT quelconque a la couleur de son NBT (blanc par défaut) et non une
couleur tirée ; un animal invoqué ainsi n'est jamais un bébé tiré au hasard. Toutes les campagnes
invoquent donc avec un NBT explicite — c'est ce qui rend chaque animal connu d'avance.

### 1.3 Deux pièges payés

* **Un couloir d'un bloc de large, et plus rien ne se reproduit.** La première campagne `radius`
  mettait chaque paire dans un couloir de verre d'un bloc : **aucune** vache amoureuse n'a bougé
  d'un millimètre, pas même à quatre blocs, et aucun veau en 600 ticks — pendant que les témoins
  non amoureux se promenaient. Le couloir de trois blocs n'a pas mieux fait. La campagne `pair`
  (cinq montages à quatre blocs : herbe nue, couloir, nourries par la sonde, enclos 2 × 1, NBT
  minimal) a donné un veau **partout** ; la campagne `reach`, sur herbe nue et à moins de 60 blocs
  de la sonde, a enfin mesuré le rayon (§ 5). Pourquoi les couloirs éloignés ne donnaient rien
  **n'est pas élucidé** — la cause n'est ni l'amour posé par NBT, ni le verre (le montage B de
  `pair` est un couloir et a marché). Les deux campagnes ratées restent dans le script, nommées.
* **La métadonnée d'un objet au sol arrive deux fois.** La première lecture de la tonte comptait
  2, 4 et 6 laines ; chaque pile était comptée par ses deux paquets. Le compte prend maintenant la
  dernière pile vue par entité, et les 150 tontes relues donnent 55 / 44 / 51.

---

## 2. La métadonnée (campagne `meta`)

Un champ NBT à la fois contre une référence de la même espèce, comme la table du zombie de
`ov/protocol/entity.hpp` :

| NBT | indice | type | valeur |
|---|---|---|---|
| `Age:-24000` (vache, mouton, cochon, poulet) | 16 | booléen | vrai |
| `Color:14b` (mouton) | 17 | octet | 14 |
| `Sheared:1b` (mouton blanc) | 17 | octet | 16 |
| `Saddle:1b` (cochon) | 17 | booléen | vrai |
| `InLove:600` (vache) | — | — | **rien** : l'amour n'est pas sur le fil |

Un apparition n'envoie que ce qui diffère du défaut : un veau porte l'indice 16, une vache adulte
ne le porte pas. Nos mises à jour (veau devenu adulte, mouton tondu) envoient le champ même à sa
valeur par défaut — un client doit apprendre le `faux`. Constantes `kAgeableBaby`, `kSheepFleece`,
`kSheepSheared`, `kPigSaddle`.

---

## 3. Le bébé (campagnes `box`, `growth`)

Bissection de la boîte (la méthode et l'étalonnage de `measure_entities.py`), marqueur aux yeux,
attribut de vitesse :

| espèce | adulte l × h / yeux | bébé l × h / yeux | vitesse |
|---|---|---|---|
| vache | 0,9 × 1,4 / 1,3 | 0,45 × 0,7 / **0,665** | 0,2 = 0,2 |
| mouton | 0,9 × 1,3 / 1,235 | 0,45 × 0,65 / 0,6175 | 0,23 = 0,23 |
| cochon | 0,9 × 0,9 / 0,765 | 0,45 × 0,45 / 0,3825 | 0,25 = 0,25 |
| poulet | 0,4 × 0,7 / 0,644 | 0,2 × 0,35 / **0,2975** | 0,25 = 0,25 |

La boîte est exactement la moitié ; les yeux **ne le sont pas** pour la vache et le poulet (la
moitié donnerait 0,65 et 0,322). Un facteur unique aurait été faux sur deux espèces sur quatre en
ayant l'air juste : `AnimalKind::baby_eye_height` porte les quatre valeurs mesurées. Le test l'a
attrapé, `0,65 ≠ 0,665`.

**L'âge monte d'un par tick**, y compris avec `NoAI:1b` : huit veaux, quatre sans IA, relus trois
fois — 43 ticks de jeu, 43 d'âge, les huit. À 0, l'indice 16 repasse à faux. Chez nous, l'âge vieillit
dans `Mob::tick_husbandry`, avant les buts, que le mob ait un niveau ou non.

**La vitesse d'un bébé est celle de l'adulte** : veau tenté 0,1348 bloc/tick contre 0,1347 ; agneau
0,1380 contre 0,1380 ; poussin 0,1349 contre 0,1348. Le porcelet fait 0,1854 contre 0,1936 (−4 %),
écart non expliqué, un seul essai.

---

## 4. La nourriture (campagnes `feed`, `food`)

### 4.1 Quels objets

1.20.1 **n'a pas** de tags d'objets `#minecraft:cow_food` et consorts (ils arrivent plus tard ; le
datapack généré n'a que `fox_food`, `piglin_food`, `sniffer_food`). Chaque espèce a donc reçu les
18 mêmes candidats, un `Interact` chacun :

| espèce | mangé | refusé (parmi les 18) |
|---|---|---|
| vache, mouton | blé | tout le reste, botte de foin comprise |
| cochon | carotte, pomme de terre, betterave | carotte dorée comprise |
| poulet | graines de blé, melon, citrouille, betterave, torchflower, **pitcher pod** | blé |
| lapin | carotte, carotte dorée, pissenlit | (mesuré, espèce non livrée) |

### 4.2 Ce que le repas fait

| cas | vanilla | `feed()` |
|---|---|---|
| adulte, `Age` 0, pas amoureux | `InLove` 600, objet consommé, Entity Event **18** | `Love` |
| adulte déjà amoureux | rien, **objet gardé** | `Refused` |
| parent au repos (`Age` 3000) | rien, objet gardé | `Refused` |
| bébé | grandit, objet consommé | `Grew` |

**La croissance** : vingt âges de −24 000 à −1. L'âge est relu avant et après le repas, et les deux
lectures sont séparées de 9 ticks (le témoin « parent au repos » descend de 9 sur la même
fenêtre) ; le gain est donc la différence moins 9 :

| âge au repas | −23 994 | −19 994 | −11 994 | −5994 | −1995 | −395 | −194 | −94 | −13 |
|---|---|---|---|---|---|---|---|---|---|
| gain | 2380 | 1980 | 1180 | 580 | 180 | 20 | **0** | 0 | 0 |

Soit `⌊−âge / 200⌋ × 20` : un dixième du restant, compté en **secondes entières**. C'est aussi le
calcul flottant `(int)(−âge / 20 × 0,1F) × 20` pour tout âge possible — 0,1F est un peu au-dessus
d'un dixième, le produit ne tombe jamais sous l'entier. **Le blé est consommé même quand le gain
est nul** (à −194). Conséquence observable : la nourriture seule ne fait jamais un adulte ; sous
200 ticks restants, il faut attendre.

---

## 5. La reproduction (campagnes `love`, `xp`, `reach`)

### 5.1 Deux vaches nourries par la sonde, cinq fois

| | essai 0 | 1 | 2 | 3 | 4 |
|---|---|---|---|---|---|
| repas → veau (ticks) | 60 | 60 | 60 | 59 | 59 |
| `Age` du veau à la relecture | −23 998 | −23 997 | −23 997 | −23 998 | −23 998 |
| `Age` des parents | 5997 / 5997 | 5997 / 5996 | … | … | 5998 / 5997 |
| `InLove` des parents après | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| orbe | 3 | 1 | 4 | 4 | 6 |
| statistique `animals_bred` | 1 | 2 | 3 | 4 | 5 |

Trois Entity Event 18 par essai : un par vache nourrie, **plus un sur l'un des parents** —
interprété comme la naissance, et c'est ce que nous envoyons. `BreedGoal` compte 60 ticks puis
exige les deux à moins de 3 blocs ; la condition des 3 blocs **n'est pas mesurée** (une paire côte à
côte ne la teste pas).

### 5.2 Soixante naissances

Soixante paires `InLove:600` en enclos 2 × 1 : 60 veaux, 60 orbes, valeurs
1 / 2 / 3 / 4 / 5 / 6 / 7 = 6 / 9 / 11 / 5 / 11 / 8 / 10 (χ² contre l'uniforme 3,9, 6 ddl,
p ≈ 0,69). Un amour posé par NBT, sans joueur, **se reproduit aussi** : l'orbe ne dépend pas d'un
joueur.

### 5.3 Le rayon

`reach` : paires sur herbe nue, chacune seule, déplacement de rapprochement en 80 ticks :

| écart (blocs) | 6 | 7 | 7,5 | 8 | 8,5 | 9 | 9,5 | 10 |
|---|---|---|---|---|---|---|---|---|
| rapprochement | 4,6 / 5,0 | 5,4 / 4,4 | 6,5 / 6,2 | 7,0 / 6,8 | 6,9 / **0** | 7,9 / 4,1 | **0 / 0** | **0 / 0,2** |

Acquis jusqu'à 9,0, jamais à 9,5 : la frontière est dans `]9,0 ; 9,5]` en écart d'invocation.
Notre modèle — la boîte du chercheur grossie de 8 sur chaque axe doit toucher celle du partenaire,
soit 8,9 de centre à centre pour deux vaches — tombe **0,1 bloc sous** l'encadrement mesuré.
L'écart n'est pas résolu (les positions de départ ne sont relues qu'après quelques ticks, et le
8,5 à 0 montre qu'un essai isolé peut rater dans le rayon) : nommé, pas corrigé à la main.

---

## 6. La tonte, la teinture, les couleurs héritées

**Tonte** (150 moutons blancs, cisailles) : 1, 2, 3 laines = 55, 44, 51 (χ² 1,24, p ≈ 0,54) ; chaque
laine est une entité d'objet à part ; l'indice 17 passe à 16 ; les cisailles prennent **1** point
d'usure par tonte (`Damage: 50` après 50). Un agneau et un mouton déjà tondu : rien, aucune usure.
Un mouton rouge donne de la laine rouge.

**Teinture** : colorant rouge sur mouton blanc → `Color` 14, un colorant consommé ; le même colorant
sur un mouton déjà rouge → rien, gardé ; un agneau se teint ; **un mouton tondu ne se teint pas**
(gardé).

**Couleurs héritées** (`inherit`) : les 256 paires ordonnées de couleurs, chacune dans son enclos,
`InLove:600`. **256 agneaux.** Exactement 18 paires ont donné une couleur qu'aucun parent n'avait :
les neuf recettes de teinture sans forme à deux colorants du datapack (bleu + vert → cyan, noir +
blanc → gris, bleu + blanc → bleu clair, gris + blanc → gris clair, vert + blanc → vert clair,
violet + rose → magenta, rouge + jaune → orange, rouge + blanc → rose, bleu + rouge → violet),
**dans les deux ordres**. Toutes les autres paires de couleurs différentes ont donné un parent :
113 fois le premier, 109 le second. `test_breeding.cpp` relit les recettes générées et vérifie que la
table de `mixed_colour` est exactement celle-là (9 trouvées).

---

## 7. La repousse (campagne `regrow`)

Quarante adultes tondus et dix agneaux, seuls dans des enclos 1 × 1 d'herbe, relus toutes les
cinq secondes pendant 2575 ticks.

* **Adultes** : 28 sur 40 ont mangé. Le maximum de vraisemblance du taux (censure par
  intervalles, 36 ticks entre la décision et la bouchée) est **1 sur 2060 par tick** — pas le
  « 1 sur 1000 » qu'on lit partout. La lecture retenue : le sélecteur de buts du jeu n'offre un
  départ à un but qu'**un tick sur deux** ; 1/1000 un tick sur deux fait 1/2000. `EatGrassGoal` tire
  donc sur les ticks pairs seulement. *Hypothèse cohérente, pas une mesure du sélecteur.*
* **Agneaux** : les dix ont mangé avant 307 ticks (1/50 un tick sur deux donne 0,95).
* **Un agneau qui mange vieillit d'une minute** : relus à +2575, leurs âges valaient
  −24 000 + 2575 + **1200, 2400 ou 3600** — un, deux ou trois repas. Mesuré, et fait.
* L'herbe sous le mouton devient de la **terre** ; un Entity Event **10** accompagne chaque repas
  (54 captés, tous 10) — nous l'envoyons quand le mouton baisse la tête.

---

## 8. La tentation (campagne `tempt`)

La sonde en (0,5 ; 0,5), la nourriture en main, un animal à la fois à une distance donnée :

* **Portée** : à 4, 6, 8, 9, 10 blocs la vache vient (2 essais chacun) ; à 11, 12, 14 elle ne vient
  pas ; main vide à 6, elle ne vient pas.
* **Arrêt** : 2,12 à 2,34 blocs du joueur — cohérent avec un arrêt sous 2,5 et l'élan qui reste.
* **Vitesse**, en blocs par tick sur la partie établie : vache 0,1347, mouton 0,1380, cochon
  0,1936, poulet 0,1349 ; une vache qui se promène, 0,0863.

**Une loi de marche, pas un rapport.** `mobs.md` convertit l'attribut en vitesse en le divisant par
deux ; c'est juste pour le zombie (0,23 → 0,115 contre 0,11419), mais la vache qui se promène
(attribut 0,2) irait à 0,1 contre **0,0863 mesuré** : 16 % trop vite. Les sept vitesses mesurées
(zombie, promenade, cinq tentations) tiennent toutes dans `v = 2,1586 · s²` à 0,5 % près, où
`s` = attribut × modificateur du but. Les modificateurs qui en sortent sont ronds : vache 1,25,
mouton 1,1, cochon 1,2, poulet 1,0. `walk_blocks_per_tick` porte la loi ; les buts de l'élevage
(reproduction, tentation) en tirent leur vitesse. **Les buts existants (errance, panique) gardent la
division par deux** — ce n'est pas le mandat de ce dossier, et c'est dit ici pour qui le reprendra.

---

## 9. La ponte et l'éclosion (campagnes `eggs`, `hatch`)

* 200 poulets frais : `EggLayTime` de 6004 à 11 992, moyenne 8972 (uniforme sur [6000 ; 12 000[
  donne 8999,5 ± 122) → `egg_interval` = `6000 + next_int(6000)`, tiré à la naissance du `Mob`.
* 20 poules à `EggLayTime:20` : un œuf chacune en moins de 60 ticks, et un nouveau délai dans
  [6000 ; 12 000[. **Avec `NoAI`, même chose** : la ponte n'est pas un but.
* 20 poussins à `EggLayTime:20` : aucun œuf, et le compteur **reste à 20** — il ne descend pas.
* 400 œufs lancés : **49 poulets** (espérance 54,7), âges −23 944 à −23 941, soit −24 000 plus les
  57 ticks écoulés : un œuf fait éclore un **poussin**. `projectiles.cpp` faisait éclore des adultes
  (« l'indice bébé n'étant pas mesuré ») ; le rappel d'éclosion du serveur appelle maintenant
  `Husbandry::make_baby`.

---

## 10. La traite et la selle (campagne `work`)

* Seau sur vache adulte : le seau devient un **seau de lait** dans la main. Sur un veau : rien. Sur
  une pile de deux seaux : un seau reste, le lait va dans l'inventaire.
* Selle sur cochon adulte : `Saddle:1b`, la selle consommée, indice 17 à vrai. Sur un porcelet :
  rien, selle gardée.

`combat.md` dit que « le seau agit par `Use Item` avec un lancer de rayon serveur » : c'est vrai du
seau **sur un fluide**. Sur une vache, le client envoie `Interact`, et c'est par là que nous
traitons la traite.

---

## 11. De bout en bout, sur notre serveur

`scripts/check_husbandry_e2e.py` : `ov_dedicated --mobs=cow,cow,sheep`, un monde neuf, la sonde en
créatif (`Set Creative Slot` est sa seule façon d'avoir du blé ; le créatif ne consomme rien, ce qui
ne change aucune des cinq preuves).

```
cœurs (statut 18)    2/2
veau                 oui, 2,7 s après le second repas, métadonnée 16 = vrai, orbe(s) [7]
veau adulte          oui, 22,9 s après le premier repas (1200 s sans nourriture)
tonte                3 laine(s) blanche(s), indice 17 = 16
traite               oui
```

Le veau est nourri 46 fois : 24 000 × 0,9⁴⁶ ≈ 190 ticks restants, sous le seuil où la nourriture ne
sert plus, et l'horloge fait le reste. Le journal du serveur montre la naissance, la croissance et
un mouton qui a brouté l'herbe du monde neuf (`1 grazed`).

---

## 12. Ce qui n'est pas fait, ou pas mesuré

* **Aucune nouvelle espèce.** Lapin, cheval / âne / mule, loup, chat : **non livrés**. La liste
  de nourriture du lapin est mesurée (§ 4.1) et c'est tout.
* **La statistique `animals_bred`** (mesurée : +1 par naissance au joueur qui a nourri) : ce
  serveur ne tient aucune statistique. `love_cause` est gardé pour le jour où.
* **La persistance** : ce serveur ne sauve pas les mobs ; `Age`, `InLove`, `Sheared`, `Color`,
  `Saddle`, `EggLayTime` vivent en mémoire seulement.
* **Monter un cochon sellé** (et la carotte sur un bâton), et la selle rendue à la mort : non faits.
* **La couleur d'un mouton apparu naturellement** : nos moutons sont blancs ; la distribution du
  jeu n'est pas mesurée ici.
* **Un bébé à l'apparition naturelle** : non fait.
* **Non mesurés** : la condition « à moins de 3 blocs » de la naissance ; la position exacte du
  veau (nous : sur le parent qui a conclu) et de l'orbe ; le délai de 100 ticks avant qu'une
  tentation reprenne ; `FollowParentGoal` ; la règle créative du lait (garder le seau, ajouter du
  lait s'il n'y en a pas) ; la portée d'interaction de 6 blocs (la sonde était toujours à moins de
  2) ; l'usure de cisailles tenues dans la main secondaire (**pas appliquée**, nommé dans le code) ;
  `mobGriefing` (le mouton mange toujours l'herbe ; la mesure est au défaut, activé).
* **Le rayon du partenaire** : 0,1 bloc d'écart non résolu entre le modèle et l'encadrement (§ 5.3).

---

## 13. Fichiers

| fichier | rôle |
|---|---|
| `src/ov_gameplay/include/ov/gameplay/animal.hpp` | l'état d'un animal, les tentateurs, les événements |
| `src/ov_gameplay/{include/ov/gameplay,src}/breeding.{hpp,cpp}` | les règles, `TemptGoal`, `EatGrassGoal`, le tick d'élevage du `Mob` |
| `src/ov_gameplay/src/goals.cpp` | `BreedGoal` : partenaire amoureux dans la boîte, naissance en événement (bloc `husbandry`) |
| `src/ov_gameplay/src/mob_logic.cpp` | priorités des buts, premier œuf, appel du tick d'élevage (blocs `husbandry`) |
| `src/ov_protocol/include/ov/protocol/entity.hpp` | les indices 16 et 17 mesurés |
| `src/ov_server/src/husbandry.{hpp,cpp}` | clics, naissances, œufs, herbe, métadonnée |
| `src/ov_server/src/server.cpp` | blocs `// ── husbandry ──` (déclaration, métadonnée d'apparition, `Interact`, éclosion, hôte, tick) |
| `src/ov_gameplay/tests/test_breeding.cpp` | 13 cas, dont les mesures `[parity]` |
| `scripts/measure_husbandry.py` | l'oracle (17 campagnes, dont `radius` et `pair` gardées pour le § 1.3) |
| `scripts/check_husbandry_e2e.py` | la preuve de bout en bout |

```bash
python3 scripts/measure_husbandry.py meta box growth feed food love xp reach inherit \
    shear regrow eggs hatch tempt work
python3 scripts/check_husbandry_e2e.py
```
