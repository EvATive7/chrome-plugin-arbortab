#include "core/chrome_api.h"

#include <cstring>

#include "core/log.h"

namespace arbortab {
namespace {

ChromeApi g_api;

void* VtableOf(const void* object) {
  return *static_cast<void* const*>(object);
}

}  // namespace

ChromeApi& GlobalApi() { return g_api; }

bool ChromeApi::IsTabView(const void* view) const {
  if (!view) {
    return false;
  }
  const void* vtable = VtableOf(view);
  for (const void* candidate : tab_view_vtables) {
    if (candidate == vtable) {
      return true;
    }
  }
  return false;
}

int ChromeApi::TabIdOf(const void* view) const {
  if (!view || !get_tab_interface || !id_for_tab) {
    return 0;
  }
  using GetTabInterfaceFn = void* (*)(const void*);
  using IdForTabFn = void (*)(int32_t*, const void*);
  auto get_interface = reinterpret_cast<GetTabInterfaceFn>(get_tab_interface);
  auto id_for_tab_fn = reinterpret_cast<IdForTabFn>(id_for_tab);

  void* tab_interface = get_interface(view);
  if (!tab_interface) {
    return 0;
  }
  void* vtable = VtableOf(tab_interface);
  if (!vtable) {
    return 0;
  }
  auto* slots = static_cast<void**>(vtable);
  auto get_contents = reinterpret_cast<void* (*)(const void*)>(slots[get_contents_slot]);
  if (!get_contents) {
    return 0;
  }
  void* contents = get_contents(tab_interface);
  if (!contents) {
    return 0;
  }
  int32_t session_id = 0;
  id_for_tab_fn(&session_id, contents);
  return static_cast<int>(session_id);
}

bool ChromeApi::IsStripCollapsed(const void* view) const {
  if (!view || !off_tab_collapsed) {
    return false;
  }
  return *reinterpret_cast<const uint8_t*>(static_cast<const uint8_t*>(view) + off_tab_collapsed) != 0;
}

bool ChromeApi::IsVertical(const void* layout) const {
  if (!layout || !off_tab_orientation) {
    return false;
  }
  return *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(layout) +
                                           off_tab_orientation) != 0;
}

void ChromeApi::InvalidateLayout(void* view) const {
  if (!view || !invalidate_layout) {
    return;
  }
  using Fn = void (*)(void*, bool);
  reinterpret_cast<Fn>(invalidate_layout)(view, false);
}

void ChromeApi::SchedulePaint(void* view) const {
  if (!view || !schedule_paint) {
    return;
  }
  using Fn = void (*)(void*);
  reinterpret_cast<Fn>(schedule_paint)(view);
}

bool ResolveChromeApi(HMODULE chrome_dll, const Offsets& offsets, ChromeApi& api) {
  auto* base = reinterpret_cast<uint8_t*>(chrome_dll);
  const auto address = [base, &offsets](const char* name) -> void* {
    const auto found = offsets.functions.find(name);
    return found == offsets.functions.end() ? nullptr : base + found->second.rva;
  };
  const auto field = [&offsets](const char* name) -> uint32_t {
    const auto found = offsets.fields.find(name);
    return found == offsets.fields.end() ? 0 : found->second;
  };

  ChromeApi resolved;
  resolved.base = base;
  resolved.get_tab_interface = address("TabView::GetTabInterface");
  resolved.id_for_tab = address("sessions::SessionTabHelper::IdForTab");
  resolved.invalidate_layout = address("views::View::InvalidateLayout");
  resolved.schedule_paint = address("views::View::SchedulePaint");
  resolved.fill_rect = address("gfx::Canvas::FillRect");

  const auto slot = offsets.vtable_slots.find("tabs::TabModel::GetContents");
  resolved.get_contents_slot = slot == offsets.vtable_slots.end() ? -1 : slot->second;

  const auto vtables = offsets.vtables.find("TabView");
  if (vtables != offsets.vtables.end()) {
    for (uint32_t rva : vtables->second) {
      resolved.tab_view_vtables.push_back(base + rva);
    }
  }

  resolved.off_tab_orientation = field("UnpinnedTabContainerViewLayout.orientation_");
  resolved.off_tab_collapsed = field("TabView.collapsed_");
  resolved.off_tab_icon = field("TabView.icon_");

  const auto missing = [&resolved]() {
    std::string text;
    if (!resolved.get_tab_interface) text += "GetTabInterface ";
    if (!resolved.id_for_tab) text += "IdForTab ";
    if (resolved.get_contents_slot < 0) text += "GetContentsSlot ";
    if (resolved.tab_view_vtables.empty()) text += "TabViewVtables ";
    if (!resolved.off_tab_orientation) text += "orientation_ ";
    if (!resolved.off_tab_collapsed) text += "TabView.collapsed_ ";
    return text;
  };
  const std::string missing_items = missing();
  if (!missing_items.empty()) {
    ARB_WARN("offsets do not describe everything the layout feature needs: %s",
             missing_items.c_str());
    return false;
  }
  if (!resolved.invalidate_layout || !resolved.schedule_paint) {
    ARB_WARN("refreshing views is unavailable; layout changes would not repaint");
  }
  api = resolved;
  api.ready = true;
  ARB_INFO("chrome api resolved (%zu TabView vtables, GetContents slot %d)",
           api.tab_view_vtables.size(), api.get_contents_slot);
  return true;
}

}  // namespace arbortab
