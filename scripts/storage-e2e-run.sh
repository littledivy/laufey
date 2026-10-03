#!/usr/bin/env bash
#
# Per-app web storage e2e: relaunches a backend with the storage_e2e_runtime
# under different LAUFEY_APP_ID / LAUFEY_DATA_DIR settings and asserts that
# localStorage + cookies persist per app, stay isolated between apps, and that
# an unconfigured launch behaves as before. See docs/app-data.md.
#
#   scripts/storage-e2e-run.sh <webview|cef>
#
# Build first: `cargo build --release -p storage_e2e_runtime` and the backend.
# Uses fixed app ids (dev.laufey.e2e.storage-{a,b}) and removes their data
# directories when done; every run writes a fresh random value, so data left
# by an earlier run can't make a check pass.
set -euo pipefail

backend="${1:?usage: storage-e2e-run.sh <webview|cef>}"

rt=""
for c in \
  target/release/libstorage_e2e_runtime.so \
  target/release/libstorage_e2e_runtime.dylib \
  target/release/storage_e2e_runtime.dll; do
  if [ -f "$c" ]; then rt="$PWD/$c"; break; fi
done
[ -n "$rt" ] || { echo "storage_e2e_runtime cdylib not found (build it first)"; exit 1; }
export LAUFEY_RUNTIME_PATH="$rt"

case "$backend" in
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

os="$(uname -s)"
case "$os" in
  Darwin) platform=macos ;;
  Linux) platform=linux ;;
  *) platform=windows ;;
esac

# Scratch root at a fixed path: WKWebView keys its store on the
# LAUFEY_DATA_DIR string, so a fixed path reuses one store across runs.
if [ "$platform" = windows ]; then
  scratch="$(cygpath -u "${TEMP:-/tmp}")/laufey-storage-e2e"
else
  scratch="${TMPDIR:-/tmp}"
  scratch="${scratch%/}/laufey-storage-e2e"
fi
rm -rf "$scratch"
mkdir -p "$scratch/logs"

# Where LAUFEY_APP_ID resolves to. On Linux point XDG_DATA_HOME into the
# scratch dir so the test exercises the XDG path without touching ~/.local.
case "$platform" in
  macos) base="$HOME/Library/Application Support" ;;
  linux)
    export XDG_DATA_HOME="$scratch/xdg-data"
    mkdir -p "$XDG_DATA_HOME"
    base="$XDG_DATA_HOME" ;;
  windows) base="$(cygpath -u "$LOCALAPPDATA")" ;;
esac

# Backend subdirectory holding the engine profile (none for WKWebView, which
# uses an identifier-based store instead of a directory).
case "$backend/$platform" in
  cef/*) sub=CEF ;;
  webview/linux) sub=WebKitGTK ;;
  webview/windows) sub=WebView2 ;;
  webview/macos) sub="" ;;
esac

id_a=dev.laufey.e2e.storage-a
id_b=dev.laufey.e2e.storage-b
explicit="$scratch/explicit-dir"
if [ "$platform" = windows ]; then
  explicit_native="$(cygpath -w "$explicit")"
else
  explicit_native="$explicit"
fi
port=$((20000 + RANDOM % 20000))
value="v$RANDOM$RANDOM$$"

# Engine helper processes (WebView2, CEF) can hold profile files for a moment
# after the backend exits, so retry, and never fail the run over cleanup.
remove() {
  for _ in 1 2 3 4 5; do
    rm -rf "$1" 2>/dev/null && return 0
    sleep 1
  done
  echo "[storage-e2e] warning: could not remove $1"
}
cleanup() {
  remove "$base/$id_a"
  remove "$base/$id_b"
  [ -n "${KEEP_SCRATCH:-}" ] || remove "$scratch"
}
trap cleanup EXIT
rm -rf "$base/$id_a" "$base/$id_b"

failed=0
pass() { echo "[storage-e2e] PASS $*"; }
fail() { echo "[storage-e2e] FAIL $*"; failed=1; }

# launch <log-name> <timeout-s> [VAR=value ...] -- runs one backend launch with
# only the given LAUFEY_* storage variables set; waits for it to exit.
launch() {
  local name="$1" secs="$2"
  shift 2
  local log="$scratch/logs/$name.log"
  local cmd=(env -u LAUFEY_APP_ID -u LAUFEY_DATA_DIR -u LAUFEY_E2E_STORAGE_EXPECT
    -u LAUFEY_E2E_STORAGE_EXPECT_NOT -u LAUFEY_E2E_STORAGE_HOLD_MS
    LAUFEY_E2E_STORAGE_PORT="$port" "$@")
  if [ "$platform" = linux ]; then
    cmd+=(xvfb-run -a dbus-run-session -- "$bin")
  else
    cmd+=("$bin")
  fi
  echo "== [$name] ${*}"
  "${cmd[@]}" >"$log" 2>&1 &
  local pid=$!
  ( sleep "$secs"; kill -9 "$pid" 2>/dev/null ) &
  local watchdog=$!
  local rc=0
  wait "$pid" || rc=$?
  kill "$watchdog" 2>/dev/null || true
  wait "$watchdog" 2>/dev/null || true
  grep -E '^\[e2e\]|^laufey:' "$log" | sed "s/^/    /" || true
  echo "    (exit $rc)"
}

# step <name> [VAR=value ...] -- one launch that must report OVERALL PASS.
step() {
  local name="$1"
  shift
  launch "$name" 90 "$@"
  if grep -q '^\[e2e\] OVERALL PASS' "$scratch/logs/$name.log"; then
    pass "$name"
  else
    fail "$name (see $scratch/logs/$name.log)"
    if [ -n "${CI:-}" ]; then sed 's/^/    | /' "$scratch/logs/$name.log" | tail -60; fi
  fi
}

echo "== storage-e2e: backend=$backend bin=$bin port=$port value=$value"

# (a) persistence: the same app id reads back what an earlier launch wrote.
step write-a LAUFEY_APP_ID="$id_a" LAUFEY_E2E_STORAGE_MODE=write \
  LAUFEY_E2E_STORAGE_VALUE="$value"
step read-a LAUFEY_APP_ID="$id_a" LAUFEY_E2E_STORAGE_MODE=read \
  LAUFEY_E2E_STORAGE_EXPECT="$value"

# (b) isolation: another app id, same origin, doesn't see it.
step read-b LAUFEY_APP_ID="$id_b" LAUFEY_E2E_STORAGE_MODE=read \
  LAUFEY_E2E_STORAGE_EXPECT_NOT="$value"

# LAUFEY_DATA_DIR: persists on its own, isolated from the app-id store.
step write-dir LAUFEY_DATA_DIR="$explicit_native" LAUFEY_E2E_STORAGE_MODE=write \
  LAUFEY_E2E_STORAGE_VALUE="${value}d"
step read-dir LAUFEY_DATA_DIR="$explicit_native" LAUFEY_E2E_STORAGE_MODE=read \
  LAUFEY_E2E_STORAGE_EXPECT="${value}d"

# (c) unconfigured: nothing of the configured apps leaks into the default
# store, and nothing is created for it.
step read-default LAUFEY_E2E_STORAGE_MODE=read \
  LAUFEY_E2E_STORAGE_EXPECT_NOT="$value"
if [ "$backend" = cef ]; then
  # CEF's unconfigured profile is a throwaway per-process temp dir, as before.
  step write-default LAUFEY_E2E_STORAGE_MODE=write \
    LAUFEY_E2E_STORAGE_VALUE="${value}u"
  step read-default-again LAUFEY_E2E_STORAGE_MODE=read \
    LAUFEY_E2E_STORAGE_EXPECT_NOT="${value}u"
fi

# On-disk layout.
if [ -n "$sub" ]; then
  if [ -d "$base/$id_a/$sub" ]; then pass "profile at <app data>/$id_a/$sub"; else fail "no profile at $base/$id_a/$sub"; fi
  if [ -d "$explicit/$sub" ]; then pass "profile at \$LAUFEY_DATA_DIR/$sub"; else fail "no profile at $explicit/$sub"; fi
  if [ "$sub" = WebKitGTK ]; then
    if [ -f "$base/$id_a/WebKitGTK/data/cookies.sqlite" ]; then
      pass "persistent cookie store at WebKitGTK/data/cookies.sqlite"
    else
      fail "no cookie store at $base/$id_a/WebKitGTK/data/cookies.sqlite"
    fi
  fi
  if [ "$platform" != windows ]; then
    mode="$(stat -c %a "$base/$id_a/$sub" 2>/dev/null || stat -f %Lp "$base/$id_a/$sub")"
    if [ "$mode" = 700 ]; then pass "profile dir is owner-only (0700)"; else fail "profile dir mode is $mode, want 700"; fi
  fi
else
  if [ ! -e "$base/$id_a" ]; then pass "no data dir created (WKWebView uses an identifier-based store)"; else fail "unexpected $base/$id_a"; fi
fi

# CEF allows one process per profile: a second launch of the same app must
# exit cleanly while the first keeps running.
if [ "$backend" = cef ]; then
  launch hold-a 90 LAUFEY_APP_ID="$id_a" LAUFEY_E2E_STORAGE_MODE=read \
    LAUFEY_E2E_STORAGE_EXPECT="$value" LAUFEY_E2E_STORAGE_HOLD_MS=15000 &
  hold_pid=$!
  for _ in $(seq 1 60); do
    grep -q 'page loaded' "$scratch/logs/hold-a.log" 2>/dev/null && break
    sleep 0.5
  done
  launch second-a 30 LAUFEY_APP_ID="$id_a" LAUFEY_E2E_STORAGE_MODE=read
  if grep -q 'another instance is already running' "$scratch/logs/second-a.log" &&
    ! grep -q '^\[e2e\]' "$scratch/logs/second-a.log"; then
    pass "second instance of the same app exits without starting"
  else
    fail "second instance (see $scratch/logs/second-a.log)"
  fi
  wait "$hold_pid" || true
  if grep -q '^\[e2e\] OVERALL PASS' "$scratch/logs/hold-a.log"; then
    pass "first instance unaffected by the second launch"
  else
    fail "first instance (see $scratch/logs/hold-a.log)"
  fi
fi

if [ "$failed" = 0 ]; then
  echo "[storage-e2e] OVERALL PASS"
else
  echo "[storage-e2e] OVERALL FAIL"
  exit 1
fi
