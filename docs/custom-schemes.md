# Custom URL schemes

A runtime can serve its web content in-process over a URL scheme of its own
instead of a loopback HTTP server. Every web-engine backend serves the built-in
`app` scheme, and a runtime may register more, such as `myapp`. Each registered
scheme is a real origin in the page: `location.origin` is `<scheme>://<host>`,
the page is a secure context (`crypto.subtle` and other secure-only APIs work),
`localStorage` and IndexedDB are scoped to that origin and, on the WebView
backends, persist across launches, and cross-origin requests carry
`Origin: <scheme>://<host>`. That gives an application a stable identity that
does not change with a port number, which storage, CORS allow-lists, and sign-in
providers that check the request origin (OAuth redirects, passkeys) depend on.

```rust
use laufey::Window;

fn main() {
  // Register before the first window: the engines read their scheme tables
  // when a web view is created.
  laufey::register_scheme_handler("myapp", |req| {
    let (status, body): (i32, &[u8]) = match req.url.as_str() {
      "myapp://app/" => (200, b"<!doctype html><h1>Hello</h1>"),
      _ => (404, b"not found"),
    };
    let headers = [("content-type".to_string(), "text/html".to_string())];
    req.exchange.begin(status, &headers);
    req.exchange.write(body);
    req.exchange.finish();
  });

  // location.origin in this window is "myapp://app".
  Window::new(800, 600).title("My App").load("myapp://app/");
}

laufey::main!(main);
```

One handler serves every registered scheme and every window: a later
`register_scheme_handler` call replaces the handler for all of them and adds the
new scheme, so dispatch on `req.url` (and `req.window_id` if windows serve
different content). The handler runs on a backend thread and must not block it;
move slow work onto your own thread or async runtime. Read the request body with
`req.exchange.read_body`. On WKWebView, WebKitGTK and CEF the response is
streamed: the page's `fetch` sees each `write` as it happens. WebView2 is the
exception. It reads a custom-scheme response to the end before the page sees any
of it, so there the body arrives whole once the handler calls `finish`, and a
response that never finishes (server-sent events, for example) never arrives.
Scheme names follow RFC 3986 (a letter, then letters, digits, `+`, `-`, or `.`),
are case-insensitive, and are stored in lowercase; an invalid name is logged and
ignored. So is a scheme the engines already give a meaning of their own, in any
case: `http`, `https`, `ws`, `wss`, `ftp`, `file`, `filesystem`, `data`, `blob`,
`javascript`, `about`, `chrome`, `chrome-extension`, `chrome-untrusted`,
`devtools` and `view-source`. Taking one of them over would hand the handler the
page's ordinary web traffic, local files or script URLs. This applies to
`register_scheme_handler`, the `--laufey-custom-schemes` switch and
`LAUFEY_CUSTOM_SCHEMES` alike. Schemes only the OS handles (`mailto`, `tel`) are
not reserved. Engine-less backends such as Winit have no scheme support;
`laufey::scheme_handlers_supported()` returns `false` there, and the application
should fall back to a loopback server.

Register every scheme before creating the first window. The system web views
read their scheme tables when a web view is created, so on most backends a
scheme registered afterwards is not served by a window that already exists, and
the backend logs a warning. Each backend installs the schemes in its own way:

- **WebView (macOS)** installs one `WKURLSchemeHandler` per scheme on the
  `WKWebViewConfiguration` of each new window. Schemes that WebKit handles
  itself (`http`, `https`, `file`, and so on) are skipped with a warning.
- **WebView (Linux)** registers each scheme on WebKitGTK's default web context
  and marks it secure and CORS-enabled in the context's security manager. The
  web context is shared, so WebKitGTK also applies a late registration to
  windows that already exist; the backend still warns, because the other
  backends do not.
- **WebView (Windows)** adds a `CoreWebView2CustomSchemeRegistration`
  (`TreatAsSecure`, `HasAuthorityComponent`) per scheme to the environment
  options. Every environment in a process shares one user data folder, and
  WebView2 requires identical scheme registrations across them, so the backend
  freezes the set when the first window is created; later registrations are
  logged and ignored for the rest of the process.
- **CEF** learns custom schemes in `CefApp::OnRegisterCustomSchemes`, which runs
  in every process during startup, before the runtime library is loaded. The
  embedder therefore also declares the schemes when it launches the host, with
  `--laufey-custom-schemes=myapp,other` or `LAUFEY_CUSTOM_SCHEMES=myapp,other`
  (comma-separated; `app` is always declared). The browser process forwards the
  list to its child processes. A declared scheme is served in every window from
  the moment it is registered. A scheme that is registered but was not declared
  is still served, but Chromium treats it as a non-standard scheme with an
  opaque origin and no secure context, and the backend logs a warning. The CEF
  host keeps its profile in a per-process temporary directory, so storage does
  not outlive the process. Chromium's Local Network Access checks also treat a
  custom-scheme page as a public origin: its requests to loopback or
  private-network addresses wait for a permission prompt that the CEF host does
  not show.

```sh
laufey --laufey-custom-schemes=myapp --runtime ./libmyapp.so
```

Treat everything in a request as untrusted input, as you would for an HTTP
server. A page on another origin, including a remote page the window navigated
to, can send requests to your scheme; engines allow it to read the response only
when the response carries CORS headers, so do not answer with
`Access-Control-Allow-Origin: *` on responses that carry private data, and check
the `origin` request header before acting on state-changing requests. Because
one handler serves every scheme, check the scheme and host of `req.url` rather
than assuming which one a request came in on.

The C-level contract is described in
[C ABI → Custom URL scheme handler](c-abi.md#custom-url-scheme-handler-api--26),
and the end-to-end battery that checks it on each backend in
[End-to-end testing](e2e-testing.md#c-custom-scheme--ipc).
