// UI-thread bridge: the WebSocket thread cannot touch views, so it posts a message to a
// message-only window that lives on the browser UI thread (PLAN.md section 5.5).
#pragma once

#include <windows.h>

namespace arbortab {

// Creates the message-only window. Must be called from the UI thread (i.e. from inside a
// hooked Views call); safe to call repeatedly.
void UiThreadEnsureWindow();

// Registers a tab container view (and the layout manager that lays it out) so state changes
// can trigger a synchronous layout pass. Called on the UI thread from the layout hook.
void UiThreadRecordHostView(void* view, void* layout_manager);

// Requests a refresh from any thread. The views are validated again on the UI thread.
void UiThreadRequestRefresh();

// Arms the ~60 Hz animation frame timer (UI thread only). On the tick the queued per-tab
// work runs, and with `hosts` every recorded tab strip is invalidated and repainted.
void UiThreadScheduleFrame(bool hosts);

// Number of UI-thread refreshes performed (diagnostics).
long UiThreadRefreshCount();

}  // namespace arbortab
