// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! The Windows prompt dialog (`LAUFEY_DIALOG_PROMPT`) of the winit / servo
//! backends: an in-process Win32 dialog built from an in-memory template.
//! The title, message and default value are each a window's or a control's
//! text, never part of a command line or a script, so they render literally.
//! The same dialog as backend-common's `dialog_win.cc`.

use windows_sys::Win32::Foundation::{HWND, LPARAM, RECT, WPARAM};
use windows_sys::Win32::Graphics::Gdi::{
  CreateFontIndirectW, DeleteObject, DrawTextW, GetDC, GetMonitorInfoW,
  MonitorFromWindow, ReleaseDC, SelectObject, DT_CALCRECT, DT_EDITCONTROL,
  DT_NOPREFIX, DT_WORDBREAK, HFONT, MONITORINFO, MONITOR_DEFAULTTOPRIMARY,
};
use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
use windows_sys::Win32::UI::HiDpi::{
  AdjustWindowRectExForDpi, GetDpiForWindow, SystemParametersInfoForDpi,
};
use windows_sys::Win32::UI::Input::KeyboardAndMouse::SetFocus;
use windows_sys::Win32::UI::WindowsAndMessaging::{
  CreateWindowExW, DialogBoxIndirectParamW, EndDialog, GetDlgItem,
  GetWindowLongPtrW, GetWindowTextLengthW, GetWindowTextW, MoveWindow,
  SendMessageW, SetWindowLongPtrW, SetWindowPos, SetWindowTextW,
  BS_DEFPUSHBUTTON, DLGTEMPLATE, DS_MODALFRAME, DS_SETFOREGROUND,
  ES_AUTOHSCROLL, GWL_EXSTYLE, GWL_STYLE, IDCANCEL, IDOK, NONCLIENTMETRICSW,
  SPI_GETNONCLIENTMETRICS, SWP_NOACTIVATE, SWP_NOZORDER, WINDOW_EX_STYLE,
  WINDOW_STYLE, WM_COMMAND, WM_INITDIALOG, WM_SETFONT, WS_CAPTION, WS_CHILD,
  WS_EX_CLIENTEDGE, WS_POPUP, WS_SYSMENU, WS_TABSTOP, WS_VISIBLE,
};

const EDIT_ID: i32 = 101;
// winuser.h values (in windows-sys's Controls / SystemServices modules).
const EM_SETSEL: u32 = 0x00B1;
const SS_NOPREFIX: u32 = 0x0080;
const SS_EDITCONTROL: u32 = 0x2000;
/// DWLP_MSGRESULT (0) + sizeof(LRESULT) + sizeof(DLGPROC).
const DWLP_USER: i32 = 2 * std::mem::size_of::<usize>() as i32;

struct State {
  title: Vec<u16>,
  message: Vec<u16>,
  value: Vec<u16>,
  font: HFONT,
  result: Option<String>,
}

/// A DLGTEMPLATE followed by its empty menu, class and title arrays; the
/// whole template must be DWORD-aligned.
#[repr(C, align(4))]
struct Template {
  header: DLGTEMPLATE,
  menu: u16,
  class: u16,
  title: u16,
}

fn wide(s: &str) -> Vec<u16> {
  s.encode_utf16().chain(std::iter::once(0)).collect()
}

unsafe fn add_control(
  dialog: HWND,
  class: &str,
  text: *const u16,
  style: WINDOW_STYLE,
  ex_style: WINDOW_EX_STYLE,
  id: i32,
  font: HFONT,
) -> HWND {
  let class = wide(class);
  let control = CreateWindowExW(
    ex_style,
    class.as_ptr(),
    text,
    WS_CHILD | WS_VISIBLE | style,
    0,
    0,
    0,
    0,
    dialog,
    id as isize as _,
    GetModuleHandleW(std::ptr::null()),
    std::ptr::null(),
  );
  if !control.is_null() && !font.is_null() {
    SendMessageW(control, WM_SETFONT, font as WPARAM, 0);
  }
  control
}

/// Creates the controls sized to the message at the dialog's DPI and centers
/// the dialog on its monitor's work area.
unsafe fn lay_out(dialog: HWND, state: &mut State) {
  let dpi = match GetDpiForWindow(dialog) {
    0 => 96,
    d => d as i32,
  };
  let px = |dip: i32| dip * dpi / 96;

  let mut metrics: NONCLIENTMETRICSW = std::mem::zeroed();
  metrics.cbSize = std::mem::size_of::<NONCLIENTMETRICSW>() as u32;
  if SystemParametersInfoForDpi(
    SPI_GETNONCLIENTMETRICS,
    metrics.cbSize,
    &mut metrics as *mut _ as *mut _,
    0,
    dpi as u32,
  ) != 0
  {
    state.font = CreateFontIndirectW(&metrics.lfMessageFont);
  }
  let font = state.font;
  let (margin, gap, button_w, button_h, edit_h, text_w) =
    (px(12), px(8), px(80), px(26), px(24), px(360));

  // The message's height wrapped to text_w; "&" is a character, not a
  // mnemonic marker (DT_NOPREFIX / SS_NOPREFIX).
  let mut text_rect = RECT {
    left: 0,
    top: 0,
    right: text_w,
    bottom: 0,
  };
  let dc = GetDC(dialog);
  let old = if font.is_null() {
    std::ptr::null_mut()
  } else {
    SelectObject(dc, font)
  };
  DrawTextW(
    dc,
    state.message.as_ptr(),
    -1,
    &mut text_rect,
    DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX,
  );
  if !old.is_null() {
    SelectObject(dc, old);
  }
  ReleaseDC(dialog, dc);
  let text_h = text_rect.bottom.clamp(px(16), px(480));

  let label = add_control(
    dialog,
    "STATIC",
    state.message.as_ptr(),
    SS_NOPREFIX | SS_EDITCONTROL,
    0,
    -1,
    font,
  );
  let edit = add_control(
    dialog,
    "EDIT",
    state.value.as_ptr(),
    WS_TABSTOP | ES_AUTOHSCROLL as u32,
    WS_EX_CLIENTEDGE,
    EDIT_ID,
    font,
  );
  let ok_text = wide("OK");
  let cancel_text = wide("Cancel");
  let ok = add_control(
    dialog,
    "BUTTON",
    ok_text.as_ptr(),
    WS_TABSTOP | BS_DEFPUSHBUTTON as u32,
    0,
    IDOK,
    font,
  );
  let cancel = add_control(
    dialog,
    "BUTTON",
    cancel_text.as_ptr(),
    WS_TABSTOP,
    0,
    IDCANCEL,
    font,
  );

  let mut y = margin;
  MoveWindow(label, margin, y, text_w, text_h, 0);
  y += text_h + gap;
  MoveWindow(edit, margin, y, text_w, edit_h, 0);
  y += edit_h + px(16);
  MoveWindow(cancel, margin + text_w - button_w, y, button_w, button_h, 0);
  MoveWindow(
    ok,
    margin + text_w - 2 * button_w - gap,
    y,
    button_w,
    button_h,
    0,
  );
  y += button_h + margin;

  let mut frame = RECT {
    left: 0,
    top: 0,
    right: margin * 2 + text_w,
    bottom: y,
  };
  AdjustWindowRectExForDpi(
    &mut frame,
    GetWindowLongPtrW(dialog, GWL_STYLE) as u32,
    0,
    GetWindowLongPtrW(dialog, GWL_EXSTYLE) as u32,
    dpi as u32,
  );
  let (width, height) = (frame.right - frame.left, frame.bottom - frame.top);
  let mut info: MONITORINFO = std::mem::zeroed();
  info.cbSize = std::mem::size_of::<MONITORINFO>() as u32;
  GetMonitorInfoW(
    MonitorFromWindow(dialog, MONITOR_DEFAULTTOPRIMARY),
    &mut info,
  );
  let work = info.rcWork;
  SetWindowPos(
    dialog,
    std::ptr::null_mut(),
    work.left + (work.right - work.left - width) / 2,
    work.top + (work.bottom - work.top - height) / 2,
    width,
    height,
    SWP_NOZORDER | SWP_NOACTIVATE,
  );
}

unsafe extern "system" fn dialog_proc(
  dialog: HWND,
  msg: u32,
  wp: WPARAM,
  lp: LPARAM,
) -> isize {
  match msg {
    WM_INITDIALOG => {
      SetWindowLongPtrW(dialog, DWLP_USER, lp);
      let state = &mut *(lp as *mut State);
      SetWindowTextW(dialog, state.title.as_ptr());
      lay_out(dialog, state);
      let edit = GetDlgItem(dialog, EDIT_ID);
      SendMessageW(edit, EM_SETSEL, 0, -1);
      SetFocus(edit);
      0 // The focus is set.
    }
    WM_COMMAND => {
      let id = (wp & 0xFFFF) as i32;
      let state = GetWindowLongPtrW(dialog, DWLP_USER) as *mut State;
      if id == IDOK && !state.is_null() {
        let edit = GetDlgItem(dialog, EDIT_ID);
        let len = GetWindowTextLengthW(edit).max(0);
        let mut buf = vec![0u16; len as usize + 1];
        let n = GetWindowTextW(edit, buf.as_mut_ptr(), len + 1).max(0);
        (*state).result = Some(String::from_utf16_lossy(&buf[..n as usize]));
        EndDialog(dialog, IDOK as isize);
        1
      } else if id == IDCANCEL {
        EndDialog(dialog, IDCANCEL as isize);
        1
      } else {
        0
      }
    }
    _ => 0,
  }
}

/// Returns `(confirmed, text)`; `text` is `Some` when confirmed.
pub(crate) fn show_prompt_dialog(
  title: &str,
  message: &str,
  default_value: &str,
) -> (bool, Option<String>) {
  // SAFETY: an all-zero DLGTEMPLATE is valid (no controls, zero size).
  let mut template: Template = unsafe { std::mem::zeroed() };
  template.header.style = WS_POPUP
    | WS_CAPTION
    | WS_SYSMENU
    | DS_MODALFRAME as u32
    | DS_SETFOREGROUND as u32;
  let mut state = State {
    title: wide(title),
    message: wide(message),
    value: wide(default_value),
    font: std::ptr::null_mut(),
    result: None,
  };
  // SAFETY: the template and the state outlive the modal call, which
  // returns only once the dialog is destroyed.
  let result = unsafe {
    DialogBoxIndirectParamW(
      GetModuleHandleW(std::ptr::null()),
      &template.header,
      std::ptr::null_mut(),
      Some(dialog_proc),
      &mut state as *mut State as LPARAM,
    )
  };
  if !state.font.is_null() {
    // SAFETY: the font was created by lay_out and its controls are gone.
    unsafe { DeleteObject(state.font) };
  }
  if result == IDOK as isize {
    (true, Some(state.result.take().unwrap_or_default()))
  } else {
    (false, None)
  }
}
