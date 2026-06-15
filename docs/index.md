<img src="laufey.png" alt="laufey" width="220" style="display: block; margin-bottom: 1rem;">

**laufey** is a web embedded framework: build cross-platform desktop apps with
web technologies and your choice of browser engine.

It is built around a small C ABI that separates the **browser engine** (the
backend) from your **application logic** (the runtime). You write the runtime in
Rust against one portable API; laufey ships prebuilt backends — Chromium via
[CEF](backends.md), the system [WebView](backends.md), and an engine-free
[Winit](backends.md) windowing backend — and your app runs on any of them.

```rust
use laufey::{Value, Window};

fn main() {
    Window::new(800, 600)
        .title("My App")
        .bind("greet", |call| {
            let name = call
                .args
                .first()
                .and_then(|v| v.as_string())
                .unwrap_or("World");
            call.resolve(Value::String(format!("Hello, {name}!")));
        })
        .load("index.html");
}

laufey::main!(main);
```

## Where to go next

- [Architecture](architecture.md) — how backends and runtimes fit together.
- [C ABI](c-abi.md) — the `laufey.h` contract: entry points, the API table, and
  the value model. Read this if you're implementing a backend or a binding.
- [Backends](backends.md) — CEF, WebView, and Winit, and how they differ.
- The feature pages — [windows](window-management.md),
  [JavaScript interop](javascript-interop.md), [menus](menus.md),
  [dialogs](dialogs.md), [tray](tray.md), [notifications](notifications.md), and
  more — each with a usage example and its per-platform notes.
- [Packaging & distribution](distribution.md) — bundling, signing, and updates.
- [Building](building.md) — prerequisites and `make` targets.

The source lives at
[github.com/littledivy/laufey](https://github.com/littledivy/laufey).
