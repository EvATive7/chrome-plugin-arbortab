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
std::vector<void*> g_host_views;
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

void RefreshViewsOnUiThread() {
  std::vector<void*> views;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    views = g_host_views;
  }
  ChromeApi& api = GlobalApi();
  for (void* view : views) {
    if (!LooksLikeTrackedHost(view)) {
      continue;
    }
    api.InvalidateLayout(view);
    api.SchedulePaint(view);
  }
  InterlockedIncrement(&g_refresh_count);
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

void UiThreadRecordHostView(void* view) {
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
  for (void* candidate : g_host_views) {
    if (candidate == view) {
      return;
    }
  }
  if (g_host_views.size() >= kMaxHostViews) {
    g_host_views.erase(g_host_views.begin());
  }
  g_host_views.push_back(view);
}

void UiThreadRequestRefresh() {
  if (!g_window) {
    return;
  }
  PostMessageW(g_window, kRefreshMessage, 0, 0);
}

long UiThreadRefreshCount() { return g_refresh_count; }

}  // namespace arbortab
