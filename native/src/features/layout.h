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

// Serialises the geometry of the last rewritten layout pass (used by tests and the
// `debug.layout` protocol message).
std::string DescribeLastLayoutJson();

// Diagnostics.
long LayoutCallCount();
long LayoutRewriteCount();
long LayoutTabIdFailures();

}  // namespace arbortab
