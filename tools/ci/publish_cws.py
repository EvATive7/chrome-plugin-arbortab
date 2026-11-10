"""Upload and submit an existing Chrome Web Store item with the v2 API.

Authentication is deliberately handled by the CI environment. Set CWS_ACCESS_TOKEN to a
short-lived access token with the https://www.googleapis.com/auth/chromewebstore scope.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from typing import Any

API = "https://chromewebstore.googleapis.com"
REPO = Path(__file__).resolve().parents[2]
UPLOAD_STATES = {"SUCCEEDED", "IN_PROGRESS", "FAILED", "NOT_FOUND"}


class ApiError(RuntimeError):
    pass


def request_json(url: str, token: str, *, data: bytes | None = None, content_type: str | None = None) -> dict[str, Any]:
    headers = {"Authorization": f"Bearer {token}", "Accept": "application/json"}
    if content_type:
        headers["Content-Type"] = content_type
    request = urllib.request.Request(url, data=data, headers=headers, method="POST" if data is not None else "GET")
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            body = response.read()
    except urllib.error.HTTPError as error:
        body = error.read().decode("utf-8", errors="replace")
        try:
            detail = json.dumps(json.loads(body), ensure_ascii=False)
        except json.JSONDecodeError:
            detail = body
        raise ApiError(f"Chrome Web Store API returned HTTP {error.code}: {detail}") from error
    except urllib.error.URLError as error:
        raise ApiError(f"Chrome Web Store API request failed: {error.reason}") from error
    try:
        result = json.loads(body)
    except json.JSONDecodeError as error:
        raise ApiError(f"Chrome Web Store API returned invalid JSON: {body[:500]!r}") from error
    if not isinstance(result, dict):
        raise ApiError(f"Chrome Web Store API returned an unexpected response: {result!r}")
    return result


def expected_item_id() -> str:
    manifest = json.loads((REPO / "extension" / "manifest.json").read_text(encoding="utf-8"))
    public_key = manifest.get("key")
    if not isinstance(public_key, str):
        raise ApiError("extension/manifest.json has no public key")
    try:
        digest = hashlib.sha256(base64.b64decode(public_key, validate=True)).hexdigest()[:32]
    except (ValueError, TypeError) as error:
        raise ApiError("extension/manifest.json has an invalid public key") from error
    return "".join(chr(ord("a") + int(character, 16)) for character in digest)


def revision_versions(revision: Any) -> set[str]:
    if not isinstance(revision, dict):
        return set()
    return {
        channel["crxVersion"]
        for channel in revision.get("distributionChannels", [])
        if isinstance(channel, dict) and isinstance(channel.get("crxVersion"), str)
    }


def package_version(package: Path) -> str:
    try:
        with zipfile.ZipFile(package) as archive:
            manifest = json.loads(archive.read("manifest.json"))
    except (OSError, zipfile.BadZipFile, KeyError, json.JSONDecodeError) as error:
        raise ApiError(f"invalid extension package {package}: {error}") from error
    version = manifest.get("version")
    if not isinstance(version, str) or not re.fullmatch(r"\d+(?:\.\d+){0,3}", version):
        raise ApiError(f"{package}: manifest has an invalid version: {version!r}")
    if "key" in manifest:
        raise ApiError(f"{package}: Chrome Web Store package must omit manifest.key")
    return version


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--publisher-id", required=True)
    parser.add_argument("--item-id", required=True)
    parser.add_argument("--poll-attempts", type=int, default=30)
    parser.add_argument("--poll-seconds", type=float, default=10)
    args = parser.parse_args()

    for name, value in (("publisher ID", args.publisher_id), ("item ID", args.item_id)):
        if not re.fullmatch(r"[A-Za-z0-9_-]+", value):
            parser.error(f"invalid {name}")
    if args.poll_attempts < 1 or args.poll_seconds < 0:
        parser.error("poll settings must be positive")
    repository_item_id = expected_item_id()
    if args.item_id != repository_item_id:
        parser.error(
            f"item ID {args.item_id!r} does not match the extension/bridge ID "
            f"{repository_item_id!r}; finish the store-ID migration before publishing"
        )

    token = os.environ.get("CWS_ACCESS_TOKEN")
    if not token:
        parser.error("CWS_ACCESS_TOKEN is required")
    version = package_version(args.package)
    name = f"publishers/{args.publisher_id}/items/{args.item_id}"

    status = request_json(f"{API}/v2/{name}:fetchStatus", token)
    published_versions = revision_versions(status.get("publishedItemRevisionStatus"))
    submitted = status.get("submittedItemRevisionStatus")
    submitted_versions = revision_versions(submitted)
    submitted_state = submitted.get("state") if isinstance(submitted, dict) else None
    if version in published_versions:
        print(f"Chrome Web Store item {args.item_id} already publishes version {version}; nothing to do")
        return 0
    if version in submitted_versions and submitted_state in {"PENDING_REVIEW", "STAGED", "PUBLISHED", "PUBLISHED_TO_TESTERS"}:
        print(f"Chrome Web Store item {args.item_id} already has version {version} in state {submitted_state}; nothing to do")
        return 0
    if submitted_state in {"PENDING_REVIEW", "STAGED"}:
        raise ApiError(
            f"item already has another active submission in state {submitted_state}: "
            f"{sorted(submitted_versions) or ['unknown version']}"
        )

    print(f"Uploading Chrome Web Store item {args.item_id}, version {version}")
    upload = request_json(
        f"{API}/upload/v2/{name}:upload",
        token,
        data=args.package.read_bytes(),
        content_type="application/zip",
    )
    state = upload.get("uploadState")
    uploaded_version = upload.get("crxVersion")
    if uploaded_version is not None and uploaded_version != version:
        raise ApiError(f"store reported uploaded version {uploaded_version!r}, expected {version!r}")
    print(f"Upload state: {state}")

    for attempt in range(args.poll_attempts):
        if state == "SUCCEEDED":
            break
        if state != "IN_PROGRESS":
            known = f" (expected one of {sorted(UPLOAD_STATES)})" if state not in UPLOAD_STATES else ""
            raise ApiError(f"upload did not succeed: {state!r}{known}; response={upload!r}")
        if attempt + 1 >= args.poll_attempts:
            raise ApiError(f"upload is still processing after {args.poll_attempts} checks")
        time.sleep(args.poll_seconds)
        status = request_json(f"{API}/v2/{name}:fetchStatus", token)
        state = status.get("lastAsyncUploadState")
        print(f"Upload state: {state}")

    result = request_json(
        f"{API}/v2/{name}:publish",
        token,
        data=json.dumps({"publishType": "DEFAULT_PUBLISH"}).encode("utf-8"),
        content_type="application/json",
    )
    submitted_state = result.get("state", "unknown")
    print(f"Submitted version {version} for review; state: {submitted_state}")
    if warnings := result.get("warningInfo"):
        print(f"Chrome Web Store warnings: {json.dumps(warnings, ensure_ascii=False)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ApiError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
