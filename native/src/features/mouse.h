// F2 (click the favicon to collapse) and F3 (collapse marker).
#pragma once

#include <windows.h>

#include "core/offsets.h"

namespace arbortab {

struct MouseFeature {
  void* original = nullptr;
  uint32_t rva = 0;
  bool installed = false;
};

struct PaintFeature {
  void* original = nullptr;
  uint32_t rva = 0;
  bool installed = false;
};

bool InstallMouseFeature(HMODULE chrome_dll, const Offsets& offsets, MouseFeature& feature);
bool InstallPaintFeature(HMODULE chrome_dll, const Offsets& offsets, PaintFeature& feature);

long TwistyClickCount();

}  // namespace arbortab
