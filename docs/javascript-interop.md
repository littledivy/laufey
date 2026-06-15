# JavaScript interop

JavaScript interop lets the page and your Rust code call each other. You expose
native functions under a namespace object in the page; when the page calls one,
it receives a promise, and your Rust handler resolves or rejects it. Your code
can also evaluate a script in a window and read back its result.

```rust
use laufey::{Value, Window};

let win = Window::new(800, 600)
  .bind("greet", |call| {
    let name = call.args.first().and_then(|v| v.as_string()).unwrap_or("World");
    call.resolve(Value::String(format!("Hello, {name}!")));
  })
  .bind_async("fetchUser", |call| async move {
    let user = load_user().await;
    call.resolve(user);
  })
  .load("index.html");

// Evaluate a script in the page and read the result.
win.execute_js("document.title", Some(|result, _error| println!("{result:?}")));
```

```js
// In the page:
const message = await Laufey.greet("Ada"); // "Hello, Ada!"
```

Arguments and results cross the boundary as a `Value`, which models the JSON
types — null, boolean, integer, double, string, list, and dictionary — along
with binary blobs. When the page passes a JavaScript function as an argument, it
arrives as a callback value that you can invoke later and must release when you
are finished with it. The namespace object is named `Laufey` by default; call
`laufey::set_js_namespace` before creating any windows to change it. All
handlers run on the user-interface thread. None of this is available on the
Winit backend, which has no JavaScript engine.
