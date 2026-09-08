#!/usr/bin/env bash
# Generate a reference world with the real 1.20.1 server, at a fixed seed.
#
# This is the oracle for terrain parity. Nothing about our generator can be
# called correct against a formula; it can only be called correct against the
# blocks the game itself puts on disk for the same seed. So the game puts them
# there first.
#
# The world is gitignored, like every other piece of vanilla output. What may be
# committed is the comparison's result, not its input.
set -euo pipefail

SEED="${1:-1234567890}"
OUT="${2:-run/reference-$SEED}"
JAR="tools/vanilla/server.jar"

if [[ ! -f "$JAR" ]]; then
    echo "error: $JAR not found. See scripts/setup_vanilla.sh." >&2
    exit 1
fi

mkdir -p "$OUT"
cp "$JAR" "$OUT/server.jar"
echo "eula=true" > "$OUT/eula.txt"

# Everything that could perturb the terrain is turned off. Structures stay on:
# they are part of what the seed decides, and a world generated without them
# would be a different world rather than a simpler one.
cat > "$OUT/server.properties" <<PROPERTIES
level-seed=$SEED
level-type=minecraft\:normal
online-mode=false
spawn-protection=0
max-players=1
view-distance=10
simulation-distance=10
sync-chunk-writes=true
allow-nether=false
spawn-npcs=false
spawn-animals=false
spawn-monsters=false
enable-command-block=false
PROPERTIES

echo "generating with seed $SEED …"
# The server generates its spawn chunks at start-up, then we ask for a wider
# square by force-loading it, wait, and stop. force-load has a 256-chunk limit
# per command — a limit that fails silently — so the area is asked for in
# pieces.
(
    sleep 25
    for x in -8 0 8; do
        for z in -8 0 8; do
            echo "forceload add $((x*16)) $((z*16)) $((x*16+112)) $((z*16+112))"
            sleep 2
        done
    done
    sleep 20
    echo "save-all flush"
    sleep 10
    echo "stop"
) | (cd "$OUT" && java -Xmx2G -jar server.jar nogui) | tail -20

echo
echo "seed $SEED -> $OUT/world"
find "$OUT/world/region" -name '*.mca' 2>/dev/null | wc -l | xargs echo "region files:"
