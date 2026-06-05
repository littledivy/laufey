# Packaging & distribution

laufey stops at the backend and the runtime. A build produces a backend executable
and your runtime shared library; turning that into something you can ship — and
keeping it up to date — is the responsibility of the embedder that wraps laufey,
not of laufey itself.

## Bundling

laufey does not produce an application bundle. The embedder decides how the backend
executable and the runtime library are laid out and packaged: a macOS `.app`, a
Windows installer or directory, a Linux `.deb`/AppImage, and so on. This is by
design — laufey stays unopinionated so that a host such as the
[`deno desktop`](https://github.com/denoland/deno) tooling can own the packaging
format end to end.

One detail does reach into laufey on macOS. Several features —
[notifications](notifications.md) and [permissions](permissions.md) — depend on
the process running inside a real `.app` with a `CFBundleIdentifier`. An
unbundled binary (run straight from `target/`, or the synthetic bundle
`cargo
run` produces) reports `unsupported` rather than failing, so those
features come to life only once the embedder has bundled the app. laufey hard-codes
no bundle identifier of its own, leaving the embedder free to set its own
identity.

## Code signing & notarization

laufey does not sign or notarize anything. Code signing (macOS Developer ID +
notarization, Windows Authenticode) is applied by the embedder to the final
bundle, using its own certificates and entitlements. Because laufey carries no
bundle identifier and no embedded entitlements, the embedder controls the app's
identity completely, which is what lets the system authorization prompts target
the embedder rather than laufey.

## Updates

laufey has no built-in updater. Shipping new versions — full replacement or
binary-diff patch updates — is left to the embedder's distribution channel. The
backend and runtime are ordinary files, so any update mechanism the host already
uses applies without special support from laufey.
