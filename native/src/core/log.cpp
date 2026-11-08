#include "core/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

#include "core/paths.h"

namespace arbortab {
namespace {

constexpr long long kMaxLogBytes = 1024 * 1024;
constexpr int kMaxLogFiles = 3;

std::mutex g_mutex;
HANDLE g_file = INVALID_HANDLE_VALUE;
LogLevel g_level = LogLevel::kInfo;
bool g_initialized = false;

const char* LevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kError:
      return "E";
    case LogLevel::kWarn:
      return "W";
    case LogLevel::kInfo:
      return "I";
    case LogLevel::kDebug:
      return "D";
  }
  return "?";
}

// Size of the open log, or of the file on disk while none is open.
long long LogSizeLocked() {
  LARGE_INTEGER size{};
  if (g_file != INVALID_HANDLE_VALUE) {
    return GetFileSizeEx(g_file, &size) ? size.QuadPart : 0;
  }
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(LogPath().c_str(), GetFileExInfoStandard, &data)) {
    return 0;
  }
  return (static_cast<long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

// Shifts native.log -> .1 -> .2 ... once it reaches kMaxLogBytes. Checked on open and after
// writes, so a long session with debug logging stays bounded too.
void RotateLocked() {
  if (LogSizeLocked() < kMaxLogBytes) {
    return;
  }
  if (g_file != INVALID_HANDLE_VALUE) {
    CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
  }
  const std::wstring base = LogPath();
  for (int index = kMaxLogFiles - 1; index >= 1; --index) {
    wchar_t from[MAX_PATH];
    wchar_t to[MAX_PATH];
    swprintf_s(from, L"%s.%d", base.c_str(), index);
    swprintf_s(to, L"%s.%d", base.c_str(), index + 1);
    MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING);
  }
  wchar_t first[MAX_PATH];
  swprintf_s(first, L"%s.1", base.c_str());
  MoveFileExW(base.c_str(), first, MOVEFILE_REPLACE_EXISTING);
}

void OpenLocked() {
  if (g_file != INVALID_HANDLE_VALUE) {
    return;
  }
  RotateLocked();
  g_file = CreateFileW(LogPath().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

}  // namespace

void LogInit(LogLevel level) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_level = level;
  g_initialized = true;
  OpenLocked();
}

void LogShutdown() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file != INVALID_HANDLE_VALUE) {
    CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
  }
  g_initialized = false;
}

void LogSetLevel(LogLevel level) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_level = level;
}

LogLevel LogGetLevel() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_level;
}

std::vector<std::string> LogTail(size_t count) {
  constexpr LONGLONG kWindow = 32 * 1024;
  std::vector<std::string> lines;
  HANDLE file = CreateFileW(LogPath().c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return lines;
  }
  LARGE_INTEGER size{};
  GetFileSizeEx(file, &size);
  LARGE_INTEGER start{};
  start.QuadPart = size.QuadPart > kWindow ? size.QuadPart - kWindow : 0;
  SetFilePointerEx(file, start, nullptr, FILE_BEGIN);
  std::string text(static_cast<size_t>(size.QuadPart - start.QuadPart), '\0');
  DWORD read = 0;
  ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
  CloseHandle(file);
  text.resize(read);
  size_t begin = start.QuadPart > 0 ? text.find('\n') + 1 : 0;  // drop the partial first line
  while (begin < text.size()) {
    size_t end = text.find('\n', begin);
    if (end == std::string::npos) {
      end = text.size();
    }
    std::string line = text.substr(begin, end - begin);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(std::move(line));
    begin = end + 1;
  }
  if (lines.size() > count) {
    lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(count));
  }
  return lines;
}

void LogWrite(LogLevel level, const char* file, int line, const char* format, ...) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_initialized || level > g_level) {
    return;
  }
  OpenLocked();
  if (g_file == INVALID_HANDLE_VALUE) {
    return;
  }

  SYSTEMTIME now{};
  GetLocalTime(&now);
  const char* base = file;
  for (const char* p = file; *p; ++p) {
    if (*p == '\\' || *p == '/') {
      base = p + 1;
    }
  }

  char buffer[2048];
  int written = _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] %s:%d ",
                            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                            now.wMilliseconds, LevelName(level), base, line);
  if (written < 0) {
    return;
  }
  va_list args;
  va_start(args, format);
  int appended = _vsnprintf_s(buffer + written, sizeof(buffer) - written, _TRUNCATE, format, args);
  va_end(args);
  if (appended < 0) {
    written = static_cast<int>(sizeof(buffer) - 2);
  } else {
    written += appended;
  }
  buffer[written++] = '\n';

  DWORD bytes = 0;
  WriteFile(g_file, buffer, static_cast<DWORD>(written), &bytes, nullptr);
  // Checking the size costs a system call: do it every 256 lines.
  static unsigned lines = 0;
  if ((++lines & 0xFF) == 0 && LogSizeLocked() >= kMaxLogBytes) {
    RotateLocked();
    OpenLocked();
  }
}

}  // namespace arbortab
