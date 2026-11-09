"""Repository consistency checks run by CI (and before a release).

    uv run python tools/ci/check_repo.py [--tag v1.2.3]

- VERSION is MAJOR.MINOR.PATCH and equals extension/manifest.json's version (and the tag).
- Every locale has exactly the keys of the default locale, and every data-i18n key used by the
  extension pages exists.
- The offsets in offsets/ match resolver/spec.yaml's schema and their own file names.
- The bridge's host manifest allows the extension ID that manifest.json's "key" produces, and
  the extension code and dev tools use that same ID and host name.
- The extension has no host permissions or loopback WebSocket fallback; the store build uses only
  Native Messaging for its local connection.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
errors: list[str] = []


def fail(message: str) -> None:
    errors.append(message)


def check_version(tag: str | None) -> None:
    version = (REPO / "VERSION").read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        fail(f"VERSION must be MAJOR.MINOR.PATCH, got {version!r}")
    manifest = json.loads((REPO / "extension" / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("version") != version:
        fail(f"extension/manifest.json version {manifest.get('version')} != VERSION {version}")
    if tag is not None and tag != f"v{version}":
        fail(f"tag {tag} does not match VERSION {version} (expected v{version})")


def check_locales() -> None:
    root = REPO / "extension" / "_locales"
    manifest = json.loads((REPO / "extension" / "manifest.json").read_text(encoding="utf-8"))
    default = manifest.get("default_locale", "en")
    reference = set(json.loads((root / default / "messages.json").read_text(encoding="utf-8")))
    for folder in sorted(root.iterdir()):
        messages = json.loads((folder / "messages.json").read_text(encoding="utf-8"))
        keys = set(messages)
        description = messages.get("extensionDescription", {}).get("message", "")
        if len(description) > 132:
            fail(f"locale {folder.name} extensionDescription is {len(description)} characters (max 132)")
        if missing := reference - keys:
            fail(f"locale {folder.name} lacks {sorted(missing)}")
        if extra := keys - reference:
            fail(f"locale {folder.name} has keys the default locale does not: {sorted(extra)}")
    used = set()
    for page in (REPO / "extension" / "src").rglob("*.html"):
        used |= set(re.findall(r'data-i18n(?:-title)?="(\w+)"', page.read_text(encoding="utf-8")))
    for script in (REPO / "extension" / "src").rglob("*.js"):
        used |= set(re.findall(r"msg\('(\w+)'", script.read_text(encoding="utf-8")))
    used |= set(re.findall(r"__MSG_(\w+)__", (REPO / "extension" / "manifest.json").read_text(encoding="utf-8")))
    if missing := used - reference:
        fail(f"keys used by the extension but not defined: {sorted(missing)}")


def check_offsets() -> None:
    spec = (REPO / "resolver" / "spec.yaml").read_text(encoding="utf-8")
    schema = int(re.search(r"^schema:\s*(\d+)", spec, re.M).group(1))
    index = json.loads((REPO / "offsets" / "index.json").read_text(encoding="utf-8"))
    if index.get("format") != 2:
        fail("offsets/index.json is not format 2")
        return
    for index_schema, versions in index.get("schemas", {}).items():
        for version, key in versions.items():
            path = REPO / "offsets" / "win64" / f"s{index_schema}" / f"{key}.json"
            if not path.exists():
                fail(f"offsets/index.json lists {version} -> {path.relative_to(REPO)}, which is missing")
                continue
            data = json.loads(path.read_text(encoding="utf-8"))
            if str(data.get("schema")) != index_schema or data.get("pdbKey") != key:
                fail(f"{path.relative_to(REPO)} says schema {data.get('schema')} / {data.get('pdbKey')}")
            if data.get("chromeVersion") != version:
                fail(f"{path.relative_to(REPO)} is Chrome {data.get('chromeVersion')}, index says {version}")
    if str(schema) not in index.get("schemas", {}):
        print(f"note: offsets/ has no sample for the current schema {schema}")


def extension_id(public_key_b64: str) -> str:
    """Chrome's ID: the first 32 hex digits of SHA-256(public key), with 0-f mapped to a-p."""
    digest = hashlib.sha256(base64.b64decode(public_key_b64)).hexdigest()[:32]
    return "".join(chr(ord("a") + int(ch, 16)) for ch in digest)


def check_bridge() -> None:
    manifest = json.loads((REPO / "extension" / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("host_permissions"):
        fail("extension/manifest.json must not request host permissions")
    if "nativeMessaging" not in manifest.get("permissions", []):
        fail("extension/manifest.json lacks the nativeMessaging permission")
    if "key" not in manifest:
        fail("extension/manifest.json has no key: the bridge's allowed_origins needs a fixed ID")
        return
    expected = extension_id(manifest["key"])
    host = json.loads((REPO / "packaging" / "windows" / "com.arbortab.bridge.json").read_text(encoding="utf-8"))
    origin = f"chrome-extension://{expected}/"
    if origin not in host.get("allowed_origins", []):
        fail(f"packaging/windows/com.arbortab.bridge.json does not allow {origin}")
    link = (REPO / "extension" / "src" / "background" / "native-link.js").read_text(encoding="utf-8")
    if "new WebSocket" in link or "ws://" in link:
        fail("native-link.js must not use the loopback WebSocket development endpoint")
    if f"'{host.get('name')}'" not in link:
        fail(f"native-link.js does not connect to the host {host.get('name')!r}")
    dev = (REPO / "tools" / "dev" / "dev.py").read_text(encoding="utf-8")
    if f'EXTENSION_ID = "{expected}"' not in dev:
        fail(f"tools/dev/dev.py EXTENSION_ID is not {expected}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag")
    args = parser.parse_args()
    check_version(args.tag)
    check_locales()
    check_offsets()
    check_bridge()
    for message in errors:
        print(f"error: {message}")
    print("ok" if not errors else f"{len(errors)} problem(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
