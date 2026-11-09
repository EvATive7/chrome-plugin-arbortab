# ArborTab Privacy Policy

Last updated: 2026-11-09

ArborTab is an open-source Chrome extension and optional native module that organize Chrome's native vertical tab strip into a tree.

## Data ArborTab handles

The extension reads the minimum tab metadata needed to maintain and display the tree, including tab identifiers, window identifiers, tab order, opener relationships, pinned/grouped state, and tab titles. It may inspect a tab URL only to distinguish Chrome's built-in new-tab page from a page opened from a link.

Tab URLs and titles are not collected, stored in ArborTab's persistent settings, or sent to the project maintainers. Tab titles are sent over local Native Messaging to the native module in the same browser installation so it can position the collapsed-tab count beside Chrome's existing title. They do not leave the device.

ArborTab stores:

- appearance and behavior settings in `chrome.storage.sync`;
- the current tree structure in `chrome.storage.session` for browser-session recovery; and
- native-module logs, crash-guard state, signed offset tables, and configuration locally under `%LOCALAPPDATA%\ArborTab`.

Chrome may synchronize `chrome.storage.sync` settings through the user's Google account according to Chrome's own privacy policy. ArborTab does not operate a synchronization server.

## Network access

The extension does not contact ArborTab or third-party servers. Its normal connection to the optional native module uses Chrome Native Messaging and a local named pipe.

The separately installed native module makes outbound HTTPS requests to GitHub's raw-content service and jsDelivr to download signed compatibility data (offset tables and a revocation list) for the installed Chrome version. These requests necessarily expose ordinary connection metadata, such as the user's IP address, to those services. They do not contain browsing history, tab URLs, tab titles, page content, account data, or the saved tab tree. Downloaded compatibility files are data, not executable extension code, and are verified with a public key embedded in the module before use.

Links to GitHub on the settings page open only when the user chooses them.

## Diagnostics

The settings page can create a diagnostic report containing version numbers, connection and compatibility status, settings, aggregate tree sizes, recent extension events, and the tail of the native log. It is designed not to contain tab URLs or titles. The report stays on the device unless the user explicitly copies, downloads, or shares it. ArborTab never uploads diagnostic reports automatically.

## Collection, sale, and sharing

The ArborTab project does not collect, sell, rent, or use user data for advertising, analytics, credit decisions, or any unrelated purpose. ArborTab does not transmit user data to the project maintainers.

## Permissions

- `tabs`: reads opener relationships and the limited tab metadata described above, and moves or closes tabs when the user uses tree operations.
- `storage`: saves settings and session tree state.
- `nativeMessaging`: communicates locally with the separately installed native module that integrates the tree into Chrome's native vertical tab strip.

## Retention and deletion

Extension settings and session data can be removed by uninstalling the extension and clearing its data in Chrome. The native module can be removed with ArborTab's `uninstall.ps1` or `uninstall.cmd`; using the documented `-Purge` option also deletes ArborTab's local logs and cached data.

## Changes and contact

Material changes to this policy will be recorded in this repository. Questions may be filed at <https://github.com/EvATive7/chrome-plugin-arbortab/issues>.
