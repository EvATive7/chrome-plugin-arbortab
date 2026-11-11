"""Compares two offsets tables (see AGENTS.md, "偏移量") and reports what changed.

Usage:
    python offsets_diff.py <old.json> <new.json> [--summary <markdown file>]

Function and vtable RVAs move in every build, so only their count is reported; everything
that describes a layout (fields, sizes, vtable slots, constants, bit fields, bases) is listed
entry by entry, as are entries that appear or disappear. --summary appends a Markdown report
(for $GITHUB_STEP_SUMMARY). Informational only: the exit code is 0 unless an input is unreadable.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

# Sections whose values are expected to differ between any two builds.
MOVING = ("functions", "hooks", "vtables")
LAYOUT = ("fields", "sizes", "vtableSlots", "constants", "bitfields", "bases")


def show(value) -> str:
    if value is None:
        return "—"
    if isinstance(value, dict):
        return ", ".join(f"{k} {v}" for k, v in value.items())
    return str(value)


def compare(old: dict, new: dict) -> tuple[list[tuple[str, str, str, str]], list[str]]:
    """(changed layout entries as (section, name, old, new), notes about the moving sections)."""
    changes: list[tuple[str, str, str, str]] = []
    notes: list[str] = []
    for section in MOVING:
        before, after = old.get(section, {}), new.get(section, {})
        moved = sum(1 for name in after if name in before and before[name] != after[name])
        notes.append(f"{section}: {moved} of {len(after)} moved")
        for name in sorted(set(before) ^ set(after)):
            changes.append((section, name, show(before.get(name)), show(after.get(name))))
    for section in LAYOUT:
        before, after = old.get(section, {}), new.get(section, {})
        for name in sorted(set(before) | set(after)):
            if before.get(name) != after.get(name):
                changes.append((section, name, show(before.get(name)), show(after.get(name))))
    return changes, notes


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("old", type=Path)
    parser.add_argument("new", type=Path)
    parser.add_argument("--summary", type=Path)
    args = parser.parse_args()
    old = json.loads(args.old.read_text(encoding="utf-8-sig"))
    new = json.loads(args.new.read_text(encoding="utf-8-sig"))
    changes, notes = compare(old, new)

    title = (f"Offsets {old.get('chromeVersion')} ({old.get('pdbKey')}) -> "
             f"{new.get('chromeVersion')} ({new.get('pdbKey')}), schema {new.get('schema')}")
    print(title)
    for note in notes:
        print(f"  {note}")
    if changes:
        width = max(len(f"{section}.{name}") for section, name, _, _ in changes)
        print(f"  {len(changes)} layout change(s):")
        for section, name, before, after in changes:
            print(f"    {f'{section}.{name}':<{width}}  {before} -> {after}")
    else:
        print("  no layout changes")

    if args.summary:
        lines = [f"### {title}", "", "; ".join(notes) + " (RVAs move in every build)", ""]
        if changes:
            lines += ["| Section | Entry | Before | After |", "| --- | --- | --- | --- |"]
            lines += [f"| {section} | `{name}` | {before} | {after} |" for section, name, before, after in changes]
        else:
            lines.append("No layout changes.")
        with args.summary.open("a", encoding="utf-8") as summary:
            summary.write("\n".join(lines) + "\n\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
