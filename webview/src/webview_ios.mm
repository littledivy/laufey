// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// iOS / iOS-simulator webview backend for laufey. Implements the LaufeyBackend
// seam (the same one WKWebViewBackend implements on macOS) on UIKit: a
// "window" is a UIWindow + root UIViewController hosting a WKWebView. The
// JS<->native bridge, value marshalling, and init script are all shared with
// the desktop backends (runtime_loader.cc / laufey_json.h / init_script.h).

#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>

#include <map>
#include <mutex>
#include <string>

#include "init_script.h"
#include "laufey_json.h"
#include "runtime_loader.h"

@class LaufeyIOSMessageHandler;

namespace {

struct IOSWindowState {
  uint32_t window_id = 0;
  UIWindow* window = nil;
  UIViewController* controller = nil;
  WKWebView* webview = nil;
  LaufeyIOSMessageHandler* handler = nil;
};

}  // namespace

// The iOS LaufeyBackend. Most desktop-only surfaces (menus, geometry, dialogs)
// are inherited no-ops or trivial stubs; the webview + bridge are real.
class WKWebViewIOSBackend : public LaufeyBackend {
 public:
  WKWebViewIOSBackend() = default;
  ~WKWebViewIOSBackend() override = default;

  void CreateWindow(uint32_t window_id, int width, int height) override {
    CreateWindowEx(window_id, width, height, 0);
  }
  void CreateWindowEx(uint32_t window_id, int width, int height,
                      uint32_t flags) override;
  void CloseWindow(uint32_t window_id) override;

  void Navigate(uint32_t window_id, const std::string& url) override;
  void SetTitle(uint32_t /*window_id*/, const std::string& /*title*/) override {}
  void ExecuteJs(uint32_t window_id, const std::string& script,
                 laufey_js_result_fn callback, void* callback_data) override;

  // Window geometry / state — fullscreen on iOS, so these are no-ops/defaults.
  void SetWindowSize(uint32_t, int, int) override {}
  void GetWindowSize(uint32_t, int* w, int* h) override {
    CGRect b = UIScreen.mainScreen.bounds;
    if (w) *w = (int)b.size.width;
    if (h) *h = (int)b.size.height;
  }
  void SetWindowPosition(uint32_t, int, int) override {}
  void GetWindowPosition(uint32_t, int* x, int* y) override {
    if (x) *x = 0;
    if (y) *y = 0;
  }
  void SetResizable(uint32_t, bool) override {}
  bool IsResizable(uint32_t) override { return false; }
  void SetAlwaysOnTop(uint32_t, bool) override {}
  bool IsAlwaysOnTop(uint32_t) override { return false; }
  bool IsVisible(uint32_t) override { return true; }
  void Show(uint32_t) override {}
  void Hide(uint32_t) override {}
  void Focus(uint32_t) override {}

  void Quit() override {}
  void PostUiTask(void (*task)(void*), void* data) override {
    dispatch_async(dispatch_get_main_queue(), ^{
      task(data);
    });
  }
  void Run() override {}  // UIApplicationMain already owns the run loop.

  void InvokeJsCallback(uint32_t window_id, uint64_t callback_id,
                        laufey::ValuePtr args) override {
    EvalOnWindow(window_id,
                 BuildInvokeCallbackScript(callback_id, json::Serialize(args)));
  }
  void ReleaseJsCallback(uint32_t window_id, uint64_t callback_id) override {
    EvalOnWindow(window_id, BuildReleaseCallbackScript(callback_id));
  }
  void RespondToJsCall(uint32_t window_id, uint64_t call_id,
                       laufey::ValuePtr result, laufey::ValuePtr error) override;

  // Desktop-only UI surfaces: not applicable on iOS.
  void SetApplicationMenu(uint32_t, laufey_value_t*,
                          const laufey_backend_api_t*, laufey_menu_click_fn,
                          void*) override {}
  void ShowContextMenu(uint32_t, int, int, laufey_value_t*,
                       const laufey_backend_api_t*, laufey_menu_click_fn,
                       void*) override {}
  void OpenDevTools(uint32_t) override {}
  int ShowDialog(uint32_t, int, const std::string&, const std::string&,
                 const std::string&, char**) override {
    return 0;
  }

  // Called from the script-message handler on the main thread.
  void HandleJsMessage(uint32_t window_id, uint64_t call_id,
                       const std::string& method, laufey::ValuePtr args) {
    RuntimeLoader::GetInstance()->OnJsCall(window_id, call_id, method, args);
  }

  IOSWindowState* GetWindow(uint32_t window_id) {
    auto it = windows_.find(window_id);
    return it == windows_.end() ? nullptr : &it->second;
  }

 private:
  void EvalOnWindow(uint32_t window_id, const std::string& script);

  std::map<uint32_t, IOSWindowState> windows_;
  std::mutex windows_mutex_;
};

// --- Script message handler: JS -> native -----------------------------------

@interface LaufeyIOSMessageHandler : NSObject <WKScriptMessageHandler>
@property(nonatomic, assign) WKWebViewIOSBackend* backend;
@property(nonatomic, assign) uint32_t windowId;
@end

@implementation LaufeyIOSMessageHandler
- (void)userContentController:(WKUserContentController*)ucc
      didReceiveScriptMessage:(WKScriptMessage*)message {
  if (![message.name isEqualToString:@"laufey"]) return;
  if (![message.body isKindOfClass:[NSDictionary class]]) return;
  NSDictionary* body = (NSDictionary*)message.body;
  NSNumber* callIdNum = body[@"callId"];
  NSString* method = body[@"method"];
  id argsJson = body[@"args"];
  if (!callIdNum || !method) return;

  uint64_t call_id = [callIdNum unsignedLongLongValue];
  std::string methodStr = [method UTF8String];

  laufey::ValuePtr args = laufey::Value::List();
  if ([argsJson isKindOfClass:[NSArray class]]) {
    NSData* jsonData = [NSJSONSerialization dataWithJSONObject:argsJson
                                                      options:0
                                                        error:nil];
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

// --- Backend method implementations -----------------------------------------

void WKWebViewIOSBackend::CreateWindowEx(uint32_t window_id, int width,
                                         int height, uint32_t /*flags*/) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      LaufeyIOSMessageHandler* handler =
          [[LaufeyIOSMessageHandler alloc] init];
      handler.backend = this;
      handler.windowId = window_id;

      WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
      [config.userContentController addScriptMessageHandler:handler
                                                       name:@"laufey"];

      std::string initScript =
          BuildInitScript(RuntimeLoader::GetInstance()->GetJsNamespace(),
                          "window.webkit.messageHandlers.laufey.postMessage({\n"
                          "            callId: callId,\n"
                          "            method: path.join('.'),\n"
                          "            args: processedArgs\n"
                          "          });");
      WKUserScript* script = [[WKUserScript alloc]
            initWithSource:[NSString stringWithUTF8String:initScript.c_str()]
             injectionTime:WKUserScriptInjectionTimeAtDocumentStart
          forMainFrameOnly:YES];
      [config.userContentController addUserScript:script];

      // Force a non-zoomable, edge-to-edge viewport (disables pinch / double-
      // tap zoom regardless of the page's own viewport meta).
      NSString* noZoom =
          @"(function(){function f(){var m=document.querySelector("
          @"'meta[name=viewport]')||document.createElement('meta');"
          @"m.name='viewport';m.content='width=device-width,initial-scale=1,"
          @"maximum-scale=1,user-scalable=no,viewport-fit=cover';"
          @"if(!m.parentNode&&document.head)document.head.appendChild(m);}"
          @"if(document.head){f();}else{document.addEventListener("
          @"'DOMContentLoaded',f);}})();";
      WKUserScript* viewport = [[WKUserScript alloc]
            initWithSource:noZoom
             injectionTime:WKUserScriptInjectionTimeAtDocumentStart
          forMainFrameOnly:YES];
      [config.userContentController addUserScript:viewport];

      // Explicitly enable content JavaScript (iOS may default it off
      // depending on the configuration / loadHTMLString path).
      if (@available(iOS 14.0, *)) {
        config.defaultWebpagePreferences.allowsContentJavaScript = YES;
      }
      config.preferences.javaScriptEnabled = YES;

      CGRect bounds = UIScreen.mainScreen.bounds;
      WKWebView* webview = [[WKWebView alloc] initWithFrame:bounds
                                              configuration:config];
      webview.translatesAutoresizingMaskIntoConstraints = NO;
      webview.opaque = NO;
      webview.backgroundColor = UIColor.blackColor;
      // Edge-to-edge: stop the scroll view from insetting content for the
      // safe area (the page handles insets via viewport-fit=cover +
      // env(safe-area-inset-*)). This is what makes the web content fill the
      // whole screen instead of being padded under the status bar / home bar.
      webview.scrollView.contentInsetAdjustmentBehavior =
          UIScrollViewContentInsetAdjustmentNever;
      // No pinch-zoom (app, not a zoomable document).
      webview.scrollView.minimumZoomScale = 1.0;
      webview.scrollView.maximumZoomScale = 1.0;
      webview.scrollView.bouncesZoom = NO;
      if ([webview respondsToSelector:@selector(setInspectable:)]) {
        [webview setInspectable:YES];
      }

      UIViewController* vc = [[UIViewController alloc] init];
      vc.view.backgroundColor = UIColor.blackColor;
      [vc.view addSubview:webview];

      // Pin to the view's full bounds (not the safe area) so the webview is
      // truly full-screen; autoresizing alone left a one-shot frame.
      [NSLayoutConstraint activateConstraints:@[
        [webview.topAnchor constraintEqualToAnchor:vc.view.topAnchor],
        [webview.bottomAnchor constraintEqualToAnchor:vc.view.bottomAnchor],
        [webview.leadingAnchor constraintEqualToAnchor:vc.view.leadingAnchor],
        [webview.trailingAnchor constraintEqualToAnchor:vc.view.trailingAnchor],
      ]];

      UIWindow* window = [[UIWindow alloc] initWithFrame:bounds];
      window.rootViewController = vc;
      [window makeKeyAndVisible];

      IOSWindowState state;
      state.window_id = window_id;
      state.window = window;
      state.controller = vc;
      state.webview = webview;
      state.handler = handler;
      {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        windows_[window_id] = state;
      }
      (void)width;
      (void)height;
    }
  });
}

void WKWebViewIOSBackend::CloseWindow(uint32_t window_id) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto it = windows_.find(window_id);
      if (it == windows_.end()) return;
      it->second.window.hidden = YES;
      windows_.erase(it);
    }
  });
}

void WKWebViewIOSBackend::Navigate(uint32_t window_id, const std::string& url) {
  std::string urlCopy = url;
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (!state) return;
      if (urlCopy.rfind("data:text/html,", 0) == 0) {
        NSString* html = [NSString stringWithUTF8String:urlCopy.c_str() + 15];
        html = [html stringByRemovingPercentEncoding];
        [state->webview loadHTMLString:html baseURL:nil];
        return;
      }
      NSURL* nsurl =
          [NSURL URLWithString:[NSString stringWithUTF8String:urlCopy.c_str()]];
      if (nsurl && nsurl.scheme.length > 0) {
        [state->webview loadRequest:[NSURLRequest requestWithURL:nsurl]];
      }
    }
  });
}

void WKWebViewIOSBackend::ExecuteJs(uint32_t window_id,
                                    const std::string& script,
                                    laufey_js_result_fn callback,
                                    void* callback_data) {
  std::string scriptCopy = script;
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (!state) {
        if (callback) callback(nullptr, nullptr, callback_data);
        return;
      }
      NSString* js = [NSString stringWithUTF8String:scriptCopy.c_str()];
      [state->webview evaluateJavaScript:js
                       completionHandler:^(id, NSError*) {
                         if (callback)
                           callback(nullptr, nullptr, callback_data);
                       }];
    }
  });
}

void WKWebViewIOSBackend::RespondToJsCall(uint32_t window_id, uint64_t call_id,
                                          laufey::ValuePtr result,
                                          laufey::ValuePtr error) {
  std::string resultJson = json::Serialize(result);
  std::string errorJson = error ? json::Serialize(error) : "null";
  std::string script = BuildRespondScript(call_id, resultJson, errorJson,
                                          static_cast<bool>(error));
  EvalOnWindow(window_id, script);
}

void WKWebViewIOSBackend::EvalOnWindow(uint32_t window_id,
                                       const std::string& script) {
  std::string scriptCopy = script;
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(windows_mutex_);
      auto* state = GetWindow(window_id);
      if (!state) return;
      NSString* js = [NSString stringWithUTF8String:scriptCopy.c_str()];
      [state->webview evaluateJavaScript:js completionHandler:nil];
    }
  });
}

LaufeyBackend* CreateLaufeyBackend() {
  return new WKWebViewIOSBackend();
}
