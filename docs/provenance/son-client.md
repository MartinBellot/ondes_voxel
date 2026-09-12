# Son côté client — l'espace, la priorité, la musique, les disques, les sous-titres

`son.md` a établi ce que le serveur fait entendre et à qui, et un premier
client qui le joue. Ce dossier couvre la suite, côté client seulement : où un
son se place dans la tête, ce qui se passe quand trop de sons jouent à la fois,
quelle musique joue dans quelle situation, les disques, les sous-titres. Pour
chaque règle, la source est dite ; ce qui n'est écrit nulle part est **nommé
comme notre choix**, pas déguisé en parité.

## 1. Sources

| Source | Ce qu'elle donne |
|---|---|
| wiki, page *Music* | les situations (menu, créatif, jeu, sous l'eau, Nether par biome, End, dragon, crédits), « 1 to 30 seconds » pour le menu, « 10 to 20 minutes » pour le reste, « instantly » pour le dragon et les crédits, le fondu sous un disque |
| wiki, page *Subtitles* | la boîte noire « above the bottom right corner », les flèches `<` `>`, le texte qui « fades, becoming less white », l'option *Show Subtitles* |
| wiki, page *Jukebox* | « heard from up to 64 blocks away » |
| wiki, pages */playsound* et *Sounds.json* | déjà tracées dans `son.md` : atténuation linéaire, 16 × volume au-delà de 1, pitch borné à 0,5–2 |
| archive protocole 763 | World Event (1010, 1011), Boss Bar et son drapeau 0x02 « play boss music », Login (play) et Respawn |
| data generator 1.20.1 | le champ `music` des biomes (31 biomes), le tag `plays_underwater_music` |
| `options.txt` écrit par le vrai client | `showSubtitles:false`, `notificationDisplayTime:1.0`, les dix `soundCategory_*` (`src/ov_client/tests/data/options_vanilla_1.20.1.txt`) |

## 2. Le disque, mesuré sur le vrai serveur

`scripts/measure_jukebox_events.py` : l'acteur met un disque dans un juke-box,
l'éjecte, en met un autre dans un second juke-box que la console casse.
L'oreille est à quatre blocs.

| Geste | Acteur | Oreille |
|---|---|---|
| mettre `music_disc_cat` | World Event **1010**, données **1123** | idem |
| éjecter | World Event **1011**, données 0 | idem |
| mettre `music_disc_stal` | 1010, données **1129** | idem |
| casser le juke-box | 1011, données 0 | idem |

Trois choses que seule la mesure tranche : les données de 1010 sont **l'id
d'objet du disque** (1123 = `minecraft:music_disc_cat` dans le registre
`minecraft:item`), l'arrêt existe bien en 1.20.1 sous le numéro 1011, et
**l'acteur les reçoit aussi** — le client ne prédit rien, il joue ce qui
arrive. Aucun Sound Effect ne porte le disque.

Le client joue alors `minecraft:music_disc.<nom>` au centre du bloc, catégorie
disque, **volume 4** : avec l'atténuation de `son.md`, 4 × 16 = 64 blocs, la
portée que donne le wiki. Le disque est en `stream: true` dans `sounds.json`.
La barre d'action affiche `record.nowPlaying` avec la description
`item.minecraft.music_disc_<nom>.desc` (« Now Playing: C418 - cat »), par le
même chemin qu'un Set Action Bar Text du serveur.

## 3. La tête

Le panoramique est la composante de la direction du son sur l'axe droit de la
tête, `regard × haut`, avec le regard et le haut tirés du lacet **et** du
tangage. Le monde de Minecraft est direct (+X est, +Y haut, +Z sud), donc
`regard × haut` est la main droite : face au sud, l'ouest, −X.

Sans roulis, cet axe reste horizontal quel que soit le tangage : **hocher la
tête ne change pas les oreilles**. C'est prouvé par le test plutôt que supposé
(`pan: the right hand from look x up, whatever the pitch`) : trois sources, cinq
tangages, écart < 10⁻⁵. Un son au-dessus ou en dessous est centré à tout lacet.

Un premier jet avait le vecteur haut faux en signe sur x et z — non orthogonal
au regard dès que le tangage n'est pas nul. Les trois tests de position qui
combinent tangage et lacet l'ont vu au premier passage.

**Modèle numérique contre mixeur** (`spatialise: the formula, and the samples
the mixer writes`) : huit positions (aux oreilles, 6 blocs à droite et à
gauche, 12 devant, en hauteur, 20 devant au volume 2, 3 à droite au volume 0,3,
45° à droite tête baissée de 70°). Pour chacune, le gain et le panoramique
sont recalculés dans le test depuis la formule documentée, puis comparés aux
échantillons que le mixeur écrit sur une onde plate : **8/8 à 10⁻⁴ près**. Le
modèle — gain linéaire, loi de puissance constante — est celui de `son.md` ;
aucun des deux n'est vérifié contre le vrai client, qui passe par OpenAL.

Un son relatif (interface, musique) n'est ni atténué ni panoramiqué : il sort
au centre, au même gain qu'un son positionné centré.

## 4. Trop de sons : une priorité

Les bornes de voix (64 courts + 4 flux) restent **les nôtres** (`son.md`). Ce
qui change est ce qui se passe quand elles sont pleines. Avant : le nouveau son
était jeté. Maintenant : **le son le plus faible aux oreilles cède sa voix** si
le nouveau serait plus fort, sinon c'est le nouveau qui est jeté. « Plus
faible » se lit sur ce qui atteint les oreilles, distance, catégorie et volume
maître compris : un pas enterré à quarante blocs cède à une explosion à côté,
un son hors de portée ou d'une catégorie muette ne prend jamais la voix d'un son
entendu, et deux sons égaux ne se délogent pas. Les flux (musique, disques) ont
leurs emplacements et ne sont jamais pris. Les deux issues sont comptées
(`stolen`, `dropped_no_voice`).

**Mesure** (`bench_ov_audio`, `ctest -L bench`, Debug, M2) : 200 sons par frame
sans répit, sur un anneau de 1 à 30 blocs, 200 frames ; un témoin à 0 son.

| Voix | Frame : play + update p50 / p99 | Mixeur, par rappel de 512 images p50 / p99 |
|---|---|---|
| 64, priorité | 0,30 / 1,77 ms | 1,29 / **4,93 ms** |
| 256 (aucun plafond effectif) | 0,40 / 2,67 ms | 8,04 / **16,01 ms** |
| témoin, 0 son | 0,0002 / 0,0003 ms | 0,004 / 0,006 ms |

Un rappel de 512 images, c'est 10,7 ms de son à 48 kHz : au-delà, le
périphérique manque d'échantillons et le son hache. **Sans plafond la rafale ne
tient pas** (16 ms au p99) ; avec le plafond et la priorité elle tient avec de la
marge (4,9 ms). Le test n'exige la borne que pour le plafond livré — c'est tout
son propos. Chiffres Debug : la version Release est plus rapide, non mesurée ici.

**De bout en bout** (`scripts/check_client_sounds_e2e.py`, notre serveur sur
le port 25615, notre client, périphérique réel, 1500 frames) : le marcheur
entend **18 pas** `block.grass.step`, le creuseur son `block.grass.break`, et la
musique de situation est **choisie et demandée** d'elle-même (`music.creative`,
variante `creative2.ogg` : le marcheur est en créatif) — puis refusée faute de
fichier, parce que la musique n'est pas importée par défaut (`--music`). C'est
le seul refus du passage, et il est voulu : ce banc vérifie le choix, pas
l'écoute d'une piste. Le travail audio de la frame — événements, écouteur, mise à jour du
moteur — vaut **p50 0,002 ms, p99 0,008 ms**, contre 0,001–0,003 et
0,006–0,007 ms avant cette vague (`son.md`) : inchangé à la mesure près. Le
temps CPU de la frame entière avec et sans son (p99 20,1 et 16,7 ms sur ce
passage) ne se départage toujours pas, pour la raison que `son.md` donne : ses
écarts d'une passe à l'autre sont mille fois l'audio. La pose scriptée a été
refusée par le serveur sur ce passage, ce que le script dit sans le compter.

## 5. Catégories et options

Les dix catégories sont lues et écrites dans `options.txt` sous les clés
`soundCategory_<nom>` du vrai client (écran Musique et sons, livré avec les
écrans). S'y ajoutent `showSubtitles` (le bouton de l'écran, désormais actif)
et `notificationDisplayTime`, relus et réécrits dans le format du vrai client.

Un **fondu** par catégorie, distinct du volume, sert au jeu et jamais au joueur :
`options.txt` ne le voit pas.

## 6. La musique

`audio::situational_music`, dans l'ordre de la page *Music* :

| Situation | Événement | Silence (ticks) | Remplace |
|---|---|---|---|
| crédits | `music.credits` | 0 | oui (« instantly ») |
| menu, sans monde | `music.menu` | 20–600 (« 1 to 30 seconds ») | oui |
| End, barre de boss avec 0x02 | `music.dragon` | 0 | oui (« instantly ») |
| End | `music.end` | 12 000–24 000 | non |
| sous l'eau, dans un biome océan ou rivière | `music.under_water` | 12 000–24 000 | non |
| créatif | `music.creative` | 12 000–24 000 | non |
| biome qui nomme sa musique | le champ `music` du biome | ceux du biome (tous 12 000–24 000) | ceux du biome |
| sinon | `music.game` | 12 000–24 000 | non |

D'où vient chaque entrée du client :

- **la dimension** : le `dimension name` de Login (play), après le codec, puis
  le deuxième champ de Respawn ;
- **la musique des biomes** : le codec de registres que porte Login (play), les
  entrées `minecraft:worldgen/biome` → `element.effects.music`. Relu depuis le
  paquet de notre serveur, le codec donne **31 biomes** — le compte du data
  generator. Lu sur le fil comme le fait vanilla, pas dans une copie locale ;
- **la barre du dragon** : Boss Bar (0x0B), ajout et mise à jour des drapeaux,
  bit 0x02 ; notre serveur l'envoie déjà (`end_fight.cpp`, 0x02 | 0x04) ;
- **sous l'eau** : les yeux dans l'eau, et le biome dans
  `#minecraft:plays_underwater_music` (`#is_ocean` + `#is_river`, onze biomes).
  Les tags voyagent dans Update Tags, que ce client ne décode pas encore : c'est
  la seule pièce lue dans une table du client, et elle est nommée.

Trois règles de transition, déduites de la page :

1. un choix qui **remplace** (menu, dragon, crédits) coupe la piste en cours et
   démarre sans attendre ;
2. une piste lancée par un tel choix **s'arrête avec sa situation** — la musique
   du menu qui « stops playing when the player enters the loading world
   screen » ; le dragon mort, sa musique aussi ;
3. un silence plus long que le maximum du nouveau choix est **ramené** à ce
   maximum — sans quoi, revenu au menu après une partie, la musique attendrait
   vingt minutes au lieu de trente secondes.

Deux choix nommés : le créatif passe **avant** la musique du biome (la page ne
dit pas qui gagne au Nether en créatif), et un événement sans piste (la forêt
distordue, `music.nether.warped_forest` a une liste vide) ou une musique non
importée attend **le silence le plus long** du choix avant de réessayer, pour ne
pas avertir chaque seconde au menu.

**Le fondu sous un disque.** « Background music fades out when a music disc
song can be heard, and fades in again when no music disc is playing » : quand
un disque joue à moins de 64 blocs, la catégorie musique descend à zéro, puis
remonte quand il s'arrête. La durée — **40 ticks, 2 s** — est la nôtre, la page
ne la donne pas. Il n'y a pas de fondu enchaîné entre deux pistes : la page
décrit des silences, pas des transitions.

## 7. Les sous-titres

`client::SubtitleOverlay` : une ligne par son entendu qui porte une clé
`subtitle` dans `sounds.json` (1285 événements sur 1471), traduite par la table
de langue. Documenté : la boîte noire en bas à droite, les flèches, le texte qui
pâlit. **Nôtre** : une ligne reste 3 s × `notificationDisplayTime`, un même son
réentendu rafraîchit sa ligne au lieu d'en ajouter une, une flèche apparaît
au-delà d'un panoramique de 0,5, le blanc pâlit jusqu'à un gris de 75, la boîte
est à 35 pixels GUI du bas et 2 du bord. Un son hors de portée à son départ n'a
pas de ligne. **Rien de cela n'est mesuré au pixel contre le vrai client.**

## 8. Ce que le client joue de plus

- **le clic des boutons** : `ui.button.click`, catégorie maître, relatif, à
  chaque bouton pressé des menus. Le volume 0,25 est **le nôtre** ; aucune
  capture ne peut l'entendre, il ne voyage pas ;
- **les disques** (§ 2).

## 9. L'import

`ov-assetimport` importe désormais les effets sonores **par défaut** (152 Mo,
dans `run/assets/`, gitignoré — rien n'entre dans le dépôt) : le client les
joue, les laisser dehors par défaut rendait le jeu muet sans raison.
`--no-sounds` les écarte, `--music` ajoute toujours musique et disques (432 Mo).

## 10. Ce qui n'est pas fait, nommé

- **Les éclaboussures** en entrant dans l'eau : ni documentées en volume ni
  capturées ; muettes plutôt qu'inventées.
- **Les crédits** : pas d'écran de fin, donc jamais cette situation.
- **Update Tags** n'est pas décodé : la liste des biomes « sous l'eau » est une
  table (§ 6).
- **« Now Playing » en arc-en-ciel** : le texte est là, pas l'animation de
  couleur de vanilla.
- **Deux juke-box qui jouent le même disque** s'arrêtent ensemble : Stop Sound
  arrête par catégorie et événement, pas par position.
- Les **bornes de voix** de vanilla restent inconnues ; la priorité est la
  nôtre.
- Le **tirage de la variante** par la graine n'est toujours pas vérifié contre
  le vrai client (`son.md`).
- Les sous-titres ne sont **pas comparés au pixel** au vrai client.
