"""Finds the Windows x64 stable Chrome builds Omaha currently serves. Prints JSON for CI.

Stable is not one version: a fresh install gets the newest build, while updating clients are
handed one of several builds of a staged rollout, picked at random per request (e.g. 157 had
.79 for fresh installs and .51/.52/.78 for updates at the same time). Only asking as a fresh
install misses the builds most users actually run, so this:

  1. samples scheduled update checks (no `installsource`: an on-demand check skips the staged
     rollout and always gets the newest build) from several "installed" versions and collects
     every build served;
  2. asks for each recent release listed by chromiumdash by version prefix, which Omaha
     answers for builds it still offers as fresh installs.

Only the newest two milestones are considered: older builds are not worth a 1GB PDB each.

With --published/--schema, builds already on the offsets branch are left out; the rest are
written newest first, at most --limit of them, as a matrix for the workflow.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

OMAHA = "https://tools.google.com/service/update2"
CHROMIUMDASH = "https://chromiumdash.appspot.com/fetch_releases?channel=Stable&platform=Windows&num=30"
REQUEST = """<?xml version="1.0" encoding="UTF-8"?>
<request protocol="3.0" updater="Omaha" ismachine="1">
  <os platform="win" version="10.0" arch="x64"/>
  <app appid="{{8A69D345-D564-463C-AFF1-A69D9E530F96}}" version="{installed}" ap="{channel}" lang="en"><updatecheck{prefix}/></app>
</request>"""
SAMPLES_PER_SOURCE = 6
RELEASE_WINDOW_DAYS = 30


def version_key(version: str) -> tuple[int, ...]:
    return tuple(int(part) for part in version.split("."))


def update_check(channel: str, installed: str = "", prefix: str = "") -> dict | None:
    """One Omaha update check; the full installer it offers, or None."""
    body = REQUEST.format(channel=channel, installed=installed,
                          prefix=f' targetversionprefix="{prefix}"' if prefix else "").encode()
    request = urllib.request.Request(OMAHA, body, {"Content-Type": "application/xml"})
    root = ET.fromstring(urllib.request.urlopen(request, timeout=60).read())
    update = root.find("./app/updatecheck")
    if update is None or update.get("status") != "ok":
        return None
    manifest = update.find("manifest")
    version = manifest.get("version")
    # Only the full installer: the resolver needs chrome.dll itself, not a patch.
    package = next((p for p in manifest.findall("./packages/package")
                    if p.get("name", "").startswith(f"{version}_chrome_installer")), None)
    urls = [node.get("codebase") for node in update.findall("./urls/url")]
    base = next((url for url in urls if url.startswith("https://")), None)
    if package is None or base is None:
        return None
    return {
        "version": version,
        "package": package.get("name"),
        "url": base + package.get("name"),
        "sha256": package.get("hash_sha256"),
        "size": int(package.get("size")),
    }


def recent_releases() -> list[str]:
    """Stable Windows versions released within the window (chromiumdash); [] if unavailable."""
    try:
        releases = json.loads(urllib.request.urlopen(CHROMIUMDASH, timeout=60).read())
    except Exception as error:  # Informational source only: Omaha sampling still works.
        print(f"::warning::chromiumdash unavailable: {error}", file=sys.stderr)
        return []
    cutoff = (time.time() - RELEASE_WINDOW_DAYS * 86400) * 1000
    return sorted({r["version"] for r in releases if r.get("time", 0) >= cutoff}, key=version_key)


def discover(channel: str) -> dict[str, dict]:
    found: dict[str, dict] = {}

    newest = update_check(channel)
    if newest is None:
        raise RuntimeError("Omaha did not return an update")
    oldest = version_key(newest["version"])[0] - 1

    def add(build: dict | None) -> None:
        if build and build["version"] not in found and version_key(build["version"])[0] >= oldest:
            found[build["version"]] = build
            print(f"served: {build['version']}", file=sys.stderr)

    add(newest)
    releases = [v for v in recent_releases() if version_key(v)[0] >= oldest]
    # Installed versions to update from: the start of each milestone and the recent releases
    # themselves (an update check from a build Omaha no longer offers gets a current one).
    sources = [f"{oldest}.0.0.0", f"{oldest + 1}.0.0.0"] + releases
    for installed in sources:
        for _ in range(SAMPLES_PER_SOURCE):
            add(update_check(channel, installed))
    for version in releases:
        if version not in found:
            add(update_check(channel, prefix=version))
    missing = [v for v in releases if v not in found]
    if missing:
        print(f"listed by chromiumdash but not served by Omaha: {', '.join(missing)}", file=sys.stderr)
    return found


def published(root: Path, schema: str) -> set[str]:
    index_path = root / "index.json"
    if not index_path.exists():
        return set()
    index = json.loads(index_path.read_text(encoding="utf-8-sig"))
    versions = index.get("schemas", {}).get(schema, {}) if index.get("format") == 2 else {}
    folder = root / "win64" / f"s{schema}"
    return {v for v, key in versions.items() if (folder / f"{key}.json").exists()}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--channel", default="x64-stable-multi-chrome")
    parser.add_argument("--published", type=Path, help="checkout of the offsets branch")
    parser.add_argument("--schema", help="offsets schema to check --published against")
    parser.add_argument("--limit", type=int, default=4, help="most builds to return per run")
    parser.add_argument("--github-output")
    args = parser.parse_args()
    builds = sorted(discover(args.channel).values(), key=lambda b: version_key(b["version"]), reverse=True)
    if args.published is not None:
        done = published(args.published, str(int(args.schema)))
        skipped = [b["version"] for b in builds if b["version"] in done]
        if skipped:
            print(f"already published: {', '.join(skipped)}", file=sys.stderr)
        builds = [b for b in builds if b["version"] not in done]
    if len(builds) > args.limit:
        later = [b["version"] for b in builds[args.limit:]]
        print(f"::notice::left for a later run: {', '.join(later)}", file=sys.stderr)
        builds = builds[:args.limit]
    print(json.dumps(builds, indent=2))
    if args.github_output:
        with open(args.github_output, "a", encoding="utf-8") as output:
            print(f"builds={json.dumps(builds)}", file=output)
            print(f"count={len(builds)}", file=output)


if __name__ == "__main__":
    main()
