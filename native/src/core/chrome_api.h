// Resolved Chrome internals (addresses come from the offsets table, never hardcoded).
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/offsets.h"

namespace arbortab {

// session id helper: SessionTabHelper::IdForTab(SessionID* out, const WebContents*)
struct ChromeApi {
  bool ready = false;
  uint8_t* base = nullptr;

  void* get_tab_interface = nullptr;  // TabInterface* (*)(const TabView*)
  void* id_for_tab = nullptr;         // void (*)(int32_t* out, const void* contents)
  void* invalidate_layout = nullptr;  // void (*)(void* view, bool invalidate_children)
  void* schedule_paint = nullptr;     // void (*)(void* view)
  void* fill_rect = nullptr;          // void (*)(void* canvas, const void* rect, uint32_t color)
  void* draw_string_rect = nullptr;   // Canvas::DrawStringRect(u16string_view, FontList, SkColor, Rect)
  void* default_font_list = nullptr;  // Label::GetDefaultFontList() -> const FontList&
  void* manager_layout = nullptr;     // void (*)(void* layout_manager, void* host_view)
  void* manager_invalidate = nullptr; // void (*)(void* layout_manager)
  int get_contents_slot = 3;
  std::vector<void*> tab_view_vtables;
  uint32_t off_tab_orientation = 0;   // UnpinnedTabContainerViewLayout::orientation_
  uint32_t off_tab_collapsed = 0;     // TabView::collapsed_ (vertical strip collapsed to icons)
  uint32_t off_tab_icon = 0;          // TabView::icon_

  // True when `view`'s vptr is one of the registered TabView vtables.
  bool IsTabView(const void* view) const;
  // Tab id for a TabView (0 when it cannot be resolved).
  int TabIdOf(const void* view) const;
  // Is this TabView shown in "collapsed strip" (icon-only) mode?
  bool IsStripCollapsed(const void* view) const;
  bool IsVertical(const void* layout) const;

  void InvalidateLayout(void* view) const;
  void SchedulePaint(void* view) const;
  // Runs the layout manager of `host_view` right away (Chrome only schedules layouts for the
  // next frame, which is too late for a WebSocket-driven state update).
  void LayoutNow(void* layout_manager, void* host_view) const;
  void InvalidateManager(void* layout_manager) const;
};

// Builds the API table from the offsets JSON. Returns false (and leaves Chrome untouched) when
// any required piece is missing.
bool ResolveChromeApi(HMODULE chrome_dll, const Offsets& offsets, ChromeApi& api);
ChromeApi& GlobalApi();

}  // namespace arbortab
