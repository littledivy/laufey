# Native dialogs

laufey can show the operating system's standard alert, confirmation, and prompt
dialogs. Each call is modal and blocks until the user dismisses the dialog, then
returns the user's response. A dialog can be attached to a specific window or
shown at the application level.

```rust
win.alert("Heads up", "File saved.");

if win.confirm("Delete", "Are you sure?") {
  // The user clicked OK or Yes.
}

if let Some(name) = win.prompt("Name", "What's your name?", "World") {
  println!("hello {name}");
}
```

Although the call blocks the calling thread, the underlying platform routine —
`runModal` on macOS, `MessageBoxW` on Windows, and `gtk_dialog_run` on Linux —
keeps pumping operating-system events while the dialog is open, so your other
windows continue to render and respond. A prompt returns the text the user
entered, or `None` if the user cancelled. The same three operations are also
available as the application-scoped free functions `laufey::alert`,
`laufey::confirm`, and `laufey::prompt`.

On the CEF and WebView backends, the page's own `alert()`, `confirm()`, and
`prompt()` calls are routed to these native dialogs. The Winit backend has no
web engine, so it has no page dialogs to route.
