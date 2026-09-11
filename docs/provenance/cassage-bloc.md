# Casser un bloc — ce que le joueur voit et entend

Avant ce dossier, notre client cassait **instantanément** : un clic envoyait
Player Action 0 puis 2 dans la même image, le serveur (qui mesure le temps de
cassage au tick près, voir la ligne « Durées de cassage » de la ROADMAP) le
refusait sous son seuil de 0,7 et finissait le bloc sur sa propre horloge.
Rien ne se voyait entre-temps : ni fissure, ni particule, ni bras. Le serveur,
lui, n'envoyait à personne la progression d'un joueur qui creuse.

Ce dossier établit, par mesure quand un oracle existe :

1. **les paquets** autour du cassage, reconnus par leur contenu sur le vrai
   serveur ;
2. **ce que le serveur dit aux autres** pendant qu'un joueur creuse ;
3. **le calendrier du client** — quand il envoie Start, Finish, Abort, Swing ;
4. **les fissures** — géométrie, projection, mélange ;
5. **les particules**, le **son**, le **contour** ;
6. ce qui **n'est pas fait**, nommé.

Sources documentaires (tracées dans `docs/PROVENANCE.md` par l'orchestrateur) :

- archive du protocole 763 (`…wiki.vg_merge/Protocol?oldid=2773082`) —
  Set Block Destroy Stage, Player Action, Swing Arm, Entity Animation. **Ses
  identifiants autour de 0x06–0x08 sont faux** (piège 24 du briefing) : seuls
  les champs en ont été retenus ;
- minecraft.wiki, *Breaking* — la formule de vitesse (déjà mesurée côté
  serveur), le délai de 6 ticks entre deux blocs, Haste +20 %/niveau, Mining
  Fatigue ×0,3^min(n,4), l'eau ÷5 sans Aqua Affinity, en l'air ÷5 ;
- minecraft.wiki, *Particles (Java Edition)* — les particules `block` :
  texturées depuis le bloc, carrées, « une partie aléatoire de la texture »,
  elles « entrent en collision avec les blocs solides » et « disparaissent après
  une courte animation ». Rien sur les nombres.

---

## 1. Les paquets, identifiés par leur contenu

`scripts/capture_destroy_stage.py` (même méthode que `capture_sound_packets.py`) :
l'**acteur** casse en survie, l'**oreille** se tient à quatre blocs. Sur le vrai
serveur (`lockf /tmp/ov-vanilla.lock python3 scripts/capture_destroy_stage.py vanilla`) :

| Paquet | Id mesuré | Contenu |
|---|---|---|
| Set Block Destroy Stage | **0x07** | entité du casseur (VarInt), position, un octet signé |
| Acknowledge Block Change | 0x06 | séquence — l'acteur seul, à chaque Start, Abort et Finish |
| Entity Animation | 0x04 | entité, 0 = bras principal |

L'archive donne 0x08 et 0x05 : faux, comme pour Explosion. Les octets reçus sont
figés dans `src/ov_protocol/tests/test_breaking_packets.cpp`
(`010000004000003fc40c` : entité 1, (1, −60, 3), étape 12).

## 2. Ce que le serveur dit aux autres

Capture du 2026-09-11, trois gestes :

| Geste | L'oreille reçoit | L'acteur reçoit |
|---|---|---|
| pierre à la main, tenue ~200 ticks puis Abort | étapes 0, 1, … 12 (une tous les 15 ticks), puis **−1** | aucun 0x07 ; 0x06 au Start et à l'Abort |
| terre à la main, Finish à ~20 ticks (il en faut 15) | 0 … **14**, puis **−1**, puis World Event 2001 | aucun 0x07 |
| planches, Finish trop tôt (36 ticks sur 60) | 0 … **10**, World Event 2001, **pas de −1** | aucun 0x07 |
| Swing Arm seul | Entity Animation (entité, 0) | **rien** |

D'où la règle, implémentée dans `src/ov_server/src/destroy_stages.{hpp,cpp}` et
tenue par `test_destroy_stages.cpp` sur ces trois séquences :

- l'étape est **floor(compte × 10)** du compte *du serveur* (vitesse × ticks
  écoulés, le tick du Start compris), envoyée **à chaque changement**, dès 0 ;
- elle **dépasse 9** quand la réclamation du client tarde — un client n'en
  dessine que 0..9, le reste efface la fissure ;
- **−1** après un Abort et après une réclamation crue ; **rien** quand le
  serveur finit lui-même un bloc réclamé trop tôt ;
- **jamais au casseur**.

Deux écarts assumés, nommés : notre serveur envoie depuis la phase *Digs* de son
tick, donc l'étape 0 part **un tick après** le Start (vanilla l'envoie dans le
traitement du paquet), et le −1 d'une réclamation crue part au tick suivant,
après le World Event 2001 au lieu d'avant. Le rayon (32 blocs) est le nôtre :
l'archive dit « un certain rayon » et la sonde était à quatre blocs.

Le Swing Arm n'allait pas qu'aux autres chez nous : `CombatSession::on_swing`
diffusait à tous, le joueur compris. Corrigé (`CombatIo::broadcast_others`).

## 3. Le calendrier du client

`gameplay::DigController` (`src/ov_gameplay/include/ov/gameplay/dig_controller.hpp`),
pur et testé (`test_dig_controller.cpp`), un tick client (20 Hz) à la fois :

- **appui** sur un bloc : Start. En survie, un bloc dont la progression par
  tick vaut ≥ 1 part aussitôt, sans délai ; sinon le compte commence à 0 ;
- **maintien** : le compte avance d'un tick de progression — **le tick de
  l'appui compris** —, le son de frappe joue tous les 4 ticks du compte (le
  premier compris) ;
- le compte atteint 1 : Finish, le bloc part côté client, **délai de 5 ticks** ;
  le bloc suivant démarre au 6ᵉ tick après la casse (le « 6 ticks » du wiki) ;
- viser un autre bloc en maintenant : Abort puis Start ; relâcher ou viser le
  vide : Abort ; changer d'objet tenu : on recommence ;
- créatif : le bloc part à l'appui, puis un tous les 6 ticks tant qu'on
  maintient — le même délai ; le maintien du tick de l'appui trouve de l'air là
  où l'appui a cassé et n'en consomme rien. Une épée, un trident ou le bâton de
  débogage ne cassent rien.

La vitesse est **la même fonction que le serveur** (`BreakRules::destroy_progress`,
mesurée sur 985 blocs), nourrie côté client par l'objet tenu et ses
enchantements (NBT de la fenêtre 0), le casque (Aqua Affinity), les effets du
joueur (Entity Effect / Remove Entity Effect filtrés sur l'entité de Login),
l'eau à hauteur des yeux et le sol.

**Un fait de flottants** que le test fige : le compte est une somme de f32, et
150 × (1/1,5/100) vaut 0,99999 : la pierre à la main est réclamée au **151ᵉ**
tick, les planches au 61ᵉ, la terre au 15ᵉ. Le serveur, qui compte avec
ceil(1/vitesse) = 150, accepte la réclamation au tick suivant par son seuil 0,7.

Mesure contre le vrai client : § 7.

## 4. Les fissures

- **Sur le vrai modèle** : chaque quad du modèle cuit du bloc (dalle, escalier,
  torche, clôture…), `render::build_crack_quads` (`crack_mesh.hpp`).
- **Projetées depuis la position**, pas depuis les UV du sprite : chaque coin
  prend la coordonnée de sa position sur la face vers laquelle le quad regarde,
  texture droite vue de l'extérieur. Une dalle se fissure sur sa moitié, coupée
  où elle s'arrête ; un élément qui déborde continue (échantillonneur en
  répétition). L'**orientation** de chaque projection est la nôtre ; § 7 la
  confronte au vrai client.
- **Le mélange de vanilla** : DST_COLOR, SRC_COLOR, soit 2 × fissure × fond
  (`rhi::BlendMode::Multiply`) — un gris moyen ne change rien, un sombre
  assombrit, un clair éclaircit. Les dix étapes de Faithful 32x ne contiennent
  que **deux gris, 61 et 155**, alpha 0 ou 255 (relevé pixel par pixel).
- **Le gamma.** Vanilla mélange dans l'espace gamma (les valeurs stockées) ;
  notre cible est sRGB et le matériel mélange en linéaire. Avec sRGB ≈ puissance
  2,2, linéaire(2·s·d) = 2^2,2·lin(s)·lin(d), donc `crumbling.frag` sort
  2^1,2 × lin(s) : 61 donne ×0,215 en linéaire (vanilla ≈ 0,197), 155 donne
  ×1,51 (vanilla ≈ 1,54). Aucun des deux gris n'atteint la saturation.
- **Sans z-fighting** : le décalage de polygone de vanilla, pente −1 et
  constante −10 (`GraphicsPipelineDesc::depth_bias_*`), profondeur testée en
  LessOrEqual, jamais écrite.
- **Ce que montrent les captures du vrai client** (`crack-*-{none,0,4,9}.png`,
  2560 × 1440) : sur une dalle, la fissure couvre la demi-hauteur et s'arrête
  où la dalle s'arrête ; sur du verre, elle assombrit **l'herbe vue au
  travers** — le produit tombe sur ce qui est derrière les pixels transparents.
  Notre passe fait de même : elle n'écrit pas la profondeur et la teste contre
  un sol que le verre ne masque pas (le verre est découpé, pas opaque).
- **Qui** : la nôtre, depuis notre compte (étape = floor(compte × 10) − 1 : rien
  sous un dixième) ; celles des autres depuis 0x07, une par casseur ; si deux
  joueurs creusent le même bloc, la plus avancée. Une fissure dont personne ne
  parle depuis 400 ticks est oubliée (valeur non mesurée).

## 5. Particules, son, contour

**Particules** (`client::BlockParticles`, testé) — deux gestes :

- *casse* : une grille sur chaque boîte de la forme, au moins 2 cellules par
  côté et une par quart de bloc (4 × 4 × 4 = 64 pour un cube, 32 pour une dalle),
  chacune lancée depuis le centre de sa boîte ;
- *frappe* : une par tick de maintien, 0,1 hors de la face visée, plus lente
  (×0,2, portance gardée) et plus petite (×0,6).

Chacune vit 4/(r·0,9 + 0,1) ticks (4 à 40), tombe de 0,04 par tick, freine de
0,98, de 0,7 de plus au sol, s'arrête sur ce qu'elle touche
(`CollisionWorld::slide`, la même vue que la physique du joueur), et se dessine
comme un carré face à la caméra découpé dans un quart du sprite de particule du
modèle, à 0,6 de sa couleur (teinte du feuillage pour les blocs teintés, jamais
pour le bloc d'herbe). § 7.2 les a relues sur le vrai client : le nombre, la
durée de vie, les tailles, les vitesses, la gravité (1), le frottement (0,98),
la couleur (0,6) et la boîte concordent. **Restent non mesurés** : les 0,04 de
chute par tick et le ×0,7 au sol (lus comme des champs, pas vus agir), et la
vitesse verticale des éclats d'une face du dessus, plus lente chez le jeu (§ 7.2).
Les aléas sont les nôtres (graine explicite) — le jeu tire les siens d'une
source non graînée, donc aucune particule ne peut être identique, seulement les
comptes et les distributions.

Le World Event 2001 reçu fait la gerbe des casses des autres ; le son, déjà
joué par `SoundDirector`, reste là.

**Son** : la frappe joue au rythme du compte (`DigOutcome::hit_sound`,
`SoundDirector::hit`, volume (v+1)/8, pitch p×0,5 — le tableau du wiki, déjà
dans `son.md`), la casse au Finish (volume (v+1)/2, pitch p×0,8).

**Contour** : noir à 40 %, le long de la **forme** du bloc et plus d'un cube :
les boîtes de collision (mesurées), coupées au sommet du bloc (une clôture
entre en collision jusqu'à 1,5 mais se contourne à 1) ; pour un bloc sans
collision (torche, fleur, rail), la boîte englobant son modèle. Les arêtes sont
celles de l'**union** (`render::shape_edges`) : un escalier montre sa marche et
pas la couture entre ses deux boîtes. La vraie forme de contour n'est dans aucun
rapport ; pour une fleur, la boîte du modèle est plus large que celle du jeu.

## 6. Non fait, nommé

- **Le bras et l'objet tenu à la première personne** : notre client n'a pas de
  main à la première personne. Le balancement est *compté* (6 ticks, −1 par
  niveau de Haste, +2 par niveau de Mining Fatigue, relancé seulement à l'arrêt
  ou passé la moitié) et Swing Arm est envoyé à chaque tick où le bras bat —
  deux fois au tick de l'appui, comme le vrai client (§ 7.1) —, mais rien ne le
  dessine. Le bras d'un *autre* joueur (Entity Animation 0)
  n'est pas animé non plus.
- **La prédiction locale** : le bloc ne disparaît qu'au Block Update du serveur
  (quelques ms en intégré) ; particules et son partent, eux, au Finish.
- **La forme de contour mesurée**, et le décalage aléatoire des fleurs.
- **L'aventure** (`CanDestroy`) et le « missTime » de 10 ticks après un clic
  dans le vide.

## 7. Mesures contre le vrai client

`scripts/measure_breaking.py` démarre le vrai serveur et, devant lui, un
**proxy** qui horodate chaque paquet ; le vrai client (l'instance PrismLauncher,
pilotée par `scripts/breaking_oracle.java` : événements GLFW injectés dans son
propre `MouseHandler`, appels à ses propres méthodes nommées par les mappings)
rejoint par le proxy. Tout sous `lockf /tmp/ov-vanilla.lock`.

### 7.1 Le calendrier du client

Chaque scénario tient le bouton sur une colonne de trois blocs posée devant le
joueur. **Deux horloges** : le temps mur du proxy, et l'**horloge des swings** —
tant que le bouton est tenu le client envoie exactement **un Swing Arm par tick**
(1,003 à 1,017 par tick mesurés), ce qui compte les ticks même quand une machine
chargée fait rattraper au client plusieurs ticks d'un coup (la première passe,
sous charge, lisait 13 ticks pour de la terre qui en exige 15 : le temps mur ne
suffit pas). Deuxième passe (2026-09-11, `breaking_records.json`) :

| Scénario | 1ᵉʳ bloc (appui) | blocs suivants (maintien) | écart Finish→Start | prédiction | notre client |
|---|---|---|---|---|---|
| pierre, main | **151** | 152 | 6 | 151 · 152 · 6 | 151 · 152, 152 · 6 |
| terre, main | **15** | 16, 16 | 6, 6 | 15 · 16 · 6 | 15 · 16, 16 · 6 |
| pierre, pioche en bois | 28 | 24, 24 | 6, 6 | 23 (+5) · 24 · 6 | 23 · 24, 24 · 6 (aucun délai hérité : serveur neuf) |
| planches, hache en bois | 35 | 31, 31 | 6, 6 | 30 (+5) · 31 · 6 | 30 · 31, 31 · 6 (aucun délai hérité) |
| pierre, pioche en bois, Haste II | 22 | 18, 18 | 6, 6 | 17 (+5) · 18 · 6 | 17 · 18 (avant la correction de `poll` : 23 · 24, l'effet ne passait pas) |
| pierre, créatif | — | un bloc tous les **6** ticks | — | 6 | non mesuré de bout en bout (test : 6) |
| bloc de slime (instantané) | — | 6 puis **1** tick | — | 6 · 1 | non mesuré de bout en bout (test : 6 · 1) |

(ticks de temps mur, le tick du Start compté comme premier). **Tout concorde**,
y compris trois faits qu'aucune documentation ne donne :

- le **151ᵉ** tick de la pierre à la main : la somme en f32 ;
- le **délai de 5 ticks survit au relâchement** : il ne décompte que bouton
  tenu et un appui ne le remet pas à zéro, d'où les +5 des premiers blocs qui
  suivent un scénario terminé par une casse (et le 6 du slime, après le
  créatif). `DigController` fait de même ;
- le **créatif** casse tous les 6 ticks, l'appui compris.

Un écart trouvé et corrigé : sur le tick de l'appui, le vrai client envoie
**deux** Swing Arm (celui de l'appui et celui du tick de creusage) — l'horloge
des swings lit un tick de plus sur les seuls premiers blocs. `DigOutcome::swings`
en porte maintenant deux.

**Notre client, par le même proxy** (`measure_breaking.py ours-dig`, un serveur
neuf par scénario — voir § 8 pour la raison) : la colonne de droite du tableau.
Les deux horloges donnent chez nous ce qu'elles donnent chez le vrai client —
temps mur 151 · 152 · 152, horloge des swings 152 · 152 · 152 pour la pierre,
écarts de 6 partout, un Swing Arm par tick (1,002 à 1,014). Les « +5 » du vrai
client n'apparaissent pas chez nous parce que chaque scénario part d'un
serveur neuf, sans délai hérité du précédent.

Un piège de la mesure elle-même : sous charge (une compilation tournait), notre
serveur a pris plusieurs secondes de retard et a renvoyé **d'un bloc** les
acquittements et le Block Update d'une casse ; le client, qui voyait encore le
bloc, l'a recreusé. Le proxy le montre (tout arrive au même instant, 166 ticks
après le premier Start) : ce n'est pas le calendrier qui est en cause.

### 7.2 Les particules

Lues champ par champ sur les particules que `ClientLevel.addDestroyBlockEffect`
(6 casses) et `ParticleEngine.crack` (64 + 64 frappes) fabriquent :

| | vrai client | le nôtre |
|---|---|---|
| casse d'un cube | **64** | 64 |
| durée de vie | 4 à 37, moyenne 9,9 | 4/(r·0,9+0,1) : 4 à 40, moyenne 10,2 |
| demi-largeur (casse) | 0,050 à 0,0999 | 0,05 à 0,1 |
| vitesse verticale (casse) | −0,10 à 0,25 | −0,08 à 0,28 |
| vitesse horizontale (casse) | ≤ 0,163 | ≤ 0,18 |
| gravité, frottement, couleur, boîte | 1 · 0,98 · 0,6 · 0,2 | 1 · 0,98 · 0,6 · 0,2 |
| frappe : une par appel, demi-largeur, boîte | 1 · 0,030 à 0,060 · 0,12 | 1 · 0,03 à 0,06 · 0,12 |
| frappe, vitesse verticale (face nord) | 0,071 à 0,125, moyenne 0,098 | 0,064 à 0,136, moyenne 0,10 |
| frappe, vitesse verticale (face du dessus) | 0,035 à 0,118, moyenne 0,069 | 0,064 à 0,136 |

Un écart **nommé et non expliqué** : les éclats de la face du dessus partent
plus lentement vers le haut chez le vrai client. Les autres constantes de § 5
sont désormais mesurées.

### 7.3 Les fissures

Le vrai client pose l'étape par `ClientLevel.destroyBlockProgress` (l'appel que
fait Set Block Destroy Stage), le nôtre par `--crack` ; même pose
(`0.5 -60 0.5`, lacet 0, tangage 30), même pack (Faithful 32x), même définition
(2560 × 1440). `compare` divise, *dans chaque client*, l'image avec fissure par
l'image sans, sur un rectangle intérieur à la face nord du bloc (l'éclairage et
les objets au sol s'annulent) :

| | assombri / éclairci (vanilla) | assombri / éclairci (nous) | rapport sombre · clair (vanilla / nous) |
|---|---|---|---|
| pierre, étape 0 | 0,556 % / 0,438 % | 0,556 % / 0,438 % | 0,478 · 1,215 / 0,462 · 1,211 |
| pierre, étape 4 | 5,437 % / 4,309 % | 5,437 % / 4,302 % | 0,478 · 1,217 / 0,458 · 1,213 |
| pierre, étape 9 | 18,158 % / 13,737 % | 18,158 % / 13,730 % | 0,478 · 1,216 / 0,458 · 1,212 |
| planches, étape 9 | 18,158 % / 13,737 % | 18,158 % / 13,730 % | 0,478 · 1,215 / 0,464 · 1,210 |
| verre, étape 9 | 19,495 % / 14,978 % | 18,158 % / 13,728 % | 0,493 · 1,225 / 0,463 · 1,209 |
| dalle, étape 9 | 16,158 % / 13,089 % | 15,691 % / 11,917 % | 0,486 · 1,236 / 0,466 · 1,208 |
| dalle, étape 0 | 1,758 % / 1,681 % | 0,554 % / 0,490 % | 0,596 · 1,404 / 0,476 · 1,205 |

La **couverture est identique au pixel près** sur la face nord de la pierre et
des planches, à toutes les étapes : la projection (orientation comprise) est
celle du jeu. L'assombrissement est un peu plus fort chez nous (0,46 contre
0,48 : l'approximation de la courbe sRGB par une puissance 2,2, § 4) ;
l'éclaircissement concorde (1,21).

Le verre et la dalle se lisent mal, et pour la même raison : le rectangle voit
**ce qui est derrière** (à travers le verre, au-dessus de la demi-hauteur de la
dalle), et chez le vrai client les objets tombés des scénarios précédents y
tournent entre deux captures. Sa dalle « étape 0 » assombrit 1,76 % du
rectangle quand la fissure de l'étape 0 en couvre 0,556 % sur la pierre : le
reste est ce bruit. À l'étape 9, où la fissure domine, les deux concordent à
quelques dixièmes près.

## 8. Pièges

- **L'archive se trompe d'identifiant ici aussi** : Set Block Destroy Stage est
  0x07 et non 0x08, Entity Animation 0x04 et non 0x05.
- **Set Creative Slot est ignoré en survie** : la première capture « planches à
  la hache » a tourné à main nue (60 ticks = 2/1/30) — un outil se donne par la
  console (`item replace`), jamais par le paquet créatif.
- **Notre client ne peut pas rejoindre un vrai serveur 1.20.1** : le serveur le
  coupe à la connexion (« DecoderException: Index 8 out of bounds for length
  3 ») juste après Client Information, avec ou sans proxy. Antérieur à ce
  dossier : le binaire de `main` fait pareil. Les seuls paquets envoyés sont la
  poignée de main, Login Start et Client Information, dont les octets sont
  conformes à l'archive — la cause n'est pas trouvée. D'où la mesure de notre
  client contre notre serveur (`measure_breaking.py ours-dig`) : son calendrier
  ne dépend pas du serveur qui confirme.
- **Notre client ignorait Game Event 3 (changement de mode de jeu)** : il
  restait dans le mode de son Login. Après un `/gamemode survival`, il se
  croyait encore en créatif et « cassait » le même bloc tous les 6 ticks sans que
  le serveur — passé en survie, lui — le lâche jamais. Corrigé dans
  `ov_netclient` ; notre serveur envoyait bien le paquet.
- **`Client::poll` recopie la boîte de réception champ par champ** : un champ
  ajouté à `ClientEvents` sans sa ligne dans `poll` est lu sur le fil puis
  jeté, sans un mot. Les trois champs du cassage (0x07, l'entité du Login, les
  effets) l'ont été ; les tests unitaires, qui tiennent chaque morceau, ne
  pouvaient pas le voir — la mesure de bout en bout l'a vu (Haste II à 23 ticks
  au lieu de 17). Ajoute toujours le champ **et** sa ligne de `poll`.
- **Un client qui se reconnecte sous le même nom à notre serveur voit ses
  Player Action ignorées** : relevé au proxy (`breaking_records_ours.json`,
  deuxième passe), le serveur a envoyé les trois Block Update de la colonne,
  puis n'a répondu à **aucune** des huit actions du client — pas un Acknowledge
  Block Change (que le gestionnaire envoie pourtant en tête, pour toute action
  lue), pas un Block Update. Le client « cassait » donc le même bloc à
  l'infini. Sur la première connexion, tout passait. Non corrigé ici (hors du
  périmètre du cassage) ; `ours-dig` redémarre un serveur par scénario.
- **Un port « ouvert » n'est pas forcément le sien** : un autre agent écoutait
  sur 25653 ; notre serveur n'a pas pu s'y lier, la sonde a vu le port ouvert et
  le client est entré chez un inconnu. `OursServer` refuse maintenant un port
  déjà pris.
- **`pkill -f <motif>` tue aussi le shell qui a lancé la commande** quand le
  motif figure dans sa ligne de commande : viser le chemin de l'interpréteur
  (`MacOS/Python scripts/…`), jamais le texte de la commande.
- **Notre serveur ne comprend pas `item replace entity … hotbar.0 with …`**
  (« Unknown or incomplete command ») : une sonde qui donne un outil ainsi mesure
  la main nue sans le savoir. `/clear` puis `/give` pose l'outil dans la
  première case, tenue, sur les deux serveurs.
- **Une erreur de validation Vulkan à chaque image** (« depthAttachmentFormat
  UNDEFINED ») : antérieure aussi, 20 occurrences avec le binaire de `main`.
- **Le temps mur ne compte pas les ticks d'un client sous charge** : il
  rattrape plusieurs ticks d'un coup et groupe ses paquets. Compter les Swing
  Arm (un par tick, bouton tenu), pas les millisecondes.
- **Le délai de 5 ticks survit au relâchement** : un scénario qui suit une casse
  hérite de 5 ticks sur son premier bloc — c'est le jeu, pas la sonde.
- **`data/vanilla/1.20.1/normalized/` et `generated/` sont des liens vers le
  dépôt principal** dans un worktree : un script qui y écrit écrit hors du
  worktree. `capture_destroy_stage.py` et `measure_breaking.py` prennent un
  chemin de sortie ; lancés depuis un worktree, donne-leur `.scratch/`.
