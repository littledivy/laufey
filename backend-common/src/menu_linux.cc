// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// GtkMenu / GtkMenuBar construction from a laufey_value_t menu template.
// Each non-separator item carries its own click handler+data in a
// per-item GtkMenuCallbackData attached via g_signal_connect_data so
// the same builder serves application menus, context menus, dock-menu
// fallbacks (none on Linux today), and tray menus.

#include <gtk/gtk.h>

#include "laufey_backend_common.h"

#include <string>

namespace laufey_common {

namespace {

struct GtkMenuCallbackData {
  laufey_menu_click_fn on_click;
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

std::string DictString(const laufey_backend_api_t* api, laufey_value_t* dict,
                       const char* key) {
  laufey_value_t* v = api->value_dict_get(dict, key);
  if (!v || !api->value_is_string(v)) return std::string();
  size_t len = 0;
  char* s = api->value_get_string(v, &len);
  if (!s) return std::string();
  std::string out(s, len);
  api->value_free_string(s);
  return out;
}

}  // namespace

GtkWidget* BuildGtkMenuFromValue(laufey_value_t* val, const laufey_backend_api_t* api,
                                  uint32_t window_id,
                                  laufey_menu_click_fn on_click,
                                  void* on_click_data, bool is_menu_bar) {
  if (!val || !api->value_is_list(val)) return nullptr;

  GtkWidget* menu = is_menu_bar ? gtk_menu_bar_new() : gtk_menu_new();
  size_t count = api->value_list_size(val);

  for (size_t i = 0; i < count; ++i) {
    laufey_value_t* itemVal = api->value_list_get(val, i);
    if (!itemVal || !api->value_is_dict(itemVal)) continue;

    std::string typeStr = DictString(api, itemVal, "type");
    if (typeStr == "separator") {
      gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                            gtk_separator_menu_item_new());
      continue;
    }

    // Role-based items — GTK has no built-in role concept, so map roles
    // to labels and forward the role as the item id.
    std::string role = DictString(api, itemVal, "role");
    if (!role.empty()) {
      std::string label;
      if (role == "quit") label = "Quit";
      else if (role == "copy") label = "Copy";
      else if (role == "paste") label = "Paste";
      else if (role == "cut") label = "Cut";
      else if (role == "selectall" || role == "selectAll") label = "Select All";
      else if (role == "undo") label = "Undo";
      else if (role == "redo") label = "Redo";
      else if (role == "minimize") label = "Minimize";
      else if (role == "close") label = "Close";
      else if (role == "about") label = "About";
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

    std::string label = DictString(api, itemVal, "label");
    if (label.empty()) continue;

    laufey_value_t* submenuVal = api->value_dict_get(itemVal, "submenu");
    if (submenuVal && api->value_is_list(submenuVal)) {
      GtkWidget* parent = gtk_menu_item_new_with_label(label.c_str());
      GtkWidget* submenu = BuildGtkMenuFromValue(submenuVal, api, window_id,
                                                  on_click, on_click_data,
                                                  false);
      if (submenu)
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(parent), submenu);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), parent);
      continue;
    }

    std::string itemId = DictString(api, itemVal, "id");
    GtkWidget* gtkItem = gtk_menu_item_new_with_label(label.c_str());
    auto* cb_data = new GtkMenuCallbackData{on_click, on_click_data, window_id,
                                             itemId.empty() ? label : itemId};
    g_signal_connect_data(gtkItem, "activate",
                          G_CALLBACK(OnGtkMenuItemActivate), cb_data,
                          DestroyGtkMenuCallbackData, (GConnectFlags)0);

    laufey_value_t* enabledVal = api->value_dict_get(itemVal, "enabled");
    if (enabledVal && api->value_is_bool(enabledVal) &&
        !api->value_get_bool(enabledVal)) {
      gtk_widget_set_sensitive(gtkItem, FALSE);
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtkItem);
  }

  return menu;
}

}  // namespace laufey_common
