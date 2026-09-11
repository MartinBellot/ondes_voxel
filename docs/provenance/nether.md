# Le Nether jouable : génération, deux dimensions, portails

*2026-09-10. Seed 1234567890. Oracles : `run/reference-nether-1234567890` (généré par le vrai
serveur 1.20.1 via `scripts/reference_nether.sh` ; supprimé une fois lu pour libérer le disque —
le script le régénère à l'identique), `scripts/measure_nether_portal.py` (le vrai serveur, un
client sonde qui traverse des portails), `scripts/check_nether_e2e.py` (notre serveur, le même
client).*

Ce document dit ce qui a été établi, comment, avec quels chiffres, et ce qui n'est **pas** fait.

---

## 1. Génération : le Nether cellule par cellule

### 1.1 `legacy_random_source`, enfin lu

`worldgen/noise_settings/nether.json` dit `legacy_random_source: true`. Rien ne le lisait : le
routeur et l'étage de surface fabriquaient une fabrique positionnelle Xoroshiro quel que soit le
réglage, et `amplitude-old-blended-noise.md` § 5 ne pouvait comparer le Nether qu'en
*distribution* (« notre tirage n'est pas le sien »).

`PositionalRandomFactory` (`ov/worldgen/random_factory.hpp`) porte l'une ou l'autre fabrique :
`new LegacyRandomSource(seed).forkPositional()` — **un** long, noms hachés par `String.hashCode`,
position XOR sur un long — ou l'équivalent Xoroshiro. Trois bruits sont remplacés dans le graphe
de densité d'un monde legacy, quoi que disent leurs fichiers :

| bruit | ce que le jeu en fait |
|---|---|
| `minecraft:temperature` | deux octaves à −7, **ancienne initialisation**, `new LegacyRandomSource(seed)` |
| `minecraft:vegetation` | idem, `seed + 1` |
| `minecraft:offset` (les `shift_x/z`) | une octave d'amplitude nulle : les bruits de biome du Nether ne sont pas décalés |

L'« ancienne initialisation » (`PerlinNoise::create_legacy`) tire les octaves en séquence depuis
le générateur lui-même, l'octave 0 d'abord, puis vers le bas, et **saute 262 tirages** pour chaque
octave hors plage ou d'amplitude nulle — la première `ImprovedNoise` est construite, et consomme
ses tirages, même quand son octave n'est pas gardée. `old_blended_noise` d'un monde legacy est
construit depuis un `new LegacyRandomSource(seed)` neuf.

Ces faits sont des hypothèses de fonctionnement confirmées par la mesure, pas lues dans un code :
aucune source tierce n'a été consultée. Le test est brutal — une seule erreur de tirage donne une
carte des biomes sans rapport :

| | biomes du Nether, cellules 4×4×4 |
|---|---|
| **3000 chunks, 3 072 000 cellules** | **3 071 999 identiques (100,000 %)** |

La cellule restante (jeu `soul_sand_valley`, nous `nether_wastes`) est isolée ; une égalité exacte
de distances dans la table des cinq biomes est l'explication la plus probable, elle n'est pas
vérifiée.

### 1.2 Le carver `nether_cave`

`NetherWorldCarver` est le carver de grottes avec quatre différences : borne externe 10 au lieu de
15, épaisseur `(f·2 + f)·2` (jamais l'élargissement une fois sur dix), rapport vertical 5, et des
fournisseurs de forme **constants** dans son JSON (`horizontal/vertical_radius_multiplier` 1,0,
`floor_level` −0,7, `yScale` 0,5) — donc aucun tirage là où le carver de l'overworld en fait trois.
`below_top` se résout contre la profondeur du **générateur** (128), pas la hauteur du chunk (256) :
y ∈ [0, 126], marge haute 128 − 1 − 7 = 120.

| | masques `CarvingMasks.AIR` (chunks laissés au statut `carvers`) |
|---|---|
| 166 chunks | **166 / 166 identiques au bit près, 541 324 cellules**, aucune d'un seul côté |

La lave : le JSON dit `lava_level: above_bottom 10`, et le jeu ne l'utilise pas. Mesuré dans les
cellules que le jeu a creusées : **lave à y ≤ 31, `cave_air` à y ≥ 32**, sans exception sur les
166 chunks (y = 2..4 : la bedrock). L'air creusé est `cave_air` (l'overworld écrit `air`), et ce
qui peut être creusé est `#minecraft:nether_carver_replaceables`.

### 1.3 L'octave lue à l'envers — et ce qu'elle cachait dans l'overworld

Avec les biomes exacts, les blocs ne l'étaient pas : **76,9 %** d'accord sur 30 chunks. Or entre
y = 24 et 104 la densité du Nether se réduit à `squeeze(0,64 × base_3d_noise)` : l'accord
solide/vide *est* l'accord du bruit lui-même, cellule par cellule. C'était donc le bruit.

`BlendedNoise` lisait ses trois piles d'octaves dans l'ordre du tableau ; le jeu les lit par le
haut (l'octave de pas 1 est celle que l'ancienne initialisation a créée en premier). Chaque octave
échantillonnait la bonne fréquence avec la permutation d'une autre : même distribution, autre
monde — exactement ce que la mesure « par distribution » ne pouvait pas voir.

| Nether, 200 chunks `carvers`, par `generate()` | avant | après |
|---|---|---|
| blocs identiques | 76,90 % (30 chunks) | **99,802 %** (13 081 311 / 13 107 200) |
| solide / vide (air, cave_air, lave) | 81,13 % | **99,973 %** |

Dans l'overworld, l'ordre seul **dégrade** l'accord, et la graine seule aussi. Les deux ensemble —
l'ordre, et `old_blended_noise` semé par `fromHashOf("minecraft:terrain")` au lieu d'un second
`fork()` de la graine du monde :

| overworld, 60 chunks, `ov_parity --terrain --carvers` | solide/air | pierre en trop | pierre manquante | mode du décalage de surface |
|---|---|---|---|---|
| avant | 99,141 % | 0,145 % | 0,714 % | −1 (23 %) |
| ordre seul | 99,016 % | 0,647 % | 0,337 % | — |
| graine seule | 98,837 % | 0,354 % | 0,808 % | — |
| **les deux** | **99,763 %** | **0,012 %** | 0,225 % | **0 (96,06 %)** |

C'est le « décalage systématique de la surface d'environ deux blocs dont la cause n'est pas
localisée » de la feuille de route : il était là. Et c'est le piège 7 du briefing à l'état pur —
un paramètre dont chaque moitié empire l'agrégat.

### 1.4 Le reste de la chaîne

- Le remplissage de bruit ne couvre que la hauteur du bruit (128) : au-dessus, la densité d'un
  `y_clamped_gradient` dépassé dirait « pierre » jusqu'à 255.
- `default_block` / `default_fluid` viennent des réglages (netherrack, lave ; sous le niveau de
  la mer 32, la lave).
- Les features résolvent `above_bottom` / `below_top` contre l'étendue du **générateur**
  (`max(min_y)`, `min(hauteur, profondeur)`) : quartz 0,57 % → **99,95 %**, or 0,19 % → **100 %**,
  débris antiques 62,5 % → **100 %** (25 chunks `full`).
- Les sources du Nether ont un `valid_blocks` réduit à un nom (un holder set d'un élément) : lu.
- Les deux patchs de feu se chargent (`NotAirBelow`) : leur prédicat exige déjà du netherrack
  (sol des âmes) dessous, donc la moitié « face solide dessous » de la règle du feu est toujours
  vraie. Feu **43/43**, feu des âmes **6/6**.

| 25 chunks `full`, pipeline + décorateur du Nether | blocs identiques |
|---|---|
| avant (étendue du chunk) | 99,389 % |
| **après** | **99,913 %** |

### 1.5 Ce qui reste, nommé

- **Features non construites** (19, le décorateur les liste) : `glowstone`, `glowstone_extra`
  (`glowstone_blob`), `crimson_fungi`, `warped_fungi` (`huge_fungus`), `crimson/warped_forest_vegetation`
  (`nether_forest_vegetation`), `nether_sprouts`, `twisting_vines`, `weeping_vines`, `basalt_blobs`,
  `blackstone_blobs` (`netherrack_replace_blobs`), `basalt_pillar`, `delta`, `small/large_basalt_columns`,
  et les quatre patchs de champignons (leur règle de survie lit la lumière). Structures : forteresse,
  bastion, fossiles — non placées.
- **Surface de la vallée des âmes** : ~10 000 blocs sur 13,1 M dans les chunks `carvers` — des sols
  directement sous une nappe de lave, et les 1 à 3 blocs au-dessus de la bedrock, où nous posons
  du sol des âmes et le jeu laisse du netherrack. Une hypothèse (une coulée de pierre qui touche
  le fond du monde n'a pas de « sol » en dessous) a été **mesurée fausse** — 99,80 % → 99,66 % —
  et retirée. Non localisé.
- La lave des sources a coulé dans les chunks `full` du jeu (256 blocs) : les fluides ne coulent
  pas pendant notre génération.

`tools/ov_netherparity` : biomes, masques et blocs par `generate()` (`--chunks=`,
`--biome-chunks=`), chunks finis par le pipeline (`--full=`), paires de désaccord en détail
(`--pairs= --pair-game= --pair-ours=`), et placement de portails (`--portal=`) sur notre terrain,
sur un monde du jeu (`--portal-world=`) ou sur les paquets que le jeu a envoyés
(`--portal-packets= --portal-unbuild=`, § 3.3).

### 1.6 La mer de lave rendue — `aquifers_enabled` (2026-09-11)

`nether-2.md` a vu la lave régresser : dans les chunks `carvers` du Nether de référence régénéré,
les cellules sous y 32 étaient de la lave chez le jeu et de l'air chez nous, « alors que le code du
carver n'a pas changé ». Il n'avait pas changé : la cause est ailleurs, et l'historique la date.
`73946c9` (l'aquifère, dans l'étage de bruit et dans les carvers) **n'est pas un ancêtre** de
`a1cd76b` (le Nether) : les deux ont été faits en parallèle, le 99,802 % de § 1.3 a été mesuré sans
aquifère, et la fusion a branché l'aquifère de l'overworld partout — y compris dans le Nether.

Or les réglages du Nether et de l'End disent `"aquifers_enabled": false`, et **rien ne lisait ce
champ**. `ChunkGenerator::aquifer_active()` ne regardait que la présence des nœuds du routeur ; le
routeur du Nether les a (constantes nulles), donc l'aquifère répondait, avec sa propre notion de
niveau local, là où le jeu n'a que la règle globale : sous 32, le fluide par défaut, la lave. Le
carver n'y est pour rien — sous y 31 il pose la lave lui-même ; c'est la mer de lave **de l'étage
de bruit** que l'aquifère vidait.

Correction : `NoiseRouter::aquifers_enabled()` lit le champ, `aquifer_active()` l'exige. Mesure,
`ov_netherparity --world=run/reference-nether-987654321/world/DIM-1 --seed=987654321 --chunks=200
--biome-chunks=0` (200 chunks `carvers`, 13 107 200 blocs, masques 158 / 158 au bit près) :

| | avant | après |
|---|---|---|
| blocs identiques | 12 861 098 — 98,122 % | **13 061 328 — 99,650 %** |
| lave du jeu retrouvée | 67 464 / 267 731 — 25,198 % | **267 694 / 267 731 — 99,986 %** |
| solide / vide | 99,973 % | 99,973 % |

Le solide/vide ne bouge pas, et c'est ce qu'il fallait : la correction ne change que *quel vide*.
Le reste de l'écart (0,35 %) est ce que § 1.5 nommait déjà — des features des voisins écrites dans
ces chunks (verrues, basalte, pierre noire) et la surface de la vallée des âmes.

**L'End** a lui aussi `aquifers_enabled: false` et suit donc la même règle désormais. Il n'a pas été
remesuré contre le jeu (aucun monde de référence de l'End sur le disque) ; la règle globale n'y
remplit rien (niveau de la mer 0, plancher 0), et 64 chunks de l'île principale générés par le
chemin du serveur (`ov_gendet --export --dimension=end`) ne contiennent **aucun** bloc de fluide.

---

## 2. Deux niveaux dans le serveur

`server.cpp` a été écrit pour un seul monde : sa carte de chunks, son ensemble sale et son
générateur sont des variables locales qu'on atteint par leur nom à des centaines d'endroits. Le
Nether n'a pas été greffé en réécrivant tout cela : l'overworld garde ses locales, et le Nether a
`NetherWorld` (`src/ov_server/src/nether_travel.hpp`) — les mêmes quatre choses réunies —, et le
serveur s'adresse à « le niveau où est ce joueur » par une petite `DimensionView`. Chaque helper qui
écrit un bloc a sa version `_in(dimension, …)` ; l'ancien nom reste, comme enveloppe de
l'overworld, et le reste du fichier n'a pas bougé.

| | overworld | Nether |
|---|---|---|
| carte de chunks, tickets | locales de `run()` | `NetherWorld::chunks()` |
| génération | `GeneratedWorld("overworld")` + ses workers | `GeneratedWorld("nether")` + ses workers |
| disque | `region/` | **`DIM-1/region/`**, comme vanilla |
| niveau (fluides, redstone) | `level` | `nether_level`, **`ultrawarm`** (la lave coule à 7 blocs, toutes les 10 ticks) |
| écriture d'un joueur | `player_level` | `nether_player_level` |

- **Le Nether est construit au premier besoin** : la première traversée, ou le premier joueur qui se
  reconnecte en s'y trouvant. Un serveur que personne n'emmène dans le Nether ne paie pas une seconde
  pile de génération. `OV_NETHER=0` le coupe ; un joueur sauvegardé dans le Nether est alors
  **refusé**, avec la raison, plutôt que posé aux coordonnées du Nether dans l'overworld. Coût
  nommé : la construction de la pile (5 piles, quelques secondes en Debug) a lieu sur le thread de
  tick, sous `chunk_mutex`.
- Le joueur porte sa `DimensionId`. Le streaming, le creusement, la pose, les objets au sol, les
  Block Update, le mouvement et « qui voit qui » passent par elle ; `broadcast` ne parle plus
  qu'aux joueurs de l'overworld, `broadcast_in` à ceux d'un niveau, `broadcast_all` (la liste des
  joueurs) à tous.
- Fichier joueur : `Dimension` est écrit, et `minecraft:the_nether` est lu là où il était
  refusé. `minecraft:the_end` l'est toujours.
- Un joueur mort dans le Nether réapparaît au point d'apparition du monde, dans l'overworld.
- `/tp` déplace un joueur dans le niveau où il se trouve. Il n'y a pas d'`/execute in`.

**Pas fait, et nommé** : les mobs (l'`EntityWorld` est celui de l'overworld ; aucun mob n'apparaît
ni ne traverse dans le Nether) ; les conteneurs, pancartes, établis, fours, plantations et la TNT
dans le Nether (le clic droit y fait les règles de bloc et d'objet d'`ItemUse`, puis une pose
simple ; une TNT allumée dans le Nether n'est pas amorcée, et le serveur le dit) ; les commandes
qui écrivent des blocs (`/setblock`, `/fill`) agissent sur l'overworld ; le random tick du Nether ;
le comparateur y lit « pas de conteneur » (−1). La lumière du ciel du Nether n'est pas calculée — il
n'en a pas — seule la lumière des blocs l'est.

---

## 3. Portails

### 3.1 Les règles, et d'où elles viennent

La page « Nether portal » du Minecraft Wiki (Java 1.20) donne : cadre d'obsidienne, intérieur de
2×3 à 21×21, **coins facultatifs** ; allumé par un feu posé dedans ; 80 ticks dans le portail en
survie, 1 en créatif ; 300 ticks de recharge ; échelle 1:8 (`floor(x/8)`, y inchangé, bornée à la
bordure ±29 999 983) ; recherche du portail le plus proche en distance **euclidienne y compris**
dans un carré de rayon 128 (overworld) ou 16 (Nether) ; à défaut, un nouveau portail au point le
plus proche, à 16 blocs au plus, qui a trois rangées de quatre blocs solides avec quatre d'air
au-dessus, sinon une seule rangée, sinon forcé à la cible avec y borné à [70, hauteur − 10] sur une
plateforme 2×3 d'obsidienne ; cadre toujours 4×5, coins compris. C'est la spécification ; ce que la
page ne dit pas — l'ordre de visite des candidats à égalité, le détail de la position de sortie —
est mesuré.

- `ov_gameplay/nether_portal.{hpp,cpp}` : la forme (`frame_at`, X essayé avant Z), l'allumage, la
  cascade qui vide un portail dont le cadre casse (un voisin dans son plan seulement : un bloc posé
  contre sa face ne fait rien, un coin non plus), la position relative, la recherche, la création.
- **Tout feu qui atterrit dans un cadre l'allume** : la boucle de règlement (`WorldTicks::settle`)
  passe chaque écriture aux règles de portail. Le briquet, la boule de feu, un distributeur —
  sans chemin propre à chacun. Un tick de retard sur vanilla (qui allume dans `onPlace`), nommé.

  **Le point d'entrée, pour qui touche au feu ou au briquet** : un seul, et il ne dépend d'aucun
  des deux. `WorldTicks::attach_portals(&rules)` (server.cpp, bloc `// ── nether ──`) branche
  `gameplay::PortalRules::on_block_changed(level, pos)` sur **chaque** position écrite dans un
  niveau ; cette fonction allume le cadre si le bloc écrit est `minecraft:fire` ou
  `minecraft:soul_fire`, et sinon vérifie qu'un portail voisin tient encore. Un nouveau chemin qui
  pose du feu — propagation, foudre, boule de feu — allume donc un portail sans rien de plus, à
  condition d'écrire à travers un `LevelWriter` du serveur (`ServerLevel`, `PlayerLevel`).
  `ItemUse` (le briquet) ne sait rien des portails et n'a pas à le savoir.
- **Le POI `nether_portal`** n'est pas persisté : il est relu au moment de la recherche, depuis les
  chunks résidents (seulement les sections dont la palette nomme un bloc de portail) et depuis les
  fichiers de région pour les autres. C'est la reconstruction « au chargement si besoin ».

### 3.2 L'oracle : un joueur sonde qui traverse, chez vanilla

`scripts/measure_nether_portal.py` : le vrai serveur (seed 1234567890), six cadres 4×5 posés par la
console dans l'overworld (trois hauteurs, deux axes), allumés par un bloc de feu, un client sonde
téléporté au centre de chacun qui envoie sa position à chaque tick — le serveur ne découvre qu'un
joueur est dans un portail qu'en traitant un paquet de mouvement. Il note le Respawn et la
position synchronisée qui suit, sort du portail d'arrivée, attend la recharge, y rentre, et note
son retour. Les portails créés sont relus dans `DIM-1/region`. **Deux passages identiques**, à la
décimale.

| départ (intérieur, axe) | arrivée Nether | portail créé (coin min.) | retour overworld |
|---|---|---|---|
| (0, 100, 0) x | (−9,0 ; 97 ; −6,5) | (−10, 97, −7) | (1,0 ; 100 ; 0,5) — le même |
| (1000, 70, −600) x | (120,0000122 ; 82 ; −88,5) | (119, 82, −89) | (1001,0000092 ; 70 ; −599,5) — le même |
| (−2500, 90, 1800) z | (−301,5 ; 91 ; 223,99995) | (−302, 91, 223) | le même |
| (4000, 120, 4000) z | (509,5 ; 92 ; 516,99995) | (509, 92, 516) | **(4078,5 ; −43 ; 4145,0) : un nouveau** |
| (−800, 40, −3200) x | (−100,99999 ; 45 ; −410,5) | (−102, 45, −411) | le même |
| (2400, 64, 3100) z | (304,5 ; 61 ; 393,99995) | (304, 61, 393) | le même |

- Tous les portails créés sont **2×3 dans un cadre 4×5 coins compris (14 obsidiennes)**, sans
  plateforme : aucun départ n'a atteint les replis.
- On arrive au **centre** du nouveau portail, sur son sol, et on revient au même point relatif du
  portail de départ : la position relative est conservée.
- Le quatrième retour est un vrai cas de création *dans l'overworld* : le portail d'origine
  (4000, 120, 4000) est à 76 blocs en x et 136 en z de la cible (4076, 92, 4135), hors du carré de
  128 — le jeu en construit un autre, à y = −43.
- Les dernières décimales (120,0000122 plutôt que 120) viennent des dimensions de l'entité en
  `float` (0,6F, 1,8F) élargies en double, et d'un ajustement de collision que le jeu applique à la
  position de sortie. Nous reproduisons les floats ; l'ajustement final n'est pas reproduit, écart
  ≤ 5·10⁻⁵ bloc, nommé.

### 3.3 Le placement : l'algorithme, séparé du terrain

Un portail créé dépend de deux choses : l'algorithme et le terrain où il cherche. Pour les séparer,
la sonde vanilla enregistre les paquets Chunk Data and Update Light qu'elle reçoit en arrivant —
le terrain **tel que le jeu l'avait quand il a construit**, avant que rien n'ait tiqué — et
`ov_netherparity --portal= --portal-packets= --portal-unbuild=` les décode avec notre propre
`parse_chunk_data`, retire le portail que le jeu y a ajouté (la rangée du bas du cadre redevient
du netherrack, le reste de l'air), et y fait tourner notre algorithme.

| cible (x/8, y, z/8) | le jeu | notre algorithme, **terrain du jeu** | notre algorithme, notre terrain |
|---|---|---|---|
| (0, 100, 0) x | (−10, 97, −7) | **(−10, 97, −7)** | (−10, 97, −7) |
| (125, 70, −75) x | (119, 82, −89) | **(119, 82, −89)** | (118, 82, −87) |
| (−313, 90, 225) z | (−302, 91, 223) | **(−302, 91, 223)** | (−302, 91, 223) |
| (500, 120, 500) z | (509, 92, 516) | **(509, 92, 516)** | (509, 92, 516) |
| (−100, 40, −400) x | (−102, 45, −411) | **(−102, 45, −411)** | (−102, 45, −411) |
| (300, 64, 387) z | (304, 61, 393) | **(304, 61, 393)** | (305, 59, 393) |

**Sur le terrain du jeu : 6 / 6, au bloc.** L'algorithme écrit depuis la spécification du wiki —
anneaux de Tchebychev croissants autour de la cible, x puis z, candidat = sol d'une colonne d'air,
trois rangées de quatre blocs solides sous quatre d'air remplaçable, la plus proche en distance
3D — est celui du jeu sur ces six cas. Il ne l'a été qu'après une correction mesurée : « remplaçable »
était « sans boîte de collision », qui accepte un champignon du Nether ou une liane ; avec le tag
`#minecraft:replaceable`, le cas 2 passe de (119, 82, −88) à (119, 82, −89).

Sur notre terrain, deux cas diffèrent encore, et c'est **le terrain** : les features que nous ne
générons pas (§ 1.5 — champignons géants, végétation, lianes) changent ce qui est remplaçable ou
solide autour de la cible. Les tailles de l'échantillon sont celles-là : six portails, les
paquets de 10 à 66 chunks reçus en trois secondes par portail.

### 3.4 Notre serveur, le même scénario

`scripts/measure_nether_portal.py --ours` rejoue le scénario de § 3.2 contre `ov_dedicated`, sur un
overworld généré à la même graine (`OV_WORLDGEN_SEED=1234567890`). Mesuré, en Debug, sur une
machine partagée par neuf agents :

| départ | portail construit par notre serveur | arrivée | le jeu |
|---|---|---|---|
| (0, 100, 0) x | **(−10, 97, −7)** | **(−9,0 ; 97,0 ; −6,5)** | (−10, 97, −7) ; (−9,0 ; 97,0 ; −6,5) — **identiques** |
| (−800, 40, −3200) x | **(−102, 45, −411)**, 6 blocs de portail et 14 obsidiennes, les mêmes | (−101,0 ; 45 ; −410,5) | (−100,99998778 ; 45 ; −410,5) : 1,2·10⁻⁵ bloc, l'ajustement de collision nommé |

Les deux portails construits par notre serveur **sont ceux du jeu, bloc pour bloc**, et les deux
arrivées aussi (la seconde à l'ajustement de sortie près). Les quatre autres départs **ne sont
pas mesurés** sur notre serveur, et c'est dit : sur trois passages, l'overworld généré en Debug
n'a pas chargé leur zone à temps (jusqu'à 301 s), une traversée a attendu ses chunks plus que la
minute de la sonde et a été abandonnée à sa sortie du portail — ce qui a conduit à journaliser
l'attente et l'abandon —, et un retour a été bloqué par la recharge (piège 10). Le retour vers le
portail d'origine est, lui, mesuré sur notre serveur par le bout-en-bout (§ 4) : au bloc et à la
décimale.

---

## 4. De bout en bout, contre notre serveur

`scripts/check_nether_e2e.py` : notre `ov_dedicated` (superflat, `--survival`), un client sonde qui
fait tout par le protocole.

| étape | mesuré |
|---|---|
| cadre 4×5 posé par 14 Use Item On | 14 blocs, tous obsidienne |
| briquet sur le bloc du bas | les 6 blocs intérieurs deviennent `nether_portal` (Block Update) |
| Respawn vers `minecraft:the_nether` | reçu ; la **première** traversée prend 14 à 24 s selon la charge — la pile du Nether se construit, puis les workers génèrent les chunks du portail à créer |
| arrivée | (−9,0 ; 73,0 ; −5,5), au centre du portail créé en (−10, 73, −6) |
| chunks du Nether | le 9×9 autour du joueur : 96 paquets en 7,7 s (Debug) |
| **retour**, recharge écoulée | Respawn vers `minecraft:overworld` après **4,27 s = 85 ticks** (80 + la cadence de la sonde ; 87 sur un autre passage) |
| point d'arrivée du retour | (4,0 ; −59,0 ; 3,5) = le point d'entrée, au bloc et à la décimale |
| sauvegarde | `DIM-1/region` contient le portail : 6 blocs de portail, 14 obsidiennes |
| fichier joueur | `Dimension: minecraft:overworld` |

**11 / 11 contrôles passent** sur le dernier passage. La traversée ne fige plus le thread de
tick : sans le ticket `Transient`, la génération des seize chunks du portail à créer y prenait
**9,2 s** d'un bloc. Revers de la médaille, mesuré et désormais journalisé : une traversée qui
attend ses chunks est abandonnée si le joueur quitte le portail avant leur arrivée.

---

## 5. Ce qui n'est pas fait — nommé

| sujet | état |
|---|---|
| L'End | refusé par nom (`dimension_by_name`, fichier joueur) |
| Mobs dans le Nether, mobs qui traversent | non : l'`EntityWorld` est celui de l'overworld |
| Conteneurs, pancartes, établis, fours, plantation, TNT dans le Nether | non ; la TNT allumée n'y est pas amorcée, et le serveur le dit |
| `/setblock`, `/fill` dans le Nether ; `/execute in` | les commandes d'écriture agissent sur l'overworld ; `/tp` reste dans le niveau du joueur |
| Allumage par propagation du feu | pas de propagation du feu dans ce serveur ; tout feu *posé* (briquet, boule de feu, distributeur) allume |
| L'eau qui s'évapore au seau | pas de seau dans ce serveur (Use Item n'est pas traité) ; le trait `ultrawarm` est déjà porté par les niveaux du Nether |
| Les lits qui explosent | pas de lit (`ItemUse` le nomme non supporté) |
| La boussole qui tourne | côté client |
| Première traversée d'une vie de serveur | construit la pile de génération du Nether sur le thread de tick (quelques secondes en Debug) |
| Ajustement de collision de la position de sortie | non reproduit, ≤ 5·10⁻⁵ bloc |
| Features du Nether | 19 non construites (§ 1.5), structures non placées |
| Surface de la vallée des âmes | ~10 000 blocs sur 13,1 M, non localisé (§ 1.5) |
| Durée d'attente : `time++ >= 80` | l'ordre exact incrément / comparaison n'est pas mesuré ; le retour mesuré prend 87 ticks, sonde comprise |

---

## 6. Pièges payés ici

1. **Un paramètre dont chaque moitié empire l'agrégat.** L'ordre des octaves de
   `old_blended_noise` seul, ou sa graine seule, font *baisser* l'accord de l'overworld ; les deux
   ensemble le font monter de 99,141 % à 99,763 %. Un balayage un paramètre à la fois ne l'aurait
   jamais trouvé. C'est le Nether — où ce bruit est seul — qui a désigné le coupable.
2. **Mesurer une distribution n'a rien dit de la réalisation.** L'ancienne mesure du Nether
   (`amplitude-old-blended-noise.md`) ne pouvait pas voir des octaves permutées : même loi, autre
   champ. Dès que la graine a été juste, la comparaison cellule par cellule l'a vu en une ligne.
3. **Le vrai serveur déclare plus d'octets de sections qu'il n'en écrit** dans Chunk Data (14 859
   déclarés, 14 850 écrits). `parse_chunk_data` exigeait l'égalité et refusait donc tout chunk
   venu d'un serveur vanilla — `ov_netclient` compris. Corrigé : lire dans le tampon déclaré et
   sauter le reste, comme le client vanilla.
4. **Un terrain que le jeu a fait tiquer n'est plus celui où il a construit.** Un instantané pris
   après 20 s de forceload avait vu la lave couler : l'oracle de placement propre est le terrain
   que le jeu *envoie* au joueur qui arrive, décodé depuis ses propres paquets, portail retiré.
5. **« Pas de boîte de collision » n'est pas « remplaçable ».** Un champignon du Nether, une liane,
   un champignon n'ont pas de boîte et ne sont pas remplaçables ; le tag `#minecraft:replaceable`
   est la bonne source (celle que `falling_block` utilisait déjà).
6. **`try_lock` par le propriétaire d'un `std::mutex` est indéfini.** La traversée tient
   `players_mutex` ; appeler depuis elle un flush qui fait `try_lock` dessus perdait, sans erreur,
   les Block Update d'un portail construit.
7. **Un joueur n'est « dans » un portail que quand le serveur traite son mouvement** : une sonde
   immobile qui n'envoie pas sa position à chaque tick ne traverse jamais, chez vanilla comme chez
   nous.
8. **Notre serveur refuse une connexion *après* Login Success** quand le chunk d'apparition n'est
   pas arrivé en 20 s — un monde généré en Debug sur une machine chargée n'y arrive pas (0 bloc
   généré en 31 s mesuré). La sonde ne voit pas de raison, seulement une socket fermée
   (`EOFError`) au premier paquet suivant. Une sonde sur un monde généré doit réessayer de se
   connecter, avec une échéance.
9. **`/fill` et `/setblock` refusent un chunk qui n'est pas chargé** — « That position is not
   loaded », chez vanilla comme chez nous. Vanilla a `/forceload` ; notre serveur ne l'a pas. Une
   sonde pilotée par la console doit d'abord charger la zone avec son propre ticket de joueur (se
   placer au-dessus, puis réessayer jusqu'à ce que la réponse ne soit plus « not loaded ») : sans
   cela, un cadre loin du point d'apparition n'est jamais posé, le feu seul l'est, et la sonde
   attend dans le vide un portail qui n'existe pas.
10. **La recharge se compte en ticks du serveur, pas en secondes.** 300 ticks, rafraîchis tant
    que le joueur reste dans un portail : un serveur Debug surchargé (« can't keep up », ticks
    avalés) n'en avait pas couru 300 en 16,5 s, la sonde est rentrée dans le portail avec une
    recharge en cours et y est restée indéfiniment — la règle de vanilla, appliquée. La sonde
    attend maintenant une minute, et s'arrête si un retour échoue au lieu de mesurer la suite
    dans la mauvaise dimension.
11. **Une traversée qui attend ses chunks meurt en silence si le joueur sort du portail.** Elle
    est désormais journalisée (« waiting for the chunks … », « the crossing is dropped »).
