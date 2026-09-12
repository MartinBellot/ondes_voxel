#!/usr/bin/env bash
# Negative tests for the project's own guard rails.
#
# check_layers.py and check_assets.py are what keep two irreversible mistakes
# out of the repository. A guard that has never been observed to fire is not a
# guard — it is a script that prints "ok". This deliberately plants each
# violation, asserts the check rejects it, and removes it again.
#
# Run in CI on every push, alongside the checks themselves.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PASS=0
FAIL=0

# The three files this script plants violations in. Restoring is done from
# copies taken up front rather than with `git checkout -- src apps`, which is
# what it used to do — that reverted every uncommitted change under src/ and
# apps/, not merely the ones planted here, and silently destroyed work in
# progress for anyone who ran it mid-edit.
TAMPERED=(
    "src/ov_math/src/math.cpp"
    "apps/ov_dedicated/src/main.cpp"
    "src/ov_math/CMakeLists.txt"
    "src/ov_protocol/include/ov/protocol/interaction.hpp"
    "docs/protocol/763/README.md"
)

BACKUP="$(mktemp -d)"
for file in "${TAMPERED[@]}"; do
    mkdir -p "$BACKUP/$(dirname "$file")"
    cp "$ROOT/$file" "$BACKUP/$file"
done

# Restores the tree even if a check aborts unexpectedly.
cleanup() {
    for file in "${TAMPERED[@]}"; do
        cp "$BACKUP/$file" "$ROOT/$file"
    done
    rm -f "$ROOT/tests/fixtures/.enforcement_probe.png"
}

finish() {
    cleanup
    rm -rf "$BACKUP"
}
trap finish EXIT

expect_rejected() {
    local description="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        printf '\033[0;31m  ✗ NOT CAUGHT\033[0m  %s\n' "$description"
        FAIL=$((FAIL + 1))
    else
        printf '\033[0;32m  ✓ rejected\033[0m   %s\n' "$description"
        PASS=$((PASS + 1))
    fi
    cleanup
}

expect_accepted() {
    local description="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        printf '\033[0;32m  ✓ accepted\033[0m   %s\n' "$description"
        PASS=$((PASS + 1))
    else
        printf '\033[0;31m  ✗ FALSE ALARM\033[0m %s\n' "$description"
        FAIL=$((FAIL + 1))
    fi
}

echo "Guard rail negative tests"
echo

echo "check_layers.py"
expect_accepted "a clean tree passes" python3 scripts/check_layers.py

printf '#include "ov/world/chunk.hpp"\n' >> src/ov_math/src/math.cpp
expect_rejected "ov_math including a higher layer" python3 scripts/check_layers.py

printf '#include "ov/sim/level.hpp"\n' >> src/ov_math/src/math.cpp
expect_rejected "a module reaching into ov_sim" python3 scripts/check_layers.py

printf '#include <vulkan/vulkan.h>\n' >> apps/ov_dedicated/src/main.cpp
expect_rejected "Vulkan in the headless server" python3 scripts/check_layers.py

printf 'target_link_libraries(ov_math PUBLIC ov_base)\n' >> src/ov_math/CMakeLists.txt
expect_rejected "target_link_libraries bypassing ov_add_library" python3 scripts/check_layers.py

echo
echo "check_progress.py"
# The drift this guards against is not hypothetical: PROGRESS.json and
# ROADMAP.md both carried the counts by hand, and they diverged in CI while
# every local run had passed. Planting the drift proves the guard still fires.
cp "$ROOT/docs/PROGRESS.json" "$BACKUP/progress.json"
python3 - "$ROOT/docs/PROGRESS.json" <<'PYEOF'
import json, sys
p = sys.argv[1]
d = json.load(open(p))
first = next(iter(d["milestones"].values()))
first["done"] = first["done"] + 1
json.dump(d, open(p, "w"), indent=2, ensure_ascii=False)
PYEOF
expect_rejected "a milestone count that drifted" python3 scripts/check_progress.py
cp "$BACKUP/progress.json" "$ROOT/docs/PROGRESS.json"
expect_accepted "counts that agree" python3 scripts/check_progress.py

echo
echo "protocol_matrix.py"
expect_accepted "the committed matrix is current" python3 scripts/protocol_matrix.py --check

# The bug this guards against was real: Set Cooldown went out as 0x16, which is
# Chat Suggestions in 763. Planting it back must be refused.
sed -i.bak 's/kSetCooldown = 0x15;/kSetCooldown = 0x16;/' \
    src/ov_protocol/include/ov/protocol/interaction.hpp
rm -f src/ov_protocol/include/ov/protocol/interaction.hpp.bak
expect_rejected "a packet id constant that contradicts the catalogue" \
    python3 scripts/protocol_matrix.py --check

printf '\n| `0x00` | edited by hand |\n' >> docs/protocol/763/README.md
expect_rejected "a matrix edited by hand (stale)" python3 scripts/protocol_matrix.py --check

echo
echo "check_assets.py"
expect_accepted "a clean tree passes" python3 scripts/check_assets.py

# Staged, not merely present: an untracked png is fine, a committed one is not.
mkdir -p tests/fixtures
printf '\x89PNG\r\n\x1a\n' > tests/fixtures/.enforcement_probe.png
git add -f tests/fixtures/.enforcement_probe.png 2>/dev/null
expect_rejected "a staged .png" python3 scripts/check_assets.py --staged
git rm --cached -q tests/fixtures/.enforcement_probe.png 2>/dev/null || true
rm -f tests/fixtures/.enforcement_probe.png

echo
if [ "$FAIL" -gt 0 ]; then
    printf '\033[0;31m%d of %d guard rail tests failed\033[0m\n' "$FAIL" "$((PASS + FAIL))"
    echo
    echo "A guard rail that does not fire is worse than none: it produces confidence"
    echo "without protection. Fix the check before shipping anything else."
    exit 1
fi

printf '\033[0;32mall %d guard rail tests passed\033[0m\n' "$PASS"
