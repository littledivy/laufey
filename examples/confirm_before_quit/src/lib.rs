//! Demonstrates `on_close_requested`: hold the window open until the user
//! confirms, only when there's actually something to lose.
//!
//! The page can mark itself "dirty" (e.g. after the user types something)
//! via the `markDirty`/`markClean` bindings. Clicking the window's close
//! button then either closes immediately (nothing unsaved) or blocks on a
//! native confirm dialog first.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use laufey::{Value, Window};

fn confirm_before_quit_main() {
  let rt = tokio::runtime::Runtime::new().unwrap();
  rt.block_on(async {
    let dirty = Arc::new(AtomicBool::new(false));

    let mark_dirty = dirty.clone();
    let mark_clean = dirty.clone();

    let _win = Window::new(700, 400)
      .title("Confirm Before Quit")
      .bind("markDirty", move |call| {
        mark_dirty.store(true, Ordering::SeqCst);
        call.resolve(Value::Null);
      })
      .bind("markClean", move |call| {
        mark_clean.store(false, Ordering::SeqCst);
        call.resolve(Value::Null);
      })
      .on_close_requested(move |event| {
        let win = Window::from_id(event.window_id);
        if !dirty.load(Ordering::SeqCst) {
          // Nothing unsaved -- close right away, same as not registering
          // a hook at all.
          win.close();
          return;
        }
        // `confirm` blocks the handler (pumping OS events so other
        // windows stay responsive) until the user answers. For a custom
        // in-page confirm UI instead of this native dialog, resolve
        // asynchronously: capture a `tokio::runtime::Handle` before this
        // closure and call `handle.spawn(...)` here instead -- see
        // docs/window-events.md.
        if win.confirm(
          "Quit without saving?",
          "You have unsaved changes. Quit anyway?",
        ) {
          win.close();
        }
        // Otherwise: do nothing. The window stays open.
      })
      .load(
        r#"data:text/html,<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>Confirm Before Quit</title>
  <style>
    body { font-family: system-ui, -apple-system, sans-serif; margin: 2rem; }
    textarea { width: 100%; height: 8rem; }
    #status { margin-top: 0.5rem; color: #666; }
  </style>
</head>
<body>
  <h1>Confirm Before Quit</h1>
  <p>Type something below, then click the window's close button.</p>
  <textarea id="notes" placeholder="Unsaved notes..."></textarea>
  <div id="status">No unsaved changes</div>
  <script>
    const notes = document.getElementById("notes");
    const status = document.getElementById("status");
    notes.addEventListener("input", () => {
      if (notes.value.length > 0) {
        Laufey.markDirty();
        status.textContent = "Unsaved changes -- closing will ask for confirmation";
      } else {
        Laufey.markClean();
        status.textContent = "No unsaved changes";
      }
    });
  </script>
</body>
</html>"#,
      );

    laufey::run().await;
  });
}

laufey::main!(confirm_before_quit_main);
