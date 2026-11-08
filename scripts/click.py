"""Clicks inside the sandbox Chrome window (window-relative coordinates).

Browser UI cannot be reached with CDP Input events, so this drives the Win32 cursor instead.

    uv run --with psutil python scripts/click.py --x 96 --y 141 [--shift]
"""
from __future__ import annotations

import argparse
import ctypes
import subprocess
import time
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)


def main_window_handle() -> int:
    return main_window_rect(return_handle=True)
    

def main_window_rect(return_handle: bool = False):
    raw = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "Get-CimInstance Win32_Process -Filter \"Name='chrome.exe'\" | "
         "Where-Object { $_.ExecutablePath -like '*_test*portable*' -and $_.CommandLine -notlike '*--type=*' } | "
         "ForEach-Object { $_.ProcessId }"],
        capture_output=True, text=True, encoding="utf-8", errors="replace").stdout
    pids = [int(x) for x in raw.split() if x.strip().isdigit()]
    if not pids:
        raise SystemExit("sandbox Chrome is not running")
    pid = pids[0]

    found: list[tuple[int, int, int, int]] = []
    handles: list[int] = []
    titles: list[str] = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def enum_proc(hwnd, _lparam):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value != pid or not user32.IsWindowVisible(hwnd):
            return True
        rect = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        if rect.right - rect.left > 400 and rect.bottom - rect.top > 300:
            found.append((rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top))
            handles.append(int(hwnd))
            buffer = ctypes.create_unicode_buffer(256)
            user32.GetWindowTextW(hwnd, buffer, 256)
            titles.append(buffer.value)
        return True

    user32.EnumWindows(enum_proc, 0)
    if not found:
        raise SystemExit(f"no visible window for pid {pid}")
    # Bring the sandbox window forward, otherwise the synthetic click goes to the console.
    user32.ShowWindow(handles[0], 9)  # SW_RESTORE
    user32.SetForegroundWindow(handles[0])
    time.sleep(0.2)
    print(f"window '{titles[0]}' rect={found[0]}")
    return handles[0] if return_handle else found[0]


def click(x: int, y: int, *, shift: bool = False, hwnd: int | None = None) -> None:
    """Posts a synthetic left click; window-relative coordinates, no focus needed.

    Posting WM_LBUTTONDOWN/UP is the only reliable way here: the terminal running this script
    usually owns the foreground, and Windows blocks foreground stealing.
    """
    if hwnd is None:
        hwnd = main_window_handle()
    point = wintypes.POINT(x, y)
    user32.ScreenToClient(wintypes.HWND(hwnd), ctypes.byref(point))
    lparam = (point.y << 16) | (point.x & 0xFFFF)
    if shift:
        user32.PostMessageW(wintypes.HWND(hwnd), 0x0100, 0x10, 0)  # WM_KEYDOWN VK_SHIFT
    user32.PostMessageW(wintypes.HWND(hwnd), 0x0201, 0x0001, lparam)  # WM_LBUTTONDOWN
    time.sleep(0.05)
    user32.PostMessageW(wintypes.HWND(hwnd), 0x0202, 0, lparam)  # WM_LBUTTONUP
    if shift:
        user32.PostMessageW(wintypes.HWND(hwnd), 0x0101, 0x10, 0)
    time.sleep(0.3)


def focus_window() -> int:
    """Brings the sandbox window to the front, which Windows only allows for trusted input."""
    handle = main_window_handle()
    user32.ShowWindow(wintypes.HWND(handle), 9)
    foreground = user32.GetForegroundWindow()
    target_thread = user32.GetWindowThreadProcessId(wintypes.HWND(handle), None)
    foreground_thread = user32.GetWindowThreadProcessId(wintypes.HWND(foreground), None) if foreground else 0
    if foreground_thread and target_thread:
        # AttachThreadInput is the classic workaround for the foreground lock.
        user32.AttachThreadInput(foreground_thread, target_thread, True)
        user32.SetForegroundWindow(wintypes.HWND(handle))
        user32.AttachThreadInput(foreground_thread, target_thread, False)
    else:
        user32.SetForegroundWindow(wintypes.HWND(handle))
    time.sleep(0.3)
    print(f"foreground now: {user32.GetForegroundWindow() == handle}")
    return handle


def click_with_cursor(x: int, y: int, *, shift: bool = False) -> None:
    rect = main_window_rect()
    left, top = rect[0], rect[1]
    focus_window()
    user32.SetCursorPos(left + x, top + y)
    time.sleep(0.15)
    if shift:
        user32.keybd_event(0x10, 0, 0, 0)
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.06)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    if shift:
        user32.keybd_event(0x10, 0, 2, 0)
    time.sleep(0.3)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--x", type=int, required=True)
    parser.add_argument("--y", type=int, required=True)
    parser.add_argument("--shift", action="store_true")
    parser.add_argument("--repeat", type=int, default=2, help="click twice by default")
    parser.add_argument("--cursor", action="store_true",
                        help="move the real cursor and click (needs the window in front)")
    args = parser.parse_args()
    handle = main_window_handle()
    for _ in range(max(1, args.repeat)):
        if args.cursor:
            click_with_cursor(args.x, args.y, shift=args.shift)
        else:
            click(args.x, args.y, shift=args.shift, hwnd=handle)
    print(f"clicked ({args.x}, {args.y})")
