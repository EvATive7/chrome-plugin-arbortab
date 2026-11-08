#include "features/drag.h"

#include <atomic>
#include <cstring>
#include <string>

#include <MinHook.h>

#include "core/chrome_api.h"
#include "core/log.h"
#include "core/state.h"
#include "core/ui_thread.h"
#include "link/ws_server.h"

namespace arbortab {
namespace {

struct DragOffsets {
  uint32_t container_host_view = 0;  // DraggedTabsContainer::host_view_
  uint32_t view_bounds = 0;          // views::View::bounds_
  uint32_t view_children = 0;        // views::View::children_
  uint32_t view_visible = 0;         // views::View::visible_
};

DragFeature g_feature;
DragOffsets g_offsets;

// Everything below is touched from the UI thread only (the hooks run there), except the
// atomics used by diagnostics.
std::atomic<bool> g_dragging{false};
void* g_drop_target_view = nullptr;
int g_drop_target_tab = 0;
void* g_previous_highlight = nullptr;
volatile LONG g_drag_begins = 0;
volatile LONG g_drops = 0;

template <typename T>
T ReadPointer(const uint8_t* base, uint32_t offset) {
  T value = nullptr;
  memcpy(&value, base + offset, sizeof(value));
  return value;
}

int32_t ReadInt(const uint8_t* address) {
  int32_t value = 0;
  memcpy(&value, address, sizeof(value));
  return value;
}

struct Rect {
  int32_t x, y, width, height;
};

Rect BoundsOf(const void* view) {
  const auto* bytes = static_cast<const uint8_t*>(view);
  return Rect{ReadInt(bytes + g_offsets.view_bounds), ReadInt(bytes + g_offsets.view_bounds + 4),
              ReadInt(bytes + g_offsets.view_bounds + 8),
              ReadInt(bytes + g_offsets.view_bounds + 12)};
}

bool Intersects(const Rect& a, const Rect& b) {
  return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

// POD result so the search can live inside __try without C++ objects.
struct DropHit {
  void* view;
  int tab_id;
};

// Finds the tab whose middle band contains the dragged tab's centre. The dragged tab itself is
// skipped by dropping candidates that overlap the dragged rectangle.
DropHit FindDropTarget(void* container, Rect dragged) {
  DropHit hit{nullptr, 0};
  if (!g_offsets.container_host_view || !g_offsets.view_children) {
    return hit;
  }
  void* host = ReadPointer<void*>(static_cast<uint8_t*>(container), g_offsets.container_host_view);
  if (!host) {
    return hit;
  }
  auto* host_bytes = static_cast<uint8_t*>(host);
  auto** begin = ReadPointer<void**>(host_bytes, g_offsets.view_children);
  auto** end = ReadPointer<void**>(host_bytes, g_offsets.view_children + 8);
  if (!begin || !end || end < begin) {
    return hit;
  }
  const uintptr_t count = static_cast<uintptr_t>(end - begin);
  if (count > 256) {
    return hit;
  }

  ChromeApi& api = GlobalApi();
  const int32_t centre_y = dragged.y + dragged.height / 2;
  for (uintptr_t index = 0; index < count; ++index) {
    void* child = begin[index];
    if (!child || !api.IsTabView(child)) {
      continue;
    }
    auto* child_bytes = static_cast<uint8_t*>(child);
    if (g_offsets.view_visible && child_bytes[g_offsets.view_visible] == 0) {
      continue;
    }
    const Rect bounds = BoundsOf(child);
    if (bounds.width <= 0 || bounds.height <= 0) {
      continue;
    }
    if (Intersects(bounds, dragged)) {
      continue;  // the dragged tab is drawn at the pointer
    }
    const int32_t band_top = bounds.y + bounds.height / 4;
    const int32_t band_bottom = bounds.y + (bounds.height * 3) / 4;
    if (centre_y < band_top || centre_y > band_bottom) {
      continue;
    }
    const int tab_id = api.TabIdOf(child);
    if (!tab_id) {
      continue;
    }
    hit.view = child;
    hit.tab_id = tab_id;
    break;
  }
  return hit;
}

void Repaint(void* view) {
  if (!view) {
    return;
  }
  ChromeApi& api = GlobalApi();
  api.SchedulePaint(view);
}

// void DraggedTabsContainer::HandleTabDragInContainer(const gfx::Rect&, const gfx::Point&)
using HandleDragFn = void (*)(void* self, const void* dragged_bounds, const void* point);

// SEH needs a function without C++ objects, hence the POD-only helper.
DropHit SafeFindDropTarget(void* container, const void* dragged_bounds) {
  DropHit hit{nullptr, 0};
  __try {
    // gfx::Rect and views::View::bounds_ share the same {x, y, width, height} layout.
    const Rect dragged = BoundsOf(dragged_bounds);
    hit = FindDropTarget(container, dragged);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    hit = DropHit{nullptr, 0};
  }
  return hit;
}

void HookHandleTabDragInContainer(void* self, const void* dragged_bounds, const void* point) {
  (void)point;
  const DropHit hit = SafeFindDropTarget(self, dragged_bounds);

  if (!g_dragging.exchange(true)) {
    InterlockedIncrement(&g_drag_begins);
    ARB_DEBUG("drag started");
    BroadcastJson("{\"type\":\"dragBegin\"}");
  }

  if (hit.view != g_drop_target_view) {
    void* previous = g_drop_target_view;
    g_drop_target_view = hit.view;
    g_drop_target_tab = hit.tab_id;
    Repaint(previous);
    Repaint(hit.view);
    if (hit.tab_id) {
      ARB_DEBUG("drop target is tab %d", hit.tab_id);
    }
  }

  if (hit.view) {
    // Swallow the move: the native code would reorder the strip, we want nesting instead.
    return;
  }
  reinterpret_cast<HandleDragFn>(g_feature.handle_original)(self, dragged_bounds, point);
}

// void TabDragHandlerImpl::EndDrag(EndDragReason reason)
using EndDragFn = void (*)(void* self, int reason);

void HookEndDrag(void* self, int reason) {
  const bool was_dragging = g_dragging.exchange(false);
  const int target = g_drop_target_tab;
  void* target_view = g_drop_target_view;
  g_drop_target_tab = 0;
  g_drop_target_view = nullptr;

  if (was_dragging) {
    const bool cancelled = reason != 0;  // EndDragReason::kComplete == 0
    if (target && !cancelled) {
      InterlockedIncrement(&g_drops);
      ARB_INFO("dropped onto tab %d", target);
      BroadcastJson("{\"type\":\"dropOnto\",\"targetTabId\":" + std::to_string(target) + "}");
    }
    BroadcastJson(std::string("{\"type\":\"dragEnd\",\"cancelled\":") +
                  (cancelled ? "true}" : "false}"));
    Repaint(target_view);
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

void PaintDropTarget(void* tab_view, void* canvas) {
  if (!tab_view || tab_view != g_drop_target_view) {
    return;
  }
  ChromeApi& api = GlobalApi();
  if (!api.fill_rect) {
    return;
  }
  const Rect bounds = BoundsOf(tab_view);
  if (bounds.width <= 0 || bounds.height <= 0) {
    return;
  }
  const DisplayConfig config = GlobalState().Config();
  // A translucent overlay marks the tab the drop would nest under.
  int32_t rect[4] = {0, 0, bounds.width, bounds.height};
  using FillRectFn = void (*)(void*, const void*, uint32_t);
  reinterpret_cast<FillRectFn>(api.fill_rect)(canvas, rect, config.highlight_color);
}

int CurrentDropTargetTabId() { return g_drop_target_tab; }

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
  if (!field("DraggedTabsContainer.host_view_", parsed.container_host_view) ||
      !field("views::View.bounds_", parsed.view_bounds) ||
      !field("views::View.children_", parsed.view_children) ||
      !field("views::View.visible_", parsed.view_visible)) {
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

  feature = g_feature;
  feature.installed = handle_ok && end_ok;
  return feature.installed;
}

}  // namespace arbortab
