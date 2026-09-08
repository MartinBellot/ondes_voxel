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
# The watchdog kills the server when a tick takes a minute, and force-loading a
# few thousand chunks at once does exactly that. This is a pregeneration run,
# not a game.
max-tick-time=-1
allow-nether=false
spawn-npcs=false
spawn-animals=false
spawn-monsters=false
enable-command-block=false
PROPERTIES

echo "generating with seed $SEED …"
# Scattered patches rather than one square around spawn.
#
# The climate noises have wavelengths of a few thousand blocks, so a single
# region only ever shows the handful of biomes that happen to be there — the
# first run of this saw thirteen of fifty-three. Patches tens of thousands of
# blocks apart are climatically independent, and that is what reaches the deserts,
# the badlands, the snowy peaks and the mushroom fields.
#
# force-load has a 256-chunk limit per command and it fails *silently*, so each
# patch is small and asked for on its own.
PATCHES="0,0 48000,0 -37000,15000 22000,-41000 -19000,-28000 61000,33000
         -55000,-51000 8000,72000 -70000,4000 35000,58000 -12000,-64000
         44000,44000 -44000,44000 67000,-17000 -26000,39000 15000,26000
         -83000,29000 52000,-68000 -31000,77000 90000,21000 -95000,-38000
         73000,63000 -63000,-83000 27000,95000 -105000,7000 41000,-95000
         -17000,-105000 110000,-45000"

(
    sleep 20
    for patch in $PATCHES; do
        x="${patch%,*}"
        z="${patch#*,}"
        # One patch at a time, then *unloaded*.
        #
        # This is the whole difference between four minutes and forty. A
        # force-load is permanent: the server keeps every chunk it holds and
        # ticks all of them, every tick, for the rest of the run. Adding
        # twenty-eight patches without removing any meant the last one was
        # generated while three thousand chunks were being ticked, and the run
        # slowed to a crawl that looked like a hang.
        echo "forceload add $x $z $((x+128)) $((z+128))"
        sleep 9
        echo "save-all flush"
        sleep 2
        echo "forceload remove all"
        sleep 1
    done
    echo "save-all flush"
    sleep 8
    echo "stop"
) | (cd "$OUT" && java -Xmx2G -jar server.jar nogui) | tail -8

echo
echo "seed $SEED -> $OUT/world"
find "$OUT/world/region" -name '*.mca' 2>/dev/null | wc -l | xargs echo "region files:"
