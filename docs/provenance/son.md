# Son — ce que le serveur fait entendre, à qui, et ce que le client joue lui-même

Avant cette vague, le client Ondes VOXEL était muet et le serveur n'envoyait
aucun paquet sonore : un client vanilla connecté à notre serveur n'entendait ni
les pas d'un autre joueur, ni une porte, ni un mob. Ce dossier établit trois
choses, chacune par mesure contre le vrai serveur 1.20.1
(`tools/vanilla/server.jar`, SHA-1 84194a2f…) plutôt que de mémoire :

1. **les paquets** — leur identifiant et leurs champs, reconnus par leur
   contenu ;
2. **qui entend quoi** — la question qui décide tout, parce que vanilla laisse
   de côté exactement le joueur dont le client a déjà joué le son ;
3. **le son de chaque bloc et de chaque créature** — qu'aucun rapport du data
   generator ne contient.

---

## 1. Les paquets, identifiés par leur contenu

`scripts/capture_sound_packets.py` connecte deux sondes au vrai serveur :
l'**acteur** fait un geste, l'**oreille** se tient à quatre blocs et ne fait
rien. Les deux notent chaque paquet reçu. Les gestes `layout.*` choisissent
chaque champ depuis la console — `/playsound` avec une position, un volume, un
pitch et une catégorie connus, un nom non enregistré, et les quatre formes de
`/stopsound` — si bien que chaque champ est reconnu à sa valeur.

| Paquet | Id mesuré | Ce que la capture a établi |
|---|---|---|
| Sound Effect | **0x62** | son = id du registre `sound_event` **+ 1**, ou 0 suivi d'un identifiant et d'un booléen de portée fixe ; catégorie ; x, y, z en **huitièmes de bloc** (i32) ; volume, pitch (f32) ; graine (i64) |
| Stop Sound | **0x63** | un octet de drapeaux (1 : source, 2 : son), puis la source (index de catégorie), puis le nom |
| World Event | **0x25** | déjà connu ; 2001 porte l'état du bloc cassé |
| Entity Sound Effect | 0x61 (archive) | **jamais observé** sur les 42 gestes capturés ; le codec suit l'archive et c'est dit |

Trois détails que seule la capture tranche :

- **`/playsound` envoie toujours par nom**, même un son enregistré
  (`minecraft:entity.cow.ambient` arrive en identifiant en ligne) ; le jeu, lui,
  envoie les sons enregistrés par id.
- **Les coordonnées sont tronquées vers zéro.** Un marcheur à x = −0,7736 est
  envoyé à −6/8 et un autre à 0,9528 à 7/8 : `floor` donnerait −7, l'arrondi 8.
  Le test `sound coordinates truncate toward zero` le fige.
- **Les catégories suivent l'ordre documenté** : bloc 4, hostile 5, neutre 6,
  joueur 7, météo 3 relevés dans la capture.

Les octets capturés sont figés dans `src/ov_protocol/tests/test_sound_packets.cpp`
et ré-encodés à l'identique.

## 2. Qui entend quoi

Relevé geste par geste (capture du 2026-09-10, deux passes — la première a
coûté quatre gestes à la sonde, voir § 7) :

| Geste | Paquet | L'acteur l'entend ? | Détail |
|---|---|---|---|
| poser un bloc | Sound Effect `…place` | **non** | centre du bloc, catégorie bloc, volume (v+1)/2, pitch p×0,8 |
| casser un bloc | World Event 2001 + état | **non** | le client joue le son lui-même ; en survie l'oreille reçoit aussi 11 × 0x07 (progression) |
| porte, trappe, portillon ouverts à la main | Sound Effect | **non** | pitch aléatoire (0,91–0,98 relevés) |
| bouton pressé | `…click_on` | **non** | pitch 1,0 |
| bouton relâché (tick) | `…click_off` | **oui** | le serveur l'a fait, pas un client |
| levier | `block.lever.click` | **oui** | volume 0,3, pitch 0,6 (on) / 0,5 (off) |
| portillon ouvert par le levier (redstone) | `block.fence_gate.open` | **oui** | |
| coffre ouvert / fermé | `block.chest.open/close` | **oui** | volume 0,5 ; le wiki donne 0,9–1,0 de pitch |
| seau d'eau / de lave vidé | `item.bucket.empty(_lava)` | **non** | catégorie bloc, au centre du bloc |
| seau rempli | `item.bucket.fill(_lava)` | **non** | catégorie **joueur**, à la position du joueur |
| TNT allumée | `entity.tnt.primed` | **oui** | à la position de l'entité, pas du bloc |
| explosion | *rien* | — | le client la joue depuis le paquet Explosion (0x1D) |
| ramasser un objet, un orbe | *rien* | — | le client joue le son depuis Take Item Entity (0x67) |
| monter aux niveaux 1, 2 | *rien* | — | |
| monter au niveau 5 | `entity.player.levelup` | **oui** | volume 0,125 = 5/30 × 0,75 |
| frapper une vache | `entity.cow.hurt` + `entity.player.attack.strong` | **oui** | vache : neutre, 0,4 |
| /kill | `…death` seul | **oui** | pas de son de coup avant |
| marcher | `block.<famille>.step` | **non** | catégorie joueur, aux pieds, volume v×0,15 |
| chute de 5 blocs | `entity.player.small_fall`, `block.grass.fall`, `entity.player.hurt` | **non** | dans cet ordre ; bloc : v×0,5, p×0,75 |
| zombie et vache laissés seuls | `…ambient` | **oui** | zombie hostile 1,0, vache neutre 0,4 |
| manger (pomme d'or, 32 ticks) | `entity.generic.eat` × 7 pendant | **non** | catégorie joueur, volume 1,0 ou 0,5, pitch 0,91–1,13 ; sept en 32 ticks, soit un tous les 4 sur les 25 derniers |
| finir de manger | `entity.player.burp` puis `entity.generic.eat` | **oui** | rot : joueur, 0,5, 0,98 ; la dernière bouchée en catégorie **neutre** |

La chute de huit blocs n'a rien donné à l'oreille, et ce n'est pas une mesure :
l'acteur tombait à 17,4 blocs d'elle, hors de portée. Le seuil entre
`small_fall` et `big_fall` n'est donc **pas mesuré** ; notre serveur prend
« plus de 4 points », et c'est le nôtre.

Le rayon : `/playsound` au volume 0,7 depuis 20 blocs n'est jamais arrivé à
l'oreille, le même au volume 2 si — un Sound Effect porte à 16 blocs, multipliés
par le volume au-delà de 1, ce que la page `/playsound` du wiki dit aussi.

## 3. Le son de chaque bloc

Le jeu de sons d'un bloc — casser, marcher, poser, frapper, tomber, avec un
volume et un pitch communs — n'est dans aucun rapport du data generator. Trois
des cinq voyagent sur le fil ; `scripts/measure_block_sounds.py` les relève sur
les **1003** blocs de 1.20.1, en trois phases sur un même serveur :

- **poser** : l'acteur (créatif) tient l'objet du bloc et clique le sol ;
  l'oreille note le Sound Effect **et** le Block Update, si bien qu'une pose est
  vérifiée par l'état qui est arrivé, pas supposée ;
- **marcher** : une allée de cases de cinq blocs posée par `fill`, l'acteur
  téléporté sur chaque case et marchant 3,4 blocs en son milieu ;
- **tomber** : une chute de cinq blocs en survie sur chaque case, sous
  Régénération, à plus de 0,6 s d'intervalle (les dix ticks d'invulnérabilité
  avaleraient sinon une chute sur deux).

Casser et frapper ne voyagent jamais — le client les joue lui-même — et sont
**déduits** de la famille que nomment les trois autres, quand l'événement
existe au registre. Le pack porte, bloc par bloc, lesquels ont été entendus et
lesquels déduits.

| | Blocs |
|---|---|
| pose entendue, vérifiée par le Block Update | **819** / 1003 |
| pas retenu | **600** |
| chute retenue | **704** |
| cohérents (un seul volume, un seul pitch, une seule famille) | **900** |
| dont les cinq événements nommés | **896** |
| désaccords de volume ou de pitch entre gestes | **0** |
| pas ou chute écartés (entendus sur un autre bloc) | **225** |
| rien de mesurable | **102** |

Les 184 blocs que la pose n'atteint pas : **147 n'ont pas d'objet à leur nom**
(variantes murales, cultures, blocs techniques), **36 ne tiennent pas** sur de
l'herbe en plein air (enseignes suspendues, spore blossom, racines et lianes
pendantes veulent un plafond ; échelle, liane et crochet un mur ; varech,
algue, nénuphar et frai de l'eau ; cactus, canne à sucre, champignons, verrue
du Nether, chorus et petite dripleaf un sol ou une lumière précis ; les blocs de
commande, de structure et jigsaw sont refusés à un non-opérateur ; `wheat` et
`air` ne se posent pas), et **1 arrive sous une autre forme** (le bambou devient
une pousse de bambou, qui a son propre son).

Trois jeux de volume et de pitch seulement : **1,0 / 1,0** pour 879 blocs,
**1,0 / 1,5** pour 18 (le métal : blocs de fer, d'or, de diamant, d'émeraude et
de redstone, rails, entonnoir, barreaux, porte et trappe de fer, plaques
pondérées, spawner, œufs), **0,3 / 1,0** pour 4 (les trois enclumes et la
cloche). 86 familles de pose. Et une curiosité **mesurée, pas corrigée** : le
grand bourgeon d'améthyste se pose avec le son du moyen, et le moyen avec celui
du grand — c'est ce que fait le vrai serveur.

### Trois façons de nommer le mauvais bloc

La table refuse ce qu'une allée ne peut pas attribuer (§ 7 pour le détail) :

1. **La boîte du joueur déborde sur la case voisine** : dans la première allée
   (cases de trois, marche de 0,2 à 2,8), 101 blocs ont reçu le pas de leur
   voisin. Cases de cinq, marche au milieu.
2. **Au-dessus d'un bloc qu'on traverse, on marche sur le bloc d'en dessous** :
   pas et chute ne sont retenus que pour les blocs qui arrêtent le mouvement
   (`normalized/motion.json`) — 225 blocs dont un pas ou une chute a été écarté
   ainsi.
3. **Quand les familles divergent, la pose l'emporte**, seul geste confirmé par
   un Block Update : un bloc de corail posé à sec meurt sur place et se marche
   comme le corail mort qu'il est devenu (pierre) ; un portillon se marche comme
   la terre qu'il surplombe. Ces sons sont vrais — d'autre chose — et sont mis
   de côté, nommés. Sans pose pour arbitrer, rien n'est gardé : la bannière
   murale orange a marché « gravier » et chuté « bois », et reste sans son.

### Ce qui s'ouvre et se ferme

`scripts/measure_sound_events.py toggle` : les **64** portes, trappes,
portillons, boutons, leviers et plaques de 1.20.1, cinq cycles chacun, à la
main (la sonde clique), au pied (la sonde se tient sur la plaque) ou à la
redstone (un bloc de redstone posé à côté, pour le fer, qui ne s'ouvre pas à la
main). **64/64** donnent un seul son d'ouverture ; 63/64 un seul son de
fermeture — la plaque en bambou est l'exception, nommée et laissée muette.

| Comment | Ouvrir | Fermer | Blocs |
|---|---|---|---|
| à la main | les autres | les autres | 33 (portes, trappes, portillons) |
| à la main | les autres | tout le monde | 13 (boutons : le relâchement est un tick) |
| à la main | tout le monde | tout le monde | 1 (le levier) |
| sur la plaque | tout le monde | tout le monde | 15 |
| par la redstone | tout le monde | tout le monde | 2 (porte et trappe de fer) |

Volumes : 1,0 partout sauf le levier (0,3). Pitch : boutons et plaques 1,0 ;
levier 0,6 puis 0,5 ; portes, trappes et portillons aléatoires, 0,90–1,00 sur
les cinq cycles, ce que les tables du wiki donnent aussi. Vingt-six familles
d'événements : chaque bois a les siennes en 1.20 (`cherry_wood_door`,
`bamboo_wood_button`, `nether_wood_trapdoor`…), le fer et la pierre aussi — une
table par bloc et non une règle par forme.

## 4. Le son des créatures

`scripts/measure_sound_events.py mobs` : chacun des **79** types vivants de
`entities.json` (joueur et support d'armure exclus), invoqué adulte, frappé
trois fois par `/damage` à 0,7 s d'écart (au-delà des dix ticks
d'invulnérabilité), puis tué par `/kill`.

- **78/79** donnent un son de coup unique. L'exception est l'ender dragon, dont
  on n'entend que l'ambiance.
- **70/79** donnent un son de mort. Les neuf autres, nommés : cod, salmon,
  pufferfish, tropical_fish, ender_dragon, enderman, slime, magma_cube, rabbit.
  Ils restent muets à la mort chez nous plutôt que d'emprunter un son.
- Catégorie : 44 neutres, 35 hostiles. Volume du coup : 1,0 pour 58 types, 0,4
  pour 9 (la vache en est), 0,8 pour 8, et trois isolés (0,1, 4,0, 5,0).
- Pitch : tous les coups de tous les types tombent dans **0,811–1,184** — ce
  qui justifie le 0,8–1,2 retenu pour le joueur blessé, dont la capture n'a
  que deux valeurs.
- Le son d'ambiance est **nommé par le registre** (`entity.<type>.ambient`),
  pas entendu : le creeper n'en a pas, et c'est le registre qui le dit. Notre
  serveur n'a pas de minuterie d'ambiance ; ce son n'est donc jamais émis
  (§ 6).

⚠ **Un son d'ambiance peut tomber dans la fenêtre d'un coup.** La première
réduction a pris l'ambiance du piglin brute pour son coup. Les fenêtres ne
gardent plus que les événements dont le dernier segment commence par `hurt`
(`hurt`, `hurt_land`) ou `death`.

## 4 bis. Parité : le vrai serveur et le nôtre, geste par geste

`scripts/check_sounds_e2e.py` rejoue les 42 gestes de la capture contre
`ov_dedicated`, avec les mêmes sondes, et compare ce que chacune reçoit : quels
événements, à qui (l'acteur, l'oreille, les deux), catégorie et volume exacts,
pitch égal quand celui de vanilla est fixe et **dans sa plage** quand il est
aléatoire (plage relevée dans les tables mesurées, ou documentée par le wiki
quand une capture n'a qu'un échantillon), position au huitième de bloc.

**24 gestes identiques sur 42, 6 écarts, 12 non applicables** (`/playsound` et
`/stopsound`, que notre serveur n'a pas ; les seaux, qu'il n'a pas ; la
minuterie d'ambiance, qu'il n'a pas). Avant les deux correctifs de cette
comparaison même — le portillon (sans effet, voir plus bas) et la septième
bouchée — le décompte était de 23 / 7 / 12. Identiques : poser, casser en
créatif et en survie, porte, trappe, portillon ouverts et fermés, boutons de
pierre et de bois (pression et relâchement), trappe de fer (rien des deux
côtés), coffre ouvert et fermé, TNT allumée, explosion (rien des deux côtés),
ramassages et niveaux 1 et 2 (rien des deux côtés), manger, chute de cinq
blocs.

Les écarts qui restent, un par un :

- **Levier → portillon.** Vanilla ouvre le portillon voisin du levier et le
  fait entendre à tous. Chez nous le Block Update du portillon arrive, mais
  l'état décodé est `open=false, powered=true` : **notre redstone alimente le
  portillon sans l'ouvrir**. La couche son a raison de se taire — un changement
  de `powered` seul n'est pas une bascule — et l'écart est à la redstone, hors
  de ce mandat. Le crochet qui fait entendre ce qu'un clic déclenche ailleurs
  est en place et testé : il jouera dès que la redstone ouvrira.
- **Vache (coup, mort) et niveau 5.** Notre `/summon` n'a fait apparaître ni la
  vache ni l'orbe pour les sondes (aucun Spawn Entity reçu), alors que la
  commande accepte bien un NBT : ces gestes n'ont **pas été exercés** chez nous.
  C'est une limite du banc, nommée ; les règles elles-mêmes sont couvertes par
  `test_sounds.cpp` (`mob_hurt`, `level_up` : 0,125 au niveau 5).
- **Marcher.** Vanilla a donné 4 pas dans cette capture ; nous 3, aux positions
  −0,75, 0,875 et 2,625 — **exactement** celles de la première capture vanilla du
  même geste. La phase des pas dépend de tout ce que le joueur a parcouru avant
  dans la session (le compteur de vanilla ne repart pas de zéro à une
  téléportation, et les deux sessions n'avaient pas le même passé). L'espacement
  est le même ; le compte d'une marche de 6,5 blocs peut différer d'un.
- **Manger** : la première comparaison a trouvé six bouchées chez nous contre
  sept — la septième tombe sur le tick même où l'usage finit, et le code ne la
  jouait que tant que l'usage durait. Corrigé. Les volumes (0,5 ou 1,0) et le
  pitch du rot sont aléatoires : ils sont comparés comme des ensembles et des
  plages, pas valeur par valeur.

## 5. Le client

### Bibliothèques, et leurs licences

| Bibliothèque | Version | Licence | Rôle |
|---|---|---|---|
| miniaudio | 0.11.25 (port vcpkg) | Unlicense **ou** MIT-0, au choix | ouvrir le périphérique de sortie |
| stb_vorbis | v1.22 (port vcpkg `stb`) | MIT **ou** domaine public, au choix | décoder les `.ogg` |

Toutes deux sous la feature `client` de `vcpkg.json` : le serveur headless n'en
dépend pas. Chacune est compilée une seule fois, dans une unité de traduction
qui ne contient rien d'autre, avec les avertissements coupés (code tiers) ;
aucun en-tête public ne les nomme — `SoundEngine` est derrière un PIMPL. Aucune
des deux licences n'impose de mention dans `NOTICE` (MIT-0 et Unlicense n'en
demandent pas ; stb est pris sous l'alternative domaine public).

### Ce que le moteur fait, et d'où vient chaque règle

- **`sounds.json`** (format documenté par la page *Sounds.json* du wiki) :
  événement → variantes pondérées, `volume`, `pitch`, `weight`, `stream`,
  `attenuation_distance`, `preload`, `type`. 1471 événements, 6100 entrées dans
  le fichier 1.20.1 ; 43 entrées de type `event`, toutes sans poids propre.
  Deux choses non documentées sont **choisies et nommées** : une référence
  garde son propre poids et multiplie volume et pitch de la cible (les 36
  imitations du perroquet portent 0,2–0,8 et 1,6–2,0) ; la variante est tirée
  par `LegacyRandomSource(graine).next_int(total)` — la graine du paquet est
  documentée comme « utilisée pour choisir la variante », le générateur ne
  l'est pas, et ce choix n'est pas vérifié contre le client vanilla.
- **Atténuation** : linéaire, silence à `attenuation_distance` × max(1, volume),
  gain min(1, volume) — la page `/playsound` du wiki.
- **Pitch** : borné à 0,5–2 (même page : « en dessous de 0,5 équivaut à 0,5 »).
- **Panoramique** : puissance constante sur la composante latérale de la
  direction ; un son stéréo (musique) n'est pas positionné. Choix du projet.
- **Voix** : 64 sons courts et 4 flux, **nos** bornes — celles du client
  vanilla ne sont pas documentées. Un son sans voix libre est abandonné et
  compté, jamais substitué à un autre.
- **Threads** : le mixeur tourne sur le thread temps réel de CoreAudio (que
  miniaudio ne crée pas, et dont la priorité n'est pas la nôtre à choisir) ; le
  décodage sur un thread `ThreadRole::Io` ; entre les deux, un anneau de
  commandes SPSC de 256 entrées et des atomiques. Aucun verrou ni allocation
  côté audio.
- **Musique** : `music.game`, ou `music.creative` en créatif, silences tirés
  uniformément dans 12 000–24 000 ticks — les bornes de chaque biome qui nomme
  sa musique dans le data generator ; que le défaut les partage est une
  inférence. La musique par biome et par dimension n'est **pas** faite : elle
  attend que `ov_netclient` décode les effets de biome du Registry Data.

### Ce que le client joue lui-même

Tirés de la capture (« l'acteur ne l'entend pas ») et, pour les deux gestes
qu'aucun paquet ne porte, des tables de son du wiki :

| Geste | Règle | Source |
|---|---|---|
| pas | v×0,15, p, catégorie joueur, un pas par 1/0,6 bloc | capture (5 espacements, moyenne 1,65) |
| poser | (v+1)/2, p×0,8, **quand le Block Update revient** | capture ; notre client ne prédit pas |
| casser | (v+1)/2, p×0,8 | wiki, table de la pierre (1,0 / 0,8) |
| coup de pioche | (v+1)/8, p×0,5, un tick sur quatre | wiki (0,25 / 0,5) — **non branché** : notre client casse instantanément |
| atterrissage | small/big_fall, chute du bloc, hurt | capture |
| porte ouverte par ce joueur | événement mesuré, **quand le Block Update revient** | capture (audience « les autres ») |
| explosion | `entity.generic.explode`, bloc, 4,0, 0,56–0,84 | wiki, page Explosion |
| ramasser un objet | `entity.item.pickup`, joueur, 0,2, 1,6–3,4 | wiki, page Item |
| ramasser un orbe | `entity.experience_orb.pickup`, joueur, 0,1, 0,55–1,25 | wiki, page Experience orb |

### De bout en bout, et ce que ça coûte

`scripts/check_client_sounds_e2e.py` lance `ov_dedicated` sur un monde à lui et
le vrai client `ov_voxel` contre lui, trois fois : un joueur qui marche (avec
son), le même sans son, un joueur qui creuse et pose sur place. Ce qui est
exigé vient du journal du moteur lui-même (`--sound-log`), sur le périphérique
réel de la machine :

- **35 pas** `block.grass.step` pour la marche (21 à une passe précédente, la
  marche n'étant pas bornée au même point) ;
- **le cassage** `block.grass.break`, joué par le client de celui qui casse ;
- **la pose** `block.stone.place`, jouée quand le Block Update revient — **2,6 s**
  après l'envoi sur ce serveur Debug, ce qui explique les passes où l'attente
  bornée à 1,65 s la manquait (§ 7, piège 11). La pose scriptée du client est
  parfois refusée par le serveur ; la vérification n'exige alors pas son son et
  le dit, plutôt que de tester la chance du script.

Coût, mesuré par frame sur le seul travail audio du thread de rendu (les
événements du serveur, l'écouteur, la mise à jour du moteur) : **p50 0,001 à
0,003 ms, p99 0,006 à 0,007 ms** sur cinq passes. Le temps CPU de la frame
entière, avec et sans son, ne se départage pas : ses écarts d'une passe à
l'autre (p50 4,0 à 9,1 ms, p99 11 à 22 ms, et dans les deux sens) sont mille
fois plus grands que ce que l'audio prend. Le mixage lui-même tourne sur le
thread de CoreAudio et n'est pas dans ce chiffre ; il ne prend ni verrou ni
allocation.

## 6. Ce qui n'est pas fait, nommé

Côté serveur :

- **Les seaux** n'existent pas dans notre serveur (`ItemUse` les refuse par
  leur nom) ; leurs quatre sons, mesurés en § 2, attendent le seau.
- **Pas de minuterie d'ambiance** pour les créatures : leur son d'ambiance est
  dans la table, jamais émis.
- **Les variantes d'attaque** : `entity.player.attack.strong` est la seule
  capturée (un coup de poing chargé, entendu par tous, 1,0 / 1,0). Les autres
  sont émises d'après les descriptions du wiki — crit si coup critique, sinon
  sweep, sinon knockback en sprint, sinon strong au-dessus de 0,9 de charge,
  sinon weak — mais **cette préséance et ce seuil sont les nôtres**, ni
  documentés ni mesurés. `nodamage` n'est pas émis.
- **Manger** : les bouchées (pour les autres), le rot et la dernière bouchée
  en catégorie neutre (pour tous) sont émis comme capturés. Le **compte** — sept
  en 32 ticks — correspond ; **les ticks où elles tombent** (restant ≤ 25 et
  ≡ 1 modulo 4) sont notre choix, une seule capture ne les fixant pas.
  **Boire** (potion, lait, miel) reste muet : aucune capture ne l'a entendu.
- **Les pas accroupi ou en spectateur** sont silencieux chez nous : non
  mesurés.
- **Les murs et les clôtures** n'ont pas de pas mesuré : debout à y = −60 la
  boîte du joueur est dans le mur de 1,5 bloc, et le jeu ne marche pas.
- **Tonneau, coffre de l'End, boîte de Shulker** : leurs sons d'ouverture sont
  distincts de celui du coffre et aucune capture ne les a entendus ; ils restent
  muets plutôt que d'emprunter celui du coffre.
- **World Event** : seul 2001 est émis ; les autres (distributeur, portail,
  enclume…) attendent leur mécanique.
- La portée d'un World Event (64 blocs) est **la nôtre**, non mesurée.

Côté client :

- **Pas de coup de pioche** : notre client casse instantanément, il n'y a pas de
  progression où le jouer.
- **Pas de son d'interface.** Notre client n'a pas encore de bouton ; ouvrir
  l'inventaire du joueur n'envoie aucun paquet, donc un éventuel son local de
  vanilla n'est vérifiable qu'avec le client vanilla, que ce banc n'a pas.
- **Pas de musique par biome ni par dimension** (§ 5).
- **Entity Sound Effect** n'a jamais été observé ; son décodage suit
  l'archive.
- Le **tirage de la variante** par la graine n'est pas vérifié contre le client
  vanilla — le seul oracle qui le trancherait.
- Les **bornes de voix** (64 + 4) sont les nôtres.

## 7. Pièges payés

1. **Un port pris ne fait pas échouer vite.** La première capture a perdu son
   port au profit du serveur d'un autre agent : vanilla écrit « FAILED TO BIND
   TO PORT », plante en s'arrêtant, et le harnais attend trois minutes un
   « Done » qui ne vient jamais. Les scripts vérifient le port, et **attendent**
   un port qui se libère : la phase suivante d'une campagne l'a trouvé encore
   tenu par la JVM précédente une seconde plus tôt.
2. **Un seau n'agit pas sur Use Item On.** Il agit sur Use Item, lancé selon
   le regard. La première capture a cliqué le sol avec un seau plein et le
   serveur n'a rien fait — sans un mot.
3. **La portée compte.** La TNT posée à 11,5 blocs n'a jamais été allumée : un
   refus de portée, pas une règle de son.
4. **À midi, un zombie brûle.** Les vingt « sons ambiants » de la première
   capture étaient vingt sons de dégâts et une mort.
5. **Un serveur en survie ignore Set Creative Slot** : la sonde a « mangé » une
   pomme d'or avec la main vide.
6. **Le bloc d'un pas n'est pas celui sous les pieds.** La première marche
   passait de 0,2 à 2,8 dans des cases de trois blocs : à 2,8 la boîte de 0,6
   du joueur mord déjà sur la case voisine, et le jeu prend le son d'un bloc sur
   lequel la boîte repose. 101 blocs sont sortis avec le pas de leur voisin
   (l'allium avec celui du bloc d'améthyste). Cases de cinq, marche au milieu.
7. **Au-dessus d'un bloc qu'on traverse, on entend le bloc d'en dessous.** Un
   bouton posé sur de la terre a « marché » `block.gravel.step` — le son de la
   terre. Le pas et la chute ne sont retenus que pour les blocs qui arrêtent le
   mouvement (`normalized/motion.json`, mesuré bloc par bloc) ; pour les autres
   ils sont écartés, nommés, et jamais fondus dans la ligne du bloc.
8. **Un tirage Java aux graines consécutives est presque constant.** Le test de
   pondération tirait les graines 0 à 3999 et a choisi la même variante 4000
   fois. Les graines du serveur sont des i64 aléatoires ; le test aussi,
   désormais.
9. **Un nom de cible CMake qui commence par `ov_` est pris pour un module**
   par `ov_check_deps`, et refusé comme inconnu.
10. **Trois clients, un seul monde : l'ordre compte.** Le client qui creuse
    passait en premier et laissait un trou au point d'apparition ; le client
    qui marchait apparaissait dedans et marchait 1 500 frames contre sa paroi —
    « aucun pas ». Celui qui creuse passe désormais en dernier.
11. **Attendre la réponse du serveur, pas une horloge.** Le son de pose de ce
    joueur se joue quand le Block Update revient. Une fenêtre de 60, puis de
    240 frames (1,65 s à 145 images/s) se refermait avant qu'un serveur Debug
    occupé à générer ait répondu, et la pose était entendue comme rien. C'est
    la première réponse sur la case qui ferme l'attente ; la limite de frames
    ne reste que comme filet.
12. **Minuter le son, pas la frame.** Le premier chrono allait de l'arrivée des
    paquets à la mise à jour du moteur et mesurait au passage le maillage et la
    physique : 4,8 ms « d'audio ». Il ne couvre plus que les deux segments
    audio.
