#include "core/hooks.h"

#include <cstring>
#include <string>

#include "core/chrome_api.h"
#include "core/log.h"
#include "core/offsets.h"
#include "features/layout.h"

namespace arbortab {
namespace {

LayoutFeature g_layout;

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
  ResolveChromeApi(chrome_dll, offsets, GlobalApi());

  features.layout = InstallLayoutFeature(chrome_dll, offsets, g_layout);

  // M4 (mouse/paint) and M5 (drag) install their hooks here; the offsets table already lists
  // them so the verification path stays exercised.
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
