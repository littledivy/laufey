#!/usr/bin/env bash
#
# Run the backend-agnostic native_e2e_runtime under a given backend and
# propagate its PASS/FAIL exit code. See docs/e2e-testing.md.
#
#   scripts/native-e2e-run.sh <winit|webview|cef> [--layer1|--headless-worker]
#
# --layer1 (Linux only) wraps the run in the D-Bus StatusNotifier/dbusmenu
# observer (native_e2e_driver) under a private session bus.
#
# --headless-worker (macOS WebView only) starts the backend as a headless
# worker (`run <script>`) and checks that it waits for the runtime to return.
set -euo pipefail

backend="${1:?usage: native-e2e-run.sh <winit|webview|cef> [--layer1|--headless-worker]}"
mode="${2:-}"

# Locate the runtime cdylib (.so / .dylib / .dll).
rt=""
for c in \
  target/release/libnative_e2e_runtime.so \
  target/release/libnative_e2e_runtime.dylib \
  target/release/native_e2e_runtime.dll; do
  if [ -f "$c" ]; then rt="$PWD/$c"; break; fi
done
[ -n "$rt" ] || { echo "native_e2e_runtime cdylib not found (build it first)"; exit 1; }
export LAUFEY_RUNTIME_PATH="$rt"
# Lets the runtime skip checks a backend does not support yet.
export LAUFEY_E2E_BACKEND="$backend"

# Resolve the backend binary (handles macOS .app bundles).
case "$backend" in
  winit)
    bin="$(ls target/release/laufey_winit target/release/laufey_winit.exe 2>/dev/null | head -1 || true)" ;;
  webview)
    bin="$(ls \
      webview/build/laufey_webview.app/Contents/MacOS/laufey_webview \
      webview/build/laufey_webview \
      webview/build/laufey_webview.exe 2>/dev/null | head -1 || true)" ;;
  cef)
    bin="$(ls \
      cef/build/Release/laufey.app/Contents/MacOS/laufey \
      cef/build/Release/laufey \
      cef/build/Release/laufey.exe 2>/dev/null | head -1 || true)" ;;
  *) echo "unknown backend: $backend"; exit 2 ;;
esac
[ -n "$bin" ] || { echo "backend binary for '$backend' not found (build it first)"; exit 1; }
echo "== native-e2e: backend=$backend bin=$bin runtime=$rt =="

is_linux() { [ "$(uname -s)" = "Linux" ]; }

if [ "$mode" = "--layer1" ]; then
  is_linux || { echo "--layer1 is Linux-only"; exit 2; }
  export LAUFEY_E2E_HOLD=1
  driver="$(ls target/release/native_e2e_driver 2>/dev/null | head -1 || true)"
  [ -n "$driver" ] || { echo "native_e2e_driver not built"; exit 1; }
  exec xvfb-run -a dbus-run-session -- "$driver" "$bin"
fi

if [ "$mode" = "--headless-worker" ]; then
  rc=0
  out="$(LAUFEY_E2E_ONLY=headless-worker "$bin" run native-e2e-worker 2>&1)" ||
    rc=$?
  echo "$out"
  if [ "$rc" -ne 0 ]; then
    echo "[e2e] FAIL the headless worker exited with status $rc"
    exit 1
  fi
  if ! grep -q '^\[e2e\] PASS a headless worker' <<<"$out"; then
    echo "[e2e] FAIL the headless worker exited before its runtime returned"
    exit 1
  fi
  exit 0
fi

# Layer 0: run the backend directly. On Linux, headless via Xvfb + a private
# session bus (some tray impls need a session bus to even initialize).
if is_linux; then
  exec xvfb-run -a dbus-run-session -- "$bin"
fi
exec "$bin"
