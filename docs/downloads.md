# Downloads

Pages in a laufey webview can download files the same way they would in a
browser: clicking an `<a download>` link, navigating to a response with
`Content-Disposition: attachment`, or saving a `Blob` the page created (a
generated CSV, an export, an image). A webview has no download UI of its own, so
the host is responsible for deciding where the bytes go. laufey asks the user
with the operating system's own "Save As" dialog:

```html
<button id="export">Export report</button>
<script>
document.getElementById("export").addEventListener("click", () => {
  const blob = new Blob(["report contents"], { type: "text/plain" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = "report.txt";
  a.click();
  URL.revokeObjectURL(url);
});
</script>
```

The dialog is seeded with the file name the page (or the server's
`Content-Disposition` header) suggests. Dismissing it cancels the download —
nothing is written. The browser engine streams the bytes to the chosen path
itself, so file contents never pass through the runtime.

The dialog is the platform's standard save panel, on every backend that has a
web engine:

| Platform | Dialog                 |
| -------- | ---------------------- |
| macOS    | `NSSavePanel`          |
| Windows  | `IFileSaveDialog`      |
| Linux    | `GtkFileChooserDialog` |

The CEF backend uses CEF's native "Save As" dialog. The Winit backend has no web
engine and never downloads anything.
