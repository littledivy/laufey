# Launch configuration

Some settings have to be known when the backend process starts, before the
runtime library is loaded. Examples are the Wayland `app_id` that WebKitGTK sets
in `main()` and the settings CEF reads while it initializes, in every process.
An embedder can pass these as environment variables, but a packaged app is often
started directly by Explorer, a Start menu or desktop shortcut, the Dock, or
`exec`. Nothing sets environment variables in that case. Such an app can instead
ship the settings in a JSON file next to the executable.

## Where the file goes

| Platform        | Path                                               |
| --------------- | -------------------------------------------------- |
| Windows, Linux  | `<directory of the executable>/laufey-launch.json` |
| macOS (bundled) | `<App>.app/Contents/Resources/laufey-launch.json`  |
| macOS (bare)    | `<directory of the executable>/laufey-launch.json` |

The directory comes from the running executable's real path
(`GetModuleFileNameW`, `/proc/self/exe`, `_NSGetExecutablePath` plus
`realpath`). It never comes from the working directory. CEF's helper apps
(`<App>.app/Contents/Frameworks/<App> Helper*.app`) read the main app's file.

## Schema

```json
{
  "appId": "com.example.myapp",
  "customSchemes": ["myapp"],
  "dataDir": "/absolute/path"
}
```

Every key is optional, and each key stands in for one environment variable:

| Key             | Environment variable    | Value                                   |
| --------------- | ----------------------- | --------------------------------------- |
| `appId`         | `LAUFEY_APP_ID`         | non-empty string                        |
| `customSchemes` | `LAUFEY_CUSTOM_SCHEMES` | array of scheme names (no `,` in names) |
| `dataDir`       | `LAUFEY_DATA_DIR`       | non-empty string                        |

`appId` sets the Wayland `app_id` and X11 `WM_CLASS` on Linux (WebView and CEF
backends), exactly like `LAUFEY_APP_ID`. `customSchemes` and `dataDir` are read
and validated and are available to the backends through `LaunchCustomSchemes()`
and `LaunchDataDir()`. This version has no backend feature that consumes them
yet.

## Precedence

For each key separately, an environment variable that is set to a non-empty
value wins over the file. So a launcher can still force a value, and setups that
rely on the environment keep working unchanged. A key absent from the file and
from the environment is simply unset. The file is read once per process.

The exceptions are `appId` and `dataDir`, which identify the installed app. For
them the shipped file wins: when the file has the key, `LAUFEY_APP_ID` /
`LAUFEY_DATA_DIR` are ignored. Environment variables are inherited, so an app
started by another laufey app would otherwise open that app's profile. A
launcher can still set them for an app whose file leaves them out.

A file with `appId` pins the rest of the app's identity with it:

- The data directory is the file's `dataDir`, or else the app id's default
  directory. `LAUFEY_DATA_DIR` is ignored even when the file has no `dataDir`.
- The custom schemes are the file's `customSchemes` only, none without the key.
  `LAUFEY_CUSTOM_SCHEMES` is ignored.

Otherwise a variable inherited from another program could still move the app's
profile or register a scheme of its choosing in the app. Without `appId` in the
file, the keys are resolved one at a time as described above.

`LAUFEY_DATA_DIR` or `LAUFEY_CUSTOM_SCHEMES` set but ignored because of the file
is reported on stderr, once per process.

## Validation

A problem with the file never stops the app. It is reported on stderr as
`laufey: <path>: <message>` and skipped:

- If there is no file, nothing happens. This is not an error.
- If the JSON is malformed, or the top level is not an object, the whole file is
  ignored.
- A file larger than 64 KiB is ignored.
- An unknown key is reported and ignored, so newer files keep working with older
  hosts.
- A value of the wrong type, an empty string or a string containing NUL is
  reported, and that key is ignored.
- An invalid entry in `customSchemes` is reported and skipped. The other entries
  are kept.
- If a key appears more than once, the last one wins, and this is reported.

## Trust

The file belongs to the installed app. It sits inside the (possibly signed)
`.app` bundle or next to the executable in the install directory, is written by
the app's packager or installer, and is trusted like the executable itself.
laufey reads it only from that location, never from the working directory or
from a per-user location. Keep it where only the installer can write it, just
like the executable.
