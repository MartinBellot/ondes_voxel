#!/usr/bin/env bash
# Génère un monde-sonde avec le vrai serveur 1.20.1, à une graine donnée.
#
# Ce n'est pas un monde de référence : c'est un banc de mesure. Tous les chunks
# sont `plains`, `plains` n'a plus aucune feature sauf quatre marqueurs, et la
# position de chaque marqueur est la lecture directe de trois `nextInt(16)`
# pris sur la graine de la feature. Ce que le jeu écrit sur le disque est donc
# le flux du générateur lui-même, et non un effet dont il faudrait le déduire.
#
# Voir scripts/probe_decoration.py pour ce que les marqueurs signifient.
set -euo pipefail

SEED="${1:-0}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${2:-$ROOT/run/probe-$SEED}"
JAR="$ROOT/tools/vanilla/server.jar"
VANILLA="$ROOT/data/vanilla/1.20.1"

if [[ ! -e "$JAR" ]]; then
    echo "error: $JAR introuvable. Voir scripts/setup_vanilla.sh." >&2
    exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT/world/datapacks/probe"
# Le jar et ses dépendances sont partagés : trois mondes-sondes tiennent dans
# quelques dizaines de mégaoctets au lieu de deux cents.
ln -s "$(cd "$(dirname "$JAR")" && pwd)/$(basename "$JAR")" "$OUT/server.jar"
ln -s "$VANILLA/libraries" "$OUT/libraries"
ln -s "$VANILLA/versions" "$OUT/versions"
echo "eula=true" > "$OUT/eula.txt"

python3 "$ROOT/scripts/probe_decoration.py" pack \
        "$OUT/world/datapacks/probe" "$VANILLA/generated/data/minecraft"

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

# Trois taches loin les unes des autres : l'origine, un quadrant positif-négatif
# et un quadrant négatif-positif. Les grandes coordonnées comptent — la forme
# `x·a + z·b` ne se distingue d'une autre qu'une fois que le produit déborde.
PATCHES="${PATCHES:-0,0 10000,-10000 -20000,30000}"

(
    sleep 25
    for patch in $PATCHES; do
        x="${patch%,*}"
        z="${patch#*,}"
        echo "forceload add $x $z $((x+127)) $((z+127))"
        sleep 12
        echo "save-all flush"
        sleep 3
        echo "forceload remove all"
        sleep 1
    done
    echo "save-all flush"
    sleep 6
    echo "stop"
) | ( cd "$OUT" && java -Xmx2G -jar server.jar nogui ) | tail -3

echo "graine $SEED -> $OUT/world"
find "$OUT/world/region" -name '*.mca' | wc -l | xargs echo "fichiers de région :"
