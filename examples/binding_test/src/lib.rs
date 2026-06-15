// Headless-ish bindings roundtrip check. Page runs echo/add cases on load,
// reports PASS/FAIL lines back through a binding, which prints them and quits.
// Regression guard for the macOS/Windows respond path that resolved every
// binding call with null.

use laufey::{Value, Window};

fn binding_test_main() {
  let rt = tokio::runtime::Runtime::new().unwrap();
  rt.block_on(async {
    let _win = Window::new(480, 320)
      .title("Bindings Roundtrip Test")
      .bind("echo", |call| {
        let v = call.args.first().cloned().unwrap_or(Value::Null);
        call.resolve(v);
      })
      .bind("add", |call| {
        let a = call.args.first().and_then(|v| v.as_int()).unwrap_or(0);
        let b = call.args.get(1).and_then(|v| v.as_int()).unwrap_or(0);
        call.resolve(Value::Int(a + b));
      })
      .bind("report", |call| {
        let report = call
          .args
          .first()
          .and_then(|v| v.as_string())
          .unwrap_or("(no report)")
          .to_string();
        println!("\n===== BINDINGS ROUNDTRIP =====\n{}\n==============================", report);
        let all_pass = !report.contains("FAIL");
        println!("RESULT: {}", if all_pass { "ALL PASS" } else { "FAILURES PRESENT" });
        call.resolve(Value::Null);
        laufey::quit();
      })
      .load(
        r#"data:text/html,<!DOCTYPE html>
<html>
<head><meta charset="utf-8"><title>roundtrip</title></head>
<body>
<pre id="out">running...</pre>
<script>
  const out = document.getElementById('out');
  function eq(a, b) { return JSON.stringify(a) === JSON.stringify(b); }
  async function run() {
    const cases = [
      ['echo string', () => Laufey.echo('hello'), 'hello'],
      ['echo int',    () => Laufey.echo(42), 42],
      ['echo bool',   () => Laufey.echo(true), true],
      ['echo array',  () => Laufey.echo([1, 'two']), [1, 'two']],
      ['echo object', () => Laufey.echo({a: 1}), {a: 1}],
      ['echo null',   () => Laufey.echo(null), null],
      ['add',         () => Laufey.add(2, 3), 5],
    ];
    const lines = [];
    for (const [name, fn, want] of cases) {
      try {
        const got = await fn();
        const ok = eq(got, want);
        lines.push((ok ? 'PASS ' : 'FAIL ') + name + ' => got ' + JSON.stringify(got) + ' want ' + JSON.stringify(want));
      } catch (e) {
        lines.push('FAIL ' + name + ' threw ' + e.message);
      }
    }
    out.textContent = lines.join('\n');
    await Laufey.report(lines.join('\n'));
  }
  run();
</script>
</body>
</html>"#,
      );

    laufey::run().await;
  });
}

laufey::main!(binding_test_main);
