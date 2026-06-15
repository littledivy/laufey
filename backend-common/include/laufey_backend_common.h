// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Public header for backend-common. Each backend (CEF, webview) pre-parses
// its backend-specific laufey_value_t into the plain structs declared here,
// then calls into the shared per-platform implementations.

#ifndef LAUFEY_BACKEND_COMMON_H_
#define LAUFEY_BACKEND_COMMON_H_

#include <laufey.h>

#include <cstdint>
#include <string>
#include <vector>

// Forward-declare AppKit types for Obj-C++ callers. Must live outside
// any namespace because `@class` is only valid at global scope.
#if defined(__APPLE__) && defined(__OBJC__)
@class NSMenu;
#endif

namespace laufey_common {

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

struct NotificationAction {
  std::string id;
  std::string title;
};

struct NotificationOptions {
  std::string title;
  std::string body;
  // If non-empty, replaces any existing notification with the same tag —
  // matches Web Notifications "tag" semantics.
  std::string tag;
  bool silent = false;
  bool require_interaction = false;
  std::vector<NotificationAction> actions;
  // PNG bytes; used by the Windows balloon. Ignored on other platforms
  // until macOS gets image-attachment support.
  std::vector<uint8_t> icon_png;
};

// Parses a laufey_value_t dict into a plain NotificationOptions. Takes
// ownership of `options` (calls `api->value_free` before returning).
// Returns a default-initialized NotificationOptions if `options` is null
// or not a dict.
NotificationOptions ParseNotificationOptions(laufey_value_t* options,
                                             const laufey_backend_api_t* api);

#ifdef __APPLE__
// UNUserNotificationCenter-backed (10.14+). Requires the process to run
// inside a bundled .app with a CFBundleIdentifier; without one, delivery
// fails and CLOSED is fired synthetically. Action buttons supported via
// UNNotificationCategory.
uint32_t ShowNotificationMac(const NotificationOptions& opts,
                             laufey_notification_event_fn on_event,
                             void* user_data);
void CloseNotificationMac(uint32_t notification_id);
#endif

#ifdef __linux__
// Shells out to `notify-send`. Fire-and-forget — only synthesizes
// SHOWN (synchronously after spawn) and CLOSED (from the matching
// CloseNotificationLinux call). Click / action events are not surfaced
// because notify-send has no IPC channel back.
uint32_t ShowNotificationLinux(const NotificationOptions& opts,
                               laufey_notification_event_fn on_event,
                               void* user_data);
void CloseNotificationLinux(uint32_t notification_id);
#endif

#ifdef _WIN32
// Shell_NotifyIcon balloon notification. On Windows 10/11 the shell
// renders the balloon as a system toast (with grouping, Action Center
// entry, etc.). Click → CLICKED; dismiss / timeout → CLOSED. Action
// buttons aren't supported by NIIF balloons — `opts.actions` is ignored
// on Windows.
uint32_t ShowNotificationWin(const NotificationOptions& opts,
                             laufey_notification_event_fn on_event,
                             void* user_data);
void CloseNotificationWin(uint32_t notification_id);

// ---------------------------------------------------------------------------
// Tray / status-bar icon (Windows, Shell_NotifyIcon)
// ---------------------------------------------------------------------------
//
// Hidden message-only window + Shell_NotifyIcon + WIC PNG decode.
// Light/dark icons are resolved against the AppsUseLightTheme registry
// value and re-applied automatically on WM_SETTINGCHANGE
// (ImmersiveColorSet) notifications.
//
// All functions are synchronous — callers that need to dispatch to a
// UI thread (e.g. CEF's TID_UI) should marshal before calling.
//
// CreateTrayIconWin returns the new id immediately (allocated atomically).
// FinalizeTrayIconWin must then be called on the thread that pumps the
// tray's message loop (CEF: TID_UI; webview: WebView2 UI thread) to do
// the Shell_NotifyIcon setup. Splitting these lets the trampoline
// return the id synchronously even when it has to marshal the actual
// Shell_NotifyIcon work onto the UI thread.

uint32_t CreateTrayIconWin();
void FinalizeTrayIconWin(uint32_t tray_id);
void DestroyTrayIconWin(uint32_t tray_id);
void SetTrayIconWin(uint32_t tray_id, const void* png_bytes, size_t len);
void SetTrayIconDarkWin(uint32_t tray_id, const void* png_bytes, size_t len);
void SetTrayTooltipWin(uint32_t tray_id, const char* tooltip_or_null);
void SetTrayMenuWin(uint32_t tray_id, laufey_value_t* menu_template,
                     const laufey_backend_api_t* api,
                     laufey_menu_click_fn on_click, void* on_click_data);
void SetTrayClickHandlerWin(uint32_t tray_id, laufey_tray_click_fn handler,
                              void* user_data);
void SetTrayDoubleClickHandlerWin(uint32_t tray_id, laufey_tray_click_fn handler,
                                    void* user_data);
// Writes the tray icon's screen rectangle (top-left origin, DIP) into the
// out-params (any may be NULL) and returns true, or false if the id is
// unknown or the shell can't report the position.
bool GetTrayIconBoundsWin(uint32_t tray_id, int* x, int* y, int* width,
                          int* height);
#endif

// ---------------------------------------------------------------------------
// Title-prefix badge (Windows/Linux dock fallback)
// ---------------------------------------------------------------------------
//
// macOS has NSDockTile.setBadgeLabel; Windows and Linux don't. The
// Slack/Discord/Telegram convention is to prepend "(N) " to each
// window's title. This helper centralizes the saved-titles bookkeeping
// so each backend just iterates its windows, reads the current title
// via its native API, calls this to compute what to set, then writes
// the result back via its native API.
//
// `window_key` is any backend-chosen unique-per-window value (HWND
// cast, GtkWindow* cast, internal id — anything). `badge` is the new
// badge text; empty means clear. Returns the title to apply to the
// window.
//
//   - badge non-empty, first time we see this window: save
//     `current_title`, return "(badge) current_title".
//   - badge non-empty, window already has a saved title: return
//     "(badge) saved_title" (replaces any prior badge prefix).
//   - badge empty, window has a saved title: forget it, return the
//     saved title.
//   - badge empty, no saved title: return `current_title` unchanged.
std::string ApplyTitlePrefixBadge(uint64_t window_key,
                                   const std::string& current_title,
                                   const std::string& badge);

// Forget the saved title for a window (call when a window closes so
// the map doesn't grow unbounded).
void ForgetTitlePrefixBadge(uint64_t window_key);

// ---------------------------------------------------------------------------
// Dialogs (alert / confirm / prompt)
// ---------------------------------------------------------------------------
//
// `dialog_type` is one of LAUFEY_DIALOG_* from laufey.h. All implementations
// block until the user dismisses the dialog. The native modal pumps OS
// events so other laufey windows keep responding.
//
// Returns 1 if OK / confirmed, 0 otherwise. For LAUFEY_DIALOG_PROMPT, on a
// confirmed result `*out_input_value` is set to a strdup'd UTF-8 string
// the caller must free with `free()`.

#ifdef __APPLE__
int ShowDialogMac(int dialog_type, const std::string& title,
                  const std::string& message,
                  const std::string& default_value, char** out_input_value);
#endif

#ifdef _WIN32
int ShowDialogWin(int dialog_type, const std::string& title,
                  const std::string& message,
                  const std::string& default_value, char** out_input_value);
#endif

#ifdef __linux__
int ShowDialogLinux(int dialog_type, const std::string& title,
                    const std::string& message,
                    const std::string& default_value, char** out_input_value);
#endif

// ---------------------------------------------------------------------------
// Permissions / runtime authorization
// ---------------------------------------------------------------------------
//
// `kind` is one of LAUFEY_PERMISSION_* from laufey.h. Results are reported via
// the callback (status one of LAUFEY_PERMISSION_STATUS_*).

#ifdef __APPLE__
// UNUserNotificationCenter-backed for LAUFEY_PERMISSION_NOTIFICATIONS.
// Reports UNSUPPORTED if the process isn't running inside a bundled
// .app, or if `kind` is anything other than notifications.
void QueryPermissionMac(int kind, laufey_permission_callback_fn cb,
                        void* user_data);
void RequestPermissionMac(int kind, laufey_permission_callback_fn cb,
                          void* user_data);
#endif

// Windows + Linux stub: notify-send (Linux) and Shell_NotifyIcon balloons
// (Windows) have no permission model, so we report GRANTED synchronously
// for LAUFEY_PERMISSION_NOTIFICATIONS and UNSUPPORTED for anything else.
void QueryPermissionStub(int kind, laufey_permission_callback_fn cb,
                         void* user_data);
void RequestPermissionStub(int kind, laufey_permission_callback_fn cb,
                           void* user_data);

// ---------------------------------------------------------------------------
// Keyboard event key/code mapping (W3C UI Events)
// ---------------------------------------------------------------------------
//
// `key` is the logical value (e.g. "a", "Enter", " "). `code` is the
// physical position (e.g. "KeyA", "Enter", "Space").
//
// CEF normalizes keyboard events to Windows VK codes on every platform,
// so the VK-based mappings below cover CEF Mac / Win / Linux as well as
// the webview Windows backend. The webview macOS and Linux backends use
// platform-native event types and have their own helpers.

// Windows VK → "key" (logical).
//   `character`: the Unicode codepoint typed (0 if not a char event /
//                unknown). When set to a printable ASCII byte, returned
//                directly (matches CEF behavior).
//   `shift_held` / `caps_on`: Windows shift/caps state for case
//                determination when `character` is 0 (matches webview
//                Windows behavior). Pass false on non-Windows callers.
std::string VkToKey(int vk, uint32_t character, bool shift_held,
                    bool caps_on);

// Windows VK → "code" (physical).
//   `is_extended`: WM_KEYDOWN lParam bit 24, distinguishes NumpadEnter
//                  vs Enter, ControlRight/Left, AltRight/Left.
//   `scancode`: WM_KEYDOWN lParam bits 16-23. Used on Windows only via
//               MapVirtualKey to distinguish ShiftLeft vs ShiftRight;
//               0 means default to ShiftLeft (CEF path).
std::string VkToCode(int vk, bool is_extended, uint32_t scancode);

#ifdef __APPLE__
// Cocoa NSEvent key code → W3C "key". `event` is an `NSEvent*` from
// keyboard NSEvents; declared as `void*` so non-Obj-C++ callers can also
// pass through without an Obj-C runtime dependency.
std::string NSEventKeyToKey(void* event_nsevent);
std::string NSEventKeyToCode(unsigned short key_code);

// ---------------------------------------------------------------------------
// Dock / taskbar (macOS app-scoped operations)
// ---------------------------------------------------------------------------
//
// macOS-only dock primitives. The Windows/Linux fallbacks (title-prefix
// badge, FlashWindowEx, GTK urgency hint) need per-window iteration and
// stay in each backend.

// Sets the application dock badge. nullptr or "" clears it.
void SetDockBadgeMac(const char* badge_or_null);

// `type` is one of LAUFEY_DOCK_BOUNCE_INFORMATIONAL / LAUFEY_DOCK_BOUNCE_CRITICAL.
void BounceDockMac(int type);

// true → NSApplicationActivationPolicyRegular (dock + menu bar)
// false → NSApplicationActivationPolicyAccessory (background, no dock)
void SetDockVisibleMac(bool visible);

// Stores the dock menu set by Backend_SetDockMenu_Mac /
// WKWebViewBackend::SetDockMenu. Both backends' AppDelegate read this
// value in applicationDockMenu:. Pass nil to clear.
#ifdef __OBJC__
void SetDockMenuMac(NSMenu* menu);
NSMenu* GetDockMenuMac();
#endif

// Stores the dock-reopen handler set by Backend_SetDockReopenHandler_Mac /
// WKWebViewBackend::SetDockReopenHandler. AppDelegate calls
// FireDockReopenMac() from applicationShouldHandleReopen:hasVisibleWindows:.
void SetDockReopenHandlerMac(laufey_dock_reopen_fn handler, void* user_data);
void FireDockReopenMac(bool has_visible_windows);

// ---------------------------------------------------------------------------
// NSMenu builder (macOS)
// ---------------------------------------------------------------------------
//
// Walks a laufey_value_t menu template and produces an NSMenu. Click events
// on non-role items invoke on_click(on_click_data, window_id, item_id).
// Role items (copy/paste/cut/quit/minimize/...) wire to First
// Responder selectors and don't reach on_click.
//
// Returns the menu through an opaque void* on non-Obj-C callers and as
// NSMenu* on Obj-C++ callers — header double-declared so the typed form
// flows through .mm files without forcing AppKit on plain .cc.
// (NSMenu is forward-declared at the top of this header.)
#ifdef __OBJC__
NSMenu* BuildNSMenuFromValue(laufey_value_t* val, const laufey_backend_api_t* api,
                             laufey_menu_click_fn on_click, void* on_click_data,
                             uint32_t window_id);
#else
void* BuildNSMenuFromValue(laufey_value_t* val, const laufey_backend_api_t* api,
                           laufey_menu_click_fn on_click, void* on_click_data,
                           uint32_t window_id);
#endif

// ---------------------------------------------------------------------------
// Tray / status-bar icon (macOS, NSStatusItem)
// ---------------------------------------------------------------------------

uint32_t CreateTrayIconMac();
void DestroyTrayIconMac(uint32_t tray_id);
void SetTrayIconMac(uint32_t tray_id, const void* png_bytes, size_t len);
void SetTrayIconDarkMac(uint32_t tray_id, const void* png_bytes, size_t len);
void SetTrayTooltipMac(uint32_t tray_id, const char* tooltip_or_null);
void SetTrayMenuMac(uint32_t tray_id, laufey_value_t* menu_template,
                     const laufey_backend_api_t* api,
                     laufey_menu_click_fn on_click, void* on_click_data);
void SetTrayClickHandlerMac(uint32_t tray_id, laufey_tray_click_fn handler,
                             void* user_data);
void SetTrayDoubleClickHandlerMac(uint32_t tray_id,
                                   laufey_tray_click_fn handler, void* user_data);
// Writes the tray icon's screen rectangle (top-left origin, points/DIP) into
// the out-params (any may be NULL) and returns true. Returns false if the id
// is unknown or the icon has no on-screen button yet.
bool GetTrayIconBoundsMac(uint32_t tray_id, int* x, int* y, int* width,
                          int* height);
#endif

#ifdef __linux__
// GDK keyval → W3C "key". `keyval` is a GDK keyval (gdk_event_key.keyval).
// `evdev_keycode` is GDK's evdev hardware keycode (gdk_event_key.hardware_keycode),
// used for "code" mapping.
std::string GdkKeyvalToKey(unsigned int keyval);
std::string GdkKeycodeToCode(unsigned int evdev_keycode);

// Build a GtkMenu (or GtkMenuBar when `is_menu_bar` is true) from a
// laufey_value_t menu template. Non-role items trigger
// on_click(on_click_data, window_id, item_id). Role items map to
// labels and forward the role as the item id. Returns NULL if `val`
// is null or not a list.
//
// Returned as void* on non-GTK callers and as GtkWidget* on GTK-aware
// translation units (gtk.h must be included before this header for the
// typed form).
#ifdef __GTK_H__
GtkWidget* BuildGtkMenuFromValue(laufey_value_t* val,
                                  const laufey_backend_api_t* api,
                                  uint32_t window_id,
                                  laufey_menu_click_fn on_click,
                                  void* on_click_data, bool is_menu_bar);
#else
void* BuildGtkMenuFromValue(laufey_value_t* val, const laufey_backend_api_t* api,
                            uint32_t window_id, laufey_menu_click_fn on_click,
                            void* on_click_data, bool is_menu_bar);
#endif

// ---------------------------------------------------------------------------
// Tray / status-bar icon (Linux, libappindicator)
// ---------------------------------------------------------------------------
//
// All functions must be called on the GTK main thread; backends with
// off-main-thread callers should marshal first (CEF uses CefPostTask).
// When libappindicator is not available (LAUFEY_HAVE_APPINDICATOR not
// defined), CreateTrayIconLinux returns 0 and all other calls no-op.
//
// AppIndicator has no left-click event — a click anywhere pops the
// indicator's menu — so SetTrayClickHandlerLinux and
// SetTrayDoubleClickHandlerLinux accept handlers for API symmetry but
// don't surface clicks back.

uint32_t CreateTrayIconLinux();
void DestroyTrayIconLinux(uint32_t tray_id);
void SetTrayIconLinux(uint32_t tray_id, const void* png_bytes, size_t len);
void SetTrayIconDarkLinux(uint32_t tray_id, const void* png_bytes, size_t len);
void SetTrayTooltipLinux(uint32_t tray_id, const char* tooltip_or_null);
void SetTrayMenuLinux(uint32_t tray_id, laufey_value_t* menu_template,
                       const laufey_backend_api_t* api,
                       laufey_menu_click_fn on_click, void* on_click_data);
void SetTrayClickHandlerLinux(uint32_t tray_id, laufey_tray_click_fn handler,
                                void* user_data);
void SetTrayDoubleClickHandlerLinux(uint32_t tray_id,
                                      laufey_tray_click_fn handler,
                                      void* user_data);
#endif

}  // namespace laufey_common

#endif  // LAUFEY_BACKEND_COMMON_H_
