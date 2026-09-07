#!/usr/bin/env bash
# Bootstrap the Ondes VOXEL build environment.
#
# Idempotent: safe to re-run. Sets up vcpkg with a binary cache, because a first
# build that compiles every port from source takes hours on an 8 GB machine and
# repeating that on every manifest change is what kills build velocity (risk R5).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

info()  { printf '\033[0;32m▸\033[0m %s\n' "$*"; }
warn()  { printf '\033[0;33m!\033[0m %s\n' "$*"; }
fail()  { printf '\033[0;31m✗\033[0m %s\n' "$*" >&2; exit 1; }

# ── Host tools ───────────────────────────────────────────────────────────────
info "Checking host tools"
for tool in git cmake ninja; do
    command -v "$tool" >/dev/null 2>&1 || fail "$tool not found. On macOS: brew install cmake ninja"
done
printf '    cmake %s\n    ninja %s\n' \
    "$(cmake --version | head -1 | awk '{print $3}')" "$(ninja --version)"

CF_VERSION="$(cat "$ROOT/.clang-format-version" 2>/dev/null || echo unknown)"
if command -v clang-format >/dev/null 2>&1; then
    HAVE="$(clang-format --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
    if [ "$HAVE" = "$CF_VERSION" ]; then
        printf '    clang-format %s\n' "$HAVE"
    else
        warn "clang-format $HAVE found, but the project pins $CF_VERSION"
        warn "  clang-format output changes between releases; a different one will"
        warn "  reformat files and fail CI. Install the pinned version:"
        warn "      pipx install clang-format==$CF_VERSION"
    fi
else
    warn "clang-format not found. Install the pinned version:"
    warn "    pipx install clang-format==$CF_VERSION"
fi

if command -v ccache >/dev/null 2>&1; then
    printf '    ccache %s\n' "$(ccache --version | head -1 | awk '{print $3}')"
else
    warn "ccache not found — rebuilds will be much slower. On macOS: brew install ccache"
fi

# ── vcpkg ────────────────────────────────────────────────────────────────────
if [ ! -d "$ROOT/vcpkg" ]; then
    info "Cloning vcpkg"
    git clone --depth 1 https://github.com/microsoft/vcpkg.git "$ROOT/vcpkg"
else
    info "vcpkg already present"
fi

if [ ! -x "$ROOT/vcpkg/vcpkg" ]; then
    info "Bootstrapping vcpkg"
    "$ROOT/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
fi

# ── Binary cache ─────────────────────────────────────────────────────────────
# Without this, every manifest change recompiles every port from source.
CACHE_DIR="${VCPKG_DEFAULT_BINARY_CACHE:-$HOME/.cache/vcpkg/archives}"
mkdir -p "$CACHE_DIR"
info "vcpkg binary cache: $CACHE_DIR"

# ── Optional: Vulkan SDK (client only, needed from M5) ───────────────────────
if [ -n "${VULKAN_SDK:-}" ]; then
    info "Vulkan SDK: $VULKAN_SDK"
elif [ -d "$HOME/VulkanSDK" ]; then
    info "Vulkan SDK found under ~/VulkanSDK (set VULKAN_SDK to select a version)"
else
    warn "No Vulkan SDK. Not needed before M5 — the server and all of M0-M4 build without it."
    warn "  https://vulkan.lunarg.com/sdk/home  (macOS ships MoltenVK inside the SDK)"
fi

# ── Git hooks ────────────────────────────────────────────────────────────────
HOOK="$ROOT/.git/hooks/pre-commit"
if [ -d "$ROOT/.git" ] && [ ! -f "$HOOK" ]; then
    info "Installing pre-commit hook"
    cat > "$HOOK" <<'HOOKEOF'
#!/usr/bin/env bash
# Blocks the two mistakes that are expensive to undo once pushed:
# committing a game asset, and committing a layering violation.
set -e
ROOT="$(git rev-parse --show-toplevel)"
python3 "$ROOT/scripts/check_assets.py" --staged || {
    echo "pre-commit: asset check failed (see CLAUDE.md, legal rules)" >&2
    exit 1
}
python3 "$ROOT/scripts/check_layers.py" || {
    echo "pre-commit: layering check failed" >&2
    exit 1
}
HOOKEOF
    chmod +x "$HOOK"
fi

# ── Done ─────────────────────────────────────────────────────────────────────
cat <<EOF

Bootstrap complete.

  Configure and build:
    cmake --preset macos-debug
    cmake --build --preset macos-debug

  Run the tests:
    ctest --preset macos-debug

  Next, to get the vanilla client and assets (needed from M1):
    ./scripts/setup_vanilla.sh

EOF
