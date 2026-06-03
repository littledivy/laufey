<img src="https://github.com/user-attachments/assets/c7d55981-ab29-4bea-a4c6-28feeb1b4520" alt="wef" width="120" align="left">

# wef

**wef** ("Web embedded framework") lets you build cross-platform desktop apps
with web technologies and your choice of browser engine.

It is built around a small C ABI that separates the **browser engine** (the
backend) from your **application logic** (the runtime). You write the runtime in
Rust against one portable API; wef ships prebuilt backends — Chromium via
[CEF](backends.md), the system [WebView](backends.md), and an engine-free
[Winit](backends.md) windowing backend — and your app runs on any of them.

```rust
use just_wef::{Value, Window};

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

just_wef::main!(main);
```

## Where to go next

- [Architecture](architecture.md) — how backends and runtimes fit together.
- [C ABI](c-abi.md) — the `wef.h` contract: entry points, the API table, and the
  value model. Read this if you're implementing a backend or a binding.
- [Backends](backends.md) — CEF, WebView, and Winit, and how they differ.
- The feature pages — [windows](window-management.md),
  [JavaScript interop](javascript-interop.md), [menus](menus.md),
  [dialogs](dialogs.md), [tray](tray.md), [notifications](notifications.md), and
  more — each with a usage example and its per-platform notes.
- [Packaging & distribution](distribution.md) — bundling, signing, and updates.
- [Building](building.md) — prerequisites and `make` targets.

The source lives at
[github.com/littledivy/just-wef](https://github.com/littledivy/just-wef).
