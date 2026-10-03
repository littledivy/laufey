# App data & web storage

Web storage — `localStorage`, IndexedDB, cookies, the HTTP and Cache Storage
caches — lives in the web engine's profile. laufey gives each app its own
profile that survives relaunches, so two apps never read each other's data and
an app finds its data again the next time it starts.

## Configuring

The directory is chosen from the environment when the backend starts, before the
runtime library is loaded (CEF and WebView2 need it that early), in the same way
as `LAUFEY_APP_ID` and `LAUFEY_APP_NAME`:

1. **`LAUFEY_DATA_DIR`**, if set and non-empty. It must be an absolute path; a
   relative value is ignored with a warning on stderr.
2. Otherwise **`LAUFEY_APP_ID`**, resolved under the per-user app data
   directory:

   | OS      | Directory                                                  |
   | ------- | ---------------------------------------------------------- |
   | macOS   | `~/Library/Application Support/<id>`                       |
   | Linux   | `$XDG_DATA_HOME/<id>`, or `~/.local/share/<id>` if unset   |
   | Windows | `%LOCALAPPDATA%\<id>` (the `FOLDERID_LocalAppData` folder) |

   The id becomes a single path component, so it must be non-empty, not `.` or
   `..`, and use only `A-Z a-z 0-9 . _ -` (a reverse-DNS id such as
   `com.example.notes` works). Any other id is rejected with a warning and the
   backend behaves as if it were not set. `LAUFEY_APP_NAME` (a display name) is
   never used for this.
3. Otherwise nothing is configured and every backend keeps the storage it had
   before this feature existed (see [below](#without-a-data-directory)).

Missing directories are created, owner-only (`0700`) on macOS and Linux; on
Windows they inherit the ACL of `%LOCALAPPDATA%` (or of the parent of
`LAUFEY_DATA_DIR`). If the directory can't be created, the backend warns and
falls back to its default storage.

## Layout

Each backend keeps its profile in its own subdirectory, so two engines never
share one profile even when an app switches backend:

| Backend           | Profile                                                                           |
| ----------------- | --------------------------------------------------------------------------------- |
| CEF               | `<dir>/CEF` (`root_cache_path` and `cache_path`)                                  |
| WebView / Windows | `<dir>\WebView2` (the WebView2 user data folder)                                  |
| WebView / Linux   | `<dir>/WebKitGTK/data`, `<dir>/WebKitGTK/cache`, cookies in `data/cookies.sqlite` |
| WebView / macOS   | none on disk under `<dir>` — see below                                            |

WKWebView can't be pointed at a directory. On macOS 14 and later the WebView
backend instead uses a persistent data store identified by a UUID
([`+[WKWebsiteDataStore dataStoreForIdentifier:]`](https://developer.apple.com/documentation/webkit/wkwebsitedatastore)),
derived as a name-based UUIDv5 in the namespace
`914813aa-3115-4724-937f-29b54f441356`:

- from the `LAUFEY_DATA_DIR` string (absolute, trailing separators removed) when
  that is set, so two data directories never share a store;
- otherwise from `LAUFEY_APP_ID`.

For example,
`python3 -c "import uuid; print(uuid.uuid5(uuid.UUID('914813aa-3115-4724-937f-29b54f441356'), 'com.example.notes'))"`
prints the store's identifier. WebKit keeps the store under
`~/Library/WebKit/<bundle id>/WebsiteDataStore/<UUID>`, so the resolved
directory itself is not created on macOS. Before macOS 14 the default store is
used.

The iOS backend and the Winit backend (no web engine) don't read these
variables.

## One instance per profile (CEF)

Chromium locks its profile: only one process may use a given `root_cache_path`.
Launching a second instance of the same app (same `LAUFEY_APP_ID` or
`LAUFEY_DATA_DIR`) while one is running makes the second one exit during startup
with `another instance is already running with the web data directory …` on
stderr; the running instance keeps going and ignores the relaunch. laufey has no
single-instance API yet, so an embedder that wants to focus the existing window
or forward arguments has to do that itself. The WebView backends don't enforce
one instance per profile.

## Without a data directory

With neither variable set, nothing changes from earlier releases:

- **CEF** uses a fresh temporary profile per process (`laufey_cef_<pid>` in the
  temp directory), so nothing persists across launches.
- **WebView2** uses its default user data folder, `<exe name>.WebView2` next to
  the executable.
- **WKWebView** uses the default data store, keyed by the host bundle id, so
  apps that share a bundle (for example during development) share storage.
- **WebKitGTK** uses the default web context, whose data lives under the program
  name in the XDG directories (`main_linux.cc` sets the program name from
  `LAUFEY_APP_ID`, falling back to `LAUFEY_APP_NAME`), with cookies kept in
  memory only.

## Upgrading

Setting `LAUFEY_APP_ID` now moves the profile, so data an app stored before this
change is not carried over: WKWebView data stays in the bundle's default store,
WebView2 data in `<exe name>.WebView2`, and WebKitGTK data directly under
`~/.local/share/<prgname>` rather than in `WebKitGTK/`. Nothing is deleted.
