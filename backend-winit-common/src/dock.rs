// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Cross-platform dock / taskbar operations for winit-based backends.
//!
//! - macOS: app-scoped NSDockTile badge, NSApp requestUserAttention, dock
//!   menu via a runtime-added `applicationDockMenu:` on winit's delegate,
//!   activation policy, and dock-reopen callback via a runtime-added
//!   `applicationShouldHandleReopen:hasVisibleWindows:`.
//! - Windows/Linux: bounce via `winit::Window::request_user_attention`
//!   (handles `FlashWindowEx` / urgency hint natively). Badge is implemented
//!   as a `"(N) "` title prefix on the focused window — a universal pattern
//!   that window managers and taskbars surface. Menu / visibility / reopen
//!   are no-ops (no native analog).

use std::collections::VecDeque;
use std::ffi::{c_int, c_void};
use std::sync::Mutex;

#[cfg(not(target_os = "macos"))]
use std::collections::HashMap;

use winit::window::Window;

use crate::{ParsedMenuItem, LaufeyMenuClickFn};

pub const LAUFEY_DOCK_BOUNCE_INFORMATIONAL: c_int = 10;
pub const LAUFEY_DOCK_BOUNCE_CRITICAL: c_int = 0;

pub type LaufeyDockReopenFn = unsafe extern "C" fn(*mut c_void, bool);

pub struct PendingDockMenu {
  pub items: Vec<ParsedMenuItem>,
  pub callback: Option<LaufeyMenuClickFn>,
  pub callback_data: usize,
}

pub enum DockOp {
  SetBadge(Option<String>),
  Bounce(c_int),
  SetMenu(Option<PendingDockMenu>),
  SetVisible(bool),
  SetReopenHandler(Option<(LaufeyDockReopenFn, usize)>),
}

static DOCK_QUEUE: Mutex<VecDeque<DockOp>> = Mutex::new(VecDeque::new());

pub fn queue_op(op: DockOp) {
  DOCK_QUEUE.lock().unwrap().push_back(op);
}

/// Drain pending dock ops and apply them. Must be called on the main thread
/// (from the winit event loop). `focused` is the currently-focused LAUFEY
/// window, required for Windows/Linux bounce and title-prefix badge.
pub fn drain_and_apply(focused: Option<(&Window, u32)>) {
  let ops: Vec<DockOp> = {
    let mut q = DOCK_QUEUE.lock().unwrap();
    q.drain(..).collect()
  };
  for op in ops {
    match op {
      DockOp::SetBadge(text) => apply_set_badge(text, focused),
      DockOp::Bounce(kind) => apply_bounce(kind, focused),
      DockOp::SetMenu(pm) => apply_set_menu(pm),
      DockOp::SetVisible(visible) => apply_set_visible(visible),
      DockOp::SetReopenHandler(h) => apply_set_reopen_handler(h),
    }
  }
}

// --- Bounce (all platforms via winit) ---

fn apply_bounce(kind: c_int, focused: Option<(&Window, u32)>) {
  let Some((window, _)) = focused else {
    return;
  };
  let attention = if kind == LAUFEY_DOCK_BOUNCE_CRITICAL {
    winit::window::UserAttentionType::Critical
  } else {
    winit::window::UserAttentionType::Informational
  };
  window.request_user_attention(Some(attention));
}

// --- Badge (macOS: NSDockTile; others: title prefix) ---

#[cfg(target_os = "macos")]
fn apply_set_badge(text: Option<String>, _focused: Option<(&Window, u32)>) {
  // SAFETY: called on the main thread from the winit event loop.
  let mtm = unsafe { objc2_foundation::MainThreadMarker::new_unchecked() };
  let app = objc2_app_kit::NSApplication::sharedApplication(mtm);
  let dock_tile = app.dockTile();
  let ns_str = match text.as_deref() {
    Some(t) if !t.is_empty() => Some(objc2_foundation::NSString::from_str(t)),
    _ => None,
  };
  dock_tile.setBadgeLabel(ns_str.as_deref());
}

#[cfg(not(target_os = "macos"))]
fn apply_set_badge(text: Option<String>, focused: Option<(&Window, u32)>) {
  let Some((window, window_id)) = focused else {
    return;
  };
  apply_title_prefix_badge(window, window_id, text);
}

#[cfg(not(target_os = "macos"))]
static BADGE_ORIGINALS: Mutex<Option<HashMap<u32, String>>> = Mutex::new(None);

#[cfg(not(target_os = "macos"))]
fn apply_title_prefix_badge(
  window: &Window,
  window_id: u32,
  text: Option<String>,
) {
  let mut store = BADGE_ORIGINALS.lock().unwrap();
  let originals = store.get_or_insert_with(HashMap::new);
  match text.as_deref() {
    Some(badge) if !badge.is_empty() => {
      let current = window.title();
      let original = originals
        .entry(window_id)
        .or_insert_with(|| current.clone());
      let new_title = format!("({}) {}", badge, original);
      window.set_title(&new_title);
    }
    _ => {
      if let Some(original) = originals.remove(&window_id) {
        window.set_title(&original);
      }
    }
  }
}

// --- Visibility (macOS only) ---

#[cfg(target_os = "macos")]
fn apply_set_visible(visible: bool) {
  // SAFETY: called on the main thread.
  let mtm = unsafe { objc2_foundation::MainThreadMarker::new_unchecked() };
  let app = objc2_app_kit::NSApplication::sharedApplication(mtm);
  let policy = if visible {
    objc2_app_kit::NSApplicationActivationPolicy::Regular
  } else {
    objc2_app_kit::NSApplicationActivationPolicy::Accessory
  };
  app.setActivationPolicy(policy);
}

#[cfg(not(target_os = "macos"))]
fn apply_set_visible(_visible: bool) {
  // No app-level equivalent on Windows or Linux.
}

// --- Menu (macOS only) ---

#[cfg(target_os = "macos")]
mod mac_menu {
  use super::*;
  use objc2::rc::Retained;
  use objc2::runtime::{AnyClass, Imp, Sel};
  use objc2::sel;
  use objc2_app_kit::{NSMenu, NSMenuItem};
  use objc2_foundation::{MainThreadMarker, NSString};
  use std::sync::Once;

  // Wrapper to store Retained<NSMenu> / target in a static. The contents are
  // only ever touched on the main thread (AppKit invariant). We hand out raw
  // pointers across the Send/Sync boundary.
  struct MainThreadOnly<T>(Option<T>);
  unsafe impl<T> Send for MainThreadOnly<T> {}
  unsafe impl<T> Sync for MainThreadOnly<T> {}

  static DOCK_MENU: Mutex<MainThreadOnly<Retained<NSMenu>>> =
    Mutex::new(MainThreadOnly(None));
  static DOCK_ITEM_IDS: Mutex<Vec<String>> = Mutex::new(Vec::new());
  static DOCK_MENU_CALLBACK: Mutex<Option<(LaufeyMenuClickFn, usize)>> =
    Mutex::new(None);
  static DOCK_REOPEN: Mutex<Option<(LaufeyDockReopenFn, usize)>> =
    Mutex::new(None);

  /// Runtime-add `applicationDockMenu:` and
  /// `applicationShouldHandleReopen:hasVisibleWindows:` to winit's
  /// `NSApplicationDelegate` subclass. Idempotent; safe to call repeatedly.
  /// Returns false if the delegate class couldn't be located.
  pub fn install_delegate_methods() -> bool {
    static INSTALL: Once = Once::new();
    static mut INSTALLED: bool = false;

    INSTALL.call_once(|| {
      // SAFETY: accessing the static is gated by Once.
      unsafe {
        INSTALLED = try_install();
      }
      // SAFETY: same — reading inside Once.
      if unsafe { !INSTALLED } {
        eprintln!(
          "laufey: failed to install dock delegate methods on winit's \
           NSApplicationDelegate — dock menu and reopen callback will not \
           fire. This likely means winit changed its delegate class name."
        );
      }
    });
    // SAFETY: once initialized by Once, the value is stable.
    unsafe { INSTALLED }
  }

  unsafe fn try_install() -> bool {
    // winit 0.30 names its delegate "WinitApplicationDelegate". If the class
    // doesn't exist yet (delegate hasn't been created), we can still add
    // methods to it ahead of time. But if the symbol isn't known to the
    // runtime at all, bail.
    let Some(cls) = AnyClass::get(c"WinitApplicationDelegate") else {
      return false;
    };

    // Cast to *mut to allow runtime modification.
    let raw_cls = cls as *const _ as *mut _;

    let dock_menu_sel = sel!(applicationDockMenu:);
    let reopen_sel = sel!(applicationShouldHandleReopen:hasVisibleWindows:);

    // Transmute the concrete trampolines to the generic `Imp` type. Their
    // real signatures match what AppKit invokes for these selectors.
    let dock_imp: Imp =
      std::mem::transmute(application_dock_menu_imp as *const ());
    let reopen_imp: Imp =
      std::mem::transmute(application_should_handle_reopen_imp as *const ());

    // class_addMethod returns false if a method with this selector already
    // exists — silently OK: a future winit may implement these itself.
    let _ = objc2::ffi::class_addMethod(
      raw_cls,
      dock_menu_sel,
      dock_imp,
      c"@@:@".as_ptr(),
    );
    let _ = objc2::ffi::class_addMethod(
      raw_cls,
      reopen_sel,
      reopen_imp,
      c"c@:@c".as_ptr(),
    );
    true
  }

  /// Trampoline: `- (NSMenu*)applicationDockMenu:(NSApplication*)sender`
  /// Returns the stored dock menu, or nil if none set.
  unsafe extern "C-unwind" fn application_dock_menu_imp(
    _self: *mut objc2::runtime::AnyObject,
    _sel: Sel,
    _sender: *mut objc2::runtime::AnyObject,
  ) -> *mut NSMenu {
    let guard = DOCK_MENU.lock().unwrap();
    match &guard.0 {
      Some(menu) => Retained::as_ptr(menu) as *mut NSMenu,
      None => std::ptr::null_mut(),
    }
  }

  /// Trampoline:
  /// `- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender
  ///                     hasVisibleWindows:(BOOL)hasVisible`
  /// Invokes the registered callback and always returns NO (swallow).
  unsafe extern "C-unwind" fn application_should_handle_reopen_imp(
    _self: *mut objc2::runtime::AnyObject,
    _sel: Sel,
    _sender: *mut objc2::runtime::AnyObject,
    has_visible_windows: bool,
  ) -> bool {
    if let Some((cb, data)) = *DOCK_REOPEN.lock().unwrap() {
      // SAFETY: the backend set this pointer via set_dock_reopen_handler;
      // it lives as long as the app since we never release it.
      unsafe { cb(data as *mut c_void, has_visible_windows) };
    }
    false
  }

  /// Builds an NSMenu from parsed items. All leaf items share a common
  /// target (self-class) and action; their `tag` is an index into
  /// `DOCK_ITEM_IDS` so we can map click → id.
  pub fn build_nsmenu(items: &[ParsedMenuItem]) -> Retained<NSMenu> {
    // SAFETY: called on the main thread.
    let mtm = unsafe { MainThreadMarker::new_unchecked() };
    let menu = NSMenu::new(mtm);
    let mut ids = DOCK_ITEM_IDS.lock().unwrap();
    ids.clear();
    append_items(&menu, items, &mut ids, mtm);
    menu
  }

  fn append_items(
    menu: &NSMenu,
    items: &[ParsedMenuItem],
    ids: &mut Vec<String>,
    mtm: MainThreadMarker,
  ) {
    for item in items {
      match item {
        ParsedMenuItem::Separator => {
          menu.addItem(&NSMenuItem::separatorItem(mtm));
        }
        ParsedMenuItem::Role { role } => {
          // Best-effort: create a titled item with no action for roles we
          // don't map. Dock menus rarely use roles. Only handle "quit".
          let title = role_title(role);
          let ns_title = NSString::from_str(&title);
          let ns_item = NSMenuItem::new(mtm);
          ns_item.setTitle(&ns_title);
          if role == "quit" {
            // NSApp terminate:
            let sel = objc2::sel!(terminate:);
            unsafe { ns_item.setAction(Some(sel)) };
          }
          menu.addItem(&ns_item);
        }
        ParsedMenuItem::Item {
          id,
          label,
          accelerator: _,
          enabled,
        } => {
          let tag = ids.len() as isize;
          ids.push(id.clone());
          let ns_title = NSString::from_str(label);
          let ns_item = NSMenuItem::new(mtm);
          ns_item.setTitle(&ns_title);
          ns_item.setEnabled(*enabled);
          ns_item.setTag(tag);
          unsafe {
            ns_item.setAction(Some(objc2::sel!(laufeyDockItemClicked:)));
            ns_item.setTarget(Some(&menu_target(mtm)));
          }
          menu.addItem(&ns_item);
        }
        ParsedMenuItem::Submenu {
          label,
          items: children,
        } => {
          let parent = NSMenuItem::new(mtm);
          parent.setTitle(&NSString::from_str(label));
          let submenu = NSMenu::new(mtm);
          append_items(&submenu, children, ids, mtm);
          parent.setSubmenu(Some(&submenu));
          menu.addItem(&parent);
        }
      }
    }
  }

  fn role_title(role: &str) -> String {
    match role {
      "quit" => "Quit".to_string(),
      other => other.to_string(),
    }
  }

  pub fn set_menu(pm: Option<PendingDockMenu>) {
    if !install_delegate_methods() {
      return;
    }
    match pm {
      Some(pm) => {
        let menu = build_nsmenu(&pm.items);
        *DOCK_MENU_CALLBACK.lock().unwrap() =
          pm.callback.map(|cb| (cb, pm.callback_data));
        *DOCK_MENU.lock().unwrap() = MainThreadOnly(Some(menu));
      }
      None => {
        *DOCK_MENU.lock().unwrap() = MainThreadOnly(None);
        *DOCK_MENU_CALLBACK.lock().unwrap() = None;
      }
    }
  }

  pub fn set_reopen_handler(h: Option<(LaufeyDockReopenFn, usize)>) {
    install_delegate_methods();
    *DOCK_REOPEN.lock().unwrap() = h;
  }

  // --- Shared click target for dock menu items ---

  objc2::define_class!(
    // SAFETY:
    // - NSObject is the superclass and has no special subclassing requirements.
    // - DockMenuTarget does not implement Drop.
    #[unsafe(super(objc2::runtime::NSObject))]
    #[thread_kind = objc2::MainThreadOnly]
    #[name = "LaufeyDockMenuTarget"]
    pub(super) struct DockMenuTarget;

    impl DockMenuTarget {
      #[unsafe(method(laufeyDockItemClicked:))]
      fn laufey_dock_item_clicked(&self, sender: *mut NSMenuItem) {
        if sender.is_null() {
          return;
        }
        // SAFETY: sender is a valid NSMenuItem supplied by AppKit.
        let tag = unsafe { (*sender).tag() };
        let ids = DOCK_ITEM_IDS.lock().unwrap();
        let Some(id) = ids.get(tag as usize).cloned() else {
          return;
        };
        drop(ids);
        let cb = *DOCK_MENU_CALLBACK.lock().unwrap();
        if let Some((cb, data)) = cb {
          let c_id = std::ffi::CString::new(id).unwrap();
          // window_id = 0 because the dock menu is app-scoped.
          // SAFETY: the backend owns this fn pointer for the app's lifetime.
          unsafe { cb(data as *mut c_void, 0, c_id.as_ptr()) };
        }
      }
    }
  );

  fn menu_target(mtm: MainThreadMarker) -> Retained<DockMenuTarget> {
    use objc2::MainThreadOnly;
    thread_local! {
      static TARGET: std::cell::OnceCell<Retained<DockMenuTarget>>
        = const { std::cell::OnceCell::new() };
    }
    TARGET.with(|cell| {
      cell
        .get_or_init(|| {
          let alloc = DockMenuTarget::alloc(mtm);
          // SAFETY: init follows alloc for an NSObject subclass.
          unsafe { objc2::msg_send![alloc, init] }
        })
        .clone()
    })
  }
}

#[cfg(target_os = "macos")]
fn apply_set_menu(pm: Option<PendingDockMenu>) {
  mac_menu::set_menu(pm);
}

#[cfg(not(target_os = "macos"))]
fn apply_set_menu(_pm: Option<PendingDockMenu>) {
  // No dock-menu analog on Windows (Jump List) or Linux.
}

// --- Reopen handler (macOS only) ---

#[cfg(target_os = "macos")]
fn apply_set_reopen_handler(h: Option<(LaufeyDockReopenFn, usize)>) {
  mac_menu::set_reopen_handler(h);
}

#[cfg(not(target_os = "macos"))]
fn apply_set_reopen_handler(_h: Option<(LaufeyDockReopenFn, usize)>) {
  // No reopen event on Windows or Linux.
}
