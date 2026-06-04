// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#import <Cocoa/Cocoa.h>
#import <UserNotifications/UserNotifications.h>
#import <WebKit/WebKit.h>

#include "runtime_loader.h"
#include "wef_backend_common.h"
#include "wef_json.h"
#include "init_script.h"

#include <atomic>
#include <map>
#include <mutex>

@class WefScriptMessageHandler;
@class WefWindowDelegate;
@class WefUIDelegate;

// Defined in main_mac.mm — appends a default Edit submenu (Cut/Copy/Paste/
// Select All/Undo/Redo) to the given menubar if no submenu in the tree
// already exposes -copy:. Cmd+C/V on macOS dispatch via the main menu, so
// this has to be present for them to work at all.
extern void EnsureEditMenu(NSMenu* menubar);

// Per-window state
struct MacWindowState {
  uint32_t window_id;
  NSWindow* window;
  WKWebView* webview;
  WefScriptMessageHandler* message_handler;
  WefWindowDelegate* window_delegate;
  id focus_observer;
  id blur_observer;
  id resize_observer;
  id move_observer;
  NSMenu* menu = nil;  // per-window menu (nil = no custom menu)
  WefUIDelegate* ui_delegate;
};

class WKWebViewBackend : public WefBackend {
 public:
  WKWebViewBackend();
  ~WKWebViewBackend() override;

  void CreateWindow(uint32_t window_id, int width, int height) override;
  void CreateWindowEx(uint32_t window_id, int width, int height,
                      uint32_t flags) override;
  void CloseWindow(uint32_t window_id) override;

  void Navigate(uint32_t window_id, const std::string& url) override;
  void SetTitle(uint32_t window_id, const std::string& title) override;
  void ExecuteJs(uint32_t window_id, const std::string& script,
                 wef_js_result_fn callback, void* callback_data) override;
  void Quit() override;
  void SetWindowSize(uint32_t window_id, int width, int height) override;
  void GetWindowSize(uint32_t window_id, int* width, int* height) override;
  void SetWindowPosition(uint32_t window_id, int x, int y) override;
  void GetWindowPosition(uint32_t window_id, int* x, int* y) override;
  void SetResizable(uint32_t window_id, bool resizable) override;
  bool IsResizable(uint32_t window_id) override;
  void SetAlwaysOnTop(uint32_t window_id, bool always_on_top) override;
  bool IsAlwaysOnTop(uint32_t window_id) override;
  bool IsVisible(uint32_t window_id) override;
  void Show(uint32_t window_id) override;
  void Hide(uint32_t window_id) override;
  void Focus(uint32_t window_id) override;
  void PostUiTask(void (*task)(void*), void* data) override;

  void InvokeJsCallback(uint32_t window_id, uint64_t callback_id,
                        wef::ValuePtr args) override;
  void ReleaseJsCallback(uint32_t window_id, uint64_t callback_id) override;
  void RespondToJsCall(uint32_t window_id, uint64_t call_id,
                       wef::ValuePtr result, wef::ValuePtr error) override;

  void Run() override;

  void SetApplicationMenu(uint32_t window_id, wef_value_t* menu_template,
                          const wef_backend_api_t* api,
                          wef_menu_click_fn on_click,
                          void* on_click_data) override;

  void ShowContextMenu(uint32_t window_id, int x, int y,
                       wef_value_t* menu_template, const wef_backend_api_t* api,
                       wef_menu_click_fn on_click,
                       void* on_click_data) override;

  void OpenDevTools(uint32_t window_id) override;

  int ShowDialog(uint32_t window_id, int dialog_type, const std::string& title,
                 const std::string& message, const std::string& default_value,
                 char** out_input_value) override;

  void SetDockBadge(const char* badge_or_null) override;
  void BounceDock(int type) override;
  void SetDockMenu(wef_value_t* menu_template, const wef_backend_api_t* api,
                   wef_menu_click_fn on_click, void* on_click_data) override;
  void SetDockVisible(bool visible) override;
  void SetDockReopenHandler(wef_dock_reopen_fn handler,
                            void* user_data) override;

  uint32_t CreateTrayIcon() override;
  void DestroyTrayIcon(uint32_t tray_id) override;
  void SetTrayIcon(uint32_t tray_id, const void* png_bytes,
                   size_t len) override;
  void SetTrayTooltip(uint32_t tray_id, const char* tooltip_or_null) override;
  void SetTrayMenu(uint32_t tray_id, wef_value_t* menu_template,
                   const wef_backend_api_t* api, wef_menu_click_fn on_click,
                   void* on_click_data) override;
  void SetTrayClickHandler(uint32_t tray_id, wef_tray_click_fn handler,
                           void* user_data) override;
  void SetTrayDoubleClickHandler(uint32_t tray_id, wef_tray_click_fn handler,
                                 void* user_data) override;
  void SetTrayIconDark(uint32_t tray_id, const void* png_bytes,
                       size_t len) override;
  bool GetTrayIconBounds(uint32_t tray_id, int* x, int* y, int* width,
                         int* height) override;

  uint32_t ShowNotification(wef_value_t* options, const wef_backend_api_t* api,
                            wef_notification_event_fn on_event,
                            void* user_data) override;
  void CloseNotification(uint32_t notification_id) override;

  void QueryPermission(int kind, wef_permission_callback_fn cb,
                       void* user_data) override;
  void RequestPermission(int kind, wef_permission_callback_fn cb,
                         void* user_data) override;

  void HandleJsMessage(uint32_t window_id, uint64_t call_id,
                       const std::string& method, wef::ValuePtr args);

 private:
  MacWindowState* GetWindow(uint32_t window_id);
  void RemoveWindowState(uint32_t window_id);
  void InstallGlobalMonitors();
  void RemoveGlobalMonitors();

  std::map<uint32_t, MacWindowState> windows_;
  std::mutex windows_mutex_;

  // Global event monitors (installed once)
  id keyboard_monitor_ = nil;
  id mouse_monitor_ = nil;
  id mouse_move_monitor_ = nil;
  id scroll_monitor_ = nil;
  bool monitors_installed_ = false;
};

// NSWindow → wef_id mapping for event routing
static std::map<void*, uint32_t> g_nswindow_to_wef_id;
static std::mutex g_nswindow_mutex;

static uint32_t WefIdForNSWindow(NSWindow* win) {
  if (!win)
    return 0;
  std::lock_guard<std::mutex> lock(g_nswindow_mutex);
  auto it = g_nswindow_to_wef_id.find((__bridge void*)win);
  return it != g_nswindow_to_wef_id.end() ? it->second : 0;
}

static void RegisterNSWindow(NSWindow* win, uint32_t window_id) {
  std::lock_guard<std::mutex> lock(g_nswindow_mutex);
  g_nswindow_to_wef_id[(__bridge void*)win] = window_id;
}

static void UnregisterNSWindow(NSWindow* win) {
  std::lock_guard<std::mutex> lock(g_nswindow_mutex);
  g_nswindow_to_wef_id.erase((__bridge void*)win);
}

@interface WefScriptMessageHandler : NSObject <WKScriptMessageHandler>
@property(nonatomic, assign) WKWebViewBackend* backend;
@property(nonatomic, assign) uint32_t windowId;
@end

@implementation WefScriptMessageHandler

- (void)userContentController:(WKUserContentController*)userContentController
      didReceiveScriptMessage:(WKScriptMessage*)message {
  if (![message.name isEqualToString:@"wef"])
    return;

  if (![message.body isKindOfClass:[NSDictionary class]])
    return;

  NSDictionary* body = (NSDictionary*)message.body;

  NSNumber* callIdNum = body[@"callId"];
  NSString* method = body[@"method"];
  id argsJson = body[@"args"];

  if (!callIdNum || !method)
    return;

  uint64_t call_id = [callIdNum unsignedLongLongValue];
  std::string methodStr = [method UTF8String];

  wef::ValuePtr args = wef::Value::List();
  if ([argsJson isKindOfClass:[NSArray class]]) {
    NSArray* argsArray = (NSArray*)argsJson;
    NSError* error = nil;
    NSData* jsonData = [NSJSONSerialization dataWithJSONObject:argsArray
                                                       options:0
                                                         error:&error];
    if (jsonData) {
      NSString* jsonStr = [[NSString alloc] initWithData:jsonData
                                                encoding:NSUTF8StringEncoding];
      args = json::ParseJson([jsonStr UTF8String]);
    }
  }

  if (self.backend) {
    self.backend->HandleJsMessage(self.windowId, call_id, methodStr, args);
  }
}

@end

@interface WefUIDelegate : NSObject <WKUIDelegate>
@end

@implementation WefUIDelegate

- (void)webView:(WKWebView*)webView
    runJavaScriptAlertPanelWithMessage:(NSString*)message
                      initiatedByFrame:(WKFrameInfo*)frame
                     completionHandler:(void (^)(void))completionHandler {
  NSAlert* alert = [[NSAlert alloc] init];
  [alert setMessageText:message];
  [alert addButtonWithTitle:@"OK"];
  [alert setAlertStyle:NSAlertStyleInformational];
  [alert runModal];
  completionHandler();
}

- (void)webView:(WKWebView*)webView
    runJavaScriptConfirmPanelWithMessage:(NSString*)message
                        initiatedByFrame:(WKFrameInfo*)frame
                       completionHandler:
                           (void (^)(BOOL result))completionHandler {
  NSAlert* alert = [[NSAlert alloc] init];
  [alert setMessageText:message];
  [alert addButtonWithTitle:@"OK"];
  [alert addButtonWithTitle:@"Cancel"];
  [alert setAlertStyle:NSAlertStyleInformational];
  NSModalResponse response = [alert runModal];
  completionHandler(response == NSAlertFirstButtonReturn);
}

- (void)webView:(WKWebView*)webView
    runJavaScriptTextInputPanelWithPrompt:(NSString*)prompt
                              defaultText:(NSString*)defaultText
                         initiatedByFrame:(WKFrameInfo*)frame
                        completionHandler:(void (^)(NSString* _Nullable result))
                                              completionHandler {
  NSAlert* alert = [[NSAlert alloc] init];
  [alert setMessageText:prompt];
  [alert addButtonWithTitle:@"OK"];
  [alert addButtonWithTitle:@"Cancel"];
  NSTextField* input =
      [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
  [input setStringValue:defaultText ?: @""];
  [alert setAccessoryView:input];
  [alert.window setInitialFirstResponder:input];
  NSModalResponse response = [alert runModal];
  if (response == NSAlertFirstButtonReturn) {
    completionHandler([input stringValue]);
  } else {
    completionHandler(nil);
  }
}

@end

@interface WefWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) uint32_t windowId;
@end

@implementation WefWindowDelegate

- (BOOL)windowShouldClose:(NSWindow*)sender {
  RuntimeLoader::GetInstance()->DispatchCloseRequestedEvent(self.windowId);
  return NO;
}

@end

namespace {

// NSEvent → W3C key/code lives in backend-common (wef_common::NSEventKeyToKey
// / NSEventKeyToCode). Local aliases keep the existing callers compiling.
inline std::string NSEventKeyToString(NSEvent* event) {
  return wef_common::NSEventKeyToKey((__bridge void*)event);
}
inline std::string NSEventKeyCodeToCode(unsigned short keyCode) {
  return wef_common::NSEventKeyToCode(keyCode);
}

uint32_t NSModifierFlagsToWef(NSEventModifierFlags flags) {
  uint32_t modifiers = 0;
  if (flags & NSEventModifierFlagShift)
    modifiers |= WEF_MOD_SHIFT;
  if (flags & NSEventModifierFlagControl)
    modifiers |= WEF_MOD_CONTROL;
  if (flags & NSEventModifierFlagOption)
    modifiers |= WEF_MOD_ALT;
  if (flags & NSEventModifierFlagCommand)
    modifiers |= WEF_MOD_META;
  return modifiers;
}

int NSButtonToWef(NSInteger buttonNumber) {
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

}  // namespace

// --- WKWebViewBackend implementation ---

WKWebViewBackend::WKWebViewBackend() {}

WKWebViewBackend::~WKWebViewBackend() {
  RemoveGlobalMonitors();
  // Close all remaining windows
  std::lock_guard<std::mutex> lock(windows_mutex_);
  for (auto& [wid, state] : windows_) {
    @autoreleasepool {
      if (state.focus_observer)
        [[NSNotificationCenter defaultCenter]
            removeObserver:state.focus_observer];
      if (state.blur_observer)
        [[NSNotificationCenter defaultCenter]
            removeObserver:state.blur_observer];
      if (state.resize_observer)
        [[NSNotificationCenter defaultCenter]
            removeObserver:state.resize_observer];
      if (state.move_observer)
        [[NSNotificationCenter defaultCenter]
            removeObserver:state.move_observer];
      if (state.webview)
        [state.webview.configuration.userContentController
            removeScriptMessageHandlerForName:@"wef"];
      UnregisterNSWindow(state.window);
    }
  }
  windows_.clear();
}

MacWindowState* WKWebViewBackend::GetWindow(uint32_t window_id) {
  auto it = windows_.find(window_id);
  return it != windows_.end() ? &it->second : nullptr;
}

void WKWebViewBackend::RemoveWindowState(uint32_t window_id) {
  auto it = windows_.find(window_id);
  if (it == windows_.end())
    return;

  auto& state = it->second;
  @autoreleasepool {
    if (state.focus_observer)
      [[NSNotificationCenter defaultCenter]
          removeObserver:state.focus_observer];
    if (state.blur_observer)
      [[NSNotificationCenter defaultCenter] removeObserver:state.blur_observer];
    if (state.resize_observer)
      [[NSNotificationCenter defaultCenter]
          removeObserver:state.resize_observer];
    if (state.move_observer)
      [[NSNotificationCenter defaultCenter] removeObserver:state.move_observer];
    if (state.webview)
      [state.webview.configuration.userContentController
          removeScriptMessageHandlerForName:@"wef"];
    UnregisterNSWindow(state.window);
  }
  windows_.erase(it);
}

void WKWebViewBackend::InstallGlobalMonitors() {
  if (monitors_installed_)
    return;
  monitors_installed_ = true;

  keyboard_monitor_ = [NSEvent
      addLocalMonitorForEventsMatchingMask:(NSEventMaskKeyDown |
                                            NSEventMaskKeyUp)
                                   handler:^NSEvent*(NSEvent* event) {
                                     NSWindow* win = [event window];
                                     uint32_t wid = WefIdForNSWindow(win);
                                     if (wid == 0)
                                       return event;

                                     int state =
                                         ([event type] == NSEventTypeKeyDown)
                                             ? WEF_KEY_PRESSED
                                             : WEF_KEY_RELEASED;
                                     std::string key =
                                         NSEventKeyToString(event);
                                     std::string code =
                                         NSEventKeyCodeToCode([event keyCode]);
                                     uint32_t modifiers = NSModifierFlagsToWef(
                                         [event modifierFlags]);
                                     bool repeat = [event isARepeat];

                                     RuntimeLoader::GetInstance()
                                         ->DispatchKeyboardEvent(
                                             wid, state, key.c_str(),
                                             code.c_str(), modifiers, repeat);

                                     return event;
                                   }];

  mouse_monitor_ = [NSEvent
      addLocalMonitorForEventsMatchingMask:(NSEventMaskLeftMouseDown |
                                            NSEventMaskLeftMouseUp |
                                            NSEventMaskRightMouseDown |
                                            NSEventMaskRightMouseUp |
                                            NSEventMaskOtherMouseDown |
                                            NSEventMaskOtherMouseUp)
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

  mouse_move_monitor_ = [NSEvent
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

  scroll_monitor_ = [NSEvent
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
}

void WKWebViewBackend::RemoveGlobalMonitors() {
  @autoreleasepool {
    if (keyboard_monitor_) {
      [NSEvent removeMonitor:keyboard_monitor_];
      keyboard_monitor_ = nil;
    }
    if (mouse_monitor_) {
      [NSEvent removeMonitor:mouse_monitor_];
      mouse_monitor_ = nil;
    }
    if (mouse_move_monitor_) {
      [NSEvent removeMonitor:mouse_move_monitor_];
      mouse_move_monitor_ = nil;
    }
    if (scroll_monitor_) {
      [NSEvent removeMonitor:scroll_monitor_];
      scroll_monitor_ = nil;
    }
  }
}

void WKWebViewBackend::CreateWindow(uint32_t window_id, int width, int height) {
  CreateWindowEx(window_id, width, height, 0);
}

void WKWebViewBackend::CreateWindowEx(uint32_t window_id, int width, int height,
                                      uint32_t flags) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      InstallGlobalMonitors();

      bool frameless = (flags & WEF_WINDOW_FLAG_FRAMELESS) != 0;
      bool no_activate = (flags & WEF_WINDOW_FLAG_NO_ACTIVATE) != 0;

      NSRect frame = NSMakeRect(0, 0, width, height);
      NSWindowStyleMask style = NSWindowStyleMaskClosable |
                                NSWindowStyleMaskMiniaturizable |
                                NSWindowStyleMaskResizable;
      if (frameless) {
        // Drop the title bar and standard chrome; borderless content area.
        style = NSWindowStyleMaskBorderless | NSWindowStyleMaskResizable;
      } else {
        style |= NSWindowStyleMaskTitled;
      }

      NSWindow* window;
      if (no_activate) {
        // A real non-activating NSPanel floats above normal windows and can
        // take key focus without activating the app — the native menu-bar /
        // tray popover behavior.
        style |= NSWindowStyleMaskNonactivatingPanel;
        NSPanel* panel =
            [[NSPanel alloc] initWithContentRect:frame
                                       styleMask:style
                                         backing:NSBackingStoreBuffered
                                           defer:NO];
        [panel setBecomesKeyOnlyIfNeeded:YES];
        [panel setFloatingPanel:YES];
        [panel setHidesOnDeactivate:NO];
        [panel setLevel:NSPopUpMenuWindowLevel];
        [panel
            setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces |
                                  NSWindowCollectionBehaviorTransient |
                                  NSWindowCollectionBehaviorIgnoresCycle];
        window = panel;
      } else {
        window = [[NSWindow alloc] initWithContentRect:frame
                                             styleMask:style
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
      }
      [window center];

      WefWindowDelegate* delegate = [[WefWindowDelegate alloc] init];
      delegate.windowId = window_id;
      [window setDelegate:delegate];

      WefScriptMessageHandler* handler = [[WefScriptMessageHandler alloc] init];
      handler.backend = this;
      handler.windowId = window_id;

      WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
      [config.userContentController addScriptMessageHandler:handler
                                                       name:@"wef"];

      std::string initScript =
          BuildInitScript(RuntimeLoader::GetInstance()->GetJsNamespace(),
                          "window.webkit.messageHandlers.wef.postMessage({\n"
                          "            callId: callId,\n"
                          "            method: path.join('.'),\n"
                          "            args: processedArgs\n"
                          "          });");
      WKUserScript* script = [[WKUserScript alloc]
            initWithSource:[NSString stringWithUTF8String:initScript.c_str()]
             injectionTime:WKUserScriptInjectionTimeAtDocumentStart
          forMainFrameOnly:YES];
      [config.userContentController addUserScript:script];

      WKWebView* webview = [[WKWebView alloc] initWithFrame:frame
                                              configuration:config];
      if ([webview respondsToSelector:@selector(setInspectable:)]) {
        [webview setInspectable:YES];
      }
      WefUIDelegate* uiDelegate = [[WefUIDelegate alloc] init];
      webview.UIDelegate = uiDelegate;
      [window setContentView:webview];

      RegisterNSWindow(window, window_id);

      // Per-window notification observers
      id focus_obs = [[NSNotificationCenter defaultCenter]
          addObserverForName:NSWindowDidBecomeKeyNotification
                      object:window
                       queue:nil
                  usingBlock:^(NSNotification*) {
                    RuntimeLoader::GetInstance()->DispatchFocusedEvent(
                        window_id, 1);
                    // Swap to this window's menu
                    std::lock_guard<std::mutex> lock(windows_mutex_);
                    auto* state = GetWindow(window_id);
                    if (state && state->menu) {
                      [NSApp setMainMenu:state->menu];
                    }
                  }];

      id blur_obs = [[NSNotificationCenter defaultCenter]
          addObserverForName:NSWindowDidResignKeyNotification
                      object:window
                       queue:nil
                  usingBlock:^(NSNotification*) {
                    RuntimeLoader::GetInstance()->DispatchFocusedEvent(
                        window_id, 0);
                  }];

      id resize_obs = [[NSNotificationCenter defaultCenter]
          addObserverForName:NSWindowDidResizeNotification
                      object:window
                       queue:nil
                  usingBlock:^(NSNotification* note) {
                    NSWindow* w = [note object];
                    if (w) {
                      NSRect f = [[w contentView] frame];
                      RuntimeLoader::GetInstance()->DispatchResizeEvent(
                          window_id, (int)f.size.width, (int)f.size.height);
                    }
                  }];

      id move_obs = [[NSNotificationCenter defaultCenter]
          addObserverForName:NSWindowDidMoveNotification
                      object:window
                       queue:nil
                  usingBlock:^(NSNotification* note) {
                    NSWindow* w = [note object];
                    if (w) {
                      NSRect f = [w frame];
                      RuntimeLoader::GetInstance()->DispatchMoveEvent(
                          window_id, (int)f.origin.x, (int)f.origin.y);
                    }
                  }];

      MacWindowState state;
      state.window_id = window_id;
      state.window = window;
      state.webview = webview;
      state.message_handler = handler;
      state.window_delegate = delegate;
      state.ui_delegate = uiDelegate;
      state.focus_observer = focus_obs;
      state.blur_observer = blur_obs;
      state.resize_observer = resize_obs;
      state.move_observer = move_obs;

      {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        windows_[window_id] = state;
      }

      if (no_activate) {
        // Show without activating the app / stealing focus.
        [window orderFrontRegardless];
      } else {
        [window makeKeyAndOrderFront:nil];
      }
    }
  });
}

void WKWebViewBackend::CloseWindow(uint32_t window_id) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto it = windows_.find(window_id);
      if (it != windows_.end()) {
        NSWindow* win = it->second.window;
        RemoveWindowState(window_id);
        [win close];
      }
    }
  });
}

void WKWebViewBackend::Navigate(uint32_t window_id, const std::string& url) {
  std::string urlCopy = url;
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (!state)
        return;

      if (urlCopy.find("data:text/html,") == 0) {
        NSString* html = [NSString stringWithUTF8String:urlCopy.c_str() + 15];
        html = [html stringByRemovingPercentEncoding];
        [state->webview loadHTMLString:html baseURL:nil];
        return;
      }

      NSURL* nsurl =
          [NSURL URLWithString:[NSString stringWithUTF8String:urlCopy.c_str()]];
      if (nsurl && nsurl.scheme && nsurl.scheme.length > 0) {
        NSURLRequest* request = [NSURLRequest requestWithURL:nsurl];
        [state->webview loadRequest:request];
      } else {
        NSString* path = [NSString stringWithUTF8String:urlCopy.c_str()];
        NSURL* fileURL = [NSURL fileURLWithPath:path];
        if (fileURL) {
          [state->webview loadFileURL:fileURL allowingReadAccessToURL:fileURL];
        }
      }
    }
  });
}

void WKWebViewBackend::SetTitle(uint32_t window_id, const std::string& title) {
  std::string titleCopy = title;
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state) {
        [state->window
            setTitle:[NSString stringWithUTF8String:titleCopy.c_str()]];
      }
    }
  });
}

void WKWebViewBackend::ExecuteJs(uint32_t window_id, const std::string& script,
                                 wef_js_result_fn callback,
                                 void* callback_data) {
  std::string scriptCopy = script;
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (!state) {
        if (callback)
          callback(nullptr, nullptr, callback_data);
        return;
      }
      if (!callback) {
        [state->webview
            evaluateJavaScript:[NSString
                                   stringWithUTF8String:scriptCopy.c_str()]
             completionHandler:nil];
        return;
      }
      [state->webview
          evaluateJavaScript:[NSString stringWithUTF8String:scriptCopy.c_str()]
           completionHandler:^(id result, NSError* error) {
             if (error) {
               std::string errMsg = [[error localizedDescription] UTF8String];
               auto errVal = wef::Value::String(errMsg);
               wef_value errWef(errVal);
               callback(nullptr, &errWef, callback_data);
               return;
             }
             if (!result || [result isKindOfClass:[NSNull class]]) {
               callback(nullptr, nullptr, callback_data);
               return;
             }
             // Convert the result to JSON, then parse it back into a wef::Value
             NSError* jsonError = nil;
             NSData* jsonData = nil;
             if ([NSJSONSerialization isValidJSONObject:@[ result ]]) {
               // Wrap in array to handle primitives
               jsonData = [NSJSONSerialization dataWithJSONObject:@[ result ]
                                                          options:0
                                                            error:&jsonError];
             }
             if (jsonData) {
               NSString* jsonStr =
                   [[NSString alloc] initWithData:jsonData
                                         encoding:NSUTF8StringEncoding];
               // Parse the wrapped array, extract first element
               auto parsed = json::ParseJson([jsonStr UTF8String]);
               if (parsed && parsed->IsList() && !parsed->GetList().empty()) {
                 wef_value resultWef(parsed->GetList()[0]);
                 callback(&resultWef, nullptr, callback_data);
               } else {
                 callback(nullptr, nullptr, callback_data);
               }
             } else if ([result isKindOfClass:[NSNumber class]]) {
               // Handle numbers that aren't valid JSON objects on their own
               NSNumber* num = (NSNumber*)result;
               const char* objcType = [num objCType];
               if (strcmp(objcType, @encode(BOOL)) == 0 ||
                   strcmp(objcType, @encode(char)) == 0) {
                 auto val = wef::Value::Bool([num boolValue]);
                 wef_value wef(val);
                 callback(&wef, nullptr, callback_data);
               } else if (strcmp(objcType, @encode(int)) == 0 ||
                          strcmp(objcType, @encode(long)) == 0 ||
                          strcmp(objcType, @encode(long long)) == 0) {
                 auto val = wef::Value::Int([num intValue]);
                 wef_value wef(val);
                 callback(&wef, nullptr, callback_data);
               } else {
                 auto val = wef::Value::Double([num doubleValue]);
                 wef_value wef(val);
                 callback(&wef, nullptr, callback_data);
               }
             } else if ([result isKindOfClass:[NSString class]]) {
               auto val = wef::Value::String([(NSString*)result UTF8String]);
               wef_value wef(val);
               callback(&wef, nullptr, callback_data);
             } else {
               callback(nullptr, nullptr, callback_data);
             }
           }];
    }
  });
}

void WKWebViewBackend::Quit() {
  dispatch_async(dispatch_get_main_queue(), ^{
    [NSApp stop:nil];
    NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSMakePoint(0, 0)
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0];
    [NSApp postEvent:event atStart:YES];
  });
}

void WKWebViewBackend::SetWindowSize(uint32_t window_id, int width,
                                     int height) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state) {
        NSRect frame = [state->window frame];
        frame.size = NSMakeSize(width, height);
        [state->window setFrame:frame display:YES];
      }
    }
  });
}

void WKWebViewBackend::GetWindowSize(uint32_t window_id, int* width,
                                     int* height) {
  __block int w = 0, h = 0;
  dispatch_sync(dispatch_get_main_queue(), ^{
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      NSRect frame = [state->window frame];
      w = static_cast<int>(frame.size.width);
      h = static_cast<int>(frame.size.height);
    }
  });
  if (width)
    *width = w;
  if (height)
    *height = h;
}

void WKWebViewBackend::SetWindowPosition(uint32_t window_id, int x, int y) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state) {
        NSRect frame = [state->window frame];
        NSRect screenFrame = [[state->window screen] frame];
        CGFloat flippedY = screenFrame.size.height - y - frame.size.height;
        [state->window setFrameOrigin:NSMakePoint(x, flippedY)];
      }
    }
  });
}

void WKWebViewBackend::GetWindowPosition(uint32_t window_id, int* x, int* y) {
  __block int px = 0, py = 0;
  dispatch_sync(dispatch_get_main_queue(), ^{
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      NSRect frame = [state->window frame];
      NSRect screenFrame = [[state->window screen] frame];
      px = static_cast<int>(frame.origin.x);
      py = static_cast<int>(screenFrame.size.height - frame.origin.y -
                            frame.size.height);
    }
  });
  if (x)
    *x = px;
  if (y)
    *y = py;
}

void WKWebViewBackend::SetResizable(uint32_t window_id, bool resizable) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state) {
        NSWindowStyleMask mask = [state->window styleMask];
        if (resizable) {
          mask |= NSWindowStyleMaskResizable;
        } else {
          mask &= ~NSWindowStyleMaskResizable;
        }
        [state->window setStyleMask:mask];
      }
    }
  });
}

bool WKWebViewBackend::IsResizable(uint32_t window_id) {
  __block bool result = false;
  dispatch_sync(dispatch_get_main_queue(), ^{
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      result = ([state->window styleMask] & NSWindowStyleMaskResizable) != 0;
    }
  });
  return result;
}

void WKWebViewBackend::SetAlwaysOnTop(uint32_t window_id, bool always_on_top) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state) {
        [state->window setLevel:always_on_top ? NSFloatingWindowLevel
                                              : NSNormalWindowLevel];
      }
    }
  });
}

bool WKWebViewBackend::IsAlwaysOnTop(uint32_t window_id) {
  __block bool result = false;
  dispatch_sync(dispatch_get_main_queue(), ^{
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      result = [state->window level] >= NSFloatingWindowLevel;
    }
  });
  return result;
}

bool WKWebViewBackend::IsVisible(uint32_t window_id) {
  __block bool result = false;
  dispatch_sync(dispatch_get_main_queue(), ^{
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      result = [state->window isVisible];
    }
  });
  return result;
}

void WKWebViewBackend::Show(uint32_t window_id) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state) {
        [state->window makeKeyAndOrderFront:nil];
      }
    }
  });
}

void WKWebViewBackend::Hide(uint32_t window_id) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state) {
        [state->window orderOut:nil];
      }
    }
  });
}

void WKWebViewBackend::Focus(uint32_t window_id) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state) {
        [NSApp activateIgnoringOtherApps:YES];
        [state->window makeKeyAndOrderFront:nil];
      }
    }
  });
}

void WKWebViewBackend::PostUiTask(void (*task)(void*), void* data) {
  dispatch_async(dispatch_get_main_queue(), ^{
    task(data);
  });
}

void WKWebViewBackend::InvokeJsCallback(uint32_t window_id,
                                        uint64_t callback_id,
                                        wef::ValuePtr args) {
  std::string argsJson = json::Serialize(args);
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      // window_id == 0 means broadcast to all windows
      if (window_id == 0) {
        for (auto& [wid, state] : windows_) {
          NSString* script = [NSString
              stringWithFormat:@"window.__wefInvokeCallback(%llu, %s);",
                               callback_id, argsJson.c_str()];
          [state.webview evaluateJavaScript:script completionHandler:nil];
        }
      } else {
        auto* state = GetWindow(window_id);
        if (state) {
          NSString* script = [NSString
              stringWithFormat:@"window.__wefInvokeCallback(%llu, %s);",
                               callback_id, argsJson.c_str()];
          [state->webview evaluateJavaScript:script completionHandler:nil];
        }
      }
    }
  });
}

void WKWebViewBackend::ReleaseJsCallback(uint32_t window_id,
                                         uint64_t callback_id) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      if (window_id == 0) {
        for (auto& [wid, state] : windows_) {
          NSString* script =
              [NSString stringWithFormat:@"window.__wefReleaseCallback(%llu);",
                                         callback_id];
          [state.webview evaluateJavaScript:script completionHandler:nil];
        }
      } else {
        auto* state = GetWindow(window_id);
        if (state) {
          NSString* script =
              [NSString stringWithFormat:@"window.__wefReleaseCallback(%llu);",
                                         callback_id];
          [state->webview evaluateJavaScript:script completionHandler:nil];
        }
      }
    }
  });
}

void WKWebViewBackend::RespondToJsCall(uint32_t window_id, uint64_t call_id,
                                       wef::ValuePtr result,
                                       wef::ValuePtr error) {
  std::string resultJson = json::Serialize(result);
  std::string errorJson = error ? json::Serialize(error) : "null";
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (!state)
        return;

      NSString* script;
      if (error) {
        script =
            [NSString stringWithFormat:@"window.__wefRespond(%llu, null, %s);",
                                       call_id, errorJson.c_str()];
      } else {
        script =
            [NSString stringWithFormat:@"window.__wefRespond(%llu, %s, null);",
                                       call_id, resultJson.c_str()];
      }
      [state->webview evaluateJavaScript:script completionHandler:nil];
    }
  });
}

void WKWebViewBackend::Run() {
  @autoreleasepool {
    [NSApp run];
  }
}

void WKWebViewBackend::HandleJsMessage(uint32_t window_id, uint64_t call_id,
                                       const std::string& method,
                                       wef::ValuePtr args) {
  RuntimeLoader::GetInstance()->OnJsCall(window_id, call_id, method, args);
}

// --- Application Menu / Context Menu ---
//
// Menu construction lives in backend-common (wef_common::BuildNSMenuFromValue).

void WKWebViewBackend::SetApplicationMenu(uint32_t window_id,
                                          wef_value_t* menu_template,
                                          const wef_backend_api_t* api,
                                          wef_menu_click_fn on_click,
                                          void* on_click_data) {
  dispatch_async(dispatch_get_main_queue(), ^{
    NSMenu* menubar = wef_common::BuildNSMenuFromValue(
        menu_template, api, on_click, on_click_data, window_id);
    if (menubar) {
      EnsureEditMenu(menubar);
      // Store the menu for this window
      {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        auto* state = GetWindow(window_id);
        if (state) {
          state->menu = menubar;
        }
      }
      // If this window is currently the key window, apply immediately
      NSWindow* keyWin = [NSApp keyWindow];
      uint32_t keyWid = WefIdForNSWindow(keyWin);
      if (keyWid == window_id) {
        [NSApp setMainMenu:menubar];
      }
    }
  });
}

void WKWebViewBackend::ShowContextMenu(uint32_t window_id, int x, int y,
                                       wef_value_t* menu_template,
                                       const wef_backend_api_t* api,
                                       wef_menu_click_fn on_click,
                                       void* on_click_data) {
  dispatch_async(dispatch_get_main_queue(), ^{
    NSMenu* menu = wef_common::BuildNSMenuFromValue(
        menu_template, api, on_click, on_click_data, window_id);
    if (!menu)
      return;

    NSWindow* win = nil;
    {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (state)
        win = state->window;
    }
    if (!win)
      return;

    NSView* view = [win contentView];
    // Convert from top-left origin (wef coordinates) to bottom-left origin
    // (NSView)
    NSPoint loc = NSMakePoint(x, [view frame].size.height - y);
    [menu popUpMenuPositioningItem:nil atLocation:loc inView:view];
  });
}

void WKWebViewBackend::OpenDevTools(uint32_t window_id) {
  dispatch_async(dispatch_get_main_queue(), ^{
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state && state->webview) {
      // WKWebView._inspector.show is available on macOS 13.3+
      @try {
        id inspector = [state->webview valueForKey:@"_inspector"];
        if (inspector) {
          [inspector performSelector:@selector(show)];
        }
      } @catch (NSException*) {
        // Fallback: not available on this macOS version
      }
    }
  });
}

int WKWebViewBackend::ShowDialog(uint32_t /*window_id*/, int dialog_type,
                                 const std::string& title,
                                 const std::string& message,
                                 const std::string& default_value,
                                 char** out_input_value) {
  return wef_common::ShowDialogMac(dialog_type, title, message, default_value,
                                   out_input_value);
}

// --- Dock (macOS) ---
//
// Dock menu + reopen handler storage lives in backend-common; the
// AppDelegate in main_mac.mm reads via wef_common::{Get,Set,Fire}*.

void WKWebViewBackend::SetDockBadge(const char* badge_or_null) {
  wef_common::SetDockBadgeMac(badge_or_null);
}

void WKWebViewBackend::BounceDock(int type) {
  wef_common::BounceDockMac(type);
}

void WKWebViewBackend::SetDockMenu(wef_value_t* menu_template,
                                   const wef_backend_api_t* api,
                                   wef_menu_click_fn on_click,
                                   void* on_click_data) {
  if (!menu_template) {
    dispatch_async(dispatch_get_main_queue(), ^{
      wef_common::SetDockMenuMac(nil);
    });
    return;
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    // window_id = 0 because the dock menu is app-scoped.
    NSMenu* menu = wef_common::BuildNSMenuFromValue(menu_template, api,
                                                    on_click, on_click_data, 0);
    wef_common::SetDockMenuMac(menu);
  });
}

void WKWebViewBackend::SetDockVisible(bool visible) {
  wef_common::SetDockVisibleMac(visible);
}

void WKWebViewBackend::SetDockReopenHandler(wef_dock_reopen_fn handler,
                                            void* user_data) {
  wef_common::SetDockReopenHandlerMac(handler, user_data);
}

// --- Tray / status-bar icon (macOS) ---
//
// Thin trampolines over backend-common/src/tray_mac.mm.

uint32_t WKWebViewBackend::CreateTrayIcon() {
  return wef_common::CreateTrayIconMac();
}

void WKWebViewBackend::DestroyTrayIcon(uint32_t tray_id) {
  wef_common::DestroyTrayIconMac(tray_id);
}

void WKWebViewBackend::SetTrayIcon(uint32_t tray_id, const void* png_bytes,
                                   size_t len) {
  wef_common::SetTrayIconMac(tray_id, png_bytes, len);
}

void WKWebViewBackend::SetTrayIconDark(uint32_t tray_id, const void* png_bytes,
                                       size_t len) {
  wef_common::SetTrayIconDarkMac(tray_id, png_bytes, len);
}

bool WKWebViewBackend::GetTrayIconBounds(uint32_t tray_id, int* x, int* y,
                                         int* width, int* height) {
  return wef_common::GetTrayIconBoundsMac(tray_id, x, y, width, height);
}

void WKWebViewBackend::SetTrayDoubleClickHandler(uint32_t tray_id,
                                                 wef_tray_click_fn handler,
                                                 void* user_data) {
  wef_common::SetTrayDoubleClickHandlerMac(tray_id, handler, user_data);
}

void WKWebViewBackend::SetTrayTooltip(uint32_t tray_id,
                                      const char* tooltip_or_null) {
  wef_common::SetTrayTooltipMac(tray_id, tooltip_or_null);
}

void WKWebViewBackend::SetTrayMenu(uint32_t tray_id, wef_value_t* menu_template,
                                   const wef_backend_api_t* api,
                                   wef_menu_click_fn on_click,
                                   void* on_click_data) {
  wef_common::SetTrayMenuMac(tray_id, menu_template, api, on_click,
                             on_click_data);
}

void WKWebViewBackend::SetTrayClickHandler(uint32_t tray_id,
                                           wef_tray_click_fn handler,
                                           void* user_data) {
  wef_common::SetTrayClickHandlerMac(tray_id, handler, user_data);
}
// --- Notifications (macOS WebView) ---
//
// Thin trampolines over backend-common/src/notifications_mac.mm
// (UNUserNotificationCenter-backed). Migrated from NSUserNotification
// (deprecated in macOS 11) to align with the CEF backend.

uint32_t WKWebViewBackend::ShowNotification(wef_value_t* options,
                                            const wef_backend_api_t* api,
                                            wef_notification_event_fn on_event,
                                            void* user_data) {
  wef_common::NotificationOptions opts =
      wef_common::ParseNotificationOptions(options, api);
  return wef_common::ShowNotificationMac(opts, on_event, user_data);
}

void WKWebViewBackend::CloseNotification(uint32_t notification_id) {
  wef_common::CloseNotificationMac(notification_id);
}

// --- Permissions (UNUserNotificationCenter) ---
//
// Mirrors cef/src/runtime_loader_mac.mm — the process posts notifications
// via NSUserNotification today, but authorization is owned by
// UNUserNotificationCenter (the modern API). Asking via UN here is
// correct regardless of what posts the banner: macOS routes both APIs
// through the same per-bundle authorization record. The webview backend
// targets the *process*, not the WKWebView — runtime-initiated
// notifications are app-scoped, not page-scoped.

// Permissions: thin trampolines over backend-common/src/permissions_mac.mm.

void WKWebViewBackend::QueryPermission(int kind, wef_permission_callback_fn cb,
                                       void* user_data) {
  wef_common::QueryPermissionMac(kind, cb, user_data);
}

void WKWebViewBackend::RequestPermission(int kind,
                                         wef_permission_callback_fn cb,
                                         void* user_data) {
  wef_common::RequestPermissionMac(kind, cb, user_data);
}

WefBackend* CreateWefBackend() {
  return new WKWebViewBackend();
}
