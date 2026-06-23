# laufey on iOS

A native iOS backend for laufey: the laufey C ABI on UIKit + WKWebView, so a
laufey runtime (capi) can drive a native iOS app. Verified on the iOS simulator.

## Pieces

- `../webview/src/webview_ios.mm` — `WKWebViewIOSBackend : LaufeyBackend`, the
  same backend seam `WKWebViewBackend` implements on macOS, ported to UIKit: a
  "window" is a `UIWindow` + root `UIViewController` hosting a `WKWebView`. The
  JS<->native bridge, value marshalling, and init script are shared with the
  desktop backends (`runtime_loader.cc` / `laufey_json.h` / `init_script.h`).
- `../webview/src/main_ios.mm` — `UIApplicationMain` entry. iOS forbids loading
  arbitrary dylibs, so the runtime is **statically linked** and
  `laufey_runtime_init/start` are called directly via
  `RuntimeLoader::LoadStatic` (no dlopen).
- `../examples/ios_hello` — a laufey app (capi `Window` + `bind`) built as a
  static lib and linked into the app.
- `build.sh` — compile the backend, link the runtime static lib, assemble
  `laufey.app`, install + launch on the booted simulator.

## Build + run on the simulator

```sh
# 1. runtime static lib
LIBRARY_PATH="$(xcrun --sdk macosx --show-sdk-path)/usr/lib" \
  cargo build --release -p ios_hello --target aarch64-apple-ios-sim
# 2. backend + app bundle + launch (boots a simulator if needed)
bash ios/build.sh
```

The JS<->native bridge is the same as desktop: page JS calls
`Laufey.<method>(...)` (the namespace is set by the runtime), which round-trips
to the bound Rust handler.

## Notes / status

- Simulator-verified; device builds (signing, `.ipa` packaging) are not wired
  up here yet.
- The backend is built via the standalone `build.sh` (direct `clang++`), not
  yet wired into the CMake build used by the desktop backends.
- Running the **Deno** desktop runtime on iOS (instead of the lightweight
  `ios_hello` capi runtime) is a separate work-in-progress and is not part of
  this directory.
