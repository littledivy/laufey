// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Win32 clipboard via the CF_UNICODETEXT format. Text is stored UTF-8 in the
// laufey ABI and converted to/from UTF-16 here.

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

char* WideToUtf8Strdup(const wchar_t* w) {
  if (!w)
    return nullptr;
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 0)
    return nullptr;
  char* out = static_cast<char*>(malloc(n));
  if (!out)
    return nullptr;
  WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, nullptr, nullptr);
  return out;
}

}  // namespace

char* ClipboardReadTextWin() {
  if (!OpenClipboard(nullptr))
    return nullptr;
  char* result = nullptr;
  HANDLE handle = GetClipboardData(CF_UNICODETEXT);
  if (handle) {
    const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(handle));
    if (w) {
      result = WideToUtf8Strdup(w);
      GlobalUnlock(handle);
    }
  }
  CloseClipboard();
  return result;
}

void ClipboardWriteTextWin(const std::string& text) {
  if (!OpenClipboard(nullptr))
    return;
  EmptyClipboard();
  std::wstring wide = Utf8ToWide(text);
  size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (mem) {
    void* dst = GlobalLock(mem);
    if (dst) {
      memcpy(dst, wide.c_str(), bytes);
      GlobalUnlock(mem);
      // Ownership of `mem` transfers to the system on success.
      if (!SetClipboardData(CF_UNICODETEXT, mem))
        GlobalFree(mem);
    } else {
      GlobalFree(mem);
    }
  }
  CloseClipboard();
}

}  // namespace laufey_common
