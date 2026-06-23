// A laufey app running on iOS. Same capi as the desktop examples — Window +
// bind() — but linked statically into an iOS app (see webview/src/main_ios.mm).

use std::collections::HashMap;
use std::sync::atomic::{AtomicI64, Ordering};

use laufey::{Value, Window};

static COUNTER: AtomicI64 = AtomicI64::new(0);

fn ios_main() {
  let rt = tokio::runtime::Runtime::new().unwrap();
  rt.block_on(async {
    let _win = Window::new(390, 844)
      .title("laufey on iOS")
      .bind("greet", |call| {
        let name = call
          .args
          .first()
          .and_then(|v| v.as_string())
          .unwrap_or("World")
          .to_string();
        call
          .resolve(Value::String(format!("Hello, {name}! — from Rust on iOS")));
      })
      .bind("add", |call| {
        let a = call.args.first().and_then(|v| v.as_int()).unwrap_or(0);
        let b = call.args.get(1).and_then(|v| v.as_int()).unwrap_or(0);
        call.resolve(Value::Int(a + b));
      })
      .bind("bump", |call| {
        let n = COUNTER.fetch_add(1, Ordering::SeqCst) + 1;
        call.resolve(Value::Int(n as i32));
      })
      .bind("platform", |call| {
        let mut m = HashMap::new();
        m.insert("runtime".into(), Value::String("laufey".into()));
        m.insert("language".into(), Value::String("Rust".into()));
        m.insert("engine".into(), Value::String("WKWebView".into()));
        m.insert("os".into(), Value::String("iOS".into()));
        call.resolve(Value::Dict(m));
      })
      .load(&page_url());

    laufey::run().await;
  });
}

// laufey's `data:text/html,` convention expects percent-encoded HTML (the
// backend percent-decodes it). Encode everything but alphanumerics so it
// round-trips exactly regardless of CSS `%`, `#`, emoji, etc.
fn page_url() -> String {
  let mut out = String::from("data:text/html,");
  for b in PAGE.bytes() {
    if b.is_ascii_alphanumeric() {
      out.push(b as char);
    } else {
      out.push_str(&format!("%{b:02X}"));
    }
  }
  out
}

const PAGE: &str = r#"<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<style>
  :root { color-scheme: dark; }
  html,body { margin:0; height:100%; }
  body {
    font-family: -apple-system, system-ui, sans-serif;
    background: radial-gradient(120% 120% at 50% 0%, #16213e 0%, #0b1020 60%, #06070d 100%);
    color: #eaf2ff; min-height:100%;
    padding: max(env(safe-area-inset-top), 30px) 22px 40px;
    -webkit-user-select: none;
  }
  .badge { display:inline-block; font-size:12px; letter-spacing:.18em; text-transform:uppercase;
    color:#00d4ff; border:1px solid rgba(0,212,255,.4); border-radius:999px; padding:5px 12px; }
  h1 { font-size:32px; margin:18px 0 4px; font-weight:800; letter-spacing:-.02em; }
  h1 .on { color:#00d4ff; }
  p.sub { margin:0 0 24px; color:#9fb3c8; font-size:15px; line-height:1.4; }
  button {
    -webkit-appearance:none; appearance:none; width:100%; text-align:left;
    background:rgba(255,255,255,.04); color:#eaf2ff; border:1px solid rgba(255,255,255,.10);
    padding:15px 18px; margin:8px 0; border-radius:16px; font-size:16px; font-weight:600;
    display:flex; align-items:center; gap:12px;
  }
  button:active { background:rgba(0,212,255,.14); }
  button .ic { font-size:11px; color:#00d4ff; }
  button .arrow { margin-left:auto; color:#5b7088; }
  #out {
    margin-top:20px; background:rgba(0,0,0,.35); border:1px solid rgba(255,255,255,.08);
    border-radius:16px; padding:16px; font:13px ui-monospace, Menlo, monospace; min-height:54px;
    white-space:pre-wrap; word-break:break-word; color:#aef5d0;
  }
</style>
</head>
<body>
  <span class="badge">laufey &middot; iOS</span>
  <h1>Native <span class="on">iOS</span> backend</h1>
  <p class="sub">WKWebView UI calling into a Rust laufey runtime, statically linked into the app.</p>

  <button onclick="run('greet', Laufey.greet('iOS'))"><span class="ic">&bull;</span> Laufey.greet("iOS") <span class="arrow">&rsaquo;</span></button>
  <button onclick="run('add', Laufey.add(20, 22))"><span class="ic">&bull;</span> Laufey.add(20, 22) <span class="arrow">&rsaquo;</span></button>
  <button onclick="run('bump', Laufey.bump())"><span class="ic">&bull;</span> Laufey.bump() — Rust counter <span class="arrow">&rsaquo;</span></button>
  <button onclick="run('platform', Laufey.platform())"><span class="ic">&bull;</span> Laufey.platform() <span class="arrow">&rsaquo;</span></button>

  <div id="out">Tap a button — JS calls Rust and back.</div>

<script>
  const out = document.getElementById('out');
  async function run(label, p) {
    try { const v = await p; out.textContent = label + ' -> ' +
      (typeof v === 'object' ? JSON.stringify(v, null, 2) : v); }
    catch (e) { out.textContent = 'error: ' + e; }
  }
  // Prove the bridge end-to-end on load: JS -> Rust -> JS.
  setTimeout(() => run('greet', Laufey.greet('iOS')), 500);
</script>
</body>
</html>"#;

laufey::main!(ios_main);
