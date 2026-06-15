// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

pub use winit;

pub mod dock;
pub mod notification;
pub mod permission;
pub mod tray;

use std::cell::RefCell;
use std::collections::HashMap;
use std::env;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::path::PathBuf;
use std::sync::atomic::{AtomicI32, AtomicPtr, AtomicU32, Ordering};
use std::sync::Mutex;
use std::thread;

use libloading::{Library, Symbol};
use muda::MenuEvent;
#[allow(unused_imports)]
use raw_window_handle::{
  HasDisplayHandle, HasWindowHandle, RawDisplayHandle, RawWindowHandle,
};
use winit::dpi::{LogicalPosition, LogicalSize};
use winit::event_loop::EventLoopProxy;
use winit::window::{Window, WindowLevel};

// --- Constants ---

pub const LAUFEY_API_VERSION: u32 = 25;

/// Creation-time window style flags (mirror `LAUFEY_WINDOW_FLAG_*` in laufey.h).
pub const LAUFEY_WINDOW_FLAG_FRAMELESS: u32 = 1 << 0;
pub const LAUFEY_WINDOW_FLAG_NO_ACTIVATE: u32 = 1 << 1;

#[allow(dead_code)]
pub const LAUFEY_WINDOW_HANDLE_UNKNOWN: c_int = 0;
#[allow(dead_code)]
pub const LAUFEY_WINDOW_HANDLE_APPKIT: c_int = 1;
#[allow(dead_code)]
pub const LAUFEY_WINDOW_HANDLE_WIN32: c_int = 2;
#[allow(dead_code)]
pub const LAUFEY_WINDOW_HANDLE_X11: c_int = 3;
#[allow(dead_code)]
pub const LAUFEY_WINDOW_HANDLE_WAYLAND: c_int = 4;

// --- FFI Types ---

#[repr(C)]
pub struct LaufeyValue {
  _opaque: [u8; 0],
}

pub type LaufeyJsCallFn = unsafe extern "C" fn(
  *mut c_void,      // user_data
  u32,              // window_id
  u64,              // call_id
  *const c_char,    // method_path
  *mut LaufeyValue, // args
);
pub type LaufeyJsResultFn =
  unsafe extern "C" fn(*mut LaufeyValue, *mut LaufeyValue, *mut c_void);
pub type LaufeyKeyboardEventFn = unsafe extern "C" fn(
  *mut c_void,   // user_data
  u32,           // window_id
  c_int,         // state (0=pressed, 1=released)
  *const c_char, // key
  *const c_char, // code
  u32,           // modifiers
  bool,          // repeat
);
pub type LaufeyMouseMoveFn = unsafe extern "C" fn(
  *mut c_void, // user_data
  u32,         // window_id
  f64,         // x
  f64,         // y
  u32,         // modifiers
);
pub type LaufeyMouseClickFn = unsafe extern "C" fn(
  *mut c_void, // user_data
  u32,         // window_id
  c_int,       // state (0=pressed, 1=released)
  c_int,       // button
  f64,         // x
  f64,         // y
  u32,         // modifiers
  i32,         // click_count
);
pub type LaufeyWheelFn = unsafe extern "C" fn(
  *mut c_void, // user_data
  u32,         // window_id
  f64,         // delta_x
  f64,         // delta_y
  f64,         // x
  f64,         // y
  u32,         // modifiers
  i32,         // delta_mode
);
pub type LaufeyCursorEnterLeaveFn = unsafe extern "C" fn(
  *mut c_void, // user_data
  u32,         // window_id
  c_int,       // entered (1=entered, 0=left)
  f64,         // x
  f64,         // y
  u32,         // modifiers
);
pub type LaufeyFocusedFn = unsafe extern "C" fn(
  *mut c_void, // user_data
  u32,         // window_id
  c_int,       // focused (1=gained, 0=lost)
);
pub type LaufeyResizeFn = unsafe extern "C" fn(
  *mut c_void, // user_data
  u32,         // window_id
  c_int,       // width
  c_int,       // height
);
pub type LaufeyMoveFn = unsafe extern "C" fn(
  *mut c_void, // user_data
  u32,         // window_id
  c_int,       // x
  c_int,       // y
);
pub type LaufeyCloseRequestedFn = unsafe extern "C" fn(
  *mut c_void, // user_data
  u32,         // window_id
);
pub const LAUFEY_DIALOG_ALERT: i32 = 0;
pub const LAUFEY_DIALOG_CONFIRM: i32 = 1;
pub const LAUFEY_DIALOG_PROMPT: i32 = 2;

pub const LAUFEY_WHEEL_DELTA_PIXEL: i32 = 0;
pub const LAUFEY_WHEEL_DELTA_LINE: i32 = 1;

#[repr(C)]
pub struct LaufeyBackendApi {
  pub version: u32,
  pub backend_data: *mut c_void,
  pub create_window: Option<unsafe extern "C" fn(*mut c_void) -> u32>,
  pub close_window: Option<unsafe extern "C" fn(*mut c_void, u32)>,
  // Must mirror laufey.h field order exactly: create_window_ex sits between
  // close_window and navigate.
  pub create_window_ex: Option<unsafe extern "C" fn(*mut c_void, u32) -> u32>,
  pub navigate: Option<unsafe extern "C" fn(*mut c_void, u32, *const c_char)>,
  pub set_title: Option<unsafe extern "C" fn(*mut c_void, u32, *const c_char)>,
  pub execute_js: Option<
    unsafe extern "C" fn(
      *mut c_void,
      u32,
      *const c_char,
      Option<LaufeyJsResultFn>,
      *mut c_void,
    ),
  >,
  pub quit: Option<unsafe extern "C" fn(*mut c_void)>,
  pub set_window_size:
    Option<unsafe extern "C" fn(*mut c_void, u32, c_int, c_int)>,
  pub get_window_size:
    Option<unsafe extern "C" fn(*mut c_void, u32, *mut c_int, *mut c_int)>,
  pub set_window_position:
    Option<unsafe extern "C" fn(*mut c_void, u32, c_int, c_int)>,
  pub get_window_position:
    Option<unsafe extern "C" fn(*mut c_void, u32, *mut c_int, *mut c_int)>,
  pub set_resizable: Option<unsafe extern "C" fn(*mut c_void, u32, bool)>,
  pub is_resizable: Option<unsafe extern "C" fn(*mut c_void, u32) -> bool>,
  pub set_always_on_top: Option<unsafe extern "C" fn(*mut c_void, u32, bool)>,
  pub is_always_on_top: Option<unsafe extern "C" fn(*mut c_void, u32) -> bool>,
  pub is_visible: Option<unsafe extern "C" fn(*mut c_void, u32) -> bool>,
  pub show: Option<unsafe extern "C" fn(*mut c_void, u32)>,
  pub hide: Option<unsafe extern "C" fn(*mut c_void, u32)>,
  pub focus: Option<unsafe extern "C" fn(*mut c_void, u32)>,
  pub post_ui_task: Option<
    unsafe extern "C" fn(
      *mut c_void,
      Option<unsafe extern "C" fn(*mut c_void)>,
      *mut c_void,
    ),
  >,
  pub value_is_null: Option<unsafe extern "C" fn(*mut LaufeyValue) -> bool>,
  pub value_is_bool: Option<unsafe extern "C" fn(*mut LaufeyValue) -> bool>,
  pub value_is_int: Option<unsafe extern "C" fn(*mut LaufeyValue) -> bool>,
  pub value_is_double: Option<unsafe extern "C" fn(*mut LaufeyValue) -> bool>,
  pub value_is_string: Option<unsafe extern "C" fn(*mut LaufeyValue) -> bool>,
  pub value_is_list: Option<unsafe extern "C" fn(*mut LaufeyValue) -> bool>,
  pub value_is_dict: Option<unsafe extern "C" fn(*mut LaufeyValue) -> bool>,
  pub value_is_binary: Option<unsafe extern "C" fn(*mut LaufeyValue) -> bool>,
  pub value_is_callback: Option<unsafe extern "C" fn(*mut LaufeyValue) -> bool>,
  pub value_get_bool: Option<unsafe extern "C" fn(*mut LaufeyValue) -> bool>,
  pub value_get_int: Option<unsafe extern "C" fn(*mut LaufeyValue) -> c_int>,
  pub value_get_double: Option<unsafe extern "C" fn(*mut LaufeyValue) -> f64>,
  pub value_get_string:
    Option<unsafe extern "C" fn(*mut LaufeyValue, *mut usize) -> *mut c_char>,
  pub value_free_string: Option<unsafe extern "C" fn(*mut c_char)>,
  pub value_list_size: Option<unsafe extern "C" fn(*mut LaufeyValue) -> usize>,
  pub value_list_get:
    Option<unsafe extern "C" fn(*mut LaufeyValue, usize) -> *mut LaufeyValue>,
  pub value_dict_get: Option<
    unsafe extern "C" fn(*mut LaufeyValue, *const c_char) -> *mut LaufeyValue,
  >,
  pub value_dict_has:
    Option<unsafe extern "C" fn(*mut LaufeyValue, *const c_char) -> bool>,
  pub value_dict_size: Option<unsafe extern "C" fn(*mut LaufeyValue) -> usize>,
  pub value_dict_keys: Option<
    unsafe extern "C" fn(*mut LaufeyValue, *mut usize) -> *mut *mut c_char,
  >,
  pub value_free_keys: Option<unsafe extern "C" fn(*mut *mut c_char, usize)>,
  pub value_get_binary:
    Option<unsafe extern "C" fn(*mut LaufeyValue, *mut usize) -> *const c_void>,
  pub value_get_callback_id:
    Option<unsafe extern "C" fn(*mut LaufeyValue) -> u64>,
  pub value_null: Option<unsafe extern "C" fn(*mut c_void) -> *mut LaufeyValue>,
  pub value_bool:
    Option<unsafe extern "C" fn(*mut c_void, bool) -> *mut LaufeyValue>,
  pub value_int:
    Option<unsafe extern "C" fn(*mut c_void, c_int) -> *mut LaufeyValue>,
  pub value_double:
    Option<unsafe extern "C" fn(*mut c_void, f64) -> *mut LaufeyValue>,
  pub value_string: Option<
    unsafe extern "C" fn(*mut c_void, *const c_char) -> *mut LaufeyValue,
  >,
  pub value_list: Option<unsafe extern "C" fn(*mut c_void) -> *mut LaufeyValue>,
  pub value_dict: Option<unsafe extern "C" fn(*mut c_void) -> *mut LaufeyValue>,
  pub value_binary: Option<
    unsafe extern "C" fn(*mut c_void, *const c_void, usize) -> *mut LaufeyValue,
  >,
  pub value_list_append:
    Option<unsafe extern "C" fn(*mut LaufeyValue, *mut LaufeyValue) -> bool>,
  pub value_list_set: Option<
    unsafe extern "C" fn(*mut LaufeyValue, usize, *mut LaufeyValue) -> bool,
  >,
  pub value_dict_set: Option<
    unsafe extern "C" fn(
      *mut LaufeyValue,
      *const c_char,
      *mut LaufeyValue,
    ) -> bool,
  >,
  pub value_free: Option<unsafe extern "C" fn(*mut LaufeyValue)>,
  pub set_js_call_handler:
    Option<unsafe extern "C" fn(*mut c_void, LaufeyJsCallFn, *mut c_void)>,
  pub js_call_respond: Option<
    unsafe extern "C" fn(*mut c_void, u64, *mut LaufeyValue, *mut LaufeyValue),
  >,
  pub invoke_js_callback:
    Option<unsafe extern "C" fn(*mut c_void, u64, *mut LaufeyValue)>,
  pub release_js_callback: Option<unsafe extern "C" fn(*mut c_void, u64)>,
  pub get_window_handle:
    Option<unsafe extern "C" fn(*mut c_void, u32) -> *mut c_void>,
  pub get_display_handle:
    Option<unsafe extern "C" fn(*mut c_void, u32) -> *mut c_void>,
  pub get_window_handle_type:
    Option<unsafe extern "C" fn(*mut c_void, u32) -> c_int>,
  pub set_keyboard_event_handler: Option<
    unsafe extern "C" fn(
      *mut c_void,
      Option<LaufeyKeyboardEventFn>,
      *mut c_void,
    ),
  >,
  pub set_mouse_click_handler: Option<
    unsafe extern "C" fn(*mut c_void, Option<LaufeyMouseClickFn>, *mut c_void),
  >,
  pub set_mouse_move_handler: Option<
    unsafe extern "C" fn(*mut c_void, Option<LaufeyMouseMoveFn>, *mut c_void),
  >,
  pub set_wheel_handler: Option<
    unsafe extern "C" fn(*mut c_void, Option<LaufeyWheelFn>, *mut c_void),
  >,
  pub set_cursor_enter_leave_handler: Option<
    unsafe extern "C" fn(
      *mut c_void,
      Option<LaufeyCursorEnterLeaveFn>,
      *mut c_void,
    ),
  >,
  pub set_focused_handler: Option<
    unsafe extern "C" fn(*mut c_void, Option<LaufeyFocusedFn>, *mut c_void),
  >,
  pub set_resize_handler: Option<
    unsafe extern "C" fn(*mut c_void, Option<LaufeyResizeFn>, *mut c_void),
  >,
  pub set_move_handler: Option<
    unsafe extern "C" fn(*mut c_void, Option<LaufeyMoveFn>, *mut c_void),
  >,
  pub set_close_requested_handler: Option<
    unsafe extern "C" fn(
      *mut c_void,
      Option<LaufeyCloseRequestedFn>,
      *mut c_void,
    ),
  >,
  pub poll_js_calls: Option<unsafe extern "C" fn(*mut c_void)>,
  pub set_js_call_notify: Option<
    unsafe extern "C" fn(
      *mut c_void,
      Option<unsafe extern "C" fn(*mut c_void)>,
      *mut c_void,
    ),
  >,
  pub set_application_menu: Option<
    unsafe extern "C" fn(
      *mut c_void,
      u32,
      *mut LaufeyValue,
      Option<unsafe extern "C" fn(*mut c_void, u32, *const c_char)>,
      *mut c_void,
    ),
  >,
  pub show_context_menu: Option<
    unsafe extern "C" fn(
      *mut c_void,
      u32,
      c_int,
      c_int,
      *mut LaufeyValue,
      Option<unsafe extern "C" fn(*mut c_void, u32, *const c_char)>,
      *mut c_void,
    ),
  >,
  pub open_devtools: Option<unsafe extern "C" fn(*mut c_void, u32)>,
  pub set_js_namespace:
    Option<unsafe extern "C" fn(*mut c_void, *const c_char)>,
  pub show_dialog: Option<
    unsafe extern "C" fn(
      *mut c_void,      // backend_data
      u32,              // window_id
      c_int,            // dialog_type
      *const c_char,    // title
      *const c_char,    // message
      *const c_char,    // default_value
      *mut *mut c_char, // out_input_value (prompt only; nullable)
    ) -> c_int,
  >,
  pub string_free: Option<unsafe extern "C" fn(*mut c_void, *mut c_char)>,
  pub set_dock_badge: Option<unsafe extern "C" fn(*mut c_void, *const c_char)>,
  pub bounce_dock: Option<unsafe extern "C" fn(*mut c_void, c_int)>,
  pub set_dock_menu: Option<
    unsafe extern "C" fn(
      *mut c_void,
      *mut LaufeyValue,
      Option<LaufeyMenuClickFn>,
      *mut c_void,
    ),
  >,
  pub set_dock_visible: Option<unsafe extern "C" fn(*mut c_void, bool)>,
  pub set_dock_reopen_handler: Option<
    unsafe extern "C" fn(
      *mut c_void,
      Option<dock::LaufeyDockReopenFn>,
      *mut c_void,
    ),
  >,
  pub create_tray_icon: Option<unsafe extern "C" fn(*mut c_void) -> u32>,
  pub destroy_tray_icon: Option<unsafe extern "C" fn(*mut c_void, u32)>,
  pub set_tray_icon:
    Option<unsafe extern "C" fn(*mut c_void, u32, *const c_void, usize)>,
  pub set_tray_tooltip:
    Option<unsafe extern "C" fn(*mut c_void, u32, *const c_char)>,
  pub set_tray_menu: Option<
    unsafe extern "C" fn(
      *mut c_void,
      u32,
      *mut LaufeyValue,
      Option<LaufeyMenuClickFn>,
      *mut c_void,
    ),
  >,
  pub set_tray_click_handler: Option<
    unsafe extern "C" fn(
      *mut c_void,
      u32,
      Option<tray::LaufeyTrayClickFn>,
      *mut c_void,
    ),
  >,
  pub set_tray_double_click_handler: Option<
    unsafe extern "C" fn(
      *mut c_void,
      u32,
      Option<tray::LaufeyTrayClickFn>,
      *mut c_void,
    ),
  >,
  pub set_tray_icon_dark:
    Option<unsafe extern "C" fn(*mut c_void, u32, *const c_void, usize)>,
  // Mirrors laufey.h: get_tray_icon_bounds sits between set_tray_icon_dark and
  // show_notification.
  pub get_tray_icon_bounds: Option<
    unsafe extern "C" fn(
      *mut c_void,
      u32,
      *mut c_int,
      *mut c_int,
      *mut c_int,
      *mut c_int,
    ) -> bool,
  >,
  pub show_notification: Option<
    unsafe extern "C" fn(
      *mut c_void,
      *mut LaufeyValue,
      Option<notification::LaufeyNotificationEventFn>,
      *mut c_void,
    ) -> u32,
  >,
  pub close_notification: Option<unsafe extern "C" fn(*mut c_void, u32)>,
  pub query_permission: Option<
    unsafe extern "C" fn(
      *mut c_void,
      c_int,
      Option<permission::LaufeyPermissionCallbackFn>,
      *mut c_void,
    ),
  >,
  pub request_permission: Option<
    unsafe extern "C" fn(
      *mut c_void,
      c_int,
      Option<permission::LaufeyPermissionCallbackFn>,
      *mut c_void,
    ),
  >,
}

unsafe impl Send for LaufeyBackendApi {}

pub type RuntimeInitFn = unsafe extern "C" fn(*const LaufeyBackendApi) -> c_int;
pub type RuntimeStartFn = unsafe extern "C" fn() -> c_int;
#[allow(dead_code)]
pub type RuntimeShutdownFn = unsafe extern "C" fn();

// --- SimpleValue: real LaufeyValue backing type ---

enum SimpleValue {
  Null,
  Bool(bool),
  Int(c_int),
  Double(f64),
  String(String),
  List(Vec<*mut LaufeyValue>),
  Dict(Vec<(String, *mut LaufeyValue)>),
  Binary(Vec<u8>),
}

fn sv_to_laufey(val: SimpleValue) -> *mut LaufeyValue {
  Box::into_raw(Box::new(val)) as *mut LaufeyValue
}

unsafe fn laufey_ref(ptr: *mut LaufeyValue) -> &'static SimpleValue {
  &*(ptr as *const SimpleValue)
}

unsafe fn laufey_mut(ptr: *mut LaufeyValue) -> &'static mut SimpleValue {
  &mut *(ptr as *mut SimpleValue)
}

unsafe fn laufey_free(ptr: *mut LaufeyValue) {
  if ptr.is_null() {
    return;
  }
  let val = Box::from_raw(ptr as *mut SimpleValue);
  match *val {
    SimpleValue::List(items) => {
      for item in items {
        laufey_free(item);
      }
    }
    SimpleValue::Dict(entries) => {
      for (_, v) in entries {
        laufey_free(v);
      }
    }
    _ => {}
  }
}

// --- Value functions ---

/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_is_null(val: *mut LaufeyValue) -> bool {
  if val.is_null() {
    return true;
  }
  matches!(laufey_ref(val), SimpleValue::Null)
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_is_bool(val: *mut LaufeyValue) -> bool {
  if val.is_null() {
    return false;
  }
  matches!(laufey_ref(val), SimpleValue::Bool(_))
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_is_int(val: *mut LaufeyValue) -> bool {
  if val.is_null() {
    return false;
  }
  matches!(laufey_ref(val), SimpleValue::Int(_))
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_is_double(val: *mut LaufeyValue) -> bool {
  if val.is_null() {
    return false;
  }
  matches!(laufey_ref(val), SimpleValue::Double(_))
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_is_string(val: *mut LaufeyValue) -> bool {
  if val.is_null() {
    return false;
  }
  matches!(laufey_ref(val), SimpleValue::String(_))
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_is_list(val: *mut LaufeyValue) -> bool {
  if val.is_null() {
    return false;
  }
  matches!(laufey_ref(val), SimpleValue::List(_))
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_is_dict(val: *mut LaufeyValue) -> bool {
  if val.is_null() {
    return false;
  }
  matches!(laufey_ref(val), SimpleValue::Dict(_))
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_is_binary(val: *mut LaufeyValue) -> bool {
  if val.is_null() {
    return false;
  }
  matches!(laufey_ref(val), SimpleValue::Binary(_))
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_is_callback(_val: *mut LaufeyValue) -> bool {
  false // no callback support in winit backend
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_get_bool(val: *mut LaufeyValue) -> bool {
  if val.is_null() {
    return false;
  }
  match laufey_ref(val) {
    SimpleValue::Bool(v) => *v,
    _ => false,
  }
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_get_int(val: *mut LaufeyValue) -> c_int {
  if val.is_null() {
    return 0;
  }
  match laufey_ref(val) {
    SimpleValue::Int(v) => *v,
    _ => 0,
  }
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_get_double(val: *mut LaufeyValue) -> f64 {
  if val.is_null() {
    return 0.0;
  }
  match laufey_ref(val) {
    SimpleValue::Double(v) => *v,
    _ => 0.0,
  }
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_get_string(
  val: *mut LaufeyValue,
  len: *mut usize,
) -> *mut c_char {
  if val.is_null() {
    if !len.is_null() {
      *len = 0;
    }
    return std::ptr::null_mut();
  }
  match laufey_ref(val) {
    SimpleValue::String(s) => {
      if !len.is_null() {
        *len = s.len();
      }
      CString::new(s.as_str()).unwrap_or_default().into_raw()
    }
    _ => {
      if !len.is_null() {
        *len = 0;
      }
      std::ptr::null_mut()
    }
  }
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_free_string(s: *mut c_char) {
  if !s.is_null() {
    let _ = CString::from_raw(s);
  }
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_list_size(val: *mut LaufeyValue) -> usize {
  if val.is_null() {
    return 0;
  }
  match laufey_ref(val) {
    SimpleValue::List(items) => items.len(),
    _ => 0,
  }
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_list_get(
  val: *mut LaufeyValue,
  idx: usize,
) -> *mut LaufeyValue {
  if val.is_null() {
    return std::ptr::null_mut();
  }
  match laufey_ref(val) {
    SimpleValue::List(items) => {
      items.get(idx).copied().unwrap_or(std::ptr::null_mut())
    }
    _ => std::ptr::null_mut(),
  }
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_dict_get(
  val: *mut LaufeyValue,
  key: *const c_char,
) -> *mut LaufeyValue {
  if val.is_null() || key.is_null() {
    return std::ptr::null_mut();
  }
  let key_str = CStr::from_ptr(key).to_string_lossy();
  match laufey_ref(val) {
    SimpleValue::Dict(entries) => entries
      .iter()
      .find(|(k, _)| k == key_str.as_ref())
      .map(|(_, v)| *v)
      .unwrap_or(std::ptr::null_mut()),
    _ => std::ptr::null_mut(),
  }
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_dict_has(
  val: *mut LaufeyValue,
  key: *const c_char,
) -> bool {
  if val.is_null() || key.is_null() {
    return false;
  }
  let key_str = CStr::from_ptr(key).to_string_lossy();
  match laufey_ref(val) {
    SimpleValue::Dict(entries) => {
      entries.iter().any(|(k, _)| k == key_str.as_ref())
    }
    _ => false,
  }
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_dict_size(val: *mut LaufeyValue) -> usize {
  if val.is_null() {
    return 0;
  }
  match laufey_ref(val) {
    SimpleValue::Dict(entries) => entries.len(),
    _ => 0,
  }
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_dict_keys(
  val: *mut LaufeyValue,
  count: *mut usize,
) -> *mut *mut c_char {
  if val.is_null() {
    if !count.is_null() {
      *count = 0;
    }
    return std::ptr::null_mut();
  }
  match laufey_ref(val) {
    SimpleValue::Dict(entries) => {
      let n = entries.len();
      if !count.is_null() {
        *count = n;
      }
      if n == 0 {
        return std::ptr::null_mut();
      }
      let layout = std::alloc::Layout::array::<*mut c_char>(n).unwrap();
      let ptr = std::alloc::alloc(layout) as *mut *mut c_char;
      for (i, (k, _)) in entries.iter().enumerate() {
        *ptr.add(i) = CString::new(k.as_str()).unwrap_or_default().into_raw();
      }
      ptr
    }
    _ => {
      if !count.is_null() {
        *count = 0;
      }
      std::ptr::null_mut()
    }
  }
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_free_keys(keys: *mut *mut c_char, count: usize) {
  if keys.is_null() {
    return;
  }
  for i in 0..count {
    let k = *keys.add(i);
    if !k.is_null() {
      let _ = CString::from_raw(k);
    }
  }
  let layout = std::alloc::Layout::array::<*mut c_char>(count).unwrap();
  std::alloc::dealloc(keys as *mut u8, layout);
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_get_binary(
  val: *mut LaufeyValue,
  len: *mut usize,
) -> *const c_void {
  if val.is_null() {
    if !len.is_null() {
      *len = 0;
    }
    return std::ptr::null();
  }
  match laufey_ref(val) {
    SimpleValue::Binary(data) => {
      if !len.is_null() {
        *len = data.len();
      }
      data.as_ptr() as *const c_void
    }
    _ => {
      if !len.is_null() {
        *len = 0;
      }
      std::ptr::null()
    }
  }
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_get_callback_id(_val: *mut LaufeyValue) -> u64 {
  0
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_null(_data: *mut c_void) -> *mut LaufeyValue {
  sv_to_laufey(SimpleValue::Null)
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_bool(
  _data: *mut c_void,
  v: bool,
) -> *mut LaufeyValue {
  sv_to_laufey(SimpleValue::Bool(v))
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_int(
  _data: *mut c_void,
  v: c_int,
) -> *mut LaufeyValue {
  sv_to_laufey(SimpleValue::Int(v))
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_double(
  _data: *mut c_void,
  v: f64,
) -> *mut LaufeyValue {
  sv_to_laufey(SimpleValue::Double(v))
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_string(
  _data: *mut c_void,
  v: *const c_char,
) -> *mut LaufeyValue {
  if v.is_null() {
    return sv_to_laufey(SimpleValue::String(String::new()));
  }
  let s = CStr::from_ptr(v).to_string_lossy().into_owned();
  sv_to_laufey(SimpleValue::String(s))
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_list(_data: *mut c_void) -> *mut LaufeyValue {
  sv_to_laufey(SimpleValue::List(Vec::new()))
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_dict(_data: *mut c_void) -> *mut LaufeyValue {
  sv_to_laufey(SimpleValue::Dict(Vec::new()))
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_binary(
  _data: *mut c_void,
  d: *const c_void,
  len: usize,
) -> *mut LaufeyValue {
  if d.is_null() || len == 0 {
    return sv_to_laufey(SimpleValue::Binary(Vec::new()));
  }
  let data = std::slice::from_raw_parts(d as *const u8, len).to_vec();
  sv_to_laufey(SimpleValue::Binary(data))
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_list_append(
  list: *mut LaufeyValue,
  val: *mut LaufeyValue,
) -> bool {
  if list.is_null() || val.is_null() {
    return false;
  }
  match laufey_mut(list) {
    SimpleValue::List(items) => {
      items.push(val);
      true
    }
    _ => false,
  }
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_list_set(
  list: *mut LaufeyValue,
  idx: usize,
  val: *mut LaufeyValue,
) -> bool {
  if list.is_null() || val.is_null() {
    return false;
  }
  match laufey_mut(list) {
    SimpleValue::List(items) if idx < items.len() => {
      laufey_free(items[idx]);
      items[idx] = val;
      true
    }
    _ => false,
  }
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_dict_set(
  dict: *mut LaufeyValue,
  key: *const c_char,
  val: *mut LaufeyValue,
) -> bool {
  if dict.is_null() || key.is_null() || val.is_null() {
    return false;
  }
  let key_str = CStr::from_ptr(key).to_string_lossy().into_owned();
  match laufey_mut(dict) {
    SimpleValue::Dict(entries) => {
      // Replace existing key if present
      for entry in entries.iter_mut() {
        if entry.0 == key_str {
          laufey_free(entry.1);
          entry.1 = val;
          return true;
        }
      }
      entries.push((key_str, val));
      true
    }
    _ => false,
  }
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn value_free(val: *mut LaufeyValue) {
  laufey_free(val);
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn set_js_call_handler(
  _data: *mut c_void,
  _handler: LaufeyJsCallFn,
  _user_data: *mut c_void,
) {
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn js_call_respond(
  _data: *mut c_void,
  _call_id: u64,
  result: *mut LaufeyValue,
  error: *mut LaufeyValue,
) {
  laufey_free(result);
  laufey_free(error);
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn invoke_js_callback(
  _data: *mut c_void,
  _cb_id: u64,
  args: *mut LaufeyValue,
) {
  laufey_free(args);
}
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn release_js_callback(_data: *mut c_void, _cb_id: u64) {}

/// Fill the value/JS stub fields of a LaufeyBackendApi. Backend-specific fields
/// (navigate, execute_js, quit, window management, handles) are left as None
/// and must be set by the caller.
pub fn create_api_base() -> LaufeyBackendApi {
  LaufeyBackendApi {
    version: LAUFEY_API_VERSION,
    backend_data: std::ptr::null_mut(),
    create_window: None,
    close_window: None,
    create_window_ex: None,
    navigate: None,
    set_title: None,
    execute_js: None,
    quit: None,
    set_window_size: None,
    get_window_size: None,
    set_window_position: None,
    get_window_position: None,
    set_resizable: None,
    is_resizable: None,
    set_always_on_top: None,
    is_always_on_top: None,
    is_visible: None,
    show: None,
    hide: None,
    focus: None,
    post_ui_task: None,
    value_is_null: Some(value_is_null),
    value_is_bool: Some(value_is_bool),
    value_is_int: Some(value_is_int),
    value_is_double: Some(value_is_double),
    value_is_string: Some(value_is_string),
    value_is_list: Some(value_is_list),
    value_is_dict: Some(value_is_dict),
    value_is_binary: Some(value_is_binary),
    value_is_callback: Some(value_is_callback),
    value_get_bool: Some(value_get_bool),
    value_get_int: Some(value_get_int),
    value_get_double: Some(value_get_double),
    value_get_string: Some(value_get_string),
    value_free_string: Some(value_free_string),
    value_list_size: Some(value_list_size),
    value_list_get: Some(value_list_get),
    value_dict_get: Some(value_dict_get),
    value_dict_has: Some(value_dict_has),
    value_dict_size: Some(value_dict_size),
    value_dict_keys: Some(value_dict_keys),
    value_free_keys: Some(value_free_keys),
    value_get_binary: Some(value_get_binary),
    value_get_callback_id: Some(value_get_callback_id),
    value_null: Some(value_null),
    value_bool: Some(value_bool),
    value_int: Some(value_int),
    value_double: Some(value_double),
    value_string: Some(value_string),
    value_list: Some(value_list),
    value_dict: Some(value_dict),
    value_binary: Some(value_binary),
    value_list_append: Some(value_list_append),
    value_list_set: Some(value_list_set),
    value_dict_set: Some(value_dict_set),
    value_free: Some(value_free),
    set_js_call_handler: Some(set_js_call_handler),
    js_call_respond: Some(js_call_respond),
    invoke_js_callback: Some(invoke_js_callback),
    release_js_callback: Some(release_js_callback),
    get_window_handle: None,
    get_display_handle: None,
    get_window_handle_type: None,
    set_keyboard_event_handler: None,
    set_mouse_click_handler: None,
    set_mouse_move_handler: None,
    set_wheel_handler: None,
    set_cursor_enter_leave_handler: None,
    set_focused_handler: None,
    set_resize_handler: None,
    set_move_handler: None,
    set_close_requested_handler: None,
    poll_js_calls: None,
    set_js_call_notify: None,
    set_application_menu: None,
    show_context_menu: None,
    open_devtools: None,
    set_js_namespace: None,
    show_dialog: None,
    string_free: None,
    set_dock_badge: None,
    bounce_dock: None,
    set_dock_menu: None,
    set_dock_visible: None,
    set_dock_reopen_handler: None,
    create_tray_icon: None,
    destroy_tray_icon: None,
    set_tray_icon: None,
    set_tray_tooltip: None,
    set_tray_menu: None,
    set_tray_click_handler: None,
    set_tray_double_click_handler: None,
    set_tray_icon_dark: None,
    get_tray_icon_bounds: None,
    show_notification: None,
    close_notification: None,
    query_permission: None,
    request_permission: None,
  }
}

// --- Window ID counter ---

static NEXT_WINDOW_ID: AtomicU32 = AtomicU32::new(1);

pub fn allocate_window_id() -> u32 {
  NEXT_WINDOW_ID.fetch_add(1, Ordering::Relaxed)
}

// --- Per-window handle storage ---

static WINDOW_HANDLES: Mutex<Option<HashMap<u32, WindowHandleInfo>>> =
  Mutex::new(None);
static DISPLAY_HANDLE: AtomicPtr<c_void> = AtomicPtr::new(std::ptr::null_mut());
static WINDOW_HANDLE_TYPE: AtomicI32 = AtomicI32::new(0);

struct WindowHandleInfo {
  handle: *mut c_void,
}

unsafe impl Send for WindowHandleInfo {}

/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn backend_get_window_handle(
  _data: *mut c_void,
  window_id: u32,
) -> *mut c_void {
  let map = WINDOW_HANDLES.lock().unwrap();
  map
    .as_ref()
    .and_then(|m| m.get(&window_id))
    .map(|info| info.handle)
    .unwrap_or(std::ptr::null_mut())
}

/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn backend_get_display_handle(
  _data: *mut c_void,
  _window_id: u32,
) -> *mut c_void {
  DISPLAY_HANDLE.load(Ordering::Acquire)
}

/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe extern "C" fn backend_get_window_handle_type(
  _data: *mut c_void,
  _window_id: u32,
) -> c_int {
  WINDOW_HANDLE_TYPE.load(Ordering::Acquire)
}

pub fn store_window_handles(window_id: u32, window: &Window) {
  let mut handle_ptr = std::ptr::null_mut();

  if let Ok(wh) = window.window_handle() {
    match wh.as_raw() {
      #[cfg(target_os = "macos")]
      RawWindowHandle::AppKit(handle) => {
        handle_ptr = handle.ns_view.as_ptr();
        WINDOW_HANDLE_TYPE
          .store(LAUFEY_WINDOW_HANDLE_APPKIT, Ordering::Release);
      }
      #[cfg(target_os = "windows")]
      RawWindowHandle::Win32(handle) => {
        handle_ptr = handle.hwnd.get() as *mut c_void;
        WINDOW_HANDLE_TYPE.store(LAUFEY_WINDOW_HANDLE_WIN32, Ordering::Release);
      }
      #[cfg(target_os = "linux")]
      RawWindowHandle::Xlib(handle) => {
        handle_ptr = handle.window as *mut c_void;
        WINDOW_HANDLE_TYPE.store(LAUFEY_WINDOW_HANDLE_X11, Ordering::Release);
      }
      #[cfg(target_os = "linux")]
      RawWindowHandle::Wayland(handle) => {
        handle_ptr = handle.surface.as_ptr();
        WINDOW_HANDLE_TYPE
          .store(LAUFEY_WINDOW_HANDLE_WAYLAND, Ordering::Release);
      }
      _ => {}
    }
  }

  {
    let mut map = WINDOW_HANDLES.lock().unwrap();
    let map = map.get_or_insert_with(HashMap::new);
    map.insert(window_id, WindowHandleInfo { handle: handle_ptr });
  }

  if let Ok(dh) = window.display_handle() {
    match dh.as_raw() {
      #[cfg(target_os = "linux")]
      RawDisplayHandle::Xlib(handle) => {
        if let Some(display) = handle.display {
          DISPLAY_HANDLE.store(display.as_ptr(), Ordering::Release);
        }
      }
      #[cfg(target_os = "linux")]
      RawDisplayHandle::Wayland(handle) => {
        DISPLAY_HANDLE.store(handle.display.as_ptr(), Ordering::Release);
      }
      _ => {}
    }
  }
}

pub fn remove_window_handles(window_id: u32) {
  let mut map = WINDOW_HANDLES.lock().unwrap();
  if let Some(m) = map.as_mut() {
    m.remove(&window_id);
  }
}

// --- Menu types ---

pub type LaufeyMenuClickFn =
  unsafe extern "C" fn(*mut c_void, u32, *const c_char);

pub enum ParsedMenuItem {
  Item {
    id: String,
    label: String,
    accelerator: Option<String>,
    enabled: bool,
  },
  Submenu {
    label: String,
    items: Vec<ParsedMenuItem>,
  },
  Separator,
  Role {
    role: String,
  },
}

pub struct PendingMenu {
  pub items: Vec<ParsedMenuItem>,
  pub callback: Option<LaufeyMenuClickFn>,
  pub callback_data: usize,
}

pub struct PendingContextMenu {
  pub x: i32,
  pub y: i32,
  pub items: Vec<ParsedMenuItem>,
  pub callback: Option<LaufeyMenuClickFn>,
  pub callback_data: usize,
}

/// Helper to read a string from a LaufeyValue dict entry.
unsafe fn laufey_dict_string(
  dict: *mut LaufeyValue,
  key: &str,
) -> Option<String> {
  let c_key = CString::new(key).ok()?;
  let val = value_dict_get(dict, c_key.as_ptr());
  if val.is_null() || !value_is_string(val) {
    return None;
  }
  let mut len: usize = 0;
  let ptr = value_get_string(val, &mut len);
  if ptr.is_null() {
    return None;
  }
  let s = CStr::from_ptr(ptr).to_string_lossy().into_owned();
  value_free_string(ptr);
  Some(s)
}

/// Parse a LaufeyValue list into a vec of ParsedMenuItem.
/// # Safety
/// Caller must pass valid pointers as defined by the LAUFEY C API contract.
pub unsafe fn parse_menu_template(
  template: *mut LaufeyValue,
) -> Vec<ParsedMenuItem> {
  let mut items = Vec::new();
  if template.is_null() || !value_is_list(template) {
    return items;
  }
  let count = value_list_size(template);
  for i in 0..count {
    let entry = value_list_get(template, i);
    if entry.is_null() || !value_is_dict(entry) {
      continue;
    }
    // Check for separator
    if let Some(ty) = laufey_dict_string(entry, "type") {
      if ty == "separator" {
        items.push(ParsedMenuItem::Separator);
        continue;
      }
    }
    // Check for role
    if let Some(role) = laufey_dict_string(entry, "role") {
      items.push(ParsedMenuItem::Role { role });
      continue;
    }
    // Check for submenu
    let c_submenu = CString::new("submenu").unwrap();
    let submenu_val = value_dict_get(entry, c_submenu.as_ptr());
    if !submenu_val.is_null() && value_is_list(submenu_val) {
      let label = laufey_dict_string(entry, "label").unwrap_or_default();
      let children = parse_menu_template(submenu_val);
      items.push(ParsedMenuItem::Submenu {
        label,
        items: children,
      });
      continue;
    }
    // Regular item
    let label = laufey_dict_string(entry, "label").unwrap_or_default();
    let id = laufey_dict_string(entry, "id").unwrap_or_else(|| label.clone());
    let accelerator = laufey_dict_string(entry, "accelerator");
    let c_enabled = CString::new("enabled").unwrap();
    let enabled_val = value_dict_get(entry, c_enabled.as_ptr());
    let enabled = if enabled_val.is_null() || !value_is_bool(enabled_val) {
      true
    } else {
      value_get_bool(enabled_val)
    };
    items.push(ParsedMenuItem::Item {
      id,
      label,
      accelerator,
      enabled,
    });
  }
  items
}

// --- Menu callback storage ---

struct MenuCallbackInfo {
  callback: LaufeyMenuClickFn,
  callback_data: usize,
  window_id: u32,
}

unsafe impl Send for MenuCallbackInfo {}

static MENU_CLICK_STORE: Mutex<Option<HashMap<String, MenuCallbackInfo>>> =
  Mutex::new(None);

thread_local! {
  static ACTIVE_APP_MENUS: RefCell<HashMap<u32, muda::Menu>> =
    RefCell::new(HashMap::new());
}

pub fn register_menu_callbacks(
  items: &[ParsedMenuItem],
  callback: Option<LaufeyMenuClickFn>,
  callback_data: usize,
  window_id: u32,
) {
  let cb = match callback {
    Some(cb) => cb,
    None => return,
  };
  let mut store = MENU_CLICK_STORE.lock().unwrap();
  let store = store.get_or_insert_with(HashMap::new);
  for item in items {
    match item {
      ParsedMenuItem::Item { id, .. } => {
        store.insert(
          id.clone(),
          MenuCallbackInfo {
            callback: cb,
            callback_data,
            window_id,
          },
        );
      }
      ParsedMenuItem::Submenu {
        items: children, ..
      } => {
        register_menu_callbacks(children, callback, callback_data, window_id);
      }
      _ => {}
    }
  }
}

/// Build a muda Menu bar from parsed items.
fn build_muda_menu(items: &[ParsedMenuItem]) -> muda::Menu {
  let menu = muda::Menu::new();
  for item in items {
    append_muda_item_to_menu(&menu, item);
  }
  menu
}

fn append_muda_item_to_menu(menu: &muda::Menu, item: &ParsedMenuItem) {
  match item {
    ParsedMenuItem::Submenu { label, items } => {
      let submenu = muda::Submenu::new(label, true);
      for child in items {
        append_muda_item_to_submenu(&submenu, child);
      }
      let _ = menu.append(&submenu);
    }
    ParsedMenuItem::Item {
      id,
      label,
      accelerator,
      enabled,
    } => {
      let accel: Option<muda::accelerator::Accelerator> =
        accelerator.as_deref().and_then(|s| s.parse().ok());
      let item = muda::MenuItem::with_id(id.as_str(), label, *enabled, accel);
      let _ = menu.append(&item);
    }
    ParsedMenuItem::Separator => {
      let _ = menu.append(&muda::PredefinedMenuItem::separator());
    }
    ParsedMenuItem::Role { role } => {
      if let Some(item) = predefined_menu_item(role) {
        let _ = menu.append(&item);
      }
    }
  }
}

fn append_muda_item_to_submenu(submenu: &muda::Submenu, item: &ParsedMenuItem) {
  match item {
    ParsedMenuItem::Submenu {
      label,
      items: children,
    } => {
      let child_submenu = muda::Submenu::new(label, true);
      for child in children {
        append_muda_item_to_submenu(&child_submenu, child);
      }
      let _ = submenu.append(&child_submenu);
    }
    ParsedMenuItem::Item {
      id,
      label,
      accelerator,
      enabled,
    } => {
      let accel: Option<muda::accelerator::Accelerator> =
        accelerator.as_deref().and_then(|s| s.parse().ok());
      let item = muda::MenuItem::with_id(id.as_str(), label, *enabled, accel);
      let _ = submenu.append(&item);
    }
    ParsedMenuItem::Separator => {
      let _ = submenu.append(&muda::PredefinedMenuItem::separator());
    }
    ParsedMenuItem::Role { role } => {
      if let Some(item) = predefined_menu_item(role) {
        let _ = submenu.append(&item);
      }
    }
  }
}

fn predefined_menu_item(role: &str) -> Option<muda::PredefinedMenuItem> {
  match role.to_lowercase().as_str() {
    "quit" => Some(muda::PredefinedMenuItem::quit(None)),
    "copy" => Some(muda::PredefinedMenuItem::copy(None)),
    "cut" => Some(muda::PredefinedMenuItem::cut(None)),
    "paste" => Some(muda::PredefinedMenuItem::paste(None)),
    "selectall" | "selectAll" => {
      Some(muda::PredefinedMenuItem::select_all(None))
    }
    "undo" => Some(muda::PredefinedMenuItem::undo(None)),
    "redo" => Some(muda::PredefinedMenuItem::redo(None)),
    "minimize" => Some(muda::PredefinedMenuItem::minimize(None)),
    "close" => Some(muda::PredefinedMenuItem::close_window(None)),
    #[cfg(target_os = "macos")]
    "hide" => Some(muda::PredefinedMenuItem::hide(None)),
    #[cfg(target_os = "macos")]
    "hideothers" | "hideOthers" => {
      Some(muda::PredefinedMenuItem::hide_others(None))
    }
    #[cfg(target_os = "macos")]
    "unhide" | "showall" | "showAll" => {
      Some(muda::PredefinedMenuItem::show_all(None))
    }
    #[cfg(target_os = "macos")]
    "about" => Some(muda::PredefinedMenuItem::about(None, None)),
    "togglefullscreen" | "toggleFullScreen" => {
      Some(muda::PredefinedMenuItem::fullscreen(None))
    }
    "separator" => Some(muda::PredefinedMenuItem::separator()),
    _ => None,
  }
}

/// Build a muda Submenu suitable for use as a context menu.
fn build_muda_context_menu(items: &[ParsedMenuItem]) -> muda::Menu {
  let menu = muda::Menu::new();
  for item in items {
    match item {
      ParsedMenuItem::Submenu { label, items } => {
        let submenu = muda::Submenu::new(label, true);
        for child in items {
          append_muda_item_to_submenu(&submenu, child);
        }
        let _ = menu.append(&submenu);
      }
      ParsedMenuItem::Item {
        id,
        label,
        accelerator,
        enabled,
      } => {
        let accel: Option<muda::accelerator::Accelerator> =
          accelerator.as_deref().and_then(|s| s.parse().ok());
        let item = muda::MenuItem::with_id(id.as_str(), label, *enabled, accel);
        let _ = menu.append(&item);
      }
      ParsedMenuItem::Separator => {
        let _ = menu.append(&muda::PredefinedMenuItem::separator());
      }
      ParsedMenuItem::Role { role } => {
        if let Some(item) = predefined_menu_item(role) {
          let _ = menu.append(&item);
        }
      }
    }
  }
  menu
}

/// Poll muda menu events and dispatch callbacks. Call from the event loop.
pub fn poll_menu_events() {
  while let Ok(event) = MenuEvent::receiver().try_recv() {
    let id_str = event.id().0.clone();
    let store = MENU_CLICK_STORE.lock().unwrap();
    if let Some(store) = store.as_ref() {
      if let Some(info) = store.get(&id_str) {
        if let Ok(c_id) = CString::new(id_str.as_str()) {
          unsafe {
            (info.callback)(
              info.callback_data as *mut c_void,
              info.window_id,
              c_id.as_ptr(),
            );
          }
        }
      }
    }
  }
}

// --- Per-window common state ---

pub struct WindowState {
  pub pending_title: Mutex<Option<String>>,
  pub pending_size: Mutex<Option<(i32, i32)>>,
  pub pending_position: Mutex<Option<(i32, i32)>>,
  pub pending_resizable: Mutex<Option<bool>>,
  pub pending_always_on_top: Mutex<Option<bool>>,
  pub pending_visible: Mutex<Option<bool>>,
  /// Creation-time style flags (LAUFEY_WINDOW_FLAG_*) for frameless /
  /// non-activating panel windows; applied when the winit Window is built.
  pub pending_flags: Mutex<u32>,
  pub pending_app_menu: Mutex<Option<PendingMenu>>,
  pub pending_context_menu: Mutex<Option<PendingContextMenu>>,
  pub cursor_position: Mutex<(f64, f64)>,
  pub last_press_time: Mutex<Option<std::time::Instant>>,
  pub last_press_button: Mutex<Option<winit::event::MouseButton>>,
  pub click_count: Mutex<i32>,
}

impl WindowState {
  pub fn new() -> Self {
    Self {
      pending_title: Mutex::new(None),
      pending_size: Mutex::new(None),
      pending_position: Mutex::new(None),
      pending_resizable: Mutex::new(None),
      pending_always_on_top: Mutex::new(None),
      pending_visible: Mutex::new(None),
      pending_flags: Mutex::new(0),
      pending_app_menu: Mutex::new(None),
      pending_context_menu: Mutex::new(None),
      cursor_position: Mutex::new((0.0, 0.0)),
      last_press_time: Mutex::new(None),
      last_press_button: Mutex::new(None),
      click_count: Mutex::new(0),
    }
  }
}

impl Default for WindowState {
  fn default() -> Self {
    Self::new()
  }
}

// --- Global event handler state (registered once, dispatches for all windows) ---

pub struct EventHandlers {
  pub keyboard_handler: Mutex<Option<(LaufeyKeyboardEventFn, usize)>>,
  pub mouse_click_handler: Mutex<Option<(LaufeyMouseClickFn, usize)>>,
  pub mouse_move_handler: Mutex<Option<(LaufeyMouseMoveFn, usize)>>,
  pub wheel_handler: Mutex<Option<(LaufeyWheelFn, usize)>>,
  pub cursor_enter_leave_handler:
    Mutex<Option<(LaufeyCursorEnterLeaveFn, usize)>>,
  pub focused_handler: Mutex<Option<(LaufeyFocusedFn, usize)>>,
  pub resize_handler: Mutex<Option<(LaufeyResizeFn, usize)>>,
  pub move_handler: Mutex<Option<(LaufeyMoveFn, usize)>>,
  pub close_requested_handler: Mutex<Option<(LaufeyCloseRequestedFn, usize)>>,
}

impl EventHandlers {
  pub fn new() -> Self {
    Self {
      keyboard_handler: Mutex::new(None),
      mouse_click_handler: Mutex::new(None),
      mouse_move_handler: Mutex::new(None),
      wheel_handler: Mutex::new(None),
      cursor_enter_leave_handler: Mutex::new(None),
      focused_handler: Mutex::new(None),
      resize_handler: Mutex::new(None),
      move_handler: Mutex::new(None),
      close_requested_handler: Mutex::new(None),
    }
  }
}

impl Default for EventHandlers {
  fn default() -> Self {
    Self::new()
  }
}

// --- Common backend state holding per-window state + global handlers ---

pub struct CommonState {
  pub windows: Mutex<HashMap<u32, WindowState>>,
  pub handlers: EventHandlers,
}

impl CommonState {
  pub fn new() -> Self {
    Self {
      windows: Mutex::new(HashMap::new()),
      handlers: EventHandlers::new(),
    }
  }

  pub fn add_window(&self, window_id: u32) {
    let mut windows = self.windows.lock().unwrap();
    windows.insert(window_id, WindowState::new());
  }

  pub fn remove_window(&self, window_id: u32) {
    let mut windows = self.windows.lock().unwrap();
    windows.remove(&window_id);
  }

  pub fn with_window<F, R>(&self, window_id: u32, f: F) -> Option<R>
  where
    F: FnOnce(&WindowState) -> R,
  {
    let windows = self.windows.lock().unwrap();
    windows.get(&window_id).map(f)
  }
}

impl Default for CommonState {
  fn default() -> Self {
    Self::new()
  }
}

// --- Common event enum ---

#[derive(Debug)]
pub enum CommonEvent {
  CreateWindow {
    window_id: u32,
  },
  CloseWindow {
    window_id: u32,
  },
  SetTitle {
    window_id: u32,
  },
  SetWindowSize {
    window_id: u32,
  },
  SetWindowPosition {
    window_id: u32,
  },
  SetResizable {
    window_id: u32,
  },
  SetAlwaysOnTop {
    window_id: u32,
  },
  Show {
    window_id: u32,
  },
  Hide {
    window_id: u32,
  },
  Focus {
    window_id: u32,
  },
  SetApplicationMenu {
    window_id: u32,
  },
  ShowContextMenu {
    window_id: u32,
  },
  Quit,
  UiTask {
    task: unsafe extern "C" fn(*mut c_void),
    data: usize,
  },
  /// App-scoped dock / taskbar op queued via `dock::queue_op`.
  /// The app handler calls `dock::drain_and_apply` to process pending ops.
  DockTask,
  /// Tray / status-bar op queued via `tray::queue_op`.
  /// The app handler calls `tray::drain_and_apply` on the main thread.
  TrayTask,
}

// --- Trait for backend state access ---

pub trait BackendAccess: Sized + 'static {
  type Event: 'static;

  fn get() -> Option<&'static Self>;
  fn proxy(&self) -> &EventLoopProxy<Self::Event>;
  fn common(&self) -> &CommonState;
  fn common_event(event: CommonEvent) -> Self::Event;
}

// --- Macro to generate C ABI backend functions ---

#[macro_export]
macro_rules! define_common_backend_fns {
  ($B:ty) => {
    unsafe extern "C" fn backend_create_window(
      _data: *mut ::std::ffi::c_void,
    ) -> u32 {
      let window_id = $crate::allocate_window_id();
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        state.common().add_window(window_id);
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::CreateWindow { window_id },
          ),
        );
      }
      window_id
    }

    unsafe extern "C" fn backend_create_window_ex(
      _data: *mut ::std::ffi::c_void,
      flags: u32,
    ) -> u32 {
      let window_id = $crate::allocate_window_id();
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        state.common().add_window(window_id);
        // Stash the style flags before the event is processed so
        // apply_pending_attrs sees them when the winit Window is built.
        state.common().with_window(window_id, |ws| {
          *ws.pending_flags.lock().unwrap() = flags;
        });
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::CreateWindow { window_id },
          ),
        );
      }
      window_id
    }

    unsafe extern "C" fn backend_close_window(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::CloseWindow { window_id },
          ),
        );
      }
    }

    unsafe extern "C" fn backend_set_title(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
      title: *const ::std::ffi::c_char,
    ) {
      if title.is_null() {
        return;
      }
      let title_str = ::std::ffi::CStr::from_ptr(title)
        .to_string_lossy()
        .into_owned();
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        state.common().with_window(window_id, |ws| {
          *ws.pending_title.lock().unwrap() = Some(title_str);
        });
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::SetTitle { window_id },
          ),
        );
      }
    }

    unsafe extern "C" fn backend_quit(_data: *mut ::std::ffi::c_void) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::Quit,
          ),
        );
      }
    }

    unsafe extern "C" fn backend_set_window_size(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
      width: ::std::ffi::c_int,
      height: ::std::ffi::c_int,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        state.common().with_window(window_id, |ws| {
          *ws.pending_size.lock().unwrap() = Some((width, height));
        });
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::SetWindowSize { window_id },
          ),
        );
      }
    }

    unsafe extern "C" fn backend_get_window_size(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
      width: *mut ::std::ffi::c_int,
      height: *mut ::std::ffi::c_int,
    ) {
      let mut found = false;
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        if let Some(()) = state.common().with_window(window_id, |ws| {
          if let Some((w, h)) = *ws.pending_size.lock().unwrap() {
            if !width.is_null() {
              *width = w;
            }
            if !height.is_null() {
              *height = h;
            }
          }
        }) {
          found = true;
        }
      }
      if !found {
        if !width.is_null() {
          *width = 0;
        }
        if !height.is_null() {
          *height = 0;
        }
      }
    }

    unsafe extern "C" fn backend_set_window_position(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
      x: ::std::ffi::c_int,
      y: ::std::ffi::c_int,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        state.common().with_window(window_id, |ws| {
          *ws.pending_position.lock().unwrap() = Some((x, y));
        });
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::SetWindowPosition { window_id },
          ),
        );
      }
    }

    unsafe extern "C" fn backend_get_window_position(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
      x: *mut ::std::ffi::c_int,
      y: *mut ::std::ffi::c_int,
    ) {
      let mut found = false;
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        if let Some(()) = state.common().with_window(window_id, |ws| {
          if let Some((px, py)) = *ws.pending_position.lock().unwrap() {
            if !x.is_null() {
              *x = px;
            }
            if !y.is_null() {
              *y = py;
            }
          }
        }) {
          found = true;
        }
      }
      if !found {
        if !x.is_null() {
          *x = 0;
        }
        if !y.is_null() {
          *y = 0;
        }
      }
    }

    unsafe extern "C" fn backend_set_resizable(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
      resizable: bool,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        state.common().with_window(window_id, |ws| {
          *ws.pending_resizable.lock().unwrap() = Some(resizable);
        });
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::SetResizable { window_id },
          ),
        );
      }
    }

    unsafe extern "C" fn backend_is_resizable(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
    ) -> bool {
      <$B as $crate::BackendAccess>::get()
        .and_then(|s| {
          s.common()
            .with_window(window_id, |ws| *ws.pending_resizable.lock().unwrap())
            .flatten()
        })
        .unwrap_or(true)
    }

    unsafe extern "C" fn backend_set_always_on_top(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
      always_on_top: bool,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        state.common().with_window(window_id, |ws| {
          *ws.pending_always_on_top.lock().unwrap() = Some(always_on_top);
        });
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::SetAlwaysOnTop { window_id },
          ),
        );
      }
    }

    unsafe extern "C" fn backend_is_always_on_top(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
    ) -> bool {
      <$B as $crate::BackendAccess>::get()
        .and_then(|s| {
          s.common()
            .with_window(window_id, |ws| {
              *ws.pending_always_on_top.lock().unwrap()
            })
            .flatten()
        })
        .unwrap_or(false)
    }

    unsafe extern "C" fn backend_is_visible(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
    ) -> bool {
      <$B as $crate::BackendAccess>::get()
        .and_then(|s| {
          s.common()
            .with_window(window_id, |ws| *ws.pending_visible.lock().unwrap())
            .flatten()
        })
        .unwrap_or(true)
    }

    unsafe extern "C" fn backend_show(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        state.common().with_window(window_id, |ws| {
          *ws.pending_visible.lock().unwrap() = Some(true);
        });
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::Show { window_id },
          ),
        );
      }
    }

    unsafe extern "C" fn backend_hide(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        state.common().with_window(window_id, |ws| {
          *ws.pending_visible.lock().unwrap() = Some(false);
        });
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::Hide { window_id },
          ),
        );
      }
    }

    unsafe extern "C" fn backend_focus(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::Focus { window_id },
          ),
        );
      }
    }

    unsafe extern "C" fn backend_post_ui_task(
      _data: *mut ::std::ffi::c_void,
      task: Option<unsafe extern "C" fn(*mut ::std::ffi::c_void)>,
      task_data: *mut ::std::ffi::c_void,
    ) {
      if let Some(task_fn) = task {
        if let Some(state) = <$B as $crate::BackendAccess>::get() {
          let _ = state.proxy().send_event(
            <$B as $crate::BackendAccess>::common_event(
              $crate::CommonEvent::UiTask {
                task: task_fn,
                data: task_data as usize,
              },
            ),
          );
        }
      }
    }

    unsafe extern "C" fn backend_set_keyboard_event_handler(
      _data: *mut ::std::ffi::c_void,
      handler: Option<$crate::LaufeyKeyboardEventFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        *state.common().handlers.keyboard_handler.lock().unwrap() =
          handler.map(|h| (h, user_data as usize));
      }
    }

    unsafe extern "C" fn backend_set_mouse_click_handler(
      _data: *mut ::std::ffi::c_void,
      handler: Option<$crate::LaufeyMouseClickFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        *state.common().handlers.mouse_click_handler.lock().unwrap() =
          handler.map(|h| (h, user_data as usize));
      }
    }

    unsafe extern "C" fn backend_set_mouse_move_handler(
      _data: *mut ::std::ffi::c_void,
      handler: Option<$crate::LaufeyMouseMoveFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        *state.common().handlers.mouse_move_handler.lock().unwrap() =
          handler.map(|h| (h, user_data as usize));
      }
    }

    unsafe extern "C" fn backend_set_wheel_handler(
      _data: *mut ::std::ffi::c_void,
      handler: Option<$crate::LaufeyWheelFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        *state.common().handlers.wheel_handler.lock().unwrap() =
          handler.map(|h| (h, user_data as usize));
      }
    }

    unsafe extern "C" fn backend_set_cursor_enter_leave_handler(
      _data: *mut ::std::ffi::c_void,
      handler: Option<$crate::LaufeyCursorEnterLeaveFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        *state
          .common()
          .handlers
          .cursor_enter_leave_handler
          .lock()
          .unwrap() = handler.map(|h| (h, user_data as usize));
      }
    }

    unsafe extern "C" fn backend_set_focused_handler(
      _data: *mut ::std::ffi::c_void,
      handler: Option<$crate::LaufeyFocusedFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        *state.common().handlers.focused_handler.lock().unwrap() =
          handler.map(|h| (h, user_data as usize));
      }
    }

    unsafe extern "C" fn backend_set_resize_handler(
      _data: *mut ::std::ffi::c_void,
      handler: Option<$crate::LaufeyResizeFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        *state.common().handlers.resize_handler.lock().unwrap() =
          handler.map(|h| (h, user_data as usize));
      }
    }

    unsafe extern "C" fn backend_set_move_handler(
      _data: *mut ::std::ffi::c_void,
      handler: Option<$crate::LaufeyMoveFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        *state.common().handlers.move_handler.lock().unwrap() =
          handler.map(|h| (h, user_data as usize));
      }
    }

    unsafe extern "C" fn backend_set_close_requested_handler(
      _data: *mut ::std::ffi::c_void,
      handler: Option<$crate::LaufeyCloseRequestedFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        *state
          .common()
          .handlers
          .close_requested_handler
          .lock()
          .unwrap() = handler.map(|h| (h, user_data as usize));
      }
    }

    unsafe extern "C" fn backend_show_dialog(
      _data: *mut ::std::ffi::c_void,
      _window_id: u32,
      dialog_type: ::std::ffi::c_int,
      title: *const ::std::ffi::c_char,
      message: *const ::std::ffi::c_char,
      default_value: *const ::std::ffi::c_char,
      out_input_value: *mut *mut ::std::ffi::c_char,
    ) -> ::std::ffi::c_int {
      if !out_input_value.is_null() {
        unsafe { *out_input_value = ::std::ptr::null_mut() };
      }
      let title_str = if title.is_null() {
        String::new()
      } else {
        unsafe { ::std::ffi::CStr::from_ptr(title) }
          .to_string_lossy()
          .into_owned()
      };
      let message_str = if message.is_null() {
        String::new()
      } else {
        unsafe { ::std::ffi::CStr::from_ptr(message) }
          .to_string_lossy()
          .into_owned()
      };
      let default_str = if default_value.is_null() {
        String::new()
      } else {
        unsafe { ::std::ffi::CStr::from_ptr(default_value) }
          .to_string_lossy()
          .into_owned()
      };
      // The platform modal APIs (NSAlert / MessageBoxW / gtk_dialog_run /
      // rfd) themselves run a nested event loop, so they don't need help
      // from the winit event loop to keep other windows responsive — call
      // them directly.
      let (confirmed, input) = $crate::show_native_dialog(
        dialog_type,
        &title_str,
        &message_str,
        &default_str,
      );
      if !out_input_value.is_null() {
        if let Some(s) = input {
          if let Ok(c_str) = ::std::ffi::CString::new(s) {
            // SAFETY: out_input_value is non-null and the consumer frees
            // the pointer via the backend's `string_free`.
            unsafe { *out_input_value = c_str.into_raw() };
          }
        }
      }
      if confirmed {
        1
      } else {
        0
      }
    }

    unsafe extern "C" fn backend_string_free(
      _data: *mut ::std::ffi::c_void,
      s: *mut ::std::ffi::c_char,
    ) {
      if !s.is_null() {
        // SAFETY: pointer originated from `CString::into_raw` in
        // `backend_show_dialog`; reclaiming the allocation is the
        // matching deallocator.
        let _ = unsafe { ::std::ffi::CString::from_raw(s) };
      }
    }

    unsafe extern "C" fn backend_set_application_menu(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
      template: *mut $crate::LaufeyValue,
      callback: Option<$crate::LaufeyMenuClickFn>,
      callback_data: *mut ::std::ffi::c_void,
    ) {
      let items = $crate::parse_menu_template(template);
      $crate::value_free(template);
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        state.common().with_window(window_id, |ws| {
          *ws.pending_app_menu.lock().unwrap() = Some($crate::PendingMenu {
            items,
            callback,
            callback_data: callback_data as usize,
          });
        });
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::SetApplicationMenu { window_id },
          ),
        );
      }
    }

    unsafe extern "C" fn backend_show_context_menu(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
      x: ::std::ffi::c_int,
      y: ::std::ffi::c_int,
      template: *mut $crate::LaufeyValue,
      callback: Option<$crate::LaufeyMenuClickFn>,
      callback_data: *mut ::std::ffi::c_void,
    ) {
      let items = $crate::parse_menu_template(template);
      $crate::value_free(template);
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        state.common().with_window(window_id, |ws| {
          *ws.pending_context_menu.lock().unwrap() =
            Some($crate::PendingContextMenu {
              x,
              y,
              items,
              callback,
              callback_data: callback_data as usize,
            });
        });
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::ShowContextMenu { window_id },
          ),
        );
      }
    }

    unsafe extern "C" fn backend_set_dock_badge(
      _data: *mut ::std::ffi::c_void,
      badge: *const ::std::ffi::c_char,
    ) {
      let text = if badge.is_null() {
        None
      } else {
        Some(
          ::std::ffi::CStr::from_ptr(badge)
            .to_string_lossy()
            .into_owned(),
        )
      };
      $crate::dock::queue_op($crate::dock::DockOp::SetBadge(text));
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::DockTask,
          ),
        );
      }
    }

    unsafe extern "C" fn backend_bounce_dock(
      _data: *mut ::std::ffi::c_void,
      kind: ::std::ffi::c_int,
    ) {
      $crate::dock::queue_op($crate::dock::DockOp::Bounce(kind));
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::DockTask,
          ),
        );
      }
    }

    unsafe extern "C" fn backend_set_dock_menu(
      _data: *mut ::std::ffi::c_void,
      template: *mut $crate::LaufeyValue,
      callback: Option<$crate::LaufeyMenuClickFn>,
      callback_data: *mut ::std::ffi::c_void,
    ) {
      let pm = if template.is_null() {
        None
      } else {
        let items = $crate::parse_menu_template(template);
        $crate::value_free(template);
        Some($crate::dock::PendingDockMenu {
          items,
          callback,
          callback_data: callback_data as usize,
        })
      };
      $crate::dock::queue_op($crate::dock::DockOp::SetMenu(pm));
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::DockTask,
          ),
        );
      }
    }

    unsafe extern "C" fn backend_set_dock_visible(
      _data: *mut ::std::ffi::c_void,
      visible: bool,
    ) {
      $crate::dock::queue_op($crate::dock::DockOp::SetVisible(visible));
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::DockTask,
          ),
        );
      }
    }

    unsafe extern "C" fn backend_set_dock_reopen_handler(
      _data: *mut ::std::ffi::c_void,
      handler: Option<$crate::dock::LaufeyDockReopenFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      let h = handler.map(|f| (f, user_data as usize));
      $crate::dock::queue_op($crate::dock::DockOp::SetReopenHandler(h));
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::DockTask,
          ),
        );
      }
    }

    unsafe extern "C" fn backend_create_tray_icon(
      _data: *mut ::std::ffi::c_void,
    ) -> u32 {
      let tray_id = $crate::tray::allocate_tray_id();
      $crate::tray::queue_op($crate::tray::TrayOp::Create { tray_id });
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::TrayTask,
          ),
        );
      }
      tray_id
    }

    unsafe extern "C" fn backend_destroy_tray_icon(
      _data: *mut ::std::ffi::c_void,
      tray_id: u32,
    ) {
      $crate::tray::queue_op($crate::tray::TrayOp::Destroy { tray_id });
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::TrayTask,
          ),
        );
      }
    }

    unsafe extern "C" fn backend_set_tray_icon(
      _data: *mut ::std::ffi::c_void,
      tray_id: u32,
      png_bytes: *const ::std::ffi::c_void,
      len: usize,
    ) {
      let png = $crate::tray::slice_to_vec(png_bytes, len);
      $crate::tray::queue_op($crate::tray::TrayOp::SetIcon { tray_id, png });
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::TrayTask,
          ),
        );
      }
    }

    unsafe extern "C" fn backend_set_tray_tooltip(
      _data: *mut ::std::ffi::c_void,
      tray_id: u32,
      tooltip: *const ::std::ffi::c_char,
    ) {
      let text = $crate::tray::cstr_opt(tooltip);
      $crate::tray::queue_op($crate::tray::TrayOp::SetTooltip {
        tray_id,
        text,
      });
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::TrayTask,
          ),
        );
      }
    }

    unsafe extern "C" fn backend_set_tray_menu(
      _data: *mut ::std::ffi::c_void,
      tray_id: u32,
      template: *mut $crate::LaufeyValue,
      callback: Option<$crate::LaufeyMenuClickFn>,
      callback_data: *mut ::std::ffi::c_void,
    ) {
      if template.is_null() {
        $crate::tray::queue_op($crate::tray::TrayOp::ClearMenu { tray_id });
      } else {
        let items = $crate::parse_menu_template(template);
        $crate::value_free(template);
        $crate::tray::queue_op($crate::tray::TrayOp::SetMenu {
          tray_id,
          items,
          callback,
          callback_data: callback_data as usize,
        });
      }
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::TrayTask,
          ),
        );
      }
    }

    unsafe extern "C" fn backend_set_tray_click_handler(
      _data: *mut ::std::ffi::c_void,
      tray_id: u32,
      handler: Option<$crate::tray::LaufeyTrayClickFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      $crate::tray::queue_op($crate::tray::TrayOp::SetClickHandler {
        tray_id,
        handler,
        user_data: user_data as usize,
      });
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::TrayTask,
          ),
        );
      }
    }

    unsafe extern "C" fn backend_set_tray_double_click_handler(
      _data: *mut ::std::ffi::c_void,
      tray_id: u32,
      handler: Option<$crate::tray::LaufeyTrayClickFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      $crate::tray::queue_op($crate::tray::TrayOp::SetDoubleClickHandler {
        tray_id,
        handler,
        user_data: user_data as usize,
      });
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::TrayTask,
          ),
        );
      }
    }

    unsafe extern "C" fn backend_set_tray_icon_dark(
      _data: *mut ::std::ffi::c_void,
      tray_id: u32,
      png_bytes: *const ::std::ffi::c_void,
      len: usize,
    ) {
      let png = $crate::tray::slice_to_vec(png_bytes, len);
      $crate::tray::queue_op($crate::tray::TrayOp::SetIconDark {
        tray_id,
        png,
      });
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::TrayTask,
          ),
        );
      }
    }

    unsafe extern "C" fn backend_get_tray_icon_bounds(
      _data: *mut ::std::ffi::c_void,
      tray_id: u32,
      x: *mut ::std::ffi::c_int,
      y: *mut ::std::ffi::c_int,
      width: *mut ::std::ffi::c_int,
      height: *mut ::std::ffi::c_int,
    ) -> bool {
      match $crate::tray::tray_bounds(tray_id) {
        Some((bx, by, bw, bh)) => {
          if !x.is_null() {
            *x = bx;
          }
          if !y.is_null() {
            *y = by;
          }
          if !width.is_null() {
            *width = bw;
          }
          if !height.is_null() {
            *height = bh;
          }
          true
        }
        None => false,
      }
    }

    unsafe extern "C" fn backend_show_notification(
      _data: *mut ::std::ffi::c_void,
      options: *mut $crate::LaufeyValue,
      on_event: ::std::option::Option<
        $crate::notification::LaufeyNotificationEventFn,
      >,
      user_data: *mut ::std::ffi::c_void,
    ) -> u32 {
      $crate::notification::show_notification(options, on_event, user_data)
    }

    unsafe extern "C" fn backend_close_notification(
      _data: *mut ::std::ffi::c_void,
      notification_id: u32,
    ) {
      $crate::notification::close_notification(notification_id);
    }

    unsafe extern "C" fn backend_query_permission(
      _data: *mut ::std::ffi::c_void,
      kind: ::std::ffi::c_int,
      cb: ::std::option::Option<$crate::permission::LaufeyPermissionCallbackFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      $crate::permission::query_permission(kind, cb, user_data);
    }

    unsafe extern "C" fn backend_request_permission(
      _data: *mut ::std::ffi::c_void,
      kind: ::std::ffi::c_int,
      cb: ::std::option::Option<$crate::permission::LaufeyPermissionCallbackFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      $crate::permission::request_permission(kind, cb, user_data);
    }
  };
}

/// Fill the common (window management) backend function pointers into an API struct.
#[macro_export]
macro_rules! fill_common_api {
  ($api:expr) => {
    $api.create_window = Some(backend_create_window);
    $api.create_window_ex = Some(backend_create_window_ex);
    $api.close_window = Some(backend_close_window);
    $api.set_title = Some(backend_set_title);
    $api.quit = Some(backend_quit);
    $api.set_window_size = Some(backend_set_window_size);
    $api.get_window_size = Some(backend_get_window_size);
    $api.set_window_position = Some(backend_set_window_position);
    $api.get_window_position = Some(backend_get_window_position);
    $api.set_resizable = Some(backend_set_resizable);
    $api.is_resizable = Some(backend_is_resizable);
    $api.set_always_on_top = Some(backend_set_always_on_top);
    $api.is_always_on_top = Some(backend_is_always_on_top);
    $api.is_visible = Some(backend_is_visible);
    $api.show = Some(backend_show);
    $api.hide = Some(backend_hide);
    $api.focus = Some(backend_focus);
    $api.post_ui_task = Some(backend_post_ui_task);
    $api.get_window_handle = Some($crate::backend_get_window_handle);
    $api.get_display_handle = Some($crate::backend_get_display_handle);
    $api.get_window_handle_type = Some($crate::backend_get_window_handle_type);
    $api.set_keyboard_event_handler = Some(backend_set_keyboard_event_handler);
    $api.set_mouse_click_handler = Some(backend_set_mouse_click_handler);
    $api.set_mouse_move_handler = Some(backend_set_mouse_move_handler);
    $api.set_wheel_handler = Some(backend_set_wheel_handler);
    $api.set_cursor_enter_leave_handler =
      Some(backend_set_cursor_enter_leave_handler);
    $api.set_focused_handler = Some(backend_set_focused_handler);
    $api.set_resize_handler = Some(backend_set_resize_handler);
    $api.set_move_handler = Some(backend_set_move_handler);
    $api.set_close_requested_handler =
      Some(backend_set_close_requested_handler);
    $api.show_dialog = Some(backend_show_dialog);
    $api.string_free = Some(backend_string_free);
    $api.set_application_menu = Some(backend_set_application_menu);
    $api.show_context_menu = Some(backend_show_context_menu);
    $api.set_dock_badge = Some(backend_set_dock_badge);
    $api.bounce_dock = Some(backend_bounce_dock);
    $api.set_dock_menu = Some(backend_set_dock_menu);
    $api.set_dock_visible = Some(backend_set_dock_visible);
    $api.set_dock_reopen_handler = Some(backend_set_dock_reopen_handler);
    $api.create_tray_icon = Some(backend_create_tray_icon);
    $api.destroy_tray_icon = Some(backend_destroy_tray_icon);
    $api.set_tray_icon = Some(backend_set_tray_icon);
    $api.set_tray_tooltip = Some(backend_set_tray_tooltip);
    $api.set_tray_menu = Some(backend_set_tray_menu);
    $api.set_tray_click_handler = Some(backend_set_tray_click_handler);
    $api.set_tray_double_click_handler =
      Some(backend_set_tray_double_click_handler);
    $api.set_tray_icon_dark = Some(backend_set_tray_icon_dark);
    $api.get_tray_icon_bounds = Some(backend_get_tray_icon_bounds);
    $api.show_notification = Some(backend_show_notification);
    $api.close_notification = Some(backend_close_notification);
    $api.query_permission = Some(backend_query_permission);
    $api.request_permission = Some(backend_request_permission);
  };
}

// --- Common event handling ---

/// Handle common window management events on a specific window.
pub fn handle_common_event<B: BackendAccess>(
  event: &CommonEvent,
  window_id: u32,
  window: &Window,
) -> bool {
  match event {
    CommonEvent::SetTitle { window_id: eid, .. } if *eid == window_id => {
      if let Some(state) = B::get() {
        state.common().with_window(window_id, |ws| {
          if let Some(title) = ws.pending_title.lock().unwrap().take() {
            window.set_title(&title);
          }
        });
      }
      true
    }
    CommonEvent::SetWindowSize { window_id: eid, .. } if *eid == window_id => {
      if let Some(state) = B::get() {
        state.common().with_window(window_id, |ws| {
          if let Some((w, h)) = ws.pending_size.lock().unwrap().take() {
            let _ = window.request_inner_size(LogicalSize::new(w, h));
          }
        });
      }
      true
    }
    CommonEvent::SetWindowPosition { window_id: eid, .. }
      if *eid == window_id =>
    {
      if let Some(state) = B::get() {
        state.common().with_window(window_id, |ws| {
          if let Some((x, y)) = ws.pending_position.lock().unwrap().take() {
            window.set_outer_position(LogicalPosition::new(x, y));
          }
        });
      }
      true
    }
    CommonEvent::SetResizable { window_id: eid, .. } if *eid == window_id => {
      if let Some(state) = B::get() {
        state.common().with_window(window_id, |ws| {
          if let Some(resizable) = *ws.pending_resizable.lock().unwrap() {
            window.set_resizable(resizable);
          }
        });
      }
      true
    }
    CommonEvent::SetAlwaysOnTop { window_id: eid, .. } if *eid == window_id => {
      if let Some(state) = B::get() {
        state.common().with_window(window_id, |ws| {
          if let Some(on_top) = *ws.pending_always_on_top.lock().unwrap() {
            window.set_window_level(if on_top {
              WindowLevel::AlwaysOnTop
            } else {
              WindowLevel::Normal
            });
          }
        });
      }
      true
    }
    CommonEvent::Show { window_id: eid, .. } if *eid == window_id => {
      window.set_visible(true);
      true
    }
    CommonEvent::Hide { window_id: eid, .. } if *eid == window_id => {
      window.set_visible(false);
      true
    }
    CommonEvent::Focus { window_id: eid, .. } if *eid == window_id => {
      window.focus_window();
      true
    }
    CommonEvent::SetApplicationMenu { window_id: eid } if *eid == window_id => {
      if let Some(state) = B::get() {
        state.common().with_window(window_id, |ws| {
          if let Some(pending) = ws.pending_app_menu.lock().unwrap().take() {
            register_menu_callbacks(
              &pending.items,
              pending.callback,
              pending.callback_data,
              window_id,
            );
            let menu = build_muda_menu(&pending.items);

            #[cfg(target_os = "macos")]
            {
              menu.init_for_nsapp();
            }
            #[cfg(target_os = "windows")]
            {
              if let Ok(wh) = window.window_handle() {
                if let RawWindowHandle::Win32(handle) = wh.as_raw() {
                  let hwnd = handle.hwnd.get() as isize;
                  unsafe {
                    let _ = menu.init_for_hwnd(hwnd);
                  }
                }
              }
            }
            #[cfg(target_os = "linux")]
            {
              // muda on Linux requires a gtk window; winit doesn't
              // provide one, so app menus are not supported on Linux.
              let _ = &menu;
            }

            ACTIVE_APP_MENUS.with(|menus| {
              menus.borrow_mut().insert(window_id, menu);
            });
          }
        });
      }
      true
    }
    CommonEvent::ShowContextMenu { window_id: eid } if *eid == window_id => {
      if let Some(state) = B::get() {
        state.common().with_window(window_id, |ws| {
          if let Some(pending) = ws.pending_context_menu.lock().unwrap().take()
          {
            register_menu_callbacks(
              &pending.items,
              pending.callback,
              pending.callback_data,
              window_id,
            );
            let menu = build_muda_context_menu(&pending.items);
            let position =
              muda::dpi::Position::Logical(muda::dpi::LogicalPosition::new(
                pending.x as f64,
                pending.y as f64,
              ));

            #[cfg(target_os = "macos")]
            {
              use muda::ContextMenu;
              if let Ok(wh) = window.window_handle() {
                if let RawWindowHandle::AppKit(handle) = wh.as_raw() {
                  let ns_view = handle.ns_view.as_ptr();
                  unsafe {
                    let _ = menu.show_context_menu_for_nsview(
                      ns_view as _,
                      Some(position),
                    );
                  }
                }
              }
            }
            #[cfg(target_os = "windows")]
            {
              use muda::ContextMenu;
              if let Ok(wh) = window.window_handle() {
                if let RawWindowHandle::Win32(handle) = wh.as_raw() {
                  let hwnd = handle.hwnd.get() as isize;
                  let _ = unsafe {
                    menu.show_context_menu_for_hwnd(hwnd, Some(position))
                  };
                }
              }
            }
            #[cfg(target_os = "linux")]
            {
              // muda on Linux requires a gtk window; winit doesn't
              // provide one, so context menus are not supported on Linux.
              let _ = (&menu, &position);
            }
          }
        });
      }
      true
    }
    CommonEvent::UiTask { task, data } => {
      unsafe { task(*data as *mut c_void) };
      true
    }
    CommonEvent::Quit => false, // caller handles exit
    _ => false,
  }
}

/// Apply pending state to window attributes before creation.
pub fn apply_pending_attrs(
  ws: &WindowState,
  mut attrs: winit::window::WindowAttributes,
) -> winit::window::WindowAttributes {
  if let Some((w, h)) = *ws.pending_size.lock().unwrap() {
    attrs = attrs.with_inner_size(LogicalSize::new(w, h));
  }
  if let Some((x, y)) = *ws.pending_position.lock().unwrap() {
    attrs = attrs.with_position(LogicalPosition::new(x, y));
  }
  if let Some(resizable) = *ws.pending_resizable.lock().unwrap() {
    attrs = attrs.with_resizable(resizable);
  }
  if let Some(title) = ws.pending_title.lock().unwrap().as_ref() {
    attrs = attrs.with_title(title.clone());
  }
  let flags = *ws.pending_flags.lock().unwrap();
  if flags & LAUFEY_WINDOW_FLAG_FRAMELESS != 0 {
    // Drop the title bar and standard window chrome.
    attrs = attrs.with_decorations(false);
  }
  if flags & LAUFEY_WINDOW_FLAG_NO_ACTIVATE != 0 {
    // Don't activate / take focus when first shown.
    attrs = attrs.with_active(false);
  }
  attrs
}

/// Apply post-creation pending state (e.g. always-on-top).
pub fn apply_pending_post_create(ws: &WindowState, window: &Window) {
  if let Some(true) = *ws.pending_always_on_top.lock().unwrap() {
    window.set_window_level(WindowLevel::AlwaysOnTop);
  }
  // A non-activating panel floats above normal windows, like a tray popover
  // (matches the NSPanel / always-on-top behavior of the other backends).
  if *ws.pending_flags.lock().unwrap() & LAUFEY_WINDOW_FLAG_NO_ACTIVATE != 0 {
    window.set_window_level(WindowLevel::AlwaysOnTop);
  }
}

// --- Native dialog implementation ---

pub fn show_native_dialog(
  dialog_type: i32,
  title: &str,
  message: &str,
  default_value: &str,
) -> (bool, Option<String>) {
  match dialog_type {
    LAUFEY_DIALOG_ALERT => {
      rfd::MessageDialog::new()
        .set_title(title)
        .set_description(message)
        .set_buttons(rfd::MessageButtons::Ok)
        .show();
      (true, None)
    }
    LAUFEY_DIALOG_CONFIRM => {
      let result = rfd::MessageDialog::new()
        .set_title(title)
        .set_description(message)
        .set_buttons(rfd::MessageButtons::OkCancel)
        .show();
      (result == rfd::MessageDialogResult::Ok, None)
    }
    LAUFEY_DIALOG_PROMPT => show_prompt_dialog(title, message, default_value),
    _ => (false, None),
  }
}

#[cfg(target_os = "macos")]
fn show_prompt_dialog(
  title: &str,
  message: &str,
  default_value: &str,
) -> (bool, Option<String>) {
  let script = format!(
    "set result to display dialog \"{}\" default answer \"{}\" with title \"{}\" buttons {{\"Cancel\", \"OK\"}} default button \"OK\"\nreturn text returned of result",
    message.replace('\\', "\\\\").replace('"', "\\\""),
    default_value.replace('\\', "\\\\").replace('"', "\\\""),
    title.replace('\\', "\\\\").replace('"', "\\\""),
  );
  match std::process::Command::new("osascript")
    .arg("-e")
    .arg(&script)
    .output()
  {
    Ok(output) if output.status.success() => {
      let text = String::from_utf8_lossy(&output.stdout).trim().to_string();
      (true, Some(text))
    }
    _ => (false, None),
  }
}

#[cfg(target_os = "windows")]
fn show_prompt_dialog(
  title: &str,
  message: &str,
  default_value: &str,
) -> (bool, Option<String>) {
  let script = format!(
    r#"Add-Type -AssemblyName Microsoft.VisualBasic; [Microsoft.VisualBasic.Interaction]::InputBox('{}', '{}', '{}')"#,
    message.replace('\'', "''"),
    title.replace('\'', "''"),
    default_value.replace('\'', "''"),
  );
  match std::process::Command::new("powershell")
    .args(["-Command", &script])
    .output()
  {
    Ok(output) if output.status.success() => {
      let text = String::from_utf8_lossy(&output.stdout).trim().to_string();
      if text.is_empty() {
        (false, None)
      } else {
        (true, Some(text))
      }
    }
    _ => (false, None),
  }
}

#[cfg(target_os = "linux")]
fn show_prompt_dialog(
  title: &str,
  message: &str,
  default_value: &str,
) -> (bool, Option<String>) {
  match std::process::Command::new("zenity")
    .args([
      "--entry",
      "--title",
      title,
      "--text",
      message,
      "--entry-text",
      default_value,
    ])
    .output()
  {
    Ok(output) if output.status.success() => {
      let text = String::from_utf8_lossy(&output.stdout).trim().to_string();
      (true, Some(text))
    }
    _ => (false, None),
  }
}

#[cfg(not(any(
  target_os = "macos",
  target_os = "windows",
  target_os = "linux"
)))]
fn show_prompt_dialog(
  _title: &str,
  _message: &str,
  default_value: &str,
) -> (bool, Option<String>) {
  (true, Some(default_value.to_string()))
}

// --- Keyboard modifier flags ---

pub const LAUFEY_MOD_SHIFT: u32 = 1 << 0;
pub const LAUFEY_MOD_CONTROL: u32 = 1 << 1;
pub const LAUFEY_MOD_ALT: u32 = 1 << 2;
pub const LAUFEY_MOD_META: u32 = 1 << 3;

pub const LAUFEY_KEY_PRESSED: c_int = 0;
pub const LAUFEY_KEY_RELEASED: c_int = 1;

pub const LAUFEY_MOUSE_BUTTON_LEFT: c_int = 0;
pub const LAUFEY_MOUSE_BUTTON_RIGHT: c_int = 1;
pub const LAUFEY_MOUSE_BUTTON_MIDDLE: c_int = 2;
pub const LAUFEY_MOUSE_BUTTON_BACK: c_int = 3;
pub const LAUFEY_MOUSE_BUTTON_FORWARD: c_int = 4;

pub const LAUFEY_MOUSE_PRESSED: c_int = 0;
pub const LAUFEY_MOUSE_RELEASED: c_int = 1;

/// Convert winit modifier state to LAUFEY modifier bitmask.
pub fn modifiers_to_laufey(mods: winit::keyboard::ModifiersState) -> u32 {
  let mut flags = 0u32;
  if mods.shift_key() {
    flags |= LAUFEY_MOD_SHIFT;
  }
  if mods.control_key() {
    flags |= LAUFEY_MOD_CONTROL;
  }
  if mods.alt_key() {
    flags |= LAUFEY_MOD_ALT;
  }
  if mods.super_key() {
    flags |= LAUFEY_MOD_META;
  }
  flags
}

/// Convert a winit logical key to its W3C UI Events `key` string representation.
pub fn winit_key_to_string(key: &winit::keyboard::Key) -> String {
  match key {
    winit::keyboard::Key::Character(c) => c.to_string(),
    winit::keyboard::Key::Named(named) => format!("{named:?}"),
    winit::keyboard::Key::Unidentified(_) => "Unidentified".to_string(),
    winit::keyboard::Key::Dead(c) => {
      if let Some(ch) = c {
        format!("Dead({ch})")
      } else {
        "Dead".to_string()
      }
    }
  }
}

/// Convert a winit physical key to its W3C UI Events `code` string representation.
pub fn winit_code_to_string(physical: &winit::keyboard::PhysicalKey) -> String {
  match physical {
    winit::keyboard::PhysicalKey::Code(code) => format!("{code:?}"),
    winit::keyboard::PhysicalKey::Unidentified(_) => "Unidentified".to_string(),
  }
}

/// Dispatch a keyboard event to the registered handler.
pub fn dispatch_keyboard_event(
  handlers: &EventHandlers,
  window_id: u32,
  key_event: &winit::event::KeyEvent,
  modifiers: winit::keyboard::ModifiersState,
) {
  let handler = handlers.keyboard_handler.lock().unwrap();
  if let Some((cb, user_data)) = *handler {
    let state = match key_event.state {
      winit::event::ElementState::Pressed => LAUFEY_KEY_PRESSED,
      winit::event::ElementState::Released => LAUFEY_KEY_RELEASED,
    };
    let key_str = winit_key_to_string(&key_event.logical_key);
    let code_str = winit_code_to_string(&key_event.physical_key);
    let mods = modifiers_to_laufey(modifiers);

    let c_key = std::ffi::CString::new(key_str).unwrap_or_default();
    let c_code = std::ffi::CString::new(code_str).unwrap_or_default();

    unsafe {
      cb(
        user_data as *mut c_void,
        window_id,
        state,
        c_key.as_ptr(),
        c_code.as_ptr(),
        mods,
        key_event.repeat,
      );
    }
  }
}

/// Convert a winit mouse button to a LAUFEY mouse button constant.
pub fn winit_button_to_laufey(button: winit::event::MouseButton) -> c_int {
  match button {
    winit::event::MouseButton::Left => LAUFEY_MOUSE_BUTTON_LEFT,
    winit::event::MouseButton::Right => LAUFEY_MOUSE_BUTTON_RIGHT,
    winit::event::MouseButton::Middle => LAUFEY_MOUSE_BUTTON_MIDDLE,
    winit::event::MouseButton::Back => LAUFEY_MOUSE_BUTTON_BACK,
    winit::event::MouseButton::Forward => LAUFEY_MOUSE_BUTTON_FORWARD,
    winit::event::MouseButton::Other(_) => -1,
  }
}

/// Dispatch a mouse click event to the registered handler.
/// Double-click interval (500ms is the standard across most platforms).
const DOUBLE_CLICK_INTERVAL: std::time::Duration =
  std::time::Duration::from_millis(500);

pub fn dispatch_mouse_click_event(
  handlers: &EventHandlers,
  ws: &WindowState,
  window_id: u32,
  button_state: winit::event::ElementState,
  button: winit::event::MouseButton,
  modifiers: winit::keyboard::ModifiersState,
) {
  let handler = handlers.mouse_click_handler.lock().unwrap();
  if let Some((cb, user_data)) = *handler {
    let state = match button_state {
      winit::event::ElementState::Pressed => LAUFEY_MOUSE_PRESSED,
      winit::event::ElementState::Released => LAUFEY_MOUSE_RELEASED,
    };
    let laufey_button = winit_button_to_laufey(button);
    if laufey_button < 0 {
      return;
    }
    let (x, y) = *ws.cursor_position.lock().unwrap();
    let mods = modifiers_to_laufey(modifiers);

    let click_count = if button_state == winit::event::ElementState::Pressed {
      let now = std::time::Instant::now();
      let mut last_time = ws.last_press_time.lock().unwrap();
      let mut last_btn = ws.last_press_button.lock().unwrap();
      let mut count = ws.click_count.lock().unwrap();

      if *last_btn == Some(button)
        && *count < 2
        && last_time
          .is_some_and(|t| now.duration_since(t) < DOUBLE_CLICK_INTERVAL)
      {
        *count = 2;
      } else if *count >= 2 || *last_btn != Some(button) {
        *count = 1;
      }
      *last_time = Some(now);
      *last_btn = Some(button);
      *count
    } else {
      *ws.click_count.lock().unwrap()
    };

    unsafe {
      cb(
        user_data as *mut c_void,
        window_id,
        state,
        laufey_button,
        x,
        y,
        mods,
        click_count,
      );
    }
  }
}

pub fn dispatch_mouse_move_event(
  handlers: &EventHandlers,
  window_id: u32,
  x: f64,
  y: f64,
  modifiers: winit::keyboard::ModifiersState,
) {
  let handler = handlers.mouse_move_handler.lock().unwrap();
  if let Some((cb, user_data)) = *handler {
    let mods = modifiers_to_laufey(modifiers);
    unsafe {
      cb(user_data as *mut c_void, window_id, x, y, mods);
    }
  }
}

pub fn dispatch_wheel_event(
  handlers: &EventHandlers,
  ws: &WindowState,
  window_id: u32,
  delta: winit::event::MouseScrollDelta,
  modifiers: winit::keyboard::ModifiersState,
) {
  let handler = handlers.wheel_handler.lock().unwrap();
  if let Some((cb, user_data)) = *handler {
    let (delta_x, delta_y, delta_mode) = match delta {
      winit::event::MouseScrollDelta::LineDelta(dx, dy) => {
        (dx as f64, dy as f64, LAUFEY_WHEEL_DELTA_LINE)
      }
      winit::event::MouseScrollDelta::PixelDelta(d) => {
        (d.x, d.y, LAUFEY_WHEEL_DELTA_PIXEL)
      }
    };
    let (x, y) = *ws.cursor_position.lock().unwrap();
    let mods = modifiers_to_laufey(modifiers);
    unsafe {
      cb(
        user_data as *mut c_void,
        window_id,
        delta_x,
        delta_y,
        x,
        y,
        mods,
        delta_mode,
      );
    }
  }
}

pub fn dispatch_cursor_enter_leave_event(
  handlers: &EventHandlers,
  ws: &WindowState,
  window_id: u32,
  entered: bool,
  modifiers: winit::keyboard::ModifiersState,
) {
  let handler = handlers.cursor_enter_leave_handler.lock().unwrap();
  if let Some((cb, user_data)) = *handler {
    let (x, y) = *ws.cursor_position.lock().unwrap();
    let mods = modifiers_to_laufey(modifiers);
    unsafe {
      cb(
        user_data as *mut c_void,
        window_id,
        if entered { 1 } else { 0 },
        x,
        y,
        mods,
      );
    }
  }
}

pub fn dispatch_focused_event(
  handlers: &EventHandlers,
  window_id: u32,
  focused: bool,
) {
  let handler = handlers.focused_handler.lock().unwrap();
  if let Some((cb, user_data)) = *handler {
    unsafe {
      cb(
        user_data as *mut c_void,
        window_id,
        if focused { 1 } else { 0 },
      );
    }
  }
}

pub fn dispatch_resize_event(
  handlers: &EventHandlers,
  window_id: u32,
  width: i32,
  height: i32,
) {
  let handler = handlers.resize_handler.lock().unwrap();
  if let Some((cb, user_data)) = *handler {
    unsafe {
      cb(user_data as *mut c_void, window_id, width, height);
    }
  }
}

pub fn dispatch_move_event(
  handlers: &EventHandlers,
  window_id: u32,
  x: i32,
  y: i32,
) {
  let handler = handlers.move_handler.lock().unwrap();
  if let Some((cb, user_data)) = *handler {
    unsafe {
      cb(user_data as *mut c_void, window_id, x, y);
    }
  }
}

pub fn dispatch_close_requested_event(
  handlers: &EventHandlers,
  window_id: u32,
) {
  let handler = handlers.close_requested_handler.lock().unwrap();
  if let Some((cb, user_data)) = *handler {
    unsafe {
      cb(user_data as *mut c_void, window_id);
    }
  }
}

// --- Runtime loading ---

pub fn find_runtime_library() -> Option<PathBuf> {
  if let Ok(path) = env::var("LAUFEY_RUNTIME_PATH") {
    let p = PathBuf::from(path);
    if p.exists() {
      return Some(p);
    }
  }

  let search_paths = [
    "./libruntime.dylib",
    "./libruntime.so",
    "./target/debug/libhello.dylib",
    "./target/release/libhello.dylib",
    "./target/debug/libhello.so",
    "./target/release/libhello.so",
  ];

  for path in &search_paths {
    let p = PathBuf::from(path);
    if p.exists() {
      return Some(p);
    }
  }

  None
}

pub fn load_and_start_runtime(api: LaufeyBackendApi) {
  let runtime_path = find_runtime_library();
  match runtime_path {
    Some(path) => {
      println!("Loading runtime from: {}", path.display());
      thread::spawn(move || unsafe {
        let lib = match Library::new(&path) {
          Ok(l) => l,
          Err(e) => {
            eprintln!("Failed to load runtime: {}", e);
            return;
          }
        };

        let init: Symbol<RuntimeInitFn> =
          match lib.get(b"laufey_runtime_init\0") {
            Ok(f) => f,
            Err(e) => {
              eprintln!("Failed to find laufey_runtime_init: {}", e);
              return;
            }
          };

        let start: Symbol<RuntimeStartFn> =
          match lib.get(b"laufey_runtime_start\0") {
            Ok(f) => f,
            Err(e) => {
              eprintln!("Failed to find laufey_runtime_start: {}", e);
              return;
            }
          };

        let result = init(&api);
        if result != 0 {
          eprintln!("Runtime init failed with code: {}", result);
          return;
        }

        println!("Runtime initialized, starting...");
        let result = start();
        if result != 0 {
          eprintln!("Runtime start failed with code: {}", result);
        }

        std::mem::forget(lib);
      });
    }
    None => {
      println!("No runtime library found. Set LAUFEY_RUNTIME_PATH or place libruntime in current directory.");
      println!("Starting without runtime integration...");
    }
  }
}
