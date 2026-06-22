// A native-UI demo for the engine-free winit backend: no web content, just
// real AppKit widgets (NSStackView / NSTextField / NSButton) built through
// laufey's native-widget API.
//
// Build:  cargo build --release -p native_ui_runtime
// Run:    LAUFEY_RUNTIME_PATH=target/release/libnative_ui_runtime.dylib \
//           target/release/laufey_winit

use std::sync::atomic::{AtomicI64, Ordering};

use laufey::{Widget, WidgetKind, Window};

static COUNT: AtomicI64 = AtomicI64::new(0);

fn native_main() {
  let rt = tokio::runtime::Runtime::new().unwrap();
  rt.block_on(async {
    let win = Window::new(360, 200).title("laufey — native UI");

    // Build the tree: a vertical stack holding a label and two buttons.
    let root = win.widget(WidgetKind::VStack);
    let label = win.widget(WidgetKind::Label).text("Count: 0");
    let label_id = label.id();

    let inc = win
      .widget(WidgetKind::Button)
      .text("Increment")
      .on_click(move || update(label_id, 1));
    let dec = win
      .widget(WidgetKind::Button)
      .text("Decrement")
      .on_click(move || update(label_id, -1));

    root.add_child(&label);
    root.add_child(&inc);
    root.add_child(&dec);
    win.set_root_widget(&root);

    laufey::run().await;
  });
}

fn update(label_id: u32, delta: i64) {
  let n = COUNT.fetch_add(delta, Ordering::SeqCst) + delta;
  // Re-wrap the label id as a Widget handle to push new text.
  Widget::from_id(label_id).set_text(&format!("Count: {n}"));
}

laufey::main!(native_main);
