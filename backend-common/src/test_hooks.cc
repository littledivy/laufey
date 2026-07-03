// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Test-only click registry shared by the menu builders. Maps a menu/tray item
// id to its on_click handler so automated e2e tests can synthesize a click
// (via the `test_click_menu_item` C ABI hook) without OS-level input injection.
// See docs/e2e-testing.md.

#include "laufey_backend_common.h"

#include <map>
#include <mutex>
#include <string>

namespace laufey_common {

namespace {

struct MenuClickEntry {
  laufey_menu_click_fn fn;
  void* data;
  uint32_t window_id;
};

std::mutex& MenuClickMutex() {
  static std::mutex m;
  return m;
}

std::map<std::string, MenuClickEntry>& MenuClickMap() {
  static std::map<std::string, MenuClickEntry> m;
  return m;
}

}  // namespace

void RegisterMenuClick(const std::string& id, laufey_menu_click_fn fn,
                       void* data, uint32_t window_id) {
  if (id.empty() || !fn) return;
  std::lock_guard<std::mutex> lock(MenuClickMutex());
  MenuClickMap()[id] = MenuClickEntry{fn, data, window_id};
}

bool TestClickMenuItem(const char* item_id) {
  if (!item_id) return false;
  std::string id(item_id);
  MenuClickEntry entry;
  {
    std::lock_guard<std::mutex> lock(MenuClickMutex());
    auto it = MenuClickMap().find(id);
    if (it == MenuClickMap().end()) return false;
    entry = it->second;
  }
  // Invoke outside the lock: the handler may re-enter the menu system.
  entry.fn(entry.data, entry.window_id, id.c_str());
  return true;
}

}  // namespace laufey_common
