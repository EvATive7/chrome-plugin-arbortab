// Named-pipe transport of link/server.h: \\.\pipe\arbortab-<pid of this browser process>.
//
// The extension reaches it through Native Messaging: Chrome starts arbortab-bridge.exe
// (native/bridge) for the extension, and the bridge connects to the pipe named after the
// browser process it was started by. That pairs each extension with its own browser instance,
// needs no port, and only Chrome can start the bridge for an allowed extension
// (allowed_origins in the host manifest).
//
// Framing on the pipe equals Native Messaging's: a 32-bit little-endian length, then UTF-8
// JSON. The bridge's first message is {"type":"bridge.hello","origin":"chrome-extension://<id>/"}.
#pragma once

#include <string>

namespace arbortab {

// "\\.\pipe\arbortab-<pid>"
std::string PipeName();

// Creates the first pipe instance (failing if another process already owns the name) and
// starts the accept thread. Returns false when the pipe could not be created.
bool StartPipeServer();

}  // namespace arbortab
