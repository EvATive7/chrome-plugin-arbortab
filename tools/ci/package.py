"""Builds the release archives into an output folder.

    uv run python tools/ci/package.py --dll artifacts/native/version.dll --out dist

- arbortab-extension-<version>.zip        unpacked/development build. Its manifest keeps the public
                                          "key", preserving the ID allowed by the current bridge.
- cws/arbortab-extension-store-<version>.zip
                                          Ordinary Chrome Web Store upload. Its manifest omits
                                          "key" and contains no private key.
- cws/arbortab-extension-store-initial-<version>.zip (only with --store-key)
                                          One-time, local-only first upload. It contains key.pem at
                                          the ZIP root so the store can preserve the existing ID.
- arbortab-native-<version>-win64.zip     version.dll + arbortab-bridge.exe + packaging/windows
                                          (install/uninstall scripts, host manifest) + LICENSE
- install.ps1, uninstall.ps1              loose assets used by the one-line installer
- SHA256SUMS
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import shutil
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
EXTENSION = REPO / "extension"
EXCLUDED_DIRS = {"test"}
EXCLUDED_SUFFIXES = {".pem", ".key", ".b64"}  # signing material stays out of the package


def zip_extension(
    version: str,
    out: Path,
    *,
    store: bool = False,
    private_key: Path | None = None,
) -> Path:
    if private_key is not None and not store:
        raise ValueError("a Chrome Web Store private key is only valid for a store package")
    suffix = "-store-initial" if private_key else "-store" if store else ""
    target = out / f"arbortab-extension{suffix}-{version}.zip"
    if private_key is not None:
        check_private_key(private_key)
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(EXTENSION.rglob("*")):
            relative = path.relative_to(EXTENSION)
            if path.is_dir() or relative.parts[0] in EXCLUDED_DIRS or path.suffix in EXCLUDED_SUFFIXES:
                continue
            if store and relative.as_posix() == "manifest.json":
                manifest = json.loads(path.read_text(encoding="utf-8"))
                manifest.pop("key", None)
                archive.writestr("manifest.json", json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
            else:
                archive.write(path, relative.as_posix())
        if private_key is not None:
            archive.write(private_key, "key.pem")
    validate_extension_archive(target, expect_key=not store, expect_private_key=private_key is not None)
    return target


def check_private_key(path: Path) -> None:
    """Refuse an initial upload if key.pem does not match manifest.json's public key."""
    try:
        from cryptography.hazmat.primitives import serialization
    except ImportError as error:
        raise RuntimeError(
            "--store-key requires cryptography; run package.py with `uv run --with cryptography`"
        ) from error
    private = serialization.load_pem_private_key(path.read_bytes(), password=None)
    public_der = private.public_key().public_bytes(
        serialization.Encoding.DER,
        serialization.PublicFormat.SubjectPublicKeyInfo,
    )
    manifest = json.loads((EXTENSION / "manifest.json").read_text(encoding="utf-8"))
    if public_der != base64.b64decode(manifest["key"]):
        raise ValueError(f"{path} does not match extension/manifest.json's public key")


def validate_extension_archive(
    path: Path,
    *,
    expect_key: bool,
    expect_private_key: bool = False,
) -> None:
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        manifest = json.loads(archive.read("manifest.json"))
    has_key = "key" in manifest
    if has_key != expect_key:
        expectation = "contain" if expect_key else "omit"
        raise ValueError(f"{path.name}: manifest must {expectation} key")
    private_keys = [name for name in names if Path(name).suffix in EXCLUDED_SUFFIXES]
    expected_private_keys = ["key.pem"] if expect_private_key else []
    if private_keys != expected_private_keys:
        raise ValueError(f"{path.name}: unexpected signing material: {private_keys}")
    forbidden = [name for name in names if name.split("/", 1)[0] in EXCLUDED_DIRS]
    if forbidden:
        raise ValueError(f"{path.name}: contains excluded files: {forbidden}")


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
    parser.add_argument(
        "--store-key",
        type=Path,
        help="local-only: also build the one-time initial CWS ZIP containing this key as key.pem",
    )
    args = parser.parse_args()
    bridge = args.bridge or args.dll.with_name("arbortab-bridge.exe")
    version = (REPO / "VERSION").read_text(encoding="utf-8").strip()
    args.out.mkdir(parents=True, exist_ok=True)
    store_out = args.out / "cws"
    store_out.mkdir(exist_ok=True)
    initial_target = store_out / f"arbortab-extension-store-initial-{version}.zip"
    if not args.store_key:
        # A later ordinary build must not leave a local private-key package looking current.
        initial_target.unlink(missing_ok=True)
    zip_extension(version, store_out, store=True)
    if args.store_key:
        initial = zip_extension(version, store_out, store=True, private_key=args.store_key)
        print(f"LOCAL-ONLY PRIVATE-KEY PACKAGE: {initial}\n")
    files = [
        zip_extension(version, args.out),
        zip_native(version, args.dll, bridge, args.out),
    ]
    for script in ("install.ps1", "uninstall.ps1"):
        files.append(Path(shutil.copy(REPO / "packaging" / "windows" / script, args.out / script)))
    sums = "".join(f"{hashlib.sha256(f.read_bytes()).hexdigest()}  {f.name}\n" for f in files)
    # LF even on Windows runners, so `sha256sum -c SHA256SUMS` works everywhere.
    (args.out / "SHA256SUMS").write_text(sums, encoding="utf-8", newline="\n")
    print(sums, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
