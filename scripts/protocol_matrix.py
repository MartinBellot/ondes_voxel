#!/usr/bin/env python3
"""The protocol 763 conformity matrix, generated from the code so it cannot rot.

Every packet of every state and direction — the 176 of data/protocol/763.json —
with what the code actually has for it:

  constant   an id constant in the code, and **its value is checked** against the
             catalogue: a constant whose number disagrees with its name's
             catalogue entry is an error, not a table cell
  encoder    an encode_* function in ov_protocol for it
  decoder    a parse_* / decode_* function in ov_protocol for it
  test       a Catch2 TEST_CASE calls one of them
  round trip one TEST_CASE calls both the encoder and the decoder
  vanilla    one TEST_CASE calls them over a hex literal and says where the
             bytes came from (the real server, vanilla, a capture)
  server     the id constant is used by ov_server
  client     the id constant is used by ov_netclient

Functions are attributed to packets by name — encode_<packet>, parse_<packet>,
with the packet named either the way our constant does or the way the wiki
does — plus the explicit table FUNCTIONS below for what the names cannot say.
Anything in that table that no longer exists is an error, so a rename cannot
leave it pointing at nothing.

  python3 scripts/protocol_matrix.py           # rewrite docs/protocol/763/README.md
  python3 scripts/protocol_matrix.py --check   # CI: fail if it is stale or an id is wrong
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CATALOG = ROOT / "data" / "protocol" / "763.json"
OUT = ROOT / "docs" / "protocol" / "763" / "README.md"
PROTOCOL_HEADERS = ROOT / "src" / "ov_protocol" / "include" / "ov" / "protocol"

# Constants whose name does not normalise to the wiki's. Key: the constant as
# written (namespace-qualified when it lives in clientbound::/serverbound::),
# value: the wiki name. Direction comes from the namespace, or from the entry.
CONSTANT_NAMES: dict[str, tuple[str, str]] = {
    "clientbound::kAcknowledgeDig": ("clientbound", "Acknowledge Block Change"),
    "clientbound::kChunkDataAndLight": ("clientbound", "Chunk Data and Update Light"),
    "clientbound::kLoginPlay": ("clientbound", "Login (play)"),
    "clientbound::kDisconnect": ("clientbound", "Disconnect (play)"),
    "clientbound::kSynchronizePosition": ("clientbound", "Synchronize Player Position"),
    "clientbound::kSetDefaultSpawn": ("clientbound", "Set Default Spawn Position"),
    "clientbound::kEntityHeadRotation": ("clientbound", "Set Head Rotation"),
    "clientbound::kEntityTeleport": ("clientbound", "Teleport Entity"),
    "clientbound::kContainerContent": ("clientbound", "Set Container Content"),
    "clientbound::kContainerSlot": ("clientbound", "Set Container Slot"),
    "clientbound::kContainerProperty": ("clientbound", "Set Container Property"),
    "clientbound::kEntityMetadata": ("clientbound", "Set Entity Metadata"),
    "clientbound::kTakeItem": ("clientbound", "Pickup Item"),
    "clientbound::kEntityPosition": ("clientbound", "Update Entity Position"),
    "clientbound::kEntityPositionRotation": ("clientbound", "Update Entity Position and Rotation"),
    "clientbound::kEntityRotation": ("clientbound", "Update Entity Rotation"),
    "clientbound::kEntityVelocity": ("clientbound", "Set Entity Velocity"),
    "clientbound::kEntityEquipment": ("clientbound", "Set Equipment"),
    "clientbound::kCommandSuggestions": ("clientbound", "Command Suggestions Response"),
    "clientbound::kPlayerChat": ("clientbound", "Player Chat Message"),
    "clientbound::kSystemChat": ("clientbound", "System Chat Message"),
    "clientbound::kDisguisedChat": ("clientbound", "Disguised Chat Message"),
    "serverbound::kConfirmTeleport": ("serverbound", "Confirm Teleportation"),
    "serverbound::kSetPlayerPositionRot": ("serverbound", "Set Player Position and Rotation"),
    "serverbound::kSetCreativeSlot": ("serverbound", "Set Creative Mode Slot"),
    "serverbound::kCommandSuggestionsRequest": ("serverbound", "Command Suggestions Request"),
    # Constants local to ov_server, outside the protocol namespaces.
    "kSetPassengers": ("clientbound", "Set Passengers"),
    "kClickContainerButton": ("serverbound", "Click Container Button"),
    "kRenameItem": ("serverbound", "Rename Item"),
    "kSelectTrade": ("serverbound", "Select Trade"),
    "kMerchantOffers": ("clientbound", "Merchant Offers"),
    "kBossBarPacket": ("clientbound", "Boss Bar"),
}

# Enumerators of the pre-play states, resolved the same way.
ENUM_NAMES: dict[str, tuple[str, str, str]] = {
    "StatusPacket::Request": ("status", "serverbound", "Status Request"),
    "StatusPacket::Response": ("status", "clientbound", "Status Response"),
    "StatusPacket::Ping": ("status", "serverbound", "Ping Request"),
    "StatusPacket::Pong": ("status", "clientbound", "Ping Response"),
    "LoginPacket::Start": ("login", "serverbound", "Login Start"),
    "LoginPacket::Disconnect": ("login", "clientbound", "Disconnect (login)"),
    "LoginPacket::SetCompression": ("login", "clientbound", "Set Compression"),
    "LoginPacket::Success": ("login", "clientbound", "Login Success"),
}

# Functions the naming rule cannot attribute, or attributes ambiguously (a
# packet name shared by both directions). Key: (state, direction, wiki name).
FUNCTIONS: dict[tuple[str, str, str], list[str]] = {
    ("handshaking", "serverbound", "Handshake"): ["parse_handshake"],
    ("status", "clientbound", "Status Response"): ["encode_status_response"],
    ("status", "clientbound", "Ping Response"): ["encode_pong"],
    ("login", "serverbound", "Login Start"): ["parse_login_start"],
    ("login", "clientbound", "Login Success"): ["encode_login_success"],
    ("login", "clientbound", "Set Compression"): ["encode_set_compression"],
    ("login", "clientbound", "Disconnect (login)"): ["encode_login_disconnect"],
    ("play", "clientbound", "Disconnect (play)"): ["encode_play_disconnect"],
    ("play", "clientbound", "Keep Alive"): ["encode_keep_alive"],
    ("play", "serverbound", "Keep Alive"): ["parse_keep_alive"],
    ("play", "clientbound", "Close Container"): ["encode_close_container",
                                                  "parse_clientbound_close_container"],
    ("play", "serverbound", "Close Container"): ["parse_close_container",
                                                  "encode_serverbound_close_container"],
    ("play", "clientbound", "Change Difficulty"): ["encode_change_difficulty"],
    ("play", "serverbound", "Change Difficulty"): ["parse_change_difficulty"],
    ("play", "clientbound", "Player Abilities"): ["encode_player_abilities"],
    ("play", "clientbound", "Set Held Item"): [],
    ("play", "serverbound", "Set Held Item"): ["parse_set_held_item"],
    ("play", "clientbound", "Acknowledge Block Change"): ["encode_acknowledge_dig"],
    ("play", "clientbound", "Chunk Data and Update Light"): ["encode_chunk_data",
                                                              "parse_chunk_data"],
    ("play", "clientbound", "Login (play)"): ["encode_login_play"],
    ("play", "clientbound", "Synchronize Player Position"): [
        "encode_synchronize_position", "encode_synchronize_position_relative"],
    ("play", "clientbound", "Player Info Update"): ["encode_player_info_add",
                                                     "encode_player_info_game_mode"],
    ("play", "clientbound", "Remove Entities"): ["encode_remove_entities", "encode_remove_entity"],
    ("play", "clientbound", "Set Entity Metadata"): ["encode_entity_metadata",
                                                      "encode_item_metadata"],
    ("play", "clientbound", "Set Head Rotation"): ["encode_entity_head_rotation"],
    ("play", "clientbound", "Pickup Item"): ["encode_take_item"],
    ("play", "clientbound", "Set Default Spawn Position"): ["encode_set_default_spawn"],
    ("play", "clientbound", "Set Container Property"): ["encode_container_property"],
    ("play", "clientbound", "Look At"): ["encode_look_at", "encode_look_at_entity"],
    ("play", "clientbound", "Update Attributes"): ["encode_update_attributes",
                                                    "encode_update_attributes_full",
                                                    "decode_update_attributes"],
    ("play", "clientbound", "Set Title Text"): ["encode_component_packet"],
    ("play", "clientbound", "Set Subtitle Text"): ["encode_component_packet"],
    ("play", "clientbound", "Set Action Bar Text"): ["encode_component_packet"],
    ("play", "clientbound", "Set Title Animation Times"): ["encode_title_animation_times"],
    ("play", "clientbound", "Command Suggestions Response"): ["encode_suggestions_response",
                                                               "parse_suggestions_response"],
    ("play", "serverbound", "Command Suggestions Request"): ["encode_suggestions_request",
                                                              "parse_suggestions_request"],
    ("play", "serverbound", "Message Acknowledgment"): ["parse_message_acknowledgment"],
    ("play", "serverbound", "Set Player Position"): ["parse_movement"],
    ("play", "serverbound", "Set Player Position and Rotation"): ["parse_movement"],
    ("play", "serverbound", "Set Player Rotation"): ["parse_movement"],
    ("play", "serverbound", "Set Player On Ground"): ["parse_movement"],
    ("play", "serverbound", "Set Creative Mode Slot"): ["parse_set_creative_slot"],
    ("play", "serverbound", "Click Container"): ["parse_container_click", "encode_container_click"],
    ("play", "serverbound", "Confirm Teleportation"): ["parse_confirm_teleport"],
    ("play", "clientbound", "Update Section Blocks"): ["encode_update_section_blocks",
                                                       "parse_update_section_blocks"],
    ("play", "clientbound", "Set Block Destroy Stage"): ["encode_block_destroy_stage",
                                                         "parse_block_destroy_stage"],
    ("play", "clientbound", "Disguised Chat Message"): ["encode_disguised_chat",
                                                         "parse_disguised_chat"],
    ("play", "clientbound", "Player Chat Message"): ["encode_player_chat", "parse_player_chat"],
    ("play", "clientbound", "System Chat Message"): ["encode_system_chat", "parse_system_chat"],
}

STATE_ORDER = ["handshaking", "status", "login", "play"]
STATE_TITLE = {"handshaking": "Handshaking", "status": "Status", "login": "Login", "play": "Play"}
DIRECTION_TITLE = {"clientbound": "serveur → client", "serverbound": "client → serveur"}

FUNCTION_RE = re.compile(r"\b((?:encode|parse|decode)_[a-z0-9_]+)\s*\(")
CONSTANT_RE = re.compile(r"constexpr\s+i32\s+(k[A-Za-z0-9]+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*;")
HEX_LITERAL_RE = re.compile(r'"(?:[0-9a-fA-F]{2}){4,}"')
PROVENANCE_RE = re.compile(r"vanilla|real 1\.20\.1|real server|captur|vrai serveur", re.IGNORECASE)


def normalise(name: str) -> str:
    return re.sub(r"[^a-z0-9]", "", name.lower())


def snake(name: str) -> str:
    """'Set Title Text' -> 'set_title_text', 'kSetTitleText' -> 'set_title_text'."""
    if name.startswith("k") and name[1:2].isupper():
        name = name[1:]
    name = re.sub(r"\(.*?\)", "", name)
    name = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", name)
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")


def rel(path: Path) -> str:
    return str(path.relative_to(ROOT))


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


class Matrix:
    def __init__(self) -> None:
        catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
        self.packets = catalog["packets"]
        self.by_name: dict[tuple[str, str, str], dict] = {}
        self.by_id: dict[tuple[str, str, int], dict] = {}
        for p in self.packets:
            p["key"] = (p["state"], p["direction"], p["name"])
            p["num"] = int(p["id"], 16)
            p["constants"] = []
            p["functions"] = []
            self.by_name[p["key"]] = p
            self.by_id[(p["state"], p["direction"], p["num"])] = p
        self.errors: list[str] = []

    # ── constants ───────────────────────────────────────────────────────────
    def find_play_packet(self, direction: str, name: str) -> dict | None:
        wanted = normalise(name)
        for p in self.packets:
            if p["state"] == "play" and p["direction"] == direction and normalise(p["name"]) == wanted:
                return p
        return None

    def attach_constant(self, qualified: str, direction: str | None, value: int, where: str,
                        strict: bool) -> None:
        short = qualified.split("::")[-1]
        packet = None
        if qualified in CONSTANT_NAMES:
            want_dir, wiki = CONSTANT_NAMES[qualified]
            packet = self.by_name.get(("play", want_dir, wiki))
            if packet is None:
                self.errors.append(f"{where}: {qualified} maps to {wiki!r}, which the catalogue lacks")
                return
        elif direction is not None:
            packet = self.find_play_packet(direction, short[1:])
        if packet is None:
            if strict:
                self.errors.append(f"{where}: {qualified} = 0x{value:02X} names no packet of the "
                                   f"catalogue; add it to CONSTANT_NAMES in {rel(Path(__file__))}")
            return
        if packet["num"] != value:
            self.errors.append(f"{where}: {qualified} = 0x{value:02X}, but {packet['name']!r} "
                               f"({packet['direction']}) is {packet['id']} in protocol 763")
        packet["constants"].append((qualified, where))

    def scan_constants(self) -> None:
        for header in sorted(PROTOCOL_HEADERS.glob("*.hpp")):
            text = strip_comments(header.read_text(encoding="utf-8"))
            for block in re.finditer(r"namespace\s+(clientbound|serverbound)\s*\{(.*?)\}", text,
                                     re.DOTALL):
                direction = block.group(1)
                for m in CONSTANT_RE.finditer(block.group(2)):
                    self.attach_constant(f"{direction}::{m.group(1)}", direction,
                                         int(m.group(2), 0), rel(header), strict=True)
            for enum in re.finditer(r"enum\s+class\s+(\w+)\s*:\s*i32\s*\{(.*?)\}", text, re.DOTALL):
                for m in re.finditer(r"(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)", enum.group(2)):
                    qualified = f"{enum.group(1)}::{m.group(1)}"
                    if qualified not in ENUM_NAMES:
                        continue
                    state, direction, wiki = ENUM_NAMES[qualified]
                    packet = self.by_name[(state, direction, wiki)]
                    if packet["num"] != int(m.group(2), 0):
                        self.errors.append(f"{rel(header)}: {qualified} = {m.group(2)}, but "
                                           f"{wiki!r} is {packet['id']}")
                    packet["constants"].append((qualified, rel(header)))
        for folder in ("ov_server", "ov_netclient", "ov_sim", "ov_gameplay"):
            for source in sorted((ROOT / "src" / folder).rglob("*.[hc]pp")):
                if "/tests/" in str(source):
                    continue
                text = strip_comments(source.read_text(encoding="utf-8"))
                for m in CONSTANT_RE.finditer(text):
                    if m.group(1) in CONSTANT_NAMES:
                        self.attach_constant(m.group(1), None, int(m.group(2), 0), rel(source),
                                             strict=False)

    # ── functions ───────────────────────────────────────────────────────────
    def scan_functions(self) -> None:
        declared: set[str] = set()
        for header in PROTOCOL_HEADERS.glob("*.hpp"):
            declared |= set(FUNCTION_RE.findall(strip_comments(header.read_text(encoding="utf-8"))))
        self.declared = declared

        for key, names in FUNCTIONS.items():
            if key not in self.by_name:
                self.errors.append(f"FUNCTIONS names {key}, which the catalogue lacks")
                continue
            for name in names:
                if name not in declared:
                    self.errors.append(f"FUNCTIONS: {name} is not declared in ov_protocol "
                                       f"(renamed or removed?)")
            self.by_name[key]["functions"] = list(names)

        claimed = {n for names in FUNCTIONS.values() for n in names}
        for p in self.packets:
            if p["key"] in FUNCTIONS:
                continue
            stems = {snake(p["name"])}
            stems |= {snake(q.split("::")[-1]) for q, _ in p["constants"]}
            found = []
            for fn in sorted(declared - claimed):
                body = fn.split("_", 1)[1]
                if body in stems:
                    found.append(fn)
            p["functions"] = found

    # ── tests ───────────────────────────────────────────────────────────────
    def scan_tests(self) -> None:
        self.blocks: list[tuple[str, set[str], bool]] = []
        for test in sorted((ROOT / "src").glob("*/tests/*.cpp")):
            # A fuzz harness calls every decoder and checks only that nothing
            # crashes; counting it would mark every packet "tested".
            if test.name.startswith("fuzz_"):
                continue
            text = test.read_text(encoding="utf-8")
            parts = re.split(r"(?=\bTEST_CASE\s*\()", text)
            header = parts[0]
            file_says = bool(PROVENANCE_RE.search(header))
            for block in parts[1:]:
                functions = set(FUNCTION_RE.findall(block))
                if not functions:
                    continue
                captured = bool(HEX_LITERAL_RE.search(block)) and (
                    file_says or bool(PROVENANCE_RE.search(block)))
                self.blocks.append((rel(test), functions, captured))

        uses_server = self.usage("ov_server")
        uses_client = self.usage("ov_netclient")
        for p in self.packets:
            enc = {f for f in p["functions"] if f.startswith("encode_")}
            dec = {f for f in p["functions"] if not f.startswith("encode_")}
            p["encoder"] = sorted(enc)
            p["decoder"] = sorted(dec)
            p["tested"] = any(fns & (enc | dec) for _, fns, _ in self.blocks)
            p["round_trip"] = bool(enc and dec) and any(
                (fns & enc) and (fns & dec) for _, fns, _ in self.blocks)
            p["vanilla"] = any(cap and (fns & (enc | dec)) for _, fns, cap in self.blocks)
            short = {q.split("::")[-1] for q, _ in p["constants"]}
            p["server"] = bool(short & uses_server)
            p["client"] = bool(short & uses_client)

    def usage(self, module: str) -> set[str]:
        used: set[str] = set()
        for source in (ROOT / "src" / module).rglob("*.[hc]pp"):
            if "/tests/" in str(source):
                continue
            used |= set(re.findall(r"\b(k[A-Z][A-Za-z0-9]+)\b",
                                   strip_comments(source.read_text(encoding="utf-8"))))
        return used

    # ── output ──────────────────────────────────────────────────────────────
    def status(self, p: dict) -> str:
        if not p["constants"] and not p["functions"]:
            return "absent"
        if p["vanilla"]:
            return "vanilla"
        if p["round_trip"]:
            return "aller-retour"
        if p["tested"]:
            return "testé"
        if p["functions"]:
            return "codé"
        return "id seul"

    def render(self) -> str:
        yes, no = "✓", "·"
        lines: list[str] = []
        total = len(self.packets)
        counts: dict[str, int] = {}
        for p in self.packets:
            counts[self.status(p)] = counts.get(self.status(p), 0) + 1
        implemented = total - counts.get("absent", 0)
        lines += [
            "# Matrice de conformité — protocole 763 (Java 1.20.1)",
            "",
            "> **Fichier généré** par `scripts/protocol_matrix.py` — ne pas éditer à la main.",
            "> `python3 scripts/protocol_matrix.py --check` échoue en CI s'il est périmé, ou si",
            "> une constante d'id du code contredit le catalogue.",
            "",
            "Catalogue : `data/protocol/763.json` (176 paquets), dérivé de deux sources qui",
            "s'accordent sur chaque id — PrismarineJS/minecraft-data (MIT) et l'archive figée",
            "wiki.vg (oldid 2773082). Voir `docs/provenance/protocole-763.md`.",
            "",
            "## Légende",
            "",
            "| Colonne | Sens |",
            "|---|---|",
            "| Constante | un id nommé dans le code, **valeur vérifiée** contre le catalogue |",
            "| Enc. / Déc. | une fonction `encode_*` / `parse_*`·`decode_*` publique de `ov_protocol` |",
            "| Test | un `TEST_CASE` appelle l'une d'elles |",
            "| A/R | un même `TEST_CASE` appelle l'encodeur **et** le décodeur (aller-retour) |",
            "| Vanilla | un `TEST_CASE` les appelle sur des octets en hexadécimal dont il dit "
            "qu'ils viennent du vrai serveur |",
            "| Srv / Cli | la constante est utilisée par `ov_server` / `ov_netclient` |",
            "",
            "Statut : le plus fort atteint — `vanilla` > `aller-retour` > `testé` > `codé` >",
            "`id seul` > `absent`. Un paquet émis en dur ailleurs (constante locale à",
            "`ov_server`) apparaît avec sa constante mais sans fonction dans `ov_protocol`.",
            "",
            "## Résumé",
            "",
            f"**{implemented} / {total}** paquets ont au moins un id ou une fonction.",
            "",
            "| Statut | Paquets |",
            "|---|---:|",
        ]
        for status in ("vanilla", "aller-retour", "testé", "codé", "id seul", "absent"):
            lines.append(f"| {status} | {counts.get(status, 0)} |")
        lines.append("")
        lines.append("| État · sens | Total | Présents | Enc. | Déc. | A/R | Vanilla |")
        lines.append("|---|---:|---:|---:|---:|---:|---:|")
        for state in STATE_ORDER:
            for direction in ("clientbound", "serverbound"):
                group = [p for p in self.packets if p["state"] == state and p["direction"] == direction]
                if not group:
                    continue
                lines.append(
                    f"| {STATE_TITLE[state]} · {DIRECTION_TITLE[direction]} | {len(group)} | "
                    f"{sum(self.status(p) != 'absent' for p in group)} | "
                    f"{sum(bool(p['encoder']) for p in group)} | "
                    f"{sum(bool(p['decoder']) for p in group)} | "
                    f"{sum(p['round_trip'] for p in group)} | "
                    f"{sum(p['vanilla'] for p in group)} |")
        lines.append("")

        for state in STATE_ORDER:
            for direction in ("clientbound", "serverbound"):
                group = [p for p in self.packets if p["state"] == state and p["direction"] == direction]
                if not group:
                    continue
                lines.append(f"## {STATE_TITLE[state]} — {DIRECTION_TITLE[direction]}")
                lines.append("")
                lines.append("| Id | Paquet | Statut | Const. | Enc. | Déc. | Test | A/R | Vanilla "
                             "| Srv | Cli |")
                lines.append("|---|---|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|")
                for p in group:
                    cells = [
                        yes if p["constants"] else no,
                        yes if p["encoder"] else no,
                        yes if p["decoder"] else no,
                        yes if p["tested"] else no,
                        yes if p["round_trip"] else no,
                        yes if p["vanilla"] else no,
                        yes if p["server"] else no,
                        yes if p["client"] else no,
                    ]
                    lines.append(f"| `{p['id']}` | {p['name']} | {self.status(p)} | "
                                 + " | ".join(cells) + " |")
                lines.append("")
        return "\n".join(lines)


def build() -> tuple[Matrix, str]:
    matrix = Matrix()
    matrix.scan_constants()
    matrix.scan_functions()
    matrix.scan_tests()
    return matrix, matrix.render()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="fail if the committed matrix is stale or an id is wrong")
    parser.add_argument("--orphans", action="store_true",
                        help="list ov_protocol functions attributed to no packet")
    args = parser.parse_args()

    matrix, text = build()
    for error in matrix.errors:
        print(f"error: {error}", file=sys.stderr)

    if args.orphans:
        used = {f for p in matrix.packets for f in p["functions"]}
        for fn in sorted(matrix.declared - used):
            print(f"  unattributed: {fn}")

    if args.check:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if current != text:
            print(f"error: {rel(OUT)} is stale; run python3 scripts/protocol_matrix.py",
                  file=sys.stderr)
            return 1
        if matrix.errors:
            return 1
        print(f"protocol matrix up to date ({len(matrix.packets)} packets, every id checked)")
        return 0

    if matrix.errors:
        return 1
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding="utf-8")
    print(f"wrote {rel(OUT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
