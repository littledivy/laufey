//! A window the user closes through the window system, then calls naming
//! its id.
//!
//! The close does not go through `close_window`: a `WM_CLOSE` is posted to the
//! window (Windows), or `gtk_window_close` -- what the window manager's close
//! button does -- runs on the GTK thread (WebKitGTK). The backend then tears
//! the window down on its own destroy path, which must drop the window's state
//! the way `close_window` does: afterwards the window reads as closed, and the
//! getters, setters, `execute_js` and the answer to a JS call the page made
//! before the close are no-ops that never reach the destroyed native window.
//! This runs while no close-requested handler is registered: with one, the
//! backend defers every OS close to the runtime, which then closes the window
//! through `close_window`. `N/A` where this runtime has no way to close a
//! window like the user would (macOS, CEF on Linux).

use std::sync::{Arc, Mutex};
use std::time::Duration;

use laufey::{JsCall, Value, Window};

use crate::{check, na, wait_for};

const TITLE: &str = "native-e2e-user-close";
/// A window id no backend hands out in this run.
const UNKNOWN_ID: u32 = 0x7fff_fff0;

type Getters = ((i32, i32), (i32, i32), bool, bool, bool, i64, bool);

/// Everything the getters read for `win` (opacity in hundredths).
fn getters(win: &Window) -> Getters {
  (
    win.get_size(),
    win.get_position(),
    win.get_visible(),
    win.get_resizable(),
    win.get_always_on_top(),
    (win.get_opacity() * 100.0).round() as i64,
    win.get_click_passthrough(),
  )
}

/// Runs `execute_js(script)` on `win` and waits up to 5s for its answer.
async fn eval(win: &Window, script: &str) -> Option<Result<Value, Value>> {
  let out: Arc<Mutex<Option<Result<Value, Value>>>> =
    Arc::new(Mutex::new(None));
  let o = out.clone();
  win.execute_js(script, Some(move |r| *o.lock().unwrap() = Some(r)));
  wait_for(|| out.lock().unwrap().is_some(), 50, 100).await;
  let r = out.lock().unwrap().take();
  r
}

pub async fn run() {
  if !os_close_supported() {
    na("a window the user closes (no OS close for this backend / platform)");
    return;
  }

  let held: Arc<Mutex<Option<JsCall>>> = Arc::new(Mutex::new(None));
  let win = Window::new(260, 200)
    .title(TITLE)
    .bind("closeHold", {
      let held = held.clone();
      // Never answered here: the answer goes out after the close.
      move |call| *held.lock().unwrap() = Some(call)
    })
    // The page title matches the window title (web engines adopt it).
    .load(&format!(
      "data:text/html,<!doctype html><title>{TITLE}</title>"
    ));
  win.show();
  if !wait_for(|| win.get_size().0 != 0, 100, 50).await {
    check("user-close window reports a size", false);
    return;
  }

  // Web engines: a JS call from the page, left pending across the close.
  let engine = matches!(
    eval(&win, "1 + 1").await,
    Some(Ok(v)) if !matches!(v, Value::Null)
  );
  if engine {
    for _ in 0..100 {
      if held.lock().unwrap().is_some() {
        break;
      }
      win.execute_js(
        "typeof Laufey !== 'undefined' && Laufey.closeHold().catch(() => {})",
        None::<fn(_)>,
      );
      tokio::time::sleep(Duration::from_millis(100)).await;
    }
    // Best effort: WebView2 accepts no bridge message from a data: page (its
    // main-frame check compares the message and document sources).
    if held.lock().unwrap().is_some() {
      check("the page's JS call is pending before the user close", true);
    } else {
      na("a JS call pending across the user close (the page's call never arrived)");
    }
  }

  if !os_close(TITLE).await {
    check("the OS close reached the window", false);
    win.close();
    return;
  }
  let closed = wait_for(|| win.get_size() == (0, 0), 100, 50).await;
  check(
    "a window the user closed reads as closed (size 0x0)",
    closed,
  );
  if !closed {
    return;
  }

  // Every call below names the destroyed window: each must find no window,
  // so every getter reads what it reads for an id the backend never knew.
  // A stale entry reads the destroyed native window instead.
  let unknown = Window::from_id(UNKNOWN_ID);
  let (closed_reads, unknown_reads) = (getters(&win), getters(&unknown));
  if closed_reads != unknown_reads {
    eprintln!(
      "[e2e] user-closed window reads {closed_reads:?}, an unknown id reads \
       {unknown_reads:?}"
    );
  }
  check(
    "a user-closed window's getters read as an unknown window's",
    closed_reads == unknown_reads,
  );
  win.set_title("after-close");
  win.set_size(300, 200);
  win.set_position(10, 10);
  win.hide();
  if engine {
    // A window the backend no longer knows answers at once with no value; a
    // stale entry hands the script to the destroyed web view instead.
    let r = eval(&win, "1 + 1").await;
    check(
      "execute_js on a user-closed window answers as a missing window",
      matches!(r, Some(Ok(Value::Null))),
    );
    win.navigate("about:blank");
    if let Some(call) = held.lock().unwrap().take() {
      call.resolve(Value::Bool(true));
    }
  }
  // A beat for anything queued above to reach the UI thread.
  tokio::time::sleep(Duration::from_millis(300)).await;
  check(
    "calls naming a user-closed window are no-ops (the process is alive)",
    win.get_size() == (0, 0),
  );
}

#[cfg(windows)]
fn os_close_supported() -> bool {
  true
}

/// Posts `WM_CLOSE` -- what the title bar's close button sends -- to the
/// top-level window titled `title`.
#[cfg(windows)]
async fn os_close(title: &str) -> bool {
  use std::ffi::c_void;
  #[link(name = "user32")]
  extern "system" {
    fn FindWindowW(class: *const u16, title: *const u16) -> *mut c_void;
    fn PostMessageW(
      hwnd: *mut c_void,
      msg: u32,
      wparam: usize,
      lparam: isize,
    ) -> i32;
  }
  const WM_CLOSE: u32 = 0x0010;
  let wide: Vec<u16> = title.encode_utf16().chain(Some(0)).collect();
  for _ in 0..100 {
    let hwnd = unsafe { FindWindowW(std::ptr::null(), wide.as_ptr()) };
    if !hwnd.is_null() {
      return unsafe { PostMessageW(hwnd, WM_CLOSE, 0, 0) } != 0;
    }
    tokio::time::sleep(Duration::from_millis(50)).await;
  }
  false
}

/// GTK toplevels are reachable only in a WebKitGTK process (the webview
/// backend); CEF and winit windows are not GTK windows.
#[cfg(target_os = "linux")]
fn os_close_supported() -> bool {
  gtk::available()
}

#[cfg(target_os = "linux")]
async fn os_close(title: &str) -> bool {
  let Some(done) = gtk::close_toplevel(title) else {
    return false;
  };
  wait_for(|| done.lock().unwrap().is_some(), 100, 50).await;
  let found = *done.lock().unwrap();
  found == Some(true)
}

#[cfg(not(any(windows, target_os = "linux")))]
fn os_close_supported() -> bool {
  false
}

#[cfg(not(any(windows, target_os = "linux")))]
async fn os_close(_title: &str) -> bool {
  false
}

/// GTK, looked up in the backend process (this runtime does not link it).
/// GTK is single-threaded, so the close runs from a `g_idle_add` callback on
/// the GTK thread.
#[cfg(target_os = "linux")]
mod gtk {
  use std::ffi::{c_char, c_void, CStr, CString};
  use std::sync::{Arc, Mutex};

  #[link(name = "dl")]
  extern "C" {
    fn dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
  }

  #[repr(C)]
  struct GList {
    data: *mut c_void,
    next: *mut GList,
    prev: *mut GList,
  }

  type IdleFn = unsafe extern "C" fn(*mut c_void) -> i32;

  fn sym(name: &CStr) -> *mut c_void {
    // RTLD_DEFAULT (null on glibc): the process's global scope.
    unsafe { dlsym(std::ptr::null_mut(), name.as_ptr()) }
  }

  pub fn available() -> bool {
    !sym(c"webkit_web_view_new").is_null()
      && !sym(c"gtk_window_close").is_null()
  }

  struct Request {
    title: CString,
    done: Arc<Mutex<Option<bool>>>,
  }

  unsafe extern "C" fn close_on_gtk_thread(data: *mut c_void) -> i32 {
    let req = Box::from_raw(data as *mut Request);
    let list_toplevels: unsafe extern "C" fn() -> *mut GList =
      std::mem::transmute(sym(c"gtk_window_list_toplevels"));
    let list_free: unsafe extern "C" fn(*mut GList) =
      std::mem::transmute(sym(c"g_list_free"));
    let get_title: unsafe extern "C" fn(*mut c_void) -> *const c_char =
      std::mem::transmute(sym(c"gtk_window_get_title"));
    let close: unsafe extern "C" fn(*mut c_void) =
      std::mem::transmute(sym(c"gtk_window_close"));

    let list = list_toplevels();
    let mut target = std::ptr::null_mut();
    let mut node = list;
    while !node.is_null() {
      let title = get_title((*node).data);
      if !title.is_null() && CStr::from_ptr(title) == req.title.as_c_str() {
        target = (*node).data;
        break;
      }
      node = (*node).next;
    }
    list_free(list);
    if !target.is_null() {
      // As the window manager's close button: delete-event, then GTK's
      // default handler destroys the window.
      close(target);
    }
    *req.done.lock().unwrap() = Some(!target.is_null());
    0 // G_SOURCE_REMOVE
  }

  /// Schedules `gtk_window_close` on the toplevel titled `title`. The result
  /// becomes `Some(found)` once it ran.
  pub fn close_toplevel(title: &str) -> Option<Arc<Mutex<Option<bool>>>> {
    let idle_add = sym(c"g_idle_add");
    if idle_add.is_null() {
      return None;
    }
    let idle_add: unsafe extern "C" fn(IdleFn, *mut c_void) -> u32 =
      unsafe { std::mem::transmute(idle_add) };
    let done = Arc::new(Mutex::new(None));
    let req = Box::new(Request {
      title: CString::new(title).ok()?,
      done: done.clone(),
    });
    unsafe { idle_add(close_on_gtk_thread, Box::into_raw(req) as *mut c_void) };
    Some(done)
  }
}
