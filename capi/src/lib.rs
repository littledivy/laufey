// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#![allow(clippy::type_complexity)]

use std::collections::HashMap;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::future::Future;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, OnceLock};

use tokio::sync::Notify;

#[allow(non_upper_case_globals)]
#[allow(non_camel_case_types)]
#[allow(non_snake_case)]
#[allow(dead_code)]
mod ffi {
  include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}

mod keyboard;
pub use keyboard::*;

mod mouse;
pub use mouse::*;

/// Version of this laufey crate. Used by downstream consumers (e.g. the Deno CLI)
/// to locate matching prebuilt backend binaries in GitHub releases
/// (`github.com/denoland/laufey/releases/tag/v{VERSION}`).
pub const VERSION: &str = env!("CARGO_PKG_VERSION");

pub const LAUFEY_API_VERSION: u32 = 25;

/// Creation-time window style flags for [`Window::new_with_options`].
/// Mirror the `LAUFEY_WINDOW_FLAG_*` constants in `laufey.h`.
pub const LAUFEY_WINDOW_FLAG_FRAMELESS: u32 = 1 << 0;
pub const LAUFEY_WINDOW_FLAG_NO_ACTIVATE: u32 = 1 << 1;
pub const LAUFEY_WINDOW_FLAG_TRANSPARENT_TITLEBAR: u32 = 1 << 2;

pub const LAUFEY_WINDOW_HANDLE_UNKNOWN: i32 = 0;
pub const LAUFEY_WINDOW_HANDLE_APPKIT: i32 = 1;
pub const LAUFEY_WINDOW_HANDLE_WIN32: i32 = 2;
pub const LAUFEY_WINDOW_HANDLE_X11: i32 = 3;
pub const LAUFEY_WINDOW_HANDLE_WAYLAND: i32 = 4;
pub type LaufeyValue = ffi::laufey_value_t;
pub type LaufeyBackendApi = ffi::laufey_backend_api_t;

unsafe impl Send for LaufeyBackendApi {}
unsafe impl Sync for LaufeyBackendApi {}

static BACKEND_API: OnceLock<&'static LaufeyBackendApi> = OnceLock::new();
static SHUTDOWN_FLAG: AtomicBool = AtomicBool::new(false);
static BINDINGS: OnceLock<
  Mutex<HashMap<u32, HashMap<String, BindingHandler>>>,
> = OnceLock::new();
static JS_CALL_NOTIFY: OnceLock<Notify> = OnceLock::new();
static MENU_CLICK_HANDLERS: OnceLock<
  Mutex<HashMap<u32, Box<dyn Fn(&str) + Send + Sync>>>,
> = OnceLock::new();
static CONTEXT_MENU_HANDLERS: OnceLock<
  Mutex<HashMap<u32, Box<dyn Fn(&str) + Send + Sync>>>,
> = OnceLock::new();
static DOCK_MENU_HANDLER: OnceLock<
  Mutex<Option<Box<dyn Fn(&str) + Send + Sync>>>,
> = OnceLock::new();
static DOCK_REOPEN_HANDLER: OnceLock<
  Mutex<Option<Box<dyn Fn(bool) + Send + Sync>>>,
> = OnceLock::new();
static TRAY_MENU_HANDLERS: OnceLock<
  Mutex<HashMap<u32, Box<dyn Fn(&str) + Send + Sync>>>,
> = OnceLock::new();
static TRAY_CLICK_HANDLERS: OnceLock<
  Mutex<HashMap<u32, Box<dyn Fn() + Send + Sync>>>,
> = OnceLock::new();
static TRAY_DBLCLICK_HANDLERS: OnceLock<
  Mutex<HashMap<u32, Box<dyn Fn() + Send + Sync>>>,
> = OnceLock::new();
static NOTIFICATION_HANDLERS: OnceLock<
  Mutex<HashMap<u32, Arc<dyn Fn(NotificationEvent) + Send + Sync>>>,
> = OnceLock::new();

enum BindingHandler {
  Sync(Box<dyn Fn(JsCall) + Send + Sync>),
  Async(
    Box<
      dyn Fn(JsCall) -> std::pin::Pin<Box<dyn Future<Output = ()> + Send>>
        + Send
        + Sync,
    >,
  ),
}

fn api() -> &'static LaufeyBackendApi {
  BACKEND_API.get().expect("Backend API not initialized")
}

fn bindings() -> &'static Mutex<HashMap<u32, HashMap<String, BindingHandler>>> {
  BINDINGS.get_or_init(|| Mutex::new(HashMap::new()))
}

fn js_call_notify() -> &'static Notify {
  JS_CALL_NOTIFY.get_or_init(Notify::new)
}

/// # Safety
/// `api` must be either null or a valid pointer to a `LaufeyBackendApi` with
/// static lifetime.
pub unsafe fn init_api(api: *const LaufeyBackendApi) -> c_int {
  if api.is_null() {
    return -1;
  }
  let api_ref: &'static LaufeyBackendApi = unsafe { &*api };
  if api_ref.version != LAUFEY_API_VERSION {
    eprintln!(
      "API version mismatch: expected {}, got {}",
      LAUFEY_API_VERSION, api_ref.version
    );
    return -2;
  }
  match BACKEND_API.set(api_ref) {
    Ok(_) => 0,
    Err(_) => -3,
  }
}

pub fn shutdown() {
  SHUTDOWN_FLAG.store(true, Ordering::SeqCst);
  if let Some(notify) = JS_CALL_NOTIFY.get() {
    notify.notify_one();
  }
}

pub fn should_shutdown() -> bool {
  SHUTDOWN_FLAG.load(Ordering::SeqCst)
}

#[derive(Clone)]
pub enum Value {
  Null,
  Bool(bool),
  Int(i32),
  Double(f64),
  String(String),
  List(Vec<Value>),
  Dict(HashMap<String, Value>),
  Binary(Vec<u8>),
}

impl Value {
  /// # Safety
  /// `ptr` must be null or a valid pointer to a `LaufeyValue` produced by the
  /// backend API.
  pub unsafe fn from_raw(ptr: *mut LaufeyValue) -> Option<Self> {
    if ptr.is_null() {
      return None;
    }
    let api = api();

    if api.value_is_null.map(|f| f(ptr)).unwrap_or(false) {
      return Some(Value::Null);
    }
    if api.value_is_bool.map(|f| f(ptr)).unwrap_or(false) {
      return Some(Value::Bool(
        api.value_get_bool.map(|f| f(ptr)).unwrap_or(false),
      ));
    }
    if api.value_is_int.map(|f| f(ptr)).unwrap_or(false) {
      return Some(Value::Int(api.value_get_int.map(|f| f(ptr)).unwrap_or(0)));
    }
    if api.value_is_double.map(|f| f(ptr)).unwrap_or(false) {
      return Some(Value::Double(
        api.value_get_double.map(|f| f(ptr)).unwrap_or(0.0),
      ));
    }
    if api.value_is_string.map(|f| f(ptr)).unwrap_or(false) {
      let mut len: usize = 0;
      if let Some(get_str) = api.value_get_string {
        let c_str = get_str(ptr, &mut len);
        if !c_str.is_null() {
          let s = CStr::from_ptr(c_str).to_string_lossy().into_owned();
          if let Some(free_str) = api.value_free_string {
            free_str(c_str);
          }
          return Some(Value::String(s));
        }
      }
      return Some(Value::String(String::new()));
    }
    if api.value_is_list.map(|f| f(ptr)).unwrap_or(false) {
      let size = api.value_list_size.map(|f| f(ptr)).unwrap_or(0);
      let mut list = Vec::with_capacity(size);
      if let Some(get_item) = api.value_list_get {
        for i in 0..size {
          let item = get_item(ptr, i);
          if let Some(v) = Value::from_raw(item) {
            list.push(v);
          }
        }
      }
      return Some(Value::List(list));
    }
    if api.value_is_dict.map(|f| f(ptr)).unwrap_or(false) {
      let mut dict = HashMap::new();
      let mut count: usize = 0;
      if let Some(get_keys) = api.value_dict_keys {
        let keys = get_keys(ptr, &mut count);
        if !keys.is_null() {
          for i in 0..count {
            let key_ptr = *keys.add(i);
            if !key_ptr.is_null() {
              let key = CStr::from_ptr(key_ptr).to_string_lossy().into_owned();
              if let Some(get_val) = api.value_dict_get {
                let c_key = CString::new(key.as_str()).unwrap();
                let val = get_val(ptr, c_key.as_ptr());
                if let Some(v) = Value::from_raw(val) {
                  dict.insert(key, v);
                }
              }
            }
          }
          if let Some(free_keys) = api.value_free_keys {
            free_keys(keys, count);
          }
        }
      }
      return Some(Value::Dict(dict));
    }
    if api.value_is_binary.map(|f| f(ptr)).unwrap_or(false) {
      let mut len: usize = 0;
      if let Some(get_bin) = api.value_get_binary {
        let data = get_bin(ptr, &mut len);
        if !data.is_null() && len > 0 {
          let slice = std::slice::from_raw_parts(data as *const u8, len);
          return Some(Value::Binary(slice.to_vec()));
        }
      }
      return Some(Value::Binary(Vec::new()));
    }

    Some(Value::Null)
  }

  pub fn to_raw(&self) -> *mut LaufeyValue {
    let api = api();
    let bd = api.backend_data;

    unsafe {
      match self {
        Value::Null => api
          .value_null
          .map(|f| f(bd))
          .unwrap_or(std::ptr::null_mut()),
        Value::Bool(v) => api
          .value_bool
          .map(|f| f(bd, *v))
          .unwrap_or(std::ptr::null_mut()),
        Value::Int(v) => api
          .value_int
          .map(|f| f(bd, *v))
          .unwrap_or(std::ptr::null_mut()),
        Value::Double(v) => api
          .value_double
          .map(|f| f(bd, *v))
          .unwrap_or(std::ptr::null_mut()),
        Value::String(s) => {
          let c_str = CString::new(s.as_str()).unwrap();
          api
            .value_string
            .map(|f| f(bd, c_str.as_ptr()))
            .unwrap_or(std::ptr::null_mut())
        }
        Value::List(items) => {
          let list = api
            .value_list
            .map(|f| f(bd))
            .unwrap_or(std::ptr::null_mut());
          if !list.is_null() {
            if let Some(append) = api.value_list_append {
              for item in items {
                let raw = item.to_raw();
                append(list, raw);
              }
            }
          }
          list
        }
        Value::Dict(map) => {
          let dict = api
            .value_dict
            .map(|f| f(bd))
            .unwrap_or(std::ptr::null_mut());
          if !dict.is_null() {
            if let Some(set) = api.value_dict_set {
              for (k, v) in map {
                let c_key = CString::new(k.as_str()).unwrap();
                let raw = v.to_raw();
                set(dict, c_key.as_ptr(), raw);
              }
            }
          }
          dict
        }
        Value::Binary(data) => api
          .value_binary
          .map(|f| f(bd, data.as_ptr() as *const c_void, data.len()))
          .unwrap_or(std::ptr::null_mut()),
      }
    }
  }

  pub fn as_string(&self) -> Option<&str> {
    match self {
      Value::String(s) => Some(s.as_str()),
      _ => None,
    }
  }

  pub fn as_int(&self) -> Option<i32> {
    match self {
      Value::Int(i) => Some(*i),
      _ => None,
    }
  }

  pub fn as_bool(&self) -> Option<bool> {
    match self {
      Value::Bool(b) => Some(*b),
      _ => None,
    }
  }

  pub fn as_list(&self) -> Option<&Vec<Value>> {
    match self {
      Value::List(l) => Some(l),
      _ => None,
    }
  }

  pub fn as_dict(&self) -> Option<&HashMap<String, Value>> {
    match self {
      Value::Dict(d) => Some(d),
      _ => None,
    }
  }
}

pub struct JsCall {
  pub window_id: u32,
  pub call_id: u64,
  pub method: String,
  pub args: Vec<Value>,
}

impl JsCall {
  pub fn resolve(self, value: Value) {
    let api = api();
    if let Some(respond) = api.js_call_respond {
      let raw = value.to_raw();
      unsafe {
        respond(api.backend_data, self.call_id, raw, std::ptr::null_mut())
      };
    }
  }

  pub fn reject(self, error: Value) {
    let api = api();
    if let Some(respond) = api.js_call_respond {
      let raw = error.to_raw();
      unsafe {
        respond(api.backend_data, self.call_id, std::ptr::null_mut(), raw)
      };
    }
  }
}

unsafe extern "C" fn js_call_handler(
  _user_data: *mut c_void,
  window_id: u32,
  call_id: u64,
  method_path: *const c_char,
  args: *mut LaufeyValue,
) {
  let method = if method_path.is_null() {
    String::new()
  } else {
    CStr::from_ptr(method_path).to_string_lossy().into_owned()
  };

  let args_vec = if args.is_null() {
    Vec::new()
  } else {
    match Value::from_raw(args) {
      Some(Value::List(l)) => l,
      _ => Vec::new(),
    }
  };

  let call = JsCall {
    window_id,
    call_id,
    method: method.clone(),
    args: args_vec,
  };

  let bindings = bindings().lock().unwrap();
  if let Some(window_bindings) = bindings.get(&window_id) {
    if let Some(handler) = window_bindings.get(&method) {
      match handler {
        BindingHandler::Sync(f) => f(call),
        BindingHandler::Async(f) => {
          let fut = f(call);
          tokio::spawn(fut);
        }
      }
      return;
    }
  }
  drop(bindings);
  call.reject(Value::String(format!("No binding for '{}'", method)));
}

fn register_js_handler() {
  let api = api();
  if let Some(set_handler) = api.set_js_call_handler {
    unsafe {
      set_handler(
        api.backend_data,
        Some(js_call_handler),
        std::ptr::null_mut(),
      );
    }
  }
}

unsafe extern "C" fn js_call_notify_callback(_user_data: *mut c_void) {
  js_call_notify().notify_one();
}

fn register_js_notify() {
  let api = api();
  if let Some(set_notify) = api.set_js_call_notify {
    unsafe {
      set_notify(
        api.backend_data,
        Some(js_call_notify_callback),
        std::ptr::null_mut(),
      );
    }
  }
}

fn ensure_js_handler() {
  static HANDLER_REGISTERED: AtomicBool = AtomicBool::new(false);
  if !HANDLER_REGISTERED.swap(true, Ordering::SeqCst) {
    register_js_handler();
    register_js_notify();
  }
}

fn poll_js_calls() {
  let api = api();
  if let Some(f) = api.poll_js_calls {
    unsafe { f(api.backend_data) };
  }
}

/// Async event loop that dispatches JS calls as they arrive.
/// Blocks until `should_shutdown()` returns true.
pub async fn run() {
  ensure_js_handler();
  loop {
    js_call_notify().notified().await;
    poll_js_calls();
    if should_shutdown() {
      break;
    }
  }
}

pub fn quit() {
  let api = api();
  if let Some(f) = api.quit {
    unsafe { f(api.backend_data) };
  }
}

// --- Window ---

pub struct Window {
  id: u32,
}

/// Creation-time window style options. Properties that must be decided when
/// the OS window is constructed (frameless chrome, non-activating panel
/// behavior). Post-creation properties (size, position, resizable,
/// always-on-top) are set through their respective `Window` setters.
#[derive(Clone, Copy, Debug, Default)]
pub struct WindowOptions {
  /// Remove the title bar and standard window chrome.
  pub frameless: bool,
  /// Float above normal windows as a utility "panel" and do not activate
  /// the app / steal key focus when shown. Combined with `frameless`, this
  /// is the configuration used for tray / menu-bar popovers.
  pub no_activate: bool,
  /// Keep the standard frame and traffic-light buttons, but make the title
  /// bar transparent and let the web content extend under it (Electron
  /// `titleBarStyle: 'hidden'`). macOS only; ignored elsewhere.
  pub transparent_titlebar: bool,
}

impl WindowOptions {
  fn to_flags(self) -> u32 {
    let mut flags = 0;
    if self.frameless {
      flags |= LAUFEY_WINDOW_FLAG_FRAMELESS;
    }
    if self.no_activate {
      flags |= LAUFEY_WINDOW_FLAG_NO_ACTIVATE;
    }
    if self.transparent_titlebar {
      flags |= LAUFEY_WINDOW_FLAG_TRANSPARENT_TITLEBAR;
    }
    flags
  }
}

impl Window {
  pub fn new(width: i32, height: i32) -> Self {
    Self::new_with_options(width, height, WindowOptions::default())
  }

  /// Create a window with creation-time style options. Falls back to a plain
  /// window (ignoring the options) on backends older than API version 25.
  pub fn new_with_options(
    width: i32,
    height: i32,
    options: WindowOptions,
  ) -> Self {
    let api = api();
    let flags = options.to_flags();
    let id = if let (Some(f), true) = (api.create_window_ex, flags != 0) {
      unsafe { f(api.backend_data, flags) }
    } else if let Some(f) = api.create_window {
      unsafe { f(api.backend_data) }
    } else {
      0
    };
    let win = Window { id };
    if let Some(f) = api.set_window_size {
      unsafe { f(api.backend_data, id, width, height) };
    }
    win
  }

  /// Wrap an existing window by its ID (does not create a new OS window).
  pub fn from_id(id: u32) -> Self {
    Window { id }
  }

  pub fn id(&self) -> u32 {
    self.id
  }

  pub fn title(self, title: &str) -> Self {
    self.set_title(title);
    self
  }

  pub fn set_title(&self, title: &str) {
    let api = api();
    if let Some(f) = api.set_title {
      let c_title = CString::new(title).expect("Invalid title");
      unsafe { f(api.backend_data, self.id, c_title.as_ptr()) };
    }
  }

  pub fn load(self, path: &str) -> Self {
    self.navigate(path);
    self
  }

  pub fn navigate(&self, url: &str) {
    let api = api();
    if let Some(f) = api.navigate {
      let c_url = CString::new(url).expect("Invalid URL");
      unsafe { f(api.backend_data, self.id, c_url.as_ptr()) };
    }
  }

  pub fn size(self, width: i32, height: i32) -> Self {
    self.set_size(width, height);
    self
  }

  pub fn set_size(&self, width: i32, height: i32) {
    let api = api();
    if let Some(f) = api.set_window_size {
      unsafe { f(api.backend_data, self.id, width, height) };
    }
  }

  pub fn get_size(&self) -> (i32, i32) {
    let api = api();
    let mut width: c_int = 0;
    let mut height: c_int = 0;
    if let Some(f) = api.get_window_size {
      unsafe { f(api.backend_data, self.id, &mut width, &mut height) };
    }
    (width, height)
  }

  pub fn position(self, x: i32, y: i32) -> Self {
    self.set_position(x, y);
    self
  }

  pub fn set_position(&self, x: i32, y: i32) {
    let api = api();
    if let Some(f) = api.set_window_position {
      unsafe { f(api.backend_data, self.id, x, y) };
    }
  }

  pub fn get_position(&self) -> (i32, i32) {
    let api = api();
    let mut x: c_int = 0;
    let mut y: c_int = 0;
    if let Some(f) = api.get_window_position {
      unsafe { f(api.backend_data, self.id, &mut x, &mut y) };
    }
    (x, y)
  }

  pub fn resizable(self, resizable: bool) -> Self {
    self.set_resizable(resizable);
    self
  }

  pub fn set_resizable(&self, resizable: bool) {
    let api = api();
    if let Some(f) = api.set_resizable {
      unsafe { f(api.backend_data, self.id, resizable) };
    }
  }

  pub fn get_resizable(&self) -> bool {
    let api = api();
    if let Some(f) = api.is_resizable {
      unsafe { f(api.backend_data, self.id) }
    } else {
      true
    }
  }

  pub fn always_on_top(self, always_on_top: bool) -> Self {
    self.set_always_on_top(always_on_top);
    self
  }

  pub fn set_always_on_top(&self, always_on_top: bool) {
    let api = api();
    if let Some(f) = api.set_always_on_top {
      unsafe { f(api.backend_data, self.id, always_on_top) };
    }
  }

  pub fn get_always_on_top(&self) -> bool {
    let api = api();
    if let Some(f) = api.is_always_on_top {
      unsafe { f(api.backend_data, self.id) }
    } else {
      false
    }
  }

  pub fn get_visible(&self) -> bool {
    let api = api();
    if let Some(f) = api.is_visible {
      unsafe { f(api.backend_data, self.id) }
    } else {
      true
    }
  }

  pub fn show(&self) {
    let api = api();
    if let Some(f) = api.show {
      unsafe { f(api.backend_data, self.id) };
    }
  }

  pub fn hide(&self) {
    let api = api();
    if let Some(f) = api.hide {
      unsafe { f(api.backend_data, self.id) };
    }
  }

  pub fn focus(&self) {
    let api = api();
    if let Some(f) = api.focus {
      unsafe { f(api.backend_data, self.id) };
    }
  }

  pub fn close(&self) {
    let api = api();
    if let Some(f) = api.close_window {
      unsafe { f(api.backend_data, self.id) };
    }
  }

  pub fn execute_js<F>(&self, script: &str, callback: Option<F>)
  where
    F: FnOnce(Result<Value, Value>) + Send + 'static,
  {
    let api = api();
    if let Some(f) = api.execute_js {
      let c_script = CString::new(script).expect("Invalid script");

      match callback {
        Some(cb_fn) => {
          unsafe extern "C" fn trampoline(
            result: *mut LaufeyValue,
            error: *mut LaufeyValue,
            user_data: *mut c_void,
          ) {
            let cb = Box::from_raw(
              user_data as *mut Box<dyn FnOnce(Result<Value, Value>) + Send>,
            );
            if !error.is_null() {
              if let Some(e) = Value::from_raw(error) {
                cb(Err(e));
                return;
              }
            }
            let val = Value::from_raw(result).unwrap_or(Value::Null);
            cb(Ok(val));
          }

          let cb: Box<Box<dyn FnOnce(Result<Value, Value>) + Send>> =
            Box::new(Box::new(cb_fn));
          let user_data = Box::into_raw(cb) as *mut c_void;

          unsafe {
            f(
              api.backend_data,
              self.id,
              c_script.as_ptr(),
              Some(trampoline),
              user_data,
            )
          };
        }
        None => {
          unsafe {
            f(
              api.backend_data,
              self.id,
              c_script.as_ptr(),
              None,
              std::ptr::null_mut(),
            )
          };
        }
      }
    }
  }

  pub fn get_window_handle(&self) -> *mut c_void {
    let api = api();
    if let Some(f) = api.get_window_handle {
      unsafe { f(api.backend_data, self.id) }
    } else {
      std::ptr::null_mut()
    }
  }

  pub fn get_display_handle(&self) -> *mut c_void {
    let api = api();
    if let Some(f) = api.get_display_handle {
      unsafe { f(api.backend_data, self.id) }
    } else {
      std::ptr::null_mut()
    }
  }

  pub fn get_window_handle_type(&self) -> i32 {
    let api = api();
    if let Some(f) = api.get_window_handle_type {
      unsafe { f(api.backend_data, self.id) }
    } else {
      LAUFEY_WINDOW_HANDLE_UNKNOWN
    }
  }

  pub fn on_keyboard_event<F>(self, handler: F) -> Self
  where
    F: Fn(KeyboardEvent) + Send + Sync + 'static,
  {
    on_keyboard_event(self.id, handler);
    self
  }

  pub fn on_mouse_click<F>(self, handler: F) -> Self
  where
    F: Fn(MouseClickEvent) + Send + Sync + 'static,
  {
    on_mouse_click(self.id, handler);
    self
  }

  pub fn on_mouse_move<F>(self, handler: F) -> Self
  where
    F: Fn(MouseMoveEvent) + Send + Sync + 'static,
  {
    on_mouse_move(self.id, handler);
    self
  }

  pub fn on_wheel<F>(self, handler: F) -> Self
  where
    F: Fn(WheelEvent) + Send + Sync + 'static,
  {
    on_wheel(self.id, handler);
    self
  }

  pub fn on_cursor_enter_leave<F>(self, handler: F) -> Self
  where
    F: Fn(CursorEnterLeaveEvent) + Send + Sync + 'static,
  {
    on_cursor_enter_leave(self.id, handler);
    self
  }

  pub fn on_focused<F>(self, handler: F) -> Self
  where
    F: Fn(FocusedEvent) + Send + Sync + 'static,
  {
    on_focused(self.id, handler);
    self
  }

  pub fn on_resize<F>(self, handler: F) -> Self
  where
    F: Fn(ResizeEvent) + Send + Sync + 'static,
  {
    on_resize(self.id, handler);
    self
  }

  pub fn on_move<F>(self, handler: F) -> Self
  where
    F: Fn(MoveEvent) + Send + Sync + 'static,
  {
    on_move(self.id, handler);
    self
  }

  pub fn on_close_requested<F>(self, handler: F) -> Self
  where
    F: Fn(CloseRequestedEvent) + Send + Sync + 'static,
  {
    on_close_requested(self.id, handler);
    self
  }

  pub fn add_binding<F>(&self, name: &str, handler: F)
  where
    F: Fn(JsCall) + Send + Sync + 'static,
  {
    ensure_js_handler();
    bindings()
      .lock()
      .unwrap()
      .entry(self.id)
      .or_default()
      .insert(name.to_string(), BindingHandler::Sync(Box::new(handler)));
  }

  pub fn add_binding_async<F, Fut>(&self, name: &str, handler: F)
  where
    F: Fn(JsCall) -> Fut + Send + Sync + 'static,
    Fut: Future<Output = ()> + Send + 'static,
  {
    ensure_js_handler();
    bindings()
      .lock()
      .unwrap()
      .entry(self.id)
      .or_default()
      .insert(
        name.to_string(),
        BindingHandler::Async(Box::new(move |call| Box::pin(handler(call)))),
      );
  }

  pub fn bind<F>(self, name: &str, handler: F) -> Self
  where
    F: Fn(JsCall) + Send + Sync + 'static,
  {
    self.add_binding(name, handler);
    self
  }

  pub fn bind_async<F, Fut>(self, name: &str, handler: F) -> Self
  where
    F: Fn(JsCall) -> Fut + Send + Sync + 'static,
    Fut: Future<Output = ()> + Send + 'static,
  {
    self.add_binding_async(name, handler);
    self
  }

  pub fn unbind(&self, name: &str) {
    let mut bindings = bindings().lock().unwrap();
    if let Some(window_bindings) = bindings.get_mut(&self.id) {
      window_bindings.remove(name);
    }
  }

  /// Set the application menu for this window.
  /// On macOS, the menu is applied to the global menu bar and swapped when this window gains focus.
  /// On Windows/Linux, the menu is attached directly to this window.
  /// `on_click` is called with the `id` of the clicked menu item.
  pub fn set_menu<F>(&self, template: &[MenuItem], on_click: F)
  where
    F: Fn(&str) + Send + Sync + 'static,
  {
    let value = Value::List(template.iter().map(|i| i.to_value()).collect());

    {
      let mut handlers = menu_click_handlers().lock().unwrap();
      handlers.insert(self.id, Box::new(on_click));
    }

    let api = api();
    if let Some(f) = api.set_application_menu {
      let raw = value.to_raw();
      unsafe {
        f(
          api.backend_data,
          self.id,
          raw,
          Some(menu_click_callback),
          std::ptr::null_mut(),
        );
      }
    }
  }

  /// Show a context menu at the given position (in window coordinates).
  /// Uses the same `MenuItem` template as `set_menu`.
  /// `on_click` is called with the `id` of the clicked menu item.
  pub fn show_context_menu<F>(
    &self,
    x: i32,
    y: i32,
    template: &[MenuItem],
    on_click: F,
  ) where
    F: Fn(&str) + Send + Sync + 'static,
  {
    let value = Value::List(template.iter().map(|i| i.to_value()).collect());

    {
      let mut handlers = context_menu_handlers().lock().unwrap();
      handlers.insert(self.id, Box::new(on_click));
    }

    let api = api();
    if let Some(f) = api.show_context_menu {
      let raw = value.to_raw();
      unsafe {
        f(
          api.backend_data,
          self.id,
          x,
          y,
          raw,
          Some(context_menu_click_callback),
          std::ptr::null_mut(),
        );
      }
    }
  }

  /// Open the DevTools inspector for this window.
  pub fn open_devtools(&self) {
    let api = api();
    if let Some(f) = api.open_devtools {
      unsafe { f(api.backend_data, self.id) };
    }
  }

  /// Show an alert dialog. Blocks until dismissed.
  pub fn alert(&self, title: &str, message: &str) {
    show_dialog_blocking(self.id, LAUFEY_DIALOG_ALERT, title, message, "");
  }

  /// Show a confirm dialog. Returns `true` if OK was pressed. Blocks
  /// until dismissed; while the modal is up the platform's event loop is
  /// pumped so other LAUFEY windows continue to render and respond.
  pub fn confirm(&self, title: &str, message: &str) -> bool {
    let (confirmed, _) =
      show_dialog_blocking(self.id, LAUFEY_DIALOG_CONFIRM, title, message, "");
    confirmed
  }

  /// Show a prompt dialog with a text input. Returns `Some(text)` if OK
  /// was pressed, `None` if cancelled. Blocking semantics as `confirm`.
  pub fn prompt(
    &self,
    title: &str,
    message: &str,
    default_value: &str,
  ) -> Option<String> {
    let (confirmed, input) = show_dialog_blocking(
      self.id,
      LAUFEY_DIALOG_PROMPT,
      title,
      message,
      default_value,
    );
    if confirmed {
      input
    } else {
      None
    }
  }
}

/// Shared dialog implementation. `window_id == 0` ⇒ app-wide modal.
/// Returns `(confirmed, input_value)`. `input_value` is `Some` only when
/// the dialog was a prompt and the user confirmed.
fn show_dialog_blocking(
  window_id: u32,
  dialog_type: i32,
  title: &str,
  message: &str,
  default_value: &str,
) -> (bool, Option<String>) {
  let api = api();
  let Some(f) = api.show_dialog else {
    return (false, None);
  };
  let c_title = CString::new(title).expect("Invalid title");
  let c_message = CString::new(message).expect("Invalid message");
  let c_default = CString::new(default_value).expect("Invalid default value");
  let mut out_input: *mut c_char = std::ptr::null_mut();
  let want_input = dialog_type == LAUFEY_DIALOG_PROMPT;
  // SAFETY: All pointers are valid for the duration of the call. The
  // backend may write a heap-allocated string into `out_input`; we hand it
  // back to the backend's deallocator below.
  let confirmed = unsafe {
    f(
      api.backend_data,
      window_id,
      dialog_type as c_int,
      c_title.as_ptr(),
      c_message.as_ptr(),
      c_default.as_ptr(),
      if want_input {
        &mut out_input as *mut *mut c_char
      } else {
        std::ptr::null_mut()
      },
    )
  } != 0;
  let input = if !out_input.is_null() {
    // SAFETY: backend just wrote a NUL-terminated UTF-8 string here.
    let s = unsafe { CStr::from_ptr(out_input) }
      .to_string_lossy()
      .into_owned();
    if let Some(free) = api.string_free {
      // SAFETY: pointer originated from `f`, freed via the matching
      // backend allocator.
      unsafe { free(api.backend_data, out_input) };
    }
    Some(s)
  } else {
    None
  };
  (confirmed, input)
}

/// A menu item in an application menu template.
#[derive(Clone, Debug)]
pub enum MenuItem {
  /// A regular menu item with label and optional properties.
  Item {
    label: String,
    id: Option<String>,
    accelerator: Option<String>,
    enabled: bool,
  },
  /// A submenu containing child items.
  Submenu { label: String, items: Vec<MenuItem> },
  /// A separator line.
  Separator,
  /// A standard role-based item (quit, copy, paste, etc.)
  Role { role: String },
}

impl MenuItem {
  fn to_value(&self) -> Value {
    match self {
      MenuItem::Item {
        label,
        id,
        accelerator,
        enabled,
      } => {
        let mut dict = HashMap::new();
        dict.insert("label".to_string(), Value::String(label.clone()));
        if let Some(id) = id {
          dict.insert("id".to_string(), Value::String(id.clone()));
        }
        if let Some(accel) = accelerator {
          dict.insert("accelerator".to_string(), Value::String(accel.clone()));
        }
        if !enabled {
          dict.insert("enabled".to_string(), Value::Bool(false));
        }
        Value::Dict(dict)
      }
      MenuItem::Submenu { label, items } => {
        let mut dict = HashMap::new();
        dict.insert("label".to_string(), Value::String(label.clone()));
        dict.insert(
          "submenu".to_string(),
          Value::List(items.iter().map(|i| i.to_value()).collect()),
        );
        Value::Dict(dict)
      }
      MenuItem::Separator => {
        let mut dict = HashMap::new();
        dict.insert("type".to_string(), Value::String("separator".to_string()));
        Value::Dict(dict)
      }
      MenuItem::Role { role } => {
        let mut dict = HashMap::new();
        dict.insert("role".to_string(), Value::String(role.clone()));
        Value::Dict(dict)
      }
    }
  }
}

unsafe extern "C" fn menu_click_callback(
  _user_data: *mut c_void,
  window_id: u32,
  item_id: *const c_char,
) {
  if item_id.is_null() {
    return;
  }
  let id = CStr::from_ptr(item_id).to_string_lossy();
  let handlers = menu_click_handlers().lock().unwrap();
  if let Some(handler) = handlers.get(&window_id) {
    handler(&id);
  }
}

fn menu_click_handlers(
) -> &'static Mutex<HashMap<u32, Box<dyn Fn(&str) + Send + Sync>>> {
  MENU_CLICK_HANDLERS.get_or_init(|| Mutex::new(HashMap::new()))
}

unsafe extern "C" fn context_menu_click_callback(
  _user_data: *mut c_void,
  window_id: u32,
  item_id: *const c_char,
) {
  if item_id.is_null() {
    return;
  }
  let id = CStr::from_ptr(item_id).to_string_lossy();
  let handlers = context_menu_handlers().lock().unwrap();
  if let Some(handler) = handlers.get(&window_id) {
    handler(&id);
  }
}

fn context_menu_handlers(
) -> &'static Mutex<HashMap<u32, Box<dyn Fn(&str) + Send + Sync>>> {
  CONTEXT_MENU_HANDLERS.get_or_init(|| Mutex::new(HashMap::new()))
}

// --- Dock / taskbar ---

/// How urgently to request the user's attention when calling [`bounce_dock`].
///
/// On macOS, `Informational` triggers a single bounce and `Critical` bounces
/// continuously until the app is focused. Behavior on Windows/Linux is the
/// closest native analog (`FlashWindowEx` / urgency hint).
#[derive(Clone, Copy, Debug)]
pub enum DockBounceType {
  Informational,
  Critical,
}

fn dock_menu_handler() -> &'static Mutex<Option<Box<dyn Fn(&str) + Send + Sync>>>
{
  DOCK_MENU_HANDLER.get_or_init(|| Mutex::new(None))
}

fn dock_reopen_handler(
) -> &'static Mutex<Option<Box<dyn Fn(bool) + Send + Sync>>> {
  DOCK_REOPEN_HANDLER.get_or_init(|| Mutex::new(None))
}

unsafe extern "C" fn dock_menu_click_callback(
  _user_data: *mut c_void,
  _window_id: u32,
  item_id: *const c_char,
) {
  if item_id.is_null() {
    return;
  }
  let id = CStr::from_ptr(item_id).to_string_lossy();
  if let Some(handler) = dock_menu_handler().lock().unwrap().as_ref() {
    handler(&id);
  }
}

unsafe extern "C" fn dock_reopen_callback(
  _user_data: *mut c_void,
  has_visible_windows: bool,
) {
  if let Some(handler) = dock_reopen_handler().lock().unwrap().as_ref() {
    handler(has_visible_windows);
  }
}

/// Set a short text badge on the app's dock icon (macOS) or taskbar icon
/// (Windows), or prefix the focused window's title with `"(text) "` (Linux).
/// Pass `None` or an empty string to clear the badge.
pub fn set_dock_badge(text: Option<&str>) {
  let api = api();
  if let Some(f) = api.set_dock_badge {
    match text {
      Some(t) if !t.is_empty() => {
        let c_text = CString::new(t).expect("Invalid badge text");
        unsafe { f(api.backend_data, c_text.as_ptr()) };
      }
      _ => unsafe { f(api.backend_data, std::ptr::null()) },
    }
  }
}

/// Bounce the dock icon (macOS), flash the focused window's taskbar button
/// (Windows), or set the urgency hint on the focused window (Linux).
pub fn bounce_dock(kind: DockBounceType) {
  let api = api();
  if let Some(f) = api.bounce_dock {
    let ty = match kind {
      DockBounceType::Informational => {
        ffi::LAUFEY_DOCK_BOUNCE_INFORMATIONAL as c_int
      }
      DockBounceType::Critical => ffi::LAUFEY_DOCK_BOUNCE_CRITICAL as c_int,
    };
    unsafe { f(api.backend_data, ty) };
  }
}

/// Set a custom right-click menu on the app's dock icon (macOS only).
/// `on_click` is called with the `id` of the clicked item.
/// Windows and Linux: no-op.
pub fn set_dock_menu<F>(template: &[MenuItem], on_click: F)
where
  F: Fn(&str) + Send + Sync + 'static,
{
  let value = Value::List(template.iter().map(|i| i.to_value()).collect());

  {
    let mut handler = dock_menu_handler().lock().unwrap();
    *handler = Some(Box::new(on_click));
  }

  let api = api();
  if let Some(f) = api.set_dock_menu {
    let raw = value.to_raw();
    unsafe {
      f(
        api.backend_data,
        raw,
        Some(dock_menu_click_callback),
        std::ptr::null_mut(),
      );
    }
  }
}

/// Remove the custom dock menu set by [`set_dock_menu`] (macOS only).
pub fn clear_dock_menu() {
  {
    let mut handler = dock_menu_handler().lock().unwrap();
    *handler = None;
  }

  let api = api();
  if let Some(f) = api.set_dock_menu {
    unsafe {
      f(
        api.backend_data,
        std::ptr::null_mut(),
        None,
        std::ptr::null_mut(),
      );
    }
  }
}

/// Show or hide the app's dock icon (macOS activation policy).
/// Windows and Linux: no-op (no app-level equivalent).
pub fn set_dock_visible(visible: bool) {
  let api = api();
  if let Some(f) = api.set_dock_visible {
    unsafe { f(api.backend_data, visible) };
  }
}

/// Register a callback invoked when the user clicks the dock icon while the
/// app has no visible windows (macOS only). The callback receives whether
/// any windows are currently visible.
///
/// The default "show last hidden window" behavior is always swallowed — the
/// callback is purely informational; user code decides what (if anything) to
/// do (e.g. call `window.show()`).
///
/// Windows and Linux: no-op (no equivalent event).
pub fn on_dock_reopen<F>(handler: F)
where
  F: Fn(bool) + Send + Sync + 'static,
{
  {
    let mut slot = dock_reopen_handler().lock().unwrap();
    *slot = Some(Box::new(handler));
  }

  let api = api();
  if let Some(f) = api.set_dock_reopen_handler {
    unsafe {
      f(
        api.backend_data,
        Some(dock_reopen_callback),
        std::ptr::null_mut(),
      );
    }
  }
}

// --- Tray / status-bar icon ---

fn tray_menu_handlers(
) -> &'static Mutex<HashMap<u32, Box<dyn Fn(&str) + Send + Sync>>> {
  TRAY_MENU_HANDLERS.get_or_init(|| Mutex::new(HashMap::new()))
}

fn tray_click_handlers(
) -> &'static Mutex<HashMap<u32, Box<dyn Fn() + Send + Sync>>> {
  TRAY_CLICK_HANDLERS.get_or_init(|| Mutex::new(HashMap::new()))
}

fn tray_dblclick_handlers(
) -> &'static Mutex<HashMap<u32, Box<dyn Fn() + Send + Sync>>> {
  TRAY_DBLCLICK_HANDLERS.get_or_init(|| Mutex::new(HashMap::new()))
}

unsafe extern "C" fn tray_menu_click_callback(
  _user_data: *mut c_void,
  tray_id: u32,
  item_id: *const c_char,
) {
  if item_id.is_null() {
    return;
  }
  let id = CStr::from_ptr(item_id).to_string_lossy();
  let handlers = tray_menu_handlers().lock().unwrap();
  if let Some(handler) = handlers.get(&tray_id) {
    handler(&id);
  }
}

unsafe extern "C" fn tray_click_callback(
  _user_data: *mut c_void,
  tray_id: u32,
) {
  let handlers = tray_click_handlers().lock().unwrap();
  if let Some(handler) = handlers.get(&tray_id) {
    handler();
  }
}

unsafe extern "C" fn tray_dblclick_callback(
  _user_data: *mut c_void,
  tray_id: u32,
) {
  let handlers = tray_dblclick_handlers().lock().unwrap();
  if let Some(handler) = handlers.get(&tray_id) {
    handler();
  }
}

/// A persistent icon in the OS status area (macOS menu bar extras, Windows
/// system tray, Linux AppIndicator). Multiple icons may be created.
///
/// The native icon is destroyed when the `TrayIcon` is dropped. Clone via
/// [`TrayIcon::id`] + [`TrayIcon::from_id`] if you need multiple handles.
pub struct TrayIcon {
  id: u32,
  owned: bool,
}

impl TrayIcon {
  /// Create a new tray icon. Returns a `TrayIcon` with `id == 0` if the
  /// backend doesn't support tray icons (e.g. CEF on Linux).
  pub fn new() -> Self {
    let api = api();
    let id = if let Some(f) = api.create_tray_icon {
      unsafe { f(api.backend_data) }
    } else {
      0
    };
    TrayIcon { id, owned: true }
  }

  /// Wrap an existing tray id (doesn't create a new native icon; doesn't
  /// destroy on drop).
  pub fn from_id(id: u32) -> Self {
    TrayIcon { id, owned: false }
  }

  pub fn id(&self) -> u32 {
    self.id
  }

  /// The tray icon's bounding rectangle `(x, y, width, height)` in screen
  /// coordinates, in the same top-left-origin space as
  /// [`Window::set_position`], or `None` if the position isn't known yet or
  /// the backend/platform can't report it. Use this to anchor a popover
  /// window under the icon.
  pub fn get_bounds(&self) -> Option<(i32, i32, i32, i32)> {
    let api = api();
    let f = api.get_tray_icon_bounds?;
    let mut x: c_int = 0;
    let mut y: c_int = 0;
    let mut width: c_int = 0;
    let mut height: c_int = 0;
    let ok = unsafe {
      f(
        api.backend_data,
        self.id,
        &mut x,
        &mut y,
        &mut width,
        &mut height,
      )
    };
    if ok {
      Some((x, y, width, height))
    } else {
      None
    }
  }

  /// Set the icon image from PNG-encoded bytes.
  pub fn icon(self, png_bytes: &[u8]) -> Self {
    self.set_icon(png_bytes);
    self
  }

  /// Set the tooltip shown on hover.
  pub fn tooltip(self, text: &str) -> Self {
    self.set_tooltip(Some(text));
    self
  }

  /// Set the right-click context menu.
  pub fn menu<F>(self, template: &[MenuItem], on_click: F) -> Self
  where
    F: Fn(&str) + Send + Sync + 'static,
  {
    self.set_menu(template, on_click);
    self
  }

  /// Register a left-click handler. Right-click is reserved for the menu.
  pub fn on_click<F>(self, handler: F) -> Self
  where
    F: Fn() + Send + Sync + 'static,
  {
    {
      let mut handlers = tray_click_handlers().lock().unwrap();
      handlers.insert(self.id, Box::new(handler));
    }
    let api = api();
    if let Some(f) = api.set_tray_click_handler {
      unsafe {
        f(
          api.backend_data,
          self.id,
          Some(tray_click_callback),
          std::ptr::null_mut(),
        );
      }
    }
    self
  }

  /// Register a left-double-click handler. Fires in addition to `on_click`
  /// for the first of the two clicks. No-op on Linux.
  pub fn on_double_click<F>(self, handler: F) -> Self
  where
    F: Fn() + Send + Sync + 'static,
  {
    self.set_double_click_handler(handler);
    self
  }

  /// Set the icon used when the OS is in dark mode. Cleared when passed an
  /// empty slice.
  pub fn icon_dark(self, png_bytes: &[u8]) -> Self {
    self.set_icon_dark(png_bytes);
    self
  }

  pub fn set_icon(&self, png_bytes: &[u8]) {
    if self.id == 0 {
      return;
    }
    let api = api();
    if let Some(f) = api.set_tray_icon {
      unsafe {
        f(
          api.backend_data,
          self.id,
          png_bytes.as_ptr() as *const c_void,
          png_bytes.len(),
        );
      }
    }
  }

  pub fn set_icon_dark(&self, png_bytes: &[u8]) {
    if self.id == 0 {
      return;
    }
    let api = api();
    if let Some(f) = api.set_tray_icon_dark {
      unsafe {
        f(
          api.backend_data,
          self.id,
          if png_bytes.is_empty() {
            std::ptr::null()
          } else {
            png_bytes.as_ptr() as *const c_void
          },
          png_bytes.len(),
        );
      }
    }
  }

  pub fn set_double_click_handler<F>(&self, handler: F)
  where
    F: Fn() + Send + Sync + 'static,
  {
    if self.id == 0 {
      return;
    }
    {
      let mut handlers = tray_dblclick_handlers().lock().unwrap();
      handlers.insert(self.id, Box::new(handler));
    }
    let api = api();
    if let Some(f) = api.set_tray_double_click_handler {
      unsafe {
        f(
          api.backend_data,
          self.id,
          Some(tray_dblclick_callback),
          std::ptr::null_mut(),
        );
      }
    }
  }

  pub fn set_tooltip(&self, text: Option<&str>) {
    if self.id == 0 {
      return;
    }
    let api = api();
    if let Some(f) = api.set_tray_tooltip {
      match text {
        Some(t) if !t.is_empty() => {
          let c_text = CString::new(t).expect("Invalid tooltip");
          unsafe { f(api.backend_data, self.id, c_text.as_ptr()) };
        }
        _ => unsafe { f(api.backend_data, self.id, std::ptr::null()) },
      }
    }
  }

  pub fn set_menu<F>(&self, template: &[MenuItem], on_click: F)
  where
    F: Fn(&str) + Send + Sync + 'static,
  {
    if self.id == 0 {
      return;
    }
    let value = Value::List(template.iter().map(|i| i.to_value()).collect());
    {
      let mut handlers = tray_menu_handlers().lock().unwrap();
      handlers.insert(self.id, Box::new(on_click));
    }
    let api = api();
    if let Some(f) = api.set_tray_menu {
      let raw = value.to_raw();
      unsafe {
        f(
          api.backend_data,
          self.id,
          raw,
          Some(tray_menu_click_callback),
          std::ptr::null_mut(),
        );
      }
    }
  }

  pub fn clear_menu(&self) {
    if self.id == 0 {
      return;
    }
    {
      let mut handlers = tray_menu_handlers().lock().unwrap();
      handlers.remove(&self.id);
    }
    let api = api();
    if let Some(f) = api.set_tray_menu {
      unsafe {
        f(
          api.backend_data,
          self.id,
          std::ptr::null_mut(),
          None,
          std::ptr::null_mut(),
        );
      }
    }
  }
}

impl Default for TrayIcon {
  fn default() -> Self {
    Self::new()
  }
}

impl Drop for TrayIcon {
  fn drop(&mut self) {
    if !self.owned || self.id == 0 {
      return;
    }
    // Clean up handler maps so we don't hold stale closures.
    if let Some(m) = TRAY_MENU_HANDLERS.get() {
      m.lock().unwrap().remove(&self.id);
    }
    if let Some(m) = TRAY_CLICK_HANDLERS.get() {
      m.lock().unwrap().remove(&self.id);
    }
    if let Some(m) = TRAY_DBLCLICK_HANDLERS.get() {
      m.lock().unwrap().remove(&self.id);
    }
    let api = api();
    if let Some(f) = api.destroy_tray_icon {
      unsafe { f(api.backend_data, self.id) };
    }
  }
}

/// Set the global JS namespace name for bindings (default: `"Laufey"`).
/// Must be called before creating any windows.
/// ```no_run
/// laufey::set_js_namespace("MyApp");
/// // JS code can now use: window.MyApp.greet("world")
/// ```
pub fn set_js_namespace(name: &str) {
  let api = api();
  if let Some(f) = api.set_js_namespace {
    let c_name = CString::new(name).unwrap();
    unsafe { f(api.backend_data, c_name.as_ptr()) };
  }
}

pub const LAUFEY_DIALOG_ALERT: i32 = 0;
pub const LAUFEY_DIALOG_CONFIRM: i32 = 1;
pub const LAUFEY_DIALOG_PROMPT: i32 = 2;

/// Show an alert dialog (app-wide, no parent window). Blocks until
/// dismissed; the platform's modal run loop pumps OS events while the
/// dialog is up so other LAUFEY windows continue to render and respond.
pub fn alert(title: &str, message: &str) {
  show_dialog_blocking(0, LAUFEY_DIALOG_ALERT, title, message, "");
}

/// Show a confirm dialog (app-wide). Returns `true` if OK was pressed.
/// Blocking semantics as `alert`.
pub fn confirm(title: &str, message: &str) -> bool {
  let (confirmed, _) =
    show_dialog_blocking(0, LAUFEY_DIALOG_CONFIRM, title, message, "");
  confirmed
}

/// Show a prompt dialog (app-wide). Returns `Some(text)` if OK, `None`
/// if cancelled. Blocking semantics as `alert`.
pub fn prompt(
  title: &str,
  message: &str,
  default_value: &str,
) -> Option<String> {
  let (confirmed, input) = show_dialog_blocking(
    0,
    LAUFEY_DIALOG_PROMPT,
    title,
    message,
    default_value,
  );
  if confirmed {
    input
  } else {
    None
  }
}

// --- Notifications ---

pub const LAUFEY_NOTIFICATION_SHOWN: i32 = 0;
pub const LAUFEY_NOTIFICATION_CLICKED: i32 = 1;
pub const LAUFEY_NOTIFICATION_CLOSED: i32 = 2;
pub const LAUFEY_NOTIFICATION_ACTION: i32 = 3;

/// What happened to a notification.
#[derive(Debug, Clone)]
pub enum NotificationEvent {
  /// The OS displayed the banner.
  Shown,
  /// The user clicked the notification body.
  Clicked,
  /// The user dismissed the notification or it expired.
  Closed,
  /// The user clicked an action button. The string is the action `id`.
  Action(String),
}

/// An action button on a notification.
#[derive(Clone, Debug)]
pub struct NotificationAction {
  pub id: String,
  pub title: String,
}

/// Builder for a system notification. Mirrors a subset of the Web
/// Notifications API constructor options. Construct with
/// [`Notification::new`] / [`Notification::builder`], chain options, then
/// [`Notification::show`].
#[derive(Clone, Debug, Default)]
pub struct Notification {
  title: String,
  body: Option<String>,
  icon: Option<Vec<u8>>,
  tag: Option<String>,
  silent: Option<bool>,
  require_interaction: Option<bool>,
  actions: Vec<NotificationAction>,
}

/// Handle to a shown notification. Use [`NotificationHandle::close`] to
/// dismiss it programmatically. Dropping the handle does NOT close the
/// notification — they're fire-and-forget on the OS side.
#[derive(Debug, Clone, Copy)]
pub struct NotificationHandle {
  id: u32,
}

impl NotificationHandle {
  pub fn id(&self) -> u32 {
    self.id
  }

  pub fn close(&self) {
    if self.id == 0 {
      return;
    }
    let api = api();
    if let Some(f) = api.close_notification {
      unsafe { f(api.backend_data, self.id) };
    }
    if let Some(m) = NOTIFICATION_HANDLERS.get() {
      m.lock().unwrap().remove(&self.id);
    }
  }
}

impl Notification {
  /// Create a new notification with the given title (required field).
  pub fn new(title: impl Into<String>) -> Self {
    Self {
      title: title.into(),
      ..Default::default()
    }
  }

  /// Alias for [`Notification::new`].
  pub fn builder(title: impl Into<String>) -> Self {
    Self::new(title)
  }

  pub fn body(mut self, body: impl Into<String>) -> Self {
    self.body = Some(body.into());
    self
  }

  /// Set the notification icon (PNG bytes).
  pub fn icon(mut self, png_bytes: impl Into<Vec<u8>>) -> Self {
    self.icon = Some(png_bytes.into());
    self
  }

  /// Replace any existing notification with the same tag instead of
  /// stacking a new one (Web Notifications spec semantics).
  pub fn tag(mut self, tag: impl Into<String>) -> Self {
    self.tag = Some(tag.into());
    self
  }

  /// Suppress the system notification sound.
  pub fn silent(mut self, silent: bool) -> Self {
    self.silent = Some(silent);
    self
  }

  /// Keep the notification visible until the user dismisses it (instead
  /// of auto-expiring). Honored where the platform supports it.
  pub fn require_interaction(mut self, require: bool) -> Self {
    self.require_interaction = Some(require);
    self
  }

  /// Add an action button. Multiple calls add multiple buttons.
  /// Ignored on platforms that don't surface action buttons.
  pub fn action(
    mut self,
    id: impl Into<String>,
    title: impl Into<String>,
  ) -> Self {
    self.actions.push(NotificationAction {
      id: id.into(),
      title: title.into(),
    });
    self
  }

  fn to_value(&self) -> Value {
    let mut dict = HashMap::new();
    dict.insert("title".to_string(), Value::String(self.title.clone()));
    if let Some(body) = &self.body {
      dict.insert("body".to_string(), Value::String(body.clone()));
    }
    if let Some(icon) = &self.icon {
      dict.insert("icon".to_string(), Value::Binary(icon.clone()));
    }
    if let Some(tag) = &self.tag {
      dict.insert("tag".to_string(), Value::String(tag.clone()));
    }
    if let Some(silent) = self.silent {
      dict.insert("silent".to_string(), Value::Bool(silent));
    }
    if let Some(require) = self.require_interaction {
      dict.insert("require_interaction".to_string(), Value::Bool(require));
    }
    if !self.actions.is_empty() {
      let actions = self
        .actions
        .iter()
        .map(|a| {
          let mut d = HashMap::new();
          d.insert("id".to_string(), Value::String(a.id.clone()));
          d.insert("title".to_string(), Value::String(a.title.clone()));
          Value::Dict(d)
        })
        .collect();
      dict.insert("actions".to_string(), Value::List(actions));
    }
    Value::Dict(dict)
  }

  /// Show the notification. Returns a handle that can be used to close
  /// it programmatically. Returns a handle with id 0 if the backend
  /// doesn't support notifications.
  pub fn show(self) -> NotificationHandle {
    self.show_with_handler::<fn(NotificationEvent)>(None)
  }

  /// Show the notification and register a callback for events
  /// (shown / clicked / closed / action).
  pub fn on_event<F>(self, handler: F) -> NotificationHandle
  where
    F: Fn(NotificationEvent) + Send + Sync + 'static,
  {
    self.show_with_handler(Some(handler))
  }

  fn show_with_handler<F>(self, handler: Option<F>) -> NotificationHandle
  where
    F: Fn(NotificationEvent) + Send + Sync + 'static,
  {
    let api = api();
    let Some(show_fn) = api.show_notification else {
      return NotificationHandle { id: 0 };
    };
    let raw = self.to_value().to_raw();
    let (cb, user_data): (
      Option<unsafe extern "C" fn(*mut c_void, u32, c_int, *const c_char)>,
      *mut c_void,
    ) = if handler.is_some() {
      (Some(notification_event_callback), std::ptr::null_mut())
    } else {
      (None, std::ptr::null_mut())
    };
    let id = unsafe { show_fn(api.backend_data, raw, cb, user_data) };
    if id != 0 {
      if let Some(h) = handler {
        notification_handlers()
          .lock()
          .unwrap()
          .insert(id, Arc::new(h));
      }
    }
    NotificationHandle { id }
  }
}

fn notification_handlers(
) -> &'static Mutex<HashMap<u32, Arc<dyn Fn(NotificationEvent) + Send + Sync>>>
{
  NOTIFICATION_HANDLERS.get_or_init(|| Mutex::new(HashMap::new()))
}

unsafe extern "C" fn notification_event_callback(
  _user_data: *mut c_void,
  notification_id: u32,
  reason: c_int,
  action_id_or_null: *const c_char,
) {
  let event = match reason {
    LAUFEY_NOTIFICATION_SHOWN => NotificationEvent::Shown,
    LAUFEY_NOTIFICATION_CLICKED => NotificationEvent::Clicked,
    LAUFEY_NOTIFICATION_CLOSED => NotificationEvent::Closed,
    LAUFEY_NOTIFICATION_ACTION => {
      let id = if action_id_or_null.is_null() {
        String::new()
      } else {
        CStr::from_ptr(action_id_or_null)
          .to_string_lossy()
          .into_owned()
      };
      NotificationEvent::Action(id)
    }
    _ => return,
  };
  let is_terminal = matches!(event, NotificationEvent::Closed);
  // Clone the Arc out of the map so the handler runs without the lock
  // held — handlers may legitimately call back into the laufey API.
  let handler = notification_handlers()
    .lock()
    .unwrap()
    .get(&notification_id)
    .cloned();
  if let Some(h) = handler {
    h(event);
  }
  if is_terminal {
    notification_handlers()
      .lock()
      .unwrap()
      .remove(&notification_id);
  }
}

// --- Permissions / runtime authorization ---

pub const LAUFEY_PERMISSION_INVALID: i32 = 0;
pub const LAUFEY_PERMISSION_NOTIFICATIONS: i32 = 1;

pub const LAUFEY_PERMISSION_STATUS_GRANTED: i32 = 0;
pub const LAUFEY_PERMISSION_STATUS_DENIED: i32 = 1;
pub const LAUFEY_PERMISSION_STATUS_PROMPT: i32 = 2;
pub const LAUFEY_PERMISSION_STATUS_UNSUPPORTED: i32 = 3;

/// Capability for which authorization can be requested.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum PermissionKind {
  Notifications = LAUFEY_PERMISSION_NOTIFICATIONS,
}

/// Result of [`request_permission`] / [`query_permission`]. Mirrors the
/// Web Permissions API state set with an extra `Unsupported` variant for
/// environments where the capability cannot be authorized at all (e.g.
/// an unbundled macOS process, or a backend that has no concept of the
/// kind on this platform).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum PermissionStatus {
  Granted = LAUFEY_PERMISSION_STATUS_GRANTED,
  Denied = LAUFEY_PERMISSION_STATUS_DENIED,
  Prompt = LAUFEY_PERMISSION_STATUS_PROMPT,
  Unsupported = LAUFEY_PERMISSION_STATUS_UNSUPPORTED,
}

impl PermissionStatus {
  fn from_raw(v: c_int) -> Self {
    match v {
      LAUFEY_PERMISSION_STATUS_GRANTED => Self::Granted,
      LAUFEY_PERMISSION_STATUS_DENIED => Self::Denied,
      LAUFEY_PERMISSION_STATUS_PROMPT => Self::Prompt,
      _ => Self::Unsupported,
    }
  }
}

unsafe extern "C" fn permission_trampoline(
  user_data: *mut c_void,
  status: c_int,
) {
  let cb =
    Box::from_raw(user_data as *mut Box<dyn FnOnce(PermissionStatus) + Send>);
  cb(PermissionStatus::from_raw(status));
}

fn dispatch_permission<F>(
  f: Option<
    unsafe extern "C" fn(
      *mut c_void,
      c_int,
      Option<unsafe extern "C" fn(*mut c_void, c_int)>,
      *mut c_void,
    ),
  >,
  kind: PermissionKind,
  callback: F,
) where
  F: FnOnce(PermissionStatus) + Send + 'static,
{
  let api = api();
  let Some(f) = f else {
    callback(PermissionStatus::Unsupported);
    return;
  };
  let boxed: Box<Box<dyn FnOnce(PermissionStatus) + Send>> =
    Box::new(Box::new(callback));
  let user_data = Box::into_raw(boxed) as *mut c_void;
  unsafe {
    f(
      api.backend_data,
      kind as c_int,
      Some(permission_trampoline),
      user_data,
    )
  };
}

/// Query the current authorization status of `kind` without prompting
/// the user. The callback runs on the UI thread.
pub fn query_permission<F>(kind: PermissionKind, callback: F)
where
  F: FnOnce(PermissionStatus) + Send + 'static,
{
  dispatch_permission(api().query_permission, kind, callback);
}

/// Request authorization for `kind`. If the current status is
/// [`PermissionStatus::Prompt`] the OS displays a system prompt;
/// otherwise the cached decision is returned without re-prompting (the
/// OS does not show a second prompt once the user has decided). The
/// callback runs on the UI thread.
pub fn request_permission<F>(kind: PermissionKind, callback: F)
where
  F: FnOnce(PermissionStatus) + Send + 'static,
{
  dispatch_permission(api().request_permission, kind, callback);
}

pub const LAUFEY_KEY_PRESSED: i32 = 0;
pub const LAUFEY_KEY_RELEASED: i32 = 1;

pub const LAUFEY_MOD_SHIFT: u32 = 1 << 0;
pub const LAUFEY_MOD_CONTROL: u32 = 1 << 1;
pub const LAUFEY_MOD_ALT: u32 = 1 << 2;
pub const LAUFEY_MOD_META: u32 = 1 << 3;

#[derive(Debug, Clone, Copy, Default)]
pub struct KeyModifiers {
  pub shift: bool,
  pub control: bool,
  pub alt: bool,
  pub meta: bool,
}

impl KeyModifiers {
  pub(crate) fn from_raw(flags: u32) -> Self {
    Self {
      shift: flags & LAUFEY_MOD_SHIFT != 0,
      control: flags & LAUFEY_MOD_CONTROL != 0,
      alt: flags & LAUFEY_MOD_ALT != 0,
      meta: flags & LAUFEY_MOD_META != 0,
    }
  }
}

#[macro_export]
macro_rules! main {
  ($main_fn:expr) => {
    #[no_mangle]
    /// # Safety
    /// `api` must be either null or a valid pointer to a `LaufeyBackendApi`
    /// with static lifetime supplied by the host runtime.
    pub unsafe extern "C" fn laufey_runtime_init(
      api: *const $crate::LaufeyBackendApi,
    ) -> std::ffi::c_int {
      unsafe { $crate::init_api(api) }
    }

    #[no_mangle]
    pub extern "C" fn laufey_runtime_start() -> std::ffi::c_int {
      let main_fn: fn() = $main_fn;
      main_fn();
      0
    }

    #[no_mangle]
    pub extern "C" fn laufey_runtime_shutdown() {
      $crate::shutdown();
    }
  };
}

#[cfg(test)]
mod tests {
  use super::*;

  // --- KeyModifiers ---

  #[test]
  fn key_modifiers_empty() {
    let m = KeyModifiers::from_raw(0);
    assert!(!m.shift && !m.control && !m.alt && !m.meta);
  }

  #[test]
  fn key_modifiers_single_flags() {
    assert!(KeyModifiers::from_raw(LAUFEY_MOD_SHIFT).shift);
    assert!(KeyModifiers::from_raw(LAUFEY_MOD_CONTROL).control);
    assert!(KeyModifiers::from_raw(LAUFEY_MOD_ALT).alt);
    assert!(KeyModifiers::from_raw(LAUFEY_MOD_META).meta);
  }

  #[test]
  fn key_modifiers_combinations() {
    // All four bits set.
    let all = KeyModifiers::from_raw(
      LAUFEY_MOD_SHIFT | LAUFEY_MOD_CONTROL | LAUFEY_MOD_ALT | LAUFEY_MOD_META,
    );
    assert!(all.shift && all.control && all.alt && all.meta);

    // Unknown high bits are ignored, known low bits still decode.
    let mixed = KeyModifiers::from_raw(LAUFEY_MOD_SHIFT | 0xF000_0000);
    assert!(mixed.shift && !mixed.control && !mixed.alt && !mixed.meta);
  }

  // --- PermissionStatus ---

  #[test]
  fn permission_status_from_raw_known() {
    assert_eq!(
      PermissionStatus::from_raw(LAUFEY_PERMISSION_STATUS_GRANTED),
      PermissionStatus::Granted
    );
    assert_eq!(
      PermissionStatus::from_raw(LAUFEY_PERMISSION_STATUS_DENIED),
      PermissionStatus::Denied
    );
    assert_eq!(
      PermissionStatus::from_raw(LAUFEY_PERMISSION_STATUS_PROMPT),
      PermissionStatus::Prompt
    );
    assert_eq!(
      PermissionStatus::from_raw(LAUFEY_PERMISSION_STATUS_UNSUPPORTED),
      PermissionStatus::Unsupported
    );
  }

  #[test]
  fn permission_status_from_raw_unknown_is_unsupported() {
    // Anything outside the LAUFEY_PERMISSION_STATUS_* range must map to
    // Unsupported so a future backend can't silently mean "Granted" by
    // returning, say, 99.
    assert_eq!(
      PermissionStatus::from_raw(99),
      PermissionStatus::Unsupported
    );
    assert_eq!(
      PermissionStatus::from_raw(-1),
      PermissionStatus::Unsupported
    );
  }

  // --- Value accessors ---

  #[test]
  fn value_accessors() {
    assert_eq!(Value::String("hi".into()).as_string(), Some("hi"));
    assert_eq!(Value::Int(42).as_int(), Some(42));
    assert_eq!(Value::Bool(true).as_bool(), Some(true));
    assert!(Value::List(vec![Value::Int(1)]).as_list().is_some());
    assert!(Value::Dict(HashMap::new()).as_dict().is_some());

    // Type mismatch must return None — these accessors are used in
    // binding handlers where the wrong type from JS is a soft error.
    assert!(Value::Int(1).as_string().is_none());
    assert!(Value::String("x".into()).as_int().is_none());
    assert!(Value::Null.as_bool().is_none());
  }

  // --- MenuItem::to_value ---

  fn dict_get<'a>(v: &'a Value, key: &str) -> Option<&'a Value> {
    v.as_dict().and_then(|d| d.get(key))
  }

  #[test]
  fn menu_item_to_value_item_minimal() {
    let item = MenuItem::Item {
      label: "Quit".into(),
      id: None,
      accelerator: None,
      enabled: true,
    };
    let v = item.to_value();
    assert_eq!(
      dict_get(&v, "label").and_then(|v| v.as_string()),
      Some("Quit")
    );
    // Absent keys when their option is None / enabled is true (default).
    assert!(dict_get(&v, "id").is_none());
    assert!(dict_get(&v, "accelerator").is_none());
    assert!(dict_get(&v, "enabled").is_none());
  }

  #[test]
  fn menu_item_to_value_item_full() {
    let item = MenuItem::Item {
      label: "Open…".into(),
      id: Some("file.open".into()),
      accelerator: Some("CmdOrCtrl+O".into()),
      enabled: false,
    };
    let v = item.to_value();
    assert_eq!(
      dict_get(&v, "id").and_then(|v| v.as_string()),
      Some("file.open")
    );
    assert_eq!(
      dict_get(&v, "accelerator").and_then(|v| v.as_string()),
      Some("CmdOrCtrl+O")
    );
    // enabled=false must serialize; enabled=true must NOT (the backend
    // defaults to enabled, so omitting it keeps the wire payload small).
    assert_eq!(
      dict_get(&v, "enabled").and_then(|v| v.as_bool()),
      Some(false)
    );
  }

  #[test]
  fn menu_item_to_value_submenu_separator_role() {
    let sub = MenuItem::Submenu {
      label: "File".into(),
      items: vec![MenuItem::Separator],
    };
    let v = sub.to_value();
    assert_eq!(
      dict_get(&v, "label").and_then(|v| v.as_string()),
      Some("File")
    );
    let items = dict_get(&v, "submenu").and_then(|v| v.as_list()).unwrap();
    assert_eq!(items.len(), 1);
    assert_eq!(
      dict_get(&items[0], "type").and_then(|v| v.as_string()),
      Some("separator")
    );

    let role = MenuItem::Role {
      role: "copy".into(),
    };
    let rv = role.to_value();
    assert_eq!(
      dict_get(&rv, "role").and_then(|v| v.as_string()),
      Some("copy")
    );
  }

  // --- Notification::to_value ---

  #[test]
  fn notification_to_value_title_only() {
    let v = Notification::new("hi").to_value();
    assert_eq!(
      dict_get(&v, "title").and_then(|v| v.as_string()),
      Some("hi")
    );
    // Absent options should not appear.
    for k in [
      "body",
      "icon",
      "tag",
      "silent",
      "require_interaction",
      "actions",
    ] {
      assert!(
        dict_get(&v, k).is_none(),
        "key {k} should be absent when not set"
      );
    }
  }

  #[test]
  fn notification_to_value_full() {
    let v = Notification::new("t")
      .body("b")
      .icon(vec![1u8, 2, 3])
      .tag("g")
      .silent(true)
      .require_interaction(true)
      .action("ok", "OK")
      .action("dismiss", "Dismiss")
      .to_value();
    assert_eq!(dict_get(&v, "body").and_then(|v| v.as_string()), Some("b"));
    assert_eq!(dict_get(&v, "tag").and_then(|v| v.as_string()), Some("g"));
    assert_eq!(dict_get(&v, "silent").and_then(|v| v.as_bool()), Some(true));
    assert_eq!(
      dict_get(&v, "require_interaction").and_then(|v| v.as_bool()),
      Some(true)
    );
    let actions = dict_get(&v, "actions").and_then(|v| v.as_list()).unwrap();
    assert_eq!(actions.len(), 2);
    assert_eq!(
      dict_get(&actions[0], "id").and_then(|v| v.as_string()),
      Some("ok")
    );
    assert_eq!(
      dict_get(&actions[1], "title").and_then(|v| v.as_string()),
      Some("Dismiss")
    );
    // Icon comes through as binary, not string.
    match dict_get(&v, "icon") {
      Some(Value::Binary(b)) => assert_eq!(b, &vec![1u8, 2, 3]),
      _ => panic!("icon must be Value::Binary"),
    }
  }

  // --- Exhaustive KeyModifiers bit combinations ---

  #[test]
  fn key_modifiers_every_combination() {
    // Walk all 16 combinations of the 4 modifier flags. Catches a
    // regression where a flag mask was renumbered (e.g. ALT and META
    // swapped) — every other-numbered subset would still pass.
    for bits in 0..16u32 {
      let raw = (if bits & 1 != 0 { LAUFEY_MOD_SHIFT } else { 0 })
        | (if bits & 2 != 0 { LAUFEY_MOD_CONTROL } else { 0 })
        | (if bits & 4 != 0 { LAUFEY_MOD_ALT } else { 0 })
        | (if bits & 8 != 0 { LAUFEY_MOD_META } else { 0 });
      let m = KeyModifiers::from_raw(raw);
      assert_eq!(m.shift, bits & 1 != 0, "shift bit @ {bits:04b}");
      assert_eq!(m.control, bits & 2 != 0, "control bit @ {bits:04b}");
      assert_eq!(m.alt, bits & 4 != 0, "alt bit @ {bits:04b}");
      assert_eq!(m.meta, bits & 8 != 0, "meta bit @ {bits:04b}");
    }
  }

  // --- Value: cases not covered by value_accessors ---

  #[test]
  fn value_accessors_for_null_and_double_and_binary() {
    // Null doesn't satisfy any of as_string/as_int/as_bool/as_list/as_dict.
    let n = Value::Null;
    assert!(n.as_string().is_none());
    assert!(n.as_int().is_none());
    assert!(n.as_bool().is_none());
    assert!(n.as_list().is_none());
    assert!(n.as_dict().is_none());

    // Doubles aren't ints — feature parity with JS where 1.5 isn't a 1.
    assert!(Value::Double(1.5).as_int().is_none());

    // No public accessor for Binary, but pattern-matching still works
    // and the variant must round-trip through the public API surface
    // (it's how Notification icons cross the boundary).
    match Value::Binary(vec![0xDE, 0xAD]) {
      Value::Binary(b) => assert_eq!(b, vec![0xDE, 0xAD]),
      _ => unreachable!(),
    }
  }

  #[test]
  fn value_list_and_dict_nest_arbitrarily() {
    let inner = Value::Dict({
      let mut m = HashMap::new();
      m.insert("k".to_string(), Value::Int(1));
      m
    });
    let outer = Value::List(vec![Value::Null, inner]);
    let list = outer.as_list().unwrap();
    assert_eq!(list.len(), 2);
    assert!(matches!(list[0], Value::Null));
    let inner_dict = list[1].as_dict().unwrap();
    assert_eq!(inner_dict.get("k").and_then(|v| v.as_int()), Some(1));
  }

  // --- MenuItem: corner cases the basic tests didn't reach ---

  #[test]
  fn menu_item_role_does_not_carry_label_or_id() {
    // Role items are wholly defined by the role name. A regression that
    // started attaching `label` or `id` would let user code masquerade
    // as a role-bound system item.
    let v = MenuItem::Role {
      role: "quit".into(),
    }
    .to_value();
    let dict = v.as_dict().unwrap();
    assert!(dict.get("role").is_some());
    assert!(dict.get("label").is_none());
    assert!(dict.get("id").is_none());
    assert!(dict.get("submenu").is_none());
  }

  #[test]
  fn menu_item_nested_submenus_recursively_serialize() {
    let menu = MenuItem::Submenu {
      label: "File".into(),
      items: vec![MenuItem::Submenu {
        label: "Recent".into(),
        items: vec![MenuItem::Item {
          label: "Open project.toml".into(),
          id: Some("recent.0".into()),
          accelerator: None,
          enabled: true,
        }],
      }],
    };
    let v = menu.to_value();
    let outer = v.as_dict().unwrap();
    let outer_items = outer.get("submenu").and_then(|v| v.as_list()).unwrap();
    assert_eq!(outer_items.len(), 1);
    let inner = outer_items[0]
      .as_dict()
      .expect("nested submenu must be dict");
    let inner_items = inner.get("submenu").and_then(|v| v.as_list()).unwrap();
    assert_eq!(inner_items.len(), 1);
    assert_eq!(
      inner_items[0]
        .as_dict()
        .and_then(|d| d.get("id"))
        .and_then(|v| v.as_string()),
      Some("recent.0")
    );
  }

  // --- Notification: argument boundary cases ---

  #[test]
  fn notification_empty_actions_list_is_omitted() {
    // Adding an `actions: []` key for a notification that opted out
    // would force the backend to allocate an empty list pointer on
    // every dispatch. The builder must elide it.
    let v = Notification::new("t").body("b").to_value();
    assert!(v.as_dict().unwrap().get("actions").is_none());
  }

  #[test]
  fn notification_partial_options_serialize_only_set_fields() {
    let v = Notification::new("t").body("b").silent(false).to_value();
    let d = v.as_dict().unwrap();
    assert_eq!(d.get("body").and_then(|v| v.as_string()), Some("b"));
    // silent=false is explicit, must serialize (the backend default
    // varies by platform).
    assert_eq!(d.get("silent").and_then(|v| v.as_bool()), Some(false));
    // Other keys remain absent.
    assert!(d.get("tag").is_none());
    assert!(d.get("require_interaction").is_none());
    assert!(d.get("icon").is_none());
    assert!(d.get("actions").is_none());
  }

  #[test]
  fn notification_handle_id_zero_is_noop_close() {
    // A zero-id handle indicates the backend didn't support
    // notifications. `close` on it must be a no-op (no panic, no
    // api() call). This is the failure mode for hello_runtime on CEF
    // Linux where notifications aren't wired up.
    let h = NotificationHandle { id: 0 };
    assert_eq!(h.id(), 0);
    // Doesn't touch api() because we never installed one.
    h.close();
  }

  // --- DockBounceType / PermissionKind: simple enum sanity ---

  #[test]
  fn window_options_map_to_flags() {
    // The flag bits cross the C ABI to create_window_ex, so a regression
    // that swapped or dropped a bit would silently mis-style windows.
    assert_eq!(WindowOptions::default().to_flags(), 0);
    assert_eq!(
      WindowOptions {
        frameless: true,
        ..Default::default()
      }
      .to_flags(),
      LAUFEY_WINDOW_FLAG_FRAMELESS
    );
    assert_eq!(
      WindowOptions {
        no_activate: true,
        ..Default::default()
      }
      .to_flags(),
      LAUFEY_WINDOW_FLAG_NO_ACTIVATE
    );
    assert_eq!(
      WindowOptions {
        transparent_titlebar: true,
        ..Default::default()
      }
      .to_flags(),
      LAUFEY_WINDOW_FLAG_TRANSPARENT_TITLEBAR
    );
    assert_eq!(
      WindowOptions {
        frameless: true,
        no_activate: true,
        ..Default::default()
      }
      .to_flags(),
      LAUFEY_WINDOW_FLAG_FRAMELESS | LAUFEY_WINDOW_FLAG_NO_ACTIVATE
    );
  }

  #[test]
  fn dock_bounce_type_is_copy_and_distinct() {
    // The enum is `#[derive(Copy)]` because it's an i32-shaped tag —
    // accidentally dropping Copy would silently force a move semantics
    // change on callers. Confirm copy + distinct discriminants.
    let a = DockBounceType::Informational;
    let b = a;
    let _ = a; // still usable after copy
    let c = DockBounceType::Critical;
    // We can't compare without PartialEq, but matches! works.
    assert!(matches!(a, DockBounceType::Informational));
    assert!(matches!(b, DockBounceType::Informational));
    assert!(matches!(c, DockBounceType::Critical));
  }

  #[test]
  fn permission_kind_repr_i32_matches_capi_constants() {
    // PermissionKind is `#[repr(i32)]` so its enum value is the same
    // integer the C ABI sends. A regression that changed the repr or
    // reordered variants would invert which capability is being asked
    // about.
    assert_eq!(
      PermissionKind::Notifications as i32,
      LAUFEY_PERMISSION_NOTIFICATIONS
    );
  }
}
