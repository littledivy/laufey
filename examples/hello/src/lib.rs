use laufey::{DockBounceType, MenuItem, Value, Window};

fn hello_main() {
  let rt = tokio::runtime::Runtime::new().unwrap();
  rt.block_on(async {
    laufey::on_dock_reopen(|has_visible| {
      println!("dock reopen fired; has_visible_windows = {}", has_visible);
    });

    // BADGE-DIAGNOSIS: tray temporarily disabled.
    // let _tray = ...;

    let _win = Window::new(800, 600)
      .title("LAUFEY - Bindings Demo")
      .bind("greet", |call| {
        let name = call
          .args
          .first()
          .and_then(|v| v.as_string())
          .unwrap_or("World")
          .to_string();
        call.resolve(Value::String(format!("Hello, {}!", name)));
      })
      .bind("add", |call| {
        let a = call.args.first().and_then(|v| v.as_int()).unwrap_or(0);
        let b = call.args.get(1).and_then(|v| v.as_int()).unwrap_or(0);
        call.resolve(Value::Int(a + b));
      })
      .bind("getInfo", |call| {
        let mut info = std::collections::HashMap::new();
        info.insert("name".to_string(), Value::String("LAUFEY".to_string()));
        info.insert("version".to_string(), Value::String("0.1.0".to_string()));
        info.insert("rust".to_string(), Value::Bool(true));
        call.resolve(Value::Dict(info));
      })
      .bind("setDockBadge", |call| {
        let text = call.args.first().and_then(|v| v.as_string());
        laufey::set_dock_badge(text);
        call.resolve(Value::Null);
      })
      .bind("bounceDock", |call| {
        let critical = call
          .args
          .first()
          .and_then(|v| v.as_bool())
          .unwrap_or(false);
        laufey::bounce_dock(if critical {
          DockBounceType::Critical
        } else {
          DockBounceType::Informational
        });
        call.resolve(Value::Null);
      })
      .bind("setDockVisible", |call| {
        let visible = call
          .args
          .first()
          .and_then(|v| v.as_bool())
          .unwrap_or(true);
        laufey::set_dock_visible(visible);
        call.resolve(Value::Null);
      })
      .bind("setDockMenu", |call| {
        let items = vec![
          MenuItem::Item {
            label: "Say hello".into(),
            id: Some("hello".into()),
            accelerator: None,
            enabled: true,
          },
          MenuItem::Separator,
          MenuItem::Item {
            label: "Make noise".into(),
            id: Some("noise".into()),
            accelerator: None,
            enabled: true,
          },
        ];
        laufey::set_dock_menu(&items, |id| {
          println!("dock menu clicked: {}", id);
        });
        call.resolve(Value::Null);
      })
      .load(
        r#"data:text/html,<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>LAUFEY hello example</title>
    <style>
        body {
            font-family: system-ui, -apple-system, sans-serif;
            max-width: 600px;
            margin: 40px auto;
            padding: 20px;
            background: linear-gradient(135deg, %231a1a2e, %2316213e);
            color: white;
            min-height: 100vh;
        }
        h1 { color: %2300d4ff; }
        button {
            background: %2300d4ff;
            color: black;
            border: none;
            padding: 10px 20px;
            margin: 5px;
            border-radius: 5px;
            cursor: pointer;
            font-size: 14px;
        }
        button:hover { background: %2300a8cc; }
        pre {
            background: rgba(0,0,0,0.3);
            padding: 15px;
            border-radius: 5px;
            overflow-x: auto;
        }
        .result { color: %2300ff88; }
        .error { color: %23ff6b6b; }
    </style>
</head>
<body>
    <h1>LAUFEY Bindings</h1>
    <p>Call Rust functions from JavaScript:</p>
    <div>
        <button onclick="testGreet()">Laufey.greet("Alice")</button>
        <button onclick="testAdd()">Laufey.add(10, 25)</button>
        <button onclick="testInfo()">Laufey.getInfo()</button>
        <button onclick="testUnknown()">Laufey.unknown()</button>
    </div>
    <h2>Dock / Taskbar</h2>
    <div>
        <button onclick="Laufey.setDockBadge('3')">Badge "3"</button>
        <button onclick="Laufey.setDockBadge('')">Clear Badge</button>
        <button onclick="Laufey.bounceDock(false)">Bounce</button>
        <button onclick="Laufey.bounceDock(true)">Bounce (Critical)</button>
        <button onclick="Laufey.setDockMenu()">Set Dock Menu</button>
        <button onclick="Laufey.setDockVisible(false)">Hide from Dock</button>
        <button onclick="Laufey.setDockVisible(true)">Show in Dock</button>
    </div>
    <pre id="output">Click a button to test...</pre>
    <script>
        const out = document.getElementById('output');
        function log(msg, isError) {
            out.innerHTML += '<div class="' + (isError ? 'error' : 'result') + '">' + msg + '</div>';
        }
        async function testGreet() {
            try {
                const result = await Laufey.greet('Alice');
                log('greet: ' + result);
            } catch(e) { log('Error: ' + e.message, true); }
        }
        async function testAdd() {
            try {
                const result = await Laufey.add(10, 25);
                log('add: ' + result);
            } catch(e) { log('Error: ' + e.message, true); }
        }
        async function testInfo() {
            try {
                const result = await Laufey.getInfo();
                log('getInfo: ' + JSON.stringify(result));
            } catch(e) { log('Error: ' + e.message, true); }
        }
        async function testUnknown() {
            try {
                const result = await Laufey.unknown();
                log('unknown: ' + result);
            } catch(e) { log('Error: ' + e.message, true); }
        }
        out.innerHTML = 'Ready! Click buttons above.\n';
    </script>
</body>
</html>"#,
      );

    laufey::run().await;
  });
}

laufey::main!(hello_main);
