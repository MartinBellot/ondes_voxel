# Provenance

Ondes VOXEL est une réimplémentation **clean-room** : chaque système est
spécifié depuis de la **documentation**, jamais depuis le code source d'une
autre implémentation.

Ce fichier trace, pour chaque système non trivial, la source documentaire
utilisée. Il n'est pas une formalité : c'est ce qui permet de répondre à la
question « d'où vient ce comportement ? » sans avoir à s'en souvenir, et ce qui
rend vérifiable l'affirmation que le projet ne contient pas de code dérivé.

Toute entrée doit être ajoutée **au moment** où le système est implémenté.

---

## Sources de référence

### Protocole réseau

⚠️ **wiki.vg a fermé le 30 novembre 2024.** Son contenu (CC BY-SA) a été fusionné
dans le wiki officiel. Deux pièges en découlent :

1. `https://minecraft.wiki/w/Java_Edition_protocol` documente la **version
   courante du jeu**, pas la 1.20.1. S'y référer produit du code pour le mauvais
   protocole, et l'erreur ne se voit qu'à la connexion.
2. Il n'existe pas d'URL officielle dédiée au protocole 763 ; il faut passer par
   une révision figée.

| Sujet | URL | Statut |
|---|---|---|
| **Protocole 763 (archive figée)** | `https://minecraft.wiki/w/Minecraft_Wiki:Projects/wiki.vg_merge/Protocol?oldid=2773082` | ✅ **la référence à utiliser** |
| Numéros de version | `https://minecraft.wiki/w/Protocol_version` | ✅ confirme 763 = 1.20 / 1.20.1 |
| Historique du protocole | `https://minecraft.wiki/w/Minecraft_Wiki:Projects/wiki.vg_merge/Protocol_History` | ✅ |
| Types de données (VarInt, VarLong) | `https://minecraft.wiki/w/Java_Edition_protocol/Data_types` | ⚠️ version courante |
| Chiffrement et login | `https://minecraft.wiki/w/Java_Edition_protocol/Encryption` | AES/CFB8, secret 16 o, hash SHA-1 |
| Métadonnées d'entité | `https://minecraft.wiki/w/Java_Edition_protocol/Entity_metadata` | ⚠️ version courante |
| Format de chunk **réseau** | `https://minecraft.wiki/w/Java_Edition_protocol/Chunk_format` | ⚠️ **distinct du format disque** |
| Miroir statique de secours | `https://c4k3.github.io/wiki.vg/` | état pré-novembre 2024 |

### Format de sauvegarde

| Sujet | URL | Points clés vérifiés |
|---|---|---|
| Fichiers région | `https://minecraft.wiki/w/Region_file_format` | En-tête 8 KiB = 1024 offsets + 1024 timestamps. Chunk = longueur (4 o BE) + type de compression (1 o) + données. **1 = GZip, 2 = Zlib (défaut), 3 = aucune.** LZ4 (4) et custom (127) sont postérieurs à 1.20.1. |
| Format Anvil | `https://minecraft.wiki/w/Anvil_file_format` | Ordre YZX, biomes par section |
| Format de chunk | `https://minecraft.wiki/w/Chunk_format` | Depuis 1.18 : `sections[].block_states.{palette,data}` et `sections[].biomes.{palette,data}` (4×4×4, ≤ 64 entrées). **Pas** l'ancien `Level.Sections[].Palette`. |
| `level.dat` | `https://minecraft.wiki/w/Java_Edition_level_format` | NBT gzippé, racine → `Data`, écriture sûre via `level.dat_old` |
| NBT | `https://minecraft.wiki/w/NBT_format` | 13 types (ID 0-12), big-endian |
| NBT (spec historique) | `https://github.com/twoolie/NBT/wiki/Specification` | Spec originale de Notch. L'URL d'origine (`minecraft.net/docs/NBT.txt`) n'existe plus. |

### Données de jeu

| Source | Licence | Usage |
|---|---|---|
| Data generator officiel (`server.jar --all`) | données Mojang | Source de vérité des IDs de registre. **Généré localement, jamais commité.** |
| `PrismarineJS/minecraft-data` | **MIT** (vérifiée) | `protocol.json` → génération des codecs de paquets ; blocks, items, recipes, entities, biomes pour 1.20.1 |
| `misode/mcmeta` | **inconnue** — aucun LICENSE trouvé | ❌ **Écarté.** Traité comme « tous droits réservés » par défaut. |

---

## Implémentations tierces

Consultables **pour l'architecture**, jamais pour copier du code.

| Projet | Licence | Verdict |
|---|---|---|
| Cuberite (C++) | Apache-2.0 | ✅ Compatible, la référence C++ la plus proche |
| Valence (Rust) | MIT | ✅ Bon modèle de protocole et d'ECS |
| MCHPRS (Rust) | MIT | ✅ Référence redstone |
| Feather (Rust) | Apache-2.0 | ✅ Compatible, mais abandonné (~avril 2024) |
| Glowstone cœur (Java) | MIT | ✅ — ⚠️ **Glowkit est GPL**, ne pas y toucher |
| Luanti / Minetest (C++) | LGPL-2.1+ | ⚠️ Architecture seulement ; un copier-collé contamine le fichier |
| **Paper / Purpur / Spigot / Bukkit** | **GPLv3** | 🚫 **Ne pas lire.** Incompatible avec Apache-2.0. |

---

## Journal par système

Une ligne par système non trivial, ajoutée au moment de son implémentation.

| Système | Module | Source | Notes |
|---|---|---|---|
| Cadence de tick 20 Hz | `ov_base` | `https://minecraft.wiki/w/Tick` | 1 tick = 50 ms exactement. Le plafond de rattrapage et l'abandon du backlog reproduisent le comportement « Can't keep up » de vanilla. |
| Numérotation des faces | `ov_math` | archive protocole 763, paquet *Use Item On* | down=0, up=1, north=2, south=3, west=4, east=5. Valeurs de protocole, pas un choix interne. |
| Index de section YZX | `ov_math` | `Chunk_format` | `(y * 16 + z) * 16 + x`. Partagé par Anvil et le paquet de chunk. |
| Slot de région | `ov_math` | `Region_file_format` | Table de 1024 entrées indexée z-major : `(z % 32) * 32 + (x % 32)`. |
| Limites de construction | `ov_math` | `Chunk_format` | Overworld y ∈ [-64, 319] depuis 1.18, sections -4 à 19. Impose la division plancher. |
| Collision AABB par axe | `ov_math` | `https://minecraft.wiki/w/Entity` + archive protocole 763 | Résolution Y puis X puis Z. Tester les trois d'un coup empêche de glisser le long d'un mur et de monter une marche. |
| VarInt / VarLong | `ov_protocol` | archive protocole 763, *Data types* | ≤ 5 et ≤ 10 octets. Un négatif occupe **toujours** la taille maximale (complément à deux). |
| Chaînes du protocole | `ov_protocol` | archive protocole 763, *Data types* | Préfixe VarInt en **octets** ; la limite déclarée est en unités UTF-16, donc la borne en octets vaut 3×. |
| Registres et blockstates | `ov_datagen` | data generator officiel, `server.jar` 1.20.1 (SHA-1 `84194a2f286ef7c14ed7ce0090dba59902951553`) | Voir les deux pièges ci-dessous. |

---

## Pièges mesurés dans les données officielles

Deux comportements du dataset 1.20.1 qui cassent silencieusement une
implémentation naïve. Tous deux vérifiés sur les données réelles, pas déduits.

### L'ordre des propriétés dans `blocks.json` n'est pas l'ordre des IDs

Les 24 135 blockstates sont bien un nombre en **base mixte** sur les propriétés
du bloc, poids fort en tête, avec des IDs contigus par bloc — c'est ce qui rend
l'accès aux propriétés arithmétique et O(1), et ce dont dépend la jouabilité de
la redstone.

Mais l'ordre des clés imprimé dans `blocks.json` **n'est pas** celui qu'utilise
cette arithmétique. Quatre blocs sur 1003 divergent :

| Bloc | Ordre dans le fichier | Ordre réel |
|---|---|---|
| `minecraft:chest` | `type, facing, waterlogged` | `facing, type, waterlogged` |
| `minecraft:trapped_chest` | `type, facing, waterlogged` | `facing, type, waterlogged` |
| `minecraft:moving_piston` | `type, facing` | `facing, type` |
| `minecraft:piston_head` | `type, facing, short` | `facing, short, type` |

Se fier à l'ordre du fichier donne des IDs faux **pour ces quatre blocs
seulement**. Rien ne le détecte avant qu'un client vanilla ne se connecte et que
quelques coffres ne deviennent autre chose — un symptôme qui coûte des semaines
à faire remonter jusqu'ici.

`tools/ov_datagen` ne lit donc jamais cet ordre : il le **déduit** des IDs
d'états et vérifie les 24 135 états contre la permutation trouvée.

### `minecraft:mob_effect` est 1-based

Tous les registres sont des plages denses, mais un seul ne commence pas à zéro :
`mob_effect` va de 1 (`speed`) à 33 (`darkness`), parce que l'ID 0 signifie
« aucun effet » dans les paquets qui en transportent un.

Un `protocol_id == index` générique décalerait **tous** les effets de statut d'un
cran. La sortie normalisée porte donc un `first_id` explicite par registre.

### Les fichiers région ne sont pas complétés jusqu'à la frontière de secteur

Un fichier `.mca` alloue à chaque chunk un nombre entier de secteurs de 4096
octets, indiqué dans l'en-tête. La lecture naïve de la documentation conduit à
valider `offset + sector_count ≤ taille_fichier / 4096` — et cette validation
**rejette des mondes parfaitement valides**.

Mesuré sur une vraie sauvegarde : `r.0.0.mca` fait **1 819 235 octets**, soit
444,15 secteurs, alors que son dernier chunk est alloué jusqu'au secteur **445**.
Le fichier s'arrête au milieu du secteur alloué. La charge utile, elle, est
intacte.

La règle correcte : le `sector_count` est une **allocation**, pas une promesse
de contenu. Ce qu'il faut valider, c'est que la **longueur déclarée** du chunk
tient dans ce qui existe réellement — `min(alloué, disponible)`.

Verrouillé par le test « a file not padded to a sector boundary is still
readable » dans `src/ov_nbt/tests/test_region.cpp`.

### Preuve de correction sur données réelles

`ov-inspect` décode, ré-encode et compare octet à octet. Exécuté sur une vraie
sauvegarde Minecraft (2026-09-07) :

| Fichier | Résultat |
|---|---|
| `level.dat` (gzip, 4257 tags, profondeur 11) | ✅ octet-identique |
| `servers.dat` (NBT non compressé) | ✅ octet-identique |
| `mfix_stronghold_cache_v2.nbt` (gzip) | ✅ octet-identique |
| **14 fichiers région, 2900 chunks, 795 277 tags** | ✅ **2900/2900 octet-identiques** |
| `minecraft-1.20.1-client.jar` (ZIP, 23 Mo) | ✅ **20 953/20 953 entrées extraites**, 41 206 169 octets, en 1,2 s |

Toutes les régions utilisaient le schéma **zlib (2)**, conformément à la
documentation. Reproductible avec :
```bash
ov-inspect nbt    <monde>/level.dat --verify
ov-inspect region <monde>/region/r.0.0.mca --verify
ov-inspect zip    <prism>/libraries/com/mojang/minecraft/1.20.1/minecraft-1.20.1-client.jar --verify
```

### Le parseur NBT ne peut pas être récursif

La limite de profondeur de 512 reprend celle de vanilla, et elle existe pour
empêcher un fichier hostile de faire déborder la pile. Elle ne suffisait pas :
un parseur **récursif** à 508 niveaux déborde la pile de **1 Mo** que Windows
donne à un thread — exactement le crash que la limite devait prévenir, à une
profondeur qu'elle autorisait encore. Découvert par le job CI Windows ; macOS et
Linux, avec 8 Mo de pile, passaient.

Baisser la limite aurait signifié rejeter des fichiers que vanilla accepte. Le
parseur utilise donc une **pile explicite sur le tas** : la profondeur ne coûte
plus rien à la pile d'appel, et la limite reste celle de vanilla.

Vérifié avec `ulimit -s 1024` : profondeur 508 acceptée et ré-encodée à
l'identique, profondeur 5000 rejetée par `nesting too deep`.

### Chiffres mesurés (et non estimés)

| Grandeur | Valeur réelle |
|---|---|
| Blocs | **1003** |
| Blockstates | **24 135** — IDs 0 à 24134, contigus, tiennent sur `u16` |
| `minecraft:air` | id **0** (permet à `memset(0)` de produire une section vide) |
| Bloc au plus d'états | `redstone_wire`, **1296** |
| Items | 1255 · Entités 124 · Fluides 5 · Block entities 41 |
| Sons 1474 · Particules 95 · Effets 33 · Enchantements **39** · Potions 43 |
| Blocs ayant `default` ≠ premier ID | 484 sur 1003 — le défaut est une donnée à part |

### Contenu du jar client 1.20.1

Mesuré, et non supposé — c'est ce que `ov-assetimport` doit aller chercher, un
resource pack ne contenant que des textures.

| Sous-arbre | Entrées |
|---|---|
| `assets/minecraft/blockstates/` | **1005** |
| `assets/minecraft/models/block/` | **2016** |
| `assets/minecraft/models/item/` | **1675** |
| `assets/minecraft/textures/block/` | 977 |
| `assets/minecraft/font/` | 7 |
| `assets/minecraft/lang/` | 1 (`en_us.json` ; les autres langues sont dans l'index d'assets) |
| `data/minecraft/` | **5887** — le jar client embarque aussi le datapack vanilla |
| **Total** | 20 953 entrées, toutes deflatées, 41 Mo décompressés |

---

## Palette bit-packée d'une section (`ov_world`)

Source : `Chunk_format` sur l'archive figée du wiki (§ 2 de `CLAUDE.md`).

### La règle qu'on croit connaître et qu'on écrit quand même de travers

Depuis 1.16, **une entrée ne chevauche jamais deux `long`**. À 5 bits, un `long`
en contient 12 et gaspille 4 bits — pas 12,8 entrées. Lire l'index se fait donc
par `index / entrées_par_long` et `index % entrées_par_long`, jamais par une
division du numéro de bit.

L'erreur est silencieuse dans les deux sens : un encodeur qui fait chevaucher les
entrées se relit parfaitement lui-même. Un aller-retour à travers notre propre
code passe donc même avec la règle inversée, et ne prouve rien.

### Ce qui le prouve

`ov-inspect chunk <region.mca>` dépaquette chaque entrée d'une section écrite par
le jeu, la repaquette depuis zéro, et compare les `long` obtenus à ceux du
disque. L'entrée vient de Mojang ; seul le codec est le nôtre.

| Mesure | Valeur |
|---|---|
| Sections traversant réellement le bit-packing | **165 318** — toutes octet-identiques |
| Largeurs couvertes | 4 bits : 73 363 · 5 : 85 656 · 6 : 6 221 · 7 : 78 |
| Entrées dépaquetées puis repaquetées | ≈ 677 millions |
| Sections single-valued (aucune donnée stockée) | 263 778 |

Deux réserves, posées honnêtement :

1. Le monde testé est en **DataVersion 3955 (1.21)**, pas 1.20.1. La règle de
   packing est inchangée depuis 1.16, donc la mesure est valide pour ce qu'elle
   couvre — mais elle ne dit **rien** sur les IDs de blocs, que la commande ne
   résout volontairement pas. La parité des IDs est vérifiée séparément par
   `check_registry_parity.py`.
2. Les sections *single-valued* sont comptées comme conformes sans repaquetage :
   il n'y a par définition aucun `long` à comparer. Le chiffre qui porte une
   affirmation est **165 318**, pas le total.

### Deux détails que vanilla impose et qu'aucune formule ne rattrape

- La largeur est **plancherée à 4 bits**. Une palette de deux entrées s'encode en
  4 bits, pas en 1 — l'encodage plus serré serait plus petit et illisible par le
  client.
- Une section uniforme n'écrit **aucune donnée du tout** : palette d'un élément,
  `data` absent. C'est le cas majoritaire (263 778 sections sur 429 096 ici), et
  celui qu'une implémentation oublie parce qu'il ne ressemble pas aux autres.

---

## `ResourceLocation` (`ov_base`)

Source : page *Resource location* du wiki, jeux de caractères vérifiés ensuite
contre les **1003 noms de blocs** produits par le data generator.

| Règle | Détail |
|---|---|
| Namespace | `[a-z0-9_.-]` — pas de `/`, pas de majuscule |
| Chemin | `[a-z0-9_.-/]` |
| Namespace absent | vaut `minecraft` |
| Deux-points en tête | `:stone` est `minecraft:stone`, **pas** une erreur |
| Deuxième deux-points | caractère de chemin invalide, pas un second séparateur |

Les jeux de caractères sont écrits en dur plutôt que dérivés de `std::isalnum`,
qui dépend de la locale : un identifiant devant correspondre octet pour octet à
celui de Mojang ne peut pas dépendre de la configuration de la machine.

### Le piège : la validité n'est pas une garantie de chemin de fichier

`.` et `/` sont **tous deux** des caractères de chemin légaux. Donc
`minecraft:../../etc/passwd` est un identifiant parfaitement conforme — et
vanilla l'accepte également. Un test le fixe **comme cas passant**, précisément
pour que personne n'en déduise l'inverse : avoir parsé un identifiant
n'autorise pas à le concaténer à un répertoire. Tout ce qui résout une
`ResourceLocation` vers un fichier doit valider le chemin séparément
(`io::is_safe_archive_path`).

C'est le même piège que Zip Slip, à un étage au-dessus.

### Écart assumé au plan

Le plan rangeait ce type dans `ov_registry` (couche 5). Il vit dans `ov_base`
(couche 0) : le chargeur de datapack appartient à `ov_data` (couche 4), donc
**sous** `ov_registry`, et manipule ces identifiants en permanence. Le graphe ne
remonte jamais ; le type n'ayant aucune dépendance, le descendre ne coûte rien.

---

## `LegacyRandomSource` (`ov_math`)

Source : la **javadoc du JDK** pour `java.util.Random`, qui spécifie l'algorithme
et ses constantes mot pour mot. C'est ce qui rend ce générateur vérifiable
contre n'importe quelle JVM, sur n'importe quelle machine.

`scripts/gen_random_vectors.java` produit les vecteurs de référence depuis une
vraie JVM et les imprime **en C++**, pour qu'aucune transcription humaine ne
s'intercale entre l'oracle et le test. Les flottants passent en bits bruts :
« assez proche » est exactement le mode de défaillance qu'on cherche à exclure.

| Tirage | Résultat contre une vraie JVM |
|---|---|
| `nextInt`, `nextLong`, `nextBoolean` | **bit-identiques** |
| `nextInt(bound)` — puissance de deux et non | **bit-identiques** |
| `nextFloat`, `nextDouble` | **bit-identiques** |
| `nextGaussian` | **44/56 bit-identiques, 4 ulp au pire** |

### L'écart, mesuré et assumé

`nextGaussian` appelle `log`. Java impose `StrictMath.log`, c'est-à-dire les
sémantiques **fdlibm** ; la libm de la plateforme est libre de diverger sur les
derniers bits, et celle de macOS diverge.

L'écart est borné dans un test qui **épingle la mesure au lieu d'affirmer un
succès**, précisément pour que personne n'en déduise plus tard la garantie plus
forte. Ce qui est concerné : la dispersion de vitesse des objets lâchés et
assimilés, où 4 ulp sont invisibles. Ce qui ne l'est **pas** : terrain, butin et
block ticks, qui ne reposent que sur des tirages bit-exacts.

Combler l'écart demande une implémentation de `log` fdlibm — inscrite à la
roadmap. Tant qu'elle n'est pas là, l'affirmation honnête est celle du tableau.

### Trois détails que la spécification impose

- Le **XOR de brouillage** au seed (`^ 0x5DEECE66D`) n'est pas une optimisation :
  sans lui, `Random(0)` et `Random(1)` produisent des flux visiblement liés.
- `nextInt(bound)` a **deux chemins**. Une borne puissance de deux prend les bits
  **hauts** d'un tirage 31 bits ; les autres rejettent la queue inégale. Un
  simple modulo biaise vers le bas — assez peu pour ressembler à du bruit, assez
  pour décaler chaque veine de minerai du monde.
- `nextDouble` tire **26 bits puis 27**, dans cet ordre. Tirer 32 et 32 consomme
  autant d'état et donne un résultat faux.

---

## `XoroshiroRandomSource` (`ov_math`)

Algorithmes publiés : **Xoroshiro128++** (Blackman & Vigna) et le finaliseur
**variante 13 de Stafford** utilisé par SplitMix64. Le tirage borné suit la
méthode de **Lemire**.

### L'oracle est meilleur que prévu

Le JDK 17 embarque `Xoroshiro128PlusPlus` comme algorithme standard. J'avais
supposé qu'il brouillait la graine avec le **ratio d'or** ; les vecteurs ont dit
non — c'est le **ratio d'argent** `0x6A09E667F3BCC909`, c'est-à-dire exactement
ce que fait le jeu.

Conséquence : `RandomGeneratorFactory.of("Xoroshiro128PlusPlus")` valide non pas
seulement la fonction de transition, mais **tout le chemin**, upgrade de graine
64 → 128 bits compris. Une graine simple entre, le flux complet est comparé.
6 graines × 8 tirages, tous identiques.

C'est un cas où une hypothèse fausse a rendu la vérification plus forte : je
cherchais un oracle pour le cœur seulement, la mesure en a livré un pour
l'ensemble.

### Ce qui diffère du générateur legacy, et pourquoi ça compte

| | `LegacyRandomSource` | `XoroshiroRandomSource` |
|---|---|---|
| Borné | rejet par modulo, deux chemins | Lemire, multiplication haute |
| `nextFloat` | 24 bits d'un tirage 24 bits | 24 bits **hauts** d'un tirage 64 bits |
| `nextDouble` | 26 bits puis 27, deux tirages | 53 bits hauts, **un seul** tirage |

Les deux consomment des quantités d'état différentes. Réutiliser une méthode de
l'un dans l'autre ne produit aucune erreur — seulement un monde différent.

### L'état interdit

Xoroshiro possède un état absorbant : tout à zéro, dont il ne ressort jamais.
C'est l'upgrade de graine qui éloigne la graine `0` de cet état, et une graine
`0` est trop courante pour que ce soit théorique. Un test le vérifie.

---

## Les 66 registres codés en dur (`ov_registry`)

Source : `generated/reports/registries.json`, produit par le data generator
officiel. Rien n'est écrit à la main — une liste de 5067 IDs maintenue
manuellement serait fausse avant la fin de la version.

C'est le risque **R1** du plan. Le client vanilla 1.20.1 connaît ces IDs avant
de se connecter et ne les reçoit **jamais**. Une seule entrée dans le mauvais
ordre et il affiche la mauvaise entité, sans qu'aucune erreur n'apparaisse
nulle part.

| Mesure | Valeur |
|---|---|
| Registres | **66** |
| IDs vérifiés entrée par entrée, ordre compris | **5067** |
| Plus gros | `sound_event` 1474 · `item` 1255 · `block` 1003 |
| Registres à `first_id ≠ 0` | **1** — `minecraft:mob_effect`, 1-based |

`scripts/check_registry_parity.py` relit le `.ovpack` **depuis la description du
format**, pas depuis l'émetteur, puis compare au rapport. Un émetteur et un
lecteur qui partagent un bug se valideraient mutuellement ; un tiers qui relit
la spec ne le peut pas.

### Ce que le rapport contient exactement

Mesuré : `registries.json` liste **uniquement** les 66 registres codés en dur.
Les six registres dynamiques — `dimension_type`, `worldgen/biome`, `chat_type`,
`damage_type`, `trim_material`, `trim_pattern` — n'y figurent pas du tout ; ils
vivent dans le datapack, et leurs IDs sont envoyés au client en NBT pendant le
login, donc ils sont **les nôtres**.

Le script les exclut malgré tout. L'exclusion ne se déclenche jamais
aujourd'hui, et c'est écrit tel quel dans le code : c'est un garde-fou. Si une
version future les faisait apparaître dans le rapport, les épingler casserait
le premier datapack ajoutant un biome — et cela doit échouer là plutôt qu'en jeu.

### `mob_effect` est stocké, pas traité en cas particulier

`first_id` est un champ du format. Coder en dur « si le nom est mob_effect,
commencer à 1 » marcherait aujourd'hui et deviendrait faux silencieusement le
jour où Mojang ajoute une seconde exception. Un décalage de un ici signifie que
chaque potion du jeu applique l'effet voisin.

---

## Tags (`ov_registry`)

Source : les 413 fichiers `data/minecraft/tags/**/*.json` du datapack vanilla,
produits par le data generator.

Un tag référence d'autres tags avec un `#` en tête, donc c'est un **graphe**.
Il est aplati **une fois, à la compilation**, en listes d'IDs triées — le jeu ne
parcourt jamais le graphe. Résoudre à chaque appel serait l'implémentation
évidente et mettrait une traversée de graphe dans le chemin de cassage de bloc.
Aplatir tôt fait aussi échouer un cycle ou une référence pendante **au build**
plutôt qu'à un tick.

| Mesure | Valeur |
|---|---|
| Fichiers de tags | **413** |
| Tags résolus | **305**, sur 10 registres |
| Membres | **4255** |
| Fichiers reportés (registres dynamiques) | **108** |
| Plus gros | `minecraft:mineable/pickaxe`, 375 membres |

### Le piège du découpage de chemin

Le répertoire du registre **et** le nom du tag peuvent contenir des `/` :

```
tags/banner_pattern/pattern_item/x.json  → registre banner_pattern, tag pattern_item/x
tags/worldgen/biome/y.json               → registre worldgen/biome,  tag y
tags/blocks/mineable/axe.json            → registre block,           tag mineable/axe
```

Découper au premier `/` se trompe sur le deuxième cas ; découper à l'avant-dernier
se trompe sur les deux autres. La bonne règle : l'ensemble des registres est
connu, donc on prend **le plus long préfixe qui nomme un registre**. Ma première
version découpait naïvement et a échoué sur `banner_pattern/pattern_item` — le
message d'erreur du normaliseur l'a montré immédiatement, ce qui est exactement
ce qu'on attend d'un `die()` explicite plutôt que d'un `get()` silencieux.

### Répertoires au pluriel

Cinq répertoires sont des pluriels hérités et ne correspondent pas au nom du
registre : `blocks` → `block`, `items` → `item`, `entity_types` → `entity_type`,
`fluids` → `fluid`, `game_events` → `game_event`. Tous les autres sont déjà au
singulier. La table est **mesurée depuis la sortie du générateur**, pas devinée.

### Ce que le datapack vanilla n'exerce pas

Mesuré : aucun `replace: true`, aucune entrée sous forme d'objet, aucune entrée
`required: false`, aucune référence hors du namespace `minecraft`. Ces cas sont
implémentés quand même — le format les définit, et le premier datapack tiers
s'en servira. Ne coder que ce que Mojang expédie donnerait un chargeur qui
marche jusqu'au premier pack de la communauté.

### Vérification indépendante

`check_registry_parity.py` **re-résout les `#` depuis les fichiers JSON bruts
avec sa propre implémentation**, puis compare au binaire. Un émetteur et un
lecteur partageant un résolveur s'accordent quoi qu'il fasse ; seule une seconde
traversée des mêmes sources peut dire que l'aplatissement est juste.
305 tags, 4255 membres, identiques.

---

## Section de chunk et stockage de la lumière (`ov_world`)

### L'ordre des indices

**YZX** — `(y * 16 + z) * 16 + x` — parce que c'est ce qu'utilisent le format de
fichier *et* le format réseau. XZY fonctionnerait parfaitement jusqu'au premier
chunk écrit, et produirait ensuite un monde qui est sa propre transposition :
chaque structure miroitée le long d'une diagonale, sans qu'aucune erreur
n'apparaisse.

### Les tableaux de lumière nullables

Mesuré sur un chunk réel : **7 sections sur 24** portaient un tableau
`BlockLight`, **2 sur 24** un `SkyLight`. Au-dessus du terrain le ciel est
uniformément plein et la lumière de bloc uniformément nulle ; sous la roche les
deux sont nulles. Un tableau entièrement uniforme ne stocke donc **rien** et se
souvient de la valeur ; il ne se matérialise que si quelque chose varie.

Le piège de la matérialisation : allouer un tampon rempli de zéros pour un
tableau uniformément **plein** plonge la section dans le noir, et seulement les
sections qu'on édite. On remplit avec la valeur implicite, pas avec zéro.

### L'ordre des demi-octets, tranché par la mesure

2048 octets portent 4096 cellules, et le format dit dans quelle moitié d'octet
vit la cellule paire. Le lire à l'envers échange chaque paire de voisins le long
de x. **Aucun aller-retour ne le détecte** — les octets ressortent identiques —
et le résultat est un monde éclairé en fin damier, qui ressemble à un bug de
shader et non de stockage.

Un éclairage réel varie doucement, donc la bonne lecture est celle qui donne le
plus petit écart entre cellules voisines. Mesuré par `ov-inspect chunk` sur les
tableaux que le jeu a lui-même écrits :

| Lecture | Écart moyen entre voisins | Gagne sur |
|---|---|---|
| cellule paire = demi-octet **bas** | **0,149** | **141 799 / 144 974** (97,8 %) |
| cellule paire = demi-octet haut | 0,230 | 3 175 |

Le contrôle est permanent dans `ov-inspect chunk`, pas une mesure jetable.

### Le compte de blocs non-air

C'est le champ « Block Count » du paquet de chunk, envoyé à chaque expédition.
Il est maintenu **incrémentalement** : recompter 4096 entrées par section à
chaque envoi dominerait le paquet. Un test compare le compte incrémental à un
recomptage complet, parce qu'un compteur incrémental est précisément le genre
qui dérive.

Les **trois** blocs d'air comptent comme air : `air` (état 0), `void_air`
(12817) et `cave_air` (12818), un seul état chacun — mesuré, pas supposé. Un
client à qui l'on dit qu'une grotte est pleine la rend pleine. Les identifiants
sont résolus depuis le registre, jamais écrits en dur.

---

## Heightmaps (`ov_world`)

Sémantique établie par **recalcul depuis les blocs**, pas par lecture de wiki.
`ov-inspect chunk` reconstruit `WORLD_SURFACE` à partir des palettes — **par nom**,
donc indépendamment de la version d'IDs du monde — et compare à ce que le jeu a
écrit.

| Mesure | Valeur |
|---|---|
| Colonnes recalculées | **4 577 024** |
| Identiques | **4 577 023** (99,99998 %) |
| À un près | **0** |

Zéro écart d'un bloc, ce qui est le point : la question était de savoir si la
valeur stockée est le sommet ou le premier espace libre au-dessus. C'est le
**premier espace libre** : `stored == top + 1 − minY`. Une colonne vide stocke 0.
L'autre convention fait tomber la pluie un bloc sous le sol.

### L'origine n'est pas la liste des sections

Deux chunks de ce monde listent **25 sections à partir de -5**, pas 24 à partir
de -4 : vanilla écrit une section supplémentaire sous le monde pour la lumière.
Déduire l'origine de la liste des sections décale **toutes** les colonnes de ces
chunks de seize blocs. L'origine est le plancher de la **dimension**, point.

C'est exactement le genre de fait qu'aucune lecture de spécification ne donne et
qu'une mesure livre en une passe.

### Le packing

La règle des palettes, encore : **9 bits par entrée, sept par long, aucune
entrée à cheval**. 37 longs pour 256 colonnes, pas 36. Neuf bits parce qu'un
monde de 384 blocs stocke les valeurs 0 à 384 — **385** possibilités, pas 384.
Dimensionner pour 384 rend le sommet du monde irreprésentable, ce qui reste
invisible jusqu'à ce que quelqu'un y construise.

### L'unique écart, expliqué et non arrondi

Une colonne sur 4,5 millions diverge de 3 blocs. Le bloc en cause est
`create:crushing_wheel_controller` — un bloc **moddé**. C'est le mod qui décide
si son bloc compte comme air ; une vérification par nom sur la liste vanilla ne
peut pas le savoir. Ce n'est pas un défaut de l'implémentation, c'est la limite
connue de la méthode de contrôle, et elle est écrite ici plutôt qu'arrondie.

---

## Le conteneur `Chunk` (`ov_world`)

### Décalage arithmétique, pas division

`section_index = (y >> 4) - min_section`. Une division tronque vers zéro, ce qui
placerait `y = -1` et `y = 0` dans la **même** section pendant que `y = -16` et
`y = -17` se retrouveraient séparés — une couture de seize blocs à l'origine du
monde. La majeure partie de l'Overworld est à `y` négatif, donc ce n'est pas un
cas limite.

### La forme du monde est un paramètre

L'Overworld fait 384 blocs depuis -64 ; le Nether et l'End font 256 depuis 0 ;
une dimension de datapack fait ce qu'elle veut. Coder -64 en dur fonctionne
jusqu'au premier chunk du Nether.

### `WORLD_SURFACE` maintenu, et vérifié comme tel

Poser un bloc, c'est une comparaison. **Casser** celui qui était la surface,
c'est un balayage vers le bas. C'est le sens coûteux, et celui qu'une
implémentation oublie : le heightmap continue alors de pointer un bloc qui
n'existe plus, et la pluie tombe sur du vide.

Vérification sur données réelles, via `ov-inspect chunk` : les blocs d'un chunk
du jeu sont **rejoués un `set_block` à la fois** dans un vrai `Chunk`, puis le
heightmap maintenu est comparé au balayage complet des mêmes blocs.

| Mesure | Valeur |
|---|---|
| Colonnes rejouées | **4 577 024** |
| Maintenu == balayé | **4 577 024** (100 %) |

Deux comparaisons distinctes, à ne pas confondre : celle-ci oppose le chemin
incrémental au balayage complet ; la ligne `WORLD_SURFACE` d'`ov-inspect` oppose
le balayage au fichier.

### Ce qui est bloqué, et pourquoi

`MOTION_BLOCKING`, `OCEAN_FLOOR`, le moteur de lumière et les collisions ont tous
besoin d'une **table de flags par état de bloc** — `blocksMotion`, émission
lumineuse, opacité, formes de collision. Les rapports officiels ne la portent
pas : en vanilla ce sont du code Java, pas des données.

**Piste écartée : `PrismarineJS/minecraft-data`.** Licence MIT, explicitement
autorisée par le `CLAUDE.md`, mais son `blocks.json` porte `emitLight` **par
bloc**, alors que l'émission dépend de l'**état** — mesuré :
`respawn_anchor` a 5 niveaux selon `charges`, `candle` 4 selon `candles`,
`cave_vines` selon `berries`, `redstone_ore` selon `lit`. L'utiliser coulerait
une approximation dans les fondations du moteur de lumière, exactement le genre
d'erreur qui donne un monde qui a l'air juste et ne l'est pas.

La source reste **à décider**. Elle doit être par état.

---

## L'état Play et le paquet de chunk (`ov_protocol`)

### Méthode : capturer, pas se souvenir

Les IDs de paquets ne se devinent pas et changent d'une version à l'autre. Trois
sources ont été confrontées :

| Source | Verdict |
|---|---|
| Page archivée du wiki, via résumé automatique | **fausse sur toute la ligne** — « Login (play) 0x01 », champs manquants |
| `PrismarineJS/minecraft-data` (MIT) | correcte |
| **Capture d'un vrai serveur 1.20.1** | correcte, et arbitre |

Le vrai `server.jar` a été lancé localement et un client écrit depuis la spec
s'y est connecté pour enregistrer ce qui arrive réellement. C'est ce qui a établi
`Login (play) = 0x28`, `Chunk Data = 0x24`, `Set Center Chunk = 0x4E`,
`Synchronize Position = 0x3C`.

Ça a aussi tranché un doute que j'avais : `portalCooldown` **existe bien** en 763.
Je soupçonnais un champ appartenant à 1.20.2 ; décoder la queue du vrai paquet a
montré l'octet. Mon doute était infondé, et mesuré plutôt que débattu.

### Comparaison directe avec vanilla

Les deux serveurs tournant côte à côte, un même décodeur a lu les deux. Les
charges de section sont **identiques** : mêmes largeurs de bits, mêmes tailles de
palette, mêmes nombres de longs, mêmes octets aux mêmes offsets.

Reste **non élucidé** : la charge de vanilla fait 23 octets de plus que la nôtre
et sa queue contient des sections à biome 0. Le client accepte la nôtre, donc ce
n'est pas bloquant, mais c'est écrit ici plutôt qu'arrondi.

### Trois pièges que seule la comparaison a révélés

**Les biomes n'ont pas la plage de bits des blocs.** Une palette de biomes fait
1 à 3 bits ; **4 signifie direct**. Notre `PalettedContainer` plancherait à 4
comme pour les blocs, et le client aurait lu des indices de palette comme des IDs
de registre — chaque biome du chunk devenu un autre. Corrigé : la plage fait
partie de l'identité du conteneur.

**« Vide » veut dire tout à zéro, pas « non stocké ».** Une section uniformément
*éclairée* doit quand même être écrite ; seule une section uniformément noire
peut être omise. L'inverse rend le monde entièrement noir, sans erreur nulle part.

**La lumière se pose par bloc, pas par section.** La surface d'un superflat est à
y = -61, dans la section allant de -64 à -49. Une règle « section entièrement
au-dessus de la surface » laisse noire précisément la section où le joueur se
tient : le ciel est éclairé, le sol non. Symptôme observé en jeu, puis corrigé.

### Le codec de registres

Six registres ne sont pas codés en dur dans le client et lui sont envoyés en NBT
au login. `tools/ov_datagen/codec.py` les construit depuis le datapack vanilla
local avec un **schéma typé champ par champ** : JSON n'a qu'un type numérique,
NBT en a six, et le décodeur du client est strict. `temperature` est un `float`,
`coordinate_scale` un `double`, chaque booléen un `byte`. Une conversion qui
déduit le type de la valeur produit un codec plausible, mal typé, et coupe la
connexion sans message utile.

Omettre un champ est sûr — les codecs du client ignorent ce qu'ils ne connaissent
pas. En envoyer un du mauvais type ne l'est pas.

Le serveur ne comprend jamais ce blob : il le recopie. Et il y **relit** l'id de
`minecraft:plains` plutôt que de le coder en dur, parce que le client n'apprend
les IDs de biomes que de ce codec-là.

### Casser et poser

`Player Action` (0x1D) et `Use Item On` (0x31), avec `Block Update` (0x0A) et
`Acknowledge Block Change` (0x06) en retour.

Trois détails qui décident du résultat :

- **Deux statuts de cassage comptent**, pas un. `0` est « commencé à creuser »,
  ce qui en créatif signifie que le bloc a déjà disparu côté client ; `2` est
  « fini de creuser » en survie. N'en traiter qu'un rend l'autre mode inopérant.
- **La position cliquée n'est pas celle où le bloc va.** La face indique le côté,
  et le nouveau bloc atterrit un pas plus loin. Poser à la position cliquée
  remplace ce que le joueur visait.
- **L'accusé de réception n'est pas optionnel.** Le client prédit le changement
  et l'affiche ; sans `Acknowledge`, il attend puis annule sa prédiction — ce qui
  ressemble exactement à un serveur qui ignore le joueur.

Le pont entre l'item tenu et le bloc posé est le **nom** : l'item
`minecraft:stone` pose le bloc `minecraft:stone`. Items et blocs sont deux
registres aux IDs distincts, et la plupart des items non-blocs n'ont simplement
pas de bloc homonyme — ce qui est exactement le test.

### La lumière se dérive du heightmap, pas d'une constante

Bug réel, trouvé en jouant : creuser un trou, se déconnecter, revenir — le trou
restait noir.

La génération cuisait la lumière du ciel à partir d'une hauteur de surface
**fixe**. Casser un bloc met à jour `WORLD_SURFACE`, mais pas le tableau de
lumière déjà stocké. Pendant la session tout allait bien parce que **le client
éclaire lui-même les blocs qu'il modifie** ; l'erreur ne se voit qu'au
rechargement, quand le serveur réenvoie sa version.

Deux conséquences de conception :

- La lumière est calculée **depuis le heightmap**, par une fonction unique
  qu'utilisent la génération *et* l'édition. Deux chemins qui calculent la
  lumière séparément s'accordent jusqu'au jour où quelqu'un creuse.
- La colonne est recalculée à chaque modification de bloc, parce que c'est
  précisément là que `WORLD_SURFACE` bouge.

Limites assumées : lumière **directe** seulement, sans propagation horizontale —
un puits est correctement éclairé, le dessous d'un surplomb ne l'est pas. Et
« non-air » tient lieu d'« opaque », ce qui est juste pour tout bloc d'un monde
plat et faux pour le verre. Les deux disparaissent avec le moteur de lumière et
la table de flags.

### La position du joueur, et son corollaire

Retenir la position ne suffit pas : réapparaître à mille blocs de l'origine avec
des chunks centrés sur le spawn fait tomber le joueur dans un monde vide. Les
deux vont ensemble — **la position restaurée détermine le centre de l'envoi**.

Elle est enregistrée à **chaque** mise à jour de mouvement, pas à la déconnexion :
un client tué n'envoie jamais de fermeture propre, et perdre la position d'une
session qui a planté est précisément le cas qu'on remarque.

En mémoire seulement. Un redémarrage du serveur l'oublie, exactement comme il
oublie les chunks — et n'en persister qu'une des deux replacerait quelqu'un à
l'intérieur d'un bloc qui n'existe plus.

### Le flood fill, et la graine qui manquait

La lumière directe seule laisse une **tache noire à bord net** sous le moindre
toit d'un bloc — mesuré : 0 sous le bloc, 15 dans la colonne voisine au même y.
C'est la première chose qu'un joueur remarque.

Le flood fill corrige ça : la lumière se propage dans les six directions en
perdant un niveau par pas. Sous un toit 3×3 : **15** dehors, **14** au pourtour,
**13** au centre.

La première version donnait **13 au lieu de 14** juste sous un bloc isolé. Cause :
je n'ensemençais que la cellule la plus basse de chaque colonne, en supposant que
tout au-dessus est déjà entouré de lumière. C'est faux exactement là où deux
colonnes ont des hauteurs différentes — c'est-à-dire précisément là où une
construction projette son ombre. Toutes les cellules directement éclairées de la
bande sont désormais des sources.

Coût mesuré : connexion complète (289 chunks générés, éclairés et envoyés) en
**0,75 s en build debug**.

Deux limites, écrites plutôt que sous-entendues : la propagation **s'arrête au
bord du chunk**, donc une construction à cheval sur une frontière ne projette pas
d'ombre chez le voisin ; et « non-air » tient lieu d'« opaque », donc du verre
ferait de l'ombre.

### Streaming de chunks, et le tick qu'il bloquait

Envoyer seulement la **différence** : un pas d'un chunk échange 17 chunks, un
saut de cinq en diagonale 157, et le total détenu reste à 289 centré sur le
joueur. Réémettre le carré entier à chaque franchissement ferait 289 chunks à
quelques pas d'intervalle, et le client passerait son temps à reconstruire des
maillages qu'il avait déjà.

**Le défaut que ça a révélé.** Le gestionnaire de paquets tient `players_mutex`
pendant toute la génération et l'envoi — des centaines de millisecondes pour un
grand saut — et le thread de tick attendait dessus pour ses keep-alive. Mesuré :
un événement « can't keep up » exactement pendant un saut de cinq chunks.

Le tick prend désormais ce verrou en `try_lock`. Un keep-alive différé d'un tick
est sans conséquence ; un tick bloqué derrière le réseau ne l'est pas. Deux
pièges évités au passage : sauter la passe avec `continue` aurait franchi le
`sleep` en fin de boucle et fait tourner le thread de tick à plein régime
précisément quand le serveur est chargé, et parcourir la map sans le verrou
serait une course à part entière.

Ce n'est **pas** la règle « un seul écrivain » du projet — c'est du verrouillage
honnête en attendant que le monde vive sur le thread de tick, ce qui est le
travail `ov_sim`.

Limite connue : le cache de chunks ne se vide jamais. Marcher longtemps le fait
croître sans borne. Une éviction demande de savoir quels chunks sont encore
référencés, c'est-à-dire le système de tickets.

---

## Écriture des fichiers région (`ov_nbt`)

Lire une région prouve qu'on sait en analyser une ; **l'écrire** est la moitié
dont dépend une sauvegarde, et celle dont les erreurs coûtent un monde.

Deux règles que le format n'énonce pas :

- **Le champ de longueur compte l'octet de compression.** N octets compressés
  s'écrivent `N+1`. Être un court tronque le dernier octet de *chaque* chunk du
  fichier, ce qui se lit comme un flux corrompu et non comme un flux court.
- **Un chunk commence sur une frontière de secteur.** La table d'offsets adresse
  des secteurs, pas des octets : il n'existe aucun moyen d'exprimer « commence
  au milieu de l'un d'eux ». Le remplissage n'est donc pas facultatif.

L'écriture est **entière puis renommée**. Réécrire un chunk sur place serait plus
rapide et laisserait, en cas de crash au milieu, une région dont l'en-tête pointe
des secteurs que le nouveau contenu a déplacés — chaque chunk du fichier se
lirait alors comme du bruit.

Les chunks que personne n'a touchés sont **reportés encore compressés** : les
ré-encoder risquerait de les modifier, alors que tout l'intérêt est qu'ils ne le
soient pas.

### Vérification

`ov-inspect region --verify` reconstruit le fichier depuis ses propres chunks et
compare. Sur le monde réel : **17 879 / 17 879 chunks octet-identiques**.

---

## Sérialisation des chunks (`ov_world`)

Le format disque est **basé sur les noms**, et c'est toute la raison pour
laquelle une sauvegarde peut être partagée entre implémentations. Sur le fil, une
entrée de palette est un identifiant numérique d'état ; sur disque c'est
`{Name: "minecraft:oak_stairs", Properties: {facing: "north", …}}`.

Écrire les ids du réseau sur disque produirait un fichier qui se recharge
parfaitement **ici** et ne veut rien dire dans Minecraft — et dont la version qui
l'a écrit serait la seule à pouvoir le décoder. La conversion passe donc par le
registre dans les deux sens, et un état dont le registre ignore le nom est
**refusé plutôt que deviné**.

Trois décisions à leurs raisons :

- **Le disque n'a pas de forme « directe ».** Un conteneur direct en mémoire est
  reconverti en palette à l'écriture : le format nomme, et 24 135 noms seraient
  absurdes.
- **La lumière est écrite, pas recalculée au chargement.** Il n'y a pas de moteur
  de lumière sur le chemin de lecture ; recalculer différerait silencieusement de
  ce que le client a vu en dernier.
- **Les heightmaps sont recalculés, pas lus.** C'est de la donnée dérivée, un
  heightmap stocké peut être périmé — mesuré sur un monde réel : une colonne sur
  4,5 millions l'était — et le recalcul coûte un balayage que le chargement paie
  déjà.

Les noms de biomes sont **passés en paramètre**. Les biomes vivent dans un
registre que le serveur *envoie* ; leurs identifiants appartiennent à qui a
construit le codec, et ce module n'a pas à les connaître.

### Le monde sur disque

`run/world/region/` — gitignoré, une sauvegarde appartient au joueur, pas au
dépôt. Le disque passe **avant** le générateur : l'ordre inverse fonctionne
jusqu'au premier rechargement puis jette silencieusement tout ce qui a été
construit.

Vérifié de bout en bout : construire une tour et creuser un trou, arrêter le
serveur, le relancer — blocs et lumière identiques, et le fichier produit passe
le contrôle d'aller-retour d'`ov-inspect`.

Autosave toutes les 30 secondes, en plus de l'arrêt propre : un serveur tué n'a
jamais d'arrêt propre, et perdre une heure de construction sur un crash est la
panne dont on se souvient. Mesuré : 4 chunks sur 3 régions, zéro surcharge de tick.

---

## `level.dat`, et l'aller-retour croisé avec Minecraft

Sans `level.dat`, Minecraft ne voit **aucun monde**, quels que soient les
fichiers région présents : la liste des sauvegardes se construit à partir de ce
fichier. Il est en **gzip**, alors que les chunks à l'intérieur d'une région sont
en zlib — rien ne signale la différence, et un `level.dat` écrit avec le mauvais
conteneur se lit comme un fichier corrompu.

Il déclare aussi le **générateur**, ce qui compte plus qu'il n'y paraît : un monde
dont les régions ont été faites à plat mais dont le `level.dat` annonce « noise »
fera pousser du terrain normal dès que le joueur dépassera ce qui était
sauvegardé, avec un mur visible à la jonction.

### Le serveur vanilla comme oracle

Premier essai : le vrai serveur a répondu **`key missing: DragonFight`**. Une
erreur qui nomme exactement le champ manquant — bien meilleure que la plupart, et
un bon rappel que le jeu est l'oracle de test le moins cher disponible. Ajouté,
puis **zéro erreur**.

### Les deux sens, prouvés

| Sens | Preuve |
|---|---|
| Notre sauvegarde → Minecraft | Le serveur 1.20.1 charge notre monde sans une erreur et **sert notre colonne de 8 pierres** dans ses propres paquets de chunk |
| Minecraft → notre sauvegarde | Le fichier région passe de 1 à 529 chunks — vanilla l'a réécrit — et notre serveur le relit avec les bons blocs |

La seconde ligne est établie sans ambiguïté par les champs présents dans le
chunk : `PostProcessing`, `isLightOn`, `structures`, `LastUpdate: 828` et
**25 sections** (vanilla en écrit une de plus pour la lumière). Aucun n'est écrit
par nous. Le chunk relu est donc bien celui de vanilla.

Cette section supplémentaire est ignorée au chargement : elle tombe hors de la
forme du monde et n'a nulle part où aller.

### Un test qui a échoué de mon fait

J'ai tenté de faire poser des blocs de diamant **par** le serveur vanilla via un
client scripté, pour un test plus direct. La pose n'a jamais abouti : le serveur
valide quelque chose que mon client ne satisfait pas. C'est une limite de mon
harnais, pas du code, et la preuve par les champs ci-dessus est plus forte de
toute façon — mais elle est notée plutôt que passée sous silence.

### La lumière traverse les frontières de chunk

La version limitée à un chunk arrêtait la propagation au bord : une construction
adossée à une frontière ne projetait aucune ombre chez le voisin, et la couture
apparaissait comme une ligne droite de sol mal éclairé. La lumière ne respecte
pas les frontières de chunk, le remplissage ne le peut pas non plus.

Le remplissage travaille désormais sur le **voisinage 3×3**, en coordonnées
monde — tout l'intérêt est qu'il ignore où sont les bords.

Seuls les chunks **déjà chargés** participent. Aller chercher les voisins
provoquerait une cascade : générer un chunk générerait ses voisins, puis les
leurs. Une construction contre le bord de la zone chargée présente donc encore
une couture — mais ce bord suit le joueur et reste hors de vue, alors qu'une
frontière de chunk au milieu d'une base, non.

La correction va **en mémoire, sans réémission**. Le client éclaire lui-même ses
propres modifications, donc la couture reste invisible jusqu'au prochain
chargement du chunk — c'est-à-dire précisément quand la valeur stockée est celle
qui compte.

#### Le test qui discrimine vraiment

Un toit à cheval sur une frontière ne prouve rien : chaque chunk a ses propres
sources de chaque côté. Il faut un cas où la lumière **ne peut venir que d'en
face** — un toit couvrant toute la largeur d'un chunk près du bord.

Mesuré : la case en x=16 (chunk 1) vaut **13**. Sa seule voisine éclairée est
x=15, à 14, **dans le chunk 0**. Depuis le chunk 1 seul, la source la plus proche
est à trois cases et donnerait 12. La valeur 13 ne peut donc venir que de l'autre
côté de la frontière.

---

## Orientation des blocs à la pose

Vanilla décide de l'état posé dans le **code Java de chaque bloc** : il n'existe
aucun fichier de données à lire. Ce qui *est* de la donnée — et que le registre
fournit — c'est la liste des propriétés d'un bloc. Les règles ne s'appliquent donc
que là où la propriété existe.

| Propriété | Règle |
|---|---|
| `axis` | déduite de la face cliquée — une bûche posée au sol est verticale, contre un mur elle est couchée |
| `facing` | déduite du lacet du joueur, **mais dans quel sens dépend du bloc** |
| `half` | `top` si le clic était sous un bloc, ou sur la moitié haute d'un côté |
| `type` | même règle, pour les dalles |

### La convention de `facing` a été mesurée, pas déduite

Vanilla n'est pas uniforme, et **le partage ne suit pas les familles**. Dix-huit
blocs ont été posés sur un vrai serveur 1.20.1, joueur orienté au sud, puis l'état
résultant relu.

| Résultat | Blocs |
|---|---|
| **Même direction** que le joueur | `stairs`, `fence_gate`, `door`, **`observer`** |
| **Opposée** | `trapdoor`, `furnace`, `chest`, `dispenser`, `lectern`, `carved_pumpkin`, `repeater`, `comparator`, `end_portal_frame`, `loom`, `stonecutter`, `beehive` |
| Autre chose | `anvil` — revenu tourné d'un quart de tour |

Deux résultats montrent pourquoi deviner ne marche pas. **`observer` suit la
direction du joueur**, ce qui se lit à l'envers de son comportement. **`trapdoor`
est opposée**, alors que portes et portillons — sa famille évidente — ne le sont
pas.

La majorité est « opposée », c'est donc le défaut. Les exceptions mesurées sont
étendues aux variantes par suffixe, celles-ci partageant la même classe Java.
`anvil`, qui échappe aux deux règles, est laissé à son état par défaut plutôt que
tourné de travers avec assurance.

Vérification : notre serveur reproduit **11/11** des orientations mesurées.

**Ce qui reste faux** : les 227 blocs à `facing` horizontal ne sont pas tous
mesurés, seulement un échantillon. Un bloc non couvert prend la convention
majoritaire et peut donc être orienté à l'envers — visible, sans gravité, et
corrigible dès que le harnais de parité couvre l'ensemble.

---

## Entités joueur

Trois paquets suffisent à faire exister un joueur chez les autres, et l'ordre
compte : **`Player Info Update` d'abord**. Le client construit l'entité à partir
de cette liste ; un `Spawn Player` pour un UUID dont il n'a jamais entendu parler
donne une entité sans nom, sans skin, ou pas d'entité du tout.

Le drapeau `update_listed` n'est pas cosmétique non plus : une entrée non listée
n'est pas rendue. Envoyer `add_player` seul ne suffit pas.

Les angles voyagent sur **un octet, 256 pas par tour**. La conversion doit
**boucler et non saturer** : un lacet de 350° et un de -10° désignent la même
direction, et saturer épinglerait un joueur regardant légèrement à l'ouest
plein sud.

La **tête tourne indépendamment du corps**. Un client à qui l'on n'envoie que la
rotation du corps affiche un joueur qui regarde éternellement droit devant.

### Absolu plutôt que relatif

Vanilla préfère les déplacements relatifs pour les petits pas — six octets contre
vingt-huit. Nous envoyons des téléportations absolues : un flux relatif qui perd
ou réordonne un paquet laisse l'entité **définitivement décalée**, et rien dans le
protocole ne permet de s'en apercevoir. L'optimisation viendra avec de quoi la
vérifier.

### Ne retransmettre que ce qui change

Un client envoie une mise à jour de position **à chaque tick**, qu'il ait bougé ou
non. Les relayer toutes représente l'essentiel du trafic d'un serveur peuplé.
Mesuré : 20 paquets d'un joueur immobile produisent **0** retransmission.

---

## Mesurer l'émission lumineuse : ce qui marche, ce qui ne marche pas

La table de flags par état reste le blocage nommé du projet. `emitLight` n'est
pas disponible par état dans les sources écartées, et le jeu lui-même est le seul
oracle exact. Une tentative de harnais a donc été montée.

**La technique.** Le serveur vanilla lit ses commandes sur l'entrée standard, et
`/setblock` place **n'importe quel état** sans contrainte de pose — contrairement
à un client, qui doit respecter portée, support et mode de jeu. On remplit une
zone de bedrock, on place les états à sonder espacés, on laisse la lumière se
propager, on sauvegarde, puis on relit `BlockLight` à chaque cellule.

**Ce qui est établi.** Plusieurs valeurs sortent exactement justes :

| État | Mesuré | Attendu |
|---|---|---|
| `glowstone` | 15 | 15 |
| `stone` | 0 | 0 |
| `redstone_ore[lit=true]` | 9 | 9 |
| `respawn_anchor[charges=0..4]` | 0, 3, 7, 11, 15 | idem |
| `furnace[lit=true]` / `[lit=false]` | 13 / 0 | idem |
| `candle[candles=2,3,4][lit=true]` | 6, 9, 12 | idem |

**Ce qui ne l'est pas.** D'autres états restent contaminés : `candles=1` donne 8
au lieu de 3, et les états éteints ne retombent pas à 0. L'isolation par bedrock
devrait pourtant rendre toute propagation impossible, et élargir l'espacement de
2 à 4 n'a pas suffi. La cause n'est **pas** établie — probablement la suppression
de lumière, qui est asynchrone chez vanilla, ou une zone partiellement chargée.

**Conséquence assumée** : aucune table n'est livrée. Publier une table dont une
partie est fausse serait exactement l'erreur refusée plus tôt avec
`minecraft-data` — une approximation coulée dans les fondations. Le harnais est
gardé au stade d'expérience documentée jusqu'à ce que ses mesures soient
reproductibles à 100 %.

### `ov-inspect light`

Ce qui *est* livré : la lecture, par le lecteur NBT vérifié du projet. La commande
prend des lignes `x y z` sur l'entrée standard et rend le bloc et sa lumière
stockée.

J'ai écrit trois parseurs NBT jetables en Python avant d'admettre qu'ils étaient
la partie la moins fiable de l'expérience — l'un d'eux a produit `glowstone = 0`
et `stone = 3`, des valeurs assez plausibles pour être crues. Le lecteur du
projet, lui, est vérifié octet à octet sur 17 879 chunks.

---

## Block entities

Un block entity porte ce qu'un état de bloc ne peut pas contenir : le texte d'un
panneau, le contenu d'un coffre. Ils sont rangés **à côté** des sections : il y en
a une poignée par chunk contre 98 304 blocs, et réserver de la place dans chaque
cellule coûterait plus que le monde lui-même.

### Deux conventions à ne pas confondre

| Destination | Coordonnées |
|---|---|
| Disque | **monde**, en trois champs `x`, `y`, `z` |
| Fil | **locales au chunk**, x et z tassés dans un octet de 4+4 bits |

Écrire l'une là où l'autre est attendue place chaque panneau et chaque coffre
dans un autre chunk, silencieusement.

### Le panneau de 1.20

Les deux faces existent : un panneau écrit seulement au recto et enregistré sans
`back_text` est refusé par le client. Et chaque ligne est un **composant de
texte**, pas une chaîne : `hello` n'est pas valide là où `{"text":"hello"}` l'est.

Un block entity ne survit jamais à son bloc : tout changement de bloc le supprime.
Un block entity orphelin est un coffre qu'on ne peut ni ouvrir ni enlever.

### Deux défauts trouvés en testant

**Un interblocage.** Le gestionnaire de paquets *Play* tient déjà `players_mutex`
pendant tout son corps ; le reprendre pour diffuser la mise à jour du panneau
bloquait la connexion — `std::mutex` n'est pas récursif. De l'extérieur, ça
ressemblait exactement à un paquet ignoré, et le serveur ne sauvegardait plus.

**Un type d'entité à zéro.** Le disque nomme les block entities, le protocole les
numérote. Un chunk rechargé repartait donc avec un `type_id` de 0 — et zéro est
un identifiant **valide**, appartenant à un autre type. L'omission ne cassait
rien de visible : elle re-typait tous les panneaux d'un chunk rechargé. Mon
client de test ne validait pas ce champ ; il a fallu aller le lire pour le voir.

Vérifié de bout en bout : poser, écrire (guillemet compris), sauvegarder, arrêter,
redémarrer — le texte est sur disque, il repart dans le paquet de chunk, et le
`type_id` vaut bien 7.

---

## Opacité à la lumière du ciel : la première colonne mesurée de la table de flags

Signalé en jouant : poser un panneau, se déconnecter, revenir — une tache noire.
Le moteur de lumière traitait **tout ce qui n'est pas de l'air comme opaque**,
donc un panneau projetait une ombre pleine. Invisible pendant la session, puisque
le client éclaire lui-même ses propres poses.

### Le montage, et l'erreur silencieuse qui l'a d'abord ruiné

Un plateau de bedrock percé de **puits d'un bloc de large**, le bloc testé posé
au sommet de chaque puits, la lumière du ciel lue au fond. La lumière ne peut
entrer que par le bloc testé.

Premier essai : tous les blocs opaques donnaient **14**, pas 0. Cause :
`/fill` plafonne à **32 768 blocs** et mon plateau en demandait 202 800 — la
commande a été **rejetée sans que rien ne s'arrête**. Il n'y avait donc aucun
plateau ; les blocs flottaient en plein ciel et le 14 était la lumière qui les
contournait. Refait couche par couche (16 384 blocs chacune), le signal est net.

### Le résultat, sur les 1003 blocs

| Classe | Nombre | Exemples |
|---|---|---|
| **Transparente** — le ciel passe intact | 483 | air, panneau, torche, verre, barrière |
| **Opaque** — le ciel s'arrête | 443 | pierre, dalle, escalier, glowstone |
| **Atténuante** — le ciel faiblit | 77 | eau, feuillage, glace |

Un bloc jamais mesuré est traité comme **opaque** : se tromper vers une pièce
sombre vaut mieux que vers un monde sans ombres.

### Une correction que cette mesure a rendue possible

La lumière directe s'arrêtait au premier bloc **non-air**, c'est-à-dire à
`WORLD_SURFACE`. Elle descend maintenant jusqu'au premier bloc **opaque** — ce
qui est la vraie règle, et ce qui ne pouvait pas s'écrire sans connaître
l'opacité. Un panneau, une torche ou une vitre reçoivent désormais 15 et non 14.

### Ce qui reste

C'est mesuré **par bloc**, pas par état : la variation selon l'état n'a pas été
sondée. Les blocs atténuants sont traités comme transparents — se tromper d'un
niveau ou deux vaut mieux que de quinze. Et les **formes de collision**, qui
bloquent le mouvement autoritatif, restent non mesurées.

### Vérification sur un vrai monde 1.20.1

Les vérifications d'aller-retour précédentes portaient sur un monde **1.21
moddé** — la seule sauvegarde réelle disponible. Un monde généré par le jeu en
**1.20.1**, la version cible, permet enfin de fermer la boucle sur la bonne
version.

| Mesure | Résultat |
|---|---|
| `level.dat` (gzip, DataVersion 3465) | aller-retour **octet-identique** |
| 4 fichiers région, 2601 chunks | **2601/2601** identiques en lecture |
| Les mêmes, réécrits par notre écrivain | **2601/2601** identiques |
| Blocs servis par notre serveur vs le fichier | **180/180** positions, 13 types de blocs |

Les 13 types rencontrés — air, andésite, bedrock, deepslate, minerai d'or du
deepslate, diorite, terre, granite, herbe, gravier, pierre, tuff, eau — couvrent
du terrain généré réel, pas un superflat.

Observation de performance : charger et envoyer les 289 chunks d'une connexion
prend **environ 10 secondes en build debug** sur un monde réel, contre 0,75 s sur
un superflat généré. La différence est le décodage des palettes et le calcul de
lumière sur des chunks pleins.

### Coffres

La numérotation des slots dans une fenêtre n'est pas l'identité : **0 à 26** sont
le coffre, **27 à 53** l'inventaire principal du joueur, **54 à 62** sa barre
d'action — alors que les slots *du joueur* sont numérotés 9 à 44. Se tromper de
correspondance déplace des objets entre les deux mauvais endroits.

Vanilla stocke le contenu comme une liste de `{Slot, id, Count}` en **omettant
les slots vides**. La liste n'est donc pas indexée par slot et sa longueur ne dit
rien de la taille du coffre.

Le NBT d'un objet est **conservé tel quel**, en octets bruts. Le serveur ne
comprend ni enchantements ni noms personnalisés, et ré-encoder une balise qu'on
ne comprend pas est la façon dont on perd des données. La laisser passer intacte
ne le peut pas.

`minecraft:generic_9x3` est lu dans le registre `minecraft:menu`, pas codé en
dur : un coffre 9×3 n'a pas le même numéro qu'un 9×6, et se tromper ouvre une
fenêtre de la mauvaise taille sur les bonnes données.

**Ce qui est traité et ce qui ne l'est pas.** Les clics gauche et droit (mode 0)
et le shift-clic (mode 1) sont appliqués côté serveur. Les glissés et les
touches numériques ne le sont pas : la fenêtre est **réémise en entier** après
chaque clic, donc ce que le serveur n'a pas implémenté disparaît de l'écran au
lieu d'y rester sous forme d'objet qui n'existe pas. C'est visible et sans
danger ; la duplication, elle, ne le serait pas.

Cliquer un coffre l'ouvre toujours. Vanilla ne pose contre un coffre que si le
joueur est accroupi, ce qui n'est pas encore suivi.

---

## Taille de pile maximale par objet

Le shift-clic ne peut pas être écrit sans elle. Déplacer une pile vers un coffre
veut dire « fusionner dans les piles compatibles, puis remplir un slot vide » —
et « compatible » s'arrête à une limite qui n'est ni 64 pour tout le monde, ni
déductible du nom de l'objet. Une épée ne s'empile pas, une perle de l'Ender
s'arrête à 16, une pomme va à 64. Deviner produit soit des piles impossibles que
le client refuse d'afficher, soit de la duplication.

La valeur n'est **pas** dans `registries.json` : ce rapport liste les
identifiants, pas les propriétés des objets. Elle n'est pas non plus dans les
modèles ni dans les tags. Elle vit dans le code Java, ce que nous ne lisons pas.

**Mesure.** Un joueur (`scripts/keepalive_client.py`, qui répond aux keep-alive
et confirme les téléportations, sans quoi le serveur le déconnecte au bout de
30 s) reste connecté à un vrai serveur 1.20.1. Les commandes sont envoyées par
un FIFO sur son entrée standard, pour chacun des 1255 objets du registre :

```
clear StackProbe
give  StackProbe <item> 65
say   PROBE <item>
data get entity StackProbe Inventory
```

Demander **65** est ce qui fait la mesure : le serveur découpe la donation en
autant de piles que nécessaire, et le `Count` de la première pile *est* la
limite. Le `say` sert d'ancre pour associer chaque réponse à son objet, la
sortie de `data get` n'en portant pas le nom.

**Résultat : 1254 / 1255.** 1032 objets à 64, 45 à 16, 177 à 1. Le seul non
mesuré est `minecraft:air`, que `give` refuse — sans conséquence, un slot
d'air est un slot vide.

**Stockage.** Un octet par objet, dans l'ordre du registre `minecraft:item`,
dans le `.ovpack` (format v5, section `stacks_offset`). Lu par
`Registries::max_stack_size(ProtocolId)`, qui répond 64 pour un identifiant hors
table — c'est la valeur de la grande majorité, donc la moins nuisible en cas de
pack périmé. La table brute n'est pas commitée : elle est régénérée dans
`data/vanilla/1.20.1/normalized/stack_sizes.json`, gitignoré comme le reste des
données dérivées de Mojang.

---

## MOTION_BLOCKING et OCEAN_FLOOR : demander au jeu ce qu'il a lui-même écrit

Trois des quatre heightmaps demandent la même chose sous des formes différentes :
« ce bloc arrête-t-il le mouvement ? ». Rien dans les rapports officiels ne le
dit — en vanilla c'est du code Java. Sans cette réponse, `MOTION_BLOCKING`,
`MOTION_BLOCKING_NO_LEAVES` et `OCEAN_FLOOR` restaient vides, et c'est ce que
`PROGRESS.json` désignait depuis M3 comme le blocage nommé.

### D'abord, le sens des deux cartes

Il était inversé dans nos propres commentaires. Réglé par la mesure, sur un vrai
monde 1.20.1 : sur 10 411 colonnes où `MOTION_BLOCKING` et `OCEAN_FLOOR`
diffèrent, le bloc désigné par la première est de l'eau dans les 40 cas
examinés, et celui désigné par la seconde de la terre, du sable ou du gravier.
`MOTION_BLOCKING` compte donc les fluides, `OCEAN_FLOOR` non — c'est ce qui en
fait le *fond* d'un océan et pas sa surface.

### L'oracle

Le jeu écrit sa propre réponse dans chaque chunk qu'il sauvegarde. Il suffit
donc de poser un bloc au sommet d'une colonne sur un vrai serveur 1.20.1, de
laisser sauvegarder, et de relire quelle heightmap il a décidé de faire monter.
La lecture passe par `ov-inspect column`, donc par le lecteur NBT vérifié sur
17 879 chunks — pas par un parseur écrit pour l'occasion.

`scripts/measure_motion.py` pose les 1003 blocs en quatre zones, parce qu'un
bloc doit survivre à sa pose avant de pouvoir être lu :

| Zone | Support | Ce qu'elle rattrape |
|---|---|---|
| A | à nu sur l'herbe | tout ce qui tient seul |
| B | espacé de 16 blocs | ceux qui portent de l'eau, et l'étalent sinon sur sept blocs |
| C | muré de pierre sur quatre côtés | torches murales, échelles, boutons, panneaux |
| D | substrat choisi | cultures sur terre labourée, cactus sur sable, chorus sur end stone |

Chaque colonne est relue **par son nom** : un bloc qui n'a pas tenu — une torche
sans mur — est écarté au lieu d'être enregistré comme « n'arrête pas le
mouvement ». Les zones A et C se recouvrent largement et **ne se contredisent
nulle part**, ce qui est le contrôle croisé de la méthode elle-même.

**Résultat : 996 blocs sur 1003.** 778 arrêtent le mouvement, 187 non, 10 sont
des feuilles, 3 sont de l'air, 22 portent un fluide dans leur état par défaut.

Les sept restants — `kelp_plant`, `weeping_vines`, `weeping_vines_plant`,
`twisting_vines_plant`, `cave_vines`, `cave_vines_plant`, `big_dripleaf_stem` —
sont des segments de plantes verticales : ils ont toujours quelque chose
au-dessus d'eux, donc ils ne peuvent jamais *être* le sommet d'une colonne.
L'oracle ne peut pas les voir, et une heightmap n'en a jamais besoin. Ils sont
marqués non mesurés plutôt que supposés inoffensifs, et
`BlockRegistry::motion_measured` permet de faire la différence.

### Par bloc ou par état ?

La question décide entre une table de 1003 entrées et une de 24 135. Dix paires
d'états contrastés ont été posées côte à côte : neige à une couche et à huit,
trappe ouverte et fermée, portillon, porte, dalle et double dalle, échafaudage,
grand dripleaf incliné. **Les dix paires sont d'accord.** Le prédicat est donc
par bloc.

Et la neige n'arrête le mouvement à *aucune* épaisseur, pas même à huit couches
où elle remplit le cube entier. Aucun raisonnement sur la forme de collision ne
l'aurait prédit ; c'est exactement le genre de réponse qui justifie de mesurer.

L'eau, elle, est bien par état : `scaffolding[waterlogged=true]` fait monter
`MOTION_BLOCKING` et `scaffolding[waterlogged=false]` non, alors qu'aucun des
deux n'arrête le mouvement. Les deux ont été posés. Six blocs sont mouillés sans
avoir de propriété `waterlogged` à mettre — `water`, `lava`, `kelp`, `seagrass`,
`tall_seagrass`, `bubble_column` — d'où un bit par bloc *et* la lecture de la
propriété par état.

### Les feuilles, par deux chemins indépendants

`MOTION_BLOCKING_NO_LEAVES` a désigné exactement dix blocs. Ce sont exactement
les dix membres du tag `minecraft:leaves`, résolus séparément depuis les
fichiers JSON du datapack. Deux sources qui n'ont rien en commun donnent la même
liste.

### Vérification

`ov-inspect heightmaps <region>` recalcule les quatre heightmaps depuis les
blocs et les compare à celles que le jeu a écrites :

| Monde | Colonnes × 4 cartes | Identiques |
|---|---|---|
| Monde 1.20.1 vanilla (2601 chunks) | 274 944 | **274 944** |
| Monde moddé (2900 chunks) | 742 020 | **742 020** |

Sur le monde moddé, 380 colonnes sont écartées : elles contiennent un des 31
blocs de palette que cette version n'a pas (`create_hypertube:hypertube_entrance`,
`simulated:throttle_lever`…). Elles sont comptées comme sautées et non comme
justes — nous n'avons pas de réponse pour ces blocs, et faire comme s'ils ne
comptaient pour rien serait une réponse.

Enfin, l'aller-retour : un monde écrit par nous avec les quatre heightmaps se
charge dans le vrai serveur 1.20.1 sans une erreur, et les valeurs qu'il
réécrit sont identiques aux nôtres — `MOTION_BLOCKING_NO_LEAVES` comprise.

### Stockage

Un octet par bloc, celui qui portait déjà l'opacité à la lumière : bits 0-1
l'opacité, bit 2 « arrête le mouvement », bit 3 « feuilles », bit 4 « air »,
bit 5 « mesuré ». Plus un bit par **état** pour le fluide, soit 3 017 octets.
Format de pack v6. La table brute est régénérée dans
`data/vanilla/1.20.1/normalized/motion.json`, gitignoré comme le reste des
données dérivées de Mojang.

---

## Durées de cassage : la table d'un tiers, vérifiée tick par tick

La dureté d'un bloc n'est ni dans les rapports du data generator ni dans les
datapacks : c'est du code Java. **PrismarineJS/minecraft-data** (MIT, autorisé
par `CLAUDE.md`) la publie pour les 1003 blocs, avec le drapeau
`harvestTools` qui dit si un outil correct est exigé. C'est de là que vient la
table.

Mais sur ce projet cette source s'est déjà révélée fausse une fois — l'émission
lumineuse, donnée par bloc alors qu'elle est par état. Elle est donc prise comme
**hypothèse** et confrontée au jeu.

### L'oracle : le serveur ne casse pas le bloc tout seul

C'est le détail qui rend la mesure possible. Le serveur vanilla n'achève pas la
destruction de lui-même : il attend que le client dise « j'ai fini », puis
vérifie. Si la progression écoulée dépasse **0,7**, il croit le client sur
parole et casse immédiatement. En dessous, il bascule sur sa propre horloge et
casse le bloc au tick exact où vanilla le casserait.

Dire « fini » dans le même tick que « je commence » passe donc toujours sous le
seuil, et le nombre de ticks qui s'écoulent ensuite **est** la durée du jeu. Le
client n'a jamais eu besoin de la connaître.

Deux pièges ont coûté chacun une série de mesures fausses :

* Le compte doit se lire sur l'horloge du serveur, pas sur la montre du client.
  Le paquet `Update Time` (0x5E) porte l'âge du monde en ticks toutes les
  secondes ; il faut **le même ancrage aux deux bouts** d'une mesure. Avec deux
  ancrages différents la même mesure donnait 15, 18, 24 ou 26 pour de la terre.
  Avec un seul, elle donne 15 six fois sur six.
* L'envoi doit être aligné sur la grille de ticks. Envoyé au milieu d'un tick,
  le départ tombe dans celui-ci ou dans le suivant selon la milliseconde, et le
  compte sort une fois sur deux trop court d'un tick.

Le serveur tenait 50,00 ms par tick sur les vingt-cinq mesures de contrôle — ce
n'était donc pas lui qui variait.

### Ce que la mesure a établi, et que la table ne disait pas

| Question | Réponse mesurée | Comment |
|---|---|---|
| Vitesse des paliers | bois 2, pierre 4, or **12**, fer 6, diamant 8, netherite **9** | sur la pierre, `ceil(45 / v)` ne laisse qu'un entier possible ; netherite a demandé l'obsidienne pour séparer 9 de 10 et 11 |
| Efficacité | ajoute exactement `niveau² + 1` | 137, 79 et 43 ticks sur l'obsidienne aux niveaux I, III et V |
| Niveau de récolte de l'or | celui du **bois** | pioche en or sur minerai de fer : 25 ticks, soit le diviseur 100 |
| Ce qui autorise la récolte | la **famille** de l'outil, pas seulement son palier | pelle en netherite sur pierre : **150** ticks, exactement comme à main nue, là où une pioche en bois en prend 23 |

Ce dernier point est le seul qu'aucun raisonnement ne donne. Une pelle en
netherite dépasse tous les paliers existants et reste, sur de la pierre, aussi
lente qu'une main nue.

### Vérification

`scripts/measure_hardness.py` confronte la table au serveur, bloc par bloc, sur
la condition la moins chère : à main nue quand elle reste sous vingt secondes —
la vitesse y vaut exactement 1, donc le compte de ticks lit la dureté sans
intervalle — et avec l'outil correct au-delà, ce que l'obsidienne exige avec ses
deux cent cinquante secondes.

**985 blocs sur 996 donnent exactement le nombre de ticks prédit.** Les onze
autres ne sont pas des désaccords mais des blocs hors de portée du banc :

* `air`, `cave_air`, `void_air` — il n'y a rien à miner ;
* `water`, `lava`, `bubble_column` — un client ne vise jamais un fluide, et le
  serveur ne donne pas suite ;
* `snow`, `snow_block`, `cactus`, `powder_snow` — posés mais jamais cassés par
  le banc, sans explication trouvée.

Un douzième bloc, `dragon_egg`, donne 1 tick au lieu des 90 prédits : frappé, il
se **téléporte** au lieu de se casser. C'est un comportement, pas une dureté.

Six écarts apparents supplémentaires — dont la pierre à 149 ticks au lieu de
150 — ont disparu en remesurant seul : ils venaient de la contention entre les
six sondes parallèles. C'est la raison pour laquelle un écart n'est jamais
retenu sans être reproduit.

### De bout en bout

Le même banc, branché sur **notre** serveur en `--survival`, donne les mêmes
nombres que sur le vrai jeu :

| | vanilla | Ondes VOXEL |
|---|---|---|
| herbe, main nue | 18 | 18 |
| pierre, main nue | 150 | 150 |
| pierre, pioche en bois | 23 | 23 |
| pierre, pelle en netherite | 150 | 150 |
| planches de chêne, main nue | 60 | 60 |
| verre, main nue | 9 | 9 |
| obsidienne, pioche en fer | 834 | 834 |
| obsidienne, pioche en netherite | 167 | 167 |

Le premier essai donnait 149, 22 et 17 : un tick de moins partout. Vanilla écrit
sa condition `progression × (écoulé + 1)` avec un départ enregistré dans le tick
même qui le testera ; le nôtre est enregistré sur le thread réseau, un tick plus
tôt, donc le « + 1 » y est déjà. Un décalage d'un tick n'est pas cosmétique : le
client prédit la même arithmétique, et un bloc cassé un tick trop tôt
réapparaît sous les yeux du joueur.

---

## Tables de butin : la seule partie vraiment data-driven

Contrairement à la dureté ou à l'opacité, les tables de butin **sont** des
données en vanilla. Elles vivent dans le datapack, on les régénère localement
avec le data generator officiel, et rien n'a besoin d'être mesuré pour les
obtenir. Ce qui est du code, c'est l'**interpréteur** — et c'est lui qu'on
écrit et qu'on vérifie.

### Un vocabulaire fermé, et refusé bruyamment quand il déborde

Les 925 fichiers de tables de blocs de 1.20.1 n'emploient qu'un vocabulaire
petit et clos, ce qu'un relevé exhaustif confirme avant d'écrire une ligne :

| | |
|---|---|
| entrées | `item` (895), `alternatives` (72), `dynamic` (17) |
| conditions | `survives_explosion` (689), `block_state_property` (222), `match_tool` (138), `table_bonus` (27), `any_of` (15), `inverted` (13), `random_chance` (7), `location_check` (4), `entity_properties` (2) |
| fonctions | `set_count` (209), `explosion_decay` (136), `copy_name` (45), `copy_nbt` (37), `apply_bonus` (31), `set_contents` (17), `limit_count` (5), `copy_state` (2) |
| `match_tool` | **quatre** prédicats distincts en tout : Silk Touch, les cisailles, et deux tags |
| poids d'entrée | aucun n'est explicite : tous valent un |
| `rolls` | toujours une constante |

Tout ce qui sort de cette liste est **refusé à la compilation** plutôt que
compilé au jugé, et les fonctions qui demandent les données d'un block entity —
un coffre nommé, une shulker box remplie — laissent la pile telle quelle au lieu
de la supprimer. Une table à moitié comprise donne des objets qui n'existent
pas.

L'aplatissement se fait au build, comme pour les tags : descendre un arbre de
conditions à chaque bloc cassé mettrait un parcours de graphe dans le chemin du
joueur.

### La table d'un bloc ne porte pas toujours son nom

C'est le piège du jalon, et il est du genre à passer inaperçu. `wall_torch.json`
**n'existe pas** : une torche murale renvoie à `blocks/torch`, un panneau mural à
`blocks/oak_sign`, une tête murale à celle de la tête posée. Déduire la table du
nom du bloc marche pour **920 blocs sur 1003** — juste assez pour avoir l'air
juste, et la première comparaison a bien montré 48 blocs muraux qui ne
donnaient rien chez nous et un objet chez vanilla.

La correspondance est donc mesurée, pas devinée : la commande `/loot` journalise
`from loot table minecraft:blocks/X` avec le X qu'elle a réellement employé.
`scripts/measure_loot_tables.py` la relève pour les 1003 blocs. Vingt et un
d'entre eux pointent sur `minecraft:empty` — l'air, les fluides, la tête de
piston, le bedrock — ce qui est une réponse différente de « une table qui n'a
rien donné ».

### L'oracle : `/loot`, qui tire la table sans casser le bloc

`execute as <joueur> at <joueur> run loot give <joueur> mine <x> <y> <z> mainhand`
tire exactement la table du bloc avec l'objet tenu, et le résultat se lit dans
l'inventaire. Pas de durée, pas de position, pas de physique : juste le tirage.

Deux précautions que le banc a dû apprendre :

* **L'inventaire ne tient que 35 piles** une fois l'outil posé. Un minerai
  généreux sous Fortune III le remplit, et le total s'arrête alors net sur
  2240 — un chiffre parfaitement plausible et entièrement faux. Les tirages se
  font donc par paquets de vingt-quatre.
* **Un désaccord ne se retient pas sans être rejoué.** Un minerai sous Fortune
  multiplie un compte aléatoire par un multiplicateur aléatoire, et la variance
  du produit est bien plus large que la racine du total : sur trente-deux
  tirages, deux échantillons corrects diffèrent couramment assez pour passer
  pour une erreur. Trois cas ont ainsi été signalés à tort avant d'être rejoués
  plus longtemps.

### Ce que la comparaison a corrigé

`apply_bonus` en formule `ore_drops` tire `nextInt(niveau + 2) - 1`, plancher à
zéro, et le multiplicateur est ce résultat **plus un** : à Fortune III, 1, 1, 2,
3, 4 à chances égales, soit une moyenne de 2,2. Plancher le tirage à un au lieu
de zéro donne 1,6 — une valeur d'apparence raisonnable, et un tiers trop basse.
Le vrai serveur a tranché : 27 diamants pour douze tirages.

### Le vert qui ne prouvait rien

`tall_grass` et `large_fern` passaient la comparaison, et pour la mauvaise
raison. Leur table exige, par un `location_check`, que la moitié haute de la
plante soit bien là ; nous répondions faux faute de savoir regarder au-dessus,
et l'oracle répondait faux aussi — parce que le banc ne pose que la moitié
basse. Deux erreurs qui s'annulent donnent un vert.

Les quatre `location_check` des tables de blocs sont tous de la même forme,
« le bloc à un cran au-dessus ou au-dessous est X dans l'état Y », et servent
uniquement aux deux plantes à deux blocs. C'est donc implémenté, avec un
émetteur qui **refuse** toute autre forme plutôt que de supposer qu'elle se
comporte pareil. Mesuré ensuite sur la vraie plante entière : le serveur donne
22 graines pour 200 tirages, et rien du tout quand la moitié haute manque.

### Résultat

`scripts/check_loot.py` tire chaque cas des deux côtés et compare. La matrice
est ciblée : chaque configuration d'outil n'est essayée que sur les tables qui
la mentionnent, parce que tirer Fortune sur neuf cents tables qui l'ignorent
coûte une heure et ne prouve rien.

**1215 cas sur 1215 concordent**, sur 32 tirages chacun, quatre d'entre eux
ayant demandé 1024 tirages pour se départager.

Ce qui n'est **pas** fait, et qui est refusé bruyamment plutôt que compilé au
jugé : `copy_name`, `copy_nbt`, `copy_state` et `set_contents`, qui demandent
les données d'un block entity — un coffre nommé retombe donc anonyme, une
shulker box vide. Et l'entrée `dynamic` du pot décoré, qui porte ses tessons.
Ces fonctions laissent la pile telle quelle au lieu de la supprimer : l'objet
tombe, son contenu non.

---

## Entités objet : quatre paquets, relevés plutôt que devinés

Le butin ne sert à rien tant qu'il ne tombe pas. Quatre paquets suffisent, et
leurs identifiants ont été **capturés** sur un vrai serveur 1.20.1 — la page de
protocole archivée s'est trompée sur chacun de ceux que ce projet a vérifiés
contre elle.

Un `summon item` observé depuis un client sonde donne, dans l'ordre :

| Paquet | Id | Ce qu'il porte |
|---|---|---|
| Spawn Entity | `0x01` | id, uuid, **type 54**, x/y/z, trois angles, données, vélocité |
| Set Entity Metadata | `0x52` | index **8**, type **7** (un slot), puis la pile |
| Take Item Entity | `0x67` | ramassé, ramasseur, nombre |
| Remove Entities | `0x3E` | la liste |

Sans la métadonnée, l'entité existe et **ne rend rien du tout** — ce qui
ressemble exactement à un paquet qui ne serait jamais arrivé. `Take Item` est
purement cosmétique et le retrait reste nécessaire ensuite, mais sans lui les
objets s'éteignent à un bloc du joueur au lieu de voler vers lui.

Les octets exacts de ces trois encodages sont figés en test
(`src/ov_protocol/tests/test_entities.cpp`) : ce sont ceux que le jeu a produits
pour une entité connue, cinq diamants en 3,5 / -60 / 0,5. Un champ qui bouge ou
un varint qui grandit échoue là, et non sous la forme d'un objet invisible.

**Ce qui n'est pas vanilla, et qui est dit plutôt que caché.** Une pile tombe au
centre du bloc, sans la vitesse aléatoire que vanilla lui donne, et elle ne
subit pas la gravité : elle reste où le bloc était. Elle n'est pas non plus
sauvegardée — un redémarrage perd ce qui traînait au sol. Le ramassage se fait
dans une boîte de 1,2 bloc autour du joueur plutôt que par intersection de
volumes. Enfin, un inventaire plein laisse la pile au sol avec ce qui n'a pas
tenu : la faire disparaître serait une perte silencieuse.

---

## Conventions de pose : ce que `setblock` ne dit pas

`setblock` place exactement l'état qu'on lui donne. Il ne dit donc rien de ce
qu'un bloc **décide** au moment d'être posé — et c'est là que vivent les
conventions : quel côté d'une dalle, quelle charnière d'une porte, où va la tête
d'un lit. Cela ne se lit qu'en posant vraiment, avec un clic, une face et un
point de contact.

`scripts/measure_placement.py` joue des scénarios sur un vrai serveur 1.20.1 et
relit le monde avec `ov-inspect state`, qui rend le nom **et toutes les
propriétés** — `column` ne donnait que le nom, ce qui suffit à savoir qu'un bloc
est là et pas à savoir lequel.

### Le bogue d'un octet qui ne se voyait pas

Pendant un moment, aucune pose ne passait : le serveur renvoyait les deux blocs
concernés à leur état d'avant, sans une ligne de journal. La cause était dans la
sonde, pas dans le serveur. Le paquet de synchronisation de position porte X, Y,
Z, lacet, tangage, **un octet de drapeaux**, puis l'identifiant de
téléportation : trente-trois octets, pas trente-deux. En confirmant avec l'octet
de drapeaux la sonde répondait toujours zéro, le serveur restait en attente — et
un serveur qui attend une confirmation de téléportation **ignore silencieusement
toute interaction avec un bloc**.

Ce qui rend l'erreur durable : le **cassage**, lui, continue de marcher. Toutes
les mesures de durée ont donc été justes tout du long, et rien n'a signalé le
problème avant qu'on essaie de poser.

### Ce que le vrai serveur répond

| Scénario | État obtenu |
|---|---|
| dalle posée au sol | `type=bottom` |
| dalle recliquée par le dessus | **`type=double`** — un seul bloc, pas deux moitiés |
| clôture seule | aucune connexion |
| deux clôtures côte à côte | `east=true` / `west=true` |
| clôture contre de la pierre | `east=true` — elle s'accroche aussi au plein |
| deux murets | `east=low` / `west=low`, et **`up=true`** |
| porte | deux moitiés, `hinge=left`, orientée comme le regard |
| lit | pied au clic, tête un cran plus loin dans la direction du regard |

Les lits rejoignent donc les portes, les escaliers, les portillons et
l'observateur dans le groupe qui s'oriente **comme** le joueur regarde, et non à
son opposé.

### Ce qui reste, et pourquoi il n'est pas fait ici

Les connexions demandent un prédicat de plus : « une clôture s'accroche-t-elle à
ce bloc ? ». Il se mesure très bien — poser une clôture, poser le bloc à côté,
relire — et le relevé donne pierre oui, verre oui, portillon oui, mais dalle
non, escalier non, feuillage non, muret non. **C'est donc une propriété de
l'état et de la face**, pas du bloc : une dalle basse n'offre pas de face pleine
à l'est, la même dalle en `type=double` si. Une table par bloc serait fausse
exactement là où on la remarquerait, et elle attend d'être faite par état.
---

## Modèles de blocs : blockstate → variant → modèle → parent → faces (`ov_render`)

Le rendu de vanilla est **piloté par les modèles**, pas par une géométrie écrite
en dur. Chaque état de bloc traverse `blockstates/<bloc>.json`, y trouve une
référence de modèle et une rotation, puis `models/<chemin>.json` et sa chaîne de
`parent` jusqu'à une liste de boîtes. Un escalier n'est pas un cas particulier
du moteur : c'est trois boîtes dans un fichier JSON.

Source : `https://minecraft.wiki/w/Tutorials/Models` et
`https://minecraft.wiki/w/Model` (format de modèle et de blockstate). Les
assets eux-mêmes — 1005 blockstates, 2016 modèles de bloc de l'instance 1.20.1
— servent d'oracle secondaire partout où la documentation reste vague.

### Écart assumé au plan : pas de mailleur greedy

Le plan initial prévoyait « mesher greedy + AO ». **C'est une erreur pour un
objectif de parité, et elle est corrigée ici.** Un mailleur greedy fusionne des
faces adjacentes en un grand quad. Cela suppose deux choses que vanilla ne
respecte pas :

1. **que les blocs soient des cubes.** Un escalier, une clôture, une fleur, une
   dalle, une trappe — la majorité du contenu — ne sont pas des cubes pleins. Un
   greedy ne peut fusionner que le sous-ensemble cubique et doit émettre le
   reste face par face de toute façon, donc il ajoute un chemin de code sans
   supprimer l'autre ;
2. **que l'éclairage soit constant sur la face fusionnée.** L'occlusion
   ambiante et la lumière lissée de vanilla sont **par sommet**, calculées
   depuis les voisins de chaque coin. Fusionner deux faces détruit les sommets
   intermédiaires, donc détruit l'information. Le résultat n'est pas « presque
   pareil » : les dégradés disparaissent et le terrain devient plat.

L'émission est donc **par face, depuis le modèle baké**, ce que fait vanilla.
La ligne correspondante de `ROADMAP.md` a été réécrite.

### La règle que la documentation ne donne pas : l'`uv` généré

Le format documente `uv` comme « optionnel, généré depuis la position de
l'élément » sans dire *comment*. Il a fallu le retrouver.

D'abord par la géométrie : sur chacune des quatre faces latérales, la texture
doit apparaître **non miroitée vue de l'extérieur**, `v` croissant vers le bas.
Cela donne un axe `u` et un axe `v` par face. Ensuite par les données : les
modèles de vanilla écrivent souvent un `uv` explicite qui reproduit exactement
ce que le générateur aurait produit, ce qui rend l'hypothèse mesurable.

| Face | u | v |
|---|---|---|
| `down` | `x` | `z` |
| `up` | `x` | `z` |
| `north` | `16 − x` | `16 − y` |
| `south` | `x` | `16 − y` |
| `west` | `z` | `16 − y` |
| `east` | `16 − z` | `16 − y` |

`down` et `up` partagent la même base — `down` est donc miroitée par rapport à
la règle « vue de l'extérieur ». Ce n'est pas une simplification de notre part :
c'est ce que les données imposent, unanimement (12 faces asymétriques sur 12).

Corroboration directe, `block/stairs`, élément supérieur `from [8,8,0]`
`to [16,16,16]` : les cinq `uv` écrits à la main dans le fichier de vanilla —
`up [8,0,16,16]`, `north [0,0,8,8]`, `south [8,0,16,8]`, `west [0,0,16,8]`,
`east [0,0,16,8]` — sont **exactement** ceux que la table ci-dessus génère. Un
test unitaire les rejoue.

### Mesure à l'échelle du corpus, et l'expérience qui tranche

`tools/ov_modelbake` rejoue la table sur les 55 503 faces qui écrivent un `uv`
explicite et compte celles où la valeur écrite à la main est celle qu'on aurait
générée :

| Face | `uv` écrits | d'accord | table retenue | table miroir |
|---|---|---|---|---|
| `down` | 6 647 | 4 656 | **70,0 %** | — |
| `up` | 9 778 | 7 809 | **79,9 %** | — |
| `north` | 10 547 | 6 254 | **59,3 %** | 35,1 % |
| `south` | 10 377 | 6 922 | **66,7 %** | — |
| `west` | 9 485 | 6 470 | **68,2 %** | — |
| `east` | 8 669 | 5 515 | **63,6 %** | 55,3 % |
| **total** | **55 503** | **37 626** | **67,8 %** | 61,9 % |

Le taux n'est pas de 100 % et ne doit pas l'être : un `uv` explicite sert
justement, la plupart du temps, à s'écarter du défaut (un escalier réutilise la
moitié de la texture de son côté). Ce qui compte est qu'**aucune face ne
s'effondre**. `north` et `east` étaient les deux orientations dont un premier
échantillon de dix fichiers laissait douter ; l'expérience a été faite dans les
deux sens, et la table retenue gagne nettement sur les deux.

⚠️ **Ce que cette mesure ne prouve pas.** Elle valide l'orientation *relative*
et l'échelle, pas la chiralité absolue : une table entièrement miroitée sur les
quatre côtés produirait les mêmes rectangles. Seule une comparaison visuelle
avec le client vanilla la tranchera, et elle est à faire dès que le renderer
dessine.

### Le sens des rotations, mesuré et non choisi

Les degrés du format tournent **dans le sens horaire autour de l'axe positif**,
c'est-à-dire l'inverse de la matrice de rotation usuelle. Deux blocs le fixent
sans ambiguïté :

- `minecraft:furnace` est modélisé face au nord et utilise `y: 90` pour
  `facing=east` → `rotate_y(90)` envoie `−Z` sur `+X` ;
- `minecraft:observer` est modélisé face au nord et utilise `x: 90` pour
  `facing=down` → `rotate_x(90)` envoie `−Z` sur `−Y`.

Écrire les matrices dans l'autre sens met tous les blocs directionnels sur le
mauvais mur. Les deux faits sont des assertions de test.

⚠️ Le **signe de la rotation d'élément** (`rotation.angle` dans le modèle) n'est
lui pas discriminé par les données : les seuls éléments tournés de vanilla —
`cross`, `big_dripleaf` — sont symétriques à ±45°. La même convention que la
rotation de blockstate est appliquée, par cohérence ; à confirmer visuellement.

### `ambientocclusion` vient de la racine, pas de la feuille

Le format dit « only works on Parent file ». Vérifié sur les assets 1.20.1 :
tout modèle qui déclare `ambientocclusion` est lui-même une racine de chaîne.
C'est la valeur de la **racine** qui est retenue. Sans cela, `block/cross`
perdrait son `false` et l'herbe recevrait un éclairage lissé qu'elle ne doit
pas avoir.

### Autres points tranchés par les données

- **`cullface: "bottom"`** est une orthographe acceptée de `"down"`, et pas une
  hypothèse : quatre faces des assets 1.20.1 l'utilisent.
- **`"elements": []`** ne veut pas dire « hérite » : la liste la plus proche
  dans la chaîne remplace celle du parent, vide comprise.
- **Une clé de variante vide (`""`)** est une vraie clé, celle des blocs à un
  seul modèle. Un parseur JSON qui confond « clé vide » et « clé absente » perd
  tous ces blocs.
- **Les variantes pondérées sont toutes conservées**, jamais choisies ici :
  vanilla tire au sort d'après la position du bloc, donc seul le mailleur, qui
  connaît la position, peut choisir — et il doit choisir la même à chaque
  remaillage.

### Vérification de bout en bout

```
$ ov_modelbake run/assets
blockstate files .....   1005  (0 failed)
model references .....   6081
models resolved ......   6081  (0 failed)
distinct models cached   1810
quads baked ..........  62227
missing sprites ......      0
```

Aucun asset n'est commité : l'outil lit `run/assets`, produit par
`ov_assetimport` et exclu par `.gitignore`.

### Occlusion ambiante et lumière lissée

La règle par sommet vient de trois voisins de chaque coin — les deux côtés dans
le plan de la face, et la diagonale :

```
si côté1 et côté2 : 0
sinon             : 3 − (côté1 + côté2 + diagonale)
```

Le cas particulier « deux côtés pleins → 0 quelle que soit la diagonale » n'est
pas un détail : c'est lui qui creuse un angle rentrant au lieu de le gonfler.
La lumière par sommet moyenne les quatre valeurs mais **saute les voisins
opaques** — un bloc opaque a une lumière de 0, et le compter assombrirait le
sommet deux fois, une fois par l'occlusion et une fois par la moyenne. Les deux
fonctions doivent s'accorder sur ce qui est visible, sinon un sommet est à la
fois totalement occlus et pleinement éclairé.

### Vertex de 8 octets : ce qui n'y tient pas

Le budget est une contrainte, pas une préférence : à 12 chunks de distance, ce
sont les **pics d'upload en déplacement** qui décident du p99, pas le FPS moyen.

```
 0-10  x       11 bits   1/64 de bloc, plage [-8, 24[
11-21  y       11 bits
22-32  z       11 bits
33-40  u        8 bits   1/8 d'unité de sprite
41-48  v        8 bits
49-52  sky      4 bits
53-56  block    4 bits
57-58  ao       2 bits
59-61  facing   3 bits   0-5 une Direction, 6 « non ombré »
62-63  tint     2 bits
```

Deux décisions à assumer :

1. **L'échelle des `uv` est 1/8 d'unité de sprite, pas 1/16.** Un premier
   découpage à 1/16 rendait `u = 16` — la valeur la plus fréquente du corpus,
   celle de toute face pleine — non représentable exactement, ce qui rétrécit
   chaque face d'un fragment de texel. Le test l'a attrapé avant le renderer.
2. **Il n'y a pas de couleur de sommet.** Vanilla multiplie la teinte de biome
   dedans côté CPU ; trois octets de plus ne rentrent pas. Le champ `tint`
   nomme le canal (herbe, feuillage, eau) et le shader va chercher la couleur.
   C'est un mécanisme **différent** de celui de vanilla, choisi pour le budget,
   et valide tant que la teinte est constante sur un quad — ce qu'elle est pour
   un bloc.

### Ce qui n'est pas encore vérifié

- La chiralité absolue de la table `uv` (voir plus haut).
- Le signe de la rotation d'élément.
- Le comportement exact d'`uvlock` sous une rotation en `x`. L'implémentation
  est générique — elle défait la rotation subie par la base tangente de la face
  — et le cas `y` est verrouillé par un test qui vérifie la définition même
  (« un point du bloc qui n'a pas bougé garde sa coordonnée de texture »). Le
  cas `x` n'a pas d'équivalent mesurable sans image de référence.

Ces trois points se tranchent en une session dès que le renderer affiche un
chunk, et ils sont listés ici pour ne pas être oubliés.