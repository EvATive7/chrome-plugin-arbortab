// Renderer/child code-integrity (CIG) workaround.
//
// Chrome asks Windows to refuse non-Microsoft-signed images in its child processes
// (MITIGATION_FORCE_MS_SIGNED_BINS). Because our unsigned version.dll sits next to
// chrome.exe, every child process creation would fail with STATUS_INVALID_IMAGE_HASH,
// which leaves the browser unusable ("Aw, Snap!" on every tab). The browser process is the
// parent of those children, so we neutralise exactly that one mitigation bit while the
// attribute list is prepared.
#pragma once

namespace arbortab {

// Installs the UpdateProcThreadAttribute detour. Must run on the browser UI thread and only
// in the browser process.
bool InstallCigBypass();
// Number of mitigation-policy updates that had the CIG bit cleared (diagnostics).
long CigStripCount();

}  // namespace arbortab
