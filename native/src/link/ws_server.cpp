#include "link/ws_server.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <memory>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "core/log.h"
#include "core/paths.h"
#include "core/state.h"
#include "core/ui_thread.h"
#include "features/layout.h"
#include "json.hpp"

#pragma comment(lib, "ws2_32.lib")

namespace arbortab {
namespace {

constexpr int kPortFirst = 47821;
constexpr int kPortLast = 47825;
constexpr size_t kMaxMessageBytes = 1024 * 1024;

// ---------------------------------------------------------------------------------------
// SHA-1 (only needed for the handshake accept key)
// ---------------------------------------------------------------------------------------
struct Sha1 {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  uint64_t length = 0;
  uint8_t buffer[64] = {0};
  size_t buffered = 0;

  static uint32_t Rotate(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
  }

  void Block(const uint8_t* block) {
    uint32_t w[80];
    for (int index = 0; index < 16; ++index) {
      w[index] = (block[index * 4] << 24) | (block[index * 4 + 1] << 16) |
                 (block[index * 4 + 2] << 8) | block[index * 4 + 3];
    }
    for (int index = 16; index < 80; ++index) {
      w[index] = Rotate(w[index - 3] ^ w[index - 8] ^ w[index - 14] ^ w[index - 16], 1);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int index = 0; index < 80; ++index) {
      uint32_t f = 0, k = 0;
      if (index < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      } else if (index < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (index < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const uint32_t temp = Rotate(a, 5) + f + e + k + w[index];
      e = d;
      d = c;
      c = Rotate(b, 30);
      b = a;
      a = temp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }

  void Update(const uint8_t* data, size_t size) {
    length += size;
    while (size > 0) {
      const size_t take = (size < 64 - buffered) ? size : 64 - buffered;
      memcpy(buffer + buffered, data, take);
      buffered += take;
      data += take;
      size -= take;
      if (buffered == 64) {
        Block(buffer);
        buffered = 0;
      }
    }
  }

  void Finish(uint8_t out[20]) {
    const uint64_t bit_length = length * 8;
    uint8_t pad = 0x80;
    Update(&pad, 1);
    uint8_t zero = 0;
    while (buffered != 56) {
      Update(&zero, 1);
    }
    uint8_t size_bytes[8];
    for (int index = 0; index < 8; ++index) {
      size_bytes[index] = static_cast<uint8_t>(bit_length >> (56 - index * 8));
    }
    Update(size_bytes, 8);
    for (int index = 0; index < 5; ++index) {
      out[index * 4] = static_cast<uint8_t>(h[index] >> 24);
      out[index * 4 + 1] = static_cast<uint8_t>(h[index] >> 16);
      out[index * 4 + 2] = static_cast<uint8_t>(h[index] >> 8);
      out[index * 4 + 3] = static_cast<uint8_t>(h[index]);
    }
  }
};

std::string Base64(const uint8_t* data, size_t size) {
  static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  for (size_t index = 0; index < size; index += 3) {
    const uint32_t triple = (static_cast<uint32_t>(data[index]) << 16) |
                            ((index + 1 < size ? data[index + 1] : 0) << 8) |
                            (index + 2 < size ? data[index + 2] : 0);
    out += table[(triple >> 18) & 0x3F];
    out += table[(triple >> 12) & 0x3F];
    out += index + 1 < size ? table[(triple >> 6) & 0x3F] : '=';
    out += index + 2 < size ? table[triple & 0x3F] : '=';
  }
  return out;
}

std::string HandshakeAccept(const std::string& key) {
  const std::string magic = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  Sha1 sha;
  sha.Update(reinterpret_cast<const uint8_t*>(magic.data()), magic.size());
  uint8_t digest[20];
  sha.Finish(digest);
  return Base64(digest, sizeof(digest));
}

// ---------------------------------------------------------------------------------------
// Allowed origins
// ---------------------------------------------------------------------------------------
std::vector<std::string> AllowedExtensionIds() {
  std::vector<std::string> ids;
  const std::string configured = ReadConfigString("extensions", "");
  size_t start = 0;
  while (start <= configured.size()) {
    const size_t comma = configured.find(',', start);
    std::string item = configured.substr(start, comma == std::string::npos ? std::string::npos
                                                                           : comma - start);
    while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) {
      item.erase(item.begin());
    }
    while (!item.empty() && (item.back() == ' ' || item.back() == '\r')) {
      item.pop_back();
    }
    if (!item.empty()) {
      ids.push_back(item);
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return ids;
}

bool OriginAllowed(const std::string& origin, std::string* extension_id) {
  const std::string prefix = "chrome-extension://";
  if (origin.rfind(prefix, 0) != 0) {
    return false;
  }
  const std::string id = origin.substr(prefix.size());
  if (extension_id) {
    *extension_id = id;
  }
  const std::vector<std::string> allowed = AllowedExtensionIds();
  if (allowed.empty()) {
    // Until config.ini lists IDs we accept any unpacked/store extension: the listener is bound
    // to loopback and only reachable from a local Chromium instance.
    return true;
  }
  for (const std::string& candidate : allowed) {
    if (candidate == id) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------------------
// Connections
// ---------------------------------------------------------------------------------------
struct Client {
  SOCKET socket = INVALID_SOCKET;
  std::string origin;
  int id = 0;
  std::mutex write_mutex;
};

ServerFeatures g_features;
std::string g_native_version;
int g_port = 0;
SOCKET g_listen = INVALID_SOCKET;
// Detached on purpose: a joinable std::thread with static storage duration aborts the process
// from its destructor while the DLL is being unloaded.
std::atomic<bool> g_running{false};
std::mutex g_clients_mutex;
std::map<int, std::shared_ptr<Client>> g_clients;
std::atomic<int> g_next_client{1};

bool SendAll(SOCKET socket, const uint8_t* data, size_t size) {
  size_t sent = 0;
  while (sent < size) {
    const int written = send(socket, reinterpret_cast<const char*>(data + sent),
                             static_cast<int>(size - sent), 0);
    if (written <= 0) {
      return false;
    }
    sent += static_cast<size_t>(written);
  }
  return true;
}

bool SendFrame(Client& client, uint8_t opcode, const std::string& payload) {
  std::vector<uint8_t> frame;
  frame.push_back(0x80 | opcode);
  const size_t size = payload.size();
  if (size < 126) {
    frame.push_back(static_cast<uint8_t>(size));
  } else if (size <= 0xFFFF) {
    frame.push_back(126);
    frame.push_back(static_cast<uint8_t>(size >> 8));
    frame.push_back(static_cast<uint8_t>(size));
  } else {
    frame.push_back(127);
    for (int index = 7; index >= 0; --index) {
      frame.push_back(static_cast<uint8_t>(static_cast<uint64_t>(size) >> (index * 8)));
    }
  }
  frame.insert(frame.end(), payload.begin(), payload.end());
  std::lock_guard<std::mutex> lock(client.write_mutex);
  return SendAll(client.socket, frame.data(), frame.size());
}

bool ReadAll(SOCKET socket, uint8_t* data, size_t size) {
  size_t read = 0;
  while (read < size) {
    const int got = recv(socket, reinterpret_cast<char*>(data + read),
                         static_cast<int>(size - read), 0);
    if (got <= 0) {
      return false;
    }
    read += static_cast<size_t>(got);
  }
  return true;
}

std::string NativeHelloJson() {
  nlohmann::json hello;
  hello["type"] = "hello";
  hello["protocol"] = 1;
  hello["nativeVersion"] = g_native_version;
  hello["features"] = {{"layout", g_features.layout},
                       {"twisty", g_features.twisty},
                       {"paint", g_features.paint},
                       {"drag", g_features.drag}};
  return hello.dump();
}

void HandleMessageUnsafe(Client& client, const std::string& text) {
  nlohmann::json message;
  try {
    message = nlohmann::json::parse(text);
  } catch (const std::exception& exception) {
    ARB_WARN("ignoring malformed client message: %s", exception.what());
    return;
  }
  const std::string type = message.value("type", std::string());
  if (type == "hello") {
    const std::string version = message.value("extVersion", std::string("?"));
    ARB_INFO("extension %s connected (origin %s)", version.c_str(), client.origin.c_str());
    SendFrame(client, 0x1, NativeHelloJson());
    return;
  }
  if (type == "ping") {
    SendFrame(client, 0x1, R"({"type":"pong"})");
    return;
  }
  if (type == "state") {
    std::vector<int> tabs;
    if (message.contains("windowTabs") && message["windowTabs"].is_array()) {
      for (const auto& value : message["windowTabs"]) {
        if (value.is_number()) {
          tabs.push_back(value.get<int>());
        }
      }
    }
    std::map<int, TabDisplayState> states;
    if (message.contains("tabs") && message["tabs"].is_object()) {
      for (const auto& [key, node] : message["tabs"].items()) {
        TabDisplayState state;
        if (node.is_object()) {
          state.depth = node.value("depth", 0);
          state.hidden = node.value("hidden", false);
          state.has_children = node.value("hasChildren", false);
          state.collapsed = node.value("collapsed", false);
          state.hidden_count = node.value("hiddenCount", 0);
        }
        try {
          states[std::stoi(key)] = state;
        } catch (...) {
          continue;
        }
      }
    }
    DisplayConfig config;
    if (message.contains("config") && message["config"].is_object()) {
      const auto& node = message["config"];
      config.indent = node.value("indent", config.indent);
      config.show_count = node.value("showCount", config.show_count);
      const std::string color = node.value("highlightColor", std::string());
      if (color.size() == 9 && color[0] == '#') {
        try {
          config.highlight_color = static_cast<uint32_t>(std::stoul(color.substr(1), nullptr, 16));
        } catch (...) {
          // keep the default colour
        }
      }
    }
    if (GlobalState().ReplaceAll(client.id, tabs, states, config)) {
      ARB_DEBUG("state update from client %d: %zu tabs (%s)", client.id, states.size(),
                GlobalState().Describe().c_str());
      UiThreadRequestRefresh();
    }
    return;
  }
  if (type == "debug.layout") {
    // Force a layout pass so the snapshot describes the current state, not a stale one.
    UiThreadRequestRefresh();
    Sleep(600);
    nlohmann::json reply;
    reply["type"] = "debug.layout";
    reply["clients"] = static_cast<int>(GlobalState().ClientCount());
    reply["stats"] = {{"layoutCalls", LayoutCallCount()},
                      {"layoutRewrites", LayoutRewriteCount()},
                      {"tabIdFailures", LayoutTabIdFailures()},
                      {"uiRefreshes", UiThreadRefreshCount()},
                      {"horizontalSkips", LayoutHorizontalSkips()},
                      {"emptySkips", LayoutEmptySkips()},
                      {"maxChildren", LayoutMaxChildren()}};
    reply["layouts"] = nlohmann::json::parse(DescribeLastLayoutJson(), nullptr, false);
    SendFrame(client, 0x1, reply.dump());
    return;
  }
  ARB_DEBUG("unhandled client message type '%s'", type.c_str());
}

// Any exception here would terminate the browser process, so the whole message handler is
// guarded.
void HandleMessage(Client& client, const std::string& text) {
  try {
    HandleMessageUnsafe(client, text);
  } catch (const std::exception& exception) {
    ARB_ERROR("message handling failed: %s", exception.what());
  } catch (...) {
    ARB_ERROR("message handling failed with an unknown exception");
  }
}

void ClientLoop(std::shared_ptr<Client> client) {
  uint8_t header[2];
  while (g_running) {
    if (!ReadAll(client->socket, header, 2)) {
      break;
    }
    const bool fin = (header[0] & 0x80) != 0;
    const uint8_t opcode = header[0] & 0x0F;
    const bool masked = (header[1] & 0x80) != 0;
    uint64_t payload_size = header[1] & 0x7F;
    if (payload_size == 126) {
      uint8_t extended[2];
      if (!ReadAll(client->socket, extended, 2)) {
        break;
      }
      payload_size = (static_cast<uint64_t>(extended[0]) << 8) | extended[1];
    } else if (payload_size == 127) {
      uint8_t extended[8];
      if (!ReadAll(client->socket, extended, 8)) {
        break;
      }
      payload_size = 0;
      for (int index = 0; index < 8; ++index) {
        payload_size = (payload_size << 8) | extended[index];
      }
    }
    if (payload_size > kMaxMessageBytes) {
      ARB_WARN("dropping oversized frame (%llu bytes)", static_cast<unsigned long long>(payload_size));
      break;
    }
    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked && !ReadAll(client->socket, mask, 4)) {
      break;
    }
    std::string payload(static_cast<size_t>(payload_size), '\0');
    if (payload_size && !ReadAll(client->socket, reinterpret_cast<uint8_t*>(payload.data()),
                                static_cast<size_t>(payload_size))) {
      break;
    }
    if (masked) {
      for (size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<char>(payload[index] ^ mask[index % 4]);
      }
    }
    (void)fin;  // only single-frame messages are expected
    if (opcode == 0x8) {  // close
      break;
    }
    if (opcode == 0x9) {  // ping
      SendFrame(*client, 0xA, payload);
      continue;
    }
    if (opcode == 0xA) {  // pong
      continue;
    }
    if (opcode == 0x1) {
      try {
        HandleMessage(*client, payload);
      } catch (...) {
        ARB_ERROR("unhandled failure while processing a client message");
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    g_clients.erase(client->id);
  }
  GlobalState().RemoveClient(client->id);
  UiThreadRequestRefresh();
  closesocket(client->socket);
  ARB_INFO("client %d disconnected (origin %s)", client->id, client->origin.c_str());
}

void RejectSocket(SOCKET socket, const char* status) {
  const std::string response = std::string("HTTP/1.1 ") + status +
                               "\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
  SendAll(socket, reinterpret_cast<const uint8_t*>(response.data()), response.size());
  closesocket(socket);
}

void HandleConnection(SOCKET socket) {
  std::string request;
  char buffer[2048];
  while (request.find("\r\n\r\n") == std::string::npos) {
    const int got = recv(socket, buffer, sizeof(buffer), 0);
    if (got <= 0) {
      closesocket(socket);
      return;
    }
    request.append(buffer, static_cast<size_t>(got));
    if (request.size() > 16384) {
      RejectSocket(socket, "431 Request Header Fields Too Large");
      return;
    }
  }

  std::map<std::string, std::string> headers;
  {
    size_t line_start = request.find("\r\n") + 2;
    while (line_start < request.size()) {
      const size_t line_end = request.find("\r\n", line_start);
      if (line_end == std::string::npos) {
        break;
      }
      const std::string line = request.substr(line_start, line_end - line_start);
      const size_t colon = line.find(':');
      if (colon != std::string::npos) {
        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        for (char& ch : name) {
          ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        }
        const size_t first = value.find_first_not_of(' ');
        value = first == std::string::npos ? std::string() : value.substr(first);
        headers[name] = value;
      }
      line_start = line_end + 2;
    }
  }

  std::string extension_id;
  if (!OriginAllowed(headers["origin"], &extension_id)) {
    ARB_WARN("rejected connection from origin '%s'", headers["origin"].c_str());
    RejectSocket(socket, "403 Forbidden");
    return;
  }
  const std::string key = headers["sec-websocket-key"];
  if (key.empty() || headers["upgrade"].find("websocket") == std::string::npos) {
    RejectSocket(socket, "400 Bad Request");
    return;
  }

  const std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                               "Upgrade: websocket\r\n"
                               "Connection: Upgrade\r\n"
                               "Sec-WebSocket-Accept: " +
                               HandshakeAccept(key) + "\r\n\r\n";
  if (!SendAll(socket, reinterpret_cast<const uint8_t*>(response.data()), response.size())) {
    closesocket(socket);
    return;
  }

  auto client = std::make_shared<Client>();
  client->socket = socket;
  client->origin = headers["origin"];
  client->id = g_next_client++;
  {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    g_clients[client->id] = client;
  }
  ARB_INFO("client %d accepted (origin %s)", client->id, client->origin.c_str());
  SendFrame(*client, 0x1, NativeHelloJson());
  std::thread(&ClientLoop, client).detach();
}

void AcceptLoop() {
  while (g_running) {
    sockaddr_in address{};
    int length = sizeof(address);
    SOCKET socket = accept(g_listen, reinterpret_cast<sockaddr*>(&address), &length);
    if (socket == INVALID_SOCKET) {
      if (!g_running) {
        break;
      }
      Sleep(50);
      continue;
    }
    HandleConnection(socket);
  }
}

}  // namespace

int StartWebSocketServer(const ServerFeatures& features, const std::string& native_version) {
  if (g_running) {
    return g_port;
  }
  g_features = features;
  g_native_version = native_version;

  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    ARB_ERROR("WSAStartup failed");
    return 0;
  }

  for (int port = kPortFirst; port <= kPortLast; ++port) {
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
      continue;
    }
    const BOOL exclusive = TRUE;
    setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
               sizeof(exclusive));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port));
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(socket, 8) != 0) {
      closesocket(socket);
      continue;
    }
    g_listen = socket;
    g_port = port;
    break;
  }
  if (g_listen == INVALID_SOCKET) {
    ARB_ERROR("cannot bind any port in %d..%d", kPortFirst, kPortLast);
    WSACleanup();
    return 0;
  }

  g_running = true;
  std::thread(&AcceptLoop).detach();
  ARB_INFO("websocket server listening on 127.0.0.1:%d", g_port);
  return g_port;
}

void StopWebSocketServer() {
  if (!g_running) {
    return;
  }
  g_running = false;
  if (g_listen != INVALID_SOCKET) {
    closesocket(g_listen);
    g_listen = INVALID_SOCKET;
  }
  std::map<int, std::shared_ptr<Client>> clients;
  {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    clients.swap(g_clients);
  }
  for (auto& [id, client] : clients) {
    closesocket(client->socket);
  }
  WSACleanup();
}

void BroadcastJson(const std::string& json) {
  std::vector<std::shared_ptr<Client>> clients;
  {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    for (const auto& [id, client] : g_clients) {
      clients.push_back(client);
    }
  }
  for (auto& client : clients) {
    SendFrame(*client, 0x1, json);
  }
}

int WebSocketPort() { return g_port; }

size_t WebSocketClientCount() {
  std::lock_guard<std::mutex> lock(g_clients_mutex);
  return g_clients.size();
}

std::string WebSocketClientOrigins() {
  std::lock_guard<std::mutex> lock(g_clients_mutex);
  std::string text;
  for (const auto& [id, client] : g_clients) {
    if (!text.empty()) {
      text += ", ";
    }
    text += client->origin;
  }
  return text;
}

}  // namespace arbortab
