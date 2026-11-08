#include "core/hooks.h"

#include <cstring>
#include <string>

#include "core/log.h"
#include "core/offsets.h"

namespace arbortab {
namespace {

// views::ProposedLayout UnpinnedTabContainerViewLayout::CalculateProposedLayout(
//     const views::SizeBounds&) const
// MSVC x64: rcx = this, rdx = hidden return buffer, r8 = &size_bounds, rax = return buffer.
using CalcLayoutFn = void* (*)(const void* self, void* out_layout, const void* size_bounds);
CalcLayoutFn g_calc_layout_original = nullptr;
volatile LONG g_layout_calls = 0;
uint32_t g_layout_rva = 0;
uint8_t* g_module_base = nullptr;

void* HookCalculateLayout(const void* self, void* out_layout, const void* size_bounds) {
  void* result = g_calc_layout_original(self, out_layout, size_bounds);
  const LONG count = InterlockedIncrement(&g_layout_calls);
  if (count <= 3) {
    ARB_DEBUG("layout hook%s rva=0x%08x layout=%p", count == 1 ? " (first call)" : "",
              g_layout_rva, result);
  }
  return result;
}

bool PrologueMatches(uint8_t* base, const FunctionEntry& entry) {
  if (entry.prologue.empty()) {
    return true;
  }
  uint8_t* target = base + entry.rva;
  return memcmp(target, entry.prologue.data(), entry.prologue.size()) == 0;
}

// Installs one detour; returns false (feature stays off) when the code does not match.
bool InstallHook(HMODULE module, const Offsets& offsets, const char* name, void* detour,
                 void** original) {
  if (!offsets.hooks.count(name)) {
    ARB_WARN("hook target missing from offsets: %s", name);
    return false;
  }
  const FunctionEntry& entry = offsets.hooks.at(name);
  auto* base = reinterpret_cast<uint8_t*>(module);
  if (!PrologueMatches(base, entry)) {
    ARB_WARN("prologue mismatch for %s (rva 0x%08x); feature disabled", name, entry.rva);
    return false;
  }
  void* target = base + entry.rva;
  const MH_STATUS created = MH_CreateHook(target, detour, original);
  if (created != MH_OK) {
    ARB_WARN("MH_CreateHook(%s) failed: %d", name, created);
    return false;
  }
  const MH_STATUS enabled = MH_EnableHook(target);
  if (enabled != MH_OK) {
    ARB_WARN("MH_EnableHook(%s) failed: %d", name, enabled);
    MH_RemoveHook(target);
    return false;
  }
  ARB_INFO("hooked %s at 0x%08x", name, entry.rva);
  return true;
}

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
  g_module_base = reinterpret_cast<uint8_t*>(chrome_dll);

  constexpr const char* kLayout = "UnpinnedTabContainerViewLayout::CalculateProposedLayout";
  if (offsets.hooks.count(kLayout)) {
    g_layout_rva = offsets.hooks.at(kLayout).rva;
    features.layout = InstallHook(chrome_dll, offsets, kLayout,
                                  reinterpret_cast<void*>(&HookCalculateLayout),
                                  reinterpret_cast<void**>(&g_calc_layout_original));
  }

  // M4 (mouse/paint) and M5 (drag) add their hooks here; the offsets table already carries
  // them so the verification path is exercised as soon as those features land.
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

long LayoutHookCallCount() { return g_layout_calls; }

}  // namespace arbortab
