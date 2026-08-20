# Window management

Every laufey application is built around one or more native windows. A `Window`
controls its title, size, position, resizable and always-on-top flags, opacity,
click passthrough, visibility, and focus. The type is a builder, so you can
configure a window fluently when you create it, and each property also has a
plain setter you can call later while the window is open.

```rust
use laufey::Window;

let win = Window::new(800, 600)
  .title("My App")
  .position(100, 100)
  .resizable(true)
  .opacity(0.95)
  .load("index.html"); // or .navigate("https://example.com")

win.set_size(1024, 768);
let (width, height) = win.get_size();
win.focus();
win.hide();
```

A few properties can only be chosen when the operating system creates the window
and cannot be changed afterwards: whether the window is frameless (drawn without
operating-system chrome), whether it is a non-activating panel that does not
steal keyboard focus, and whether it has a transparent background. You set those
through `Window::new_with_options`. Everything else is a live setter. All
positions and sizes are expressed in density-independent pixels with the origin
at the top-left of the screen. The Winit backend can create and manage windows,
but because it has no web engine it cannot navigate to a URL or execute
JavaScript.

## Opacity and transparency

These are two distinct things:

- **Opacity** (`Window::opacity` / `set_opacity` / `get_opacity`) fades the
  _entire_ window — web content and native chrome alike — by a uniform factor in
  `0.0..=1.0`, where `1.0` is fully opaque (the default), like CSS `opacity` on
  the whole window. It is a live setter you can animate at runtime. The web
  backends implement it on every desktop platform (macOS `NSWindow.alphaValue`,
  Windows layered-window alpha, Linux `gtk_widget_set_opacity`). The Winit
  backend has no opacity API, so the call is a no-op there and `get_opacity`
  returns `1.0`.

  ```rust
  win.set_opacity(0.8); // 80% opaque
  ```

- **Transparency** (`WindowOptions::transparent`) gives the window a transparent
  _background_ so the web content's own alpha composites against whatever is
  behind the window. Any region the page leaves transparent (e.g. a
  `transparent` root background) shows the desktop through it. This must be
  chosen at creation time and is commonly paired with `frameless`.

  ```rust
  use laufey::{Window, WindowOptions};

  let win = Window::new_with_options(
    400,
    300,
    WindowOptions { frameless: true, transparent: true, ..Default::default() },
  )
  .load("index.html");
  ```

  Transparency is supported by the system-WebView backend on macOS and on Linux
  (WebKitGTK, on a compositing window manager), and by the Winit backend. It is
  not supported by the Windows WebView2 backend or the CEF backend, which paint
  an opaque window background; the flag is ignored there.

## Click passthrough

`Window::click_passthrough` / `set_click_passthrough` / `get_click_passthrough`
makes the window ignore _all_ mouse input — clicks, moves, and wheel events fall
through to whatever window is beneath it, like Electron's
`setIgnoreMouseEvents(true)`. Keyboard input is unaffected. It is a live setter
you can toggle at any time, intended for frameless/transparent overlay windows:
HUDs, notification toasts, screen annotations.

```rust
use laufey::{Window, WindowOptions};

let overlay = Window::new_with_options(
  400,
  300,
  WindowOptions { frameless: true, transparent: true, ..Default::default() },
)
.always_on_top(true)
.click_passthrough(true)
.load("overlay.html");

// Later, to start accepting input again:
overlay.set_click_passthrough(false);
```

Platform notes:

- **macOS** — `NSWindow.ignoresMouseEvents`; works with every backend.
- **Windows** — the top-level window gets the `WS_EX_TRANSPARENT` and
  `WS_EX_LAYERED` extended styles, which exclude it (children included) from
  mouse hit-testing. Composes with `set_opacity`, which shares the layered
  style.
- **Linux** — the window's X11/Wayland input shape region is cleared.
  Best-effort under a reparenting X11 window manager, where the WM's frame may
  still catch clicks — pair it with a frameless window (the intended overlay use
  case) for reliable behavior.
- The Winit backend uses winit's `set_cursor_hittest`, with the same platform
  behavior as above.
