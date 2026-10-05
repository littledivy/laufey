// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Win32 dialogs: MessageBoxW for alert / confirm, and a prompt built from an
// in-memory dialog template (Windows has no stock prompt dialog). Every
// string is handed to a window or control as its text, never to a command
// line or a script, so a title, message or default value renders literally.
// Each modal pumps the Win32 message loop while it is open so other laufey
// windows keep responding.

#include "laufey_backend_common.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

namespace laufey_common {

namespace {

constexpr int kPromptEditId = 101;

struct PromptState {
  std::wstring title;
  std::wstring message;
  std::wstring value;
  HFONT font = nullptr;
};

HWND AddControl(HWND dialog, const wchar_t* cls, const wchar_t* text,
                DWORD style, DWORD ex_style, int id, HFONT font) {
  HWND control = CreateWindowExW(
      ex_style, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, dialog,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandleW(nullptr), nullptr);
  if (control && font)
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
  return control;
}

// Creates the prompt's controls sized to the message at the dialog's DPI and
// centers the dialog on its monitor's work area.
void LayOutPrompt(HWND dialog, PromptState* state) {
  UINT dpi = GetDpiForWindow(dialog);
  if (dpi == 0)
    dpi = USER_DEFAULT_SCREEN_DPI;
  auto px = [dpi](int dip) { return MulDiv(dip, static_cast<int>(dpi), 96); };

  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                 &metrics, 0, dpi)) {
    state->font = CreateFontIndirectW(&metrics.lfMessageFont);
  }

  const int margin = px(12);
  const int gap = px(8);
  const int button_w = px(80);
  const int button_h = px(26);
  const int edit_h = px(24);
  const int text_w = px(360);

  // The message's height wrapped to text_w. DT_NOPREFIX / SS_NOPREFIX: an
  // "&" is a character, not a mnemonic marker.
  RECT text_rect = {0, 0, text_w, 0};
  HDC dc = GetDC(dialog);
  HGDIOBJ old_font = state->font ? SelectObject(dc, state->font) : nullptr;
  DrawTextW(dc, state->message.c_str(), static_cast<int>(state->message.size()),
            &text_rect,
            DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
  if (old_font)
    SelectObject(dc, old_font);
  ReleaseDC(dialog, dc);
  const int text_h =
      std::min<int>(std::max<int>(text_rect.bottom, px(16)), px(480));

  HWND label =
      AddControl(dialog, L"STATIC", state->message.c_str(),
                 SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL, 0, -1, state->font);
  HWND edit = AddControl(dialog, L"EDIT", state->value.c_str(),
                         WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
                         kPromptEditId, state->font);
  HWND ok = AddControl(dialog, L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON,
                       0, IDOK, state->font);
  HWND cancel = AddControl(dialog, L"BUTTON", L"Cancel", WS_TABSTOP, 0,
                           IDCANCEL, state->font);

  int y = margin;
  MoveWindow(label, margin, y, text_w, text_h, FALSE);
  y += text_h + gap;
  MoveWindow(edit, margin, y, text_w, edit_h, FALSE);
  y += edit_h + px(16);
  MoveWindow(cancel, margin + text_w - button_w, y, button_w, button_h, FALSE);
  MoveWindow(ok, margin + text_w - 2 * button_w - gap, y, button_w, button_h,
             FALSE);
  y += button_h + margin;

  RECT frame = {0, 0, margin * 2 + text_w, y};
  AdjustWindowRectExForDpi(
      &frame, static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE)), FALSE,
      static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE)), dpi);
  const int width = frame.right - frame.left;
  const int height = frame.bottom - frame.top;
  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTOPRIMARY), &info);
  const RECT& work = info.rcWork;
  SetWindowPos(dialog, nullptr,
               work.left + (work.right - work.left - width) / 2,
               work.top + (work.bottom - work.top - height) / 2, width, height,
               SWP_NOZORDER | SWP_NOACTIVATE);
}

INT_PTR CALLBACK PromptProc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp) {
  auto* state =
      reinterpret_cast<PromptState*>(GetWindowLongPtrW(dialog, DWLP_USER));
  switch (msg) {
    case WM_INITDIALOG: {
      state = reinterpret_cast<PromptState*>(lp);
      SetWindowLongPtrW(dialog, DWLP_USER, lp);
      SetWindowTextW(dialog, state->title.c_str());
      LayOutPrompt(dialog, state);
      HWND edit = GetDlgItem(dialog, kPromptEditId);
      SendMessageW(edit, EM_SETSEL, 0, -1);
      SetFocus(edit);
      return FALSE;  // The focus is set.
    }
    case WM_COMMAND:
      if (LOWORD(wp) == IDOK && state) {
        HWND edit = GetDlgItem(dialog, kPromptEditId);
        int len = GetWindowTextLengthW(edit);
        std::wstring text(static_cast<size_t>(len) + 1, L'\0');
        len = GetWindowTextW(edit, text.data(), len + 1);
        text.resize(static_cast<size_t>((std::max)(len, 0)));
        state->value = std::move(text);
        EndDialog(dialog, IDOK);
        return TRUE;
      }
      if (LOWORD(wp) == IDCANCEL) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
      }
      break;
  }
  return FALSE;
}

// An empty dialog template: no controls (WM_INITDIALOG creates them), no
// menu, the default dialog class, the title set at init. A DLGTEMPLATE must
// be DWORD-aligned; the menu, class and title WORD arrays follow it.
struct alignas(4) EmptyDialogTemplate {
  DLGTEMPLATE header;
  WORD menu;
  WORD window_class;
  WORD title;
};

// True if OK was pressed, with the edit's text in `*value`.
bool RunPrompt(const std::wstring& title, const std::wstring& message,
               std::wstring* value) {
  EmptyDialogTemplate tmpl = {};
  tmpl.header.style =
      WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFOREGROUND;
  PromptState state;
  state.title = title;
  state.message = message;
  state.value = *value;
  INT_PTR result =
      DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &tmpl.header, nullptr,
                              PromptProc, reinterpret_cast<LPARAM>(&state));
  if (state.font)
    DeleteObject(state.font);
  if (result != IDOK)
    return false;
  *value = std::move(state.value);
  return true;
}

}  // namespace

int ShowDialogWin(int dialog_type, const std::string& title,
                  const std::string& message, const std::string& default_value,
                  char** out_input_value) {
  if (out_input_value)
    *out_input_value = nullptr;

  std::wstring wTitle = Utf8ToWide(title);
  std::wstring wMessage = Utf8ToWide(message);

  if (dialog_type == LAUFEY_DIALOG_ALERT) {
    MessageBoxW(nullptr, wMessage.c_str(), wTitle.c_str(),
                MB_OK | MB_ICONINFORMATION);
    return 1;
  }
  if (dialog_type == LAUFEY_DIALOG_CONFIRM) {
    int ret = MessageBoxW(nullptr, wMessage.c_str(), wTitle.c_str(),
                          MB_OKCANCEL | MB_ICONQUESTION);
    return (ret == IDOK) ? 1 : 0;
  }
  if (dialog_type == LAUFEY_DIALOG_PROMPT) {
    std::wstring value = Utf8ToWide(default_value);
    if (!RunPrompt(wTitle, wMessage, &value))
      return 0;
    if (out_input_value)
      *out_input_value = _strdup(WideToUtf8(value).c_str());
    return 1;
  }
  return 0;
}

}  // namespace laufey_common
