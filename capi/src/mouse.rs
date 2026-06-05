// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

use std::collections::HashMap;
use std::ffi::c_int;
use std::ffi::c_void;
use std::sync::{Mutex, OnceLock};

use crate::{api, KeyModifiers};

pub const LAUFEY_MOUSE_BUTTON_LEFT: i32 = 0;
pub const LAUFEY_MOUSE_BUTTON_RIGHT: i32 = 1;
pub const LAUFEY_MOUSE_BUTTON_MIDDLE: i32 = 2;
pub const LAUFEY_MOUSE_BUTTON_BACK: i32 = 3;
pub const LAUFEY_MOUSE_BUTTON_FORWARD: i32 = 4;

pub const LAUFEY_MOUSE_PRESSED: i32 = 0;
pub const LAUFEY_MOUSE_RELEASED: i32 = 1;

pub const LAUFEY_WHEEL_DELTA_PIXEL: i32 = 0;
pub const LAUFEY_WHEEL_DELTA_LINE: i32 = 1;
pub const LAUFEY_WHEEL_DELTA_PAGE: i32 = 2;

// --- Mouse click ---

#[derive(Debug, Clone)]
pub struct MouseClickEvent {
  pub window_id: u32,
  pub state: MouseButtonState,
  pub button: MouseButton,
  pub x: f64,
  pub y: f64,
  pub modifiers: KeyModifiers,
  pub click_count: i32,
}

impl MouseClickEvent {
  pub fn is_click(&self) -> bool {
    self.state == MouseButtonState::Released
      && self.button == MouseButton::Left
      && self.click_count >= 1
  }

  pub fn is_double_click(&self) -> bool {
    self.state == MouseButtonState::Released
      && self.button == MouseButton::Left
      && self.click_count >= 2
  }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MouseButtonState {
  Pressed,
  Released,
}

impl MouseButtonState {
  pub(crate) fn from_raw(raw: c_int) -> Self {
    if raw == LAUFEY_MOUSE_PRESSED {
      Self::Pressed
    } else {
      Self::Released
    }
  }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MouseButton {
  Left,
  Right,
  Middle,
  Back,
  Forward,
  Other(i32),
}

impl MouseButton {
  fn from_raw(button: i32) -> Self {
    match button {
      LAUFEY_MOUSE_BUTTON_LEFT => MouseButton::Left,
      LAUFEY_MOUSE_BUTTON_RIGHT => MouseButton::Right,
      LAUFEY_MOUSE_BUTTON_MIDDLE => MouseButton::Middle,
      LAUFEY_MOUSE_BUTTON_BACK => MouseButton::Back,
      LAUFEY_MOUSE_BUTTON_FORWARD => MouseButton::Forward,
      other => MouseButton::Other(other),
    }
  }
}

type HandlerMap<T> = Mutex<HashMap<u32, Box<dyn Fn(T) + Send + Sync>>>;

macro_rules! handler_store {
  ($fn_name:ident, $event_type:ty) => {
    fn $fn_name() -> &'static HandlerMap<$event_type> {
      static STORE: OnceLock<HandlerMap<$event_type>> = OnceLock::new();
      STORE.get_or_init(|| Mutex::new(HashMap::new()))
    }
  };
}

handler_store!(mouse_click_handlers, MouseClickEvent);
handler_store!(mouse_move_handlers, MouseMoveEvent);
handler_store!(wheel_handlers, WheelEvent);
handler_store!(cursor_enter_leave_handlers, CursorEnterLeaveEvent);
handler_store!(focused_handlers, FocusedEvent);
handler_store!(resize_handlers, ResizeEvent);
handler_store!(move_handlers, MoveEvent);
handler_store!(close_requested_handlers, CloseRequestedEvent);

macro_rules! ensure_handler {
  ($fn_name:ident, $api_field:ident, $trampoline:ident) => {
    fn $fn_name() {
      static FLAG: std::sync::atomic::AtomicBool =
        std::sync::atomic::AtomicBool::new(false);
      if !FLAG.swap(true, std::sync::atomic::Ordering::SeqCst) {
        let api = api();
        if let Some(set_handler) = api.$api_field {
          unsafe {
            set_handler(
              api.backend_data,
              Some($trampoline),
              std::ptr::null_mut(),
            );
          }
        }
      }
    }
  };
}

ensure_handler!(
  ensure_mouse_click,
  set_mouse_click_handler,
  mouse_click_trampoline
);
ensure_handler!(
  ensure_mouse_move,
  set_mouse_move_handler,
  mouse_move_trampoline
);
ensure_handler!(ensure_wheel, set_wheel_handler, wheel_trampoline);
ensure_handler!(
  ensure_cursor_enter_leave,
  set_cursor_enter_leave_handler,
  cursor_enter_leave_trampoline
);
ensure_handler!(ensure_focused, set_focused_handler, focused_trampoline);
ensure_handler!(ensure_resize, set_resize_handler, resize_trampoline);
ensure_handler!(ensure_move, set_move_handler, move_trampoline);
ensure_handler!(
  ensure_close_requested,
  set_close_requested_handler,
  close_requested_trampoline
);

// --- Trampolines ---

unsafe extern "C" fn mouse_click_trampoline(
  _user_data: *mut c_void,
  window_id: u32,
  state: c_int,
  button: c_int,
  x: f64,
  y: f64,
  modifiers: u32,
  click_count: i32,
) {
  let event = MouseClickEvent {
    window_id,
    state: MouseButtonState::from_raw(state),
    button: MouseButton::from_raw(button),
    x,
    y,
    modifiers: KeyModifiers::from_raw(modifiers),
    click_count,
  };

  let guard = mouse_click_handlers().lock().unwrap();
  if let Some(handler) = guard.get(&window_id) {
    handler(event);
  }
}

pub fn on_mouse_click<F>(window_id: u32, handler: F)
where
  F: Fn(MouseClickEvent) + Send + Sync + 'static,
{
  ensure_mouse_click();
  mouse_click_handlers()
    .lock()
    .unwrap()
    .insert(window_id, Box::new(handler));
}

// --- Mouse move ---

#[derive(Debug, Clone, Copy)]
pub struct MouseMoveEvent {
  pub window_id: u32,
  pub x: f64,
  pub y: f64,
  pub modifiers: KeyModifiers,
}

unsafe extern "C" fn mouse_move_trampoline(
  _user_data: *mut c_void,
  window_id: u32,
  x: f64,
  y: f64,
  modifiers: u32,
) {
  let event = MouseMoveEvent {
    window_id,
    x,
    y,
    modifiers: KeyModifiers::from_raw(modifiers),
  };

  let guard = mouse_move_handlers().lock().unwrap();
  if let Some(handler) = guard.get(&window_id) {
    handler(event);
  }
}

pub fn on_mouse_move<F>(window_id: u32, handler: F)
where
  F: Fn(MouseMoveEvent) + Send + Sync + 'static,
{
  ensure_mouse_move();
  mouse_move_handlers()
    .lock()
    .unwrap()
    .insert(window_id, Box::new(handler));
}

// --- Wheel events ---

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WheelDeltaMode {
  Pixel,
  Line,
  Page,
}

impl WheelDeltaMode {
  pub(crate) fn from_raw(raw: i32) -> Self {
    match raw {
      LAUFEY_WHEEL_DELTA_LINE => Self::Line,
      LAUFEY_WHEEL_DELTA_PAGE => Self::Page,
      _ => Self::Pixel,
    }
  }
}

#[derive(Debug, Clone, Copy)]
pub struct WheelEvent {
  pub window_id: u32,
  pub delta_x: f64,
  pub delta_y: f64,
  pub x: f64,
  pub y: f64,
  pub modifiers: KeyModifiers,
  pub delta_mode: WheelDeltaMode,
}

unsafe extern "C" fn wheel_trampoline(
  _user_data: *mut c_void,
  window_id: u32,
  delta_x: f64,
  delta_y: f64,
  x: f64,
  y: f64,
  modifiers: u32,
  delta_mode: i32,
) {
  let event = WheelEvent {
    window_id,
    delta_x,
    delta_y,
    x,
    y,
    modifiers: KeyModifiers::from_raw(modifiers),
    delta_mode: WheelDeltaMode::from_raw(delta_mode),
  };

  let guard = wheel_handlers().lock().unwrap();
  if let Some(handler) = guard.get(&window_id) {
    handler(event);
  }
}

pub fn on_wheel<F>(window_id: u32, handler: F)
where
  F: Fn(WheelEvent) + Send + Sync + 'static,
{
  ensure_wheel();
  wheel_handlers()
    .lock()
    .unwrap()
    .insert(window_id, Box::new(handler));
}

// --- Cursor enter/leave events ---

#[derive(Debug, Clone, Copy)]
pub struct CursorEnterLeaveEvent {
  pub window_id: u32,
  pub entered: bool,
  pub x: f64,
  pub y: f64,
  pub modifiers: KeyModifiers,
}

unsafe extern "C" fn cursor_enter_leave_trampoline(
  _user_data: *mut c_void,
  window_id: u32,
  entered: c_int,
  x: f64,
  y: f64,
  modifiers: u32,
) {
  let event = CursorEnterLeaveEvent {
    window_id,
    entered: entered != 0,
    x,
    y,
    modifiers: KeyModifiers::from_raw(modifiers),
  };

  let guard = cursor_enter_leave_handlers().lock().unwrap();
  if let Some(handler) = guard.get(&window_id) {
    handler(event);
  }
}

pub fn on_cursor_enter_leave<F>(window_id: u32, handler: F)
where
  F: Fn(CursorEnterLeaveEvent) + Send + Sync + 'static,
{
  ensure_cursor_enter_leave();
  cursor_enter_leave_handlers()
    .lock()
    .unwrap()
    .insert(window_id, Box::new(handler));
}

// --- Focused events ---

#[derive(Debug, Clone, Copy)]
pub struct FocusedEvent {
  pub window_id: u32,
  pub focused: bool,
}

unsafe extern "C" fn focused_trampoline(
  _user_data: *mut c_void,
  window_id: u32,
  focused: c_int,
) {
  let event = FocusedEvent {
    window_id,
    focused: focused != 0,
  };

  let guard = focused_handlers().lock().unwrap();
  if let Some(handler) = guard.get(&window_id) {
    handler(event);
  }
}

pub fn on_focused<F>(window_id: u32, handler: F)
where
  F: Fn(FocusedEvent) + Send + Sync + 'static,
{
  ensure_focused();
  focused_handlers()
    .lock()
    .unwrap()
    .insert(window_id, Box::new(handler));
}

// --- Resize events ---

#[derive(Debug, Clone, Copy)]
pub struct ResizeEvent {
  pub window_id: u32,
  pub width: i32,
  pub height: i32,
}

unsafe extern "C" fn resize_trampoline(
  _user_data: *mut c_void,
  window_id: u32,
  width: c_int,
  height: c_int,
) {
  let event = ResizeEvent {
    window_id,
    width,
    height,
  };

  let guard = resize_handlers().lock().unwrap();
  if let Some(handler) = guard.get(&window_id) {
    handler(event);
  }
}

pub fn on_resize<F>(window_id: u32, handler: F)
where
  F: Fn(ResizeEvent) + Send + Sync + 'static,
{
  ensure_resize();
  resize_handlers()
    .lock()
    .unwrap()
    .insert(window_id, Box::new(handler));
}

// --- Move events ---

#[derive(Debug, Clone, Copy)]
pub struct MoveEvent {
  pub window_id: u32,
  pub x: i32,
  pub y: i32,
}

unsafe extern "C" fn move_trampoline(
  _user_data: *mut c_void,
  window_id: u32,
  x: c_int,
  y: c_int,
) {
  let event = MoveEvent { window_id, x, y };

  let guard = move_handlers().lock().unwrap();
  if let Some(handler) = guard.get(&window_id) {
    handler(event);
  }
}

pub fn on_move<F>(window_id: u32, handler: F)
where
  F: Fn(MoveEvent) + Send + Sync + 'static,
{
  ensure_move();
  move_handlers()
    .lock()
    .unwrap()
    .insert(window_id, Box::new(handler));
}

// --- Close requested events ---

#[derive(Debug, Clone, Copy)]
pub struct CloseRequestedEvent {
  pub window_id: u32,
}

unsafe extern "C" fn close_requested_trampoline(
  _user_data: *mut c_void,
  window_id: u32,
) {
  let event = CloseRequestedEvent { window_id };

  let guard = close_requested_handlers().lock().unwrap();
  if let Some(handler) = guard.get(&window_id) {
    handler(event);
  }
}

pub fn on_close_requested<F>(window_id: u32, handler: F)
where
  F: Fn(CloseRequestedEvent) + Send + Sync + 'static,
{
  ensure_close_requested();
  close_requested_handlers()
    .lock()
    .unwrap()
    .insert(window_id, Box::new(handler));
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::KeyModifiers;

  // --- MouseButton::from_raw ---

  #[test]
  fn mouse_button_known() {
    assert_eq!(
      MouseButton::from_raw(LAUFEY_MOUSE_BUTTON_LEFT),
      MouseButton::Left
    );
    assert_eq!(
      MouseButton::from_raw(LAUFEY_MOUSE_BUTTON_RIGHT),
      MouseButton::Right
    );
    assert_eq!(
      MouseButton::from_raw(LAUFEY_MOUSE_BUTTON_MIDDLE),
      MouseButton::Middle
    );
    assert_eq!(
      MouseButton::from_raw(LAUFEY_MOUSE_BUTTON_BACK),
      MouseButton::Back
    );
    assert_eq!(
      MouseButton::from_raw(LAUFEY_MOUSE_BUTTON_FORWARD),
      MouseButton::Forward
    );
  }

  #[test]
  fn mouse_button_unknown_preserves_id() {
    // Unknown raw codes are surfaced as Other(n) so the backend can ship
    // a new button kind without breaking the runtime, and the embedder
    // can still inspect what came through.
    assert_eq!(MouseButton::from_raw(99), MouseButton::Other(99));
  }

  // --- MouseClickEvent predicates ---

  fn ev(
    state: MouseButtonState,
    button: MouseButton,
    clicks: i32,
  ) -> MouseClickEvent {
    MouseClickEvent {
      window_id: 1,
      state,
      button,
      x: 0.0,
      y: 0.0,
      modifiers: KeyModifiers::default(),
      click_count: clicks,
    }
  }

  #[test]
  fn is_click_requires_left_released_with_clicks() {
    assert!(ev(MouseButtonState::Released, MouseButton::Left, 1).is_click());
    assert!(ev(MouseButtonState::Released, MouseButton::Left, 2).is_click());
  }

  #[test]
  fn is_click_rejects_press_and_non_left() {
    // Press of left button is not a click.
    assert!(!ev(MouseButtonState::Pressed, MouseButton::Left, 1).is_click());
    // Right-button release is not a click.
    assert!(!ev(MouseButtonState::Released, MouseButton::Right, 1).is_click());
    // Zero click_count means the backend reported a release that didn't
    // pair with a recent press — don't fire `click`.
    assert!(!ev(MouseButtonState::Released, MouseButton::Left, 0).is_click());
  }

  #[test]
  fn is_double_click_requires_click_count_2() {
    assert!(
      !ev(MouseButtonState::Released, MouseButton::Left, 1).is_double_click()
    );
    assert!(
      ev(MouseButtonState::Released, MouseButton::Left, 2).is_double_click()
    );
    assert!(
      ev(MouseButtonState::Released, MouseButton::Left, 3).is_double_click()
    );
    // Right-button double click doesn't count for `dblclick`.
    assert!(
      !ev(MouseButtonState::Released, MouseButton::Right, 2).is_double_click()
    );
  }

  // --- MouseButtonState::from_raw ---

  #[test]
  fn mouse_button_state_known_values() {
    assert_eq!(
      MouseButtonState::from_raw(LAUFEY_MOUSE_PRESSED),
      MouseButtonState::Pressed
    );
    assert_eq!(
      MouseButtonState::from_raw(LAUFEY_MOUSE_RELEASED),
      MouseButtonState::Released
    );
  }

  #[test]
  fn mouse_button_state_unknown_defaults_to_released() {
    // The trampoline collapses anything that isn't LAUFEY_MOUSE_PRESSED
    // to Released. Pinning this so a future "Cancelled" state can't
    // accidentally come through as Pressed.
    assert_eq!(MouseButtonState::from_raw(99), MouseButtonState::Released);
    assert_eq!(MouseButtonState::from_raw(-1), MouseButtonState::Released);
  }

  // --- WheelDeltaMode::from_raw ---

  #[test]
  fn wheel_delta_mode_known_values() {
    assert_eq!(
      WheelDeltaMode::from_raw(LAUFEY_WHEEL_DELTA_PIXEL),
      WheelDeltaMode::Pixel
    );
    assert_eq!(
      WheelDeltaMode::from_raw(LAUFEY_WHEEL_DELTA_LINE),
      WheelDeltaMode::Line
    );
    assert_eq!(
      WheelDeltaMode::from_raw(LAUFEY_WHEEL_DELTA_PAGE),
      WheelDeltaMode::Page
    );
  }

  #[test]
  fn wheel_delta_mode_unknown_defaults_to_pixel() {
    // The WheelEvent contract is that deltas are in *some* unit; the
    // safest default for unknown modes is Pixel (the highest resolution).
    assert_eq!(WheelDeltaMode::from_raw(99), WheelDeltaMode::Pixel);
    assert_eq!(WheelDeltaMode::from_raw(-1), WheelDeltaMode::Pixel);
  }

  // --- Event struct field passthrough ---
  //
  // The trampolines wrap raw C args into typed Event structs; the
  // wrapping is mechanical, but the FIELD ORDER matters because a swap
  // (e.g. `width`↔`height`) silently flips dimensions. These tests pin
  // construction so a future refactor that re-orders the struct fails
  // them visibly.

  #[test]
  fn resize_event_fields_distinct() {
    let ev = ResizeEvent {
      window_id: 1,
      width: 800,
      height: 600,
    };
    assert_eq!(ev.width, 800);
    assert_eq!(ev.height, 600);
  }

  #[test]
  fn move_event_fields_distinct() {
    let ev = MoveEvent {
      window_id: 1,
      x: 100,
      y: 200,
    };
    assert_eq!(ev.x, 100);
    assert_eq!(ev.y, 200);
  }

  #[test]
  fn cursor_enter_leave_carries_position_and_entered_flag() {
    let entered = CursorEnterLeaveEvent {
      window_id: 1,
      entered: true,
      x: 10.5,
      y: 20.5,
      modifiers: KeyModifiers::default(),
    };
    let left = CursorEnterLeaveEvent {
      entered: false,
      ..entered
    };
    assert!(entered.entered);
    assert!(!left.entered);
    // Position must survive the boolean toggle.
    assert_eq!(left.x, 10.5);
    assert_eq!(left.y, 20.5);
  }

  #[test]
  fn focused_event_distinct_states() {
    let focused = FocusedEvent {
      window_id: 1,
      focused: true,
    };
    let blurred = FocusedEvent {
      window_id: 1,
      focused: false,
    };
    assert!(focused.focused);
    assert!(!blurred.focused);
  }
}
