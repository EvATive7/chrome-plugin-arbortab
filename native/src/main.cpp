// ArborTab native module entry point (version.dll proxy).
#include <windows.h>

#include "core/cig.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/offsets.h"
#include "core/paths.h"
#include "proxy/exports.h"

namespace {

constexpr const char* kModuleVersion = "0.1.0";
constexpr const wchar_t* kChromeDllName = L"chrome.dll";

HMODULE g_chrome_dll = nullptr;
void* g_dll_notification = nullptr;

// Loader notification callbacks must return quickly: heavy work goes to a worker thread,
// while the detours we need before any child process is spawned were already installed from
// DllMain.
struct ModuleName {
  USHORT length;
  USHORT maximum_length;
  const wchar_t* buffer;
};

// Mirrors LDR_DLL_LOADED_NOTIFICATION_DATA (winternl.h is not includable here without
// dragging in conflicting ntdll declarations).
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
      arbortab::InstallFeatureHooks(g_chrome_dll, offsets);
  ARB_INFO("features: %s", arbortab::DescribeFeatures(features).c_str());
  if (!features.layout) {
    ARB_WARN("no feature could be enabled; Chrome keeps its native tab strip");
  }
}

void StartPipeline(HMODULE chrome_dll) {
  g_chrome_dll = chrome_dll;
  arbortab::StartOffsetsResolution(chrome_dll, &OnOffsetsResolved);
}

void CALLBACK OnDllNotification(ULONG reason, const DllLoadedData* data, PVOID) {
  constexpr ULONG kLoaded = 1;  // LDR_DLL_NOTIFICATION_REASON_LOADED
  if (reason != kLoaded || !data || !data->base_name) {
    return;
  }
  if (data->base_name->length != wcslen(kChromeDllName) * sizeof(wchar_t) ||
      _wcsnicmp(data->base_name->buffer, kChromeDllName, wcslen(kChromeDllName)) != 0) {
    return;
  }
  ARB_INFO("chrome.dll mapped at %p", data->base);
  StartPipeline(static_cast<HMODULE>(data->base));
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
  ARB_INFO("ArborTab native %s attached to browser process %lu (log=%ls)",
                     kModuleVersion, GetCurrentProcessId(), arbortab::LogPath().c_str());

  // chrome.dll may already be mapped if the notification arrived before this thread ran.
  HMODULE chrome = GetModuleHandleW(kChromeDllName);
  if (chrome && !g_chrome_dll) {
    ARB_INFO("chrome.dll already mapped at %p", chrome);
    StartPipeline(chrome);
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

  auto* ntdll = GetModuleHandleW(L"ntdll.dll");
  auto register_notification = reinterpret_cast<LdrRegisterDllNotificationFn>(
      ntdll ? GetProcAddress(ntdll, "LdrRegisterDllNotification") : nullptr);
  if (register_notification) {
    const LONG status =
        register_notification(0, &OnDllNotification, nullptr, &g_dll_notification);
    if (status != 0) {
      arbortab::LogInit(arbortab::LogLevel::kInfo);
      ARB_ERROR("LdrRegisterDllNotification failed: 0x%08lx", status);
    }
  } else {
    arbortab::LogInit(arbortab::LogLevel::kInfo);
    ARB_ERROR("LdrRegisterDllNotification is unavailable");
  }

  if (HANDLE thread = CreateThread(nullptr, 0, &InitThread, nullptr, 0, nullptr)) {
    CloseHandle(thread);
  }
  return TRUE;
}
