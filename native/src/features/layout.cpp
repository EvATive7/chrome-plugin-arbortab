#include "features/layout.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include <MinHook.h>
#include "json.hpp"

#include "core/chrome_api.h"
#include "core/log.h"
#include "core/state.h"
#include "core/ui_thread.h"

namespace arbortab {
namespace {

// views::ProposedLayout (size 0x190) / views::ChildLayout (size 0x30). All offsets come from
// the offsets table at runtime; the layout of these two types is validated by the resolver.
struct ProposedLayoutOffsets {
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
std::atomic<bool> g_snapshot_requested{false};
volatile LONG g_rewrites = 0;
volatile LONG g_id_failures = 0;

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
void RewriteLayout(void* layout_ptr, const void* host_view) {
  auto* layout = static_cast<uint8_t*>(layout_ptr);
  ChromeApi& api = GlobalApi();
  if (!api.ready || !api.IsVertical(host_view)) {
    return;
  }
  const ChildRange children = ChildrenOf(layout, g_layout_offsets.child_layouts);
  if (!children.begin || children.count == 0) {
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

  const bool want_snapshot = g_snapshot_requested.exchange(false);
  nlohmann::json snapshot = nlohmann::json::array();

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
    const bool strip_collapsed = is_tab && api.IsStripCollapsed(child_view);
    const int depth = strip_collapsed ? 0 : state.depth;

    const int32_t height = ReadInt(child + g_layout_offsets.child_bounds + 12);
    const int32_t width = ReadInt(child + g_layout_offsets.child_bounds + 8);
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

    if (is_tab && state.hidden) {
      WriteInt(child + g_layout_offsets.child_bounds + 4, y);
      child[g_layout_offsets.child_visible] = 0;
      // Native code already treats invisible children as zero-height; keep both consistent.
      WriteInt(child + g_layout_offsets.child_bounds + 12, 0);
      changed = true;
      continue;
    }

    if (!have_first_visible) {
      y = ReadInt(child + g_layout_offsets.child_bounds + 4);
      have_first_visible = true;
    }
    void* existing_view = child_view;
    (void)existing_view;

    WriteInt(child + g_layout_offsets.child_bounds + 4, y);
    if (depth > 0) {
      const int32_t x = ReadInt(child + g_layout_offsets.child_bounds) + depth * indent;
      int32_t new_width = width - depth * indent;
      if (new_width < min_width) {
        new_width = min_width;
      }
      WriteInt(child + g_layout_offsets.child_bounds, x);
      WriteInt(child + g_layout_offsets.child_bounds + 8, new_width);
      changed = true;
    }
    y += height;
    if (index + 1 < children.count) {
      y += gap_before(index + 1);
    }
  }

  if (want_snapshot) {
    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    g_snapshot_json = snapshot.dump();
  }

  if (changed) {
    InterlockedIncrement(&g_rewrites);
    int32_t* host_size = reinterpret_cast<int32_t*>(layout + g_layout_offsets.host_size);
    host_size[1] = y;
  }
}

void* HookCalculateLayout(const void* self, void* out_layout, const void* size_bounds) {
  void* result = reinterpret_cast<CalcLayoutFn>(g_feature.original)(self, out_layout, size_bounds);
  const LONG call = InterlockedIncrement(&g_calls);
  UiThreadEnsureWindow();
  UiThreadRecordHostView(const_cast<void*>(self));
  if (result) {
    RewriteLayout(result, self);
  }
  if (call <= 3) {
    ARB_DEBUG("layout hook call %ld (host=%p result=%p)", call, self, result);
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
  if (!field("views::ProposedLayout.host_size", parsed.host_size) ||
      !field("views::ProposedLayout.child_layouts", parsed.child_layouts) ||
      !field("views::ChildLayout.size", parsed.child_size) ||
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
  layout.installed = true;
  ARB_INFO("layout feature installed at 0x%08x", layout.rva);
  return true;
}

std::string DescribeLastLayoutJson() {
  g_snapshot_requested = true;
  std::lock_guard<std::mutex> lock(g_snapshot_mutex);
  return g_snapshot_json;
}

long LayoutCallCount() { return g_calls; }
long LayoutRewriteCount() { return g_rewrites; }
long LayoutTabIdFailures() { return g_id_failures; }

}  // namespace arbortab
