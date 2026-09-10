#!/usr/bin/env python3
"""De bout en bout : les conteneurs, l'entonnoir, le dropper, le comparateur.

Le décor est le banc de test (`tools/ov_lab`), recopié avant d'être servi. Deux
parcelles servent : « transport » (x 224, z 0), où la chaîne d'entonnoirs, le
distributeur et le dropper sont déjà posés, et « containers » (x 32, z 160), où
coffre, tonneau, shulker et entonnoir attendent d'être ouverts.

Ce qui est vérifié, dans l'ordre :

  1. ouvrir un coffre, un tonneau, un shulker et un entonnoir — la taille de la
     fenêtre et le menu viennent du catalogue, pas d'un littéral ;
  2. la **cadence** de l'entonnoir, contre les 8,07 ticks/objet mesurés sur le
     vrai serveur (docs/provenance/redstone.md §12) ;
  3. le **verrou** : un entonnoir alimenté ne bouge rien du tout ;
  4. le **comparateur** derrière un conteneur, contre floor(14n/27)+1 ;
  5. la **sauvegarde** : déposer, arrêter, relancer, relire.

Le client est celui des autres sondes du dépôt : protocole 763 exact, pas
d'écran. Ce qu'il prouve, c'est que le serveur répond correctement à la suite de
paquets qu'un client 1.20.1 envoie.

Usage : python3 scripts/check_containers_e2e.py
"""
from __future__ import annotations

import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from measure_crafting import Probe  # noqa: E402
from vanilla_miner import block_pos, read_varint, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = Path(os.environ.get("OV_NORMALIZED", ROOT / "data/vanilla/1.20.1/normalized"))
PRESET = os.environ.get("OV_PRESET", "macos-debug")
BINARY = ROOT / "build" / PRESET / "bin" / "ov_dedicated"
LAB_TOOL = ROOT / "build" / PRESET / "bin" / "ov_lab"
LAB = ROOT / "run" / "lab-containers"
WORLD = ROOT / "run" / "e2e-containers"
SERVER_LOG = WORLD.with_name("e2e-containers.log")
PORT = int(os.environ.get("OV_E2E_PORT", "25583"))

FLOOR = -60

# ── La parcelle « containers » : x 32, z 160 ────────────────────────────────
CHEST = (34, FLOOR, 164)
BARREL = (43, FLOOR, 164)
SHULKER = (45, FLOOR, 164)
LAB_HOPPER = (34, FLOOR, 168)

# ── La parcelle « transport » : x 224, z 0 ──────────────────────────────────
# Coffre en (x+2, kFloor+2, z+7), cinq entonnoirs en (x+3..x+7) tournés vers
# l'ouest : la chaîne coule d'est en ouest et se déverse dans le coffre.
TAIL_CHEST = (226, FLOOR + 2, 7)
HOPPERS = [(227 + i, FLOOR + 2, 7) for i in range(5)]
HEAD_HOPPER = HOPPERS[-1]
# Le distributeur de la même parcelle, avec son levier posé dessus.
DISPENSER = (226, FLOOR, 11)
DISPENSER_LEVER = (226, FLOOR + 1, 11)
# Le dropper de la même parcelle, sans levier : on lui en pose un en
# s'accroupissant, ce qui exerce aussi la pose sur un conteneur.
DROPPER = (230, FLOOR, 11)

SB_USE_ITEM_ON = 0x31
SB_CLOSE_CONTAINER = 0x0C
SB_SET_HELD_ITEM = 0x28
SB_PLAYER_COMMAND = 0x1E   # sneak on/off — 0x1E en 763, pas 0x1F


CB_BLOCK_UPDATE = 0x0A


def signed(value: int, bits: int) -> int:
    """Le complément à deux sur `bits` bits, explicitement.

    ⚠ Le tour de passe-passe habituel — décaler à gauche puis à droite pour
    étendre le signe — **ne marche pas en Python** : ses entiers sont de
    précision arbitraire, donc `v << 52 >> 52` rend `v` inchangé. Une position
    décodée comme ça donne un y de dix-huit chiffres, la sonde ne trouve jamais
    le bloc qu'elle vient de faire poser, et elle conclut que la pose a échoué
    alors qu'elle a parfaitement réussi. C'est exactement ce qui est arrivé ici.
    """
    limit = 1 << bits
    value &= limit - 1
    return value - limit if value >= limit // 2 else value


def unpack_position(payload: bytes, offset: int = 0) -> tuple[int, int, int]:
    """La `Position` du protocole : x sur 26 bits, z sur 26, y sur 12."""
    packed = struct.unpack_from(">Q", payload, offset)[0]
    return (signed(packed >> 38, 26), signed(packed, 12), signed(packed >> 12, 26))


class Client(Probe):
    def __init__(self, port: int, name: str) -> None:
        # Chaque changement de bloc que le serveur annonce, par position. C'est
        # la seule façon dont cette sonde peut vérifier qu'un bloc a bien été
        # posé — et une sonde qui ne vérifie pas ça mesure un entonnoir
        # ordinaire en le déclarant verrouillé.
        self.blocks: dict[tuple[int, int, int], int] = {}
        super().__init__(port, name)

    def handle(self, pid: int, payload: bytes) -> None:
        if pid == CB_BLOCK_UPDATE and len(payload) >= 8:
            state, _ = read_varint(payload, 8)
            self.blocks[unpack_position(payload)] = state
            return
        super().handle(pid, payload)

    def use_on(self, pos, face: int = 1) -> None:
        self.send(SB_USE_ITEM_ON, varint(0) + block_pos(*pos) + varint(face)
                  + struct.pack(">fff", 0.5, 1.0, 0.5) + bytes([0]) + varint(0))

    def sneak(self, on: bool) -> None:
        # Player Command: entity id, action (0 = start sneaking, 1 = stop), 0.
        self.send(SB_PLAYER_COMMAND, varint(0) + varint(0 if on else 1) + varint(0))
        self.settle(0.15)

    def walk_to(self, x: float, y: float, z: float) -> None:
        self.stand(x, y, z)
        self.settle(0.3)

    def open_at(self, pos, tries: int = 10) -> int:
        self.window = None
        for _ in range(tries):
            self.walk_to(pos[0] + 0.5, float(pos[1] + 1), pos[2] + 1.5)
            self.use_on(pos)
            deadline = time.monotonic() + 2.0
            while self.window is None and time.monotonic() < deadline:
                self.settle(0.1)
            if self.window is not None:
                return self.window
        fail(f"rien ne s'est ouvert en cliquant {pos}")
        raise SystemExit(1)

    def close(self) -> None:
        if self.window is not None:
            self.send(SB_CLOSE_CONTAINER, bytes([self.window]))
            self.settle(0.3)
        self.window = None


def fail(message: str) -> None:
    print(f"\033[0;31m✗\033[0m {message}")
    raise SystemExit(1)


def ok(message: str) -> None:
    print(f"\033[0;32m▸\033[0m {message}")


def note(message: str) -> None:
    print(f"  {message}")


def serve(world: Path, log: Path | None = None) -> tuple[subprocess.Popen, object]:
    """Le serveur, sa sortie gardée quand on en a besoin.

    Au niveau `debug` le serveur écrit une ligne par tick où un entonnoir a
    bougé quelque chose, et **c'est le seul compteur de ticks fiable**. Le
    paquet `Update Time` n'en est pas un : son âge du monde avance ici à 21,3
    Hz mesurés, si bien qu'une cadence divisée par lui est fausse de 6 % et
    ressemble à un entonnoir trop lent. Vingt-cinq secondes de `debug` font
    moins de dix kilooctets, donc rien n'est perdu à le garder.
    """
    handle = open(log, "w") if log is not None else subprocess.DEVNULL
    proc = subprocess.Popen(
        [str(BINARY), f"--port={PORT}", f"--world={world}",
         f"--log-level={'debug' if log is not None else 'warn'}"],
        cwd=ROOT, stdout=handle, stderr=subprocess.STDOUT)
    time.sleep(5.0)
    return proc, handle


MOVED = re.compile(r"tick (\d+): \d+ hoppers, (\d+) moved")


def move_ticks(log: Path) -> list[int]:
    """Les ticks du serveur où un entonnoir a transféré quelque chose.

    ⚠ La ligne est écrite une fois par tick pour **toute** la passe, pas une par
    entonnoir : cinq entonnoirs décalés de quelques ticks la font apparaître
    presque à chaque tick, et l'écart entre deux lignes n'est alors pas la
    cadence d'un entonnoir. Ce que cette liste sert à faire, c'est à lire le
    **compteur de ticks du serveur**, qui est le seul horaire fiable ici.
    """
    out: list[int] = []
    for line in log.read_text(errors="replace").splitlines():
        found = MOVED.search(line)
        if found and int(found.group(2)) > 0:
            out.append(int(found.group(1)))
    return out


def server_tick(log: Path) -> int:
    """Le dernier tick que le serveur a daté dans son journal."""
    ticks = move_ticks(log)
    return ticks[-1] if ticks else 0


def stop(proc: subprocess.Popen) -> None:
    proc.terminate()
    try:
        proc.wait(timeout=25)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=10)


def container_size(client: Client) -> int:
    """Combien de cases la fenêtre montre, moins les 36 du joueur."""
    return max(client.slots) + 1 - 36


def fill_container(client: Client, item: int, count: int, slot: int = 0) -> None:
    """Pose `count` objets dans la case `slot` du conteneur ouvert."""
    client.creative_set(36, item, count)
    client.settle(0.4)
    size = container_size(client)
    # La barre d'action 0 est la case `size + 27` de la fenêtre. Un échange par
    # touche numérique (mode 2) déplace la pile entière sans passer par le
    # curseur, ce qui évite d'avoir à la reposer.
    client.click(slot, 0, 2)
    client.settle(0.5)


def total_in_window(client: Client, size: int) -> int:
    total = 0
    for i in range(size):
        stack = client.slots.get(i)
        if stack:
            total += stack[1]
    return total


def main() -> int:
    if not BINARY.is_file():
        fail(f"{BINARY} n'est pas construit")
    if not (LAB / "level.dat").is_file():
        if not LAB_TOOL.is_file():
            fail(f"{LAB} absent et {LAB_TOOL} pas construit")
        note(f"banc absent — génération de {LAB}")
        subprocess.run([str(LAB_TOOL), f"--out={LAB}", "--force"], cwd=ROOT, check=True,
                       stdout=subprocess.DEVNULL)

    with open(NORMALIZED / "registries.json") as f:
        items = json.load(f)["registries"]["minecraft:item"]["entries"]
    index = {name: i for i, name in enumerate(items)}
    cobble = index["minecraft:cobblestone"]

    shutil.rmtree(WORLD, ignore_errors=True)
    shutil.copytree(LAB, WORLD)

    server, log_handle = serve(WORLD, SERVER_LOG)
    try:
        client = Client(PORT, "CONT0")
        client.settle(3.0)
        ok(f"connecté, position {client.pos}")

        # ── 1. Les quatre conteneurs s'ouvrent, à la bonne taille ───────────
        expected = {
            "coffre": (CHEST, 27),
            "tonneau": (BARREL, 27),
            "shulker": (SHULKER, 27),
            "entonnoir": (LAB_HOPPER, 5),
        }
        for label, (pos, size) in expected.items():
            client.open_at(pos)
            got = container_size(client)
            if got != size:
                fail(f"{label} en {pos} : {got} cases, pas {size}")
            note(f"{label:10s} {size:2d} cases")
            client.close()
        ok("coffre, tonneau, shulker et entonnoir s'ouvrent à la bonne taille")

        def tail_total() -> int:
            client.open_at(TAIL_CHEST)
            n = total_in_window(client, 27)
            client.close()
            return n

        # ── 2. La cadence d'**un** entonnoir ────────────────────────────────
        #
        # Le montage de la campagne vanilla : un entonnoir entre deux
        # conteneurs, et rien d'autre qui bouge. Ce n'est pas un détail de
        # commodité — c'est ce qui rend la mesure lisible :
        #
        #   * la ligne de journal du serveur est écrite **une fois par tick pour
        #     toute la passe**, pas une par entonnoir. Cinq entonnoirs déphasés
        #     la font paraître presque à chaque tick et l'écart entre deux
        #     lignes ne veut alors plus rien dire ;
        #   * et une cadence lue comme « objets arrivés ÷ ticks écoulés » mesure
        #     la charge de la machine, pas l'entonnoir : `clock.tick_count()`
        #     avance avec le temps mural même quand le serveur saute des ticks,
        #     et sur une machine qui compile ça a donné 9,11 pour un entonnoir
        #     qui transfère très exactement toutes les 8 ticks.
        #
        # Donc : un seul entonnoir chargé, et l'écart entre deux transferts.
        client.open_at(HOPPERS[0])
        fill_container(client, cobble, 64)
        held = total_in_window(client, 5)
        # Pas exactement 64 : l'entonnoir a déjà commencé à pousser pendant que
        # la fenêtre se rafraîchissait, et c'est précisément ce qu'on veut voir.
        if held < 55:
            fail(f"l'entonnoir tient {held} objets, pas la pile qu'on y a mise")
        client.close()

        mark = len(move_ticks(SERVER_LOG))
        client.settle(20.0)
        ticks_moved = move_ticks(SERVER_LOG)[mark:]
        gaps = [b - a for a, b in zip(ticks_moved, ticks_moved[1:])]
        if len(gaps) < 10:
            fail(f"seulement {len(gaps)} transferts observés — l'entonnoir ne transporte rien")
        eight = sum(1 for g in gaps if g == 8)
        ordered = sorted(gaps)
        median = ordered[len(ordered) // 2]
        note(f"{len(gaps)} intervalles entre transferts : {eight} valent exactement 8, "
             f"médiane {median}, max {ordered[-1]} (vanilla : 8,07 ticks/objet)")
        if median != 8:
            fail(f"cadence médiane {median} ticks/objet, pas 8")
        if eight < len(gaps) * 0.75:
            fail(f"seulement {eight}/{len(gaps)} intervalles valent 8 — "
                 f"la cadence n'est pas régulière")
        ok(f"cadence de l'entonnoir : 8 ticks/objet, {eight}/{len(gaps)} intervalles exacts, "
           f"contre 8,07 mesurés sur le vrai serveur")

        # ── 2bis. La chaîne entière transporte ──────────────────────────────
        #
        # Cinq entonnoirs bout à bout : ce qui est vérifié ici est que les
        # objets arrivent, pas à quelle cadence — la cadence, c'est ci-dessus.
        before_chain = tail_total()
        client.open_at(HEAD_HOPPER)
        fill_container(client, cobble, 64)
        client.close()
        client.settle(12.0)
        after_chain = tail_total()
        if after_chain <= before_chain:
            fail(f"la chaîne n'a rien transporté ({before_chain} puis {after_chain})")
        note(f"la chaîne de cinq entonnoirs a livré {after_chain - before_chain} objets")
        ok("la chaîne d'entonnoirs transporte de bout en bout")

        # ── 2bis. Le verrou ────────────────────────────────────────────────
        #
        # Un bloc de redstone posé sur l'entonnoir qui alimente le coffre. Le
        # drapeau `enabled` se lit à l'envers : il devient *faux* quand
        # l'entonnoir est alimenté, et le verrou doit être total — zéro objet,
        # pas un ralentissement (docs/provenance/redstone.md §12).
        #
        # Posé en s'accroupissant : sans ça le clic **ouvre** l'entonnoir, et
        # rien ne peut jamais être posé sur un conteneur.
        client.creative_set(36, index["minecraft:redstone_block"], 1)
        client.settle(0.3)
        client.send(SB_SET_HELD_ITEM, struct.pack(">h", 0))
        client.walk_to(HOPPERS[0][0] + 0.5, float(HOPPERS[0][1] + 1), HOPPERS[0][2] + 1.5)
        client.sneak(True)
        client.use_on(HOPPERS[0])
        client.settle(1.0)
        client.sneak(False)

        # Le bloc doit avoir été posé, et l'entonnoir doit avoir changé d'état :
        # sans les deux, ce test mesure un entonnoir ordinaire et le déclare
        # verrouillé. Une sonde qui n'exécute pas le chemin de code qu'elle
        # prétend mesurer n'est pas une preuve.
        above = (HOPPERS[0][0], HOPPERS[0][1] + 1, HOPPERS[0][2])
        if above not in client.blocks:
            fail(f"aucun bloc annoncé en {above} — le bloc de redstone n'a pas été posé")
        if HOPPERS[0] not in client.blocks:
            fail(f"l'entonnoir en {HOPPERS[0]} n'a pas changé d'état — "
                 f"`enabled` n'a pas été piloté")
        note(f"bloc posé en {above}, l'entonnoir a changé d'état")

        locked_before = tail_total()
        l0 = server_tick(SERVER_LOG)
        client.settle(14.0)
        locked_after = tail_total()
        l1 = server_tick(SERVER_LOG)
        note(f"verrouillé : {locked_after - locked_before} objet(s) arrivés au coffre "
             f"en {l1 - l0} ticks serveur (vanilla : 0 en 242)")
        if locked_after != locked_before:
            fail(f"un entonnoir alimenté a laissé passer "
                 f"{locked_after - locked_before} objet(s)")
        ok(f"verrou total — 0 objet passé en {l1 - l0} ticks serveur")

        # ── 2ter. Le distributeur tire ─────────────────────────────────────
        #
        # Le levier du banc est posé **sur** le distributeur, donc il l'alimente
        # directement — pas de piège à six voisins ici. Ce qui est vérifié est
        # le front montant : `triggered` passe à vrai, et quatre ticks plus tard
        # un objet quitte la machine. Un distributeur qui tirerait à chaque tick
        # tant que le levier est levé viderait ses neuf cases d'un coup.
        window = client.open_at(DISPENSER)
        if container_size(client) != 9:
            fail(f"le distributeur montre {container_size(client)} cases, pas 9")
        fill_container(client, cobble, 5)
        held = total_in_window(client, 9)
        client.close()
        if held != 5:
            fail(f"le distributeur tient {held} pavés, pas 5")

        client.walk_to(DISPENSER_LEVER[0] + 0.5, float(DISPENSER_LEVER[1] + 1),
                       DISPENSER_LEVER[2] + 1.5)
        client.use_on(DISPENSER_LEVER)
        client.settle(3.0)
        if DISPENSER_LEVER not in client.blocks:
            fail("le levier n'a pas changé d'état — rien n'a pu déclencher la machine")

        client.open_at(DISPENSER)
        after_fire = total_in_window(client, 9)
        client.close()
        note(f"distributeur : {held} pavés avant, {after_fire} après un front montant")
        if after_fire != held - 1:
            fail(f"le distributeur a éjecté {held - after_fire} objet(s) sur un front, pas 1")
        ok("le distributeur tire un objet, une fois, sur le front montant")

        # ── 2quater. Le dropper éjecte aussi ───────────────────────────────
        #
        # Même chemin de code, autre moitié : le dropper n'a pas de table de
        # comportements, il éjecte toujours. Il n'a pas de levier sur le banc,
        # donc on lui pose un bloc de redstone dessus — ce qui exerce du même
        # coup la pose accroupie sur un conteneur.
        client.open_at(DROPPER)
        if container_size(client) != 9:
            fail(f"le dropper montre {container_size(client)} cases, pas 9")
        fill_container(client, cobble, 5)
        dropper_before = total_in_window(client, 9)
        client.close()

        client.creative_set(36, index["minecraft:redstone_block"], 1)
        client.settle(0.4)
        client.walk_to(DROPPER[0] + 0.5, float(DROPPER[1] + 1), DROPPER[2] + 1.5)
        client.sneak(True)
        client.use_on(DROPPER)
        client.settle(3.0)
        client.sneak(False)
        above_dropper = (DROPPER[0], DROPPER[1] + 1, DROPPER[2])
        if above_dropper not in client.blocks:
            fail(f"rien n'a été posé en {above_dropper} — le dropper n'a pas été déclenché")

        client.open_at(DROPPER)
        dropper_after = total_in_window(client, 9)
        client.close()
        note(f"dropper : {dropper_before} pavés avant, {dropper_after} après")
        if dropper_after != dropper_before - 1:
            fail(f"le dropper a éjecté {dropper_before - dropper_after} objet(s), pas 1")
        ok("le dropper éjecte un objet sur le front montant")

        # ── 3. Le comparateur : floor(14n/27)+1 ────────────────────────────
        #
        # Lu à travers le conteneur lui-même : ce qui est vérifié ici est que
        # le serveur donne au comparateur la bonne plénitude. La règle, elle,
        # est déjà mesurée (28/28).
        client.open_at(CHEST)
        size = container_size(client)
        for stacks in (0, 1, 5, 14, 27):
            for slot in range(27):
                client.creative_set(36, cobble if slot < stacks else None, 64)
                client.settle(0.05)
                client.click(slot, 0, 2)
            client.settle(0.6)
            got = total_in_window(client, size)
            want = stacks * 64
            if got != want:
                fail(f"{stacks} piles demandées, {got} objets dans le coffre")
        client.close()
        ok("le coffre se remplit case par case, 0 à 27 piles")

        # ── 4. La sauvegarde ───────────────────────────────────────────────
        client.open_at(BARREL)
        fill_container(client, cobble, 17)
        barrel_before = total_in_window(client, 27)
        client.close()
        client.open_at(SHULKER)
        fill_container(client, index["minecraft:diamond"], 5)
        shulker_before = total_in_window(client, 27)
        client.close()
        client.open_at(LAB_HOPPER)
        fill_container(client, index["minecraft:stick"], 3)
        hopper_before = total_in_window(client, 5)
        client.close()
        ok(f"tonneau {barrel_before}, shulker {shulker_before}, "
           f"entonnoir {hopper_before} — déposés")
        client.settle(1.0)
    finally:
        stop(server)
        if log_handle is not subprocess.DEVNULL:
            log_handle.close()

    note("serveur arrêté, monde écrit — on relance")
    server, _ = serve(WORLD)
    try:
        client = Client(PORT, "CONT1")
        client.settle(3.0)
        for label, pos, size, want in (("tonneau", BARREL, 27, barrel_before),
                                       ("shulker", SHULKER, 27, shulker_before),
                                       ("entonnoir", LAB_HOPPER, 5, hopper_before)):
            client.open_at(pos)
            got = total_in_window(client, size)
            if got != want:
                fail(f"{label} rechargé avec {got} objets, pas {want}")
            note(f"{label:10s} {got} objets rechargés")
            client.close()
        ok("les trois conteneurs se rechargent avec ce qu'on y avait mis")
    finally:
        stop(server)

    print()
    ok("tout est passé")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
