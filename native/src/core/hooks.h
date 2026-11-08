// MinHook runtime plus the feature hooks.
#pragma once

#include <windows.h>

#include <MinHook.h>

#include <string>

#include "core/offsets.h"

namespace arbortab {

// Idempotent MH_Initialize() wrapper shared by every hook user.
MH_STATUS HookRuntimeInitialize();

struct FeatureState {
  bool layout = false;
  bool twisty = false;
  bool paint = false;
  bool drag = false;
};

// Verifies prologues against the offsets table and detours every hook we support. Features
// whose function fails verification stay disabled; Chrome keeps working unchanged.
FeatureState InstallFeatureHooks(HMODULE chrome_dll, const Offsets& offsets);

// Pretty-prints a feature state for logs.
std::string DescribeFeatures(const FeatureState& features);

// F1: number of layout calls that went through the hook (diagnostics).
long LayoutHookCallCount();

}  // namespace arbortab
