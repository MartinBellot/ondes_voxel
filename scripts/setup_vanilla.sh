#!/usr/bin/env bash
# Locate the vanilla Minecraft 1.20.1 files that Ondes VOXEL needs, and say
# exactly what to do when they are missing.
#
# Nothing is downloaded and nothing is copied into the repository. Two things
# are needed, both from the user's own machine:
#
#   1. A vanilla 1.20.1 client   — the test oracle for the whole project, plus
#                                  the source of block models, blockstates,
#                                  sounds, fonts and language files, none of
#                                  which a texture pack contains.
#   2. A vanilla 1.20.1 server.jar — for the official data generator, which
#                                    produces the registry IDs the network
#                                    protocol requires to be byte-identical.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TARGET_VERSION="1.20.1"

ok()   { printf '\033[0;32m  ✓\033[0m %s\n' "$*"; }
miss() { printf '\033[0;33m  ✗\033[0m %s\n' "$*"; }
head_() { printf '\n\033[1m%s\033[0m\n' "$*"; }

FOUND_CLIENT=""
FOUND_ASSETS=""
FOUND_SERVER=""
LAUNCHER=""

# ── Launcher data directories, in order of likelihood ────────────────────────
CANDIDATES=(
    "$HOME/Library/Application Support/PrismLauncher"
    "$HOME/.local/share/PrismLauncher"
    "$HOME/AppData/Roaming/PrismLauncher"
    "$HOME/Library/Application Support/multimc"
    "$HOME/.local/share/multimc"
    "$HOME/Library/Application Support/minecraft"
    "$HOME/.minecraft"
    "$HOME/AppData/Roaming/.minecraft"
)

head_ "Launcher"
for dir in "${CANDIDATES[@]}"; do
    if [ -d "$dir" ]; then
        LAUNCHER="$dir"
        ok "found: $dir"
        break
    fi
done

if [ -z "$LAUNCHER" ]; then
    miss "no Minecraft launcher found"
    echo
    echo "  Install PrismLauncher (https://prismlauncher.org) and add a vanilla"
    echo "  $TARGET_VERSION instance, or point OV_MINECRAFT_DIR at an existing install."
    exit 1
fi

# ── Client jar ───────────────────────────────────────────────────────────────
head_ "Client $TARGET_VERSION"
CLIENT_JAR="$(find "$LAUNCHER" -name "*${TARGET_VERSION}*client*.jar" -o -name "${TARGET_VERSION}.jar" 2>/dev/null | head -1)"
if [ -n "$CLIENT_JAR" ]; then
    FOUND_CLIENT="$CLIENT_JAR"
    ok "$(basename "$CLIENT_JAR")"
    ok "  $CLIENT_JAR"
else
    miss "no $TARGET_VERSION client jar"
    OTHER="$(find "$LAUNCHER" -name "*client*.jar" 2>/dev/null | grep -oE '1\.[0-9]+(\.[0-9]+)?' | sort -uV | tr '\n' ' ')"
    [ -n "$OTHER" ] && echo "      versions present: $OTHER"
fi

# ── Asset index ──────────────────────────────────────────────────────────────
# Sounds and language files are content-addressed under assets/objects/ and
# resolved through an index. 1.20.1 uses index 5; 1.21 uses 17, and its objects
# do not cover 1.20.1.
head_ "Asset index"
# Launcher paths contain spaces ("Application Support"), so this reads line by
# line rather than word-splitting the find output.
INDEX_COUNT=0
while IFS= read -r f; do
    [ -n "$f" ] || continue
    INDEX_COUNT=$((INDEX_COUNT + 1))
    name="$(basename "$f")"
    if [ "$name" = "5.json" ]; then
        FOUND_ASSETS="$f"
        objects="$(grep -o '"hash"' "$f" 2>/dev/null | wc -l | tr -d ' ')"
        ok "$name — the $TARGET_VERSION index ($objects objects)"
    else
        ok "$name (other version)"
    fi
done < <(find "$LAUNCHER" -path "*assets/indexes/*.json" 2>/dev/null | sort)

if [ "$INDEX_COUNT" -eq 0 ]; then
    miss "no asset index"
elif [ -z "$FOUND_ASSETS" ]; then
    miss "no $TARGET_VERSION index (expected 5.json) — launch a $TARGET_VERSION instance once"
fi

# ── Server jar ───────────────────────────────────────────────────────────────
head_ "Server $TARGET_VERSION (for the official data generator)"
SERVER_JAR=""
for path in "$ROOT/tools/vanilla/server-${TARGET_VERSION}.jar" \
            "$ROOT/server.jar" \
            "$(find "$LAUNCHER" -name "*${TARGET_VERSION}*server*.jar" 2>/dev/null | head -1)"; do
    if [ -n "$path" ] && [ -f "$path" ]; then
        SERVER_JAR="$path"
        break
    fi
done

if [ -n "$SERVER_JAR" ]; then
    FOUND_SERVER="$SERVER_JAR"
    ok "$SERVER_JAR"
else
    miss "no $TARGET_VERSION server jar"
fi

# ── Resource packs ───────────────────────────────────────────────────────────
head_ "Resource packs"
if [ -d "$ROOT/ressourcepacks" ] && [ -n "$(ls -A "$ROOT/ressourcepacks" 2>/dev/null)" ]; then
    for pack in "$ROOT/ressourcepacks"/*; do
        [ -e "$pack" ] || continue
        name="$(basename "$pack")"
        meta="$pack/pack.mcmeta"
        [ -f "$pack" ] && meta=""  # a .zip pack; not inspected here
        if [ -n "$meta" ] && [ -f "$meta" ]; then
            fmt="$(grep -o '"pack_format"[^,}]*' "$meta" | grep -oE '[0-9]+' | head -1)"
            if [ "$fmt" = "15" ]; then
                ok "$name (pack_format $fmt — matches $TARGET_VERSION)"
            else
                miss "$name (pack_format $fmt — $TARGET_VERSION expects 15)"
            fi
        else
            ok "$name"
        fi
    done
else
    miss "ressourcepacks/ is empty — the procedural atlas will be used instead"
fi

# ── Verdict ──────────────────────────────────────────────────────────────────
head_ "Verdict"

if [ -n "$FOUND_CLIENT" ] && [ -n "$FOUND_ASSETS" ]; then
    ok "M0-M1 can proceed: client assets available"
else
    cat <<EOF
  Missing the vanilla $TARGET_VERSION client. It is needed from M1 onward, and it is
  not optional: it is the oracle the whole project is tested against.

  In PrismLauncher:
      Add Instance → Vanilla → $TARGET_VERSION → OK, then launch it once so the
      assets download.

EOF
fi

if [ -n "$FOUND_SERVER" ]; then
    ok "M2 can proceed: data generator available"
else
    cat <<EOF
  Missing the vanilla $TARGET_VERSION server jar. It is needed from M2 to generate the
  registry IDs, which the network protocol requires to match Mojang's exactly.

  Download server.jar for $TARGET_VERSION from minecraft.net and place it at:
      tools/vanilla/server-${TARGET_VERSION}.jar

  It is gitignored and never redistributed.

EOF
fi

# Not an error: M0 is fully buildable without any of this.
exit 0
