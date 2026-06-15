# Input events

A window can deliver native keyboard, mouse, wheel, and cursor enter/leave
events to your runtime. The handlers run before the events reach the page, which
lets the application observe or react to raw input.

```rust
let win = Window::new(800, 600)
  .on_keyboard_event(|e| println!("{} {:?}", e.key, e.modifiers))
  .on_mouse_click(|e| println!("button {} at {},{}", e.button, e.x, e.y))
  .on_wheel(|e| println!("scroll {},{}", e.delta_x, e.delta_y))
  .on_cursor_enter_leave(|e| println!("entered: {}", e.entered))
  .load("index.html");
```

Keyboard events carry the W3C `key` and `code` strings together with a modifier
bitmask. Mouse events carry the button, the pressed or released state, the
cursor position, the active modifiers, and the click count. Each backend
translates its own native event source — Chromium's event path under CEF,
`NSEvent` on macOS, GDK on Linux, and the Win32 message loop on Windows — into
this common shape, so the same handler works on every backend.
