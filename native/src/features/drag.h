// F4/F5/F6: drag a tab onto the middle of another tab to make it a child (PLAN.md §5.4).
#pragma once

#include <windows.h>

#include "core/offsets.h"

namespace arbortab {

struct DragFeature {
  void* handle_original = nullptr;
  uint32_t handle_rva = 0;
  void* end_original = nullptr;
  uint32_t end_rva = 0;
  bool installed = false;
};

bool InstallDragFeature(HMODULE chrome_dll, const Offsets& offsets, DragFeature& feature);

// Called from the paint hook: highlights the current drop target.
void PaintDropTarget(void* tab_view, void* canvas);

// Diagnostics.
int CurrentDropTargetTabId();
int CurrentDropZone();  // 0 none, 1 nest, 2 before, 3 after
bool DragInProgress();

}  // namespace arbortab
