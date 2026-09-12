# Physique des blocs : glace, échelles, slime, miel, toiles, colonnes de bulles

*2026-09-11 — agent « physique du mouvement »*

Ce que les blocs font au mouvement de ce qui marche dessus ou passe dedans. Rien
de cela n'est dans les rapports de Mojang : c'est du code Java, et ce projet ne
lit pas de code Java. Les règles viennent de la documentation publique ; les
nombres viennent d'une trace du vrai serveur 1.20.1, tick par tick.

## 1. Où c'est dans le code

Une seule implémentation, lue par le joueur **et** par les entités :

* `ov/gameplay/block_motion.hpp` — la table `BlockMotionTable`, construite une
  fois depuis le registre (par nom de bloc, puis lue par identifiant : aucune
  comparaison de chaînes dans le tick), et les fonctions qui lisent les blocs
  autour d'une boîte : bloc porteur, friction, facteurs de vitesse et de saut,
  grimpable, multiplicateur « coincé », effets à l'intérieur, atterrissage ;
* `CollisionWorld` porte la table en option. **Sans table, tout bloc est un sol
  ordinaire**, exactement comme avant : aucun appelant existant ne change de
  comportement sans l'avoir demandé ;
* `step()` (le joueur, `physics.cpp`) et `step_entity()` (les mobs,
  `entity_physics.cpp`) appellent les mêmes fonctions ;
* `ov/gameplay/fluid_push.hpp` — la magnitude de la poussée d'un courant ;
* branchements : `Session` (notre client) et la boucle des mobs du serveur
  construisent chacun leur table une fois ; un mob divise sa marche par la
  friction réelle du sol et la multiplie par le rapport de la loi de marche
  pour ce sol.

## 2. La mesure

`scripts/measure_block_motion.py run` pose en une fois, sur un vrai serveur
1.20.1, des pistes de chaque sol, des colonnes de chaque grimpable, des colonnes
de toile, de baies et de neige poudreuse, des tubes de bulles au-dessus de sable
des âmes et de magma, et deux chenaux d'eau et de lave courantes. Des supports
d'armure (entités vivantes sans IA : seule la physique les bouge) et des objets
lâchés y sont lancés.

L'enregistreur rend la mesure exacte : un datapack dont la fonction `tick`
ajoute, **à chaque tick**, `Pos`, `Motion` et `OnGround` de chaque entité
marquée à une liste en stockage de commandes, avec l'heure du jeu. La console
imprime la liste : des doubles écrits par le jeu à pleine précision, la
trajectoire complète, sans échantillonnage. Une entité marquée `pushx` reçoit
`Motion[0] = 0,1` au début de chaque tick — ce que fait un mob qui marche dans
un mur, et ce que grimper par collision exige.

**76 entités sur 76 tracées, 6 232 lignes.** `fit` lit la trace ; la trace
elle-même reste dans `.scratch/` (sortie du jeu, jamais commitée).

## 3. Ce que la trace dit

### Friction : un produit en float

Le rapport de deux vitesses successives au sol :

| Sol | Support d'armure (× 0,91) | Objet (× 0,98) | Friction |
|---|---|---|---|
| pierre, herbe, terre des âmes | 0,546 000 063 419 | 0,588 000 059 128 | 0,6 |
| glace, glace compactée | 0,891 800 045 967 | 0,960 400 044 918 | 0,98 |
| glace bleue | 0,899 990 022 182 | 0,969 220 042 229 | 0,989 |
| sable des âmes, miel | 0,218 400 028 622 | 0,235 200 027 156 | 0,6 × facteur 0,4 |

0,546 000 063 419, c'est **float(0,6F × 0,91F)**. Le même produit en double
donne 0,546 000 021 696 — le témoin qui échoue, à la huitième décimale. Le jeu
multiplie les deux flottants *avant* d'élargir en double. Notre pas le fait
désormais aussi. La terre des âmes n'a **aucun** facteur de vitesse (seule la
Vitesse des âmes s'en sert) ; le miel a la friction ordinaire.

### Traînée verticale : 0,98F pour le vivant, 0,98 pour l'objet

Un support d'armure stocke −0,078 400 001 5 après un tick de chute : −0,08 ×
0,98F. Un objet stocke −0,0392 tout rond : −0,04 × 0,98. Deux constantes, pas
une. La traînée de l'eau est 0,8F dans les deux sens (−0,053 000 000 7 et
0,011 200 000 2 ci-dessous).

### Grimpables

Échelle, liane, lianes tordues, pleureuses, des cavernes, échafaudage : la chute
est bornée à **0,150 000 006** par tick (0,15F) et la montée par collision vaut
**0,117 600 002 3** par tick, soit (0,2 − 0,08) × 0,98F — 2,35 m/s. Le témoin
sans grimpable tombe librement ; un objet sur une échelle aussi (grimper est
réservé au vivant).

### Coincé

| Bloc | Support d'armure | Objet |
|---|---|---|
| toile | 0,003 920 000 1 par tick = 0,0784 × 0,05F | 0,002 = 0,04 × 0,05 |
| neige poudreuse | 0,117 600 002 3 = 0,0784 × 1,5 | 0,06 = 0,04 × 1,5 |
| buisson de baies | 0,058 800 001 1 = 0,0784 × 0,75 | traverse (vivant seulement) |

Dans chaque cas `Motion` retombe à −0,0784 : la vitesse est jetée et la gravité
la reconstruit, tick après tick.

### Colonnes de bulles : une fois par bloc touché

Le premier tick d'un support d'armure dans une colonne descendante vaut
−0,053 000 000 7 : **deux** blocs touchés × −0,03, puis × 0,8F, puis −0,005. Le
deuxième, trois blocs. Le régime −0,245 000 003 6 = −0,3 × 0,8F − 0,005 : la
borne −0,3 atteinte. Montante : +0,06 par bloc, borne 0,7, régime
0,555 000 008 3 ; +0,1 au bloc de surface. **16 vitesses sur 16** reproduites
(`test_block_motion.cpp`).

### Slime, miel, foin

Un support d'armure qui touche le slime à −1,139 458 865 4 repart à
+1,038 269 708 3 = (1,139 458 865 4 − 0,08) × 0,98F : un rebond de facteur
**1** exact. Un objet : 0,8. Le miel et le foin arrêtent la chute net.

### Courant

Le premier incrément de `Motion.x` d'un support d'armure dans un chenal d'eau
vaut 0,011 200 000 2 = **0,014** × 0,8F ; la vitesse tend vers 0,056 = 0,014 ×
0,8 / 0,2. La direction était déjà mesurée (`fluides.md`) ; la magnitude
l'est maintenant.

## 4. Ce qui reste, nommé

* **Les règles propres au joueur** — la poussée au sol sur glace avec une
  entrée, le maintien accroupi sur l'échelle, le saut sur le miel — ne
  s'observent que dans un vrai client. Le code les applique d'après la
  documentation (minecraft.wiki *Ice*, *Ladder*, *Honey Block* ; mcpk.wiki
  *Horizontal Movement Formulas*) ; un oracle client reste à écrire.
* **L'ordre du pas des mobs** : `step_entity` applique la traînée avant le
  déplacement, le jeu après. Les déplacements sont les mêmes, décalés d'un
  tick ; les vitesses stockées diffèrent sous une borne (échelle). Changer
  l'ordre changerait toutes les vitesses de marche mesurées des mobs.
* **Colonnes de bulles pour les mobs** : l'effet est appliqué, mais nos mobs
  n'ont pas de physique de l'eau (c'est `FloatGoal` qui nage) ; la formation
  des colonnes au-dessus du sable des âmes et du magma n'est pas faite.
* **Poussée de la lave** : non mesurée — la lave n'avait pas atteint le
  support d'armure dans le temps d'attente.
* **Poussée du courant** : la fonction existe et est testée ; elle n'est encore
  appelée ni par le client ni par les mobs.
* **Neige poudreuse et échafaudage** : leur forme de collision dépend de
  l'entité (chute, bottes en cuir, accroupi) ; nos formes sont par état.
* **Vitesse des âmes** (enchantement) : non faite.
* **Objets au sol** : notre serveur ne leur donne pas de physique.
* **Contrôle de bout en bout sur notre serveur** : non fait.
