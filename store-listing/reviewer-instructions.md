# Reviewer instructions

ArborTab supports **Windows x64 only** and targets Chrome's native vertical tab strip.

## What works without the native module

After installing only the extension:

1. Open several tabs from links in another tab.
2. Open the extension settings page and optionally enable “The + button opens a child tab.”
3. Use the keyboard shortcuts shown at `chrome://extensions/shortcuts` to indent/outdent tabs, collapse tree state, or close a tab with its descendants.

The extension maintains parent-child relationships and keeps Chrome's tab order in tree pre-order. Without the native module Chrome's strip remains visually flat; this is expected and is clearly reported on the settings page.

## Testing the complete native-strip integration

1. Use a Windows x64 test system with Chrome's native vertical tabs enabled.
2. Review the source and releases at <https://github.com/EvATive7/chrome-plugin-arbortab>.
3. Read the security disclosure below.
4. Install the latest native package from the GitHub release, either by inspecting and running `install.cmd`, or by using the command displayed on the extension settings page.
5. Fully quit and restart Chrome.
6. Open the extension settings page. “Native module” should report “Connected.”
7. Open links from a parent tab. Child tabs should be indented. Click a parent favicon to collapse/expand its subtree, and drag a tab onto another tab to nest it.

No account or credentials are required.

## Required security disclosure

The native component is a `version.dll` proxy installed beside `chrome.exe`. It hooks Chrome's native vertical-tab UI. Because the distributed DLL is not Microsoft-signed, it clears Chrome's Microsoft-signed-binaries-only mitigation bit for child processes so the proxy can load there. This reduces code-integrity protection. The store description, README, settings page, and installer path disclose this behavior before installation. The native package includes an uninstaller.

The extension itself contains no remote code and does not contact a service. The native module downloads only signed, version-specific compatibility data from GitHub/jsDelivr and does not upload browsing data.
