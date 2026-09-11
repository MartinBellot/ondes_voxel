#!/usr/bin/env bash
# Generate an End reference world with the real 1.20.1 server, at a fixed seed.
#
# The End has no climate table: its biome is a fixed rule over the island
# noise (`end_islands`), read at the centre of each chunk, and the main island
# is the same noise near the origin. So the patches below are chosen for the
# rule's own thresholds rather than for wavelengths:
#
#   * four patches round the origin cover the main island, its ten obsidian
#     spikes (radius 42) and the empty ring round it — every chunk within
#     64 chunks of the origin is `minecraft:the_end` whatever the noise says;
#   * the others sit beyond 1000 blocks, where the outer islands begin and the
#     four other biomes (highlands, midlands, small islands, barrens) exist.
#
# Nobody enters the End, so the dragon fight never starts: no exit portal, no
# dragon, no gateways. What is on disk is worldgen and nothing else — except
# the crystals' own fire, which a crystal lights on its first ticks (named in
# docs/provenance/end.md).
#
# The world is gitignored like every other piece of vanilla output. Run it
# through the machine-wide lock:
#   lockf /tmp/ov-vanilla.lock scripts/reference_end.sh
set -euo pipefail

SEED="${1:-1234567890}"
OUT="${2:-run/reference-end-$SEED}"
JAR="tools/vanilla/server.jar"

if [[ ! -f "$JAR" ]]; then
    echo "error: $JAR not found. See scripts/setup_vanilla.sh." >&2
    exit 1
fi

mkdir -p "$OUT"
cp "$JAR" "$OUT/server.jar"
echo "eula=true" > "$OUT/eula.txt"

cat > "$OUT/server.properties" <<PROPERTIES
level-seed=$SEED
level-type=minecraft\:normal
online-mode=false
spawn-protection=0
max-players=1
view-distance=4
simulation-distance=4
sync-chunk-writes=true
max-tick-time=-1
allow-nether=false
spawn-npcs=false
spawn-animals=false
spawn-monsters=false
enable-command-block=false
PROPERTIES

echo "generating the end with seed $SEED …"
# `forceload` acts on the executor's dimension; `execute in minecraft:the_end`
# moves the console's. A forceload area is at most 256 chunks, so the main
# island is four patches of 10 x 10.
PATCHES="-160,-160 0,-160 -160,0 0,0
         1100,0 0,-1400 -1900,1700 2600,2600 -3700,-900 1500,-4600
         5200,600 -600,6100 -5000,-5000 4100,-3300"

(
    sleep 25
    for patch in $PATCHES; do
        x="${patch%,*}"
        z="${patch#*,}"
        size=144
        if (( x < -200 || x > 200 || z < -200 || z > 200 )); then
            size=96
        fi
        echo "execute in minecraft:the_end run forceload add $x $z $((x+size)) $((z+size))"
        sleep 10
        echo "save-all flush"
        sleep 2
        echo "execute in minecraft:the_end run forceload remove all"
        sleep 1
    done
    echo "save-all flush"
    sleep 8
    echo "stop"
) | (cd "$OUT" && java -Xmx1500M -jar server.jar nogui) | tail -8

echo
echo "seed $SEED -> $OUT/world/DIM1"
find "$OUT/world/DIM1/region" -name '*.mca' 2>/dev/null | wc -l | xargs echo "region files:"
