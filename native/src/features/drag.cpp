#include "features/drag.h"

#include <atomic>
#include <climits>
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
#include "link/ws_server.h"

namespace arbortab {
namespace {

struct DragOffsets {
  uint32_t container_host_view = 0;  // DraggedTabsContainer::host_view_
};

DragFeature g_feature;
DragOffsets g_offsets;

// Everything below is touched from the UI thread only (the hooks run there), except the
// atomics used by diagnostics.
std::atomic<bool> g_dragging{false};
std::atomic<int> g_pressed_tab{0};  // last tab the mouse went down on: the drag source
int g_drag_source = 0;
void* g_drop_target_view = nullptr;
int g_drop_target_tab = 0;
int g_drop_zone = 0;
void* g_drop_host = nullptr;
ULONGLONG g_drop_changed_at = 0;  // the indicator fades in from here
volatile LONG g_drag_begins = 0;
volatile LONG g_drops = 0;

template <typename T>
T ReadPointer(const uint8_t* base, uint32_t offset) {
  T value = nullptr;
  memcpy(&value, base + offset, sizeof(value));
  return value;
}

// The dragged tab is shown translucent so the rows underneath stay readable. Views paint
// children separately, so only the tab's compositing layer can fade it as a whole.
struct GhostApi {
  using SetPaintToLayerFn = void (*)(void* view, int layer_type);
  using DestroyLayerFn = void (*)(void* view);
  using SetOpacityFn = void (*)(void* layer, float opacity);
  SetPaintToLayerFn set_paint_to_layer = nullptr;
  DestroyLayerFn destroy_layer = nullptr;
  SetOpacityFn set_opacity = nullptr;
  uint32_t layer_offset = 0;  // views::View -> ui::LayerOwner::layer_
};
GhostApi g_ghost;
constexpr int kLayerTextured = 1;  // ui::LayerType::LAYER_TEXTURED
constexpr float kGhostOpacity = 0.4f;
void* g_ghost_view = nullptr;
bool g_ghost_owns_layer = false;  // the layer was created for the drag and is removed after
bool g_ghost_retry_needed = false;
void* g_ghost_host = nullptr;  // container of the dragged tab

int32_t ReadInt(const uint8_t* address) {
  int32_t value = 0;
  memcpy(&value, address, sizeof(value));
  return value;
}

constexpr int kZoneNone = 0;
constexpr int kZoneNest = 1;
constexpr int kZoneBefore = 2;
constexpr int kZoneAfter = 3;
constexpr int kZoneEnd = 4;  // below the last tab: becomes the last top-level tab

const char* ZoneName(int zone) {
  switch (zone) {
    case kZoneNest: return "nest";
    case kZoneBefore: return "before";
    case kZoneAfter: return "after";
    case kZoneEnd: return "end";
    default: return "none";
  }
}

struct DropHit {
  void* view;
  int tab_id;
  int zone;
};

// Classifies the pointer against the rows of the last layout pass. Those rows are exactly what
// is on screen (indent and hidden tabs already applied), unlike views::View::children_, and
// they live in the same coordinate space as the drag point (the container's).
//
// Each row owns the band from its top to the next row's top: the top quarter inserts before
// it, the bottom quarter after it (same parent), the middle half nests under it.
DropHit FindDropTarget(const StripSnapshot& strip, int32_t pointer_y, int source_tab) {
  DropHit hit{nullptr, 0, kZoneNone};
  if (strip.count == 0) {
    return hit;
  }
  if (pointer_y >= strip.content_bottom) {
    const StripRow& last = strip.rows[strip.count - 1];
    return DropHit{last.view, last.tab_id, kZoneEnd};
  }
  for (size_t index = 0; index < strip.count; ++index) {
    const StripRow& row = strip.rows[index];
    const int32_t top = index == 0 ? INT32_MIN : row.y;
    const int32_t bottom = index + 1 < strip.count ? strip.rows[index + 1].y
                                                   : strip.content_bottom;
    if (pointer_y < top || pointer_y >= bottom) {
      continue;
    }
    if (row.tab_id == source_tab || row.height <= 0) {
      return hit;  // hovering the dragged tab itself: no drop
    }
    const int32_t offset = pointer_y - row.y;
    const int zone = offset < row.height / 4
                         ? kZoneBefore
                         : (offset >= row.height - row.height / 4 ? kZoneAfter : kZoneNest);
    return DropHit{row.view, row.tab_id, zone};
  }
  return hit;
}

// gfx::Point is two consecutive ints, already in the container's coordinates.
int32_t SafeReadPointerY(const void* point) {
  __try {
    return ReadInt(static_cast<const uint8_t*>(point) + sizeof(int32_t));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return INT32_MIN;
  }
}

void* SafeReadHostView(void* container) {
  __try {
    return ReadPointer<void*>(static_cast<uint8_t*>(container), g_offsets.container_host_view);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

void* LayerOf(void* view) {
  return ReadPointer<void*>(static_cast<uint8_t*>(view), g_ghost.layer_offset);
}

void* ViewOfTab(const StripSnapshot& strip, int tab_id) {
  for (size_t index = 0; index < strip.count; ++index) {
    if (strip.rows[index].tab_id == tab_id) {
      return strip.rows[index].view;
    }
  }
  return nullptr;
}

// (Re)applies the translucency. Chrome's animating tab layout gives views temporary layers
// and removes or resets them when its animation ends, which happens right after the drag
// starts when the dragged parent gets collapsed; so this runs again on every drag move and
// layout pass instead of once.
void GhostApply(void* view, bool may_create) {
  if (!view || !g_ghost.set_opacity || !g_ghost.layer_offset) {
    return;
  }
  __try {
    if (view != g_ghost_view) {
      g_ghost_view = view;
      g_ghost_owns_layer = false;
    }
    void* layer = LayerOf(view);
    if (!layer && !may_create) {
      g_ghost_retry_needed = true;
      return;
    }
    if (!layer && g_ghost.set_paint_to_layer && g_ghost.destroy_layer) {
      g_ghost.set_paint_to_layer(view, kLayerTextured);
      layer = LayerOf(view);
      g_ghost_owns_layer = layer != nullptr;
    }
    if (layer) {
      g_ghost.set_opacity(layer, kGhostOpacity);  // no-op when already set
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_ghost_view = nullptr;
  }
}

void GhostRetry();

// `may_create` is false inside layout passes: a layer is only created from the drag handler
// or the next frame tick.
void GhostRefreshFrom(const StripSnapshot& strip, bool may_create) {
  if (!g_dragging.load() || !g_drag_source) {
    return;
  }
  g_ghost_retry_needed = false;
  GhostApply(ViewOfTab(strip, g_drag_source), may_create);
  if (g_ghost_retry_needed) {
    RequestFrameCallback(&GhostRetry);
  }
}

void GhostRetry() {
  if (const StripSnapshot* strip = FindStripSnapshot(g_drop_host ? g_drop_host : g_ghost_host)) {
    GhostRefreshFrom(*strip, /*may_create=*/true);
  }
}

void GhostEnd() {
  void* view = g_ghost_view;
  const bool owns = g_ghost_owns_layer;
  g_ghost_view = nullptr;
  g_ghost_owns_layer = false;
  // The view may have been destroyed during the drag (closed, or moved to another window);
  // a freed TabView no longer carries its vtable.
  if (!view || !GlobalApi().IsTabView(view)) {
    return;
  }
  __try {
    if (owns) {
      g_ghost.destroy_layer(view);
    } else if (void* layer = LayerOf(view)) {
      g_ghost.set_opacity(layer, 1.f);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void Repaint(void* view) {
  if (view) {
    GlobalApi().SchedulePaint(view);
  }
}

// void DraggedTabsContainer::HandleTabDragInContainer(const gfx::Rect&, const gfx::Point&)
//
// The original computes a new strip index for the dragged tabs and moves them in the tab
// model right away: that is the "other tabs make room" reorder. In the tree strip that move is
// never wanted (the drop decides the tree change and the extension applies it), so the
// original only runs for strips we do not lay out.
using HandleDragFn = void (*)(void* self, const void* dragged_bounds, const void* point);

void HookHandleTabDragInContainer(void* self, const void* dragged_bounds, const void* point) {
  void* host = SafeReadHostView(self);
  const StripSnapshot* strip = FindStripSnapshot(host);
  const bool exact = strip != nullptr;
  if (!strip) {
    strip = LatestStripSnapshot();
  }
  const int32_t pointer_y = SafeReadPointerY(point);
  if (!strip || pointer_y == INT32_MIN) {
    reinterpret_cast<HandleDragFn>(g_feature.handle_original)(self, dragged_bounds, point);
    return;
  }

  if (!g_dragging.exchange(true)) {
    g_drag_source = g_pressed_tab.load();
    InterlockedIncrement(&g_drag_begins);
    ARB_DEBUG("drag started: source tab %d, host %p (%s snapshot, %zu rows, bottom %d), "
              "pointer_y %d",
              g_drag_source, host, exact ? "own" : "latest", strip->count,
              strip->content_bottom, pointer_y);
    BroadcastJson("{\"type\":\"dragBegin\",\"draggedTabId\":" + std::to_string(g_drag_source) +
                  "}");
  }

  g_ghost_host = strip->host_view;
  GhostRefreshFrom(*strip, /*may_create=*/true);
  const DropHit hit = FindDropTarget(*strip, pointer_y, g_drag_source);
  if (hit.view != g_drop_target_view || hit.zone != g_drop_zone) {
    void* previous = g_drop_target_view;
    g_drop_target_view = hit.view;
    g_drop_target_tab = hit.tab_id;
    const bool end_changed = (g_drop_zone == kZoneEnd) != (hit.zone == kZoneEnd);
    g_drop_zone = hit.zone;
    g_drop_host = strip->host_view;
    g_drop_changed_at = GetTickCount64();
    Repaint(previous);
    Repaint(hit.view);
    if (end_changed) {
      Repaint(g_drop_host);  // the end indicator is painted by the container
    }
    ARB_DEBUG("drop target: tab %d zone %s (pointer_y %d)", hit.tab_id, ZoneName(hit.zone),
              pointer_y);
  }
  // Swallowed on purpose: no native reorder while dragging inside the tree strip.
}

// void TabDragHandlerImpl::EndDrag(EndDragReason reason)
using EndDragFn = void (*)(void* self, int reason);

void HookEndDrag(void* self, int reason) {
  const bool was_dragging = g_dragging.exchange(false);
  const int target = g_drop_target_tab;
  const int zone = g_drop_zone;
  const int source = g_drag_source;
  void* target_view = g_drop_target_view;
  void* host = g_drop_host;
  g_drop_target_tab = 0;
  g_drop_target_view = nullptr;
  g_drop_zone = kZoneNone;
  g_drop_host = nullptr;
  g_drag_source = 0;

  g_ghost_host = nullptr;
  if (was_dragging) {
    GhostEnd();
    const bool cancelled = reason != 0;  // EndDragReason::kComplete == 0
    if (target && source && !cancelled && zone != kZoneNone) {
      InterlockedIncrement(&g_drops);
      ARB_INFO("dropped tab %d onto tab %d (%s)", source, target, ZoneName(zone));
      BroadcastJson(std::string("{\"type\":\"dropOnto\",\"draggedTabId\":") +
                    std::to_string(source) + ",\"targetTabId\":" + std::to_string(target) +
                    ",\"zone\":\"" + ZoneName(zone) + "\"}");
    } else {
      ARB_DEBUG("drag ended without a drop (reason %d, target %d, zone %s)", reason, target,
                ZoneName(zone));
    }
    BroadcastJson(std::string("{\"type\":\"dragEnd\",\"cancelled\":") +
                  (cancelled ? "true}" : "false}"));
    Repaint(target_view);
    Repaint(host);
    UiThreadRequestRefresh();
  }
  reinterpret_cast<EndDragFn>(g_feature.end_original)(self, reason);
}

bool PrologueMatches(uint8_t* base, const FunctionEntry& entry) {
  if (entry.prologue.empty()) {
    return true;
  }
  return memcmp(base + entry.rva, entry.prologue.data(), entry.prologue.size()) == 0;
}

}  // namespace

void NoteTabPressed(int tab_id) {
  if (!g_dragging.load()) {
    g_pressed_tab = tab_id;
  }
}

// 0..1 while the indicator fades in after the target changed.
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
    return;  // kZoneEnd is drawn below the last row by the container (PaintDropEnd)
  }
  const float opacity = DropIndicatorOpacity();
  if (opacity < 1.f) {
    RequestTabRepaint(tab_view);
  }
  const uint32_t accent = AccentColor(ThemeInk(tab_view));
  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);
  if (zone == kZoneNest) {
    // The tab the drop would nest under: a soft tint with a fine ring, under its contents.
    const float radius = h / 2.f < 8.f ? h / 2.f : 8.f;
    FillRoundRect(canvas, 1.f, 1.f, w - 2.f, h - 2.f, radius, Fade(WithAlpha(accent, 0x2E), opacity));
    StrokeRoundRect(canvas, 1.f, 1.f, w - 2.f, h - 2.f, radius, 1.5f, Fade(accent, opacity));
    return;
  }
  // Edge drops insert a sibling: a thin round-capped line at the edge, starting at the row's
  // own indent, which is where the dropped tab will land.
  const float y = zone == kZoneBefore ? 1.5f : h - 1.5f;
  StrokeLine(canvas, 6.f, y, w - 6.f, y, 2.f, Fade(accent, opacity));
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
  // The dropped tab becomes the last top-level tab: draw at depth 0.
  const StripRow& last = strip->rows[strip->count - 1];
  const auto left = static_cast<float>(last.x - last.shift);
  const auto right = static_cast<float>(last.x + last.width);
  const auto y = static_cast<float>(strip->content_bottom) + 1.5f;
  StrokeLine(canvas, left + 6.f, y, right - 6.f, y, 2.f,
             Fade(AccentColor(ThemeInk(host_view)), opacity));
}

void RefreshDragGhost(const void* host_view) {
  if (!g_dragging.load()) {
    return;
  }
  if (const StripSnapshot* strip = FindStripSnapshot(host_view)) {
    GhostRefreshFrom(*strip, /*may_create=*/false);
  }
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
  DragOffsets parsed;
  if (!field("DraggedTabsContainer.host_view_", parsed.container_host_view)) {
    ARB_WARN("offsets table lacks the fields the drag feature needs");
    return false;
  }
  g_offsets = parsed;

  const auto attach = [&](const char* name, void* detour, void** original, uint32_t& rva) {
    const auto found = offsets.hooks.find(name);
    if (found == offsets.hooks.end()) {
      ARB_WARN("%s is not in the offsets table", name);
      return false;
    }
    auto* base = reinterpret_cast<uint8_t*>(chrome_dll);
    if (!PrologueMatches(base, found->second)) {
      ARB_WARN("prologue mismatch for %s; drag feature disabled", name);
      return false;
    }
    if (MH_CreateHook(base + found->second.rva, detour, original) != MH_OK) {
      ARB_WARN("MH_CreateHook(%s) failed", name);
      return false;
    }
    if (MH_EnableHook(base + found->second.rva) != MH_OK) {
      MH_RemoveHook(base + found->second.rva);
      ARB_WARN("MH_EnableHook(%s) failed", name);
      return false;
    }
    rva = found->second.rva;
    ARB_INFO("hooked %s at 0x%08x", name, rva);
    return true;
  };

  const bool handle_ok =
      attach("DraggedTabsContainer::HandleTabDragInContainer",
             reinterpret_cast<void*>(&HookHandleTabDragInContainer), &g_feature.handle_original,
             g_feature.handle_rva);
  // Ending the drag is best effort: without it the state would simply never be reported.
  const bool end_ok = attach("TabDragHandlerImpl::EndDrag",
                             reinterpret_cast<void*>(&HookEndDrag), &g_feature.end_original,
                             g_feature.end_rva);

  auto* module = reinterpret_cast<uint8_t*>(chrome_dll);
  const auto function = [&offsets, module](const char* name) -> void* {
    const auto found = offsets.functions.find(name);
    return found == offsets.functions.end() ? nullptr : module + found->second.rva;
  };
  const auto base_offset = offsets.bases.find("views::View>ui::LayerOwner");
  const auto layer_field = offsets.fields.find("ui::LayerOwner.layer_");
  if (base_offset != offsets.bases.end() && layer_field != offsets.fields.end()) {
    g_ghost.layer_offset = base_offset->second + layer_field->second;
    g_ghost.set_paint_to_layer =
        reinterpret_cast<GhostApi::SetPaintToLayerFn>(function("views::View::SetPaintToLayer"));
    g_ghost.destroy_layer =
        reinterpret_cast<GhostApi::DestroyLayerFn>(function("views::View::DestroyLayer"));
    g_ghost.set_opacity = reinterpret_cast<GhostApi::SetOpacityFn>(function("ui::Layer::SetOpacity"));
  }
  ARB_INFO("translucent drag source: %s", g_ghost.set_opacity ? "on" : "off");

  feature = g_feature;
  feature.installed = handle_ok && end_ok;
  return feature.installed;
}

}  // namespace arbortab
