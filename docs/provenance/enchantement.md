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

La propriété 3 est la graine `& 0xFFFFFFF0` — et, comme toute *Container Property*, elle voyage
en **short** : seuls les bits 4 à 15 arrivent. Le test compare `(i16)(seed & -16)`.

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

*(Les chiffres de ce paragraphe sont remplis à partir de `normalized/enchanting.json`.)*

---

## 4. Les 39, un par un

| Enchantement | Où vit l'effet |
|---|---|
| Protection, Protection contre le feu / les explosions / les projectiles, Chute amortie | EPF dans le chemin des dégâts du joueur |
| Respiration | **non branché** : `respiration_saves_air()` existe, `SurvivalSession::tick_air` ne reçoit pas le casque |
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

**1. Le serveur envoyait un second keep-alive avant d'avoir lu la réponse au premier — et
coupait le client qui avait tout bien répondu.** Le gestionnaire de réponse refuse tout
identifiant autre que le dernier (comme vanilla) ; l'émetteur, lui, en envoyait un toutes les
10 s **même en attente de réponse** (vanilla ne le fait jamais). Sur une machine chargée — un
tick médian de 1,6 s, un p90 de 6,2 s mesurés dans le journal du serveur —, la réponse au premier
était traitée après l'envoi du second, le gestionnaire rendait `false`, et `listener.cpp` fermait
la socket sans rien écrire. La sonde de bout en bout a perdu sa connexion deux fois, au même
endroit, pendant la pose des quinze étagères. Corrigé dans `server.cpp` : pas de nouveau
keep-alive tant que le précédent attend. **Ce n'est pas un bogue de l'enchantement** ; tout
joueur d'un serveur qui rame y était exposé.

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

**5. La propriété 3 de la table est un short.** `seed & 0xFFFFFFF0` est ce que dit l'archive du
protocole ; sur le fil, seuls les bits 4 à 15 arrivent, comme pour toute *Container Property*.

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

---

## Reproduire

```bash
lockf /tmp/ov-vanilla.lock python3 scripts/measure_enchanting.py   # l'oracle, ~30 min
python3 scripts/check_enchanting.py                                # les chiffres, la table plate
./build/macos-debug/bin/test_ov_gameplay "[enchanting]"            # parité table / Protection
./build/macos-debug/bin/test_ov_server "[enchanting]"              # parité enclume, fenêtres
python3 scripts/check_enchanting_e2e.py                            # notre serveur, de bout en bout
```
