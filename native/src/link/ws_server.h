// Minimal WebSocket server (RFC 6455) used to talk to the ArborTab extension:
// one text frame per message, no extensions, no compression.
#pragma once

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
};

// Starts the listener on 127.0.0.1:47821..47825 and returns the bound port (0 on failure).
// It starts as soon as chrome.dll is known, before offsets exist, so the extension can tell
// "not installed" from "waiting for offsets".
int StartWebSocketServer(const std::string& native_version);
// Replaces the reported status and pushes a `status` message to every client.
void UpdateServerStatus(const ServerStatus& status);
void StopWebSocketServer();

// Sends one JSON text frame to every connected client.
void BroadcastJson(const std::string& json);

// Diagnostics.
int WebSocketPort();
size_t WebSocketClientCount();
std::string WebSocketClientOrigins();

}  // namespace arbortab
