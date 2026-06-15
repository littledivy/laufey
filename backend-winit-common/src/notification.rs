// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Cross-platform system notifications for winit-based backends.
//!
//! Wraps the `notify-rust` crate, which dispatches to:
//! - Linux: org.freedesktop.Notifications via DBus
//! - macOS: NSUserNotification (deprecated but functional)
//! - Windows: Toast notifications
//!
//! Event coverage is fire-and-forget: `on_event` only fires `SHOWN`
//! synchronously when the OS accepts the notification, and `CLOSED`
//! synthetically when the runtime calls `close_notification`. Click and
//! action events are not surfaced — surfacing them portably requires
//! per-platform watcher threads, which is out of scope for v1. Use the
//! CEF or WebView backends if you need full event support.

use std::collections::HashMap;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Mutex;

use crate::{
  value_dict_get, value_get_bool, value_get_string, value_is_bool,
  value_is_dict, value_is_string, LaufeyValue,
};

pub const LAUFEY_NOTIFICATION_SHOWN: c_int = 0;
pub const LAUFEY_NOTIFICATION_CLICKED: c_int = 1;
pub const LAUFEY_NOTIFICATION_CLOSED: c_int = 2;
pub const LAUFEY_NOTIFICATION_ACTION: c_int = 3;

pub type LaufeyNotificationEventFn =
  unsafe extern "C" fn(*mut c_void, u32, c_int, *const c_char);

struct NotifEntry {
  on_event: Option<LaufeyNotificationEventFn>,
  user_data: usize,
}

static NEXT_ID: AtomicU32 = AtomicU32::new(1);
static NOTIFS: Mutex<Option<HashMap<u32, NotifEntry>>> = Mutex::new(None);

fn map() -> std::sync::MutexGuard<'static, Option<HashMap<u32, NotifEntry>>> {
  let mut g = NOTIFS.lock().unwrap();
  if g.is_none() {
    *g = Some(HashMap::new());
  }
  g
}

fn fire_event(
  on_event: Option<LaufeyNotificationEventFn>,
  user_data: usize,
  id: u32,
  reason: c_int,
) {
  let Some(cb) = on_event else { return };
  unsafe { cb(user_data as *mut c_void, id, reason, std::ptr::null()) };
}

unsafe fn read_string(dict: *mut LaufeyValue, key: &str) -> Option<String> {
  let c_key = CString::new(key).ok()?;
  let v = value_dict_get(dict, c_key.as_ptr());
  if v.is_null() || !value_is_string(v) {
    return None;
  }
  let mut len: usize = 0;
  let p = value_get_string(v, &mut len);
  if p.is_null() {
    return None;
  }
  let s = CStr::from_ptr(p).to_string_lossy().into_owned();
  crate::value_free_string(p);
  Some(s)
}

unsafe fn read_bool(dict: *mut LaufeyValue, key: &str) -> Option<bool> {
  let c_key = CString::new(key).ok()?;
  let v = value_dict_get(dict, c_key.as_ptr());
  if v.is_null() || !value_is_bool(v) {
    return None;
  }
  Some(value_get_bool(v))
}

/// Implementation of the `show_notification` ABI entry point.
///
/// # Safety
/// `options` must be a valid `LaufeyValue*` produced by the runtime; this
/// function takes ownership and frees it via [`crate::value_free`].
pub unsafe fn show_notification(
  options: *mut LaufeyValue,
  on_event: Option<LaufeyNotificationEventFn>,
  user_data: *mut c_void,
) -> u32 {
  if options.is_null() || !value_is_dict(options) {
    crate::value_free(options);
    return 0;
  }

  let title = read_string(options, "title").unwrap_or_default();
  let body = read_string(options, "body").unwrap_or_default();
  let _tag = read_string(options, "tag");
  let _require_interaction =
    read_bool(options, "require_interaction").unwrap_or(false);
  crate::value_free(options);

  let id = NEXT_ID.fetch_add(1, Ordering::Relaxed);

  let mut notif = notify_rust::Notification::new();
  notif.summary(&title);
  if !body.is_empty() {
    notif.body(&body);
  }
  // Linux-only options that other targets ignore.
  #[cfg(target_os = "linux")]
  {
    if let Some(tag) = _tag.as_deref() {
      use std::hash::{Hash, Hasher};
      let mut h = std::collections::hash_map::DefaultHasher::new();
      tag.hash(&mut h);
      let nid = ((h.finish() as u32) | 1) & 0x7FFF_FFFF;
      notif.id(nid);
    }
    if _require_interaction {
      notif.timeout(notify_rust::Timeout::Never);
    }
  }

  if let Err(e) = notif.show() {
    eprintln!("laufey: failed to show notification: {}", e);
    return 0;
  }

  let entry = NotifEntry {
    on_event,
    user_data: user_data as usize,
  };
  map().as_mut().unwrap().insert(id, entry);

  // Fire SHOWN synchronously after the OS accepts the notification.
  fire_event(on_event, user_data as usize, id, LAUFEY_NOTIFICATION_SHOWN);
  id
}

/// Implementation of the `close_notification` ABI entry point.
///
/// notify-rust doesn't expose a portable programmatic-close on the
/// returned handle, so this drops our bookkeeping and fires `CLOSED`
/// for the runtime; the OS-side notification will dismiss on its own
/// (timeout or user action).
pub fn close_notification(notification_id: u32) {
  let entry = {
    let mut m = map();
    m.as_mut().and_then(|map| map.remove(&notification_id))
  };
  let Some(entry) = entry else { return };
  fire_event(
    entry.on_event,
    entry.user_data,
    notification_id,
    LAUFEY_NOTIFICATION_CLOSED,
  );
}
