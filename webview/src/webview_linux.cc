// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include "runtime_loader.h"
#include "laufey_backend_common.h"
#include "laufey_json.h"
#include "init_script.h"
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

// GDK → W3C key/code lives in backend-common
// (laufey_common::GdkKeyvalToKey / GdkKeycodeToCode).
inline std::string GdkKeyvalToKey(guint keyval) {
  return laufey_common::GdkKeyvalToKey(keyval);
}
inline std::string GdkKeycodeToCode(guint16 hardware_keycode) {
  return laufey_common::GdkKeycodeToCode(hardware_keycode);
}

uint32_t GdkModifiersToLaufey(guint state) {
  uint32_t modifiers = 0;
  if (state & GDK_SHIFT_MASK)
    modifiers |= LAUFEY_MOD_SHIFT;
  if (state & GDK_CONTROL_MASK)
    modifiers |= LAUFEY_MOD_CONTROL;
  if (state & GDK_MOD1_MASK)
    modifiers |= LAUFEY_MOD_ALT;
  if (state & GDK_MOD4_MASK)
    modifiers |= LAUFEY_MOD_META;
  return modifiers;
}

}  // namespace keyboard

// GtkWidget → laufey_id mapping for event routing
static std::map<GtkWidget*, uint32_t> g_widget_to_laufey_id;
static std::mutex g_widget_mutex;

static uint32_t LaufeyIdForWidget(GtkWidget* widget) {
  if (!widget)
    return 0;
  // Walk up to find the toplevel window
  GtkWidget* toplevel = gtk_widget_get_toplevel(widget);
  std::lock_guard<std::mutex> lock(g_widget_mutex);
  auto it = g_widget_to_laufey_id.find(toplevel);
  return it != g_widget_to_laufey_id.end() ? it->second : 0;
}

static void RegisterWidget(GtkWidget* widget, uint32_t window_id) {
  std::lock_guard<std::mutex> lock(g_widget_mutex);
  g_widget_to_laufey_id[widget] = window_id;
}

static void UnregisterWidget(GtkWidget* widget) {
  std::lock_guard<std::mutex> lock(g_widget_mutex);
  g_widget_to_laufey_id.erase(widget);
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
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid == 0)
    return FALSE;

  int state;
  int32_t click_count;

  switch (event->type) {
    case GDK_BUTTON_PRESS:
      state = LAUFEY_MOUSE_PRESSED;
      click_count = 1;
      break;
    case GDK_2BUTTON_PRESS:
      state = LAUFEY_MOUSE_PRESSED;
      click_count = 2;
      break;
    case GDK_3BUTTON_PRESS:
      state = LAUFEY_MOUSE_PRESSED;
      click_count = 2;
      break;
    case GDK_BUTTON_RELEASE:
      state = LAUFEY_MOUSE_RELEASED;
      click_count = g_last_click_count;
      break;
    default:
      return FALSE;
  }

  if (state == LAUFEY_MOUSE_PRESSED) {
    g_last_click_count = click_count;
  }

  int button;
  switch (event->button) {
    case 1:
      button = LAUFEY_MOUSE_BUTTON_LEFT;
      break;
    case 2:
      button = LAUFEY_MOUSE_BUTTON_MIDDLE;
      break;
    case 3:
      button = LAUFEY_MOUSE_BUTTON_RIGHT;
      break;
    case 8:
      button = LAUFEY_MOUSE_BUTTON_BACK;
      break;
    case 9:
      button = LAUFEY_MOUSE_BUTTON_FORWARD;
      break;
    default:
      button = static_cast<int>(event->button);
      break;
  }
  uint32_t modifiers = keyboard::GdkModifiersToLaufey(event->state);

  RuntimeLoader::GetInstance()->DispatchMouseClickEvent(
      wid, state, button, event->x, event->y, modifiers, click_count);

  return FALSE;
}

static gboolean on_motion_event(GtkWidget* widget, GdkEventMotion* event,
                                gpointer user_data) {
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid == 0)
    return FALSE;
  uint32_t modifiers = keyboard::GdkModifiersToLaufey(event->state);
  RuntimeLoader::GetInstance()->DispatchMouseMoveEvent(wid, event->x, event->y,
                                                       modifiers);
  return FALSE;
}

static gboolean on_scroll_event(GtkWidget* widget, GdkEventScroll* event,
                                gpointer user_data) {
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid == 0)
    return FALSE;

  double delta_x = 0, delta_y = 0;
  int32_t delta_mode = LAUFEY_WHEEL_DELTA_LINE;

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
      delta_mode = LAUFEY_WHEEL_DELTA_PIXEL;
      break;
  }

  uint32_t modifiers = keyboard::GdkModifiersToLaufey(event->state);
  RuntimeLoader::GetInstance()->DispatchWheelEvent(
      wid, delta_x, delta_y, event->x, event->y, modifiers, delta_mode);
  return FALSE;
}

static gboolean on_enter_notify_event(GtkWidget* widget,
                                      GdkEventCrossing* event,
                                      gpointer user_data) {
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid == 0)
    return FALSE;
  uint32_t modifiers = keyboard::GdkModifiersToLaufey(event->state);
  RuntimeLoader::GetInstance()->DispatchCursorEnterLeaveEvent(
      wid, 1, event->x, event->y, modifiers);
  return FALSE;
}

static gboolean on_leave_notify_event(GtkWidget* widget,
                                      GdkEventCrossing* event,
                                      gpointer user_data) {
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid == 0)
    return FALSE;
  uint32_t modifiers = keyboard::GdkModifiersToLaufey(event->state);
  RuntimeLoader::GetInstance()->DispatchCursorEnterLeaveEvent(
      wid, 0, event->x, event->y, modifiers);
  return FALSE;
}

static gboolean on_focus_in_event(GtkWidget* widget, GdkEventFocus* event,
                                  gpointer user_data) {
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid == 0)
    return FALSE;
  RuntimeLoader::GetInstance()->DispatchFocusedEvent(wid, 1);
  return FALSE;
}

static gboolean on_focus_out_event(GtkWidget* widget, GdkEventFocus* event,
                                   gpointer user_data) {
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid == 0)
    return FALSE;
  RuntimeLoader::GetInstance()->DispatchFocusedEvent(wid, 0);
  return FALSE;
}

static gboolean on_configure_event(GtkWidget* widget, GdkEventConfigure* event,
                                   gpointer user_data) {
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid == 0)
    return FALSE;
  RuntimeLoader::GetInstance()->DispatchResizeEvent(wid, event->width,
                                                    event->height);
  RuntimeLoader::GetInstance()->DispatchMoveEvent(wid, event->x, event->y);
  return FALSE;
}

static gboolean on_key_event(GtkWidget* widget, GdkEventKey* event,
                             gpointer user_data) {
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid == 0)
    return FALSE;

  int state =
      (event->type == GDK_KEY_PRESS) ? LAUFEY_KEY_PRESSED : LAUFEY_KEY_RELEASED;
  std::string key = keyboard::GdkKeyvalToKey(event->keyval);
  std::string code = keyboard::GdkKeycodeToCode(event->hardware_keycode);
  uint32_t modifiers = keyboard::GdkModifiersToLaufey(event->state);

  RuntimeLoader::GetInstance()->DispatchKeyboardEvent(
      wid, state, key.c_str(), code.c_str(), modifiers, false);

  return FALSE;
}

// ============================================================================
// WebKitGTK Backend
// ============================================================================

class WebKitGTKBackend : public LaufeyBackend {
 public:
  WebKitGTKBackend();
  ~WebKitGTKBackend() override;

  void CreateWindow(uint32_t window_id, int width, int height) override;
  void CreateWindowEx(uint32_t window_id, int width, int height,
                      uint32_t flags) override;
  void CloseWindow(uint32_t window_id) override;

  void Navigate(uint32_t window_id, const std::string& url) override;
  void SetTitle(uint32_t window_id, const std::string& title) override;
  void ExecuteJs(uint32_t window_id, const std::string& script,
                 laufey_js_result_fn callback, void* callback_data) override;
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
                        laufey::ValuePtr args) override;
  void ReleaseJsCallback(uint32_t window_id, uint64_t callback_id) override;
  void RespondToJsCall(uint32_t window_id, uint64_t call_id,
                       laufey::ValuePtr result, laufey::ValuePtr error) override;

  void Run() override;

  void SetApplicationMenu(uint32_t window_id, laufey_value_t* menu_template,
                          const laufey_backend_api_t* api,
                          laufey_menu_click_fn on_click,
                          void* on_click_data) override;

  void ShowContextMenu(uint32_t window_id, int x, int y,
                       laufey_value_t* menu_template, const laufey_backend_api_t* api,
                       laufey_menu_click_fn on_click,
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
  void SetTrayMenu(uint32_t tray_id, laufey_value_t* menu_template,
                   const laufey_backend_api_t* api, laufey_menu_click_fn on_click,
                   void* on_click_data) override;
  void SetTrayClickHandler(uint32_t tray_id, laufey_tray_click_fn handler,
                           void* user_data) override;

  uint32_t ShowNotification(laufey_value_t* options, const laufey_backend_api_t* api,
                            laufey_notification_event_fn on_event,
                            void* user_data) override;
  void CloseNotification(uint32_t notification_id) override;

  // libnotify / notify-send have no permission model — always granted.
  void QueryPermission(int kind, laufey_permission_callback_fn cb,
                       void* user_data) override {
    laufey_common::QueryPermissionStub(kind, cb, user_data);
  }
  void RequestPermission(int kind, laufey_permission_callback_fn cb,
                         void* user_data) override {
    laufey_common::RequestPermissionStub(kind, cb, user_data);
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
    g_content_manager_to_laufey_id;

static void on_script_message(WebKitUserContentManager* manager,
                              WebKitJavascriptResult* js_result,
                              gpointer user_data) {
  auto it = g_content_manager_to_laufey_id.find(manager);
  uint32_t wid = (it != g_content_manager_to_laufey_id.end()) ? it->second : 0;

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
  uint32_t wid = LaufeyIdForWidget(widget);
  if (wid > 0) {
    RuntimeLoader::GetInstance()->DispatchCloseRequestedEvent(wid);
    UnregisterWidget(widget);
  }
  // If no more windows, quit
  {
    std::lock_guard<std::mutex> lock(g_widget_mutex);
    if (g_widget_to_laufey_id.empty()) {
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
        state.content_manager, "laufey");
    g_content_manager_to_laufey_id.erase(state.content_manager);
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
  CreateWindowEx(window_id, width, height, 0);
}

void WebKitGTKBackend::CreateWindowEx(uint32_t window_id, int width, int height,
                                      uint32_t flags) {
  GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  if (flags & LAUFEY_WINDOW_FLAG_FRAMELESS) {
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
  }
  if (flags & LAUFEY_WINDOW_FLAG_NO_ACTIVATE) {
    // Treat as a utility/panel window: out of taskbar & pager, and don't
    // grab focus when shown (the GTK equivalent of a non-activating panel).
    gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_UTILITY);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_focus_on_map(GTK_WINDOW(window), FALSE);
  }
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
  g_signal_connect(content_manager, "script-message-received::laufey",
                   G_CALLBACK(on_script_message), nullptr);
  webkit_user_content_manager_register_script_message_handler(content_manager,
                                                              "laufey");
  g_content_manager_to_laufey_id[content_manager] = window_id;

  WebKitWebView* webview = WEBKIT_WEB_VIEW(
      webkit_web_view_new_with_user_content_manager(content_manager));

  g_signal_connect(webview, "script-dialog", G_CALLBACK(on_script_dialog),
                   nullptr);

  WebKitSettings* wk_settings = webkit_web_view_get_settings(webview);
  webkit_settings_set_enable_developer_extras(wk_settings, TRUE);

  std::string initScript = BuildInitScript(
      RuntimeLoader::GetInstance()->GetJsNamespace(),
      "window.webkit.messageHandlers.laufey.postMessage(JSON.stringify({\n"
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
        state->content_manager, "laufey");
    g_content_manager_to_laufey_id.erase(state->content_manager);
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
  laufey_js_result_fn callback;
  void* user_data;
};

static void on_execute_js_finished(GObject* source, GAsyncResult* result,
                                   gpointer user_data) {
  auto* cb_data = static_cast<ExecuteJsCallbackData*>(user_data);

  GError* error = nullptr;
  WebKitJavascriptResult* js_result = webkit_web_view_run_javascript_finish(
      WEBKIT_WEB_VIEW(source), result, &error);

  if (error) {
    auto errVal = laufey::Value::String(error->message);
    laufey_value errLaufey(errVal);
    cb_data->callback(nullptr, &errLaufey, cb_data->user_data);
    g_error_free(error);
  } else if (js_result) {
    JSCValue* value = webkit_javascript_result_get_js_value(js_result);
    if (jsc_value_is_null(value) || jsc_value_is_undefined(value)) {
      cb_data->callback(nullptr, nullptr, cb_data->user_data);
    } else if (jsc_value_is_boolean(value)) {
      auto val = laufey::Value::Bool(jsc_value_to_boolean(value));
      laufey_value laufey(val);
      cb_data->callback(&laufey, nullptr, cb_data->user_data);
    } else if (jsc_value_is_number(value)) {
      double d = jsc_value_to_double(value);
      if (d == (int)d && d >= INT_MIN && d <= INT_MAX) {
        auto val = laufey::Value::Int((int)d);
        laufey_value laufey(val);
        cb_data->callback(&laufey, nullptr, cb_data->user_data);
      } else {
        auto val = laufey::Value::Double(d);
        laufey_value laufey(val);
        cb_data->callback(&laufey, nullptr, cb_data->user_data);
      }
    } else if (jsc_value_is_string(value)) {
      gchar* str = jsc_value_to_string(value);
      auto val = laufey::Value::String(str);
      laufey_value laufey(val);
      cb_data->callback(&laufey, nullptr, cb_data->user_data);
      g_free(str);
    } else {
      // For objects/arrays, serialize to JSON and parse
      gchar* json = jsc_value_to_json(value, 0);
      if (json) {
        auto val = json::ParseJson(json);
        laufey_value laufey(val);
        cb_data->callback(&laufey, nullptr, cb_data->user_data);
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
                                 laufey_js_result_fn callback,
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
                                        laufey::ValuePtr args) {
  std::string argsJson = json::Serialize(args);
  std::string script = BuildInvokeCallbackScript(callback_id, argsJson);
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
  std::string script = BuildReleaseCallbackScript(callback_id);
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
                                       laufey::ValuePtr result,
                                       laufey::ValuePtr error) {
  std::string resultJson = json::Serialize(result);
  std::string errorJson =
      (error && !error->IsNull()) ? json::Serialize(error) : "null";
  std::string script =
      BuildRespondScript(call_id, resultJson, errorJson, errorJson != "null");
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
  laufey::ValuePtr msg = json::ParseJson(jsonStr);
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
  laufey::ValuePtr args =
      (argsIt != dict.end()) ? argsIt->second : laufey::Value::List();

  RuntimeLoader::GetInstance()->OnJsCall(window_id, call_id, method, args);
}

// ============================================================================
// Application Menu / Context Menu
// ============================================================================
//
// Menu construction lives in backend-common
// (laufey_common::BuildGtkMenuFromValue).

void WebKitGTKBackend::SetApplicationMenu(uint32_t window_id,
                                          laufey_value_t* menu_template,
                                          const laufey_backend_api_t* api,
                                          laufey_menu_click_fn on_click,
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

  GtkWidget* menu_bar = laufey_common::BuildGtkMenuFromValue(
      menu_template, api, window_id, on_click, on_click_data, true);
  if (menu_bar) {
    // Pack menu bar at the top (before the webview)
    gtk_box_pack_start(GTK_BOX(state->vbox), menu_bar, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(state->vbox), menu_bar, 0);
    state->menu_bar = menu_bar;
    gtk_widget_show_all(menu_bar);
  }
}

void WebKitGTKBackend::ShowContextMenu(uint32_t window_id, int /*x*/, int /*y*/,
                                       laufey_value_t* menu_template,
                                       const laufey_backend_api_t* api,
                                       laufey_menu_click_fn on_click,
                                       void* on_click_data) {
  if (!menu_template)
    return;

  GtkWidget* menu = laufey_common::BuildGtkMenuFromValue(
      menu_template, api, window_id, on_click, on_click_data, false);
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

int WebKitGTKBackend::ShowDialog(uint32_t /*window_id*/, int dialog_type,
                                 const std::string& title,
                                 const std::string& message,
                                 const std::string& default_value,
                                 char** out_input_value) {
  return laufey_common::ShowDialogLinux(dialog_type, title, message, default_value,
                                     out_input_value);
}

// ============================================================================
// Dock / taskbar (Linux — X11 urgency hint)
// ============================================================================

void WebKitGTKBackend::BounceDock(int /*type*/) {
  // X11 urgency hint is binary (no informational vs critical). Set it on
  // every LAUFEY window; the WM surfaces attention (flash the taskbar button,
  // highlight the window in overview, etc.).
  std::lock_guard<std::mutex> lock(windows_mutex_);
  for (auto& [wid, state] : windows_) {
    if (!state.window)
      continue;
    gtk_window_set_urgency_hint(GTK_WINDOW(state.window), TRUE);
  }
}

// Badge via title prefix. Saved-titles map lives in
// laufey_common::ApplyTitlePrefixBadge.
void WebKitGTKBackend::SetDockBadge(const char* badge_or_null) {
  std::string badge =
      (badge_or_null && *badge_or_null) ? std::string(badge_or_null) : "";
  std::lock_guard<std::mutex> wlock(windows_mutex_);
  for (auto& [wid, state] : windows_) {
    if (!state.window)
      continue;
    GtkWindow* gw = GTK_WINDOW(state.window);
    const char* current = gtk_window_get_title(gw);
    std::string next = laufey_common::ApplyTitlePrefixBadge(
        wid, current ? std::string(current) : std::string(), badge);
    gtk_window_set_title(gw, next.c_str());
  }
}

// ============================================================================
// Tray / status bar (Linux) — libappindicator if available
// ============================================================================

// Thin trampolines over backend-common/src/tray_linux.cc.

uint32_t WebKitGTKBackend::CreateTrayIcon() {
  return laufey_common::CreateTrayIconLinux();
}
void WebKitGTKBackend::DestroyTrayIcon(uint32_t tray_id) {
  laufey_common::DestroyTrayIconLinux(tray_id);
}
void WebKitGTKBackend::SetTrayIcon(uint32_t tray_id, const void* png_bytes,
                                   size_t len) {
  laufey_common::SetTrayIconLinux(tray_id, png_bytes, len);
}
void WebKitGTKBackend::SetTrayTooltip(uint32_t tray_id,
                                      const char* tooltip_or_null) {
  laufey_common::SetTrayTooltipLinux(tray_id, tooltip_or_null);
}
void WebKitGTKBackend::SetTrayMenu(uint32_t tray_id, laufey_value_t* menu_template,
                                   const laufey_backend_api_t* api,
                                   laufey_menu_click_fn on_click,
                                   void* on_click_data) {
  laufey_common::SetTrayMenuLinux(tray_id, menu_template, api, on_click,
                               on_click_data);
}
void WebKitGTKBackend::SetTrayClickHandler(uint32_t tray_id,
                                           laufey_tray_click_fn handler,
                                           void* user_data) {
  laufey_common::SetTrayClickHandlerLinux(tray_id, handler, user_data);
}

// ============================================================================
// Notifications (WebKitGTK Linux)
// ============================================================================
//
// Thin trampoline over the shared notify-send implementation in
// backend-common/src/notifications_linux.cc.

uint32_t WebKitGTKBackend::ShowNotification(laufey_value_t* options,
                                            const laufey_backend_api_t* api,
                                            laufey_notification_event_fn on_event,
                                            void* user_data) {
  laufey_common::NotificationOptions opts =
      laufey_common::ParseNotificationOptions(options, api);
  return laufey_common::ShowNotificationLinux(opts, on_event, user_data);
}

void WebKitGTKBackend::CloseNotification(uint32_t notification_id) {
  laufey_common::CloseNotificationLinux(notification_id);
}

// ============================================================================
// Factory Function
// ============================================================================

LaufeyBackend* CreateLaufeyBackend() {
  return new WebKitGTKBackend();
}
