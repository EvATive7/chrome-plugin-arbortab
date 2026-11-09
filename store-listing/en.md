ArborTab — Tree-style tabs in Chrome's native vertical strip

Windows x64 only. Requires Chrome's native vertical tab strip. Full visual integration requires a separate open-source native module from GitHub. Read the security disclosure below before installing it.

ArborTab organizes Chrome tabs into a real parent-child tree while keeping them in Chrome's own vertical tab strip—no sidebar and no duplicate tab list.

Features:

- Tabs opened from links become children of their opener.
- Collapse and expand complete subtrees.
- Close a parent together with all descendants.
- Drag a tab onto another tab to nest it, or drop at an edge to reorder siblings.
- Indent guides, hidden-tab counts, theme-aware colors, and optional animation.
- Keyboard shortcuts for collapse, close-tree, indent, and outdent.
- Localized in English, German, Spanish, French, Japanese, Korean, Simplified Chinese, and Traditional Chinese.

The extension alone maintains the tree, reorders tabs, supports shortcuts, and performs tree-aware closing. Chrome's strip remains visually flat until the native module is installed.

Native module installation:

Open ArborTab's settings page after installing the extension. It links to the latest GitHub release and shows the installation command. Source code, install scripts, packages, checksums, and uninstall instructions are public:
https://github.com/EvATive7/chrome-plugin-arbortab

Installation command:
powershell -NoExit -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/EvATive7/chrome-plugin-arbortab/releases/latest/download/install.ps1 | iex"

Security disclosure:

The native component is a `version.dll` proxy installed beside `chrome.exe`. It integrates with Chrome's vertical-tab layout, painting, mouse, and drag handling. Because the distributed DLL is not Microsoft-signed, it turns off Chrome's Microsoft-signed-binaries-only mitigation for child processes so the proxy can load. This lowers their code-integrity protection. Install it only after reviewing and accepting this trade-off. The native package includes an uninstaller.

Privacy:

ArborTab does not run ads, analytics, or a project-operated server, and does not upload browsing data. The extension communicates with the native module locally through Chrome Native Messaging. The native module downloads signed compatibility data from GitHub/jsDelivr; it does not upload tab URLs, titles, or the tree. Diagnostic reports remain local unless the user explicitly exports and shares them.
