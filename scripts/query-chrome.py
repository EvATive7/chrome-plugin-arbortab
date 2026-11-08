"""Query Omaha for a Windows x64 Chrome package. Prints JSON for CI."""
from __future__ import annotations

import argparse
import json
import urllib.request
import xml.etree.ElementTree as ET

REQUEST = """<?xml version="1.0" encoding="UTF-8"?>
<request protocol="3.0" updater="Omaha" ismachine="1" installsource="ondemandcheckforupdate">
  <os platform="win" version="10.0" arch="x64"/>
  <app appid="{{8A69D345-D564-463C-AFF1-A69D9E530F96}}" version="" ap="{channel}" lang="en"><updatecheck/></app>
</request>"""


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--channel", default="x64-stable-multi-chrome")
    parser.add_argument("--github-output")
    args = parser.parse_args()
    body = REQUEST.format(channel=args.channel).encode()
    request = urllib.request.Request(
        "https://tools.google.com/service/update2",
        body,
        {"Content-Type": "application/xml"},
    )
    root = ET.fromstring(urllib.request.urlopen(request, timeout=60).read())
    update = root.find("./app/updatecheck")
    if update is None or update.get("status") != "ok":
        raise RuntimeError("Omaha did not return an update")
    manifest = update.find("manifest")
    package = manifest.find("./packages/package")
    urls = [node.get("codebase") for node in update.findall("./urls/url")]
    base = next(url for url in urls if url.startswith("https://"))
    result = {
        "version": manifest.get("version"),
        "package": package.get("name"),
        "url": base + package.get("name"),
        "sha256": package.get("hash_sha256"),
        "size": int(package.get("size")),
    }
    print(json.dumps(result))
    if args.github_output:
        with open(args.github_output, "a", encoding="utf-8") as output:
            for key, value in result.items():
                print(f"{key}={value}", file=output)


if __name__ == "__main__":
    main()
