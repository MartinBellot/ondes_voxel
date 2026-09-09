# Les égalités de la table des biomes

Ce qui restait entre 99,972 % et 100 % de parité sur les biomes, et comment
c'est fermé. Mesuré contre `run/reference-1234567890`, graine 1234567890,
5092 chunks au statut `minecraft:full`, 7 821 312 cellules de biome.

## Le point de départ

`biome_source.cpp` faisait un balayage linéaire des 7593 boîtes de climat et
gardait la plus proche, la première gagnant à égalité. Résultat :

    7 819 115 / 7 821 312   99,972 %   (2197 désaccords)

`ov_parity` disait déjà l'essentiel : **les 2197 désaccords étaient tous des
égalités exactes**, c'est-à-dire deux boîtes à la même distance au carré. Le
calcul du climat était donc identique au jeu ; seul le **départage** différait.

## Ce qui départage, chez le jeu

Deux mécanismes, et il faut les deux.

### 1. L'ordre de parcours de l'arbre

Le jeu indexe les 7593 boîtes dans un R-tree. L'argument « un arbre trouve le
même minimum qu'un balayage, donc la réponse est identique » est vrai pour la
**distance** et faux pour la **boîte** : la recherche ne remplace son meilleur
courant que si un candidat est **strictement** plus proche. À égalité, c'est
donc le premier atteint qui gagne — et l'ordre d'atteinte est celui de l'arbre,
pas celui du fichier.

La construction reproduite est ascendante :

* les feuilles partent dans l'ordre de la table exportée ;
* un groupe de six ou moins est trié par la somme des valeurs absolues des
  milieux de ses sept axes ;
* un groupe plus grand est découpé en essayant les **sept** axes : tri par cet
  axe (égalités départagées par les axes suivants, en boucle), découpe en
  paquets de la plus grande puissance de six **inférieure** au compte, et on
  garde l'axe qui minimise la somme des boîtes englobantes ;
* les paquets sont ensuite réordonnés par milieu absolu sur l'axe gagnant.

Le détail qui compte : la taille de paquet est calculée depuis `compte - 0,01`,
sinon une puissance exacte de six se découpe en **un** paquet au lieu de six.
Avec 7593 boîtes cela donne 6^4 = 1296 au premier niveau, puis exactement six
paquets de 216, etc. L'arbre construit fait 9112 nœuds.

Le tri doit être **stable** (`std::stable_sort`) : là où les sept clés sont
égales, c'est l'ordre source qui est la réponse.

Mesure, arbre seul :

    7 819 131 / 7 821 312   99,972 %   (2181 égalités)

**16 cellules gagnées sur 2197.** L'arbre seul ne suffit pas, et c'est ce
chiffre qui l'a prouvé plutôt qu'un raisonnement.

### 2. Le cache d'une entrée

La recherche démarre depuis la dernière boîte retournée. Elle sert de meilleur
courant, donc elle **gagne toute égalité dont elle fait partie**, et elle élague
aussi : un sous-arbre pas plus proche que la boîte en cache n'est jamais ouvert,
donc une boîte à égalité qui s'y trouve n'est même pas atteinte.

Conséquence : **le biome d'un endroit dépend de ce qu'on a demandé avant lui.**
Chez le jeu le cache vit dans un thread-local à l'intérieur de l'arbre ; ici
c'est une valeur que l'appelant possède (`BiomeSearchCache`), pour que la
dépendance soit écrite au lieu d'être cachée et pour que deux threads ne puissent
pas en partager une.

## L'ordre des requêtes, mesuré et non deviné

Restait à savoir dans quel ordre le jeu pose ses questions. Les cellules de
biome sont **stockées** dans l'ordre y, z, x (index = y·16 + z·4 + x). L'ordre
de remplissage n'est pas forcément le même. `ov_parity --cache=` permet de
choisir, et le chiffre tranche :

| recherche | cache | ordre | cellules identiques | |
|---|---|---|---:|---|
| balayage | non | — | 7 819 115 / 7 821 312 | 99,972 % |
| arbre | non | — | 7 819 131 / 7 821 312 | 99,972 % |
| arbre | oui | stockage (y, z, x) | 7 819 233 / 7 821 312 | 99,973 % |
| arbre | oui | remplissage (x, y, z) | **7 821 312 / 7 821 312** | **100,000 %** |

L'ordre de stockage ne récupère presque rien (102 cellules sur 2181) ; l'ordre
de remplissage ferme tout. C'est cette mesure — et non un raisonnement sur le
code — qui a nommé l'imbrication des boucles dans `chunk_generator.cpp` :
section par section depuis le bas, puis x, puis y, puis z. Le cache est remis à
zéro par chunk ; chez le jeu il traîne d'un chunk à l'autre sur le même thread
du pool de génération, mais l'influence de l'état initial est lavée après les
premières cellules, et le résultat est exact sans elle.

## Reproduire

```bash
cmake --preset macos-release -DOV_BUILD_CLIENT=OFF -DOV_BUILD_TESTS=OFF
cmake --build --preset macos-release --target ov_parity

OV_BIOME_SCAN=1 ./build/macos-release/bin/ov_parity --chunks=100000 --cache=none
./build/macos-release/bin/ov_parity --chunks=100000 --cache=none
./build/macos-release/bin/ov_parity --chunks=100000 --cache=yzx
./build/macos-release/bin/ov_parity --chunks=100000          # xyz, le défaut
```

`OV_BIOME_SCAN=1` rebranche le balayage linéaire, comme `OV_NO_INTERPOLATION`
rebranche l'ancienne densité : les quatre chiffres du tableau sont à une
reconstruction l'un de l'autre, pas à une séance d'archéologie.

## Pièges rencontrés

* **`std::sort` au lieu de `std::stable_sort`** dans la construction de l'arbre :
  l'ordre source fait partie de la réponse là où les clés sont égales.
* **La division entière du milieu** `(min + max) / 2` tronque vers zéro. Arrondir
  autrement réordonne les boîtes dont le milieu chevauche zéro.
* **`compte - 0,01`** dans la taille de paquet. Sans le retrait, 1296 boîtes
  donnent un paquet de 1296 au lieu de six paquets de 216, et tout l'ordre change.
* Le cache rend `biome_at` **dépendant de l'ordre** : un appelant qui échantillonne
  dans un autre ordre que le jeu obtient un résultat plausible et faux. C'est
  pour cela que le cache est un paramètre et pas un membre.

## Sources

Algorithme du R-tree de climat et du cache : documentation communautaire sur la
distribution des biomes 1.18+. Aucune ligne de code décompilé. Toute forme
retenue ici a été **validée par la mesure** ci-dessus : c'est le passage de
99,972 % à 100,000 % qui la justifie, pas l'inverse.
