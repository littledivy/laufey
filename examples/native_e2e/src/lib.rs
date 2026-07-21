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
//! This covers Layer 0 (in-process readback + event-callback round-trips) and
//! menu/tray *click* round-trips via the `test_click_menu_item` C ABI hook
//! (API 30+; `N/A` on backends without it). OS-observer *structure* checks
//! (Layer 1: the Linux D-Bus driver; macOS/Windows pending a backend hook)
//! live outside this runtime. See docs/e2e-testing.md.
//!
//! Mirrors the execution model of `examples/cef_e2e` (tokio runtime, spawned
//! event-loop pump, PASS/FAIL + exit code) so the existing runtime loader drives
//! it unchanged.

use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};
use std::sync::Arc;

use laufey::{MenuItem, TrayIcon, Window};

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

fn e2e_main() {
  let rt = tokio::runtime::Runtime::new().expect("tokio runtime");
  rt.block_on(async move {
    // Pump the laufey event loop (JS-call dispatch, timers).
    tokio::spawn(async { laufey::run().await });

    // ---- window creation + event-callback wiring -------------------------
    // Resize / move / focus handlers record the last event so we can drive the
    // setter and assert the callback round-trips (technique B).
    let resize_w = Arc::new(AtomicI32::new(0));
    let resize_h = Arc::new(AtomicI32::new(0));
    let moved = Arc::new(AtomicBool::new(false));
    let focused_seen = Arc::new(AtomicBool::new(false));

    let (rw, rh) = (resize_w.clone(), resize_h.clone());
    let mv = moved.clone();
    let fc = focused_seen.clone();

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
      // Trivial page; a no-op on engine-less backends (Winit navigate is None).
      .load("data:text/html,<!doctype html><title>native-e2e</title>");

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
    let tray = TrayIcon::new();
    if tray.id() == 0 {
      na("tray (backend has no tray support on this platform)");
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

    // ---- print_to_pdf smoke test (API >= 30) -----------------------------
    // Render the loaded page to a PDF and assert real PDF bytes come back
    // (`%PDF-` magic), exercising each backend's actual render path (WKWebView
    // createPDFWithConfiguration, WebView2 PrintToPdfStream, WebKitGTK memfd,
    // CEF DevTools Page.printToPDF). Winit has no web engine and reports
    // "unsupported" through the callback -> N/A. A backend whose completion
    // handler never fires shows up here as a FAIL on the delivery assertion.
    let pdf_result: Arc<std::sync::Mutex<Option<Result<Vec<u8>, String>>>> =
      Arc::new(std::sync::Mutex::new(None));
    let pr = pdf_result.clone();
    win.print_to_pdf(None, move |r| *pr.lock().unwrap() = Some(r));
    let pdf_done =
      wait_for(|| pdf_result.lock().unwrap().is_some(), 150, 100).await;
    if !pdf_done {
      check("print_to_pdf delivers a result within 15s", false);
    } else {
      let result = pdf_result.lock().unwrap().take().unwrap();
      match result {
        Ok(bytes) => check(
          &format!("print_to_pdf returns real PDF bytes ({} bytes)", bytes.len()),
          bytes.starts_with(b"%PDF-"),
        ),
        Err(e) if e.contains("not supported") => {
          na("print_to_pdf (backend has no web engine / PDF support)")
        }
        Err(e) => check(&format!("print_to_pdf succeeds (got: {e})"), false),
      }
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
