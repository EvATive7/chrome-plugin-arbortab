#include "core/hooks.h"

#include <cstring>
#include <string>

#include "core/chrome_api.h"
#include "core/log.h"
#include "core/offsets.h"
#include "features/layout.h"
#include "features/drag.h"
#include "features/mouse.h"
#include "features/decor.h"
#include "features/paint.h"

namespace arbortab {
namespace {

LayoutFeature g_layout;
MouseFeature g_mouse;
PaintFeature g_paint;
DragFeature g_drag;
DecorFeature g_decor;


}  // namespace

MH_STATUS HookRuntimeInitialize() {
  const MH_STATUS status = MH_Initialize();
  return status == MH_ERROR_ALREADY_INITIALIZED ? MH_OK : status;
}

FeatureState InstallFeatureHooks(HMODULE chrome_dll, const Offsets& offsets) {
  FeatureState features;
  if (HookRuntimeInitialize() != MH_OK) {
    ARB_ERROR("MinHook is unavailable; every feature stays off");
    return features;
  }
  // tools/dev/dev.py looks for this line to confirm which DLL is running.
  ARB_INFO("installing features");
  ResolveChromeApi(chrome_dll, offsets, GlobalApi());
  InitPaint(chrome_dll, offsets);

  features.layout = InstallLayoutFeature(chrome_dll, offsets, g_layout);
  features.twisty = InstallMouseFeature(chrome_dll, offsets, g_mouse);
  features.paint = InstallPaintFeature(chrome_dll, offsets, g_paint);
  features.drag = InstallDragFeature(chrome_dll, offsets, g_drag);
  // Cosmetic only: the tree works the same without the decorations.
  if (InstallDecorFeature(chrome_dll, offsets, g_decor)) {
    ARB_INFO("decorations: guides=%s badge=%s chevron=%s", g_decor.guides ? "on" : "off",
             g_decor.badge ? "on" : "off", g_decor.chevron ? "on" : "off");
  }
  return features;
}

std::string DescribeFeatures(const FeatureState& features) {
  std::string text;
  const std::pair<const char*, bool> items[] = {
      {"layout", features.layout},
      {"twisty", features.twisty},
      {"paint", features.paint},
      {"drag", features.drag},
  };
  for (const auto& [name, enabled] : items) {
    if (!text.empty()) {
      text += ' ';
    }
    text += name;
    text += enabled ? "=on" : "=off";
  }
  return text;
}

long LayoutHookCallCount() { return LayoutCallCount(); }

}  // namespace arbortab
