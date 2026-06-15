//! End-to-end harness for the CEF backend.
//!
//! Designed to run under `xvfb-run` on Linux CI. Exercises the parts
//! of the laufey contract a unit test can't reach: the C ABI handshake,
//! window/webview creation, navigate() actually loading a page, the
//! JS bindings round-trip, executeJs returning a value, and clean
//! shutdown.
//!
//! Doesn't drive dialogs (alert/confirm/prompt) because those need
//! either real OS input or a backend-side test stub — neither exists
//! today. Adding a `LAUFEY_TEST_MODE` flag in the CEF backend is the
//! next step if dialog coverage in CI matters.
//!
//! Exit status 0 on PASS, 1 on FAIL. Each check prints
//! `[e2e] PASS …` / `[e2e] FAIL …`.

use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::Arc;

use laufey::{Value, Window};
use tokio::sync::oneshot;

static FAILED: AtomicBool = AtomicBool::new(false);

fn check(name: &str, ok: bool) {
  if ok {
    eprintln!("[e2e] PASS {name}");
  } else {
    eprintln!("[e2e] FAIL {name}");
    FAILED.store(true, Ordering::SeqCst);
  }
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

fn e2e_main() {
  let rt = tokio::runtime::Runtime::new().expect("tokio runtime");
  rt.block_on(async move {
        // Spawn the laufey event-loop pump. Without this, queued JS calls
        // (Laufey.report, Laufey.add) never reach our bind handlers because
        // poll_js_calls() never gets called.
        tokio::spawn(async { laufey::run().await });

        // Counter of binding invocations from the page. The page below
        // calls `Laufey.report("loaded")` on DOMContentLoaded; if the
        // binding pipeline works end-to-end (CEF process model, IPC
        // back to the browser process, JsCall dispatch through capi)
        // we see the counter increment.
        let binding_calls = Arc::new(AtomicU32::new(0));

        // Capture the args of the LAST report() call for shape pinning.
        let last_label: Arc<std::sync::Mutex<Option<String>>> =
            Arc::new(std::sync::Mutex::new(None));

        let win = Window::new(800, 600).title("cef-e2e");
        check("window id is nonzero", win.id() != 0);

        let bc = binding_calls.clone();
        let ll = last_label.clone();
        let win = win.bind("report", move |call| {
            bc.fetch_add(1, Ordering::SeqCst);
            if let Some(Value::String(s)) = call.args.first() {
                *ll.lock().unwrap() = Some(s.clone());
            }
            call.resolve(Value::Bool(true));
        });

        // A binding that takes two ints and returns their sum — proves
        // multi-arg + return-value round-trip.
        let win = win.bind("add", |call| {
            let a = call.args.first().and_then(|v| v.as_int()).unwrap_or(0);
            let b = call.args.get(1).and_then(|v| v.as_int()).unwrap_or(0);
            call.resolve(Value::Int(a + b));
        });

        // Trigger from the page itself: call report("loaded") on load,
        // then call add(2, 40) and verify the sum.
        let html = r#"data:text/html,<!doctype html>
<html><body>
<script>
// CEF registers bindings asynchronously via IPC from the browser
// process. They may not be on Laufey.* the moment this script runs —
// wait up to 5s for each.
async function waitForBinding(name) {
  for (let i = 0; i < 100; i++) {
    if (typeof Laufey === 'object' && typeof Laufey[name] === 'function') return;
    await new Promise(r => setTimeout(r, 50));
  }
  throw new Error('binding never appeared: ' + name);
}
(async () => {
  try {
    await waitForBinding('report');
    await Laufey.report('loaded');
    await waitForBinding('add');
    const sum = await Laufey.add(2, 40);
    await Laufey.report('sum=' + sum);
  } catch (e) {
    document.body.innerText = 'JS ERROR: ' + (e && e.message);
    try { await Laufey.report('js-error:' + (e && e.message)); } catch(_) {}
  }
})();
</script>
</body></html>"#;

        let win = win.load(html);

        // Step 1: poll until the page-side script has actually run.
        // We use executeJs to read a sentinel from the page's window
        // object — this proves CEF loaded the data URL and is running
        // JS at all, before we get into binding-pipeline assertions.
        for attempt in 0..60u32 {
            let (probe_tx, probe_rx) = oneshot::channel::<Result<Value, Value>>();
            win.execute_js(
                "(typeof Laufey === 'object' && typeof document !== 'undefined') ? 'ready' : 'no'",
                Some(move |r: Result<Value, Value>| {
                    let _ = probe_tx.send(r);
                }),
            );
            if let Ok(Ok(Ok(Value::String(s)))) = tokio::time::timeout(
                std::time::Duration::from_millis(500),
                probe_rx,
            )
            .await
            {
                if s == "ready" {
                    eprintln!("[e2e] page ready after {} attempts", attempt + 1);
                    break;
                }
            }
            tokio::time::sleep(std::time::Duration::from_millis(200)).await;
        }

        // CEF spins up multiple processes (browser, renderer, GPU,
        // ...) and the renderer needs to come up + execute the inline
        // script. Allow up to ~10s on slow CI runners.
        let saw_loaded = wait_for(
            || {
                last_label
                    .lock()
                    .unwrap()
                    .as_deref()
                    .map(|s| s == "loaded" || s.starts_with("sum="))
                    .unwrap_or(false)
            },
            200,
            50,
        )
        .await;
        check("Laufey.report('loaded') round-tripped to Deno binding", saw_loaded);

        let saw_sum = wait_for(
            || {
                last_label
                    .lock()
                    .unwrap()
                    .as_deref()
                    .map(|s| s == "sum=42")
                    .unwrap_or(false)
            },
            200,
            50,
        )
        .await;
        check("Laufey.add(2, 40) returned 42 to JS", saw_sum);

        check(
            "at least two binding invocations recorded",
            binding_calls.load(Ordering::SeqCst) >= 2,
        );

        // executeJs round-trip. The capi has an `execute_js` API with
        // optional callback that delivers the JS expression's result
        // back to Rust.
        let (tx, rx) = oneshot::channel::<Result<Value, Value>>();
        win.execute_js(
            "(() => { return 7 * 6; })()",
            Some(move |r: Result<Value, Value>| {
                let _ = tx.send(r);
            }),
        );
        let exec_result = tokio::time::timeout(
            std::time::Duration::from_secs(5),
            rx,
        )
        .await;
        match exec_result {
            Ok(Ok(Ok(Value::Int(42)))) => check("executeJs returned 42", true),
            Ok(Ok(Ok(other))) => {
                eprintln!("[e2e] unexpected executeJs result variant");
                check("executeJs returned 42", matches!(other, Value::Int(42)));
            }
            Ok(Ok(Err(e))) => {
                eprintln!("[e2e] executeJs JS error: {:?}", e.as_string());
                check("executeJs returned 42", false);
            }
            Ok(Err(_)) => {
                check("executeJs callback fired before timeout", false);
            }
            Err(_) => check("executeJs returned within 5s", false),
        }

        // Shut down: close the window then quit the backend.
        win.close();
        tokio::time::sleep(std::time::Duration::from_millis(200)).await;
        laufey::quit();

        if FAILED.load(Ordering::SeqCst) {
            eprintln!("[e2e] OVERALL FAIL");
            std::process::exit(1);
        }
        eprintln!("[e2e] OVERALL PASS");
    });
}

laufey::main!(e2e_main);
