# Dock / taskbar

laufey can badge the application icon, request the user's attention, and — on
macOS — drive the dock menu, the icon's visibility, and a reopen callback. These
are free functions rather than window methods, because the dock is
application-scoped on macOS, while on Windows and Linux the equivalent
operations act on the currently focused window's taskbar button.

```rust
use laufey::DockBounceType;

laufey::set_dock_badge(Some("3"));      // pass None to clear the badge
laufey::bounce_dock(DockBounceType::Critical);

laufey::on_dock_reopen(|has_visible_windows| {
  // On macOS, the user clicked the dock icon while no windows were open.
});
```

On macOS the badge is a native red overlay drawn on the dock tile, and on
Windows it is a small overlay icon composited onto the taskbar button. Linux has
no icon overlay, so every backend falls back to prefixing the focused window's
title with `"(N) "`, the convention used by applications such as Slack, Discord,
and Telegram; taskbars and window-manager overviews surface that title.
Requesting attention bounces the dock icon on macOS, flashes the taskbar button
on Windows, and sets the window's urgency hint on Linux. The dock menu, the
ability to hide the dock icon, and the reopen callback exist only on macOS.
