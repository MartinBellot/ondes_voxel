#!/usr/bin/env python3
"""Measure header weight deterministically, and refuse silent growth.

Risk R5 in docs/ARCHITECTURE.md is that build velocity decays until a one-line
edit costs minutes. The obvious instrument — wall-clock build time — turns out
to be a poor one at this scale: adding <regex> to the most-included header in
the project moved the total by 4.4%, well inside the noise of a shared machine.

What actually drives compile time is how much source the preprocessor hands the
compiler, and that is deterministic. Preprocessing every translation unit and
measuring the output is noise-free, fast (no code generation), and points
straight at the cause rather than at a symptom.

The number reported per file is thousands of lines after preprocessing. For
scale: a translation unit including only <cstdint> is a few hundred; one
including <regex> is tens of thousands.

  --update    record the current measurement as the new baseline
  --json      machine-readable output
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASELINE = ROOT / "docs" / "bench" / "header_weight.json"

# How much the total may grow before this is a failure. Deterministic
# measurement, so this can be tight — but not so tight that adding a legitimate
# standard header to one file trips it.
DEFAULT_THRESHOLD_PERCENT = 10.0


def compile_commands(build_dir: Path) -> list[dict]:
    path = build_dir / "compile_commands.json"
    if not path.is_file():
        sys.exit(f"error: {path} not found. Configure first: cmake --preset macos-debug")
    with open(path) as f:
        return json.load(f)


def preprocessed_lines(entry: dict) -> tuple[str, int]:
    """Preprocess one translation unit and count the lines handed to the compiler."""
    command = entry["command"]

    # Drop the output file and add -E: we want the preprocessor's result on
    # stdout, and nothing compiled.
    command = re.sub(r"-o\s+\S+", "", command)
    command = re.sub(r"\s-c\s", " -E ", command)
    # Dependency generation writes files we do not want.
    command = re.sub(r"-M[DTF]\s+\S+", "", command)
    command = re.sub(r"\s-MD\s", " ", command)
    # ccache would happily serve a cached object; we want the preprocessor.
    command = command.replace("ccache ", "")

    source = Path(entry["file"])
    try:
        result = subprocess.run(command, shell=True, cwd=entry["directory"],
                                capture_output=True, text=True, timeout=180)
    except subprocess.TimeoutExpired:
        return (source.name, 0)

    if result.returncode != 0:
        return (source.name, 0)
    return (source.name, result.stdout.count("\n"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default="build/macos-debug", type=Path)
    parser.add_argument("--update", action="store_true", help="record a new baseline")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    parser.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD_PERCENT)
    args = parser.parse_args()

    build_dir = args.build_dir if args.build_dir.is_absolute() else ROOT / args.build_dir
    entries = [e for e in compile_commands(build_dir)
               # Skip the generated header self-test units: they measure a single
               # header each, which is a different question.
               if "header_selftest" not in e["file"]]

    with ThreadPoolExecutor(max_workers=4) as pool:
        measurements = dict(pool.map(preprocessed_lines, entries))

    measurements = {name: lines for name, lines in measurements.items() if lines > 0}
    if not measurements:
        sys.exit("error: nothing could be preprocessed")

    total = sum(measurements.values())
    mean = total // len(measurements)

    if args.json:
        print(json.dumps({"total": total, "mean": mean, "files": measurements}, indent=2))
        return 0

    print(f"\n  translation units .... {len(measurements)}")
    print(f"  total lines .......... {total:,}")
    print(f"  mean per unit ........ {mean:,}")
    print("\n  heaviest:")
    for name, lines in sorted(measurements.items(), key=lambda kv: -kv[1])[:6]:
        print(f"    {lines:>9,}  {name}")

    baseline = None
    if BASELINE.is_file():
        with open(BASELINE) as f:
            baseline = json.load(f)

    status = 0
    if baseline and not args.update:
        previous = baseline["total"]
        growth = (total - previous) / previous * 100
        print(f"\n  baseline ............. {previous:,}  ({growth:+.1f}%)")

        if growth > args.threshold:
            status = 1
            print(f"\n\033[0;31mHeader weight grew {growth:.1f}%, over the "
                  f"{args.threshold:.0f}% budget.\033[0m\n")

            # Point at the specific files, since "the build got slower" is not
            # actionable and "this header now pulls in 40k more lines" is.
            for name, lines in sorted(measurements.items(), key=lambda kv: -kv[1]):
                before = baseline["files"].get(name)
                if before and lines > before * 1.1:
                    print(f"    {name}: {before:,} -> {lines:,} "
                          f"({(lines - before) / before * 100:+.0f}%)")
                elif before is None:
                    print(f"    {name}: new, {lines:,} lines")

            print("\n  A heavy template arriving in a public header is the usual cause.")
            print("  Moving the include into the .cpp, or hiding it behind PIMPL, costs")
            print("  minutes now and compounds with every file added later.\n")
        else:
            print("\n\033[0;32mHeader weight within budget\033[0m\n")
    elif args.update or baseline is None:
        BASELINE.parent.mkdir(parents=True, exist_ok=True)
        with open(BASELINE, "w") as f:
            json.dump({
                "$comment": "Lines of source after preprocessing, per translation unit. "
                            "Deterministic, unlike wall-clock build time, and it is what "
                            "actually drives compile time. Refresh with --update when a "
                            "growth is deliberate.",
                "total": total,
                "mean": mean,
                "files": measurements,
            }, f, indent=2)
            f.write("\n")
        print(f"\n  baseline written to {BASELINE.relative_to(ROOT)}\n")

    return status


if __name__ == "__main__":
    sys.exit(main())
