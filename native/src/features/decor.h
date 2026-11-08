// Tree decorations of the vertical strip:
//   - indent guides in the gutter, painted by the tab container (views::View::OnPaint);
//   - the hidden-descendant count badge of a collapsed parent (TabView::OnPaint), with the
//     title shortened to make room for it (TabView::Layout);
//   - the expand / collapse chevron that replaces a parent's favicon while it is hovered
//     (TabIcon::OnPaint).
#pragma once

#include <windows.h>

#include "core/offsets.h"

namespace arbortab {

struct DecorFeature {
  bool guides = false;
  bool badge = false;
  bool chevron = false;
};

bool InstallDecorFeature(HMODULE chrome_dll, const Offsets& offsets, DecorFeature& feature);

// Called from the TabView::OnPaint hook after the native paint.
void PaintTabDecor(void* tab_view, void* canvas);

}  // namespace arbortab
