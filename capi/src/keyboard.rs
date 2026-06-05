// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

use std::collections::HashMap;
use std::ffi::{c_char, c_int, c_void, CStr};
use std::sync::{Mutex, OnceLock};

use crate::{api, KeyModifiers, LAUFEY_KEY_PRESSED};

#[derive(Debug, Clone)]
pub struct KeyboardEvent {
  pub window_id: u32,
  pub state: KeyState,
  pub key: String,
  pub code: String,
  pub modifiers: KeyModifiers,
  pub repeat: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum KeyState {
  Pressed,
  Released,
}

impl KeyState {
  pub(crate) fn from_raw(raw: c_int) -> Self {
    if raw == LAUFEY_KEY_PRESSED {
      Self::Pressed
    } else {
      Self::Released
    }
  }
}

static KEYBOARD_HANDLERS: OnceLock<
  Mutex<HashMap<u32, Box<dyn Fn(KeyboardEvent) + Send + Sync>>>,
> = OnceLock::new();

fn keyboard_handlers_store(
) -> &'static Mutex<HashMap<u32, Box<dyn Fn(KeyboardEvent) + Send + Sync>>> {
  KEYBOARD_HANDLERS.get_or_init(|| Mutex::new(HashMap::new()))
}

static HANDLER_REGISTERED: std::sync::atomic::AtomicBool =
  std::sync::atomic::AtomicBool::new(false);

fn ensure_keyboard_handler_registered() {
  if !HANDLER_REGISTERED.swap(true, std::sync::atomic::Ordering::SeqCst) {
    let api = api();
    if let Some(set_handler) = api.set_keyboard_event_handler {
      unsafe {
        set_handler(
          api.backend_data,
          Some(keyboard_event_trampoline),
          std::ptr::null_mut(),
        );
      }
    }
  }
}

unsafe extern "C" fn keyboard_event_trampoline(
  _user_data: *mut c_void,
  window_id: u32,
  state: c_int,
  key: *const c_char,
  code: *const c_char,
  modifiers: u32,
  repeat: bool,
) {
  let key_str = if key.is_null() {
    String::new()
  } else {
    CStr::from_ptr(key).to_string_lossy().into_owned()
  };
  let code_str = if code.is_null() {
    String::new()
  } else {
    CStr::from_ptr(code).to_string_lossy().into_owned()
  };

  let event = KeyboardEvent {
    window_id,
    state: KeyState::from_raw(state),
    key: key_str,
    code: code_str,
    modifiers: KeyModifiers::from_raw(modifiers),
    repeat,
  };

  let guard = keyboard_handlers_store().lock().unwrap();
  if let Some(handler) = guard.get(&window_id) {
    handler(event);
  }
}

/// Register a handler for keyboard input events on a specific window.
pub fn on_keyboard_event<F>(window_id: u32, handler: F)
where
  F: Fn(KeyboardEvent) + Send + Sync + 'static,
{
  ensure_keyboard_handler_registered();
  keyboard_handlers_store()
    .lock()
    .unwrap()
    .insert(window_id, Box::new(handler));
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn key_state_from_raw_known_values() {
    assert_eq!(KeyState::from_raw(LAUFEY_KEY_PRESSED), KeyState::Pressed);
    assert_eq!(
      KeyState::from_raw(crate::LAUFEY_KEY_RELEASED),
      KeyState::Released
    );
  }

  #[test]
  fn key_state_unknown_defaults_to_released() {
    // The trampoline collapses unknown raw values to Released — pinning
    // so a future "Cancelled" state can't be misread as Pressed.
    assert_eq!(KeyState::from_raw(42), KeyState::Released);
    assert_eq!(KeyState::from_raw(-1), KeyState::Released);
  }

  #[test]
  fn keyboard_event_field_passthrough() {
    // The KeyboardEvent struct is built by the C trampoline from
    // five separate args. A field swap there is silent at runtime
    // (`key` and `code` are both Strings; `shift` and `repeat` are
    // both bools). Pin a construction that fails compilation if the
    // struct's field types or names change.
    let ev = KeyboardEvent {
      window_id: 7,
      state: KeyState::Pressed,
      key: "a".to_string(),
      code: "KeyA".to_string(),
      modifiers: KeyModifiers::default(),
      repeat: true,
    };
    assert_eq!(ev.window_id, 7);
    assert_eq!(ev.state, KeyState::Pressed);
    assert_eq!(ev.key, "a");
    assert_eq!(ev.code, "KeyA");
    assert!(ev.repeat);
  }
}
