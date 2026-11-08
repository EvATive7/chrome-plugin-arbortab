#include "core/paths.h"

#include <windows.h>
#include <shlobj.h>

#include <mutex>

namespace arbortab {
namespace {

std::once_flag g_once;
std::wstring g_data_dir;
std::wstring g_log_dir;
std::wstring g_log_path;
std::wstring g_config_path;

void EnsureDirs() {
  std::call_once(g_once, [] {
    wchar_t local_app_data[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local_app_data))) {
      GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
    }
    g_data_dir = std::wstring(local_app_data) + L"\\ArborTab";
    CreateDirectoryW(g_data_dir.c_str(), nullptr);
    g_log_dir = g_data_dir + L"\\logs";
    CreateDirectoryW(g_log_dir.c_str(), nullptr);
    g_log_path = g_log_dir + L"\\native.log";
    g_config_path = g_data_dir + L"\\config.ini";
  });
}

}  // namespace

const std::wstring& DataDir() {
  EnsureDirs();
  return g_data_dir;
}

const std::wstring& LogDir() {
  EnsureDirs();
  return g_log_dir;
}

const std::wstring& LogPath() {
  EnsureDirs();
  return g_log_path;
}

const std::wstring& ConfigPath() {
  EnsureDirs();
  return g_config_path;
}

std::wstring CachedOffsetsPath(const std::string& pdb_key) {
  std::wstring dir = DataDir() + L"\\offsets";
  CreateDirectoryW(dir.c_str(), nullptr);
  const std::wstring wide_key(pdb_key.begin(), pdb_key.end());
  return dir + L"\\" + wide_key + L".json";
}

std::string ReadConfigString(const char* key, const char* fallback) {
  const std::wstring& path = ConfigPath();
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return fallback;
  }
  wchar_t buffer[512] = {0};
  const std::wstring wide_key(key, key + strlen(key));
  GetPrivateProfileStringW(L"arbortab", wide_key.c_str(), L"", buffer, 512, path.c_str());
  if (!buffer[0]) {
    return fallback;
  }
  char utf8[1024] = {0};
  const int written = WideCharToMultiByte(CP_UTF8, 0, buffer, -1, utf8, sizeof(utf8), nullptr, nullptr);
  if (written <= 1) {
    return fallback;
  }
  return std::string(utf8, static_cast<size_t>(written - 1));
}

int ReadConfigInt(const char* key, int fallback) {
  const std::wstring& path = ConfigPath();
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return fallback;
  }
  const std::wstring wide_key(key, key + strlen(key));
  return GetPrivateProfileIntW(L"arbortab", wide_key.c_str(), fallback, path.c_str());
}

}  // namespace arbortab
