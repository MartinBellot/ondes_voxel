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
