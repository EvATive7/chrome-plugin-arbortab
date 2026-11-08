#include "link/server.h"

#include <windows.h>

#include <atomic>
#include <map>
#include <vector>

#include "core/crash_guard.h"
#include "core/log.h"
#include "core/offsets.h"
#include "core/paths.h"
#include "core/state.h"
#include "core/ui_thread.h"
#include "features/drag.h"
#include "features/layout.h"
#include "json.hpp"
#include "link/pipe_server.h"
#include "link/protocol.h"
#include "link/ws_server.h"

namespace arbortab {
namespace {

std::mutex g_status_mutex;
ServerStatus g_status;
std::string g_native_version;
std::string g_build_stamp;
SafeModeResetHandler g_safe_mode_reset = nullptr;
std::mutex g_clients_mutex;
std::map<int, std::shared_ptr<LinkClient>> g_clients;
std::atomic<int> g_next_client{1};

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

// `hello` (reply to the extension's hello) and `status` (pushed on change) share one body.
std::string StatusJson(const char* type) {
  ServerStatus status;
  {
    std::lock_guard<std::mutex> lock(g_status_mutex);
    status = g_status;
  }
  nlohmann::json message;
  message["type"] = type;
  message["protocol"] = kProtocolMax;
  message["protocolMin"] = kProtocolMin;
  message["protocolMax"] = kProtocolMax;
  message["nativeVersion"] = g_native_version;
  message["features"] = {{"layout", status.features.layout},
                         {"twisty", status.features.twisty},
                         {"paint", status.features.paint},
                         {"drag", status.features.drag}};
  message["offsets"] = {{"status", status.offsets},
                        {"schema", kOffsetsSchema},
                        {"chromeVersion", status.chrome_version}};
  message["safeMode"] = {{"active", status.safe_mode},
                         {"crashes", status.crashes},
                         {"limit", kCrashLimit}};
  return message.dump();
}

// Everything the extension's debug export needs from this side. No tab content: the tree
// state is summarised by GlobalState().Describe(), which only has ids and depths.
std::string DebugReportJson(const LinkClient& client) {
  ServerStatus status;
  {
    std::lock_guard<std::mutex> lock(g_status_mutex);
    status = g_status;
  }
  nlohmann::json reply;
  reply["type"] = "debug.report";
  reply["nativeVersion"] = g_native_version;
  reply["build"] = g_build_stamp;
  reply["pid"] = GetCurrentProcessId();
  reply["port"] = WebSocketPort();
  reply["pipe"] = PipeName();
  reply["transport"] = client.transport;
  reply["chromeVersion"] = status.chrome_version;
  reply["pdbKey"] = status.pdb_key;
  reply["offsets"] = {{"status", status.offsets}, {"schema", kOffsetsSchema},
                      {"source", status.offsets_source}};
  reply["features"] = {{"layout", status.features.layout},
                       {"twisty", status.features.twisty},
                       {"paint", status.features.paint},
                       {"drag", status.features.drag}};
  reply["safeMode"] = {{"active", status.safe_mode}, {"crashes", status.crashes}};
  reply["config"] = {{"extensions", ReadConfigString("extensions", "")},
                     {"logLevel", ReadConfigString("logLevel", "info")},
                     {"disableDownload", ReadConfigInt("disableDownload", 0)},
                     {"safeMode", ReadConfigInt("safeMode", 1)}};
  reply["stats"] = {{"clients", static_cast<int>(GlobalState().ClientCount())},
                    {"layoutCalls", LayoutCallCount()},
                    {"layoutRewrites", LayoutRewriteCount()},
                    {"tabIdFailures", LayoutTabIdFailures()},
                    {"uiRefreshes", UiThreadRefreshCount()}};
  nlohmann::json log = nlohmann::json::array();
  for (const std::string& line : LogTail(120)) {
    log.push_back(line);
  }
  reply["log"] = log;
  // Log lines may hold paths in the ANSI code page: replace what is not valid UTF-8.
  return reply.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

void HandleMessageUnsafe(LinkClient& client, const std::string& text) {
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
    ARB_INFO("extension %s connected (origin %s, protocol %d)", version.c_str(),
             client.origin.c_str(), message.value("protocol", 0));
    client.Send(StatusJson("hello"));
    return;
  }
  if (type == "debug.report") {
    client.Send(DebugReportJson(client));
    return;
  }
  if (type == "safeMode.reset") {
    ARB_INFO("safe mode reset requested by client %d", client.id);
    if (g_safe_mode_reset) {
      g_safe_mode_reset();
    }
    return;
  }
  if (type == "ping") {
    client.Send(R"({"type":"pong"})");
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
          const std::string title = node.value("title", std::string());
          strncpy_s(state.title.data(), state.title.size(), title.c_str(), _TRUNCATE);
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
      config.show_guides = node.value("showGuides", config.show_guides);
      config.animations = node.value("animations", config.animations);
      // "auto" (or anything that is not #AARRGGBB) follows the browser theme.
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
    // DescribeLastLayoutJson() opens the snapshot window, so a refresh has to happen after
    // it: a refresh before the window opens would be recorded as an empty snapshot.
    UiThreadRequestRefresh();
    Sleep(300);
    (void)DescribeLastLayoutJson();
    UiThreadRequestRefresh();
    Sleep(400);
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
    reply["stats"]["dropTarget"] = CurrentDropTargetTabId();
    reply["stats"]["dropZone"] = CurrentDropZone();
    reply["stats"]["dragging"] = DragInProgress();
    reply["layouts"] = nlohmann::json::parse(DescribeLastLayoutJson(), nullptr, false);
    client.Send(reply.dump());
    return;
  }
  ARB_DEBUG("unhandled client message type '%s'", type.c_str());
}

}  // namespace

bool ExtensionAllowed(const std::string& origin) {
  const std::string prefix = "chrome-extension://";
  if (origin.rfind(prefix, 0) != 0) {
    return false;
  }
  std::string id = origin.substr(prefix.size());
  if (!id.empty() && id.back() == '/') {
    id.pop_back();  // Native Messaging passes "chrome-extension://<id>/"
  }
  const std::vector<std::string> allowed = AllowedExtensionIds();
  if (allowed.empty()) {
    // Until config.ini lists IDs any extension is accepted: both transports are local only
    // (loopback, or a pipe whose client must be a child of this browser).
    return true;
  }
  for (const std::string& candidate : allowed) {
    if (candidate == id) {
      return true;
    }
  }
  return false;
}

void AddClient(const std::shared_ptr<LinkClient>& client) {
  client->id = g_next_client++;
  {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    g_clients[client->id] = client;
  }
  ARB_INFO("client %d accepted over %s (origin %s)", client->id, client->transport.c_str(),
           client->origin.c_str());
  client->Send(StatusJson("hello"));
}

void RemoveClient(int id) {
  std::string description;
  {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    auto found = g_clients.find(id);
    if (found == g_clients.end()) {
      return;
    }
    description = found->second->transport + ", origin " + found->second->origin;
    g_clients.erase(found);
  }
  GlobalState().RemoveClient(id);
  UiThreadRequestRefresh();
  ARB_INFO("client %d disconnected (%s)", id, description.c_str());
}

// Any exception here would terminate the browser process, so the whole message handler is
// guarded.
void HandleClientMessage(LinkClient& client, const std::string& text) {
  try {
    HandleMessageUnsafe(client, text);
  } catch (const std::exception& exception) {
    ARB_ERROR("message handling failed: %s", exception.what());
  } catch (...) {
    ARB_ERROR("message handling failed with an unknown exception");
  }
}

void UpdateServerStatus(const ServerStatus& status) {
  {
    std::lock_guard<std::mutex> lock(g_status_mutex);
    g_status = status;
  }
  BroadcastJson(StatusJson("status"));
}

void SetSafeModeResetHandler(SafeModeResetHandler handler) { g_safe_mode_reset = handler; }

void BroadcastJson(const std::string& json) {
  std::vector<std::shared_ptr<LinkClient>> clients;
  {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    for (const auto& [id, client] : g_clients) {
      clients.push_back(client);
    }
  }
  for (auto& client : clients) {
    client->Send(json);
  }
}

bool StartLinkServers(const std::string& native_version, const std::string& build_stamp) {
  g_native_version = native_version;
  g_build_stamp = build_stamp;
  const bool pipe = StartPipeServer();
  const int port = StartWebSocketServer();
  return pipe || port != 0;
}

}  // namespace arbortab
