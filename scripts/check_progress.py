#!/usr/bin/env python3
"""Keep PROGRESS.json honest against ROADMAP.md.

PROGRESS.json is the first file read at the start of a work session: it says
which milestone is active and how far along it is. A summary that drifts from
reality is worse than no summary, because it is trusted. This counts the actual
checkboxes in ROADMAP.md and refuses any disagreement.

Exit code 0 if consistent, 1 otherwise.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ROADMAP = ROOT / "docs" / "ROADMAP.md"
PROGRESS = ROOT / "docs" / "PROGRESS.json"

# "## M0 — Fondations", "## M6+ — Contenu complet"
MILESTONE_RE = re.compile(r"^##\s+(M\d+)\+?\s*—", re.MULTILINE)
CHECKBOX_RE = re.compile(r"^\s*-\s*\[([ x~])\]", re.MULTILINE)

VALID_STATUSES = {"not_started", "in_progress", "done", "blocked"}


def count_by_milestone() -> dict[str, tuple[int, int]]:
    """Return {milestone: (done, total)} from the roadmap's checkboxes."""
    text = ROADMAP.read_text(encoding="utf-8")

    matches = list(MILESTONE_RE.finditer(text))
    counts: dict[str, tuple[int, int]] = {}

    for index, match in enumerate(matches):
        name = match.group(1)
        start = match.end()
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        section = text[start:end]

        boxes = CHECKBOX_RE.findall(section)
        done = sum(1 for box in boxes if box == "x")
        counts[name] = (done, len(boxes))

    return counts


def main() -> int:
    if not ROADMAP.is_file():
        sys.exit(f"error: {ROADMAP} not found")
    if not PROGRESS.is_file():
        sys.exit(f"error: {PROGRESS} not found")

    try:
        progress = json.loads(PROGRESS.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        sys.exit(f"error: {PROGRESS} is not valid JSON: {exc}")

    counts = count_by_milestone()
    errors: list[str] = []
    milestones = progress.get("milestones", {})

    for name, entry in milestones.items():
        if name not in counts:
            errors.append(f"{name}: in PROGRESS.json but has no '## {name} — ...' heading in ROADMAP.md")
            continue

        done, total = counts[name]
        if entry.get("done") != done or entry.get("total") != total:
            errors.append(
                f"{name}: PROGRESS.json says {entry.get('done')}/{entry.get('total')}, "
                f"ROADMAP.md has {done}/{total} checkboxes"
            )

        status = entry.get("status")
        if status not in VALID_STATUSES:
            errors.append(f"{name}: unknown status '{status}' (expected one of {sorted(VALID_STATUSES)})")
        elif status == "done" and done != total:
            errors.append(f"{name}: marked done but {total - done} boxes are unchecked")
        elif status == "not_started" and done > 0:
            errors.append(f"{name}: marked not_started but {done} boxes are checked")

        if not entry.get("exit_criterion"):
            errors.append(f"{name}: has no exit_criterion — a milestone without a proof of completion is a wish")

    for name in counts:
        if name not in milestones:
            errors.append(f"{name}: in ROADMAP.md but missing from PROGRESS.json")

    current = progress.get("current_milestone")
    if current and current not in milestones:
        errors.append(f"current_milestone '{current}' is not one of the declared milestones")

    if errors:
        print("PROGRESS.json AND ROADMAP.md DISAGREE", file=sys.stderr)
        print("=" * 72, file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        print("=" * 72, file=sys.stderr)
        print(
            "\nPROGRESS.json is what a session reads to know where the work stands.\n"
            "If it can drift, it stops being worth reading.\n",
            file=sys.stderr,
        )
        return 1

    total_done = sum(done for done, _ in counts.values())
    total_all = sum(total for _, total in counts.values())
    print(f"progress ok — {total_done}/{total_all} tasks across {len(counts)} milestones "
          f"(current: {current})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
