# Les mobs, cinquième vague — l'enderman

Campagnes `enderman`, `enderman2`, `enderman3` de `scripts/measure_hostile.py`,
vrai serveur 1.20.1, monde plat, minuit ; relevés bruts dans
`.scratch/hostile.json` (non suivi). De bout en bout contre notre serveur :
`scripts/check_hostile_e2e.py enderman` (port 25613).

Les règles sont dans `ov_gameplay` (`enderman.{hpp,cpp}`), la session qui les
fait tourner dans le serveur (`endermen.{hpp,cpp}`), sur le modèle de
`mob_effects.cpp` : une ligne par enderman à côté du monde des entités.

---

## 1. Le regard

* Une sonde en survie à 8 blocs regarde les yeux d'un enderman tenu dans une
  fosse de 2 (il ne peut pas sortir du regard) : `AngryAt` = la sonde dès la
  première lecture, dix fois sur dix ; regard détourné : jamais.
* **Le cône** (on regarde au-dessus des yeux, en degrés) :

  | distance | 0 | 1 | 2 | 3 | 4 | 5 | 7 |
  |---|---|---|---|---|---|---|---|
  | 8 blocs | colère | colère | colère | colère | colère | — | — |
  | 16 blocs | colère | colère | colère | colère | — | — | colère (aberrant) |

  Le cône se referme avec la distance : ce n'est pas un angle fixe. La forme
  `cos θ > 1 − k / d` le rend : 4° < θ₈ < 5° donne k ∈ [0,0195 ; 0,0304], 3° <
  θ₁₆ < 4° donne k ∈ [0,0219 ; 0,0390] ; ensemble **k ∈ [0,022 ; 0,030]**. Nous
  prenons **0,025**, dans l'intervalle. La case à 7° et 16 blocs en colère est
  notée et non expliquée.
* **Citrouille sculptée** sur la tête : pas de colère.
* **Durée** : `AngerTime` = 709 lu ≈ 50 ticks après le regard, dans le tirage
  de 20 à 39 s du wiki (400 à 779 ticks), que nous tirons.
* Il faut aussi que rien ne s'interpose entre les yeux du joueur et ceux de
  l'enderman (`has_clear_line`, le rayon échantillonné de `has_line_of_sight`
  mis en fonction de deux points).

## 2. Les métadonnées (sur le fil)

| indice | type | quoi |
|---|---|---|
| 16 | bloc optionnel | le bloc porté (9 = `grass_block[snowy=false]`), 0 sans |
| 17 | booléen | hurle (en colère) |
| 18 | booléen | regardé |

Au regard : 18 à vrai, puis 17 à vrai. Envoyées à chaque changement, et à
l'apparition pour un client qui arrive.

## 3. L'eau et la pluie

* Dans l'eau : **1** de dégâts, puis un téléport de ≈ 15 blocs.
* Sous la pluie, à découvert : un téléport toutes les quelques ticks et **1** de
  dégâts environ tous les 10 ticks (40 → 29 en ≈ 105 ticks) — un coup par
  fenêtre de dégâts.

Chez nous : chaque tick où les pieds ou les yeux sont dans l'eau (pas la lave),
ou sous la pluie, 1 de dégâts de type `drown` par la fenêtre du mob
(`MobCombat`), puis un téléport ; la mort éventuelle est celle de `/kill`.

## 4. Le téléport

Seize coups `damage … generic` : les sauts simples tombent dans **±32** par axe
(11,8 / 12,8 ; −24,6 / −16,5 ; 24,0 / −24,7 ; −26,2 / 4,9 ; −31,4 / 26,2 ;
16,5 / −28,1…), y inchangé sur le plat ; trois coups sans déplacement, quelques
petits, un aberrant de 117 blocs (plusieurs sauts d'affilée, sans doute).

Chez nous (`random_teleport`) : un point tiré dans le cube de ±32 (le wiki),
descendu sur le premier bloc qui arrête le mouvement, refusé si les pieds
tombent dans un liquide ou si la boîte ne tient pas ; 64 essais (le wiki). Un
coup de joueur met l'enderman en colère contre lui et le téléporte au tick
suivant.

## 5. Porter et poser

* Sur l'herbe nue, en 80 s : **aucune** prise — le seul bloc portable était
  sous les pieds ; la zone de prise commence aux pieds.
* Pissenlits au niveau des pieds : 7 prises sur 8 en 1 179 ticks (26, 55, 55,
  220, 448, 589, 785, une jamais) → taux ≈ 0,0021 par tick.
* Porteurs de terre sur l'herbe : 4 poses sur 8 en 1 989 ticks (175, 287, 628,
  1 650) → taux ≈ 0,00037 par tick.
* Les chances du wiki (1/20 de tentative par tick dans 4 × 3 × 4, 1/2000 pour
  poser) ne rendent pas ces taux : la prise serait plusieurs fois trop rapide,
  la pose deux fois trop lente. La structure (une tentative par tick, un point
  tiré dans la zone, les conditions du bloc) est gardée ; les chances sont
  **étalonnées** sur ces deux essais — prise **1/160** dans 4 × 3 × 4 depuis les
  pieds (un tiers de pissenlits), pose **1/1 340** dans 2 × 2 × 2 depuis les
  pieds (la moitié de sol libre). Huit endermen chacun : intervalle large,
  nommé.

Chez nous : avec `mobGriefing`, un bloc de `#enderman_holdable` est pris
(remplacé par de l'air) ; posé sur de l'air au-dessus d'une face supérieure
pleine. Les écritures sont **mises en file** et faites après le verrou des
chunks, par le chemin ordinaire d'une édition (mise à jour de bloc envoyée). La
sauvegarde écrit `carriedBlockState` à la clé vanilla, `{Name, Properties}`
comme le `BlockState` d'un bloc qui tombe.

## 6. De bout en bout, sur notre serveur

`scripts/check_hostile_e2e.py enderman` (port 25613), la sonde en survie :

* regardé dans les yeux (la sonde re-visée à chaque demi-seconde : notre serveur
  ignore `NoAI`) : **17 = vrai** (hurle) et 18 = vrai ;
* avec une citrouille sculptée : **17 jamais** en 6 s ;
* de l'eau sous ses pieds : santé **40 → 39** sur le fil et un téléport de
  28 blocs ;
* quatre endermen dans les pissenlits : l'un porte un pissenlit (16 = 2 075) en
  moins de 60 s.

**Le piège payé** : le premier passage lisait 40 après l'eau alors que le saut
avait eu lieu. Le coup mouillé passait par la fenêtre du mob mais personne ne
disait sa santé aux clients : le chemin des coups d'effets et des coups de
joueur envoie l'indice 9, pas celui de l'enderman. La vidange des coups
mouillés l'envoie maintenant.

## 7. Ce qui n'est pas fait

* L'**esquive des flèches** (un projectile le téléporte au lieu de le blesser).
* Le **téléport vers une cible lointaine** en colère, et les **téléports de
  jour** au soleil.
* `AngerTime` / `AngryAt` dans la sauvegarde : un enderman relu n'est plus en
  colère.
* La **ligne de vue** des yeux au bloc pris.
* Les endermen du **Nether** et de l'**End** (autres mondes d'entités).
