#include "core/state.h"

#include <cstdio>

namespace arbortab {
namespace {

StateStore g_state;

}  // namespace

StateStore& GlobalState() { return g_state; }

bool StateStore::ReplaceAll(int client_id, const std::vector<int>& tabs,
                            const std::map<int, TabDisplayState>& states,
                            const DisplayConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::map<int, TabDisplayState> snapshot;
  for (int tab_id : tabs) {
    const auto found = states.find(tab_id);
    snapshot[tab_id] = found == states.end() ? TabDisplayState{} : found->second;
  }
  const bool config_changed = config_.indent != config.indent ||
                              config_.show_count != config.show_count ||
                              config_.highlight_color != config.highlight_color ||
                              config_.min_tab_width != config.min_tab_width;
  const bool states_changed = clients_[client_id] != snapshot;
  if (!config_changed && !states_changed) {
    return false;
  }
  clients_[client_id] = std::move(snapshot);
  config_ = config;
  ++version_;
  return true;
}

void StateStore::RemoveClient(int client_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (clients_.erase(client_id) > 0) {
    ++version_;
  }
}

TabDisplayState StateStore::Get(int tab_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [client, tabs] : clients_) {
    const auto found = tabs.find(tab_id);
    if (found != tabs.end()) {
      return found->second;
    }
  }
  return TabDisplayState{};
}

DisplayConfig StateStore::Config() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return config_;
}

uint64_t StateStore::Version() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return version_;
}

size_t StateStore::ClientCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return clients_.size();
}

std::string StateStore::Describe() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string text;
  char buffer[128];
  for (const auto& [client, tabs] : clients_) {
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "client%d=%zu tabs", client, tabs.size());
    if (!text.empty()) {
      text += ' ';
    }
    text += buffer;
  }
  return text.empty() ? "no clients" : text;
}

}  // namespace arbortab
