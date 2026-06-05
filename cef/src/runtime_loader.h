// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef LAUFEY_RUNTIME_LOADER_H_
#define LAUFEY_RUNTIME_LOADER_H_

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <map>

#include "include/cef_browser.h"
#include "include/cef_values.h"
#include <laufey.h>

// laufey_value and the value_* marshalling are shared across backends
// (backend-common/include/laufey_value.h). CEF stores values as laufey::Value and
// converts to/from CefValue only at the renderer<->browser IPC boundary
// (CefValueToLaufey / LaufeyToCefValue in runtime_loader.cc).
#include "laufey_value.h"

class RuntimeLoader {
 public:
  static RuntimeLoader* GetInstance();

  bool Load(const std::string& path);

  bool Start();

  void Shutdown();

  const laufey_backend_api_t& GetBackendApi() const {
    return backend_api_;
  }

  uint32_t AllocateWindowId() {
    return next_window_id_.fetch_add(1);
  }

  void RegisterBrowser(uint32_t window_id, CefRefPtr<CefBrowser> browser) {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    browsers_[window_id] = browser;
    browser_id_to_laufey_id_[browser->GetIdentifier()] = window_id;
    browser_ready_cv_.notify_all();
  }

  // Block until the browser for window_id has been registered (with timeout).
  bool WaitForBrowser(uint32_t window_id, int timeout_ms = 5000) {
    std::unique_lock<std::mutex> lock(windows_mutex_);
    return browser_ready_cv_.wait_for(
        lock, std::chrono::milliseconds(timeout_ms),
        [&]() { return browsers_.find(window_id) != browsers_.end(); });
  }

  void UnregisterBrowser(uint32_t window_id) {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto it = browsers_.find(window_id);
    if (it != browsers_.end()) {
      browser_id_to_laufey_id_.erase(it->second->GetIdentifier());
      browsers_.erase(it);
    }
  }

  CefRefPtr<CefBrowser> GetBrowserForWindow(uint32_t window_id) {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto it = browsers_.find(window_id);
    return it != browsers_.end() ? it->second : nullptr;
  }

  uint32_t GetLaufeyIdForBrowser(CefRefPtr<CefBrowser> browser) {
    if (!browser)
      return 0;
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto it = browser_id_to_laufey_id_.find(browser->GetIdentifier());
    return it != browser_id_to_laufey_id_.end() ? it->second : 0;
  }

  uint32_t GetLaufeyIdForBrowserId(int browser_id) {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto it = browser_id_to_laufey_id_.find(browser_id);
    return it != browser_id_to_laufey_id_.end() ? it->second : 0;
  }

  bool HasWindows() {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    return !browsers_.empty();
  }

  void StoreCallWindow(uint64_t call_id, uint32_t window_id) {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    call_to_window_[call_id] = window_id;
  }

  uint32_t ConsumeCallWindow(uint64_t call_id) {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto it = call_to_window_.find(call_id);
    if (it != call_to_window_.end()) {
      uint32_t wid = it->second;
      call_to_window_.erase(it);
      return wid;
    }
    return 0;
  }

  void RegisterNSWindow(void* nswindow, uint32_t window_id) {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    nswindow_to_laufey_id_[nswindow] = window_id;
  }

  void UnregisterNSWindow(void* nswindow) {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    nswindow_to_laufey_id_.erase(nswindow);
  }

  uint32_t GetLaufeyIdForNSWindow(void* nswindow) {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto it = nswindow_to_laufey_id_.find(nswindow);
    return it != nswindow_to_laufey_id_.end() ? it->second : 0;
  }

  void RegisterNativeHandle(void* handle, uint32_t window_id) {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    native_handle_to_laufey_id_[handle] = window_id;
  }

  void UnregisterNativeHandle(void* handle) {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    native_handle_to_laufey_id_.erase(handle);
  }

  uint32_t GetLaufeyIdForNativeHandle(void* handle) {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto it = native_handle_to_laufey_id_.find(handle);
    return it != native_handle_to_laufey_id_.end() ? it->second : 0;
  }

  template <typename F>
  void ForEachBrowser(F&& fn) {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    for (auto& [wid, browser] : browsers_) {
      fn(browser);
    }
  }

  template <typename F>
  void ForEachBrowserWithId(F&& fn) {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    for (auto& [wid, browser] : browsers_) {
      fn(wid, browser);
    }
  }

  void OnJsCall(uint32_t window_id, uint64_t call_id,
                const std::string& method_path, CefRefPtr<CefListValue> args);

  void PollPendingJsCalls();

  void SetJsCallHandler(laufey_js_call_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    js_call_handler_ = handler;
    js_call_user_data_ = user_data;
  }

  void SetKeyboardEventHandler(laufey_keyboard_event_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(keyboard_mutex_);
    keyboard_handler_ = handler;
    keyboard_user_data_ = user_data;
  }

  void DispatchKeyboardEvent(uint32_t window_id, int state, const char* key,
                             const char* code, uint32_t modifiers,
                             bool repeat) {
    std::lock_guard<std::mutex> lock(keyboard_mutex_);
    if (keyboard_handler_) {
      keyboard_handler_(keyboard_user_data_, window_id, state, key, code,
                        modifiers, repeat);
    }
  }

  void SetMouseClickHandler(laufey_mouse_click_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(mouse_mutex_);
    mouse_click_handler_ = handler;
    mouse_click_user_data_ = user_data;
  }

  void DispatchMouseClickEvent(uint32_t window_id, int state, int button,
                               double x, double y, uint32_t modifiers,
                               int32_t click_count) {
    std::lock_guard<std::mutex> lock(mouse_mutex_);
    if (mouse_click_handler_) {
      mouse_click_handler_(mouse_click_user_data_, window_id, state, button, x,
                           y, modifiers, click_count);
    }
  }

  void SetMouseMoveHandler(laufey_mouse_move_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(mouse_move_mutex_);
    mouse_move_handler_ = handler;
    mouse_move_user_data_ = user_data;
  }

  void DispatchMouseMoveEvent(uint32_t window_id, double x, double y,
                              uint32_t modifiers) {
    std::lock_guard<std::mutex> lock(mouse_move_mutex_);
    if (mouse_move_handler_) {
      mouse_move_handler_(mouse_move_user_data_, window_id, x, y, modifiers);
    }
  }

  void SetWheelHandler(laufey_wheel_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(wheel_mutex_);
    wheel_handler_ = handler;
    wheel_user_data_ = user_data;
  }

  void DispatchWheelEvent(uint32_t window_id, double delta_x, double delta_y,
                          double x, double y, uint32_t modifiers,
                          int32_t delta_mode) {
    std::lock_guard<std::mutex> lock(wheel_mutex_);
    if (wheel_handler_) {
      wheel_handler_(wheel_user_data_, window_id, delta_x, delta_y, x, y,
                     modifiers, delta_mode);
    }
  }

  void SetCursorEnterLeaveHandler(laufey_cursor_enter_leave_fn handler,
                                  void* user_data) {
    std::lock_guard<std::mutex> lock(cursor_enter_leave_mutex_);
    cursor_enter_leave_handler_ = handler;
    cursor_enter_leave_user_data_ = user_data;
  }

  void DispatchCursorEnterLeaveEvent(uint32_t window_id, int entered, double x,
                                     double y, uint32_t modifiers) {
    std::lock_guard<std::mutex> lock(cursor_enter_leave_mutex_);
    if (cursor_enter_leave_handler_) {
      cursor_enter_leave_handler_(cursor_enter_leave_user_data_, window_id,
                                  entered, x, y, modifiers);
    }
  }

  void SetFocusedHandler(laufey_focused_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(focused_mutex_);
    focused_handler_ = handler;
    focused_user_data_ = user_data;
  }

  void DispatchFocusedEvent(uint32_t window_id, int focused) {
    std::lock_guard<std::mutex> lock(focused_mutex_);
    if (focused_handler_) {
      focused_handler_(focused_user_data_, window_id, focused);
    }
  }

  void SetResizeHandler(laufey_resize_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(resize_mutex_);
    resize_handler_ = handler;
    resize_user_data_ = user_data;
  }

  void DispatchResizeEvent(uint32_t window_id, int width, int height) {
    std::lock_guard<std::mutex> lock(resize_mutex_);
    if (resize_handler_) {
      resize_handler_(resize_user_data_, window_id, width, height);
    }
  }

  void SetMoveHandler(laufey_move_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(move_mutex_);
    move_handler_ = handler;
    move_user_data_ = user_data;
  }

  void DispatchMoveEvent(uint32_t window_id, int x, int y) {
    std::lock_guard<std::mutex> lock(move_mutex_);
    if (move_handler_) {
      move_handler_(move_user_data_, window_id, x, y);
    }
  }

  void SetCloseRequestedHandler(laufey_close_requested_fn handler,
                                void* user_data) {
    std::lock_guard<std::mutex> lock(close_requested_mutex_);
    close_requested_handler_ = handler;
    close_requested_user_data_ = user_data;
  }

  void DispatchCloseRequestedEvent(uint32_t window_id) {
    std::lock_guard<std::mutex> lock(close_requested_mutex_);
    if (close_requested_handler_) {
      close_requested_handler_(close_requested_user_data_, window_id);
    }
  }

  void SetJsCallNotify(void (*notify_fn)(void*), void* notify_data) {
    std::lock_guard<std::mutex> lock(notify_mutex_);
    js_call_notify_fn_ = notify_fn;
    js_call_notify_data_ = notify_data;
  }

  uint64_t StoreEvalCallback(laufey_js_result_fn callback, void* callback_data) {
    std::lock_guard<std::mutex> lock(eval_mutex_);
    uint64_t id = next_eval_id_++;
    pending_evals_[id] = {callback, callback_data};
    return id;
  }

  void HandleEvalResult(uint64_t eval_id, CefRefPtr<CefValue> result,
                        const std::string& error);

  void SetJsNamespace(const std::string& name) {
    std::lock_guard<std::mutex> lock(js_namespace_mutex_);
    js_namespace_ = name;
  }
  std::string GetJsNamespace() const {
    std::lock_guard<std::mutex> lock(js_namespace_mutex_);
    return js_namespace_;
  }

 private:
  RuntimeLoader();
  ~RuntimeLoader();

  void RuntimeThread();
  void InitializeBackendApi();

  void* library_handle_ = nullptr;
  laufey_runtime_init_fn init_fn_ = nullptr;
  laufey_runtime_start_fn start_fn_ = nullptr;
  laufey_runtime_shutdown_fn shutdown_fn_ = nullptr;

  std::thread runtime_thread_;
  std::atomic<bool> running_{false};

  std::map<uint32_t, CefRefPtr<CefBrowser>> browsers_;
  std::map<int, uint32_t>
      browser_id_to_laufey_id_;  // CefBrowser::GetIdentifier() -> laufey_id
  std::map<uint64_t, uint32_t>
      call_to_window_;  // call_id -> window_id for JsCallRespond
  std::map<void*, uint32_t> nswindow_to_laufey_id_;
  std::map<void*, uint32_t> native_handle_to_laufey_id_;
  std::mutex windows_mutex_;
  std::condition_variable browser_ready_cv_;
  std::atomic<uint32_t> next_window_id_{1};

  laufey_backend_api_t backend_api_;

  laufey_js_call_fn js_call_handler_ = nullptr;
  void* js_call_user_data_ = nullptr;
  std::mutex handler_mutex_;

  laufey_keyboard_event_fn keyboard_handler_ = nullptr;
  void* keyboard_user_data_ = nullptr;
  std::mutex keyboard_mutex_;

  laufey_mouse_click_fn mouse_click_handler_ = nullptr;
  void* mouse_click_user_data_ = nullptr;
  std::mutex mouse_mutex_;

  laufey_mouse_move_fn mouse_move_handler_ = nullptr;
  void* mouse_move_user_data_ = nullptr;
  std::mutex mouse_move_mutex_;

  laufey_wheel_fn wheel_handler_ = nullptr;
  void* wheel_user_data_ = nullptr;
  std::mutex wheel_mutex_;

  laufey_cursor_enter_leave_fn cursor_enter_leave_handler_ = nullptr;
  void* cursor_enter_leave_user_data_ = nullptr;
  std::mutex cursor_enter_leave_mutex_;

  laufey_focused_fn focused_handler_ = nullptr;
  void* focused_user_data_ = nullptr;
  std::mutex focused_mutex_;

  laufey_resize_fn resize_handler_ = nullptr;
  void* resize_user_data_ = nullptr;
  std::mutex resize_mutex_;

  laufey_move_fn move_handler_ = nullptr;
  void* move_user_data_ = nullptr;
  std::mutex move_mutex_;

  laufey_close_requested_fn close_requested_handler_ = nullptr;
  void* close_requested_user_data_ = nullptr;
  std::mutex close_requested_mutex_;

  void (*js_call_notify_fn_)(void*) = nullptr;
  void* js_call_notify_data_ = nullptr;
  std::mutex notify_mutex_;

  std::string js_namespace_ = "Laufey";
  mutable std::mutex js_namespace_mutex_;

  struct PendingEval {
    laufey_js_result_fn callback;
    void* callback_data;
  };
  std::map<uint64_t, PendingEval> pending_evals_;
  std::atomic<uint64_t> next_eval_id_{1};
  std::mutex eval_mutex_;

  struct PendingJsCall {
    uint32_t window_id;
    uint64_t call_id;
    std::string method_path;
    CefRefPtr<CefListValue> args;
  };
  std::queue<PendingJsCall> pending_js_calls_;
  std::mutex pending_mutex_;

  static RuntimeLoader* instance_;
};

// Platform-specific native event monitor hooks.
// Implemented in the platform-specific file (e.g. runtime_loader_mac.mm).
void InstallNativeMouseMonitor();
void RemoveNativeMouseMonitor();

#ifdef __APPLE__
// NSWindow helpers for cross-platform code (implemented in
// runtime_loader_mac.mm).
void RegisterNSWindowForCefHandle(void* cef_handle, uint32_t window_id);
void UnregisterNSWindowForCefHandle(void* cef_handle);
void SetNSWindowResizable(void* cef_handle, bool resizable);
bool IsNSWindowResizable(void* cef_handle);
// Reconfigure the window backing a CEF handle to behave as a floating,
// non-activating utility panel (used for tray popovers): floats above
// normal windows, joins all spaces, and doesn't steal focus from the
// foreground app when shown.
void ConfigureNSWindowAsPanelForCefHandle(void* cef_handle);
// Give the window a transparent, full-size-content-view title bar so the web
// content draws under it and the traffic-light buttons overlay the page
// (Electron `titleBarStyle: 'hidden'`). For
// LAUFEY_WINDOW_FLAG_TRANSPARENT_TITLEBAR.
void ConfigureNSWindowTransparentTitlebarForCefHandle(void* cef_handle);
#endif

#ifdef _WIN32
// Reconfigure the HWND to a non-activating tool window (WS_EX_NOACTIVATE |
// WS_EX_TOOLWINDOW) so showing it doesn't steal focus from the foreground
// app. Implemented inline in runtime_loader.cc.
void ConfigureWin32WindowAsPanel(void* hwnd);
#endif

#ifdef __linux__
// Register a CEF window for XI2 event monitoring (implemented in
// main_linux.cc).
void MonitorLinuxWindowEvents(unsigned long xid);
void SetLinuxWindowResizable(unsigned long xid, bool resizable);
bool IsLinuxWindowResizable(unsigned long xid);
// Mark the X11 window as a utility/panel window (_NET_WM_WINDOW_TYPE_UTILITY,
// skip taskbar/pager) so the WM treats it as an auxiliary panel that doesn't
// take part in normal focus/taskbar handling. Implemented in main_linux.cc.
void ConfigureLinuxWindowAsPanel(unsigned long xid);
#endif

#endif
