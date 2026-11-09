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
import shutil
import subprocess
import sys
import time
import urllib.request
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import sandbox  # noqa: E402  (paths, kill(), install_dll(), wait_cdp())

REPO = Path(__file__).resolve().parents[2]
DLL = REPO / "artifacts" / "native" / "version.dll"
BRIDGE = REPO / "artifacts" / "native" / "arbortab-bridge.exe"
HOST_MANIFEST = REPO / "packaging" / "windows" / "com.arbortab.bridge.json"
DEV_HOST = "com.arbortab.bridge.dev"
DEV_HOST_KEY = rf"Software\Google\Chrome\NativeMessagingHosts\{DEV_HOST}"
EXTENSION_SOURCE = REPO / "extension"
DEV_EXTENSION = sandbox.SANDBOX / "extension"
EXTENSION_ID = "egjpjafmlomkpnncliaigklbdbflface"
DEV_MARKER_NAME = "__arbortabDevBuild"
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


def prepare_extension() -> str:
    """Builds a sandbox-only copy with a separate Native Messaging host and run marker.

    The checked-in extension remains byte-for-byte unchanged, so release packages can only
    contain the production host name.
    """
    marker = uuid.uuid4().hex
    if DEV_EXTENSION.exists():
        shutil.rmtree(DEV_EXTENSION)
    shutil.copytree(EXTENSION_SOURCE, DEV_EXTENSION, ignore=shutil.ignore_patterns("key.pem"))

    native_link = DEV_EXTENSION / "src" / "background" / "native-link.js"
    source = native_link.read_text(encoding="utf-8")
    production_declaration = "const NATIVE_HOST = 'com.arbortab.bridge';"
    if source.count(production_declaration) != 1:
        raise SystemExit("cannot prepare the development extension: production host declaration changed")
    native_link.write_text(
        source.replace(production_declaration, f"const NATIVE_HOST = '{DEV_HOST}';"),
        encoding="utf-8",
    )

    worker = DEV_EXTENSION / "src" / "background" / "service-worker.js"
    source = worker.read_text(encoding="utf-8")
    anchor = "\nvoid ensureInit();"
    if source.count(anchor) != 1:
        raise SystemExit("cannot prepare the development extension: service worker entry changed")
    worker.write_text(
        source.replace(anchor, f"\nglobalThis.{DEV_MARKER_NAME} = '{marker}';{anchor}"),
        encoding="utf-8",
    )
    return marker


def load_extension() -> None:
    import websocket  # noqa: PLC0415

    info = sandbox.wait_cdp()
    ws = websocket.create_connection(info["webSocketDebuggerUrl"], timeout=30)
    try:
        ws.send(json.dumps({"id": 1, "method": "Extensions.loadUnpacked",
                            "params": {"path": str(DEV_EXTENSION)}}))
        while True:
            reply = json.loads(ws.recv())
            if reply.get("id") == 1:
                break
    finally:
        ws.close()
    if "error" in reply:
        raise SystemExit(f"loading the extension failed: {reply['error']}")


def install_bridge() -> Path:
    """Installs a sandbox bridge under a development-only Native Messaging host name."""
    import winreg  # noqa: PLC0415

    target = sandbox.SANDBOX / "bridge"
    target.mkdir(exist_ok=True)
    shutil.copy2(BRIDGE, target / BRIDGE.name)
    manifest = target / f"{DEV_HOST}.json"
    payload = json.loads(HOST_MANIFEST.read_text(encoding="utf-8"))
    payload["name"] = DEV_HOST
    manifest.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, DEV_HOST_KEY) as key:
        winreg.SetValueEx(key, "", 0, winreg.REG_SZ, str(manifest))
    return manifest


def extension_workers() -> list[dict]:
    with urllib.request.urlopen(f"http://127.0.0.1:{sandbox.PORT}/json", timeout=2) as response:
        return [target for target in json.load(response)
                if target["type"] == "service_worker" and EXTENSION_ID in target["url"]]


def browser_call(method: str, params: dict | None = None) -> dict:
    import websocket  # noqa: PLC0415

    info = sandbox.wait_cdp()
    ws = websocket.create_connection(info["webSocketDebuggerUrl"], timeout=10)
    try:
        ws.send(json.dumps({"id": 1, "method": method, "params": params or {}}))
        while True:
            reply = json.loads(ws.recv())
            if reply.get("id") != 1:
                continue
            if "error" in reply:
                raise RuntimeError(f"{method} failed: {reply['error']}")
            return reply.get("result", {})
    finally:
        ws.close()


def evaluate_worker(worker: dict, expression: str) -> object:
    import websocket  # noqa: PLC0415

    ws = websocket.create_connection(worker["webSocketDebuggerUrl"], timeout=5,
                                     suppress_origin=True)
    try:
        ws.send(json.dumps({"id": 1, "method": "Runtime.evaluate", "params": {
            "expression": expression, "returnByValue": True}}))
        while True:
            reply = json.loads(ws.recv())
            if reply.get("id") != 1:
                continue
            if "error" in reply or reply.get("result", {}).get("exceptionDetails"):
                raise RuntimeError(f"worker evaluation failed: {reply}")
            return reply.get("result", {}).get("result", {}).get("value")
    finally:
        ws.close()


def wait_for_worker(exclude: set[str] | None = None, timeout: float = 40) -> dict:
    import websocket  # noqa: PLC0415

    deadline = time.time() + timeout
    wake_target = None
    try:
        while time.time() < deadline:
            for worker in extension_workers():
                if worker["id"] in (exclude or set()):
                    continue
                try:
                    ws = websocket.create_connection(worker["webSocketDebuggerUrl"], timeout=2,
                                                     suppress_origin=True)
                    ws.close()
                    return worker
                except (OSError, websocket.WebSocketException):
                    pass
            # A tab event wakes an installed MV3 worker that Chrome has left idle after
            # loadUnpacked/reload. Keep the tab only until the target becomes attachable.
            if wake_target is None:
                wake_target = browser_call("Target.createTarget", {"url": "about:blank"})["targetId"]
            time.sleep(0.2)
    finally:
        if wake_target is not None:
            try:
                browser_call("Target.closeTarget", {"targetId": wake_target})
            except Exception:
                pass
    raise SystemExit("the extension's service worker did not start")


def wait_for_worker_exit(worker_id: str, timeout: float = 3) -> None:
    """Gives a reloaded worker time to disappear, so the next attempt does not reattach to it
    and reload once more. Target ids are not guaranteed to change, so this only waits briefly
    and never excludes the id afterwards."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if all(worker["id"] != worker_id for worker in extension_workers()):
                return
        except OSError:
            pass
        time.sleep(0.2)


def reload_extension(marker: str) -> None:
    """Reloads until a worker proves that it runs this invocation's sandbox copy."""
    deadline = time.time() + 60
    last_marker = None
    while time.time() < deadline:
        worker = wait_for_worker(timeout=min(10, max(1, deadline - time.time())))
        try:
            last_marker = evaluate_worker(worker, f"globalThis.{DEV_MARKER_NAME}")
            if last_marker == marker:
                step(f"verified fresh extension worker {worker['id']} (marker {marker[:8]})")
                return
            evaluate_worker(worker, "chrome.runtime.reload()")
        except Exception:
            # Reload normally destroys the worker and its debugger connection before replying.
            pass
        wait_for_worker_exit(worker["id"])
    raise SystemExit(
        "extension reload kept a stale service worker "
        f"(expected marker {marker}, last saw {last_marker!r})"
    )


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
    step("preparing the sandbox-only extension")
    marker = prepare_extension()
    step(f"installing version.dll (build {stamp})")
    sandbox.install_dll(DLL)
    step(f"registering the development bridge: {install_bridge()}")

    log_start = LOG.stat().st_size if LOG.exists() else 0
    step("starting the sandbox Chrome")
    launch()
    step(f"loading the extension from {DEV_EXTENSION}")
    load_extension()
    connection_start = LOG.stat().st_size if LOG.exists() else 0
    reload_extension(marker)

    step("verifying")
    attached = wait_for_log(log_start, r"attached to browser process", 15)
    loaded = wait_for_log(log_start, r"\(build " + re.escape(stamp) + r"\)", 15)
    # Features only appear once offsets for this Chrome build are loaded.
    offsets = wait_for_log(log_start, r"features: |safe mode: |no usable offsets|downloads are disabled|offsets are unavailable", 20)
    connected = wait_for_log(connection_start, r"client \d+ accepted over pipe", 20)
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
