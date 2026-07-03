//! laufey native-chrome e2e driver (Linux).
//!
//! Runs under `xvfb-run -a dbus-run-session -- native_e2e_driver <backend-bin>
//! [args...]` and plays the role of the desktop shell so that the tray
//! implementation takes its D-Bus code path instead of the legacy
//! GtkStatusIcon/XEmbed fallback. Works for *any* backend binary: CEF/WebView
//! (libayatana-appindicator) and Winit (the `tray-icon` crate) both speak
//! StatusNotifierItem + com.canonical.dbusmenu.
//!
//! It:
//!   1. owns `org.kde.StatusNotifierWatcher` and reports a host registered,
//!   2. owns `org.freedesktop.Notifications` (stub) to capture Notify calls,
//!   3. spawns the backend + runtime and waits for the tray item to register,
//!   4. reads the item's `org.kde.StatusNotifierItem` properties, walks its
//!      `com.canonical.dbusmenu` menu, and fires a menu `Event` that must
//!      round-trip to the app's `laufey_menu_click_fn`.
//!
//! On non-Linux targets this compiles to a stub so `cargo check --workspace`
//! stays green everywhere. See docs/e2e-testing.md §7.1.

#[cfg(not(target_os = "linux"))]
fn main() {
  eprintln!("native_e2e_driver is Linux-only (D-Bus StatusNotifier observer)");
  std::process::exit(2);
}

#[cfg(target_os = "linux")]
fn main() -> zbus::Result<()> {
  linux::run()
}

#[cfg(target_os = "linux")]
mod linux {
  use std::collections::HashMap;
  use std::sync::Arc;
  use std::time::Duration;

  use tokio::sync::mpsc;
  use zbus::message::Header;
  use zbus::zvariant::{OwnedValue, Value};
  use zbus::{connection, interface, Connection, Proxy};

  /// Reported when an SNI registers: (bus_name, object_path).
  type ItemTx = mpsc::UnboundedSender<(String, String)>;

  struct Watcher {
    items: std::sync::Mutex<Vec<String>>,
    tx: ItemTx,
  }

  #[interface(name = "org.kde.StatusNotifierWatcher")]
  impl Watcher {
    /// libayatana-appindicator and the `tray-icon` crate pass the *object
    /// path* here; the item's bus name is then the D-Bus message sender (not
    /// the argument). KDE apps pass a bus name instead. Handle both.
    async fn register_status_notifier_item(
      &self,
      service: &str,
      #[zbus(header)] hdr: Header<'_>,
    ) {
      let sender = hdr.sender().map(|s| s.to_string()).unwrap_or_default();
      let (bus_name, path) = if service.starts_with('/') {
        (sender, service.to_string())
      } else {
        (service.to_string(), "/StatusNotifierItem".to_string())
      };
      eprintln!("[e2e] watcher: item registered bus={bus_name} path={path}");
      self.items.lock().unwrap().push(bus_name.clone());
      let _ = self.tx.send((bus_name, path));
    }

    async fn register_status_notifier_host(&self, _service: &str) {}

    #[zbus(property)]
    async fn registered_status_notifier_items(&self) -> Vec<String> {
      self.items.lock().unwrap().clone()
    }

    /// Must be true or the item falls back to XEmbed and never hits D-Bus.
    #[zbus(property)]
    async fn is_status_notifier_host_registered(&self) -> bool {
      true
    }

    #[zbus(property)]
    async fn protocol_version(&self) -> i32 {
      0
    }
  }

  #[derive(Debug, Clone)]
  struct NotifyCall {
    summary: String,
    body: String,
    #[allow(dead_code)]
    actions: Vec<String>,
  }

  struct Notifications {
    tx: mpsc::UnboundedSender<NotifyCall>,
    next_id: std::sync::atomic::AtomicU32,
  }

  #[interface(name = "org.freedesktop.Notifications")]
  impl Notifications {
    #[allow(clippy::too_many_arguments)]
    async fn notify(
      &self,
      _app_name: &str,
      _replaces_id: u32,
      _app_icon: &str,
      summary: &str,
      body: &str,
      actions: Vec<String>,
      _hints: HashMap<String, OwnedValue>,
      _timeout: i32,
    ) -> u32 {
      let _ = self.tx.send(NotifyCall {
        summary: summary.to_string(),
        body: body.to_string(),
        actions,
      });
      self
        .next_id
        .fetch_add(1, std::sync::atomic::Ordering::SeqCst)
    }

    async fn get_capabilities(&self) -> Vec<String> {
      vec!["body".into(), "actions".into()]
    }

    async fn get_server_information(&self) -> (String, String, String, String) {
      (
        "laufey-e2e".into(),
        "laufey".into(),
        "0".into(),
        "1.2".into(),
      )
    }

    async fn close_notification(&self, _id: u32) {}
  }

  fn check(name: &str, ok: bool, failed: &std::sync::atomic::AtomicBool) {
    if ok {
      eprintln!("[e2e] PASS {name}");
    } else {
      eprintln!("[e2e] FAIL {name}");
      failed.store(true, std::sync::atomic::Ordering::SeqCst);
    }
  }

  /// One dbusmenu layout node: (id, properties, children).
  struct MenuNode {
    id: i32,
    props: HashMap<String, OwnedValue>,
    children: Vec<MenuNode>,
  }

  /// Parse the recursive `(ia{sv}av)` layout returned by GetLayout.
  fn parse_node(v: &Value<'_>) -> Option<MenuNode> {
    let Value::Structure(s) = v else { return None };
    let fields = s.fields();
    let id = if let Value::I32(i) = &fields[0] {
      *i
    } else {
      return None;
    };
    let mut props = HashMap::new();
    if let Value::Dict(d) = &fields[1] {
      for (k, val) in d.iter() {
        if let Value::Str(k) = k {
          if let Ok(ov) = OwnedValue::try_from(val.clone()) {
            props.insert(k.to_string(), ov);
          }
        }
      }
    }
    let mut children = Vec::new();
    if let Value::Array(arr) = &fields[2] {
      for child in arr.iter() {
        if let Value::Value(inner) = child {
          if let Some(n) = parse_node(inner) {
            children.push(n);
          }
        }
      }
    }
    Some(MenuNode {
      id,
      props,
      children,
    })
  }

  fn find_by_label<'a>(
    node: &'a MenuNode,
    label: &str,
  ) -> Option<&'a MenuNode> {
    if let Some(l) = node
      .props
      .get("label")
      .and_then(|v| String::try_from(v.clone()).ok())
    {
      if l == label {
        return Some(node);
      }
    }
    for c in &node.children {
      if let Some(found) = find_by_label(c, label) {
        return Some(found);
      }
    }
    None
  }

  async fn run_assertions(
    conn: &Connection,
    bus_name: &str,
    sni_path: &str,
    failed: &std::sync::atomic::AtomicBool,
  ) -> zbus::Result<()> {
    let props =
      Proxy::new(conn, bus_name, sni_path, "org.freedesktop.DBus.Properties")
        .await?;
    let all: HashMap<String, OwnedValue> = props
      .call("GetAll", &("org.kde.StatusNotifierItem",))
      .await?;

    let status = all
      .get("Status")
      .and_then(|v| String::try_from(v.clone()).ok())
      .unwrap_or_default();
    check("tray Status == Active", status == "Active", failed);

    let has_icon =
      all.contains_key("IconThemePath") || all.contains_key("IconName");
    check(
      "tray icon set (IconThemePath/IconName present)",
      has_icon,
      failed,
    );

    let menu_path = all
      .get("Menu")
      .and_then(|v| zbus::zvariant::OwnedObjectPath::try_from(v.clone()).ok())
      .map(|p| p.as_str().to_string());
    check(
      "tray exposes a Menu object path",
      menu_path.is_some(),
      failed,
    );

    let Some(menu_path) = menu_path else {
      return Ok(());
    };
    let menu =
      Proxy::new(conn, bus_name, menu_path.as_str(), "com.canonical.dbusmenu")
        .await?;

    let (_revision, root): (u32, OwnedValue) = menu
      .call("GetLayout", &(0i32, -1i32, Vec::<String>::new()))
      .await?;
    let root_node = parse_node(&root).expect("layout root");
    check("menu has children", !root_node.children.is_empty(), failed);

    if let Some(target) = find_by_label(&root_node, "Ping") {
      check("menu contains 'Ping' item", true, failed);
      let _: bool = menu.call("AboutToShow", &(0i32,)).await?;
      let empty = Value::from("");
      menu
        .call::<_, _, ()>("Event", &(target.id, "clicked", &empty, 0u32))
        .await?;
      eprintln!("[e2e] fired dbusmenu Event(clicked) on id={}", target.id);
    } else {
      check("menu contains 'Ping' item", false, failed);
    }
    Ok(())
  }

  #[tokio::main]
  pub async fn run() -> zbus::Result<()> {
    let failed = Arc::new(std::sync::atomic::AtomicBool::new(false));
    let (item_tx, mut item_rx) = mpsc::unbounded_channel::<(String, String)>();
    let (notif_tx, mut notif_rx) = mpsc::unbounded_channel::<NotifyCall>();

    let watcher = Watcher {
      items: std::sync::Mutex::new(Vec::new()),
      tx: item_tx,
    };
    let notifications = Notifications {
      tx: notif_tx,
      next_id: std::sync::atomic::AtomicU32::new(1),
    };

    let conn = connection::Builder::session()?
      .name("org.kde.StatusNotifierWatcher")?
      .name("org.freedesktop.Notifications")?
      .serve_at("/StatusNotifierWatcher", watcher)?
      .serve_at("/org/freedesktop/Notifications", notifications)?
      .build()
      .await?;
    eprintln!("[e2e] shell services up; launching runtime");

    let mut args = std::env::args().skip(1);
    let program = args
      .next()
      .expect("usage: native_e2e_driver <backend-cmd> [args...]");
    let mut child = tokio::process::Command::new(program)
      .args(args)
      .spawn()
      .expect("spawn backend");

    let (bus_name, sni_path) =
      tokio::time::timeout(Duration::from_secs(20), item_rx.recv())
        .await
        .ok()
        .flatten()
        .expect("[e2e] FAIL: backend never registered a StatusNotifierItem");
    check("StatusNotifierItem registered on bus", true, &failed);

    tokio::time::sleep(Duration::from_millis(300)).await;
    run_assertions(&conn, &bus_name, &sni_path, &failed).await?;

    if let Ok(Some(n)) =
      tokio::time::timeout(Duration::from_secs(3), notif_rx.recv()).await
    {
      eprintln!(
        "[e2e] captured Notify summary={:?} body={:?}",
        n.summary, n.body
      );
      check(
        "notification summary non-empty",
        !n.summary.is_empty(),
        &failed,
      );
    }

    let _ = child.start_kill();
    if failed.load(std::sync::atomic::Ordering::SeqCst) {
      eprintln!("[e2e] OVERALL FAIL");
      std::process::exit(1);
    }
    eprintln!("[e2e] OVERALL PASS");
    Ok(())
  }
}
