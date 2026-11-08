"""One-shot dev loop for the sandbox Chrome: build -> stop -> install DLL -> start -> load the
extension -> verify that the new build is the one running.

    uv run --with websocket-client python tools/dev/dev.py            # rebuild DLL + restart
    uv run --with websocket-client python tools/dev/dev.py --no-build # restart only

(tools/dev/dev.ps1 wraps this.) Only the sandbox Chrome is touched, never the installed one.
Tabs come back through Chrome's own session restore.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import sandbox  # noqa: E402  (paths, kill(), install_dll(), wait_cdp())

REPO = Path(__file__).resolve().parents[2]
DLL = REPO / "artifacts" / "native" / "version.dll"
EXTENSION = REPO / "extension"
EXTENSION_ID = "egjpjafmlomkpnncliaigklbdbflface"
LOG = Path(os.environ["LOCALAPPDATA"]) / "ArborTab" / "logs" / "native.log"
STAMP = re.compile(rb"\d\d-\d\d \d\d:\d\d")


def step(text: str) -> None:
    print(f"==> {text}", flush=True)


def build() -> None:
    step("building native/version.dll")
    result = subprocess.run(["pwsh", "-NoProfile", "-File", str(REPO / "tools" / "dev" / "build-native.ps1")],
                            cwd=REPO)
    if result.returncode != 0:
        raise SystemExit("build failed; the running Chrome was left alone")


def build_stamp(dll: Path) -> str:
    match = STAMP.search(dll.read_bytes())
    return match.group().decode() if match else "?"


def launch() -> None:
    first_run = not (sandbox.PROFILE / "Default" / "Preferences").exists()
    if first_run:
        sandbox.ensure_vertical_tabs(sandbox.PROFILE)
    cmd = [
        str(sandbox.chrome_exe()),
        f"--user-data-dir={sandbox.PROFILE}",
        "--no-first-run",
        "--no-default-browser-check",
        "--restore-last-session",
        f"--remote-debugging-port={sandbox.PORT}",
        "--enable-unsafe-extension-debugging",
        "--disable-features=CalculateNativeWinOcclusion",
    ]
    subprocess.Popen(cmd, cwd=str(sandbox.APP), close_fds=True)
    sandbox.wait_cdp()


def load_extension() -> None:
    import websocket  # noqa: PLC0415

    info = sandbox.wait_cdp()
    ws = websocket.create_connection(info["webSocketDebuggerUrl"], timeout=30)
    try:
        ws.send(json.dumps({"id": 1, "method": "Extensions.loadUnpacked",
                            "params": {"path": str(EXTENSION)}}))
        while True:
            reply = json.loads(ws.recv())
            if reply.get("id") == 1:
                break
    finally:
        ws.close()
    if "error" in reply:
        raise SystemExit(f"loading the extension failed: {reply['error']}")


def wait_for_log(start: int, pattern: str, timeout: float) -> str | None:
    regex = re.compile(pattern)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if LOG.exists():
            with LOG.open("rb") as handle:
                handle.seek(start if LOG.stat().st_size >= start else 0)
                for line in handle.read().decode("utf-8", "replace").splitlines():
                    if regex.search(line):
                        return line
        time.sleep(0.3)
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--no-build", action="store_true", help="restart with the current DLL")
    args = parser.parse_args()

    if not args.no_build:
        build()
    if not DLL.exists():
        raise SystemExit(f"missing {DLL}; run without --no-build first")
    stamp = build_stamp(DLL)

    step("stopping the sandbox Chrome")
    sandbox.kill()
    step(f"installing version.dll (build {stamp})")
    sandbox.install_dll(DLL)

    log_start = LOG.stat().st_size if LOG.exists() else 0
    step("starting the sandbox Chrome")
    launch()
    step("loading the extension")
    load_extension()

    step("verifying")
    attached = wait_for_log(log_start, r"attached to browser process", 15)
    loaded = wait_for_log(log_start, r"\(build " + re.escape(stamp) + r"\)", 15)
    # Features only appear once offsets for this Chrome build are loaded.
    offsets = wait_for_log(log_start, r"features: |no usable offsets|downloads are disabled|offsets are unavailable", 20)
    connected = wait_for_log(log_start, r"client \d+ accepted", 20)
    ok = True
    for label, line in (("native module attached", attached),
                        (f"running build {stamp}", loaded),
                        ("offsets / features", offsets),
                        ("extension connected", connected)):
        mark = "OK  " if line else "FAIL"
        ok &= bool(line)
        detail = line.split("] ", 1)[-1] if line else "(not found in native.log)"
        print(f"  [{mark}] {label}: {detail}")
    print(f"\nSandbox Chrome is running build {stamp}." if ok else
          f"\nSomething is off; see {LOG}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
