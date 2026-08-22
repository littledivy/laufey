// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Deep-link (custom URL scheme) plumbing shared by the CEF and WebView
// backends on macOS. Each backend's AppDelegate implements
// `application:openURLs:` and calls FireOpenUrlMac() once per URL; the
// storage and the cold-start buffer live here so both delegates behave
// identically. See docs/deep-links.md.

#include "laufey_backend_common.h"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace laufey_common {

namespace {

std::mutex& OpenUrlMutex() {
  static std::mutex m;
  return m;
}

laufey_open_url_fn g_open_url_fn = nullptr;
void* g_open_url_data = nullptr;

// URLs delivered before a handler was registered. A cold-start deep link is
// always in this position: AppKit dispatches the Apple Event as soon as the
// app finishes launching, while the runtime that registers the handler is
// still coming up on its own thread.
std::vector<std::string>& PendingOpenUrls() {
  static std::vector<std::string> v;
  return v;
}

}  // namespace

void SetOpenUrlHandlerMac(laufey_open_url_fn handler, void* user_data) {
  std::vector<std::string> pending;
  {
    std::lock_guard<std::mutex> lock(OpenUrlMutex());
    g_open_url_fn = handler;
    g_open_url_data = user_data;
    // Clearing the handler re-arms buffering; anything still queued stays
    // queued for the next registration.
    if (handler) {
      pending = std::move(PendingOpenUrls());
      PendingOpenUrls().clear();
    }
  }

  // Outside the lock: the handler may re-enter (e.g. clear itself), and it is
  // embedder code we don't want to run under our own mutex. Note this runs on
  // the registering thread, not the UI thread — the only case where an
  // open-url callback fires anywhere else.
  for (const std::string& url : pending) {
    handler(user_data, url.c_str());
  }
}

void FireOpenUrlMac(const char* url) {
  if (!url) {
    return;
  }

  laufey_open_url_fn fn = nullptr;
  void* data = nullptr;
  {
    std::lock_guard<std::mutex> lock(OpenUrlMutex());
    fn = g_open_url_fn;
    data = g_open_url_data;
    if (!fn) {
      std::vector<std::string>& pending = PendingOpenUrls();
      // Drop the oldest rather than the newest: the most recent link is the
      // one the user is waiting on.
      if (pending.size() >= LAUFEY_MAX_PENDING_OPEN_URLS) {
        pending.erase(pending.begin());
      }
      pending.emplace_back(url);
    }
  }

  if (fn) {
    fn(data, url);
  }
}

bool TestTriggerOpenUrlMac(const char* url) {
  if (!url) {
    return false;
  }
  bool delivered;
  {
    std::lock_guard<std::mutex> lock(OpenUrlMutex());
    delivered = g_open_url_fn != nullptr;
  }
  FireOpenUrlMac(url);
  return delivered;
}

}  // namespace laufey_common
