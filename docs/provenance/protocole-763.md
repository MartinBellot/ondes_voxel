# Protocole 763 — catalogue, matrice de conformité, fuzz

## Le catalogue : `data/protocol/763.json`

176 paquets — Handshaking 2, Status 2 + 2, Login 5 + 3, Play 111 + 51 — avec, pour
chacun, l'état, le sens, l'id, le nom du wiki et celui de minecraft-data. **Pas les
champs** : les champs sont spécifiés paquet par paquet dans le C++, depuis l'archive,
et testés octet à octet. Un schéma de champs serait une seconde copie qui dériverait.

Deux sources indépendantes, lues par `scripts/derive_protocol_catalog.py`, qui
refuse d'écrire quoi que ce soit si elles divergent sur un seul id :

| Source | Licence | Ce qui est lu |
|---|---|---|
| PrismarineJS/minecraft-data, `data/pc/1.20/protocol.json` (`dataPaths.json` associe pc 1.20.1 à ce fichier) | MIT, © PrismarineJS | les tables `mappings` id → nom de chaque état et sens |
| Archive figée wiki.vg, `minecraft.wiki/w/Minecraft_Wiki:Projects/wiki.vg_merge/Protocol?oldid=2773082` (bandeau : « 1.20.1, protocol 763 ») | CC BY-NC-SA (wiki) — seuls des noms et des numéros sont repris | les titres de paquets de niveau 4 et leurs cellules « Packet ID » / « Bound To » |

**Mesure** : les deux sources s'accordent sur les **176 ids sur 176**, dans les deux
sens (rien chez l'une n'est absent de l'autre). Aucune donnée Mojang n'est lue.

Une remarque d'historique : `play.hpp` affirme que « l'archive du wiki s'est trompée
sur chaque id vérifié ». Ce n'est pas vrai de **cette** archive (oldid 2773082), qui
est conforme partout ; la remarque date vraisemblablement d'une autre révision de la
page, qui documentait une autre version du protocole. C'est la raison d'être de la
règle « archive figée » de `CLAUDE.md`.

## La matrice : `docs/protocol/763/README.md`

Générée par `scripts/protocol_matrix.py` à partir du code, jamais écrite à la main :

- les **constantes d'id** sont lues dans les espaces `clientbound` / `serverbound`
  des en-têtes de `ov_protocol` (et quelques constantes locales à `ov_server`), et
  **leur valeur est comparée au catalogue** ;
- les fonctions `encode_*` / `parse_*` / `decode_*` sont attribuées aux paquets par
  leur nom, plus une table explicite pour les cas ambigus (même nom dans les deux
  sens) — une entrée de cette table qui ne désigne plus rien est une erreur ;
- les tests sont découpés en `TEST_CASE` : « aller-retour » veut dire qu'un même cas
  appelle l'encodeur et le décodeur, « vanilla » qu'un cas les appelle sur un
  littéral hexadécimal dont il dit qu'il vient du vrai serveur.

`--check` (job CI *Guard rails*) échoue si la matrice commitée est périmée **ou**
si une constante contredit le catalogue. `scripts/test_enforcement.sh` plante les
deux fautes et vérifie qu'elles sont refusées.

### Ce que la vérification a trouvé

**Set Cooldown partait avec l'id 0x16.** En 763, 0x16 est *Chat Suggestions* ;
*Set Cooldown* est 0x15 — les deux sources le disent, et 0x14 (*Set Container
Slot*) comme 0x17 (*Plugin Message*) encadrent la valeur. Un client vanilla qui
lançait une perle recevait donc ses deux varints (objet, ticks) comme une action
de suggestions de chat. Corrigé ; la constante est maintenant vérifiée à chaque
push.

## Le fuzz : `src/ov_protocol/tests/fuzz_decoders.cpp`

Un exécutable de test à part, `fuzz_ov_protocol` (étiquettes `unit` et `fuzz`),
pour que le build ASan puisse le compiler seul (`--target`).

**Pourquoi pas libFuzzer** : il demande une chaîne instrumentée pour la
couverture que macOS, Linux et Windows ne partagent pas, et un corpus qui dérive
d'une machine à l'autre. Ici le générateur est un SplitMix64 à graine fixe : un
échec se reproduit à partir de la graine seule, partout, et le passage tient dans
le budget des tests unitaires. Sa valeur vient du preset ASan + UBSan, où une
lecture hors bornes ou une allocation absurde devient un crash au lieu d'une
réponse fausse et silencieuse.

**72 points d'entrée** : tous les décodeurs qui lisent des octets de socket, dans
les deux sens (un serveur hostile vise `ov_netclient` autant qu'un client hostile
vise le serveur), le chunk dans les deux formes de monde, les primitives (VarInt,
VarLong, chaîne, UUID, position, angle, slot) et le framer. Chaque entrée est
donnée à **tous** les décodeurs, pas seulement au sien : une socket ne garantit
pas que les octets correspondent à l'id.

Quatre sortes d'entrées :

1. chaque préfixe de chaque paquet valide (la troncature est l'attaque la plus
   courante) ;
2. des paquets valides mutés : bit inversé, octet remplacé par une valeur limite,
   VarInt hostile inséré (2³¹−1, −1, une longueur au-delà du plafond de 2 Mio),
   suite d'octets de continuation ;
3. des octets aléatoires ;
4. un flux de paquets bien encadrés, muté puis coupé à des endroits aléatoires et
   passé au framer, sans compression, avec un seuil à 0 et à 256.

Plus un cas ciblé : une trame compressée qui annonce une taille décompressée
au-delà du plafond, que le framer doit refuser avant d'allouer.

Les propriétés vérifiées : aucun crash ; un VarInt lu ne dépasse jamais 5
octets (10 pour un VarLong) ; aucun corps accepté par le framer ne dépasse
2 Mio. Le corpus de départ est lui-même vérifié : un cas contrôle que nos propres
décodeurs acceptent les graines, sans quoi chaque mutation ne testerait que le
premier champ.

**Mesure (2026-09-11)** : vert sous ASan + UBSan (macOS, `-gline-tables-only`)
au premier passage, soit 6 cas et 28 929 assertions. **Aucun crash trouvé** :
les décodeurs existants bornaient déjà leurs comptes avant d'allouer. Le fuzz
n'a donc rien corrigé ; il empêche que ça régresse.

`protocol_matrix.py` ignore les fichiers `fuzz_*` : un harnais qui appelle tous
les décodeurs sans vérifier ce qu'ils rendent marquerait chaque paquet « testé ».

## Les paquets d'interface : `ov/protocol/hud.hpp`

Boss Bar, les six paquets de bordure du monde, Display Objective, Update
Objectives, Update Teams, Update Score, Award Statistics, Select Advancements Tab
et Seen Advancements — encodeur **et** décodeur pour chacun.

Les dispositions de champs viennent des tableaux de l'archive figée (oldid
2773082), puis ont été **comparées champ par champ** à `data/pc/1.20/protocol.json`
de minecraft-data. Les tests écrivent les octets attendus à la main depuis ces
tableaux, pas depuis notre encodeur.

**Un seul désaccord** : la durée d'interpolation de la bordure (*Speed* dans
Initialize World Border et Set Border Lerp Size) est une **VarLong** pour
l'archive, une **varint** pour minecraft-data. L'archive est suivie. Une durée en
millisecondes dépasse 2³¹ au bout de 24 jours, ce qu'une commande
`/worldborder set … <temps>` atteint facilement. Un test fixe le cas de 2⁴⁰ ms
sur six octets, qu'un lecteur de varint refuserait. **Non vérifié sur le vrai
serveur** : aucune capture n'a été faite pour ces paquets.

Update Advancements (0x69), l'arbre complet des progrès avec affichage et
critères, n'est **pas** fait : c'est un paquet d'une autre taille, qui mérite son
propre lot.
