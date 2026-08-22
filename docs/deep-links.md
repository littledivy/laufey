# Deep links

A deep link is a custom URL scheme the OS routes to your app — clicking
`acme://open/document/42` in a browser, a mail client, or a terminal hands that
URL to the app that claims the `acme` scheme. laufey delivers the URL to your
runtime; it does not register the scheme.

```rust
laufey::on_open_url(|url| {
  // "acme://open/document/42"
  window.navigate(&route_for(url));
});
```

## Registering the scheme is the embedder's job

laufey [does not build application bundles](distribution.md), and scheme
registration lives entirely inside the bundle metadata, so it belongs to
whatever packages the app:

| Platform | Where the scheme is declared                                              |
| -------- | ------------------------------------------------------------------------- |
| macOS    | `CFBundleURLTypes` / `CFBundleURLSchemes` in the bundle `Info.plist`      |
| Linux    | `MimeType=x-scheme-handler/acme;` plus `Exec=… %u` in the `.desktop`      |
| Windows  | `HKCU\Software\Classes\acme` with `URL Protocol` and `shell\open\command` |

The OS also has to have _seen_ that metadata: macOS registers schemes through
LaunchServices when the `.app` is installed (or after `lsregister -f`), Linux
needs the `.desktop` file in `~/.local/share/applications` and
`update-desktop-database`, and the Windows keys are normally written by an
installer.

## macOS only, and why

`on_open_url` fires on macOS. Windows and Linux are a no-op.

This isn't an implementation gap — the platforms hand deep links over in
fundamentally different shapes. macOS delivers the URL to the _already running_
app as an Apple Event (`application:openURLs:`), launching it first if
necessary, so a single process sees every link and laufey can turn that into a
callback.

Windows and Linux instead **spawn a new process** with the URL in `argv`. Making
that behave like a deep link means recognizing the second process as a
duplicate, forwarding the URL to the first one, and raising its window — which
needs a single-instance lock keyed to a stable app identity, and a decision
about whether a second instance is wrong at all. laufey has neither the identity
nor the standing to make that call, for the same reason it doesn't bundle or
sign: the embedder owns packaging. So on those platforms the embedder reads
`std::env::args()` itself and does its own forwarding.

Backends report this through the ABI: `set_open_url_handler` is `NULL` on every
non-macOS backend, so an embedder can detect the absence rather than register a
handler that silently never fires.

## Cold start is buffered, not dropped

A launch URL — the case where clicking the link _starts_ the app — arrives while
the runtime is still coming up. The backend loads the runtime shared library on
a worker thread, so at the moment AppKit dispatches the Apple Event there is no
handler to call yet.

Rather than lose it, the backend buffers URLs while no handler is registered and
flushes them, in order, the instant `on_open_url` is called. Registering a
handler at startup is therefore enough to catch both cases:

```rust
// Fires for a link clicked five minutes from now, and for the link that
// launched the app a moment ago.
laufey::on_open_url(|url| handle(url));
```

Up to `LAUFEY_MAX_PENDING_OPEN_URLS` (16) URLs are held; beyond that the oldest
are discarded, since the newest link is the one the user is waiting on.

## Threading

The callback runs on the backend's UI thread, like every other event handler —
with one exception: URLs replayed from the buffer run on the thread that called
`on_open_url`, because the flush happens inside that call.

## Validate the URL

The URL is passed through exactly as the OS delivered it, with no filtering.
Anything on the machine can invoke your app with an arbitrary URL, so treat it
as untrusted input: check the scheme against the one you registered, and don't
route on it without validating the rest.

```rust
laufey::on_open_url(|url| {
  let Some(rest) = url.strip_prefix("acme://") else {
    return; // not ours
  };
  route(rest);
});
```

## Testing

Deep links need OS-level registration to exercise for real, which automated
tests can't rely on. The `test_trigger_open_url` hook (API ≥ 35) synthesizes a
delivery through the same dispatch path — buffer included, so a call made before
any handler is registered is replayed on registration exactly like a cold start:

```rust
assert!(!laufey::test_trigger_open_url("acme://cold")); // buffered
laufey::on_open_url(|url| { /* receives "acme://cold" immediately */ });
assert!(laufey::test_trigger_open_url("acme://live")); // delivered
```

See [`docs/e2e-testing.md`](e2e-testing.md) and the probe in
`examples/native_e2e`. For an end-to-end check, build a bundle with a scheme in
its `Info.plist`, register it with `lsregister -f`, and `open 'acme://…'` with
the app both running and closed.
