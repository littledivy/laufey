# Window events

A window reports lifecycle events as they happen: focus and blur, resize, move,
and a request to close. These are commonly used to persist a window's geometry
between runs or to intervene before the window goes away.

```rust
let win = Window::new(800, 600)
  .on_focused(|focused| println!("focused: {focused}"))
  .on_resize(|w, h| println!("resized {w}x{h}"))
  .on_move(|x, y| println!("moved {x},{y}"))
  .on_close_requested(|| println!("user clicked close"))
  .load("index.html");
```

The close-requested handler fires when the user clicks the window's close
button, before the window is destroyed. This gives the runtime a chance to ask
for confirmation or save unsaved work first.
