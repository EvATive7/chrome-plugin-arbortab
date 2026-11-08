// PoC: version.dll proxy + hook UnpinnedTabContainerViewLayout::CalculateProposedLayout (Chrome 156.0.8078.80 only)
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include "MinHook.h"

// ---------- logging ----------
static void Log(const char* fmt, ...) {
  char path[MAX_PATH]; GetTempPathA(MAX_PATH, path); strcat_s(path, "arbortab_poc.log");
  FILE* f; if (fopen_s(&f, path, "a")) return;
  va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fputc('\n', f); fclose(f);
}

// ---------- version.dll forwarding (generic x64 forwarders, <=8 integer args) ----------
static HMODULE g_real;
static FARPROC Real(const char* name) {
  if (!g_real) {
    wchar_t p[MAX_PATH]; GetSystemDirectoryW(p, MAX_PATH); wcscat_s(p, L"\\version.dll");
    g_real = LoadLibraryW(p);
  }
  FARPROC r = GetProcAddress(g_real, name);
  return r;
}
typedef uintptr_t (*Fwd)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
#define FWD(n) extern "C" uintptr_t P_##n(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h) { \
  Fwd fn = (Fwd)GetProcAddress(g_real, #n); return fn(a, b, c, d, e, f, g, h); }
FWD(GetFileVersionInfoA) FWD(GetFileVersionInfoByHandle) FWD(GetFileVersionInfoExA) FWD(GetFileVersionInfoExW)
FWD(GetFileVersionInfoSizeA) FWD(GetFileVersionInfoSizeExA) FWD(GetFileVersionInfoSizeExW) FWD(GetFileVersionInfoSizeW)
FWD(GetFileVersionInfoW) FWD(VerFindFileA) FWD(VerFindFileW) FWD(VerInstallFileA) FWD(VerInstallFileW)
FWD(VerLanguageNameA) FWD(VerLanguageNameW) FWD(VerQueryValueA) FWD(VerQueryValueW)

// ---------- hook ----------
static const uint32_t kRvaCalcProposedLayout = 0x0993c680;
static const uint8_t kPrologue[] = {0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83, 0xec, 0x20};

typedef void* (*CalcFn)(const void* self, void* ret, const void* size_bounds);
static CalcFn g_orig;
static volatile LONG g_calls;

static void* HookCalc(const void* self, void* ret, const void* size_bounds) {
  void* r = g_orig(self, ret, size_bounds);
  // ProposedLayout::child_layouts is absl::InlinedVector<ChildLayout, 8>:
  // +0x08 metadata = (size << 1) | is_allocated, +0x10 inline data or {ptr, capacity}.
  size_t meta = *(size_t*)((char*)r + 0x08);
  char* begin = (meta & 1) ? *(char**)((char*)r + 0x10) : (char*)r + 0x10;
  char* end = begin + (meta >> 1) * 0x30;
  LONG n = InterlockedIncrement(&g_calls);
  int count = (int)(meta >> 1);
  int first_before = count > 0 ? *(int*)(begin + 0x0c) : -1;
  int first_w = count > 0 ? *(int*)(begin + 0x14) : -1;
  if (n <= 60) Log("call#%ld self=%p ret=%p orient=%d count=%d alloc=%d host=%dx%d first_x=%d w=%d", n,
                   self, r, *(int*)((char*)self + 0x270), count, (int)(meta & 1),
                   *(int*)((char*)r), *(int*)((char*)r + 4), first_before, first_w);
  char mode[64] = {0};
  GetEnvironmentVariableA("ARBORTAB_POC_MODE", mode, sizeof(mode));
  if (strcmp(mode, "hooknoop") != 0) {
    for (char* p = begin; p < begin + count * 0x30; p += 0x30) { // sizeof(ChildLayout)
      int* b = (int*)(p + 0x0c);                                // ChildLayout::bounds {x,y,w,h}
      b[0] += 20;
      if (b[2] > 20) b[2] -= 20;
    }
  }
  return r;
}

// Chrome enables PROCESS_CREATION_MITIGATION_POLICY_BLOCK_NON_MICROSOFT_BINARIES for
// renderer processes, which makes process creation fail with STATUS_INVALID_IMAGE_HASH as
// soon as an unsigned version.dll sits next to chrome.exe. Children are created by the
// browser process, so strip those bits here (CIG stays off for Chrome children only).
typedef BOOL(WINAPI* UpdateProcAttrFn)(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD_PTR, PVOID, SIZE_T, PVOID, PSIZE_T);
static UpdateProcAttrFn g_update_proc_attr;
static volatile LONG g_cig_strips;

static BOOL WINAPI HookUpdateProcThreadAttribute(LPPROC_THREAD_ATTRIBUTE_LIST list, DWORD flags,
                                                  DWORD_PTR attr, PVOID value, SIZE_T size,
                                                  PVOID prev, PSIZE_T ret_size) {
  char bitbuf[16] = {0};
  int clear_bit = 44;
  if (GetEnvironmentVariableA("ARBORTAB_POC_CLEARBIT", bitbuf, sizeof(bitbuf)))
    clear_bit = atoi(bitbuf);
  char nomod[8] = {0};
  const bool nomod_on = GetEnvironmentVariableA("ARBORTAB_POC_NOMOD", nomod, sizeof(nomod)) != 0;
  char v0[64] = {0};
  if (value && size >= 8) {
    DWORD64 w0 = *(DWORD64*)value;
    _snprintf_s(v0, sizeof(v0), _TRUNCATE, "%016llx", (unsigned long long)w0);
    if (size >= 16) {
      DWORD64 w1 = ((DWORD64*)value)[1];
      char tmp[32]; _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "/%016llx", (unsigned long long)w1);
      strcat_s(v0, tmp);
    }
  }
  DWORD64 mask = 0xFULL << (clear_bit & ~3);
  if (!nomod_on && attr == PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY && value && size >= 8) {
    DWORD64* inplace = (DWORD64*)value;
    inplace[0] &= ~mask;
  }
  BOOL ok = g_update_proc_attr(list, flags, attr, value, size, prev, ret_size);
  LONG n = InterlockedIncrement(&g_cig_strips);
  if (n <= 80)
    Log("UPTA attr=%llu size=%zu val=%s nomod=%d ok=%d err=%lu", (unsigned long long)attr, size, v0,
        (int)nomod_on, ok, ok ? 0 : GetLastError());
  return ok;
}

static void InstallCigBypass() {
  MH_STATUS s = MH_Initialize();
  if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { Log("MH_Initialize failed %d", s); return; }
  HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
  if (!kb) kb = LoadLibraryW(L"kernelbase.dll");
  void* target = (void*)GetProcAddress(kb, "UpdateProcThreadAttribute");
  if (!target) { Log("UpdateProcThreadAttribute not found"); return; }
  int c = MH_CreateHook(target, (void*)HookUpdateProcThreadAttribute, (void**)&g_update_proc_attr);
  int e = MH_EnableHook(target);
  Log("cig bypass create=%d enable=%d target=%p", c, e, target);
}

typedef void (*VoidFn)();
static VoidFn g_other_orig;
static void HookOther() { g_other_orig(); }

typedef BOOL(WINAPI* CreateProcWFn)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
                                    BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
static CreateProcWFn g_create_process_w;

static BOOL WINAPI HookCreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa,
                                      LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags,
                                      LPVOID env, LPCWSTR cwd, LPSTARTUPINFOW si,
                                      LPPROCESS_INFORMATION pi) {
  BOOL ok = g_create_process_w(app, cmd, pa, ta, inherit, flags, env, cwd, si, pi);
  wchar_t buf[300] = {0};
  if (cmd) wcsncpy_s(buf, cmd, 220);
  Log("CreateProcessW ok=%d err=%lu pid=%lu cmd=%ls", ok, ok ? 0 : GetLastError(),
      ok && pi ? pi->dwProcessId : 0, buf);
  return ok;
}

typedef BOOL(WINAPI* CreateProcAsUserWFn)(HANDLE, LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
                                          LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR,
                                          LPSTARTUPINFOW, LPPROCESS_INFORMATION);
static CreateProcAsUserWFn g_create_process_as_user_w;

static BOOL WINAPI HookCreateProcessAsUserW(HANDLE token, LPCWSTR app, LPWSTR cmd,
                                            LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
                                            BOOL inherit, DWORD flags, LPVOID env, LPCWSTR cwd,
                                            LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
  BOOL ok = g_create_process_as_user_w(token, app, cmd, pa, ta, inherit, flags, env, cwd, si, pi);
  wchar_t buf[300] = {0};
  if (cmd) wcsncpy_s(buf, cmd, 200);
  Log("CreateProcessAsUserW ok=%d err=%lu pid=%lu cmd=%ls", ok, ok ? 0 : GetLastError(),
      ok && pi ? pi->dwProcessId : 0, buf);
  return ok;
}

static void InstallCreateProcessTrace() {
  MH_Initialize();
  HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
  {
    void* t2 = (void*)GetProcAddress(kb, "CreateProcessAsUserW");
    if (t2) {
      int c2 = MH_CreateHook(t2, (void*)HookCreateProcessAsUserW, (void**)&g_create_process_as_user_w);
      int e2 = MH_EnableHook(t2);
      Log("trace asuser create=%d enable=%d target=%p", c2, e2, t2);
    }
  }
  void* target = (void*)GetProcAddress(kb, "CreateProcessW");
  int c = MH_CreateHook(target, (void*)HookCreateProcessW, (void**)&g_create_process_w);
  int e = MH_EnableHook(target);
  Log("tracecp create=%d enable=%d target=%p", c, e, target);
}

static void InstallHooks(uint8_t* base) {
  char mode[64] = {0};
  GetEnvironmentVariableA("ARBORTAB_POC_MODE", mode, sizeof(mode));
  if (strcmp(mode, "otherhook") == 0) {
    uint8_t* other = base + 0x0acfcda0;  // gfx::Canvas::DrawStringRect, unrelated to spawning
    int c = MH_CreateHook(other, (void*)HookOther, (void**)&g_other_orig);
    int e = MH_EnableHook(other);
    Log("otherhook create=%d enable=%d at %p", c, e, other);
    return;
  }
  uint8_t* target = base + kRvaCalcProposedLayout;
  if (memcmp(target, kPrologue, sizeof(kPrologue)) != 0) { Log("prologue mismatch, not hooking"); return; }
  MH_Initialize();
  int s1 = MH_CreateHook(target, (void*)HookCalc, (void**)&g_orig);
  int s2 = MH_EnableHook(target);
  Log("hook installed create=%d enable=%d", s1, s2);
}

// ---------- wait for chrome.dll ----------
struct US { USHORT Length, MaximumLength; PWSTR Buffer; };
struct LdrData { ULONG Flags; const US* FullDllName; const US* BaseDllName; PVOID DllBase; ULONG SizeOfImage; };
typedef VOID(CALLBACK* LdrCb)(ULONG reason, const LdrData* data, PVOID ctx);
typedef LONG(NTAPI* LdrRegFn)(ULONG, LdrCb, PVOID, PVOID*);
static PVOID g_cookie;

static VOID CALLBACK OnDll(ULONG reason, const LdrData* d, PVOID) {
  if (reason != 1) return;  // LDR_DLL_NOTIFICATION_REASON_LOADED
  if (d->BaseDllName->Length == 20 && _wcsnicmp(d->BaseDllName->Buffer, L"chrome.dll", 10) == 0) {
    Log("chrome.dll loaded at %p", d->DllBase);
    InstallHooks((uint8_t*)d->DllBase);
  }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(h);
    Real("VerQueryValueW");  // load the system version.dll before any sandbox is applied
    if (wcsstr(GetCommandLineW(), L"--type=")) return TRUE;  // only the browser process
    Log("version.dll proxy attached to browser process pid=%lu", GetCurrentProcessId());
    char mode[64] = {0};
    GetEnvironmentVariableA("ARBORTAB_POC_MODE", mode, sizeof(mode));
    Log("mode=%s", mode[0] ? mode : "full");
    if (strcmp(mode, "noop") == 0) return TRUE;  // forwarding only, no hooks
    char nocig[8] = {0};
    if (!GetEnvironmentVariableA("ARBORTAB_POC_NOCIG", nocig, sizeof(nocig))) InstallCigBypass();
    if (strcmp(mode, "cigonly") == 0) return TRUE;
    if (strcmp(mode, "tracecp") == 0) { InstallCreateProcessTrace(); return TRUE; }
    Log("verifying hook path; mode=%s", mode);
    if (strcmp(mode, "nocb") == 0) {
      CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        for (int i = 0; i < 2000; ++i) {
          HMODULE m = GetModuleHandleW(L"chrome.dll");
          if (m) { Log("nocb: chrome.dll at %p", m); InstallHooks((uint8_t*)m); return 0; }
          Sleep(5);
        }
        Log("nocb: chrome.dll never appeared");
        return 1;
      }, nullptr, 0, nullptr);
      return TRUE;
    }
    auto reg = (LdrRegFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    if (reg) reg(0, OnDll, nullptr, &g_cookie);
  }
  return TRUE;
}
