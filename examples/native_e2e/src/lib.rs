//! Backend-agnostic native-chrome / windowing e2e battery.
//!
//! This runtime is a cdylib loaded by *any* backend via `--runtime <path>` /
//! `LAUFEY_RUNTIME_PATH`, so the same assertions run under CEF, WebView, and
//! Winit. Because backends implement different subsets of the C ABI (Winit has
//! no web engine, some tray/menu fns may be absent), every assertion is
//! *capability-probed*: it emits one of
//!
//!   [e2e] PASS <name>   assertion held
//!   [e2e] FAIL <name>   supported here but wrong  -> process exits 1
//!   [e2e] N/A  <name>   capability absent on this backend (not a failure)
//!
//! This covers Layer 0 (in-process readback + event-callback round-trips),
//! menu/tray *click* round-trips via the `test_click_menu_item` C ABI hook
//! (API 30+; `N/A` on backends without it), the close-handler round-trip
//! via `test_trigger_close_requested` (API 31+; `N/A` on backends without
//! it), and the custom-scheme origin contract (a page served over the
//! battery's own `laufey-e2e://` scheme is a secure `<scheme>://<host>`
//! origin; `N/A` on engine-less backends). OS-observer *structure* checks
//! (Layer 1: the Linux D-Bus driver; macOS/Windows pending a backend hook)
//! live outside this runtime. See docs/e2e-testing.md.
//!
//! Mirrors the execution model of `examples/cef_e2e` (tokio runtime, spawned
//! event-loop pump, PASS/FAIL + exit code) so the existing runtime loader drives
//! it unchanged.

use std::io::{Read, Write};
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};
use std::sync::{Arc, Mutex};

use laufey::{MenuItem, SchemeRequest, TrayIcon, Value, Window};

static FAILED: AtomicBool = AtomicBool::new(false);

/// Minimal valid 1x1 transparent PNG. Used to give the tray a real icon so the
/// Linux D-Bus observer's `IconThemePath`/`IconName` assertion holds.
const TINY_PNG: &[u8] = &[
  0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49,
  0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06,
  0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0A, 0x49, 0x44,
  0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0D,
  0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42,
  0x60, 0x82,
];

fn check(name: &str, ok: bool) {
  if ok {
    eprintln!("[e2e] PASS {name}");
  } else {
    eprintln!("[e2e] FAIL {name}");
    FAILED.store(true, Ordering::SeqCst);
  }
}

/// Capability absent on this backend — informational, never fails the run.
fn na(name: &str) {
  eprintln!("[e2e] N/A  {name}");
}

async fn wait_for<F: Fn() -> bool>(f: F, attempts: u32, step_ms: u64) -> bool {
  for _ in 0..attempts {
    if f() {
      return true;
    }
    tokio::time::sleep(std::time::Duration::from_millis(step_ms)).await;
  }
  f()
}

fn expected_handle_type() -> (&'static str, &'static [i32]) {
  // (label, accepted type enums). Linux may be X11 or Wayland; under Xvfb it's
  // X11. See LAUFEY_WINDOW_HANDLE_* in the capi.
  if cfg!(target_os = "macos") {
    ("APPKIT", &[laufey::LAUFEY_WINDOW_HANDLE_APPKIT])
  } else if cfg!(target_os = "windows") {
    ("WIN32", &[laufey::LAUFEY_WINDOW_HANDLE_WIN32])
  } else {
    (
      "X11/WAYLAND",
      &[
        laufey::LAUFEY_WINDOW_HANDLE_X11,
        laufey::LAUFEY_WINDOW_HANDLE_WAYLAND,
      ],
    )
  }
}

// ---- custom scheme origin (technique C) --------------------------------
//
// The battery registers its own scheme and serves a page over it, then reads
// back what the page observed. On every web-engine backend a registered
// scheme must be a real origin: `location.origin == "laufey-e2e://app"`, a
// secure context (crypto.subtle works), same-origin fetch streams, storage
// works, and a cross-origin fetch carries `Origin: laufey-e2e://app`.

/// The custom scheme this battery registers. Must be declared to the CEF
/// backend at launch (`LAUFEY_CUSTOM_SCHEMES=laufey-e2e`, done by
/// scripts/native-e2e-run.sh) — see custom_schemes.h in cef/src.
const E2E_SCHEME: &str = "laufey-e2e";
const E2E_ORIGIN: &str = "laufey-e2e://app";
/// Never registered: fetching it must fail without reaching the handler.
const UNREGISTERED_SCHEME: &str = "laufey-e2e-unregistered";
/// Registered only after the first window exists, breaking the ordering
/// contract on purpose: that window must not serve it. Deliberately left out
/// of LAUFEY_CUSTOM_SCHEMES — CEF serves a scheme declared at launch in every
/// window whenever it is registered.
const LATE_SCHEME: &str = "laufey-e2e-late";
/// Body of `/stream`, sent as three separate writes with pauses in between.
const STREAM_CHUNKS: [&str; 3] = ["chunk-1|", "chunk-2|", "chunk-3|"];

/// What the custom-scheme page reported back through the `schemeReport`
/// binding (see the page script in `serve_scheme_request`).
#[derive(Clone, Debug, Default)]
struct SchemeReport {
  origin: String,
  secure: bool,
  subtle: String,
  digest_bytes: i32,
  storage: String,
  storage_prev: String,
  stream_text: String,
  stream_chunks: i32,
  echo_body: String,
}

fn arg_string(args: &[Value], i: usize) -> String {
  match args.get(i) {
    Some(Value::String(s)) => s.clone(),
    Some(Value::Bool(b)) => b.to_string(),
    Some(Value::Int(n)) => n.to_string(),
    Some(Value::Double(d)) => d.to_string(),
    Some(Value::Null) | None => String::new(),
    Some(_) => "<non-scalar>".to_string(),
  }
}

fn arg_bool(args: &[Value], i: usize) -> bool {
  args.get(i).and_then(|v| v.as_bool()).unwrap_or(false)
}

fn arg_int(args: &[Value], i: usize) -> i32 {
  match args.get(i) {
    Some(Value::Int(n)) => *n,
    Some(Value::Double(d)) => *d as i32,
    _ => 0,
  }
}

/// The page served at `laufey-e2e://app/`. Gathers the origin facts, exercises
/// crypto.subtle / localStorage / a streamed same-origin fetch / a cross-origin
/// fetch to the echo server, then reports through the `schemeReport` binding.
/// Every step is wrapped so one failure still lets the others report.
fn scheme_page_html(echo_url: &str) -> String {
  format!(
    r#"<!doctype html><html><head><meta charset="utf-8"><title>scheme</title></head><body>
<script>
// The namespace is a plain object on CEF but a callable Proxy on the WebView
// backends (typeof 'function'), so test only for the binding itself.
async function waitForBinding(name) {{
  for (let i = 0; i < 100; i++) {{
    if (typeof Laufey !== 'undefined' && typeof Laufey[name] === 'function') return;
    await new Promise(r => setTimeout(r, 50));
  }}
  throw new Error('binding never appeared: ' + name);
}}
const r = {{ origin: location.origin, secure: !!window.isSecureContext,
             subtle: typeof (self.crypto && crypto.subtle), digestBytes: 0,
             storage: '', storagePrev: '', streamText: '', streamChunks: 0,
             echoBody: '' }};
// Whatever happens, the runtime hears about it: an uncaught error is folded
// into the report instead of leaving the test to time out silently.
window.addEventListener('error', e => report('uncaught: ' + (e && e.message)));
window.addEventListener('unhandledrejection', e => report('unhandled: ' + (e && e.reason)));
let reported = false;
// Watchdog: if a step hangs (a stream that never ends, a fetch that never
// settles), report the partial state so the failure says where it stuck.
setTimeout(() => report('watchdog: stuck with ' + JSON.stringify(r)), 20000);
async function report(problem) {{
  if (reported) return;
  reported = true;
  if (problem) r.echoBody = problem + ' | ' + r.echoBody;
  await waitForBinding('schemeReport');
  await Laufey.schemeReport(r.origin, r.secure, r.subtle, r.digestBytes, r.storage,
                            r.storagePrev, r.streamText, r.streamChunks, r.echoBody);
}}
(async () => {{
  try {{
    const d = await crypto.subtle.digest('SHA-256', new TextEncoder().encode('laufey'));
    r.digestBytes = d.byteLength;
  }} catch (e) {{ r.subtle = 'error: ' + (e && e.message); }}
  try {{
    r.storagePrev = localStorage.getItem('laufey_e2e') || '';
    const v = 'run-' + Date.now();
    localStorage.setItem('laufey_e2e', v);
    r.storage = localStorage.getItem('laufey_e2e') === v ? 'ok' : 'read back mismatch';
  }} catch (e) {{ r.storage = 'error: ' + (e && e.message); }}
  try {{
    const res = await fetch('/stream');
    const reader = res.body.getReader();
    const dec = new TextDecoder();
    for (;;) {{
      const {{ value, done }} = await reader.read();
      if (done) break;
      r.streamChunks++;
      r.streamText += dec.decode(value, {{ stream: true }});
    }}
    r.streamText += dec.decode();
  }} catch (e) {{ r.streamText = 'error: ' + (e && e.message); }}
  const echoUrl = {echo_url:?};
  if (echoUrl) {{
    try {{
      const res = await fetch(echoUrl, {{ mode: 'cors' }});
      r.echoBody = await res.text();
    }} catch (e) {{ r.echoBody = 'error: ' + (e && e.message); }}
  }} else {{
    r.echoBody = 'skipped';
  }}
  await report(null);
}})().catch(e => report('script: ' + (e && e.message)));
</script></body></html>"#
  )
}

/// The page served at `app://e2e/`: proves the built-in scheme still works
/// next to the registered one, by reporting its origin through `appPing`.
const APP_PAGE_HTML: &str = r#"<!doctype html><html><head><meta charset="utf-8"><title>app</title></head><body>
<script>
(async () => {
  for (let i = 0; i < 100; i++) {
    if (typeof Laufey !== 'undefined' && typeof Laufey.appPing === 'function') {
      await Laufey.appPing(location.origin);
      return;
    }
    await new Promise(r => setTimeout(r, 50));
  }
})();
</script></body></html>"#;

fn respond(req: SchemeRequest, status: i32, content_type: &str, body: &[u8]) {
  let headers = vec![
    ("content-type".to_string(), content_type.to_string()),
    ("cache-control".to_string(), "no-store".to_string()),
  ];
  req.exchange.begin(status, &headers);
  if !body.is_empty() {
    req.exchange.write(body);
  }
  req.exchange.finish();
}

/// Scheme handler for both `laufey-e2e://app/...` and `app://e2e/...`. One
/// handler serves every registered scheme (the C ABI contract), so dispatch
/// on the URL. Every URL it sees is appended to `seen` so the negative checks
/// can prove a request never reached it. Runs on a backend thread; the
/// streamed route hops to its own thread so it never blocks the caller.
fn serve_scheme_request(
  req: SchemeRequest,
  echo_url: &str,
  seen: &Mutex<Vec<String>>,
) {
  let url = req.url.clone();
  seen.lock().unwrap().push(url.clone());
  // Strip an optional query/fragment; the routes below don't use them.
  let path = url.split(['?', '#']).next().unwrap_or("");
  match path {
    "laufey-e2e://app/" | "laufey-e2e://app" => respond(
      req,
      200,
      "text/html; charset=utf-8",
      scheme_page_html(echo_url).as_bytes(),
    ),
    "laufey-e2e://app/stream" => {
      std::thread::spawn(move || {
        let headers = vec![(
          "content-type".to_string(),
          "text/plain; charset=utf-8".to_string(),
        )];
        req.exchange.begin(200, &headers);
        for (i, chunk) in STREAM_CHUNKS.iter().enumerate() {
          if i > 0 {
            std::thread::sleep(std::time::Duration::from_millis(120));
          }
          if req.exchange.write(chunk.as_bytes()) < 0 {
            break; // consumer went away
          }
        }
        req.exchange.finish();
      });
    }
    "app://e2e/" | "app://e2e" => respond(
      req,
      200,
      "text/html; charset=utf-8",
      APP_PAGE_HTML.as_bytes(),
    ),
    // Target of the negative probes. CORS-open so that, were the engine to
    // serve the (cross-origin) request, the page's fetch would resolve
    // rather than fail on CORS — only a real refusal reads as "rejected".
    p if p.ends_with("://app/probe") => {
      let headers = vec![
        ("content-type".to_string(), "text/plain".to_string()),
        ("access-control-allow-origin".to_string(), "*".to_string()),
      ];
      req.exchange.begin(200, &headers);
      req.exchange.write(b"probe");
      req.exchange.finish();
    }
    _ => respond(req, 404, "text/plain", b"not found"),
  }
}

/// Script run in the custom-scheme window: fetch `url` and report through the
/// `schemeProbe` binding whether the engine served it (`served:<status>`) or
/// rejected the request (`rejected`).
fn fetch_probe_js(label: &str, url: &str) -> String {
  format!(
    r#"(async () => {{
  let out;
  try {{
    const res = await fetch({url:?});
    out = 'served:' + res.status;
  }} catch (e) {{
    out = 'rejected';
  }}
  await Laufey.schemeProbe({label:?}, out);
}})();"#
  )
}

/// A minimal loopback HTTP server that echoes the request's `Origin` header
/// (`origin=<value>` or `origin=none`) with `Access-Control-Allow-Origin: *`,
/// so a cross-origin fetch from the custom-scheme page proves which origin
/// the engine sends. Returns the URL to fetch, or `None` if no socket could
/// be bound (the check is then N/A).
fn start_origin_echo_server() -> Option<String> {
  let listener = std::net::TcpListener::bind("127.0.0.1:0").ok()?;
  let port = listener.local_addr().ok()?.port();
  std::thread::spawn(move || {
    for stream in listener.incoming() {
      let Ok(mut stream) = stream else { continue };
      let _ = stream.set_read_timeout(Some(std::time::Duration::from_secs(5)));
      let mut buf = Vec::new();
      let mut chunk = [0u8; 4096];
      // Read headers only (GET, no body).
      while !buf.windows(4).any(|w| w == b"\r\n\r\n") {
        match stream.read(&mut chunk) {
          Ok(0) | Err(_) => break,
          Ok(n) => buf.extend_from_slice(&chunk[..n]),
        }
      }
      let text = String::from_utf8_lossy(&buf);
      let origin = text
        .lines()
        .find_map(|l| {
          let (name, value) = l.split_once(':')?;
          name
            .trim()
            .eq_ignore_ascii_case("origin")
            .then(|| value.trim().to_string())
        })
        .unwrap_or_else(|| "none".to_string());
      let body = format!("origin={origin}");
      let response = format!(
        "HTTP/1.1 200 OK\r\ncontent-type: text/plain\r\n\
         access-control-allow-origin: *\r\ncontent-length: {}\r\n\
         connection: close\r\n\r\n{}",
        body.len(),
        body
      );
      let _ = stream.write_all(response.as_bytes());
    }
  });
  Some(format!("http://127.0.0.1:{port}/echo"))
}

fn e2e_main() {
  let rt = tokio::runtime::Runtime::new().expect("tokio runtime");
  rt.block_on(async move {
    // Pump the laufey event loop (JS-call dispatch, timers).
    tokio::spawn(async { laufey::run().await });

    // ---- C. custom scheme registration -----------------------------------
    // Registered BEFORE the first window: the engines read their scheme
    // tables when a web view is created (WebView2 fixes the set for the whole
    // process at its first environment), so this ordering is the documented
    // contract, not a convenience. "app" is served by the same handler.
    let scheme_supported = laufey::scheme_handlers_supported();
    let echo_url = start_origin_echo_server();
    let seen_urls: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));
    let make_handler = {
      let echo = echo_url.clone().unwrap_or_default();
      let seen = seen_urls.clone();
      move || {
        let (echo, seen) = (echo.clone(), seen.clone());
        move |req: SchemeRequest| serve_scheme_request(req, &echo, &seen)
      }
    };
    laufey::register_scheme_handler(E2E_SCHEME, make_handler());
    let scheme_report: Arc<Mutex<Option<SchemeReport>>> =
      Arc::new(Mutex::new(None));
    let scheme_probes: Arc<Mutex<std::collections::HashMap<String, String>>> =
      Arc::new(Mutex::new(std::collections::HashMap::new()));

    // ---- window creation + event-callback wiring -------------------------
    // Resize / move / focus handlers record the last event so we can drive the
    // setter and assert the callback round-trips (technique B).
    let resize_w = Arc::new(AtomicI32::new(0));
    let resize_h = Arc::new(AtomicI32::new(0));
    let moved = Arc::new(AtomicBool::new(false));
    let focused_seen = Arc::new(AtomicBool::new(false));
    let page_loaded = Arc::new(AtomicBool::new(false));

    let (rw, rh) = (resize_w.clone(), resize_h.clone());
    let mv = moved.clone();
    let fc = focused_seen.clone();
    let pl = page_loaded.clone();

    let win = Window::new(800, 600)
      .title("native-e2e")
      .on_resize(move |e| {
        rw.store(e.width, Ordering::SeqCst);
        rh.store(e.height, Ordering::SeqCst);
      })
      .on_move(move |_e| {
        mv.store(true, Ordering::SeqCst);
      })
      .on_focused(move |e| {
        if e.focused {
          fc.store(true, Ordering::SeqCst);
        }
      })
      // Readiness signal for the print_to_pdf test below; engine-less
      // backends never fire it, which that test treats as best-effort.
      .on_page_load(move |_e| {
        pl.store(true, Ordering::SeqCst);
      })
      // The custom-scheme page reports what it observed (technique C).
      .bind("schemeReport", {
        let report = scheme_report.clone();
        move |call| {
          let a = &call.args;
          *report.lock().unwrap() = Some(SchemeReport {
            origin: arg_string(a, 0),
            secure: arg_bool(a, 1),
            subtle: arg_string(a, 2),
            digest_bytes: arg_int(a, 3),
            storage: arg_string(a, 4),
            storage_prev: arg_string(a, 5),
            stream_text: arg_string(a, 6),
            stream_chunks: arg_int(a, 7),
            echo_body: arg_string(a, 8),
          });
          call.resolve(Value::Bool(true));
        }
      })
      // Negative probes (unregistered / late scheme) report here.
      .bind("schemeProbe", {
        let probes = scheme_probes.clone();
        move |call| {
          probes
            .lock()
            .unwrap()
            .insert(arg_string(&call.args, 0), arg_string(&call.args, 1));
          call.resolve(Value::Bool(true));
        }
      })
      // Serve the main page over the battery's own scheme so the origin
      // assertions below have something to measure. A no-op on engine-less
      // backends (Winit navigate is None).
      .load("laufey-e2e://app/");

    check("window id is nonzero", win.id() != 0);

    // ---- race probe: native handle immediately after creation ------------
    // Regression guard for denoland/deno#35785. The winit/raw backend creates
    // the OS window asynchronously, so reading the handle *right now* — with no
    // wait — is exactly the race that returned an uninitialized type 0
    // ("unknown Laufey window handle type: 0"). The getter must block until the
    // window exists and hand back the real type. Capability-probed: backends
    // that don't expose native handles legitimately return null/UNKNOWN.
    let early_handle = win.get_window_handle();
    if early_handle.is_null() {
      na("early window handle (backend doesn't expose native handles)");
    } else {
      let (label, accepted) = expected_handle_type();
      let ht = win.get_window_handle_type();
      check(
        &format!("early handle type resolved without racing (want {label}, got {ht})"),
        accepted.contains(&ht),
      );
    }

    // Give the backend a moment to realize the window on screen.
    tokio::time::sleep(std::time::Duration::from_millis(300)).await;

    // ---- A. direct state readback ---------------------------------------
    win.set_size(640, 480);
    let sized = wait_for(
      || {
        let (w, h) = win.get_size();
        (w - 640).abs() <= 2 && (h - 480).abs() <= 2
      },
      50,
      40,
    )
    .await;
    check("set_size -> get_size round-trips", sized);

    // Position is advisory: window managers may constrain it. Assert loosely.
    win.set_position(150, 170);
    tokio::time::sleep(std::time::Duration::from_millis(200)).await;
    let (px, py) = win.get_position();
    if (px - 150).abs() <= 40 && (py - 170).abs() <= 40 {
      check("set_position -> get_position round-trips", true);
    } else {
      na("set_position (window manager constrained placement)");
    }

    win.set_resizable(false);
    let not_resizable = wait_for(|| !win.get_resizable(), 25, 20).await;
    win.set_resizable(true);
    let resizable_again = wait_for(|| win.get_resizable(), 25, 20).await;
    check(
      "set_resizable false/true round-trips",
      not_resizable && resizable_again,
    );

    // always-on-top and opacity are advisory: several backends / window
    // managers don't honor a runtime toggle or don't reflect it in the getter
    // (GTK/X11, some WMs). Treat "didn't round-trip" as N/A, not a failure —
    // this is a capability probe, not a WM-conformance test.
    win.set_always_on_top(true);
    let aot = wait_for(|| win.get_always_on_top(), 25, 20).await;
    win.set_always_on_top(false);
    if aot {
      check("set_always_on_top round-trips", true);
    } else {
      na("set_always_on_top (backend/WM doesn't reflect the toggle)");
    }

    win.set_opacity(0.6);
    tokio::time::sleep(std::time::Duration::from_millis(150)).await;
    if (win.get_opacity() - 0.6).abs() < 0.05 {
      check("set_opacity -> get_opacity round-trips", true);
    } else {
      na("set_opacity (backend doesn't support a runtime opacity toggle)");
    }
    win.set_opacity(1.0);

    win.set_click_passthrough(true);
    let passthrough = wait_for(|| win.get_click_passthrough(), 25, 20).await;
    win.set_click_passthrough(false);
    if passthrough {
      check("set_click_passthrough round-trips", true);
    } else {
      na("set_click_passthrough (backend doesn't reflect the toggle)");
    }

    // Forwarding is macOS-only for now; elsewhere the setter is a no-op and
    // the getter reports false, which this treats as N/A. Whether forwarded
    // events actually arrive needs real OS input and is not probed here.
    win.set_click_passthrough_forward(true);
    let forward = wait_for(|| win.get_click_passthrough_forward(), 25, 20).await;
    win.set_click_passthrough_forward(false);
    if forward {
      check("set_click_passthrough_forward round-trips", true);
    } else {
      na("set_click_passthrough_forward (platform doesn't support forwarding)");
    }

    // Visibility.
    win.show();
    let visible = wait_for(|| win.get_visible(), 25, 20).await;
    check("show -> get_visible true", visible);

    // ---- window handle (capability-probed) -------------------------------
    // Some backends (e.g. the system WebView) intentionally don't expose native
    // window handles and return null / UNKNOWN — that's N/A, not a failure.
    let handle = win.get_window_handle();
    if handle.is_null() {
      na("window handle (backend doesn't expose native handles)");
    } else {
      check("get_window_handle non-null", true);
      let (label, accepted) = expected_handle_type();
      let ht = win.get_window_handle_type();
      check(
        &format!("window handle type is {label} (got {ht})"),
        accepted.contains(&ht),
      );
    }

    // ---- B. event-callback round-trips ----------------------------------
    win.set_size(720, 540);
    let resize_fired = wait_for(
      || {
        (resize_w.load(Ordering::SeqCst) - 720).abs() <= 4
          && (resize_h.load(Ordering::SeqCst) - 540).abs() <= 4
      },
      60,
      40,
    )
    .await;
    if resize_fired {
      check("on_resize callback round-trips", true);
    } else if resize_w.load(Ordering::SeqCst) != 0 {
      // Fired but with different dims (HiDPI scaling etc.) — still a round-trip.
      check("on_resize callback fired", true);
    } else {
      na("on_resize (backend emits no resize events)");
    }

    win.set_position(220, 240);
    let move_fired = wait_for(|| moved.load(Ordering::SeqCst), 40, 40).await;
    if move_fired {
      check("on_move callback fires", true);
    } else {
      na("on_move (backend emits no move events / WM ignored)");
    }

    win.focus();
    let focus_fired =
      wait_for(|| focused_seen.load(Ordering::SeqCst), 40, 40).await;
    if focus_fired {
      check("on_focused callback fires", true);
    } else {
      na("on_focused (no focus event in headless session)");
    }

    // ---- system integration: clipboard round-trip -----------------------
    let nonce = std::process::id();
    let payload = format!("laufey-e2e-{nonce}");
    laufey::write_clipboard_text(&payload);
    match laufey::read_clipboard_text() {
      Some(got) if got == payload => {
        check("clipboard write/read round-trips", true)
      }
      Some(_) => check("clipboard write/read round-trips", false),
      None => na("clipboard (backend has no clipboard support)"),
    }

    // ---- native chrome: menu + tray click round-trips -------------------
    // Build an app menu and a tray menu with distinct item ids, then use the
    // test hook (test_click_menu_item) to synthesize a click and assert the
    // registered on_click handler fires with the right id. This exercises the
    // backend's template -> id-registration -> dispatch plumbing without OS
    // input. On backends without the hook (API < 30) the synth returns false
    // -> N/A.
    let app_click = Arc::new(std::sync::Mutex::new(None::<String>));
    let ac = app_click.clone();
    win.set_menu(
      &[MenuItem::Submenu {
        label: "E2E".into(),
        items: vec![
          MenuItem::Item {
            label: "Ping".into(),
            id: Some("app_ping".into()),
            accelerator: None,
            enabled: true,
            checked: false,
            icon: None,
            tooltip: None,
          },
          MenuItem::Separator,
          MenuItem::Role {
            role: "quit".into(),
          },
        ],
      }],
      move |id| *ac.lock().unwrap() = Some(id.to_string()),
    );
    tokio::time::sleep(std::time::Duration::from_millis(200)).await;
    if laufey::test_click_menu_item("app_ping") {
      check(
        "app menu click round-trips to on_click with id 'app_ping'",
        app_click.lock().unwrap().as_deref() == Some("app_ping"),
      );
    } else {
      na("app menu click round-trip (backend has no test_click hook)");
    }

    // Tray: id == 0 means the backend can't create tray icons here. Keep the
    // binding alive past this block (it destroys the native icon on drop) so
    // the Layer-1 D-Bus observer can introspect it during the hold below.
    //
    // On Linux the cef/webview tray is dlopen-gated on an appindicator
    // runtime library, so id 0 normally reads as N/A. A CI leg that installs
    // the library sets LAUFEY_E2E_REQUIRE_TRAY so a stub tray FAILS instead
    // of passing silently (issue #63).
    let require_tray =
      std::env::var("LAUFEY_E2E_REQUIRE_TRAY").is_ok_and(|v| !v.is_empty());
    let tray = TrayIcon::new();
    if tray.id() == 0 {
      if require_tray {
        check(
          "create_tray_icon returned nonzero id (LAUFEY_E2E_REQUIRE_TRAY)",
          false,
        );
      } else {
        na("tray (backend has no tray support on this platform)");
      }
    } else {
      check("create_tray_icon returned nonzero id", true);
      tray.set_icon(TINY_PNG);
      let tray_click = Arc::new(std::sync::Mutex::new(None::<String>));
      let tc = tray_click.clone();
      tray.set_menu(
        &[
          MenuItem::Item {
            label: "Ping".into(),
            id: Some("tray_ping".into()),
            accelerator: None,
            enabled: true,
            checked: false,
            icon: None,
            tooltip: None,
          },
          MenuItem::Role {
            role: "quit".into(),
          },
        ],
        move |id| *tc.lock().unwrap() = Some(id.to_string()),
      );
      tokio::time::sleep(std::time::Duration::from_millis(200)).await;
      if laufey::test_click_menu_item("tray_ping") {
        check(
          "tray menu click round-trips to on_click with id 'tray_ping'",
          tray_click.lock().unwrap().as_deref() == Some("tray_ping"),
        );
      } else {
        na("tray menu click round-trip (backend has no test_click hook)");
      }
    }

    // Verifying menu/tray *structure* against the OS (that the widget was
    // really registered, not just that set_menu was accepted) needs main-thread
    // UI access. Linux is covered out-of-process by the D-Bus driver (Layer 1).
    // macOS self-AX is proven (docs/e2e-testing.md §7.2) but must run on the
    // backend's main thread — which this worker-thread runtime can't reach
    // (a dispatch_sync to the main queue deadlocks against the backend event
    // loop). It belongs behind a backend test hook; tracked as a follow-up.
    na("menu/tray OS-structure check (Linux: D-Bus driver; macOS/Windows: pending backend hook)");

    // ---- print_to_pdf smoke test (API >= 32) -----------------------------
    // Render the loaded page to a PDF and assert real PDF bytes come back
    // (`%PDF-` magic), exercising each backend's actual render path (WKWebView
    // createPDFWithConfiguration, WebView2 PrintToPdfStream, WebKitGTK
    // print-to-file, CEF DevTools Page.printToPDF). Winit has no web engine
    // and reports "unsupported" through the callback -> N/A. A backend whose
    // completion handler never fires shows up here as a FAIL on the delivery
    // assertion.
    //
    // The render races page load: WKWebView's createPDFWithConfiguration
    // fails with a generic NSError while the page is still loading, so wait
    // for on_page_load first (best effort -- engine-less backends never fire
    // it) and retry a transient error before judging; only a persistent error
    // is a real failure.
    let _ = wait_for(|| page_loaded.load(Ordering::SeqCst), 50, 100).await;
    let mut outcome: Option<Result<Vec<u8>, String>> = None;
    for attempt in 0..3u32 {
      if attempt > 0 {
        tokio::time::sleep(std::time::Duration::from_millis(700)).await;
      }
      let pdf_result =
        Arc::new(std::sync::Mutex::new(None::<Result<Vec<u8>, String>>));
      let pr = pdf_result.clone();
      win.print_to_pdf(None, move |r| *pr.lock().unwrap() = Some(r));
      if !wait_for(|| pdf_result.lock().unwrap().is_some(), 150, 100).await {
        continue; // nothing delivered this attempt; retry
      }
      let result = pdf_result.lock().unwrap().take().unwrap();
      let transient = matches!(&result, Err(e) if !e.contains("not supported"));
      outcome = Some(result);
      if !transient {
        break;
      }
    }
    match outcome {
      None => check("print_to_pdf delivers a result within 15s", false),
      Some(Ok(bytes)) => check(
        &format!("print_to_pdf returns real PDF bytes ({} bytes)", bytes.len()),
        bytes.starts_with(b"%PDF-"),
      ),
      Some(Err(e)) if e.contains("not supported") => {
        na("print_to_pdf (backend has no web engine / PDF support)")
      }
      Some(Err(e)) => {
        check(&format!("print_to_pdf succeeds (got: {e})"), false)
      }
    }

    // ---- C. custom scheme is a real origin --------------------------------
    // The page at laufey-e2e://app/ (loaded above) reports back through the
    // schemeReport binding. Engine-less backends leave register_scheme_handler
    // NULL -> N/A. On a web-engine backend a missing report is a FAIL: the
    // scheme was registered before the window, so the page must load.
    if !scheme_supported {
      na("custom scheme origin (backend has no scheme handler support)");
    } else {
      let reported =
        wait_for(|| scheme_report.lock().unwrap().is_some(), 300, 100).await;
      check("custom-scheme page loaded and reported back", reported);
      if let Some(r) = scheme_report.lock().unwrap().clone() {
        check(
          &format!("location.origin is {E2E_ORIGIN} (got {:?})", r.origin),
          r.origin == E2E_ORIGIN,
        );
        check("custom-scheme page is a secure context", r.secure);
        check(
          &format!(
            "crypto.subtle works on the custom scheme (typeof {}, digest {} bytes)",
            r.subtle, r.digest_bytes
          ),
          r.subtle == "object" && r.digest_bytes == 32,
        );
        check(
          &format!("localStorage works on the custom-scheme origin ({})", r.storage),
          r.storage == "ok",
        );
        // Persistence across launches is only observable on the second run;
        // report it so a manual two-run check has the evidence.
        eprintln!(
          "[e2e] INFO localStorage value from a previous run: {:?}",
          r.storage_prev
        );
        let expected_stream: String = STREAM_CHUNKS.concat();
        check(
          &format!(
            "same-origin fetch over the custom scheme delivers the streamed body (got {:?})",
            r.stream_text
          ),
          r.stream_text == expected_stream,
        );
        // Chunk arrival is engine-dependent (WebView2 buffers the whole
        // response before answering); informational only.
        eprintln!(
          "[e2e] INFO streamed response arrived in {} read(s) ({} writes)",
          r.stream_chunks,
          STREAM_CHUNKS.len()
        );
        match &echo_url {
          Some(_) => check(
            &format!(
              "cross-origin fetch from the custom scheme sends Origin: {E2E_ORIGIN} (got {:?})",
              r.echo_body
            ),
            r.echo_body == format!("origin={E2E_ORIGIN}"),
          ),
          None => na("cross-origin Origin header (could not bind a loopback echo server)"),
        }
      }

      // Negative: a scheme nobody registered is not served — the fetch
      // fails and the request never reaches the handler.
      let probe = |label: &'static str, url: String| {
        let probes = scheme_probes.clone();
        win.execute_js(&fetch_probe_js(label, &url), None::<fn(_)>);
        async move {
          wait_for(|| probes.lock().unwrap().contains_key(label), 100, 100)
            .await;
          probes.lock().unwrap().get(label).cloned().unwrap_or_default()
        }
      };
      let reached = |scheme: &str| {
        let prefix = format!("{scheme}:");
        seen_urls
          .lock()
          .unwrap()
          .iter()
          .any(|u| u.starts_with(&prefix))
      };
      let got = probe(
        "unregistered",
        format!("{UNREGISTERED_SCHEME}://app/probe"),
      )
      .await;
      check(
        &format!("an unregistered scheme is not served (fetch: {got:?})"),
        got == "rejected" && !reached(UNREGISTERED_SCHEME),
      );

      // Negative: registering a scheme after the window exists breaks the
      // ordering contract, so that window must not serve it (the backend
      // logs a warning on stderr). WebKitGTK is the exception: its schemes
      // live on the shared web context, which applies a late registration to
      // existing web views too — reported, not asserted, there.
      laufey::register_scheme_handler(LATE_SCHEME, make_handler());
      tokio::time::sleep(std::time::Duration::from_millis(300)).await;
      let got = probe("late", format!("{LATE_SCHEME}://app/probe")).await;
      let backend = std::env::var("LAUFEY_E2E_BACKEND").unwrap_or_default();
      if cfg!(target_os = "linux") && backend == "webview" {
        eprintln!(
          "[e2e] INFO WebKitGTK: scheme registered after the first window, \
           fetched from it: {got:?}"
        );
      } else {
        check(
          &format!(
            "a scheme registered after the first window is not served by it (fetch: {got:?})"
          ),
          got == "rejected" && !reached(LATE_SCHEME),
        );
      }
    }

    // ---- close-requested handler round-trip --------------------------------
    // A second window (kept separate from `win`, which must survive to
    // shutdown). Registers an on_close_requested handler that *stashes* the
    // window_id instead of resolving inline — proving resolution genuinely
    // works from outside the handler (any thread, any time), not just
    // synchronously inline. Synthesizes the close via
    // test_trigger_close_requested (API >= 31, implemented by every in-tree
    // backend — init_api's version check guarantees it's present, so a
    // failure here is a regression, never a missing hook).
    //
    // Its page is served over the built-in "app" scheme by the same handler
    // as laufey-e2e://, proving "app" keeps working next to a registered
    // scheme (technique C, backward compatibility).
    let app_origin: Arc<Mutex<Option<String>>> = Arc::new(Mutex::new(None));
    let close_win = Window::new(200, 150)
      .title("native-e2e-close")
      .bind("appPing", {
        let origin = app_origin.clone();
        move |call| {
          *origin.lock().unwrap() = Some(arg_string(&call.args, 0));
          call.resolve(Value::Bool(true));
        }
      })
      .load("app://e2e/");
    let close_win_id = close_win.id();
    if scheme_supported {
      let pinged =
        wait_for(|| app_origin.lock().unwrap().is_some(), 150, 100).await;
      let got = app_origin.lock().unwrap().clone().unwrap_or_default();
      check(
        &format!("built-in app:// scheme still served next to the registered one (origin {got:?})"),
        pinged && got == "app://e2e",
      );
    }
    let baseline_sized = wait_for(
      || {
        let (w, _h) = close_win.get_size();
        w != 0
      },
      50,
      40,
    )
    .await;

    let close_handler_fired = Arc::new(AtomicBool::new(false));
    let pending_close: Arc<std::sync::Mutex<Option<u32>>> =
      Arc::new(std::sync::Mutex::new(None));
    let close_win = {
      let fired = close_handler_fired.clone();
      let pending = pending_close.clone();
      close_win.on_close_requested(move |event| {
        fired.store(true, Ordering::SeqCst);
        *pending.lock().unwrap() = Some(event.window_id);
      })
    };

    let still_open_after_defer =
      laufey::test_trigger_close_requested(close_win_id);
    if !baseline_sized {
      // Precondition, not a close-dispatch check: without a sized baseline
      // the stays-open/closed probes below are meaningless.
      na("close handler (precondition failed: close_win never reported a nonzero size)");
    } else {
      // The hook itself is guaranteed present: init_api rejects any backend
      // whose API version differs from the current one, and every in-tree
      // backend implements test_trigger_close_requested. Dispatch is synchronous,
      // so a handler that didn't fire is a real regression — fail, don't
      // N/A.
      check(
        "on_close_requested handler fired for synthesized close",
        close_handler_fired.load(Ordering::SeqCst),
      );
      check(
        "close handler defers — window stays open",
        still_open_after_defer,
      );
      let (w, _h) = close_win.get_size();
      check("window state still readable after deferred close", w != 0);

      // Resolve the stashed window_id — from here, not from inside the
      // handler — and confirm the window actually closes.
      let pending_id = pending_close.lock().unwrap().take();
      if let Some(id) = pending_id {
        Window::from_id(id).close();
      }
      let closed = wait_for(
        || {
          let (w, h) = close_win.get_size();
          w == 0 && h == 0
        },
        50,
        40,
      )
      .await;
      check(
        "closing from outside the on_close_requested handler actually closes the window",
        closed,
      );
    }

    // ---- Layer-1 hold ----------------------------------------------------
    // When driven by the D-Bus observer (native_e2e_driver), stay alive with
    // the tray + menu registered so it can read the StatusNotifierItem, walk
    // the dbusmenu layout, and fire a menu Event that round-trips to the
    // `on_click` above. The driver kills us when it's done.
    if std::env::var_os("LAUFEY_E2E_HOLD").is_some() {
      eprintln!("[e2e] holding for Layer-1 observer");
      tokio::time::sleep(std::time::Duration::from_secs(8)).await;
    }

    // ---- shutdown --------------------------------------------------------
    // Decide the result and terminate immediately with a deterministic exit
    // code. We deliberately skip close()/quit(): tearing the window/webview
    // down on the backend's main thread can crash or race and clobber the exit
    // code (e.g. SIGTRAP -> 133), which would corrupt the CI signal. The OS
    // reclaims everything on exit. `_ = &win;` keeps the window alive to here.
    let _ = &win;
    let failed = FAILED.load(Ordering::SeqCst);
    eprintln!("[e2e] OVERALL {}", if failed { "FAIL" } else { "PASS" });
    let _ = std::io::Write::flush(&mut std::io::stderr());
    // _exit avoids running C++ static destructors / atexit handlers in the
    // backend, which is where the teardown crash lives.
    unsafe { libc_exit(if failed { 1 } else { 0 }) };
  });
}

extern "C" {
  #[link_name = "_exit"]
  fn libc_exit(code: i32) -> !;
}

laufey::main!(e2e_main);
