#include "link/pipe_io.h"

#include <tlhelp32.h>

#include <map>

namespace arbortab {
namespace {

// One transfer, waiting for completion; returns the byte count, 0 on failure or EOF.
DWORD Transfer(HANDLE handle, void* data, DWORD size, bool overlapped, bool write) {
  DWORD done = 0;
  if (!overlapped) {
    const BOOL ok = write ? WriteFile(handle, data, size, &done, nullptr)
                          : ReadFile(handle, data, size, &done, nullptr);
    return ok ? done : 0;
  }
  OVERLAPPED operation{};
  operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!operation.hEvent) {
    return 0;
  }
  BOOL ok = write ? WriteFile(handle, data, size, nullptr, &operation)
                  : ReadFile(handle, data, size, nullptr, &operation);
  if (ok || GetLastError() == ERROR_IO_PENDING) {
    ok = GetOverlappedResult(handle, &operation, &done, TRUE);
  }
  CloseHandle(operation.hEvent);
  return ok ? done : 0;
}

unsigned long long CreationTime(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) {
    return 0;
  }
  FILETIME created{}, exited{}, kernel{}, user{};
  unsigned long long value = 0;
  if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
    value = (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  }
  CloseHandle(process);
  return value;
}

}  // namespace

bool ReadExact(HANDLE handle, void* data, DWORD size, bool overlapped) {
  auto* bytes = static_cast<char*>(data);
  while (size > 0) {
    const DWORD got = Transfer(handle, bytes, size, overlapped, false);
    if (got == 0) {
      return false;
    }
    bytes += got;
    size -= got;
  }
  return true;
}

bool WriteAll(HANDLE handle, const void* data, DWORD size, bool overlapped) {
  auto* bytes = static_cast<char*>(const_cast<void*>(data));
  while (size > 0) {
    const DWORD put = Transfer(handle, bytes, size, overlapped, true);
    if (put == 0) {
      return false;
    }
    bytes += put;
    size -= put;
  }
  return true;
}

bool ReadMessage(HANDLE handle, std::string& message, bool overlapped) {
  uint32_t size = 0;
  if (!ReadExact(handle, &size, sizeof(size), overlapped) || size > kMaxPipeMessage) {
    return false;
  }
  message.assign(size, '\0');
  return size == 0 || ReadExact(handle, message.data(), size, overlapped);
}

bool WriteMessage(HANDLE handle, const std::string& message, bool overlapped) {
  if (message.size() > kMaxPipeMessage) {
    return false;
  }
  // One write per message, so concurrent writers (guarded by the caller) never interleave.
  std::string frame(sizeof(uint32_t), '\0');
  const auto size = static_cast<uint32_t>(message.size());
  memcpy(frame.data(), &size, sizeof(size));
  frame += message;
  return WriteAll(handle, frame.data(), static_cast<DWORD>(frame.size()), overlapped);
}

std::vector<DWORD> ProcessAncestors(DWORD pid, int depth) {
  std::map<DWORD, DWORD> parents;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot != INVALID_HANDLE_VALUE) {
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
      parents[entry.th32ProcessID] = entry.th32ParentProcessID;
    }
    CloseHandle(snapshot);
  }
  std::vector<DWORD> chain;
  unsigned long long child_created = CreationTime(pid);
  for (DWORD current = pid; static_cast<int>(chain.size()) < depth;) {
    const auto found = parents.find(current);
    if (found == parents.end() || found->second == 0) {
      break;
    }
    const DWORD parent = found->second;
    const unsigned long long parent_created = CreationTime(parent);
    // A parent created after its child is a newer process that reused the pid.
    if (parent_created == 0 || (child_created != 0 && parent_created > child_created)) {
      break;
    }
    chain.push_back(parent);
    current = parent;
    child_created = parent_created;
  }
  return chain;
}

}  // namespace arbortab
