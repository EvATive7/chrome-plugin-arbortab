#include "core/cig.h"

#include <windows.h>

#include <MinHook.h>

#include "core/hooks.h"
#include "core/log.h"

namespace arbortab {
namespace {

using UpdateProcAttrFn = BOOL(WINAPI*)(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD_PTR, PVOID,
                                       SIZE_T, PVOID, PSIZE_T);

UpdateProcAttrFn g_original = nullptr;
volatile LONG g_strip_count = 0;

// PROCESS_CREATION_MITIGATION_POLICY_BLOCK_NON_MICROSOFT_BINARIES_* lives in bits 44..47 of
// policy value 1 (ALWAYS_ON | AUDIT | RESERVED).
constexpr DWORD64 kBlockNonMicrosoftMask = 0xFULL << 44;

BOOL WINAPI HookUpdateProcThreadAttribute(LPPROC_THREAD_ATTRIBUTE_LIST list, DWORD flags,
                                          DWORD_PTR attribute, PVOID value, SIZE_T size,
                                          PVOID previous, PSIZE_T return_size) {
  if (attribute != PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY || !value || size < sizeof(DWORD64)) {
    return g_original(list, flags, attribute, value, size, previous, return_size);
  }

  auto* policy = static_cast<DWORD64*>(value);
  const DWORD64 before = policy[0];
  if ((before & kBlockNonMicrosoftMask) == 0) {
    return g_original(list, flags, attribute, value, size, previous, return_size);
  }
  policy[0] = before & ~kBlockNonMicrosoftMask;
  const BOOL ok = g_original(list, flags, attribute, value, size, previous, return_size);
  const LONG count = InterlockedIncrement(&g_strip_count);
  if (count <= 8) {
    ARB_INFO("child CIG disabled: policy %016llx -> %016llx (ok=%d)",
             static_cast<unsigned long long>(before),
             static_cast<unsigned long long>(policy[0]), ok);
  }
  return ok;
}

}  // namespace

bool InstallCigBypass() {
  MH_STATUS status = HookRuntimeInitialize();
  if (status != MH_OK) {
    ARB_ERROR("MinHook init failed: %d", status);
    return false;
  }
  HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
  if (!kernelbase) {
    ARB_ERROR("kernelbase.dll is not loaded");
    return false;
  }
  auto* target = reinterpret_cast<void*>(GetProcAddress(kernelbase, "UpdateProcThreadAttribute"));
  if (!target) {
    ARB_ERROR("UpdateProcThreadAttribute not found");
    return false;
  }
  const MH_STATUS created =
      MH_CreateHook(target, reinterpret_cast<LPVOID>(&HookUpdateProcThreadAttribute),
                    reinterpret_cast<LPVOID*>(&g_original));
  if (created != MH_OK) {
    ARB_ERROR("MH_CreateHook(UpdateProcThreadAttribute) failed: %d", created);
    return false;
  }
  const MH_STATUS enabled = MH_EnableHook(target);
  if (enabled != MH_OK) {
    ARB_ERROR("MH_EnableHook(UpdateProcThreadAttribute) failed: %d", enabled);
    return false;
  }
  ARB_INFO("child process CIG mitigation is disabled (this is required, not optional)");
  return true;
}

long CigStripCount() { return g_strip_count; }

}  // namespace arbortab
