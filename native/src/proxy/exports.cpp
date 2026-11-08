#include "proxy/exports.h"

#include <string>

#include "core/log.h"

namespace arbortab {
namespace {

HMODULE g_real = nullptr;

FARPROC Resolve(const char* name) {
  if (!g_real) {
    return nullptr;
  }
  return GetProcAddress(g_real, name);
}

template <typename T>
T Forward(const char* name) {
  return reinterpret_cast<T>(Resolve(name));
}

// Generic fallback for the one export without a documented prototype.
using GenericFn = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                uintptr_t, uintptr_t);

}  // namespace

void LoadRealVersionModule() {
  if (g_real) {
    return;
  }
  wchar_t path[MAX_PATH] = {0};
  const UINT length = GetSystemDirectoryW(path, MAX_PATH);
  if (length && length < MAX_PATH - 16) {
    wcscat_s(path, L"\\version.dll");
    g_real = LoadLibraryW(path);
  }
  if (!g_real) {
    g_real = LoadLibraryW(L"version.dll");
  }
}

FARPROC RealVersionProc(const char* name) { return Resolve(name); }

}  // namespace arbortab

// The exports carry the system names through native/version.def so that they never clash with
// the declarations from <winver.h>.
using namespace arbortab;

extern "C" {

uintptr_t P_GetFileVersionInfoA(LPCSTR filename, DWORD handle, DWORD length, LPVOID data) {
  using Fn = BOOL(WINAPI*)(LPCSTR, DWORD, DWORD, LPVOID);
  static Fn fn = Forward<Fn>("GetFileVersionInfoA");
  return fn ? fn(filename, handle, length, data) : FALSE;
}

uintptr_t P_GetFileVersionInfoW(LPCWSTR filename, DWORD handle, DWORD length, LPVOID data) {
  using Fn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
  static Fn fn = Forward<Fn>("GetFileVersionInfoW");
  return fn ? fn(filename, handle, length, data) : FALSE;
}

uintptr_t P_GetFileVersionInfoExA(DWORD flags, LPCSTR filename, DWORD handle, DWORD length,
                                 LPVOID data) {
  using Fn = BOOL(WINAPI*)(DWORD, LPCSTR, DWORD, DWORD, LPVOID);
  static Fn fn = Forward<Fn>("GetFileVersionInfoExA");
  return fn ? fn(flags, filename, handle, length, data) : FALSE;
}

uintptr_t P_GetFileVersionInfoExW(DWORD flags, LPCWSTR filename, DWORD handle, DWORD length,
                                 LPVOID data) {
  using Fn = BOOL(WINAPI*)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
  static Fn fn = Forward<Fn>("GetFileVersionInfoExW");
  return fn ? fn(flags, filename, handle, length, data) : FALSE;
}

uintptr_t P_GetFileVersionInfoByHandle(HANDLE file, LPCWSTR filename, DWORD length, LPVOID data) {
  static GenericFn fn = Forward<GenericFn>("GetFileVersionInfoByHandle");
  if (!fn) {
    return FALSE;
  }
  return fn(reinterpret_cast<uintptr_t>(file), reinterpret_cast<uintptr_t>(filename), length,
            reinterpret_cast<uintptr_t>(data), 0, 0, 0, 0);
}

uintptr_t P_GetFileVersionInfoSizeA(LPCSTR filename, LPDWORD handle) {
  using Fn = DWORD(WINAPI*)(LPCSTR, LPDWORD);
  static Fn fn = Forward<Fn>("GetFileVersionInfoSizeA");
  return fn ? fn(filename, handle) : 0;
}

uintptr_t P_GetFileVersionInfoSizeW(LPCWSTR filename, LPDWORD handle) {
  using Fn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
  static Fn fn = Forward<Fn>("GetFileVersionInfoSizeW");
  return fn ? fn(filename, handle) : 0;
}

uintptr_t P_GetFileVersionInfoSizeExA(DWORD flags, LPCSTR filename, LPDWORD handle) {
  using Fn = DWORD(WINAPI*)(DWORD, LPCSTR, LPDWORD);
  static Fn fn = Forward<Fn>("GetFileVersionInfoSizeExA");
  return fn ? fn(flags, filename, handle) : 0;
}

uintptr_t P_GetFileVersionInfoSizeExW(DWORD flags, LPCWSTR filename, LPDWORD handle) {
  using Fn = DWORD(WINAPI*)(DWORD, LPCWSTR, LPDWORD);
  static Fn fn = Forward<Fn>("GetFileVersionInfoSizeExW");
  return fn ? fn(flags, filename, handle) : 0;
}

uintptr_t P_VerFindFileA(DWORD flags, LPCSTR filename, LPCSTR win_dir, LPCSTR app_dir,
                         LPSTR cur_dir, PUINT cur_dir_len, LPSTR dest_dir, PUINT dest_dir_len) {
  using Fn = DWORD(WINAPI*)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT, LPSTR, PUINT);
  static Fn fn = Forward<Fn>("VerFindFileA");
  return fn ? fn(flags, filename, win_dir, app_dir, cur_dir, cur_dir_len, dest_dir, dest_dir_len)
            : 0;
}

uintptr_t P_VerFindFileW(DWORD flags, LPCWSTR filename, LPCWSTR win_dir, LPCWSTR app_dir,
                         LPWSTR cur_dir, PUINT cur_dir_len, LPWSTR dest_dir, PUINT dest_dir_len) {
  using Fn = DWORD(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT, LPWSTR, PUINT);
  static Fn fn = Forward<Fn>("VerFindFileW");
  return fn ? fn(flags, filename, win_dir, app_dir, cur_dir, cur_dir_len, dest_dir, dest_dir_len)
            : 0;
}

uintptr_t P_VerInstallFileA(DWORD flags, LPCSTR src_filename, LPCSTR dest_filename,
                            LPCSTR src_dir, LPCSTR dest_dir, LPSTR tmp_file, PUINT tmp_file_len) {
  using Fn = DWORD(WINAPI*)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT);
  static Fn fn = Forward<Fn>("VerInstallFileA");
  return fn ? fn(flags, src_filename, dest_filename, src_dir, dest_dir, tmp_file, tmp_file_len) : 0;
}

uintptr_t P_VerInstallFileW(DWORD flags, LPCWSTR src_filename, LPCWSTR dest_filename,
                            LPCWSTR src_dir, LPCWSTR dest_dir, LPWSTR tmp_file,
                            PUINT tmp_file_len) {
  using Fn = DWORD(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT);
  static Fn fn = Forward<Fn>("VerInstallFileW");
  return fn ? fn(flags, src_filename, dest_filename, src_dir, dest_dir, tmp_file, tmp_file_len) : 0;
}

uintptr_t P_VerLanguageNameA(DWORD language, LPSTR buffer, DWORD length) {
  using Fn = DWORD(WINAPI*)(DWORD, LPSTR, DWORD);
  static Fn fn = Forward<Fn>("VerLanguageNameA");
  return fn ? fn(language, buffer, length) : 0;
}

uintptr_t P_VerLanguageNameW(DWORD language, LPWSTR buffer, DWORD length) {
  using Fn = DWORD(WINAPI*)(DWORD, LPWSTR, DWORD);
  static Fn fn = Forward<Fn>("VerLanguageNameW");
  return fn ? fn(language, buffer, length) : 0;
}

uintptr_t P_VerQueryValueA(const void* block, LPCSTR sub_block, LPVOID* buffer, PUINT length) {
  using Fn = BOOL(WINAPI*)(LPCVOID, LPCSTR, LPVOID*, PUINT);
  static Fn fn = Forward<Fn>("VerQueryValueA");
  return fn ? fn(block, sub_block, buffer, length) : FALSE;
}

uintptr_t P_VerQueryValueW(const void* block, LPCWSTR sub_block, LPVOID* buffer, PUINT length) {
  using Fn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
  static Fn fn = Forward<Fn>("VerQueryValueW");
  return fn ? fn(block, sub_block, buffer, length) : FALSE;
}

}  // extern "C"
