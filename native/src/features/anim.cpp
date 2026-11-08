#include "features/anim.h"

#include <cmath>
#include <vector>

#include "core/chrome_api.h"
#include "core/state.h"
#include "core/ui_thread.h"

namespace arbortab {
namespace {

constexpr size_t kMaxPendingTabs = 256;

struct PendingTab {
  void* view;
  bool relayout;
};
std::vector<PendingTab> g_pending;
std::vector<void (*)()> g_callbacks;

float EaseOutCubic(float t) {
  const float inverse = 1.f - t;
  return 1.f - inverse * inverse * inverse;
}

bool SystemAnimationsEnabled() {
  static ULONGLONG checked_at = 0;
  static BOOL enabled = TRUE;
  const ULONGLONG now = GetTickCount64();
  if (now - checked_at > 2000) {
    BOOL value = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &value, 0)) {
      enabled = value;
    }
    checked_at = now;
  }
  return enabled != FALSE;
}

void QueueTab(void* tab_view, bool relayout) {
  if (!tab_view) {
    return;
  }
  for (PendingTab& pending : g_pending) {
    if (pending.view == tab_view) {
      pending.relayout |= relayout;
      UiThreadScheduleFrame(false);
      return;
    }
  }
  if (g_pending.size() < kMaxPendingTabs) {
    g_pending.push_back(PendingTab{tab_view, relayout});
  }
  UiThreadScheduleFrame(false);
}

// The view pointer was recorded during an earlier paint. A tab closed since then has been
// freed, and PartitionAlloc overwrites the start of a freed slot (the vptr) with its free-list
// entry, so the vtable check rejects it; SEH covers whatever slips through.
void TouchTab(const PendingTab& pending) {
  ChromeApi& api = GlobalApi();
  __try {
    if (!api.IsTabView(pending.view)) {
      return;
    }
    if (pending.relayout) {
      api.InvalidateLayout(pending.view);
    }
    api.SchedulePaint(pending.view);
    if (api.off_tab_icon) {
      void* icon = *reinterpret_cast<void**>(static_cast<uint8_t*>(pending.view) + api.off_tab_icon);
      api.SchedulePaint(icon);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

}  // namespace

bool AnimationsEnabled() {
  return GlobalState().Config().animations && SystemAnimationsEnabled();
}

float Tween::At(ULONGLONG now) const {
  if (!Running(now)) {
    return to;
  }
  const float t = static_cast<float>(now - start) / static_cast<float>(duration);
  return from + (to - from) * EaseOutCubic(t < 0.f ? 0.f : t);
}

void Tween::Snap(float value) {
  from = to = value;
  duration = 0;
  start = 0;
}

void Tween::Retarget(float target, ULONGLONG now, uint32_t duration_ms) {
  if (std::fabs(target - to) < 0.001f) {
    return;
  }
  if (!duration_ms || !AnimationsEnabled()) {
    Snap(target);
    return;
  }
  from = At(now);
  to = target;
  start = now;
  duration = duration_ms;
}

void RequestAnimationFrame() { UiThreadScheduleFrame(true); }

void RequestTabRepaint(void* tab_view) { QueueTab(tab_view, false); }

void RequestTabRelayout(void* tab_view) { QueueTab(tab_view, true); }

void RequestFrameCallback(void (*callback)()) {
  for (auto existing : g_callbacks) {
    if (existing == callback) {
      UiThreadScheduleFrame(false);
      return;
    }
  }
  g_callbacks.push_back(callback);
  UiThreadScheduleFrame(false);
}

void RunPendingTabWork() {
  std::vector<void (*)()> callbacks;
  callbacks.swap(g_callbacks);
  for (auto callback : callbacks) {
    callback();
  }
  std::vector<PendingTab> pending;
  pending.swap(g_pending);
  for (const PendingTab& tab : pending) {
    TouchTab(tab);
  }
}

}  // namespace arbortab
