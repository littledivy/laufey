// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Title-prefix badge bookkeeping. macOS has NSDockTile.setBadgeLabel;
// Windows and Linux fall back to "(N) " prefix on each window's title.
// This module owns the per-window saved-title map so each backend
// (CEF Win+Linux, webview Win, webview Linux) doesn't have its own copy.

#include "laufey_backend_common.h"

#include <map>
#include <mutex>
#include <string>

namespace laufey_common {

namespace {

std::mutex& BadgeMutex() {
  static std::mutex m;
  return m;
}
std::map<uint64_t, std::string>& SavedTitles() {
  static std::map<uint64_t, std::string> map;
  return map;
}

}  // namespace

std::string ApplyTitlePrefixBadge(uint64_t window_key,
                                   const std::string& current_title,
                                   const std::string& badge) {
  std::lock_guard<std::mutex> lock(BadgeMutex());
  auto& map = SavedTitles();
  if (!badge.empty()) {
    auto it = map.find(window_key);
    if (it == map.end()) {
      map[window_key] = current_title;
      return "(" + badge + ") " + current_title;
    }
    return "(" + badge + ") " + it->second;
  }
  // badge empty — restore if we have a saved title.
  auto it = map.find(window_key);
  if (it == map.end()) return current_title;
  std::string saved = it->second;
  map.erase(it);
  return saved;
}

void ForgetTitlePrefixBadge(uint64_t window_key) {
  std::lock_guard<std::mutex> lock(BadgeMutex());
  SavedTitles().erase(window_key);
}

}  // namespace laufey_common
