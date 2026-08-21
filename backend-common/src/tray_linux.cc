// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// AppIndicator-backed tray / status-bar icon. Thread-safe: GTK calls are
// marshaled to the GTK main thread via g_idle_add, so backends can call
// these from any thread. tray_id is allocated synchronously so the
// caller gets a useful return value immediately.
//
// The appindicator library is dlopen()ed at runtime — Ayatana first,
// then the legacy libappindicator — instead of being linked at build
// time. Release artifacts must work both on distros that ship only the
// Ayatana fork and on ones with the legacy library, and must still
// start on systems with neither; the two flavors are ABI-compatible for
// the handful of symbols used here (the fork kept names and
// signatures). When no library can be loaded, CreateTrayIconLinux
// returns 0 and the other functions no-op.

#include <dlfcn.h>
#include <gtk/gtk.h>

#include "laufey_backend_common.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace laufey_common {

namespace {

// Minimal appindicator ABI. AppIndicator is opaque; the enum values are
// stable across both flavors (APP_INDICATOR_CATEGORY_APPLICATION_STATUS,
// APP_INDICATOR_STATUS_{PASSIVE,ACTIVE}).
typedef struct _AppIndicator AppIndicator;

constexpr int kAppIndicatorCategoryApplicationStatus = 0;
constexpr int kAppIndicatorStatusPassive = 0;
constexpr int kAppIndicatorStatusActive = 1;

struct AppIndicatorApi {
  AppIndicator* (*app_indicator_new)(const gchar* id, const gchar* icon_name,
                                     int category);
  void (*set_status)(AppIndicator* self, int status);
  void (*set_menu)(AppIndicator* self, GtkMenu* menu);
  void (*set_icon_full)(AppIndicator* self, const gchar* icon_name,
                        const gchar* icon_desc);
};

// Loads the appindicator library once; nullptr if unavailable. Safe to
// call from any thread (dlopen is thread-safe and the initializer is a
// C++11 magic static).
const AppIndicatorApi* GetAppIndicatorApi() {
  static AppIndicatorApi api;
  static const bool loaded = [] {
    static const char* const kSonames[] = {
        "libayatana-appindicator3.so.1",
        "libappindicator3.so.1",
    };
    void* lib = nullptr;
    for (const char* soname : kSonames) {
      // RTLD_GLOBAL: the library registers GObject types whose symbols
      // its dbusmenu helpers may resolve across module boundaries.
      lib = dlopen(soname, RTLD_NOW | RTLD_GLOBAL);
      if (lib) break;
    }
    if (!lib) {
      // Say why the tray id will be 0 — a silent stub cost real debugging
      // time downstream (issue #63).
      g_warning(
          "laufey: tray icons unavailable: could not load "
          "libayatana-appindicator3.so.1 or libappindicator3.so.1");
      return false;
    }
    api.app_indicator_new = reinterpret_cast<decltype(api.app_indicator_new)>(
        dlsym(lib, "app_indicator_new"));
    api.set_status = reinterpret_cast<decltype(api.set_status)>(
        dlsym(lib, "app_indicator_set_status"));
    api.set_menu = reinterpret_cast<decltype(api.set_menu)>(
        dlsym(lib, "app_indicator_set_menu"));
    api.set_icon_full = reinterpret_cast<decltype(api.set_icon_full)>(
        dlsym(lib, "app_indicator_set_icon_full"));
    return api.app_indicator_new && api.set_status && api.set_menu &&
           api.set_icon_full;
  }();
  return loaded ? &api : nullptr;
}

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
  const AppIndicatorApi* api = GetAppIndicatorApi();
  if (!api) return 0;
  uint32_t tray_id =
      g_next_tray_id_linux.fetch_add(1, std::memory_order_relaxed);
  OnGtkMain([api, tray_id] {
    std::string idstr = "laufey-tray-" + std::to_string(tray_id);
    AppIndicator* ind = api->app_indicator_new(
        idstr.c_str(), "", kAppIndicatorCategoryApplicationStatus);
    if (!ind) return;
    api->set_status(ind, kAppIndicatorStatusActive);
    GtkWidget* placeholder = gtk_menu_new();
    gtk_widget_show_all(placeholder);
    api->set_menu(ind, GTK_MENU(placeholder));

    LinuxTrayEntry entry{};
    entry.indicator = ind;
    entry.menu = placeholder;
    std::lock_guard<std::mutex> lock(LinuxTrayMutex());
    LinuxTrayMap()[tray_id] = std::move(entry);
  });
  return tray_id;
}

void DestroyTrayIconLinux(uint32_t tray_id) {
  const AppIndicatorApi* api = GetAppIndicatorApi();
  if (!api) return;
  OnGtkMain([api, tray_id] {
    std::lock_guard<std::mutex> lock(LinuxTrayMutex());
    auto it = LinuxTrayMap().find(tray_id);
    if (it == LinuxTrayMap().end()) return;
    if (it->second.indicator) {
      api->set_status(it->second.indicator, kAppIndicatorStatusPassive);
      g_object_unref(it->second.indicator);
    }
    LinuxTrayMap().erase(it);
  });
}

void SetTrayIconLinux(uint32_t tray_id, const void* png_bytes, size_t len) {
  const AppIndicatorApi* api = GetAppIndicatorApi();
  if (!api || !png_bytes || len == 0) return;
  // AppIndicator on most DEs reads icons by name from the icon theme,
  // not from raw bytes. Write the bytes to a per-tray temp file and
  // point the indicator at its full path.
  std::vector<uint8_t> bytes(static_cast<const uint8_t*>(png_bytes),
                              static_cast<const uint8_t*>(png_bytes) + len);
  OnGtkMain([api, tray_id, bytes = std::move(bytes)]() mutable {
    std::string path = "/tmp/laufey-tray-" + std::to_string(tray_id) + ".png";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    std::lock_guard<std::mutex> lock(LinuxTrayMutex());
    auto it = LinuxTrayMap().find(tray_id);
    if (it == LinuxTrayMap().end() || !it->second.indicator) return;
    api->set_icon_full(it->second.indicator, path.c_str(), "");
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
  const AppIndicatorApi* ind_api = GetAppIndicatorApi();
  if (!ind_api) {
    if (menu_template && api) api->value_free(menu_template);
    return;
  }
  OnGtkMain([ind_api, tray_id, menu_template, api, on_click, on_click_data] {
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
      ind_api->set_menu(it->second.indicator, GTK_MENU(new_menu));
      it->second.menu = new_menu;
    } else {
      GtkWidget* empty = gtk_menu_new();
      gtk_widget_show_all(empty);
      ind_api->set_menu(it->second.indicator, GTK_MENU(empty));
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

}  // namespace laufey_common
