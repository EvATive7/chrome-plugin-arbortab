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

// Starts the listener on 127.0.0.1:47821..47825 and returns the bound port (0 on failure).
int StartWebSocketServer(const ServerFeatures& features, const std::string& native_version);
void StopWebSocketServer();

// Sends one JSON text frame to every connected client.
void BroadcastJson(const std::string& json);

// Diagnostics.
int WebSocketPort();
size_t WebSocketClientCount();
std::string WebSocketClientOrigins();

}  // namespace arbortab
