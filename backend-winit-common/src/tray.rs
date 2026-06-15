// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Cross-platform tray / status-bar icons for winit-based backends.
//! Thin wrapper around the `tray-icon` crate (from tauri-apps, same family
//! as `muda`). Handles macOS `NSStatusItem`, Windows `Shell_NotifyIcon`, and
//! Linux tray via the crate's internal StatusNotifier / AppIndicator logic.

use std::collections::HashMap;
use std::ffi::{c_char, c_void, CStr};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Mutex;

use tray_icon::menu::Menu as TrayMenu;
use tray_icon::{
  Icon, TrayIcon, TrayIconAttributes, TrayIconBuilder, TrayIconEvent,
  TrayIconEventReceiver,
};

use crate::{LaufeyMenuClickFn, ParsedMenuItem};

pub type LaufeyTrayClickFn = unsafe extern "C" fn(*mut c_void, u32);

static NEXT_TRAY_ID: AtomicU32 = AtomicU32::new(1);

/// Allocate a new tray icon id.
pub fn allocate_tray_id() -> u32 {
  NEXT_TRAY_ID.fetch_add(1, Ordering::Relaxed)
}

struct TrayEntry {
  icon: TrayIcon,
  click_fn: Option<(LaufeyTrayClickFn, usize)>,
  dblclick_fn: Option<(LaufeyTrayClickFn, usize)>,
  light_png: Option<Vec<u8>>,
  dark_png: Option<Vec<u8>>,
}

// SAFETY: `tray_icon::TrayIcon` is only touched on the main thread, which
// we guarantee by dispatching all tray ops through `CommonEvent::TrayTask`.
unsafe impl Send for TrayEntry {}

static TRAYS: Mutex<Option<HashMap<u32, TrayEntry>>> = Mutex::new(None);

type TrayRect = (i32, i32, i32, i32);

/// Tray icon bounds in logical, top-left screen coordinates (the same space
/// the winit backend uses for window positions), keyed by tray id. Refreshed
/// on the main thread by [`poll_tray_events`]; read from any thread by the
/// `get_tray_icon_bounds` trampoline. Empty on platforms where `tray-icon`
/// can't report geometry (Linux).
static TRAY_RECTS: Mutex<Option<HashMap<u32, TrayRect>>> = Mutex::new(None);

fn trays() -> std::sync::MutexGuard<'static, Option<HashMap<u32, TrayEntry>>> {
  let mut g = TRAYS.lock().unwrap();
  if g.is_none() {
    *g = Some(HashMap::new());
  }
  g
}

/// The cached bounds of a tray icon, or `None` if not known (no icon, or a
/// platform that can't report geometry).
pub fn tray_bounds(tray_id: u32) -> Option<(i32, i32, i32, i32)> {
  TRAY_RECTS
    .lock()
    .unwrap()
    .as_ref()
    .and_then(|m| m.get(&tray_id).copied())
}

/// Refresh [`TRAY_RECTS`] from each tray's current screen rect. `tray-icon`
/// reports a top-left-origin rect in physical pixels; we divide by the
/// primary monitor's `scale_factor` to land in the logical space used for
/// window positions. Must run on the main thread.
fn refresh_tray_rects(scale_factor: f64) {
  let s = if scale_factor > 0.0 {
    scale_factor
  } else {
    1.0
  };
  let mut out: HashMap<u32, (i32, i32, i32, i32)> = HashMap::new();
  {
    let guard = trays();
    if let Some(map) = guard.as_ref() {
      for (id, entry) in map.iter() {
        if let Some(rect) = entry.icon.rect() {
          out.insert(
            *id,
            (
              (rect.position.x / s).round() as i32,
              (rect.position.y / s).round() as i32,
              (rect.size.width as f64 / s).round() as i32,
              (rect.size.height as f64 / s).round() as i32,
            ),
          );
        }
      }
    }
  }
  *TRAY_RECTS.lock().unwrap() = Some(out);
}

/// Commands queued from non-main threads (the backend C trampolines) and
/// applied on the main thread when `CommonEvent::TrayTask` fires.
pub enum TrayOp {
  Create {
    tray_id: u32,
  },
  Destroy {
    tray_id: u32,
  },
  SetIcon {
    tray_id: u32,
    png: Vec<u8>,
  },
  SetTooltip {
    tray_id: u32,
    text: Option<String>,
  },
  SetMenu {
    tray_id: u32,
    items: Vec<ParsedMenuItem>,
    callback: Option<LaufeyMenuClickFn>,
    callback_data: usize,
  },
  ClearMenu {
    tray_id: u32,
  },
  SetClickHandler {
    tray_id: u32,
    handler: Option<LaufeyTrayClickFn>,
    user_data: usize,
  },
  SetDoubleClickHandler {
    tray_id: u32,
    handler: Option<LaufeyTrayClickFn>,
    user_data: usize,
  },
  SetIconDark {
    tray_id: u32,
    png: Vec<u8>,
  },
}

static TRAY_QUEUE: Mutex<Vec<TrayOp>> = Mutex::new(Vec::new());

pub fn queue_op(op: TrayOp) {
  TRAY_QUEUE.lock().unwrap().push(op);
}

/// Drain queued ops and apply them on the main thread.
pub fn drain_and_apply() {
  let ops: Vec<TrayOp> = {
    let mut q = TRAY_QUEUE.lock().unwrap();
    std::mem::take(&mut *q)
  };
  for op in ops {
    apply(op);
  }
}

fn apply(op: TrayOp) {
  match op {
    TrayOp::Create { tray_id } => apply_create(tray_id),
    TrayOp::Destroy { tray_id } => {
      let mut guard = trays();
      if let Some(map) = guard.as_mut() {
        map.remove(&tray_id);
      }
    }
    TrayOp::SetIcon { tray_id, png } => {
      {
        let mut guard = trays();
        if let Some(entry) = guard.as_mut().and_then(|m| m.get_mut(&tray_id)) {
          entry.light_png = if png.is_empty() { None } else { Some(png) };
        }
      }
      apply_active_icon(tray_id);
    }
    TrayOp::SetTooltip { tray_id, text } => {
      let mut guard = trays();
      if let Some(entry) = guard.as_mut().and_then(|m| m.get_mut(&tray_id)) {
        let _ = entry.icon.set_tooltip(text.as_deref());
      }
    }
    TrayOp::SetMenu {
      tray_id,
      items,
      callback,
      callback_data,
    } => apply_set_menu(tray_id, items, callback, callback_data),
    TrayOp::ClearMenu { tray_id } => {
      let mut guard = trays();
      if let Some(entry) = guard.as_mut().and_then(|m| m.get_mut(&tray_id)) {
        entry.icon.set_menu(None);
      }
    }
    TrayOp::SetClickHandler {
      tray_id,
      handler,
      user_data,
    } => {
      let mut guard = trays();
      if let Some(entry) = guard.as_mut().and_then(|m| m.get_mut(&tray_id)) {
        entry.click_fn = handler.map(|f| (f, user_data));
      }
    }
    TrayOp::SetDoubleClickHandler {
      tray_id,
      handler,
      user_data,
    } => {
      let mut guard = trays();
      if let Some(entry) = guard.as_mut().and_then(|m| m.get_mut(&tray_id)) {
        entry.dblclick_fn = handler.map(|f| (f, user_data));
      }
    }
    TrayOp::SetIconDark { tray_id, png } => {
      let mut guard = trays();
      if let Some(entry) = guard.as_mut().and_then(|m| m.get_mut(&tray_id)) {
        entry.dark_png = if png.is_empty() { None } else { Some(png) };
      }
      apply_active_icon(tray_id);
    }
  }
}

fn apply_create(tray_id: u32) {
  let attrs = TrayIconAttributes::default();
  let built = TrayIconBuilder::new()
    .with_id(tray_icon::TrayIconId::new(tray_id.to_string()))
    .with_tooltip(attrs.tooltip.unwrap_or_default())
    .build();
  let Ok(icon) = built else { return };
  let mut guard = trays();
  let map = guard.get_or_insert_with(HashMap::new);
  map.insert(
    tray_id,
    TrayEntry {
      icon,
      click_fn: None,
      dblclick_fn: None,
      light_png: None,
      dark_png: None,
    },
  );
}

fn apply_set_menu(
  tray_id: u32,
  items: Vec<ParsedMenuItem>,
  callback: Option<LaufeyMenuClickFn>,
  callback_data: usize,
) {
  // Register menu-item callbacks in the shared MENU_CLICK_STORE; the
  // existing `poll_menu_events` will dispatch them. We pass `tray_id` as
  // the `window_id` argument — the runtime-side Rust wrapper knows to
  // interpret it as a tray id for tray menus.
  crate::register_menu_callbacks(&items, callback, callback_data, tray_id);
  let menu = build_tray_menu(&items);
  let mut guard = trays();
  if let Some(entry) = guard.as_mut().and_then(|m| m.get_mut(&tray_id)) {
    entry.icon.set_menu(Some(Box::new(menu)));
  }
}

fn build_tray_menu(items: &[ParsedMenuItem]) -> TrayMenu {
  // tray-icon's menu module re-exports muda's types. Reuse the same
  // building logic as the existing context-menu builder.
  let menu = TrayMenu::new();
  for item in items {
    append_item_to_menu(&menu, item);
  }
  menu
}

fn append_item_to_menu(menu: &TrayMenu, item: &ParsedMenuItem) {
  use tray_icon::menu::{MenuItem, PredefinedMenuItem, Submenu};
  match item {
    ParsedMenuItem::Submenu {
      label,
      items: children,
    } => {
      let submenu = Submenu::new(label, true);
      for child in children {
        append_item_to_submenu(&submenu, child);
      }
      let _ = menu.append(&submenu);
    }
    ParsedMenuItem::Item {
      id,
      label,
      accelerator: _,
      enabled,
    } => {
      let item = MenuItem::with_id(id.as_str(), label, *enabled, None);
      let _ = menu.append(&item);
    }
    ParsedMenuItem::Separator => {
      let _ = menu.append(&PredefinedMenuItem::separator());
    }
    ParsedMenuItem::Role { role } => {
      if let Some(item) = predefined_menu_item(role) {
        let _ = menu.append(&item);
      }
    }
  }
}

fn append_item_to_submenu(
  submenu: &tray_icon::menu::Submenu,
  item: &ParsedMenuItem,
) {
  use tray_icon::menu::{MenuItem, PredefinedMenuItem, Submenu};
  match item {
    ParsedMenuItem::Submenu {
      label,
      items: children,
    } => {
      let child = Submenu::new(label, true);
      for c in children {
        append_item_to_submenu(&child, c);
      }
      let _ = submenu.append(&child);
    }
    ParsedMenuItem::Item {
      id,
      label,
      accelerator: _,
      enabled,
    } => {
      let item = MenuItem::with_id(id.as_str(), label, *enabled, None);
      let _ = submenu.append(&item);
    }
    ParsedMenuItem::Separator => {
      let _ = submenu.append(&PredefinedMenuItem::separator());
    }
    ParsedMenuItem::Role { role } => {
      if let Some(item) = predefined_menu_item(role) {
        let _ = submenu.append(&item);
      }
    }
  }
}

fn predefined_menu_item(
  role: &str,
) -> Option<tray_icon::menu::PredefinedMenuItem> {
  use tray_icon::menu::PredefinedMenuItem as P;
  Some(match role {
    "quit" => P::quit(None),
    "copy" => P::copy(None),
    "cut" => P::cut(None),
    "paste" => P::paste(None),
    "selectall" | "selectAll" => P::select_all(None),
    "undo" => P::undo(None),
    "redo" => P::redo(None),
    "minimize" => P::minimize(None),
    "close" => P::close_window(None),
    "hide" => P::hide(None),
    "hideothers" | "hideOthers" => P::hide_others(None),
    "showall" | "showAll" => P::show_all(None),
    "about" => P::about(None, None),
    "togglefullscreen" | "toggleFullScreen" => P::fullscreen(None),
    "separator" => P::separator(),
    _ => return None,
  })
}

fn decode_png(bytes: &[u8]) -> Option<Icon> {
  let img = image::load_from_memory_with_format(bytes, image::ImageFormat::Png)
    .ok()?
    .into_rgba8();
  let (w, h) = img.dimensions();
  Icon::from_rgba(img.into_raw(), w, h).ok()
}

/// Resolve the currently-active icon (dark variant if the system is in
/// dark mode and a dark icon is registered, else light) and apply it.
fn apply_active_icon(tray_id: u32) {
  let png = {
    let guard = trays();
    let Some(entry) = guard.as_ref().and_then(|m| m.get(&tray_id)) else {
      return;
    };
    let want_dark = is_dark_mode();
    if want_dark {
      entry.dark_png.clone().or_else(|| entry.light_png.clone())
    } else {
      entry.light_png.clone()
    }
  };
  let Some(bytes) = png else { return };
  let Some(icon) = decode_png(&bytes) else {
    return;
  };
  let mut guard = trays();
  if let Some(entry) = guard.as_mut().and_then(|m| m.get_mut(&tray_id)) {
    let _ = entry.icon.set_icon(Some(icon));
  }
}

/// Re-apply every tray's active icon. Called when the system appearance
/// changes.
pub fn reapply_all_icons_for_theme_change() {
  let ids: Vec<u32> = {
    let guard = trays();
    guard
      .as_ref()
      .map(|m| m.keys().copied().collect())
      .unwrap_or_default()
  };
  for id in ids {
    apply_active_icon(id);
  }
}

#[cfg(target_os = "macos")]
fn is_dark_mode() -> bool {
  // NSApp.effectiveAppearance.name == NSAppearanceNameDarkAqua
  // SAFETY: called on the main thread from the winit event loop.
  unsafe {
    let mtm = objc2_foundation::MainThreadMarker::new_unchecked();
    let app = objc2_app_kit::NSApplication::sharedApplication(mtm);
    let appearance = app.effectiveAppearance();
    let name = appearance.name();
    name.to_string().to_lowercase().contains("dark")
  }
}

#[cfg(target_os = "windows")]
fn is_dark_mode() -> bool {
  // Read HKCU\...\Themes\Personalize\AppsUseLightTheme (DWORD: 0=dark, 1=light).
  use windows_sys::Win32::System::Registry::{
    RegCloseKey, RegOpenKeyExW, RegQueryValueExW, HKEY, HKEY_CURRENT_USER,
    KEY_READ, REG_DWORD,
  };
  let subkey: Vec<u16> =
    "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"
      .encode_utf16()
      .chain(std::iter::once(0))
      .collect();
  let value_name: Vec<u16> = "AppsUseLightTheme"
    .encode_utf16()
    .chain(std::iter::once(0))
    .collect();
  unsafe {
    let mut key: HKEY = std::ptr::null_mut();
    if RegOpenKeyExW(HKEY_CURRENT_USER, subkey.as_ptr(), 0, KEY_READ, &mut key)
      != 0
    {
      return false;
    }
    let mut data: u32 = 1;
    let mut size: u32 = std::mem::size_of::<u32>() as u32;
    let mut kind: u32 = 0;
    let rc = RegQueryValueExW(
      key,
      value_name.as_ptr(),
      std::ptr::null_mut(),
      &mut kind,
      &mut data as *mut u32 as *mut u8,
      &mut size,
    );
    RegCloseKey(key);
    if rc != 0 || kind != REG_DWORD {
      return false;
    }
    data == 0
  }
}

#[cfg(not(any(target_os = "macos", target_os = "windows")))]
fn is_dark_mode() -> bool {
  // Linux has no standardized runtime dark-mode signal reachable without
  // a full desktop-environment library. Always return false; users who
  // care can set only the light icon.
  false
}

/// Drain tray left-click events and dispatch them to user-registered
/// callbacks. Menu-item clicks are already handled by the shared
/// `poll_menu_events` (tray menus register into the same
/// `MENU_CLICK_STORE`). Call once per event loop tick on the main thread.
pub fn poll_tray_events(scale_factor: f64) {
  // Keep cached tray bounds current so get_tray_icon_bounds can be answered
  // synchronously from the Deno runtime thread.
  refresh_tray_rects(scale_factor);

  // Detect theme changes between ticks and re-apply every tray's icon.
  // `is_dark_mode` is a cheap call (KVO lookup on macOS, single registry
  // read on Windows, constant on Linux), and this only runs once per
  // event-loop wake — no noticeable cost.
  {
    use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
    static INITIALIZED: AtomicBool = AtomicBool::new(false);
    static LAST_DARK: AtomicU8 = AtomicU8::new(0);
    let dark_now = is_dark_mode();
    let dark_now_u8 = dark_now as u8;
    if !INITIALIZED.swap(true, Ordering::AcqRel) {
      LAST_DARK.store(dark_now_u8, Ordering::Release);
    } else if LAST_DARK.swap(dark_now_u8, Ordering::AcqRel) != dark_now_u8 {
      reapply_all_icons_for_theme_change();
    }
  }

  let rx: &TrayIconEventReceiver = TrayIconEvent::receiver();
  while let Ok(event) = rx.try_recv() {
    let (tray_id, is_double) = match event {
      TrayIconEvent::Click {
        ref id,
        button,
        button_state,
        ..
      } => {
        if button != tray_icon::MouseButton::Left
          || button_state != tray_icon::MouseButtonState::Up
        {
          continue;
        }
        let tid: u32 = id.0.parse().unwrap_or(0);
        if tid == 0 {
          continue;
        }
        (tid, false)
      }
      TrayIconEvent::DoubleClick { ref id, button, .. } => {
        if button != tray_icon::MouseButton::Left {
          continue;
        }
        let tid: u32 = id.0.parse().unwrap_or(0);
        if tid == 0 {
          continue;
        }
        (tid, true)
      }
      _ => continue,
    };
    let (click, dblclick) = {
      let guard = trays();
      let entry = guard.as_ref().and_then(|m| m.get(&tray_id));
      (
        entry.and_then(|e| e.click_fn),
        entry.and_then(|e| e.dblclick_fn),
      )
    };
    if is_double {
      if let Some((cb, data)) = dblclick {
        // SAFETY: user-registered callback; lifetime maintained by caller.
        unsafe { cb(data as *mut c_void, tray_id) };
      }
    } else if let Some((cb, data)) = click {
      // SAFETY: user-registered callback; lifetime maintained by caller.
      unsafe { cb(data as *mut c_void, tray_id) };
    }
  }
}

/// Helper used by the C trampolines to decode an owned `Vec<u8>` from a
/// `(ptr, len)` pair.
#[doc(hidden)]
pub unsafe fn slice_to_vec(ptr: *const c_void, len: usize) -> Vec<u8> {
  if ptr.is_null() || len == 0 {
    return Vec::new();
  }
  std::slice::from_raw_parts(ptr as *const u8, len).to_vec()
}

/// Helper to decode an optional C string.
#[doc(hidden)]
pub unsafe fn cstr_opt(ptr: *const c_char) -> Option<String> {
  if ptr.is_null() {
    None
  } else {
    let s = CStr::from_ptr(ptr).to_string_lossy().into_owned();
    if s.is_empty() {
      None
    } else {
      Some(s)
    }
  }
}
