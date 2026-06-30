# Clipboard

laufey can read and write the operating system's clipboard as plain text,
mirroring the web `navigator.clipboard.readText()` / `writeText()` surface. The
two operations are application-scoped free functions:

```rust
// Write text to the system clipboard.
laufey::write_clipboard_text("hello from laufey");

// Read it back. Returns `None` if the clipboard is empty, holds no text, or
// the backend doesn't support clipboard access.
if let Some(text) = laufey::read_clipboard_text() {
  println!("clipboard: {text}");
}
```

Passing an empty string to `write_clipboard_text` clears the clipboard. Both
functions must be called on the UI thread.

The implementation differs by platform. macOS uses `NSPasteboard`
(`UIPasteboard` on iOS), Windows uses the Win32 clipboard with the
`CF_UNICODETEXT` format, and Linux uses the GTK clipboard (the `CLIPBOARD`
selection) on the CEF and WebView backends.

The engine-free Winit backend has no web engine bundled, so it shells out to the
platform's standard clipboard tools instead — `pbcopy` / `pbpaste` on macOS,
`clip` / `Get-Clipboard` on Windows, and `wl-clipboard` (falling back to
`xclip`) on Linux. These are present on a default desktop install of each
platform; if the Linux tools are missing, reads return `None` and writes are a
no-op.
