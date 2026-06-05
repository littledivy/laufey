# Window management

Every laufey application is built around one or more native windows. A `Window`
controls its title, size, position, resizable and always-on-top flags,
visibility, and focus. The type is a builder, so you can configure a window
fluently when you create it, and each property also has a plain setter you can
call later while the window is open.

```rust
use laufey::Window;

let win = Window::new(800, 600)
  .title("My App")
  .position(100, 100)
  .resizable(true)
  .load("index.html"); // or .navigate("https://example.com")

win.set_size(1024, 768);
let (width, height) = win.get_size();
win.focus();
win.hide();
```

A few properties can only be chosen when the operating system creates the window
and cannot be changed afterwards: whether the window is frameless (drawn without
operating-system chrome) and whether it is a non-activating panel that does not
steal keyboard focus. You set those through `Window::new_with_options`.
Everything else is a live setter. All positions and sizes are expressed in
density-independent pixels with the origin at the top-left of the screen. The
Winit backend can create and manage windows, but because it has no web engine it
cannot navigate to a URL or execute JavaScript.
