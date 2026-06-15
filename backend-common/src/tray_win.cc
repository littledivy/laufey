// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Shell_NotifyIcon-backed tray / status-bar icon. Hidden message-only
// window receives WM_LAUFEY_TRAYICON callbacks and dispatches click /
// double-click / right-click-menu events. WIC handles PNG → HICON
// decode. Light vs dark icons are resolved against the
// AppsUseLightTheme registry value and re-applied on
// WM_SETTINGCHANGE (ImmersiveColorSet).
//
// All entry points are synchronous; callers that need to dispatch to
// a UI thread (CEF's TID_UI) should marshal before calling.

#include "laufey_backend_common.h"

#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>
#include <objbase.h>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace laufey_common {

namespace {

#define WM_LAUFEY_COMMON_TRAYICON (WM_APP + 65)

struct WinTrayEntry {
  UINT uid;
  HICON hicon_light;
  HICON hicon_dark;
  HMENU hmenu;
  std::map<UINT, std::string> cmd_to_id;
  laufey_menu_click_fn menu_click_fn;
  void* menu_click_data;
  laufey_tray_click_fn click_fn;
  void* click_data;
  laufey_tray_click_fn dblclick_fn;
  void* dblclick_data;
};

std::mutex& TrayMutex() {
  static std::mutex m;
  return m;
}
std::map<uint32_t, WinTrayEntry>& TrayMap() {
  static std::map<uint32_t, WinTrayEntry> map;
  return map;
}
std::atomic<uint32_t> g_next_tray_id{1};
std::atomic<UINT> g_next_cmd_id{1000};
HWND g_tray_msg_hwnd = nullptr;

bool WinIsDarkMode() {
  DWORD data = 1, size = sizeof(data), kind = 0;
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\"
                    L"Personalize",
                    0, KEY_READ, &key) != 0) {
    return false;
  }
  LONG rc = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &kind,
                              (LPBYTE)&data, &size);
  RegCloseKey(key);
  return (rc == 0 && kind == REG_DWORD && data == 0);
}

void ApplyActiveIcon(uint32_t tray_id) {
  HWND hwnd = g_tray_msg_hwnd;
  if (!hwnd) return;
  std::lock_guard<std::mutex> lock(TrayMutex());
  auto it = TrayMap().find(tray_id);
  if (it == TrayMap().end()) return;
  HICON chosen = (WinIsDarkMode() && it->second.hicon_dark)
                     ? it->second.hicon_dark
                     : it->second.hicon_light;
  if (!chosen) return;
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd;
  nid.uID = tray_id;
  nid.uFlags = NIF_ICON;
  nid.hIcon = chosen;
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void ReapplyAllIcons() {
  std::vector<uint32_t> ids;
  {
    std::lock_guard<std::mutex> lock(TrayMutex());
    for (auto& [tid, e] : TrayMap()) ids.push_back(tid);
  }
  for (uint32_t id : ids) ApplyActiveIcon(id);
}

LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_SETTINGCHANGE) {
    if (lp && wcscmp((LPCWSTR)lp, L"ImmersiveColorSet") == 0) {
      ReapplyAllIcons();
    }
    return 0;
  }
  if (msg != WM_LAUFEY_COMMON_TRAYICON) return DefWindowProc(hwnd, msg, wp, lp);

  uint32_t tray_id = (uint32_t)wp;
  UINT event = LOWORD(lp);
  if (event == WM_LBUTTONDBLCLK) {
    laufey_tray_click_fn fn = nullptr;
    void* data = nullptr;
    {
      std::lock_guard<std::mutex> lock(TrayMutex());
      auto it = TrayMap().find(tray_id);
      if (it != TrayMap().end()) {
        fn = it->second.dblclick_fn;
        data = it->second.dblclick_data;
      }
    }
    if (fn) fn(data, tray_id);
    return 0;
  }
  if (event == WM_LBUTTONUP) {
    laufey_tray_click_fn fn = nullptr;
    void* data = nullptr;
    {
      std::lock_guard<std::mutex> lock(TrayMutex());
      auto it = TrayMap().find(tray_id);
      if (it != TrayMap().end()) {
        fn = it->second.click_fn;
        data = it->second.click_data;
      }
    }
    if (fn) fn(data, tray_id);
    return 0;
  }
  if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
    HMENU menu = nullptr;
    {
      std::lock_guard<std::mutex> lock(TrayMutex());
      auto it = TrayMap().find(tray_id);
      if (it != TrayMap().end()) menu = it->second.hmenu;
    }
    if (!menu) return 0;
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    UINT cmd = TrackPopupMenu(menu,
                              TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                              pt.x, pt.y, 0, hwnd, nullptr);
    if (!cmd) return 0;
    laufey_menu_click_fn fn = nullptr;
    void* data = nullptr;
    std::string item_id;
    {
      std::lock_guard<std::mutex> lock(TrayMutex());
      auto it = TrayMap().find(tray_id);
      if (it != TrayMap().end()) {
        auto cit = it->second.cmd_to_id.find(cmd);
        if (cit != it->second.cmd_to_id.end()) item_id = cit->second;
        fn = it->second.menu_click_fn;
        data = it->second.menu_click_data;
      }
    }
    if (fn && !item_id.empty()) fn(data, tray_id, item_id.c_str());
  }
  return 0;
}

HWND EnsureTrayMessageWindow() {
  if (g_tray_msg_hwnd) return g_tray_msg_hwnd;
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = TrayWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"LaufeyCommonTrayWindow";
  RegisterClassExW(&wc);
  g_tray_msg_hwnd =
      CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                      nullptr, wc.hInstance, nullptr);
  return g_tray_msg_hwnd;
}

HICON DecodePngToHicon(const void* bytes, size_t len, int desired) {
  if (!bytes || len == 0) return nullptr;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IWICImagingFactory* factory = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                               CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
    return nullptr;
  }
  IWICStream* stream = nullptr;
  factory->CreateStream(&stream);
  if (stream) stream->InitializeFromMemory((BYTE*)bytes, (DWORD)len);
  IWICBitmapDecoder* decoder = nullptr;
  if (stream)
    factory->CreateDecoderFromStream(stream, nullptr,
                                      WICDecodeMetadataCacheOnLoad, &decoder);
  IWICBitmapFrameDecode* frame = nullptr;
  if (decoder) decoder->GetFrame(0, &frame);
  IWICFormatConverter* conv = nullptr;
  factory->CreateFormatConverter(&conv);
  if (frame && conv) {
    conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                      WICBitmapDitherTypeNone, nullptr, 0.0,
                      WICBitmapPaletteTypeCustom);
  }
  IWICBitmapScaler* scaler = nullptr;
  factory->CreateBitmapScaler(&scaler);
  UINT w = desired, h = desired;
  if (conv && scaler)
    scaler->Initialize(conv, w, h, WICBitmapInterpolationModeHighQualityCubic);
  std::vector<BYTE> pixels(w * h * 4);
  if (scaler)
    scaler->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data());
  HICON hicon = nullptr;
  if (scaler) {
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = w;
    bi.bV5Height = -(LONG)h;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    HDC hdc = GetDC(nullptr);
    void* bits = nullptr;
    ii.hbmColor = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits,
                                    nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (ii.hbmColor && bits) memcpy(bits, pixels.data(), pixels.size());
    ii.hbmMask = CreateBitmap(w, h, 1, 1, nullptr);
    if (ii.hbmColor && ii.hbmMask) hicon = CreateIconIndirect(&ii);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
  }
  if (scaler) scaler->Release();
  if (conv) conv->Release();
  if (frame) frame->Release();
  if (decoder) decoder->Release();
  if (stream) stream->Release();
  if (factory) factory->Release();
  return hicon;
}

std::wstring Utf8ToWide(const char* s) {
  if (!s || !*s) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  std::wstring out;
  if (n > 0) {
    out.resize(n - 1);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
  }
  return out;
}

HMENU BuildWinMenuFromValue(laufey_value_t* val, const laufey_backend_api_t* api,
                             std::map<UINT, std::string>& cmd_to_id) {
  if (!val || !api->value_is_list(val)) return nullptr;
  HMENU menu = CreatePopupMenu();
  size_t count = api->value_list_size(val);
  for (size_t i = 0; i < count; ++i) {
    laufey_value_t* itemVal = api->value_list_get(val, i);
    if (!itemVal || !api->value_is_dict(itemVal)) continue;

    // Separator
    laufey_value_t* typeVal = api->value_dict_get(itemVal, "type");
    if (typeVal && api->value_is_string(typeVal)) {
      size_t len = 0;
      char* typeStr = api->value_get_string(typeVal, &len);
      if (typeStr && std::string(typeStr) == "separator") {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        api->value_free_string(typeStr);
        continue;
      }
      if (typeStr) api->value_free_string(typeStr);
    }

    laufey_value_t* labelVal = api->value_dict_get(itemVal, "label");
    std::wstring wlabel;
    if (labelVal && api->value_is_string(labelVal)) {
      size_t len = 0;
      char* s = api->value_get_string(labelVal, &len);
      if (s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s, (int)len, nullptr, 0);
        wlabel.resize(n);
        MultiByteToWideChar(CP_UTF8, 0, s, (int)len, wlabel.data(), n);
        api->value_free_string(s);
      }
    }

    laufey_value_t* submenuVal = api->value_dict_get(itemVal, "submenu");
    if (submenuVal && api->value_is_list(submenuVal)) {
      HMENU sub = BuildWinMenuFromValue(submenuVal, api, cmd_to_id);
      AppendMenuW(menu, MF_POPUP | MF_STRING, (UINT_PTR)sub, wlabel.c_str());
      continue;
    }

    laufey_value_t* idVal = api->value_dict_get(itemVal, "id");
    std::string item_id;
    if (idVal && api->value_is_string(idVal)) {
      size_t len = 0;
      char* s = api->value_get_string(idVal, &len);
      if (s) {
        item_id = std::string(s, len);
        api->value_free_string(s);
      }
    }

    UINT cmd = g_next_cmd_id.fetch_add(1, std::memory_order_relaxed);
    UINT flags = MF_STRING;
    laufey_value_t* enabledVal = api->value_dict_get(itemVal, "enabled");
    if (enabledVal && api->value_is_bool(enabledVal) &&
        !api->value_get_bool(enabledVal)) {
      flags |= MF_GRAYED;
    }
    AppendMenuW(menu, flags, cmd, wlabel.c_str());
    if (!item_id.empty()) cmd_to_id[cmd] = item_id;
  }
  return menu;
}

}  // namespace

uint32_t CreateTrayIconWin() {
  uint32_t tray_id = g_next_tray_id.fetch_add(1, std::memory_order_relaxed);
  // Reserve the map entry now so other API calls keyed on tray_id
  // (SetTrayIcon, SetTrayMenu, ...) don't race against finalize.
  WinTrayEntry entry = {};
  entry.uid = tray_id;
  std::lock_guard<std::mutex> lock(TrayMutex());
  TrayMap()[tray_id] = std::move(entry);
  return tray_id;
}

void FinalizeTrayIconWin(uint32_t tray_id) {
  HWND hwnd = EnsureTrayMessageWindow();
  if (!hwnd) return;
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd;
  nid.uID = tray_id;
  nid.uFlags = NIF_MESSAGE;
  nid.uCallbackMessage = WM_LAUFEY_COMMON_TRAYICON;
  Shell_NotifyIconW(NIM_ADD, &nid);
}

void DestroyTrayIconWin(uint32_t tray_id) {
  HWND hwnd = g_tray_msg_hwnd;
  if (hwnd) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = tray_id;
    Shell_NotifyIconW(NIM_DELETE, &nid);
  }
  std::lock_guard<std::mutex> lock(TrayMutex());
  auto it = TrayMap().find(tray_id);
  if (it == TrayMap().end()) return;
  if (it->second.hicon_light) DestroyIcon(it->second.hicon_light);
  if (it->second.hicon_dark) DestroyIcon(it->second.hicon_dark);
  if (it->second.hmenu) DestroyMenu(it->second.hmenu);
  TrayMap().erase(it);
}

void SetTrayIconWin(uint32_t tray_id, const void* png_bytes, size_t len) {
  if (!png_bytes || len == 0) return;
  HICON hicon = DecodePngToHicon(png_bytes, len, GetSystemMetrics(SM_CXSMICON));
  if (!hicon) return;
  {
    std::lock_guard<std::mutex> lock(TrayMutex());
    auto it = TrayMap().find(tray_id);
    if (it == TrayMap().end()) {
      DestroyIcon(hicon);
      return;
    }
    if (it->second.hicon_light) DestroyIcon(it->second.hicon_light);
    it->second.hicon_light = hicon;
  }
  ApplyActiveIcon(tray_id);
}

void SetTrayIconDarkWin(uint32_t tray_id, const void* png_bytes, size_t len) {
  if (!png_bytes || len == 0) {
    {
      std::lock_guard<std::mutex> lock(TrayMutex());
      auto it = TrayMap().find(tray_id);
      if (it != TrayMap().end() && it->second.hicon_dark) {
        DestroyIcon(it->second.hicon_dark);
        it->second.hicon_dark = nullptr;
      }
    }
    ApplyActiveIcon(tray_id);
    return;
  }
  HICON hicon = DecodePngToHicon(png_bytes, len, GetSystemMetrics(SM_CXSMICON));
  if (!hicon) return;
  {
    std::lock_guard<std::mutex> lock(TrayMutex());
    auto it = TrayMap().find(tray_id);
    if (it == TrayMap().end()) {
      DestroyIcon(hicon);
      return;
    }
    if (it->second.hicon_dark) DestroyIcon(it->second.hicon_dark);
    it->second.hicon_dark = hicon;
  }
  ApplyActiveIcon(tray_id);
}

void SetTrayTooltipWin(uint32_t tray_id, const char* tooltip_or_null) {
  HWND hwnd = g_tray_msg_hwnd;
  if (!hwnd) return;
  std::wstring wtip = Utf8ToWide(tooltip_or_null);
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd;
  nid.uID = tray_id;
  nid.uFlags = NIF_TIP;
  wcsncpy_s(nid.szTip, wtip.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void SetTrayMenuWin(uint32_t tray_id, laufey_value_t* menu_template,
                     const laufey_backend_api_t* api,
                     laufey_menu_click_fn on_click, void* on_click_data) {
  std::map<UINT, std::string> cmd_to_id;
  HMENU menu = menu_template
                    ? BuildWinMenuFromValue(menu_template, api, cmd_to_id)
                    : nullptr;
  if (menu_template) api->value_free(menu_template);
  std::lock_guard<std::mutex> lock(TrayMutex());
  auto it = TrayMap().find(tray_id);
  if (it == TrayMap().end()) {
    if (menu) DestroyMenu(menu);
    return;
  }
  if (it->second.hmenu) DestroyMenu(it->second.hmenu);
  it->second.hmenu = menu;
  it->second.cmd_to_id = std::move(cmd_to_id);
  it->second.menu_click_fn = on_click;
  it->second.menu_click_data = on_click_data;
}

void SetTrayClickHandlerWin(uint32_t tray_id, laufey_tray_click_fn handler,
                              void* user_data) {
  std::lock_guard<std::mutex> lock(TrayMutex());
  auto it = TrayMap().find(tray_id);
  if (it == TrayMap().end()) return;
  it->second.click_fn = handler;
  it->second.click_data = user_data;
}

void SetTrayDoubleClickHandlerWin(uint32_t tray_id,
                                    laufey_tray_click_fn handler,
                                    void* user_data) {
  std::lock_guard<std::mutex> lock(TrayMutex());
  auto it = TrayMap().find(tray_id);
  if (it == TrayMap().end()) return;
  it->second.dblclick_fn = handler;
  it->second.dblclick_data = user_data;
}

bool GetTrayIconBoundsWin(uint32_t tray_id, int* x, int* y, int* width,
                            int* height) {
  HWND hwnd = g_tray_msg_hwnd;
  if (!hwnd) return false;
  {
    std::lock_guard<std::mutex> lock(TrayMutex());
    if (TrayMap().find(tray_id) == TrayMap().end()) return false;
  }
  NOTIFYICONIDENTIFIER nii = {};
  nii.cbSize = sizeof(nii);
  nii.hWnd = hwnd;
  nii.uID = tray_id;
  RECT rc = {};
  if (Shell_NotifyIconGetRect(&nii, &rc) != S_OK) return false;
  // The rect is in physical pixels; CEF Views window positions are in DIPs,
  // so scale by the tray window's DPI to land in the same space.
  UINT dpi = GetDpiForWindow(hwnd);
  if (dpi == 0) dpi = 96;
  if (x) *x = MulDiv(rc.left, 96, dpi);
  if (y) *y = MulDiv(rc.top, 96, dpi);
  if (width) *width = MulDiv(rc.right - rc.left, 96, dpi);
  if (height) *height = MulDiv(rc.bottom - rc.top, 96, dpi);
  return true;
}

}  // namespace laufey_common
