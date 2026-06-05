// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

use std::collections::HashMap;
use std::error::Error;
use std::ffi::c_void;
use std::sync::atomic::{AtomicPtr, Ordering};

use laufey_backend_winit_common::{
  define_common_backend_fns, fill_common_api, winit, BackendAccess,
  CommonEvent, CommonState, LaufeyBackendApi, LaufeyJsResultFn,
};
use winit::application::ApplicationHandler;
use winit::dpi::{PhysicalPosition, PhysicalSize};
use winit::event::WindowEvent;
use winit::event_loop::{EventLoop, EventLoopProxy};
use winit::keyboard::ModifiersState;
use winit::window::Window;

// --- Backend state ---

static BACKEND_STATE: AtomicPtr<BackendState> =
  AtomicPtr::new(std::ptr::null_mut());

struct BackendState {
  event_proxy: EventLoopProxy<UserEvent>,
  common: CommonState,
}

impl BackendAccess for BackendState {
  type Event = UserEvent;

  fn get() -> Option<&'static Self> {
    let ptr = BACKEND_STATE.load(Ordering::Acquire);
    if ptr.is_null() {
      None
    } else {
      Some(unsafe { &*ptr })
    }
  }

  fn proxy(&self) -> &EventLoopProxy<UserEvent> {
    &self.event_proxy
  }

  fn common(&self) -> &CommonState {
    &self.common
  }

  fn common_event(event: CommonEvent) -> UserEvent {
    UserEvent::Common(event)
  }
}

// Generate all common backend C ABI functions
define_common_backend_fns!(BackendState);

// --- Backend-specific functions ---

unsafe extern "C" fn backend_navigate(
  _data: *mut c_void,
  _window_id: u32,
  _url: *const std::ffi::c_char,
) {
  // no-op — no web engine
}

unsafe extern "C" fn backend_execute_js(
  _data: *mut c_void,
  _window_id: u32,
  _script: *const std::ffi::c_char,
  _callback: Option<LaufeyJsResultFn>,
  _callback_data: *mut c_void,
) {
  // no-op — no web engine
}

// --- API construction ---

fn create_backend_api() -> LaufeyBackendApi {
  let mut api = laufey_backend_winit_common::create_api_base();
  fill_common_api!(api);
  api.navigate = Some(backend_navigate);
  api.execute_js = Some(backend_execute_js);
  api
}

// --- Event loop ---

#[derive(Debug)]
enum UserEvent {
  Common(CommonEvent),
}

struct WindowInfo {
  window: Window,
  modifiers: ModifiersState,
}

struct App {
  // Map from our window_id to winit WindowId + Window
  windows: HashMap<u32, WindowInfo>,
  // Reverse map from winit WindowId to our window_id
  winit_to_laufey: HashMap<winit::window::WindowId, u32>,
  // Most recently focused LAUFEY window (for app-scoped dock ops on Windows/Linux).
  focused_laufey_id: Option<u32>,
}

impl App {
  fn new() -> Self {
    Self {
      windows: HashMap::new(),
      winit_to_laufey: HashMap::new(),
      focused_laufey_id: None,
    }
  }

  fn focused_window(&self) -> Option<(&Window, u32)> {
    let id = self
      .focused_laufey_id
      .or_else(|| self.windows.keys().next().copied())?;
    self.windows.get(&id).map(|info| (&info.window, id))
  }

  fn create_window(
    &mut self,
    event_loop: &winit::event_loop::ActiveEventLoop,
    window_id: u32,
  ) {
    let state = BackendState::get().expect("BackendState not initialized");
    let attrs = state
      .common
      .with_window(window_id, |ws| {
        laufey_backend_winit_common::apply_pending_attrs(
          ws,
          Window::default_attributes(),
        )
      })
      .unwrap_or_else(Window::default_attributes);

    let window = event_loop
      .create_window(attrs)
      .expect("Failed to create winit Window");

    state.common.with_window(window_id, |ws| {
      laufey_backend_winit_common::apply_pending_post_create(ws, &window);
    });
    laufey_backend_winit_common::store_window_handles(window_id, &window);

    let winit_id = window.id();
    self.winit_to_laufey.insert(winit_id, window_id);
    self.windows.insert(
      window_id,
      WindowInfo {
        window,
        modifiers: ModifiersState::default(),
      },
    );
  }

  fn close_window(&mut self, window_id: u32) {
    if let Some(info) = self.windows.remove(&window_id) {
      self.winit_to_laufey.remove(&info.window.id());
      laufey_backend_winit_common::remove_window_handles(window_id);
      if let Some(state) = BackendState::get() {
        state.common.remove_window(window_id);
      }
    }
  }

  fn laufey_id(&self, winit_id: winit::window::WindowId) -> Option<u32> {
    self.winit_to_laufey.get(&winit_id).copied()
  }
}

impl ApplicationHandler<UserEvent> for App {
  fn resumed(&mut self, _event_loop: &winit::event_loop::ActiveEventLoop) {
    // Windows are created on-demand via CreateWindow events
  }

  fn user_event(
    &mut self,
    event_loop: &winit::event_loop::ActiveEventLoop,
    event: UserEvent,
  ) {
    match event {
      UserEvent::Common(ref common) => match common {
        CommonEvent::Quit => {
          event_loop.exit();
        }
        CommonEvent::CreateWindow { window_id } => {
          self.create_window(event_loop, *window_id);
        }
        CommonEvent::CloseWindow { window_id } => {
          self.close_window(*window_id);
          if self.windows.is_empty() {
            event_loop.exit();
          }
        }
        CommonEvent::UiTask { task, data } => {
          unsafe { task(*data as *mut c_void) };
        }
        CommonEvent::DockTask => {
          laufey_backend_winit_common::dock::drain_and_apply(
            self.focused_window(),
          );
        }
        CommonEvent::TrayTask => {
          laufey_backend_winit_common::tray::drain_and_apply();
        }
        other => {
          let wid = match other {
            CommonEvent::SetTitle { window_id }
            | CommonEvent::SetWindowSize { window_id }
            | CommonEvent::SetWindowPosition { window_id }
            | CommonEvent::SetResizable { window_id }
            | CommonEvent::SetAlwaysOnTop { window_id }
            | CommonEvent::Show { window_id }
            | CommonEvent::Hide { window_id }
            | CommonEvent::Focus { window_id }
            | CommonEvent::SetApplicationMenu { window_id }
            | CommonEvent::ShowContextMenu { window_id } => *window_id,
            _ => return,
          };
          if let Some(info) = self.windows.get(&wid) {
            laufey_backend_winit_common::handle_common_event::<BackendState>(
              common,
              wid,
              &info.window,
            );
          }
        }
      },
    }
  }

  fn about_to_wait(&mut self, event_loop: &winit::event_loop::ActiveEventLoop) {
    laufey_backend_winit_common::poll_menu_events();
    // The tray lives on the primary monitor (menu bar / taskbar); its scale
    // factor converts tray-icon's physical rect into the logical window space.
    let scale_factor = event_loop
      .primary_monitor()
      .map(|m| m.scale_factor())
      .unwrap_or(1.0);
    laufey_backend_winit_common::tray::poll_tray_events(scale_factor);
  }

  fn window_event(
    &mut self,
    event_loop: &winit::event_loop::ActiveEventLoop,
    winit_window_id: winit::window::WindowId,
    event: WindowEvent,
  ) {
    let laufey_id = match self.laufey_id(winit_window_id) {
      Some(id) => id,
      None => return,
    };

    let state = match BackendState::get() {
      Some(s) => s,
      None => return,
    };

    let modifiers = match self.windows.get_mut(&laufey_id) {
      Some(info) => &mut info.modifiers,
      None => return,
    };

    match event {
      WindowEvent::CloseRequested => {
        laufey_backend_winit_common::dispatch_close_requested_event(
          &state.common.handlers,
          laufey_id,
        );
        self.close_window(laufey_id);
        if self.windows.is_empty() {
          event_loop.exit();
        }
      }
      WindowEvent::Resized(PhysicalSize { width, height }) => {
        state.common.with_window(laufey_id, |ws| {
          *ws.pending_size.lock().unwrap() =
            Some((width as i32, height as i32));
        });
        laufey_backend_winit_common::dispatch_resize_event(
          &state.common.handlers,
          laufey_id,
          width as i32,
          height as i32,
        );
      }
      WindowEvent::Moved(PhysicalPosition { x, y }) => {
        state.common.with_window(laufey_id, |ws| {
          *ws.pending_position.lock().unwrap() = Some((x, y));
        });
        laufey_backend_winit_common::dispatch_move_event(
          &state.common.handlers,
          laufey_id,
          x,
          y,
        );
      }
      WindowEvent::ModifiersChanged(new_modifiers) => {
        *modifiers = new_modifiers.state();
      }
      WindowEvent::KeyboardInput {
        event: ref key_event,
        ..
      } => {
        laufey_backend_winit_common::dispatch_keyboard_event(
          &state.common.handlers,
          laufey_id,
          key_event,
          *modifiers,
        );
      }
      WindowEvent::CursorMoved { position, .. } => {
        state.common.with_window(laufey_id, |ws| {
          *ws.cursor_position.lock().unwrap() = (position.x, position.y);
        });
        laufey_backend_winit_common::dispatch_mouse_move_event(
          &state.common.handlers,
          laufey_id,
          position.x,
          position.y,
          *modifiers,
        );
      }
      WindowEvent::MouseInput {
        state: button_state,
        button,
        ..
      } => {
        state.common.with_window(laufey_id, |ws| {
          laufey_backend_winit_common::dispatch_mouse_click_event(
            &state.common.handlers,
            ws,
            laufey_id,
            button_state,
            button,
            *modifiers,
          );
        });
      }
      WindowEvent::MouseWheel { delta, .. } => {
        state.common.with_window(laufey_id, |ws| {
          laufey_backend_winit_common::dispatch_wheel_event(
            &state.common.handlers,
            ws,
            laufey_id,
            delta,
            *modifiers,
          );
        });
      }
      WindowEvent::CursorEntered { .. } => {
        state.common.with_window(laufey_id, |ws| {
          laufey_backend_winit_common::dispatch_cursor_enter_leave_event(
            &state.common.handlers,
            ws,
            laufey_id,
            true,
            *modifiers,
          );
        });
      }
      WindowEvent::CursorLeft { .. } => {
        state.common.with_window(laufey_id, |ws| {
          laufey_backend_winit_common::dispatch_cursor_enter_leave_event(
            &state.common.handlers,
            ws,
            laufey_id,
            false,
            *modifiers,
          );
        });
      }
      WindowEvent::Focused(focused) => {
        if focused {
          self.focused_laufey_id = Some(laufey_id);
        } else if self.focused_laufey_id == Some(laufey_id) {
          self.focused_laufey_id = None;
        }
        laufey_backend_winit_common::dispatch_focused_event(
          &state.common.handlers,
          laufey_id,
          focused,
        );
      }

      WindowEvent::ThemeChanged(_) => {}
      WindowEvent::Destroyed => {}
      WindowEvent::DroppedFile(_) => {}
      WindowEvent::HoveredFile(_) => {}
      WindowEvent::HoveredFileCancelled => {}
      WindowEvent::Ime(_) => {}

      WindowEvent::Touch(_)
      | WindowEvent::PinchGesture { .. }
      | WindowEvent::PanGesture { .. }
      | WindowEvent::DoubleTapGesture { .. }
      | WindowEvent::RotationGesture { .. }
      | WindowEvent::TouchpadPressure { .. } => {
        // TODO: touch
      }
      WindowEvent::ActivationTokenDone { .. }
      | WindowEvent::AxisMotion { .. }
      | WindowEvent::ScaleFactorChanged { .. }
      | WindowEvent::Occluded(_)
      | WindowEvent::RedrawRequested => {
        // wont implement
      }
    }
  }
}

fn main() -> Result<(), Box<dyn Error>> {
  let event_loop = EventLoop::with_user_event()
    .build()
    .expect("Failed to create EventLoop");

  let backend_state = Box::new(BackendState {
    event_proxy: event_loop.create_proxy(),
    common: CommonState::new(),
  });
  BACKEND_STATE.store(Box::into_raw(backend_state), Ordering::Release);

  laufey_backend_winit_common::load_and_start_runtime(create_backend_api());

  let mut app = App::new();
  Ok(event_loop.run_app(&mut app)?)
}
