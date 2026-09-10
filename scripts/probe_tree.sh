#!/usr/bin/env bash
# Génère un monde-sonde où chaque chunk porte un seul arbre d'une espèce donnée.
#
# Ce n'est pas un monde de référence : c'est un banc de mesure. Tous les chunks
# sont `plains`, `plains` n'a plus aucune feature ni aucun carver, et la seule
# qui reste est `count(1) → in_square → heightmap → <feature>` à l'étape 9.
# Un arbre par chunk, isolé, sur un terrain de plaine : la forme lue sur le
# disque est celle du placer et de rien d'autre.
#
# Usage : scripts/probe_tree.sh "minecraft:fancy_oak [autre…]" [graine] [sortie]
#         un nom de feature, ou @fichier.json pour une définition à nous.
set -euo pipefail

# Plusieurs features séparées par des espaces sont acceptées : elles vont aux
# index 0, 1, 2… de l'étape 9. Donner à chacune une essence de bois différente
# les rend distinguables même quand deux canopées se recouvrent, et une seule
# génération de monde mesure alors trois variantes.
FEATURES="${1:-minecraft:fancy_oak}"
SEED="${2:-0}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SLUG="$(echo "${FEATURES%% *}" | sed 's|.*[:/]||; s|\.json$||')"
OUT="${3:-$ROOT/run/probe-tree-$SLUG-$SEED}"
JAR="$ROOT/tools/vanilla/server.jar"
VANILLA="$ROOT/data/vanilla/1.20.1"

if [[ ! -e "$JAR" ]]; then
    echo "error: $JAR introuvable. Voir scripts/setup_vanilla.sh." >&2
    exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT/world/datapacks/probe"
ln -s "$(cd "$(dirname "$JAR")" && pwd)/$(basename "$JAR")" "$OUT/server.jar"
ln -s "$VANILLA/libraries" "$OUT/libraries"
ln -s "$VANILLA/versions" "$OUT/versions"
echo "eula=true" > "$OUT/eula.txt"

python3 "$ROOT/scripts/probe_tree.py" pack \
        "$OUT/world/datapacks/probe" "$VANILLA/generated/data/minecraft" $FEATURES

cat > "$OUT/server.properties" <<PROPERTIES
level-seed=$SEED
level-type=probe\:probe
online-mode=false
spawn-protection=0
max-players=1
view-distance=4
simulation-distance=4
sync-chunk-writes=true
max-tick-time=-1
generate-structures=false
allow-nether=false
spawn-npcs=false
spawn-animals=false
spawn-monsters=false
enable-command-block=false
PROPERTIES

PATCHES="${PATCHES:-0,0 10000,-10000}"

(
    sleep 25
    for patch in $PATCHES; do
        x="${patch%,*}"
        z="${patch#*,}"
        echo "forceload add $x $z $((x+255)) $((z+255))"
        sleep 30
        echo "save-all flush"
        sleep 4
        echo "forceload remove all"
        sleep 1
    done
    echo "save-all flush"
    sleep 6
    echo "stop"
) | ( cd "$OUT" && java -Xmx2G -jar server.jar nogui ) | tail -3

echo "$FEATURES, graine $SEED -> $OUT/world"
find "$OUT/world/region" -name '*.mca' | wc -l | xargs echo "fichiers de région :"
