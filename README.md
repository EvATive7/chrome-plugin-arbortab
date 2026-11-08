<div align="center">

<img src="assets/logo/logo.svg" alt="ArborTab logo" width="112" />

# ArborTab

**Tree-style tabs inside Chrome's native vertical tab strip.**

Collapsible subtrees, close-with-children and drag-to-nest — without a sidebar, without a second tab list.

![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D4?logo=windows)
![Chrome](https://img.shields.io/badge/Chrome-vertical%20tabs-4285F4?logo=googlechrome&logoColor=white)
![Manifest](https://img.shields.io/badge/extension-Manifest%20V3-34A853)
![License](https://img.shields.io/badge/license-GPL--3.0-blue)

<img src="assets/screenshots/overview.png" alt="ArborTab running in Chrome's vertical tab strip" width="860" />

</div>

---

## ✨ Features

- 🌲 **Real tree structure.** Tabs opened from a link become children of the tab that opened them, and the strip shows them indented under their parent.
- 📂 **Collapse and expand.** Click a parent's favicon to fold its subtree away. A badge after the title shows how many tabs are hidden.
- 🧲 **Drag to nest.** Drop a tab onto the middle of another tab to make it a child, or onto an edge to place it beside it. A highlight shows where the tab will land before you let go.
- 🗑️ **Close a whole branch.** Closing a parent closes its subtree. You can have ArborTab ask first when a branch is large.
- 📏 **Indent guides.** Faint lines link children to their parent, and the active branch is drawn a little stronger.
- 🎞️ **Smooth motion.** Collapsing, expanding and re-parenting glide into place. Animations turn off when Windows animations are off.
- 🎨 **Fits your theme.** Colours come from Chrome's own theme, in light and dark mode. The drop highlight can use a custom colour.
- 🌍 **Localized** into English, German, Spanish, French, Japanese, Korean, and Simplified and Traditional Chinese.

## 📸 Screenshots

<table>
  <tr>
    <td align="center" width="42%"><img src="assets/screenshots/collapse.png" alt="A subtree collapsed and expanded" /><br /><sub>Collapse a branch: the badge counts the hidden tabs</sub></td>
    <td align="center"><img src="assets/screenshots/settings-light.png" alt="Settings page" /><br /><sub>Settings with a live preview of the strip</sub></td>
  </tr>
</table>

<details>
<summary>🌙 Settings in dark mode</summary>
<br />
<img src="assets/screenshots/settings-dark.png" alt="Settings page in dark mode" width="600" />
</details>

## 📥 Download

> 🚧 Releases are not published yet. Download links will appear here.

| Component | Download |
| --- | --- |
| 🧩 Extension | _coming soon_ |
| ⚙️ Native module (`version.dll`) | _coming soon_ |

## 🚀 Installation

1. Turn on Chrome's vertical tab strip.
2. Install the **ArborTab extension**.
3. Close Chrome, then copy **`version.dll`** next to `chrome.exe`, usually `C:\Program Files\Google\Chrome\Application\` (this needs administrator rights).
4. Start Chrome. On the extension's settings page, **Native module** should show **Connected**.

> [!NOTE]
> The extension keeps the tree on its own. The native module is what draws it in the strip: indentation, hiding, badges and drop highlights. Without the module, tabs stay flat and unchanged.

## ⌨️ Keyboard shortcuts

| Shortcut | Action |
| --- | --- |
| <kbd>Alt</kbd> + <kbd>Shift</kbd> + <kbd>,</kbd> | Collapse or expand the current tab's subtree |
| <kbd>Alt</kbd> + <kbd>Shift</kbd> + <kbd>W</kbd> | Close the current tab and its whole subtree |
| <kbd>Alt</kbd> + <kbd>Shift</kbd> + <kbd>↑</kbd> | Promote the current tab one level |
| <kbd>Alt</kbd> + <kbd>Shift</kbd> + <kbd>↓</kbd> | Nest the current tab under the previous sibling |

You can change them at `chrome://extensions/shortcuts`.

## 🔧 How it works

ArborTab has two parts that talk over a local WebSocket (`127.0.0.1`):

| Part | Role |
| --- | --- |
| **Extension** (`extension/`) | Owns the tree. It tracks parents and collapsed state, performs every move through `chrome.tabs`, and keeps the real tab order equal to the tree's pre-order walk. |
| **Native module** (`native/`) | A `version.dll` proxy that Chrome loads at startup. It hooks the vertical tab strip's layout, painting and drag handling to show the tree. |

The native module needs the internal function addresses and field offsets of each Chrome build. A [GitHub Actions workflow](.github/workflows/offsets.yml) checks for new stable releases every 6 hours. For each new release it pulls Chrome's public debug symbols, generates the offsets with [`resolver/`](resolver/), and publishes them to the `offsets` branch. The module downloads the matching table at runtime. Until a build's offsets are available, the module stays inactive and leaves Chrome untouched.

> [!IMPORTANT]
> **Security trade-off.** Chrome only lets Microsoft-signed DLLs load into its renderer and GPU processes. For an unsigned `version.dll` to work, the module turns off that one mitigation for the child processes Chrome starts, which lowers their code-integrity protection. Please weigh this before installing.

## 🛠️ Building from source

Requirements: Windows x64, Visual Studio 2022 Build Tools (MSVC), CMake + Ninja, [uv](https://github.com/astral-sh/uv) and Node.js.

```sh
# Build the native module → artifacts/native/version.dll
pwsh -File tools/dev/build-native.ps1

# Unit tests for the tree model
node --test extension/test/tree.test.mjs
```

Load `extension/` as an unpacked extension from `chrome://extensions` with Developer mode on.

For the sandboxed dev loop, end-to-end tests and the debugging notes, see [AGENTS.md](AGENTS.md).

## 📄 License

[GPL-3.0-only](LICENSE)
