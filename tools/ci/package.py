"""Builds the release archives into an output folder.

    uv run python tools/ci/package.py --dll artifacts/native/version.dll --out dist

- arbortab-extension-<version>.zip      extension/ without tests. The manifest keeps its "key", so
                                        the unpacked release has the fixed ID that the bridge's
                                        allowed_origins lists (a store build gets its own ID)
- arbortab-native-<version>-win64.zip   version.dll + arbortab-bridge.exe + packaging/windows
                                        (install/uninstall scripts, bridge host manifest) + LICENSE
- install.ps1, uninstall.ps1            also as loose assets: the one-line install command fetches
                                        releases/latest/download/install.ps1, which then downloads
                                        the native zip of the same release
- SHA256SUMS
"""
from __future__ import annotations

import argparse
import hashlib
import shutil
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
EXTENSION = REPO / "extension"
EXCLUDED_DIRS = {"test"}
EXCLUDED_SUFFIXES = {".pem", ".key", ".b64"}  # signing material stays out of the package


def zip_extension(version: str, out: Path) -> Path:
    target = out / f"arbortab-extension-{version}.zip"
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(EXTENSION.rglob("*")):
            relative = path.relative_to(EXTENSION)
            if path.is_dir() or relative.parts[0] in EXCLUDED_DIRS or path.suffix in EXCLUDED_SUFFIXES:
                continue
            archive.write(path, relative.as_posix())
    return target


def zip_native(version: str, dll: Path, bridge: Path, out: Path) -> Path:
    target = out / f"arbortab-native-{version}-win64.zip"
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.write(dll, "version.dll")
        archive.write(bridge, "arbortab-bridge.exe")
        for script in sorted((REPO / "packaging" / "windows").iterdir()):
            archive.write(script, script.name)
        archive.write(REPO / "LICENSE", "LICENSE")
    return target


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dll", required=True, type=Path)
    parser.add_argument("--bridge", type=Path, help="default: arbortab-bridge.exe next to --dll")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    bridge = args.bridge or args.dll.with_name("arbortab-bridge.exe")
    version = (REPO / "VERSION").read_text(encoding="utf-8").strip()
    args.out.mkdir(parents=True, exist_ok=True)
    files = [zip_extension(version, args.out), zip_native(version, args.dll, bridge, args.out)]
    for script in ("install.ps1", "uninstall.ps1"):
        files.append(Path(shutil.copy(REPO / "packaging" / "windows" / script, args.out / script)))
    sums = "".join(f"{hashlib.sha256(f.read_bytes()).hexdigest()}  {f.name}\n" for f in files)
    (args.out / "SHA256SUMS").write_text(sums, encoding="utf-8")
    print(sums, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
