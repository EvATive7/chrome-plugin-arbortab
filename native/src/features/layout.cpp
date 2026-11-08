#include "features/layout.h"

#include <atomic>
#include <exception>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <MinHook.h>
#include "json.hpp"

#include "core/chrome_api.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/state.h"
#include "core/ui_thread.h"
#include "features/anim.h"
#include "features/drag.h"

namespace arbortab {
namespace {

// views::ProposedLayout (size 0x190) / views::ChildLayout (size 0x30). All offsets come from
// the offsets table at runtime; the layout of these two types is validated by the resolver.
struct ProposedLayoutOffsets {
  uint32_t layout_host_view = 0;
  uint32_t host_size = 0;
  uint32_t child_layouts = 0;
  uint32_t child_size = 0;
  uint32_t child_view = 0;
  uint32_t child_visible = 0;
  uint32_t child_bounds = 0;
};

struct ChildRange {
  uint8_t* begin = nullptr;
  size_t count = 0;
};

LayoutFeature g_feature;
ProposedLayoutOffsets g_layout_offsets;
constexpr int kDefaultGap = 2;
volatile LONG g_calls = 0;
std::mutex g_snapshot_mutex;
std::string g_snapshot_json = "{}";
// While this deadline is in the future every layout pass refreshes the geometry snapshot.
std::atomic<ULONGLONG> g_snapshot_deadline{0};
volatile LONG g_rewrites = 0;
volatile LONG g_id_failures = 0;
volatile LONG g_horizontal_skips = 0;
volatile LONG g_empty_skips = 0;
volatile LONG g_max_children = 0;
volatile LONG g_faults = 0;

// One snapshot per tab container (one per browser window); the oldest is recycled.
constexpr size_t kMaxSnapshots = 8;
StripSnapshot g_snapshots[kMaxSnapshots];

// Chrome lays the tab container out with a TabCollectionAnimatingLayoutManager, which animates
// the container's own height towards every new target. While our rows glide, the target moves
// each frame and Chrome's height animation restarts each time, trailing behind: the rows
// settle first and the container (and the + button below it) catches up in a second step.
// So while our motion runs, the manager's host-size animation is held off and the height
// follows the rows frame by frame; afterwards the original setting is restored.
struct HostSizeHoldOffsets {
  uint32_t view_layout_manager = 0;  // views::View::layout_manager_
  uint32_t animate_host_size = 0;    // TabCollectionAnimatingLayoutManager::animate_host_size_
  uint32_t starting_layout = 0;      // ...::starting_layout_ (views::ProposedLayout)
  uint32_t current_layout = 0;       // ...::current_layout_ (views::ProposedLayout)
  uint32_t view_bounds = 0;          // views::View::bounds_
  std::vector<void*> manager_vtables;
};
HostSizeHoldOffsets g_hold;

struct HeldManager {
  void* manager;
  void* host_view;
  uint8_t original;
};
std::vector<HeldManager> g_held;
// Chrome's own row animation (started by the same change) outlives ours; releasing the hold
// while it still runs makes the height jump back to its interpolated value. So the hold is
// kept a little longer and released from the frame timer, outside any layout pass.
constexpr ULONGLONG kHoldTailMs = 450;
ULONGLONG g_release_at = 0;

bool IsAnimatingManager(void* manager) {
  if (!manager) {
    return false;
  }
  void* vtable = *static_cast<void**>(manager);
  for (void* candidate : g_hold.manager_vtables) {
    if (candidate == vtable) {
      return true;
    }
  }
  return false;
}

void ReleaseOne(const HeldManager& held) {
  __try {
    // A manager destroyed meanwhile (window closed) no longer carries its vtable.
    if (!IsAnimatingManager(held.manager)) {
      return;
    }
    auto* manager = static_cast<uint8_t*>(held.manager);
    // Its cached layouts kept the height of before the hold; bring them to what is on screen
    // so the next change animates from there.
    if (g_hold.view_bounds && g_layout_offsets.host_size) {
      int32_t height = 0;
      memcpy(&height, static_cast<uint8_t*>(held.host_view) + g_hold.view_bounds + 12,
             sizeof(height));
      for (uint32_t layout : {g_hold.starting_layout, g_hold.current_layout}) {
        if (layout) {
          memcpy(manager + layout + g_layout_offsets.host_size + 4, &height, sizeof(height));
        }
      }
    }
    manager[g_hold.animate_host_size] = held.original;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void ReleaseHeldManagers() {
  if (GetTickCount64() < g_release_at) {
    RequestFrameCallback(&ReleaseHeldManagers);
    return;
  }
  std::vector<HeldManager> held;
  held.swap(g_held);
  for (const HeldManager& entry : held) {
    ReleaseOne(entry);
  }
}

void HoldHostSizeAnimation(void* host_view, bool animating) {
  if (!g_hold.view_layout_manager || !g_hold.animate_host_size) {
    return;
  }
  void* manager = nullptr;
  memcpy(&manager, static_cast<uint8_t*>(host_view) + g_hold.view_layout_manager,
         sizeof(manager));
  if (!IsAnimatingManager(manager)) {
    return;
  }
  auto held = g_held.begin();
  while (held != g_held.end() && held->manager != manager) {
    ++held;
  }
  if (!animating) {
    return;  // released later by ReleaseHeldManagers
  }
  uint8_t* flag = static_cast<uint8_t*>(manager) + g_hold.animate_host_size;
  if (held == g_held.end()) {
    g_held.push_back(HeldManager{manager, host_view, *flag});
  }
  *flag = 0;
  g_release_at = GetTickCount64() + kMotionMs + kHoldTailMs;
  RequestFrameCallback(&ReleaseHeldManagers);
}

// Per-tab motion: the depth and the reveal (1 = shown, 0 = folded away) glide towards the
// tree state instead of jumping. UI thread only.
//
// Both are kept in tree units, never in pixels: Chrome evaluates this layout several times
// per frame, some of them at the icon-only width, and a pixel target would flip between
// those passes and restart the tween forever.
struct RowMotion {
  Tween depth;
  Tween reveal;
  bool badge = false;  // collapsed parent showing a count (its title must make room)
  ULONGLONG last_seen = 0;
};
std::unordered_map<int, RowMotion> g_motion;

// Tabs the extension has not described yet (just created): they stay folded away until their
// place in the tree is known, then grow in at the right depth, instead of showing up at the
// root and sliding over. Value: when the tab was first seen.
std::unordered_map<int, ULONGLONG> g_unknown_since;
constexpr ULONGLONG kUnknownGraceMs = 600;

template <typename Map>
void Prune(Map& map, size_t limit, ULONGLONG now, ULONGLONG ttl,
           ULONGLONG (*stamp)(const typename Map::mapped_type&)) {
  if (map.size() <= limit) {
    return;
  }
  for (auto it = map.begin(); it != map.end();) {
    it = now - stamp(it->second) > ttl ? map.erase(it) : std::next(it);
  }
}

RowMotion& MotionFor(int tab_id, ULONGLONG now, float depth, bool hidden, bool grow_in,
                     bool& fresh) {
  Prune(g_motion, 2048, now, 10000, +[](const RowMotion& motion) { return motion.last_seen; });
  auto [it, inserted] = g_motion.try_emplace(tab_id);
  fresh = inserted;
  if (inserted) {
    // A tab seen for the first time takes its place without moving; a brand-new tab whose
    // place was only just learned grows in from nothing.
    it->second.depth.Snap(depth);
    it->second.reveal.Snap(hidden || grow_in ? 0.f : 1.f);
  }
  it->second.last_seen = now;
  return it->second;
}

StripSnapshot& SnapshotSlotFor(void* host_view) {
  StripSnapshot* oldest = &g_snapshots[0];
  for (StripSnapshot& snapshot : g_snapshots) {
    if (snapshot.host_view == host_view) {
      return snapshot;
    }
    if (snapshot.updated_at < oldest->updated_at) {
      oldest = &snapshot;
    }
  }
  oldest->host_view = host_view;
  oldest->count = 0;
  return *oldest;
}

// MSVC x64: rcx = this, rdx = hidden return buffer, r8 = &size_bounds.
using CalcLayoutFn = void* (*)(const void* self, void* out_layout, const void* size_bounds);

// absl::InlinedVector<ChildLayout, 8>: metadata at +0x08 holds (size << 1) | is_allocated,
// the payload is either inline at +0x10 or a {data, capacity} pair at +0x10.
ChildRange ChildrenOf(uint8_t* layout, uint32_t field_offset) {
  uint8_t* container = layout + field_offset;
  size_t metadata = 0;
  memcpy(&metadata, container, sizeof(size_t));
  const size_t count = metadata >> 1;
  const bool allocated = (metadata & 1) != 0;
  uint8_t* data = allocated ? *reinterpret_cast<uint8_t**>(container + 8) : container + 8;
  return ChildRange{data, count};
}

int32_t ReadInt(const uint8_t* address) {
  int32_t value = 0;
  memcpy(&value, address, sizeof(value));
  return value;
}

void WriteInt(uint8_t* address, int32_t value) {
  memcpy(address, &value, sizeof(value));
}

int32_t& BoundsField(uint8_t* child, int index) {
  return *reinterpret_cast<int32_t*>(child + g_layout_offsets.child_bounds + index * 4);
}

// Rewrites the child list of the proposed layout: hidden tabs get zero height and lose their
// visibility, visible tabs are shifted by depth * indent and stacked without gaps.
// Set from config.ini (layoutDisabled=1) to keep the hook installed but do nothing; used to
// bisect layout problems during development.
bool LayoutRewriteDisabled() {
  static const bool disabled = ReadConfigInt("layoutDisabled", 0) != 0;
  return disabled;
}

void RewriteLayout(void* layout_ptr, const void* layout_object) {
  if (LayoutRewriteDisabled()) {
    return;
  }
  auto* layout = static_cast<uint8_t*>(layout_ptr);
  ChromeApi& api = GlobalApi();
  if (!api.ready || !api.IsVertical(layout_object)) {
    InterlockedIncrement(&g_horizontal_skips);
    return;
  }
  // `layout_object` is the LayoutManagerBase, not the container view: the view that has to be
  // invalidated lives in LayoutManagerBase::host_view_.
  void* host_view = nullptr;
  if (g_layout_offsets.layout_host_view) {
    memcpy(&host_view, static_cast<uint8_t*>(const_cast<void*>(layout_object)) +
                           g_layout_offsets.layout_host_view,
           sizeof(host_view));
  }
  if (!host_view) {
    return;
  }
  const ChildRange children = ChildrenOf(layout, g_layout_offsets.child_layouts);
  if (static_cast<LONG>(children.count) > g_max_children) {
    g_max_children = static_cast<LONG>(children.count);
  }
  if (!children.begin || children.count == 0) {
    InterlockedIncrement(&g_empty_skips);
    return;
  }

  const DisplayConfig config = GlobalState().Config();
  const int indent = config.indent > 0 ? config.indent : 16;
  const int min_width = config.min_tab_width > 0 ? config.min_tab_width : 48;

  // Original gaps keep the native rhythm; fall back to the known 2px padding.
  const auto gap_before = [&children](size_t index) -> int {
    if (index == 0) {
      return 0;
    }
    uint8_t* previous = children.begin + (index - 1) * g_layout_offsets.child_size;
    uint8_t* current = children.begin + index * g_layout_offsets.child_size;
    const int32_t previous_bottom = ReadInt(previous + g_layout_offsets.child_bounds + 4) +
                                    ReadInt(previous + g_layout_offsets.child_bounds + 12);
    const int32_t gap = ReadInt(current + g_layout_offsets.child_bounds + 4) - previous_bottom;
    return gap >= 0 && gap <= 16 ? gap : kDefaultGap;
  };

  const bool want_snapshot = GetTickCount64() < g_snapshot_deadline.load();
  nlohmann::json snapshot = nlohmann::json::array();

  StripSnapshot& strip = SnapshotSlotFor(host_view);
  strip.count = 0;

  const ULONGLONG now = GetTickCount64();
  bool animating = false;
  int32_t y = 0;
  bool have_first_visible = false;
  bool changed = false;
  for (size_t index = 0; index < children.count; ++index) {
    uint8_t* child = children.begin + index * g_layout_offsets.child_size;
    void* child_view = nullptr;
    memcpy(&child_view, child + g_layout_offsets.child_view, sizeof(child_view));

    const bool is_tab = api.IsTabView(child_view);
    const int tab_id = is_tab ? api.TabIdOf(child_view) : 0;
    if (is_tab && tab_id == 0) {
      InterlockedIncrement(&g_id_failures);
    }
    const TabDisplayState state = tab_id ? GlobalState().Get(tab_id) : TabDisplayState{};
    const int32_t height = ReadInt(child + g_layout_offsets.child_bounds + 12);
    const int32_t width = ReadInt(child + g_layout_offsets.child_bounds + 8);

    // Icon-only rows (collapsed strip) are never indented. The decision follows the row width,
    // not TabView::collapsed_: that flag stays set while the collapsed strip is temporarily
    // expanded on hover, and the hover flyout must show the tree like the expanded strip.
    const bool icon_only = is_tab && width < kMinTreeRowWidth;
    const int depth = icon_only ? 0 : state.depth;
    const int32_t original_x = ReadInt(child + g_layout_offsets.child_bounds);
    const int32_t original_y = ReadInt(child + g_layout_offsets.child_bounds + 4);

    if (want_snapshot) {
      snapshot.push_back({{"tabId", tab_id},
                          {"isTab", is_tab},
                          {"depth", depth},
                          {"hidden", state.hidden},
                          {"hasChildren", state.has_children},
                          {"collapsed", state.collapsed},
                          {"x", original_x},
                          {"y", original_y},
                          {"w", width},
                          {"h", height}});
    }

    const bool target_hidden = is_tab && state.hidden;
    float shown_depth = static_cast<float>(state.depth);
    float reveal = target_hidden ? 0.f : 1.f;
    if (is_tab && tab_id) {
      TabDisplayState described;
      const bool known = GlobalState().Find(tab_id, described);
      bool waiting = false;
      bool grow_in = false;
      if (!known && GlobalState().ClientCount() > 0) {
        if (g_unknown_since.size() > 4096) {
          g_unknown_since.clear();  // tabs of profiles the extension never describes
        }
        const auto [entry, inserted] = g_unknown_since.try_emplace(tab_id, now);
        waiting = now - entry->second < kUnknownGraceMs;
        animating |= waiting;  // keep laying out until the grace period runs out
      } else if (known) {
        grow_in = g_unknown_since.erase(tab_id) > 0;
      }
      if (waiting) {
        reveal = 0.f;
      } else {
        bool fresh = false;
        RowMotion& motion = MotionFor(tab_id, now, static_cast<float>(state.depth), target_hidden,
                                      grow_in, fresh);
        motion.depth.Retarget(static_cast<float>(state.depth), now, kMotionMs);
        motion.reveal.Retarget(target_hidden ? 0.f : 1.f, now, kMotionMs);
        shown_depth = motion.depth.At(now);
        reveal = motion.reveal.At(now);
        animating |= motion.depth.Running(now) || motion.reveal.Running(now);
        const bool badge = config.show_count && state.collapsed && state.has_children &&
                           state.hidden_count > 0;
        if (badge != motion.badge) {
          motion.badge = badge;
          if (!fresh || badge) {
            RequestTabRelayout(child_view);  // the title width depends on the count badge
          }
        }
      }
    }
    const float offset = icon_only ? 0.f : shown_depth * static_cast<float>(indent);

    if (is_tab && reveal <= 0.001f) {
      WriteInt(child + g_layout_offsets.child_bounds + 4, y);
      child[g_layout_offsets.child_visible] = 0;
      // Native code already treats invisible children as zero-height; keep both consistent.
      WriteInt(child + g_layout_offsets.child_bounds + 12, 0);
      changed = true;
      continue;
    }

    if (!have_first_visible) {
      y = original_y;
      have_first_visible = true;
    }

    // While folding, the row shrinks around its centre and Views clips its contents.
    const int32_t shown_height =
        reveal >= 0.999f ? height : static_cast<int32_t>(static_cast<float>(height) * reveal + 0.5f);
    WriteInt(child + g_layout_offsets.child_bounds + 4, y);
    if (shown_height != height) {
      WriteInt(child + g_layout_offsets.child_bounds + 12, shown_height);
      changed = true;
    }
    const int32_t shift = static_cast<int32_t>(offset + 0.5f);
    if (shift > 0) {
      int32_t new_width = width - shift;
      if (new_width < min_width) {
        new_width = min_width;
      }
      WriteInt(child + g_layout_offsets.child_bounds, original_x + shift);
      WriteInt(child + g_layout_offsets.child_bounds + 8, new_width);
      changed = true;
    }
    if (is_tab && tab_id && shown_height > 0 && strip.count < kMaxStripRows) {
      strip.rows[strip.count++] =
          StripRow{child_view,
                   tab_id,
                   depth,
                   shift,
                   ReadInt(child + g_layout_offsets.child_bounds),
                   y,
                   ReadInt(child + g_layout_offsets.child_bounds + 8),
                   shown_height};
    }
    y += shown_height;
    if (index + 1 < children.count) {
      y += static_cast<int32_t>(static_cast<float>(gap_before(index + 1)) * reveal + 0.5f);
    }
  }
  if (animating) {
    RequestAnimationFrame();
  }
  HoldHostSizeAnimation(host_view, animating);
  RefreshDragGhost(host_view);

  if (want_snapshot) {
    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    g_snapshot_json = snapshot.dump();
  }

  strip.content_bottom = y;
  strip.updated_at = GetTickCount64();

  if (changed) {
    InterlockedIncrement(&g_rewrites);
  }
  // Always own the height: hidden and folding rows no longer take their native space.
  int32_t* host_size = reinterpret_cast<int32_t*>(layout + g_layout_offsets.host_size);
  host_size[1] = y + kBottomPadding;
}

// SEH-guarded entry point: a stray pointer in a rewritten layout must never kill the browser.
void RewriteLayoutGuarded(void* layout_ptr, const void* layout_object) {
  __try {
    RewriteLayout(layout_ptr, layout_object);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    InterlockedIncrement(&g_faults);
  }
}

void* HookCalculateLayout(const void* self, void* out_layout, const void* size_bounds) {
  void* result = reinterpret_cast<CalcLayoutFn>(g_feature.original)(self, out_layout, size_bounds);
  // A C++ exception escaping into Chrome's layout code terminates the browser, so the whole
  // post-processing step is guarded.
  try {
  const LONG call = InterlockedIncrement(&g_calls);
  UiThreadEnsureWindow();
  if (g_layout_offsets.layout_host_view) {
    void* host_view = nullptr;
    memcpy(&host_view, static_cast<uint8_t*>(const_cast<void*>(self)) +
                           g_layout_offsets.layout_host_view,
           sizeof(host_view));
    UiThreadRecordHostView(host_view, const_cast<void*>(self));
  }
  if (result) {
    RewriteLayoutGuarded(result, self);
  }
  if (call <= 3 || call % 200 == 0) {
    ARB_DEBUG("layout hook call %ld (layout=%p result=%p)", call, self, result);
  }
  } catch (const std::exception& exception) {
    InterlockedIncrement(&g_faults);
    ARB_ERROR("layout post-processing failed: %s", exception.what());
  } catch (...) {
    InterlockedIncrement(&g_faults);
    ARB_ERROR("layout post-processing failed with an unknown exception");
  }
  return result;
}

bool PrologueMatches(uint8_t* base, const FunctionEntry& entry) {
  if (entry.prologue.empty()) {
    return true;
  }
  return memcmp(base + entry.rva, entry.prologue.data(), entry.prologue.size()) == 0;
}

}  // namespace

bool InstallLayoutFeature(HMODULE chrome_dll, const Offsets& offsets, LayoutFeature& layout) {
  constexpr const char* kName = "UnpinnedTabContainerViewLayout::CalculateProposedLayout";
  const auto found = offsets.hooks.find(kName);
  if (found == offsets.hooks.end()) {
    ARB_WARN("layout hook target is not in the offsets table");
    return false;
  }
  const auto field = [&offsets](const char* name, uint32_t& out) {
    const auto entry = offsets.fields.find(name);
    if (entry == offsets.fields.end()) {
      return false;
    }
    out = entry->second;
    return true;
  };
  ProposedLayoutOffsets parsed;
  const auto size_of = [&offsets](const char* name) -> uint32_t {
    const auto entry = offsets.sizes.find(name);
    return entry == offsets.sizes.end() ? 0 : entry->second;
  };
  parsed.child_size = size_of("views::ChildLayout");
  if (parsed.child_size == 0) {
    ARB_WARN("offsets table lacks sizeof(views::ChildLayout)");
    return false;
  }
  if (!field("views::LayoutManagerBase.host_view_", parsed.layout_host_view) ||
      !field("views::ProposedLayout.host_size", parsed.host_size) ||
      !field("views::ProposedLayout.child_layouts", parsed.child_layouts) ||
      !field("views::ChildLayout.child_view", parsed.child_view) ||
      !field("views::ChildLayout.visible", parsed.child_visible) ||
      !field("views::ChildLayout.bounds", parsed.child_bounds)) {
    ARB_WARN("offsets table lacks the ProposedLayout/ChildLayout fields");
    return false;
  }
  auto* base = reinterpret_cast<uint8_t*>(chrome_dll);
  if (!PrologueMatches(base, found->second)) {
    ARB_WARN("prologue mismatch for %s; layout feature disabled", kName);
    return false;
  }
  layout.rva = found->second.rva;
  layout.original = nullptr;
  const MH_STATUS created = MH_CreateHook(base + layout.rva,
                                          reinterpret_cast<LPVOID>(&HookCalculateLayout),
                                          &layout.original);
  if (created != MH_OK) {
    ARB_WARN("MH_CreateHook(layout) failed: %d", created);
    return false;
  }
  const MH_STATUS enabled = MH_EnableHook(base + layout.rva);
  if (enabled != MH_OK) {
    ARB_WARN("MH_EnableHook(layout) failed: %d", enabled);
    MH_RemoveHook(base + layout.rva);
    return false;
  }
  g_feature = layout;
  g_layout_offsets = parsed;
  // Optional: without these the container height animates on its own (two-step motion).
  HostSizeHoldOffsets hold;
  field("views::View.layout_manager_", hold.view_layout_manager);
  field("TabCollectionAnimatingLayoutManager.animate_host_size_", hold.animate_host_size);
  field("TabCollectionAnimatingLayoutManager.starting_layout_", hold.starting_layout);
  field("TabCollectionAnimatingLayoutManager.current_layout_", hold.current_layout);
  field("views::View.bounds_", hold.view_bounds);
  if (const auto vtables = offsets.vtables.find("TabCollectionAnimatingLayoutManager");
      vtables != offsets.vtables.end()) {
    for (uint32_t rva : vtables->second) {
      hold.manager_vtables.push_back(base + rva);
    }
  }
  if (hold.manager_vtables.empty()) {
    hold.animate_host_size = 0;
  }
  g_hold = hold;
  ARB_INFO("container height follows row motion: %s", g_hold.animate_host_size ? "on" : "off");
  layout.installed = true;
  ARB_INFO("layout feature installed at 0x%08x", layout.rva);
  return true;
}

const StripSnapshot* FindStripSnapshot(const void* host_view) {
  if (!host_view) {
    return nullptr;
  }
  for (const StripSnapshot& snapshot : g_snapshots) {
    if (snapshot.host_view == host_view && snapshot.updated_at) {
      return &snapshot;
    }
  }
  return nullptr;
}

const StripSnapshot* LatestStripSnapshot() {
  const StripSnapshot* latest = nullptr;
  for (const StripSnapshot& snapshot : g_snapshots) {
    if (snapshot.updated_at && (!latest || snapshot.updated_at > latest->updated_at)) {
      latest = &snapshot;
    }
  }
  return latest;
}

std::string DescribeLastLayoutJson() {
  g_snapshot_deadline = GetTickCount64() + 3000;
  std::lock_guard<std::mutex> lock(g_snapshot_mutex);
  return g_snapshot_json;
}

long LayoutHorizontalSkips() { return g_horizontal_skips; }
long LayoutEmptySkips() { return g_empty_skips; }
long LayoutMaxChildren() { return g_max_children; }

long LayoutFaults() { return g_faults; }

long LayoutCallCount() { return g_calls; }
long LayoutRewriteCount() { return g_rewrites; }
long LayoutTabIdFailures() { return g_id_failures; }

}  // namespace arbortab
