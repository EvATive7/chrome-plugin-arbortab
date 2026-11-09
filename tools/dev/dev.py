"""One-shot dev loop for the sandbox Chrome: build -> stop -> install DLL and bridge -> start ->
load (and reload) the extension -> verify that the new build is the one running.

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
BRIDGE = REPO / "artifacts" / "native" / "arbortab-bridge.exe"
HOST_MANIFEST = REPO / "packaging" / "windows" / "com.arbortab.bridge.json"
HOST_KEY = r"Software\Google\Chrome\NativeMessagingHosts\com.arbortab.bridge"
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


def install_bridge() -> Path:
    """Copies the bridge into the sandbox folder (a running copy never locks artifacts/) and
    registers it as the Native Messaging host. The registration is per user and read by every
    Chrome, but only the extension IDs in allowed_origins can start it."""
    import shutil  # noqa: PLC0415
    import winreg  # noqa: PLC0415

    target = sandbox.SANDBOX / "bridge"
    target.mkdir(exist_ok=True)
    shutil.copy2(BRIDGE, target / BRIDGE.name)
    manifest = target / HOST_MANIFEST.name
    shutil.copy2(HOST_MANIFEST, manifest)
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, HOST_KEY) as key:
        winreg.SetValueEx(key, "", 0, winreg.REG_SZ, str(manifest))
    return manifest


def reload_extension() -> None:
    """Chrome keeps running a cached service worker while the version stays the same, even
    after loadUnpacked (AGENTS.md, pitfall 17): reload it so the current code runs."""
    import websocket  # noqa: PLC0415

    # loadUnpacked may restart the worker right away: retry until one accepts the connection.
    for _ in range(50):
        targets = json.load(urllib.request.urlopen(f"http://127.0.0.1:{sandbox.PORT}/json"))
        worker = next((t for t in targets if t["type"] == "service_worker" and EXTENSION_ID in t["url"]), None)
        if worker:
            try:
                ws = websocket.create_connection(worker["webSocketDebuggerUrl"], timeout=10,
                                                 suppress_origin=True)
                break
            except websocket.WebSocketException:
                pass
        time.sleep(0.2)
    else:
        raise SystemExit("the extension's service worker did not start")
    try:
        ws.send(json.dumps({"id": 1, "method": "Runtime.evaluate",
                            "params": {"expression": "chrome.runtime.reload()"}}))
        ws.recv()
    except Exception:  # noqa: BLE001  the worker goes away while reloading
        pass
    finally:
        ws.close()


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
    for artifact in (DLL, BRIDGE):
        if not artifact.exists():
            raise SystemExit(f"missing {artifact}; run without --no-build first")
    stamp = build_stamp(DLL)

    step("stopping the sandbox Chrome")
    sandbox.kill()
    step(f"installing version.dll (build {stamp})")
    sandbox.install_dll(DLL)
    step(f"registering the bridge: {install_bridge()}")

    log_start = LOG.stat().st_size if LOG.exists() else 0
    step("starting the sandbox Chrome")
    launch()
    step("loading the extension")
    load_extension()
    reload_extension()

    step("verifying")
    attached = wait_for_log(log_start, r"attached to browser process", 15)
    loaded = wait_for_log(log_start, r"\(build " + re.escape(stamp) + r"\)", 15)
    # Features only appear once offsets for this Chrome build are loaded.
    offsets = wait_for_log(log_start, r"features: |safe mode: |no usable offsets|downloads are disabled|offsets are unavailable", 20)
    connected = wait_for_log(log_start, r"client \d+ accepted over pipe", 20)
    ok = True
    for label, line in (("native module attached", attached),
                        (f"running build {stamp}", loaded),
                        ("offsets / features", offsets),
                        ("extension connected through the bridge", connected)):
        mark = "OK  " if line else "FAIL"
        ok &= bool(line)
        detail = line.split("] ", 1)[-1] if line else "(not found in native.log)"
        print(f"  [{mark}] {label}: {detail}")
    print(f"\nSandbox Chrome is running build {stamp}." if ok else
          f"\nSomething is off; see {LOG}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
