# L'eau implicite : herbes marines, kelp, colonnes de bulles — et le nénuphar

> Rapport de bogue de l'utilisateur : « Plantes aquatiques : le bloc où se
> trouve l'herbe marine contient de l'AIR, pas de l'eau. Et la texture du
> nénuphar est mal gérée. »

## 1. Ce qui était faux

**Trois façons** pour un état de bloc de contenir un fluide, et le code n'en
connaissait que deux, et pas partout les mêmes :

1. il **est** le fluide — `minecraft:water` ou `minecraft:lava`, à son `level` ;
2. il est `waterlogged=true` — une source d'eau tenue par le bloc ;
3. il est l'un des **cinq blocs toujours dans l'eau sans propriété pour le
   dire** : `seagrass`, `tall_seagrass`, `kelp`, `kelp_plant`,
   `bubble_column`. Leur fluide est une source d'eau, quel que soit leur état.

Relevé site par site (avant ce correctif) :

| Question « est-ce de l'eau ? » | Où | Réponse avant |
|---|---|---|
| Le maillage dessine-t-il de l'eau dans la case ? | `ov_render/chunk_mesher.cpp` | **seulement** `water`/`lava` : un bloc engorgé (escalier, corail, cornichon de mer) **et** les cinq plantes étaient dessinés **sans leur eau** — une bulle d'air dans la mer, un trou dans la surface quand la plante est dans la case de surface. C'est le bogue de l'utilisateur. |
| La face entre l'eau et la case voisine est-elle cachée ? | `ChunkSectionView::fluid_at` | **id de bloc** `water` : la mer dessinait une face contre chaque herbe marine |
| Moteur de fluides du serveur | `gameplay/fluid.cpp` `fluid_at` | `water`/`lava` + `waterlogged` : **l'herbe marine et le kelp n'étaient pas de l'eau** — ni source pour leurs voisines, ni eau laissée derrière eux |
| Noyade des zombies | `server.cpp` | `holds_fluid && nom == water` : **uniquement le bloc d'eau** — un zombie dans le kelp ne se noyait pas |
| Yeux du joueur sous l'eau (air, noyade) | `server.cpp` | nom `minecraft:water` : un joueur dans un escalier engorgé, une herbe marine ou du kelp **respirait** |
| Feu éteint, flèche éteinte | `fire_session.cpp`, `projectile.cpp` | `water` + `waterlogged`, ou le bloc `water` seul : le kelp n'éteignait rien |
| Physique du joueur, brouillard sous l'eau, musique sous l'eau | `apps/ov_voxel/session.cpp` | `holds_fluid` : juste, **sauf `kelp_plant`** (voir § 2) |
| Heightmaps, apparition, pathfinding, sable qui tombe, pluie | `holds_fluid` / listes | juste, sauf `kelp_plant` |

## 2. La règle, et d'où elle vient

**Une seule requête**, au niveau du registre, que client et serveur lisent :
`BlockRegistry::fluid(state)` → `{type, level, is_fluid_block}`, une table
d'un octet par état (24 135 octets) construite au chargement à partir des noms,
de la propriété `level` et de la propriété `waterlogged`.
`holds_fluid(state)` en dérive désormais aussi.

Sources :

* **Minecraft Wiki, « Waterlogging »** — le tableau de comportement marque
  exactement cinq blocs « Inherent » en Java : Bubble Column, Kelp, Kelp Plant,
  Seagrass, Tall Seagrass. Et : « If the non-cube block is destroyed, the
  water source block still remains in the original spot it came from. »
* **Minecraft Wiki, « Kelp »** — « in Java Edition, the water cannot be removed
  by using a bucket ».
* **Mesure existante** (`data/vanilla/1.20.1/normalized/motion.json`, voir
  `physique-blocs.md`) : quatre de ces blocs posés en haut d'une colonne d'un
  vrai serveur 1.20.1 lèvent MOTION_BLOCKING comme seul un fluide le fait
  (`fluid: true`). Le cinquième, `kelp_plant`, est le corps d'une colonne de
  kelp et **ne peut jamais être le sommet d'une colonne** : l'oracle ne l'a pas
  vu, et c'est pourquoi il manquait au bit mesuré. Le test du registre vérifie
  que le bit mesuré n'est **jamais** en désaccord avec la règle, sur les
  24 135 états.

Couverture du test (`test_block_states.cpp`, « one fluid answer for every
state ») : les 1003 blocs et tous leurs états — 32 états de fluide (16 d'eau,
16 de lave, au bon niveau), tous les états des cinq blocs implicites, chaque
`waterlogged=true` (source d'eau) et `waterlogged=false` (rien). Vérifiés un par
un : cornichon de mer, éventails et coraux vivants et morts, conduit (tous
`waterlogged`), blocs de corail (jamais), nénuphar (jamais : il est **sur**
l'eau, pas dedans).

## 3. Côté serveur

* `FluidRules::fluid_at` lit la requête : une herbe marine est une **source**
  pour ses voisines (un losange de rayon 7 autour d'une herbe marine seule sur
  un sol), le fluide qui coule ne **rentre pas** dans le kelp (il est déjà de
  l'eau), et `state_after_break` laisse de l'eau à la place d'une herbe marine,
  d'un kelp ou d'une colonne de bulles (le wiki, Waterlogging).
* Seau vide sur une herbe marine ou du kelp : **rien** (le wiki, Kelp). Seau
  d'eau sur une herbe marine : rien. **La colonne de bulles n'est pas
  documentée** et n'a pas été mesurée : même règle faute de mieux — laissé à
  l'agent des objets.
* Noyade, yeux sous l'eau, feu et flèches : la même requête.
* Les 3172/3172 positions de `fluides.md` restent identiques (`test_fluid`).

## 4. Côté client : le maillage

* Chaque case qui **tient** de l'eau sans être le bloc d'eau dessine d'abord la
  **source d'eau** (le même modèle, la même teinte de biome, la même couche
  translucide que la mer), puis son propre modèle. Le modèle de la colonne de
  bulles n'a pas de géométrie : sans ce correctif, la colonne entière était
  **vide**.
* `fluid_at` renvoie le **type** de fluide, plus l'id du bloc : l'eau, un
  escalier engorgé et une herbe marine répondent tous « eau », donc la mer ne
  dessine aucune face contre la plante, et la surface au-dessus d'un kelp qui
  l'atteint est la surface d'une source (14/16, comme le reste du mailleur de
  fluides).
* Non fait : un bloc engorgé qui couvre lui-même une face de sa case (feuilles,
  dalle du haut) ne cache pas la face d'eau correspondante.

## 5. Le nénuphar

* Son modèle (`block/lily_pad`) déclare un `tintindex` sur ses deux faces et
  sa texture est **grise** (Faithful 32x : 5 gris de 92 à 163, alpha 0 ou 255
  seulement). Nous ne lui donnions **aucune teinte** : un nénuphar gris.
* Minecraft Wiki, « Lily Pad » : « Otherwise, lily pads have the color:
  #208030 » (dans le monde, quel que soit le biome) ; « In the inventory, lily
  pads have the color: #71c35c » (l'objet — laissé à l'agent des objets).
  Canal `TintChannel::LilyPad` = `0x208030`.
* La texture n'a que de l'alpha 0 et 255 : couche **cutout**, pas translucide.
  La rotation par position vient du blockstate (quatre variantes) et du tirage
  par position déjà mesuré (`rendu-parite.md`). La hauteur est celle du modèle
  (0,25/16 au-dessus du bas de la case, soit 1/64 au-dessus du bord de l'eau).

## 6. Parité de rendu

Deux scènes nouvelles dans `scripts/render_parity_scenes.txt` :

* `aquatic` — un bassin de pierre sur la plateforme des plaines : herbes
  marines dans la case de surface (sur un gradin), grande herbe marine, deux
  colonnes de kelp, une colonne de bulles sur du sable des âmes, éventail de
  corail, corail et cornichons de mer engorgés, conduit et escalier engorgés,
  éventail mort et cornichons au sec sur le bord, cinq nénuphars ;
* `lilypads` — un bassin au-dessus de l'océan tiède du spawn : un autre biome,
  une autre couleur d'eau sous la même couleur de nénuphar.

Les deux clients rejoignent le même serveur (`measure_render_parity.py`) ; les
commandes d'une scène éloignée suivent la scène précédente, pour que notre
client les envoie en se tenant à côté.

RESULTS_PLACEHOLDER

## 7. Ce qui n'est pas fait

* Teintes des tiges (âge) et de la poudre de redstone (puissance) : aucune
  valeur documentée trouvée (wiki « Melon Seeds », « Redstone Dust ») ; à
  mesurer sur le vrai client.
* Seau et colonne de bulles : non documenté, non mesuré.
* Auto-occultation de l'eau par le bloc engorgé qui la tient (§ 4).
