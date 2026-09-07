#!/usr/bin/env bash
# Measure how long the project takes to build, and refuse silent regressions.
#
# Risk R5 in docs/ARCHITECTURE.md: build velocity is what kills projects this
# size. Not with a bang — nobody decides to make the build slow. A heavy
# template creeps into a public header, then another, and eighteen months later
# a one-line edit costs four minutes and people stop making one-line edits.
#
# The number that matters is not the total, which grows honestly as the project
# does. It is the *per-translation-unit* cost of a full rebuild: that is what
# header weight drives, and it should stay roughly flat as files are added.
#
# Three measurements:
#   clean          everything, from nothing
#   header touch   after touching the most widely included header — the cost of
#                  a header edit, and the one that hurts day to day
#   single TU      after touching one .cpp, the floor
#
# ccache is disabled throughout: a cached build measures the cache.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="build/bench-build"
HISTORY="docs/bench/build_history.json"
JOBS="${OV_BENCH_JOBS:-4}"

# The header nearly everything includes; touching it is the worst realistic case.
CANARY_HEADER="src/ov_base/include/ov/base/types.hpp"

# How much the per-TU cost may grow before this is a failure. Generous, because
# the measurement is wall-clock on a shared machine; the point is to catch a
# structural change, not noise.
THRESHOLD_PERCENT="${OV_BENCH_THRESHOLD:-25}"

info() { printf '\033[0;32m▸\033[0m %s\n' "$*"; }
fail() { printf '\033[0;31m✗\033[0m %s\n' "$*" >&2; }

command -v cmake >/dev/null 2>&1 || { fail "cmake not found"; exit 1; }
[ -f "$CANARY_HEADER" ] || { fail "$CANARY_HEADER not found"; exit 1; }

elapsed() { python3 -c "import time; print(f'{time.time() - $1:.2f}')"; }
now() { python3 -c "import time; print(time.time())"; }

# ── Configure ────────────────────────────────────────────────────────────────
rm -rf "$BUILD_DIR"
info "Configuring"
if ! CCACHE_DISABLE=1 cmake -S . -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/vcpkg/scripts/buildsystems/vcpkg.cmake" \
        -DVCPKG_MANIFEST_FEATURES=tests >/dev/null 2>&1; then
    fail "configure failed"
    exit 1
fi

TU_COUNT=$(find src apps tools -name '*.cpp' | wc -l | tr -d ' ')

# ── Measure ──────────────────────────────────────────────────────────────────
info "Clean build ($TU_COUNT translation units, -j$JOBS)"
START=$(now)
CCACHE_DISABLE=1 cmake --build "$BUILD_DIR" -j"$JOBS" >/dev/null 2>&1 || { fail "build failed"; exit 1; }
CLEAN=$(elapsed "$START")

info "Rebuild after touching $(basename "$CANARY_HEADER")"
touch "$CANARY_HEADER"
START=$(now)
CCACHE_DISABLE=1 cmake --build "$BUILD_DIR" -j"$JOBS" >/dev/null 2>&1
HEADER=$(elapsed "$START")

info "Rebuild after touching one source file"
touch src/ov_nbt/src/binary.cpp
START=$(now)
CCACHE_DISABLE=1 cmake --build "$BUILD_DIR" -j"$JOBS" >/dev/null 2>&1
SINGLE=$(elapsed "$START")

PER_TU=$(python3 -c "print(f'{$CLEAN / $TU_COUNT:.4f}')")

echo
printf '  translation units .... %s\n' "$TU_COUNT"
printf '  clean build .......... %s s\n' "$CLEAN"
printf '  header touch ......... %s s\n' "$HEADER"
printf '  single source ........ %s s\n' "$SINGLE"
printf '  \033[1mper TU ............... %s s\033[0m\n' "$PER_TU"
echo

# ── Compare against history ──────────────────────────────────────────────────
mkdir -p "$(dirname "$HISTORY")"

python3 - "$HISTORY" "$TU_COUNT" "$CLEAN" "$HEADER" "$SINGLE" "$PER_TU" "$THRESHOLD_PERCENT" <<'PY'
import json, os, platform, subprocess, sys
from datetime import date

history_path, tu_count, clean, header, single, per_tu, threshold = sys.argv[1:8]
tu_count = int(tu_count)
per_tu = float(per_tu)
threshold = float(threshold)

history = []
if os.path.exists(history_path):
    with open(history_path) as f:
        history = json.load(f).get("runs", [])

try:
    commit = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], text=True).strip()
except Exception:
    commit = "unknown"

machine = f"{platform.system()} {platform.machine()}"

# Compare only against runs from the same machine: wall-clock numbers from a
# laptop and a CI runner are not the same measurement.
same_machine = [r for r in history if r.get("machine") == machine]
baseline = same_machine[-1] if same_machine else None

status = "ok"
if baseline:
    previous = baseline["per_tu_seconds"]
    growth = (per_tu - previous) / previous * 100 if previous else 0.0
    print(f"  previous per TU ...... {previous:.4f} s  ({growth:+.1f}%)")
    if growth > threshold:
        status = "regression"
        print()
        print(f"\033[0;31mPer-TU compile time grew {growth:.1f}%, over the {threshold:.0f}% budget.\033[0m")
        print()
        print("This is almost always a heavy template arriving in a public header.")
        print("See risk R5: the cost compounds with every file added afterwards, and")
        print("it is far cheaper to move the include now than to unpick it later.")
else:
    print("  no baseline for this machine yet — recording one")

history.append({
    "date": date.today().isoformat(),
    "commit": commit,
    "machine": machine,
    "translation_units": tu_count,
    "clean_seconds": float(clean),
    "header_touch_seconds": float(header),
    "single_source_seconds": float(single),
    "per_tu_seconds": per_tu,
})

with open(history_path, "w") as f:
    json.dump({
        "$comment": "Build-time history. Compared per machine: wall-clock from a "
                    "laptop and from a CI runner are different measurements. The "
                    "number that matters is per_tu_seconds, which header weight "
                    "drives and which should stay flat as files are added.",
        "runs": history,
    }, f, indent=2)
    f.write("\n")

sys.exit(1 if status == "regression" else 0)
PY
STATUS=$?

rm -rf "$BUILD_DIR"
exit "$STATUS"
