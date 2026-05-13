![wef-doodle](https://github.com/user-attachments/assets/c7d55981-ab29-4bea-a4c6-28feeb1b4520)

wef ("Web embedded framework") lets you build cross platform apps using web
technologies with your choice of browser engine.

### How it works

wef is built around a C ABI that separates browser engines (backends) from
application logic (runtimes). Prebuilt backends for [CEF](./cef) and the system
[WebView](./webview) implement the `wef_backend_api_t` interface defined in
[`capi/include/wef.h`](capi/include/wef.h), and a [Winit](./winit) backend
handles windowing without a web engine. (A Servo-based backend lives on the
[`servo`](https://github.com/littledivy/wef/tree/servo) branch.)

Runtimes are shared libraries compiled with user application logic. When the
backend starts, it loads the runtime dylib and hands control to the runtime. The
runtime uses the interface to create windows, register JavaScript bindings, and
respond to calls from the web content.

wef also handles bidirectional marshalling that abstracts message passing
between JS and native code. This modular approach leads to fast development and
packaging of wef applications.

```rust
use wef::{Window, Value};

fn main() {
    Window::new(800, 600)
      .title("My App")
      .bind("greet", |call| {
         let name = call.args.get(0)
             .and_then(|v| v.as_string())
             .unwrap_or("World");
         call.resolve(Value::String(format!("Hello, {}!", name)));
      })
      .load_url("https://example.com");
}

wef::main!(main);
```

### Backend matrix

wef ships three backends today. The [CEF](./cef) backend embeds Chromium 144 and
runs a multi-process architecture identical to a full browser — you get the same
renderer, GPU process, and DevTools you would in Chrome, bundled into your app.
The system [WebView](./webview) backend delegates to the platform's native web
engine (WKWebView on macOS, WebView2 on Windows, WebKitGTK on Linux), so the
engine is never bundled and your app stays small. The [Winit](./winit) backend
is engine-free — it creates native windows for apps that draw their own content
(GPU surfaces, custom renderers) without loading a web engine.

An experimental [Servo](https://github.com/littledivy/wef/tree/servo) backend is
preserved on a branch for future work.

|             | CEF             | WebView         | Winit           |
| ----------- | --------------- | --------------- | --------------- |
| **macOS**   | x86_64, aarch64 | x86_64, aarch64 | x86_64, aarch64 |
| **Windows** | x86_64          | x86_64          | x86_64          |
| **Linux**   | x86_64, aarch64 | x86_64, aarch64 | x86_64, aarch64 |
| **Android** | ❌              | ❌              | ❌              |

|             | Engine        | Multi-process | Bundled | JS bridge |
| ----------- | ------------- | ------------- | ------- | --------- |
| **CEF**     | Chromium 144  | ✅            | ✅      | ✅        |
| **WebView** | System native | ❌            | ❌      | ✅        |
| **Winit**   | none          | ❌            | ❌      | ❌        |

### Feature support matrix

Features marked with platform qualifiers indicate per-platform differences
within a backend.

#### Window management

| Feature                   | CEF | WebView | Winit |
| ------------------------- | --- | ------- | ----- |
| Create / close window     | ✅  | ✅      | ✅    |
| Navigate (load URL)       | ✅  | ✅      | ❌    |
| Set title                 | ✅  | ✅      | ✅    |
| Execute JavaScript        | ✅  | ✅      | ❌    |
| Set / get window size     | ✅  | ✅      | ✅    |
| Set / get window position | ✅  | ✅      | ✅    |
| Set / get resizable       | ✅  | ✅      | ✅    |
| Set / get always on top   | ✅  | ✅      | ✅    |
| Show / hide / is visible  | ✅  | ✅      | ✅    |
| Focus                     | ✅  | ✅      | ✅    |
| Quit                      | ✅  | ✅      | ✅    |
| Post UI task              | ✅  | ✅      | ✅    |

#### JavaScript interop

| Feature                           | CEF | WebView | Winit |
| --------------------------------- | --- | ------- | ----- |
| JS call handler (native bindings) | ✅  | ✅      | ❌    |
| Respond to JS calls               | ✅  | ✅      | ❌    |
| Invoke JS callbacks               | ✅  | ✅      | ❌    |
| Release JS callbacks              | ✅  | ✅      | ❌    |
| Custom JS namespace               | ✅  | ✅      | ❌    |
| Execute JS with result callback   | ✅  | ✅      | ❌    |

#### Menus

| Feature          | CEF            | WebView | Winit          |
| ---------------- | -------------- | ------- | -------------- |
| Application menu | macOS, Windows | ✅      | macOS, Windows |
| Context menu     | ✅             | ✅      | macOS, Windows |
| Open DevTools    | ✅             | ✅      | ❌             |

Context menus work on CEF Linux because `GtkMenu` popups don't need a GtkWindow
container. Application menu on CEF Linux still doesn't work: a `GtkMenuBar` has
to be packed into a GtkWindow above the browser, and reparenting CEF into a
client-owned GtkWindow via `CefWindowInfo::SetAsChild` breaks on XWayland
(cross-client X11 child windows aren't supported natively by Wayland). OSR would
sidestep it but is out of scope. Winit on Linux has the same constraint.

#### Dock / taskbar

The dock is a macOS concept; on Windows the analog is the taskbar button, on
Linux it's the window-manager urgency hint / title convention.

| Feature         | CEF   | WebView | Winit |
| --------------- | ----- | ------- | ----- |
| Bounce / flash  | ✅    | ✅      | ✅    |
| Badge           | ✅    | ✅      | ✅    |
| Dock menu       | macOS | macOS   | macOS |
| Hide dock icon  | macOS | macOS   | macOS |
| Reopen callback | macOS | macOS   | macOS |

On macOS, the badge uses `NSDockTile.badgeLabel` (native red overlay on the dock
icon). On Windows and Linux, all three backends implement the badge as a
`"(N) "` prefix on the window title — the convention used by Slack, Discord,
Telegram, etc. Taskbars and window managers surface the title, so the badge
appears on the taskbar button / window manager overview. Proper Windows overlay
icons (`ITaskbarList3::SetOverlayIcon`) and Linux libunity counts are future
work. The dock menu, dock visibility, and reopen callback have no clean
cross-platform analog.

#### Tray / status bar

Tray icons are explicitly-created, persistent icons in the OS status area: macOS
menu bar extras (NSStatusItem), Windows system tray (Shell_NotifyIcon), Linux
via libappindicator. Each tray icon has its own PNG image, tooltip, left-click
handler, and right-click menu.

| Feature              | CEF            | WebView        | Winit          |
| -------------------- | -------------- | -------------- | -------------- |
| Tray icon            | ✅             | ✅             | ✅             |
| Tooltip              | macOS, Windows | macOS, Windows | ✅             |
| Right-click menu     | ✅             | ✅             | ✅             |
| Left-click handler   | macOS, Windows | macOS, Windows | ✅             |
| Double-click handler | macOS, Windows | macOS, Windows | ✅             |
| Dark-mode icon       | macOS, Windows | macOS, Windows | macOS, Windows |

CEF on Linux uses libappindicator (the same as WebView); this is independent of
the main browser window, so no GtkWindow is required. On Linux in general (CEF,
WebView, Winit), left-click and double-click are swallowed by the menu
(AppIndicator convention), tooltips are absent from the StatusNotifierItem spec,
and dark-mode icon swapping is left to the DE since libappindicator renders
through the theme.

Dark-mode icon swapping: when both `icon(...)` and `icon_dark(...)` are set, the
backends observe the system appearance (NSDistributedNotificationCenter
`AppleInterfaceThemeChangedNotification` on macOS, `WM_SETTINGCHANGE` /
`ImmersiveColorSet` + `AppsUseLightTheme` registry on Windows) and swap live.
Winit polls once per event-loop tick.

#### Native dialogs

| Feature                        | CEF | WebView | Winit |
| ------------------------------ | --- | ------- | ----- |
| Alert                          | ✅  | ✅      | ✅    |
| Confirm                        | ✅  | ✅      | ✅    |
| Prompt                         | ✅  | ✅      | ✅    |
| Browser JS dialogs (native UI) | ✅  | ✅      | ❌    |

#### Input events

| Feature               | CEF | WebView | Winit |
| --------------------- | --- | ------- | ----- |
| Keyboard events       | ✅  | ✅      | ✅    |
| Mouse click events    | ✅  | ✅      | ✅    |
| Mouse move events     | ✅  | ✅      | ✅    |
| Wheel / scroll events | ✅  | ✅      | ✅    |
| Cursor enter / leave  | ✅  | ✅      | ✅    |

#### Window events

| Feature         | CEF | WebView | Winit |
| --------------- | --- | ------- | ----- |
| Focus / blur    | ✅  | ✅      | ✅    |
| Resize          | ✅  | ✅      | ✅    |
| Move            | ✅  | ✅      | ✅    |
| Close requested | ✅  | ✅      | ✅    |

#### Window handles (GPU surface creation)

| Feature                | CEF | WebView | Winit |
| ---------------------- | --- | ------- | ----- |
| Get window handle      | ❌  | ❌      | ✅    |
| Get display handle     | ❌  | ❌      | ✅    |
| Get window handle type | ❌  | ❌      | ✅    |

#### Notifications

System notifications mirror (a subset of) the Web Notifications API: title,
body, icon, tag (replaces an existing notification with the same id),
silent, requireInteraction, and action buttons. They are app-scoped.

| Feature                  | CEF            | WebView        | Winit |
| ------------------------ | -------------- | -------------- | ----- |
| Show / close             | ✅             | ✅             | ✅    |
| Title, body              | ✅             | ✅             | ✅    |
| Icon                     | Windows        | Windows        | ❌    |
| Tag (replace)            | ✅             | ✅             | Linux |
| Silent                   | macOS, Windows | macOS, Windows | ❌    |
| Require interaction      | Linux          | Linux          | Linux |
| Action buttons           | macOS          | macOS          | ❌    |
| Shown / closed callbacks | ✅             | ✅             | ✅    |
| Clicked callback         | macOS, Windows | macOS, Windows | ❌    |
| Action callback          | macOS          | macOS          | ❌    |

Implementation per platform:

- **macOS** (CEF, WebView): `NSUserNotification` for posting — deprecated
  in macOS 11 but functional through 15 and chosen for v1 because
  posting doesn't require authorization. Authorization queries
  (`request_permission` / `query_permission` below) route through
  `UNUserNotificationCenter`, the modern API, so the embedder still gets
  the system prompt UX. Action buttons surface through
  `actionButtonTitle` plus the `additionalActions` dropdown.
- **Windows** (CEF, WebView): `Shell_NotifyIcon` balloon. On Windows 10
  and 11 the shell intercepts the balloon and renders a system toast
  (with Action Center grouping). NIIF balloons don't support action
  buttons — the `actions` field is ignored.
- **Linux** (CEF, WebView): shells out to `notify-send`, which is
  fire-and-forget — only the synthetic `shown` and `closed` events fire,
  click / action events are not surfaced. Action buttons are ignored.
- **Winit** (all platforms): `notify-rust`. Show + close + a synthetic
  `shown` callback. Click / action events are not surfaced on any
  platform because the watcher pattern needed to surface them portably
  isn't built; use CEF or WebView if you need full interactivity.

#### Permissions / runtime authorization

Wef exposes a generic `request_permission(kind) → granted | denied |
prompt | unsupported` API mirroring the Web Permissions API state set.
Bundle id, entitlements, and code-signing remain the embedder's
responsibility — wef itself doesn't hard-code any bundle identifier at
runtime, so apps that package their own `.app` (e.g. via Deno) can
configure CFBundleIdentifier + entitlements freely and the
authorization prompts target the embedder's identity.

| Feature                       | CEF | WebView | Winit |
| ----------------------------- | --- | ------- | ----- |
| Query / request notifications | ✅  | ✅      | ✅    |

On macOS all three backends call `UNUserNotificationCenter`. Unbundled
processes (no `CFBundleIdentifier`, or a binary that doesn't live in an
`.app`) report `UNSUPPORTED` rather than `DENIED` so the embedder can
distinguish "the user said no" from "this environment can't be
authorized." On Windows (`Shell_NotifyIcon` balloons) and Linux
(`libnotify` / `notify-send`) the underlying APIs have no permission
model — both `query_permission` and `request_permission` report
`GRANTED` synchronously.
