// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Deep links (custom URL schemes) for winit-based backends.
//!
//! macOS only, matching the C ABI contract in `capi/include/laufey.h`: AppKit
//! delivers `acme://…` to the *running* app as an Apple Event, so one process
//! handles every link. Windows and Linux instead spawn a new process with the
//! URL in argv, which only the embedder that owns packaging can turn into
//! "focus the running app" — those platforms leave the ABI pointers NULL.
//!
//! The delivery point is a runtime-added `application:openURLs:` on winit's
//! `NSApplicationDelegate` subclass, the same technique `dock.rs` uses for
//! `applicationDockMenu:`. Because the runtime is loaded on a worker thread
//! (see `load_and_start_runtime`), a launch URL always arrives before any
//! handler is registered, so URLs are buffered until one is.

use std::ffi::{c_char, c_void, CString};
use std::sync::Mutex;

pub type LaufeyOpenUrlFn = unsafe extern "C" fn(*mut c_void, *const c_char);

/// Mirrors `LAUFEY_MAX_PENDING_OPEN_URLS` in `capi/include/laufey.h`.
const MAX_PENDING: usize = 16;

static HANDLER: Mutex<Option<(LaufeyOpenUrlFn, usize)>> = Mutex::new(None);
static PENDING: Mutex<Vec<CString>> = Mutex::new(Vec::new());

/// Add `application:openURLs:` to winit's `NSApplicationDelegate` subclass.
/// Idempotent. Call as early as possible — AppKit only routes a launch URL to
/// a delegate that already responds to the selector, and the runtime that
/// registers the handler starts well after the app finishes launching.
/// Returns false if winit's delegate class couldn't be located.
#[cfg(target_os = "macos")]
pub fn install_delegate_methods() -> bool {
  mac::install_delegate_methods()
}

/// Register (or, with `None`, clear) the open-url handler and flush anything
/// buffered while none was set. Clearing re-arms buffering and leaves the
/// queue intact for the next registration.
pub fn set_handler(handler: Option<(LaufeyOpenUrlFn, usize)>) {
  // Install as early as possible: AppKit only routes the launch URL to a
  // delegate that already responds to the selector.
  #[cfg(target_os = "macos")]
  mac::install_delegate_methods();

  let flushed = {
    let mut slot = HANDLER.lock().unwrap();
    *slot = handler;
    if handler.is_some() {
      std::mem::take(&mut *PENDING.lock().unwrap())
    } else {
      Vec::new()
    }
  };

  // Outside the lock — the handler is embedder code and may re-enter. Note
  // this runs on the registering thread, not the UI thread; it's the one case
  // where an open-url callback fires anywhere else.
  if let Some((callback, data)) = handler {
    for url in flushed {
      unsafe { callback(data as *mut c_void, url.as_ptr()) };
    }
  }
}

/// Deliver `url` to the registered handler, or buffer it until one registers.
pub fn fire(url: &str) {
  // A URL with an interior NUL can't cross the C ABI; drop it rather than
  // truncating into a different URL than the user clicked.
  let Ok(c_url) = CString::new(url) else {
    return;
  };

  let handler = {
    let slot = HANDLER.lock().unwrap();
    match *slot {
      Some(handler) => Some(handler),
      None => {
        // Push while still holding HANDLER, so a concurrent set_handler
        // can't flush an empty queue and strand this URL.
        let mut pending = PENDING.lock().unwrap();
        if pending.len() >= MAX_PENDING {
          // Drop the oldest: the most recent link is the one the user is
          // waiting on.
          pending.remove(0);
        }
        pending.push(c_url.clone());
        None
      }
    }
  };

  if let Some((callback, data)) = handler {
    unsafe { callback(data as *mut c_void, c_url.as_ptr()) };
  }
}

/// Backs the `test_trigger_open_url` C ABI hook. Returns true if a handler
/// consumed the URL, false if it was buffered for a later registration.
pub fn test_trigger(url: &str) -> bool {
  let delivered = HANDLER.lock().unwrap().is_some();
  fire(url);
  delivered
}

#[cfg(target_os = "macos")]
mod mac {
  use objc2::runtime::{AnyClass, AnyObject, Imp, Sel};
  use objc2::sel;
  use objc2_foundation::{NSArray, NSURL};
  use std::sync::Once;

  /// Runtime-add `application:openURLs:` to winit's `NSApplicationDelegate`
  /// subclass. Idempotent; safe to call repeatedly. Returns false if the
  /// delegate class couldn't be located.
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
          "laufey: failed to install the open-url delegate method on winit's \
           NSApplicationDelegate — deep links will not fire. This likely \
           means winit changed its delegate class name."
        );
      }
    });
    // SAFETY: once initialized by Once, the value is stable.
    unsafe { INSTALLED }
  }

  fn try_install() -> bool {
    // Same class winit 0.30 uses for the dock methods; see dock.rs.
    let Some(cls) = AnyClass::get(c"WinitApplicationDelegate") else {
      return false;
    };

    // Cast to *mut to allow runtime modification.
    let raw_cls = cls as *const _ as *mut _;

    // SAFETY: the trampoline's real signature matches what AppKit invokes
    // for this selector — (void)(id, SEL, NSApplication*, NSArray*) — which
    // is what the "v@:@@" encoding declares.
    unsafe {
      let imp: Imp =
        std::mem::transmute(application_open_urls_imp as *const ());
      // class_addMethod returns false if the selector already exists —
      // silently OK: a future winit may implement it itself.
      let _ = objc2::ffi::class_addMethod(
        raw_cls,
        sel!(application:openURLs:),
        imp,
        c"v@:@@".as_ptr(),
      );
    }
    true
  }

  /// Trampoline:
  /// `- (void)application:(NSApplication*)app openURLs:(NSArray<NSURL*>*)urls`
  unsafe extern "C-unwind" fn application_open_urls_imp(
    _self: *mut AnyObject,
    _sel: Sel,
    _app: *mut AnyObject,
    urls: *mut AnyObject,
  ) {
    // SAFETY: AppKit passes a non-null NSArray<NSURL*> for this selector;
    // the null check covers a malformed caller.
    let urls = urls.cast::<NSArray<NSURL>>();
    let Some(urls) = (unsafe { urls.as_ref() }) else {
      return;
    };
    for index in 0..urls.count() {
      let url = urls.objectAtIndex(index);
      if let Some(absolute) = url.absoluteString() {
        super::fire(&absolute.to_string());
      }
    }
  }
}
