#include "core/crash_guard.h"

#include <windows.h>

#include <cstdio>
#include <string>

#include "core/log.h"
#include "core/paths.h"

namespace arbortab {
namespace {

// Formatted up front: CrashGuardEndSession runs under the loader lock during process exit.
wchar_t g_session_path[MAX_PATH] = {0};
wchar_t g_crash_path[MAX_PATH] = {0};
char g_key[160] = {0};
volatile bool g_session_active = false;

unsigned long long CreationTime(HANDLE process) {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
    return 0;
  }
  return (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

bool ReadSmallFile(const wchar_t* path, std::string& text) {
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  char buffer[512];
  DWORD read = 0;
  const BOOL ok = ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr);
  CloseHandle(file);
  if (!ok) {
    return false;
  }
  text.assign(buffer, read);
  return true;
}

void WriteSmallFile(const wchar_t* path, const char* text, size_t size) {
  HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD written = 0;
  WriteFile(file, text, static_cast<DWORD>(size), &written, nullptr);
  CloseHandle(file);
}

// crashes.txt: "<key>\n<count>\n"
void WriteCrashCount(int count) {
  char text[200];
  const int size = _snprintf_s(text, sizeof(text), _TRUNCATE, "%s\n%d\n", g_key, count);
  if (size > 0) {
    WriteSmallFile(g_crash_path, text, static_cast<size_t>(size));
  }
}

int ReadCrashCount() {
  std::string text;
  if (!ReadSmallFile(g_crash_path, text)) {
    return 0;
  }
  const size_t newline = text.find('\n');
  if (newline == std::string::npos || text.substr(0, newline) != g_key) {
    return 0;  // another build: its crashes say nothing about this one
  }
  return atoi(text.c_str() + newline + 1);
}

// A session file whose process is gone (or whose pid now belongs to a newer process) was left
// by a browser that never exited cleanly.
bool SessionStillRunning(DWORD pid, unsigned long long created) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) {
    return false;
  }
  DWORD exit_code = 0;
  const bool running = GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE &&
                       CreationTime(process) == created;
  CloseHandle(process);
  return running;
}

}  // namespace

CrashGuardState CrashGuardCheck(const std::string& module_version, const std::string& pdb_key) {
  _snprintf_s(g_key, sizeof(g_key), _TRUNCATE, "%s|%s", module_version.c_str(), pdb_key.c_str());
  const std::wstring sessions = DataDir() + L"\\sessions";
  CreateDirectoryW(sessions.c_str(), nullptr);
  swprintf_s(g_session_path, L"%s\\%lu.session", sessions.c_str(), GetCurrentProcessId());
  swprintf_s(g_crash_path, L"%s\\crashes.txt", DataDir().c_str());

  int crashes = ReadCrashCount();
  WIN32_FIND_DATAW found{};
  HANDLE search = FindFirstFileW((sessions + L"\\*.session").c_str(), &found);
  if (search != INVALID_HANDLE_VALUE) {
    do {
      const DWORD pid = wcstoul(found.cFileName, nullptr, 10);
      const std::wstring path = sessions + L"\\" + found.cFileName;
      std::string text;
      if (pid == GetCurrentProcessId() || !ReadSmallFile(path.c_str(), text)) {
        continue;
      }
      // "<creation time> <key>"
      const size_t space = text.find(' ');
      const unsigned long long created = strtoull(text.c_str(), nullptr, 10);
      if (SessionStillRunning(pid, created)) {
        continue;  // another browser instance with the hooks live
      }
      std::string key = space == std::string::npos ? std::string() : text.substr(space + 1);
      while (!key.empty() && (key.back() == '\n' || key.back() == '\r')) {
        key.pop_back();
      }
      if (key == g_key) {
        ++crashes;
      }
      ARB_WARN("browser process %lu ended abnormally with the hooks installed (%s)", pid,
               key.c_str());
      DeleteFileW(path.c_str());
    } while (FindNextFileW(search, &found));
    FindClose(search);
  }
  WriteCrashCount(crashes);

  CrashGuardState state;
  state.crashes = crashes;
  state.safe_mode = crashes >= kCrashLimit && ReadConfigInt("safeMode", 1) != 0;
  return state;
}

void CrashGuardBeginSession() {
  if (!g_session_path[0]) {
    return;
  }
  char text[220];
  const int size = _snprintf_s(text, sizeof(text), _TRUNCATE, "%llu %s\n",
                               CreationTime(GetCurrentProcess()), g_key);
  if (size > 0) {
    WriteSmallFile(g_session_path, text, static_cast<size_t>(size));
    g_session_active = true;
  }
}

void CrashGuardEndSession() {
  if (!g_session_active) {
    return;
  }
  g_session_active = false;
  DeleteFileW(g_session_path);
  WriteCrashCount(0);
}

void CrashGuardReset() {
  if (g_crash_path[0]) {
    WriteCrashCount(0);
  }
}

}  // namespace arbortab
