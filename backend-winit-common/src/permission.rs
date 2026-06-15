// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Runtime authorization for winit-based backends.
//!
//! macOS: drives `UNUserNotificationCenter` to query / request
//! authorization for desktop notifications. Falls back to `UNSUPPORTED`
//! when running outside a bundled `.app` (no CFBundleIdentifier), since
//! the OS rejects the request immediately in that case.
//!
//! Windows / Linux: `notify-rust` targets shell-level notification APIs
//! that have no permission model, so we report `GRANTED` synchronously
//! for `LAUFEY_PERMISSION_NOTIFICATIONS` and `UNSUPPORTED` for any other
//! kind.

use std::ffi::{c_int, c_void};

pub const LAUFEY_PERMISSION_INVALID: c_int = 0;
pub const LAUFEY_PERMISSION_NOTIFICATIONS: c_int = 1;

pub const LAUFEY_PERMISSION_STATUS_GRANTED: c_int = 0;
pub const LAUFEY_PERMISSION_STATUS_DENIED: c_int = 1;
pub const LAUFEY_PERMISSION_STATUS_PROMPT: c_int = 2;
pub const LAUFEY_PERMISSION_STATUS_UNSUPPORTED: c_int = 3;

pub type LaufeyPermissionCallbackFn = unsafe extern "C" fn(*mut c_void, c_int);

#[cfg(not(target_os = "macos"))]
fn fire(
  cb: Option<LaufeyPermissionCallbackFn>,
  ud: *mut c_void,
  status: c_int,
) {
  if let Some(cb) = cb {
    unsafe { cb(ud, status) };
  }
}

#[cfg(target_os = "macos")]
mod imp {
  use super::*;
  use block2::RcBlock;
  use objc2::rc::Retained;
  use objc2::runtime::Bool;
  use objc2_foundation::{NSBundle, NSError};
  use objc2_user_notifications::{
    UNAuthorizationOptions, UNAuthorizationStatus, UNNotificationSettings,
    UNUserNotificationCenter,
  };

  fn map_status(s: UNAuthorizationStatus) -> c_int {
    match s {
      UNAuthorizationStatus::NotDetermined => LAUFEY_PERMISSION_STATUS_PROMPT,
      UNAuthorizationStatus::Denied => LAUFEY_PERMISSION_STATUS_DENIED,
      UNAuthorizationStatus::Authorized
      | UNAuthorizationStatus::Provisional
      | UNAuthorizationStatus::Ephemeral => LAUFEY_PERMISSION_STATUS_GRANTED,
      _ => LAUFEY_PERMISSION_STATUS_UNSUPPORTED,
    }
  }

  fn process_is_bundled() -> bool {
    let mb = NSBundle::mainBundle();
    if mb.bundleIdentifier().is_none() {
      return false;
    }
    mb.bundlePath().to_string().ends_with(".app")
  }

  // Hop the callback onto the main thread via dispatch_async. UN
  // completion handlers fire on a background queue; we promise the laufey
  // ABI to deliver on the UI thread, so route through libdispatch.
  fn fire_on_main(
    cb: Option<LaufeyPermissionCallbackFn>,
    ud: *mut c_void,
    status: c_int,
  ) {
    let Some(cb) = cb else { return };
    let ud_addr = ud as usize;
    dispatch2::DispatchQueue::main().exec_async(move || {
      unsafe { cb(ud_addr as *mut c_void, status) };
    });
  }

  pub(super) fn query(
    kind: c_int,
    cb: Option<LaufeyPermissionCallbackFn>,
    user_data: *mut c_void,
  ) {
    if kind != LAUFEY_PERMISSION_NOTIFICATIONS || !process_is_bundled() {
      fire_on_main(cb, user_data, LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
      return;
    }
    let center = UNUserNotificationCenter::currentNotificationCenter();
    let ud_addr = user_data as usize;
    let block = RcBlock::new(
      move |settings: ::core::ptr::NonNull<UNNotificationSettings>| {
        let s = unsafe { settings.as_ref() };
        let status = map_status(s.authorizationStatus());
        fire_on_main(cb, ud_addr as *mut c_void, status);
      },
    );
    center.getNotificationSettingsWithCompletionHandler(&block);
  }

  pub(super) fn request(
    kind: c_int,
    cb: Option<LaufeyPermissionCallbackFn>,
    user_data: *mut c_void,
  ) {
    if kind != LAUFEY_PERMISSION_NOTIFICATIONS || !process_is_bundled() {
      fire_on_main(cb, user_data, LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
      return;
    }
    let center = UNUserNotificationCenter::currentNotificationCenter();
    let opts = UNAuthorizationOptions::Alert
      | UNAuthorizationOptions::Sound
      | UNAuthorizationOptions::Badge;
    let ud_addr = user_data as usize;
    let center_clone: Retained<UNUserNotificationCenter> = center.clone();
    let outer = RcBlock::new(move |granted: Bool, _err: *mut NSError| {
      // After the prompt resolves, fetch the resulting settings to
      // map through map_status. This collapses Provisional/Ephemeral
      // to GRANTED consistently with the CEF/WebView paths.
      let ud = ud_addr;
      let granted_b = granted.as_bool();
      let inner = RcBlock::new(
        move |settings: ::core::ptr::NonNull<UNNotificationSettings>| {
          let s = unsafe { settings.as_ref() };
          let auth = s.authorizationStatus();
          let status = if granted_b {
            map_status(auth)
          } else if auth == UNAuthorizationStatus::NotDetermined {
            LAUFEY_PERMISSION_STATUS_DENIED
          } else {
            map_status(auth)
          };
          fire_on_main(cb, ud as *mut c_void, status);
        },
      );
      center_clone.getNotificationSettingsWithCompletionHandler(&inner);
    });
    center.requestAuthorizationWithOptions_completionHandler(opts, &outer);
  }
}

#[cfg(not(target_os = "macos"))]
mod imp {
  use super::*;

  pub(super) fn query(
    kind: c_int,
    cb: Option<LaufeyPermissionCallbackFn>,
    user_data: *mut c_void,
  ) {
    let status = if kind == LAUFEY_PERMISSION_NOTIFICATIONS {
      LAUFEY_PERMISSION_STATUS_GRANTED
    } else {
      LAUFEY_PERMISSION_STATUS_UNSUPPORTED
    };
    fire(cb, user_data, status);
  }

  pub(super) fn request(
    kind: c_int,
    cb: Option<LaufeyPermissionCallbackFn>,
    user_data: *mut c_void,
  ) {
    query(kind, cb, user_data);
  }
}

/// Implementation of the `query_permission` ABI entry point.
///
/// # Safety
/// `cb` must be a valid C function pointer or `None`; `user_data` is
/// passed through to it unchanged.
pub unsafe fn query_permission(
  kind: c_int,
  cb: Option<LaufeyPermissionCallbackFn>,
  user_data: *mut c_void,
) {
  imp::query(kind, cb, user_data);
}

/// Implementation of the `request_permission` ABI entry point.
///
/// # Safety
/// `cb` must be a valid C function pointer or `None`; `user_data` is
/// passed through to it unchanged.
pub unsafe fn request_permission(
  kind: c_int,
  cb: Option<LaufeyPermissionCallbackFn>,
  user_data: *mut c_void,
) {
  imp::request(kind, cb, user_data);
}
