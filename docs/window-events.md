# Window events

A window reports lifecycle events as they happen: focus and blur, resize, move,
and a request to close. These are commonly used to persist a window's geometry
between runs or to intervene before the window goes away.

```rust
let win = Window::new(800, 600)
  .on_focused(|event| println!("focused: {}", event.focused))
  .on_resize(|event| println!("resized {}x{}", event.width, event.height))
  .on_move(|event| println!("moved {},{}", event.x, event.y))
  .on_close_requested(|event| {
    println!("user clicked close on window {}", event.window_id);
    Window::from_id(event.window_id).close();
  })
  .load("index.html");
```

`on_close_requested` fires when the user clicks the window's own close control
(title bar button, `Alt+F4`, a window manager's close action) — never for
`Cmd+Q`, a "Quit" menu/tray item, or any other app-level termination.
Registering it holds the window open: it won't close on its own until you call
`Window::close()`, synchronously in the handler or later from any thread, e.g.
after a confirm dialog or once unsaved work is flushed. Doing nothing leaves it
open — which is also how you'd implement "hide to tray" instead of closing:

```rust
.on_close_requested(|event| {
  Window::from_id(event.window_id).hide();
})
```

Or, for a synchronous confirm dialog, call `Window::confirm()` directly in the
handler — it blocks (pumping OS events) until the user answers:

```rust
.on_close_requested(|event| {
  let win = Window::from_id(event.window_id);
  if win.confirm("Quit?", "You have unsaved changes. Quit anyway?") {
    win.close();
  }
})
```

See [c-abi.md](c-abi.md), "Close-requested handler defers the close", for the
full contract, including why app-level quit paths are deliberately out of scope,
and `examples/confirm_before_quit` for a complete runnable version of the
confirm-dialog pattern above.
