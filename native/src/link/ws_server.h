// Minimal WebSocket server (RFC 6455): one text frame per message, no extensions, no
// compression. The fallback transport of link/server.h; also used by tools/dev and tools/test.
#pragma once

namespace arbortab {

// Starts the listener on 127.0.0.1:47821..47825 and returns the bound port (0 on failure).
int StartWebSocketServer();

// Bound port, 0 when not listening.
int WebSocketPort();

}  // namespace arbortab
