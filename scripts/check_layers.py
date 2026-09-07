#!/usr/bin/env python3
"""Layering lock #2: verify the #include graph, not the declarations.

cmake/OvModule.cmake checks what each module *declares* it depends on. That
catches an honest mistake in a CMakeLists file, but not an #include added to a
.cpp that happens to link anyway through a transitive dependency. This script
reads the actual includes.

Also enforces that nobody bypasses ov_add_library by calling
target_link_libraries directly under src/, which would sidestep lock #1
entirely.

Exit code 0 if clean, 1 otherwise. Run from anywhere.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
MODULE_CMAKE = ROOT / "cmake" / "OvModule.cmake"

# Modules at or above this layer are rendering; the dedicated server must not
# reach them, transitively or otherwise.
RENDER_FRONTIER = 13

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]ov/([a-z_]+)/', re.MULTILINE)
LAYER_RE = re.compile(r"^set\(OV_LAYER_(\w+)\s+(\d+)\)", re.MULTILINE)
FORBID_RE = re.compile(r"^set\(OV_FORBID_(\w+)\s+([^)]*)\)", re.MULTILINE)
TLL_RE = re.compile(r"^\s*target_link_libraries\s*\(", re.MULTILINE)

# ov/base/... maps to module ov_base, ov/math/... to ov_math, and so on.
def module_of_include(include_dir: str) -> str:
    return f"ov_{include_dir}"


def load_rules() -> tuple[dict[str, int], dict[str, set[str]], list[str]]:
    text = MODULE_CMAKE.read_text(encoding="utf-8")

    layers = {name: int(value) for name, value in LAYER_RE.findall(text)}
    if not layers:
        sys.exit(f"error: no OV_LAYER_* declarations found in {MODULE_CMAKE}")

    forbidden: dict[str, set[str]] = {}
    for name, deps in FORBID_RE.findall(text):
        entries = set()
        for token in deps.split():
            # OV_FORBID_ov_rhi expands ${OV_ALL_MODULES}; treat that as "all".
            if token.startswith("${"):
                entries.update(layers)
            else:
                entries.add(token)
        forbidden[name] = entries

    return layers, forbidden, sorted(layers)


def module_sources(module: str) -> list[Path]:
    directory = SRC / module
    if not directory.is_dir():
        return []
    return [
        path
        for path in directory.rglob("*")
        if path.suffix in {".hpp", ".cpp", ".h", ".cc"} and "tests" not in path.parts
    ]


def main() -> int:
    layers, forbidden, modules = load_rules()
    errors: list[str] = []

    # ── Real include edges ──────────────────────────────────────────────────
    for module in modules:
        own_layer = layers[module]
        banned = forbidden.get(module, set())

        for path in module_sources(module):
            text = path.read_text(encoding="utf-8", errors="replace")
            for include_dir in set(INCLUDE_RE.findall(text)):
                dep = module_of_include(include_dir)
                if dep == module or dep not in layers:
                    continue

                rel = path.relative_to(ROOT)
                if layers[dep] >= own_layer:
                    errors.append(
                        f"{rel}: {module}(L{own_layer}) includes {dep}(L{layers[dep]}) "
                        f"— a module may only include strictly lower layers"
                    )
                elif dep in banned:
                    errors.append(
                        f"{rel}: {module} includes {dep}, which is explicitly forbidden "
                        f"(see OV_FORBID_{module} in cmake/OvModule.cmake)"
                    )

    # ── Nobody bypasses ov_add_library ──────────────────────────────────────
    for cmakelists in SRC.rglob("CMakeLists.txt"):
        text = cmakelists.read_text(encoding="utf-8")
        if TLL_RE.search(text):
            errors.append(
                f"{cmakelists.relative_to(ROOT)}: calls target_link_libraries directly. "
                f"Use ov_add_library(... DEPS ...) so the layering check runs."
            )

    # ── Headless server reachability ────────────────────────────────────────
    dedicated = ROOT / "apps" / "ov_dedicated"
    if dedicated.is_dir():
        for path in dedicated.rglob("*"):
            if path.suffix not in {".hpp", ".cpp"}:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for include_dir in set(INCLUDE_RE.findall(text)):
                dep = module_of_include(include_dir)
                if layers.get(dep, -1) >= RENDER_FRONTIER:
                    errors.append(
                        f"{path.relative_to(ROOT)}: ov_dedicated includes {dep}, which is "
                        f"across the rendering frontier. The headless server must build "
                        f"with zero rendering dependencies."
                    )
            if "vulkan" in text.lower() or "GLFW" in text:
                errors.append(
                    f"{path.relative_to(ROOT)}: ov_dedicated references Vulkan or GLFW."
                )

    if errors:
        print("LAYERING VIOLATIONS", file=sys.stderr)
        print("=" * 72, file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        print("=" * 72, file=sys.stderr)
        print(
            "\nThese rules exist because they are impossible to restore later. "
            "See CLAUDE.md section 3.\n",
            file=sys.stderr,
        )
        return 1

    checked = sum(len(module_sources(m)) for m in modules)
    print(f"layering ok — {len(modules)} modules declared, {checked} source files checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
