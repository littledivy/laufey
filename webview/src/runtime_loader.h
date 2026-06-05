// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef LAUFEY_RUNTIME_LOADER_H_
#define LAUFEY_RUNTIME_LOADER_H_

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <map>

#include "laufey.h"
#include "webview_value.h"

#ifdef _WIN32
// <windows.h> defines CreateWindow/CreateWindowEx as macros that collide with
// our backend methods; pull it in once and drop them.
#include <windows.h>
#undef CreateWindow
#undef CreateWindowEx
#endif

class LaufeyBackend;

class RuntimeLoader {
 public:
  static RuntimeLoader* GetInstance();

  bool Load(const std::string& path);

  bool Start();

  void Shutdown();

  void SetBackend(LaufeyBackend* backend) {
    backend_ = backend;
  }
  LaufeyBackend* GetBackend() {
    return backend_;
  }
  const laufey_backend_api_t& GetBackendApi() const {
    return backend_api_;
  }

  uint32_t AllocateWindowId() {
    return next_window_id_.fetch_add(1);
  }

  void StoreCallWindow(uint64_t call_id, uint32_t window_id) {
    std::lock_guard<std::mutex> lock(call_map_mutex_);
    call_to_window_[call_id] = window_id;
  }

  uint32_t ConsumeCallWindow(uint64_t call_id) {
    std::lock_guard<std::mutex> lock(call_map_mutex_);
    auto it = call_to_window_.find(call_id);
    if (it != call_to_window_.end()) {
      uint32_t wid = it->second;
      call_to_window_.erase(it);
      return wid;
    }
    return 0;
  }

  void OnJsCall(uint32_t window_id, uint64_t call_id,
                const std::string& method_path, laufey::ValuePtr args);

  void PollPendingJsCalls();

  void JsCallRespond(uint32_t window_id, uint64_t call_id, laufey::ValuePtr result,
                     laufey::ValuePtr error);

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

  LaufeyBackend* backend_ = nullptr;
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

  std::atomic<uint32_t> next_window_id_{1};
  std::map<uint64_t, uint32_t> call_to_window_;
  std::mutex call_map_mutex_;

  void (*js_call_notify_fn_)(void*) = nullptr;
  void* js_call_notify_data_ = nullptr;
  std::mutex notify_mutex_;

  std::string js_namespace_ = "Laufey";
  mutable std::mutex js_namespace_mutex_;

  struct PendingJsCall {
    uint32_t window_id;
    uint64_t call_id;
    std::string method_path;
    laufey::ValuePtr args;
  };
  std::queue<PendingJsCall> pending_js_calls_;
  std::mutex pending_mutex_;

  static RuntimeLoader* instance_;
};

class LaufeyBackend {
 public:
  virtual ~LaufeyBackend() = default;

  // Window lifecycle
  virtual void CreateWindow(uint32_t window_id, int width, int height) = 0;
  // Create with creation-time style flags (LAUFEY_WINDOW_FLAG_*). Default
  // ignores the flags and creates a plain window; platform backends override
  // to honor frameless / non-activating-panel flags.
  virtual void CreateWindowEx(uint32_t window_id, int width, int height,
                              uint32_t /*flags*/) {
    CreateWindow(window_id, width, height);
  }
  virtual void CloseWindow(uint32_t window_id) = 0;

  // Per-window operations
  virtual void Navigate(uint32_t window_id, const std::string& url) = 0;
  virtual void SetTitle(uint32_t window_id, const std::string& title) = 0;
  virtual void ExecuteJs(uint32_t window_id, const std::string& script,
                         laufey_js_result_fn callback, void* callback_data) = 0;
  virtual void SetWindowSize(uint32_t window_id, int width, int height) = 0;
  virtual void GetWindowSize(uint32_t window_id, int* width, int* height) = 0;
  virtual void SetWindowPosition(uint32_t window_id, int x, int y) = 0;
  virtual void GetWindowPosition(uint32_t window_id, int* x, int* y) = 0;
  virtual void SetResizable(uint32_t window_id, bool resizable) = 0;
  virtual bool IsResizable(uint32_t window_id) = 0;
  virtual void SetAlwaysOnTop(uint32_t window_id, bool always_on_top) = 0;
  virtual bool IsAlwaysOnTop(uint32_t window_id) = 0;
  virtual bool IsVisible(uint32_t window_id) = 0;
  virtual void Show(uint32_t window_id) = 0;
  virtual void Hide(uint32_t window_id) = 0;
  virtual void Focus(uint32_t window_id) = 0;

  // Global operations
  virtual void Quit() = 0;
  virtual void PostUiTask(void (*task)(void*), void* data) = 0;
  virtual void Run() = 0;

  // JS interop (broadcast to all windows for callback operations)
  virtual void InvokeJsCallback(uint32_t window_id, uint64_t callback_id,
                                laufey::ValuePtr args) = 0;
  virtual void ReleaseJsCallback(uint32_t window_id, uint64_t callback_id) = 0;
  virtual void RespondToJsCall(uint32_t window_id, uint64_t call_id,
                               laufey::ValuePtr result, laufey::ValuePtr error) = 0;

  virtual void SetApplicationMenu(uint32_t window_id,
                                  laufey_value_t* menu_template,
                                  const laufey_backend_api_t* api,
                                  laufey_menu_click_fn on_click,
                                  void* on_click_data) = 0;

  virtual void ShowContextMenu(uint32_t window_id, int x, int y,
                               laufey_value_t* menu_template,
                               const laufey_backend_api_t* api,
                               laufey_menu_click_fn on_click,
                               void* on_click_data) = 0;

  virtual void OpenDevTools(uint32_t window_id) = 0;

  // Show a modal dialog and BLOCK until the user dismisses it. Backends
  // run the platform's native modal loop (`runModal` / `MessageBoxW` /
  // `gtk_dialog_run`), which itself pumps OS events while the dialog is
  // up so other LAUFEY windows continue to render and respond.
  // Returns 1 if OK was pressed, 0 otherwise. For prompts, on a confirmed
  // result `*out_input_value` is set to a `strdup`'d UTF-8 string the
  // caller frees via `BackendStringFree`. NULL otherwise.
  virtual int ShowDialog(uint32_t window_id, int dialog_type,
                         const std::string& title, const std::string& message,
                         const std::string& default_value,
                         char** out_input_value) = 0;

  // --- Dock / taskbar ---
  // Default implementations are no-ops so platforms that don't support a
  // given operation inherit silently. The canonical macOS implementation is
  // in WKWebViewBackend; Windows/Linux override Bounce only.
  virtual void SetDockBadge(const char* /*badge_or_null*/) {}
  virtual void BounceDock(int /*type*/) {}
  virtual void SetDockMenu(laufey_value_t* /*menu_template*/,
                           const laufey_backend_api_t* /*api*/,
                           laufey_menu_click_fn /*on_click*/,
                           void* /*on_click_data*/) {}
  virtual void SetDockVisible(bool /*visible*/) {}
  virtual void SetDockReopenHandler(laufey_dock_reopen_fn /*handler*/,
                                    void* /*user_data*/) {}

  // --- Tray / status-bar icon ---
  virtual uint32_t CreateTrayIcon() {
    return 0;
  }
  virtual void DestroyTrayIcon(uint32_t /*tray_id*/) {}
  virtual void SetTrayIcon(uint32_t /*tray_id*/, const void* /*png_bytes*/,
                           size_t /*len*/) {}
  virtual void SetTrayTooltip(uint32_t /*tray_id*/,
                              const char* /*tooltip_or_null*/) {}
  virtual void SetTrayMenu(uint32_t /*tray_id*/, laufey_value_t* /*menu_template*/,
                           const laufey_backend_api_t* /*api*/,
                           laufey_menu_click_fn /*on_click*/,
                           void* /*on_click_data*/) {}
  virtual void SetTrayClickHandler(uint32_t /*tray_id*/,
                                   laufey_tray_click_fn /*handler*/,
                                   void* /*user_data*/) {}
  virtual void SetTrayDoubleClickHandler(uint32_t /*tray_id*/,
                                         laufey_tray_click_fn /*handler*/,
                                         void* /*user_data*/) {}
  virtual void SetTrayIconDark(uint32_t /*tray_id*/, const void* /*png_bytes*/,
                               size_t /*len*/) {}
  // Tray icon screen bounds (top-left origin, DIP). Default: unsupported.
  virtual bool GetTrayIconBounds(uint32_t /*tray_id*/, int* /*x*/, int* /*y*/,
                                 int* /*width*/, int* /*height*/) {
    return false;
  }

  // --- Notifications ---
  // Default: not supported. Subclasses override per-platform.
  virtual uint32_t ShowNotification(laufey_value_t* options,
                                    const laufey_backend_api_t* /*api*/,
                                    laufey_notification_event_fn /*on_event*/,
                                    void* /*user_data*/) {
    // Subclass owns the options pointer if it accepts the call. Default
    // path frees so we don't leak.
    (void)options;
    return 0;
  }
  virtual void CloseNotification(uint32_t /*notification_id*/) {}

  // --- Permissions / runtime authorization ---
  // Default: synchronously report UNSUPPORTED. macOS subclass overrides
  // to drive UNUserNotificationCenter. Windows/Linux subclasses report
  // GRANTED for LAUFEY_PERMISSION_NOTIFICATIONS (the balloon / libnotify
  // APIs they use have no permission model).
  virtual void QueryPermission(int /*kind*/, laufey_permission_callback_fn cb,
                               void* user_data) {
    if (cb)
      cb(user_data, LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
  }
  virtual void RequestPermission(int /*kind*/, laufey_permission_callback_fn cb,
                                 void* user_data) {
    if (cb)
      cb(user_data, LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
  }
};

LaufeyBackend* CreateLaufeyBackend();

#endif  // LAUFEY_RUNTIME_LOADER_H_
