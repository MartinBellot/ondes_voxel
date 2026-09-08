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
