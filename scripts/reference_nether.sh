#!/usr/bin/env bash
# Generate a Nether reference world with the real 1.20.1 server, at a fixed seed.
#
# The Nether is the only place where old_blended_noise can be *seen*. In the
# overworld that term is one addend among continentalness, erosion, depth,
# jaggedness and the aquifers, and a question about its amplitude arrives at the
# surface buried under five other stages. The Nether's noise_settings name no
# depth, no factor, no jaggedness and no aquifers at all: between y = 24 and
# y = 104 its final_density reduces to squeeze(0.64 x base_3d_noise), so a block
# is stone exactly where that noise is positive. Above y = 104 a clamped
# gradient slides the threshold away from zero one step per block, which turns
# the fraction of stone at each height into a reading of the noise's own
# distribution — which is the quantity in question.
#
# The world is gitignored like every other piece of vanilla output.
set -euo pipefail

SEED="${1:-1234567890}"
OUT="${2:-run/reference-nether-$SEED}"
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
view-distance=10
simulation-distance=10
sync-chunk-writes=true
max-tick-time=-1
allow-nether=true
spawn-npcs=false
spawn-animals=false
spawn-monsters=false
enable-command-block=false
PROPERTIES

echo "generating the nether with seed $SEED …"
# `forceload` acts on the executor's dimension, and the console's executor
# stands in the overworld. `execute in minecraft:the_nether` moves it, and is
# the whole trick: no player ever has to enter the dimension for its chunks to
# be generated.
#
# Patches thousands of blocks apart rather than one square, for the same reason
# as the overworld script: the Nether's biome noises have wavelengths of
# hundreds of blocks, and one square samples one place.
PATCHES="0,0 4000,0 -3000,2500 2200,-4100 -1900,-2800 6100,3300
         -5500,-5100 800,7200 -7000,400 3500,5800 -1200,-6400 4400,4400"

(
    sleep 20
    for patch in $PATCHES; do
        x="${patch%,*}"
        z="${patch#*,}"
        echo "execute in minecraft:the_nether run forceload add $x $z $((x+96)) $((z+96))"
        sleep 9
        echo "save-all flush"
        sleep 2
        echo "execute in minecraft:the_nether run forceload remove all"
        sleep 1
    done
    echo "save-all flush"
    sleep 8
    echo "stop"
) | (cd "$OUT" && java -Xmx2G -jar server.jar nogui) | tail -8

echo
echo "seed $SEED -> $OUT/world/DIM-1"
find "$OUT/world/DIM-1/region" -name '*.mca' 2>/dev/null | wc -l | xargs echo "region files:"
