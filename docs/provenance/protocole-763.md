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
