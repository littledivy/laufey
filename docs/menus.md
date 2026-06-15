# Menus

laufey supports three kinds of menus: an application menu bar, per-window
context menus, and the developer tools. A menu is described by a slice of
`MenuItem` values, which can be regular items, submenus, separators, or standard
roles such as quit, copy, and paste. Items may carry a keyboard accelerator.
When the user clicks an item that has an identifier, your callback is invoked
with that identifier.

```rust
use laufey::MenuItem;

let menu = [MenuItem::Submenu {
  label: "File".into(),
  items: vec![
    MenuItem::Item {
      label: "Open".into(),
      id: Some("open".into()),
      accelerator: Some("CmdOrCtrl+O".into()),
      enabled: true,
    },
    MenuItem::Separator,
    MenuItem::Role { role: "quit".into() },
  ],
}];

win.set_menu(&menu, |id| println!("menu: {id}"));
win.show_context_menu(x, y, &menu, |id| println!("context: {id}"));
win.open_devtools();
```

On macOS the application menu is the global menu bar at the top of the screen,
and laufey swaps it as windows take focus. On Windows and Linux the menu is
attached to the individual window. A context menu is a pop-up shown at a point
you specify, in window coordinates.

The application menu does not work under the CEF and Winit backends on Linux. A
`GtkMenuBar` must be packed into a GtkWindow placed above the browser, and
reparenting CEF into a client-owned GtkWindow through
`CefWindowInfo::SetAsChild` breaks on XWayland, where cross-client X11 child
windows are not supported natively. Context menus work everywhere, because a
`GtkMenu` pop-up does not need a containing window.
