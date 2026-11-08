// F1: indent and hide tabs inside the native vertical tab strip (PLAN.md section 5.4).
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

#include "core/offsets.h"

namespace arbortab {

struct LayoutFeature {
  void* original = nullptr;
  uint32_t rva = 0;
  bool installed = false;
};

// Installs the CalculateProposedLayout detour. `layout.meta` carries the fields used to
// rewrite the returned views::ProposedLayout.
bool InstallLayoutFeature(HMODULE chrome_dll, const Offsets& offsets, LayoutFeature& layout);

// Geometry of the last vertical layout pass of one tab container, in the container's own
// coordinates (i.e. after the indent/hide rewrite). Read and written on the UI thread only.
struct StripRow {
  void* view;
  int tab_id;
  int depth;       // tree depth (0 in the icon-only collapsed strip)
  int32_t shift;   // current (possibly animating) indent in pixels
  int32_t x, y, width, height;
};

constexpr size_t kMaxStripRows = 512;

struct StripSnapshot {
  void* host_view;
  ULONGLONG updated_at;
  int32_t content_bottom;  // bottom of the last visible row
  size_t count;
  StripRow rows[kMaxStripRows];
};

// Snapshot of `host_view`, or nullptr when that container was never laid out vertically.
const StripSnapshot* FindStripSnapshot(const void* host_view);
// Most recently updated snapshot (fallback when the caller cannot name its container).
const StripSnapshot* LatestStripSnapshot();

// Rows narrower than this are the icon-only tabs of the collapsed strip (about 40 DIP wide):
// they get no indent and no tree decorations. Full rows (expanded strip or its hover flyout)
// are well above it.
constexpr int32_t kMinTreeRowWidth = 100;

// Vertical space kept below the last tab (room for the "drop at the end" indicator).
constexpr int32_t kBottomPadding = 4;

// Serialises the geometry of the last rewritten layout pass (used by tests and the
// `debug.layout` protocol message).
std::string DescribeLastLayoutJson();

// Diagnostics.
long LayoutCallCount();
long LayoutRewriteCount();
long LayoutTabIdFailures();
long LayoutHorizontalSkips();
long LayoutEmptySkips();
long LayoutMaxChildren();
long LayoutFaults();

}  // namespace arbortab
