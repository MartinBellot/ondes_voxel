#!/usr/bin/env bash
# Start the test world, and let anything join it.
#
# The lab is a real Anvil save on a real socket, so the two clients that matter
# both reach it the same way: an unmodified Minecraft 1.20.1 through
# Multiplayer -> Direct Connect, and our own renderer through --connect. If the
# two ever disagree about what is in there, that disagreement is the finding.
#
# With --seed it serves a generated overworld instead of the catalogue bench, so
# the terrain generator can be walked through by hand — and, with the same seed
# typed into vanilla's "Create New World" screen, compared against the game.
set -uo pipefail
cd "$(dirname "$0")/.."

# Release unless told otherwise: the lab is played in, and a Debug server
# generates an order of magnitude slower — measured on seed 12345 with no
# player, 0 chunks in the first minute against 64 in Release. The first trip
# to the Nether waits for its chunks, and in Debug that is minutes.
PRESET=${OV_PRESET:-macos-release}
BIN="build/${PRESET}/bin"
PORT=${OV_LAB_PORT:-25565}
CLIENT=0
REBUILD=0
FRESH=0
SEED=""
SERVER_ARGS=()
OPS=()

usage() {
    cat <<'USAGE'
scripts/lab.sh — serve the Ondes VOXEL test world

  --rebuild         regenerate the bench from tools/ov_lab, discarding changes made in it
  --client          also launch our own renderer against it once the server is up
  --seed=<seed>     serve a GENERATED world for this seed instead of the bench
                    (also --seed <seed>). A number is used as is; any other text is
                    hashed the way vanilla's Create New World screen hashes it, so
                    the same seed gives the same map in both games.
  --fresh           with --seed: delete that seed's world first. Chunks already on
                    disk are served as saved, so this is how a generator change is seen.
  --op=<name>       make this player an operator (level 4) in ops.json, which /gamemode
                    and the other level-2 commands need; repeatable. Default: OndesVoxel,
                    our client's name. The server console also takes `op <name>`.
  -- <args>         everything after -- is passed to ov_dedicated (e.g. -- --survival)

  OV_LAB_WORLD      world directory (default run/lab, or run/seed-<seed> with --seed)
  OV_LAB_PORT       listen port    (default 25565)
  OV_PRESET         cmake preset   (default macos-release; macos-debug to debug)
  OV_WORLDGEN_WORKERS  generation threads (default: the machine's recommendation)

Join from Minecraft 1.20.1: Multiplayer -> Direct Connection -> localhost
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --client)  CLIENT=1 ;;
        --rebuild) REBUILD=1 ;;
        --fresh)   FRESH=1 ;;
        --op=*)    OPS+=("${1#--op=}") ;;
        --seed=*)  SEED="${1#--seed=}"
                   [ -n "$SEED" ] || { echo "--seed= is empty: name a seed" >&2; exit 2; } ;;
        --seed)
            [ $# -ge 2 ] && [ -n "$2" ] || { echo "--seed needs a value" >&2; exit 2; }
            SEED="$2"; shift ;;
        --)        shift; SERVER_ARGS=("$@"); break ;;
        --help|-h) usage; exit 0 ;;
        *)         echo "unknown argument '$1' (try --help)" >&2; exit 2 ;;
    esac
    shift
done

if [ -n "$SEED" ]; then
    # Vanilla: a seed that parses as a long is that long; anything else is
    # String.hashCode(), a 32-bit int. Blank is refused rather than randomised,
    # because a map nobody can name again cannot be compared with anything.
    if [[ "$SEED" =~ ^-?[0-9]+$ ]]; then
        SEED_VALUE="$SEED"
    else
        SEED_VALUE=$(python3 -c '
import sys
h = 0
for unit in sys.argv[1].encode("utf-16-be").hex(" ", 2).split():
    h = (31 * h + int(unit, 16)) & 0xFFFFFFFF
print(h - (1 << 32) if h >= 1 << 31 else h)' "$SEED") || exit 1
    fi
    WORLD=${OV_LAB_WORLD:-run/seed-${SEED_VALUE}}
    if [ "$REBUILD" = 1 ]; then
        echo "--rebuild rebuilds the bench; with --seed use --fresh" >&2; exit 2
    fi
else
    WORLD=${OV_LAB_WORLD:-run/lab}
    if [ "$FRESH" = 1 ]; then
        echo "--fresh applies to --seed worlds; the bench is rebuilt with --rebuild" >&2; exit 2
    fi
fi

[ -d "build/${PRESET}" ] || cmake --preset "$PRESET" >/dev/null || {
    echo "configure of preset $PRESET failed" >&2; exit 1
}
# Quiet when it works, and the actual errors when it does not — a bare "build
# failed" left nothing to go on.
BUILD_LOG=$(mktemp -t ov-lab-build)
cmake --build --preset "$PRESET" --parallel 4 >"$BUILD_LOG" 2>&1 || {
    grep -E "error|FAILED|conflict|<<<<<<<" "$BUILD_LOG" | head -30 >&2
    echo "build failed (preset $PRESET) — full log: $BUILD_LOG" >&2; exit 1
}
rm -f "$BUILD_LOG"

if [ -n "$SEED" ]; then
    if [ "$FRESH" = 1 ] && [ -d "$WORLD" ]; then
        # Only ever a seed world this script named, never the bench.
        case "$WORLD" in
            run/seed-*) rm -rf "$WORLD" ;;
            *) echo "--fresh refuses to delete '$WORLD': only run/seed-* worlds are disposable" >&2
               exit 2 ;;
        esac
    fi
    mkdir -p "$WORLD"
    export OV_WORLDGEN_SEED="$SEED_VALUE"
    KIND="generated, seed $SEED_VALUE$([ "$SEED_VALUE" != "$SEED" ] && printf ' ("%s")' "$SEED")"
else
    if [ "$REBUILD" = 1 ] || [ ! -d "$WORLD/region" ]; then
        # --force only when asked: the lab is persistent on purpose, and someone
        # has left a probe rig standing in it.
        "$BIN/ov_lab" --out="$WORLD" ${REBUILD:+--force} || exit 1
    fi
    KIND="test bench"
fi

# A dedicated server gives a player level 0 unless ops.json says otherwise, and
# /gamemode, /tp, /give and the rest need level 2 — which is why a joining
# player could run none of them. The lab is a local bench, so its players are
# made operators here, under the same offline uuid the server computes.
[ ${#OPS[@]} -gt 0 ] || OPS=(OndesVoxel)
python3 - "${OPS[@]}" <<'OPS_PY' || exit 1
import hashlib, json, os, sys, uuid
path = "ops.json"
entries = json.load(open(path)) if os.path.exists(path) else []
known = {e.get("name", "").lower() for e in entries}
for name in sys.argv[1:]:
    digest = bytearray(hashlib.md5(("OfflinePlayer:" + name).encode()).digest())
    digest[6] = (digest[6] & 0x0F) | 0x30
    digest[8] = (digest[8] & 0x3F) | 0x80
    if name.lower() not in known:
        entries.append({"uuid": str(uuid.UUID(bytes=bytes(digest))), "name": name,
                        "level": 4, "bypassesPlayerLimit": False})
    print("  op     " + name)
json.dump(entries, open(path, "w"), indent=2)
OPS_PY

printf '\n  world  %s (%s)\n  port   %s\n  join   Minecraft 1.20.1 -> Multiplayer -> Direct Connection -> localhost:%s\n\n' \
    "$WORLD" "$KIND" "$PORT" "$PORT"

"$BIN/ov_dedicated" --world="$WORLD" --port="$PORT" --motd="Ondes VOXEL — banc de test" \
    ${SERVER_ARGS[@]+"${SERVER_ARGS[@]}"} 0<&0 &
# `0<&0`: without job control a background job's stdin is /dev/null, so the
# console never heard `op <name>` or anything else typed here.
SERVER=$!
trap 'kill $SERVER 2>/dev/null' EXIT INT TERM

if [ "$CLIENT" = 1 ]; then
    # The server needs its socket open before the client knocks.
    until nc -z 127.0.0.1 "$PORT" 2>/dev/null; do
        kill -0 $SERVER 2>/dev/null || { echo "server exited before it listened" >&2; exit 1; }
        sleep 0.2
    done
    "$BIN/ov_voxel" --connect="127.0.0.1:$PORT"
    kill $SERVER 2>/dev/null
else
    wait $SERVER
fi
