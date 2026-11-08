#include "features/mouse.h"

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

// ui::EventFlags (ui/events/event_constants.h)
constexpr uint32_t kEventModifierMask = 0x1E;  // shift | control | alt | command
constexpr uint32_t kLeftMouseButton = 0x100;

// ui::LocatedEvent::location_ is a gfx::PointF at +0xb0 (fields come from the offsets table).
struct Geometry {
  uint32_t event_flags = 0;
  uint32_t event_location = 0;
  uint32_t view_bounds = 0;
  uint32_t tab_icon = 0;
  uint32_t tab_close_button = 0;
};

MouseFeature g_mouse;
PaintFeature g_paint;
Geometry g_geometry;
volatile LONG g_twisty_clicks = 0;
volatile LONG g_count_labels = 0;
TabDisplayState g_candidate_state;

float ReadFloat(const uint8_t* address) {
  float value = 0;
  memcpy(&value, address, sizeof(value));
  return value;
}

int32_t ReadInt(const uint8_t* address) {
  int32_t value = 0;
  memcpy(&value, address, sizeof(value));
  return value;
}

template <typename T>
T ReadPointer(const uint8_t* base, uint32_t offset) {
  T value = nullptr;
  memcpy(&value, base + offset, sizeof(value));
  return value;
}

// Returns true when the point lies inside the favicon of this tab.
bool ClickIsOnFavicon(void* tab_view, const uint8_t* event) {
  if (!g_geometry.tab_icon || !g_geometry.view_bounds || !g_geometry.event_location) {
    return false;
  }
  auto* view_bytes = static_cast<uint8_t*>(tab_view);
  void* icon = ReadPointer<void*>(view_bytes, g_geometry.tab_icon);
  if (!icon) {
    return false;
  }
  const uint8_t* icon_bytes = static_cast<const uint8_t*>(icon);
  // The event location is already in TabView coordinates, and a child's bounds_ are relative to
  // its parent, so no extra translation is needed here.
  const int32_t left = ReadInt(icon_bytes + g_geometry.view_bounds);
  const int32_t top = ReadInt(icon_bytes + g_geometry.view_bounds + 4);
  const int32_t width = ReadInt(icon_bytes + g_geometry.view_bounds + 8);
  const int32_t height = ReadInt(icon_bytes + g_geometry.view_bounds + 12);
  if (width <= 0 || height <= 0) {
    return false;
  }
  const float x = ReadFloat(event + g_geometry.event_location);
  const float y = ReadFloat(event + g_geometry.event_location + 4);
  // A few pixels of slack make the target feel like the whole icon rather than its inner box.
  constexpr int32_t kSlack = 4;
  return x >= left - kSlack && x <= left + width + kSlack && y >= top - kSlack &&
         y <= top + height + kSlack;
}

// bool TabView::OnMousePressed(const ui::MouseEvent&)
using OnMousePressedFn = bool (*)(void* self, const void* event);

// Returns the tab id whose favicon was clicked (0 when the click must go through). Kept apart
// from HookOnMousePressed because __try cannot coexist with objects that need unwinding.
int TwistyTargetTabId(void* self, const void* event) {
  __try {
    auto* event_bytes = static_cast<const uint8_t*>(event);
    const uint32_t flags = *reinterpret_cast<const uint32_t*>(event_bytes + g_geometry.event_flags);
    const bool plain_left_click =
        (flags & kLeftMouseButton) != 0 && (flags & kEventModifierMask) == 0;
    if (!plain_left_click) {
      return 0;
    }
    const int tab_id = GlobalApi().TabIdOf(self);
    if (!tab_id) {
      return 0;
    }
    g_candidate_state = GlobalState().Get(tab_id);
    if (!g_candidate_state.has_children || !ClickIsOnFavicon(self, event_bytes)) {
      return 0;
    }
    return tab_id;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

bool HookOnMousePressed(void* self, const void* event) {
  const int tab_id = TwistyTargetTabId(self, event);
  if (tab_id) {
    InterlockedIncrement(&g_twisty_clicks);
    ARB_DEBUG("twisty click on tab %d", tab_id);
    BroadcastJson("{\"type\":\"twistyClick\",\"tabId\":" + std::to_string(tab_id) + "}");
    // Swallow the click: the tab must not activate and no drag may start.
    return true;
  }
  return reinterpret_cast<OnMousePressedFn>(g_mouse.original)(self, event);
}

// ---------------------------------------------------------------------------------------
// F3: collapse marker and hidden-tab count
// ---------------------------------------------------------------------------------------
void DrawCollapseMarker(void* self, void* canvas) {
  ChromeApi& api = GlobalApi();
  const int tab_id = api.TabIdOf(self);
  if (!tab_id) {
    return;
  }
  const TabDisplayState state = GlobalState().Get(tab_id);
  if (!state.collapsed || !state.has_children) {
    return;
  }
  const DisplayConfig config = GlobalState().Config();
  auto* view_bytes = static_cast<uint8_t*>(self);
  const int32_t width = ReadInt(view_bytes + g_geometry.view_bounds + 8);
  const int32_t height = ReadInt(view_bytes + g_geometry.view_bounds + 12);
  if (width <= 0 || height <= 0 || !api.fill_rect) {
    return;
  }

  // A 3px accent bar on the leading edge marks the collapsed parent.
  int32_t marker[4] = {0, 4, 3, height - 8};
  using FillRectFn = void (*)(void*, const void*, uint32_t);
  reinterpret_cast<FillRectFn>(api.fill_rect)(canvas, marker, 0xFF3D7EFF);

  if (!config.show_count || state.hidden_count <= 0) {
    return;
  }

  // The count is drawn with the same FillRect primitive as the marker: a small seven-segment
  // renderer avoids depending on gfx::FontList/gfx::Canvas text APIs whose calling convention
  // varies between Chrome versions.
  using FillRectFn = void (*)(void*, const void*, uint32_t);
  auto fill = [&api](void* canvas, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    int32_t rect[4] = {x, y, w, h};
    reinterpret_cast<FillRectFn>(api.fill_rect)(canvas, rect, color);
  };

  constexpr uint32_t kInk = 0xFF6FA8FF;
  constexpr int32_t kDigitWidth = 8;
  constexpr int32_t kDigitHeight = 12;
  constexpr int32_t kThickness = 2;
  constexpr int32_t kPlusWidth = 7;
  constexpr int32_t kGap = 4;

  const int32_t top = (height - kDigitHeight) / 2;
  // Keep clear of the close button: it occupies the trailing corner of the tab.
  int32_t anchor = width - 8;
  if (g_geometry.tab_close_button) {
    if (void* close_view = ReadPointer<void*>(view_bytes, g_geometry.tab_close_button)) {
      const int32_t close_left =
          ReadInt(static_cast<uint8_t*>(close_view) + g_geometry.view_bounds);
      if (close_left > kDigitWidth + kPlusWidth + 16) {
        anchor = close_left - 6;
      }
    }
  }
  if (anchor < kDigitWidth + kPlusWidth + kGap + 12) {
    return;  // too narrow to draw a readable label
  }
  const int32_t digit_x = anchor - kDigitWidth;
  const int32_t plus_x = digit_x - kGap - kPlusWidth;
  const int32_t middle_y = top + kDigitHeight / 2;

  // "+" sign
  fill(canvas, plus_x, middle_y - kThickness / 2, kPlusWidth, kThickness, kInk);
  fill(canvas, plus_x + kPlusWidth / 2 - kThickness / 2, middle_y - kPlusWidth / 2, kThickness,
       kPlusWidth, kInk);

  const int32_t value = state.hidden_count > 9 ? 9 : state.hidden_count;
  static const bool kSegments[10][7] = {
      {true, true, true, true, true, true, false},      // 0
      {false, true, true, false, false, false, false},  // 1
      {true, true, false, true, true, false, true},     // 2
      {true, true, true, true, false, false, true},     // 3
      {false, true, true, false, false, true, true},    // 4
      {true, false, true, true, false, true, true},     // 5
      {true, false, true, true, true, true, true},      // 6
      {true, true, true, false, false, false, false},   // 7
      {true, true, true, true, true, true, true},       // 8
      {true, true, true, true, false, true, true},      // 9
  };
  const bool* segments = kSegments[value];
  const int32_t half = kDigitHeight / 2;
  const int32_t inner = kDigitWidth - kThickness;
  if (segments[0]) fill(canvas, digit_x, top, kDigitWidth, kThickness, kInk);
  if (segments[1]) fill(canvas, digit_x + inner, top, kThickness, half, kInk);
  if (segments[2]) fill(canvas, digit_x + inner, top + half, kThickness, half, kInk);
  if (segments[3]) fill(canvas, digit_x, top + kDigitHeight - kThickness, kDigitWidth, kThickness, kInk);
  if (segments[4]) fill(canvas, digit_x, top + half, kThickness, half, kInk);
  if (segments[5]) fill(canvas, digit_x, top, kThickness, half, kInk);
  if (segments[6]) fill(canvas, digit_x, middle_y - kThickness / 2, kDigitWidth, kThickness, kInk);

  if (state.hidden_count > 9) {
    // A trailing "+" marks "9 or more".
    const int32_t more_x = plus_x - kGap - kPlusWidth;
    fill(canvas, more_x, middle_y - kThickness / 2, kPlusWidth, kThickness, kInk);
    fill(canvas, more_x + kPlusWidth / 2 - kThickness / 2, middle_y - kPlusWidth / 2, kThickness,
         kPlusWidth, kInk);
  }

  const LONG drawn = InterlockedIncrement(&g_count_labels);
  if (drawn <= 3) {
    ARB_DEBUG("drew hidden-count label %d on tab %d (tab %dx%d)", state.hidden_count, tab_id,
              width, height);
  }
}

// void TabView::OnPaint(gfx::Canvas*)
using OnPaintFn = void (*)(void* self, void* canvas);

void HookOnPaint(void* self, void* canvas) {
  reinterpret_cast<OnPaintFn>(g_paint.original)(self, canvas);
  __try {
    DrawCollapseMarker(self, canvas);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Painting must never break the browser.
  }
}

bool PrologueMatches(uint8_t* base, const FunctionEntry& entry) {
  if (entry.prologue.empty()) {
    return true;
  }
  return memcmp(base + entry.rva, entry.prologue.data(), entry.prologue.size()) == 0;
}

bool Install(const char* name, HMODULE chrome_dll, const Offsets& offsets, void* detour,
             void** original, uint32_t* rva) {
  const auto found = offsets.hooks.find(name);
  if (found == offsets.hooks.end()) {
    ARB_WARN("%s is not in the offsets table", name);
    return false;
  }
  auto* base = reinterpret_cast<uint8_t*>(chrome_dll);
  if (!PrologueMatches(base, found->second)) {
    ARB_WARN("prologue mismatch for %s; feature disabled", name);
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
  *rva = found->second.rva;
  ARB_INFO("hooked %s at 0x%08x", name, *rva);
  return true;
}

bool LoadGeometry(const Offsets& offsets) {
  Geometry geometry;
  const auto field = [&offsets](const char* name, uint32_t& out) {
    const auto found = offsets.fields.find(name);
    if (found == offsets.fields.end()) {
      return false;
    }
    out = found->second;
    return true;
  };
  if (!field("ui::Event.flags_", geometry.event_flags) ||
      !field("ui::LocatedEvent.location_", geometry.event_location) ||
      !field("views::View.bounds_", geometry.view_bounds) ||
      !field("TabView.icon_", geometry.tab_icon)) {
    ARB_WARN("offsets table lacks the fields the mouse feature needs");
    return false;
  }
  // The close button is optional: without it the label simply moves to the right edge.
  const auto close_button = offsets.fields.find("TabView.close_button_");
  geometry.tab_close_button =
      close_button == offsets.fields.end() ? 0 : close_button->second;
  g_geometry = geometry;
  return true;
}

}  // namespace

bool InstallMouseFeature(HMODULE chrome_dll, const Offsets& offsets, MouseFeature& feature) {
  if (!LoadGeometry(offsets)) {
    return false;
  }
  // The hook bodies read the globals, so the trampoline pointer must live there too.
  const bool installed = Install("TabView::OnMousePressed", chrome_dll, offsets,
                                 reinterpret_cast<void*>(&HookOnMousePressed),
                                 &g_mouse.original, &g_mouse.rva);
  feature = g_mouse;
  feature.installed = installed;
  // The message-only window is created from the layout hook: it must live on the browser UI
  // thread, and this installer runs on the offsets thread.
  return installed;
}

bool InstallPaintFeature(HMODULE chrome_dll, const Offsets& offsets, PaintFeature& feature) {
  ChromeApi& api = GlobalApi();
  if (!g_geometry.view_bounds) {
    LoadGeometry(offsets);  // the paint hook only needs view bounds, but keep it consistent
  }
  const bool installed = Install("TabView::OnPaint", chrome_dll, offsets,
                                 reinterpret_cast<void*>(&HookOnPaint), &g_paint.original,
                                 &g_paint.rva);
  feature = g_paint;
  feature.installed = installed;
  if (installed && (!api.fill_rect || !api.draw_string_rect)) {
    ARB_WARN("canvas helpers are missing; the collapse marker will be a bare accent bar");
  }
  return installed;
}

long TwistyClickCount() { return g_twisty_clicks; }

}  // namespace arbortab
