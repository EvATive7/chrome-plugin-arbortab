// UI-thread bridge: the WebSocket thread cannot touch views, so it posts a message to a
// message-only window that lives on the browser UI thread (PLAN.md section 5.5).
#pragma once

#include <windows.h>

namespace arbortab {

// Creates the message-only window. Must be called from the UI thread (i.e. from inside a
// hooked Views call); safe to call repeatedly.
void UiThreadEnsureWindow();

// Registers a tab container view that should be re-laid out and repainted after state
// changes. Called from the UI thread on every layout pass; stale views are dropped.
void UiThreadRecordHostView(void* view);

// Requests a refresh from any thread. The views are validated again on the UI thread.
void UiThreadRequestRefresh();

// Number of UI-thread refreshes performed (diagnostics).
long UiThreadRefreshCount();

}  // namespace arbortab
