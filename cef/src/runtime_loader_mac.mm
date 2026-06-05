// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
// macOS-specific RuntimeLoader parts: NSEvent monitors, NSMenu, NSWindow
// helpers.

#include "runtime_loader.h"
#include "laufey_backend_common.h"

#include <atomic>
#include <map>
#include <string>
#include <vector>

#import <Cocoa/Cocoa.h>
#import <UserNotifications/UserNotifications.h>
#import <objc/message.h>

// Defined in main_mac.mm — appends a default Edit submenu (Cut/Copy/Paste/
// Select All/Undo/Redo) to the given menubar if no submenu in the tree
// already exposes -copy:. Cmd+C/V on macOS dispatch via the main menu, so
// this has to be present for them to work at all.
extern void EnsureEditMenu(NSMenu* menubar);

// --- NSWindow Helpers (called from app.cc to avoid Obj-C in cross-platform
// code) ---

void SetNSWindowResizable(void* cef_handle, bool resizable) {
  NSView* view = (__bridge NSView*)cef_handle;
  NSWindow* nswindow = [view window];
  if (nswindow) {
    if (resizable) {
      nswindow.styleMask |= NSWindowStyleMaskResizable;
    } else {
      nswindow.styleMask &= ~NSWindowStyleMaskResizable;
    }
  }
}

bool IsNSWindowResizable(void* cef_handle) {
  NSView* view = (__bridge NSView*)cef_handle;
  NSWindow* nswindow = [view window];
  if (nswindow) {
    return (nswindow.styleMask & NSWindowStyleMaskResizable) != 0;
  }
  return true;
}

void ConfigureNSWindowAsPanelForCefHandle(void* cef_handle) {
  NSView* view = (__bridge NSView*)cef_handle;
  NSWindow* nswindow = [view window];
  if (!nswindow)
    return;
  // Float above normal windows, follow the active space, and stay visible
  // without taking part in window cycling — the behavior expected of a
  // menu-bar / tray popover.
  [nswindow setLevel:NSPopUpMenuWindowLevel];
  [nswindow setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces |
                                  NSWindowCollectionBehaviorTransient |
                                  NSWindowCollectionBehaviorIgnoresCycle];
  // Don't drop the panel when the owning app deactivates; the app manages
  // visibility itself (typically hiding on blur).
  [nswindow setHidesOnDeactivate:NO];
  // Showing the panel must not steal key focus from the foreground app. There
  // is no public NSWindow API for this on a non-NSPanel window, so use the
  // private -_setPreventsActivation: when available (invoked with a correctly
  // typed objc_msgSend since the arg is a BOOL). Degrades gracefully — the
  // window still floats and accepts first-mouse clicks without it.
  SEL prevent_sel = NSSelectorFromString(@"_setPreventsActivation:");
  if ([nswindow respondsToSelector:prevent_sel]) {
    using PreventsActivationFn = void (*)(id, SEL, BOOL);
    auto prevent = reinterpret_cast<PreventsActivationFn>(objc_msgSend);
    prevent(nswindow, prevent_sel, YES);
  }
}

void ConfigureNSWindowTransparentTitlebarForCefHandle(void* cef_handle) {
  NSView* view = (__bridge NSView*)cef_handle;
  NSWindow* nswindow = [view window];
  if (!nswindow)
    return;
  // Hidden/inset title bar: the content view extends the full height of the
  // window (under the title bar), the title bar itself is transparent and its
  // text hidden, so the web page draws into that strip with the standard
  // traffic-light buttons floating on top. Matches Electron's
  // `titleBarStyle: 'hidden'`. The window stays draggable by its title-bar
  // region; the app insets its own toolbar to clear the traffic lights.
  nswindow.styleMask |= NSWindowStyleMaskFullSizeContentView;
  nswindow.titlebarAppearsTransparent = YES;
  nswindow.titleVisibility = NSWindowTitleHidden;
}

void RegisterNSWindowForCefHandle(void* cef_handle, uint32_t window_id) {
  NSView* view = (__bridge NSView*)cef_handle;
  NSWindow* nswindow = [view window];
  if (nswindow) {
    RuntimeLoader::GetInstance()->RegisterNSWindow((__bridge void*)nswindow,
                                                   window_id);
  }
}

void UnregisterNSWindowForCefHandle(void* cef_handle) {
  NSView* view = (__bridge NSView*)cef_handle;
  NSWindow* nswindow = [view window];
  if (nswindow) {
    RuntimeLoader::GetInstance()->UnregisterNSWindow((__bridge void*)nswindow);
  }
}

// --- Native Mouse Monitor (macOS) ---

static id g_mouse_monitor = nil;
static id g_mouse_move_monitor = nil;
static id g_scroll_monitor = nil;
static id g_focus_observer = nil;
static id g_blur_observer = nil;
static id g_resize_observer = nil;
static id g_move_observer = nil;

static uint32_t NSModifierFlagsToLaufey(NSEventModifierFlags flags) {
  uint32_t mods = 0;
  if (flags & NSEventModifierFlagShift)
    mods |= LAUFEY_MOD_SHIFT;
  if (flags & NSEventModifierFlagControl)
    mods |= LAUFEY_MOD_CONTROL;
  if (flags & NSEventModifierFlagOption)
    mods |= LAUFEY_MOD_ALT;
  if (flags & NSEventModifierFlagCommand)
    mods |= LAUFEY_MOD_META;
  return mods;
}

static int NSButtonToLaufey(NSInteger buttonNumber) {
  switch (buttonNumber) {
    case 0:
      return LAUFEY_MOUSE_BUTTON_LEFT;
    case 1:
      return LAUFEY_MOUSE_BUTTON_RIGHT;
    case 2:
      return LAUFEY_MOUSE_BUTTON_MIDDLE;
    case 3:
      return LAUFEY_MOUSE_BUTTON_BACK;
    case 4:
      return LAUFEY_MOUSE_BUTTON_FORWARD;
    default:
      return LAUFEY_MOUSE_BUTTON_LEFT;
  }
}

static uint32_t LaufeyIdForNSWindow(NSWindow* win) {
  if (!win)
    return 0;
  return RuntimeLoader::GetInstance()->GetLaufeyIdForNSWindow((__bridge void*)win);
}

// Per-window menu storage (must be declared before focus observer uses them)
static std::map<uint32_t, NSMenu*> g_window_menus;
static std::mutex g_window_menus_mutex;

void InstallNativeMouseMonitor() {
  if (g_mouse_monitor)
    return;

  NSEventMask mask = NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp |
                     NSEventMaskRightMouseDown | NSEventMaskRightMouseUp |
                     NSEventMaskOtherMouseDown | NSEventMaskOtherMouseUp;

  g_mouse_monitor = [NSEvent
      addLocalMonitorForEventsMatchingMask:mask
                                   handler:^NSEvent*(NSEvent* event) {
                                     NSWindow* win = [event window];
                                     uint32_t wid = LaufeyIdForNSWindow(win);
                                     if (wid == 0)
                                       return event;

                                     int state;
                                     switch ([event type]) {
                                       case NSEventTypeLeftMouseDown:
                                       case NSEventTypeRightMouseDown:
                                       case NSEventTypeOtherMouseDown:
                                         state = LAUFEY_MOUSE_PRESSED;
                                         break;
                                       default:
                                         state = LAUFEY_MOUSE_RELEASED;
                                         break;
                                     }

                                     int button =
                                         NSButtonToLaufey([event buttonNumber]);
                                     uint32_t modifiers = NSModifierFlagsToLaufey(
                                         [event modifierFlags]);
                                     int32_t click_count =
                                         (int32_t)[event clickCount];

                                     NSPoint loc = [event locationInWindow];
                                     double x = loc.x;
                                     double y = 0;
                                     if (win) {
                                       y = [win contentLayoutRect].size.height -
                                           loc.y;
                                     }

                                     RuntimeLoader::GetInstance()
                                         ->DispatchMouseClickEvent(
                                             wid, state, button, x, y,
                                             modifiers, click_count);

                                     return event;
                                   }];

  g_mouse_move_monitor = [NSEvent
      addLocalMonitorForEventsMatchingMask:(NSEventMaskMouseMoved |
                                            NSEventMaskLeftMouseDragged |
                                            NSEventMaskRightMouseDragged |
                                            NSEventMaskOtherMouseDragged)
                                   handler:^NSEvent*(NSEvent* event) {
                                     NSWindow* win = [event window];
                                     uint32_t wid = LaufeyIdForNSWindow(win);
                                     if (wid == 0)
                                       return event;

                                     uint32_t modifiers = NSModifierFlagsToLaufey(
                                         [event modifierFlags]);
                                     NSPoint loc = [event locationInWindow];
                                     double x = loc.x;
                                     double y = 0;
                                     if (win) {
                                       y = [win contentLayoutRect].size.height -
                                           loc.y;
                                     }

                                     RuntimeLoader::GetInstance()
                                         ->DispatchMouseMoveEvent(wid, x, y,
                                                                  modifiers);
                                     return event;
                                   }];

  g_scroll_monitor = [NSEvent
      addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel
                                   handler:^NSEvent*(NSEvent* event) {
                                     NSWindow* win = [event window];
                                     uint32_t wid = LaufeyIdForNSWindow(win);
                                     if (wid == 0)
                                       return event;

                                     double delta_x = [event scrollingDeltaX];
                                     double delta_y = [event scrollingDeltaY];
                                     uint32_t modifiers = NSModifierFlagsToLaufey(
                                         [event modifierFlags]);

                                     int32_t delta_mode =
                                         [event hasPreciseScrollingDeltas]
                                             ? LAUFEY_WHEEL_DELTA_PIXEL
                                             : LAUFEY_WHEEL_DELTA_LINE;

                                     NSPoint loc = [event locationInWindow];
                                     double x = loc.x;
                                     double y = 0;
                                     if (win) {
                                       y = [win contentLayoutRect].size.height -
                                           loc.y;
                                     }

                                     RuntimeLoader::GetInstance()
                                         ->DispatchWheelEvent(
                                             wid, delta_x, delta_y, x, y,
                                             modifiers, delta_mode);
                                     return event;
                                   }];

  g_focus_observer = [[NSNotificationCenter defaultCenter]
      addObserverForName:NSWindowDidBecomeKeyNotification
                  object:nil
                   queue:nil
              usingBlock:^(NSNotification* note) {
                uint32_t wid = LaufeyIdForNSWindow([note object]);
                if (wid > 0) {
                  RuntimeLoader::GetInstance()->DispatchFocusedEvent(wid, 1);
                  // Swap to this window's menu
                  std::lock_guard<std::mutex> lock(g_window_menus_mutex);
                  auto it = g_window_menus.find(wid);
                  if (it != g_window_menus.end() && it->second) {
                    [NSApp setMainMenu:it->second];
                  }
                }
              }];

  g_blur_observer = [[NSNotificationCenter defaultCenter]
      addObserverForName:NSWindowDidResignKeyNotification
                  object:nil
                   queue:nil
              usingBlock:^(NSNotification* note) {
                uint32_t wid = LaufeyIdForNSWindow([note object]);
                if (wid > 0) {
                  RuntimeLoader::GetInstance()->DispatchFocusedEvent(wid, 0);
                }
              }];

  g_resize_observer = [[NSNotificationCenter defaultCenter]
      addObserverForName:NSWindowDidResizeNotification
                  object:nil
                   queue:nil
              usingBlock:^(NSNotification* note) {
                NSWindow* win = [note object];
                uint32_t wid = LaufeyIdForNSWindow(win);
                if (wid > 0 && win) {
                  NSRect frame = [[win contentView] frame];
                  RuntimeLoader::GetInstance()->DispatchResizeEvent(
                      wid, (int)frame.size.width, (int)frame.size.height);
                }
              }];

  g_move_observer = [[NSNotificationCenter defaultCenter]
      addObserverForName:NSWindowDidMoveNotification
                  object:nil
                   queue:nil
              usingBlock:^(NSNotification* note) {
                NSWindow* win = [note object];
                uint32_t wid = LaufeyIdForNSWindow(win);
                if (wid > 0 && win) {
                  NSRect frame = [win frame];
                  RuntimeLoader::GetInstance()->DispatchMoveEvent(
                      wid, (int)frame.origin.x, (int)frame.origin.y);
                }
              }];
}

void RemoveNativeMouseMonitor() {
  if (g_mouse_monitor) {
    [NSEvent removeMonitor:g_mouse_monitor];
    g_mouse_monitor = nil;
  }
  if (g_mouse_move_monitor) {
    [NSEvent removeMonitor:g_mouse_move_monitor];
    g_mouse_move_monitor = nil;
  }
  if (g_scroll_monitor) {
    [NSEvent removeMonitor:g_scroll_monitor];
    g_scroll_monitor = nil;
  }
  if (g_focus_observer) {
    [[NSNotificationCenter defaultCenter] removeObserver:g_focus_observer];
    g_focus_observer = nil;
  }
  if (g_blur_observer) {
    [[NSNotificationCenter defaultCenter] removeObserver:g_blur_observer];
    g_blur_observer = nil;
  }
  if (g_resize_observer) {
    [[NSNotificationCenter defaultCenter] removeObserver:g_resize_observer];
    g_resize_observer = nil;
  }
  if (g_move_observer) {
    [[NSNotificationCenter defaultCenter] removeObserver:g_move_observer];
    g_move_observer = nil;
  }
}

// --- JS Dialog for CEF (macOS) ---
// Called from app.cc's OnJSDialog handler. Runs synchronously on the UI thread.

struct NativeDialogResult {
  bool confirmed;
  std::string text;
};

NativeDialogResult ShowNativeJSDialog_Mac(int type, const std::string& message,
                                          const std::string& default_text) {
  NativeDialogResult result{false, ""};
  // type: 0=alert, 1=confirm, 2=prompt
  @autoreleasepool {
    NSAlert* alert = [[NSAlert alloc] init];
    [alert setMessageText:[NSString stringWithUTF8String:message.c_str()]];
    [alert addButtonWithTitle:@"OK"];
    if (type >= 1) {
      [alert addButtonWithTitle:@"Cancel"];
    }
    [alert setAlertStyle:NSAlertStyleInformational];

    NSTextField* input = nil;
    if (type == 2) {
      input = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
      [input
          setStringValue:[NSString stringWithUTF8String:default_text.c_str()]];
      [alert setAccessoryView:input];
      [alert.window setInitialFirstResponder:input];
    }

    NSModalResponse response = [alert runModal];
    result.confirmed = (response == NSAlertFirstButtonReturn);
    if (type == 2 && result.confirmed && input) {
      result.text = [[input stringValue] UTF8String];
    }
  }
  return result;
}

// --- Application Menu / Context Menu (macOS) ---
//
// Menu construction lives in backend-common (laufey_common::BuildNSMenuFromValue).

void Backend_ShowContextMenu_Mac(void* data, uint32_t window_id, int x, int y,
                                 laufey_value_t* menu_template,
                                 laufey_menu_click_fn on_click,
                                 void* on_click_data) {
  if (!menu_template)
    return;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const laufey_backend_api_t* api = &loader->GetBackendApi();

  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (!browser)
    return;

  void* handle = browser->GetHost()->GetWindowHandle();
  dispatch_async(dispatch_get_main_queue(), ^{
    NSMenu* menu = laufey_common::BuildNSMenuFromValue(
        menu_template, api, on_click, on_click_data, window_id);
    if (!menu)
      return;

    NSView* view = (__bridge NSView*)handle;
    NSWindow* win = [view window];
    if (!win)
      return;

    NSView* contentView = [win contentView];
    // Convert from top-left origin (laufey coordinates) to bottom-left origin
    // (NSView)
    NSPoint loc = NSMakePoint(x, [contentView frame].size.height - y);
    [menu popUpMenuPositioningItem:nil atLocation:loc inView:contentView];
  });
}

void Backend_SetApplicationMenu_Mac(void* data, uint32_t window_id,
                                    laufey_value_t* menu_template,
                                    laufey_menu_click_fn on_click,
                                    void* on_click_data) {
  if (!menu_template)
    return;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const laufey_backend_api_t* api = &loader->GetBackendApi();
  dispatch_async(dispatch_get_main_queue(), ^{
    NSMenu* menubar = laufey_common::BuildNSMenuFromValue(
        menu_template, api, on_click, on_click_data, window_id);
    if (menubar) {
      EnsureEditMenu(menubar);
      // Store per-window
      {
        std::lock_guard<std::mutex> lock(g_window_menus_mutex);
        g_window_menus[window_id] = menubar;
      }
      // If this window is currently key, apply immediately
      uint32_t keyWid = LaufeyIdForNSWindow([NSApp keyWindow]);
      if (keyWid == window_id) {
        [NSApp setMainMenu:menubar];
      }
    }
  });
}

// --- Dock (macOS) ---

void Backend_SetDockBadge_Mac(void* /*data*/, const char* badge_or_null) {
  laufey_common::SetDockBadgeMac(badge_or_null);
}

void Backend_BounceDock_Mac(void* /*data*/, int type) {
  laufey_common::BounceDockMac(type);
}

void Backend_SetDockMenu_Mac(void* data, laufey_value_t* menu_template,
                             laufey_menu_click_fn on_click, void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const laufey_backend_api_t* api = &loader->GetBackendApi();
  if (!menu_template) {
    dispatch_async(dispatch_get_main_queue(), ^{
      laufey_common::SetDockMenuMac(nil);
    });
    return;
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    // window_id = 0 because dock menu is app-scoped.
    NSMenu* menu = laufey_common::BuildNSMenuFromValue(menu_template, api,
                                                    on_click, on_click_data, 0);
    laufey_common::SetDockMenuMac(menu);
  });
}

void Backend_SetDockVisible_Mac(void* /*data*/, bool visible) {
  laufey_common::SetDockVisibleMac(visible);
}

void Backend_SetDockReopenHandler_Mac(void* /*data*/,
                                      laufey_dock_reopen_fn handler,
                                      void* user_data) {
  laufey_common::SetDockReopenHandlerMac(handler, user_data);
}

// --- Tray / status-bar icon (macOS) ---
//
// Thin trampolines over backend-common/src/tray_mac.mm.

uint32_t Backend_CreateTrayIcon_Mac(void* /*data*/) {
  return laufey_common::CreateTrayIconMac();
}

void Backend_DestroyTrayIcon_Mac(void* /*data*/, uint32_t tray_id) {
  laufey_common::DestroyTrayIconMac(tray_id);
}

void Backend_SetTrayIcon_Mac(void* /*data*/, uint32_t tray_id,
                             const void* png_bytes, size_t len) {
  laufey_common::SetTrayIconMac(tray_id, png_bytes, len);
}

void Backend_SetTrayIconDark_Mac(void* /*data*/, uint32_t tray_id,
                                 const void* png_bytes, size_t len) {
  laufey_common::SetTrayIconDarkMac(tray_id, png_bytes, len);
}

bool Backend_GetTrayIconBounds_Mac(void* /*data*/, uint32_t tray_id, int* x,
                                   int* y, int* width, int* height) {
  return laufey_common::GetTrayIconBoundsMac(tray_id, x, y, width, height);
}

void Backend_SetTrayTooltip_Mac(void* /*data*/, uint32_t tray_id,
                                const char* tooltip_or_null) {
  laufey_common::SetTrayTooltipMac(tray_id, tooltip_or_null);
}

void Backend_SetTrayMenu_Mac(void* data, uint32_t tray_id,
                             laufey_value_t* menu_template,
                             laufey_menu_click_fn on_click, void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  laufey_common::SetTrayMenuMac(tray_id, menu_template, &loader->GetBackendApi(),
                             on_click, on_click_data);
}

void Backend_SetTrayClickHandler_Mac(void* /*data*/, uint32_t tray_id,
                                     laufey_tray_click_fn handler,
                                     void* user_data) {
  laufey_common::SetTrayClickHandlerMac(tray_id, handler, user_data);
}

void Backend_SetTrayDoubleClickHandler_Mac(void* /*data*/, uint32_t tray_id,
                                           laufey_tray_click_fn handler,
                                           void* user_data) {
  laufey_common::SetTrayDoubleClickHandlerMac(tray_id, handler, user_data);
}

// --- Notifications (macOS) ---
//
// Thin trampolines over backend-common/src/notifications_mac.mm
// (UNUserNotificationCenter-backed).

uint32_t Backend_ShowNotification_Mac(void* data, laufey_value_t* options,
                                      laufey_notification_event_fn on_event,
                                      void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  laufey_common::NotificationOptions opts =
      laufey_common::ParseNotificationOptions(options, &loader->GetBackendApi());
  return laufey_common::ShowNotificationMac(opts, on_event, user_data);
}

void Backend_CloseNotification_Mac(void* /*data*/, uint32_t notification_id) {
  laufey_common::CloseNotificationMac(notification_id);
}

// --- Permissions (UNUserNotificationCenter) ---
//
// UN is the modern (10.14+) replacement for NSUserNotification's
// implicit "always granted" model. It requires the process to run
// inside a bundled .app with a CFBundleIdentifier; without one
// `getNotificationSettings:` returns garbage and `requestAuthorization:`
// fails immediately. We detect that case and report UNSUPPORTED so the
// embedder (Deno) can branch on it instead of seeing a phantom DENIED.

// --- Permissions (macOS) ---
//
// Thin trampolines over backend-common/src/permissions_mac.mm.

void Backend_QueryPermission_Mac(void* /*data*/, int kind,
                                 laufey_permission_callback_fn cb,
                                 void* user_data) {
  laufey_common::QueryPermissionMac(kind, cb, user_data);
}

void Backend_RequestPermission_Mac(void* /*data*/, int kind,
                                   laufey_permission_callback_fn cb,
                                   void* user_data) {
  laufey_common::RequestPermissionMac(kind, cb, user_data);
}
