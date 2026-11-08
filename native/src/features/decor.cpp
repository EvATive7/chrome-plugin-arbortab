#include "features/decor.h"

#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

#include <MinHook.h>

#include "core/chrome_api.h"
#include "core/log.h"
#include "core/state.h"
#include "features/anim.h"
#include "features/drag.h"
#include "features/layout.h"
#include "features/paint.h"

namespace arbortab {
namespace {

struct DecorOffsets {
  uint32_t view_parent = 0;
  uint32_t view_bounds = 0;
  uint32_t view_visible = 0;
  uint32_t tab_title = 0;
  uint32_t tab_close_button = 0;
  uint32_t tab_hovered = 0;
  uint32_t tab_active = 0;
};

using ViewPaintFn = void (*)(void* self, void* canvas);
// TabView::Layout(base::NonCopyablePassKey<views::View>): the empty pass key travels in a
// register and is forwarded untouched.
using TabLayoutFn = void (*)(void* self, uintptr_t pass_key);
using SetBoundsRectFn = void (*)(void* view, const int32_t* rect);

DecorOffsets g_offsets;
void* g_view_paint_original = nullptr;
void* g_tab_layout_original = nullptr;
void* g_icon_paint_original = nullptr;
SetBoundsRectFn g_set_bounds_rect = nullptr;
int g_last_active_tab = 0;

// Favicon <-> chevron crossfade and the chevron's rotation (0 = pointing right / collapsed,
// 1 = pointing down / expanded). UI thread only.
struct IconMotion {
  Tween reveal;
  Tween turn;
  ULONGLONG last_seen = 0;
};
std::unordered_map<int, IconMotion> g_icons;

int32_t ReadInt(const void* base, uint32_t offset) {
  int32_t value = 0;
  memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(value));
  return value;
}

bool ReadBool(const void* base, uint32_t offset) {
  return offset && static_cast<const uint8_t*>(base)[offset] != 0;
}

void* ReadPointer(const void* base, uint32_t offset) {
  void* value = nullptr;
  memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(value));
  return value;
}

struct Bounds {
  int32_t x, y, width, height;
};

Bounds BoundsOf(const void* view) {
  return Bounds{ReadInt(view, g_offsets.view_bounds), ReadInt(view, g_offsets.view_bounds + 4),
                ReadInt(view, g_offsets.view_bounds + 8),
                ReadInt(view, g_offsets.view_bounds + 12)};
}

// ---------------------------------------------------------------------------------------
// Count badge
// ---------------------------------------------------------------------------------------
struct Badge {
  bool shown = false;
  int32_t x = 0, y = 0, width = 0, height = 0;
  int32_t text_width = 0, text_height = 0;
  float pill_y = 0.f;  // exact (half-pixel) top of the pill; `y` is its rounded twin for text
  std::u16string text;
};

// Vertical centre of the close button relative to the tab's own centre (x2, to keep halves).
int32_t g_close_center_delta2 = 0;

// Badge fade-in per tab (UI thread only).
struct BadgeMotion {
  Tween opacity;
  ULONGLONG last_seen = 0;
};
std::unordered_map<int, BadgeMotion> g_badges;
// Geometry is captured immediately after Chrome lays out the full title. Paint happens after
// we shorten the label, so recomputing from its bounds there would make the badge drift left.
std::unordered_map<int, Badge> g_badge_geometry;

std::u16string Utf8ToUtf16(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) {
    return {};
  }
  std::u16string converted(static_cast<size_t>(length), u'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), reinterpret_cast<wchar_t*>(converted.data()),
                      length);
  return converted;
}

Badge BadgeFor(const void* tab_view, const void* title_view, const TabDisplayState& state) {
  Badge badge;
  const DisplayConfig config = GlobalState().Config();
  const Bounds tab = BoundsOf(tab_view);
  if (!config.show_count || !state.collapsed || !state.has_children || state.hidden_count <= 0 ||
      tab.width < kMinTreeRowWidth || tab.height <= 0) {
    return badge;
  }
  const std::string digits =
      state.hidden_count > 99 ? std::string("99+") : std::to_string(state.hidden_count);
  badge.text.assign(digits.begin(), digits.end());
  badge.text_width = SmallTextWidth(badge.text);
  badge.text_height = SmallTextHeight();
  if (badge.text_width <= 0 || badge.text_height <= 0) {
    return badge;
  }
  badge.height = badge.text_height + 4 < 18 ? 18 : badge.text_height + 4;
  badge.width = badge.text_width + 14 > badge.height ? badge.text_width + 14 : badge.height;
  if (g_offsets.tab_close_button) {
    const void* close = ReadPointer(tab_view, g_offsets.tab_close_button);
    if (close && ReadBool(close, g_offsets.view_visible)) {
      const Bounds button = BoundsOf(close);
      if (button.width > 0) {
        g_close_center_delta2 = (2 * button.y + button.height) - tab.height;
      }
    }
  }
  if (!title_view) {
    return badge;
  }
  // Keep the count inside Chrome's title allocation. That allocation already stops before
  // native controls such as mute and close, unlike the old right-edge placement.
  const Bounds title = BoundsOf(title_view);
  const int32_t title_right = title.x + title.width;
  const std::u16string title_text = Utf8ToUtf16(state.title.data());
  const int32_t title_width = DefaultTextWidth(title_text);
  const int32_t after_text = title.x + (title_width > 0 ? title_width : 0) + 6;
  badge.x = after_text < title_right - badge.width ? after_text : title_right - badge.width;
  // Centred on the close button's row, so the pill and the x line up.
  badge.pill_y = static_cast<float>(tab.height + g_close_center_delta2 - badge.height) / 2.f;
  badge.y = (tab.height + g_close_center_delta2 - badge.height) / 2;
  badge.shown = title.width >= badge.width + 6 && badge.x >= title.x;
  return badge;
}

// 0..1: the badge fades in when a parent is collapsed.
float BadgeOpacity(void* tab_view, int tab_id) {
  const ULONGLONG now = GetTickCount64();
  if (g_badges.size() > 1024) {
    for (auto it = g_badges.begin(); it != g_badges.end();) {
      it = now - it->second.last_seen > 10000 ? g_badges.erase(it) : std::next(it);
    }
  }
  auto [it, inserted] = g_badges.try_emplace(tab_id);
  BadgeMotion& motion = it->second;
  if (inserted) {
    // The entry is dropped whenever the tab paints without a badge, so a new entry means the
    // badge just appeared (a collapse): fade it in.
    motion.opacity.Snap(0.f);
  }
  motion.last_seen = now;
  motion.opacity.Retarget(1.f, now, kFadeMs);
  if (motion.opacity.Running(now)) {
    RequestTabRepaint(tab_view);
  }
  return motion.opacity.At(now);
}

void PaintBadge(void* tab_view, void* canvas, const Badge& badge, float opacity) {
  const Ink ink = ThemeInk(tab_view);
  FillRoundRect(canvas, static_cast<float>(badge.x), badge.pill_y,
                static_cast<float>(badge.width), static_cast<float>(badge.height),
                static_cast<float>(badge.height) / 2.f,
                Fade(WithAlpha(ink.on_surface, 0x1A), opacity));
  DrawSmallTextCentered(canvas, badge.text, Fade(WithAlpha(ink.on_surface, 0xD9), opacity),
                        badge.x, badge.y, badge.width, badge.height);
}

// The title label would run underneath the badge: after the native layout, end it before.
void ShortenTitleForBadge(void* tab_view) {
  if (!g_set_bounds_rect || !g_offsets.tab_title) {
    return;
  }
  const int tab_id = GlobalApi().TabIdOf(tab_view);
  if (!tab_id) {
    return;
  }
  void* title = ReadPointer(tab_view, g_offsets.tab_title);
  const Badge badge = BadgeFor(tab_view, title, GlobalState().Get(tab_id));
  if (!badge.shown || !title) {
    g_badge_geometry.erase(tab_id);
    return;
  }
  g_badge_geometry[tab_id] = badge;
  const Bounds bounds = BoundsOf(title);
  const int32_t limit = badge.x - 6;
  if (bounds.x + bounds.width <= limit) {
    return;
  }
  const int32_t rect[4] = {bounds.x, bounds.y, limit > bounds.x ? limit - bounds.x : 0,
                           bounds.height};
  g_set_bounds_rect(title, rect);
}

void HookTabLayout(void* self, uintptr_t pass_key) {
  reinterpret_cast<TabLayoutFn>(g_tab_layout_original)(self, pass_key);
  __try {
    ShortenTitleForBadge(self);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// ---------------------------------------------------------------------------------------
// Chevron
// ---------------------------------------------------------------------------------------
// Updates the crossfade targets of a tab's favicon; returns the motion, or nullptr when the
// tab is not a parent (its favicon is never touched).
IconMotion* IconMotionFor(void* tab_view, int tab_id, const TabDisplayState& state,
                          bool& target_changed) {
  target_changed = false;
  if (!tab_id || !state.has_children) {
    g_icons.erase(tab_id);
    return nullptr;
  }
  const ULONGLONG now = GetTickCount64();
  const bool wide = BoundsOf(tab_view).width >= kMinTreeRowWidth;
  const bool want_chevron = wide && ReadBool(tab_view, g_offsets.tab_hovered) && !DragInProgress();
  const float reveal = want_chevron ? 1.f : 0.f;
  const float turn = state.collapsed ? 0.f : 1.f;
  if (g_icons.size() > 1024) {
    for (auto it = g_icons.begin(); it != g_icons.end();) {
      it = now - it->second.last_seen > 10000 ? g_icons.erase(it) : std::next(it);
    }
  }
  auto [it, inserted] = g_icons.try_emplace(tab_id);
  IconMotion& motion = it->second;
  if (inserted) {
    motion.reveal.Snap(reveal);
    motion.turn.Snap(turn);
  }
  target_changed = std::fabs(motion.reveal.to - reveal) > 0.001f ||
                   std::fabs(motion.turn.to - turn) > 0.001f;
  motion.reveal.Retarget(reveal, now, kFadeMs);
  motion.turn.Retarget(turn, now, kFadeMs);
  motion.last_seen = now;
  return &motion;
}

void PaintChevron(void* canvas, const Ink& ink, float center_x, float center_y, float turn,
                  float opacity) {
  // A right-pointing chevron rotated clockwise by turn * 90 degrees.
  constexpr float kPi = 3.14159265f;
  const float angle = turn * kPi / 2.f;
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  const float points[3][2] = {{-2.f, -4.f}, {2.f, 0.f}, {-2.f, 4.f}};
  float x[3], y[3];
  for (int i = 0; i < 3; ++i) {
    x[i] = center_x + points[i][0] * c - points[i][1] * s;
    y[i] = center_y + points[i][0] * s + points[i][1] * c;
  }
  const uint32_t color = Fade(ink.on_surface, opacity);
  StrokeLine(canvas, x[0], y[0], x[1], y[1], 1.75f, color);
  StrokeLine(canvas, x[1], y[1], x[2], y[2], 1.75f, color);
}

void PaintIcon(void* icon, void* canvas) {
  const auto original = reinterpret_cast<ViewPaintFn>(g_icon_paint_original);
  void* tab_view = ReadPointer(icon, g_offsets.view_parent);
  ChromeApi& api = GlobalApi();
  if (!PaintIsRich() || !api.IsTabView(tab_view)) {
    original(icon, canvas);
    return;
  }
  const int tab_id = api.TabIdOf(tab_view);
  const TabDisplayState state = tab_id ? GlobalState().Get(tab_id) : TabDisplayState{};
  bool target_changed = false;
  IconMotion* motion = IconMotionFor(tab_view, tab_id, state, target_changed);
  if (!motion) {
    original(icon, canvas);
    return;
  }
  const ULONGLONG now = GetTickCount64();
  const float reveal = motion->reveal.At(now);
  if (motion->reveal.Running(now) || motion->turn.Running(now)) {
    RequestTabRepaint(tab_view);
  }
  if (reveal <= 0.001f) {
    original(icon, canvas);
    return;
  }
  if (reveal < 0.999f) {
    if (LayersAvailable()) {
      BeginLayerAlpha(canvas, static_cast<uint8_t>((1.f - reveal) * 255.f));
      original(icon, canvas);
      EndLayer(canvas);
    } else {
      original(icon, canvas);
    }
  }
  const Bounds bounds = BoundsOf(icon);
  PaintChevron(canvas, ThemeInk(tab_view), static_cast<float>(bounds.width) / 2.f,
               static_cast<float>(bounds.height) / 2.f, motion->turn.At(now), reveal);
}

// SEH and C++ unwinding cannot share a frame, hence the separate guarded wrapper; a fault
// before the original ran paints the favicon plainly.
bool PaintIconGuarded(void* icon, void* canvas) {
  __try {
    PaintIcon(icon, canvas);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void HookIconPaint(void* self, void* canvas) {
  if (!PaintIconGuarded(self, canvas)) {
    reinterpret_cast<ViewPaintFn>(g_icon_paint_original)(self, canvas);
  }
}

// ---------------------------------------------------------------------------------------
// Indent guides (container)
// ---------------------------------------------------------------------------------------
int LevelOf(const StripRow& row, int indent) {
  return indent > 0 ? (row.shift + indent / 2) / indent : 0;
}

void PaintGuides(void* host_view, void* canvas, const StripSnapshot& strip) {
  const DisplayConfig config = GlobalState().Config();
  const int indent = config.indent > 0 ? config.indent : 14;
  if (!config.show_guides || strip.count == 0) {
    return;
  }
  int max_level = 0;
  int active = -1;
  for (size_t index = 0; index < strip.count; ++index) {
    const int level = LevelOf(strip.rows[index], indent);
    max_level = level > max_level ? level : max_level;
    if (active < 0 && g_offsets.tab_active && GlobalApi().IsTabView(strip.rows[index].view) &&
        ReadBool(strip.rows[index].view, g_offsets.tab_active)) {
      active = static_cast<int>(index);
    }
  }
  if (max_level == 0) {
    return;
  }
  const Ink ink = ThemeInk(host_view);
  const uint32_t faint = WithAlpha(ink.on_surface, 0x24);
  const uint32_t strong = WithAlpha(ink.on_surface, 0x5C);
  // The guide of the active tab's own parent is drawn stronger.
  const int active_level = active >= 0 ? LevelOf(strip.rows[active], indent) : 0;
  for (int column = 0; column < max_level; ++column) {
    size_t index = 0;
    while (index < strip.count) {
      if (LevelOf(strip.rows[index], indent) <= column) {
        ++index;
        continue;
      }
      const size_t first = index;
      while (index < strip.count && LevelOf(strip.rows[index], indent) > column) {
        ++index;
      }
      const StripRow& top = strip.rows[first];
      const StripRow& bottom = strip.rows[index - 1];
      const bool highlighted = active >= static_cast<int>(first) &&
                               active < static_cast<int>(index) && column == active_level - 1;
      const int32_t x = top.x - top.shift + column * indent + indent / 2;
      const int32_t y1 = top.y + 3;
      const int32_t y2 = bottom.y + bottom.height - 3;
      FillRect(canvas, x, y1, 1, y2 - y1, highlighted ? strong : faint);
    }
  }
}

void PaintContainer(void* self, void* canvas) {
  const StripSnapshot* strip = FindStripSnapshot(self);
  if (!strip) {
    return;
  }
  PaintGuides(self, canvas, *strip);
  PaintDropEnd(self, canvas);
}

// void views::View::OnPaint(gfx::Canvas*): runs for every view that does not override it, so
// the filter (pointer compare against at most a handful of containers) has to stay trivial.
void HookViewPaint(void* self, void* canvas) {
  reinterpret_cast<ViewPaintFn>(g_view_paint_original)(self, canvas);
  if (!FindStripSnapshot(self)) {
    return;
  }
  __try {
    PaintContainer(self, canvas);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

bool InstallHook(HMODULE chrome_dll, const Offsets& offsets, const char* name, void* detour,
                 void** original) {
  const auto found = offsets.hooks.find(name);
  if (found == offsets.hooks.end()) {
    ARB_WARN("%s is not in the offsets table", name);
    return false;
  }
  auto* base = reinterpret_cast<uint8_t*>(chrome_dll);
  const FunctionEntry& entry = found->second;
  if (!entry.prologue.empty() &&
      memcmp(base + entry.rva, entry.prologue.data(), entry.prologue.size()) != 0) {
    ARB_WARN("prologue mismatch for %s", name);
    return false;
  }
  if (MH_CreateHook(base + entry.rva, detour, original) != MH_OK) {
    ARB_WARN("MH_CreateHook(%s) failed", name);
    return false;
  }
  if (MH_EnableHook(base + entry.rva) != MH_OK) {
    MH_RemoveHook(base + entry.rva);
    ARB_WARN("MH_EnableHook(%s) failed", name);
    return false;
  }
  ARB_INFO("hooked %s at 0x%08x", name, entry.rva);
  return true;
}

}  // namespace

void PaintTabDecor(void* tab_view, void* canvas) {
  ChromeApi& api = GlobalApi();
  const int tab_id = api.TabIdOf(tab_view);
  if (!tab_id) {
    return;
  }
  // The indent guide of the active branch lives in the container: repaint it when the
  // active tab changes.
  if (ReadBool(tab_view, g_offsets.tab_active) && tab_id != g_last_active_tab) {
    g_last_active_tab = tab_id;
    RequestAnimationFrame();
  }
  const TabDisplayState state = GlobalState().Get(tab_id);
  // Hover changes repaint the tab but not necessarily its favicon child.
  bool target_changed = false;
  if (IconMotionFor(tab_view, tab_id, state, target_changed) && target_changed) {
    RequestTabRepaint(tab_view);
  }
  const auto geometry = g_badge_geometry.find(tab_id);
  if (geometry != g_badge_geometry.end() && state.collapsed && state.has_children &&
      state.hidden_count > 0 && GlobalState().Config().show_count) {
    PaintBadge(tab_view, canvas, geometry->second, BadgeOpacity(tab_view, tab_id));
  } else {
    g_badges.erase(tab_id);
    g_badge_geometry.erase(tab_id);
  }
}

bool InstallDecorFeature(HMODULE chrome_dll, const Offsets& offsets, DecorFeature& feature) {
  const auto field = [&offsets](const char* name, uint32_t& out) {
    const auto found = offsets.fields.find(name);
    if (found == offsets.fields.end()) {
      return false;
    }
    out = found->second;
    return true;
  };
  DecorOffsets parsed;
  if (!field("views::View.bounds_", parsed.view_bounds) ||
      !field("views::View.parent_", parsed.view_parent)) {
    ARB_WARN("offsets table lacks views::View bounds_/parent_; no tree decorations");
    return false;
  }
  field("views::View.visible_", parsed.view_visible);
  field("TabView.title_", parsed.tab_title);
  field("TabView.close_button_", parsed.tab_close_button);
  field("TabView.hovered_", parsed.tab_hovered);
  field("TabView.active_", parsed.tab_active);
  g_offsets = parsed;
  const auto set_bounds = offsets.functions.find("views::View::SetBoundsRect");
  if (set_bounds != offsets.functions.end()) {
    g_set_bounds_rect = reinterpret_cast<SetBoundsRectFn>(
        reinterpret_cast<uint8_t*>(chrome_dll) + set_bounds->second.rva);
  }

  feature.guides = InstallHook(chrome_dll, offsets, "views::View::OnPaint",
                               reinterpret_cast<void*>(&HookViewPaint), &g_view_paint_original);
  feature.badge = InstallHook(chrome_dll, offsets, "TabView::Layout",
                              reinterpret_cast<void*>(&HookTabLayout), &g_tab_layout_original);
  feature.chevron = parsed.tab_hovered &&
                    InstallHook(chrome_dll, offsets, "TabIcon::OnPaint",
                                reinterpret_cast<void*>(&HookIconPaint), &g_icon_paint_original);
  return feature.guides || feature.badge || feature.chevron;
}

}  // namespace arbortab
