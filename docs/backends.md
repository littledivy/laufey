# Backends

A backend is the native executable that hosts a browser (or windowing) engine
and implements the [C ABI](c-abi.md). laufey ships three; a fourth is on a
branch. All implement the same `laufey_backend_api_t`, so a runtime is portable
across them — the differences are in engine, process model, size, and a few
features that a given engine can't express on a given OS (see
[the feature pages](window-management.md)).

| Backend                                                           | Engine        | Process model | Bundled | JS bridge |
| ----------------------------------------------------------------- | ------------- | ------------- | ------- | --------- |
| [CEF](https://github.com/littledivy/laufey/tree/main/cef)         | Chromium 144  | multi-process | yes     | yes       |
| [WebView](https://github.com/littledivy/laufey/tree/main/webview) | system native | single        | no      | yes       |
| [Winit](https://github.com/littledivy/laufey/tree/main/winit)     | none          | single        | n/a     | no        |

Platform support is x86_64 + aarch64 on macOS and Linux, x86_64 on Windows.
There is also an **iOS** backend (UIKit + WKWebView, statically linked) — see
[iOS](ios.md). Android is not supported.

## CEF

Embeds Chromium 144 through the Chromium Embedded Framework and runs Chromium's
real multi-process architecture — a browser process plus renderer, GPU, and
utility subprocesses, with the same rendering and DevTools you get in Chrome.
The engine is bundled into the app, so binaries are large but rendering is
identical everywhere and independent of the host OS.

Sources live in [`cef/`](https://github.com/littledivy/laufey/tree/main/cef);
shared native features come from `backend-common`. On Windows the backend links
the static CRT (`/MT`), so everything it links — including `backend-common` — is
built `/MT`.

Linux caveat: the application menu doesn't work under CEF (a `GtkMenuBar` must
be packed into a GtkWindow above the browser, and reparenting CEF into a
client-owned GtkWindow via `CefWindowInfo::SetAsChild` breaks on XWayland).
Context menus do work, because `GtkMenu` popups need no GtkWindow container.

Custom schemes: Chromium registers them at process start, before the runtime is
loaded, so besides calling `register_scheme_handler` the embedder declares them
when launching the host — `--laufey-custom-schemes=myapp` or
`LAUFEY_CUSTOM_SCHEMES=myapp` (`cef/src/custom_schemes.h`). See
[Custom URL schemes](custom-schemes.md).

## WebView

Delegates to the platform's native web engine — **WKWebView** on macOS,
**WebView2** on Windows, **WebKitGTK** on Linux. The engine is never bundled, so
apps stay small, at the cost of rendering that varies by OS and engine version.
Single-process.

Sources live in
[`webview/`](https://github.com/littledivy/laufey/tree/main/webview), one file
per platform (`webview_macos.mm`, `webview_windows.cc`, `webview_linux.cc`),
sharing `backend-common` for menus, tray, dialogs, dock, and notifications.

Custom schemes registered through `register_scheme_handler` are installed per
engine — a `WKURLSchemeHandler` per scheme on the `WKWebViewConfiguration`,
`webkit_web_context_register_uri_scheme` plus the security manager's secure/CORS
flags on WebKitGTK, `CoreWebView2CustomSchemeRegistration` (TreatAsSecure,
HasAuthorityComponent) on the WebView2 environment — so each is a real
`<scheme>://<host>` origin. WKWebView and WebView2 read the set when a web view
(WebView2: the first one) is created; register schemes before the first window.
See [Custom URL schemes](custom-schemes.md).

## Winit

Engine-free. It creates native windows via
[winit](https://github.com/rust-windowing/winit) for apps that draw their own
content — GPU surfaces, custom renderers — without loading a web engine. There
is no JS bridge; `get_window_handle` / `get_display_handle` expose the raw
handles needed to create a rendering surface. Sources in
[`winit/`](https://github.com/littledivy/laufey/tree/main/winit).

## Servo (experimental)

A [Servo](https://servo.org)-based backend is preserved on the
[`servo`](https://github.com/littledivy/laufey/tree/servo) branch for future
work and is not part of the mainline build.

## backend-common

CEF and WebView share their native-API implementations (menus, tray, dock,
dialogs, notifications, key mapping) in
[`backend-common/`](https://github.com/littledivy/laufey/tree/main/backend-common),
included as a CMake subdirectory by each backend. The winit backend shares its
non-engine pieces through `backend-winit-common` instead.
