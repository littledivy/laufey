// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

// Linux-specific backend implementations: tray (via libappindicator) and
// context menu (via GtkMenu popup). Neither needs a GtkWindow parent, so
// both work alongside CEF Views' raw X11 windows.
//
// set_application_menu (an in-window menu bar) is NOT implemented here: it
// requires packing a GtkMenuBar into a GtkBox above the browser widget,
// which means the top-level window must itself be a GtkWindow. CEF Views
// owns the X11 window directly, so menubar support is deferred until the
// Linux backend is switched to windowed-mode CEF embedded in a GtkWindow.

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

#include <gtk/gtk.h>

#include "include/base/cef_callback.h"
#include "include/cef_browser.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"

#include "runtime_loader.h"
#include "wef.h"

#ifdef WEF_HAVE_APPINDICATOR
extern "C" {
#include <libappindicator/app-indicator.h>
}
#endif

// ---------------------------------------------------------------------------
// GTK lazy init. CEF's Chromium process initializes GTK internally for its
// own dialogs/theming, but we don't rely on that — call gtk_init_check once
// before any GTK API use.
// ---------------------------------------------------------------------------

static void EnsureGtkInit() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    int argc = 0;
    char** argv = nullptr;
    gtk_init_check(&argc, &argv);
  });
}

// ---------------------------------------------------------------------------
// Menu-template → GtkMenu conversion
// ---------------------------------------------------------------------------

namespace {

struct GtkMenuCallbackData {
  wef_menu_click_fn on_click;
  void* on_click_data;
  uint32_t window_id;
  std::string item_id;
};

void OnGtkMenuItemActivate(GtkMenuItem* /*item*/, gpointer user_data) {
  auto* data = static_cast<GtkMenuCallbackData*>(user_data);
  if (data->on_click) {
    data->on_click(data->on_click_data, data->window_id, data->item_id.c_str());
  }
}

void DestroyGtkMenuCallbackData(gpointer user_data, GClosure* /*closure*/) {
  delete static_cast<GtkMenuCallbackData*>(user_data);
}

GtkWidget* BuildGtkMenuFromValue(wef_value_t* val, const wef_backend_api_t* api,
                                 uint32_t window_id, wef_menu_click_fn on_click,
                                 void* on_click_data, bool is_menu_bar) {
  if (!val || !api->value_is_list(val))
    return nullptr;

  GtkWidget* menu = is_menu_bar ? gtk_menu_bar_new() : gtk_menu_new();
  size_t count = api->value_list_size(val);

  for (size_t i = 0; i < count; ++i) {
    wef_value_t* itemVal = api->value_list_get(val, i);
    if (!itemVal || !api->value_is_dict(itemVal))
      continue;

    wef_value_t* typeVal = api->value_dict_get(itemVal, "type");
    if (typeVal && api->value_is_string(typeVal)) {
      size_t len = 0;
      char* typeStr = api->value_get_string(typeVal, &len);
      if (typeStr && std::string(typeStr) == "separator") {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());
        api->value_free_string(typeStr);
        continue;
      }
      if (typeStr)
        api->value_free_string(typeStr);
    }

    // Role-based items — GTK has no built-in role concept, so map roles to
    // labels and forward the role as the item id.
    wef_value_t* roleVal = api->value_dict_get(itemVal, "role");
    if (roleVal && api->value_is_string(roleVal)) {
      size_t len = 0;
      char* roleStr = api->value_get_string(roleVal, &len);
      if (roleStr) {
        std::string role = roleStr;
        api->value_free_string(roleStr);

        std::string label;
        if (role == "quit")
          label = "Quit";
        else if (role == "copy")
          label = "Copy";
        else if (role == "paste")
          label = "Paste";
        else if (role == "cut")
          label = "Cut";
        else if (role == "selectall" || role == "selectAll")
          label = "Select All";
        else if (role == "undo")
          label = "Undo";
        else if (role == "redo")
          label = "Redo";
        else if (role == "minimize")
          label = "Minimize";
        else if (role == "close")
          label = "Close";
        else if (role == "about")
          label = "About";
        else if (role == "togglefullscreen" || role == "toggleFullScreen")
          label = "Toggle Full Screen";

        if (!label.empty()) {
          GtkWidget* item = gtk_menu_item_new_with_label(label.c_str());
          auto* cb_data =
              new GtkMenuCallbackData{on_click, on_click_data, window_id, role};
          g_signal_connect_data(item, "activate",
                                G_CALLBACK(OnGtkMenuItemActivate), cb_data,
                                DestroyGtkMenuCallbackData, (GConnectFlags)0);
          gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        }
        continue;
      }
    }

    wef_value_t* labelVal = api->value_dict_get(itemVal, "label");
    if (!labelVal || !api->value_is_string(labelVal))
      continue;
    size_t labelLen = 0;
    char* labelStr = api->value_get_string(labelVal, &labelLen);
    if (!labelStr)
      continue;
    std::string label = labelStr;
    api->value_free_string(labelStr);

    wef_value_t* submenuVal = api->value_dict_get(itemVal, "submenu");
    if (submenuVal && api->value_is_list(submenuVal)) {
      GtkWidget* parent = gtk_menu_item_new_with_label(label.c_str());
      GtkWidget* submenu = BuildGtkMenuFromValue(
          submenuVal, api, window_id, on_click, on_click_data, false);
      if (submenu)
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(parent), submenu);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), parent);
      continue;
    }

    std::string itemId;
    wef_value_t* idVal = api->value_dict_get(itemVal, "id");
    if (idVal && api->value_is_string(idVal)) {
      size_t idLen = 0;
      char* idStr = api->value_get_string(idVal, &idLen);
      if (idStr) {
        itemId = idStr;
        api->value_free_string(idStr);
      }
    }

    GtkWidget* gtkItem = gtk_menu_item_new_with_label(label.c_str());
    auto* cb_data = new GtkMenuCallbackData{on_click, on_click_data, window_id,
                                            itemId.empty() ? label : itemId};
    g_signal_connect_data(gtkItem, "activate",
                          G_CALLBACK(OnGtkMenuItemActivate), cb_data,
                          DestroyGtkMenuCallbackData, (GConnectFlags)0);

    wef_value_t* enabledVal = api->value_dict_get(itemVal, "enabled");
    if (enabledVal && api->value_is_bool(enabledVal) &&
        !api->value_get_bool(enabledVal)) {
      gtk_widget_set_sensitive(gtkItem, FALSE);
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtkItem);
  }

  return menu;
}

}  // namespace

// ---------------------------------------------------------------------------
// Context menu
// ---------------------------------------------------------------------------

void Backend_ShowContextMenu_Linux(void* data, uint32_t window_id, int /*x*/,
                                   int /*y*/, wef_value_t* menu_template,
                                   wef_menu_click_fn on_click,
                                   void* on_click_data) {
  if (!menu_template)
    return;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const wef_backend_api_t* api = &loader->GetBackendApi();

  CefPostTask(
      TID_UI,
      base::BindOnce(
          [](uint32_t wid, wef_value_t* tmpl, const wef_backend_api_t* a,
             wef_menu_click_fn cb, void* cb_data) {
            EnsureGtkInit();
            GtkWidget* menu =
                BuildGtkMenuFromValue(tmpl, a, wid, cb, cb_data, false);
            a->value_free(tmpl);
            if (!menu)
              return;
            gtk_widget_show_all(menu);
            // gtk_menu_popup_at_pointer uses the current X11 event to position
            // the menu — in practice, the click that triggered
            // show_context_menu is still fresh. (x,y) args from the WEF call
            // are window-relative and would require a GdkWindow reference to
            // honor; pointer-positioning is the robust fallback.
            gtk_menu_popup_at_pointer(GTK_MENU(menu), nullptr);
          },
          window_id, menu_template, api, on_click, on_click_data));
}

// ---------------------------------------------------------------------------
// Tray / status-bar icon (libappindicator)
// ---------------------------------------------------------------------------

#ifdef WEF_HAVE_APPINDICATOR

namespace {

struct LinuxTrayEntry {
  AppIndicator* indicator;
  GtkWidget* menu;
  wef_menu_click_fn menu_click_fn;
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

struct MenuActivateCtx {
  uint32_t tray_id;
  std::string item_id;
};

void OnLinuxTrayMenuActivate(GtkMenuItem* /*item*/, gpointer user_data) {
  auto* ctx = static_cast<MenuActivateCtx*>(user_data);
  wef_menu_click_fn fn = nullptr;
  void* data = nullptr;
  {
    std::lock_guard<std::mutex> lock(LinuxTrayMutex());
    auto it = LinuxTrayMap().find(ctx->tray_id);
    if (it != LinuxTrayMap().end()) {
      fn = it->second.menu_click_fn;
      data = it->second.menu_click_data;
    }
  }
  if (fn)
    fn(data, ctx->tray_id, ctx->item_id.c_str());
}

void DestroyMenuActivateCtx(gpointer data, GClosure* /*closure*/) {
  delete static_cast<MenuActivateCtx*>(data);
}

GtkWidget* BuildLinuxTrayMenu(uint32_t tray_id, wef_value_t* val,
                              const wef_backend_api_t* api) {
  if (!val || !api->value_is_list(val))
    return nullptr;
  GtkWidget* menu = gtk_menu_new();
  size_t count = api->value_list_size(val);
  for (size_t i = 0; i < count; ++i) {
    wef_value_t* itemVal = api->value_list_get(val, i);
    if (!itemVal || !api->value_is_dict(itemVal))
      continue;

    wef_value_t* typeVal = api->value_dict_get(itemVal, "type");
    if (typeVal && api->value_is_string(typeVal)) {
      size_t len = 0;
      char* s = api->value_get_string(typeVal, &len);
      if (s && std::string(s) == "separator") {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());
        api->value_free_string(s);
        continue;
      }
      if (s)
        api->value_free_string(s);
    }

    wef_value_t* labelVal = api->value_dict_get(itemVal, "label");
    std::string label;
    if (labelVal && api->value_is_string(labelVal)) {
      size_t len = 0;
      char* s = api->value_get_string(labelVal, &len);
      if (s) {
        label = std::string(s, len);
        api->value_free_string(s);
      }
    }

    wef_value_t* submenuVal = api->value_dict_get(itemVal, "submenu");
    if (submenuVal && api->value_is_list(submenuVal)) {
      GtkWidget* parent = gtk_menu_item_new_with_label(label.c_str());
      GtkWidget* sub = BuildLinuxTrayMenu(tray_id, submenuVal, api);
      if (sub)
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(parent), sub);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), parent);
      continue;
    }

    wef_value_t* idVal = api->value_dict_get(itemVal, "id");
    std::string item_id;
    if (idVal && api->value_is_string(idVal)) {
      size_t len = 0;
      char* s = api->value_get_string(idVal, &len);
      if (s) {
        item_id = std::string(s, len);
        api->value_free_string(s);
      }
    }

    GtkWidget* mi = gtk_menu_item_new_with_label(label.c_str());
    wef_value_t* enabledVal = api->value_dict_get(itemVal, "enabled");
    if (enabledVal && api->value_is_bool(enabledVal) &&
        !api->value_get_bool(enabledVal)) {
      gtk_widget_set_sensitive(mi, FALSE);
    }
    if (!item_id.empty()) {
      auto* ctx = new MenuActivateCtx{tray_id, item_id};
      g_signal_connect_data(mi, "activate", G_CALLBACK(OnLinuxTrayMenuActivate),
                            ctx, DestroyMenuActivateCtx, (GConnectFlags)0);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
  }
  gtk_widget_show_all(menu);
  return menu;
}

}  // namespace

uint32_t Backend_CreateTrayIcon_Linux(void* /*data*/) {
  uint32_t tray_id =
      g_next_tray_id_linux.fetch_add(1, std::memory_order_relaxed);
  // Run on the UI thread so GTK calls stay thread-affine.
  CefPostTask(TID_UI,
              base::BindOnce(
                  [](uint32_t tid) {
                    EnsureGtkInit();
                    std::string idstr = "wef-tray-" + std::to_string(tid);
                    AppIndicator* ind = app_indicator_new(
                        idstr.c_str(), "",
                        APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
                    if (!ind)
                      return;
                    app_indicator_set_status(ind, APP_INDICATOR_STATUS_ACTIVE);
                    // AppIndicator requires a non-null menu to be visible in
                    // most DEs.
                    GtkWidget* placeholder = gtk_menu_new();
                    gtk_widget_show_all(placeholder);
                    app_indicator_set_menu(ind, GTK_MENU(placeholder));

                    LinuxTrayEntry entry{};
                    entry.indicator = ind;
                    entry.menu = placeholder;
                    std::lock_guard<std::mutex> lock(LinuxTrayMutex());
                    LinuxTrayMap()[tid] = std::move(entry);
                  },
                  tray_id));
  return tray_id;
}

void Backend_DestroyTrayIcon_Linux(void* /*data*/, uint32_t tray_id) {
  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t tid) {
                            std::lock_guard<std::mutex> lock(LinuxTrayMutex());
                            auto it = LinuxTrayMap().find(tid);
                            if (it == LinuxTrayMap().end())
                              return;
                            if (it->second.indicator) {
                              app_indicator_set_status(
                                  it->second.indicator,
                                  APP_INDICATOR_STATUS_PASSIVE);
                              g_object_unref(it->second.indicator);
                            }
                            LinuxTrayMap().erase(it);
                          },
                          tray_id));
}

// AppIndicator on most DEs reads icons by name from the icon theme, not
// from raw bytes. Write the PNG to a per-tray temp file and point the
// indicator at its full path.
static void ApplyTrayIconPath(uint32_t tray_id, const std::string& path) {
  std::lock_guard<std::mutex> lock(LinuxTrayMutex());
  auto it = LinuxTrayMap().find(tray_id);
  if (it == LinuxTrayMap().end() || !it->second.indicator)
    return;
  app_indicator_set_icon_full(it->second.indicator, path.c_str(), "");
}

void Backend_SetTrayIcon_Linux(void* /*data*/, uint32_t tray_id,
                               const void* png_bytes, size_t len) {
  if (!png_bytes || len == 0)
    return;
  std::string path = "/tmp/wef-tray-" + std::to_string(tray_id) + ".png";
  FILE* f = fopen(path.c_str(), "wb");
  if (!f)
    return;
  fwrite(png_bytes, 1, len, f);
  fclose(f);
  CefPostTask(TID_UI, base::BindOnce(&ApplyTrayIconPath, tray_id, path));
}

void Backend_SetTrayTooltip_Linux(void* /*data*/, uint32_t /*tray_id*/,
                                  const char* /*tooltip_or_null*/) {
  // AppIndicator / StatusNotifier protocol has no tooltip concept.
}

void Backend_SetTrayMenu_Linux(void* data, uint32_t tray_id,
                               wef_value_t* menu_template,
                               wef_menu_click_fn on_click,
                               void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const wef_backend_api_t* api = &loader->GetBackendApi();

  CefPostTask(
      TID_UI,
      base::BindOnce(
          [](uint32_t tid, wef_value_t* tmpl, const wef_backend_api_t* a,
             wef_menu_click_fn cb, void* cb_data) {
            EnsureGtkInit();
            GtkWidget* new_menu =
                tmpl ? BuildLinuxTrayMenu(tid, tmpl, a) : nullptr;
            if (tmpl)
              a->value_free(tmpl);

            std::lock_guard<std::mutex> lock(LinuxTrayMutex());
            auto it = LinuxTrayMap().find(tid);
            if (it == LinuxTrayMap().end()) {
              if (new_menu)
                gtk_widget_destroy(new_menu);
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
            it->second.menu_click_fn = cb;
            it->second.menu_click_data = cb_data;
          },
          tray_id, menu_template, api, on_click, on_click_data));
}

void Backend_SetTrayClickHandler_Linux(void* /*data*/, uint32_t /*tray_id*/,
                                       wef_tray_click_fn /*handler*/,
                                       void* /*user_data*/) {
  // AppIndicator has no left-click event; a click anywhere on the indicator
  // pops up its menu.
}

void Backend_SetTrayDoubleClickHandler_Linux(void* /*data*/,
                                             uint32_t /*tray_id*/,
                                             wef_tray_click_fn /*handler*/,
                                             void* /*user_data*/) {
  // Same reasoning as SetTrayClickHandler: no click events exposed.
}

void Backend_SetTrayIconDark_Linux(void* /*data*/, uint32_t /*tray_id*/,
                                   const void* /*png_bytes*/, size_t /*len*/) {
  // StatusNotifier handles light/dark theming via the host's icon rendering;
  // there's no separate dark-variant API. No-op.
}

#else  // !WEF_HAVE_APPINDICATOR

uint32_t Backend_CreateTrayIcon_Linux(void* /*data*/) {
  return 0;
}
void Backend_DestroyTrayIcon_Linux(void* /*data*/, uint32_t /*tray_id*/) {}
void Backend_SetTrayIcon_Linux(void* /*data*/, uint32_t /*tray_id*/,
                               const void* /*png_bytes*/, size_t /*len*/) {}
void Backend_SetTrayTooltip_Linux(void* /*data*/, uint32_t /*tray_id*/,
                                  const char* /*tooltip_or_null*/) {}
void Backend_SetTrayMenu_Linux(void* data, uint32_t /*tray_id*/,
                               wef_value_t* menu_template,
                               wef_menu_click_fn /*on_click*/,
                               void* /*on_click_data*/) {
  if (!menu_template)
    return;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->GetBackendApi().value_free(menu_template);
}
void Backend_SetTrayClickHandler_Linux(void* /*data*/, uint32_t /*tray_id*/,
                                       wef_tray_click_fn /*handler*/,
                                       void* /*user_data*/) {}
void Backend_SetTrayDoubleClickHandler_Linux(void* /*data*/,
                                             uint32_t /*tray_id*/,
                                             wef_tray_click_fn /*handler*/,
                                             void* /*user_data*/) {}
void Backend_SetTrayIconDark_Linux(void* /*data*/, uint32_t /*tray_id*/,
                                   const void* /*png_bytes*/, size_t /*len*/) {}

#endif  // WEF_HAVE_APPINDICATOR

// ---------------------------------------------------------------------------
// Notifications (Linux): shells out to `notify-send`, which exists on
// effectively every modern desktop Linux. Doesn't surface click / action /
// close events back to us (notify-send is fire-and-forget), so on_event is
// only invoked synchronously with SHOWN immediately after spawn and CLOSED
// from close_notification. Actions in the options dict are ignored.
// ---------------------------------------------------------------------------

namespace {

struct LinuxNotifEntry {
  std::string tag;
  wef_notification_event_fn on_event;
  void* user_data;
};

std::mutex& NotifMutexLinux() {
  static std::mutex m;
  return m;
}
std::map<uint32_t, LinuxNotifEntry>& NotifMapLinux() {
  static std::map<uint32_t, LinuxNotifEntry> map;
  return map;
}
std::atomic<uint32_t> g_next_notif_id_linux{1};

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

extern "C" uint32_t Backend_ShowNotification_Linux(
    void* data, wef_value_t* options, wef_notification_event_fn on_event,
    void* user_data) {
  if (!options)
    return 0;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const wef_backend_api_t* api = &loader->GetBackendApi();
  if (!api->value_is_dict(options)) {
    api->value_free(options);
    return 0;
  }

  auto get_string = [&](const char* key) -> std::string {
    wef_value_t* v = api->value_dict_get(options, key);
    if (!v || !api->value_is_string(v))
      return std::string();
    size_t len = 0;
    char* s = api->value_get_string(v, &len);
    if (!s)
      return std::string();
    std::string out(s, len);
    api->value_free_string(s);
    return out;
  };
  auto get_bool = [&](const char* key, bool dfl) -> bool {
    wef_value_t* v = api->value_dict_get(options, key);
    if (!v || !api->value_is_bool(v))
      return dfl;
    return api->value_get_bool(v);
  };

  std::string title = get_string("title");
  std::string body = get_string("body");
  std::string tag = get_string("tag");
  bool require_interaction = get_bool("require_interaction", false);
  api->value_free(options);

  uint32_t nid = g_next_notif_id_linux.fetch_add(1, std::memory_order_relaxed);

  std::string cmd = "notify-send";
  if (require_interaction) {
    cmd += " --urgency=critical";
  }
  if (!tag.empty()) {
    // notify-send doesn't support a "replace this id" flag without
    // libnotify-tools 0.7.10+; --hint=string:x-canonical-private-synchronous
    // is honored by most servers and provides the desired collapse.
    cmd += " --hint=string:x-canonical-private-synchronous:";
    cmd += ShellEscape(tag);
  }
  cmd += " -- ";
  cmd += ShellEscape(title);
  cmd += " ";
  cmd += ShellEscape(body);
  cmd += " &";  // background; we don't wait for the notification daemon

  int rc = std::system(cmd.c_str());
  (void)rc;  // ignore — even on failure we still create the entry

  {
    std::lock_guard<std::mutex> lock(NotifMutexLinux());
    NotifMapLinux()[nid] = {tag, on_event, user_data};
  }
  if (on_event) {
    on_event(user_data, nid, WEF_NOTIFICATION_SHOWN, nullptr);
  }
  return nid;
}

extern "C" void Backend_CloseNotification_Linux(void* /*data*/,
                                                uint32_t notification_id) {
  wef_notification_event_fn fn = nullptr;
  void* ud = nullptr;
  {
    std::lock_guard<std::mutex> lock(NotifMutexLinux());
    auto it = NotifMapLinux().find(notification_id);
    if (it == NotifMapLinux().end())
      return;
    fn = it->second.on_event;
    ud = it->second.user_data;
    NotifMapLinux().erase(it);
  }
  // The OS notification has its own lifecycle — we can't programmatically
  // close it via notify-send. Fire the CLOSED callback so the runtime can
  // clean up its handler bookkeeping.
  if (fn)
    fn(ud, notification_id, WEF_NOTIFICATION_CLOSED, nullptr);
}
