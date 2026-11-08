// Filesystem locations used by the native module.
#pragma once

#include <string>

namespace arbortab {

// %LOCALAPPDATA%\ArborTab (created on first call).
const std::wstring& DataDir();
// %LOCALAPPDATA%\ArborTab\logs (created on first call).
const std::wstring& LogDir();
// %LOCALAPPDATA%\ArborTab\logs\native.log
const std::wstring& LogPath();
// %LOCALAPPDATA%\ArborTab\offsets\s<schema>\<pdbKey>.json
std::wstring CachedOffsetsPath(int schema, const std::string& pdb_key);
// %LOCALAPPDATA%\ArborTab\config.ini
const std::wstring& ConfigPath();

// Reads a value from config.ini ([arbortab] section). Returns `fallback` when absent.
std::string ReadConfigString(const char* key, const char* fallback);
int ReadConfigInt(const char* key, int fallback);

}  // namespace arbortab
