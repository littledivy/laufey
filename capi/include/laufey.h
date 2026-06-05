// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef LAUFEY_H
#define LAUFEY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LAUFEY_API_VERSION 25

// Window handle types for get_window_handle_type
#define LAUFEY_WINDOW_HANDLE_UNKNOWN 0
#define LAUFEY_WINDOW_HANDLE_APPKIT 1
#define LAUFEY_WINDOW_HANDLE_WIN32 2
#define LAUFEY_WINDOW_HANDLE_X11 3
#define LAUFEY_WINDOW_HANDLE_WAYLAND 4

// Window creation flags for create_window_ex (bitmask).
//
// FRAMELESS removes the title bar and standard window chrome (border,
// traffic-light / caption buttons). NO_ACTIVATE creates a utility "panel"
// window that floats above normal windows and does not activate the app or
// steal key focus from the foreground app when shown — the combination used
// for tray / menu-bar popovers. On macOS NO_ACTIVATE maps to a
// non-activating NSPanel; on Windows to WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
// on Linux to a utility window type hint.
//
// TRANSPARENT_TITLEBAR keeps the standard window frame and traffic-light /
// caption buttons but makes the title bar transparent and lets the web
// content extend underneath it (Electron `titleBarStyle: 'hidden'`). The page
// can then draw its own toolbar in that strip, with the system buttons
// overlaid. The app is responsible for insetting its UI to clear the
// traffic-light buttons. macOS only for now; ignored by other backends.
#define LAUFEY_WINDOW_FLAG_FRAMELESS (1u << 0)
#define LAUFEY_WINDOW_FLAG_NO_ACTIVATE (1u << 1)
#define LAUFEY_WINDOW_FLAG_TRANSPARENT_TITLEBAR (1u << 2)

typedef struct laufey_backend_api laufey_backend_api_t;

typedef int (*laufey_runtime_init_fn)(const laufey_backend_api_t* api);
#define LAUFEY_RUNTIME_INIT_SYMBOL "laufey_runtime_init"

typedef int (*laufey_runtime_start_fn)(void);
#define LAUFEY_RUNTIME_START_SYMBOL "laufey_runtime_start"

typedef void (*laufey_runtime_shutdown_fn)(void);
#define LAUFEY_RUNTIME_SHUTDOWN_SYMBOL "laufey_runtime_shutdown"

typedef struct laufey_value laufey_value_t;

typedef void (*laufey_js_call_fn)(void* user_data, uint32_t window_id,
                               uint64_t call_id, const char* method_path,
                               laufey_value_t* args);

// Callback for execute_js results. Pass NULL to execute_js for fire-and-forget.
typedef void (*laufey_js_result_fn)(laufey_value_t* result, laufey_value_t* error,
                                 void* user_data);

typedef void (*laufey_menu_click_fn)(void* user_data, uint32_t window_id,
                                  const char* item_id);

// Keyboard event state
#define LAUFEY_KEY_PRESSED 0
#define LAUFEY_KEY_RELEASED 1

// Keyboard modifier flags (bitmask)
#define LAUFEY_MOD_SHIFT (1 << 0)
#define LAUFEY_MOD_CONTROL (1 << 1)
#define LAUFEY_MOD_ALT (1 << 2)
#define LAUFEY_MOD_META (1 << 3)

// Mouse button constants
#define LAUFEY_MOUSE_BUTTON_LEFT 0
#define LAUFEY_MOUSE_BUTTON_RIGHT 1
#define LAUFEY_MOUSE_BUTTON_MIDDLE 2
#define LAUFEY_MOUSE_BUTTON_BACK 3
#define LAUFEY_MOUSE_BUTTON_FORWARD 4

// Mouse event state
#define LAUFEY_MOUSE_PRESSED 0
#define LAUFEY_MOUSE_RELEASED 1

// Dialog types
#define LAUFEY_DIALOG_ALERT 0
#define LAUFEY_DIALOG_CONFIRM 1
#define LAUFEY_DIALOG_PROMPT 2

// Dock / taskbar bounce / flash types. Values match macOS
// NSRequestUserAttentionType so the ABI passes through unchanged.
#define LAUFEY_DOCK_BOUNCE_INFORMATIONAL 10
#define LAUFEY_DOCK_BOUNCE_CRITICAL 0

// Callback fired when the user clicks the dock / taskbar icon for an app that
// has no visible windows (macOS only; Windows/Linux have no equivalent event).
// has_visible_windows is true if any app window is currently on-screen.
typedef void (*laufey_dock_reopen_fn)(void* user_data, bool has_visible_windows);

// Callback fired when the user left-clicks a tray / status-bar icon.
// (Right-click is reserved for the tray's menu.)
typedef void (*laufey_tray_click_fn)(void* user_data, uint32_t tray_id);

// Notification event reasons (passed as `reason` to laufey_notification_event_fn).
#define LAUFEY_NOTIFICATION_SHOWN 0
#define LAUFEY_NOTIFICATION_CLICKED 1
#define LAUFEY_NOTIFICATION_CLOSED 2
#define LAUFEY_NOTIFICATION_ACTION 3

// Callback fired for events on a notification previously shown via
// `show_notification`. `action_id_or_null` is non-NULL only for
// LAUFEY_NOTIFICATION_ACTION (the id of the action button the user clicked).
typedef void (*laufey_notification_event_fn)(void* user_data,
                                          uint32_t notification_id, int reason,
                                          const char* action_id_or_null);

// --- Permissions / runtime authorization ---
//
// Capability kinds. `0` is reserved as INVALID so that an uninitialized int
// can't be mistaken for a real kind. Add new kinds at the end; this is
// part of the wire ABI.
#define LAUFEY_PERMISSION_INVALID 0
#define LAUFEY_PERMISSION_NOTIFICATIONS 1

// Authorization state returned by permission callbacks.
//   GRANTED:     the runtime may use the capability.
//   DENIED:      the user (or policy) declined; do not prompt again -- the
//                OS won't show it.
//   PROMPT:      the user has not yet decided; calling request_permission
//                will display a system prompt.
//   UNSUPPORTED: this capability is not authorizable in the current
//                environment (e.g. unbundled macOS process with no
//                CFBundleIdentifier, or a backend that has no concept of
//                the kind on this platform).
#define LAUFEY_PERMISSION_STATUS_GRANTED 0
#define LAUFEY_PERMISSION_STATUS_DENIED 1
#define LAUFEY_PERMISSION_STATUS_PROMPT 2
#define LAUFEY_PERMISSION_STATUS_UNSUPPORTED 3

// Callback invoked with the result of request_permission / query_permission.
// Always fired on the UI thread (backends hop via their UI dispatch if the
// underlying OS API completes on a background queue).
typedef void (*laufey_permission_callback_fn)(void* user_data, int status);

// Callback for mouse click events.
typedef void (*laufey_mouse_click_fn)(
    void* user_data, uint32_t window_id,
    int state,           // LAUFEY_MOUSE_PRESSED or LAUFEY_MOUSE_RELEASED
    int button,          // LAUFEY_MOUSE_BUTTON_*
    double x,            // x position in window coordinates
    double y,            // y position in window coordinates
    uint32_t modifiers,  // bitmask of LAUFEY_MOD_* flags
    int32_t click_count  // 1 = single, 2 = double click
);

// Callback for mouse move events.
typedef void (*laufey_mouse_move_fn)(
    void* user_data, uint32_t window_id,
    double x,           // x position in window coordinates
    double y,           // y position in window coordinates
    uint32_t modifiers  // bitmask of LAUFEY_MOD_* flags
);

// Wheel delta mode
#define LAUFEY_WHEEL_DELTA_PIXEL 0
#define LAUFEY_WHEEL_DELTA_LINE 1
#define LAUFEY_WHEEL_DELTA_PAGE 2

// Callback for wheel (scroll) events.
typedef void (*laufey_wheel_fn)(
    void* user_data, uint32_t window_id,
    double delta_x,      // horizontal scroll amount
    double delta_y,      // vertical scroll amount
    double x,            // cursor x position in window coordinates
    double y,            // cursor y position in window coordinates
    uint32_t modifiers,  // bitmask of LAUFEY_MOD_* flags
    int32_t delta_mode   // LAUFEY_WHEEL_DELTA_*
);

// Callback for cursor enter/leave events (mouseenter/mouseleave).
typedef void (*laufey_cursor_enter_leave_fn)(
    void* user_data, uint32_t window_id,
    int entered,        // 1 = cursor entered window, 0 = cursor left window
    double x,           // cursor x position in window coordinates
    double y,           // cursor y position in window coordinates
    uint32_t modifiers  // bitmask of LAUFEY_MOD_* flags
);

// Callback for window move events.
typedef void (*laufey_move_fn)(void* user_data, uint32_t window_id,
                            int x,  // new x position
                            int y   // new y position
);

// Callback for window resize events.
typedef void (*laufey_resize_fn)(void* user_data, uint32_t window_id,
                              int width,  // new width in pixels
                              int height  // new height in pixels
);

// Callback for window focus/blur events.
typedef void (*laufey_focused_fn)(
    void* user_data, uint32_t window_id,
    int focused  // 1 = window gained focus, 0 = window lost focus
);

// Callback for keyboard events.
typedef void (*laufey_keyboard_event_fn)(
    void* user_data, uint32_t window_id,
    int state,        // LAUFEY_KEY_PRESSED or LAUFEY_KEY_RELEASED
    const char* key,  // logical key (W3C UI Events key value, e.g. "a",
                      // "Enter", "Shift")
    const char*
        code,  // physical key code (W3C UI Events code, e.g. "KeyA", "Enter")
    uint32_t modifiers,  // bitmask of LAUFEY_MOD_* flags
    bool repeat);

// Callback for window close requested events.
typedef void (*laufey_close_requested_fn)(void* user_data, uint32_t window_id);

struct laufey_backend_api {
  uint32_t version;
  void* backend_data;

  // Window lifecycle
  uint32_t (*create_window)(void* backend_data);
  void (*close_window)(void* backend_data, uint32_t window_id);

  // Create a window with creation-time style flags (see LAUFEY_WINDOW_FLAG_*).
  // Flags that can only be decided when the OS window is constructed
  // (frameless chrome, non-activating panel level) are honored here;
  // post-creation properties (size, position, resizable, always-on-top) are
  // still set via their respective setters. Backends added before API
  // version 25 leave this NULL, in which case callers fall back to
  // create_window and the flags are ignored.
  uint32_t (*create_window_ex)(void* backend_data, uint32_t flags);

  void (*navigate)(void* backend_data, uint32_t window_id, const char* url);
  void (*set_title)(void* backend_data, uint32_t window_id, const char* title);
  void (*execute_js)(void* backend_data, uint32_t window_id, const char* script,
                     laufey_js_result_fn callback, void* callback_data);
  void (*quit)(void* backend_data);
  void (*set_window_size)(void* backend_data, uint32_t window_id, int width,
                          int height);
  void (*get_window_size)(void* backend_data, uint32_t window_id, int* width,
                          int* height);
  void (*set_window_position)(void* backend_data, uint32_t window_id, int x,
                              int y);
  void (*get_window_position)(void* backend_data, uint32_t window_id, int* x,
                              int* y);
  void (*set_resizable)(void* backend_data, uint32_t window_id, bool resizable);
  bool (*is_resizable)(void* backend_data, uint32_t window_id);
  void (*set_always_on_top)(void* backend_data, uint32_t window_id,
                            bool always_on_top);
  bool (*is_always_on_top)(void* backend_data, uint32_t window_id);
  bool (*is_visible)(void* backend_data, uint32_t window_id);
  void (*show)(void* backend_data, uint32_t window_id);
  void (*hide)(void* backend_data, uint32_t window_id);
  void (*focus)(void* backend_data, uint32_t window_id);
  void (*post_ui_task)(void* backend_data, void (*task)(void* data),
                       void* data);

  bool (*value_is_null)(laufey_value_t* val);
  bool (*value_is_bool)(laufey_value_t* val);
  bool (*value_is_int)(laufey_value_t* val);
  bool (*value_is_double)(laufey_value_t* val);
  bool (*value_is_string)(laufey_value_t* val);
  bool (*value_is_list)(laufey_value_t* val);
  bool (*value_is_dict)(laufey_value_t* val);
  bool (*value_is_binary)(laufey_value_t* val);
  bool (*value_is_callback)(laufey_value_t* val);

  bool (*value_get_bool)(laufey_value_t* val);
  int (*value_get_int)(laufey_value_t* val);
  double (*value_get_double)(laufey_value_t* val);

  char* (*value_get_string)(laufey_value_t* val, size_t* len_out);
  void (*value_free_string)(char* str);

  size_t (*value_list_size)(laufey_value_t* val);
  laufey_value_t* (*value_list_get)(laufey_value_t* val, size_t index);

  laufey_value_t* (*value_dict_get)(laufey_value_t* dict, const char* key);
  bool (*value_dict_has)(laufey_value_t* dict, const char* key);
  size_t (*value_dict_size)(laufey_value_t* dict);

  char** (*value_dict_keys)(laufey_value_t* dict, size_t* count_out);
  void (*value_free_keys)(char** keys, size_t count);

  const void* (*value_get_binary)(laufey_value_t* val, size_t* len_out);

  uint64_t (*value_get_callback_id)(laufey_value_t* val);

  laufey_value_t* (*value_null)(void* backend_data);
  laufey_value_t* (*value_bool)(void* backend_data, bool val);
  laufey_value_t* (*value_int)(void* backend_data, int val);
  laufey_value_t* (*value_double)(void* backend_data, double val);
  laufey_value_t* (*value_string)(void* backend_data, const char* val);
  laufey_value_t* (*value_list)(void* backend_data);
  laufey_value_t* (*value_dict)(void* backend_data);
  laufey_value_t* (*value_binary)(void* backend_data, const void* data,
                               size_t len);

  bool (*value_list_append)(laufey_value_t* list, laufey_value_t* val);
  bool (*value_list_set)(laufey_value_t* list, size_t index, laufey_value_t* val);

  bool (*value_dict_set)(laufey_value_t* dict, const char* key, laufey_value_t* val);

  void (*value_free)(laufey_value_t* val);

  void (*set_js_call_handler)(void* backend_data, laufey_js_call_fn handler,
                              void* user_data);
  void (*js_call_respond)(void* backend_data, uint64_t call_id,
                          laufey_value_t* result, laufey_value_t* error);
  void (*invoke_js_callback)(void* backend_data, uint64_t callback_id,
                             laufey_value_t* args);
  void (*release_js_callback)(void* backend_data, uint64_t callback_id);

  // Raw window/display handles for GPU surface creation.
  // Returns platform-specific handle:
  //   AppKit: NSView*, Win32: HWND, X11: Window (cast to void*), Wayland:
  //   wl_surface*
  void* (*get_window_handle)(void* backend_data, uint32_t window_id);
  // Returns platform-specific display handle:
  //   AppKit: NULL, Win32: NULL (or HINSTANCE), X11: Display*, Wayland:
  //   wl_display*
  void* (*get_display_handle)(void* backend_data, uint32_t window_id);
  // Returns LAUFEY_WINDOW_HANDLE_* constant identifying the platform
  int (*get_window_handle_type)(void* backend_data, uint32_t window_id);

  // Register a handler for keyboard input events (global, receives window_id in
  // callback).
  void (*set_keyboard_event_handler)(void* backend_data,
                                     laufey_keyboard_event_fn handler,
                                     void* user_data);

  // Register a handler for mouse click events.
  void (*set_mouse_click_handler)(void* backend_data,
                                  laufey_mouse_click_fn handler, void* user_data);

  // Register a handler for mouse move events.
  void (*set_mouse_move_handler)(void* backend_data, laufey_mouse_move_fn handler,
                                 void* user_data);

  // Register a handler for wheel (scroll) events.
  void (*set_wheel_handler)(void* backend_data, laufey_wheel_fn handler,
                            void* user_data);

  // Register a handler for cursor enter/leave events.
  void (*set_cursor_enter_leave_handler)(void* backend_data,
                                         laufey_cursor_enter_leave_fn handler,
                                         void* user_data);

  // Register a handler for window focus/blur events.
  void (*set_focused_handler)(void* backend_data, laufey_focused_fn handler,
                              void* user_data);

  // Register a handler for window resize events.
  void (*set_resize_handler)(void* backend_data, laufey_resize_fn handler,
                             void* user_data);

  // Register a handler for window move events.
  void (*set_move_handler)(void* backend_data, laufey_move_fn handler,
                           void* user_data);

  // Register a handler for window close requested events.
  void (*set_close_requested_handler)(void* backend_data,
                                      laufey_close_requested_fn handler,
                                      void* user_data);

  void (*poll_js_calls)(void* backend_data);

  void (*set_js_call_notify)(void* backend_data,
                             void (*notify_fn)(void* notify_data),
                             void* notify_data);

  // Application menu. menu_template is a laufey_value_t list of menu items.
  // Each item is a dict with: label, submenu (list), role, type, id,
  // accelerator. When a custom item (with "id") is clicked, on_click is called
  // with the id. On macOS the menu is applied to the global menu bar and
  // swapped on window focus. On Windows/Linux the menu is attached to the
  // specific window.
  void (*set_application_menu)(void* backend_data, uint32_t window_id,
                               laufey_value_t* menu_template,
                               laufey_menu_click_fn on_click, void* on_click_data);

  // Show a context menu at the given position (in window coordinates).
  // menu_template uses the same format as set_application_menu (list of menu
  // item dicts). on_click is called with the id of the clicked item.
  void (*show_context_menu)(void* backend_data, uint32_t window_id, int x,
                            int y, laufey_value_t* menu_template,
                            laufey_menu_click_fn on_click, void* on_click_data);

  // Open the DevTools inspector for the given window.
  void (*open_devtools)(void* backend_data, uint32_t window_id);

  // Set the global JS namespace name for bindings (default: "Laufey").
  // Must be called before creating any windows.
  void (*set_js_namespace)(void* backend_data, const char* name);

  // Show a native dialog (alert, confirm, or prompt) and BLOCK until the
  // user dismisses it. Must be called on the main / UI thread (the same
  // thread the LAUFEY event loop runs on); backends use the platform's modal
  // run loop (`runModal` / `MessageBoxW` / `gtk_dialog_run`) which itself
  // pumps OS events while the dialog is up, so other LAUFEY windows continue
  // to render and respond.
  //
  // Returns 1 if the user clicked OK/Yes, 0 otherwise.
  // For LAUFEY_DIALOG_PROMPT, on a confirmed result `*out_input_value` is set
  // to a heap-allocated UTF-8 string the caller must free by calling
  // `string_free` (below). For alert/confirm, or on cancel, set to NULL.
  // Pass NULL for `out_input_value` if you don't need the input value.
  int (*show_dialog)(
      void* backend_data, uint32_t window_id,
      int dialog_type,  // LAUFEY_DIALOG_*
      const char* title, const char* message,
      const char* default_value,  // For prompt: default input text. NULL for
                                  // alert/confirm.
      char** out_input_value);

  // Free a string returned by `show_dialog` (the prompt input value).
  // Routed through the backend so the matching allocator is used (avoids
  // cross-runtime free hazards on Windows MSVC). Safe to call with NULL.
  void (*string_free)(void* backend_data, char* s);

  // --- Dock / taskbar ---
  //
  // Semantics are app-scoped on macOS (all operate on the process's Dock
  // tile) and focused-window-scoped on Windows/Linux (taskbar button for the
  // currently-focused LAUFEY window). Backends that don't support an operation
  // on a given platform leave the function pointer NULL.

  // Set or clear a short text badge on the app's dock / taskbar icon.
  // Pass NULL or "" to clear. macOS: NSDockTile badgeLabel. Windows: renders
  // text to a small overlay icon via GDI+ + ITaskbarList3::SetOverlayIcon.
  // Linux: prepends "(text) " to the focused window's title.
  void (*set_dock_badge)(void* backend_data, const char* badge_or_null);

  // Request the user's attention by bouncing the dock icon (macOS) or
  // flashing the focused window's taskbar button (Windows) or setting the
  // urgency hint (Linux). `type` is LAUFEY_DOCK_BOUNCE_*.
  void (*bounce_dock)(void* backend_data, int type);

  // Set a custom menu for the app's dock icon (macOS only).
  // menu_template uses the same format as set_application_menu. on_click is
  // called with the id of the clicked item. window_id in the callback will
  // be 0 since the menu is app-scoped. Windows/Linux: leave NULL.
  void (*set_dock_menu)(void* backend_data, laufey_value_t* menu_template,
                        laufey_menu_click_fn on_click, void* on_click_data);

  // Show or hide the app from the dock / task switcher (macOS activation
  // policy). Windows/Linux: leave NULL (no app-level equivalent).
  void (*set_dock_visible)(void* backend_data, bool visible);

  // Register a callback invoked when the user clicks the dock icon for an
  // app that has no visible windows (macOS). The backend always swallows
  // the default "show hidden window" behavior — the user callback is
  // informational. Windows/Linux: leave NULL.
  void (*set_dock_reopen_handler)(void* backend_data, laufey_dock_reopen_fn fn,
                                  void* user_data);

  // --- Tray / status-bar icon ---
  //
  // A tray icon is an explicitly-created, persistent icon in the OS status
  // area (macOS menu bar extras, Windows system tray, Linux AppIndicator).
  // Each call to create_tray_icon returns a new id; destroy removes it.
  // Backends that don't support tray icons leave these NULL.

  // Create a new empty tray icon. Returns a tray_id > 0, or 0 on failure.
  uint32_t (*create_tray_icon)(void* backend_data);

  // Destroy a tray icon created via create_tray_icon.
  void (*destroy_tray_icon)(void* backend_data, uint32_t tray_id);

  // Set the icon image (PNG-encoded bytes). Required before the icon is
  // visible on most platforms.
  void (*set_tray_icon)(void* backend_data, uint32_t tray_id,
                        const void* png_bytes, size_t len);

  // Set or clear the tooltip shown on hover. Pass NULL or "" to clear.
  void (*set_tray_tooltip)(void* backend_data, uint32_t tray_id,
                           const char* tooltip_or_null);

  // Set the context (right-click) menu. menu_template uses the same format
  // as set_application_menu. on_click is called with the id of the clicked
  // item; the window_id argument of the callback is 0 (tray menus are
  // app-scoped, not window-scoped). Pass NULL menu_template to clear.
  void (*set_tray_menu)(void* backend_data, uint32_t tray_id,
                        laufey_value_t* menu_template, laufey_menu_click_fn on_click,
                        void* on_click_data);

  // Register a handler for left-click on the tray icon.
  void (*set_tray_click_handler)(void* backend_data, uint32_t tray_id,
                                 laufey_tray_click_fn handler, void* user_data);

  // Register a handler for left-double-click. Fires after a quick second
  // click; the single-click handler (if any) still fires for the first
  // click. No-op on Linux (AppIndicator has no click events).
  void (*set_tray_double_click_handler)(void* backend_data, uint32_t tray_id,
                                        laufey_tray_click_fn handler,
                                        void* user_data);

  // Set the icon used when the OS is in dark mode. When set, the backend
  // swaps between the primary (light) icon from set_tray_icon and this
  // one based on the current system appearance. Pass NULL/zero len to
  // clear the dark variant (then the primary icon is used in both modes).
  void (*set_tray_icon_dark)(void* backend_data, uint32_t tray_id,
                             const void* png_bytes, size_t len);

  // Get the tray icon's bounding rectangle in screen coordinates, using the
  // same top-left-origin, density-independent-pixel space as
  // get_window_position / set_window_position so a window can be anchored to
  // the icon. Writes x/y/width/height (any may be NULL) and returns true on
  // success. Returns false if the id is unknown, the icon has no on-screen
  // position yet, or the backend/platform can't report it (NULL fn pointer
  // included). Used to position tray popover panels under the icon.
  bool (*get_tray_icon_bounds)(void* backend_data, uint32_t tray_id, int* x,
                               int* y, int* width, int* height);

  // --- Notifications (system / desktop) ---
  //
  // App-scoped. `options` is a laufey_value_t dict mirroring (a subset of)
  // the Web Notification API constructor options:
  //   "title"               string  (required)
  //   "body"                string
  //   "icon"                binary  (PNG bytes)
  //   "tag"                 string  — replaces an existing notification
  //                                   with the same tag rather than
  //                                   creating a new one
  //   "silent"              bool    — suppress system notification sound
  //   "require_interaction" bool    — keep visible until user dismisses
  //   "actions"             list of dicts, each {"id": string,
  //                                   "title": string} — action buttons.
  //                                   Ignored on platforms that don't
  //                                   support them.
  //
  // Ownership: the backend takes ownership of `options` (calls value_free
  // on it), matching the convention used by set_application_menu /
  // show_context_menu / set_tray_menu.
  //
  // Returns a notification_id > 0 used by close_notification and as the
  // first arg to the event callback. Returns 0 on failure (including when
  // notifications aren't supported on the current platform / backend).
  //
  // Pass NULL for `on_event` for fire-and-forget. Backends that don't
  // support tracking events for a given OS API may invoke only some of
  // the LAUFEY_NOTIFICATION_* reasons.
  uint32_t (*show_notification)(void* backend_data, laufey_value_t* options,
                                laufey_notification_event_fn on_event,
                                void* user_data);

  // Close a notification previously shown via show_notification. No-op
  // if the id is unknown or the notification has already been dismissed.
  void (*close_notification)(void* backend_data, uint32_t notification_id);

  // --- Permissions / runtime authorization ---
  //
  // Ask the OS for the current authorization status of a capability
  // (`kind` is LAUFEY_PERMISSION_*). Does NOT prompt the user. The callback
  // is invoked on the UI thread with one of LAUFEY_PERMISSION_STATUS_*.
  // If this function pointer is NULL the runtime treats every kind as
  // UNSUPPORTED.
  void (*query_permission)(void* backend_data, int kind,
                           laufey_permission_callback_fn cb, void* user_data);

  // Request authorization for a capability. If the current status is
  // PROMPT the OS will display a system prompt; otherwise the cached
  // decision is returned without re-prompting (the OS will not show a
  // second prompt for a kind the user has already decided -- this is an
  // OS-level constraint, not a laufey one). The callback fires on the UI
  // thread. NULL function pointer is equivalent to a stub that reports
  // UNSUPPORTED.
  void (*request_permission)(void* backend_data, int kind,
                             laufey_permission_callback_fn cb, void* user_data);
};

#ifdef __cplusplus
}
#endif

#endif  // LAUFEY_H
