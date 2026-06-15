// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Notifications (Windows): hidden Shell_NotifyIcon balloon. On Windows
// 10/11 the shell intercepts the balloon and renders it as a system toast
// (with grouping, Action Center entry, etc.). Click → CLICKED;
// dismiss / timeout → CLOSED. Action buttons (NIIF balloons don't have
// them) are silently ignored.
//
// Owns its own message-only window. When tray icons get extracted to
// backend-common (task #4), the two can share one HWND — for now keeping
// them separate so this module is self-contained.

#include "laufey_backend_common.h"

#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>
#include <objbase.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace laufey_common {

namespace {

#define WM_LAUFEY_COMMON_NOTIFICATION (WM_APP + 64)

struct WinNotifEntry {
  UINT uid;
  HICON hicon;  // hidden icon for the balloon (one per notification)
  std::string tag;
  laufey_notification_event_fn on_event;
  void* user_data;
};

std::mutex& NotifMutex() {
  static std::mutex m;
  return m;
}
std::map<uint32_t, WinNotifEntry>& NotifMap() {
  static std::map<uint32_t, WinNotifEntry> m;
  return m;
}
// uid (Shell_NotifyIcon ID) → notification_id (our id space)
std::map<UINT, uint32_t>& NotifUidToId() {
  static std::map<UINT, uint32_t> m;
  return m;
}
std::atomic<uint32_t> g_next_notif_id{1};
std::atomic<UINT> g_next_notif_uid{0x4000};
HWND g_notif_msg_hwnd = nullptr;

HICON DecodePngToHicon(const void* bytes, size_t len, int desired) {
  if (!bytes || len == 0)
    return nullptr;
  // CoInitializeEx may fail with RPC_E_CHANGED_MODE if a different
  // apartment was already initialized on this thread; that's fine, we
  // just use whatever's there.
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  IWICImagingFactory* factory = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(hr) || !factory)
    return nullptr;

  IWICStream* stream = nullptr;
  IWICBitmapDecoder* decoder = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* conv = nullptr;
  HICON result = nullptr;

  hr = factory->CreateStream(&stream);
  if (FAILED(hr) || !stream)
    goto done;
  hr = stream->InitializeFromMemory((BYTE*)const_cast<void*>(bytes), (DWORD)len);
  if (FAILED(hr))
    goto done;
  hr = factory->CreateDecoderFromStream(stream, nullptr,
                                        WICDecodeMetadataCacheOnDemand,
                                        &decoder);
  if (FAILED(hr) || !decoder)
    goto done;
  hr = decoder->GetFrame(0, &frame);
  if (FAILED(hr) || !frame)
    goto done;
  hr = factory->CreateFormatConverter(&conv);
  if (FAILED(hr) || !conv)
    goto done;
  hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                        WICBitmapDitherTypeNone, nullptr, 0.0,
                        WICBitmapPaletteTypeMedianCut);
  if (FAILED(hr))
    goto done;

  {
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    std::vector<BYTE> pixels(w * h * 4);
    hr = conv->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data());
    if (FAILED(hr))
      goto done;

    // Pixels are BGRA premultiplied for GDI compatibility.
    HDC hdc = GetDC(nullptr);
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = (LONG)w;
    bi.bV5Height = -(LONG)h;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* dib_bits = nullptr;
    HBITMAP color = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS,
                                     &dib_bits, nullptr, 0);
    HBITMAP mask = CreateBitmap((int)w, (int)h, 1, 1, nullptr);
    ReleaseDC(nullptr, hdc);
    if (color && dib_bits) {
      memcpy(dib_bits, pixels.data(), pixels.size());
      ICONINFO ii = {};
      ii.fIcon = TRUE;
      ii.hbmColor = color;
      ii.hbmMask = mask;
      result = CreateIconIndirect(&ii);
    }
    if (color)
      DeleteObject(color);
    if (mask)
      DeleteObject(mask);
    (void)desired;
  }

done:
  if (conv)
    conv->Release();
  if (frame)
    frame->Release();
  if (decoder)
    decoder->Release();
  if (stream)
    stream->Release();
  if (factory)
    factory->Release();
  return result;
}

HICON LoadDefaultAppIcon() {
  return (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(32512),
                           IMAGE_ICON, 0, 0, LR_SHARED | LR_DEFAULTSIZE);
}

LRESULT CALLBACK NotifWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg != WM_LAUFEY_COMMON_NOTIFICATION)
    return DefWindowProc(hwnd, msg, wp, lp);

  UINT uid = (UINT)wp;
  UINT event = LOWORD(lp);
  uint32_t nid = 0;
  {
    std::lock_guard<std::mutex> lock(NotifMutex());
    auto it = NotifUidToId().find(uid);
    if (it != NotifUidToId().end())
      nid = it->second;
  }
  if (!nid)
    return 0;
  int reason = -1;
  if (event == NIN_BALLOONSHOW)
    reason = LAUFEY_NOTIFICATION_SHOWN;
  else if (event == NIN_BALLOONUSERCLICK)
    reason = LAUFEY_NOTIFICATION_CLICKED;
  else if (event == NIN_BALLOONHIDE || event == NIN_BALLOONTIMEOUT)
    reason = LAUFEY_NOTIFICATION_CLOSED;
  if (reason < 0)
    return 0;
  laufey_notification_event_fn fn = nullptr;
  void* user_data = nullptr;
  bool is_terminal = (reason == LAUFEY_NOTIFICATION_CLOSED ||
                      reason == LAUFEY_NOTIFICATION_CLICKED);
  {
    std::lock_guard<std::mutex> lock(NotifMutex());
    auto it = NotifMap().find(nid);
    if (it != NotifMap().end()) {
      fn = it->second.on_event;
      user_data = it->second.user_data;
    }
  }
  if (fn)
    fn(user_data, nid, reason, nullptr);
  if (is_terminal) {
    // Tear down the Shell_NotifyIcon on terminal events so the hidden
    // icon doesn't accumulate.
    std::lock_guard<std::mutex> lock(NotifMutex());
    auto it = NotifMap().find(nid);
    if (it != NotifMap().end()) {
      NOTIFYICONDATAW nid_data = {};
      nid_data.cbSize = sizeof(nid_data);
      nid_data.hWnd = hwnd;
      nid_data.uID = it->second.uid;
      Shell_NotifyIconW(NIM_DELETE, &nid_data);
      if (it->second.hicon)
        DestroyIcon(it->second.hicon);
      NotifUidToId().erase(it->second.uid);
      NotifMap().erase(it);
    }
  }
  return 0;
}

HWND EnsureNotifMessageWindow() {
  if (g_notif_msg_hwnd)
    return g_notif_msg_hwnd;
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = NotifWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"LaufeyCommonNotifWindow";
  RegisterClassExW(&wc);
  g_notif_msg_hwnd =
      CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                      nullptr, wc.hInstance, nullptr);
  return g_notif_msg_hwnd;
}

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

uint32_t ShowNotificationWin(const NotificationOptions& opts,
                             laufey_notification_event_fn on_event,
                             void* user_data) {
  // Tag-based replacement: drop any existing notification with the same tag.
  if (!opts.tag.empty()) {
    std::vector<uint32_t> to_drop;
    {
      std::lock_guard<std::mutex> lock(NotifMutex());
      for (auto& [id, e] : NotifMap()) {
        if (e.tag == opts.tag)
          to_drop.push_back(id);
      }
    }
    for (uint32_t old : to_drop) {
      std::lock_guard<std::mutex> lock(NotifMutex());
      auto it = NotifMap().find(old);
      if (it == NotifMap().end())
        continue;
      HWND hwnd = g_notif_msg_hwnd;
      if (hwnd) {
        NOTIFYICONDATAW del = {};
        del.cbSize = sizeof(del);
        del.hWnd = hwnd;
        del.uID = it->second.uid;
        Shell_NotifyIconW(NIM_DELETE, &del);
      }
      if (it->second.hicon)
        DestroyIcon(it->second.hicon);
      NotifUidToId().erase(it->second.uid);
      NotifMap().erase(it);
    }
  }

  uint32_t nid = g_next_notif_id.fetch_add(1, std::memory_order_relaxed);
  UINT uid = g_next_notif_uid.fetch_add(1, std::memory_order_relaxed);

  std::wstring wtitle = Utf8ToWide(opts.title);
  std::wstring wbody = Utf8ToWide(opts.body);

  HWND hwnd = EnsureNotifMessageWindow();
  if (!hwnd)
    return 0;

  HICON hicon = nullptr;
  if (!opts.icon_png.empty()) {
    hicon = DecodePngToHicon(opts.icon_png.data(), opts.icon_png.size(),
                             GetSystemMetrics(SM_CXICON));
  }
  if (!hicon)
    hicon = LoadDefaultAppIcon();

  NOTIFYICONDATAW nd = {};
  nd.cbSize = sizeof(nd);
  nd.hWnd = hwnd;
  nd.uID = uid;
  nd.uFlags = NIF_MESSAGE | NIF_ICON | NIF_INFO;
  nd.uCallbackMessage = WM_LAUFEY_COMMON_NOTIFICATION;
  nd.hIcon = hicon;
  wcsncpy_s(nd.szInfoTitle, wtitle.c_str(), _TRUNCATE);
  wcsncpy_s(nd.szInfo, wbody.c_str(), _TRUNCATE);
  nd.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
  if (opts.silent)
    nd.dwInfoFlags |= NIIF_NOSOUND;

  if (!Shell_NotifyIconW(NIM_ADD, &nd)) {
    if (hicon)
      DestroyIcon(hicon);
    return 0;
  }

  std::lock_guard<std::mutex> lock(NotifMutex());
  WinNotifEntry e = {};
  e.uid = uid;
  e.hicon = hicon;
  e.tag = opts.tag;
  e.on_event = on_event;
  e.user_data = user_data;
  NotifMap()[nid] = e;
  NotifUidToId()[uid] = nid;
  return nid;
}

void CloseNotificationWin(uint32_t notification_id) {
  HWND hwnd = g_notif_msg_hwnd;
  laufey_notification_event_fn fn = nullptr;
  void* ud = nullptr;
  {
    std::lock_guard<std::mutex> lock(NotifMutex());
    auto it = NotifMap().find(notification_id);
    if (it == NotifMap().end())
      return;
    if (hwnd) {
      NOTIFYICONDATAW del = {};
      del.cbSize = sizeof(del);
      del.hWnd = hwnd;
      del.uID = it->second.uid;
      Shell_NotifyIconW(NIM_DELETE, &del);
    }
    if (it->second.hicon)
      DestroyIcon(it->second.hicon);
    NotifUidToId().erase(it->second.uid);
    fn = it->second.on_event;
    ud = it->second.user_data;
    NotifMap().erase(it);
  }
  if (fn)
    fn(ud, notification_id, LAUFEY_NOTIFICATION_CLOSED, nullptr);
}

}  // namespace laufey_common
