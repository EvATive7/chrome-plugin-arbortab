// F4/F5/F6: drag a tab onto another tab to re-parent it, or onto an edge to reorder it among
// its siblings. The native strip reorder is suppressed while dragging.
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

// Called from TabView::OnMousePressed: the tab a drag would start from.
void NoteTabPressed(int tab_id);

// Called from the paint hook: highlights the current drop target (size in DIP).
void PaintDropTarget(void* tab_view, void* canvas, int32_t width, int32_t height);
// Called from the container paint hook: the "drop at the end" line below the last row.
void PaintDropEnd(const void* host_view, void* canvas);

// Called after each layout pass of a tab container: keeps the dragged tab translucent.
void RefreshDragGhost(const void* host_view);

// Diagnostics.
int CurrentDropTargetTabId();
int CurrentDropZone();  // 0 none, 1 nest, 2 before, 3 after, 4 end
bool DragInProgress();

}  // namespace arbortab
