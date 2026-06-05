// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// AppIndicator-backed tray / status-bar icon. Thread-safe: GTK calls are
// marshaled to the GTK main thread via g_idle_add, so backends can call
// these from any thread. tray_id is allocated synchronously so the
// caller gets a useful return value immediately.
//
// When libappindicator is not available (no LAUFEY_HAVE_APPINDICATOR
// define), CreateTrayIconLinux returns 0 and the other functions no-op.

#include <gtk/gtk.h>

#include "laufey_backend_common.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifdef LAUFEY_HAVE_APPINDICATOR
extern "C" {
#include <libappindicator/app-indicator.h>
}
#endif

namespace laufey_common {

#ifdef LAUFEY_HAVE_APPINDICATOR

namespace {

struct LinuxTrayEntry {
  AppIndicator* indicator;
  GtkWidget* menu;
  laufey_menu_click_fn menu_click_fn;
  void* menu_click_data;
};

std::mutex& LinuxTrayMutex() {
  static std::mutex m;
  return m;
}
std::map<uint32_t, LinuxTrayEntry>& LinuxTrayMap() {
  static std::map<uint32_t, LinuxTrayEntry> m;
  return m;
}
std::atomic<uint32_t> g_next_tray_id_linux{1};

// Idle-dispatch helper for moving owned closures onto the GTK main loop.
template <typename Fn>
void OnGtkMain(Fn&& fn) {
  using FnT = std::decay_t<Fn>;
  auto* heap = new FnT(std::forward<Fn>(fn));
  g_idle_add_full(
      G_PRIORITY_DEFAULT_IDLE,
      [](gpointer data) -> gboolean {
        auto* f = static_cast<FnT*>(data);
        (*f)();
        return G_SOURCE_REMOVE;
      },
      heap,
      [](gpointer data) { delete static_cast<FnT*>(data); });
}

}  // namespace

uint32_t CreateTrayIconLinux() {
  uint32_t tray_id =
      g_next_tray_id_linux.fetch_add(1, std::memory_order_relaxed);
  OnGtkMain([tray_id] {
    std::string idstr = "laufey-tray-" + std::to_string(tray_id);
    AppIndicator* ind = app_indicator_new(
        idstr.c_str(), "", APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    if (!ind) return;
    app_indicator_set_status(ind, APP_INDICATOR_STATUS_ACTIVE);
    GtkWidget* placeholder = gtk_menu_new();
    gtk_widget_show_all(placeholder);
    app_indicator_set_menu(ind, GTK_MENU(placeholder));

    LinuxTrayEntry entry{};
    entry.indicator = ind;
    entry.menu = placeholder;
    std::lock_guard<std::mutex> lock(LinuxTrayMutex());
    LinuxTrayMap()[tray_id] = std::move(entry);
  });
  return tray_id;
}

void DestroyTrayIconLinux(uint32_t tray_id) {
  OnGtkMain([tray_id] {
    std::lock_guard<std::mutex> lock(LinuxTrayMutex());
    auto it = LinuxTrayMap().find(tray_id);
    if (it == LinuxTrayMap().end()) return;
    if (it->second.indicator) {
      app_indicator_set_status(it->second.indicator,
                               APP_INDICATOR_STATUS_PASSIVE);
      g_object_unref(it->second.indicator);
    }
    LinuxTrayMap().erase(it);
  });
}

void SetTrayIconLinux(uint32_t tray_id, const void* png_bytes, size_t len) {
  if (!png_bytes || len == 0) return;
  // AppIndicator on most DEs reads icons by name from the icon theme,
  // not from raw bytes. Write the bytes to a per-tray temp file and
  // point the indicator at its full path.
  std::vector<uint8_t> bytes(static_cast<const uint8_t*>(png_bytes),
                              static_cast<const uint8_t*>(png_bytes) + len);
  OnGtkMain([tray_id, bytes = std::move(bytes)]() mutable {
    std::string path = "/tmp/laufey-tray-" + std::to_string(tray_id) + ".png";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    std::lock_guard<std::mutex> lock(LinuxTrayMutex());
    auto it = LinuxTrayMap().find(tray_id);
    if (it == LinuxTrayMap().end() || !it->second.indicator) return;
    app_indicator_set_icon_full(it->second.indicator, path.c_str(), "");
  });
}

void SetTrayIconDarkLinux(uint32_t /*tray_id*/, const void* /*png_bytes*/,
                            size_t /*len*/) {
  // AppIndicator has no separate dark-icon API; the icon theme handles
  // light/dark. Backends that previously stored a dark icon kept it
  // for API symmetry only; same here.
}

void SetTrayTooltipLinux(uint32_t /*tray_id*/,
                           const char* /*tooltip_or_null*/) {
  // The AppIndicator / StatusNotifier protocol has no tooltip concept.
}

void SetTrayMenuLinux(uint32_t tray_id, laufey_value_t* menu_template,
                       const laufey_backend_api_t* api,
                       laufey_menu_click_fn on_click, void* on_click_data) {
  OnGtkMain([tray_id, menu_template, api, on_click, on_click_data] {
    // tray_id passed as window_id so the shared click dispatcher routes
    // back through on_click with the right tray identifier.
    GtkWidget* new_menu = nullptr;
    if (menu_template) {
      new_menu = BuildGtkMenuFromValue(menu_template, api, tray_id, on_click,
                                        on_click_data, false);
      if (new_menu) gtk_widget_show_all(new_menu);
      api->value_free(menu_template);
    }

    std::lock_guard<std::mutex> lock(LinuxTrayMutex());
    auto it = LinuxTrayMap().find(tray_id);
    if (it == LinuxTrayMap().end()) {
      if (new_menu) gtk_widget_destroy(new_menu);
      return;
    }
    if (new_menu) {
      app_indicator_set_menu(it->second.indicator, GTK_MENU(new_menu));
      it->second.menu = new_menu;
    } else {
      GtkWidget* empty = gtk_menu_new();
      gtk_widget_show_all(empty);
      app_indicator_set_menu(it->second.indicator, GTK_MENU(empty));
      it->second.menu = empty;
    }
    it->second.menu_click_fn = on_click;
    it->second.menu_click_data = on_click_data;
  });
}

void SetTrayClickHandlerLinux(uint32_t /*tray_id*/,
                                laufey_tray_click_fn /*handler*/,
                                void* /*user_data*/) {
  // AppIndicator has no left-click event; clicks always open the menu.
}

void SetTrayDoubleClickHandlerLinux(uint32_t /*tray_id*/,
                                      laufey_tray_click_fn /*handler*/,
                                      void* /*user_data*/) {
  // Same: no double-click event from AppIndicator.
}

#else  // !LAUFEY_HAVE_APPINDICATOR

uint32_t CreateTrayIconLinux() { return 0; }
void DestroyTrayIconLinux(uint32_t) {}
void SetTrayIconLinux(uint32_t, const void*, size_t) {}
void SetTrayIconDarkLinux(uint32_t, const void*, size_t) {}
void SetTrayTooltipLinux(uint32_t, const char*) {}
void SetTrayMenuLinux(uint32_t, laufey_value_t* tmpl, const laufey_backend_api_t* api,
                       laufey_menu_click_fn, void*) {
  if (tmpl && api) api->value_free(tmpl);
}
void SetTrayClickHandlerLinux(uint32_t, laufey_tray_click_fn, void*) {}
void SetTrayDoubleClickHandlerLinux(uint32_t, laufey_tray_click_fn, void*) {}

#endif  // LAUFEY_HAVE_APPINDICATOR

}  // namespace laufey_common
