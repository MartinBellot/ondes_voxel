# L'amplitude de `old_blended_noise` : les deux hypothèses sont fausses

*2026-09-09. Seeds 1234567890 et 987654321. Suite de `docs/provenance/etage-de-bruit.md`.*

Le mandat demandait de trancher entre deux arithmétiques qui divisent
`old_blended_noise` par quatre — **deux octaves de moins** dans chaque pile
limite, ou **un second diviseur de 512 au lieu de 128**. La mesure en désigne
une troisième : **aucune des deux**. L'amplitude actuelle est la bonne, à
environ ±20 % près, et les deux candidates sont fausses d'un facteur ~4.

**Aucune constante de génération n'a été modifiée.** La parité est inchangée,
puisqu'il n'y avait rien à corriger.

---

## 1. Ce qui était supposé, et qui n'était pas mesuré

`etage-de-bruit.md` localisait le déficit de surface dans l'amplitude de
`base_3d_noise`, sur la base d'un balayage de `OV_BASE3D_GAIN` : à 0,25
l'histogramme du décalage de surface a son mode en 0 et sa forme la plus
resserrée, à 1,0 son mode est en −2. Ce document notait déjà, honnêtement, que
l'accord global est « presque plat » sur ce balayage et que seul l'histogramme
sépare quelque chose.

Ce raisonnement a un trou, et le trou est le **témoin manquant**. Il compare
gain 1,0 et gain 0,25 sans jamais demander ce que fait gain 0.

## 2. Le témoin qui casse la méthode : le bruit déplacé

`ov_parity --surface --dump=` écrit la carte de la hauteur de surface, colonne
par colonne, pour nous et pour le jeu (les 256 colonnes de chaque chunk, pas un
pas de 3 : les statistiques ci-dessous portent sur des *voisins*). Trois cartes
sont produites : avec le bruit, sans lui (`OV_BASE3D_GAIN` ≈ 0), et avec le
bruit **échantillonné dix mille blocs plus loin** (`OV_BASE3D_SHIFT`, ajouté
pour cela). Sur 120 chunks, 23 882 colonnes sèches.

Le résidu `r = h_jeu − h_sans_bruit` est ce que le terme est censé expliquer ;
`c = h_candidat − h_sans_bruit` est ce qu'il explique effectivement. On
corrèle les deux **par bande spatiale** (différences à distance k, ce qui
retire tout ce qui est plus long que ~2k — sans quoi l'estimateur dérive et
culmine à un décalage de 32 blocs plutôt qu'à zéro).

| k | corr, bruit en place | corr, **même bruit déplacé de 10 007 blocs** |
|---|---|---|
| 1 | 0,2452 | **0,2868** |
| 4 | 0,2349 | 0,2406 |
| 8 | 0,2051 | 0,2032 |
| 16 | 0,1238 | 0,1707 |

Et pour le candidat « diviseur 512 » : 0,2760 en place, **0,2656 déplacé**.

**Le champ faux marque aussi bien, parfois mieux, que le champ vrai.** La
corrélation ne mesure pas la réalisation : elle mesure le fait que `h_sans_bruit`
est soustrait des deux côtés, si bien que *n'importe quelle* perturbation de
notre surface corrèle avec l'erreur que nous faisons déjà. Toute la famille de
mesures « la hauteur de surface de l'overworld » est donc **aveugle** à la
question posée. Le même diagnostic explique le balayage de gain : un champ plus
lisse gagne mécaniquement, quelle que soit sa forme.

Chiffre du témoin, sur 120 chunks : erreur rms de surface **sans bruit du tout**
1,736 bloc ; avec le candidat « 14 octaves » 1,824 ; avec « diviseur 512 »
1,823 ; avec l'actuel 3,188. **Les deux candidats sont pires que retirer le
terme entièrement.** Un critère qui préfère l'absence d'un terme ne peut pas
choisir sa constante.

## 3. L'oracle qui tranche : le Nether

Le Nether est le seul endroit où `old_blended_noise` se voit seul.
`worldgen/noise_settings/nether.json` ne nomme **ni** `depth`, **ni** `factor`,
**ni** `jaggedness`, et pose `aquifers_enabled: false`. Sa `final_density` se
réduit à

```
squeeze(0.64 × interpolated(2.5 + gb × (−2.5 + 0.9375 + gh × (−0.9375 + N))))
gb = y_clamped_gradient(−8 → 0, 24 → 1)     gh = y_clamped_gradient(104 → 1, 128 → 0)
```

Entre y = 24 et y = 104 les deux gradients valent 1 et tout s'annule : la
formule devient `squeeze(0.64 × N)`. **Un bloc est de la pierre exactement là où
`base_3d_noise` est positif.**

Au-dessus de y = 104, `gh` décroît et le seuil glisse :
`N > −0,9375 (1 − gh)/gh`, soit −0,041 à y = 105, −0,085 à 106, −0,134 à 107,
−0,188 à 108, … −0,9375 à 116. **La fraction de pierre à chaque hauteur est donc
la fonction de survie de ce bruit, écrite par le jeu lui-même.** Idem sous
y = 24 avec `gb` et la constante 2,5.

Rien là-dedans n'exige que les graines coïncident — ce qui est essentiel,
puisque le Nether utilise `legacy_random_source: true` et que notre routeur ne
l'implémente pas. On compare deux **distributions**, pas deux réalisations.

`scripts/reference_nether.sh` génère le monde : `execute in minecraft:the_nether
run forceload add …` depuis la console suffit, aucun joueur n'a besoin d'entrer
dans la dimension. Douze patchs, 48 fichiers de région.

### Le verdict, seed 1234567890

`ov_parity --nether`, 273 chunks, 9828 colonnes. Fraction de pierre à chaque
hauteur du toit :

| y | seuil | le jeu | actuel | 14 octaves | diviseur 512 |
|---|---|---|---|---|---|
| 104 | 0 | 0,382 | 0,583 | 0,472 | 0,583 |
| 105 | −0,041 | 0,427 | 0,632 | 0,819 | 0,792 |
| 106 | −0,085 | 0,495 | 0,698 | 0,986 | 0,943 |
| 107 | −0,134 | 0,627 | 0,810 | **1,000** | 0,996 |
| 108 | −0,188 | 0,776 | 0,863 | 1,000 | **1,000** |
| 110 | −0,313 | 0,930 | 0,942 | 1,000 | 1,000 |
| 112 | −0,469 | 0,975 | 0,975 | 1,000 | 1,000 |
| 114 | −0,670 | 0,999 | 1,000 | 1,000 | 1,000 |

Le jeu a besoin d'un seuil de −0,47 pour atteindre 0,975. Les deux candidates y
sont à 1,000 dès −0,134. **Elles saturent quatre fois trop tôt**, exactement le
facteur que leur arithmétique prédit.

### Le chiffre, sans le biais de niveau

Nos deux courbes ne partent pas du même niveau (notre bruit est positif un peu
plus souvent que le sien, §5), et une distance point à point mélangerait cet
écart à la réponse. La **largeur** ne le fait pas : c'est le chemin que le seuil
parcourt pendant que la courbe du toit monte de 0,75 à 0,99, mesurée dans les
unités du bruit lui-même.

| | largeur, seed 1234567890 | rapport | largeur, seed 987654321 | rapport |
|---|---|---|---|---|
| **le jeu** | **0,4076** | 1,000 | **0,2680** | 1,000 |
| gain 0,25 | — (sature) | — | 0,1123 | 0,419 |
| gain 0,50 | 0,1971 | 0,484 | 0,1366 | 0,510 |
| gain 0,85 | 0,3114 | 0,764 | 0,1667 | 0,622 |
| **gain 1,00** | **0,4189** | **1,028** | **0,2155** | **0,804** |
| gain 1,20 | 0,4973 | 1,220 | 0,2813 | 1,050 |
| gain 1,40 | 0,5857 | 1,437 | 0,3271 | 1,221 |

Distance moyenne absolue à la courbe du jeu, bande du toit :

| gain | 0,25 | 0,50 | 0,70 | 0,85 | **1,00** | 1,20 | 1,40 | 1,70 |
|---|---|---|---|---|---|---|---|---|
| seed 1234567890 | 0,0931 | 0,0753 | 0,0611 | 0,0512 | 0,0408 | **0,0384** | 0,0434 | 0,0509 |
| seed 987654321 | 0,0598 | 0,0336 | 0,0144 | **0,0073** | 0,0129 | 0,0269 | 0,0421 | — |

Sur les deux seeds, **gain 0,25 est le pire point du balayage** et le minimum
est intérieur, entre 0,85 et 1,2. Les deux statistiques encadrent le vrai gain
dans **[0,80 ; 1,22]**.

**Conclusion : l'amplitude actuelle est correcte à ±20 % près. Ni « deux octaves
de moins » ni « un second diviseur de 512 » n'est la bonne arithmétique ; toutes
deux sont fausses d'un facteur ~4.**

## 4. Une erreur de fait dans le mandat, qui vaut d'être notée

Le mandat annonçait que « deux octaves de moins retire du contenu haute
fréquence ». **C'est l'inverse dans cette implémentation.** Les octaves
*croissent* : l'octave `i` est échantillonnée à `x·684,412·xz_scale·2⁻ⁱ` et
divisée par `2⁻ⁱ`, donc l'indice 15 est la **plus basse fréquence** et la
**plus grande amplitude**. Avec `xz_scale = 0,25` les longueurs d'onde sont
192, 96, 48, 24, 12, 6, 3 blocs pour i = 15…9, avec des poids 0,5 · 0,25 ·
0,125 … après division par 65 536.

Retirer deux octaves, c'est donc retirer **192 et 96 blocs** — la forme large —
et garder le détail fin intact. C'est le seul découpage qui donne exactement 4
(Σ₀¹³ 2·2ⁱ = 32 766 contre Σ₀¹⁵ = 131 070) ; retirer les deux plus fines donne
un rapport de 1,00005. Mesuré : le rapport `rms(N(x+1) − N(x)) / rms(N)` vaut
0,119 à 16 octaves **et à 16 octaves divisées par 4** (une mise à l'échelle ne
change pas un rapport), mais **0,159 à 14 octaves**. C'est ce rapport, et pas
l'amplitude, qui distingue une troncature spectrale d'un facteur — il est dans
le test.

## 5. Ce que le Nether révèle en passant, et qui reste ouvert

Aucun gain ne rattrape un écart de **niveau** : dans la bande à seuil nul
(y = 24…104), le jeu a 0,36 à 0,56 de pierre selon la hauteur, nous 0,48 à 0,62.
Notre `base_3d_noise` est positif plus souvent que le sien. Le gain ne peut rien
y faire — `signe(g·N) = signe(N)` — et les colonnes le montrent : toutes les
colonnes de gain sont identiques dans cette bande.

Trois explications restent ouvertes et ne sont pas départagées ici :
1. **la réalisation** — le Nether est semé par `legacy_random_source`, que nous
   n'implémentons pas ; notre tirage n'est pas le sien, et un champ rouge de
   190 blocs de longueur d'onde a une moyenne d'échantillon qui bouge ;
2. **le sélecteur** — `blend = (selector/10 + 1)/2` où `selector` peut atteindre
   ±510 (Σ 2·2ⁱ sur 8 octaves) : il est saturé presque partout, donc le résultat
   est *soit* `min_limit` *soit* `max_limit`, jamais un mélange. Si le jeu
   normalise cette pile, la commutation dure devient un fondu doux et la
   distribution change de forme sans changer beaucoup d'échelle ;
3. les carvers et les structures du Nether, qui ne sont pas soustraits.

C'est la piste suivante pour qui reprend ce terme, et le Nether est l'endroit
où la mesurer.

## 6. L'autre chose que le témoin a trouvée

Notre surface d'overworld est **40 % plus rugueuse que celle du jeu à un bloc de
distance**, et ce n'est pas `base_3d_noise` : la fonction de structure à d = 1
vaut 1,028 pour le jeu, 1,438 pour nous **avec le bruit retiré**, 1,555 avec.
Quelque chose d'autre — l'interpolation, `jaggedness`, ou le seuil de
`is_solid` — ajoute du grain fin que le jeu n'a pas. Aux grandes distances
l'accord est excellent (rapport 1,003 à d = 64). Ce n'est pas ce mandat, mais
c'est mesuré, reproductible par `ov_parity --surface`, et cela pèse sur le
déficit de surface autant que ce qui a été cherché ici.

## 7. Parité — inchangée, puisque rien n'a changé

Seed 1234567890, 60 chunks, pas de 3, glace exclue, 345 600 blocs.

| | avant | après |
|---|---|---|
| accord solide/air | **98,309 %** | **98,309 %** |
| pierre en trop | 1,016 % | 1,016 % |
| pierre manquante | 0,675 % | 0,675 % |
| accord avec `--carvers` | **99,141 %** | **99,141 %** |
| pierre en trop, carvers | 0,145 % | 0,145 % |

Histogramme du décalage de surface sur les 1779 colonnes sèches, avant comme
après : mode −1 à 23,05 %, −2 à 20,91 %, 0 à 13,10 %, moyenne −1,28.

Le mandat demandait un avant/après. Il est plat, et c'est le résultat : la
constante mise en cause n'était pas fausse.

## 8. Instruments laissés en place

- `OV_BASE3D_GAIN` — multiplie ce bruit et rien d'autre. Le balayage de
  `--nether` s'en sert : il reconstruit le routeur du Nether une fois par gain.
- `OV_BASE3D_SHIFT` — échantillonne le même bruit ailleurs. C'est le témoin du
  §2 ; sans lui, la corrélation de 0,32 obtenue en §2 aurait été prise pour une
  preuve.
- `ov_parity --surface [--dump=]` — la carte de surface, son écart-type, sa
  corrélation et sa fonction de structure, plus la même lecture de
  `base_3d_noise` lui-même.
- `ov_parity --nether [--sweep=g1,g2,…]` — l'oracle du §3.
- `scripts/reference_nether.sh` — le monde de référence du Nether.

`OV_BASE3D_OCTAVES` et `OV_BASE3D_DIV2` ont existé le temps de l'expérience et
sont retirés : la question qu'ils posaient est tranchée, et deux boutons capables
de changer le monde en silence ne valent pas d'être gardés. Les trois constantes
qu'ils mettaient en doute — 16 octaves, 512, 128 — sont maintenant nommées dans
`noise.cpp` avec la mesure qui les tient.

## 9. Sources

- `data/vanilla/1.20.1/generated/data/minecraft/worldgen/noise_settings/nether.json`
  — sortie du data generator du serveur 1.20.1 (SHA-1
  84194a2f286ef7c14ed7ce0090dba59902951553). C'est de là que vient la formule du
  §3, lue et non devinée.
- `run/reference-nether-1234567890` et `run/reference-nether-987654321` —
  générés par le vrai serveur via `scripts/reference_nether.sh`.
- Aucune source de code tierce n'a été consultée pour ce travail.
