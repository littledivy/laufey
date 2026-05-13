// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#import <Cocoa/Cocoa.h>
#import <UserNotifications/UserNotifications.h>
#import <WebKit/WebKit.h>

#include "runtime_loader.h"
#include "wef_json.h"

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

  uint32_t ShowNotification(wef_value_t* options,
                            const wef_backend_api_t* api,
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

std::string NSEventKeyToString(NSEvent* event) {
  NSString* chars = [event characters];
  if (chars && [chars length] > 0) {
    unichar c = [chars characterAtIndex:0];
    // Map special characters to W3C key values
    switch (c) {
      case NSUpArrowFunctionKey:
        return "ArrowUp";
      case NSDownArrowFunctionKey:
        return "ArrowDown";
      case NSLeftArrowFunctionKey:
        return "ArrowLeft";
      case NSRightArrowFunctionKey:
        return "ArrowRight";
      case NSHomeFunctionKey:
        return "Home";
      case NSEndFunctionKey:
        return "End";
      case NSPageUpFunctionKey:
        return "PageUp";
      case NSPageDownFunctionKey:
        return "PageDown";
      case NSDeleteFunctionKey:
        return "Delete";
      case NSInsertFunctionKey:
        return "Insert";
      case NSF1FunctionKey:
        return "F1";
      case NSF2FunctionKey:
        return "F2";
      case NSF3FunctionKey:
        return "F3";
      case NSF4FunctionKey:
        return "F4";
      case NSF5FunctionKey:
        return "F5";
      case NSF6FunctionKey:
        return "F6";
      case NSF7FunctionKey:
        return "F7";
      case NSF8FunctionKey:
        return "F8";
      case NSF9FunctionKey:
        return "F9";
      case NSF10FunctionKey:
        return "F10";
      case NSF11FunctionKey:
        return "F11";
      case NSF12FunctionKey:
        return "F12";
      case 27:
        return "Escape";
      case 13:
      case 3:
        return "Enter";
      case 9:
        return "Tab";
      case 127:
        return "Backspace";
      case 32:
        return " ";
      default:
        if (c >= 0x20 && c < 0x7F) {
          return std::string(1, static_cast<char>(c));
        }
        return [chars UTF8String] ?: "Unidentified";
    }
  }
  return "Unidentified";
}

std::string NSEventKeyCodeToCode(unsigned short keyCode) {
  switch (keyCode) {
    case 0:
      return "KeyA";
    case 1:
      return "KeyS";
    case 2:
      return "KeyD";
    case 3:
      return "KeyF";
    case 4:
      return "KeyH";
    case 5:
      return "KeyG";
    case 6:
      return "KeyZ";
    case 7:
      return "KeyX";
    case 8:
      return "KeyC";
    case 9:
      return "KeyV";
    case 11:
      return "KeyB";
    case 12:
      return "KeyQ";
    case 13:
      return "KeyW";
    case 14:
      return "KeyE";
    case 15:
      return "KeyR";
    case 16:
      return "KeyY";
    case 17:
      return "KeyT";
    case 18:
      return "Digit1";
    case 19:
      return "Digit2";
    case 20:
      return "Digit3";
    case 21:
      return "Digit4";
    case 22:
      return "Digit6";
    case 23:
      return "Digit5";
    case 24:
      return "Equal";
    case 25:
      return "Digit9";
    case 26:
      return "Digit7";
    case 27:
      return "Minus";
    case 28:
      return "Digit8";
    case 29:
      return "Digit0";
    case 30:
      return "BracketRight";
    case 31:
      return "KeyO";
    case 32:
      return "KeyU";
    case 33:
      return "BracketLeft";
    case 34:
      return "KeyI";
    case 35:
      return "KeyP";
    case 36:
      return "Enter";
    case 37:
      return "KeyL";
    case 38:
      return "KeyJ";
    case 39:
      return "Quote";
    case 40:
      return "KeyK";
    case 41:
      return "Semicolon";
    case 42:
      return "Backslash";
    case 43:
      return "Comma";
    case 44:
      return "Slash";
    case 45:
      return "KeyN";
    case 46:
      return "KeyM";
    case 47:
      return "Period";
    case 48:
      return "Tab";
    case 49:
      return "Space";
    case 50:
      return "Backquote";
    case 51:
      return "Backspace";
    case 53:
      return "Escape";
    case 55:
      return "MetaLeft";
    case 56:
      return "ShiftLeft";
    case 57:
      return "CapsLock";
    case 58:
      return "AltLeft";
    case 59:
      return "ControlLeft";
    case 60:
      return "ShiftRight";
    case 61:
      return "AltRight";
    case 62:
      return "ControlRight";
    case 96:
      return "F5";
    case 97:
      return "F6";
    case 98:
      return "F7";
    case 99:
      return "F3";
    case 100:
      return "F8";
    case 101:
      return "F9";
    case 109:
      return "F10";
    case 103:
      return "F11";
    case 111:
      return "F12";
    case 118:
      return "F4";
    case 120:
      return "F2";
    case 122:
      return "F1";
    case 123:
      return "ArrowLeft";
    case 124:
      return "ArrowRight";
    case 125:
      return "ArrowDown";
    case 126:
      return "ArrowUp";
    case 117:
      return "Delete";
    case 114:
      return "Insert";
    case 115:
      return "Home";
    case 119:
      return "End";
    case 116:
      return "PageUp";
    case 121:
      return "PageDown";
    default:
      return "Unidentified";
  }
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

std::string BuildInitScript(const std::string& ns,
                            const std::string& postMessage) {
  return R"JS(
(function() {
  const pendingCalls = new Map();
  let nextCallId = 1;

  function createWefProxy(path = []) {
    return new Proxy(function() {}, {
      get(target, prop) {
        if (prop === 'then' || prop === 'catch' || prop === 'finally' ||
            prop === 'constructor' || prop === Symbol.toStringTag) {
          return undefined;
        }
        return createWefProxy([...path, prop]);
      },
      apply(target, thisArg, args) {
        return new Promise((resolve, reject) => {
          const callId = nextCallId++;
          pendingCalls.set(callId, { resolve, reject });

          const processedArgs = args.map(arg => {
            if (typeof arg === 'function') {
              const cbId = nextCallId++;
              window.__wefCallbacks = window.__wefCallbacks || {};
              window.__wefCallbacks[cbId] = arg;
              return { __callback__: String(cbId) };
            }
            if (arg instanceof ArrayBuffer) {
              const bytes = new Uint8Array(arg);
              let binary = '';
              bytes.forEach(b => binary += String.fromCharCode(b));
              return { __binary__: btoa(binary) };
            }
            if (arg instanceof Uint8Array) {
              let binary = '';
              arg.forEach(b => binary += String.fromCharCode(b));
              return { __binary__: btoa(binary) };
            }
            return arg;
          });

          )JS" +
         postMessage + R"JS(
        });
      }
    });
  }

  window[")JS" +
         ns + R"JS("] = createWefProxy();

  window.__wefRespond = function(callId, result, error) {
    const pending = pendingCalls.get(callId);
    if (pending) {
      pendingCalls.delete(callId);
      if (error) {
        pending.reject(new Error(error));
      } else {
        function convertBinary(obj) {
          if (obj && typeof obj === 'object') {
            if (obj.__binary__) {
              const binary = atob(obj.__binary__);
              const bytes = new Uint8Array(binary.length);
              for (let i = 0; i < binary.length; i++) {
                bytes[i] = binary.charCodeAt(i);
              }
              return bytes.buffer;
            }
            if (Array.isArray(obj)) {
              return obj.map(convertBinary);
            }
            const result = {};
            for (const key in obj) {
              result[key] = convertBinary(obj[key]);
            }
            return result;
          }
          return obj;
        }
        pending.resolve(convertBinary(result));
      }
    }
  };

  window.__wefInvokeCallback = function(callbackId, args) {
    const cb = window.__wefCallbacks && window.__wefCallbacks[callbackId];
    if (cb) {
      cb.apply(null, args);
    }
  };

  window.__wefReleaseCallback = function(callbackId) {
    if (window.__wefCallbacks) {
      delete window.__wefCallbacks[callbackId];
    }
  };

})();
)JS";
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
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      InstallGlobalMonitors();

      NSRect frame = NSMakeRect(0, 0, width, height);
      NSWindowStyleMask style =
          NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
          NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
      NSWindow* window =
          [[NSWindow alloc] initWithContentRect:frame
                                      styleMask:style
                                        backing:NSBackingStoreBuffered
                                          defer:NO];
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

      [window makeKeyAndOrderFront:nil];
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

// --- Application Menu ---

static wef_menu_click_fn g_webview_menu_click_fn = nullptr;
static void* g_webview_menu_click_data = nullptr;

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
  if (itemId && g_webview_menu_click_fn) {
    uint32_t wid = WefIdForNSWindow([NSApp keyWindow]);
    g_webview_menu_click_fn(g_webview_menu_click_data, wid,
                            [itemId UTF8String]);
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

    // Separator
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

    // Role
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

    // Label
    wef_value_t* labelVal = api->value_dict_get(itemVal, "label");
    if (!labelVal || !api->value_is_string(labelVal))
      continue;
    size_t labelLen = 0;
    char* labelStr = api->value_get_string(labelVal, &labelLen);
    if (!labelStr)
      continue;
    NSString* label = [NSString stringWithUTF8String:labelStr];
    api->value_free_string(labelStr);

    // Submenu
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

    // Regular clickable item
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

void WKWebViewBackend::SetApplicationMenu(uint32_t window_id,
                                          wef_value_t* menu_template,
                                          const wef_backend_api_t* api,
                                          wef_menu_click_fn on_click,
                                          void* on_click_data) {
  g_webview_menu_click_fn = on_click;
  g_webview_menu_click_data = on_click_data;

  dispatch_async(dispatch_get_main_queue(), ^{
    NSMenu* menubar =
        BuildMenuFromValue(menu_template, api, [WefMenuTarget shared],
                           @selector(menuItemClicked:));
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
  g_webview_menu_click_fn = on_click;
  g_webview_menu_click_data = on_click_data;

  dispatch_async(dispatch_get_main_queue(), ^{
    NSMenu* menu =
        BuildMenuFromValue(menu_template, api, [WefMenuTarget shared],
                           @selector(menuItemClicked:));
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
  if (out_input_value)
    *out_input_value = nullptr;

  NSString* nsTitle = [NSString stringWithUTF8String:title.c_str()];
  NSString* nsMessage = [NSString stringWithUTF8String:message.c_str()];
  NSString* nsDefault = [NSString stringWithUTF8String:default_value.c_str()];

  // The block runs the modal — `runModal` itself spins NSRunLoop, so other
  // WEF windows / dispatch queues continue to drain while it's up. Done as
  // dispatch_sync so callers off the main thread are forwarded to it (the
  // typical Deno-runtime caller is *already* on main, in which case
  // dispatch_sync executes the block immediately on this thread).
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

// --- Dock (macOS) ---

// Consumed by AppDelegate in main_mac.mm (declared extern there).
NSMenu* g_wv_dock_menu = nil;
static wef_menu_click_fn g_wv_dock_click_fn = nullptr;
static void* g_wv_dock_click_data = nullptr;
wef_dock_reopen_fn g_wv_dock_reopen_fn = nullptr;
void* g_wv_dock_reopen_data = nullptr;

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
  if (itemId && g_wv_dock_click_fn) {
    // window_id = 0 because the dock menu is app-scoped.
    g_wv_dock_click_fn(g_wv_dock_click_data, 0, [itemId UTF8String]);
  }
}
@end

void WKWebViewBackend::SetDockBadge(const char* badge_or_null) {
  NSString* ns = badge_or_null && *badge_or_null
                     ? [NSString stringWithUTF8String:badge_or_null]
                     : nil;
  dispatch_async(dispatch_get_main_queue(), ^{
    [[NSApp dockTile] setBadgeLabel:ns];
  });
}

void WKWebViewBackend::BounceDock(int type) {
  NSRequestUserAttentionType t = (type == WEF_DOCK_BOUNCE_CRITICAL)
                                     ? NSCriticalRequest
                                     : NSInformationalRequest;
  dispatch_async(dispatch_get_main_queue(), ^{
    [NSApp requestUserAttention:t];
  });
}

void WKWebViewBackend::SetDockMenu(wef_value_t* menu_template,
                                   const wef_backend_api_t* api,
                                   wef_menu_click_fn on_click,
                                   void* on_click_data) {
  if (!menu_template) {
    dispatch_async(dispatch_get_main_queue(), ^{
      g_wv_dock_menu = nil;
      g_wv_dock_click_fn = nullptr;
      g_wv_dock_click_data = nullptr;
    });
    return;
  }
  g_wv_dock_click_fn = on_click;
  g_wv_dock_click_data = on_click_data;
  dispatch_async(dispatch_get_main_queue(), ^{
    NSMenu* menu =
        BuildMenuFromValue(menu_template, api, [WefDockMenuTarget shared],
                           @selector(dockMenuItemClicked:));
    g_wv_dock_menu = menu;
  });
}

void WKWebViewBackend::SetDockVisible(bool visible) {
  NSApplicationActivationPolicy policy =
      visible ? NSApplicationActivationPolicyRegular
              : NSApplicationActivationPolicyAccessory;
  dispatch_async(dispatch_get_main_queue(), ^{
    [NSApp setActivationPolicy:policy];
  });
}

void WKWebViewBackend::SetDockReopenHandler(wef_dock_reopen_fn handler,
                                            void* user_data) {
  g_wv_dock_reopen_fn = handler;
  g_wv_dock_reopen_data = user_data;
}

// --- Tray / status-bar icon (macOS) ---

namespace {
struct WvTrayEntry {
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
std::map<uint32_t, WvTrayEntry>& WvTrayMap() {
  static std::map<uint32_t, WvTrayEntry> m;
  return m;
}
std::atomic<uint32_t> g_wv_next_tray_id{1};

bool WvSystemIsDarkMode() {
  if (@available(macOS 10.14, *)) {
    NSAppearance* appearance = [NSApp effectiveAppearance];
    NSAppearanceName match = [appearance bestMatchFromAppearancesWithNames:@[
      NSAppearanceNameAqua, NSAppearanceNameDarkAqua
    ]];
    return [match isEqualToString:NSAppearanceNameDarkAqua];
  }
  return false;
}

NSImage* WvImageFromPng(const void* bytes, size_t len) {
  if (!bytes || len == 0)
    return nil;
  NSData* data = [NSData dataWithBytes:bytes length:len];
  NSImage* image = [[NSImage alloc] initWithData:data];
  if (!image)
    return nil;
  [image setSize:NSMakeSize(18, 18)];
  [image setTemplate:YES];
  return image;
}

void WvApplyActiveIcon(WvTrayEntry& entry) {
  if (!entry.item)
    return;
  bool dark = WvSystemIsDarkMode();
  NSImage* chosen =
      (dark && entry.dark_image) ? entry.dark_image : entry.light_image;
  if (chosen)
    [[entry.item button] setImage:chosen];
}

void WvEnsureAppearanceObserver() {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    [[NSDistributedNotificationCenter defaultCenter]
        addObserverForName:@"AppleInterfaceThemeChangedNotification"
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification* /*n*/) {
                  for (auto& [tid, entry] : WvTrayMap()) {
                    WvApplyActiveIcon(entry);
                  }
                }];
  });
}
}  // namespace

@interface WefWvTrayTarget : NSObject
+ (instancetype)shared;
- (void)trayClicked:(id)sender;
- (void)trayMenuItemClicked:(id)sender;
@end

@implementation WefWvTrayTarget
+ (instancetype)shared {
  static WefWvTrayTarget* instance = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    instance = [[WefWvTrayTarget alloc] init];
  });
  return instance;
}

- (void)trayClicked:(id)sender {
  NSStatusBarButton* button = (NSStatusBarButton*)sender;
  NSNumber* tagObj = [[button cell] representedObject];
  if (!tagObj)
    return;
  uint32_t tray_id = (uint32_t)[tagObj unsignedIntValue];
  auto& map = WvTrayMap();
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
  if (it->second.click_fn)
    it->second.click_fn(it->second.click_data, tray_id);
}

- (void)trayMenuItemClicked:(id)sender {
  NSMenuItem* item = (NSMenuItem*)sender;
  NSArray* pair = [item representedObject];
  if (!pair || [pair count] != 2)
    return;
  uint32_t tray_id = (uint32_t)[(NSNumber*)pair[0] unsignedIntValue];
  NSString* itemId = pair[1];
  auto& map = WvTrayMap();
  auto it = map.find(tray_id);
  if (it == map.end() || !it->second.menu_click_fn)
    return;
  it->second.menu_click_fn(it->second.menu_click_data, tray_id,
                           [itemId UTF8String]);
}
@end

static void TagWvTrayMenuItems(NSMenu* menu, uint32_t tray_id) {
  for (NSMenuItem* mi in [menu itemArray]) {
    if ([mi hasSubmenu]) {
      TagWvTrayMenuItems([mi submenu], tray_id);
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
    [mi setTarget:[WefWvTrayTarget shared]];
    [mi setAction:@selector(trayMenuItemClicked:)];
  }
}

uint32_t WKWebViewBackend::CreateTrayIcon() {
  uint32_t tray_id = g_wv_next_tray_id.fetch_add(1, std::memory_order_relaxed);
  dispatch_async(dispatch_get_main_queue(), ^{
    NSStatusItem* item = [[NSStatusBar systemStatusBar]
        statusItemWithLength:NSSquareStatusItemLength];
    if (!item)
      return;
    NSStatusBarButton* button = [item button];
    if (button) {
      [[button cell] setRepresentedObject:@(tray_id)];
      [button setTarget:[WefWvTrayTarget shared]];
      [button setAction:@selector(trayClicked:)];
      [button sendActionOn:NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp];
    }
    WvTrayEntry entry = {};
    entry.item = item;
    WvTrayMap()[tray_id] = entry;
  });
  return tray_id;
}

void WKWebViewBackend::DestroyTrayIcon(uint32_t tray_id) {
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& map = WvTrayMap();
    auto it = map.find(tray_id);
    if (it == map.end())
      return;
    if (it->second.item)
      [[NSStatusBar systemStatusBar] removeStatusItem:it->second.item];
    map.erase(it);
  });
}

void WKWebViewBackend::SetTrayIcon(uint32_t tray_id, const void* png_bytes,
                                   size_t len) {
  if (!png_bytes || len == 0)
    return;
  NSData* data = [NSData dataWithBytes:png_bytes length:len];
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& map = WvTrayMap();
    auto it = map.find(tray_id);
    if (it == map.end() || !it->second.item)
      return;
    NSImage* image = WvImageFromPng([data bytes], [data length]);
    if (!image)
      return;
    it->second.light_image = image;
    WvEnsureAppearanceObserver();
    WvApplyActiveIcon(it->second);
  });
}

void WKWebViewBackend::SetTrayIconDark(uint32_t tray_id, const void* png_bytes,
                                       size_t len) {
  NSData* data = (png_bytes && len > 0)
                     ? [NSData dataWithBytes:png_bytes length:len]
                     : nil;
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& map = WvTrayMap();
    auto it = map.find(tray_id);
    if (it == map.end() || !it->second.item)
      return;
    it->second.dark_image =
        data ? WvImageFromPng([data bytes], [data length]) : nil;
    WvEnsureAppearanceObserver();
    WvApplyActiveIcon(it->second);
  });
}

void WKWebViewBackend::SetTrayDoubleClickHandler(uint32_t tray_id,
                                                 wef_tray_click_fn handler,
                                                 void* user_data) {
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& map = WvTrayMap();
    auto it = map.find(tray_id);
    if (it == map.end())
      return;
    it->second.dblclick_fn = handler;
    it->second.dblclick_data = user_data;
  });
}

void WKWebViewBackend::SetTrayTooltip(uint32_t tray_id,
                                      const char* tooltip_or_null) {
  NSString* tip = (tooltip_or_null && *tooltip_or_null)
                      ? [NSString stringWithUTF8String:tooltip_or_null]
                      : nil;
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& map = WvTrayMap();
    auto it = map.find(tray_id);
    if (it == map.end() || !it->second.item)
      return;
    [[it->second.item button] setToolTip:tip];
  });
}

void WKWebViewBackend::SetTrayMenu(uint32_t tray_id, wef_value_t* menu_template,
                                   const wef_backend_api_t* api,
                                   wef_menu_click_fn on_click,
                                   void* on_click_data) {
  if (!menu_template) {
    dispatch_async(dispatch_get_main_queue(), ^{
      auto& map = WvTrayMap();
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
        BuildMenuFromValue(menu_template, api, [WefWvTrayTarget shared],
                           @selector(trayMenuItemClicked:));
    if (!menu)
      return;
    TagWvTrayMenuItems(menu, tray_id);
    auto& map = WvTrayMap();
    auto it = map.find(tray_id);
    if (it == map.end())
      return;
    it->second.menu = menu;
    it->second.menu_click_fn = on_click;
    it->second.menu_click_data = on_click_data;
  });
}

void WKWebViewBackend::SetTrayClickHandler(uint32_t tray_id,
                                           wef_tray_click_fn handler,
                                           void* user_data) {
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& map = WvTrayMap();
    auto it = map.find(tray_id);
    if (it == map.end())
      return;
    it->second.click_fn = handler;
    it->second.click_data = user_data;
  });
}

// --- Notifications (macOS WebView) ---
//
// Same approach as the CEF macOS backend: NSUserNotification (deprecated
// in macOS 11 but functional in 15; chosen over UNUserNotificationCenter
// to avoid the framework's bundle-id / entitlement / runtime-auth dance).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace {

struct WvMacNotifEntry {
  NSUserNotification* notif;
  NSString* tag;
  wef_notification_event_fn on_event;
  void* user_data;
  std::vector<std::string> action_ids;
};

std::map<uint32_t, WvMacNotifEntry>& WvMacNotifMap() {
  static std::map<uint32_t, WvMacNotifEntry> map;
  return map;
}
std::map<void*, uint32_t>& WvNSNotifToId() {
  static std::map<void*, uint32_t> map;
  return map;
}
std::atomic<uint32_t> g_wv_next_notif_id{1};

}  // namespace

@interface WefWvNotifDelegate : NSObject <NSUserNotificationCenterDelegate>
+ (instancetype)shared;
@end

@implementation WefWvNotifDelegate
+ (instancetype)shared {
  static WefWvNotifDelegate* instance = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    instance = [[WefWvNotifDelegate alloc] init];
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
  auto& m = WvNSNotifToId();
  auto it = m.find((__bridge void*)notification);
  if (it == m.end())
    return;
  uint32_t nid = it->second;
  auto& nm = WvMacNotifMap();
  auto nit = nm.find(nid);
  if (nit != nm.end() && nit->second.on_event) {
    nit->second.on_event(nit->second.user_data, nid, WEF_NOTIFICATION_SHOWN,
                         nullptr);
  }
}
- (void)userNotificationCenter:(NSUserNotificationCenter*)center
       didActivateNotification:(NSUserNotification*)notification {
  (void)center;
  auto& m = WvNSNotifToId();
  auto it = m.find((__bridge void*)notification);
  if (it == m.end())
    return;
  uint32_t nid = it->second;
  auto& nm = WvMacNotifMap();
  auto nit = nm.find(nid);
  if (nit == nm.end() || !nit->second.on_event)
    return;
  switch (notification.activationType) {
    case NSUserNotificationActivationTypeContentsClicked:
      nit->second.on_event(nit->second.user_data, nid,
                           WEF_NOTIFICATION_CLICKED, nullptr);
      break;
    case NSUserNotificationActivationTypeActionButtonClicked: {
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
      NSUserNotificationAction* action =
          notification.additionalActivationAction;
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

static void EnsureWvNotifDelegate() {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    [[NSUserNotificationCenter defaultUserNotificationCenter]
        setDelegate:[WefWvNotifDelegate shared]];
  });
}

uint32_t WKWebViewBackend::ShowNotification(
    wef_value_t* options, const wef_backend_api_t* api,
    wef_notification_event_fn on_event, void* user_data) {
  if (!options)
    return 0;
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
  bool silent = get_bool("silent", false);

  std::vector<std::pair<std::string, std::string>> actions;
  wef_value_t* actions_val = api->value_dict_get(options, "actions");
  if (actions_val && api->value_is_list(actions_val)) {
    size_t n = api->value_list_size(actions_val);
    for (size_t i = 0; i < n; ++i) {
      wef_value_t* a = api->value_list_get(actions_val, i);
      if (!a || !api->value_is_dict(a))
        continue;
      auto aid = [&]() -> std::string {
        wef_value_t* v = api->value_dict_get(a, "id");
        if (!v || !api->value_is_string(v))
          return std::string();
        size_t len = 0;
        char* s = api->value_get_string(v, &len);
        if (!s)
          return std::string();
        std::string out(s, len);
        api->value_free_string(s);
        return out;
      }();
      auto atitle = [&]() -> std::string {
        wef_value_t* v = api->value_dict_get(a, "title");
        if (!v || !api->value_is_string(v))
          return std::string();
        size_t len = 0;
        char* s = api->value_get_string(v, &len);
        if (!s)
          return std::string();
        std::string out(s, len);
        api->value_free_string(s);
        return out;
      }();
      if (!aid.empty() && !atitle.empty())
        actions.emplace_back(aid, atitle);
    }
  }
  api->value_free(options);

  uint32_t nid = g_wv_next_notif_id.fetch_add(1, std::memory_order_relaxed);

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
    EnsureWvNotifDelegate();
    NSUserNotificationCenter* center =
        [NSUserNotificationCenter defaultUserNotificationCenter];
    if (nsTag) {
      NSArray<NSUserNotification*>* delivered = [center deliveredNotifications];
      for (NSUserNotification* d in delivered) {
        if (d.identifier && [d.identifier isEqualToString:nsTag])
          [center removeDeliveredNotification:d];
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

    WvMacNotifEntry e = {};
    e.notif = notif;
    e.tag = nsTag;
    e.on_event = on_event;
    e.user_data = user_data;
    e.action_ids = action_ids;
    WvMacNotifMap()[nid] = e;
    WvNSNotifToId()[(__bridge void*)notif] = nid;

    [center deliverNotification:notif];
  });

  return nid;
}

void WKWebViewBackend::CloseNotification(uint32_t notification_id) {
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& nm = WvMacNotifMap();
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
      WvNSNotifToId().erase((__bridge void*)notif);
    }
    nm.erase(it);
    if (cb)
      cb(ud, notification_id, WEF_NOTIFICATION_CLOSED, nullptr);
  });
}

#pragma clang diagnostic pop

// --- Permissions (UNUserNotificationCenter) ---
//
// Mirrors cef/src/runtime_loader_mac.mm — the process posts notifications
// via NSUserNotification today, but authorization is owned by
// UNUserNotificationCenter (the modern API). Asking via UN here is
// correct regardless of what posts the banner: macOS routes both APIs
// through the same per-bundle authorization record. The webview backend
// targets the *process*, not the WKWebView — runtime-initiated
// notifications are app-scoped, not page-scoped.

static int MapUNStatusWv(UNAuthorizationStatus s) {
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

static bool WvProcessIsBundled() {
  NSBundle* mb = [NSBundle mainBundle];
  if (!mb || ![mb bundleIdentifier])
    return false;
  NSString* path = [mb bundlePath];
  return path && [path hasSuffix:@".app"];
}

static void FirePermOnMainWv(wef_permission_callback_fn cb, void* ud,
                             int status) {
  if (!cb)
    return;
  dispatch_async(dispatch_get_main_queue(), ^{
    cb(ud, status);
  });
}

void WKWebViewBackend::QueryPermission(int kind,
                                       wef_permission_callback_fn cb,
                                       void* user_data) {
  if (kind != WEF_PERMISSION_NOTIFICATIONS || !WvProcessIsBundled()) {
    FirePermOnMainWv(cb, user_data, WEF_PERMISSION_STATUS_UNSUPPORTED);
    return;
  }
  UNUserNotificationCenter* center =
      [UNUserNotificationCenter currentNotificationCenter];
  [center getNotificationSettingsWithCompletionHandler:^(
              UNNotificationSettings* settings) {
    FirePermOnMainWv(cb, user_data, MapUNStatusWv(settings.authorizationStatus));
  }];
}

void WKWebViewBackend::RequestPermission(int kind,
                                         wef_permission_callback_fn cb,
                                         void* user_data) {
  if (kind != WEF_PERMISSION_NOTIFICATIONS || !WvProcessIsBundled()) {
    FirePermOnMainWv(cb, user_data, WEF_PERMISSION_STATUS_UNSUPPORTED);
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
                          [center getNotificationSettingsWithCompletionHandler:^(
                                      UNNotificationSettings* settings) {
                            int status;
                            if (granted) {
                              status = MapUNStatusWv(settings.authorizationStatus);
                            } else {
                              status = (settings.authorizationStatus ==
                                        UNAuthorizationStatusNotDetermined)
                                           ? WEF_PERMISSION_STATUS_DENIED
                                           : MapUNStatusWv(
                                                 settings.authorizationStatus);
                            }
                            FirePermOnMainWv(cb, user_data, status);
                          }];
                        }];
}

WefBackend* CreateWefBackend() {
  return new WKWebViewBackend();
}
