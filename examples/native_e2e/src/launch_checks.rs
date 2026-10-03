//! The first window of a freshly launched app is on screen and its page
//! renders (LAUFEY_E2E_ONLY=launch-visibility).
//!
//! An app is usually started by something that is not itself frontmost: a
//! shell, an IDE's task runner, a CI agent. On macOS 14+ such a launch can
//! leave the app inactive, and a window that was only made key and ordered
//! front then opened behind the active app's windows; WebKit and Chromium
//! read the covered window as occluded, report `document.visibilityState`
//! "hidden" and stop `requestAnimationFrame` until the window is uncovered.
//! scripts/native-e2e-run.sh --launch-visibility starts a window from another
//! process that covers the screen first (scripts/launch-occluder.swift on
//! macOS), so the launch has to bring its window to the front for these
//! checks to pass.
//!
//! The battery opens, in this order:
//!   1. a window created hidden and never shown: launching must not reveal it;
//!   2. the launch window, created hidden and shown once its page has loaded:
//!      visible, and a frame is drawn within `FRAME_BUDGET`;
//!   3. a second ordinary window, left unfocused when the launch window is
//!      focused again: visible, and frames are not throttled for lacking
//!      focus.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use laufey::{Value, Window, WindowOptions};

use crate::{check, na, wait_for};

/// How long a shown window may take to draw its first frame.
const FRAME_BUDGET: Duration = Duration::from_secs(2);

/// `expr` evaluated in `win`'s page, or `None` without an answer within 1 s.
async fn eval(win: &Window, expr: &str) -> Option<Value> {
  let (tx, rx) = tokio::sync::oneshot::channel();
  win.execute_js(
    expr,
    Some(move |r: Result<Value, Value>| {
      let _ = tx.send(r);
    }),
  );
  match tokio::time::timeout(Duration::from_secs(1), rx).await {
    Ok(Ok(Ok(v))) => Some(v),
    _ => None,
  }
}

fn as_f64(v: &Value) -> Option<f64> {
  match v {
    Value::Int(n) => Some(*n as f64),
    Value::Double(d) => Some(*d),
    _ => None,
  }
}

/// Waits until the page in `win` answers scripts (it has loaded far enough).
async fn page_answers(win: &Window) -> bool {
  let deadline = Instant::now() + Duration::from_secs(20);
  while Instant::now() < deadline {
    if let Some(Value::String(_)) = eval(win, "document.readyState").await {
      return true;
    }
    tokio::time::sleep(Duration::from_millis(100)).await;
  }
  false
}

/// The page's `document.visibilityState` once it reads `want`, or the last
/// value seen within `FRAME_BUDGET` (the engine learns about a show a moment
/// after the window system does).
async fn visibility(win: &Window, want: &str) -> String {
  let deadline = Instant::now() + FRAME_BUDGET;
  let mut last = String::from("(no answer)");
  loop {
    if let Some(Value::String(s)) = eval(win, "document.visibilityState").await
    {
      if s == want {
        return s;
      }
      last = s;
    }
    if Instant::now() >= deadline {
      return last;
    }
    tokio::time::sleep(Duration::from_millis(50)).await;
  }
}

/// How long the page took to run a `requestAnimationFrame` callback asked
/// for now, or `None` when none ran within `budget`. Measured here, so
/// script round trips count against the budget too.
async fn frame_delay(win: &Window, budget: Duration) -> Option<Duration> {
  let start = Instant::now();
  eval(
    win,
    "window.__e2eFrame = null; \
     requestAnimationFrame(() => { window.__e2eFrame = performance.now(); }); \
     true",
  )
  .await?;
  while start.elapsed() < budget {
    if let Some(v) = eval(win, "window.__e2eFrame").await {
      if as_f64(&v).is_some() {
        return Some(start.elapsed());
      }
    }
    tokio::time::sleep(Duration::from_millis(25)).await;
  }
  None
}

async fn has_focus(win: &Window) -> String {
  match eval(win, "document.hasFocus()").await {
    Some(Value::Bool(b)) => b.to_string(),
    _ => "(no answer)".into(),
  }
}

fn page(title: &str) -> String {
  format!("data:text/html,<!doctype html><title>{title}</title><p>{title}")
}

pub async fn run() {
  let backend = std::env::var("LAUFEY_E2E_BACKEND").unwrap_or_default();
  // Winit has no web engine: no page, no frames.
  let engine = backend != "winit";

  // 1. Created hidden, never shown.
  let hidden_title = "native-e2e-launch-hidden";
  let hidden = Window::new_with_options(
    320,
    240,
    WindowOptions {
      hidden: true,
      ..WindowOptions::default()
    },
  )
  .title(hidden_title)
  .load(&page(hidden_title));

  // 2. The launch window, revealed once its page has loaded.
  let launch_title = "native-e2e-launch";
  let loaded = Arc::new(AtomicBool::new(false));
  let launch = Window::new_with_options(
    800,
    600,
    WindowOptions {
      hidden: true,
      ..WindowOptions::default()
    },
  )
  .title(launch_title)
  .on_page_load({
    let loaded = loaded.clone();
    move |_| loaded.store(true, Ordering::SeqCst)
  })
  .load(&page(launch_title));
  if engine {
    // Engine-less backends never load a page; reveal at once there.
    let _ = wait_for(|| loaded.load(Ordering::SeqCst), 200, 50).await;
  }
  launch.show();
  check(
    "the launch window is visible after show",
    wait_for(|| launch.get_visible(), 100, 50).await,
  );

  if !engine {
    na("launch page visibility / frames (no web engine)");
  } else if !page_answers(&launch).await {
    check("the launch window's page answers scripts", false);
  } else {
    let vis = visibility(&launch, "visible").await;
    check(
      &format!("the launch page's visibilityState is visible (got {vis})"),
      vis == "visible",
    );
    match frame_delay(&launch, FRAME_BUDGET).await {
      Some(d) => check(
        &format!(
          "the launch page draws a frame within {} ms ({} ms)",
          FRAME_BUDGET.as_millis(),
          d.as_millis()
        ),
        true,
      ),
      None => check(
        &format!(
          "the launch page draws a frame within {} ms (none ran)",
          FRAME_BUDGET.as_millis()
        ),
        false,
      ),
    }
    eprintln!(
      "[e2e] INFO launch page document.hasFocus() = {}",
      has_focus(&launch).await
    );
  }

  // 3. A second ordinary window, then the launch window focused again: the
  //    second one is on screen but not focused.
  let second_title = "native-e2e-launch-second";
  let second = Window::new(360, 240)
    .title(second_title)
    .load(&page(second_title));
  // Mostly to the right of the launch window, so that window coming back to
  // the front covers only part of it (a fully covered window is occluded,
  // and engines rightly stop drawing it).
  let (lx, ly) = launch.get_position();
  let (lw, _) = launch.get_size();
  second.set_position(lx + lw - 100, ly + 40);
  if engine && page_answers(&second).await {
    launch.focus();
    tokio::time::sleep(Duration::from_millis(300)).await;
    eprintln!(
      "[e2e] INFO unfocused window document.hasFocus() = {}",
      has_focus(&second).await
    );
    let vis = visibility(&second, "visible").await;
    check(
      &format!("an unfocused window's page is visible (got {vis})"),
      vis == "visible",
    );
    let d = frame_delay(&second, FRAME_BUDGET).await;
    check(
      &format!(
        "an unfocused window's page draws a frame within {} ms ({})",
        FRAME_BUDGET.as_millis(),
        d.map_or("none ran".into(), |d| format!("{} ms", d.as_millis()))
      ),
      d.is_some(),
    );
  } else if engine {
    check("the unfocused window's page answers scripts", false);
  }

  // Back to 1, after both reveals: still hidden. Only the WebView backends
  // implement hidden-on-create; CEF and Winit windows show at creation.
  if backend != "webview" {
    na(&format!(
      "a hidden window stays hidden at launch ({backend} has no \
       hidden-on-create)"
    ));
  } else {
    check(
      "a hidden window stays hidden through the launch reveal",
      !hidden.get_visible(),
    );
  }

  // The windows stay open: the caller ends the process, and closing the
  // last one first would race the backend's quit-on-last-window.
  let _ = (&hidden, &launch, &second);
}
