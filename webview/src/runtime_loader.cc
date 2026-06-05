// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "runtime_loader.h"

#ifndef _WIN32
#include <dlfcn.h>
#else
#include <windows.h>
// windows.h defines CreateWindow as a macro which conflicts with
// LaufeyBackend::CreateWindow
#undef CreateWindow
#endif

#include <iostream>
#include <cstring>

RuntimeLoader* RuntimeLoader::instance_ = nullptr;

static void Backend_Navigate(void* data, uint32_t window_id, const char* url) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend && url) {
    backend->Navigate(window_id, url);
  }
}

static void Backend_SetTitle(void* data, uint32_t window_id,
                             const char* title) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend && title) {
    backend->SetTitle(window_id, title);
  }
}

static void Backend_ExecuteJs(void* data, uint32_t window_id,
                              const char* script, laufey_js_result_fn callback,
                              void* callback_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend && script) {
    backend->ExecuteJs(window_id, script, callback, callback_data);
  }
}

static void Backend_Quit(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->Quit();
  }
}

static void Backend_SetWindowSize(void* data, uint32_t window_id, int width,
                                  int height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->SetWindowSize(window_id, width, height);
  }
}

static void Backend_GetWindowSize(void* data, uint32_t window_id, int* width,
                                  int* height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->GetWindowSize(window_id, width, height);
  }
}

static void Backend_SetWindowPosition(void* data, uint32_t window_id, int x,
                                      int y) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->SetWindowPosition(window_id, x, y);
  }
}

static void Backend_GetWindowPosition(void* data, uint32_t window_id, int* x,
                                      int* y) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->GetWindowPosition(window_id, x, y);
  }
}

static void Backend_SetResizable(void* data, uint32_t window_id,
                                 bool resizable) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->SetResizable(window_id, resizable);
  }
}

static bool Backend_IsResizable(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    return backend->IsResizable(window_id);
  }
  return false;
}

static void Backend_SetAlwaysOnTop(void* data, uint32_t window_id,
                                   bool always_on_top) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->SetAlwaysOnTop(window_id, always_on_top);
  }
}

static bool Backend_IsAlwaysOnTop(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    return backend->IsAlwaysOnTop(window_id);
  }
  return false;
}

static bool Backend_IsVisible(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    return backend->IsVisible(window_id);
  }
  return false;
}

static void Backend_Show(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->Show(window_id);
  }
}

static void Backend_Hide(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->Hide(window_id);
  }
}

static void Backend_Focus(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->Focus(window_id);
  }
}

static void Backend_PostUiTask(void* data, void (*task)(void*),
                               void* task_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend && task) {
    backend->PostUiTask(task, task_data);
  }
}

static void Backend_SetJsCallHandler(void* data, laufey_js_call_fn handler,
                                     void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetJsCallHandler(handler, user_data);
}

static void Backend_JsCallRespond(void* data, uint64_t call_id,
                                  laufey_value_t* result, laufey_value_t* error) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  uint32_t window_id = loader->ConsumeCallWindow(call_id);
  laufey::ValuePtr resultPtr =
      (result && result->value) ? result->value : laufey::Value::Null();
  laufey::ValuePtr errorPtr =
      (error && error->value) ? error->value : laufey::Value::Null();
  loader->JsCallRespond(window_id, call_id, resultPtr, errorPtr);
}

static void Backend_InvokeJsCallback(void* data, uint64_t callback_id,
                                     laufey_value_t* args) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    laufey::ValuePtr argsPtr =
        (args && args->value) ? args->value : laufey::Value::List();
    // Broadcast to window 0 (all windows) since callback_id isn't tied to a
    // window
    backend->InvokeJsCallback(0, callback_id, argsPtr);
  }
}

static void Backend_SetKeyboardEventHandler(void* data,
                                            laufey_keyboard_event_fn handler,
                                            void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetKeyboardEventHandler(handler, user_data);
}

static void Backend_SetMouseClickHandler(void* data, laufey_mouse_click_fn handler,
                                         void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetMouseClickHandler(handler, user_data);
}

static void Backend_SetMouseMoveHandler(void* data, laufey_mouse_move_fn handler,
                                        void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetMouseMoveHandler(handler, user_data);
}

static void Backend_SetWheelHandler(void* data, laufey_wheel_fn handler,
                                    void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetWheelHandler(handler, user_data);
}

static void Backend_SetCursorEnterLeaveHandler(
    void* data, laufey_cursor_enter_leave_fn handler, void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetCursorEnterLeaveHandler(handler, user_data);
}

static void Backend_SetFocusedHandler(void* data, laufey_focused_fn handler,
                                      void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetFocusedHandler(handler, user_data);
}

static void Backend_SetResizeHandler(void* data, laufey_resize_fn handler,
                                     void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetResizeHandler(handler, user_data);
}

static void Backend_SetMoveHandler(void* data, laufey_move_fn handler,
                                   void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetMoveHandler(handler, user_data);
}

static void Backend_ReleaseJsCallback(void* data, uint64_t callback_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->ReleaseJsCallback(0, callback_id);
  }
}

static void Backend_PollJsCalls(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->PollPendingJsCalls();
}

static void Backend_SetJsCallNotify(void* data, void (*notify_fn)(void*),
                                    void* notify_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetJsCallNotify(notify_fn, notify_data);
}

static void Backend_SetApplicationMenu(void* data, uint32_t window_id,
                                       laufey_value_t* menu_template,
                                       laufey_menu_click_fn on_click,
                                       void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend && menu_template) {
    backend->SetApplicationMenu(window_id, menu_template,
                                &loader->GetBackendApi(), on_click,
                                on_click_data);
  }
}

static void Backend_ShowContextMenu(void* data, uint32_t window_id, int x,
                                    int y, laufey_value_t* menu_template,
                                    laufey_menu_click_fn on_click,
                                    void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend && menu_template) {
    backend->ShowContextMenu(window_id, x, y, menu_template,
                             &loader->GetBackendApi(), on_click, on_click_data);
  }
}

static void Backend_OpenDevTools(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->OpenDevTools(window_id);
  }
}

static void Backend_SetJsNamespace(void* data, const char* name) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (name) {
    loader->SetJsNamespace(name);
  }
}

static uint32_t Backend_CreateWindow(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  uint32_t window_id = loader->AllocateWindowId();
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->CreateWindow(window_id, 800, 600);
  }
  return window_id;
}

static uint32_t Backend_CreateWindowEx(void* data, uint32_t flags) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  uint32_t window_id = loader->AllocateWindowId();
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->CreateWindowEx(window_id, 800, 600, flags);
  }
  return window_id;
}

static bool Backend_GetTrayIconBounds(void* data, uint32_t tray_id, int* x,
                                      int* y, int* width, int* height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (!backend) {
    return false;
  }
  return backend->GetTrayIconBounds(tray_id, x, y, width, height);
}

static void Backend_CloseWindow(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->CloseWindow(window_id);
  }
}

static void Backend_SetCloseRequestedHandler(void* data,
                                             laufey_close_requested_fn handler,
                                             void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetCloseRequestedHandler(handler, user_data);
}

static int Backend_ShowDialog(void* data, uint32_t window_id, int dialog_type,
                              const char* title, const char* message,
                              const char* default_value,
                              char** out_input_value) {
  if (out_input_value)
    *out_input_value = nullptr;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (!backend)
    return 0;
  std::string t = title ? title : "";
  std::string m = message ? message : "";
  std::string d = default_value ? default_value : "";
  return backend->ShowDialog(window_id, dialog_type, t, m, d, out_input_value);
}

static void Backend_StringFree(void* /*backend_data*/, char* s) {
  if (s)
    free(s);
}

// --- Dock / taskbar ---

static void Backend_SetDockBadge(void* data, const char* badge_or_null) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->SetDockBadge(badge_or_null);
  }
}

static void Backend_BounceDock(void* data, int type) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->BounceDock(type);
  }
}

static void Backend_SetDockMenu(void* data, laufey_value_t* menu_template,
                                laufey_menu_click_fn on_click,
                                void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->SetDockMenu(menu_template, &loader->GetBackendApi(), on_click,
                         on_click_data);
  }
}

static void Backend_SetDockVisible(void* data, bool visible) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->SetDockVisible(visible);
  }
}

static void Backend_SetDockReopenHandler(void* data, laufey_dock_reopen_fn handler,
                                         void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->SetDockReopenHandler(handler, user_data);
  }
}

// --- Tray / status bar ---

static uint32_t Backend_CreateTrayIcon(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->CreateTrayIcon();
  return 0;
}

static void Backend_DestroyTrayIcon(void* data, uint32_t tray_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->DestroyTrayIcon(tray_id);
}

static void Backend_SetTrayIcon(void* data, uint32_t tray_id,
                                const void* png_bytes, size_t len) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetTrayIcon(tray_id, png_bytes, len);
}

static void Backend_SetTrayTooltip(void* data, uint32_t tray_id,
                                   const char* tooltip_or_null) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetTrayTooltip(tray_id, tooltip_or_null);
}

static void Backend_SetTrayMenu(void* data, uint32_t tray_id,
                                laufey_value_t* menu_template,
                                laufey_menu_click_fn on_click,
                                void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetTrayMenu(tray_id, menu_template, &loader->GetBackendApi(),
                         on_click, on_click_data);
}

static void Backend_SetTrayClickHandler(void* data, uint32_t tray_id,
                                        laufey_tray_click_fn handler,
                                        void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetTrayClickHandler(tray_id, handler, user_data);
}

static void Backend_SetTrayDoubleClickHandler(void* data, uint32_t tray_id,
                                              laufey_tray_click_fn handler,
                                              void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetTrayDoubleClickHandler(tray_id, handler, user_data);
}

static void Backend_SetTrayIconDark(void* data, uint32_t tray_id,
                                    const void* png_bytes, size_t len) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetTrayIconDark(tray_id, png_bytes, len);
}

static uint32_t Backend_ShowNotification(void* data, laufey_value_t* options,
                                         laufey_notification_event_fn on_event,
                                         void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (!backend) {
    if (options) {
      loader->GetBackendApi().value_free(options);
    }
    return 0;
  }
  return backend->ShowNotification(options, &loader->GetBackendApi(), on_event,
                                   user_data);
}

static void Backend_CloseNotification(void* data, uint32_t notification_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->CloseNotification(notification_id);
}

static void Backend_QueryPermission(void* data, int kind,
                                    laufey_permission_callback_fn cb,
                                    void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->QueryPermission(kind, cb, user_data);
  } else if (cb) {
    cb(user_data, LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
  }
}

static void Backend_RequestPermission(void* data, int kind,
                                      laufey_permission_callback_fn cb,
                                      void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->RequestPermission(kind, cb, user_data);
  } else if (cb) {
    cb(user_data, LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
  }
}

void RuntimeLoader::InitializeBackendApi() {
  memset(&backend_api_, 0, sizeof(backend_api_));
  backend_api_.version = LAUFEY_API_VERSION;
  backend_api_.backend_data = this;

  backend_api_.navigate = Backend_Navigate;
  backend_api_.set_title = Backend_SetTitle;
  backend_api_.execute_js = Backend_ExecuteJs;
  backend_api_.quit = Backend_Quit;
  backend_api_.set_window_size = Backend_SetWindowSize;
  backend_api_.get_window_size = Backend_GetWindowSize;
  backend_api_.set_window_position = Backend_SetWindowPosition;
  backend_api_.get_window_position = Backend_GetWindowPosition;
  backend_api_.set_resizable = Backend_SetResizable;
  backend_api_.is_resizable = Backend_IsResizable;
  backend_api_.set_always_on_top = Backend_SetAlwaysOnTop;
  backend_api_.is_always_on_top = Backend_IsAlwaysOnTop;
  backend_api_.is_visible = Backend_IsVisible;
  backend_api_.show = Backend_Show;
  backend_api_.hide = Backend_Hide;
  backend_api_.focus = Backend_Focus;
  backend_api_.post_ui_task = Backend_PostUiTask;

  laufey_register_value_api(&backend_api_);

  backend_api_.set_js_call_handler = Backend_SetJsCallHandler;
  backend_api_.js_call_respond = Backend_JsCallRespond;

  backend_api_.invoke_js_callback = Backend_InvokeJsCallback;
  backend_api_.release_js_callback = Backend_ReleaseJsCallback;

  backend_api_.get_window_handle = [](void*, uint32_t) -> void* {
    return nullptr;
  };
  backend_api_.get_display_handle = [](void*, uint32_t) -> void* {
    return nullptr;
  };
  backend_api_.get_window_handle_type = [](void*, uint32_t) -> int {
    return LAUFEY_WINDOW_HANDLE_UNKNOWN;
  };

  backend_api_.set_keyboard_event_handler = Backend_SetKeyboardEventHandler;
  backend_api_.set_mouse_click_handler = Backend_SetMouseClickHandler;
  backend_api_.set_mouse_move_handler = Backend_SetMouseMoveHandler;
  backend_api_.set_wheel_handler = Backend_SetWheelHandler;
  backend_api_.set_cursor_enter_leave_handler =
      Backend_SetCursorEnterLeaveHandler;
  backend_api_.set_focused_handler = Backend_SetFocusedHandler;
  backend_api_.set_resize_handler = Backend_SetResizeHandler;
  backend_api_.set_move_handler = Backend_SetMoveHandler;
  backend_api_.poll_js_calls = Backend_PollJsCalls;
  backend_api_.set_js_call_notify = Backend_SetJsCallNotify;
  backend_api_.set_application_menu = Backend_SetApplicationMenu;
  backend_api_.show_context_menu = Backend_ShowContextMenu;
  backend_api_.open_devtools = Backend_OpenDevTools;
  backend_api_.set_js_namespace = Backend_SetJsNamespace;
  backend_api_.create_window = Backend_CreateWindow;
  backend_api_.create_window_ex = Backend_CreateWindowEx;
  backend_api_.close_window = Backend_CloseWindow;
  backend_api_.set_close_requested_handler = Backend_SetCloseRequestedHandler;
  backend_api_.show_dialog = Backend_ShowDialog;
  backend_api_.string_free = Backend_StringFree;

  backend_api_.set_dock_badge = Backend_SetDockBadge;
  backend_api_.bounce_dock = Backend_BounceDock;
  backend_api_.set_dock_menu = Backend_SetDockMenu;
  backend_api_.set_dock_visible = Backend_SetDockVisible;
  backend_api_.set_dock_reopen_handler = Backend_SetDockReopenHandler;

  backend_api_.create_tray_icon = Backend_CreateTrayIcon;
  backend_api_.destroy_tray_icon = Backend_DestroyTrayIcon;
  backend_api_.set_tray_icon = Backend_SetTrayIcon;
  backend_api_.set_tray_tooltip = Backend_SetTrayTooltip;
  backend_api_.set_tray_menu = Backend_SetTrayMenu;
  backend_api_.set_tray_click_handler = Backend_SetTrayClickHandler;
  backend_api_.set_tray_double_click_handler =
      Backend_SetTrayDoubleClickHandler;
  backend_api_.set_tray_icon_dark = Backend_SetTrayIconDark;
  backend_api_.get_tray_icon_bounds = Backend_GetTrayIconBounds;

  backend_api_.show_notification = Backend_ShowNotification;
  backend_api_.close_notification = Backend_CloseNotification;

  backend_api_.query_permission = Backend_QueryPermission;
  backend_api_.request_permission = Backend_RequestPermission;
}

RuntimeLoader::RuntimeLoader() {
  instance_ = this;
  InitializeBackendApi();
}

RuntimeLoader::~RuntimeLoader() {
  Shutdown();
  if (library_handle_) {
#ifndef _WIN32
    dlclose(library_handle_);
#else
    FreeLibrary(static_cast<HMODULE>(library_handle_));
#endif
  }
  instance_ = nullptr;
}

RuntimeLoader* RuntimeLoader::GetInstance() {
  if (!instance_) {
    instance_ = new RuntimeLoader();
  }
  return instance_;
}

bool RuntimeLoader::Load(const std::string& path) {
#ifndef _WIN32
  library_handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library_handle_) {
    std::cerr << "Failed to load runtime: " << dlerror() << std::endl;
    return false;
  }

  init_fn_ = reinterpret_cast<laufey_runtime_init_fn>(
      dlsym(library_handle_, LAUFEY_RUNTIME_INIT_SYMBOL));
  if (!init_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_INIT_SYMBOL << ": "
              << dlerror() << std::endl;
    return false;
  }

  start_fn_ = reinterpret_cast<laufey_runtime_start_fn>(
      dlsym(library_handle_, LAUFEY_RUNTIME_START_SYMBOL));
  if (!start_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_START_SYMBOL << ": "
              << dlerror() << std::endl;
    return false;
  }

  shutdown_fn_ = reinterpret_cast<laufey_runtime_shutdown_fn>(
      dlsym(library_handle_, LAUFEY_RUNTIME_SHUTDOWN_SYMBOL));
  if (!shutdown_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_SHUTDOWN_SYMBOL << ": "
              << dlerror() << std::endl;
    return false;
  }
#else
  library_handle_ = LoadLibraryA(path.c_str());
  if (!library_handle_) {
    std::cerr << "Failed to load runtime: " << GetLastError() << std::endl;
    return false;
  }

  init_fn_ = reinterpret_cast<laufey_runtime_init_fn>(GetProcAddress(
      static_cast<HMODULE>(library_handle_), LAUFEY_RUNTIME_INIT_SYMBOL));
  if (!init_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_INIT_SYMBOL << std::endl;
    return false;
  }

  start_fn_ = reinterpret_cast<laufey_runtime_start_fn>(GetProcAddress(
      static_cast<HMODULE>(library_handle_), LAUFEY_RUNTIME_START_SYMBOL));
  if (!start_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_START_SYMBOL << std::endl;
    return false;
  }

  shutdown_fn_ = reinterpret_cast<laufey_runtime_shutdown_fn>(GetProcAddress(
      static_cast<HMODULE>(library_handle_), LAUFEY_RUNTIME_SHUTDOWN_SYMBOL));
  if (!shutdown_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_SHUTDOWN_SYMBOL << std::endl;
    return false;
  }
#endif

  std::cout << "Runtime loaded successfully from: " << path << std::endl;
  return true;
}

bool RuntimeLoader::Start() {
  if (running_) {
    return true;
  }

  if (!init_fn_ || !start_fn_) {
    std::cerr << "Runtime not loaded" << std::endl;
    return false;
  }

  int result = init_fn_(&backend_api_);
  if (result != 0) {
    std::cerr << "Runtime init failed with code: " << result << std::endl;
    return false;
  }

  running_ = true;
  runtime_thread_ = std::thread(&RuntimeLoader::RuntimeThread, this);

  std::cout << "Runtime started" << std::endl;
  return true;
}

void RuntimeLoader::RuntimeThread() {
  int result = start_fn_();
  if (result != 0) {
    std::cerr << "Runtime start returned error: " << result << std::endl;
  }
  running_ = false;
}

void RuntimeLoader::Shutdown() {
  if (shutdown_fn_) {
    shutdown_fn_();
  }

  if (runtime_thread_.joinable()) {
    runtime_thread_.join();
  }
}

void RuntimeLoader::OnJsCall(uint32_t window_id, uint64_t call_id,
                             const std::string& method_path,
                             laufey::ValuePtr args) {
  StoreCallWindow(call_id, window_id);
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_js_calls_.push({window_id, call_id, method_path, args});
  }

  std::lock_guard<std::mutex> lock(notify_mutex_);
  if (js_call_notify_fn_) {
    js_call_notify_fn_(js_call_notify_data_);
  }
}

void RuntimeLoader::PollPendingJsCalls() {
  std::vector<PendingJsCall> calls;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    while (!pending_js_calls_.empty()) {
      calls.push_back(std::move(pending_js_calls_.front()));
      pending_js_calls_.pop();
    }
  }

  if (calls.empty())
    return;

  laufey_js_call_fn handler;
  void* user_data;
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handler = js_call_handler_;
    user_data = js_call_user_data_;
  }

  for (auto& call : calls) {
    if (handler) {
      laufey_value_t* argsWrapper = new laufey_value(call.args);
      handler(user_data, call.window_id, call.call_id, call.method_path.c_str(),
              argsWrapper);
    } else {
      JsCallRespond(call.window_id, call.call_id, nullptr,
                    laufey::Value::String("No JS call handler registered"));
    }
  }
}

void RuntimeLoader::JsCallRespond(uint32_t window_id, uint64_t call_id,
                                  laufey::ValuePtr result, laufey::ValuePtr error) {
  LaufeyBackend* backend = GetBackend();
  if (backend) {
    backend->RespondToJsCall(window_id, call_id, result, error);
  }
}
