# Notifications

laufey can post system notifications. The options mirror a subset of the Web
Notifications API: a title and body, an icon, a tag that replaces an earlier
notification carrying the same tag, a silent flag, a require-interaction flag,
and action buttons. Notifications are application-scoped.

```rust
use laufey::Notification;

let handle = Notification::new("Build finished")
  .body("3 warnings")
  .icon(include_bytes!("icon.png").to_vec())
  .tag("build")
  .action("rebuild", "Rebuild")
  .on_event(|event| println!("{event:?}")) // shown, clicked, closed, or action
  .show();

handle.close();
```

The implementation differs by platform. macOS posts through
`NSUserNotification`, which does not require authorization to post; the modern
`UNUserNotificationCenter` is used only for the permission prompt described in
[permissions.md](permissions.md). Windows posts a `Shell_NotifyIcon` balloon,
which Windows 10 and 11 render as a system toast, but those balloons cannot show
action buttons. Linux shells out to `notify-send`, which is fire-and-forget, so
only the synthetic shown and closed events are reported. The Winit backend uses
`notify-rust` and reports show, close, and a synthetic shown event; use the CEF
or WebView backend when you need click or action callbacks.
