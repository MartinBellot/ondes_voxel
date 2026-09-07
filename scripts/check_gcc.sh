#!/usr/bin/env bash
# Compile every source with GCC, warnings and all, without building anything.
#
# Why this exists: GCC and clang disagree about which conversions deserve a
# warning. -Wsign-conversion fires on `count += cond ? 1 : 0` under GCC and not
# under clang; libstdc++ does not transitively include what libc++ does. Every
# one of those differences has cost a three-minute CI round trip to discover.
#
# This runs GCC in -fsyntax-only mode over the same sources with the same flags,
# reusing the include paths the normal build already resolved. No linking, no
# vcpkg rebuild for a second ABI — a few seconds instead of several minutes.
#
# It complements the CI rather than replacing it: the runner's GCC is a
# different version on a different platform.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${1:-build/macos-debug}"

GXX="${OV_GXX:-}"
if [ -z "$GXX" ]; then
    GXX="$(ls /opt/homebrew/bin/g++-1* /usr/bin/g++-1* 2>/dev/null | sort -V | tail -1)"
fi
if [ -z "$GXX" ] || ! command -v "$GXX" >/dev/null 2>&1; then
    printf '\033[0;33m!\033[0m No GCC found. On macOS: brew install gcc\n'
    printf '  (this check is optional; CI runs the real thing)\n'
    exit 0
fi

if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
    printf '\033[0;31m✗\033[0m %s/compile_commands.json not found. Configure first:\n' "$BUILD_DIR"
    printf '    cmake --preset macos-debug\n'
    exit 1
fi

printf '\033[0;32m▸\033[0m %s\n' "$("$GXX" --version | head -1)"

# Include paths, taken from what CMake already worked out.
INCLUDES="$(python3 - "$BUILD_DIR/compile_commands.json" <<'PY'
import json, sys, re
flags = []
seen = set()
for entry in json.load(open(sys.argv[1])):
    for match in re.finditer(r'-(I|isystem)\s*(\S+)', entry["command"]):
        flag = f"-{match.group(1)}{match.group(2)}" if match.group(1) == "I" else f"-isystem {match.group(2)}"
        if flag not in seen:
            seen.add(flag)
            flags.append(flag)
print(" ".join(flags))
PY
)"

WARNINGS="-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-align \
-Woverloaded-virtual -Wconversion -Wsign-conversion -Wdouble-promotion -Wformat=2 \
-Wimplicit-fallthrough -Wold-style-cast -Werror"

FAILED=0
CHECKED=0

while IFS= read -r source; do
    CHECKED=$((CHECKED + 1))
    # shellcheck disable=SC2086
    if ! "$GXX" -std=c++23 -fsyntax-only -ffp-contract=off $WARNINGS $INCLUDES "$source" 2>&1 \
        | grep -vE "^In file included from|^ +from |warning: .*libdeflate|^\s*\|" \
        | head -20 > /tmp/ov_gcc_out; then
        :
    fi
    if [ -s /tmp/ov_gcc_out ]; then
        printf '\033[0;31m✗\033[0m %s\n' "${source#"$ROOT"/}"
        sed 's/^/    /' /tmp/ov_gcc_out
        FAILED=$((FAILED + 1))
    fi
done < <(find src apps tools -name '*.cpp' -not -path '*/tests/*')

rm -f /tmp/ov_gcc_out

echo
if [ "$FAILED" -gt 0 ]; then
    printf '\033[0;31m%d of %d sources have GCC diagnostics\033[0m\n' "$FAILED" "$CHECKED"
    exit 1
fi
printf '\033[0;32mGCC clean — %d sources\033[0m\n' "$CHECKED"
