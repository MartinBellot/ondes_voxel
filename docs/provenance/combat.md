# Combat — frapper, utiliser, manger, user

Ce qui suit est établi par **mesure contre le vrai serveur 1.20.1**
(`tools/vanilla/server.jar`, SHA-1 `84194a2f286ef7c14ed7ce0090dba59902951553`),
par `scripts/measure_combat.py` et `scripts/check_entity_loot.py`. Aucun nombre
de ce document n'a été recopié d'un résumé : chacun a une campagne, un protocole
et un chiffre.

Les résultats bruts vont dans `data/vanilla/1.20.1/normalized/combat.json`
(gitignoré, régénérable). Les tables retenues vivent dans
`src/ov_gameplay/src/{combat,durability,item_use,loot}.cpp` et sont rejouées par
`src/ov_gameplay/tests/test_{combat,item_use,durability,entity_loot}.cpp`.

---

## 0. Comment le serveur a été interrogé

Trois instruments, et le choix de l'instrument est encore la moitié du travail.

**La console.** `data get entity <cible> <chemin>` lit le NBT, `attribute <cible>
<attribut> get` lit un attribut *après* application des modificateurs de l'objet
tenu, `loot give <joueur> kill <entité>` tire une table de butin sans tuer.
Trois oracles propres.

**Un client sonde.** `Fighter` étend le `Probe` de
`scripts/capture_entity_packets.py` avec les quatre verbes : `Interact` (0x10),
`Use Item On` (0x31), `Use Item` (0x32), `Swing Arm` (0x2F), plus `Player
Action` pour miner. Tout ce qui vient d'un *joueur* passe par lui : le serveur
n'a aucune commande pour frapper.

**Les paquets reçus par cette sonde.** `Set Entity Velocity` (0x54) porte le
recul en 1/8000 de bloc par tick ; `Block Update` (0x0A) porte l'**id d'état**
résultant d'une interaction. Le second est ce qui a rendu la campagne
d'utilisation possible : `/data get block` ne répond que pour les blocs qui ont
une *block entity* et rend « pas de block entity » pour une terre labourée, une
bougie allumée et tout ce qui compte ici.

---

## 1. Les dégâts d'arme et le refroidissement

**Deux routes indépendantes, trente-quatre objets, accord exact.**

La première : `/attribute <joueur> minecraft:generic.attack_damage get` et
`…attack_speed get`, l'objet en main. Les modificateurs d'attribut de l'objet
sont repliés dans la carte d'attributs du joueur, donc la console répond avec le
nombre que le jeu utilisera.

La seconde : un coup **pleinement chargé** dans un mouton à 1024 PV, sa `Health`
relue. Les deux ont donné le même nombre pour les trente-quatre objets, à la
décimale.

| famille | bois | pierre | fer | or | diamant | netherite |
|---|---|---|---|---|---|---|
| épée — dégâts | 4 | 5 | 6 | **4** | 7 | 8 |
| épée — vitesse | 1,6 | 1,6 | 1,6 | 1,6 | 1,6 | 1,6 |
| hache — dégâts | 7 | 9 | 9 | **7** | 9 | 10 |
| hache — vitesse | 0,8 | 0,8 | **0,9** | **1,0** | 1,0 | 1,0 |
| pioche — dégâts | 2 | 3 | 4 | **2** | 5 | 6 |
| pioche — vitesse | 1,2 | 1,2 | 1,2 | 1,2 | 1,2 | 1,2 |
| pelle — dégâts | 2,5 | 3,5 | 4,5 | **2,5** | 5,5 | 6,5 |
| pelle — vitesse | 1,0 | 1,0 | 1,0 | 1,0 | 1,0 | 1,0 |
| houe — dégâts | 1 | 1 | 1 | 1 | 1 | 1 |
| houe — vitesse | 1,0 | 2,0 | 3,0 | **1,0** | 4,0 | 4,0 |

Trois choses ne se devinent depuis aucune autre :

* **L'or n'est pas un palier ici.** Une épée en or frappe comme une épée en
  bois, une houe en or frappe à la vitesse d'une houe en bois — alors qu'en
  minage l'or est le palier *le plus rapide*. Deux ordres pour un matériau.
* **Les vitesses de hache ne forment pas une série** : bois et pierre partagent
  0,8, le fer prend 0,9, diamant et netherite partagent 1,0.
* **Les houes vont à l'envers** : leurs dégâts sont ceux de la main nue à tous
  les paliers, seule leur vitesse monte.

Le trident : 9 dégâts, vitesse 1,1. La main nue, un bâton et une paire de
cisailles : 1,0 et 4,0 — la ligne « main nue » est mesurée et non supposée.

Le refroidissement est `20 / vitesse` ticks : 12,5 pour toute épée, 25 pour une
hache en bois, 5 pour une main nue. **Ce n'est pas toujours un entier**, et la
section suivante montre que ça compte.

---

## 2. La jauge d'attaque — la mesure principale

**Protocole.** Le bot frappe une cible d'*amorçage*, attend un délai commandé,
puis frappe une cible **fraîche**. Fraîche parce qu'un second coup dans les
vingt ticks d'invulnérabilité ne pose que la différence : mesurer la jauge à
travers cette fenêtre mesure la fenêtre. Délai balayé toutes les 25 ms —
un demi-tick — de 0 à 775 ms, deux répétitions.

Le résultat est un **escalier**, une marche par tick, et chaque marche a été
retrouvée à l'identique aux deux répétitions.

Épée en diamant, 7 dégâts, refroidissement 12,5 ticks :

| ticks | mesuré | `7 · (0,2 + 0,8·f²)`, `f = (t+0,5)/12,5` |
|---|---|---|
| 0 | 1,4089 | 1,40896 |
| 1 | 1,4806 | 1,48064 |
| 2 | 1,6240 | 1,62400 |
| 3 | 1,8391 | 1,83904 |
| 4 | 2,1257 | 2,12576 |
| 5 | 2,4841 | 2,48416 |
| 6 | 2,9143 | 2,91424 |
| 7 | 3,4160 | 3,41600 |
| 8 | 3,9894 | 3,98944 |
| 9 | 4,6346 | 4,63456 |
| 10 | 5,3514 | 5,35136 |
| 11 | 6,1398 | 6,13984 |
| ≥ 12 | 7,0000 | 7,00000 |

**13 marches sur 13 identiques**, à l'arrondi près du flottant que le serveur
imprime (6·10⁻⁵ au pire). Hache en diamant, 9 dégâts, refroidissement 20 ticks :
1,8045 · 1,8405 · 1,9125 au début, 8,6445 puis 9,0 à la fin — **5 marches sur
5**, avec un refroidissement différent, ce qui teste la formule et non une
tabulation.

Ce que la mesure établit et qu'aucun résumé ne dit :

* Le décalage est **un demi-tick**, pas zéro. Un coup porté au même tick que le
  précédent vaut 1,4089 et non 1,4. Sans ce demi-tick, la première marche est
  fausse de 9·10⁻³ — invisible en jeu, et le test `test_combat.cpp` la refuse.
* Le seuil « chargé » est **f > 0,9** : c'est lui qui ouvre le coup critique, le
  balayage et le recul de sprint, et il est identifié par la section 4.

---

## 3. Le coup critique

Bot en chute libre, épée en diamant, cible fraîche : **10,5** contre **7,0** au
sol. Multiplicateur **1,5**, exactement.

Le multiplicateur porte sur les dégâts de l'arme, pas sur le bonus
d'enchantement : Tranchant V ajoute 3 qui ne sont jamais multipliés.

**Deux pièges payés sur cette campagne.**

La première version lâchait le bot de **douze blocs** : la cible au sol était
alors **hors de portée d'interaction** — six blocs, mesurés contre la boîte de
la cible — et le paquet d'attaque était jeté sans un mot. La campagne a rapporté
« un coup critique fait 0,0 dégât ». La chute est maintenant de trois blocs.

Et rien ne vérifiait que le serveur avait crédité la moindre distance de chute.
`FallDistance` est désormais relu et reporté avec le résultat : une mesure dont
la précondition n'est pas vérifiée n'est pas une mesure (piège 13).

Six conditions disqualifient un critique, et elles sont toutes vérifiées :
au sol, distance de chute nulle, dans l'eau, sur une échelle, aveuglé, en
sprint. Un jeu qui n'en teste que trois rend les critiques deux fois trop
fréquents.

---

## 4. Le balayage

Deux moutons à **0,9 bloc** du bot de part et d'autre : la boîte de balayage est
celle de l'*attaquant* agrandie d'un bloc en x et z, pas celle de la victime.
C'est pourquoi l'espacement de trois blocs que toutes les autres campagnes
utilisent ne balaie jamais rien — et c'est exactement pour ça qu'il est utilisé
partout ailleurs.

| arme | coup direct | mouton d'à côté |
|---|---|---|
| épée en diamant, chargée | 7,0 | **1,0** |
| épée en diamant, non chargée | 7,0 | **0,0** |
| épée en bois, chargée | 4,0 | **1,0** |
| épée en netherite, chargée | 8,0 | **1,0** |
| hache en diamant, chargée | 9,0 | **0,0** |

Donc : **1,0 point fixe**, indépendant des dégâts de l'arme, et **seulement pour
une épée**. Avec Fil du rasoir, le jeu ajoute `niveau/(niveau+1)` des dégâts du
coup ; sans, exactement un.

**Le piège.** Les deux premières versions de cette campagne rapportaient que le
balayage partait aussi sur un coup **non chargé**. Deux raisons cumulées :

1. `sleep(0.05)` avant le coup ne rend pas le coup non chargé — les aller-retours
   console qui montent le décor avaient déjà pris une seconde et la jauge était
   pleine avant que le minuteur ne démarre ;
2. frapper deux fois de suite ne le mesure pas non plus, parce que le **premier**
   coup balaie et que ses dégâts sont encore sur la seconde cible quand on lit
   la seconde frappe.

La version retenue frappe, remet la seconde cible à plein, refrappe, et rapporte
l'**écart de ticks que le serveur lui-même a vu** entre les deux coups —
`time query gametime` de part et d'autre. L'écart mesuré est **2 ticks**, donc
`f = 2,5/12,5 = 0,2 < 0,9`, et le balayé prend **0,0**. C'est ce chiffre-là qui
place le seuil, pas une valeur retenue de mémoire.

---

## 5. Le recul

Lu sur le paquet `Set Entity Velocity` (0x54) reçu par la sonde, en 1/8000 de
bloc par tick. Mouton à `x = 3,5`, bot à `x = 0,5`, lacet 0 (donc regard vers
+z), résistance au recul nulle.

| cas | vitesse communiquée (x, y, z) |
|---|---|
| main nue | (0,4 · 0,4 · 0,0) |
| épée | (0,4 · 0,4 · 0,0) |
| main nue + sprint | (0,2 · 0,4 · **0,5**) |
| épée + sprint | (0,2 · 0,4 · **0,5**) |
| Recul I, sans sprint | (0,2 · 0,4 · **0,5**) |
| Recul II, sans sprint | (0,2 · 0,4 · **1,0**) |

**C'est deux impulsions, pas une somme**, et la forme de la mesure le dit :

1. celle du coup lui-même, **0,4**, le long de la ligne attaquant → victime —
   d'où le 0,4 sur x quand la cible est sur +x ;
2. celle du Recul et du sprint, **0,5 par niveau** (un sprint compte pour un
   niveau), le long du **regard de l'attaquant**, pas de la ligne entre les
   deux. À lacet 0 le regard est +z, et le 0,5 apparaît sur **z**, là où le
   mouton n'était pas.

La seconde impulsion **divise par deux** ce que la première a laissé : le 0,4 sur
x devient 0,2. Un modèle qui additionne les deux dans la même direction donne
(0,9 · 0,4 · 0,0) et se trompe sur deux axes sur trois.

Le terme vertical : `min(0,4 ; v_y/2 + impulsion)` pour une victime au sol, et
`v_y` inchangé pour une victime déjà en l'air — c'est pourquoi on ne peut pas
faire rebondir un mob indéfiniment.

Les paquets suivants (0,384125 · 0,3615 · 0,34025) sont le frottement des ticks
d'après et ne font pas partie de l'impulsion.

**Lacune nommée.** Le jeu calcule ce regard avec `Mth.sin`/`Mth.cos`, la table
de 65536 flottants (piège 1), pas avec libm. Cette table vit dans `ov_worldgen`,
couche 10, au-dessus de `ov_gameplay` ; `knockback_direction()` utilise donc
libm. L'écart est nul aux quatre lacets cardinaux — c'est pourquoi la mesure
ci-dessus tombe juste au dernier chiffre — et ailleurs inférieur à une unité du
1/8000 de la ligne, mais il peut arrondir à l'unité voisine. Descendre `mth_sin`
dans `ov_math` fermerait la lacune ; ce n'est pas fait ici.

---

## 6. La durabilité

**Dégâts d'usage par action**, mesurés en faisant faire l'action une fois à un
outil neuf et en relisant `Damage` :

| action | coût |
|---|---|
| casser un bloc, outil quelconque | 1 |
| casser un bloc, **épée** | 2 |
| casser un bloc **instantané** (torche) | 0 |
| frapper un mob, **épée** | 1 |
| frapper un mob, pioche / hache / pelle / houe | 2 |
| frapper un mob, **cisailles** | 0 |
| labourer, allumer, écorcer, tracer un chemin | 1 |

L'asymétrie est le résultat : une épée coûte **deux** pour creuser et **un** pour
frapper, tous les autres outils l'inverse. Le jeu fait payer double l'outil qu'on
détourne de son usage.

**Maximum de durabilité**, par bissection : un outil livré à `Damage: N` qui fait
une action survit ou non, et le plus grand `N` qui survit est la réponse en
treize aller-retours au lieu de deux mille. Le maximum est `N + coût + 1` — le
`coût` dans cette formule n'est pas décoratif : une épée s'use de deux par bloc,
donc son plus grand survivant est trois sous son maximum et non deux. Lire
`N + 2` partout donnait 58 pour une épée en bois et 59 pour une pioche en bois,
qui sont le même palier.

Valeurs confirmées (`per break 1`, campagne `durability`), identiques pour les
cinq familles d'un même palier : bois **59**, pierre **131**, fer **250**, or
**32**, diamant **1561**, netherite **2031**. Les cisailles : **238**, mesurées
sur une toile d'araignée et confirmées trois fois de chaque côté de la borne.

**Contre-épreuve indépendante.** Une pioche en or neuve a été usée bloc par bloc
jusqu'à sa disparition : elle a duré **32 cassages**, exactement ce que la
bissection annonce. Le `+ coût + 1` n'est donc pas une convention choisie pour
faire tomber juste — c'est le nombre d'utilisations que l'outil a réellement.

**Deux pièges payés ici, et ils étaient invisibles.**

1. **Chaque outil doit casser un bloc qu'il peut casser.** La première version
   cassait de la *pierre* avec tous les outils, y compris une pelle et une épée
   en bois : la pioche n'y arrivait pas en deux secondes, le coup ne se
   terminait jamais, l'outil ne prenait aucun dégât, et la bissection qui suit
   annonçait une pelle en bois à **2926** points de durabilité. Un chiffre aussi
   gros est ce qui trahit ; un chiffre simplement faux serait passé.

2. **Une bissection suppose ses réponses monotones.** Un seul coup qui, pour une
   raison de synchronisation, ne casse pas le bloc se lit « l'outil a survécu »,
   pousse la borne vers le haut et y reste. C'est exactement ce qui a produit une
   hache en fer à **258** et une houe en or à **1503** dans un tableau dont
   toutes les autres lignes étaient justes. La version retenue **vérifie que le
   bloc a réellement disparu** (`execute if block … minecraft:air`), réessaie
   sinon, et **confirme la borne** trois fois de chaque côté.

   Cause racine de tous ces écarts, et elle est unique : **le cassage
   instantané**. Quand la progression d'un coup atteint 1 en un seul tick,
   vanilla ne prend pas le même chemin que pour un bloc qui demande plusieurs
   ticks, et la bissection y remonte jusqu'à sa borne haute — 4089 pour des
   cisailles sur des feuilles, 1185 pour une pelle en or sur de la terre. Deux
   choses le déclenchaient : l'effet **Hâte II** donné au bot, qui rendait
   instantané tout outil en or sur un bloc tendre, et le choix des feuilles pour
   les cisailles (vitesse 15 pour une dureté de 0,2, soit 2,5 de progression par
   tick).

   La Hâte a été retirée et chaque outil a maintenant un bloc qui lui demande
   plusieurs ticks — les cisailles cassent une **toile d'araignée**, dureté 4,
   soit huit ticks. Une mesure dont les sujets ne passent pas tous par le même
   code n'est pas une mesure (piège 13). Ce que ce chemin instantané fait
   réellement à la durabilité **n'est pas établi ici** : il est évité, pas
   expliqué.

   Les cinq lignes concernées ont été reprises avec le montage corrigé et
   toutes les cinq sont maintenant **confirmées** :

   | outil | avant | après |
   |---|---|---|
   | cisailles | 4089 (non confirmé) | **238** |
   | pelle en or | 1185 | **32** |
   | houe en or | 1503 | **32** |
   | hache en fer | 258 | **250** |
   | pioche en or | 32 | **32** |

   Les vingt-sept autres lignes de la première campagne tombaient déjà chacune
   exactement sur la valeur de leur palier ; elles sont conservées.

**Indestructibilité.** `unbreaking_survives()` tire une fois par point de dégât :
chance `niveau/(niveau+1)` d'ignorer le point pour un outil, avec une porte
supplémentaire à 60 % devant pour une armure. Les deux tirages sont pris dans cet
ordre même quand le premier rend le second inutile — la moyenne serait la même,
le *flux* aléatoire non, et c'est le flux qu'une prédiction client doit suivre.
**Ce dernier point n'est pas mesuré ici** ; il est écrit, testé pour sa forme, et
nommé comme non confronté.

---

## 7. L'utilisation d'objets

Trente-huit dispositions, un clic chacune, lues sur les paquets `Block Update`.

**L'ordre.** Le bloc a le premier refus, *sauf* si le joueur est accroupi **avec
quelque chose en main** ; ensuite l'objet agit. Inverser rend tous les conteneurs
du jeu impossibles à ouvrir avec un objet en main.

| ce qu'on fait | ce qu'on obtient |
|---|---|
| main nue sur porte / trappe / portillon en bois | `open` basculé |
| main nue sur **porte / trappe en fer** | **inchangé** |
| main nue sur levier | `powered` basculé |
| main nue sur bouton | `powered=true` + tick programmé |
| main nue sur bloc musical | `note` +1 modulo 25 |
| main nue sur répéteur | `delay` 1→2, puis cycle jusqu'à 4→1 |
| main nue sur comparateur | `mode` compare ↔ subtract |
| main nue sur capteur de lumière | `inverted` basculé |
| main nue sur minerai de redstone | `lit=true` |
| main nue sur bougie allumée | `lit=false` |
| main nue sur gâteau | `bites` +1, disparaît après 6 |
| main nue sur **TNT** | **inchangé** |
| houe sur terre / herbe / chemin | `farmland[moisture=0]` |
| houe sur **terre grossière / terre enracinée** | **`dirt`**, pas farmland |
| pelle sur herbe | `dirt_path` |
| pelle sur feu de camp | `lit=false` |
| hache sur bûche | `stripped_…[axis]` — **l'axe est conservé** |
| hache sur cuivre oxydé | un cran de moins d'oxydation |
| hache sur cuivre ciré | décire |
| briquet sur pierre | `fire[age=0]` un bloc le long de la face |
| briquet sur **TNT** | bloc → air, **entité TNT amorcée** |
| poudre d'os sur blé `age=0` | `age=4` |

Le partage des houes est le résultat : trois blocs deviennent de la terre
labourée et **deux deviennent de la terre nue**. Une table qui envoie les cinq
vers `farmland` est fausse sur deux d'entre eux et a l'air juste sur les trois
autres.

**Le seau n'agit pas par `Use Item On`.** C'est `Item.use`, atteint par le paquet
**Use Item** (0x32), et le serveur y fait son propre lancer de rayon en
`SOURCE_ONLY` pour voir un fluide que le rayon des blocs traverse. C'est ce qui a
fait que la première campagne rapportait un seau qui ne faisait rien : trente-six
autres interactions passent par `Use Item On` et celle-là n'en fait pas partie.
`item_use.cpp` **le nomme** au lieu de faire semblant ; la campagne qui le
mesurerait doit orienter la sonde et envoyer 0x32, et **elle n'a pas été faite
dans cette vague**.

De même nommés et non finis : la poudre d'os (le montant de croissance demande
un générateur aléatoire, et sur une pousse c'est un arbre, donc du worldgen,
au-dessus de cette couche), l'œuf de dragon (téléportation aléatoire), le
lutrin, la cloche, le juke-box, la ruche, le composteur, le chaudron, les
panneaux et les lits.

**Manger** : 32 ticks pour presque tout. Le compte descend pendant que le bouton
est tenu et **lâcher au tick 31 n'a rien mangé** — c'est une règle, pas un
minuteur. Les valeurs nutritives et le 32 lui-même étaient déjà mesurés
(`docs/provenance/survie.md`, `FoodConstants::default_use_duration`) ; ce qui est
ajouté ici est la machine à états et son branchement.

Les trois exceptions — 16 ticks pour le varech séché, 40 pour une fiole de miel,
32 pour une potion et pour du lait — **ne sont pas mesurées** dans cette vague.
Elles sont écrites une par ligne dans `item_use.cpp` avec la mention, pour que
la lacune fasse une ligne de diff le jour où quelqu'un les chronomètre.

---

## 8. Le butin d'entité

Les tables d'entité sont **des données de vanilla**, comme celles des blocs :
compilées par `tools/ov_datagen/entity_loot.py` vers
`data/vanilla/1.20.1/entity_loot.ovpack`. Un fichier **à part** et non une
section de `registry.ovpack` : le pack des registres est partagé par toutes les
branches en cours, et y ajouter une section obligerait chacune à le régénérer au
même instant.

Vocabulaire réellement utilisé par les 97 tables de 1.20.1 — petit et fermé,
comme celui des blocs : six prédicats (`killed_by_player`, `random_chance`,
`random_chance_with_looting`, `inverted`, `entity_properties`,
`damage_source_properties`), cinq fonctions (`set_count`, `looting_enchant`,
`furnace_smelt`, `set_potion`), quatre genres d'entrée (`item`, `empty`, `tag`,
`loot_table`). Tout ce qui n'y est pas est refusé bruyamment.

**Confrontation.** `scripts/check_entity_loot.py` tire chaque table 256 fois des
deux côtés — `ov_mobloot` chez nous, `/loot give <joueur> kill <entité>` chez
vanilla — et compare l'ensemble des objets puis les totaux, avec la même
tolérance que `check_loot.py` pour les blocs. Un désaccord est rejoué sur
2048 tirages avant d'être retenu.

**Résultat : 91 tables conformes sur 92.**

La seule qui reste est l'ours polaire : morue 1571 contre 1590, saumon 528
contre 446. La table tire un à trois objets d'un sac où la morue pèse 3 et le
saumon 1, chacun compté zéro à deux ; sur 2048 tirages l'espérance est 1536 et
512. **Nos deux chiffres sont plus proches de l'espérance que ceux de vanilla** —
c'est l'échantillon du serveur qui s'écarte de trois écarts-types cette
fois-là. Retenu comme désaccord plutôt que blanchi, parce qu'un désaccord
expliqué reste un désaccord.

Cinq tables sont écartées et nommées plutôt que comptées conformes : le dragon
et le wither (invocables seulement au prix du monde de test), le joueur (pas
invocable), le géant (table vide) et `entities/sheep` — celle-ci n'est jamais
tirée directement, puisqu'un mouton a toujours une couleur et que les seize
tables de couleur y délèguent.

Le contexte que `/loot kill` construit est précis et il est reproduit à
l'identique de notre côté : pas de tueur, donc `killed_by_player` faux et
`looting` nul ; l'entité est lue telle qu'elle est. Les tirages rares que seul un
joueur déclenche sont donc absents des **deux** côtés — c'est une égalité de
contexte, pas une lacune.

**Trois pièges payés.**

* Les seize tables de laine vivent dans `entities/sheep/` et un glob `*.json` les
  rate **toutes** : 81 tables au lieu de 97.
* Le `Size` NBT d'une slime est décalé de un — `Size:0` est la taille 1, celle
  que le prédicat `type_specific.size` nomme.
* Le trident d'un noyé **n'est pas dans sa table de butin** : c'est un objet
  d'équipement, un autre système. Ce que sa table donne à 11 % (+2 points par
  niveau de Butin) est un **lingot de cuivre**.

**Une table peut être une autre table.** Les seize tables de laine sont un
bassin de laine plus une référence à `entities/sheep` pour le mouton. La
première confrontation refusait toutes les références et perdait donc seize
tables sur quatre-vingt-douze d'un coup : chaque couleur de mouton lâchait sa
laine et aucune viande. Une référence que le fichier contient est maintenant
suivie ; le nom du datapack est `minecraft:entities/x` et les tables sont
nommées `minecraft:x`, donc le préfixe est retiré à la résolution.

Ce qui reste sont les deux tables de gardien, qui délèguent à
`minecraft:gameplay/fishing/fish` — pas une table d'entité, pas dans ce fichier.
Un tirage qui en atteint une le **signale** (`DrawResult`) au lieu de rendre un
résultat qui ressemblerait à un mob avec moins de butin.

---

## 9. Ce qui n'est pas mesuré

Nommé plutôt que caché :

* **Le seau**, dans les deux sens : la campagne devrait orienter la sonde et
  envoyer `Use Item` (0x32). Le code le refuse et le nomme.
* **La croissance à la poudre d'os** : le montant est aléatoire et sur une pousse
  c'est un arbre.
* **La durée d'appui d'un bouton** (30 ticks bois / 20 ticks pierre) et la durée
  d'allumage d'un minerai de redstone : écrites, nommées comme non mesurées.
* **Indestructibilité** : la formule est écrite et testée pour sa forme, pas
  confrontée.
* **L'XP lâchée par type de mob** : la campagne `mob_xp` existe dans
  `scripts/measure_combat.py` et **n'a pas été lancée** dans cette vague. Rien
  dans `src/` ne prétend le contraire — il n'y a pas de table d'XP de mob.
* **Le chemin de cassage instantané** et ce qu'il fait à la durabilité : évité
  par construction dans la campagne, pas expliqué.
* **Les tirages avec Butin et avec un vrai meurtre** : la campagne `looting`
  existe et n'a pas été lancée non plus.
