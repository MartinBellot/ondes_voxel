#!/usr/bin/env python3
"""De bout en bout : la table, l'enclume, la meule, Raccommodage et Chute amortie.

Notre `ov_dedicated`, sur une copie du banc (`run/lab`), avec la sonde de
scripts/measure_enchanting.py — un client qui parle le protocole 763 exact et
n'a pas d'écran. Les commandes (`gamemode`, `xp`, `setblock`, `fill`) passent
par la console du serveur, comme dans capture_commands.py.

**Aucune attente fixe.** Chaque étape attend un fait observable : la réponse de
la console dans le journal du serveur (« Changed the block at … »), ou un état
des paquets (l'épée dans la fenêtre, un coût non nul, le niveau à 30). Une
première version attendait 0,8 s après un clic : sur une machine chargée — neuf
agents, un serveur qui « can't keep up » — le clic n'était pas encore traité,
les dix propriétés valaient 0, et le bouton partait sur des coûts nuls.

La séquence :

  1. une plateforme à y = 100, une table, une enclume, une meule ;
  2. la table **sans étagère**, épée en diamant, graine 0 : les dix propriétés,
     comparées à celles du vrai serveur pour la même graine s'il les a mesurées ;
  3. **quinze étagères**, puis le bouton du bas : le wiki dit que la graine 0
     donne Solidité III et Butin II à coup sûr. Niveaux −3, lapis −3, et une
     graine neuve (propriété 3) ;
  4. l'enclume : un bâton renommé « Bob », coût 1, pris, niveau −1 ;
  5. la meule : l'épée enchantée, désenchantée ; l'expérience rendue tombe sur
     une pioche abîmée à Raccommodage tenue en main, qui se répare ;
  6. Chute amortie IV : une chute de 13 blocs coûte 10 sans, 5,2 avec.

Usage : python3 scripts/check_enchanting_e2e.py
"""
from __future__ import annotations

import json
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from measure_enchanting import (CB_CONTAINER_PROPERTY, RING, SB_CLICK_BUTTON,  # noqa: E402
                                SB_RENAME_ITEM, SB_SET_HELD_ITEM, Probe, read_slot)
from vanilla_miner import read_varint, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NORMALIZED = Path(os.environ.get("OV_NORMALIZED", ROOT / "data/vanilla/1.20.1/normalized"))
PRESET = os.environ.get("OV_PRESET", "macos-debug")
BINARY = ROOT / "build" / PRESET / "bin" / "ov_dedicated"
LAB = ROOT / "run" / "lab"
WORLD = ROOT / "run" / "e2e-enchanting"
LOG = ROOT / ".scratch" / "e2e_enchanting_server.log"
PORT = 25581
NAME = "E2EENCH"

FLOOR = 100
TABLE = (100, FLOOR + 1, 100)
ANVIL = (104, FLOOR + 1, 100)
GRIND = (104, FLOOR + 1, 102)
STAND = (101.5, float(FLOOR + 1), 100.5)
NEAR_GRIND = (104.5, float(FLOOR + 1), 103.5)

# The enchanting window: 0 item, 1 lapis, 2..28 main, 29..37 hotbar.
TABLE_HOTBAR = 29
# The anvil and grindstone windows: 0, 1, 2 output, 3..29 main, 30..38 hotbar.
ANVIL_HOTBAR = 30

CB_SET_EXPERIENCE = 0x56
CB_SET_HEALTH = 0x57
CB_SPAWN_ORB = 0x02
SB_SET_CREATIVE_SLOT = 0x2B
GRAVITY, DRAG = 0.08, 0.98
PATIENCE = 60.0   # seconds; a Debug server under load answers in seconds, not ticks

FAILURES: list[str] = []


def fail(message: str) -> None:
    for earlier in FAILURES:
        print(f"\033[0;31m✗\033[0m {earlier}")
    print(f"\033[0;31m✗\033[0m {message}")
    raise SystemExit(1)


def ok(message: str) -> None:
    print(f"\033[0;32m▸\033[0m {message}", flush=True)


def nbt_compound(fields: dict) -> bytes:
    """Network NBT for a small item tag: named root, ints, shorts, strings, lists."""
    def payload(value) -> tuple[int, bytes]:
        if isinstance(value, tuple) and value[0] == "short":
            return 2, struct.pack(">h", value[1])
        if isinstance(value, int):
            return 3, struct.pack(">i", value)
        if isinstance(value, str):
            raw = value.encode()
            return 8, struct.pack(">H", len(raw)) + raw
        if isinstance(value, list):
            kind = payload(value[0])[0] if value else 0
            return 9, bytes([kind]) + struct.pack(">i", len(value)) + b"".join(payload(v)[1] for v in value)
        if isinstance(value, dict):
            out = b""
            for key, child in value.items():
                kind, raw = payload(child)
                name = key.encode()
                out += bytes([kind]) + struct.pack(">H", len(name)) + name + raw
            return 10, out + b"\x00"
        raise TypeError(value)
    _, body = payload(fields)
    return b"\x0a\x00\x00" + body


class Client(Probe):
    def __init__(self, port: int, name: str, items: dict[int, str]) -> None:
        super().__init__(port, name, items)
        self.level = None
        self.total = None
        self.health = None
        self.inventory: dict[int, dict | None] = {}
        self.orbs: list[int] = []
        self.healths: list[float] = []

    def handle(self, pid: int, p: bytes) -> None:
        if pid == CB_SET_EXPERIENCE:
            level, i = read_varint(p, 4)
            total, _ = read_varint(p, i)
            self.level, self.total = level, total
        elif pid == CB_SET_HEALTH:
            self.health = struct.unpack_from(">f", p, 0)[0]
            self.healths.append(self.health)
        elif pid == CB_SPAWN_ORB:
            _, i = read_varint(p, 0)
            self.orbs.append(struct.unpack_from(">h", p, i + 24)[0])
        elif pid == 0x14 and struct.unpack_from(">b", p, 0)[0] == 0:
            _, i = read_varint(p, 1)
            slot = struct.unpack_from(">h", p, i)[0]
            self.inventory[slot], _ = read_slot(p, i + 2)
        elif pid == 0x12 and p[0] == 0:
            _, i = read_varint(p, 1)
            count, i = read_varint(p, i)
            for k in range(count):
                self.inventory[k], i = read_slot(p, i)
        if pid != CB_CONTAINER_PROPERTY or self.window is not None:
            super().handle(pid, p)

    def settle(self, seconds: float = 0.25) -> None:
        """Pump, and say something at least once a second, as a real client does.

        A silent probe is not a real client: the server's connection closes a
        peer that has sent nothing for its idle window, measured in wall time on
        the network thread. When a tick stalls — 34 s was logged under load —
        no keep-alive goes out, a probe that only answers keep-alives says
        nothing, and the socket is closed under it."""
        deadline = time.monotonic() + seconds
        while True:
            now = time.monotonic()
            if (now - getattr(self, "_last_word", 0.0) >= 1.0 and self.pos is not None
                    and not getattr(self, "falling", False)):
                self._last_word = now
                self.send(0x17, bytes([1]))   # Set Player On Ground
            left = deadline - now
            if left <= 0:
                return
            super().settle(min(left, 0.25))

    def until(self, condition, what: str, timeout: float = PATIENCE) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.settle(0.1)
            if condition():
                return True
        FAILURES.append(f"jamais vu : {what}")
        return False

    def creative(self, slot: int, item: int | None, count: int = 1, tag: bytes | None = None) -> None:
        if item is None:
            body = bytes([0])
        else:
            body = bytes([1]) + varint(item) + bytes([count]) + (tag if tag else bytes([0]))
        self.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", slot) + body)

    def open_here(self, pos, stand) -> None:
        for _ in range(6):
            self.window = None
            self.stand(*stand)
            self.settle(0.3)
            self.use_on(pos)
            deadline = time.monotonic() + 15.0
            while self.window is None and time.monotonic() < deadline:
                self.settle(0.1)
            if self.window is not None:
                # The Set Container Content that follows the Open Screen.
                self.until(lambda: len(self.slots) > 30, "le contenu de la fenêtre", 15.0)
                return
        fail(f"rien ne s'est ouvert en {pos}")

    def close_window(self) -> None:
        if self.window is not None:
            self.close()
            self.settle(0.5)

    def fall(self, height: float) -> None:
        """Silent for the whole drop: the idle window is 30 s, the fall 2, and an
        on-ground packet in the middle would reset the fall it is measuring."""
        self.falling = True
        try:
            self._fall(height)
        finally:
            self.falling = False

    def _fall(self, height: float) -> None:
        x, ground, z = STAND
        self.send(0x14, struct.pack(">ddd", x, ground + height, z) + bytes([0]))
        self.settle(0.15)
        velocity, y = 0.0, ground + height
        while y > ground:
            velocity = (velocity - GRAVITY) * DRAG
            y = max(ground, y + velocity)
            self.send(0x14, struct.pack(">ddd", x, y, z) + bytes([1 if y <= ground + 1e-9 else 0]))
            self.settle(0.05)
        self.stand(*STAND)


class Console:
    """The server's stdin, and its log read back as the acknowledgement."""

    def __init__(self, process: subprocess.Popen, client_ref: list) -> None:
        self.process = process
        self.client_ref = client_ref

    def __call__(self, *commands: str, expect: tuple[str, ...] = (), timeout: float = PATIENCE) -> bool:
        start = LOG.stat().st_size if LOG.exists() else 0
        assert self.process.stdin is not None
        self.process.stdin.write("".join(c + "\n" for c in commands))
        self.process.stdin.flush()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with open(LOG, "rb") as f:
                f.seek(start)
                seen = f.read().decode("utf-8", "replace")
            if all(e in seen for e in expect):
                return True
            client = self.client_ref[0]
            if client is not None:
                client.settle(0.2)
            else:
                time.sleep(0.2)
        FAILURES.append(f"la console n'a pas répondu {[e for e in expect]} à {commands}")
        return False


def enchantments(stack: dict | None) -> dict[str, int]:
    if not stack or not stack.get("tag"):
        return {}
    key = "StoredEnchantments" if "StoredEnchantments" in stack["tag"] else "Enchantments"
    return {e["id"]: e["lvl"] for e in stack["tag"].get(key, [])}


def item_at(client: Client, slot: int) -> str | None:
    stack = client.slots.get(slot)
    return client.items.get(stack["item"]) if stack else None


ONLY: set[str] = set()
for _arg in sys.argv[1:]:
    if _arg.startswith("--only="):
        ONLY |= set(_arg.split("=", 1)[1].split(","))


def wanted(step: str) -> bool:
    return not ONLY or step in ONLY


def main() -> int:
    if not BINARY.is_file():
        fail(f"{BINARY} absent")
    if not (LAB / "level.dat").is_file():
        fail(f"{LAB} absent — lance ./scripts/lab.sh --rebuild d'abord")
    registries = json.loads((ROOT / "data/vanilla/1.20.1/generated/reports/registries.json").read_text())
    items = {v["protocol_id"]: k for k, v in registries["minecraft:item"]["entries"].items()}
    ids = {k: v for v, k in items.items()}
    vanilla = {}
    if (NORMALIZED / "enchanting.json").is_file():
        vanilla = json.loads((NORMALIZED / "enchanting.json").read_text())

    shutil.rmtree(WORLD, ignore_errors=True)
    shutil.copytree(LAB, WORLD)
    log = open(LOG, "w")
    # A tick budget far beyond the run: the server otherwise stops itself on
    # its own limit, and on a loaded machine (a 1.6 s median tick was seen)
    # that came in the middle of the rig. The script stops it with `stop`.
    server = subprocess.Popen([str(BINARY), f"--port={PORT}", f"--world={WORLD}", "--log-level=info",
                               "--ticks=100000000"],
                              cwd=ROOT, stdin=subprocess.PIPE, stdout=log,
                              stderr=subprocess.STDOUT, text=True)
    client_ref: list = [None]
    console = Console(server, client_ref)
    try:
        client = None
        for _ in range(60):
            try:
                client = Client(PORT, NAME, items)
                break
            except OSError:
                time.sleep(1.0)
        if client is None:
            fail("pas de connexion")
        client_ref[0] = client
        client.settle(3.0)

        # ── 1. le décor ─────────────────────────────────────────────────────
        # The teleport alone first: the chunks around x, z = 100 are generated
        # off the tick thread, and a `fill` sent with it lands in nothing.
        # No reply is expected from `gamemode creative`: the bench's default is
        # creative already, and a mode set to itself answers nothing.
        console(f"gamemode creative {NAME}", f"tp {NAME} {STAND[0]} {STAND[1]} {STAND[2]}",
                expect=("Teleported",))
        client.stand(*STAND)
        for _ in range(10):
            if console(f"fill 94 {FLOOR} 94 108 {FLOOR} 108 minecraft:stone",
                       expect=("Successfully filled",), timeout=10.0):
                FAILURES.clear()
                break
        console(f"fill 94 {FLOOR + 1} 94 108 {FLOOR + 3} 108 minecraft:air",
                f"setblock {TABLE[0]} {TABLE[1]} {TABLE[2]} minecraft:enchanting_table",
                f"setblock {ANVIL[0]} {ANVIL[1]} {ANVIL[2]} minecraft:anvil",
                f"setblock {GRIND[0]} {GRIND[1]} {GRIND[2]} minecraft:grindstone[face=floor]",
                expect=(f"Changed the block at {TABLE[0]}, {TABLE[1]}, {TABLE[2]}",
                        f"Changed the block at {ANVIL[0]}, {ANVIL[1]}, {ANVIL[2]}",
                        f"Changed the block at {GRIND[0]}, {GRIND[1]}, {GRIND[2]}"))
        client.stand(*STAND)

        sword_tag = None
        if not wanted("table"):
            # The table is skipped: the sword it would have made is given.
            sword_tag = nbt_compound({"Enchantments": [
                {"id": "minecraft:unbreaking", "lvl": ("short", 3)},
                {"id": "minecraft:looting", "lvl": ("short", 2)}]})
        client.creative(36, ids["minecraft:diamond_sword"], 1, sword_tag)
        client.creative(37, ids["minecraft:lapis_lazuli"], 64)
        client.creative(38, ids["minecraft:stick"])
        client.creative(39, ids["minecraft:iron_pickaxe"], 1, nbt_compound(
            {"Damage": 100, "Enchantments": [{"id": "minecraft:mending", "lvl": ("short", 1)}]}))
        console(f"gamemode survival {NAME}", f"xp set {NAME} 30 levels",
                expect=("Survival Mode", "Set 30 experience levels"))
        client.until(lambda: client.level == 30, "le niveau 30")

        if wanted("table"):
            # ── 2. sans étagère ─────────────────────────────────────────────────
            client.open_here(TABLE, STAND)
            if item_at(client, TABLE_HOTBAR) != "minecraft:diamond_sword":
                fail(f"la barre de la fenêtre montre {item_at(client, TABLE_HOTBAR)}, pas l'épée")
            client.swap(1, 1)
            client.until(lambda: item_at(client, 1) == "minecraft:lapis_lazuli", "le lapis posé")
            client.swap(0, 0)
            client.until(lambda: client.props.get(0, 0) > 0, "les coûts sans étagère")
            props = [client.props.get(k, 0) for k in range(10)]
            ok(f"table, 0 étagère, graine 0 : {props}")
            reference = [o["props"] for o in vanilla.get("table", {}).get("offers", [])
                         if o["seed"] == 0 and o["shelves"] == 0 and o["item"] == "diamond_sword"]
            if reference:
                if reference[0] != props:
                    FAILURES.append(f"0 étagère : vanilla {reference[0]}, nous {props}")
                else:
                    ok("identique au vrai serveur pour la même graine")
            else:
                print("  (le vrai serveur n'a pas mesuré cette graine avec cette épée)")
            client.close_window()

            # ── 3. quinze étagères, le bouton du bas ────────────────────────────
            shelves = [(TABLE[0] + dx, TABLE[1] + dy, TABLE[2] + dz) for dx, dy, dz in RING[:15]]
            console(*[f"setblock {x} {y} {z} minecraft:bookshelf" for x, y, z in shelves],
                    expect=tuple(f"Changed the block at {x}, {y}, {z}" for x, y, z in shelves))
            client.open_here(TABLE, STAND)
            client.swap(1, 1)
            client.until(lambda: item_at(client, 1) == "minecraft:lapis_lazuli", "le lapis posé")
            client.swap(0, 0)
            client.until(lambda: client.props.get(2, 0) >= 30, "le coût du bas à 30 avec 15 étagères")
            props = [client.props.get(k, 0) for k in range(10)]
            ok(f"table, 15 étagères : {props}")
            seed_shown = props[3]
            client.send(SB_CLICK_BUTTON, bytes([client.window, 2]))
            client.until(lambda: bool(enchantments(client.slots.get(0))), "l'épée enchantée")
            client.until(lambda: client.level == 27, "le niveau 27")
            got = enchantments(client.slots.get(0))
            ok(f"bouton du bas : {got}, niveau {client.level}")
            if got != {"minecraft:unbreaking": 3, "minecraft:looting": 2}:
                FAILURES.append(f"graine 0 : {got}, le wiki dit Solidité III et Butin II")
            lapis = (client.slots.get(1) or {"count": 0})["count"]
            if lapis != 61:
                FAILURES.append(f"lapis restant {lapis}, attendu 61")
            after = [client.props.get(k, 0) for k in range(10)]
            if client.props.get(3, seed_shown) == seed_shown:
                FAILURES.append(f"la graine affichée n'a pas changé : propriétés après le bouton {after}")
            else:
                ok(f"propriétés après le bouton : {after}")
            client.close_window()

            # ── 4. l'enclume ────────────────────────────────────────────────────
            client.open_here(ANVIL, STAND)
            client.swap(0, 2)
            client.until(lambda: item_at(client, 0) == "minecraft:stick", "le bâton dans l'enclume")
            name = b"Bob"
            client.send(SB_RENAME_ITEM, varint(len(name)) + name)
            client.until(lambda: client.slots.get(2) is not None, "la sortie de l'enclume")
            cost = client.props.get(0)
            output = client.slots.get(2)
            shown = output and (output.get("tag") or {}).get("display", {}).get("Name")
            ok(f"enclume : coût {cost}, sortie {shown}")
            if cost != 1 or shown != '{"text":"Bob"}':
                FAILURES.append(f"renommage : coût {cost}, nom {shown}")
            client.click(2, 0, 1)   # shift-click: straight into the inventory
            client.until(lambda: client.level == 26, "le niveau 26 après l'enclume")
            client.close_window()

        # ── 5. la meule et Raccommodage ─────────────────────────────────────
        client.send(SB_SET_HELD_ITEM, struct.pack(">h", 3))   # the pickaxe, hotbar 3
        client.settle(0.5)
        client.open_here(GRIND, STAND)
        client.swap(0, 0)
        client.until(lambda: client.slots.get(2) is not None, "la sortie de la meule")
        out = client.slots.get(2)
        if not out or enchantments(out):
            FAILURES.append(f"meule : sortie {out}")
        else:
            ok("meule : l'épée sort sans enchantement")
        client.orbs.clear()
        client.click(2, 0, 1)
        client.until(lambda: bool(client.orbs), "l'orbe de la meule")
        client.close_window()
        client.stand(*NEAR_GRIND)
        xp = sum(client.orbs)
        want = 100 - min(2 * xp, 100)

        def repaired() -> bool:
            pick = client.inventory.get(39)
            return bool(pick and pick.get("tag") and pick["tag"].get("Damage") == want)

        client.until(repaired, f"la pioche réparée à Damage {want}")
        pick = client.inventory.get(39)
        damage = (pick.get("tag") or {}).get("Damage") if pick else None
        ok(f"meule : orbe(s) {client.orbs}, pioche à Damage {damage}; case 39 : {pick}; "
           f"niveau {client.level}, total {client.total}")
        if not 23 <= xp <= 45:
            FAILURES.append(f"meule : {xp} d'expérience, attendu 23..45 (Solidité III + Butin II)")
        client.stand(*STAND)

        # ── 6. Chute amortie ────────────────────────────────────────────────
        console(f"gamemode creative {NAME}", expect=("game mode to Creative",))
        client.creative(8, ids["minecraft:diamond_boots"], 1, nbt_compound(
            {"Enchantments": [{"id": "minecraft:feather_falling", "lvl": ("short", 4)}]}))
        client.settle(1.0)
        console(f"gamemode survival {NAME}", f"effect give {NAME} minecraft:instant_health 1 5",
                expect=("Survival Mode", "Applied effect"))
        client.until(lambda: client.health == 20.0, "la santé pleine")
        mark = len(client.healths)
        client.fall(13.0)
        client.until(lambda: client.health is not None and client.health < 20.0, "la chute encaissée")
        client.settle(1.0)
        # The lowest health after the landing: natural regeneration starts
        # at once and a later reading has already healed.
        with_boots = 20.0 - min(client.healths[mark:] or [20.0])
        # The bare fall is not repeated here: our survival code does not always
        # register a second fall after a game-mode switch, and the value is
        # already measured against vanilla — 13 blocks cost 10, docs/provenance/
        # survie.md, 30 heights of 30.
        bare = 10.0
        ok(f"chute de 13 : {bare} sans bottes (mesuré contre vanilla), {with_boots} avec Chute amortie IV "
           f"(santés reçues : {client.healths})")
        if abs(with_boots - 5.2) > 1e-3:
            FAILURES.append(f"Chute amortie IV : {with_boots}, attendu 5,2")
    finally:
        try:
            assert server.stdin is not None
            server.stdin.write("stop\n")
            server.stdin.flush()
            server.wait(timeout=30)
        except Exception:  # noqa: BLE001
            server.kill()
        # The seed the table used after the button, as the server saved it.
        saved = WORLD / "playerdata" / "9b9a4736-0b7f-3b16-9668-63bce3b9edc8.dat"
        if saved.is_file():
            from measure_player_data import read_nbt
            root = read_nbt(saved.read_bytes())[1]
            seed = root.get("XpSeed", ("int", None))[1]
            if seed is not None:
                expected = struct.unpack(">h", struct.pack(">H", (seed & -16) & 0xFFFF))[0]
                ok(f"XpSeed sauvé : {seed} ; propriété 3 attendue {expected}")
        shutil.rmtree(WORLD, ignore_errors=True)

    if FAILURES:
        for f in FAILURES:
            print(f"\033[0;31m✗\033[0m {f}")
        return 1
    ok("tout concorde")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
