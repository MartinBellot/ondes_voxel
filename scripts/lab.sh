#!/usr/bin/env bash
# Start the test world, and let anything join it.
#
# The lab is a real Anvil save on a real socket, so the two clients that matter
# both reach it the same way: an unmodified Minecraft 1.20.1 through
# Multiplayer -> Direct Connect, and our own renderer through --connect. If the
# two ever disagree about what is in there, that disagreement is the finding.
set -uo pipefail
cd "$(dirname "$0")/.."

PRESET=${OV_PRESET:-macos-debug}
BIN="build/${PRESET}/bin"
WORLD=${OV_LAB_WORLD:-run/lab}
PORT=${OV_LAB_PORT:-25565}
CLIENT=0
REBUILD=0

for arg in "$@"; do
    case "$arg" in
        --client)  CLIENT=1 ;;
        --rebuild) REBUILD=1 ;;
        --help|-h)
            cat <<'USAGE'
scripts/lab.sh — serve the Ondes VOXEL test world

  --rebuild   regenerate the world from tools/ov_lab, discarding changes made in it
  --client    also launch our own renderer against it once the server is up

  OV_LAB_WORLD  world directory (default run/lab)
  OV_LAB_PORT   listen port    (default 25565)
  OV_PRESET     cmake preset   (default macos-debug)

Join from Minecraft 1.20.1: Multiplayer -> Direct Connection -> localhost
USAGE
            exit 0 ;;
    esac
done

cmake --build --preset "$PRESET" --parallel 4 >/dev/null || {
    echo "build failed" >&2; exit 1
}

if [ "$REBUILD" = 1 ] || [ ! -d "$WORLD/region" ]; then
    # --force only when asked: the lab is persistent on purpose, and someone
    # has left a probe rig standing in it.
    "$BIN/ov_lab" --out="$WORLD" ${REBUILD:+--force} || exit 1
fi

printf '\n  world  %s\n  port   %s\n  join   Minecraft 1.20.1 -> Multiplayer -> Direct Connection -> localhost:%s\n\n' \
    "$WORLD" "$PORT" "$PORT"

"$BIN/ov_dedicated" --world="$WORLD" --port="$PORT" --motd="Ondes VOXEL — banc de test" &
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
