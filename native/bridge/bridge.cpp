// arbortab-bridge.exe: the Native Messaging host of the ArborTab extension.
//
// Chrome starts it for chrome.runtime.connectNative('com.arbortab.bridge') and passes the
// caller's origin as the first argument. The bridge finds the browser process that started
// it, connects to that browser's \\.\pipe\arbortab-<pid> (created by version.dll) and copies
// messages both ways. Native Messaging and the pipe use the same framing (32-bit length +
// JSON), so apart from its own first message the bridge never parses anything.
//
// If no ancestor has the pipe, the module is not loaded in this browser: the bridge says so
// in one message and exits, and the extension reports "not connected".
#include <windows.h>

#include <string>
#include <thread>

#include "link/pipe_io.h"

namespace {

using arbortab::kPipePrefix;

// bridge -> cmd.exe -> chrome.exe (browser) is the usual chain.
constexpr int kMaxBrowserDepth = 4;

std::string Utf8(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                       nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size,
                      nullptr, nullptr);
  return out;
}

// Only characters an extension origin can contain, so the JSON below needs no escaping.
std::string SafeOrigin(const std::string& origin) {
  for (const char ch : origin) {
    if (!isalnum(static_cast<unsigned char>(ch)) && ch != ':' && ch != '/' && ch != '-' &&
        ch != '.') {
      return {};
    }
  }
  return origin;
}

HANDLE OpenBrowserPipe(DWORD* browser_pid) {
  for (const DWORD pid : arbortab::ProcessAncestors(GetCurrentProcessId(), kMaxBrowserDepth)) {
    const std::wstring name = kPipePrefix + std::to_wstring(pid);
    for (int attempt = 0; attempt < 3; ++attempt) {
      HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
      if (pipe != INVALID_HANDLE_VALUE) {
        *browser_pid = pid;
        return pipe;
      }
      if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(name.c_str(), 2000)) {
        break;
      }
    }
  }
  return INVALID_HANDLE_VALUE;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  const std::string origin = SafeOrigin(argc > 1 ? Utf8(argv[1]) : std::string());

  DWORD browser = 0;
  HANDLE pipe = OpenBrowserPipe(&browser);
  if (pipe == INVALID_HANDLE_VALUE) {
    arbortab::WriteMessage(out, R"({"type":"bridge.unavailable","reason":"module-not-loaded"})", false);
    return 0;
  }
  const std::string hello = std::string(R"({"type":"bridge.hello","bridgeVersion":")") +
                            ARBORTAB_VERSION + R"(","origin":")" + origin + R"("})";
  if (!arbortab::WriteMessage(pipe, hello, true)) {
    return 1;
  }

  // Extension -> module. Chrome closes stdin when the port disconnects.
  std::thread([in, pipe] {
    std::string message;
    while (arbortab::ReadMessage(in, message, false) && arbortab::WriteMessage(pipe, message, true)) {
    }
    ExitProcess(0);
  }).detach();

  // Module -> extension. The pipe breaks when the browser (and the module) goes away.
  std::string message;
  while (arbortab::ReadMessage(pipe, message, true) && arbortab::WriteMessage(out, message, false)) {
  }
  ExitProcess(0);
}
