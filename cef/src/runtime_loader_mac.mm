// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
// macOS-specific RuntimeLoader parts: NSEvent monitors, NSMenu, NSWindow
// helpers.

#include "runtime_loader.h"

#include <atomic>
#include <map>
#include <string>
#include <vector>

#import <Cocoa/Cocoa.h>
#import <UserNotifications/UserNotifications.h>

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

static uint32_t NSModifierFlagsToWef(NSEventModifierFlags flags) {
  uint32_t mods = 0;
  if (flags & NSEventModifierFlagShift)
    mods |= WEF_MOD_SHIFT;
  if (flags & NSEventModifierFlagControl)
    mods |= WEF_MOD_CONTROL;
  if (flags & NSEventModifierFlagOption)
    mods |= WEF_MOD_ALT;
  if (flags & NSEventModifierFlagCommand)
    mods |= WEF_MOD_META;
  return mods;
}

static int NSButtonToWef(NSInteger buttonNumber) {
  switch (buttonNumber) {
    case 0:
      return WEF_MOUSE_BUTTON_LEFT;
    case 1:
      return WEF_MOUSE_BUTTON_RIGHT;
    case 2:
      return WEF_MOUSE_BUTTON_MIDDLE;
    case 3:
      return WEF_MOUSE_BUTTON_BACK;
    case 4:
      return WEF_MOUSE_BUTTON_FORWARD;
    default:
      return WEF_MOUSE_BUTTON_LEFT;
  }
}

static uint32_t WefIdForNSWindow(NSWindow* win) {
  if (!win)
    return 0;
  return RuntimeLoader::GetInstance()->GetWefIdForNSWindow((__bridge void*)win);
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
                                     uint32_t wid = WefIdForNSWindow(win);
                                     if (wid == 0)
                                       return event;

                                     int state;
                                     switch ([event type]) {
                                       case NSEventTypeLeftMouseDown:
                                       case NSEventTypeRightMouseDown:
                                       case NSEventTypeOtherMouseDown:
                                         state = WEF_MOUSE_PRESSED;
                                         break;
                                       default:
                                         state = WEF_MOUSE_RELEASED;
                                         break;
                                     }

                                     int button =
                                         NSButtonToWef([event buttonNumber]);
                                     uint32_t modifiers = NSModifierFlagsToWef(
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
                                     uint32_t wid = WefIdForNSWindow(win);
                                     if (wid == 0)
                                       return event;

                                     uint32_t modifiers = NSModifierFlagsToWef(
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
                                     uint32_t wid = WefIdForNSWindow(win);
                                     if (wid == 0)
                                       return event;

                                     double delta_x = [event scrollingDeltaX];
                                     double delta_y = [event scrollingDeltaY];
                                     uint32_t modifiers = NSModifierFlagsToWef(
                                         [event modifierFlags]);

                                     int32_t delta_mode =
                                         [event hasPreciseScrollingDeltas]
                                             ? WEF_WHEEL_DELTA_PIXEL
                                             : WEF_WHEEL_DELTA_LINE;

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
                uint32_t wid = WefIdForNSWindow([note object]);
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
                uint32_t wid = WefIdForNSWindow([note object]);
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
                uint32_t wid = WefIdForNSWindow(win);
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
                uint32_t wid = WefIdForNSWindow(win);
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

// --- Native Dialog (macOS) ---

int ShowNativeDialog_Mac(int dialog_type, const char* title,
                         const char* message, const char* default_value,
                         char** out_input_value) {
  if (out_input_value)
    *out_input_value = nullptr;
  NSString* nsTitle = title ? [NSString stringWithUTF8String:title] : @"";
  NSString* nsMessage = message ? [NSString stringWithUTF8String:message] : @"";
  NSString* nsDefault =
      default_value ? [NSString stringWithUTF8String:default_value] : @"";

  // `runModal` itself spins NSRunLoop, pumping AppKit events for other
  // CEF windows while the dialog is up. Done on main directly when the
  // caller is already there (the typical case for the Deno runtime),
  // otherwise dispatch_sync forwards.
  __block bool confirmed = false;
  __block char* input_strdup = nullptr;
  void (^body)(void) = ^{
    NSAlert* alert = [[NSAlert alloc] init];
    [alert setMessageText:nsTitle];
    [alert setInformativeText:nsMessage];

    NSTextField* inputField = nil;

    if (dialog_type == WEF_DIALOG_ALERT) {
      [alert addButtonWithTitle:@"OK"];
    } else if (dialog_type == WEF_DIALOG_CONFIRM) {
      [alert addButtonWithTitle:@"OK"];
      [alert addButtonWithTitle:@"Cancel"];
    } else if (dialog_type == WEF_DIALOG_PROMPT) {
      [alert addButtonWithTitle:@"OK"];
      [alert addButtonWithTitle:@"Cancel"];
      inputField =
          [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
      [inputField setStringValue:nsDefault];
      [alert setAccessoryView:inputField];
      [alert layout];
      [[alert window] makeFirstResponder:inputField];
    }

    NSModalResponse response = [alert runModal];
    confirmed = (response == NSAlertFirstButtonReturn);
    if (dialog_type == WEF_DIALOG_PROMPT && confirmed && inputField &&
        out_input_value) {
      const char* text = [[inputField stringValue] UTF8String];
      if (text)
        input_strdup = strdup(text);
    }
  };
  if ([NSThread isMainThread]) {
    body();
  } else {
    dispatch_sync(dispatch_get_main_queue(), body);
  }
  if (out_input_value)
    *out_input_value = input_strdup;
  return confirmed ? 1 : 0;
}

// --- Application Menu (macOS) ---

static wef_menu_click_fn g_menu_click_fn = nullptr;
static void* g_menu_click_data = nullptr;

@interface WefMenuTarget : NSObject
+ (instancetype)shared;
- (void)menuItemClicked:(id)sender;
@end

@implementation WefMenuTarget

+ (instancetype)shared {
  static WefMenuTarget* instance = nil;
  static dispatch_once_t onceToken;
  dispatch_once(&onceToken, ^{
    instance = [[WefMenuTarget alloc] init];
  });
  return instance;
}

- (void)menuItemClicked:(id)sender {
  NSMenuItem* item = (NSMenuItem*)sender;
  NSString* itemId = [item representedObject];
  if (itemId && g_menu_click_fn) {
    uint32_t wid = WefIdForNSWindow([NSApp keyWindow]);
    g_menu_click_fn(g_menu_click_data, wid, [itemId UTF8String]);
  }
}

@end

static void ParseAccelerator(const std::string& accel, NSString** outKey,
                             NSEventModifierFlags* outMask) {
  *outKey = @"";
  *outMask = 0;

  std::string lower = accel;
  for (auto& c : lower)
    c = tolower(c);

  size_t pos = 0;
  std::vector<std::string> parts;
  std::string remaining = lower;
  while ((pos = remaining.find('+')) != std::string::npos) {
    parts.push_back(remaining.substr(0, pos));
    remaining = remaining.substr(pos + 1);
  }
  if (!remaining.empty())
    parts.push_back(remaining);

  for (const auto& part : parts) {
    if (part == "cmd" || part == "command" || part == "cmdorctrl" ||
        part == "commandorcontrol") {
      *outMask |= NSEventModifierFlagCommand;
    } else if (part == "shift") {
      *outMask |= NSEventModifierFlagShift;
    } else if (part == "alt" || part == "option") {
      *outMask |= NSEventModifierFlagOption;
    } else if (part == "ctrl" || part == "control") {
      *outMask |= NSEventModifierFlagControl;
    } else {
      *outKey = [NSString stringWithUTF8String:part.c_str()];
    }
  }
}

static NSMenuItem* CreateRoleMenuItem(const std::string& role) {
  NSString* title = @"";
  SEL action = nil;
  NSString* keyEquiv = @"";
  NSEventModifierFlags mask = NSEventModifierFlagCommand;

  if (role == "quit") {
    title = @"Quit";
    action = @selector(terminate:);
    keyEquiv = @"q";
  } else if (role == "copy") {
    title = @"Copy";
    action = @selector(copy:);
    keyEquiv = @"c";
  } else if (role == "paste") {
    title = @"Paste";
    action = @selector(paste:);
    keyEquiv = @"v";
  } else if (role == "cut") {
    title = @"Cut";
    action = @selector(cut:);
    keyEquiv = @"x";
  } else if (role == "selectall" || role == "selectAll") {
    title = @"Select All";
    action = @selector(selectAll:);
    keyEquiv = @"a";
  } else if (role == "undo") {
    title = @"Undo";
    action = @selector(undo:);
    keyEquiv = @"z";
  } else if (role == "redo") {
    title = @"Redo";
    action = @selector(redo:);
    keyEquiv = @"Z";
    mask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
  } else if (role == "minimize") {
    title = @"Minimize";
    action = @selector(performMiniaturize:);
    keyEquiv = @"m";
  } else if (role == "zoom") {
    title = @"Zoom";
    action = @selector(performZoom:);
  } else if (role == "close") {
    title = @"Close";
    action = @selector(performClose:);
    keyEquiv = @"w";
  } else if (role == "about") {
    title = @"About";
    action = @selector(orderFrontStandardAboutPanel:);
  } else if (role == "hide") {
    title = @"Hide";
    action = @selector(hide:);
    keyEquiv = @"h";
  } else if (role == "hideothers" || role == "hideOthers") {
    title = @"Hide Others";
    action = @selector(hideOtherApplications:);
    keyEquiv = @"h";
    mask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
  } else if (role == "unhide") {
    title = @"Show All";
    action = @selector(unhideAllApplications:);
  } else if (role == "front") {
    title = @"Bring All to Front";
    action = @selector(arrangeInFront:);
  } else if (role == "togglefullscreen" || role == "toggleFullScreen") {
    title = @"Toggle Full Screen";
    action = @selector(toggleFullScreen:);
    keyEquiv = @"f";
    mask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
  } else {
    return nil;
  }

  NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                action:action
                                         keyEquivalent:keyEquiv];
  [item setKeyEquivalentModifierMask:mask];
  return item;
}

static NSMenu* BuildMenuFromValue(wef_value_t* val,
                                  const wef_backend_api_t* api, id target,
                                  SEL action) {
  if (!val || !api->value_is_list(val))
    return nil;
  NSMenu* menu = [[NSMenu alloc] init];
  [menu setAutoenablesItems:NO];
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
        [menu addItem:[NSMenuItem separatorItem]];
        api->value_free_string(typeStr);
        continue;
      }
      if (typeStr)
        api->value_free_string(typeStr);
    }
    wef_value_t* roleVal = api->value_dict_get(itemVal, "role");
    if (roleVal && api->value_is_string(roleVal)) {
      size_t len = 0;
      char* roleStr = api->value_get_string(roleVal, &len);
      if (roleStr) {
        NSMenuItem* roleItem = CreateRoleMenuItem(roleStr);
        if (roleItem)
          [menu addItem:roleItem];
        api->value_free_string(roleStr);
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
    NSString* label = [NSString stringWithUTF8String:labelStr];
    api->value_free_string(labelStr);
    wef_value_t* submenuVal = api->value_dict_get(itemVal, "submenu");
    if (submenuVal && api->value_is_list(submenuVal)) {
      NSMenuItem* submenuItem = [[NSMenuItem alloc] init];
      [submenuItem setTitle:label];
      NSMenu* submenu = BuildMenuFromValue(submenuVal, api, target, action);
      [submenu setTitle:label];
      [submenuItem setSubmenu:submenu];
      [menu addItem:submenuItem];
      continue;
    }
    NSString* keyEquiv = @"";
    NSEventModifierFlags modMask = NSEventModifierFlagCommand;
    wef_value_t* accelVal = api->value_dict_get(itemVal, "accelerator");
    if (accelVal && api->value_is_string(accelVal)) {
      size_t accelLen = 0;
      char* accelStr = api->value_get_string(accelVal, &accelLen);
      if (accelStr) {
        ParseAccelerator(accelStr, &keyEquiv, &modMask);
        api->value_free_string(accelStr);
      }
    }
    NSMenuItem* nsItem = [[NSMenuItem alloc] initWithTitle:label
                                                    action:action
                                             keyEquivalent:keyEquiv];
    [nsItem setKeyEquivalentModifierMask:modMask];
    [nsItem setTarget:target];
    wef_value_t* idVal = api->value_dict_get(itemVal, "id");
    if (idVal && api->value_is_string(idVal)) {
      size_t idLen = 0;
      char* idStr = api->value_get_string(idVal, &idLen);
      if (idStr) {
        [nsItem setRepresentedObject:[NSString stringWithUTF8String:idStr]];
        api->value_free_string(idStr);
      }
    }
    wef_value_t* enabledVal = api->value_dict_get(itemVal, "enabled");
    if (enabledVal && api->value_is_bool(enabledVal)) {
      [nsItem setEnabled:api->value_get_bool(enabledVal)];
    } else {
      [nsItem setEnabled:YES];
    }
    [menu addItem:nsItem];
  }
  return menu;
}

// Exported function called from runtime_loader.cc on macOS
void Backend_ShowContextMenu_Mac(void* data, uint32_t window_id, int x, int y,
                                 wef_value_t* menu_template,
                                 wef_menu_click_fn on_click,
                                 void* on_click_data) {
  if (!menu_template)
    return;
  g_menu_click_fn = on_click;
  g_menu_click_data = on_click_data;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const wef_backend_api_t* api = &loader->GetBackendApi();

  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (!browser)
    return;

  void* handle = browser->GetHost()->GetWindowHandle();
  dispatch_async(dispatch_get_main_queue(), ^{
    NSMenu* menu =
        BuildMenuFromValue(menu_template, api, [WefMenuTarget shared],
                           @selector(menuItemClicked:));
    if (!menu)
      return;

    NSView* view = (__bridge NSView*)handle;
    NSWindow* win = [view window];
    if (!win)
      return;

    NSView* contentView = [win contentView];
    // Convert from top-left origin (wef coordinates) to bottom-left origin
    // (NSView)
    NSPoint loc = NSMakePoint(x, [contentView frame].size.height - y);
    [menu popUpMenuPositioningItem:nil atLocation:loc inView:contentView];
  });
}

void Backend_SetApplicationMenu_Mac(void* data, uint32_t window_id,
                                    wef_value_t* menu_template,
                                    wef_menu_click_fn on_click,
                                    void* on_click_data) {
  if (!menu_template)
    return;
  g_menu_click_fn = on_click;
  g_menu_click_data = on_click_data;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const wef_backend_api_t* api = &loader->GetBackendApi();
  dispatch_async(dispatch_get_main_queue(), ^{
    NSMenu* menubar =
        BuildMenuFromValue(menu_template, api, [WefMenuTarget shared],
                           @selector(menuItemClicked:));
    if (menubar) {
      EnsureEditMenu(menubar);
      // Store per-window
      {
        std::lock_guard<std::mutex> lock(g_window_menus_mutex);
        g_window_menus[window_id] = menubar;
      }
      // If this window is currently key, apply immediately
      uint32_t keyWid = WefIdForNSWindow([NSApp keyWindow]);
      if (keyWid == window_id) {
        [NSApp setMainMenu:menubar];
      }
    }
  });
}

// --- Dock (macOS) ---

// Dock menu + reopen handler storage (consumed by WefAppDelegate in
// main_mac.mm — declared extern there).
NSMenu* g_dock_menu = nil;
static wef_menu_click_fn g_dock_click_fn = nullptr;
static void* g_dock_click_data = nullptr;
wef_dock_reopen_fn g_dock_reopen_fn = nullptr;
void* g_dock_reopen_data = nullptr;

@interface WefDockMenuTarget : NSObject
+ (instancetype)shared;
- (void)dockMenuItemClicked:(id)sender;
@end

@implementation WefDockMenuTarget

+ (instancetype)shared {
  static WefDockMenuTarget* instance = nil;
  static dispatch_once_t onceToken;
  dispatch_once(&onceToken, ^{
    instance = [[WefDockMenuTarget alloc] init];
  });
  return instance;
}

- (void)dockMenuItemClicked:(id)sender {
  NSMenuItem* item = (NSMenuItem*)sender;
  NSString* itemId = [item representedObject];
  if (itemId && g_dock_click_fn) {
    // window_id = 0 because dock menu is app-scoped.
    g_dock_click_fn(g_dock_click_data, 0, [itemId UTF8String]);
  }
}

@end

void Backend_SetDockBadge_Mac(void* /*data*/, const char* badge_or_null) {
  NSString* ns = badge_or_null && *badge_or_null
                     ? [NSString stringWithUTF8String:badge_or_null]
                     : nil;
  dispatch_async(dispatch_get_main_queue(), ^{
    NSDockTile* tile = [NSApp dockTile];
    [tile setBadgeLabel:ns];
    [tile display];
    NSRunningApplication* me = [NSRunningApplication currentApplication];
    NSLog(@"[wef-debug] policy=%ld active=%d hidden=%d finishedLaunching=%d "
          @"bundleURL=%@ bundleId=%@ owns_dock_tile=%d badge_after=%@",
          (long)[NSApp activationPolicy], me.active, me.hidden,
          me.finishedLaunching, me.bundleURL, me.bundleIdentifier,
          (tile == [NSApp dockTile]), tile.badgeLabel);
  });
}

void Backend_BounceDock_Mac(void* /*data*/, int type) {
  NSRequestUserAttentionType t = (type == WEF_DOCK_BOUNCE_CRITICAL)
                                     ? NSCriticalRequest
                                     : NSInformationalRequest;
  dispatch_async(dispatch_get_main_queue(), ^{
    [NSApp requestUserAttention:t];
  });
}

void Backend_SetDockMenu_Mac(void* data, wef_value_t* menu_template,
                             wef_menu_click_fn on_click, void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const wef_backend_api_t* api = &loader->GetBackendApi();
  if (!menu_template) {
    dispatch_async(dispatch_get_main_queue(), ^{
      g_dock_menu = nil;
      g_dock_click_fn = nullptr;
      g_dock_click_data = nullptr;
    });
    return;
  }
  g_dock_click_fn = on_click;
  g_dock_click_data = on_click_data;
  dispatch_async(dispatch_get_main_queue(), ^{
    NSMenu* menu =
        BuildMenuFromValue(menu_template, api, [WefDockMenuTarget shared],
                           @selector(dockMenuItemClicked:));
    g_dock_menu = menu;
  });
}

void Backend_SetDockVisible_Mac(void* /*data*/, bool visible) {
  NSApplicationActivationPolicy policy =
      visible ? NSApplicationActivationPolicyRegular
              : NSApplicationActivationPolicyAccessory;
  dispatch_async(dispatch_get_main_queue(), ^{
    [NSApp setActivationPolicy:policy];
  });
}

void Backend_SetDockReopenHandler_Mac(void* /*data*/,
                                      wef_dock_reopen_fn handler,
                                      void* user_data) {
  g_dock_reopen_fn = handler;
  g_dock_reopen_data = user_data;
}

// --- Tray / status-bar icon (macOS) ---

struct TrayEntry {
  NSStatusItem* item;
  NSMenu* menu;
  wef_menu_click_fn menu_click_fn;
  void* menu_click_data;
  wef_tray_click_fn click_fn;
  void* click_data;
  wef_tray_click_fn dblclick_fn;
  void* dblclick_data;
  NSImage* light_image;
  NSImage* dark_image;
};

static std::map<uint32_t, TrayEntry>& TrayMap() {
  static std::map<uint32_t, TrayEntry> map;
  return map;
}

static std::atomic<uint32_t> g_next_tray_id{1};

@interface WefTrayTarget : NSObject
+ (instancetype)shared;
- (void)trayClicked:(id)sender;
- (void)trayMenuItemClicked:(id)sender;
@end

@implementation WefTrayTarget
+ (instancetype)shared {
  static WefTrayTarget* instance = nil;
  static dispatch_once_t onceToken;
  dispatch_once(&onceToken, ^{
    instance = [[WefTrayTarget alloc] init];
  });
  return instance;
}

- (void)trayClicked:(id)sender {
  NSStatusBarButton* button = (NSStatusBarButton*)sender;
  NSNumber* tagObj = [button cell].representedObject;
  if (!tagObj)
    return;
  uint32_t tray_id = (uint32_t)[tagObj unsignedIntValue];
  auto& map = TrayMap();
  auto it = map.find(tray_id);
  if (it == map.end())
    return;
  NSEvent* event = [NSApp currentEvent];
  if (event && event.type == NSEventTypeRightMouseUp && it->second.menu) {
    [it->second.item popUpStatusItemMenu:it->second.menu];
    return;
  }
  if (event && event.clickCount >= 2 && it->second.dblclick_fn) {
    it->second.dblclick_fn(it->second.dblclick_data, tray_id);
    return;
  }
  if (it->second.click_fn) {
    it->second.click_fn(it->second.click_data, tray_id);
  }
}

- (void)trayMenuItemClicked:(id)sender {
  NSMenuItem* item = (NSMenuItem*)sender;
  NSArray* pair = [item representedObject];
  if (!pair || [pair count] != 2)
    return;
  NSNumber* tagObj = pair[0];
  NSString* itemId = pair[1];
  uint32_t tray_id = (uint32_t)[tagObj unsignedIntValue];
  auto& map = TrayMap();
  auto it = map.find(tray_id);
  if (it == map.end() || !it->second.menu_click_fn)
    return;
  it->second.menu_click_fn(it->second.menu_click_data, tray_id,
                           [itemId UTF8String]);
}
@end

// Tag each tray-menu item with (tray_id, item_id) so the shared click
// handler can route the click correctly.
static void TagTrayMenuItems(NSMenu* menu, uint32_t tray_id) {
  for (NSMenuItem* mi in [menu itemArray]) {
    if ([mi hasSubmenu]) {
      TagTrayMenuItems([mi submenu], tray_id);
      continue;
    }
    if ([mi isSeparatorItem])
      continue;
    id rep = [mi representedObject];
    if (![rep isKindOfClass:[NSString class]])
      continue;
    NSArray* pair =
        @[ [NSNumber numberWithUnsignedInt:tray_id], (NSString*)rep ];
    [mi setRepresentedObject:pair];
    [mi setTarget:[WefTrayTarget shared]];
    [mi setAction:@selector(trayMenuItemClicked:)];
  }
}

uint32_t Backend_CreateTrayIcon_Mac(void* /*data*/) {
  uint32_t tray_id = g_next_tray_id.fetch_add(1, std::memory_order_relaxed);
  dispatch_async(dispatch_get_main_queue(), ^{
    NSStatusItem* item = [[NSStatusBar systemStatusBar]
        statusItemWithLength:NSSquareStatusItemLength];
    if (!item)
      return;
    NSStatusBarButton* button = [item button];
    if (button) {
      [[button cell] setRepresentedObject:@(tray_id)];
      [button setTarget:[WefTrayTarget shared]];
      [button setAction:@selector(trayClicked:)];
      [button sendActionOn:NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp];
    }
    TrayEntry entry = {};
    entry.item = item;
    TrayMap()[tray_id] = entry;
  });
  return tray_id;
}

void Backend_DestroyTrayIcon_Mac(void* /*data*/, uint32_t tray_id) {
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& map = TrayMap();
    auto it = map.find(tray_id);
    if (it == map.end())
      return;
    if (it->second.item) {
      [[NSStatusBar systemStatusBar] removeStatusItem:it->second.item];
    }
    map.erase(it);
  });
}

static bool SystemIsDarkMode() {
  if (@available(macOS 10.14, *)) {
    NSAppearance* appearance = [NSApp effectiveAppearance];
    NSAppearanceName match = [appearance bestMatchFromAppearancesWithNames:@[
      NSAppearanceNameAqua, NSAppearanceNameDarkAqua
    ]];
    return [match isEqualToString:NSAppearanceNameDarkAqua];
  }
  return false;
}

static void ApplyActiveIconForTray(TrayEntry& entry) {
  if (!entry.item)
    return;
  bool dark = SystemIsDarkMode();
  NSImage* chosen =
      (dark && entry.dark_image) ? entry.dark_image : entry.light_image;
  if (chosen)
    [[entry.item button] setImage:chosen];
}

static NSImage* ImageFromPng(const void* bytes, size_t len) {
  if (!bytes || len == 0)
    return nil;
  NSData* data = [NSData dataWithBytes:bytes length:len];
  NSImage* image = [[NSImage alloc] initWithData:data];
  if (!image)
    return nil;
  [image setSize:NSMakeSize(18, 18)];
  // Treating the supplied PNG as a template gives the best menu-bar
  // appearance in most cases. Users who want a full-color icon should
  // simply not mark it as template — but our API doesn't expose that yet,
  // so default to template for v1 (matches Electron's behavior).
  [image setTemplate:YES];
  return image;
}

// One-shot installer for a distributed-notification observer that fires
// on system theme changes (light <-> dark). Re-applies every tray icon.
static void EnsureTrayAppearanceObserver() {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    [[NSDistributedNotificationCenter defaultCenter]
        addObserverForName:@"AppleInterfaceThemeChangedNotification"
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification* /*n*/) {
                  for (auto& [tid, entry] : TrayMap()) {
                    ApplyActiveIconForTray(entry);
                  }
                }];
  });
}

void Backend_SetTrayIcon_Mac(void* /*data*/, uint32_t tray_id,
                             const void* png_bytes, size_t len) {
  if (!png_bytes || len == 0)
    return;
  NSData* data = [NSData dataWithBytes:png_bytes length:len];
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& map = TrayMap();
    auto it = map.find(tray_id);
    if (it == map.end() || !it->second.item)
      return;
    NSImage* image = ImageFromPng([data bytes], [data length]);
    if (!image)
      return;
    it->second.light_image = image;
    EnsureTrayAppearanceObserver();
    ApplyActiveIconForTray(it->second);
  });
}

void Backend_SetTrayIconDark_Mac(void* /*data*/, uint32_t tray_id,
                                 const void* png_bytes, size_t len) {
  NSData* data = (png_bytes && len > 0)
                     ? [NSData dataWithBytes:png_bytes length:len]
                     : nil;
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& map = TrayMap();
    auto it = map.find(tray_id);
    if (it == map.end() || !it->second.item)
      return;
    it->second.dark_image =
        data ? ImageFromPng([data bytes], [data length]) : nil;
    EnsureTrayAppearanceObserver();
    ApplyActiveIconForTray(it->second);
  });
}

void Backend_SetTrayTooltip_Mac(void* /*data*/, uint32_t tray_id,
                                const char* tooltip_or_null) {
  NSString* tip = (tooltip_or_null && *tooltip_or_null)
                      ? [NSString stringWithUTF8String:tooltip_or_null]
                      : nil;
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& map = TrayMap();
    auto it = map.find(tray_id);
    if (it == map.end() || !it->second.item)
      return;
    [[it->second.item button] setToolTip:tip];
  });
}

void Backend_SetTrayMenu_Mac(void* data, uint32_t tray_id,
                             wef_value_t* menu_template,
                             wef_menu_click_fn on_click, void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const wef_backend_api_t* api = &loader->GetBackendApi();
  if (!menu_template) {
    dispatch_async(dispatch_get_main_queue(), ^{
      auto& map = TrayMap();
      auto it = map.find(tray_id);
      if (it == map.end())
        return;
      it->second.menu = nil;
      it->second.menu_click_fn = nullptr;
      it->second.menu_click_data = nullptr;
    });
    return;
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    NSMenu* menu =
        BuildMenuFromValue(menu_template, api, [WefTrayTarget shared],
                           @selector(trayMenuItemClicked:));
    if (!menu)
      return;
    TagTrayMenuItems(menu, tray_id);
    auto& map = TrayMap();
    auto it = map.find(tray_id);
    if (it == map.end())
      return;
    it->second.menu = menu;
    it->second.menu_click_fn = on_click;
    it->second.menu_click_data = on_click_data;
  });
}

void Backend_SetTrayClickHandler_Mac(void* /*data*/, uint32_t tray_id,
                                     wef_tray_click_fn handler,
                                     void* user_data) {
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& map = TrayMap();
    auto it = map.find(tray_id);
    if (it == map.end())
      return;
    it->second.click_fn = handler;
    it->second.click_data = user_data;
  });
}

void Backend_SetTrayDoubleClickHandler_Mac(void* /*data*/, uint32_t tray_id,
                                           wef_tray_click_fn handler,
                                           void* user_data) {
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& map = TrayMap();
    auto it = map.find(tray_id);
    if (it == map.end())
      return;
    it->second.dblclick_fn = handler;
    it->second.dblclick_data = user_data;
  });
}

// --- Notifications (macOS) ---
//
// Uses NSUserNotification (deprecated in macOS 11+ but still functional in
// macOS 15 and works without UNUserNotificationCenter's entitlement /
// runtime-authorization dance). NSUserNotification supports a single
// `actionButtonTitle` plus an `additionalActions` dropdown.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

struct MacNotifEntry {
  NSUserNotification* notif;
  NSString* tag;
  wef_notification_event_fn on_event;
  void* user_data;
  std::vector<std::string> action_ids;
};

static std::map<uint32_t, MacNotifEntry>& MacNotifMap() {
  static std::map<uint32_t, MacNotifEntry> map;
  return map;
}

static std::map<void*, uint32_t>& NSNotifToId() {
  static std::map<void*, uint32_t> map;
  return map;
}

static std::atomic<uint32_t> g_next_notif_id_mac{1};

@interface WefNotifDelegate : NSObject <NSUserNotificationCenterDelegate>
+ (instancetype)shared;
@end

@implementation WefNotifDelegate
+ (instancetype)shared {
  static WefNotifDelegate* instance = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    instance = [[WefNotifDelegate alloc] init];
  });
  return instance;
}

- (BOOL)userNotificationCenter:(NSUserNotificationCenter*)center
     shouldPresentNotification:(NSUserNotification*)notification {
  (void)center;
  (void)notification;
  return YES;
}

- (void)userNotificationCenter:(NSUserNotificationCenter*)center
        didDeliverNotification:(NSUserNotification*)notification {
  (void)center;
  auto& m = NSNotifToId();
  auto it = m.find((__bridge void*)notification);
  if (it == m.end())
    return;
  uint32_t nid = it->second;
  auto& nm = MacNotifMap();
  auto nit = nm.find(nid);
  if (nit != nm.end() && nit->second.on_event) {
    nit->second.on_event(nit->second.user_data, nid, WEF_NOTIFICATION_SHOWN,
                         nullptr);
  }
}

- (void)userNotificationCenter:(NSUserNotificationCenter*)center
       didActivateNotification:(NSUserNotification*)notification {
  (void)center;
  auto& m = NSNotifToId();
  auto it = m.find((__bridge void*)notification);
  if (it == m.end())
    return;
  uint32_t nid = it->second;
  auto& nm = MacNotifMap();
  auto nit = nm.find(nid);
  if (nit == nm.end() || !nit->second.on_event)
    return;

  switch (notification.activationType) {
    case NSUserNotificationActivationTypeContentsClicked:
      nit->second.on_event(nit->second.user_data, nid,
                           WEF_NOTIFICATION_CLICKED, nullptr);
      break;
    case NSUserNotificationActivationTypeActionButtonClicked: {
      // The "main" action button is the first action in the list.
      const char* aid = nullptr;
      std::string aid_storage;
      if (!nit->second.action_ids.empty()) {
        aid_storage = nit->second.action_ids[0];
        aid = aid_storage.c_str();
      }
      nit->second.on_event(nit->second.user_data, nid,
                           WEF_NOTIFICATION_ACTION, aid);
      break;
    }
    case NSUserNotificationActivationTypeAdditionalActionClicked: {
      NSUserNotificationAction* action = notification.additionalActivationAction;
      if (action && action.identifier) {
        std::string aid = [action.identifier UTF8String];
        nit->second.on_event(nit->second.user_data, nid,
                             WEF_NOTIFICATION_ACTION, aid.c_str());
      }
      break;
    }
    default:
      break;
  }
}
@end

static void EnsureNotifDelegate() {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    [[NSUserNotificationCenter defaultUserNotificationCenter]
        setDelegate:[WefNotifDelegate shared]];
  });
}

// Helper: read a string field out of a wef_value_t dict.
static std::string ReadDictString(const wef_backend_api_t* api,
                                  wef_value_t* dict, const char* key) {
  wef_value_t* v = api->value_dict_get(dict, key);
  if (!v || !api->value_is_string(v))
    return std::string();
  size_t len = 0;
  char* s = api->value_get_string(v, &len);
  if (!s)
    return std::string();
  std::string out(s, len);
  api->value_free_string(s);
  return out;
}

static bool ReadDictBool(const wef_backend_api_t* api, wef_value_t* dict,
                         const char* key, bool dfl) {
  wef_value_t* v = api->value_dict_get(dict, key);
  if (!v || !api->value_is_bool(v))
    return dfl;
  return api->value_get_bool(v);
}

uint32_t Backend_ShowNotification_Mac(void* data, wef_value_t* options,
                                      wef_notification_event_fn on_event,
                                      void* user_data) {
  if (!options)
    return 0;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const wef_backend_api_t* api = &loader->GetBackendApi();
  if (!api->value_is_dict(options)) {
    api->value_free(options);
    return 0;
  }

  std::string title = ReadDictString(api, options, "title");
  std::string body = ReadDictString(api, options, "body");
  std::string tag = ReadDictString(api, options, "tag");
  bool silent = ReadDictBool(api, options, "silent", false);

  std::vector<std::pair<std::string, std::string>> actions;
  wef_value_t* actions_val = api->value_dict_get(options, "actions");
  if (actions_val && api->value_is_list(actions_val)) {
    size_t n = api->value_list_size(actions_val);
    for (size_t i = 0; i < n; ++i) {
      wef_value_t* a = api->value_list_get(actions_val, i);
      if (!a || !api->value_is_dict(a))
        continue;
      std::string aid = ReadDictString(api, a, "id");
      std::string atitle = ReadDictString(api, a, "title");
      if (!aid.empty() && !atitle.empty()) {
        actions.emplace_back(aid, atitle);
      }
    }
  }

  api->value_free(options);

  uint32_t nid =
      g_next_notif_id_mac.fetch_add(1, std::memory_order_relaxed);

  NSString* nsTitle = [NSString stringWithUTF8String:title.c_str()];
  NSString* nsBody = [NSString stringWithUTF8String:body.c_str()];
  NSString* nsTag = tag.empty() ? nil : [NSString stringWithUTF8String:tag.c_str()];

  std::vector<std::string> action_ids;
  std::vector<std::string> action_titles;
  action_ids.reserve(actions.size());
  action_titles.reserve(actions.size());
  for (auto& a : actions) {
    action_ids.push_back(a.first);
    action_titles.push_back(a.second);
  }

  dispatch_async(dispatch_get_main_queue(), ^{
    EnsureNotifDelegate();
    NSUserNotificationCenter* center =
        [NSUserNotificationCenter defaultUserNotificationCenter];

    if (nsTag) {
      // "tag" semantics: replace any existing notification with the same id.
      NSArray<NSUserNotification*>* delivered = [center deliveredNotifications];
      for (NSUserNotification* d in delivered) {
        if (d.identifier && [d.identifier isEqualToString:nsTag]) {
          [center removeDeliveredNotification:d];
        }
      }
    }

    NSUserNotification* notif = [[NSUserNotification alloc] init];
    notif.title = nsTitle;
    notif.informativeText = nsBody;
    if (nsTag)
      notif.identifier = nsTag;
    notif.soundName = silent ? nil : NSUserNotificationDefaultSoundName;

    if (!action_ids.empty()) {
      notif.hasActionButton = YES;
      notif.actionButtonTitle =
          [NSString stringWithUTF8String:action_titles[0].c_str()];
      if (action_ids.size() > 1) {
        NSMutableArray<NSUserNotificationAction*>* extras =
            [NSMutableArray array];
        for (size_t i = 1; i < action_ids.size(); ++i) {
          NSUserNotificationAction* act = [NSUserNotificationAction
              actionWithIdentifier:[NSString stringWithUTF8String:action_ids[i]
                                                                  .c_str()]
                             title:[NSString stringWithUTF8String:action_titles[i]
                                                                  .c_str()]];
          [extras addObject:act];
        }
        notif.additionalActions = extras;
      }
    }

    MacNotifEntry entry = {};
    entry.notif = notif;
    entry.tag = nsTag;
    entry.on_event = on_event;
    entry.user_data = user_data;
    entry.action_ids = action_ids;
    MacNotifMap()[nid] = entry;
    NSNotifToId()[(__bridge void*)notif] = nid;

    [center deliverNotification:notif];
  });

  return nid;
}

void Backend_CloseNotification_Mac(void* /*data*/, uint32_t notification_id) {
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& nm = MacNotifMap();
    auto it = nm.find(notification_id);
    if (it == nm.end())
      return;
    NSUserNotification* notif = it->second.notif;
    wef_notification_event_fn cb = it->second.on_event;
    void* ud = it->second.user_data;
    if (notif) {
      NSUserNotificationCenter* center =
          [NSUserNotificationCenter defaultUserNotificationCenter];
      [center removeDeliveredNotification:notif];
      NSNotifToId().erase((__bridge void*)notif);
    }
    nm.erase(it);
    if (cb) {
      cb(ud, notification_id, WEF_NOTIFICATION_CLOSED, nullptr);
    }
  });
}

#pragma clang diagnostic pop

// --- Permissions (UNUserNotificationCenter) ---
//
// UN is the modern (10.14+) replacement for NSUserNotification's
// implicit "always granted" model. It requires the process to run
// inside a bundled .app with a CFBundleIdentifier; without one
// `getNotificationSettings:` returns garbage and `requestAuthorization:`
// fails immediately. We detect that case and report UNSUPPORTED so the
// embedder (Deno) can branch on it instead of seeing a phantom DENIED.

static int MapUNStatus(UNAuthorizationStatus s) {
  switch (s) {
    case UNAuthorizationStatusNotDetermined:
      return WEF_PERMISSION_STATUS_PROMPT;
    case UNAuthorizationStatusDenied:
      return WEF_PERMISSION_STATUS_DENIED;
    case UNAuthorizationStatusAuthorized:
    case UNAuthorizationStatusProvisional:
      return WEF_PERMISSION_STATUS_GRANTED;
    default:
      return WEF_PERMISSION_STATUS_UNSUPPORTED;
  }
}

static bool MacProcessIsBundled() {
  NSBundle* mb = [NSBundle mainBundle];
  if (!mb)
    return false;
  if (![mb bundleIdentifier])
    return false;
  // Reject the synthetic bundle `cargo run` etc. produces for a bare
  // exe (path doesn't end in .app).
  NSString* path = [mb bundlePath];
  return path && [path hasSuffix:@".app"];
}

static void FirePermissionOnMain(wef_permission_callback_fn cb, void* ud,
                                 int status) {
  if (!cb)
    return;
  dispatch_async(dispatch_get_main_queue(), ^{
    cb(ud, status);
  });
}

void Backend_QueryPermission_Mac(void* /*data*/, int kind,
                                 wef_permission_callback_fn cb,
                                 void* user_data) {
  if (kind != WEF_PERMISSION_NOTIFICATIONS) {
    FirePermissionOnMain(cb, user_data, WEF_PERMISSION_STATUS_UNSUPPORTED);
    return;
  }
  if (!MacProcessIsBundled()) {
    FirePermissionOnMain(cb, user_data, WEF_PERMISSION_STATUS_UNSUPPORTED);
    return;
  }
  UNUserNotificationCenter* center =
      [UNUserNotificationCenter currentNotificationCenter];
  [center getNotificationSettingsWithCompletionHandler:^(
              UNNotificationSettings* settings) {
    int status = MapUNStatus(settings.authorizationStatus);
    FirePermissionOnMain(cb, user_data, status);
  }];
}

void Backend_RequestPermission_Mac(void* /*data*/, int kind,
                                   wef_permission_callback_fn cb,
                                   void* user_data) {
  if (kind != WEF_PERMISSION_NOTIFICATIONS) {
    FirePermissionOnMain(cb, user_data, WEF_PERMISSION_STATUS_UNSUPPORTED);
    return;
  }
  if (!MacProcessIsBundled()) {
    FirePermissionOnMain(cb, user_data, WEF_PERMISSION_STATUS_UNSUPPORTED);
    return;
  }
  UNUserNotificationCenter* center =
      [UNUserNotificationCenter currentNotificationCenter];
  UNAuthorizationOptions opts = UNAuthorizationOptionAlert |
                                 UNAuthorizationOptionSound |
                                 UNAuthorizationOptionBadge;
  [center requestAuthorizationWithOptions:opts
                        completionHandler:^(BOOL granted, NSError* error) {
                          (void)error;
                          // After the user picks, fetch the real status -
                          // `granted` is BOOL but the cached state can be
                          // PROVISIONAL or EPHEMERAL which we still want
                          // mapped through MapUNStatus.
                          [center getNotificationSettingsWithCompletionHandler:^(
                                      UNNotificationSettings* settings) {
                            int status;
                            if (granted) {
                              status = MapUNStatus(settings.authorizationStatus);
                            } else {
                              // The OS rejected; map by current settings
                              // (NotDetermined collapses to DENIED here
                              // because the request was rejected).
                              status = (settings.authorizationStatus ==
                                        UNAuthorizationStatusNotDetermined)
                                           ? WEF_PERMISSION_STATUS_DENIED
                                           : MapUNStatus(
                                                 settings.authorizationStatus);
                            }
                            FirePermissionOnMain(cb, user_data, status);
                          }];
                        }];
}
