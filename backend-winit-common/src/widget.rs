// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Engine-free native widgets for the winit backend.
//!
//! The runtime drives this from its own thread, but native UI toolkits must be
//! touched only on the UI/main thread. So, like the [`crate::dock`] and
//! [`crate::tray`] modules, operations are queued with [`queue_op`] and applied
//! on the main thread by [`drain_and_apply`] (called when the winit backend
//! processes a `WidgetTask` user event).
//!
//! The macOS implementation maps widget kinds to AppKit views hosted in a
//! window's content view (`NSView`): stacks -> `NSStackView`, labels ->
//! `NSTextField`, buttons -> `NSButton`.

use std::ffi::c_void;
use std::os::raw::c_int;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Mutex;

static NEXT_WIDGET_ID: AtomicU32 = AtomicU32::new(1);

/// Allocate a fresh, process-unique widget id.
pub fn allocate_widget_id() -> u32 {
  NEXT_WIDGET_ID.fetch_add(1, Ordering::Relaxed)
}

/// Callback fired when a widget is activated (e.g. a button click).
pub type LaufeyWidgetClickFn = unsafe extern "C" fn(*mut c_void, u32);

/// Commands queued from the runtime thread and applied on the main thread.
pub enum WidgetOp {
  Create {
    widget_id: u32,
    window_id: u32,
    kind: c_int,
  },
  SetText {
    widget_id: u32,
    text: String,
  },
  AddChild {
    parent_id: u32,
    child_id: u32,
  },
  SetRoot {
    window_id: u32,
    widget_id: u32,
  },
}

static WIDGET_QUEUE: Mutex<Vec<WidgetOp>> = Mutex::new(Vec::new());
static CLICK_HANDLER: Mutex<Option<(LaufeyWidgetClickFn, usize)>> =
  Mutex::new(None);

pub fn queue_op(op: WidgetOp) {
  WIDGET_QUEUE.lock().unwrap().push(op);
}

/// Register (or clear) the global widget-click handler. Safe to call from any
/// thread — only a function pointer and its user-data are stored.
pub fn set_click_handler(handler: Option<LaufeyWidgetClickFn>, user_data: usize) {
  *CLICK_HANDLER.lock().unwrap() = handler.map(|f| (f, user_data));
}

/// Drain queued ops and apply them. Must be called on the main thread.
pub fn drain_and_apply() {
  let ops: Vec<WidgetOp> = {
    let mut q = WIDGET_QUEUE.lock().unwrap();
    std::mem::take(&mut *q)
  };
  for op in ops {
    apply(op);
  }
}

#[allow(dead_code)]
fn dispatch_click(widget_id: u32) {
  let handler = *CLICK_HANDLER.lock().unwrap();
  if let Some((f, user_data)) = handler {
    unsafe { f(user_data as *mut c_void, widget_id) };
  }
}

#[cfg(target_os = "macos")]
fn apply(op: WidgetOp) {
  imp::apply(op);
}

#[cfg(not(target_os = "macos"))]
fn apply(_op: WidgetOp) {
  // Native widgets are macOS-only for now (Linux/Windows TODO).
}

#[cfg(target_os = "macos")]
mod imp {
  use super::WidgetOp;
  use std::cell::RefCell;
  use std::collections::HashMap;

  use objc2::rc::Retained;
  use objc2::runtime::{AnyObject, NSObject};
  use objc2::{
    define_class, msg_send, sel, AllocAnyThread, DefinedClass,
  };
  use objc2_app_kit::{
    NSAutoresizingMaskOptions, NSButton, NSStackView, NSTextField,
    NSUserInterfaceLayoutOrientation, NSView,
  };
  use objc2_foundation::{MainThreadMarker, NSString};

  /// Ivars for the per-button target object.
  struct TargetIvars {
    widget_id: u32,
  }

  define_class!(
    // The button's target: a tiny NSObject subclass that maps an AppKit
    // action back to the widget id, then into the registered handler.
    #[unsafe(super(NSObject))]
    #[name = "LaufeyWidgetTarget"]
    #[ivars = TargetIvars]
    struct WidgetTarget;

    impl WidgetTarget {
      #[unsafe(method(widgetAction:))]
      fn widget_action(&self, _sender: *mut AnyObject) {
        super::dispatch_click(self.ivars().widget_id);
      }
    }
  );

  impl WidgetTarget {
    fn new(widget_id: u32) -> Retained<Self> {
      let this = Self::alloc().set_ivars(TargetIvars { widget_id });
      unsafe { msg_send![super(this), init] }
    }
  }

  /// A live widget. The concrete AppKit type is retained so the view stays
  /// alive while it is in the tree; a button also retains its action target.
  enum Entry {
    Stack(Retained<NSStackView>),
    Label(Retained<NSTextField>),
    Button(Retained<NSButton>, #[allow(dead_code)] Retained<WidgetTarget>),
  }

  impl Entry {
    fn view(&self) -> &NSView {
      match self {
        Entry::Stack(v) => v,
        Entry::Label(v) => v,
        Entry::Button(v, _) => v,
      }
    }

    fn is_stack(&self) -> bool {
      matches!(self, Entry::Stack(_))
    }
  }

  thread_local! {
    static WIDGETS: RefCell<HashMap<u32, Entry>> = RefCell::new(HashMap::new());
  }

  pub(super) fn apply(op: WidgetOp) {
    // All AppKit work happens on the main thread.
    let Some(mtm) = MainThreadMarker::new() else {
      return;
    };
    match op {
      WidgetOp::Create {
        widget_id,
        window_id: _,
        kind,
      } => {
        let entry = create(mtm, widget_id, kind);
        if let Some(entry) = entry {
          WIDGETS.with(|w| {
            w.borrow_mut().insert(widget_id, entry);
          });
        }
      }
      WidgetOp::SetText { widget_id, text } => {
        let ns = NSString::from_str(&text);
        WIDGETS.with(|w| {
          if let Some(entry) = w.borrow().get(&widget_id) {
            match entry {
              Entry::Label(label) => label.setStringValue(&ns),
              Entry::Button(button, _) => button.setTitle(&ns),
              Entry::Stack(_) => {}
            }
          }
        });
      }
      WidgetOp::AddChild {
        parent_id,
        child_id,
      } => {
        WIDGETS.with(|w| {
          let map = w.borrow();
          let (Some(parent), Some(child)) =
            (map.get(&parent_id), map.get(&child_id))
          else {
            return;
          };
          let parent_view = parent.view();
          let child_view = child.view();
          if parent.is_stack() {
            unsafe {
              let _: () =
                msg_send![parent_view, addArrangedSubview: child_view];
            }
          } else {
            parent_view.addSubview(child_view);
          }
        });
      }
      WidgetOp::SetRoot {
        window_id,
        widget_id,
      } => {
        let content_ptr =
          unsafe { crate::backend_get_window_handle(std::ptr::null_mut(), window_id) };
        if content_ptr.is_null() {
          return;
        }
        let content: &NSView = unsafe { &*(content_ptr as *const NSView) };
        WIDGETS.with(|w| {
          let map = w.borrow();
          let Some(root) = map.get(&widget_id) else {
            return;
          };
          let root_view = root.view();
          let bounds = content.bounds();
          root_view.setFrame(bounds);
          root_view.setAutoresizingMask(
            NSAutoresizingMaskOptions::ViewWidthSizable
              | NSAutoresizingMaskOptions::ViewHeightSizable,
          );
          content.addSubview(root_view);
        });
      }
    }
  }

  fn create(
    mtm: MainThreadMarker,
    widget_id: u32,
    kind: i32,
  ) -> Option<Entry> {
    match kind {
      crate::LAUFEY_WIDGET_VSTACK | crate::LAUFEY_WIDGET_HSTACK => {
        let stack = NSStackView::new(mtm);
        let orientation = if kind == crate::LAUFEY_WIDGET_VSTACK {
          NSUserInterfaceLayoutOrientation::Vertical
        } else {
          NSUserInterfaceLayoutOrientation::Horizontal
        };
        stack.setOrientation(orientation);
        stack.setSpacing(8.0);
        Some(Entry::Stack(stack))
      }
      crate::LAUFEY_WIDGET_LABEL => {
        let label =
          NSTextField::labelWithString(&NSString::from_str(""), mtm);
        Some(Entry::Label(label))
      }
      crate::LAUFEY_WIDGET_BUTTON => {
        // Created in two steps so the button can target our action object,
        // which carries the widget id back to the click handler.
        let target = WidgetTarget::new(widget_id);
        let title = NSString::from_str("");
        let target_obj: &AnyObject = &target;
        let button = unsafe {
          NSButton::buttonWithTitle_target_action(
            &title,
            Some(target_obj),
            Some(sel!(widgetAction:)),
            mtm,
          )
        };
        Some(Entry::Button(button, target))
      }
      _ => None,
    }
  }
}
