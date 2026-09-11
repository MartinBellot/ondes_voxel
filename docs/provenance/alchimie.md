# Alchimie — alambic, recettes, potions

Ce qui suit est établi par **mesure contre le vrai serveur 1.20.1**
(`tools/vanilla/server.jar`, SHA-1 `84194a2f286ef7c14ed7ce0090dba59902951553`), par
`scripts/measure_brewing.py` (dix campagnes), puis rejoué **contre notre propre serveur** par
`scripts/check_brewing_e2e.py`. Les relevés bruts vont dans
`data/vanilla/1.20.1/normalized/brewing.json` (gitignoré, régénérable) et sont rejoués entrée par
entrée par les tests `[brewing][parity]` de `test_brewing.cpp`.

| | |
|---|---|
| `src/ov_gameplay/{include/ov/gameplay,src}/brewing.{hpp,cpp}` | les 43 potions, les mélanges, l'horloge et le combustible de l'alambic, la loi de l'éclaboussure, le nuage, la flèche |
| `src/ov_gameplay/tests/test_brewing.cpp` | l'ordre du registre, les règles, et la parité contre les campagnes brutes |
| `src/ov_server/src/brewing_session.{hpp,cpp}` | l'alambic comme block entity tickée, sa fenêtre, boire, éclabousser, le nuage, la flèche, le ragoût |
| `src/ov_server/src/server.cpp`, `projectiles.*`, `effect_session.*`, `block_container.*` | blocs courts `// ── brewing ──` |

---

## 0. Les sources

**Il n'y a pas de rapport du data generator pour le brassage.** Le registre `minecraft:potion`
donne quarante-trois noms et leurs identifiants, rien d'autre : pas de type de recette
`brewing`, pas de durée, pas d'effet. La table des mélanges est donc écrite depuis la
**documentation** — l'article « Brewing » du wiki Minecraft, pour la table des ingrédients et
des flèches entre potions ; l'article « Potion » pour les effets et les durées ; « Brewing Stand »
pour le clic majuscule de la fenêtre ; « Area Effect Cloud » pour les champs du nuage — puis
**chaque ligne est vérifiée contre le vrai serveur**. Aucun nombre de ce document n'est retenu
sans sa campagne ; ceux qui n'en ont pas sont nommés comme tels au § 9.

## 1. Les recettes — 2709 fioles sur 2709

**La campagne ne choisit rien.** Chaque fiole possible — trois formes (`potion`,
`splash_potion`, `lingering_potion`) × les 43 potions du registre, soit 129 — est placée dans un
alambic avec chaque ingrédient candidat : les 17 que le wiki nomme, plus **quatre témoins** qui
n'en sont pas (carotte, morue, pomme dorée, pépite d'or). Trois fioles par alambic, 903 alambics
posés par `setblock` avec leur NBT, combustible 20, puis **443 ticks** sur l'horloge du serveur,
et chaque alambic relu.

| | |
|---|---|
| fioles | 2709 |
| fioles changées | **251** |
| dont mélanges de contenant | 43 (poudre à canon sur les 43 potions à boire) + 43 (souffle de dragon sur les 43 jetables) |
| dont mélanges de potion | 165 = 55 flèches du wiki × 3 formes |
| alambics qui ont brassé | 178 (combustible relu **19**) ; les 725 autres relisent **20** |
| **accord avec notre table** | **2709 / 2709**, dont les 2458 fioles qui ne devaient *pas* changer |

Par ingrédient : verrue 3, redstone 42 (14 × 3), glowstone 30 (10 × 3), œil fermenté 36 (12 × 3),
sucre, patte de lapin, pastèque scintillante, œil d'araignée, crème de magma, poudre de blaze et
larme de ghast 6 chacun (eau → banale, et étrange → leur potion), poisson-globe, carotte dorée,
carapace de tortue et membrane 3 chacun. **Les quatre témoins : zéro** — et un alambic qui n'a rien
à brasser ne dépense rien.

Ce que la campagne a appris que le wiki ne disait pas :

* **Les mélanges de contenant ignorent la potion.** La potion « non fabricable »
  (`minecraft:empty`) devient une jetable, puis une persistante, comme n'importe laquelle.
* **Le vrai serveur retire le tag `Potion` d'une potion vide.** La jetable issue de
  `minecraft:empty` ressort **sans aucun tag** ; notre `potion_tag(Empty)` écrit donc des octets
  vides plutôt que `{Potion:"minecraft:empty"}` — les deux se lisent pareil, mais une pile compare
  son NBT, et la nôtre ne s'empilerait pas avec celle du jeu. Le test de parité a d'abord **planté**
  sur ce `null`, c'est comme ça qu'il a été vu.

## 2. L'alambic — temps de brassage, combustible, état du bloc

Cinq alambics lus ensemble, `BrewTime`, `Fuel` et leurs cases, avec `time query gametime` dans
le même lot — donc au même tick :

| alambic | départ | ce que le serveur a fait |
|---|---|---|
| A | 3 eaux + verrue, combustible 20 | tick 1 : `BrewTime` **400**, combustible **19** ; tick **401** : trois étranges |
| B | combustible 0 + 1 poudre | tick 1 : poudre prise, **20 puis 19** dans le même tick, brassage lancé |
| C | combustible 0, pas de poudre | rien, jamais |
| D | combustible 1 + 1 poudre | tick 1 : brassage à combustible **1 → 0**, poudre gardée ; tick 5 : **déjà rechargé à 20 en plein brassage**, poudre prise ; second brassage : 19 |
| E | comme A | ingrédient retiré au tick 105 : `BrewTime` **0** au relevé suivant, combustible **19** — pas rendu |

Trois brassages successifs sur A : combustible **18, 17, 16** au départ de chacun. D'où les
règles de `brewing_stand_tick`, dans cet ordre : **recharger** dès que le combustible est à 0 et
qu'il y a de la poudre — que quelque chose soit à brasser ou non ; compter à rebours ; **brasser** à
0 ; **arrêter** dès que l'ingrédient manque ou change ; sinon **démarrer** en payant 1. **400 ticks,
20 brassages par poudre** — mesurés, pas recopiés.

* **État du bloc** : fioles en cases 0 et 2 → `has_bottle_0=true,has_bottle_1=false,has_bottle_2=true`.
* **Le dernier souffle de dragon ne rend rien.** Brassé avec **un seul** souffle, la case
  d'ingrédient est **vide** ensuite — pas de fiole vide, contre le wiki. Avec deux, il en reste un et
  une fiole vide gît au sol près de l'alambic. ⚠ Les deux alambics étaient à trois blocs l'un de
  l'autre et la recherche d'objet portait à trois blocs : quelle machine a lâché cette fiole n'est
  **pas séparé** par cette mesure. `scripts/measure_brewing.py` écarte maintenant les deux alambics de
  huit blocs pour la prochaine exécution.
* **Déchargé au milieu d'un brassage, l'alambic recommence.** Un alambic loin de tout, son chunk
  retiré du forceload jusqu'à ce que `execute if loaded` réponde non, puis rechargé : `BrewTime`
  **314 → 396**, combustible **19 → 18**. L'ingrédient du brassage en cours n'est pas sauvegardé ; au
  premier tick le « même ingrédient ? » échoue, le brassage est abandonné, puis relancé en payant.
  Notre alambic fait **exactement** cela — le passage de bout en bout l'a montré avant la mesure
  (§ 9), et c'est la mesure qui a dit que c'était juste.

## 3. Les faces — entonnoirs, 13 cellules sur 13

Un entonnoir au-dessus, un sur le côté, un objet chacun ; puis un entonnoir dessous. Le
combustible est relu en même temps, parce qu'une poudre qui entre **brûle aussitôt** et se lirait
« refusée » sinon.

| face | potion | poudre de blaze | verrue | fiole vide | redstone |
|---|---|---|---|---|---|
| dessus | refusée | case **3** (c'est un ingrédient) | case 3 | refusée | case 3 |
| côté | case **0** | case **4**, brûlée : combustible **0 → 20** | refusée | case 0 | refusée |

Dessous : l'entonnoir **tire la potion** et laisse la verrue et la poudre ; il tire aussi une fiole
vide de la case d'ingrédient et d'une case de fiole. `ContainerBridge` (`SidedAccess::BrewingStand`)
rend ces treize réponses ; les alambics posés par une main ou par une commande reçoivent leur block
entity par le même chemin que les coffres.

## 4. La fenêtre

Le bot ouvre un alambic en train de brasser : **fenêtre 1, menu 10** (`minecraft:brewing_stand`),
**41 cases** (5 + 36). Paquet `0x13` (Container Property), propriété **0 = le temps de brassage** :
362 valeurs d'affilée, **361 → 0, une par tick**. La propriété 1 (combustible) n'a pas bougé pendant
ce brassage et la rafale d'ouverture n'a pas été enregistrée : « 1 = combustible » vient de
l'archive du protocole, **non capturé** (la campagne garde maintenant la rafale). Chez nous, le
passage de bout en bout lit 1 = 20 puis 19, et la barre de flamme du client en dépend.

## 5. Boire — 43 potions sur 43

Le bot boit chaque potion du registre, en survie. Les effets sont lus sur les paquets **Entity
Effect** qu'il reçoit — la durée exacte au tick, sans aucune lecture de console — et les effets
instantanés sur Set Health.

* **43 / 43** identiques à notre table (199 assertions du test `[parity]`) : effet, amplificateur
  et durée. Les durées sont celles de l'article « Potion » du wiki — 3600 / 9600 pour les
  potions « simples », 1800 au niveau II, 900 / 1800 / 432 pour le poison, 900 / 1800 / 450 pour
  la régénération, 1800 / 4800 pour la lenteur, la faiblesse et la chute lente, 6000 pour la
  chance — et le **maître tortue** porte deux effets : lenteur IV + résistance III (400, puis 800),
  lenteur **VI** + résistance **IV** (400) au niveau II.
* **Instantanés** : soin **+4**, soin II **+8** (de 6 à 10, de 6 à 14) ; dégâts **−6**, dégâts II
  **−12**. Aucun Entity Effect n'est envoyé pour eux.
* **Ce qui reste en main** : une **fiole vide**, pour chacune des 43. En **créatif**, la potion
  reste et l'effet s'applique quand même.
* Les cinq potions sans effet — `empty`, `water`, `mundane`, `thick`, `awkward` — ne font rien et
  rendent leur fiole.

**Le temps de boire — 31 ticks sur l'horloge du serveur, 8 essais sur 8.** `time query gametime`
envoyé juste après le paquet Use Item date le début ; le tick où l'effet est tombé se déduit de sa
durée restante lue avec l'horloge dans le même lot, et la phase de cette déduction est **étalonnée**
par un `effect give` de console lu de la même façon (décalage : 0, huit fois). Notre serveur achève
un usage **32** ticks après le paquet (`begin_use` pose 32, un décompte par tick), la valeur que
`survie.md` a mesurée pour les aliments par une autre méthode. Les deux ne se contredisent que si
l'instrument ne porte pas de décalage de phase propre à l'usage : c'est ce que le témoin
« pomme dorée » du second passage tranche (§ 12 bis).

## 6. La potion jetable — la loi, le plancher, le point d'origine

Une potion jetable de vitesse de **1000** ticks (`CustomPotionEffects`, sur une base « awkward »
pour qu'elle ne soit pas traitée comme de l'eau), brisée à une distance r des pieds du bot,
invoquée 0,01 au-dessus du sol avec un mouvement vers le bas :

| r | 1 | 1,5 | 2 | 2,4 | 2,5 | 3 | 3,1 | 3,2 | 3,5 | 3,9 | 3,99 | 4,05 | 4,5 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| mesuré | 750 | 625 | 500 | 400 | 375 | 250 | 225 | 200 | 125 | 25 | — | — | — |

Exactement `(int)(1000 × (1 − r/4) + 0,5)`. **Le plancher** : sur une base de 100, r = 3,1 donne
22 et l'effet est posé, r = 3,2 donne **20 pile et rien n'est posé** — il faut *plus* de 20 ticks.
**Le soin II** à r = 1, 1,5, 2, 2,5, 3 : +6, +5, +4, +3, +2.

**Le point d'origine — la ligne qui a changé le code.** Le cas « coup direct » de la campagne n'en
est pas un : la fiole, lâchée de deux blocs au-dessus du bot à −0,5, l'a **traversé sans le
toucher** et s'est brisée au sol. Elle a quand même donné **909 / 91 / +7**, et non 1000 / 100 /
+8. En refaisant sa chute avec la gravité et la traînée du module des projectiles (0,05F, 0,99F),
elle était à **0,365** au-dessus du sol quand son dernier tick a commencé ; `1 − 0,365/4 = 0,909`.
**La distance se mesure depuis la position de la fiole au début de son tick de rupture, pas depuis
le point d'impact** — lequel aurait donné une distance nulle et 1000. Notre serveur prenait le
point d'impact : `ProjectileEvent` porte maintenant `from`, et l'éclaboussure part de là.

Le facteur 1 du **vrai** coup direct n'a donc pas été observé : il vient du wiki, et il est nommé.

## 7. La potion persistante — le nuage

Une persistante brisée au sol, le bot amené dans le nuage :

* **l'effet dure le quart** : vitesse longue → **2400** ticks (9600 / 4) ;
* il est **réappliqué toutes les ~20 ticks** tant qu'on reste dedans (Entity Effect reçus aux
  horloges 9152, 9171, 9190 — datés à la console, donc au tick près seulement) ;
* **le soin II dans un nuage soigne +4 par application** (2 → 6 → 10 → 14) : la moitié de 8, le
  facteur 0,5 des instantanés d'un nuage ;
* métadonnées : **index 9 = la couleur** (3402751 = 0x33EBFF pour la vitesse, **16262179 =
  0xF82423 pour le soin**), **index 10 = « en attente »** (booléen, vrai à la naissance).

⚠ **Le NBT du nuage n'a pas été lu par ce passage** : la commande portait un espace final, et
Brigadier attend alors un chemin et rejette la commande — chaque lecture est revenue vide. Le rayon
(3,0), sa décroissance par tick et par usage (−3/600, −0,5), l'attente (10) et le délai de
réapplication (20) sont ceux du wiki jusqu'au second passage (§ 12 bis).

**La couleur du soin est une correction.** La table des effets met 0 pour les deux instantanés —
« jamais visibles sur une entité », `effets.md` § 10 — mais une potion les montre. `potion_color`
prend donc 0xF82423 pour le soin instantané ; celle des dégâts instantanés est lue par le second
passage.

## 8. Flèches, ragoût suspect, lait

**Flèches à pointe** — une flèche invoquée qui file vers le bot :

| potion | effet reçu | = durée ÷ 8 |
|---|---|---|
| rapidité (3600) | vitesse **450** | ✓ |
| rapidité longue (9600) | vitesse **1200** | ✓ |
| poison (900) | poison **112** | ✓ (plancher de la division) |
| poison II (432) | poison II **54** | ✓ |
| maître tortue (400 + 400) | lenteur IV **50** + résistance III **50** | ✓ |
| `CustomPotionEffects` vitesse 5 | vitesse **5** | ✗ — **pas divisée** |
| dégâts instantanés | 6 au total (1 de la flèche, puis la différence dans la fenêtre) ; **annoncés par un Entity Effect d'un tick** | |

**Seuls les effets propres à la potion sont divisés par huit ; les effets personnalisés gardent leur
durée.** `arrow_effects` prend maintenant les deux listes séparément. La **flèche spectrale** fait
briller **200** ticks, ou sa propre `Duration` (300 lu tel quel).

**Ragoût suspect**, 5 / 5 : les effets de `Effects` (`EffectId`, `EffectDuration`) — vision
nocturne 100, deux effets à la fois, **160 ticks** quand `EffectDuration` manque ; un ragoût sans
tag ne fait rien, et les clés de 1.20.2 (`effects`, `id`, `duration`) sont **ignorées** en 1.20.1.
Le **bol** revient dans la main à chaque fois — chez nous aussi.

**Le lait** : `effets.md` l'a mesuré (il vide tout et rend le seau) et le serveur le fait ; ce mandat
n'y a rien changé.

## 9. De bout en bout, contre notre serveur

`scripts/check_brewing_e2e.py` : `ov_dedicated` sur une copie du banc, un client sonde qui parle
le protocole 763 exact et ne fait que des gestes de joueur — poser, cliquer, glisser par touche
numérique, boire, lancer. Rien n'est appelé à côté du protocole.

**Avant** (le code de `main`) : un clic sur un alambic ne fait rien — `unmodelled_containers()`
le nommait, le serveur n'ouvrait aucune fenêtre, et une potion bue ne s'appliquait pas
(« finished using minecraft:potion, which this server does not apply yet »). **Après** :

| étape | ce que la sonde a lu |
|---|---|
| poser, ouvrir | Open Screen fenêtre **3**, menu `minecraft:brewing_stand`, **41** cases (5 + 36) |
| charger | trois fioles d'eau (par `/give`), deux poudres de blaze en combustible, une verrue |
| brasser | barre (propriété 0) partie de **400**, finie à 0 ; combustible (propriété 1) **20 → 19** ; trois `minecraft:awkward`, verrue consommée |
| second brassage coupé | poudre de blaze en ingrédient ; **serveur arrêté** à la barre 351, relancé |
| relu du disque | trois potions étranges et la poudre toujours là ; la barre **repart à 391** et le combustible passe **18 → 17** — ce que vanilla fait aussi, § 2 |
| fin | trois `minecraft:strength` |
| boire (survie) | Entity Effect **force I, 3600 ticks** ; **fiole vide** dans la main |
| potion jetable à ses pieds | vitesse II, **1517** ticks = 1800 × 0,843 : la distance part de la fiole au début de son tick de rupture (≈ 0,63), § 6 — avant la correction, le même geste donnait 1575, le point d'impact à 0,5 |
| potion persistante à ses pieds | vision nocturne, **2400** ticks = 9600 / 4 |

Le passage final a pris 53 s pour les 400 ticks du premier brassage ; les précédents, jusqu'à 292 s
sur une machine à charge 32 (huit cœurs). C'est le piège n° 1 ci-dessous.

## 10. Ce qui n'est pas fait, nommé

Listé aussi par `Brewing::gaps()`, journalisé au démarrage du serveur.

* **Les mobs ne reçoivent aucun effet de potion** — ni éclaboussure, ni nuage, ni flèche : ce
  serveur ne porte pas d'effets sur les mobs (`effets.md` § 16). Les joueurs, oui.
* **La fiole d'eau** jetée ou persistante n'éteint aucun feu et ne blesse aucun enderman ni
  blaze : le feu est le mandat d'un autre agent. Elle envoie ses particules et s'arrête là.
* **Fabriquer les flèches à pointe** (potion persistante + 8 flèches) est une recette spéciale
  que le livre de recettes ne réalise pas encore ; les flèches à pointe *existantes* agissent.
* **La potion lancée ne porte pas de métadonnée d'objet** : le client dessine la fiole par défaut.
* **Le nuage** : pas de propriétaire, pas de particule ni de couleur imposée ; un joueur arrivé
  après sa naissance ne le voit pas.
* **Le ragoût de champignons, de lapin et la soupe de betterave** ne rendent pas leur bol ; seul
  le ragoût suspect a été mesuré et le fait.
* **Glisser et double-cliquer** dans la fenêtre de l'alambic sont refusés, la fenêtre renvoyée.
* **Le clic majuscule vers l'alambic** (poudre → combustible puis ingrédient, fiole seule → cases
  de fiole) suit l'article « Brewing Stand » du wiki ; c'est une règle de client, **non mesurée**.
* **Les particules d'éclaboussure** (World Event 2002 / 2007, couleur en donnée) viennent du
  tableau des World Events de l'archive du protocole ; **non capturées**.
* **Le facteur 1 d'un coup direct** n'a pas été observé (§ 6 : la fiole a traversé le bot) ; il
  vient du wiki.
* **L'effet instantané d'une flèche** est appliqué d'un coup ici ; vanilla le pose comme un effet
  d'un tick, annoncé par un Entity Effect (§ 8). Même total de dégâts, un paquet de moins.
* **Le propriétaire d'une fiole** n'est pas exclu de son éclaboussure, comme en vanilla ; il n'est
  pas mesuré non plus.

## 11. Pièges

1. **Ce serveur de debug tombe à un ou deux ticks par seconde quand la machine est chargée.**
   Mesuré ici : charge moyenne **32** sur huit cœurs, 56 Mo de pages libres, et le premier
   brassage (400 ticks) a pris de **72,6 s à 292 s** de temps mural selon le passage ; le journal
   du serveur aligne les « can't keep up ». Un e2e qui attend « 45 s » ou « 2,6 s » échoue sans
   que rien ne soit faux — le premier passage a conclu « pas de potion étrange » avec la barre à
   179, puis « aucune force reçue » avec une gorgée de 32 ticks encore en cours. Les attentes de
   `check_brewing_e2e.py` suivent donc la **progression** (la barre qui bouge, l'Entity Effect
   qui arrive), jamais une durée murale.
2. **Une commande peut arriver après la fenêtre.** `/give` puis l'ouverture de l'alambic : sur un
   serveur à ce régime, la fenêtre s'est ouverte une fois avant que le `/give` ne s'exécute, et la
   sonde a vu un inventaire vide. Les réponses aux clics, elles, attendent le verrou que le tick
   surchargé tient : une troisième fiole « manquante » après une seconde était simplement en
   route. On relit l'état jusqu'à ce qu'il soit là.
3. **Set Creative Slot ne porte pas de NBT sur ce serveur** — et une fiole d'eau n'est une fiole
   d'eau que par `{Potion:"minecraft:water"}` ; sans tag, c'est `minecraft:empty`, la potion
   « non fabricable », sur laquelle la verrue ne fait rien. La sonde passe par `/give`, qui garde
   le tag.
4. **Un alambic relu du disque au milieu d'un brassage recommence — et c'est juste.** L'ingrédient
   avec lequel le brassage a commencé n'est pas sauvegardé (il n'y a pas de clé pour ça dans le
   NBT) : au premier tick, « même ingrédient ? » échoue, le brassage est abandonné, puis relancé en
   payant un combustible de plus. Le passage de bout en bout l'a montré chez nous (382 → 398,
   combustible 18 → 17) **avant** que l'expérience de déchargement du § 2 ne mesure la même chose
   chez vanilla (314 → 396, 19 → 18). Un résultat qu'on aurait « corrigé » en gardant le brassage
   aurait été faux.
5. **Un espace final a coûté une campagne entière.** `data get entity @e[…] ` — avec l'espace —
   n'est pas la même commande que sans : Brigadier attend alors l'argument « chemin », rejette la
   ligne, et la sonde lit « rien » pour chaque champ du nuage. Rien ne distingue ce rien d'un nuage
   sans NBT.
6. **Une fiole qui traverse sa cible ne la touche pas forcément.** Le « coup direct » de la campagne
   jetable, lâché à travers le bot, s'est brisé au sol — et c'est cet accident qui a montré d'où
   l'éclaboussure se mesure (§ 6). Une mesure qui ne fait pas ce qu'on croyait peut en faire une
   autre, meilleure, à condition de refaire le calcul au lieu de conclure « écart ».
7. **Le vrai serveur retire le tag d'une potion vide** : un `null` dans la table brute a d'abord
   fait **planter** le test de parité, pas échouer. Le tag absent veut dire `minecraft:empty`, et
   notre alambic n'en écrit plus.
8. **Un champ inséré au milieu d'une structure initialisée par position change le sens de tous les
   initialiseurs.** `from` a d'abord été ajouté à `ProjectileEvent` entre `point` et `velocity` ;
   les événements de `projectile.cpp` s'écrivent `ProjectileEvent{kind, id, cible, joueur, point,
   v}`, donc `v` est allé dans `from`, **la vitesse de chaque événement est restée à 0, et chaque
   flèche touchait pour 0**. Ni les tests `[brewing]` ni le passage de bout en bout (qui ne tire pas
   de flèche) ne pouvaient le voir ; seule la suite complète l'a vu, par `test_projectile`
   (`0.0 == 0.3`). Le champ est maintenant **le dernier**. À retenir pour quiconque ajoute un champ
   à une structure d'un autre module : lancer **toute** la suite, pas le tag de son mandat.

## 12 bis. Le second passage — ce qu'il doit trancher

Le premier passage a laissé cinq questions ouvertes, chacune nommée plus haut ; un second passage
de `scripts/measure_brewing.py --only timing,window,drinktime,lingering`, écrit dans
`brewing_followup.json` pour ne rien écraser du premier, les pose :

| question | comment | en attendant |
|---|---|---|
| le NBT du nuage (rayon, décroissances, attente, délai) | lecture corrigée (sans l'espace final, piège 5) | les valeurs du wiki : 3,0 ; −0,5 par usage ; −rayon/durée par tick ; 10 ; 20 |
| la couleur des dégâts instantanés dans une potion | un nuage de dégâts, son index 9 | la table des effets (0) — les potions de dégâts ont une couleur fausse |
| la propriété 1 de la fenêtre | la rafale d'ouverture, maintenant gardée | « 1 = combustible », archive du protocole |
| 31 ou 32 ticks pour boire | le même instrument sur une pomme dorée | 32, la valeur des aliments de `survie.md` |
| qui lâche la fiole du souffle de dragon | deux alambics à huit blocs l'un de l'autre | la case vide du dernier souffle est certaine ; la fiole au sol du souffle restant, probable |

**Au moment où ce document est écrit, ce passage attend encore le verrou du serveur vanilla
partagé** (une douzaine de campagnes d'autres agents devant lui). Tant qu'il n'a pas tourné, les
cinq lignes restent dans la colonne de droite, et c'est ce que le code fait.

## 12. Reproduire

```bash
lockf /tmp/ov-vanilla.lock python3 scripts/measure_brewing.py     # l'oracle, dix campagnes
ctest --preset macos-debug -R test_ov_gameplay                      # [brewing], [parity]
python3 scripts/check_brewing_e2e.py                                # notre serveur
```

Port 25641 pour vanilla, 25651 pour notre serveur (`OV_BREW_E2E_PORT`). La campagne efface son
monde en partant.
