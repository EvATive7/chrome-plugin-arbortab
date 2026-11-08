#include "core/ui_thread.h"

#include <mutex>
#include <vector>

#include "core/chrome_api.h"
#include "core/log.h"

namespace arbortab {
namespace {

constexpr UINT kRefreshMessage = WM_APP + 0x41;
constexpr size_t kMaxHostViews = 32;

HWND g_window = nullptr;
DWORD g_ui_thread = 0;
std::mutex g_mutex;
// Views can be destroyed at any time (the strip is rebuilt on various Chrome events), and a
// stale view pointer cannot be validated reliably. Entries are therefore kept fresh by the
// layout hook and expire quickly instead of being trusted forever.
constexpr ULONGLONG kHostTtlMs = 60000;

struct HostEntry {
  void* view = nullptr;
  void* layout_manager = nullptr;
  ULONGLONG last_seen = 0;
};
std::vector<HostEntry> g_host_views;
volatile LONG g_refresh_count = 0;

// A stale view pointer would corrupt memory, so before touching one we verify that it is
// still a live container object: its vtable must be one we have seen during layout.
std::vector<void*> g_known_vtables;

bool LooksLikeTrackedHost(void* view) {
  if (!view) {
    return false;
  }
  void* vtable = *static_cast<void**>(view);
  for (void* candidate : g_known_vtables) {
    if (candidate == vtable) {
      return true;
    }
  }
  return false;
}

// Kept separate from RefreshViewsOnUiThread because __try cannot live in a function that
// needs C++ object unwinding.
bool RefreshOneHost(const HostEntry& host) {
  ChromeApi& api = GlobalApi();
  __try {
    api.InvalidateLayout(host.view);
    if (host.layout_manager) {
      api.InvalidateManager(host.layout_manager);
    }
    api.SchedulePaint(host.view);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void RefreshViewsOnUiThread() {
  std::vector<HostEntry> hosts;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    hosts = g_host_views;
  }
  const ULONGLONG now = GetTickCount64();
  size_t used = 0;
  for (const HostEntry& host : hosts) {
    if (!host.view || now - host.last_seen > kHostTtlMs || !LooksLikeTrackedHost(host.view)) {
      continue;
    }
    // Only invalidate: Chrome's own frame then runs one layout pass. Calling
    // LayoutManagerBase::Layout() synchronously from here re-enters layout and makes the
    // strip relayout forever.
    if (RefreshOneHost(host)) {
      used += 1;
    } else {
      ARB_WARN("refreshing a recorded view faulted; dropping it");
    }
  }
  const LONG count = InterlockedIncrement(&g_refresh_count);
  ARB_DEBUG("ui refresh #%ld over %zu valid hosts (of %zu recorded)", count, used, hosts.size());
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == kRefreshMessage) {
    RefreshViewsOnUiThread();
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

void UiThreadEnsureWindow() {
  if (g_window) {
    return;
  }
  const DWORD current = GetCurrentThreadId();
  if (g_ui_thread && g_ui_thread != current) {
    return;  // a different thread already owns the window
  }
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = &WindowProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"ArborTabUiThreadBridge";
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      ARB_WARN("RegisterClassExW failed: %lu", GetLastError());
      return;
    }
    registered = true;
  }
  g_window = CreateWindowExW(0, L"ArborTabUiThreadBridge", L"ArborTab", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!g_window) {
    ARB_WARN("message-only window creation failed: %lu", GetLastError());
    return;
  }
  g_ui_thread = current;
  ARB_INFO("UI thread bridge ready (thread %lu)", current);
}

void UiThreadRecordHostView(void* view, void* layout_manager) {
  if (!view) {
    return;
  }
  void* vtable = *static_cast<void**>(view);
  std::lock_guard<std::mutex> lock(g_mutex);
  bool known = false;
  for (void* candidate : g_known_vtables) {
    if (candidate == vtable) {
      known = true;
      break;
    }
  }
  if (!known) {
    if (g_known_vtables.size() < kMaxHostViews) {
      g_known_vtables.push_back(vtable);
    }
  }
  const ULONGLONG now = GetTickCount64();
  for (HostEntry& candidate : g_host_views) {
    if (candidate.view == view) {
      candidate.layout_manager = layout_manager;
      candidate.last_seen = now;
      return;
    }
  }
  if (g_host_views.size() >= kMaxHostViews) {
    g_host_views.erase(g_host_views.begin());
  }
  g_host_views.push_back(HostEntry{view, layout_manager, now});
}

void UiThreadRequestRefresh() {
  if (!g_window) {
    return;
  }
  PostMessageW(g_window, kRefreshMessage, 0, 0);
}

long UiThreadRefreshCount() { return g_refresh_count; }

}  // namespace arbortab
