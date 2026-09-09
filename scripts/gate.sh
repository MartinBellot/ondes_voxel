#!/usr/bin/env bash
# The gate every merge must pass before it lands on main.
#
# Nothing here is new: it is the project's own checks, in the order that fails
# fastest. It exists so that a wave of parallel work is verified the same way
# every time, instead of from memory.
set -uo pipefail
cd "$(dirname "$0")/.."

FAILED=()
step() {
    printf '\n\033[1m── %s\033[0m\n' "$1"
    shift
    if "$@"; then
        printf '\033[32m   ok\033[0m\n'
    else
        printf '\033[31m   FAILED\033[0m\n'
        FAILED+=("$1")
    fi
}

step "layers"       ./scripts/check_layers.py
step "assets"       ./scripts/check_assets.py
step "build"        cmake --build --preset macos-debug --parallel 4
step "tests"        ctest --preset macos-debug --output-on-failure
step "progress"     ./scripts/check_progress.py

printf '\n'
if [ ${#FAILED[@]} -eq 0 ]; then
    printf '\033[32mgate: all clear\033[0m\n'
    exit 0
fi
printf '\033[31mgate: %d step(s) failed:\033[0m %s\n' "${#FAILED[@]}" "${FAILED[*]}"
exit 1
