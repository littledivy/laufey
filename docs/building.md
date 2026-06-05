# Building

A `Makefile` drives the build. `make help` lists every target.

## Prerequisites

- Rust (stable)
- `cmake` and `ninja`
- macOS: `brew install llvm` (for `libclang`)
- Linux: GTK + X dev packages
  (`libgtk-3-dev libxkbcommon-dev libxrandr-dev
  libxrender-dev libxtst-dev`)
- Windows: Visual Studio (MSVC) + LLVM; build from a `vcvars64` shell

`make check-deps` verifies the base tools.

## Backends

```sh
make cef        # CEF backend (downloads + builds the CEF dll wrapper first)
make webview    # system WebView backend (WebKitGTK / WebView2)
make winit      # windowing-only backend, no web engine
make all        # everything
```

`make cef` runs `make cef-deps`, which downloads the pinned CEF build into
`vendor/cef/` and builds `libcef_dll_wrapper`. The CEF version is pinned at the
top of the `Makefile` (`CEF_VERSION`). Re-download with `make clean-cef-vendor`.

Host OS/arch and the matching CEF archive are detected automatically.

## Runtimes

```sh
make runtimes   # builds the hello + ddcore example runtimes
```

A runtime is a shared library linked against the `capi` crate; a backend loads
it at startup (see [architecture.md](architecture.md)).

## Formatting, linting, tests

```sh
make fmt        # cargo fmt + deno fmt + clang-format
make fmt-check
make lint       # cargo clippy + deno lint
cargo test -p laufey --lib
```

## Output

Backends build under each backend dir's `build/` (`cef/build`, `webview/build`).
`make clean` removes build artifacts; `make clean-cef-vendor` drops the
downloaded CEF.
