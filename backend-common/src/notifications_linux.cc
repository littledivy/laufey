// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Notifications (Linux): shells out to `notify-send`, which exists on
// effectively every modern desktop Linux. notify-send is fire-and-forget,
// so on_event only sees SHOWN (synthetic, fired immediately after spawn)
// and CLOSED (synthetic, fired from CloseNotificationLinux). Click and
// action events are not surfaced because notify-send has no callback
// channel back to us.

#include "laufey_backend_common.h"

#include <atomic>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

namespace laufey_common {

namespace {

struct LinuxNotifEntry {
  std::string tag;
  laufey_notification_event_fn on_event;
  void* user_data;
};

std::mutex& NotifMutex() {
  static std::mutex m;
  return m;
}

std::map<uint32_t, LinuxNotifEntry>& NotifMap() {
  static std::map<uint32_t, LinuxNotifEntry> map;
  return map;
}

std::atomic<uint32_t> g_next_notif_id{1};

std::string ShellEscape(const std::string& s) {
  // Wrap in single quotes; escape any embedded single quotes.
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

}  // namespace

uint32_t ShowNotificationLinux(const NotificationOptions& opts,
                               laufey_notification_event_fn on_event,
                               void* user_data) {
  uint32_t nid = g_next_notif_id.fetch_add(1, std::memory_order_relaxed);

  std::string cmd = "notify-send";
  if (opts.require_interaction)
    cmd += " --urgency=critical";
  if (!opts.tag.empty()) {
    // notify-send doesn't support a "replace this id" flag without
    // libnotify-tools 0.7.10+; --hint=string:x-canonical-private-synchronous
    // is honored by most servers and provides the desired collapse.
    cmd += " --hint=string:x-canonical-private-synchronous:";
    cmd += ShellEscape(opts.tag);
  }
  cmd += " -- ";
  cmd += ShellEscape(opts.title);
  cmd += " ";
  cmd += ShellEscape(opts.body);
  cmd += " &";  // background; we don't wait for the notification daemon

  int rc = std::system(cmd.c_str());
  (void)rc;  // ignore — even on failure we still create the entry

  {
    std::lock_guard<std::mutex> lock(NotifMutex());
    NotifMap()[nid] = {opts.tag, on_event, user_data};
  }
  if (on_event)
    on_event(user_data, nid, LAUFEY_NOTIFICATION_SHOWN, nullptr);
  return nid;
}

void CloseNotificationLinux(uint32_t notification_id) {
  laufey_notification_event_fn fn = nullptr;
  void* ud = nullptr;
  {
    std::lock_guard<std::mutex> lock(NotifMutex());
    auto it = NotifMap().find(notification_id);
    if (it == NotifMap().end())
      return;
    fn = it->second.on_event;
    ud = it->second.user_data;
    NotifMap().erase(it);
  }
  // The OS notification has its own lifecycle — we can't programmatically
  // close it via notify-send. Fire the CLOSED callback so the runtime can
  // clean up its handler bookkeeping.
  if (fn)
    fn(ud, notification_id, LAUFEY_NOTIFICATION_CLOSED, nullptr);
}

}  // namespace laufey_common
