// Rolling text log for the ArborTab native module.
#pragma once

#include <string>

namespace arbortab {

enum class LogLevel : int { kError = 0, kWarn = 1, kInfo = 2, kDebug = 3 };

// Prepares %LOCALAPPDATA%\ArborTab\logs\native.log. Safe to call from any thread.
void LogInit(LogLevel level);
void LogShutdown();
void LogSetLevel(LogLevel level);
LogLevel LogGetLevel();
void LogWrite(LogLevel level, const char* file, int line, const char* format, ...);

}  // namespace arbortab

#define ARB_LOGF(level, ...) \
  ::arbortab::LogWrite(level, __FILE__, __LINE__, __VA_ARGS__)
#define ARB_ERROR(...) ARB_LOGF(::arbortab::LogLevel::kError, __VA_ARGS__)
#define ARB_WARN(...) ARB_LOGF(::arbortab::LogLevel::kWarn, __VA_ARGS__)
#define ARB_INFO(...) ARB_LOGF(::arbortab::LogLevel::kInfo, __VA_ARGS__)
#define ARB_DEBUG(...) ARB_LOGF(::arbortab::LogLevel::kDebug, __VA_ARGS__)
