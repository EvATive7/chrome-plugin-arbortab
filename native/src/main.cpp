// ArborTab native module entry point (version.dll proxy).
#include <windows.h>
#include <shlwapi.h>

#include <exception>
#include <cstdlib>

#include "core/cig.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/offsets.h"
#include "core/paths.h"
#include "core/state.h"
#include "link/ws_server.h"
#include "proxy/exports.h"

namespace {

// ARBORTAB_VERSION comes from the repository's VERSION file (native/CMakeLists.txt).
constexpr const char* kModuleVersion = ARBORTAB_VERSION;
#ifdef ARBORTAB_BUILD_STAMP
constexpr const char* kBuildStamp = ARBORTAB_BUILD_STAMP;  // "MM-DD HH:MM", for tools/dev/dev.py
#else
constexpr const char* kBuildStamp = "unknown";
#endif
constexpr const wchar_t* kChromeDllName = L"chrome.dll";

// The DLL notification callback runs while the loader lock is held: it must not log, allocate
// threads or touch the filesystem. It only records the base address and wakes the worker.
HANDLE g_chrome_loaded_event = nullptr;
PVOID volatile g_chrome_base = nullptr;

struct ModuleName {
  USHORT length;
  USHORT maximum_length;
  const wchar_t* buffer;
};

// Mirrors LDR_DLL_LOADED_NOTIFICATION_DATA without pulling in winternl.h, whose declarations
// clash with the ones this translation unit already gets from <windows.h>.
struct DllLoadedData {
  ULONG flags;
  const ModuleName* full_name;
  const ModuleName* base_name;
  PVOID base;
  ULONG size;
};

using DllNotificationCallback = void(CALLBACK*)(ULONG, const DllLoadedData*, PVOID);
using LdrRegisterDllNotificationFn = LONG(NTAPI*)(ULONG, DllNotificationCallback, PVOID, PVOID*);

std::string g_chrome_version;  // written by the offsets worker before every status update

void OnOffsetsStatus(arbortab::OffsetsStatus status, const std::string& chrome_version) {
  g_chrome_version = chrome_version;
  if (status == arbortab::OffsetsStatus::kReady) {
    return;  // reported together with the features once the hooks are installed
  }
  arbortab::ServerStatus server;
  server.offsets = arbortab::OffsetsStatusName(status);
  server.chrome_version = chrome_version;
  arbortab::UpdateServerStatus(server);
}

void OnOffsetsResolved(const arbortab::Offsets& offsets) {
  ARB_INFO("offsets ready (source=%s, chrome=%s, hooks=%zu)", offsets.source.c_str(),
           offsets.chrome_version.c_str(), offsets.hooks.size());
  const arbortab::FeatureState features =
      arbortab::InstallFeatureHooks(static_cast<HMODULE>(g_chrome_base), offsets);
  ARB_INFO("features: %s", arbortab::DescribeFeatures(features).c_str());
  if (!features.layout) {
    ARB_WARN("no feature could be enabled; Chrome keeps its native tab strip");
  }
  arbortab::ServerStatus server;
  server.features.layout = features.layout;
  server.features.twisty = features.twisty;
  server.features.paint = features.paint;
  server.features.drag = features.drag;
  server.offsets = arbortab::OffsetsStatusName(arbortab::OffsetsStatus::kReady);
  server.chrome_version = offsets.chrome_version.empty() ? g_chrome_version : offsets.chrome_version;
  arbortab::UpdateServerStatus(server);
}

// The endpoint comes up before the offsets, so the extension can report "waiting for offsets"
// instead of "not installed".
void StartPipeline(HMODULE chrome) {
  const int port = arbortab::StartWebSocketServer(kModuleVersion);
  if (port) {
    ARB_INFO("extension endpoint ready: ws://127.0.0.1:%d/arbortab", port);
  } else {
    ARB_WARN("extension endpoint unavailable; tab tree state cannot be applied");
  }
  arbortab::StartOffsetsResolution(chrome, &OnOffsetsResolved, &OnOffsetsStatus);
}

void CALLBACK OnDllNotification(ULONG reason, const DllLoadedData* data, PVOID) {
  constexpr ULONG kLoaded = 1;  // LDR_DLL_NOTIFICATION_REASON_LOADED
  if (reason != kLoaded || !data || !data->base_name || g_chrome_base) {
    return;
  }
  if (data->base_name->length != wcslen(kChromeDllName) * sizeof(wchar_t) ||
      _wcsnicmp(data->base_name->buffer, kChromeDllName, wcslen(kChromeDllName)) != 0) {
    return;
  }
  g_chrome_base = data->base;
  if (g_chrome_loaded_event) {
    SetEvent(g_chrome_loaded_event);
  }
}

// Waits for chrome.dll and then starts the offsets pipeline. Runs outside the loader lock.
DWORD WINAPI PipelineThread(LPVOID) {
  const HANDLE waits[] = {g_chrome_loaded_event};
  for (;;) {
    const DWORD result = WaitForMultipleObjects(1, waits, FALSE, 60000);
    if (result == WAIT_OBJECT_0) {
      HMODULE chrome = static_cast<HMODULE>(g_chrome_base);
      if (!chrome) {
        continue;
      }
      ARB_INFO("chrome.dll mapped at %p", chrome);
      StartPipeline(chrome);
      return 0;
    }
    // Periodically check GetModuleHandle: the notification only fires while our hook is
    // registered, but chrome.dll may already be mapped before this thread starts.
    if (HMODULE chrome = GetModuleHandleW(kChromeDllName)) {
      g_chrome_base = chrome;
      ARB_INFO("chrome.dll already mapped at %p", chrome);
      StartPipeline(chrome);
      return 0;
    }
  }
}

// Records the faulting address of fail-fast terminations (0xC0000409) and access violations
// inside this DLL, which keeps the log usable when Chrome dies without a crash dump.
LONG CALLBACK CrashProbe(EXCEPTION_POINTERS* info) {
  if (!info || !info->ExceptionRecord) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (code != 0xC0000409 && code != 0xC0000005) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&CrashProbe), &self);
  const auto base = reinterpret_cast<uintptr_t>(self);
  const auto address = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
  char buffer[256];
  const char* format =
      "first-chance exception code=0x%08lx address=%p self=%p self_rva=0x%llx info0=%p info1=%p";
  _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, format,
              code, reinterpret_cast<void*>(address), reinterpret_cast<void*>(base),
              static_cast<unsigned long long>(address - base),
              info->ExceptionRecord->ExceptionInformation[0] == 0
                  ? nullptr
                  : reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[0]),
              reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]));
  HANDLE file = CreateFileW(arbortab::LogPath().c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(file, buffer, static_cast<DWORD>(strlen(buffer)), &written, nullptr);
    CloseHandle(file);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

using AbortFn = void (*)(void);
AbortFn g_real_abort = nullptr;

void LogAbortStack() {
  void* frames[24] = {0};
  const USHORT count = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
  for (USHORT index = 0; index < count; ++index) {
    HMODULE module = nullptr;
    const auto address = reinterpret_cast<uintptr_t>(frames[index]);
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(frames[index]), &module)) {
      wchar_t path[MAX_PATH] = {0};
      GetModuleFileNameW(module, path, MAX_PATH);
      LPCWSTR filename = PathFindFileNameW(path);
      ARB_ERROR("abort frame[%u] %ls+0x%llx", index, filename,
                static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(module)));
    } else {
      ARB_ERROR("abort frame[%u] %p", index, frames[index]);
    }
  }
}

void HookedAbort() {
  ARB_ERROR("abort() called inside the browser process");
  LogAbortStack();
  if (g_real_abort) {
    g_real_abort();
  }
  ExitProcess(3);
}

void OnTerminate() {
  // Log what actually escaped: without this a terminate handler leaves no trace at all.
  try {
    const std::exception_ptr current = std::current_exception();
    if (current) {
      std::rethrow_exception(current);
    }
    ARB_ERROR("process terminating without an active exception");
  } catch (const std::exception& exception) {
    ARB_ERROR("uncaught exception: %s", exception.what());
  } catch (...) {
    ARB_ERROR("uncaught exception of an unknown type");
  }
  arbortab::LogShutdown();
  abort();
}

DWORD WINAPI InitThread(LPVOID) {
  arbortab::LogInit(arbortab::LogLevel::kInfo);
  const std::string level = arbortab::ReadConfigString("logLevel", "info");
  if (level == "debug") {
    arbortab::LogSetLevel(arbortab::LogLevel::kDebug);
  } else if (level == "warn") {
    arbortab::LogSetLevel(arbortab::LogLevel::kWarn);
  } else if (level == "error") {
    arbortab::LogSetLevel(arbortab::LogLevel::kError);
  }
  ARB_INFO("ArborTab native %s (build %s) attached to browser process %lu (log=%ls)",
           kModuleVersion, kBuildStamp, GetCurrentProcessId(), arbortab::LogPath().c_str());
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&InitThread), &self);
  ARB_INFO("version.dll base=%p", self);
  AddVectoredExceptionHandler(1, &CrashProbe);
  std::set_terminate(&OnTerminate);
  // Catch abort() itself so a CRT-level failure leaves a usable stack in the log.
  if (MH_CreateHook(reinterpret_cast<LPVOID>(&abort), reinterpret_cast<LPVOID>(&HookedAbort),
                    reinterpret_cast<LPVOID*>(&g_real_abort)) == MH_OK) {
    MH_EnableHook(reinterpret_cast<LPVOID>(&abort));
  }

  // chrome.dll loading is reported on another thread, so swap in the real pipeline thread
  // once the event object exists and re-check whether the module is already mapped.
  if (HANDLE thread = CreateThread(nullptr, 0, &PipelineThread, nullptr, 0, nullptr)) {
    CloseHandle(thread);
  } else {
    ARB_ERROR("cannot start the pipeline thread: %lu", GetLastError());
  }
  return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason != DLL_PROCESS_ATTACH) {
    return TRUE;
  }
  DisableThreadLibraryCalls(instance);

  // The system DLL must be loaded while the loader lock is held and before Chrome's sandbox
  // token exists, otherwise child processes crash on the first forwarded call.
  arbortab::LoadRealVersionModule();

  if (wcsstr(GetCommandLineW(), L"--type=") != nullptr) {
    return TRUE;  // child process: forwarding only
  }

  // Required, not optional: without this every Chrome child process dies with
  // STATUS_INVALID_IMAGE_HASH because our unsigned version.dll sits next to chrome.exe.
  arbortab::InstallCigBypass();

  g_chrome_loaded_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  auto* ntdll = GetModuleHandleW(L"ntdll.dll");
  auto register_notification = reinterpret_cast<LdrRegisterDllNotificationFn>(
      ntdll ? GetProcAddress(ntdll, "LdrRegisterDllNotification") : nullptr);
  if (!register_notification) {
    arbortab::LogInit(arbortab::LogLevel::kInfo);
    ARB_ERROR("LdrRegisterDllNotification is unavailable");
  } else {
    PVOID cookie = nullptr;
    const LONG status = register_notification(0, &OnDllNotification, nullptr, &cookie);
    if (status != 0) {
      arbortab::LogInit(arbortab::LogLevel::kInfo);
      ARB_ERROR("LdrRegisterDllNotification failed: 0x%08lx", status);
    }
  }

  if (HANDLE thread = CreateThread(nullptr, 0, &InitThread, nullptr, 0, nullptr)) {
    CloseHandle(thread);
  }
  return TRUE;
}
