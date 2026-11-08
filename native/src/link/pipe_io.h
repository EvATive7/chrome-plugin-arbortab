// Pipe helpers shared by version.dll (link/pipe_server) and arbortab-bridge.exe.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace arbortab {

// Largest message either side accepts; Native Messaging caps host -> Chrome at 1 MB anyway.
constexpr size_t kMaxPipeMessage = 1024 * 1024;

// Prefix of the per-browser pipe: kPipePrefix + <browser pid>.
constexpr const wchar_t* kPipePrefix = L"\\\\.\\pipe\\arbortab-";

// Blocking read/write of exactly `size` bytes. `overlapped` must match how the handle was
// opened: a synchronous handle serialises a pending read and a write from another thread, so
// both ends open the pipe with FILE_FLAG_OVERLAPPED; the bridge's stdio handles are
// synchronous.
bool ReadExact(HANDLE handle, void* data, DWORD size, bool overlapped);
bool WriteAll(HANDLE handle, const void* data, DWORD size, bool overlapped);

// One message: 32-bit little-endian length, then the payload (Native Messaging framing).
bool ReadMessage(HANDLE handle, std::string& message, bool overlapped);
bool WriteMessage(HANDLE handle, const std::string& message, bool overlapped);

// `pid`'s parent, grandparent, ... (at most `depth` levels, nearest first). A parent whose
// pid was reused by a younger process ends the chain.
std::vector<DWORD> ProcessAncestors(DWORD pid, int depth);

}  // namespace arbortab
