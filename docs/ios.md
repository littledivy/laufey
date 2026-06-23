# iOS

laufey has an iOS backend: the C ABI (the same `laufey_backend_api_t` the
desktop backends implement) on **UIKit + WKWebView**. A laufey runtime drives a
native iOS app — web UI in a `WKWebView`, talking to native code through the
usual JS bridge. Verified building, signing, and running on a physical device.

## How it differs from desktop

The desktop model is a **backend executable** that `dlopen`s a **runtime
dylib**. iOS forbids loading arbitrary dylibs and the OS owns the app lifecycle
(`UIApplicationMain`), so on iOS the two collapse into **one statically-linked
app binary**:

```
MyApp.app/MyApp            ← one signed Mach-O
├─ UIKit shell             main_ios.mm: UIApplicationMain → UIViewController + WKWebView
├─ laufey iOS backend      webview_ios.mm: fills laufey_backend_api_t
└─ laufey runtime          your app (capi), linked as a static lib
```

The runtime's `laufey_runtime_init` / `_start` / `_shutdown` are resolved at
**link time** (via `RuntimeLoader::LoadStatic`) instead of `dlopen`. A weak
`main` in `main_ios.mm` lets the same file serve either a C-linked or a
Rust-linked app.

- A "window" is a `UIWindow` + root `UIViewController` hosting a `WKWebView`.
- The JS bridge, value marshalling, and init script are shared verbatim with the
  desktop WebView backend (`runtime_loader.cc`, `laufey_json.h`,
  `init_script.h`).
- Desktop-only surfaces (menus, tray, dock) are no-ops; iOS doesn't link
  `backend-common`, only its value marshalling.

Sources:
[`webview/src/webview_ios.mm`](https://github.com/littledivy/laufey/blob/main/webview/src/webview_ios.mm),
[`webview/src/main_ios.mm`](https://github.com/littledivy/laufey/blob/main/webview/src/main_ios.mm).

## Build

The iOS backend is wired into the webview CMake build. It needs a laufey runtime
compiled as a **static lib** for the iOS target (e.g. the
[`examples/ios_hello`](https://github.com/littledivy/laufey/tree/main/examples/ios_hello)
runtime):

```sh
# 1. runtime static lib (device: aarch64-apple-ios; simulator: aarch64-apple-ios-sim)
cargo build --release -p ios_hello --target aarch64-apple-ios

# 2. iOS app via CMake (point LAUFEY_IOS_RUNTIME_LIB at the static lib)
cd webview
cmake -B build-ios -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DCMAKE_C_COMPILER="$(xcrun -f clang)" \
  -DCMAKE_CXX_COMPILER="$(xcrun -f clang++)" \
  -DLAUFEY_IOS_RUNTIME_LIB="$PWD/../target/aarch64-apple-ios/release/libios_hello.a"
cmake --build build-ios
# → build-ios/laufey_webview.app
```

For the **simulator**, use `-DCMAKE_OSX_SYSROOT=iphonesimulator` and the
`aarch64-apple-ios-sim` runtime lib; install/launch with
[`ios/build.sh`](https://github.com/littledivy/laufey/blob/main/ios/build.sh) or
`xcrun simctl install/launch`.

## Sign + package an `.ipa`

[`ios/package-ipa.sh`](https://github.com/littledivy/laufey/blob/main/ios/package-ipa.sh)
sets the bundle id to match a provisioning profile's App ID, embeds the profile,
signs with the profile's entitlements, and zips a `Payload/`:

```sh
ios/package-ipa.sh webview/build-ios/laufey_webview.app \
  <bundle-id-matching-profile> \
  "Apple Distribution: Your Team (TEAMID)" \
  ~/Library/MobileDevice/Provisioning\ Profiles/<uuid>.mobileprovision \
  laufey.ipa
```

The bundle id must match the profile's App ID, and the device must be in the
profile's provisioned devices (ad-hoc) or the profile must be a distribution
profile for the App Store.

## Install on a device

```sh
xcrun devicectl list devices                       # find the device id
xcrun devicectl device install app --device <id> laufey.ipa
xcrun devicectl device process launch --device <id> <bundle-id>
```

## Status

- Verified: CMake build → device binary → signed `.ipa` → install + launch on a
  physical iPhone, and the same backend on the simulator.
- Not yet: the iOS backend isn't exercised by CI; App Store submission flow is
  not automated.
