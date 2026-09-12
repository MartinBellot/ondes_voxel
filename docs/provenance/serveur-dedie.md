# Serveur dédié — `server.properties`, listes, RCON, Query, chien de garde

Ce que le serveur dédié fait comme vanilla 1.20.1 pour être **administré** : ses fichiers, ses
commandes d'administration, la porte d'entrée, l'icône et le MOTD, la console distante (RCON),
l'interrogation UDP (Query), le chien de garde, l'arrêt propre et la sauvegarde périodique.

Code : `src/ov_server/src/admin/` (`java_compat`, `server_properties`, `user_lists`,
`server_admin`, `rcon`, `query`, `watchdog`, `status_icon`), `src/ov_server/src/commands/
admin_commands.cpp`, la complétion de `commands/console.cpp`, la compression de
`ov_protocol/src/{framing,listener}.cpp`, et des blocs `// ── dedicated server administration ──`
dans `server.cpp`. Mesures : `scripts/capture_admin.py` (les deux serveurs, même répertoire de
départ) et `scripts/check_admin.py`. Tests : `src/ov_server/tests/test_admin.cpp`,
`test_admin_commands.cpp`.

**Authentification : hors ligne uniquement**, par décision produit. Les UUID sont les UUID hors
ligne (`OfflinePlayer:<nom>`, MD5, version 3).

---

## 1. Les sources

| Sujet | Source | Statut |
|---|---|---|
| Clés de `server.properties`, défauts, effets | Minecraft Wiki, *Server.properties* (révision 1.20.1) ; **le fichier que le vrai serveur écrit à son premier démarrage** (`capture_admin.py properties`) | ✅ l'ensemble des clés et les défauts viennent du fichier mesuré |
| Format du fichier | Javadoc de `java.util.Properties` (`load`, `store`) : lignes logiques, séparateurs `=` `:` blanc, échappements, `\uXXXX` | ✅ |
| Ordre des clés | comportement documenté de `ConcurrentHashMap` (JDK 17, celui de la 1.20.1) : table en puissances de deux, `spread(h) = (h ^ h>>>16) & 0x7fffffff`, seuils de redimensionnement, ordre dans un seau ; le jar itère **la table que ses insertions ont remplie**, sans copie (§ 8) | ✅ re-spécifié, tranché par OpenJDK 17 (`scripts/jdk_order_oracle.java`), **57 / 57 lignes** du fichier mesuré |
| `ops.json`, `whitelist.json`, `banned-players.json`, `banned-ips.json` | Minecraft Wiki, *ops.json*, *whitelist.json*, *banned-players.json*, *banned-ips.json* ; les fichiers écrits par le vrai serveur | ✅ |
| Ordre des entrées de ces listes | comportement documenté de `HashMap` (seaux, ordre d'insertion dans un seau, table qui ne rétrécit pas) — l'ordre des seaux de hachage de Java était déjà payé dans `effets.md` | ✅ |
| Échappement JSON | documentation de Gson (`GsonBuilder`, échappement HTML par défaut : `=` `<` `>` `&` `'`) | ✅ |
| Dates des listes | `SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z")` ; « forever » pour une expiration absente | ✅ |
| Textes de refus à la connexion | clés `multiplayer.disconnect.*` de `en_us.json` (asset local) ; l'ordre des refus relu sur les réponses du jar | ✅ mesuré |
| RCON | Minecraft Wiki, *RCON* (archive wiki.vg) : paquet `longueur, id, type, corps, 0x00 0x00`, types 3/2/0, `-1` au mauvais mot de passe, réponses découpées à 4096 octets | ✅ client écrit depuis la page, rejoué contre les deux serveurs |
| Query | Minecraft Wiki, *Query* (archive wiki.vg) : GameSpy 4, `0xFE 0xFD`, poignée de main type 9, statistiques de base et complètes, jeton de défi | ✅ idem |
| Icône | *Server List Ping* (archive `…/Protocol?oldid=2773082`) : `favicon` = `data:image/png;base64,…`, PNG 64 × 64 | ✅ |
| Set Compression | archive `…/Protocol?oldid=2773082`, *Set Compression* (login, 0x03) et le format compressé des trames | ✅ |
| Chien de garde | Minecraft Wiki, *Server.properties* (`max-tick-time`) : message, rapport de plantage, sortie | ✅ textes du wiki |

Aucun code de Paper, Spigot, Bukkit, CraftBukkit ou Purpur n'a été lu, ni aucun code décompilé.
Le comportement a été re-spécifié depuis la documentation et **mesuré** sur les réponses du jar.

---

## 2. `server.properties`

Au démarrage d'un serveur **dédié** (jamais d'un serveur intégré : ses réglages sont ceux du
monde et de la fenêtre), le fichier du répertoire courant est lu, chaque clé connue est
complétée par son défaut, et le fichier est **réécrit comme vanilla le réécrit** : en-tête
`#Minecraft server properties`, ligne de date au format `Date.toString()`
(`#Fri Sep 11 18:40:12 CEST 2026`), puis les clés dans l'ordre d'itération de la table de hachage
du JDK. Une clé inconnue est **gardée** et réécrite. Une valeur illisible retombe sur son défaut et
est réécrite ainsi comprise, comme vanilla : `max-players=abc` devient `20`, `view-distance=08`
devient `8`, `hardcore=yes` devient `false` (un booléen n'est vrai que s'il dit `true`),
`difficulty=3` devient `hard`.

Un drapeau de ligne de commande (`--port`, `--world`, `--motd`) l'emporte pour ce lancement et
n'est pas réécrit, comme `--port` et `--world` chez vanilla.

### Ce qui agit

| Clé | Effet ici |
|---|---|
| `motd`, `max-players`, `server-port`, `server-ip` | Status, porte d'entrée, `/list`, écoute ; RCON et Query sur `server-ip` |
| `level-name` | le répertoire du monde |
| `level-seed` | un monde **pas encore créé** : un nombre est la graine, un autre texte son `String.hashCode` |
| `gamemode`, `difficulty`, `hardcore` | appliqués au monde à chaque démarrage, comme vanilla — **seulement depuis un fichier présent avant ce démarrage** (§ 7) |
| `white-list`, `enforce-whitelist` | la porte ; `/whitelist on|off` réécrit `white-list` |
| `online-mode` | doit être `false` ; `true` est **refusé** avec un message clair et le serveur ne démarre pas |
| `network-compression-threshold` | Set Compression avant Login Success ; `-1` n'en envoie pas |
| `player-idle-timeout` | exclusion des inactifs ; `/setidletimeout` le réécrit |
| `max-tick-time` | le chien de garde ; `-1` (ou tout ≤ 0) le coupe |
| `enable-rcon`, `rcon.port`, `rcon.password`, `broadcast-rcon-to-ops` | § 4 |
| `enable-query`, `query.port` | § 5 |
| `op-permission-level` | le niveau que `/op` donne |
| `broadcast-console-to-ops` | les opérateurs lisent-ils les commandes de la console |
| `spawn-protection` | casser et poser refusés près du point d'apparition (surface seulement, jamais pour un opérateur, et seulement s'il existe un opérateur) ; le client reçoit le bloc tel qu'il est |
| `allow-nether` | `false` ferme le Nether (en plus de `OV_NETHER=0`) |
| `spawn-monsters`, `spawn-animals` | les propositions du générateur naturel de la catégorie sont écartées (§ 7) |
| `view-distance`, `simulation-distance` | envoyées au client dans Login ; le rayon de chargement suit `view-distance` jusqu'à 8 (§ 7) |
| `hide-online-players` | pas d'échantillon de joueurs dans Status |

### Lu, écrit, sans système pour agir

`pvp` (ce serveur n'a pas de chemin d'attaque joueur contre joueur), `spawn-npcs` (aucun PNJ
n'apparaît naturellement ici), `allow-flight`, `force-gamemode`, `enable-command-block`,
`function-permission-level` (pas de fonctions), `enforce-secure-profile` et
`prevent-proxy-connections` (sans effet hors ligne : vanilla ne les consulte qu'en ligne, et
annonce lui-même `enforcesSecureChat=false` hors ligne), `resource-pack*`, `rate-limit`,
`entity-broadcast-range-percentage`, `max-world-size`, `sync-chunk-writes`, `use-native-transport`,
`enable-jmx-monitoring`, `enable-status`, `text-filtering-config`, `generator-settings`,
`initial-*-packs`, `max-chained-neighbor-updates`.

---

## 3. Les listes et la porte

Les quatre fichiers ont la forme de vanilla — mêmes clés, même ordre de clés, indentation de
Gson, échappement HTML de Gson — et leurs entrées sont écrites **dans l'ordre de la `HashMap`**
de vanilla, clé = texte de l'UUID (ou l'adresse IP) : un fichier écrit par le vrai serveur est lu
ici, réécrit à l'identique, et relu par lui.

Les dates sont les seules heures murales du serveur, et ce sont des métadonnées pour les
humains, jamais de la logique de jeu : l'horloge et le fuseau sont **passés explicitement**
(`WallClock`), les tests tournent sur un instant fixe. Une date illisible vaut « maintenant »
pour `created` et « forever » pour `expires`, les replis de vanilla. Un bannissement expiré est
retiré quand on le demande, comme chez vanilla.

**La porte**, dans l'ordre de vanilla, avant Set Compression, en Login Disconnect : profil banni
(`multiplayer.disconnect.banned.reason` + l'expiration s'il y en a une), puis la liste blanche
(`multiplayer.disconnect.not_whitelisted` ; un opérateur y est toujours), puis l'adresse bannie
(`multiplayer.disconnect.banned_ip.reason`), puis le serveur plein
(`multiplayer.disconnect.server_full`, sauf `bypassesPlayerLimit`). Un même profil déjà
connecté est renvoyé avec `multiplayer.disconnect.duplicate_login` et le nouveau entre.

Les listes sont lues par le thread réseau (chaque Login Start) et modifiées par le thread de
tick (les commandes) : elles ont leur propre verrou. Ce n'est pas le monde — aucun bloc ni aucune
entité ne passe par cet objet — et la règle « aucun mutex sur le monde » tient.

### Les commandes

`ban`, `ban-ip`, `banlist [ips|players]`, `pardon`, `pardon-ip`, `whitelist
(on|off|list|add|remove|reload)`, `op`/`deop` (niveau `op-permission-level`), `kick`,
`setidletimeout`, `save-all [flush]`, `save-on`, `save-off`, `stop`, `list [uuids]`,
`defaultgamemode`, `seed`, `debug (start|stop)` — enregistrées **à leur place dans l'ordre de
vanilla** (celui de son paquet Commands). Les commandes propres au serveur dédié n'existent que
là où il y a des listes : un serveur intégré ne les a pas, comme vanilla.

**Un nom que personne n'a sur le serveur devient le profil hors ligne de ce nom en minuscules.**
Mesuré : `ban Ovq_alice` répond « Banned **ovq_alice** » et inscrit l'UUID hors ligne de
`ovq_alice` — qui n'est pas celui du joueur `Ovq_alice`, que le vrai serveur laisse donc entrer
juste après l'avoir banni. Il en va de même pour `pardon`, `op`, `deop` et `whitelist`. Un joueur
en ligne garde son propre profil. La raison d'un bannissement est une **chaîne nue** dans les
réponses de `ban`, `ban-ip` et `banlist` ; une expiration s'affiche
« `2099-01-02 at 04:04:05 CET` ».

Status laisse de côté un échantillon vide et `enforcesSecureChat` quand il est faux, comme le jar.

`publish` n'existe que sur un serveur intégré (vanilla : « Unknown or incomplete command » sur un
dédié) ; son arbre est celui du jar, `publish [allowCommands] [gamemode] [port]`, et sa réponse
un **refus par son nom** (§ 7).

---

## 4. RCON

TCP, le paquet de la page *RCON* : `int32 LE longueur | int32 LE id | int32 LE type | corps |
0x00 0x00`. Type 3 : connexion, réponse type 2 avec l'id, ou `-1` au mauvais mot de passe. Type 2
envoyé avant le mot de passe : **aucune réponse** (mesuré ; `-1` n'est que pour un mauvais mot de
passe). Type 2 authentifié : la commande tourne **sur le thread de tick** comme une
ligne de console, sous le nom `Rcon` au niveau 4, et ce qu'elle aurait imprimé revient en type 0,
découpé en morceaux de 4096 octets au plus, un paquet chacun (un paquet vide pour une réponse
vide). Un type inconnu répond `Unknown request <hex>`. Un paquet coupé en deux écritures TCP est
réassemblé — le jar, lui, n'y répond pas et la session meurt (§ 7). Sans mot de passe, RCON ne démarre pas (le message de vanilla).

## 5. Query

UDP, GameSpy 4 : `0xFE 0xFD | type | session`. Type 9 : un jeton de défi (texte décimal), retenu
30 secondes pour l'expéditeur. Type 0 + jeton : statistiques de base (MOTD, `SMP`, carte,
joueurs, maximum, port en petit-boutiste, IP) ; + 4 octets : statistiques complètes (`splitnum`,
paires clé/valeur, `player_`, noms). Un mauvais jeton ne reçoit rien. Les chaînes partent en **ISO-8859-1** (« é » est l'octet 0xE9,
mesuré) et `hostip` est l'adresse de la machine quand `server-ip` est vide (le jar donne son adresse
de réseau local, pas `0.0.0.0`). La réponse est bâtie depuis
un instantané pris sous le verrou de la table des joueurs : le thread Query ne lit jamais le monde.

## 6. Chien de garde, arrêt, sauvegarde, console

* **Chien de garde** : le thread de tick date le début de chaque tick ; un thread à lui compare.
  Un tick plus long que `max-tick-time` : les deux lignes de vanilla (« A single server tick took
  60.00 seconds (should be max 0.05) » / « Considering it to be crashed, server will forcibly
  shutdown. »), un rapport `crash-reports/crash-<date>-server.txt`, sortie **1 sans sauvegarder**
  — le thread qui possède le monde est celui qui est bloqué. Horloge monotone uniquement ; le
  monde reste déterministe que le chien de garde tourne ou non.
* **Arrêt propre** : `/stop`, SIGINT, SIGTERM — la boucle finit son tick, le chien de garde, RCON
  et Query s'arrêtent, chaque joueur reçoit `multiplayer.disconnect.server_shutdown`, les joueurs
  et le monde sont sauvés.
* **Sauvegarde périodique** : toutes les **6000 ticks** comme vanilla (au lieu de 30 secondes
  d'horloge murale auparavant), et plus du tout après `save-off`. `save-all` sauve toujours.
* **Console** : dans un terminal, Tab complète avec les suggestions du moteur de commandes — les
  mêmes qu'un client reçoit —, une seule proposition remplace le mot, plusieurs sont listées.
  Hors terminal (tube, script), la console lit les lignes exactement comme avant. La console de
  vanilla 1.20.1 ne complète pas : c'est un ajout, qui ne change aucune réponse.
* **Inactivité** : toute action compte sauf l'entretien (keep-alive, confirmation de téléport,
  réglages du client, canal de plugin) ; un paquet de mouvement ne compte que s'il dit quelque
  chose de nouveau. Au-delà du délai : `multiplayer.disconnect.idling`.

---

## 7. Écarts nommés

* **Premier démarrage : `online-mode=false`**, là où vanilla écrit `true` — sinon le serveur
  qu'il vient de configurer refuserait de démarrer. Un fichier existant qui dit `true` est refusé.
* **`gamemode` et `difficulty` ne s'appliquent que depuis un fichier présent avant ce
  démarrage.** Un premier démarrage garde les défauts de ce serveur (créatif, normal) dont tous les
  outils du projet dépendent. Décision validée par la coordination.
* **Le rayon de chargement ne dépasse pas 8** : `view-distance` le réduit, ne l'agrandit pas ;
  le client reçoit la valeur du fichier.
* **`spawn-monsters` / `spawn-animals`** : vanilla saute les tentatives de la catégorie ; ici ses
  propositions sont écartées après tirage. Rien n'apparaît dans les deux cas, mais le flux
  aléatoire du générateur n'est pas consommé pareil.
* **`save-off`** arrête la sauvegarde périodique ; un chunk déchargé peut encore être écrit.
* **`debug function`** attend les fonctions : son nœud manque (l'arbre de `debug` diffère du jar
  sur ce seul nœud). `debug stop` n'écrit pas de fichier `debug/profile-results-*.txt`.
* **`perf` et `jfr`** ne sont pas là.
* **`publish`** : refusé par son nom. Le serveur intégré écoute déjà sur une socket à lui, mais
  l'annonce LAN et les droits donnés aux invités ne sont pas écrits.
* **`usercache.json`** n'est pas écrit (aucune résolution de nom par l'API de Mojang ici) : un nom
  déjà venu mais hors ligne se résout comme un inconnu, en minuscules.
* **L'icône** : le jar ré-encode `server-icon.png` avec ImageIO avant de l'envoyer ; nous envoyons
  les octets du fichier. Même image, pixel pour pixel, octets différents.
* **Une requête RCON coupée en deux écritures TCP** : le jar n'y répond pas et la session meurt ;
  celui-ci la réassemble. Aucune réponse à un client ordinaire n'en change.
* **`help`** liste 79 commandes chez le jar, moins ici (les commandes manquantes, pas
  l'administration) ; et l'horloge d'un monde neuf part de 0 chez le jar, de 1000 ici — un défaut du
  monde, hors de ce travail.
* **La compression** ne s'applique qu'au serveur dédié ; l'hôte d'un serveur intégré est local et,
  comme chez vanilla, non compressé. Les paquets sont bâtis non compressés puis retramés par la
  connexion sur son propre thread — jamais sur le thread de tick.

---

## 8. Ce que la mesure a trouvé

**Une connexion fermée par le serveur n'était jamais signalée.** Avec `max-players=3` et deux
joueurs connectés, un troisième était refusé : « serveur plein ». Le journal du serveur, gardé par
la capture, montrait pourquoi : un joueur renvoyé pour double connexion restait dans la table.
La cause était dans `ov_protocol` : `close()` marquait la connexion fermée sans prévenir
l'écouteur, et la lecture suivante, qui aboutit dans `finish()`, voyait la marque et repartait sans
rien dire. Seul un client qui partait de lui-même était signalé. Un joueur exclu (`/kick`, `/ban`),
expiré (keep-alive), refusé (paquet invalide) ou doublé restait compté, n'était pas sauvé à ce
moment-là, et expirait de nouveau à chaque tick — le journal montrait trois fois le même
« did not answer a keep-alive ». Les deux chemins aboutissent maintenant, une seule fois, dans
`notify_closed()` ; `test_listener.cpp` le vérifie sur une vraie socket, des deux côtés.

**Le jar écrit sa propre table, pas une copie — 1 ligne sur 57, puis 57 sur 57.** La première
version supposait que le fichier était l'itération d'une copie (`putAll` dans un `Properties` neuf,
pré-dimensionné à 256 seaux) : comparé au fichier du vrai serveur, **une seule ligne sur 57** était
à sa place. L'ordre de vanilla gardait pourtant notre ordre relatif sur des séries entières, les
autres clés intercalées : la signature d'une table plus petite. Le jar écrit donc la table même
qu'il a remplie, que les insertions ont fait grandir jusqu'à 128 seaux : **53 lignes sur 57** d'un
coup. Les quatre dernières étaient deux paires échangées dans un même seau, dont l'ordre dépend de
celui des insertions — et elles se sont placées quand les quatre clés du pack de ressources du
serveur sont lues **en dernier**, après les réglages du monde : **57 lignes sur 57**. Chaque étape a
été tranchée par OpenJDK 17 lui-même, lancé sur `scripts/jdk_order_oracle.java` (du `java.util`
pur, aucun code du jeu), puis comparée au fichier capturé. Ce même oracle montre qu'un redémarrage
ne déplace aucune clé. Ses ordres sont épinglés dans les tests : les 56 clés d'un premier
démarrage, la clé inconnue à sa place, le redémarrage, et 14 UUID et 5 adresses dans l'ordre d'une
`HashMap`.

**Deux clés en trop, et un é échappé.** `log-ips` et `resource-pack-id` ne sont pas dans le fichier
du jar : elles appartiennent à des versions plus récentes. Le jar écrit `motd=Café \: \= x` —
l'é tel quel, en UTF-8 : c'est la forme `Writer` de `Properties.store`, qui n'échappe que les
caractères spéciaux ; nous écrivions l'échappement `Caf\u00E9` de la forme `OutputStream`.

**Ce que la comparaison avec le jar a corrigé.** Les noms inconnus en minuscules (le vrai serveur
laissait entrer le joueur qu'il venait de bannir, et c'est ce qu'il fallait reproduire) ; la
raison en chaîne nue ; le « at » de l'expiration ; l'échantillon vide et `enforcesSecureChat` omis
dans Status ; l'ISO-8859-1 et l'adresse de la machine dans Query ; le silence de RCON avant le mot
de passe ; et, côté écouteur, **ce qui était mis en file avant une fermeture** : la cible d'un
`ban-ip <joueur>` recevait la ligne de succès, ni la ligne « affects 1 player » ni sa propre
déconnexion — la fermeture coupait la socket derrière le paquet en cours d'écriture. Elle attend
maintenant que la file soit vide (`test_listener.cpp`, 160 Ko envoyés puis fermés, tout arrive).

**Les pièges de la mesure elle-même.** RCON et Query sur le port + 1 et + 2 tombaient sur le port
d'une autre campagne (« Address already in use ») : ils sont maintenant sur les deux ports de celle-ci,
RCON sur le second, Query en UDP sur le premier — la valeur par défaut de vanilla. « Done » paraît
**avant** que le jar ait fini de démarrer : un ping de Status envoyé aussitôt était fermé sans
réponse ; la capture attend la dernière ligne du démarrage. Et un nom inconnu part vers l'API de
profils de Mojang avant la réponse : la fenêtre de ces commandes est allongée, sans quoi leur réponse
arrive dans celle de la commande suivante.

---

## 9. Mesures

*(Section complétée par la campagne `capture_admin.py` sur les deux serveurs.)*
