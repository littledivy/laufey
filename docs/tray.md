# Tray / status bar

A tray icon is a persistent icon in the operating system's status area: the menu
bar on macOS, the system tray on Windows, and the AppIndicator area on Linux.
Each icon has an image, a tooltip, a right-click menu, and click handlers.
`TrayIcon` is a builder; you must keep the returned value alive for the icon to
remain visible.

```rust
use laufey::{MenuItem, TrayIcon};

let tray = TrayIcon::new()
  .icon(include_bytes!("icon.png"))
  .icon_dark(include_bytes!("icon-dark.png")) // optional dark-mode variant
  .tooltip("My App")
  .menu(&[MenuItem::Role { role: "quit".into() }], |id| println!("{id}"))
  .on_click(|| println!("clicked"))
  .on_double_click(|| println!("double clicked"));

// The icon's bounds let you anchor a popover panel beneath it.
let bounds = tray.get_bounds(); // Option<(x, y, width, height)>
```

When you provide both a light and a dark icon, the backend watches the system
appearance and swaps between them live: it observes
`AppleInterfaceThemeChangedNotification` on macOS, the `WM_SETTINGCHANGE`
message together with the `AppsUseLightTheme` setting on Windows, and polls once
per event-loop tick on Winit. On Linux, AppIndicator renders the icon through
the desktop theme and does not deliver click or double-click events, and the
StatusNotifierItem specification has no tooltip, so click handlers, tooltips,
and dark-mode swapping have no effect there. The CEF backend also uses
AppIndicator on Linux, so a tray icon does not require a browser window.
