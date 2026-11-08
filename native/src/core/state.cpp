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
  bool changed = config_ != config;
  config_ = config;

  // Drop the tabs this client used to own but no longer reports, then apply the snapshot.
  std::set<int> wanted(tabs.begin(), tabs.end());
  std::vector<int>& previous = client_tabs_[client_id];
  std::set<int> next;
  for (int tab_id : previous) {
    if (wanted.count(tab_id) == 0 && tabs_.erase(tab_id) > 0) {
      changed = true;
    }
  }
  for (int tab_id : tabs) {
    const auto found = states.find(tab_id);
    const TabDisplayState state = found == states.end() ? TabDisplayState{} : found->second;
    const auto existing = tabs_.find(tab_id);
    if (existing == tabs_.end() || !(existing->second == state)) {
      tabs_[tab_id] = state;
      changed = true;
    }
    next.insert(tab_id);
  }
  previous.assign(next.begin(), next.end());
  if (changed) {
    ++version_;
  }
  return changed;
}

void StateStore::RemoveClient(int client_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto owned = client_tabs_.find(client_id);
  if (owned == client_tabs_.end()) {
    return;
  }
  for (int tab_id : owned->second) {
    tabs_.erase(tab_id);
  }
  client_tabs_.erase(owned);
  ++version_;
}

TabDisplayState StateStore::Get(int tab_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = tabs_.find(tab_id);
  return found == tabs_.end() ? TabDisplayState{} : found->second;
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
  return client_tabs_.size();
}

std::string StateStore::Describe() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string text;
  char buffer[128];
  for (const auto& [client, tabs] : client_tabs_) {
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "client%d=%zu tabs", client, tabs.size());
    if (!text.empty()) {
      text += ' ';
    }
    text += buffer;
  }
  _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, " (%zu known tabs)", tabs_.size());
  text += buffer;
  return text;
}

}  // namespace arbortab
