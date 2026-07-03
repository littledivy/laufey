# End-to-end testing strategy

This document describes how laufey can automatically test its **native chrome
and windowing surface** — menus, tray icons, notifications, dialogs, clipboard,
window geometry/state, input events, and the JavaScript bridge — across **every
backend** (CEF, WebView, Winit) on **Linux, macOS, and Windows**.

It is a design/contributor document, not a user reference. It records the
verification techniques, the empirical findings that back them, a full coverage
matrix over the C ABI surface, and a phased rollout.

> Status: implemented for all three backends. `examples/native_e2e` (the Layer-0
> battery + menu/tray click round-trips via the `test_click_menu_item` hook) and
> `examples/native_e2e_driver` (the Linux D-Bus observer) run green under
> **Winit, WebView, and CEF** (verified on macOS); a `native-e2e` CI job
> (`backend × os` matrix) drives the same runtime under each backend on
> Linux/macOS/Windows via `scripts/native-e2e-run.sh`. The
> `test_click_menu_item` hook (`§8`) is implemented for the Winit backend (Rust)
> and the C++ `backend-common` shared by CEF/WebView. The macOS
> self-accessibility approach (`§7.2`) is verified standalone but not yet
> embedded (it needs the backend's main thread — see `§8`). Implementing the
> hook exposed and fixed a pre-existing self-deadlock in the Winit menu-callback
> registration.

---

## 1. Goals and non-goals

**Goals**

- Automatically verify, in CI, that laufey's native surface actually works — not
  just that the C ABI accepts a call, but that the OS registered/rendered the
  widget and that user-driven callbacks round-trip back to app code.
- Cover **all backends**, because native chrome has two independent
  implementations (C++ `backend-common` for CEF/WebView; Rust
  `backend-winit-common` for Winit) and four web engines.
- Maximize the fraction of the surface that runs as a **blocking PR gate** on
  stock GitHub-hosted runners (no self-hosted infra, no manual permission
  setup).

**Non-goals**

- Pixel-perfect visual regression. Screenshot diffing is high-flake and
  low-diagnostic; it is not part of the gate. (A last-resort visual smoke on
  notifications is acceptable nightly.)
- Simulating real hardware user input where a cheaper deterministic path exists.
  We drive callbacks through the same dispatch code a real event would, not by
  injecting OS-level mouse/keyboard events, except where noted.

---

## 2. Why this is hard (and where it isn't)

The existing harness, `examples/cef_e2e`, exercises the CEF web bridge
(bindings, `execute_js`, navigation) but explicitly punts on everything that
lives **outside the webview**:

> Doesn't drive dialogs (alert/confirm/prompt) because those need either real OS
> input or a backend-side test stub — neither exists today.

Native chrome is OS-owned and has no return value to assert against — a menu or
a tray icon is "somewhere on the screen", owned by the window server, the shell,
or another process. That is the genuinely hard part.

The key insight of this document is that the difficulty is **wildly
asymmetric**, and that most of the surface is _not_ actually hard:

- A large fraction of the API has **direct readback** (`set_window_size` →
  `get_window_size`, `write_clipboard_text` → `read_clipboard_text`). This is
  the cheapest, most reliable category and needs none of the machinery below.
- The truly OS-owned chrome (tray/menu/notifications) turns out to be
  **introspectable without pixels** on every platform, via a different mechanism
  each: D-Bus on Linux, the Accessibility API on macOS, UI Automation on
  Windows.
- Only genuinely modal/outward-facing surfaces (dialogs, devtools, external
  browser launch) resist automation and are relegated to nightly.

---

## 3. Backend and surface landscape

### 3.1 Backends are not interchangeable

Runtimes are backend-agnostic cdylibs; a backend loads one via
`--runtime <path>` or `LAUFEY_RUNTIME_PATH`. That means **one test runtime can
be driven by every backend binary**. But the backends differ in what they
implement:

| Backend | Web engine                                 | Native-chrome impl                                    | OSes      |
| ------- | ------------------------------------------ | ----------------------------------------------------- | --------- |
| CEF     | Chromium (multiprocess)                    | C++ `backend-common`                                  | L/mac/win |
| WebView | WKWebView / WebView2 / WebKitGTK (per-OS!) | C++ `backend-common`                                  | L/mac/win |
| Winit   | **none**                                   | **Rust `backend-winit-common`** (`tray-icon`, `muda`) | L/mac/win |
| Servo   | Servo (branch)                             | —                                                     | deferred  |
| iOS     | WKWebView (iOS)                            | subset                                                | deferred  |

Consequences:

- There are **two native-chrome codebases**. Running the tray/menu/clipboard
  suite under _both_ a `backend-common` backend and Winit validates two separate
  implementations and catches divergence. This is the real payoff of "test all
  backends" — not redundancy.
- WebView is **three different web engines** by OS, so its web-layer tests
  genuinely differ per platform.
- Winit has **no web engine**: `navigate`, `execute_js`,
  `set_page_load_handler`, and `register_scheme_handler` are `None`. Winit
  leaves ~68 API fields unimplemented in total. Capability probing (`§6`) is
  therefore mandatory.

### 3.2 The full C ABI surface

Every function pointer in `laufey_backend_api_t` is a capability to cover. They
group as:

- **Windowing / state**: create, size, position, resizable, always-on-top,
  visibility (show/hide), opacity, window flags (frameless, transparent,
  transparent-titlebar, hidden, no-activate), handles.
- **Window & input events**: resize, move, focus, close-requested, mouse
  click/move, wheel, cursor enter/leave, keyboard.
- **Web bridge** (CEF/WebView only): navigate, execute_js, JS bindings /
  namespace / callbacks, page-load, custom scheme handlers, devtools.
- **Native chrome**: application menu, context menu, tray (icon/tooltip/menu/
  click/double-click/dark-icon/bounds), dock/taskbar (badge/bounce/menu/
  visibility/reopen), notifications, dialogs (alert/confirm/prompt/file).
- **System integration**: clipboard read/write, permissions (query/request).

---

## 4. Verification techniques

Ordered cheapest → hardest. Each capability maps to one (or a combination).

### A. Direct state readback — _cheapest, most reliable, all-platform gate_

Set a property, read it back from the real OS object via the backend's own
getter. No display-server introspection needed.

- Window: `set_window_size`/`get_window_size`,
  `set_window_position`/`get_window_position`, `set_resizable`/`is_resizable`,
  `set_always_on_top`/`is_always_on_top`, `show`/`hide`/`is_visible`,
  `set_window_opacity`/`get_window_opacity`.
- Clipboard: `write_clipboard_text` → `read_clipboard_text` (round-trips through
  the real OS clipboard).
- Handles: `get_window_handle` / `get_display_handle` / `get_window_handle_type`
  (assert non-null and correct type enum per platform).
- Permissions: `query_permission`.
- Tray geometry: `get_tray_icon_bounds`.

### B. Event injection → callback round-trip

Drive a callback by calling the corresponding setter and asserting the handler
fires with the right arguments. No OS input required.

- `set_resize_handler` ← `set_window_size`; `set_move_handler` ←
  `set_window_position`; `set_focused_handler` ← `focus`/`show`;
  `set_close_requested_handler` ← programmatic close.
- Menu / tray clicks via the platform's non-modal invoke primitive (`§5`).
- `set_page_load_handler` ← `navigate` (CEF/WebView).

### C. Custom scheme / IPC

Navigate to a custom-scheme URL and assert the registered handler served the
expected bytes. Same shape as the existing binding round-trip.

### D. OS-observer introspection — _for chrome with no getter_

The three platform mechanisms (`§7`): Linux D-Bus, macOS self-Accessibility,
Windows UI Automation. Covers tray, menu structure as the OS sees it,
notifications, window title, decorations, dock.

### E. Raw input events

Mouse/keyboard/wheel/cursor handlers. Best driven by a **backend test-inject
hook** that posts a synthetic event down the same path (deterministic), rather
than OS-level input injection (flaky). Part of the Layer-0 hook (`§8`).

### F. Modal / outward-facing — _nightly_

`show_dialog` (alert/confirm/prompt/file), `request_permission`,
`open_devtools`, `bounce_dock`, external-browser open. Modal dialogs block the
main thread (see the macOS modal trap in `§7.2`), so they need either a backend
auto-answer stub or an external AX/UIA driver on a separate thread.

---

## 5. Per-platform non-modal click primitives

For Layer-0 click round-trips we invoke an item through the same dispatch a real
click uses, **without** entering a modal tracking loop:

| Platform | Primitive                                    | Fires                                                     |
| -------- | -------------------------------------------- | --------------------------------------------------------- |
| macOS    | `[NSMenu performActionForItem:idx]`          | `LaufeyCommonMenuTarget menuItemClicked:` (`menu_mac.mm`) |
| Linux    | `gtk_menu_item_activate(item)`               | `OnGtkMenuItemActivate` (`menu_linux.cc`)                 |
| Windows  | post `WM_COMMAND` with the item's command id | `WM_COMMAND` handler (`tray_win.cc` / menu)               |

macOS note: `AXPress` on a status item _opens the menu modally on the main
thread_ and deadlocks in-process driving — do **not** use AX to invoke;
`performActionForItem` is the correct primitive (verified, `§7.2`).

---

## 6. Capability probing (mandatory)

Because backends implement different subsets, a test must distinguish
"unsupported here" (→ `N/A`) from "supported but broken" (→ `FAIL`). Probe via
documented signals:

- Tray: `create_tray_icon()` returns `0` when unsupported.
- Any capability whose backend fn pointer is `None`: the capi Rust wrapper
  returns `Option::None` / no-ops — the runtime treats that as `N/A`.
- Web capabilities are absent on Winit (`navigate`/`execute_js`/... are `None`)
  → web/JS/scheme/devtools assertions are skipped on Winit.

Each assertion is tagged with the capability it requires; the harness emits
`[e2e] PASS <name>`, `[e2e] FAIL <name>`, or `[e2e] N/A <name>` and exits
non-zero only on `FAIL`. This lets **one runtime binary** be valid across all
backends.

---

## 7. The observers

### 7.1 Linux — D-Bus watcher/observer (written, compiles)

On Linux, both native-chrome implementations expose the tray over the
freedesktop **StatusNotifierItem** spec and the menu over
**`com.canonical.dbusmenu`**:

- CEF/WebView: libayatana-appindicator (`backend-common/src/tray_linux.cc`).
- Winit: the `tray-icon` crate's internal StatusNotifier logic.

So a **single D-Bus driver validates both**. The driver _is_ the desktop shell:
it owns `org.kde.StatusNotifierWatcher` (with
`IsStatusNotifierHostRegistered = true`, without which libappindicator silently
falls back to legacy GtkStatusIcon/XEmbed and never touches D-Bus), owns a stub
`org.freedesktop.Notifications` to capture `Notify` payloads, spawns the backend

- runtime, and then introspects.

Run line (works for **any** backend binary):

```
xvfb-run -a dbus-run-session -- sni-driver <backend-bin> --runtime libnative_e2e.so
```

Key facts baked into the driver:

- libayatana passes the item's **object path** to `RegisterStatusNotifierItem`;
  the bus name is the **message sender** (`#[zbus(header)] hdr → hdr.sender()`),
  not the argument. (KDE-style apps pass a bus name instead — handle both.)
- `com.canonical.dbusmenu.GetLayout(0, -1, [])` returns
  `(u32 revision, (i32 id, a{sv} props, av children))`; children are variants
  wrapping the recursive struct — walk manually.
- A menu click is `AboutToShow(id)` then
  `Event(id, "clicked", <variant "">, <timestamp u32>)`, which fires the app's
  `laufey_menu_click_fn`.
- Icon set via a `/tmp` PNG path surfaces as `IconThemePath`, **not**
  `IconPixmap`; Linux tray tooltip and left-click are no-ops — don't assert
  them.

The driver lives at `examples/native_e2e/driver` (Rust, `zbus` v5 with the
`tokio` feature). Skeleton of the watcher interface:

```rust
#[interface(name = "org.kde.StatusNotifierWatcher")]
impl Watcher {
    async fn register_status_notifier_item(
        &self, service: &str, #[zbus(header)] hdr: Header<'_>,
    ) {
        let sender = hdr.sender().map(|s| s.to_string()).unwrap_or_default();
        let (bus_name, path) = if service.starts_with('/') {
            (sender, service.to_string())            // ayatana / Winit tray-icon
        } else {
            (service.to_string(), "/StatusNotifierItem".to_string()) // KDE-style
        };
        self.tx.send((bus_name, path)).ok();
    }
    #[zbus(property)] async fn is_status_notifier_host_registered(&self) -> bool { true }
    #[zbus(property)] async fn registered_status_notifier_items(&self) -> Vec<String> { /* ... */ }
    #[zbus(property)] async fn protocol_version(&self) -> i32 { 0 }
}
```

### 7.2 macOS — self-Accessibility (empirically verified, no permission)

macOS has no protocol boundary; the chrome is live AppKit objects. The
Accessibility API reaches them, and — verified on macOS 15.5 with
`AXIsProcessTrusted() == false` (i.e. **no TCC permission granted**) — a process
can read its **own** tree:

```
AXUIElementCreateApplication(getpid())
  AXMenuBar        -> AXError 0, full app menu
  AXExtrasMenuBar  -> AXError 0, the app's own NSStatusItem + its menu items
```

So macOS **menu and tray structure are verifiable on hosted CI with zero
permission setup** — no XCUITest, no self-hosted runner, no `TCC.db` surgery
(which is SIP-protected and unavailable on hosted runners anyway). The click
half uses `NSMenu.performActionForItem(at:)` in-process (verified to fire the
target-action, no permission, no modal loop).

This runs **in-process inside the test runtime** (a small AX verifier invoked
after the runtime builds its menus), so it is backend- and engine-agnostic.

Limitation: true _external_ user-input simulation still needs TCC/XCUITest, but
structure + callback wiring — which is what we care about — does not.

### 7.3 Windows — UI Automation

UIA has no permission gate; any process can inspect any UI on the session.

- Menus: `ControlType.Menu`/`MenuItem` via **FlaUI** (.NET/UIA3 — the maintained
  choice; WinAppDriver has had no release since 2020). Enumerate and `Invoke`.
- Tray: icons live in Explorer (`Shell_TrayWnd` → notification-area toolbar +
  `NotifyIconOverflowWindow`). Enumerate by name (= tooltip). Win11 moved most
  icons into the overflow flyout and changed the shell toolbar model, so
  enumeration is brittle → prefer the Layer-0 callback per-PR and treat tray
  _scraping_ as nightly.
- Toasts: UIA over the toast / Action Center — nightly.

---

## 8. Layer 0 — the in-process test hook

A small, test-only extension to `laufey_backend_api_t` that proves laufey's own
plumbing (template parse → native build → callback dispatch → id routing) on
**all** backends cheaply and deterministically — essentially what Electron's own
spec suite does. Appending to the end of the struct is ABI-safe because every
backend `memset`s its api table (unimplemented hooks stay NULL → the capi
wrapper returns `false`/`None` → the runtime reports `N/A`); the API version is
bumped alongside (29 → 30).

**Implemented (API 30):**

```c
// Synthesizes a click on the menu/tray item with id `item_id` by invoking the
// same on_click dispatch a real click uses (looks the handler up by id in the
// backend's shared click store and calls it). Returns true if an item with
// that id was registered and its handler ran. Runs on the caller's thread — no
// main-thread UI access needed — so it works from the worker-thread runtime.
bool (*test_click_menu_item)(void* backend_data, const char* item_id);
```

Implemented for **both** native-chrome codebases, so every backend has it:

- **Winit** (`backend-winit-common`): `dispatch_menu_click_by_id` reuses the
  exact path `poll_menu_events` uses for a real muda `MenuEvent`.
- **CEF + WebView** (C++ `backend-common`): a shared click registry
  (`test_hooks.cc`: `RegisterMenuClick` / `TestClickMenuItem`) that the menu
  builders (`menu_mac.mm`, `menu_linux.cc`, `tray_win.cc`) populate; each
  backend's api table points `test_click_menu_item` at it.

The capi exposes `laufey::test_click_menu_item(item_id)`. All three backends run
the same runtime green (both app-menu and tray-menu click round-trips PASS).
Doing this surfaced and fixed a pre-existing self-deadlock in Winit:
`register_menu_callbacks` held the click-store mutex while recursing into
submenus (std `Mutex` is not reentrant), freezing the main thread on any menu
containing a submenu.

**Not yet added** (future hooks, same append-and-`N/A` pattern):

```c
// Serialize the menu the backend ACTUALLY built, for template->native readback.
laufey_value_t* (*test_dump_menu)(void* backend_data, int surface, uint32_t id);
// Post a synthetic input event for the mouse/keyboard/wheel/cursor handlers.
bool (*test_inject_input)(void* backend_data, uint32_t window_id,
                          const laufey_test_input_t* event);
```

Note the contrast with macOS self-AX (`§7.2`): reading the OS's view of a widget
needs the backend's **main thread**, which the worker-thread runtime can't reach
(a `dispatch_sync` to the main queue deadlocks against the backend event loop) —
so structure checks must live behind a backend hook too, whereas the click hook
above only touches an in-process mutex and works from any thread.

---

## 9. Full coverage matrix

Rows are capability groups; each cell is per-backend. Every ✅ is additionally
per-OS (`Linux/macOS/Windows`); WebView's web-layer cells differ by engine.

| Capability                             | Technique | CEF | WebView | Winit            | Gate                   |
| -------------------------------------- | --------- | --- | ------- | ---------------- | ---------------------- |
| Window geometry/state/opacity readback | A         | ✅  | ✅      | ✅ (Rust)        | ✅                     |
| Window lifecycle events                | B         | ✅  | ✅      | ✅               | ✅                     |
| Clipboard round-trip                   | A         | ✅  | ✅      | ✅               | ✅                     |
| Window handles / types                 | A         | ✅  | ✅      | ✅               | ✅                     |
| Application / context menu             | D + B     | ✅  | ✅      | ✅ (`muda`)      | ✅                     |
| Tray icon / menu / click               | D + B     | ✅  | ✅      | ✅ (`tray-icon`) | ✅ (win tray nightly)  |
| Notifications payload                  | D         | ✅  | ✅      | probe            | Linux ✅, else nightly |
| Dock / taskbar                         | A/D/F     | ✅  | ✅      | probe            | partial                |
| Raw mouse/keyboard/wheel events        | E         | ✅  | ✅      | ✅               | ✅ (with hook)         |
| Web: bindings/execute_js/navigate/load | B/C/E     | ✅  | ✅      | **N/A**          | ✅ (CEF/WebView)       |
| Custom scheme handlers                 | C         | ✅  | ✅      | **N/A**          | ✅ (CEF/WebView)       |
| DevTools                               | F         | ✅  | partial | N/A              | nightly                |
| Dialogs (alert/confirm/prompt/file)    | F         | ⚠️  | ⚠️      | ⚠️               | nightly                |

Net: ~90% of the C ABI is a hosted-CI PR gate across all backends; only modal /
outward-facing surfaces are nightly.

---

## 10. CI architecture

Today CI builds only Winit + lint + a `capi` unit/doc test. To test all
backends:

1. **Build jobs** for CEF and WebView per OS. CEF is expensive (downloads/builds
   ~GBs) — cache aggressively; consider running CEF nightly while WebView +
   Winit gate per-PR.
2. **Test matrix**
   `backend ∈ {cef, webview, winit} × os ∈ {linux, macos,
   windows}`, each
   launching the shared runtime under the right wrapper:

   ```
   # Linux (covers cef/webview libappindicator AND winit tray-icon):
   xvfb-run -a dbus-run-session -- sni-driver <backend-bin> --runtime libnative_e2e.so
   # macOS (self-AX + readback, in-process, no permission):
   <backend-bin> --runtime libnative_e2e.dylib
   # Windows (readback + FlaUI attach):
   <backend-bin> --runtime native_e2e.dll
   ```

Because the runtime is written once and the observers are backend-agnostic, the
_incremental_ cost of "all backends" is mostly build time and matrix legs, not
new test code.

---

## 11. Rollout

1. **Layer 0 battery** — the capability-probing `native_e2e` runtime: readback +
   event-callback + clipboard + scheme + menu/tray assertions, each tagged with
   its required capability and the `PASS/FAIL/N/A` protocol. Highest ROI, covers
   recent code (opacity, clipboard, app_id). ~2–3 days.
2. **Backend-launch matrix** driving that runtime under CEF/WebView/Winit; reuse
   `sni-driver` as the Linux wrapper for all three. ~2 days.
3. **Layer 1 Linux** — land `sni-driver` + `e2e-linux-native` job. Flagship.
   ~2–3 days (driver already compiles).
4. **Layer 1 macOS** — embed the self-AX verifier; add to the macOS leg. ~2
   days.
5. **Layer 1 Windows** — FlaUI menu suite; tray scraping nightly. ~2–3 days.
6. **CI** — add CEF/WebView build jobs (cache CEF; CEF nightly if needed).
7. **Nightly** — dialogs/modal/outward-facing; later Servo branch and iOS.

---

## 12. Open questions / spikes

- Whether the CI libayatana version registers the item at `/StatusNotifierItem`
  vs `/org/ayatana/NotificationItem/*` — the sender-based capture handles both,
  but assertions on the path should not hard-code it.
- laufey's real macOS `NSStatusItem` uses a button _image_, not a title, so its
  AX bar item may have no `AXTitle` — assert on the _menu items_ (which do have
  titles) rather than the bar item.
- Winit notification / dock capability coverage (probe at runtime; several
  fields are `None`).
- Dialog auto-answer: backend stub vs external AX/UIA driver on a side thread.

---

## 13. Verified artifacts

- Linux D-Bus driver: written, `cargo check` passes against `zbus` 5.16
  (`§7.1`).
- macOS self-AX read of `AXMenuBar` / `AXExtrasMenuBar` and
  `NSMenu.performActionForItem(at:)`: empirically confirmed on macOS 15.5 with
  no accessibility permission granted (`§7.2`).
