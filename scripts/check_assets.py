#!/usr/bin/env python3
"""Refuse to let a game asset enter the repository.

Ondes VOXEL is a public Apache-2.0 repository. Mojang's usage guidelines forbid
redistributing game assets, and the Faithful license attaches conditions to
theirs. The project's answer is that assets are never committed at all: they are
imported at runtime from the user's own installation into ./run/, which is
gitignored.

That rule only holds if something enforces it, because a single `git add -A`
after running the importer would break it permanently — and rewriting published
history is not a fix.

  --staged   check what is staged for commit (used by the pre-commit hook)
  (default)  check what is tracked in the working tree
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Binary formats that would only be here if someone committed game content.
ASSET_SUFFIXES = {
    ".png", ".jpg", ".jpeg", ".tga", ".gif", ".webp",
    ".ogg", ".wav", ".mp3", ".flac",
    ".ttf", ".otf", ".woff", ".woff2",
    ".jar", ".zip", ".mca", ".mcr", ".nbt", ".dat",
}

# Paths that must never be tracked, whatever they contain.
FORBIDDEN_PREFIXES = (
    "ressourcepacks/",
    "resourcepacks/",
    "run/",
    "vcpkg/",
    "build/",
)

# Narrow, deliberate exceptions. Each one is a file we authored.
ALLOWED = {
    "docs/assets/",       # diagrams drawn for the documentation
    ".github/",           # workflow badges and the like
}

MAX_BINARY_BYTES = 256 * 1024


def tracked_files(staged: bool) -> list[str]:
    if staged:
        cmd = ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"]
    else:
        cmd = ["git", "ls-files"]
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, check=True)
    return [line for line in result.stdout.splitlines() if line]


def is_allowed(path: str) -> bool:
    return any(path.startswith(prefix) for prefix in ALLOWED)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--staged", action="store_true", help="check staged changes only")
    args = parser.parse_args()

    violations: list[str] = []

    for path in tracked_files(args.staged):
        if is_allowed(path):
            continue

        for prefix in FORBIDDEN_PREFIXES:
            if path.startswith(prefix):
                violations.append(f"{path}\n      → {prefix} must never be tracked (see .gitignore)")
                break
        else:
            suffix = Path(path).suffix.lower()
            if suffix in ASSET_SUFFIXES:
                violations.append(
                    f"{path}\n      → '{suffix}' is a game-asset format. Assets are imported "
                    f"at runtime into ./run/, never committed."
                )
                continue

            full = ROOT / path
            if full.is_file() and full.stat().st_size > MAX_BINARY_BYTES:
                try:
                    full.read_text(encoding="utf-8")
                except (UnicodeDecodeError, OSError):
                    size_kb = full.stat().st_size // 1024
                    violations.append(
                        f"{path}\n      → {size_kb} KB binary file. If this is legitimate, add it "
                        f"to ALLOWED in scripts/check_assets.py with a reason."
                    )

    if violations:
        print("ASSET POLICY VIOLATIONS", file=sys.stderr)
        print("=" * 72, file=sys.stderr)
        for violation in violations:
            print(f"  {violation}", file=sys.stderr)
        print("=" * 72, file=sys.stderr)
        print(
            "\nOndes VOXEL redistributes no game content. Mojang's usage guidelines\n"
            "forbid it, and once pushed to a public repository it is in the history\n"
            "for good. See CLAUDE.md section 1.\n",
            file=sys.stderr,
        )
        return 1

    scope = "staged changes" if args.staged else "tracked files"
    print(f"asset policy ok — no game content in {scope}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
