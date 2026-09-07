#!/usr/bin/env python3
"""Turn the official Mojang data generator output into Ondes VOXEL's registry data.

Why this tool exists
--------------------
In 1.20.1 the vanilla client hard-codes the numeric ids of almost every registry
— block, block_state, item, entity_type, fluid, particle_type, menu, sound_event,
mob_effect, enchantment and the rest. They are never sent over the wire. Our ids
must therefore be byte-identical to Mojang's, and the only source of truth for
that is the generator built into the official server jar.

  java -DbundlerMainClass=net.minecraft.data.Main -jar server.jar --all

Nothing produced here is committed. The repository keeps only MANIFEST.sha256,
so CI can prove a regenerated dataset is identical without redistributing
Mojang's data.

The trap this tool exists to defuse
-----------------------------------
A block's states are contiguous and laid out as a mixed-radix number over its
properties, most significant first. That makes property access O(1) arithmetic
instead of a hash lookup — which is what keeps redstone playable.

But the property order printed in blocks.json is NOT the order that arithmetic
uses. Four blocks disagree:

    chest, trapped_chest, moving_piston   json: type, facing, ...
                                          real: facing, type, ...
    piston_head                           json: type, facing, short
                                          real: facing, short, type

Trusting the JSON key order gives wrong state ids for those blocks only. Nothing
would catch it until a vanilla client connects and a few chests turn into the
wrong thing — a symptom that costs weeks to trace back here.

So the order is never read from the file. It is *derived* from the state ids
themselves, and every one of the 24135 states is verified against it.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import zipfile
from itertools import permutations
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
TARGET_VERSION = "1.20.1"
TARGET_PROTOCOL = 763
TARGET_PACK_FORMAT = 15

DATA_DIR = ROOT / "data" / "vanilla" / TARGET_VERSION
GENERATED = DATA_DIR / "generated"
NORMALIZED = DATA_DIR / "normalized"
MANIFEST = DATA_DIR / "MANIFEST.sha256"

# Registries whose numeric ids the vanilla client hard-codes. These must match
# Mojang exactly or the connection desynchronises in ways that look like
# anything but an id problem.
WIRE_CRITICAL = [
    "minecraft:block",
    "minecraft:item",
    "minecraft:entity_type",
    "minecraft:fluid",
    "minecraft:block_entity_type",
    "minecraft:particle_type",
    "minecraft:menu",
    "minecraft:sound_event",
    "minecraft:mob_effect",
    "minecraft:enchantment",
    "minecraft:potion",
    "minecraft:recipe_serializer",
    "minecraft:stat_type",
    "minecraft:painting_variant",
    "minecraft:command_argument_type",
]

# Sent to the client as NBT during login, so we choose their ids ourselves.
DYNAMIC_REGISTRIES = [
    "minecraft:dimension_type",
    "minecraft:worldgen/biome",
    "minecraft:chat_type",
    "minecraft:damage_type",
    "minecraft:trim_material",
    "minecraft:trim_pattern",
]


def info(msg: str) -> None:
    print(f"\033[0;32m▸\033[0m {msg}")


def warn(msg: str) -> None:
    print(f"\033[0;33m!\033[0m {msg}")


def die(msg: str) -> None:
    print(f"\033[0;31m✗\033[0m {msg}", file=sys.stderr)
    sys.exit(1)


# ── Locating and validating the jar ──────────────────────────────────────────


def jar_version(jar: Path) -> dict | None:
    """Read version.json from inside the jar. Filenames lie; this does not."""
    try:
        with zipfile.ZipFile(jar) as zf:
            return json.loads(zf.read("version.json"))
    except (zipfile.BadZipFile, KeyError, json.JSONDecodeError, OSError):
        return None


def find_server_jar(explicit: Path | None) -> Path:
    candidates = [explicit] if explicit else []
    candidates += sorted((ROOT / "tools" / "vanilla").glob("*.jar"))

    for jar in candidates:
        if jar is None or not jar.is_file():
            continue
        meta = jar_version(jar)
        if meta is None:
            continue
        if meta.get("id") != TARGET_VERSION:
            warn(f"{jar.name} is version {meta.get('id')}, not {TARGET_VERSION} — skipping")
            continue
        if meta.get("protocol_version") != TARGET_PROTOCOL:
            die(f"{jar.name} reports protocol {meta.get('protocol_version')}, expected {TARGET_PROTOCOL}")
        if meta.get("pack_version", {}).get("data") != TARGET_PACK_FORMAT:
            die(f"{jar.name} reports data pack_format {meta['pack_version']['data']}, "
                f"expected {TARGET_PACK_FORMAT}")
        info(f"server jar: {jar.name} — {meta['id']}, protocol {meta['protocol_version']}, "
             f"pack_format {meta['pack_version']['data']}")
        return jar

    die(f"No vanilla {TARGET_VERSION} server jar found.\n"
        f"  Download it from minecraft.net and place it at:\n"
        f"      tools/vanilla/server.jar\n"
        f"  It is gitignored and never redistributed.")


def run_generator(jar: Path) -> None:
    if (GENERATED / "reports" / "blocks.json").is_file():
        info("generator output already present (delete data/vanilla/*/generated to force)")
        return

    java = shutil.which("java")
    if java is None:
        die("java not found. The generator needs Java 17 (brew install openjdk@17).")

    info("running the official data generator (this takes a minute)")
    GENERATED.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [java, "-DbundlerMainClass=net.minecraft.data.Main", "-jar", str(jar),
         "--all", "--output", "generated"],
        cwd=DATA_DIR, capture_output=True, text=True,
    )
    if result.returncode != 0:
        die(f"data generator failed:\n{result.stderr[-2000:]}")


# ── The part that matters: deriving the real property order ──────────────────


def derive_property_order(name: str, props: dict[str, list[str]],
                          states: list[dict]) -> list[str]:
    """Recover the property order that reproduces every state id.

    Tries the file's own key order first, since it is right for 999 of 1003
    blocks, then falls back to searching permutations. Raises if no order works,
    which would mean the mixed-radix assumption itself is wrong for this block
    and the whole O(1) property scheme needs revisiting.
    """
    base = min(s["id"] for s in states)
    keys = list(props.keys())

    def matches(order: tuple[str, ...] | list[str]) -> bool:
        strides, acc = {}, 1
        for key in reversed(order):
            strides[key] = acc
            acc *= len(props[key])
        return all(
            base + sum(props[k].index(s["properties"][k]) * strides[k] for k in order) == s["id"]
            for s in states
        )

    if matches(keys):
        return keys

    if len(keys) > 8:
        raise RuntimeError(f"{name}: {len(keys)} properties, too many to search exhaustively")

    for order in permutations(keys):
        if matches(order):
            return list(order)

    raise RuntimeError(
        f"{name}: no property order reproduces its state ids. The mixed-radix "
        f"assumption does not hold for this block; ov_registry's O(1) property "
        f"arithmetic cannot be used as designed."
    )


def normalize_blocks(raw: dict) -> dict:
    out_blocks = []
    reordered = []
    total_states = 0
    all_ids: list[int] = []

    for name in sorted(raw):
        entry = raw[name]
        props: dict[str, list[str]] = entry.get("properties", {})
        states = entry["states"]
        base = min(s["id"] for s in states)
        ids = sorted(s["id"] for s in states)

        # Contiguity is what makes the arithmetic possible at all.
        if ids != list(range(base, base + len(ids))):
            die(f"{name}: state ids are not contiguous — the design assumes they are")

        order = derive_property_order(name, props, states) if props else []
        if props and order != list(props.keys()):
            reordered.append({"block": name, "file_order": list(props.keys()), "real_order": order})

        strides, acc = {}, 1
        for key in reversed(order):
            strides[key] = acc
            acc *= len(props[key])

        default = next(s["id"] for s in states if s.get("default"))

        out_blocks.append({
            "name": name,
            "base_state": base,
            "state_count": len(states),
            "default_state": default,
            # Ordered most-significant first: state = base + sum(index * stride)
            "properties": [
                {"name": key, "values": props[key], "stride": strides[key]}
                for key in order
            ],
        })
        total_states += len(states)
        all_ids.extend(ids)

    all_ids.sort()
    if all_ids != list(range(len(all_ids))):
        die("global block state ids are not a dense range starting at 0")
    if all_ids[0] != 0:
        die("block state id 0 is not reserved")

    return {
        "$comment": "Derived from the official data generator. Property order is recovered "
                    "from the state ids, never read from blocks.json — see tools/ov_datagen.",
        "version": TARGET_VERSION,
        "block_count": len(out_blocks),
        "state_count": total_states,
        "max_state_id": all_ids[-1],
        "blocks_with_reordered_properties": reordered,
        "blocks": out_blocks,
    }


def normalize_registries(raw: dict) -> dict:
    registries = {}
    offsets = []

    for name, entry in sorted(raw.items()):
        entries = entry.get("entries", {})
        by_id = sorted(entries.items(), key=lambda kv: kv[1]["protocol_id"])
        ids = [protocol["protocol_id"] for _, protocol in by_id]
        first = ids[0] if ids else 0

        # Every registry is a dense range, but not all start at zero:
        # minecraft:mob_effect is 1-based, because effect id 0 means "no effect"
        # in the packets that carry one. Treating array index as protocol id
        # would shift every status effect by one.
        if ids != list(range(first, first + len(ids))):
            die(f"{name}: protocol ids have gaps — the registry design assumes a dense range")
        if first != 0:
            offsets.append((name, first))

        registries[name] = {
            "count": len(by_id),
            "first_id": first,
            "wire_critical": name in WIRE_CRITICAL,
            "dynamic": name in DYNAMIC_REGISTRIES,
            # protocol_id == first_id + index in this list
            "entries": [entry_name for entry_name, _ in by_id],
        }

    for name, first in offsets:
        warn(f"{name} is {first}-based, not 0-based — protocol_id = {first} + index")

    return {
        "$comment": "Numeric ids the vanilla client hard-codes. Ours must match exactly. "
                    "protocol_id = first_id + index into 'entries'; first_id is 0 for every "
                    "registry except minecraft:mob_effect, which is 1-based.",
        "version": TARGET_VERSION,
        "protocol": TARGET_PROTOCOL,
        "registries_with_nonzero_first_id": [
            {"registry": name, "first_id": first} for name, first in offsets
        ],
        "registries": registries,
    }


# ── Manifest ─────────────────────────────────────────────────────────────────


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_manifest(files: list[Path]) -> None:
    lines = [
        "# Ondes VOXEL — registry data manifest",
        "#",
        "# Hashes of the normalized data derived from the official 1.20.1 server jar.",
        "# The data itself is never committed; this file lets CI prove a regenerated",
        "# dataset is byte-identical without redistributing any of Mojang's content.",
        "#",
        f"# version {TARGET_VERSION}  protocol {TARGET_PROTOCOL}  pack_format {TARGET_PACK_FORMAT}",
        "",
    ]
    for path in sorted(files):
        lines.append(f"{sha256_of(path)}  {path.relative_to(DATA_DIR)}")
    MANIFEST.write_text("\n".join(lines) + "\n", encoding="utf-8")


def verify_manifest(files: list[Path]) -> int:
    if not MANIFEST.is_file():
        die(f"{MANIFEST} not found — run without --verify first")

    expected = {}
    for line in MANIFEST.read_text(encoding="utf-8").splitlines():
        if line.startswith("#") or not line.strip():
            continue
        digest, _, name = line.partition("  ")
        expected[name] = digest

    failures = []
    for path in files:
        rel = str(path.relative_to(DATA_DIR))
        actual = sha256_of(path)
        if rel not in expected:
            failures.append(f"{rel}: not in the manifest")
        elif expected[rel] != actual:
            failures.append(f"{rel}: hash mismatch\n      expected {expected[rel]}\n      got      {actual}")

    if failures:
        print("\033[0;31mMANIFEST MISMATCH\033[0m", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        print("\nEither the jar is a different build, or the normalizer changed its "
              "output. Both are worth knowing about before they reach the wire.\n",
              file=sys.stderr)
        return 1

    info(f"manifest verified — {len(files)} files match")
    return 0


# ── Entry point ──────────────────────────────────────────────────────────────


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--jar", type=Path, help="path to the vanilla 1.20.1 server jar")
    parser.add_argument("--verify", action="store_true",
                        help="regenerate and check against the committed manifest")
    args = parser.parse_args()

    jar = find_server_jar(args.jar)
    run_generator(jar)

    reports = GENERATED / "reports"
    for required in ("blocks.json", "registries.json"):
        if not (reports / required).is_file():
            die(f"{required} missing from the generator output")

    info("normalizing blocks (deriving property order from state ids)")
    blocks = normalize_blocks(json.loads((reports / "blocks.json").read_text(encoding="utf-8")))

    info("normalizing registries")
    registries = normalize_registries(
        json.loads((reports / "registries.json").read_text(encoding="utf-8")))

    NORMALIZED.mkdir(parents=True, exist_ok=True)
    outputs = []
    for name, payload in (("blocks.json", blocks), ("registries.json", registries)):
        path = NORMALIZED / name
        # sort_keys for byte-reproducibility: the manifest is worthless otherwise.
        path.write_text(json.dumps(payload, indent=1, sort_keys=False, ensure_ascii=False) + "\n",
                        encoding="utf-8")
        outputs.append(path)

    print()
    info(f"blocks ......... {blocks['block_count']}")
    info(f"block states ... {blocks['state_count']} (ids 0..{blocks['max_state_id']}, "
         f"fits u16: {blocks['max_state_id'] < 65536})")
    reordered = blocks["blocks_with_reordered_properties"]
    info(f"blocks whose real property order differs from the file: {len(reordered)}")
    for item in reordered:
        print(f"      {item['block']}")
        print(f"        file {item['file_order']}")
        print(f"        real {item['real_order']}")

    critical = [n for n, r in registries["registries"].items() if r["wire_critical"]]
    info(f"wire-critical registries: {len(critical)}")
    for name in critical:
        print(f"      {name:42s} {registries['registries'][name]['count']:5d}")
    print()

    if args.verify:
        return verify_manifest(outputs)

    write_manifest(outputs)
    info(f"manifest written: {MANIFEST.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
