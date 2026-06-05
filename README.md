<img src="docs/laufey.png" alt="laufey" width="320">

laufey is a web embedded framework: build cross platform apps using web
technologies with your choice of browser engine.

### How it works

laufey is built around a C ABI that separates browser engines (backends) from
application logic (runtimes). Prebuilt backends for [CEF](./cef) and the system
[WebView](./webview) implement the `laufey_backend_api_t` interface defined in
[`capi/include/laufey.h`](capi/include/laufey.h), and a [Winit](./winit) backend
handles windowing without a web engine. (A Servo-based backend lives on the
[`servo`](https://github.com/littledivy/wef/tree/servo) branch.)

Runtimes are shared libraries compiled with user application logic. When the
backend starts, it loads the runtime dylib and hands control to the runtime. The
runtime uses the interface to create windows, register JavaScript bindings, and
respond to calls from the web content.

laufey also handles bidirectional marshalling that abstracts message passing
between JS and native code. This modular approach leads to fast development and
packaging of laufey applications.

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

### Docs

- [architecture.md](docs/architecture.md) — backend/runtime split
- [c-abi.md](docs/c-abi.md) — the `laufey.h` C ABI: runtime entry points, the API
  table, the value model, and the JS call flow
- [backends.md](docs/backends.md) — CEF, WebView, and Winit
- [features/](docs/features/) — one page per feature, with a usage example and
  the per-platform support matrix
- [building.md](docs/building.md) — `make` targets and prerequisites
