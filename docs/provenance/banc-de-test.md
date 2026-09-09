# Le banc de test — `ov-lab`

## Ce que c'est

Un monde Anvil persistant, écrit **depuis du code** par `tools/ov_lab`, contenant une
parcelle par système à mesurer. Il vit dans `run/lab`, donc il n'est jamais commité ; ce
qui est commité, c'est le programme qui le fabrique.

C'est un choix, pas une commodité. Un banc construit à la main dans un client dérive :
quelqu'un déplace un levier, une mesure change, et rien n'enregistre pourquoi. Un banc
écrit depuis un catalogue se régénère à l'identique, et corriger une parcelle est un diff.

31 parcelles, 441 chunks, 460 494 blocs. `ov-lab --list` imprime le catalogue avec les
coordonnées ; les origines sont espacées de 32 blocs, donc chaque parcelle de 16×16 a une
route autour d'elle.

## Ce qui est vérifié, et comment

| Affirmation | Comment elle a été établie |
|---|---|
| Chaque bloc et chaque état posés existent | `ov-lab` **refuse et nomme** tout nom ou combinaison de propriétés que le registre ne connaît pas, et sort en erreur. La première génération est passée sans un seul refus, sur les 24 135 états. |
| Notre serveur le sert | `ov_dedicated --world=run/lab` : `world data version 3465`, un client sonde rejoint à (-7.5, -59.0, -7.5), le spawn déclaré par le `level.dat`. |
| Notre client le rend | `ov_voxel --connect` : 31 chunks, 44 sections, 240 frames, GPU p50 0,56 ms / p99 2,11 ms. Capture dans `run/lab-plaza.png`. |
| **Le vrai Minecraft 1.20.1 l'ouvre** | `server.jar` lancé sur une copie : `Done (7.362s)`, **aucune erreur, aucune exception**. Il a réécrit les quatre fichiers de région. |
| Nos blocs survivent à sa réécriture | Relus par `ov-inspect state` dans la sauvegarde **réécrite par lui** : `smooth_stone` de la place, `redstone_block`, `redstone_wire`, `redstone_lamp[lit=false]`, `lever[face=floor,facing=north,powered=false]`. |

## Trois choses apprises en le construisant

**1. Un monde écrit sans lumière est servi noir.** Le serveur recalculait la lumière de
bloc à la lecture mais pas celle du ciel, et `ov-lab` n'a pas de moteur de lumière — le
banc entier était dans le noir. Le serveur recalcule désormais la lumière du ciel
**seulement quand le fichier n'en porte aucune** : un monde écrit par un outil est éclairé,
un monde écrit par vanilla garde la lumière que vanilla a calculée à travers les frontières
de chunk, qu'une passe par chunk chez nous ne pourrait que dégrader.

**2. Le serveur écrasait le `level.dat` à chaque sauvegarde.** Il en reconstruisait un
neuf avec ses propres constantes, donc le nom et le point d'apparition du monde ouvert
étaient perdus à la première autosauvegarde. Un banc persistant ne survit pas à ça. Le
serveur lit maintenant `LevelName`, `SpawnX/Y/Z` au démarrage, s'en sert pour placer une
première arrivée, et les réécrit inchangés.

**3. Vanilla ne réévalue pas la redstone au chargement.** Le fil posé à côté d'un bloc de
redstone est relu `power=0, east=none, …` après un cycle complet de chargement et de
sauvegarde par le vrai serveur : l'état stocké est cru tel quel, et rien ne le recalcule
tant qu'un changement de bloc ne le réveille pas. Les parcelles de redstone sont donc
livrées dans un état **non évalué** — il faut les pousser (poser ou casser un bloc à côté)
pour que le jeu les calcule. Ce n'est pas un défaut du banc : c'est ce qu'il faut savoir
pour ne pas mesurer un circuit endormi.

## Ce qui n'est pas fait

- **Les panneaux ne se rendent pas** dans notre client : un panneau est une entité de bloc
  avec son propre rendu, et ce rendu n'existe pas encore. Le texte est bien dans la
  sauvegarde et s'affiche dans le client vanilla ; chez nous on ne voit que le poteau.
- **Les parcelles de mélange lave/eau sont livrées vides** — les cuves sont creusées, les
  fluides ne sont pas posés. Les poser à l'écriture les ferait se mélanger avant que
  quiconque regarde.
- **Rayon d'action de l'éponge et plafond de 65 blocs** : la parcelle existe, la mesure
  n'a pas été faite ici.
