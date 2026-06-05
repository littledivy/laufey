# Permissions

laufey lets you query or request the operating system's authorization for a
capability. The only capability today is notifications. The status set mirrors
the Web Permissions API: granted, denied, prompt, and unsupported.

```rust
use laufey::{PermissionKind, PermissionStatus};

laufey::request_permission(PermissionKind::Notifications, |status| {
  if status == PermissionStatus::Granted {
    // The capability is authorized.
  }
});
```

`query_permission` reads the current status without prompting the user.
`request_permission` shows the system prompt only when the status is prompt;
once the user has decided, the operating system returns the cached decision
rather than prompting again. Both callbacks run on the user-interface thread.

On macOS all backends route through `UNUserNotificationCenter`. A process that
is not bundled — one with no `CFBundleIdentifier`, or a binary that does not
live inside an `.app` — reports unsupported rather than denied, so that an
embedder can distinguish "the user declined" from "this environment cannot be
authorized at all." An application that packages its own `.app` sets its own
bundle identifier and entitlements; laufey hard-codes none of its own. Windows
(`Shell_NotifyIcon`) and Linux (`notify-send`) have no permission model, so both
calls report granted immediately.
