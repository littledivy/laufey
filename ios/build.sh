#!/usr/bin/env bash
# Build + run the laufey iOS demo on the booted simulator.
#   1. compile the C++/ObjC++ iOS backend
#   2. link with the statically-linked laufey runtime (libios_hello.a)
#   3. assemble a .app bundle, install + launch on the sim
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TRIPLE="arm64-apple-ios15.0-simulator"
SDK="$(xcrun --sdk iphonesimulator --show-sdk-path)"
CLANGXX="$(xcrun -f clang++)"
OUT="$ROOT/ios/build"
APP="$OUT/laufey.app"
RT="$ROOT/target/aarch64-apple-ios-sim/release/libios_hello.a"

rm -rf "$OUT"; mkdir -p "$APP"

INCS=(-Icapi/include -Iwebview/src -Ibackend-common/include)
CXXFLAGS=(-target "$TRIPLE" -isysroot "$SDK" -std=c++17 -O2 -fobjc-arc "${INCS[@]}")
CFLAGS=(-target "$TRIPLE" -isysroot "$SDK" -std=c++17 -O2 "${INCS[@]}")

echo "== compile =="
"$CLANGXX" "${CFLAGS[@]}"   -c webview/src/runtime_loader.cc        -o "$OUT/runtime_loader.o"
"$CLANGXX" "${CFLAGS[@]}"   -c backend-common/src/laufey_value.cc   -o "$OUT/laufey_value.o"
"$CLANGXX" "${CXXFLAGS[@]}" -c webview/src/webview_ios.mm           -o "$OUT/webview_ios.o"
"$CLANGXX" "${CXXFLAGS[@]}" -c webview/src/main_ios.mm              -o "$OUT/main_ios.o"

echo "== link =="
"$CLANGXX" -target "$TRIPLE" -isysroot "$SDK" \
  "$OUT/runtime_loader.o" "$OUT/laufey_value.o" "$OUT/webview_ios.o" "$OUT/main_ios.o" \
  "$RT" \
  -lc++ \
  -framework UIKit -framework WebKit -framework Foundation \
  -framework Security -framework CoreFoundation -framework SystemConfiguration \
  -o "$APP/laufey"

cp ios/Info.plist "$APP/Info.plist"

echo "== install + launch =="
xcrun simctl install booted "$APP"
xcrun simctl launch --console-pty booted dev.laufey.ios &
sleep 4
echo "done"
