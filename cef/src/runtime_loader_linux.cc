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
#include "wef_backend_common.h"

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

// Menu-template → GtkMenu conversion lives in backend-common
// (wef_common::BuildGtkMenuFromValue).

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
            GtkWidget* menu = wef_common::BuildGtkMenuFromValue(
                tmpl, a, wid, cb, cb_data, false);
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
//
// Trampolines over backend-common/src/tray_linux.cc, which handles its
// own GTK-main-thread marshaling via g_idle_add. CreateTrayIcon
// allocates the id atomically and returns immediately.

uint32_t Backend_CreateTrayIcon_Linux(void* /*data*/) {
  return wef_common::CreateTrayIconLinux();
}
void Backend_DestroyTrayIcon_Linux(void* /*data*/, uint32_t tray_id) {
  wef_common::DestroyTrayIconLinux(tray_id);
}
void Backend_SetTrayIcon_Linux(void* /*data*/, uint32_t tray_id,
                               const void* png_bytes, size_t len) {
  wef_common::SetTrayIconLinux(tray_id, png_bytes, len);
}
void Backend_SetTrayTooltip_Linux(void* /*data*/, uint32_t tray_id,
                                  const char* tooltip_or_null) {
  wef_common::SetTrayTooltipLinux(tray_id, tooltip_or_null);
}
void Backend_SetTrayMenu_Linux(void* data, uint32_t tray_id,
                               wef_value_t* menu_template,
                               wef_menu_click_fn on_click,
                               void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  wef_common::SetTrayMenuLinux(tray_id, menu_template, &loader->GetBackendApi(),
                               on_click, on_click_data);
}
void Backend_SetTrayClickHandler_Linux(void* /*data*/, uint32_t tray_id,
                                       wef_tray_click_fn handler,
                                       void* user_data) {
  wef_common::SetTrayClickHandlerLinux(tray_id, handler, user_data);
}
void Backend_SetTrayDoubleClickHandler_Linux(void* /*data*/, uint32_t tray_id,
                                             wef_tray_click_fn handler,
                                             void* user_data) {
  wef_common::SetTrayDoubleClickHandlerLinux(tray_id, handler, user_data);
}
void Backend_SetTrayIconDark_Linux(void* /*data*/, uint32_t tray_id,
                                   const void* png_bytes, size_t len) {
  wef_common::SetTrayIconDarkLinux(tray_id, png_bytes, len);
}

// ---------------------------------------------------------------------------
// Notifications (Linux): thin trampolines over the shared notify-send
// implementation in backend-common/src/notifications_linux.cc. The whole
// state map / shell-escape / lifecycle dance lives there now.
// ---------------------------------------------------------------------------

extern "C" uint32_t Backend_ShowNotification_Linux(
    void* data, wef_value_t* options, wef_notification_event_fn on_event,
    void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  wef_common::NotificationOptions opts =
      wef_common::ParseNotificationOptions(options, &loader->GetBackendApi());
  return wef_common::ShowNotificationLinux(opts, on_event, user_data);
}

extern "C" void Backend_CloseNotification_Linux(void* /*data*/,
                                                uint32_t notification_id) {
  wef_common::CloseNotificationLinux(notification_id);
}
