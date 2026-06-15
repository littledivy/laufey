// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// MessageBoxW-based dialogs. MessageBoxW itself pumps the Win32 message
// loop for the duration of the modal so other laufey windows keep
// responding.

#include "laufey_backend_common.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace laufey_common {

namespace {

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty())
    return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring out;
  if (n > 0) {
    out.resize(n - 1);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
  }
  return out;
}

}  // namespace

int ShowDialogWin(int dialog_type, const std::string& title,
                  const std::string& message,
                  const std::string& default_value, char** out_input_value) {
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
    // Windows has no built-in prompt dialog. Shell out to PowerShell's
    // Microsoft.VisualBasic.Interaction.InputBox — same approach as the
    // webview backend prior to this unification.
    std::string script =
        "Add-Type -AssemblyName Microsoft.VisualBasic; "
        "[Microsoft.VisualBasic.Interaction]::InputBox('" +
        message + "', '" + title + "', '" + default_value + "')";
    std::string cmd = "powershell -Command \"" + script + "\"";

    FILE* fp = _popen(cmd.c_str(), "r");
    if (!fp)
      return 0;
    char buf[4096] = {};
    std::string result;
    while (fgets(buf, sizeof(buf), fp)) {
      result += buf;
    }
    _pclose(fp);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
      result.pop_back();
    if (result.empty())
      return 0;
    if (out_input_value)
      *out_input_value = _strdup(result.c_str());
    return 1;
  }
  return 0;
}

}  // namespace laufey_common
