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
  for (char* p = begin; p < end; p += 0x30) { // sizeof(ChildLayout)
    int* b = (int*)(p + 0x0c);                // ChildLayout::bounds {x,y,w,h}
    b[0] += 20;
    if (b[2] > 20) b[2] -= 20;
  }
  if (InterlockedIncrement(&g_calls) <= 3) Log("hook called, children=%d", (int)((end - begin) / 0x30));
  return r;
}

static void InstallHooks(uint8_t* base) {
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
    auto reg = (LdrRegFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    if (reg) reg(0, OnDll, nullptr, &g_cookie);
  }
  return TRUE;
}
