# Architecture

## Overview

laufey separates browser engines (**backends**) from application logic
(**runtimes**). Backends are native executables; runtimes are shared libraries
(`.dylib`/`.so`/`.dll`) loaded at startup. They communicate through a C ABI
defined in `capi/include/laufey.h`.

```
┌──────────────────┐         ┌───────────────────┐
│  Backend (exe)   │ ──C ABI──▶  Runtime (dylib) │
│  CEF / WebView / ◀─────────│  User app logic   │
│  Winit           │         │  (links laufey capi) │
└──────────────────┘         └───────────────────┘
```

### Backends

| Directory  | Engine                                            | Language           | Window ownership     |
| ---------- | ------------------------------------------------- | ------------------ | -------------------- |
| `cef/`     | Chromium Embedded Framework                       | C++                | CEF Views (internal) |
| `webview/` | System webview (WKWebView / WebView2 / WebKitGTK) | C++ (per-platform) | Created directly     |
| `winit/`   | None (winit only, no web content)                 | Rust (winit)       | Created directly     |

An experimental Servo backend lives on the
[`servo`](https://github.com/littledivy/wef/tree/servo) branch.

### Runtime (capi)

`capi/` provides the Rust crate that runtimes link against. It wraps the raw C
function pointers from `laufey_backend_api_t` into safe Rust types (`Window`,
`Value`, `JsCall`, `KeyboardEvent`, `MouseClickEvent`, etc.).

## Key Patterns

### The C ABI contract (`laufey_backend_api_t`)

The central interface is a struct of function pointers (`capi/include/laufey.h`).
The backend fills this struct and passes a pointer to `laufey_runtime_init()`. The
runtime stores it for the process lifetime. Every capability (navigation, JS
execution, event handlers, window management) is a nullable function pointer in
this struct.

**Adding a new API**: add the field to `laufey_backend_api_t` in
`capi/include/laufey.h`, then implement it in every backend. Both C++ backends
include the canonical header from `capi/include/` via the `LAUFEY_INCLUDE_DIR`
CMake variable — there is no second copy to sync.

### Callback registration (event handlers)

All event handlers follow the same pattern:

1. Define a C callback type (`laufey_keyboard_event_fn`, `laufey_mouse_click_fn`)
2. Add a `set_*_handler(backend_data, callback, user_data)` function pointer to
   the API struct
3. Backend stores the callback+user_data behind a mutex
4. Backend dispatches from its native event handler, passing the user_data back

On the runtime side (`capi/src/lib.rs`), an `unsafe extern "C"` trampoline
converts C types to Rust types and forwards to a stored `Box<dyn Fn(Event)>`.

**Events are non-consuming** -- handlers always return the event to the
underlying engine. This is an interception model, not a consumption model.

### C++ backend code sharing (`backend-common`)

The CEF and webview backends both link `backend-common/`, a CMake static library
that holds platform implementations of APIs the two backends would otherwise
duplicate. Each backend `add_subdirectory`s it and links `laufey_backend_common`
from its platform branch.

The bridge is intentionally minimal — common code never touches the
backend-specific `laufey_value_t` types. Each backend pre-parses `laufey_value_t` into
plain C++ structs (`laufey_common::NotificationOptions`, etc.) before calling into
common functions. Header: `backend-common/include/laufey_backend_common.h`.

Currently shared:

| Area                               | macOS                                                                                                            | Windows                                                        | Linux                                                                      |
| ---------------------------------- | ---------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------- | -------------------------------------------------------------------------- |
| **Notifications**                  | `notifications_mac.mm` (UN)                                                                                      | `notifications_win.cc` (NIIF)                                  | `notifications_linux.cc` (notify-send)                                     |
| **Dialogs**                        | `dialog_mac.mm` (NSAlert)                                                                                        | `dialog_win.cc` (MessageBoxW + PowerShell prompt)              | `dialog_linux.cc` (gtk_message_dialog)                                     |
| **Permissions**                    | `permissions_mac.mm` (UN auth)                                                                                   | `permissions_stub.cc` (always granted)                         | `permissions_stub.cc` (always granted)                                     |
| **Dock**                           | `dock_mac.mm` (badge / bounce / visible / dock menu storage / reopen handler)                                    | per-backend (FlashWindowEx) + `title_badge.cc` for the badge   | per-backend (gtk_window_set_urgency_hint) + `title_badge.cc` for the badge |
| **Key mapping**                    | `keymap_mac.mm` (NSEvent → W3C)                                                                                  | `keymap_vk.cc` (VK → W3C; CEF uses on every platform)          | `keymap_gdk.cc` (GDK → W3C)                                                |
| **App / context menu**             | `menu_mac.mm` (NSMenu)                                                                                           | `capi/include/win32_menu.h` (HMENU + SetMenu / TrackPopupMenu) | `menu_linux.cc` (GtkMenu / GtkMenuBar)                                     |
| **Tray icons**                     | `tray_mac.mm` (NSStatusItem)                                                                                     | `tray_win.cc` (Shell_NotifyIcon + WIC + HMENU)                 | `tray_linux.cc` (libappindicator + g_idle_add)                             |
| **Option parsing**                 | `parse_options.cc` (compiled on every platform; bridges `laufey_value_t` → plain structs)                           |                                                                |                                                                            |
| **Title-prefix badge bookkeeping** | `title_badge.cc` (`ApplyTitlePrefixBadge` — used by CEF Win+Linux and webview Win+Linux for Dock-badge fallback) |                                                                |                                                                            |

Notes:

- `win32_menu.h` is the older shared header-only library for Windows menu
  construction; it predates `backend-common` and stays as-is. The two patterns
  coexist — both backends use `win32_menu` for Windows app/context menus, and
  `backend-common` for the rest.
- The dock fallback on Windows/Linux still iterates per-window state inside each
  backend (because the native title-get/set APIs differ — `SetWindowTextW(HWND)`
  vs `gtk_window_set_title(GtkWindow*)` vs CEF's `CefWindow::SetTitle`), but the
  saved-titles bookkeeping and `"(badge) " + title` string concatenation are
  unified in `laufey_common::ApplyTitlePrefixBadge`.
- FlashWindow (Windows) and `gtk_window_set_urgency_hint` (Linux) for
  `bounce_dock` remain per-backend — each is ~5 LOC and the native call differs
  enough that an abstraction would cost more than it saves.

To add a new shared API: declare it in `laufey_backend_common.h`, add the
implementation file(s) to `backend-common/CMakeLists.txt`, then call it from
each backend's existing API trampoline.

### Winit backend code sharing (`backend-winit-common`)

The `winit/` backend uses winit for windowing; the Servo backend on the `servo`
branch shares this same code. Shared code lives in
`backend-winit-common/src/lib.rs`:

- **`BackendAccess` trait**: each backend implements this to provide access to
  its `CommonState`, event loop proxy, and event type mapping.
- **`define_common_backend_fns!` macro**: generates the `unsafe extern "C"`
  functions for all common operations (title, size, position, visibility, event
  handlers, etc.).
- **`fill_common_api!` macro**: wires those generated functions into a
  `LaufeyBackendApi` struct.
- **`CommonState`**: holds pending window mutations (`Mutex<Option<T>>`) and
  event handler callbacks.
- **`handle_common_event()`**: processes `CommonEvent` variants against a winit
  `Window`.

To add a new winit-based API: add the pending state to `CommonState`, the
function to `define_common_backend_fns!`, the assignment to `fill_common_api!`,
and the dispatch to `handle_common_event()`. The winit backend picks it up
automatically (and the Servo branch, if rebased).

### Pending state pattern (async window ops)

Backend API functions are called from the runtime thread, but window operations
must happen on the UI thread. The pattern is:

1. Store the desired value in a `Mutex<Option<T>>` on `CommonState`
2. Send an event via the winit `EventLoopProxy`
3. On the UI thread, take the pending value and apply it to the window

C++ backends use platform-specific dispatch instead (`dispatch_async` on macOS,
`PostMessage` on Windows, `g_idle_add` on Linux).

### CEF: no native mouse/input handlers

CEF provides `CefKeyboardHandler` for keyboard events but has **no equivalent
`CefMouseHandler`**. This is because CEF Views creates and owns the native
window internally -- the embedder has no direct access to the native event loop.

**Workaround**: platform-specific native event monitors that hook into the OS
event system:

| Platform | Technique                                                                                      | File                           |
| -------- | ---------------------------------------------------------------------------------------------- | ------------------------------ |
| macOS    | `[NSEvent addLocalMonitorForEventsMatchingMask:]`                                              | `cef/src/main_mac.mm`          |
| Windows  | `WM_*BUTTON*` messages in `WindowProc` (via `CefWindow::GetWindowHandle()` + subclassing)      | `cef/src/main_win.cc` (TODO)   |
| Linux    | GTK `button-press-event` / `button-release-event` signals (via `CefWindow::GetWindowHandle()`) | `cef/src/main_linux.cc` (TODO) |

The monitor functions (`InstallNativeMouseMonitor()` /
`RemoveNativeMouseMonitor()`) are declared in `cef/src/runtime_loader.h` and
called from `LaufeyWindowDelegate::OnWindowCreated` / `OnWindowDestroyed` in
`cef/src/app.cc`. This is the same approach Electron uses -- Electron creates
native windows directly (bypassing CEF Views), but since we use CEF Views, we
instead install post-hoc monitors on the window CEF creates.

### Webview backends: direct native window access

Unlike CEF, the webview backends create their own native windows, so event
interception is straightforward:

| Platform                       | Keyboard                                                       | Mouse                                                     |
| ------------------------------ | -------------------------------------------------------------- | --------------------------------------------------------- |
| macOS (`webview_macos.mm`)     | `NSEvent addLocalMonitorForEventsMatchingMask:` for key events | Same mechanism for mouse events                           |
| Windows (`webview_windows.cc`) | `WM_KEYDOWN` / `WM_KEYUP` in `WindowProc`                      | `WM_*BUTTON*` in `WindowProc`                             |
| Linux (`webview_linux.cc`)     | GTK `key-press-event` / `key-release-event` signals            | GTK `button-press-event` / `button-release-event` signals |

### W3C UI Events key mapping

Keyboard events expose `key` (logical, e.g. `"a"`, `"Enter"`) and `code`
(physical, e.g. `"KeyA"`, `"Enter"`) following the W3C UI Events specification.
Each platform has its own mapping:

- **winit backends**: `winit_key_to_string()` / `winit_code_to_string()` in
  `backend-winit-common`
- **CEF**: `CefKeyCodeToString()` / `CefKeyCodeToCode()` in `cef/src/app.cc`
  (maps Windows virtual key codes)
- **webview macOS**: `NSEventKeyToString()` / `NSEventKeyCodeToCode()` (maps
  macOS key codes)
- **webview Windows**: `VirtualKeyToKey()` / `VirtualKeyToCode()` (maps Win32 VK
  codes)
- **webview Linux**: `GdkKeyvalToKey()` / `GdkKeycodeToCode()` (maps GDK keyvals
  and evdev hardware keycodes)

### Mouse button mapping

Mouse buttons are normalized to `LAUFEY_MOUSE_BUTTON_*` constants.
Platform-specific mappings:

- **NSEvent** `buttonNumber`: 0=left, 1=right, 2+=other (detect via event type
  mask)
- **Win32**: separate `WM_*BUTTON*` messages per button; `XBUTTON1`/`XBUTTON2`
  for back/forward
- **GDK**: `event->button`: 1=left, 2=middle, 3=right, 8=back, 9=forward

### Value marshalling

The laufey API has a rich value type (`laufey_value_t`) for JS interop. Backends own
the value representation:

- **CEF**: wraps `CefValue` / `CefListValue` directly
- **Webview**: uses a custom `Value` class with JSON serialization for JS
  communication
- **Winit backends**: stub implementations (no JS engine)

The runtime crate (`capi/src/lib.rs`) wraps these into a Rust `Value` enum via
the function pointer API, completely opaque to the value's backend
representation.

### Modifier flags

All platforms normalize keyboard modifiers to a shared bitmask:

```
LAUFEY_MOD_SHIFT   = 1 << 0
LAUFEY_MOD_CONTROL = 1 << 1
LAUFEY_MOD_ALT     = 1 << 2
LAUFEY_MOD_META    = 1 << 3
```

Each platform maps from its native representation (`NSEventModifierFlags`,
`GetKeyState()`, `GdkModifierType`, `CefEventFlags`, winit `ModifiersState`).
