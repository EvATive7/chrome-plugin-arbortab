// Thread-safe display state shared between the WebSocket thread and the UI thread.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <set>
#include <vector>

namespace arbortab {

struct TabDisplayState {
  int depth = 0;
  bool hidden = false;
  bool has_children = false;
  bool collapsed = false;
  int hidden_count = 0;
  // Fixed storage keeps this state trivially destructible: it is copied inside guarded UI
  // hooks where MSVC does not allow objects that require C++ unwinding.
  std::array<char, 1024> title{};

  bool operator==(const TabDisplayState&) const = default;
};

struct DisplayConfig {
  int indent = 14;
  bool show_count = true;
  bool show_guides = true;
  bool animations = true;
  // ARGB drop-target accent; 0 follows the browser theme (ui::kColorSysPrimary).
  uint32_t highlight_color = 0;
  int min_tab_width = 48;

  bool operator==(const DisplayConfig&) const = default;
};

class StateStore {
 public:
  // Replaces the full snapshot for one client. Returns true when anything changed.
  bool ReplaceAll(int client_id, const std::vector<int>& tabs,
                  const std::map<int, TabDisplayState>& states, const DisplayConfig& config);
  void RemoveClient(int client_id);

  TabDisplayState Get(int tab_id) const;
  // Like Get, but tells whether any client has described the tab yet.
  bool Find(int tab_id, TabDisplayState& out) const;
  DisplayConfig Config() const;
  // Increments on every accepted change so the UI thread can skip redundant refreshes.
  uint64_t Version() const;
  size_t ClientCount() const;
  std::string Describe() const;

  // Snapshots received, and those that only changed the tab order. A reorder changes nothing
  // the strip draws, so it is otherwise invisible here; a steady stream of them means tabs keep
  // being moved (e.g. the extension and Chrome, or another extension, fighting over the order).
  struct Counters {
    uint64_t snapshots = 0;
    uint64_t reorders = 0;
  };
  Counters Counts() const;

 private:
  mutable std::mutex mutex_;
  // Tab ids are unique inside one browser process, so a single flat table with
  // last-writer-wins semantics is enough (and correct with several profiles connected).
  std::map<int, TabDisplayState> tabs_;
  std::map<int, std::vector<int>> client_tabs_;
  std::map<int, std::vector<int>> client_order_;  // last reported strip order per client
  DisplayConfig config_;
  uint64_t version_ = 0;
  uint64_t snapshots_ = 0;
  uint64_t reorders_ = 0;
};

StateStore& GlobalState();

}  // namespace arbortab
