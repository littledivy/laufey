// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "runtime_loader.h"
#include "wef_json.h"

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <webkit2/webkit2.h>
#include <JavaScriptCore/JavaScript.h>

#include <iostream>
#include <map>
#include <mutex>
#include <condition_variable>

// Helper to run a callback synchronously on the GTK main thread.
// If already on the main thread, runs immediately.
template <typename F>
static void gtk_invoke_sync(F&& fn) {
  if (g_main_context_is_owner(g_main_context_default())) {
    fn();
    return;
  }
  std::mutex mtx;
  std::condition_variable cv;
  bool done = false;
  struct Ctx {
    F* fn;
    std::mutex* mtx;
    std::condition_variable* cv;
    bool* done;
  };
  Ctx ctx{&fn, &mtx, &cv, &done};
  g_idle_add(
      [](gpointer data) -> gboolean {
        auto* c = static_cast<Ctx*>(data);
        (*c->fn)();
        {
          std::lock_guard<std::mutex> lock(*c->mtx);
          *c->done = true;
        }
        c->cv->notify_one();
        return G_SOURCE_REMOVE;
      },
      &ctx);
  std::unique_lock<std::mutex> lock(mtx);
  cv.wait(lock, [&done] { return done; });
}

namespace keyboard {

std::string GdkKeyvalToKey(guint keyval) {
  switch (keyval) {
    case GDK_KEY_BackSpace:
      return "Backspace";
    case GDK_KEY_Tab:
    case GDK_KEY_ISO_Left_Tab:
      return "Tab";
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      return "Enter";
    case GDK_KEY_Escape:
      return "Escape";
    case GDK_KEY_space:
      return " ";
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete:
      return "Delete";
    case GDK_KEY_Insert:
    case GDK_KEY_KP_Insert:
      return "Insert";
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home:
      return "Home";
    case GDK_KEY_End:
    case GDK_KEY_KP_End:
      return "End";
    case GDK_KEY_Page_Up:
    case GDK_KEY_KP_Page_Up:
      return "PageUp";
    case GDK_KEY_Page_Down:
    case GDK_KEY_KP_Page_Down:
      return "PageDown";
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
      return "ArrowLeft";
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
      return "ArrowRight";
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
      return "ArrowUp";
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
      return "ArrowDown";
    case GDK_KEY_Shift_L:
    case GDK_KEY_Shift_R:
      return "Shift";
    case GDK_KEY_Control_L:
    case GDK_KEY_Control_R:
      return "Control";
    case GDK_KEY_Alt_L:
    case GDK_KEY_Alt_R:
      return "Alt";
    case GDK_KEY_Meta_L:
    case GDK_KEY_Meta_R:
    case GDK_KEY_Super_L:
    case GDK_KEY_Super_R:
      return "Meta";
    case GDK_KEY_Caps_Lock:
      return "CapsLock";
    case GDK_KEY_Num_Lock:
      return "NumLock";
    case GDK_KEY_Scroll_Lock:
      return "ScrollLock";
    case GDK_KEY_F1:
      return "F1";
    case GDK_KEY_F2:
      return "F2";
    case GDK_KEY_F3:
      return "F3";
    case GDK_KEY_F4:
      return "F4";
    case GDK_KEY_F5:
      return "F5";
    case GDK_KEY_F6:
      return "F6";
    case GDK_KEY_F7:
      return "F7";
    case GDK_KEY_F8:
      return "F8";
    case GDK_KEY_F9:
      return "F9";
    case GDK_KEY_F10:
      return "F10";
    case GDK_KEY_F11:
      return "F11";
    case GDK_KEY_F12:
      return "F12";
    case GDK_KEY_Pause:
      return "Pause";
    default: {
      guint32 uc = gdk_keyval_to_unicode(keyval);
      if (uc > 0 && g_unichar_isprint(uc)) {
        char buf[7];
        int len = g_unichar_to_utf8(uc, buf);
        buf[len] = '\0';
        return std::string(buf);
      }
      return "Unidentified";
    }
  }
}

std::string GdkKeycodeToCode(guint16 hardware_keycode) {
  switch (hardware_keycode) {
    case 9:
      return "Escape";
    case 10:
      return "Digit1";
    case 11:
      return "Digit2";
    case 12:
      return "Digit3";
    case 13:
      return "Digit4";
    case 14:
      return "Digit5";
    case 15:
      return "Digit6";
    case 16:
      return "Digit7";
    case 17:
      return "Digit8";
    case 18:
      return "Digit9";
    case 19:
      return "Digit0";
    case 20:
      return "Minus";
    case 21:
      return "Equal";
    case 22:
      return "Backspace";
    case 23:
      return "Tab";
    case 24:
      return "KeyQ";
    case 25:
      return "KeyW";
    case 26:
      return "KeyE";
    case 27:
      return "KeyR";
    case 28:
      return "KeyT";
    case 29:
      return "KeyY";
    case 30:
      return "KeyU";
    case 31:
      return "KeyI";
    case 32:
      return "KeyO";
    case 33:
      return "KeyP";
    case 34:
      return "BracketLeft";
    case 35:
      return "BracketRight";
    case 36:
      return "Enter";
    case 37:
      return "ControlLeft";
    case 38:
      return "KeyA";
    case 39:
      return "KeyS";
    case 40:
      return "KeyD";
    case 41:
      return "KeyF";
    case 42:
      return "KeyG";
    case 43:
      return "KeyH";
    case 44:
      return "KeyJ";
    case 45:
      return "KeyK";
    case 46:
      return "KeyL";
    case 47:
      return "Semicolon";
    case 48:
      return "Quote";
    case 49:
      return "Backquote";
    case 50:
      return "ShiftLeft";
    case 51:
      return "Backslash";
    case 52:
      return "KeyZ";
    case 53:
      return "KeyX";
    case 54:
      return "KeyC";
    case 55:
      return "KeyV";
    case 56:
      return "KeyB";
    case 57:
      return "KeyN";
    case 58:
      return "KeyM";
    case 59:
      return "Comma";
    case 60:
      return "Period";
    case 61:
      return "Slash";
    case 62:
      return "ShiftRight";
    case 64:
      return "AltLeft";
    case 65:
      return "Space";
    case 66:
      return "CapsLock";
    case 67:
      return "F1";
    case 68:
      return "F2";
    case 69:
      return "F3";
    case 70:
      return "F4";
    case 71:
      return "F5";
    case 72:
      return "F6";
    case 73:
      return "F7";
    case 74:
      return "F8";
    case 75:
      return "F9";
    case 76:
      return "F10";
    case 95:
      return "F11";
    case 96:
      return "F12";
    case 105:
      return "ControlRight";
    case 108:
      return "AltRight";
    case 110:
      return "Home";
    case 111:
      return "ArrowUp";
    case 112:
      return "PageUp";
    case 113:
      return "ArrowLeft";
    case 114:
      return "ArrowRight";
    case 115:
      return "End";
    case 116:
      return "ArrowDown";
    case 117:
      return "PageDown";
    case 118:
      return "Insert";
    case 119:
      return "Delete";
    case 133:
      return "MetaLeft";
    case 134:
      return "MetaRight";
    default:
      return "Unidentified";
  }
}

uint32_t GdkModifiersToWef(guint state) {
  uint32_t modifiers = 0;
  if (state & GDK_SHIFT_MASK)
    modifiers |= WEF_MOD_SHIFT;
  if (state & GDK_CONTROL_MASK)
    modifiers |= WEF_MOD_CONTROL;
  if (state & GDK_MOD1_MASK)
    modifiers |= WEF_MOD_ALT;
  if (state & GDK_MOD4_MASK)
    modifiers |= WEF_MOD_META;
  return modifiers;
}

}  // namespace keyboard

// GtkWidget → wef_id mapping for event routing
static std::map<GtkWidget*, uint32_t> g_widget_to_wef_id;
static std::mutex g_widget_mutex;

static uint32_t WefIdForWidget(GtkWidget* widget) {
  if (!widget)
    return 0;
  // Walk up to find the toplevel window
  GtkWidget* toplevel = gtk_widget_get_toplevel(widget);
  std::lock_guard<std::mutex> lock(g_widget_mutex);
  auto it = g_widget_to_wef_id.find(toplevel);
  return it != g_widget_to_wef_id.end() ? it->second : 0;
}

static void RegisterWidget(GtkWidget* widget, uint32_t window_id) {
  std::lock_guard<std::mutex> lock(g_widget_mutex);
  g_widget_to_wef_id[widget] = window_id;
}

static void UnregisterWidget(GtkWidget* widget) {
  std::lock_guard<std::mutex> lock(g_widget_mutex);
  g_widget_to_wef_id.erase(widget);
}

// Per-window state
struct LinuxWindowState {
  uint32_t window_id;
  GtkWidget* window;
  GtkWidget* vbox;      // container for menu bar + webview
  GtkWidget* menu_bar;  // per-window menu bar (nullptr = none)
  WebKitWebView* webview;
  WebKitUserContentManager* content_manager;
};

// Track the click_count from press events for use in the corresponding release.
static int32_t g_last_click_count = 1;

static gboolean on_button_event(GtkWidget* widget, GdkEventButton* event,
                                gpointer user_data) {
  uint32_t wid = WefIdForWidget(widget);
  if (wid == 0)
    return FALSE;

  int state;
  int32_t click_count;

  switch (event->type) {
    case GDK_BUTTON_PRESS:
      state = WEF_MOUSE_PRESSED;
      click_count = 1;
      break;
    case GDK_2BUTTON_PRESS:
      state = WEF_MOUSE_PRESSED;
      click_count = 2;
      break;
    case GDK_3BUTTON_PRESS:
      state = WEF_MOUSE_PRESSED;
      click_count = 2;
      break;
    case GDK_BUTTON_RELEASE:
      state = WEF_MOUSE_RELEASED;
      click_count = g_last_click_count;
      break;
    default:
      return FALSE;
  }

  if (state == WEF_MOUSE_PRESSED) {
    g_last_click_count = click_count;
  }

  int button;
  switch (event->button) {
    case 1:
      button = WEF_MOUSE_BUTTON_LEFT;
      break;
    case 2:
      button = WEF_MOUSE_BUTTON_MIDDLE;
      break;
    case 3:
      button = WEF_MOUSE_BUTTON_RIGHT;
      break;
    case 8:
      button = WEF_MOUSE_BUTTON_BACK;
      break;
    case 9:
      button = WEF_MOUSE_BUTTON_FORWARD;
      break;
    default:
      button = static_cast<int>(event->button);
      break;
  }
  uint32_t modifiers = keyboard::GdkModifiersToWef(event->state);

  RuntimeLoader::GetInstance()->DispatchMouseClickEvent(
      wid, state, button, event->x, event->y, modifiers, click_count);

  return FALSE;
}

static gboolean on_motion_event(GtkWidget* widget, GdkEventMotion* event,
                                gpointer user_data) {
  uint32_t wid = WefIdForWidget(widget);
  if (wid == 0)
    return FALSE;
  uint32_t modifiers = keyboard::GdkModifiersToWef(event->state);
  RuntimeLoader::GetInstance()->DispatchMouseMoveEvent(wid, event->x, event->y,
                                                       modifiers);
  return FALSE;
}

static gboolean on_scroll_event(GtkWidget* widget, GdkEventScroll* event,
                                gpointer user_data) {
  uint32_t wid = WefIdForWidget(widget);
  if (wid == 0)
    return FALSE;

  double delta_x = 0, delta_y = 0;
  int32_t delta_mode = WEF_WHEEL_DELTA_LINE;

  switch (event->direction) {
    case GDK_SCROLL_UP:
      delta_y = -1.0;
      break;
    case GDK_SCROLL_DOWN:
      delta_y = 1.0;
      break;
    case GDK_SCROLL_LEFT:
      delta_x = -1.0;
      break;
    case GDK_SCROLL_RIGHT:
      delta_x = 1.0;
      break;
    case GDK_SCROLL_SMOOTH:
      gdk_event_get_scroll_deltas((GdkEvent*)event, &delta_x, &delta_y);
      delta_mode = WEF_WHEEL_DELTA_PIXEL;
      break;
  }

  uint32_t modifiers = keyboard::GdkModifiersToWef(event->state);
  RuntimeLoader::GetInstance()->DispatchWheelEvent(
      wid, delta_x, delta_y, event->x, event->y, modifiers, delta_mode);
  return FALSE;
}

static gboolean on_enter_notify_event(GtkWidget* widget,
                                      GdkEventCrossing* event,
                                      gpointer user_data) {
  uint32_t wid = WefIdForWidget(widget);
  if (wid == 0)
    return FALSE;
  uint32_t modifiers = keyboard::GdkModifiersToWef(event->state);
  RuntimeLoader::GetInstance()->DispatchCursorEnterLeaveEvent(
      wid, 1, event->x, event->y, modifiers);
  return FALSE;
}

static gboolean on_leave_notify_event(GtkWidget* widget,
                                      GdkEventCrossing* event,
                                      gpointer user_data) {
  uint32_t wid = WefIdForWidget(widget);
  if (wid == 0)
    return FALSE;
  uint32_t modifiers = keyboard::GdkModifiersToWef(event->state);
  RuntimeLoader::GetInstance()->DispatchCursorEnterLeaveEvent(
      wid, 0, event->x, event->y, modifiers);
  return FALSE;
}

static gboolean on_focus_in_event(GtkWidget* widget, GdkEventFocus* event,
                                  gpointer user_data) {
  uint32_t wid = WefIdForWidget(widget);
  if (wid == 0)
    return FALSE;
  RuntimeLoader::GetInstance()->DispatchFocusedEvent(wid, 1);
  return FALSE;
}

static gboolean on_focus_out_event(GtkWidget* widget, GdkEventFocus* event,
                                   gpointer user_data) {
  uint32_t wid = WefIdForWidget(widget);
  if (wid == 0)
    return FALSE;
  RuntimeLoader::GetInstance()->DispatchFocusedEvent(wid, 0);
  return FALSE;
}

static gboolean on_configure_event(GtkWidget* widget, GdkEventConfigure* event,
                                   gpointer user_data) {
  uint32_t wid = WefIdForWidget(widget);
  if (wid == 0)
    return FALSE;
  RuntimeLoader::GetInstance()->DispatchResizeEvent(wid, event->width,
                                                    event->height);
  RuntimeLoader::GetInstance()->DispatchMoveEvent(wid, event->x, event->y);
  return FALSE;
}

static gboolean on_key_event(GtkWidget* widget, GdkEventKey* event,
                             gpointer user_data) {
  uint32_t wid = WefIdForWidget(widget);
  if (wid == 0)
    return FALSE;

  int state =
      (event->type == GDK_KEY_PRESS) ? WEF_KEY_PRESSED : WEF_KEY_RELEASED;
  std::string key = keyboard::GdkKeyvalToKey(event->keyval);
  std::string code = keyboard::GdkKeycodeToCode(event->hardware_keycode);
  uint32_t modifiers = keyboard::GdkModifiersToWef(event->state);

  RuntimeLoader::GetInstance()->DispatchKeyboardEvent(
      wid, state, key.c_str(), code.c_str(), modifiers, false);

  return FALSE;
}

// ============================================================================
// WebKitGTK Backend
// ============================================================================

class WebKitGTKBackend : public WefBackend {
 public:
  WebKitGTKBackend();
  ~WebKitGTKBackend() override;

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

  void BounceDock(int type) override;
  void SetDockBadge(const char* badge_or_null) override;

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

  uint32_t ShowNotification(wef_value_t* options,
                            const wef_backend_api_t* api,
                            wef_notification_event_fn on_event,
                            void* user_data) override;
  void CloseNotification(uint32_t notification_id) override;

  // libnotify / notify-send have no permission model — always granted.
  void QueryPermission(int kind, wef_permission_callback_fn cb,
                       void* user_data) override {
    if (cb)
      cb(user_data, kind == WEF_PERMISSION_NOTIFICATIONS
                        ? WEF_PERMISSION_STATUS_GRANTED
                        : WEF_PERMISSION_STATUS_UNSUPPORTED);
  }
  void RequestPermission(int kind, wef_permission_callback_fn cb,
                         void* user_data) override {
    QueryPermission(kind, cb, user_data);
  }

  void HandleJsMessage(uint32_t window_id, const char* json);

 private:
  LinuxWindowState* GetWindow(uint32_t window_id);

  std::map<uint32_t, LinuxWindowState> windows_;
  std::mutex windows_mutex_;
};

// Static instance pointer for GTK callbacks
static WebKitGTKBackend* g_gtk_backend = nullptr;

// GtkWidget → window_id mapping for script message routing
static std::map<WebKitUserContentManager*, uint32_t>
    g_content_manager_to_wef_id;

static std::string BuildInitScript(const std::string& ns,
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

static void on_script_message(WebKitUserContentManager* manager,
                              WebKitJavascriptResult* js_result,
                              gpointer user_data) {
  auto it = g_content_manager_to_wef_id.find(manager);
  uint32_t wid = (it != g_content_manager_to_wef_id.end()) ? it->second : 0;

  JSCValue* value = webkit_javascript_result_get_js_value(js_result);
  if (jsc_value_is_string(value)) {
    gchar* str = jsc_value_to_string(value);
    if (g_gtk_backend) {
      g_gtk_backend->HandleJsMessage(wid, str);
    }
    g_free(str);
  }
}

static void on_window_destroy(GtkWidget* widget, gpointer user_data) {
  uint32_t wid = WefIdForWidget(widget);
  if (wid > 0) {
    RuntimeLoader::GetInstance()->DispatchCloseRequestedEvent(wid);
    UnregisterWidget(widget);
  }
  // If no more windows, quit
  {
    std::lock_guard<std::mutex> lock(g_widget_mutex);
    if (g_widget_to_wef_id.empty()) {
      gtk_main_quit();
    }
  }
}

WebKitGTKBackend::WebKitGTKBackend() {
  g_gtk_backend = this;
}

WebKitGTKBackend::~WebKitGTKBackend() {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  for (auto& [wid, state] : windows_) {
    webkit_user_content_manager_unregister_script_message_handler(
        state.content_manager, "wef");
    g_content_manager_to_wef_id.erase(state.content_manager);
    UnregisterWidget(state.window);
  }
  windows_.clear();
  g_gtk_backend = nullptr;
}

LinuxWindowState* WebKitGTKBackend::GetWindow(uint32_t window_id) {
  auto it = windows_.find(window_id);
  return it != windows_.end() ? &it->second : nullptr;
}

static gboolean on_script_dialog(WebKitWebView* webview,
                                 WebKitScriptDialog* dialog,
                                 gpointer user_data) {
  WebKitScriptDialogType type = webkit_script_dialog_get_dialog_type(dialog);
  const gchar* message = webkit_script_dialog_get_message(dialog);

  GtkWidget* toplevel = gtk_widget_get_toplevel(GTK_WIDGET(webview));
  GtkWindow* parent = GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : nullptr;

  if (type == WEBKIT_SCRIPT_DIALOG_ALERT) {
    GtkWidget* dlg =
        gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                               GTK_BUTTONS_OK, "%s", message);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    webkit_script_dialog_confirm_set_confirmed(dialog, TRUE);
    return TRUE;
  }

  if (type == WEBKIT_SCRIPT_DIALOG_CONFIRM) {
    GtkWidget* dlg =
        gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                               GTK_BUTTONS_OK_CANCEL, "%s", message);
    gint result = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    webkit_script_dialog_confirm_set_confirmed(dialog,
                                               result == GTK_RESPONSE_OK);
    return TRUE;
  }

  if (type == WEBKIT_SCRIPT_DIALOG_PROMPT) {
    GtkWidget* dlg =
        gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                               GTK_BUTTONS_OK_CANCEL, "%s", message);
    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget* entry = gtk_entry_new();
    const gchar* default_text =
        webkit_script_dialog_prompt_get_default_text(dialog);
    if (default_text) {
      gtk_entry_set_text(GTK_ENTRY(entry), default_text);
    }
    gtk_container_add(GTK_CONTAINER(content), entry);
    gtk_widget_show(entry);
    gint result = gtk_dialog_run(GTK_DIALOG(dlg));
    if (result == GTK_RESPONSE_OK) {
      webkit_script_dialog_prompt_set_text(
          dialog, gtk_entry_get_text(GTK_ENTRY(entry)));
    }
    webkit_script_dialog_confirm_set_confirmed(dialog,
                                               result == GTK_RESPONSE_OK);
    gtk_widget_destroy(dlg);
    return TRUE;
  }

  return FALSE;
}

void WebKitGTKBackend::CreateWindow(uint32_t window_id, int width, int height) {
  GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size(GTK_WINDOW(window), width, height);
  g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), nullptr);
  g_signal_connect(window, "key-press-event", G_CALLBACK(on_key_event),
                   nullptr);
  g_signal_connect(window, "key-release-event", G_CALLBACK(on_key_event),
                   nullptr);
  g_signal_connect(window, "button-press-event", G_CALLBACK(on_button_event),
                   nullptr);
  g_signal_connect(window, "button-release-event", G_CALLBACK(on_button_event),
                   nullptr);
  gtk_widget_add_events(window, GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK |
                                    GDK_SMOOTH_SCROLL_MASK |
                                    GDK_ENTER_NOTIFY_MASK |
                                    GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(window, "motion-notify-event", G_CALLBACK(on_motion_event),
                   nullptr);
  g_signal_connect(window, "scroll-event", G_CALLBACK(on_scroll_event),
                   nullptr);
  g_signal_connect(window, "enter-notify-event",
                   G_CALLBACK(on_enter_notify_event), nullptr);
  g_signal_connect(window, "leave-notify-event",
                   G_CALLBACK(on_leave_notify_event), nullptr);
  g_signal_connect(window, "focus-in-event", G_CALLBACK(on_focus_in_event),
                   nullptr);
  g_signal_connect(window, "focus-out-event", G_CALLBACK(on_focus_out_event),
                   nullptr);
  g_signal_connect(window, "configure-event", G_CALLBACK(on_configure_event),
                   nullptr);

  RegisterWidget(window, window_id);

  WebKitUserContentManager* content_manager = webkit_user_content_manager_new();
  g_signal_connect(content_manager, "script-message-received::wef",
                   G_CALLBACK(on_script_message), nullptr);
  webkit_user_content_manager_register_script_message_handler(content_manager,
                                                              "wef");
  g_content_manager_to_wef_id[content_manager] = window_id;

  WebKitWebView* webview = WEBKIT_WEB_VIEW(
      webkit_web_view_new_with_user_content_manager(content_manager));

  g_signal_connect(webview, "script-dialog", G_CALLBACK(on_script_dialog),
                   nullptr);

  WebKitSettings* wk_settings = webkit_web_view_get_settings(webview);
  webkit_settings_set_enable_developer_extras(wk_settings, TRUE);

  std::string initScript = BuildInitScript(
      RuntimeLoader::GetInstance()->GetJsNamespace(),
      "window.webkit.messageHandlers.wef.postMessage(JSON.stringify({\n"
      "            callId: callId,\n"
      "            method: path.join('.'),\n"
      "            args: processedArgs\n"
      "          }));");
  WebKitUserScript* script = webkit_user_script_new(
      initScript.c_str(), WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
  webkit_user_content_manager_add_script(content_manager, script);
  webkit_user_script_unref(script);

  GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(window), vbox);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(webview), TRUE, TRUE, 0);

  LinuxWindowState state;
  state.window_id = window_id;
  state.window = window;
  state.vbox = vbox;
  state.menu_bar = nullptr;
  state.webview = webview;
  state.content_manager = content_manager;

  {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    windows_[window_id] = state;
  }

  gtk_widget_show_all(window);
}

void WebKitGTKBackend::CloseWindow(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    webkit_user_content_manager_unregister_script_message_handler(
        state->content_manager, "wef");
    g_content_manager_to_wef_id.erase(state->content_manager);
    UnregisterWidget(state->window);
    gtk_widget_destroy(state->window);
    windows_.erase(window_id);
  }
}

void WebKitGTKBackend::Navigate(uint32_t window_id, const std::string& url) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    webkit_web_view_load_uri(state->webview, url.c_str());
  }
}

void WebKitGTKBackend::SetTitle(uint32_t window_id, const std::string& title) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    gtk_window_set_title(GTK_WINDOW(state->window), title.c_str());
  }
}

struct ExecuteJsCallbackData {
  wef_js_result_fn callback;
  void* user_data;
};

static void on_execute_js_finished(GObject* source, GAsyncResult* result,
                                   gpointer user_data) {
  auto* cb_data = static_cast<ExecuteJsCallbackData*>(user_data);

  GError* error = nullptr;
  WebKitJavascriptResult* js_result = webkit_web_view_run_javascript_finish(
      WEBKIT_WEB_VIEW(source), result, &error);

  if (error) {
    auto errVal = wef::Value::String(error->message);
    wef_value errWef(errVal);
    cb_data->callback(nullptr, &errWef, cb_data->user_data);
    g_error_free(error);
  } else if (js_result) {
    JSCValue* value = webkit_javascript_result_get_js_value(js_result);
    if (jsc_value_is_null(value) || jsc_value_is_undefined(value)) {
      cb_data->callback(nullptr, nullptr, cb_data->user_data);
    } else if (jsc_value_is_boolean(value)) {
      auto val = wef::Value::Bool(jsc_value_to_boolean(value));
      wef_value wef(val);
      cb_data->callback(&wef, nullptr, cb_data->user_data);
    } else if (jsc_value_is_number(value)) {
      double d = jsc_value_to_double(value);
      if (d == (int)d && d >= INT_MIN && d <= INT_MAX) {
        auto val = wef::Value::Int((int)d);
        wef_value wef(val);
        cb_data->callback(&wef, nullptr, cb_data->user_data);
      } else {
        auto val = wef::Value::Double(d);
        wef_value wef(val);
        cb_data->callback(&wef, nullptr, cb_data->user_data);
      }
    } else if (jsc_value_is_string(value)) {
      gchar* str = jsc_value_to_string(value);
      auto val = wef::Value::String(str);
      wef_value wef(val);
      cb_data->callback(&wef, nullptr, cb_data->user_data);
      g_free(str);
    } else {
      // For objects/arrays, serialize to JSON and parse
      gchar* json = jsc_value_to_json(value, 0);
      if (json) {
        auto val = json::ParseJson(json);
        wef_value wef(val);
        cb_data->callback(&wef, nullptr, cb_data->user_data);
        g_free(json);
      } else {
        cb_data->callback(nullptr, nullptr, cb_data->user_data);
      }
    }
    webkit_javascript_result_unref(js_result);
  } else {
    cb_data->callback(nullptr, nullptr, cb_data->user_data);
  }

  delete cb_data;
}

void WebKitGTKBackend::ExecuteJs(uint32_t window_id, const std::string& script,
                                 wef_js_result_fn callback,
                                 void* callback_data) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (!state) {
    if (callback)
      callback(nullptr, nullptr, callback_data);
    return;
  }
  if (!callback) {
    webkit_web_view_run_javascript(state->webview, script.c_str(), nullptr,
                                   nullptr, nullptr);
  } else {
    auto* cb_data = new ExecuteJsCallbackData{callback, callback_data};
    webkit_web_view_run_javascript(state->webview, script.c_str(), nullptr,
                                   on_execute_js_finished, cb_data);
  }
}

void WebKitGTKBackend::Quit() {
  g_idle_add(
      [](gpointer) -> gboolean {
        gtk_main_quit();
        return G_SOURCE_REMOVE;
      },
      nullptr);
}

void WebKitGTKBackend::SetWindowSize(uint32_t window_id, int width,
                                     int height) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    gtk_window_resize(GTK_WINDOW(state->window), width, height);
  }
}

void WebKitGTKBackend::GetWindowSize(uint32_t window_id, int* width,
                                     int* height) {
  int w = 0, h = 0;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      gtk_window_get_size(GTK_WINDOW(state->window), &w, &h);
    }
  });
  if (width)
    *width = w;
  if (height)
    *height = h;
}

void WebKitGTKBackend::SetWindowPosition(uint32_t window_id, int x, int y) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    gtk_window_move(GTK_WINDOW(state->window), x, y);
  }
}

void WebKitGTKBackend::GetWindowPosition(uint32_t window_id, int* x, int* y) {
  int wx = 0, wy = 0;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      gtk_window_get_position(GTK_WINDOW(state->window), &wx, &wy);
    }
  });
  if (x)
    *x = wx;
  if (y)
    *y = wy;
}

void WebKitGTKBackend::SetResizable(uint32_t window_id, bool resizable) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    gtk_window_set_resizable(GTK_WINDOW(state->window), resizable);
  }
}

bool WebKitGTKBackend::IsResizable(uint32_t window_id) {
  bool result = false;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      result = gtk_window_get_resizable(GTK_WINDOW(state->window)) != FALSE;
    }
  });
  return result;
}

void WebKitGTKBackend::SetAlwaysOnTop(uint32_t window_id, bool always_on_top) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    gtk_window_set_keep_above(GTK_WINDOW(state->window), always_on_top);
  }
}

bool WebKitGTKBackend::IsAlwaysOnTop(uint32_t window_id) {
  bool result = false;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      GdkWindow* gdk_window = gtk_widget_get_window(state->window);
      if (gdk_window) {
        GdkWindowState wstate = gdk_window_get_state(gdk_window);
        result = (wstate & GDK_WINDOW_STATE_ABOVE) != 0;
      }
    }
  });
  return result;
}

bool WebKitGTKBackend::IsVisible(uint32_t window_id) {
  bool result = false;
  gtk_invoke_sync([&] {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    auto* state = GetWindow(window_id);
    if (state) {
      result = gtk_widget_get_visible(state->window) != FALSE;
    }
  });
  return result;
}

void WebKitGTKBackend::Show(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    gtk_widget_show_all(state->window);
  }
}

void WebKitGTKBackend::Hide(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    gtk_widget_hide(state->window);
  }
}

void WebKitGTKBackend::Focus(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    gtk_widget_show(state->window);
    gtk_window_present(GTK_WINDOW(state->window));
  }
}

void WebKitGTKBackend::PostUiTask(void (*task)(void*), void* data) {
  struct TaskData {
    void (*task)(void*);
    void* data;
  };
  auto* td = new TaskData{task, data};
  g_idle_add(
      [](gpointer data) -> gboolean {
        auto* td = static_cast<TaskData*>(data);
        td->task(td->data);
        delete td;
        return G_SOURCE_REMOVE;
      },
      td);
}

void WebKitGTKBackend::InvokeJsCallback(uint32_t window_id,
                                        uint64_t callback_id,
                                        wef::ValuePtr args) {
  std::string argsJson = json::Serialize(args);
  std::string script = "window.__wefInvokeCallback(" +
                       std::to_string(callback_id) + ", " + argsJson + ");";
  std::lock_guard<std::mutex> lock(windows_mutex_);
  if (window_id == 0) {
    for (auto& [wid, state] : windows_) {
      webkit_web_view_run_javascript(state.webview, script.c_str(), nullptr,
                                     nullptr, nullptr);
    }
  } else {
    auto* state = GetWindow(window_id);
    if (state) {
      webkit_web_view_run_javascript(state->webview, script.c_str(), nullptr,
                                     nullptr, nullptr);
    }
  }
}

void WebKitGTKBackend::ReleaseJsCallback(uint32_t window_id,
                                         uint64_t callback_id) {
  std::string script =
      "window.__wefReleaseCallback(" + std::to_string(callback_id) + ");";
  std::lock_guard<std::mutex> lock(windows_mutex_);
  if (window_id == 0) {
    for (auto& [wid, state] : windows_) {
      webkit_web_view_run_javascript(state.webview, script.c_str(), nullptr,
                                     nullptr, nullptr);
    }
  } else {
    auto* state = GetWindow(window_id);
    if (state) {
      webkit_web_view_run_javascript(state->webview, script.c_str(), nullptr,
                                     nullptr, nullptr);
    }
  }
}

void WebKitGTKBackend::RespondToJsCall(uint32_t window_id, uint64_t call_id,
                                       wef::ValuePtr result,
                                       wef::ValuePtr error) {
  std::string resultJson = json::Serialize(result);
  std::string errorJson =
      (error && !error->IsNull()) ? json::Serialize(error) : "null";
  std::string script;
  if (errorJson == "null") {
    script = "window.__wefRespond(" + std::to_string(call_id) + ", " +
             resultJson + ", null);";
  } else {
    script = "window.__wefRespond(" + std::to_string(call_id) + ", null, " +
             errorJson + ");";
  }
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    webkit_web_view_run_javascript(state->webview, script.c_str(), nullptr,
                                   nullptr, nullptr);
  }
}

void WebKitGTKBackend::Run() {
  gtk_main();
}

void WebKitGTKBackend::HandleJsMessage(uint32_t window_id,
                                       const char* jsonStr) {
  wef::ValuePtr msg = json::ParseJson(jsonStr);
  if (!msg || !msg->IsDict())
    return;

  const auto& dict = msg->GetDict();

  auto callIdIt = dict.find("callId");
  auto methodIt = dict.find("method");
  auto argsIt = dict.find("args");

  if (callIdIt == dict.end() || methodIt == dict.end())
    return;

  uint64_t call_id = 0;
  if (callIdIt->second->IsInt()) {
    call_id = static_cast<uint64_t>(callIdIt->second->GetInt());
  } else if (callIdIt->second->IsDouble()) {
    call_id = static_cast<uint64_t>(callIdIt->second->GetDouble());
  }

  std::string method =
      methodIt->second->IsString() ? methodIt->second->GetString() : "";
  wef::ValuePtr args =
      (argsIt != dict.end()) ? argsIt->second : wef::Value::List();

  RuntimeLoader::GetInstance()->OnJsCall(window_id, call_id, method, args);
}

// ============================================================================
// Application Menu
// ============================================================================

struct GtkMenuCallbackData {
  wef_menu_click_fn on_click;
  void* on_click_data;
  uint32_t window_id;
  std::string item_id;
};

static void on_gtk_menu_item_activate(GtkMenuItem* /*item*/,
                                      gpointer user_data) {
  auto* data = static_cast<GtkMenuCallbackData*>(user_data);
  if (data->on_click) {
    data->on_click(data->on_click_data, data->window_id, data->item_id.c_str());
  }
}

static void on_gtk_menu_item_destroy(gpointer user_data,
                                     GClosure* /*closure*/) {
  delete static_cast<GtkMenuCallbackData*>(user_data);
}

static GtkWidget* BuildGtkMenuFromValue(wef_value_t* val,
                                        const wef_backend_api_t* api,
                                        uint32_t window_id,
                                        wef_menu_click_fn on_click,
                                        void* on_click_data, bool is_menu_bar) {
  if (!val || !api->value_is_list(val))
    return nullptr;

  GtkWidget* menu = is_menu_bar ? gtk_menu_bar_new() : gtk_menu_new();
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
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());
        api->value_free_string(typeStr);
        continue;
      }
      if (typeStr)
        api->value_free_string(typeStr);
    }

    // Role-based items (map to labels; GTK doesn't have built-in roles)
    wef_value_t* roleVal = api->value_dict_get(itemVal, "role");
    if (roleVal && api->value_is_string(roleVal)) {
      size_t len = 0;
      char* roleStr = api->value_get_string(roleVal, &len);
      if (roleStr) {
        std::string role = roleStr;
        api->value_free_string(roleStr);

        // Map roles to labels
        std::string label;
        if (role == "quit")
          label = "Quit";
        else if (role == "copy")
          label = "Copy";
        else if (role == "paste")
          label = "Paste";
        else if (role == "cut")
          label = "Cut";
        else if (role == "selectall" || role == "selectAll")
          label = "Select All";
        else if (role == "undo")
          label = "Undo";
        else if (role == "redo")
          label = "Redo";
        else if (role == "minimize")
          label = "Minimize";
        else if (role == "close")
          label = "Close";
        else if (role == "about")
          label = "About";
        else if (role == "togglefullscreen" || role == "toggleFullScreen")
          label = "Toggle Full Screen";

        if (!label.empty()) {
          GtkWidget* item = gtk_menu_item_new_with_label(label.c_str());
          auto* cb_data =
              new GtkMenuCallbackData{on_click, on_click_data, window_id, role};
          g_signal_connect_data(item, "activate",
                                G_CALLBACK(on_gtk_menu_item_activate), cb_data,
                                on_gtk_menu_item_destroy, (GConnectFlags)0);
          gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        }
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
    std::string label = labelStr;
    api->value_free_string(labelStr);

    // Submenu
    wef_value_t* submenuVal = api->value_dict_get(itemVal, "submenu");
    if (submenuVal && api->value_is_list(submenuVal)) {
      GtkWidget* submenuItem = gtk_menu_item_new_with_label(label.c_str());
      GtkWidget* submenu = BuildGtkMenuFromValue(
          submenuVal, api, window_id, on_click, on_click_data, false);
      if (submenu) {
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(submenuItem), submenu);
      }
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), submenuItem);
      continue;
    }

    // Regular clickable item
    GtkWidget* gtkItem = gtk_menu_item_new_with_label(label.c_str());

    std::string itemId;
    wef_value_t* idVal = api->value_dict_get(itemVal, "id");
    if (idVal && api->value_is_string(idVal)) {
      size_t idLen = 0;
      char* idStr = api->value_get_string(idVal, &idLen);
      if (idStr) {
        itemId = idStr;
        api->value_free_string(idStr);
      }
    }

    auto* cb_data = new GtkMenuCallbackData{on_click, on_click_data, window_id,
                                            itemId.empty() ? label : itemId};
    g_signal_connect_data(gtkItem, "activate",
                          G_CALLBACK(on_gtk_menu_item_activate), cb_data,
                          on_gtk_menu_item_destroy, (GConnectFlags)0);

    wef_value_t* enabledVal = api->value_dict_get(itemVal, "enabled");
    if (enabledVal && api->value_is_bool(enabledVal)) {
      gtk_widget_set_sensitive(gtkItem, api->value_get_bool(enabledVal));
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtkItem);
  }

  return menu;
}

void WebKitGTKBackend::SetApplicationMenu(uint32_t window_id,
                                          wef_value_t* menu_template,
                                          const wef_backend_api_t* api,
                                          wef_menu_click_fn on_click,
                                          void* on_click_data) {
  if (!menu_template)
    return;
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (!state || !state->vbox)
    return;

  // Remove old menu bar if present
  if (state->menu_bar) {
    gtk_container_remove(GTK_CONTAINER(state->vbox), state->menu_bar);
    state->menu_bar = nullptr;
  }

  GtkWidget* menu_bar = BuildGtkMenuFromValue(menu_template, api, window_id,
                                              on_click, on_click_data, true);
  if (menu_bar) {
    // Pack menu bar at the top (before the webview)
    gtk_box_pack_start(GTK_BOX(state->vbox), menu_bar, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(state->vbox), menu_bar, 0);
    state->menu_bar = menu_bar;
    gtk_widget_show_all(menu_bar);
  }
}

// ============================================================================
// Context Menu
// ============================================================================

void WebKitGTKBackend::ShowContextMenu(uint32_t window_id, int x, int y,
                                       wef_value_t* menu_template,
                                       const wef_backend_api_t* api,
                                       wef_menu_click_fn on_click,
                                       void* on_click_data) {
  if (!menu_template)
    return;

  GtkWidget* menu = BuildGtkMenuFromValue(menu_template, api, window_id,
                                          on_click, on_click_data, false);
  if (!menu)
    return;

  gtk_widget_show_all(menu);
  gtk_menu_popup_at_pointer(GTK_MENU(menu), nullptr);
}

// ============================================================================
// DevTools
// ============================================================================

void WebKitGTKBackend::OpenDevTools(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state && state->webview) {
    WebKitWebInspector* inspector =
        webkit_web_view_get_inspector(state->webview);
    webkit_web_inspector_show(inspector);
  }
}

// ============================================================================
// Dialog
// ============================================================================

int WebKitGTKBackend::ShowDialog(uint32_t window_id, int dialog_type,
                                 const std::string& title,
                                 const std::string& message,
                                 const std::string& default_value,
                                 char** out_input_value) {
  if (out_input_value)
    *out_input_value = nullptr;
  GtkWindow* parent = nullptr;
  auto* win = GetWindow(window_id);
  if (win && win->window)
    parent = GTK_WINDOW(win->window);

  if (dialog_type == WEF_DIALOG_ALERT) {
    GtkWidget* dialog =
        gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                               GTK_BUTTONS_OK, "%s", message.c_str());
    gtk_window_set_title(GTK_WINDOW(dialog), title.c_str());
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return 1;
  } else if (dialog_type == WEF_DIALOG_CONFIRM) {
    GtkWidget* dialog =
        gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                               GTK_BUTTONS_OK_CANCEL, "%s", message.c_str());
    gtk_window_set_title(GTK_WINDOW(dialog), title.c_str());
    gint result = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return (result == GTK_RESPONSE_OK) ? 1 : 0;
  } else if (dialog_type == WEF_DIALOG_PROMPT) {
    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        title.c_str(), parent,
        (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, nullptr);
    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget* label = gtk_label_new(message.c_str());
    gtk_container_add(GTK_CONTAINER(content), label);
    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), default_value.c_str());
    gtk_container_add(GTK_CONTAINER(content), entry);
    gtk_widget_show_all(dialog);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);

    gint result = gtk_dialog_run(GTK_DIALOG(dialog));
    const gchar* text = gtk_entry_get_text(GTK_ENTRY(entry));
    std::string result_text = text ? text : "";
    gtk_widget_destroy(dialog);

    if (result == GTK_RESPONSE_OK) {
      if (out_input_value)
        *out_input_value = strdup(result_text.c_str());
      return 1;
    }
    return 0;
  }
  return 0;
}

// ============================================================================
// Dock / taskbar (Linux — X11 urgency hint)
// ============================================================================

void WebKitGTKBackend::BounceDock(int /*type*/) {
  // X11 urgency hint is binary (no informational vs critical). Set it on
  // every WEF window; the WM surfaces attention (flash the taskbar button,
  // highlight the window in overview, etc.).
  std::lock_guard<std::mutex> lock(windows_mutex_);
  for (auto& [wid, state] : windows_) {
    if (!state.window)
      continue;
    gtk_window_set_urgency_hint(GTK_WINDOW(state.window), TRUE);
  }
}

// Badge via title prefix. See CEF backend for the same best-effort model.
static std::mutex g_gtk_badge_mutex;
static std::map<uint32_t, std::string> g_gtk_saved_titles;

void WebKitGTKBackend::SetDockBadge(const char* badge_or_null) {
  std::string badge =
      (badge_or_null && *badge_or_null) ? std::string(badge_or_null) : "";
  std::lock_guard<std::mutex> wlock(windows_mutex_);
  std::lock_guard<std::mutex> block(g_gtk_badge_mutex);
  for (auto& [wid, state] : windows_) {
    if (!state.window)
      continue;
    GtkWindow* gw = GTK_WINDOW(state.window);
    if (!badge.empty()) {
      if (g_gtk_saved_titles.find(wid) == g_gtk_saved_titles.end()) {
        const char* current = gtk_window_get_title(gw);
        g_gtk_saved_titles[wid] = current ? current : "";
      }
      std::string prefixed = "(" + badge + ") " + g_gtk_saved_titles[wid];
      gtk_window_set_title(gw, prefixed.c_str());
    } else {
      auto it = g_gtk_saved_titles.find(wid);
      if (it != g_gtk_saved_titles.end()) {
        gtk_window_set_title(gw, it->second.c_str());
        g_gtk_saved_titles.erase(it);
      }
    }
  }
}

// ============================================================================
// Tray / status bar (Linux) — libappindicator if available
// ============================================================================

#ifdef WEF_HAVE_APPINDICATOR
extern "C" {
#include <libappindicator/app-indicator.h>
}

namespace {
struct LinuxTrayEntry {
  AppIndicator* indicator;
  GtkWidget* menu;
  // Map from dynamically-allocated item command id strings (we use a
  // counter) back to the user-space menu item id.
  std::map<std::string, std::string> cmd_to_id;
  wef_menu_click_fn menu_click_fn;
  void* menu_click_data;
  wef_tray_click_fn click_fn;  // unused on Linux — AppIndicator has no
  void* click_data;            // left-click; left-click shows the menu.
};
std::mutex g_linux_tray_mutex;
std::map<uint32_t, LinuxTrayEntry> g_linux_trays;
std::atomic<uint32_t> g_linux_next_tray_id{1};
std::atomic<uint64_t> g_linux_next_cmd{1};

struct MenuActivateCtx {
  uint32_t tray_id;
  std::string item_id;
};

void OnLinuxTrayMenuActivate(GtkMenuItem* /*item*/, gpointer user_data) {
  auto* ctx = static_cast<MenuActivateCtx*>(user_data);
  wef_menu_click_fn fn = nullptr;
  void* data = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_linux_tray_mutex);
    auto it = g_linux_trays.find(ctx->tray_id);
    if (it != g_linux_trays.end()) {
      fn = it->second.menu_click_fn;
      data = it->second.menu_click_data;
    }
  }
  if (fn)
    fn(data, ctx->tray_id, ctx->item_id.c_str());
}

void DestroyMenuActivateCtx(gpointer data, GClosure* /*closure*/) {
  delete static_cast<MenuActivateCtx*>(data);
}

GtkWidget* BuildLinuxTrayMenu(uint32_t tray_id, wef_value_t* val,
                              const wef_backend_api_t* api) {
  if (!val || !api->value_is_list(val))
    return nullptr;
  GtkWidget* menu = gtk_menu_new();
  size_t count = api->value_list_size(val);
  for (size_t i = 0; i < count; ++i) {
    wef_value_t* itemVal = api->value_list_get(val, i);
    if (!itemVal || !api->value_is_dict(itemVal))
      continue;
    wef_value_t* typeVal = api->value_dict_get(itemVal, "type");
    if (typeVal && api->value_is_string(typeVal)) {
      size_t len = 0;
      char* s = api->value_get_string(typeVal, &len);
      if (s && std::string(s) == "separator") {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());
        api->value_free_string(s);
        continue;
      }
      if (s)
        api->value_free_string(s);
    }
    wef_value_t* labelVal = api->value_dict_get(itemVal, "label");
    std::string label;
    if (labelVal && api->value_is_string(labelVal)) {
      size_t len = 0;
      char* s = api->value_get_string(labelVal, &len);
      if (s) {
        label = std::string(s, len);
        api->value_free_string(s);
      }
    }
    wef_value_t* submenuVal = api->value_dict_get(itemVal, "submenu");
    if (submenuVal && api->value_is_list(submenuVal)) {
      GtkWidget* parent = gtk_menu_item_new_with_label(label.c_str());
      GtkWidget* sub = BuildLinuxTrayMenu(tray_id, submenuVal, api);
      if (sub)
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(parent), sub);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), parent);
      continue;
    }
    wef_value_t* idVal = api->value_dict_get(itemVal, "id");
    std::string item_id;
    if (idVal && api->value_is_string(idVal)) {
      size_t len = 0;
      char* s = api->value_get_string(idVal, &len);
      if (s) {
        item_id = std::string(s, len);
        api->value_free_string(s);
      }
    }
    GtkWidget* mi = gtk_menu_item_new_with_label(label.c_str());
    wef_value_t* enabledVal = api->value_dict_get(itemVal, "enabled");
    if (enabledVal && api->value_is_bool(enabledVal) &&
        !api->value_get_bool(enabledVal)) {
      gtk_widget_set_sensitive(mi, FALSE);
    }
    if (!item_id.empty()) {
      auto* ctx = new MenuActivateCtx{tray_id, item_id};
      g_signal_connect_data(mi, "activate", G_CALLBACK(OnLinuxTrayMenuActivate),
                            ctx, DestroyMenuActivateCtx, (GConnectFlags)0);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
  }
  gtk_widget_show_all(menu);
  return menu;
}
}  // namespace
#endif  // WEF_HAVE_APPINDICATOR

uint32_t WebKitGTKBackend::CreateTrayIcon() {
#ifdef WEF_HAVE_APPINDICATOR
  uint32_t tray_id =
      g_linux_next_tray_id.fetch_add(1, std::memory_order_relaxed);
  std::string idstr = "wef-tray-" + std::to_string(tray_id);
  AppIndicator* ind = app_indicator_new(
      idstr.c_str(), "", APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
  if (!ind)
    return 0;
  app_indicator_set_status(ind, APP_INDICATOR_STATUS_ACTIVE);
  // AppIndicator requires a non-null menu to be visible in most DEs.
  GtkWidget* placeholder = gtk_menu_new();
  gtk_widget_show_all(placeholder);
  app_indicator_set_menu(ind, GTK_MENU(placeholder));
  LinuxTrayEntry entry{};
  entry.indicator = ind;
  entry.menu = placeholder;
  std::lock_guard<std::mutex> lock(g_linux_tray_mutex);
  g_linux_trays[tray_id] = std::move(entry);
  return tray_id;
#else
  return 0;
#endif
}

void WebKitGTKBackend::DestroyTrayIcon(uint32_t tray_id) {
#ifdef WEF_HAVE_APPINDICATOR
  std::lock_guard<std::mutex> lock(g_linux_tray_mutex);
  auto it = g_linux_trays.find(tray_id);
  if (it == g_linux_trays.end())
    return;
  if (it->second.indicator) {
    app_indicator_set_status(it->second.indicator,
                             APP_INDICATOR_STATUS_PASSIVE);
    g_object_unref(it->second.indicator);
  }
  g_linux_trays.erase(it);
#else
  (void)tray_id;
#endif
}

void WebKitGTKBackend::SetTrayIcon(uint32_t tray_id, const void* png_bytes,
                                   size_t len) {
#ifdef WEF_HAVE_APPINDICATOR
  if (!png_bytes || len == 0)
    return;
  // AppIndicator on most DEs reads icons by name from the icon theme, not
  // from raw bytes. Write the bytes to a per-tray temp file and point the
  // indicator at its full path.
  std::string path = "/tmp/wef-tray-" + std::to_string(tray_id) + ".png";
  FILE* f = fopen(path.c_str(), "wb");
  if (!f)
    return;
  fwrite(png_bytes, 1, len, f);
  fclose(f);
  std::lock_guard<std::mutex> lock(g_linux_tray_mutex);
  auto it = g_linux_trays.find(tray_id);
  if (it == g_linux_trays.end() || !it->second.indicator)
    return;
  app_indicator_set_icon_full(it->second.indicator, path.c_str(), "");
#else
  (void)tray_id;
  (void)png_bytes;
  (void)len;
#endif
}

void WebKitGTKBackend::SetTrayTooltip(uint32_t /*tray_id*/,
                                      const char* /*tooltip_or_null*/) {
  // AppIndicator / StatusNotifier has no tooltip concept. The title is
  // visible in some DEs — a v1 approximation would be app_indicator_set_title,
  // but most users expect tooltips not to appear, so we no-op.
}

void WebKitGTKBackend::SetTrayMenu(uint32_t tray_id, wef_value_t* menu_template,
                                   const wef_backend_api_t* api,
                                   wef_menu_click_fn on_click,
                                   void* on_click_data) {
#ifdef WEF_HAVE_APPINDICATOR
  GtkWidget* new_menu =
      menu_template ? BuildLinuxTrayMenu(tray_id, menu_template, api) : nullptr;
  if (menu_template)
    api->value_free(menu_template);
  std::lock_guard<std::mutex> lock(g_linux_tray_mutex);
  auto it = g_linux_trays.find(tray_id);
  if (it == g_linux_trays.end()) {
    if (new_menu)
      gtk_widget_destroy(new_menu);
    return;
  }
  if (new_menu) {
    app_indicator_set_menu(it->second.indicator, GTK_MENU(new_menu));
    it->second.menu = new_menu;
  } else {
    GtkWidget* empty = gtk_menu_new();
    gtk_widget_show_all(empty);
    app_indicator_set_menu(it->second.indicator, GTK_MENU(empty));
    it->second.menu = empty;
  }
  it->second.menu_click_fn = on_click;
  it->second.menu_click_data = on_click_data;
#else
  (void)tray_id;
  (void)menu_template;
  (void)api;
  (void)on_click;
  (void)on_click_data;
#endif
}

void WebKitGTKBackend::SetTrayClickHandler(uint32_t /*tray_id*/,
                                           wef_tray_click_fn /*handler*/,
                                           void* /*user_data*/) {
  // AppIndicator has no left-click event; the indicator's menu pops up on
  // any click. Left-click handlers are a no-op on Linux.
}

// ============================================================================
// Notifications (WebKitGTK Linux)
// ============================================================================
//
// Same as the CEF Linux backend: shell out to `notify-send`. notify-send
// is fire-and-forget, so on_event only sees SHOWN (synthetic, fired
// immediately after spawn) and CLOSED (synthetic, fired from
// CloseNotification). Click / action events are not surfaced.

namespace {

struct WvLinuxNotifEntry {
  std::string tag;
  wef_notification_event_fn on_event;
  void* user_data;
};
std::mutex& WvNotifMutexLinux() {
  static std::mutex m;
  return m;
}
std::map<uint32_t, WvLinuxNotifEntry>& WvNotifMapLinux() {
  static std::map<uint32_t, WvLinuxNotifEntry> map;
  return map;
}
std::atomic<uint32_t> g_wv_next_notif_id_linux{1};

std::string WvShellEscape(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

}  // namespace

uint32_t WebKitGTKBackend::ShowNotification(
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
  bool require_interaction = get_bool("require_interaction", false);
  api->value_free(options);

  uint32_t nid =
      g_wv_next_notif_id_linux.fetch_add(1, std::memory_order_relaxed);

  std::string cmd = "notify-send";
  if (require_interaction)
    cmd += " --urgency=critical";
  if (!tag.empty()) {
    cmd += " --hint=string:x-canonical-private-synchronous:";
    cmd += WvShellEscape(tag);
  }
  cmd += " -- ";
  cmd += WvShellEscape(title);
  cmd += " ";
  cmd += WvShellEscape(body);
  cmd += " &";
  int rc = std::system(cmd.c_str());
  (void)rc;

  {
    std::lock_guard<std::mutex> lock(WvNotifMutexLinux());
    WvNotifMapLinux()[nid] = {tag, on_event, user_data};
  }
  if (on_event)
    on_event(user_data, nid, WEF_NOTIFICATION_SHOWN, nullptr);
  return nid;
}

void WebKitGTKBackend::CloseNotification(uint32_t notification_id) {
  wef_notification_event_fn fn = nullptr;
  void* ud = nullptr;
  {
    std::lock_guard<std::mutex> lock(WvNotifMutexLinux());
    auto it = WvNotifMapLinux().find(notification_id);
    if (it == WvNotifMapLinux().end())
      return;
    fn = it->second.on_event;
    ud = it->second.user_data;
    WvNotifMapLinux().erase(it);
  }
  if (fn)
    fn(ud, notification_id, WEF_NOTIFICATION_CLOSED, nullptr);
}

// ============================================================================
// Factory Function
// ============================================================================

WefBackend* CreateWefBackend() {
  return new WebKitGTKBackend();
}
