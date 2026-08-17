// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "laufey_backend_common.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace laufey_common {

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty())
    return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 0)
    return std::wstring();
  std::wstring w(static_cast<size_t>(n - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}

std::string WideToUtf8(const std::wstring& w) {
  if (w.empty())
    return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr,
                              nullptr);
  if (n <= 0)
    return std::string();
  std::string s(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
  return s;
}

}  // namespace laufey_common
