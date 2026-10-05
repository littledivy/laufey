//! Request-body round trip over the custom scheme.
//!
//! A page served at `app://e2e-body/` sends POST/PUT/PATCH requests with
//! bodies (UTF-8 text, a small binary body with NUL and high bytes, a
//! binary body over 1 MB, an empty body, bodies one byte either side of and
//! exactly at the 256 KiB chunk WebKitGTK reads them in, and eight bodies
//! sent at once) to `app://e2e-body/echo/<label>`.
//! The scheme handler reads each body through `SchemeExchange::read_body`,
//! records what it received, and echoes it back byte for byte; the page
//! compares the echo with what it sent and reports through the `bodyReport`
//! binding. The runtime then checks both halves: the bytes the handler
//! received equal the expected bytes (regenerated here), and the page saw an
//! identical echo.
//!
//! The same page then checks that a response's Content-Type reaches the
//! engine as its MIME type and charset (`MIME_CHECKS`): a Latin-1 body
//! decodes as Latin-1, a JSON response keeps its type, and documents loaded
//! in a frame are HTML (or plain text) in the declared encoding. WebKitGTK
//! once sent no MIME type at all (a navigation became a download) and CEF
//! once passed `text/html; charset=utf-8` whole as the MIME type (an empty
//! document).

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use laufey::SchemeRequest;

/// Page that drives the round trips.
pub const PAGE_URL: &str = "app://e2e-body/";
const PAGE_PREFIX: &str = "app://e2e-body";
const ECHO_PREFIX: &str = "app://e2e-body/echo/";
const MIME_PREFIX: &str = "app://e2e-body/mime/";

/// Size of the large binary body: over 1 MiB and not a multiple of any
/// plausible read chunk.
pub const BIG_LEN: usize = 1_536_007;

/// One request the page sends: label, HTTP method, expected body.
pub struct Case {
  pub label: &'static str,
  pub method: &'static str,
  pub body: Vec<u8>,
}

/// WebKitGTK reads request bodies in chunks of this size
/// (webview_linux.cc kSchemeBodyChunk); the boundary cases sit around it.
pub const CHUNK: usize = 256 * 1024;

/// How many bodies the page sends at once (`concurrent-<i>`).
pub const CONCURRENT: usize = 8;

/// Deterministic binary pattern covering every byte value (0..=255,
/// including NUL and bytes >= 0x80), varied by `seed`. Mirrored by
/// `pattern()` in the page.
fn pattern(len: usize, seed: usize) -> Vec<u8> {
  (0..len)
    .map(|i| ((i % 251) ^ ((i / 251) & 0xff) ^ (seed & 0xff)) as u8)
    .collect()
}

fn concurrent_len(i: usize) -> usize {
  100_003 + i * 40_961
}

/// Page-side checks of how a response's Content-Type reaches the engine:
/// (label, what it shows).
pub const MIME_CHECKS: &[(&str, &str)] = &[
  (
    "mime-latin1-text",
    "a text/plain; charset=iso-8859-1 response keeps its type and decodes as Latin-1",
  ),
  (
    "mime-quoted-charset",
    "a quoted charset parameter is honoured",
  ),
  ("mime-json", "an application/json response keeps its type"),
  (
    "mime-doc-utf8",
    "a text/html; charset=utf-8 document loads as UTF-8 HTML",
  ),
  (
    "mime-doc-latin1",
    "a text/html; charset=iso-8859-1 document loads as windows-1252 HTML",
  ),
  (
    "mime-doc-plain",
    "a text/plain document loads as text, not a download",
  ),
];

pub const TEXT_BODY: &str = "h\u{e9}llo, body \u{2713}\nline 2";

pub fn cases() -> Vec<Case> {
  let mut cases = vec![
    Case {
      label: "text",
      method: "POST",
      body: TEXT_BODY.as_bytes().to_vec(),
    },
    Case {
      label: "binary-small",
      method: "PATCH",
      body: vec![0, 1, 2, 0x7f, 0x80, 0xff, 0, 0xfe],
    },
    Case {
      label: "binary-big",
      method: "PUT",
      body: pattern(BIG_LEN, 0),
    },
    Case {
      label: "empty",
      method: "POST",
      body: Vec::new(),
    },
  ];
  for (label, method, len) in [
    ("chunk-minus-1", "POST", CHUNK - 1),
    ("chunk-exact", "PUT", CHUNK),
    ("chunk-plus-1", "PATCH", CHUNK + 1),
    ("chunk-double", "POST", 2 * CHUNK),
  ] {
    cases.push(Case {
      label,
      method,
      body: pattern(len, len),
    });
  }
  for (i, label) in CONCURRENT_LABELS.iter().enumerate() {
    cases.push(Case {
      label,
      method: "POST",
      body: pattern(concurrent_len(i), i + 1),
    });
  }
  cases
}

const CONCURRENT_LABELS: [&str; CONCURRENT] = [
  "concurrent-0",
  "concurrent-1",
  "concurrent-2",
  "concurrent-3",
  "concurrent-4",
  "concurrent-5",
  "concurrent-6",
  "concurrent-7",
];

/// What the handler received for one label: (method, body).
pub type Received = Arc<Mutex<HashMap<String, (String, Vec<u8>)>>>;

/// What the page reported for one label: (echo identical, echo length,
/// status/error detail).
pub type Reports = Arc<Mutex<HashMap<String, (bool, i64, String)>>>;

pub fn page_html() -> String {
  format!(
    r#"<!doctype html><html><head><meta charset="utf-8"><title>body</title></head><body>
<script>
async function waitForBinding(name) {{
  for (let i = 0; i < 200; i++) {{
    if (typeof Laufey !== 'undefined' && typeof Laufey[name] === 'function') return;
    await new Promise(r => setTimeout(r, 50));
  }}
  throw new Error('binding never appeared: ' + name);
}}
function pattern(n, seed) {{
  const a = new Uint8Array(n);
  for (let i = 0; i < n; i++) a[i] = (i % 251) ^ (Math.floor(i / 251) & 255) ^ (seed & 255);
  return a;
}}
const CHUNK = {chunk};
const cases = [
  ['text', 'POST', new TextEncoder().encode({text:?}), {text:?}],
  ['binary-small', 'PATCH', new Uint8Array([0, 1, 2, 0x7f, 0x80, 0xff, 0, 0xfe]), null],
  ['binary-big', 'PUT', pattern({big}, 0), null],
  ['empty', 'POST', new Uint8Array(0), undefined],
  ['chunk-minus-1', 'POST', pattern(CHUNK - 1, CHUNK - 1), null],
  ['chunk-exact', 'PUT', pattern(CHUNK, CHUNK), null],
  ['chunk-plus-1', 'PATCH', pattern(CHUNK + 1, CHUNK + 1), null],
  ['chunk-double', 'POST', pattern(2 * CHUNK, 2 * CHUNK), null],
];
async function roundTrip(label, method, bytes, sendAs) {{
  let same = false, len = -1, detail = '';
  try {{
    // `sendAs` sends the text case as a string (the engine encodes it) and
    // the empty case with no body at all; the rest as raw bytes.
    const body = sendAs === undefined ? undefined : (sendAs !== null ? sendAs : bytes);
    const res = await fetch('/echo/' + label, {{ method, body }});
    const got = new Uint8Array(await res.arrayBuffer());
    len = got.length;
    same = got.length === bytes.length && got.every((b, i) => b === bytes[i]);
    detail = 'status ' + res.status;
  }} catch (e) {{
    detail = 'error: ' + (e && e.message);
  }}
  await Laufey.bodyReport(label, same, len, detail);
}}
// Loads `path` in a same-origin frame; its document, or an error.
function frameDoc(path) {{
  return new Promise((resolve, reject) => {{
    const f = document.createElement('iframe');
    const timer = setTimeout(() => reject(new Error('the frame never loaded')), 5000);
    f.onload = () => {{
      clearTimeout(timer);
      try {{ resolve(f.contentDocument); }} catch (e) {{ reject(e); }}
    }};
    f.src = path;
    document.body.appendChild(f);
  }});
}}
function xhrText(path) {{
  return new Promise((resolve, reject) => {{
    const x = new XMLHttpRequest();
    x.open('GET', path);
    x.onload = () => resolve(x.responseText);
    x.onerror = () => reject(new Error('XHR failed'));
    x.send();
  }});
}}
const mimeChecks = [
  // fetch().text() always decodes UTF-8 (the Fetch spec), so the charset
  // shows in XHR's responseText, which decodes with the response's charset.
  ['mime-latin1-text', async () => {{
    const ct = (await fetch('/mime/latin1-text')).headers.get('content-type') || '';
    const text = await xhrText('/mime/latin1-text');
    return [text === 'caf\u00e9' && ct === 'text/plain; charset=iso-8859-1',
            'text ' + JSON.stringify(text) + ', type ' + ct];
  }}],
  ['mime-quoted-charset', async () => {{
    const text = await xhrText('/mime/quoted-charset');
    return [text === 'caf\u00e9', 'text ' + JSON.stringify(text)];
  }}],
  ['mime-json', async () => {{
    const res = await fetch('/mime/json');
    const ct = res.headers.get('content-type') || '';
    const v = await res.json();
    return [ct === 'application/json' && v.k === 'v\u00e9', 'type ' + ct + ', k ' + JSON.stringify(v.k)];
  }}],
  ['mime-doc-utf8', async () => {{
    const d = await frameDoc('/mime/doc-utf8');
    return [d.contentType === 'text/html' && d.characterSet === 'UTF-8' && d.title === '\u00fcn\u00ef \u2713',
            d.contentType + ' ' + d.characterSet + ' ' + JSON.stringify(d.title)];
  }}],
  ['mime-doc-latin1', async () => {{
    const d = await frameDoc('/mime/doc-latin1');
    return [d.contentType === 'text/html' && d.characterSet === 'windows-1252' && d.title === 'caf\u00e9',
            d.contentType + ' ' + d.characterSet + ' ' + JSON.stringify(d.title)];
  }}],
  ['mime-doc-plain', async () => {{
    const d = await frameDoc('/mime/doc-plain');
    const text = d.body ? d.body.textContent : '';
    return [d.contentType === 'text/plain' && text.includes('plain <b>text</b>'),
            d.contentType + ' ' + JSON.stringify(text)];
  }}],
];
(async () => {{
  await waitForBinding('bodyReport');
  for (const [label, method, bytes, sendAs] of cases) {{
    await roundTrip(label, method, bytes, sendAs);
  }}
  // Several bodies in flight at once: each must reach its own request.
  const concurrent = [];
  for (let i = 0; i < {concurrent}; i++) {{
    concurrent.push(roundTrip('concurrent-' + i, 'POST',
                              pattern(100003 + i * 40961, i + 1), null));
  }}
  await Promise.all(concurrent);
  for (const [label, run] of mimeChecks) {{
    let ok = false, detail = '';
    try {{ [ok, detail] = await run(); }} catch (e) {{ detail = 'error: ' + (e && e.message); }}
    await Laufey.bodyReport(label, ok, 0, detail);
  }}
}})().catch(e => Laufey.bodyReport('script', false, -1, String(e && e.message)));
</script></body></html>"#,
    text = TEXT_BODY,
    big = BIG_LEN,
    chunk = CHUNK,
    concurrent = CONCURRENT,
  )
}

/// Serve `req` if it belongs to the body round trip; otherwise hand it back.
/// Echo requests are answered on their own thread: `read_body` may block, and
/// the scheme handler must not block the backend thread it runs on.
pub fn serve(req: SchemeRequest, received: &Received) -> Option<SchemeRequest> {
  let path = req.url.split(['?', '#']).next().unwrap_or("").to_string();
  if path == PAGE_PREFIX || path == PAGE_URL {
    // No charset parameter: the page declares it with <meta charset>, and a
    // CEF backend that takes the whole Content-Type value as the MIME type
    // would otherwise load an empty document.
    let headers = vec![
      ("content-type".to_string(), "text/html".to_string()),
      ("cache-control".to_string(), "no-store".to_string()),
    ];
    req.exchange.begin(200, &headers);
    req.exchange.write(page_html().as_bytes());
    req.exchange.finish();
    return None;
  }
  if let Some(name) = path.strip_prefix(MIME_PREFIX) {
    let (content_type, body): (&str, &[u8]) = match name {
      "latin1-text" => ("text/plain; charset=iso-8859-1", b"caf\xe9"),
      "quoted-charset" => ("text/plain; charset=\"ISO-8859-1\"", b"caf\xe9"),
      "json" => ("application/json", "{\"k\":\"v\u{e9}\"}".as_bytes()),
      "doc-utf8" => (
        "text/html; charset=utf-8",
        "<!doctype html><title>\u{fc}n\u{ef} \u{2713}</title><p>utf-8"
          .as_bytes(),
      ),
      "doc-latin1" => (
        "text/html; charset=iso-8859-1",
        b"<!doctype html><title>caf\xe9</title><p>latin-1",
      ),
      "doc-plain" => ("text/plain", b"plain <b>text</b>"),
      _ => ("text/plain", b"not found"),
    };
    let headers = vec![
      ("content-type".to_string(), content_type.to_string()),
      ("cache-control".to_string(), "no-store".to_string()),
    ];
    req.exchange.begin(200, &headers);
    req.exchange.write(body);
    req.exchange.finish();
    return None;
  }
  let Some(label) = path.strip_prefix(ECHO_PREFIX).map(str::to_string) else {
    return Some(req);
  };
  let received = received.clone();
  std::thread::spawn(move || {
    let mut body = Vec::new();
    let mut buf = vec![0u8; 64 * 1024];
    let mut ok = true;
    loop {
      let n = req.exchange.read_body(&mut buf);
      if n == 0 {
        break;
      }
      if n < 0 {
        ok = false;
        break;
      }
      body.extend_from_slice(&buf[..n as usize]);
    }
    received
      .lock()
      .unwrap()
      .insert(label, (req.method.clone(), body.clone()));
    let headers = vec![
      (
        "content-type".to_string(),
        "application/octet-stream".to_string(),
      ),
      ("cache-control".to_string(), "no-store".to_string()),
    ];
    req.exchange.begin(if ok { 200 } else { 500 }, &headers);
    if !body.is_empty() {
      req.exchange.write(&body);
    }
    req.exchange.finish();
  });
  None
}
