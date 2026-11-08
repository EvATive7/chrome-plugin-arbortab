// ArborTab native module entry point (version.dll proxy).
#include <windows.h>

#include "core/cig.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/offsets.h"
#include "core/paths.h"
#include "core/state.h"
#include "link/ws_server.h"
#include "proxy/exports.h"

namespace {

constexpr const char* kModuleVersion = "0.1.0";
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

void OnOffsetsResolved(bool ok, const arbortab::Offsets& offsets, const char* error) {
  if (!ok) {
    ARB_ERROR("offsets unavailable: %s", error ? error : "unknown error");
    return;
  }
  ARB_INFO("offsets ready (source=%s, chrome=%s, hooks=%zu)", offsets.source.c_str(),
           offsets.chrome_version.c_str(), offsets.hooks.size());
  const arbortab::FeatureState features =
      arbortab::InstallFeatureHooks(static_cast<HMODULE>(g_chrome_base), offsets);
  ARB_INFO("features: %s", arbortab::DescribeFeatures(features).c_str());
  if (!features.layout) {
    ARB_WARN("no feature could be enabled; Chrome keeps its native tab strip");
  }
  arbortab::ServerFeatures server_features;
  server_features.layout = features.layout;
  server_features.twisty = features.twisty;
  server_features.paint = features.paint;
  server_features.drag = features.drag;
  const int port = arbortab::StartWebSocketServer(server_features, kModuleVersion);
  if (port) {
    ARB_INFO("extension endpoint ready: ws://127.0.0.1:%d/arbortab", port);
  } else {
    ARB_WARN("extension endpoint unavailable; tab tree state cannot be applied");
  }
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
      arbortab::StartOffsetsResolution(chrome, &OnOffsetsResolved);
      return 0;
    }
    // Periodically check GetModuleHandle: the notification only fires while our hook is
    // registered, but chrome.dll may already be mapped before this thread starts.
    if (HMODULE chrome = GetModuleHandleW(kChromeDllName)) {
      g_chrome_base = chrome;
      ARB_INFO("chrome.dll already mapped at %p", chrome);
      arbortab::StartOffsetsResolution(chrome, &OnOffsetsResolved);
      return 0;
    }
  }
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
  ARB_INFO("ArborTab native %s attached to browser process %lu (log=%ls)", kModuleVersion,
           GetCurrentProcessId(), arbortab::LogPath().c_str());

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
