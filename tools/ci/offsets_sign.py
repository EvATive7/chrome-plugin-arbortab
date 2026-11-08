"""Signs the offsets branch and maintains its revocation list (see AGENTS.md, "偏移量").

Every published file gets a detached signature next to it:
    win64/s<schema>/<pdb key>.json.sig     base64 of the raw ECDSA P-256 signature (r || s)
    revoked.json                           {"format": 1, "sha256": ["<hex>", ...]}
    revoked.json.sig
over SHA-256 of the file's exact bytes. version.dll verifies them with the public key in
native/src/core/offsets_key.h and refuses unsigned or badly signed files.

    uv run --with cryptography python tools/ci/offsets_sign.py keygen <private.pem>
    uv run --with cryptography python tools/ci/offsets_sign.py sign <root>
    uv run --with cryptography python tools/ci/offsets_sign.py revoke <root> <file or sha256>
    uv run --with cryptography python tools/ci/offsets_sign.py verify <root>

`sign` and `revoke` read the PEM private key from OFFSETS_SIGNING_KEY. `sign` only signs files
without a valid signature, so re-running it changes nothing.
"""
from __future__ import annotations

import base64
import hashlib
import json
import os
import re
import sys
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature, encode_dss_signature

REPO = Path(__file__).resolve().parents[2]
KEY_HEADER = REPO / "native" / "src" / "core" / "offsets_key.h"
REVOKED = "revoked.json"


def public_key() -> ec.EllipticCurvePublicKey:
    """The key compiled into version.dll."""
    data = bytes(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", KEY_HEADER.read_text(encoding="utf-8")))
    if len(data) != 64:
        raise SystemExit(f"{KEY_HEADER} must hold 64 bytes (X || Y), found {len(data)}")
    return ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), b"\x04" + data)


def private_key() -> ec.EllipticCurvePrivateKey:
    pem = os.environ.get("OFFSETS_SIGNING_KEY", "").strip()
    if not pem:
        raise SystemExit("OFFSETS_SIGNING_KEY is not set: signing needs the private key")
    key = serialization.load_pem_private_key(pem.encode(), password=None)
    if key.public_key().public_numbers() != public_key().public_numbers():
        raise SystemExit("OFFSETS_SIGNING_KEY does not match the public key in offsets_key.h")
    return key


def signed_files(root: Path) -> list[Path]:
    files = sorted((root / "win64").glob("s*/*.json"))
    if (root / REVOKED).exists():
        files.append(root / REVOKED)
    return files


def is_valid(path: Path) -> bool:
    signature = path.with_name(path.name + ".sig")
    if not signature.exists():
        return False
    raw = base64.b64decode(signature.read_text(encoding="ascii").strip())
    if len(raw) != 64:
        return False
    der = encode_dss_signature(int.from_bytes(raw[:32], "big"), int.from_bytes(raw[32:], "big"))
    try:
        public_key().verify(der, path.read_bytes(), ec.ECDSA(hashes.SHA256()))
        return True
    except InvalidSignature:
        return False


def sign_file(key: ec.EllipticCurvePrivateKey, path: Path) -> None:
    r, s = decode_dss_signature(key.sign(path.read_bytes(), ec.ECDSA(hashes.SHA256())))
    raw = r.to_bytes(32, "big") + s.to_bytes(32, "big")
    path.with_name(path.name + ".sig").write_text(base64.b64encode(raw).decode() + "\n", encoding="ascii")


def cmd_keygen(target: Path) -> None:
    if target.exists():
        raise SystemExit(f"{target} exists; refusing to overwrite a signing key")
    key = ec.generate_private_key(ec.SECP256R1())
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                         serialization.NoEncryption()))
    point = key.public_key().public_bytes(serialization.Encoding.X962,
                                          serialization.PublicFormat.UncompressedPoint)[1:]
    rows = [", ".join(f"0x{b:02x}" for b in point[i:i + 8]) for i in range(0, 64, 8)]
    print(f"private key written to {target}; public key for {KEY_HEADER.relative_to(REPO)}:")
    print(",\n".join("    " + row for row in rows))


def cmd_sign(root: Path) -> None:
    key = None
    for path in signed_files(root):
        if is_valid(path):
            continue
        key = key or private_key()
        sign_file(key, path)
        print(f"signed {path.relative_to(root)}")


def cmd_revoke(root: Path, target: str) -> None:
    digest = target.lower()
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        digest = hashlib.sha256(Path(target).read_bytes()).hexdigest()
    path = root / REVOKED
    data = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {"format": 1, "sha256": []}
    if digest not in data["sha256"]:
        data["sha256"].append(digest)
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    sign_file(private_key(), path)
    print(f"revoked {digest}")


def cmd_verify(root: Path) -> None:
    if list((root / "win64").glob("*.json")):
        raise SystemExit("win64/ still holds files in the old layout; run offsets_index.py migrate first")
    bad = [path for path in signed_files(root) if not is_valid(path)]
    for path in bad:
        print(f"unsigned or badly signed: {path.relative_to(root)}")
    if bad:
        raise SystemExit(1)
    print("all signatures valid")


def main() -> None:
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    command, arg = sys.argv[1], Path(sys.argv[2])
    if command == "keygen":
        cmd_keygen(arg)
    elif command == "sign":
        cmd_sign(arg)
    elif command == "revoke" and len(sys.argv) == 4:
        cmd_revoke(arg, sys.argv[3])
    elif command == "verify":
        cmd_verify(arg)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
