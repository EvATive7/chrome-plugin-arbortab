#include "features/drag.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstring>
#include <string>

#include <MinHook.h>

#include "core/chrome_api.h"
#include "core/log.h"
#include "core/state.h"
#include "core/ui_thread.h"
#include "features/anim.h"
#include "features/layout.h"
#include "features/paint.h"
#include "link/server.h"

namespace arbortab {
namespace {

struct DragGeometry {
  uint32_t event_flags = 0;
  uint32_t event_location = 0;
};

DragFeature g_feature;
DragGeometry g_geometry;
std::atomic<bool> g_feature_ready{false};
std::atomic<bool> g_dragging{false};

// Hook state is UI-thread-only. The atomics above are also read by diagnostics/painting.
bool g_press_owned = false;
bool g_suppress_initialize = false;
void* g_pressed_view = nullptr;
void* g_pressed_host = nullptr;
HWND g_pressed_window = nullptr;
int g_pressed_tab = 0;
float g_press_x = 0.f;
float g_press_y = 0.f;
int g_drag_source = 0;
bool g_drag_outside = false;
void* g_drop_target_view = nullptr;
int g_drop_target_tab = 0;
int g_drop_zone = 0;
void* g_drop_host = nullptr;
ULONGLONG g_drop_changed_at = 0;
volatile LONG g_drag_begins = 0;
volatile LONG g_drops = 0;
volatile LONG g_drag_outs = 0;

// ui::EventFlags (ui/events/event_constants.h). Modifiers remain allowed; only extra mouse
// buttons disqualify a press, matching MouseEvent::IsOnlyLeftMouseButton().
constexpr uint32_t kSynthesizedEvent = 0x0001;
constexpr uint32_t kLeftMouseButton = 0x0800;
constexpr uint32_t kMouseButtonMask = 0xF800;
constexpr uint32_t kFromTouch = 0x80000;

constexpr int kZoneNone = 0;
constexpr int kZoneNest = 1;
constexpr int kZoneBefore = 2;
constexpr int kZoneAfter = 3;
constexpr int kZoneEnd = 4;

const char* ZoneName(int zone) {
  switch (zone) {
    case kZoneNest:
      return "nest";
    case kZoneBefore:
      return "before";
    case kZoneAfter:
      return "after";
    case kZoneEnd:
      return "end";
    default:
      return "none";
  }
}

int32_t ReadInt(const uint8_t* address) {
  int32_t value = 0;
  memcpy(&value, address, sizeof(value));
  return value;
}

float ReadFloat(const uint8_t* address) {
  float value = 0.f;
  memcpy(&value, address, sizeof(value));
  return value;
}

bool IsTreeTab(int tab_id) {
  TabDisplayState state;
  return tab_id != 0 && GlobalState().Find(tab_id, state) && state.tree_tab;
}

struct EventPoint {
  uint32_t flags = 0;
  float x = 0.f;
  float y = 0.f;
  bool valid = false;
};

EventPoint ReadEventPoint(const void* event) {
  EventPoint point;
  __try {
    const auto* bytes = static_cast<const uint8_t*>(event);
    point.flags = *reinterpret_cast<const uint32_t*>(bytes + g_geometry.event_flags);
    point.x = ReadFloat(bytes + g_geometry.event_location);
    point.y = ReadFloat(bytes + g_geometry.event_location + sizeof(float));
    point.valid = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return point;
}

void Repaint(void* view) {
  if (view) {
    GlobalApi().SchedulePaint(view);
  }
}

// The dragged tab is shown translucent so rows underneath stay readable. It is applied exactly
// once at drag begin; ArborTab no longer retries from layout passes.
struct GhostApi {
  using SetPaintToLayerFn = void (*)(void* view, int layer_type);
  using DestroyLayerFn = void (*)(void* view);
  using SetOpacityFn = void (*)(void* layer, float opacity);
  SetPaintToLayerFn set_paint_to_layer = nullptr;
  DestroyLayerFn destroy_layer = nullptr;
  SetOpacityFn set_opacity = nullptr;
  uint32_t layer_offset = 0;
};
GhostApi g_ghost;
constexpr int kLayerTextured = 1;
constexpr float kGhostOpacity = 0.4f;
void* g_ghost_view = nullptr;
bool g_ghost_owns_layer = false;

template <typename T>
T ReadPointer(const uint8_t* base, uint32_t offset) {
  T value = nullptr;
  memcpy(&value, base + offset, sizeof(value));
  return value;
}

void* LayerOf(void* view) {
  return ReadPointer<void*>(static_cast<uint8_t*>(view), g_ghost.layer_offset);
}

void GhostApply(void* view) {
  if (!view || !g_ghost.set_opacity || !g_ghost.layer_offset) {
    return;
  }
  __try {
    g_ghost_view = view;
    g_ghost_owns_layer = false;
    void* layer = LayerOf(view);
    if (!layer && g_ghost.set_paint_to_layer && g_ghost.destroy_layer) {
      g_ghost.set_paint_to_layer(view, kLayerTextured);
      layer = LayerOf(view);
      g_ghost_owns_layer = layer != nullptr;
    }
    if (layer) {
      g_ghost.set_opacity(layer, kGhostOpacity);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_ghost_view = nullptr;
    g_ghost_owns_layer = false;
  }
}

void GhostEnd() {
  void* view = g_ghost_view;
  const bool owns = g_ghost_owns_layer;
  g_ghost_view = nullptr;
  g_ghost_owns_layer = false;
  if (!view || !GlobalApi().IsTabView(view)) {
    return;
  }
  __try {
    if (owns && g_ghost.destroy_layer) {
      g_ghost.destroy_layer(view);
    } else if (void* layer = LayerOf(view)) {
      g_ghost.set_opacity(layer, 1.f);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

struct DropHit {
  void* view = nullptr;
  int tab_id = 0;
  int zone = kZoneNone;
};

DropHit FindDropTarget(const StripSnapshot& strip, int32_t pointer_y, int source_tab) {
  if (strip.count == 0) {
    return {};
  }
  if (pointer_y >= strip.content_bottom) {
    for (size_t index = strip.count; index > 0; --index) {
      const StripRow& row = strip.rows[index - 1];
      if (row.tab_id != source_tab && IsTreeTab(row.tab_id)) {
        return DropHit{row.view, row.tab_id, kZoneEnd};
      }
    }
    return {};
  }
  for (size_t index = 0; index < strip.count; ++index) {
    const StripRow& row = strip.rows[index];
    const int32_t top = index == 0 ? INT32_MIN : row.y;
    const int32_t bottom = index + 1 < strip.count ? strip.rows[index + 1].y
                                                   : strip.content_bottom;
    if (pointer_y < top || pointer_y >= bottom) {
      continue;
    }
    if (row.tab_id == source_tab || row.height <= 0 || !IsTreeTab(row.tab_id)) {
      return {};
    }
    const int32_t offset = pointer_y - row.y;
    const int zone = offset < row.height / 4
                         ? kZoneBefore
                         : (offset >= row.height - row.height / 4 ? kZoneAfter : kZoneNest);
    return DropHit{row.view, row.tab_id, zone};
  }
  return {};
}

void SetDropHit(const StripSnapshot& strip, const DropHit& hit) {
  if (hit.view == g_drop_target_view && hit.zone == g_drop_zone &&
      strip.host_view == g_drop_host) {
    return;
  }
  void* previous = g_drop_target_view;
  void* previous_host = g_drop_host;
  const bool end_changed = (g_drop_zone == kZoneEnd) != (hit.zone == kZoneEnd) ||
                           previous_host != strip.host_view;
  g_drop_target_view = hit.view;
  g_drop_target_tab = hit.tab_id;
  g_drop_zone = hit.zone;
  g_drop_host = strip.host_view;
  g_drop_changed_at = GetTickCount64();
  Repaint(previous);
  Repaint(hit.view);
  if (end_changed) {
    Repaint(previous_host);
    Repaint(g_drop_host);
  }
  ARB_DEBUG("drop target: tab %d zone %s", hit.tab_id, ZoneName(hit.zone));
}

void ClearDropHit() {
  void* target = g_drop_target_view;
  void* host = g_drop_host;
  g_drop_target_view = nullptr;
  g_drop_target_tab = 0;
  g_drop_zone = kZoneNone;
  g_drop_host = nullptr;
  Repaint(target);
  Repaint(host);
}

void ClearPress() {
  g_press_owned = false;
  g_suppress_initialize = false;
  g_pressed_view = nullptr;
  g_pressed_host = nullptr;
  g_pressed_window = nullptr;
  g_pressed_tab = 0;
  g_press_x = 0.f;
  g_press_y = 0.f;
}

bool CursorInBrowserWindowDip(const POINT& cursor, int& window_x, int& window_y) {
  HWND window = g_pressed_window;
  if (window && !IsWindow(window)) {
    window = nullptr;
  }
  if (!window) {
    window = GetCapture();
  }
  if (window) {
    window = GetAncestor(window, GA_ROOT);
  }
  if (!window) {
    window = GetAncestor(WindowFromPoint(cursor), GA_ROOT);
  }
  DWORD process_id = 0;
  GetWindowThreadProcessId(window, &process_id);
  if (process_id != GetCurrentProcessId()) {
    window = nullptr;
  }
  RECT rect{};
  if (!window || !GetWindowRect(window, &rect)) {
    return false;
  }
  const UINT dpi = std::max<UINT>(96, GetDpiForWindow(window));
  window_x = static_cast<int>(std::lround(
      static_cast<double>(cursor.x - rect.left) * 96.0 / static_cast<double>(dpi)));
  window_y = static_cast<int>(std::lround(
      static_cast<double>(cursor.y - rect.top) * 96.0 / static_cast<double>(dpi)));
  return true;
}

void BeginOwnDrag() {
  g_drag_source = g_pressed_tab;
  g_drag_outside = false;
  g_dragging = true;
  InterlockedIncrement(&g_drag_begins);
  GhostApply(g_pressed_view);
  ARB_DEBUG("owned drag started: source tab %d, host %p", g_drag_source, g_pressed_host);
  BroadcastJson("{\"type\":\"dragBegin\",\"draggedTabId\":" +
                std::to_string(g_drag_source) + "}");
}

void UpdateOwnedDrag(const EventPoint& point) {
  const StripRow* source_row = nullptr;
  const StripSnapshot* strip = FindStripSnapshotForTabView(g_pressed_view, &source_row);
  if (!strip || !source_row || strip->host_view != g_pressed_host) {
    return;
  }
  const int32_t pointer_x = source_row->x + static_cast<int32_t>(std::lround(point.x));
  const int32_t pointer_y = source_row->y + static_cast<int32_t>(std::lround(point.y));
  int32_t left = INT32_MAX;
  int32_t right = INT32_MIN;
  int32_t top = INT32_MAX;
  for (size_t index = 0; index < strip->count; ++index) {
    const StripRow& row = strip->rows[index];
    left = std::min(left, row.x - row.shift);
    right = std::max(right, row.x + row.width);
    top = std::min(top, row.y);
  }
  const int margin = std::max(16, 2 * std::max(GetSystemMetrics(SM_CXDRAG),
                                               GetSystemMetrics(SM_CYDRAG)));
  g_drag_outside = strip->count == 0 || pointer_x < left - margin || pointer_x > right + margin ||
                   pointer_y < top - margin || pointer_y > strip->content_bottom + margin;
  if (g_drag_outside) {
    ClearDropHit();
    return;
  }
  SetDropHit(*strip, FindDropTarget(*strip, pointer_y, g_drag_source));
}

void EndOwnDrag(bool cancelled) {
  if (!g_dragging.exchange(false)) {
    ClearPress();
    return;
  }
  const int source = g_drag_source;
  const int target = g_drop_target_tab;
  const int zone = g_drop_zone;
  const bool outside = g_drag_outside;
  POINT cursor{};
  GetCursorPos(&cursor);
  int window_x = 0;
  int window_y = 0;
  const bool has_window_point = CursorInBrowserWindowDip(cursor, window_x, window_y);

  GhostEnd();
  ClearDropHit();
  g_drag_source = 0;
  g_drag_outside = false;
  ClearPress();

  if (!cancelled && outside && source) {
    InterlockedIncrement(&g_drag_outs);
    std::string message =
        "{\"type\":\"dragOut\",\"draggedTabId\":" + std::to_string(source);
    if (has_window_point) {
      message += ",\"windowX\":" + std::to_string(window_x) +
                 ",\"windowY\":" + std::to_string(window_y);
      ARB_INFO("dragged tab %d out to window DIP %d,%d (screen px %ld,%ld)", source,
               window_x, window_y, cursor.x, cursor.y);
    } else {
      ARB_WARN("dragged tab %d out without a browser-relative cursor point", source);
    }
    BroadcastJson(message + "}");
  } else if (!cancelled && source && target && zone != kZoneNone) {
    InterlockedIncrement(&g_drops);
    ARB_INFO("dropped tab %d onto tab %d (%s)", source, target, ZoneName(zone));
    BroadcastJson("{\"type\":\"dropOnto\",\"draggedTabId\":" + std::to_string(source) +
                  ",\"targetTabId\":" + std::to_string(target) + ",\"zone\":\"" +
                  ZoneName(zone) + "\"}");
  } else {
    ARB_DEBUG("owned drag ended without a drop (cancelled %d, target %d, zone %s)",
              cancelled ? 1 : 0, target, ZoneName(zone));
  }
  BroadcastJson(std::string("{\"type\":\"dragEnd\",\"cancelled\":") +
                (cancelled ? "true}" : "false}"));
  UiThreadRequestRefresh();
}

using InitializeDragFn = void (*)(void* self, void* node, const void* selection,
                                  const void* event);
using OnMouseDraggedFn = bool (*)(void* self, const void* event);
using OnMouseReleasedFn = void (*)(void* self, const void* event);
using OnMouseCaptureLostFn = void (*)(void* self);

void HookInitializeDrag(void* self, void* node, const void* selection, const void* event) {
  if (g_suppress_initialize && g_press_owned) {
    return;
  }
  reinterpret_cast<InitializeDragFn>(g_feature.initialize_original)(self, node, selection, event);
}

bool HookOnMouseDragged(void* self, const void* event) {
  if (!g_press_owned || self != g_pressed_view) {
    return reinterpret_cast<OnMouseDraggedFn>(g_feature.dragged_original)(self, event);
  }
  const EventPoint point = ReadEventPoint(event);
  if (!point.valid) {
    EndOwnDrag(true);
    return true;
  }
  if (!g_dragging.load()) {
    const int threshold_x = std::max(1, GetSystemMetrics(SM_CXDRAG));
    const int threshold_y = std::max(1, GetSystemMetrics(SM_CYDRAG));
    if (std::fabs(point.x - g_press_x) < threshold_x &&
        std::fabs(point.y - g_press_y) < threshold_y) {
      return true;
    }
    BeginOwnDrag();
  }
  UpdateOwnedDrag(point);
  return true;
}

void HookOnMouseReleased(void* self, const void* event) {
  if (!g_press_owned || self != g_pressed_view) {
    reinterpret_cast<OnMouseReleasedFn>(g_feature.released_original)(self, event);
    return;
  }
  if (g_dragging.load()) {
    const EventPoint point = ReadEventPoint(event);
    if (point.valid) {
      UpdateOwnedDrag(point);
    }
    EndOwnDrag(false);
    return;  // ArborTab owns a drag release; do not re-select or invoke native EndDrag.
  }
  ClearPress();
  // A click never crossed the threshold: preserve Chrome's release selection and cleanup.
  reinterpret_cast<OnMouseReleasedFn>(g_feature.released_original)(self, event);
}

void HookOnMouseCaptureLost(void* self) {
  if (g_press_owned) {
    EndOwnDrag(g_dragging.load());
  }
  reinterpret_cast<OnMouseCaptureLostFn>(g_feature.capture_lost_original)(self);
}

bool PrologueMatches(uint8_t* base, const FunctionEntry& entry) {
  return entry.prologue.empty() ||
         memcmp(base + entry.rva, entry.prologue.data(), entry.prologue.size()) == 0;
}

}  // namespace

void PrepareOwnDragPress(void* tab_view, const void* event) {
  if (!g_feature_ready.load() || g_dragging.load()) {
    return;
  }
  ClearPress();
  const EventPoint point = ReadEventPoint(event);
  if (!point.valid || (point.flags & kMouseButtonMask) != kLeftMouseButton ||
      (point.flags & (kSynthesizedEvent | kFromTouch)) != 0) {
    return;
  }
  const StripRow* row = nullptr;
  const StripSnapshot* strip = FindStripSnapshotForTabView(tab_view, &row);
  if (!strip || !row || !IsTreeTab(row->tab_id)) {
    return;
  }
  g_pressed_view = tab_view;
  g_pressed_host = strip->host_view;
  POINT cursor{};
  if (GetCursorPos(&cursor)) {
    HWND window = GetAncestor(WindowFromPoint(cursor), GA_ROOT);
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id == GetCurrentProcessId()) {
      g_pressed_window = window;
    }
  }
  g_pressed_tab = row->tab_id;
  g_press_x = point.x;
  g_press_y = point.y;
  g_press_owned = true;
  g_suppress_initialize = true;
}

void FinishOwnDragPress(bool handled) {
  g_suppress_initialize = false;
  if (g_press_owned && !handled) {
    ClearPress();
  }
}

float DropIndicatorOpacity() {
  if (!AnimationsEnabled()) {
    return 1.f;
  }
  Tween fade;
  fade.from = 0.f;
  fade.to = 1.f;
  fade.start = g_drop_changed_at;
  fade.duration = kFadeMs;
  return fade.At(GetTickCount64());
}

void PaintDropTarget(void* tab_view, void* canvas, int32_t width, int32_t height) {
  if (!tab_view || tab_view != g_drop_target_view || width <= 0 || height <= 0) {
    return;
  }
  const int zone = g_drop_zone;
  if (zone != kZoneNest && zone != kZoneBefore && zone != kZoneAfter) {
    return;
  }
  const float opacity = DropIndicatorOpacity();
  if (opacity < 1.f) {
    RequestTabRepaint(tab_view);
  }
  const uint32_t accent = AccentColor(ThemeInk(tab_view));
  const auto width_f = static_cast<float>(width);
  const auto height_f = static_cast<float>(height);
  if (zone == kZoneNest) {
    const float radius = height_f / 2.f < 8.f ? height_f / 2.f : 8.f;
    FillRoundRect(canvas, 1.f, 1.f, width_f - 2.f, height_f - 2.f, radius,
                  Fade(WithAlpha(accent, 0x2E), opacity));
    StrokeRoundRect(canvas, 1.f, 1.f, width_f - 2.f, height_f - 2.f, radius, 1.5f,
                    Fade(accent, opacity));
    return;
  }
  const float y = zone == kZoneBefore ? 1.5f : height_f - 1.5f;
  StrokeLine(canvas, 6.f, y, width_f - 6.f, y, 2.f, Fade(accent, opacity));
}

void PaintDropEnd(const void* host_view, void* canvas) {
  if (!host_view || host_view != g_drop_host || g_drop_zone != kZoneEnd) {
    return;
  }
  const StripSnapshot* strip = FindStripSnapshot(host_view);
  if (!strip || strip->count == 0) {
    return;
  }
  const float opacity = DropIndicatorOpacity();
  if (opacity < 1.f) {
    RequestAnimationFrame();
  }
  const StripRow& last = strip->rows[strip->count - 1];
  const auto left = static_cast<float>(last.x - last.shift);
  const auto right = static_cast<float>(last.x + last.width);
  const auto y = static_cast<float>(strip->content_bottom) + 1.5f;
  StrokeLine(canvas, left + 6.f, y, right - 6.f, y, 2.f,
             Fade(AccentColor(ThemeInk(host_view)), opacity));
}

int CurrentDropTargetTabId() { return g_drop_target_tab; }

int CurrentDropZone() { return g_drop_zone; }

bool DragInProgress() { return g_dragging.load(); }

bool InstallDragFeature(HMODULE chrome_dll, const Offsets& offsets, DragFeature& feature) {
  const auto field = [&offsets](const char* name, uint32_t& out) {
    const auto found = offsets.fields.find(name);
    if (found == offsets.fields.end()) {
      return false;
    }
    out = found->second;
    return true;
  };
  if (!field("ui::Event.flags_", g_geometry.event_flags) ||
      !field("ui::LocatedEvent.location_", g_geometry.event_location)) {
    ARB_WARN("offsets table lacks the event fields the drag feature needs");
    return false;
  }

  constexpr const char* kConcreteInitialize = "TabDragHandler::InitializeDrag";
  constexpr const char* kImplInitialize = "TabDragHandlerImpl::InitializeDrag";
  const bool concrete = offsets.hooks.count(kConcreteInitialize) != 0;
  const bool impl = offsets.hooks.count(kImplInitialize) != 0;
  if (concrete == impl) {
    ARB_WARN("offsets table must contain exactly one InitializeDrag hook target");
    return false;
  }
  const char* initialize_name = concrete ? kConcreteInitialize : kImplInitialize;

  struct HookPlan {
    const char* name;
    void* detour;
    void** original;
    uint32_t* rva;
    void* target = nullptr;
  };
  HookPlan plans[] = {
      {initialize_name, reinterpret_cast<void*>(&HookInitializeDrag),
       &g_feature.initialize_original, &g_feature.initialize_rva},
      {"TabView::OnMouseDragged", reinterpret_cast<void*>(&HookOnMouseDragged),
       &g_feature.dragged_original, &g_feature.dragged_rva},
      {"TabView::OnMouseReleased", reinterpret_cast<void*>(&HookOnMouseReleased),
       &g_feature.released_original, &g_feature.released_rva},
      {"views::internal::RootView::OnMouseCaptureLost",
       reinterpret_cast<void*>(&HookOnMouseCaptureLost), &g_feature.capture_lost_original,
       &g_feature.capture_lost_rva},
  };

  auto* base = reinterpret_cast<uint8_t*>(chrome_dll);
  for (HookPlan& plan : plans) {
    const auto found = offsets.hooks.find(plan.name);
    if (found == offsets.hooks.end()) {
      ARB_WARN("%s is not in the offsets table; drag feature disabled", plan.name);
      return false;
    }
    if (!PrologueMatches(base, found->second)) {
      ARB_WARN("prologue mismatch for %s; drag feature disabled", plan.name);
      return false;
    }
    plan.target = base + found->second.rva;
    *plan.rva = found->second.rva;
  }

  constexpr size_t kHookCount = sizeof(plans) / sizeof(plans[0]);
  size_t created = 0;
  for (; created < kHookCount; ++created) {
    HookPlan& plan = plans[created];
    if (MH_CreateHook(plan.target, plan.detour, plan.original) != MH_OK) {
      ARB_WARN("MH_CreateHook(%s) failed; drag feature disabled", plan.name);
      break;
    }
  }
  if (created != kHookCount) {
    while (created > 0) {
      MH_RemoveHook(plans[--created].target);
    }
    return false;
  }
  bool queued = true;
  for (HookPlan& plan : plans) {
    if (MH_QueueEnableHook(plan.target) != MH_OK) {
      ARB_WARN("MH_QueueEnableHook(%s) failed; drag feature disabled", plan.name);
      queued = false;
      break;
    }
  }
  if (!queued || MH_ApplyQueued() != MH_OK) {
    ARB_WARN("could not enable the owned-drag hook set; drag feature disabled");
    for (HookPlan& plan : plans) {
      MH_DisableHook(plan.target);
      MH_RemoveHook(plan.target);
    }
    return false;
  }
  for (const HookPlan& plan : plans) {
    ARB_INFO("hooked %s at 0x%08x", plan.name, *plan.rva);
  }

  const auto function = [&offsets, base](const char* name) -> void* {
    const auto found = offsets.functions.find(name);
    return found == offsets.functions.end() ? nullptr : base + found->second.rva;
  };
  const auto base_offset = offsets.bases.find("views::View>ui::LayerOwner");
  const auto layer_field = offsets.fields.find("ui::LayerOwner.layer_");
  if (base_offset != offsets.bases.end() && layer_field != offsets.fields.end()) {
    g_ghost.layer_offset = base_offset->second + layer_field->second;
    g_ghost.set_paint_to_layer =
        reinterpret_cast<GhostApi::SetPaintToLayerFn>(function("views::View::SetPaintToLayer"));
    g_ghost.destroy_layer =
        reinterpret_cast<GhostApi::DestroyLayerFn>(function("views::View::DestroyLayer"));
    g_ghost.set_opacity =
        reinterpret_cast<GhostApi::SetOpacityFn>(function("ui::Layer::SetOpacity"));
  }
  ARB_INFO("one-shot translucent drag source: %s", g_ghost.set_opacity ? "on" : "off");

  g_feature.installed = true;
  feature = g_feature;
  g_feature_ready = true;
  return true;
}

}  // namespace arbortab
