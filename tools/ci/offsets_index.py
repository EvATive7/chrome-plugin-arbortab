"""Maintains the layout of the `offsets` branch (see AGENTS.md, "偏移量").

    index.json                     {"format": 2, "schemas": {"<schema>": {"<chrome version>": "<pdb key>"}}}
    win64/s<schema>/<pdb key>.json

Usage (run inside a checkout of the offsets branch, or of offsets/ in main):
    python offsets_index.py has <root> <schema> <chrome version>     # exit 0 if published
    python offsets_index.py add <root> <schema> <chrome version> <pdb key> <offsets.json>
    python offsets_index.py migrate <root>                            # format 1 -> 2, in place

`add` also migrates the format-1 layout (index {"schema": 1, "versions": {...}} with files
directly under win64/), which only ever held schema 1.
"""
from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path

FORMAT = 2


def load(root: Path, migrate: bool) -> dict:
    """Reads index.json; a format-1 index is converted, and its files moved only if `migrate`."""
    path = root / "index.json"
    if not path.exists():
        return {"format": FORMAT, "schemas": {}}
    index = json.loads(path.read_text(encoding="utf-8-sig"))
    if index.get("format") == FORMAT:
        return index
    # Format 1: one implicit schema, files directly under win64/.
    migrated = {"format": FORMAT, "schemas": {"1": dict(index.get("versions", {}))}}
    if migrate:
        target = root / "win64" / "s1"
        target.mkdir(parents=True, exist_ok=True)
        for file in (root / "win64").glob("*.json"):
            shutil.move(str(file), target / file.name)
    else:
        migrated["legacy"] = True
    return migrated


def save(root: Path, index: dict) -> None:
    # Signatures cover exact bytes: git must never convert line endings on this branch.
    attributes = root / ".gitattributes"
    if not attributes.exists():
        attributes.write_bytes(b"* -text\n")
    index.pop("legacy", None)
    index["schemas"] = {schema: dict(sorted(versions.items()))
                        for schema, versions in sorted(index["schemas"].items(), key=lambda kv: int(kv[0]))}
    (root / "index.json").write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")


def main(argv: list[str]) -> int:
    command, root = argv[1], Path(argv[2])
    if command == "migrate":
        if (root / "index.json").exists():
            save(root, load(root, migrate=True))
        return 0
    schema, version = str(int(argv[3])), argv[4]
    index = load(root, migrate=command == "add")
    if command == "has":
        key = index["schemas"].get(schema, {}).get(version)
        folder = root / "win64" if index.get("legacy") else root / "win64" / f"s{schema}"
        return 0 if key and (folder / f"{key}.json").exists() else 1
    if command == "add":
        key, source = argv[5], Path(argv[6])
        offsets = json.loads(source.read_text(encoding="utf-8"))
        if str(offsets.get("schema")) != schema or offsets.get("pdbKey", "").upper() != key.upper():
            raise SystemExit(f"{source} is schema {offsets.get('schema')} / {offsets.get('pdbKey')}, "
                             f"expected schema {schema} / {key}")
        target = root / "win64" / f"s{schema}"
        target.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target / f"{key}.json")
        index["schemas"].setdefault(schema, {})[version] = key
        save(root, index)
        print(f"published {version} -> win64/s{schema}/{key}.json")
        return 0
    raise SystemExit(f"unknown command {command}")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
