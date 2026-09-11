# Matrice de conformité — protocole 763 (Java 1.20.1)

> **Fichier généré** par `scripts/protocol_matrix.py` — ne pas éditer à la main.
> `python3 scripts/protocol_matrix.py --check` échoue en CI s'il est périmé, ou si
> une constante d'id du code contredit le catalogue.

Catalogue : `data/protocol/763.json` (176 paquets), dérivé de deux sources qui
s'accordent sur chaque id — PrismarineJS/minecraft-data (MIT) et l'archive figée
wiki.vg (oldid 2773082). Voir `docs/provenance/protocole-763.md`.

## Légende

| Colonne | Sens |
|---|---|
| Constante | un id nommé dans le code, **valeur vérifiée** contre le catalogue |
| Enc. / Déc. | une fonction `encode_*` / `parse_*`·`decode_*` publique de `ov_protocol` |
| Test | un `TEST_CASE` appelle l'une d'elles |
| A/R | un même `TEST_CASE` appelle l'encodeur **et** le décodeur (aller-retour) |
| Vanilla | un `TEST_CASE` les appelle sur des octets en hexadécimal dont il dit qu'ils viennent du vrai serveur |
| Srv / Cli | la constante est utilisée par `ov_server` / `ov_netclient` |

Statut : le plus fort atteint — `vanilla` > `aller-retour` > `testé` > `codé` >
`id seul` > `absent`. Un paquet émis en dur ailleurs (constante locale à
`ov_server`) apparaît avec sa constante mais sans fonction dans `ov_protocol`.

## Résumé

**125 / 176** paquets ont au moins un id ou une fonction.

| Statut | Paquets |
|---|---:|
| vanilla | 15 |
| aller-retour | 36 |
| testé | 27 |
| codé | 38 |
| id seul | 9 |
| absent | 51 |

| État · sens | Total | Présents | Enc. | Déc. | A/R | Vanilla |
|---|---:|---:|---:|---:|---:|---:|
| Handshaking · client → serveur | 2 | 1 | 0 | 1 | 0 | 0 |
| Status · serveur → client | 2 | 2 | 2 | 0 | 0 | 0 |
| Status · client → serveur | 2 | 2 | 0 | 0 | 0 | 0 |
| Login · serveur → client | 5 | 3 | 3 | 0 | 0 | 0 |
| Login · client → serveur | 3 | 1 | 0 | 1 | 0 | 0 |
| Play · serveur → client | 111 | 85 | 84 | 39 | 39 | 15 |
| Play · client → serveur | 51 | 31 | 8 | 25 | 7 | 0 |

## Handshaking — client → serveur

| Id | Paquet | Statut | Const. | Enc. | Déc. | Test | A/R | Vanilla | Srv | Cli |
|---|---|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| `0x00` | Handshake | testé | · | · | ✓ | ✓ | · | · | · | · |
| `0xFE` | Legacy Server List Ping | absent | · | · | · | · | · | · | · | · |

## Status — serveur → client

| Id | Paquet | Statut | Const. | Enc. | Déc. | Test | A/R | Vanilla | Srv | Cli |
|---|---|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| `0x00` | Status Response | testé | ✓ | ✓ | · | ✓ | · | · | · | · |
| `0x01` | Ping Response | testé | ✓ | ✓ | · | ✓ | · | · | · | · |

## Status — client → serveur

| Id | Paquet | Statut | Const. | Enc. | Déc. | Test | A/R | Vanilla | Srv | Cli |
|---|---|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| `0x00` | Status Request | id seul | ✓ | · | · | · | · | · | · | · |
| `0x01` | Ping Request | id seul | ✓ | · | · | · | · | · | · | · |

## Login — serveur → client

| Id | Paquet | Statut | Const. | Enc. | Déc. | Test | A/R | Vanilla | Srv | Cli |
|---|---|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| `0x00` | Disconnect (login) | testé | ✓ | ✓ | · | ✓ | · | · | · | · |
| `0x01` | Encryption Request | absent | · | · | · | · | · | · | · | · |
| `0x02` | Login Success | testé | ✓ | ✓ | · | ✓ | · | · | · | · |
| `0x03` | Set Compression | testé | ✓ | ✓ | · | ✓ | · | · | · | · |
| `0x04` | Login Plugin Request | absent | · | · | · | · | · | · | · | · |

## Login — client → serveur

| Id | Paquet | Statut | Const. | Enc. | Déc. | Test | A/R | Vanilla | Srv | Cli |
|---|---|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| `0x00` | Login Start | testé | ✓ | · | ✓ | ✓ | · | · | · | · |
| `0x01` | Encryption Response | absent | · | · | · | · | · | · | · | · |
| `0x02` | Login Plugin Response | absent | · | · | · | · | · | · | · | · |

## Play — serveur → client

| Id | Paquet | Statut | Const. | Enc. | Déc. | Test | A/R | Vanilla | Srv | Cli |
|---|---|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| `0x00` | Bundle Delimiter | absent | · | · | · | · | · | · | · | · |
| `0x01` | Spawn Entity | vanilla | ✓ | ✓ | · | ✓ | · | ✓ | ✓ | ✓ |
| `0x02` | Spawn Experience Orb | testé | ✓ | ✓ | · | ✓ | · | · | ✓ | ✓ |
| `0x03` | Spawn Player | codé | ✓ | ✓ | · | · | · | · | ✓ | ✓ |
| `0x04` | Entity Animation | codé | ✓ | ✓ | · | · | · | · | ✓ | · |
| `0x05` | Award Statistics | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | · | · |
| `0x06` | Acknowledge Block Change | codé | ✓ | ✓ | · | · | · | · | ✓ | · |
| `0x07` | Set Block Destroy Stage | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ | ✓ |
| `0x08` | Block Entity Data | codé | ✓ | ✓ | · | · | · | · | ✓ | · |
| `0x09` | Block Action | vanilla | ✓ | ✓ | · | ✓ | · | ✓ | · | · |
| `0x0A` | Block Update | codé | ✓ | ✓ | · | · | · | · | ✓ | ✓ |
| `0x0B` | Boss Bar | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ | ✓ |
| `0x0C` | Change Difficulty | testé | ✓ | ✓ | · | ✓ | · | · | ✓ | · |
| `0x0D` | Chunk Biomes | absent | · | · | · | · | · | · | · | · |
| `0x0E` | Clear Titles | testé | ✓ | ✓ | · | ✓ | · | · | ✓ | ✓ |
| `0x0F` | Command Suggestions Response | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ | ✓ |
| `0x10` | Commands | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ | ✓ |
| `0x11` | Close Container | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ | ✓ |
| `0x12` | Set Container Content | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ | ✓ |
| `0x13` | Set Container Property | codé | ✓ | ✓ | · | · | · | · | ✓ | · |
| `0x14` | Set Container Slot | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ | ✓ |
| `0x15` | Set Cooldown | testé | ✓ | ✓ | · | ✓ | · | · | ✓ | · |
| `0x16` | Chat Suggestions | absent | · | · | · | · | · | · | · | · |
| `0x17` | Plugin Message | absent | · | · | · | · | · | · | · | · |
| `0x18` | Damage Event | vanilla | ✓ | ✓ | · | ✓ | · | ✓ | ✓ | ✓ |
| `0x19` | Delete Message | absent | · | · | · | · | · | · | · | · |
| `0x1A` | Disconnect (play) | codé | ✓ | ✓ | · | · | · | · | ✓ | ✓ |
| `0x1B` | Disguised Chat Message | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ | ✓ |
| `0x1C` | Entity Event | vanilla | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `0x1D` | Explosion | vanilla | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `0x1E` | Unload Chunk | codé | ✓ | ✓ | · | · | · | · | ✓ | ✓ |
| `0x1F` | Game Event | testé | ✓ | ✓ | · | ✓ | · | · | ✓ | ✓ |
| `0x20` | Open Horse Screen | absent | · | · | · | · | · | · | · | · |
| `0x21` | Hurt Animation | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | · | ✓ |
| `0x22` | Initialize World Border | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | · | · |
| `0x23` | Keep Alive | codé | ✓ | ✓ | · | · | · | · | ✓ | ✓ |
| `0x24` | Chunk Data and Update Light | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ | ✓ |
| `0x25` | World Event | vanilla | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `0x26` | Particle | absent | · | · | · | · | · | · | · | · |
| `0x27` | Update Light | absent | · | · | · | · | · | · | · | · |
| `0x28` | Login (play) | testé | ✓ | ✓ | · | ✓ | · | · | ✓ | ✓ |
| `0x29` | Map Data | absent | · | · | · | · | · | · | · | · |
| `0x2A` | Merchant Offers | id seul | ✓ | · | · | · | · | · | ✓ | · |
| `0x2B` | Update Entity Position | testé | ✓ | ✓ | · | ✓ | · | · | ✓ | ✓ |
| `0x2C` | Update Entity Position and Rotation | testé | ✓ | ✓ | · | ✓ | · | · | · | ✓ |
| `0x2D` | Update Entity Rotation | codé | ✓ | ✓ | · | · | · | · | · | ✓ |
| `0x2E` | Move Vehicle | absent | · | · | · | · | · | · | · | · |
| `0x2F` | Open Book | absent | · | · | · | · | · | · | · | · |
| `0x30` | Open Screen | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ | ✓ |
| `0x31` | Open Sign Editor | codé | ✓ | ✓ | · | · | · | · | ✓ | · |
| `0x32` | Ping (play) | absent | · | · | · | · | · | · | · | · |
| `0x33` | Place Ghost Recipe | absent | · | · | · | · | · | · | · | · |
| `0x34` | Player Abilities | codé | ✓ | ✓ | · | · | · | · | ✓ | ✓ |
| `0x35` | Player Chat Message | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ | ✓ |
| `0x36` | End Combat | absent | · | · | · | · | · | · | · | · |
| `0x37` | Enter Combat | absent | · | · | · | · | · | · | · | · |
| `0x38` | Combat Death | testé | ✓ | ✓ | · | ✓ | · | · | ✓ | ✓ |
| `0x39` | Player Info Remove | codé | ✓ | ✓ | · | · | · | · | ✓ | · |
| `0x3A` | Player Info Update | codé | ✓ | ✓ | · | · | · | · | ✓ | · |
| `0x3B` | Look At | codé | ✓ | ✓ | · | · | · | · | ✓ | · |
| `0x3C` | Synchronize Player Position | codé | ✓ | ✓ | · | · | · | · | ✓ | ✓ |
| `0x3D` | Update Recipe Book | codé | ✓ | ✓ | · | · | · | · | ✓ | · |
| `0x3E` | Remove Entities | testé | ✓ | ✓ | · | ✓ | · | · | ✓ | ✓ |
| `0x3F` | Remove Entity Effect | vanilla | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `0x40` | Resource Pack | absent | · | · | · | · | · | · | · | · |
| `0x41` | Respawn | testé | ✓ | ✓ | · | ✓ | · | · | ✓ | ✓ |
| `0x42` | Set Head Rotation | codé | ✓ | ✓ | · | · | · | · | ✓ | ✓ |
| `0x43` | Update Section Blocks | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ | · |
| `0x44` | Select Advancements Tab | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | · | · |
| `0x45` | Server Data | testé | ✓ | ✓ | · | ✓ | · | · | ✓ | · |
| `0x46` | Set Action Bar Text | codé | ✓ | ✓ | · | · | · | · | ✓ | ✓ |
| `0x47` | Set Border Center | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | · | · |
| `0x48` | Set Border Lerp Size | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | · | · |
| `0x49` | Set Border Size | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | · | · |
| `0x4A` | Set Border Warning Delay | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | · | · |
| `0x4B` | Set Border Warning Distance | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | · | · |
| `0x4C` | Set Camera | absent | · | · | · | · | · | · | · | · |
| `0x4D` | Set Held Item | absent | · | · | · | · | · | · | · | · |
| `0x4E` | Set Center Chunk | codé | ✓ | ✓ | · | · | · | · | ✓ | · |
| `0x4F` | Set Render Distance | absent | · | · | · | · | · | · | · | · |
| `0x50` | Set Default Spawn Position | codé | ✓ | ✓ | · | · | · | · | ✓ | · |
| `0x51` | Display Objective | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | · | · |
| `0x52` | Set Entity Metadata | vanilla | ✓ | ✓ | · | ✓ | · | ✓ | ✓ | ✓ |
| `0x53` | Link Entities | absent | · | · | · | · | · | · | · | · |
| `0x54` | Set Entity Velocity | vanilla | ✓ | ✓ | · | ✓ | · | ✓ | ✓ | · |
| `0x55` | Set Equipment | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ | ✓ |
| `0x56` | Set Experience | vanilla | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `0x57` | Set Health | vanilla | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `0x58` | Update Objectives | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | · | · |
| `0x59` | Set Passengers | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ | ✓ |
| `0x5A` | Update Teams | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | · | · |
| `0x5B` | Update Score | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | · | · |
| `0x5C` | Set Simulation Distance | absent | · | · | · | · | · | · | · | · |
| `0x5D` | Set Subtitle Text | codé | ✓ | ✓ | · | · | · | · | ✓ | ✓ |
| `0x5E` | Update Time | testé | ✓ | ✓ | · | ✓ | · | · | ✓ | ✓ |
| `0x5F` | Set Title Text | codé | ✓ | ✓ | · | · | · | · | ✓ | ✓ |
| `0x60` | Set Title Animation Times | testé | ✓ | ✓ | · | ✓ | · | · | ✓ | ✓ |
| `0x61` | Entity Sound Effect | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | · | ✓ |
| `0x62` | Sound Effect | vanilla | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `0x63` | Stop Sound | vanilla | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ |
| `0x64` | System Chat Message | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ | ✓ |
| `0x65` | Set Tab List Header And Footer | absent | · | · | · | · | · | · | · | · |
| `0x66` | Tag Query Response | absent | · | · | · | · | · | · | · | · |
| `0x67` | Pickup Item | testé | ✓ | ✓ | · | ✓ | · | · | ✓ | ✓ |
| `0x68` | Teleport Entity | codé | ✓ | ✓ | · | · | · | · | ✓ | ✓ |
| `0x69` | Update Advancements | absent | · | · | · | · | · | · | · | · |
| `0x6A` | Update Attributes | vanilla | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | · |
| `0x6B` | Feature Flags | absent | · | · | · | · | · | · | · | · |
| `0x6C` | Entity Effect | vanilla | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `0x6D` | Update Recipes | codé | ✓ | ✓ | · | · | · | · | ✓ | · |
| `0x6E` | Update Tags | absent | · | · | · | · | · | · | · | · |

## Play — client → serveur

| Id | Paquet | Statut | Const. | Enc. | Déc. | Test | A/R | Vanilla | Srv | Cli |
|---|---|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| `0x00` | Confirm Teleportation | codé | ✓ | · | ✓ | · | · | · | ✓ | ✓ |
| `0x01` | Query Block Entity Tag | absent | · | · | · | · | · | · | · | · |
| `0x02` | Change Difficulty | testé | ✓ | · | ✓ | ✓ | · | · | ✓ | · |
| `0x03` | Message Acknowledgment | codé | ✓ | · | ✓ | · | · | · | ✓ | · |
| `0x04` | Chat Command | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ | ✓ |
| `0x05` | Chat Message | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ | ✓ |
| `0x06` | Player Session | id seul | ✓ | · | · | · | · | · | ✓ | · |
| `0x07` | Client Command | testé | ✓ | · | ✓ | ✓ | · | · | ✓ | ✓ |
| `0x08` | Client Information | codé | ✓ | · | ✓ | · | · | · | ✓ | ✓ |
| `0x09` | Command Suggestions Request | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ | ✓ |
| `0x0A` | Click Container Button | id seul | ✓ | · | · | · | · | · | ✓ | · |
| `0x0B` | Click Container | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ | ✓ |
| `0x0C` | Close Container | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ | ✓ |
| `0x0D` | Plugin Message | id seul | ✓ | · | · | · | · | · | ✓ | · |
| `0x0E` | Edit Book | absent | · | · | · | · | · | · | · | · |
| `0x0F` | Query Entity Tag | absent | · | · | · | · | · | · | · | · |
| `0x10` | Interact | testé | ✓ | · | ✓ | ✓ | · | · | ✓ | · |
| `0x11` | Jigsaw Generate | absent | · | · | · | · | · | · | · | · |
| `0x12` | Keep Alive | codé | ✓ | · | ✓ | · | · | · | ✓ | ✓ |
| `0x13` | Lock Difficulty | absent | · | · | · | · | · | · | · | · |
| `0x14` | Set Player Position | codé | ✓ | · | ✓ | · | · | · | ✓ | · |
| `0x15` | Set Player Position and Rotation | codé | ✓ | · | ✓ | · | · | · | ✓ | ✓ |
| `0x16` | Set Player Rotation | codé | ✓ | · | ✓ | · | · | · | ✓ | · |
| `0x17` | Set Player On Ground | codé | ✓ | · | ✓ | · | · | · | ✓ | · |
| `0x18` | Move Vehicle | absent | · | · | · | · | · | · | · | · |
| `0x19` | Paddle Boat | absent | · | · | · | · | · | · | · | · |
| `0x1A` | Pick Item | absent | · | · | · | · | · | · | · | · |
| `0x1B` | Place Recipe | absent | · | · | · | · | · | · | · | · |
| `0x1C` | Player Abilities | id seul | ✓ | · | · | · | · | · | ✓ | ✓ |
| `0x1D` | Player Action | testé | ✓ | ✓ | ✓ | ✓ | · | · | ✓ | ✓ |
| `0x1E` | Player Command | codé | ✓ | · | ✓ | · | · | · | ✓ | · |
| `0x1F` | Player Input | absent | · | · | · | · | · | · | · | · |
| `0x20` | Pong (play) | absent | · | · | · | · | · | · | · | · |
| `0x21` | Change Recipe Book Settings | absent | · | · | · | · | · | · | · | · |
| `0x22` | Set Seen Recipe | absent | · | · | · | · | · | · | · | · |
| `0x23` | Rename Item | id seul | ✓ | · | · | · | · | · | ✓ | · |
| `0x24` | Resource Pack | absent | · | · | · | · | · | · | · | · |
| `0x25` | Seen Advancements | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | · | · |
| `0x26` | Select Trade | id seul | ✓ | · | · | · | · | · | ✓ | · |
| `0x27` | Set Beacon Effect | absent | · | · | · | · | · | · | · | · |
| `0x28` | Set Held Item | codé | ✓ | · | ✓ | · | · | · | ✓ | ✓ |
| `0x29` | Program Command Block | absent | · | · | · | · | · | · | · | · |
| `0x2A` | Program Command Block Minecart | absent | · | · | · | · | · | · | · | · |
| `0x2B` | Set Creative Mode Slot | codé | ✓ | · | ✓ | · | · | · | ✓ | ✓ |
| `0x2C` | Program Jigsaw Block | absent | · | · | · | · | · | · | · | · |
| `0x2D` | Program Structure Block | absent | · | · | · | · | · | · | · | · |
| `0x2E` | Update Sign | codé | ✓ | · | ✓ | · | · | · | ✓ | · |
| `0x2F` | Swing Arm | aller-retour | ✓ | ✓ | ✓ | ✓ | ✓ | · | ✓ | ✓ |
| `0x30` | Teleport To Entity | absent | · | · | · | · | · | · | · | · |
| `0x31` | Use Item On | codé | ✓ | · | ✓ | · | · | · | ✓ | ✓ |
| `0x32` | Use Item | testé | ✓ | · | ✓ | ✓ | · | · | ✓ | · |
