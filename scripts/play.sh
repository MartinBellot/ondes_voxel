#!/usr/bin/env bash
# Play the game: build our client and open it on the title screen.
#
# The lab (scripts/lab.sh) is a bench a client is dropped into. This is the
# other way in, the one a player takes: the title screen, then Singleplayer
# (a world list under run/saves, hosted inside the client — the same bytes on
# the same socket as a dedicated server) or Multiplayer. Nothing is chosen for
# you unless you ask, with --singleplayer or --connect.
set -uo pipefail
cd "$(dirname "$0")/.."

# Release unless told otherwise, for the reason lab.sh gives: a Debug
# integrated server generates an order of magnitude slower, and the first
# world waits minutes for its spawn chunks.
PRESET=${OV_PRESET:-macos-release}
BIN="build/${PRESET}/bin"
CLIENT_ARGS=()

usage() {
    cat <<'USAGE'
scripts/play.sh — launch Ondes VOXEL on its title screen

  --debug              build and run the Debug preset (slow worldgen; for a debugger)
  --username=<name>    the player's name (default: the client's, OndesVoxel)
  --singleplayer       skip the menus: host run/world and join it
  --connect=<host[:port]>  skip the menus: join a server (e.g. one from scripts/lab.sh)
  -- <args>            everything after -- is passed to ov_voxel (e.g. -- --f3 --no-vsync)

  OV_PRESET            cmake preset (default macos-release)

Worlds made from the title screen live in run/saves; options in the client's
options file, as vanilla keeps options.txt.
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --debug)        PRESET=macos-debug; BIN="build/${PRESET}/bin" ;;
        --username=*)   CLIENT_ARGS+=("$1") ;;
        --singleplayer) CLIENT_ARGS+=("--singleplayer") ;;
        --connect=*)    CLIENT_ARGS+=("$1") ;;
        --)             shift; CLIENT_ARGS+=("$@"); break ;;
        --help|-h)      usage; exit 0 ;;
        *)              echo "unknown argument '$1' (try --help)" >&2; exit 2 ;;
    esac
    shift
done

[ -d "build/${PRESET}" ] || cmake --preset "$PRESET" >/dev/null || {
    echo "configure of preset $PRESET failed" >&2; exit 1
}
# The client alone: the integrated server is linked into it, so ov_dedicated
# and the test binaries are not needed to play.
echo "  build  ov_voxel ($PRESET)…"
cmake --build --preset "$PRESET" --target ov_voxel ov_assetimport --parallel 4 >/dev/null || {
    echo "build failed — rerun with: cmake --build --preset $PRESET --target ov_voxel" >&2; exit 1
}

# Textures, sounds and fonts come from the user's own resource pack and client
# jar, never from the repository (CLAUDE.md § 1). Imported once, into run/.
if [ ! -d run/assets/assets ]; then
    echo "  assets run/assets missing — importing from ressourcepacks/ and the 1.20.1 client jar"
    "$BIN/ov_assetimport" || {
        echo "asset import failed: see ./scripts/setup_vanilla.sh and ressourcepacks/" >&2; exit 1
    }
fi

printf '\n  client %s\n  saves  run/saves\n\n' "$BIN/ov_voxel"
exec "$BIN/ov_voxel" ${CLIENT_ARGS[@]+"${CLIENT_ARGS[@]}"}
