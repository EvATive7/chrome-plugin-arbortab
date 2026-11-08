#include "link/pipe_server.h"

#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>

#include "core/log.h"
#include "json.hpp"
#include "link/pipe_io.h"
#include "link/server.h"

#pragma comment(lib, "advapi32.lib")

namespace arbortab {
namespace {

// The bridge runs as a child of this browser, usually through cmd.exe (Chrome starts
// Native Messaging hosts with `cmd /c`): allow a few levels.
constexpr int kMaxBridgeDepth = 4;

std::wstring g_pipe_name;
SECURITY_ATTRIBUTES g_security{};
PSECURITY_DESCRIPTOR g_descriptor = nullptr;

class PipeClient : public LinkClient {
 public:
  explicit PipeClient(HANDLE pipe) : pipe_(pipe) {}
  ~PipeClient() override { CloseHandle(pipe_); }

  bool Send(const std::string& json) override {
    std::lock_guard<std::mutex> lock(write_mutex_);
    return WriteMessage(pipe_, json, true);
  }

  HANDLE pipe() const { return pipe_; }

 private:
  HANDLE pipe_;
  std::mutex write_mutex_;
};

// Only the current user (and SYSTEM) may open the pipe.
bool BuildSecurity() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return false;
  }
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::unique_ptr<BYTE[]> buffer(new BYTE[size]);
  wchar_t* sid = nullptr;
  const bool ok = GetTokenInformation(token, TokenUser, buffer.get(), size, &size) &&
                  ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.get())->User.Sid, &sid);
  CloseHandle(token);
  if (!ok) {
    return false;
  }
  const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")(A;;GA;;;SY)";
  LocalFree(sid);
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                            &g_descriptor, nullptr)) {
    return false;
  }
  g_security.nLength = sizeof(g_security);
  g_security.lpSecurityDescriptor = g_descriptor;
  g_security.bInheritHandle = FALSE;
  return true;
}

HANDLE CreateInstance(bool first) {
  return CreateNamedPipeW(
      g_pipe_name.c_str(),
      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &g_security);
}

// Waits for a client on an overlapped pipe instance.
bool WaitForClient(HANDLE pipe) {
  OVERLAPPED operation{};
  operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!operation.hEvent) {
    return false;
  }
  bool connected = ConnectNamedPipe(pipe, &operation) != FALSE;
  if (!connected) {
    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) {
      connected = true;
    } else if (error == ERROR_IO_PENDING) {
      DWORD ignored = 0;
      connected = GetOverlappedResult(pipe, &operation, &ignored, TRUE) != FALSE;
    }
  }
  CloseHandle(operation.hEvent);
  return connected;
}

void ServeClient(std::shared_ptr<PipeClient> client, DWORD client_pid) {
  // The bridge introduces itself with the extension origin Chrome gave it.
  std::string text;
  if (!ReadMessage(client->pipe(), text, true)) {
    return;
  }
  const nlohmann::json hello = nlohmann::json::parse(text, nullptr, false);
  if (!hello.is_object() || hello.value("type", std::string()) != "bridge.hello") {
    ARB_WARN("pipe client %lu did not introduce itself; closing", client_pid);
    return;
  }
  client->origin = hello.value("origin", std::string());
  if (!client->origin.empty() && client->origin.back() == '/') {
    client->origin.pop_back();
  }
  if (!ExtensionAllowed(client->origin)) {
    ARB_WARN("rejected bridge for origin '%s'", client->origin.c_str());
    return;
  }
  ARB_INFO("bridge %s (pid %lu) connected", hello.value("bridgeVersion", std::string("?")).c_str(),
           client_pid);
  AddClient(client);
  while (ReadMessage(client->pipe(), text, true)) {
    HandleClientMessage(*client, text);
  }
  RemoveClient(client->id);
}

void AcceptLoop(HANDLE pipe) {
  const DWORD self = GetCurrentProcessId();
  for (;;) {
    if (WaitForClient(pipe)) {
      ULONG client_pid = 0;
      GetNamedPipeClientProcessId(pipe, &client_pid);
      const std::vector<DWORD> ancestors = ProcessAncestors(client_pid, kMaxBridgeDepth);
      if (std::find(ancestors.begin(), ancestors.end(), self) != ancestors.end()) {
        auto client = std::make_shared<PipeClient>(pipe);
        client->transport = "pipe";
        std::thread(&ServeClient, client, client_pid).detach();
      } else {
        // Only processes this browser started (the bridge) may talk to it.
        ARB_WARN("rejected pipe client %lu: not started by this browser", client_pid);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
      }
    } else {
      CloseHandle(pipe);
    }
    pipe = CreateInstance(false);
    if (pipe == INVALID_HANDLE_VALUE) {
      ARB_ERROR("cannot create another pipe instance: %lu", GetLastError());
      return;
    }
  }
}

}  // namespace

std::string PipeName() {
  std::string name;
  for (const wchar_t ch : g_pipe_name) {
    name += static_cast<char>(ch);  // ASCII only: prefix + digits
  }
  return name;
}

bool StartPipeServer() {
  g_pipe_name = std::wstring(kPipePrefix) + std::to_wstring(GetCurrentProcessId());
  if (!BuildSecurity()) {
    ARB_ERROR("cannot build the pipe security descriptor: %lu", GetLastError());
    return false;
  }
  // FILE_FLAG_FIRST_PIPE_INSTANCE: fail instead of joining a pipe someone else created.
  HANDLE pipe = CreateInstance(true);
  if (pipe == INVALID_HANDLE_VALUE) {
    ARB_ERROR("cannot create %ls: %lu", g_pipe_name.c_str(), GetLastError());
    return false;
  }
  std::thread(&AcceptLoop, pipe).detach();
  ARB_INFO("extension endpoint ready: %ls", g_pipe_name.c_str());
  return true;
}

}  // namespace arbortab
