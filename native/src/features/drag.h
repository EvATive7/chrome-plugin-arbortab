// F4/F5/F6: ArborTab owns left-button drag in its rewritten tree tab strips. Chrome still
// handles the press (selection/activation), but its native tab-drag initialization is suppressed.
#pragma once

#include <windows.h>

#include "core/offsets.h"

namespace arbortab {

struct DragFeature {
  void* initialize_original = nullptr;
  uint32_t initialize_rva = 0;
  void* dragged_original = nullptr;
  uint32_t dragged_rva = 0;
  void* released_original = nullptr;
  uint32_t released_rva = 0;
  void* capture_lost_original = nullptr;
  uint32_t capture_lost_rva = 0;
  bool installed = false;
};

bool InstallDragFeature(HMODULE chrome_dll, const Offsets& offsets, DragFeature& feature);

// Called around TabView::OnMousePressed. Preparation is deliberately separate so the original
// press can run while InitializeDrag is suppressed, preserving Chrome's click/selection behavior.
void PrepareOwnDragPress(void* tab_view, const void* event);
void FinishOwnDragPress(bool handled);

// Called from the paint hooks: highlights the current drop target (size in DIP).
void PaintDropTarget(void* tab_view, void* canvas, int32_t width, int32_t height);
// Called from the container paint hook: the "drop at the end" line below the last row.
void PaintDropEnd(const void* host_view, void* canvas);

// Diagnostics.
int CurrentDropTargetTabId();
int CurrentDropZone();  // 0 none, 1 nest, 2 before, 3 after, 4 end
bool DragInProgress();

}  // namespace arbortab
