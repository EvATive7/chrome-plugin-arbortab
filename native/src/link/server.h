// Transport-independent half of the extension link: client registry, status reporting and
// message handling. Two transports feed it:
//   - pipe_server: \\.\pipe\arbortab-<browser pid>, reached through the Native Messaging
//     bridge (arbortab-bridge.exe) that this browser starts for the extension;
//   - ws_server: ws://127.0.0.1:47821-47825, the older transport, kept as a fallback and for
//     the development tools.
// Both carry the same JSON messages (see AGENTS.md, "通信协议").
#pragma once

#include <memory>
#include <mutex>
#include <string>

namespace arbortab {

struct ServerFeatures {
  bool layout = false;
  bool twisty = false;
  bool paint = false;
  bool drag = false;
};

// What the module can currently do, reported in `hello` and in `status` messages.
struct ServerStatus {
  ServerFeatures features;
  std::string offsets = "resolving";  // OffsetsStatusName(): resolving|waiting|ready|disabled|error
  std::string chrome_version;
  std::string pdb_key;
  std::string offsets_source;  // "cache" or "download" once ready
  // Crash guard (core/crash_guard.h): hooks kept off after repeated abnormal exits.
  bool safe_mode = false;
  int crashes = 0;
};

// One connected extension, whatever the transport.
class LinkClient {
 public:
  virtual ~LinkClient() = default;
  // Sends one JSON message; false when the connection is gone.
  virtual bool Send(const std::string& json) = 0;

  int id = 0;
  std::string transport;  // "pipe" or "websocket"
  std::string origin;     // chrome-extension://<id>
};

// Starts every transport. Returns false when none could start.
bool StartLinkServers(const std::string& native_version, const std::string& build_stamp);

// Replaces the reported status and pushes a `status` message to every client.
void UpdateServerStatus(const ServerStatus& status);

// Called on `safeMode.reset` from the extension (the user chose to try the hooks again).
using SafeModeResetHandler = void (*)();
void SetSafeModeResetHandler(SafeModeResetHandler handler);

// Sends one JSON message to every connected client.
void BroadcastJson(const std::string& json);

// --- for the transports ---------------------------------------------------------------

// config.ini `extensions` (comma-separated IDs); an empty list allows any extension.
bool ExtensionAllowed(const std::string& origin);
// Registers a connected client, assigns its id and sends it the initial `hello`.
void AddClient(const std::shared_ptr<LinkClient>& client);
// Forgets a client and drops the tab state it pushed.
void RemoveClient(int id);
// Handles one text message from a client. Never throws.
void HandleClientMessage(LinkClient& client, const std::string& text);

}  // namespace arbortab
