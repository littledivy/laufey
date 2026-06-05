// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "runtime_loader.h"
#include "laufey_backend_common.h"
#include "laufey_json.h"
#include "init_script.h"
#include <win32_menu.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl.h>

// windows.h defines CreateWindow as a macro which conflicts with
// LaufeyBackend::CreateWindow
#undef CreateWindow

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "windowscodecs.lib")

// WebView2 headers
#include "WebView2.h"

#include <iostream>
#include <string>
#include <map>
#include <mutex>

using namespace Microsoft::WRL;

namespace keyboard {

// VK → W3C key/code mapping lives in backend-common
// (laufey_common::VkToKey / VkToCode). These thin wrappers extract
// the Windows-only state (GetKeyState / lParam scancode) and forward.
inline std::string VirtualKeyToKey(WPARAM vk, LPARAM /*lParam*/) {
  bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  bool caps = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
  return laufey_common::VkToKey(static_cast<int>(vk), 0, shift, caps);
}

inline std::string VirtualKeyToCode(WPARAM vk, LPARAM lParam) {
  bool is_extended = (lParam & (1 << 24)) != 0;
  uint32_t scancode = static_cast<uint32_t>((lParam >> 16) & 0xFF);
  return laufey_common::VkToCode(static_cast<int>(vk), is_extended, scancode);
}

inline uint32_t GetLaufeyModifiers() {
  uint32_t modifiers = 0;
  if (GetKeyState(VK_SHIFT) & 0x8000)
    modifiers |= LAUFEY_MOD_SHIFT;
  if (GetKeyState(VK_CONTROL) & 0x8000)
    modifiers |= LAUFEY_MOD_CONTROL;
  if (GetKeyState(VK_MENU) & 0x8000)
    modifiers |= LAUFEY_MOD_ALT;
  if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000)
    modifiers |= LAUFEY_MOD_META;
  return modifiers;
}

}  // namespace keyboard

// Convert UTF-8 to wide string
static std::wstring Utf8ToWide(const std::string& str) {
  if (str.empty())
    return std::wstring();
  int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
  std::wstring result(size - 1, 0);
  MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], size);
  return result;
}

// Convert wide string to UTF-8
static std::string WideToUtf8(const std::wstring& wstr) {
  if (wstr.empty())
    return std::string();
  int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0,
                                 nullptr, nullptr);
  std::string result(size - 1, 0);
  WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], size, nullptr,
                      nullptr);
  return result;
}

// HWND → laufey_id mapping
static std::map<HWND, uint32_t> g_hwnd_to_laufey_id;
static std::mutex g_hwnd_mutex;

static uint32_t LaufeyIdForHwnd(HWND hwnd) {
  if (!hwnd)
    return 0;
  std::lock_guard<std::mutex> lock(g_hwnd_mutex);
  auto it = g_hwnd_to_laufey_id.find(hwnd);
  return it != g_hwnd_to_laufey_id.end() ? it->second : 0;
}

// Per-window state
struct WinWindowState {
  uint32_t window_id;
  HWND hwnd;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> webview;
  bool webview_ready = false;
  std::wstring pending_url;
  std::wstring pending_title;
};

// Custom window message for UI tasks
#define WM_UI_TASK (WM_USER + 1)

struct UiTaskData {
  void (*task)(void*);
  void* data;
};

// ============================================================================
// WebView2 Backend
// ============================================================================

class WebView2Backend;
static WebView2Backend* g_win_backend = nullptr;

class WebView2Backend : public LaufeyBackend {
 public:
  WebView2Backend();
  ~WebView2Backend() override;

  void CreateWindow(uint32_t window_id, int width, int height) override;
  void CreateWindowEx(uint32_t window_id, int width, int height,
                      uint32_t flags) override;
  void CloseWindow(uint32_t window_id) override;

  void Navigate(uint32_t window_id, const std::string& url) override;
  void SetTitle(uint32_t window_id, const std::string& title) override;
  void ExecuteJs(uint32_t window_id, const std::string& script,
                 laufey_js_result_fn callback, void* callback_data) override;
  void Quit() override;
  void SetWindowSize(uint32_t window_id, int width, int height) override;
  void GetWindowSize(uint32_t window_id, int* width, int* height) override;
  void SetWindowPosition(uint32_t window_id, int x, int y) override;
  void GetWindowPosition(uint32_t window_id, int* x, int* y) override;
  void SetResizable(uint32_t window_id, bool resizable) override;
  bool IsResizable(uint32_t window_id) override;
  void SetAlwaysOnTop(uint32_t window_id, bool always_on_top) override;
  bool IsAlwaysOnTop(uint32_t window_id) override;
  bool IsVisible(uint32_t window_id) override;
  void Show(uint32_t window_id) override;
  void Hide(uint32_t window_id) override;
  void Focus(uint32_t window_id) override;
  void PostUiTask(void (*task)(void*), void* data) override;

  void InvokeJsCallback(uint32_t window_id, uint64_t callback_id,
                        laufey::ValuePtr args) override;
  void ReleaseJsCallback(uint32_t window_id, uint64_t callback_id) override;
  void RespondToJsCall(uint32_t window_id, uint64_t call_id,
                       laufey::ValuePtr result, laufey::ValuePtr error) override;

  void Run() override;

  void SetApplicationMenu(uint32_t window_id, laufey_value_t* menu_template,
                          const laufey_backend_api_t* api,
                          laufey_menu_click_fn on_click,
                          void* on_click_data) override;

  void ShowContextMenu(uint32_t window_id, int x, int y,
                       laufey_value_t* menu_template, const laufey_backend_api_t* api,
                       laufey_menu_click_fn on_click,
                       void* on_click_data) override;

  void OpenDevTools(uint32_t window_id) override;

  int ShowDialog(uint32_t window_id, int dialog_type, const std::string& title,
                 const std::string& message, const std::string& default_value,
                 char** out_input_value) override;

  void BounceDock(int type) override;
  void SetDockBadge(const char* badge_or_null) override;

  uint32_t CreateTrayIcon() override;
  void DestroyTrayIcon(uint32_t tray_id) override;
  void SetTrayIcon(uint32_t tray_id, const void* png_bytes,
                   size_t len) override;
  void SetTrayTooltip(uint32_t tray_id, const char* tooltip_or_null) override;
  void SetTrayMenu(uint32_t tray_id, laufey_value_t* menu_template,
                   const laufey_backend_api_t* api, laufey_menu_click_fn on_click,
                   void* on_click_data) override;
  void SetTrayClickHandler(uint32_t tray_id, laufey_tray_click_fn handler,
                           void* user_data) override;
  void SetTrayDoubleClickHandler(uint32_t tray_id, laufey_tray_click_fn handler,
                                 void* user_data) override;
  void SetTrayIconDark(uint32_t tray_id, const void* png_bytes,
                       size_t len) override;
  bool GetTrayIconBounds(uint32_t tray_id, int* x, int* y, int* width,
                         int* height) override;

  uint32_t ShowNotification(laufey_value_t* options, const laufey_backend_api_t* api,
                            laufey_notification_event_fn on_event,
                            void* user_data) override;
  void CloseNotification(uint32_t notification_id) override;

  // Shell_NotifyIcon balloons have no permission model — always granted.
  void QueryPermission(int kind, laufey_permission_callback_fn cb,
                       void* user_data) override {
    laufey_common::QueryPermissionStub(kind, cb, user_data);
  }
  void RequestPermission(int kind, laufey_permission_callback_fn cb,
                         void* user_data) override {
    laufey_common::RequestPermissionStub(kind, cb, user_data);
  }

  void HandleJsMessage(uint32_t window_id, const std::wstring& json);

 private:
  WinWindowState* GetWindow(uint32_t window_id);
  void InitializeWebViewForWindow(uint32_t window_id, HWND hwnd);
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam);

  std::map<uint32_t, WinWindowState> windows_;
  std::mutex windows_mutex_;
  bool class_registered_ = false;
};

LRESULT CALLBACK WebView2Backend::WindowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                             LPARAM lParam) {
  uint32_t wid = LaufeyIdForHwnd(hwnd);

  switch (msg) {
    case WM_SIZE: {
      if (g_win_backend && wid > 0) {
        std::lock_guard<std::mutex> lock(g_win_backend->windows_mutex_);
        auto* state = g_win_backend->GetWindow(wid);
        if (state && state->controller) {
          RECT bounds;
          GetClientRect(hwnd, &bounds);
          state->controller->put_Bounds(bounds);
        }
      }
      if (wid > 0) {
        RECT rect;
        GetClientRect(hwnd, &rect);
        RuntimeLoader::GetInstance()->DispatchResizeEvent(
            wid, rect.right - rect.left, rect.bottom - rect.top);
      }
      return 0;
    }
    case WM_MOVE:
      if (wid > 0) {
        RuntimeLoader::GetInstance()->DispatchMoveEvent(
            wid, (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam));
      }
      return 0;
    case WM_SETFOCUS:
      if (wid > 0)
        RuntimeLoader::GetInstance()->DispatchFocusedEvent(wid, 1);
      return 0;
    case WM_KILLFOCUS:
      if (wid > 0)
        RuntimeLoader::GetInstance()->DispatchFocusedEvent(wid, 0);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: {
      if (wid == 0)
        break;
      int state = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN ||
                   msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN)
                      ? LAUFEY_MOUSE_PRESSED
                      : LAUFEY_MOUSE_RELEASED;
      int button;
      switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
          button = LAUFEY_MOUSE_BUTTON_LEFT;
          break;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
          button = LAUFEY_MOUSE_BUTTON_RIGHT;
          break;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
          button = LAUFEY_MOUSE_BUTTON_MIDDLE;
          break;
        default:
          button = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1)
                       ? LAUFEY_MOUSE_BUTTON_BACK
                       : LAUFEY_MOUSE_BUTTON_FORWARD;
          break;
      }
      double x = static_cast<double>(GET_X_LPARAM(lParam));
      double y = static_cast<double>(GET_Y_LPARAM(lParam));
      uint32_t modifiers = keyboard::GetLaufeyModifiers();
      RuntimeLoader::GetInstance()->DispatchMouseClickEvent(wid, state, button,
                                                            x, y, modifiers, 1);
      break;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      if (wid == 0)
        break;
      std::string key = keyboard::VirtualKeyToKey(wParam, lParam);
      std::string code = keyboard::VirtualKeyToCode(wParam, lParam);
      uint32_t modifiers = keyboard::GetLaufeyModifiers();
      bool repeat = (lParam & (1 << 30)) != 0;
      RuntimeLoader::GetInstance()->DispatchKeyboardEvent(
          wid, LAUFEY_KEY_PRESSED, key.c_str(), code.c_str(), modifiers, repeat);
      break;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
      if (wid == 0)
        break;
      std::string key = keyboard::VirtualKeyToKey(wParam, lParam);
      std::string code = keyboard::VirtualKeyToCode(wParam, lParam);
      uint32_t modifiers = keyboard::GetLaufeyModifiers();
      RuntimeLoader::GetInstance()->DispatchKeyboardEvent(
          wid, LAUFEY_KEY_RELEASED, key.c_str(), code.c_str(), modifiers, false);
      break;
    }
    case WM_CLOSE:
      if (wid > 0) {
        RuntimeLoader::GetInstance()->DispatchCloseRequestedEvent(wid);
      }
      // Check if any windows remain
      {
        std::lock_guard<std::mutex> lock(g_hwnd_mutex);
        // Remove this window from map
        g_hwnd_to_laufey_id.erase(hwnd);
        if (g_hwnd_to_laufey_id.empty()) {
          PostQuitMessage(0);
        }
      }
      DestroyWindow(hwnd);
      return 0;
    case WM_COMMAND:
      if (win32_menu::HandleMenuCommand(hwnd, wParam))
        return 0;
      break;
    case WM_DESTROY:
      return 0;
    case WM_UI_TASK: {
      UiTaskData* taskData = reinterpret_cast<UiTaskData*>(lParam);
      if (taskData) {
        taskData->task(taskData->data);
        delete taskData;
      }
      return 0;
    }
  }

  return DefWindowProc(hwnd, msg, wParam, lParam);
}

WebView2Backend::WebView2Backend() {
  g_win_backend = this;

  // Register window class
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(WNDCLASSEXW);
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.lpszClassName = L"LaufeyWebView2";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  RegisterClassExW(&wc);
  class_registered_ = true;
}

WebView2Backend::~WebView2Backend() {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  for (auto& [wid, state] : windows_) {
    if (state.controller)
      state.controller->Close();
    {
      std::lock_guard<std::mutex> hlock(g_hwnd_mutex);
      g_hwnd_to_laufey_id.erase(state.hwnd);
    }
  }
  windows_.clear();
  g_win_backend = nullptr;
}

WinWindowState* WebView2Backend::GetWindow(uint32_t window_id) {
  auto it = windows_.find(window_id);
  return it != windows_.end() ? &it->second : nullptr;
}

void WebView2Backend::CreateWindow(uint32_t window_id, int width, int height) {
  CreateWindowEx(window_id, width, height, 0);
}

void WebView2Backend::CreateWindowEx(uint32_t window_id, int width, int height,
                                     uint32_t flags) {
  DWORD style = WS_OVERLAPPEDWINDOW;
  DWORD ex_style = 0;
  if (flags & LAUFEY_WINDOW_FLAG_FRAMELESS) {
    // Borderless popup: no caption / sizing frame.
    style = WS_POPUP;
  }
  if (flags & LAUFEY_WINDOW_FLAG_NO_ACTIVATE) {
    // Don't steal foreground/focus; keep out of the taskbar and Alt-Tab.
    ex_style |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
  }

  HWND hwnd = CreateWindowExW(
      ex_style, L"LaufeyWebView2", L"", style, CW_USEDEFAULT, CW_USEDEFAULT, width,
      height, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);

  {
    std::lock_guard<std::mutex> lock(g_hwnd_mutex);
    g_hwnd_to_laufey_id[hwnd] = window_id;
  }

  WinWindowState state;
  state.window_id = window_id;
  state.hwnd = hwnd;

  {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    windows_[window_id] = state;
  }

  InitializeWebViewForWindow(window_id, hwnd);

  // Showing a non-activating panel must not take foreground from the user's
  // active window.
  ShowWindow(hwnd, (flags & LAUFEY_WINDOW_FLAG_NO_ACTIVATE) ? SW_SHOWNOACTIVATE
                                                         : SW_SHOW);
  UpdateWindow(hwnd);
}

void WebView2Backend::InitializeWebViewForWindow(uint32_t window_id,
                                                 HWND hwnd) {
  CreateCoreWebView2EnvironmentWithOptions(
      nullptr, nullptr, nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this, window_id, hwnd](HRESULT result,
                                  ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result)) {
              std::cerr << "Failed to create WebView2 environment" << std::endl;
              return result;
            }

            env->CreateCoreWebView2Controller(
                hwnd,
                Callback<
                    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this, window_id, hwnd](
                        HRESULT result,
                        ICoreWebView2Controller* controller) -> HRESULT {
                      if (FAILED(result) || !controller) {
                        std::cerr << "Failed to create WebView2 controller"
                                  << std::endl;
                        return result;
                      }

                      std::lock_guard<std::mutex> lock(windows_mutex_);
                      auto* state = GetWindow(window_id);
                      if (!state)
                        return S_OK;

                      state->controller = controller;
                      controller->get_CoreWebView2(&state->webview);

                      RECT bounds;
                      GetClientRect(hwnd, &bounds);
                      controller->put_Bounds(bounds);

                      std::string initScript = BuildInitScript(
                          RuntimeLoader::GetInstance()->GetJsNamespace(),
                          "window.chrome.webview.postMessage(JSON.stringify({\n"
                          "            callId: callId,\n"
                          "            method: path.join('.'),\n"
                          "            args: processedArgs\n"
                          "          }));");
                      std::wstring wInitScript(initScript.begin(),
                                               initScript.end());
                      state->webview->AddScriptToExecuteOnDocumentCreated(
                          wInitScript.c_str(), nullptr);

                      uint32_t wid = window_id;
                      state->webview->add_WebMessageReceived(
                          Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                              [this, wid](
                                  ICoreWebView2* sender,
                                  ICoreWebView2WebMessageReceivedEventArgs*
                                      args) -> HRESULT {
                                LPWSTR messageRaw;
                                args->TryGetWebMessageAsString(&messageRaw);
                                if (messageRaw) {
                                  HandleJsMessage(wid, messageRaw);
                                  CoTaskMemFree(messageRaw);
                                }
                                return S_OK;
                              })
                              .Get(),
                          nullptr);

                      state->webview->add_ScriptDialogOpening(
                          Callback<
                              ICoreWebView2ScriptDialogOpeningEventHandler>(
                              [hwnd](ICoreWebView2* sender,
                                     ICoreWebView2ScriptDialogOpeningEventArgs*
                                         args) -> HRESULT {
                                COREWEBVIEW2_SCRIPT_DIALOG_KIND kind;
                                args->get_Kind(&kind);

                                LPWSTR messageRaw = nullptr;
                                args->get_Message(&messageRaw);
                                std::wstring message =
                                    messageRaw ? messageRaw : L"";
                                if (messageRaw)
                                  CoTaskMemFree(messageRaw);

                                if (kind ==
                                    COREWEBVIEW2_SCRIPT_DIALOG_KIND_ALERT) {
                                  MessageBoxW(hwnd, message.c_str(), L"Alert",
                                              MB_OK | MB_ICONINFORMATION);
                                  args->Accept();
                                } else if (
                                    kind ==
                                    COREWEBVIEW2_SCRIPT_DIALOG_KIND_CONFIRM) {
                                  int result = MessageBoxW(
                                      hwnd, message.c_str(), L"Confirm",
                                      MB_OKCANCEL | MB_ICONQUESTION);
                                  if (result == IDOK) {
                                    args->Accept();
                                  }
                                } else if (
                                    kind ==
                                    COREWEBVIEW2_SCRIPT_DIALOG_KIND_PROMPT) {
                                  // For prompt, we need a custom dialog. Use a
                                  // simple approach with TaskDialog-style
                                  // input. WebView2 doesn't have a built-in way
                                  // to show prompt with input, so we accept
                                  // with the default.
                                  LPWSTR defaultTextRaw = nullptr;
                                  args->get_DefaultText(&defaultTextRaw);
                                  std::wstring defaultText =
                                      defaultTextRaw ? defaultTextRaw : L"";
                                  if (defaultTextRaw)
                                    CoTaskMemFree(defaultTextRaw);

                                  // Use a simple MessageBox for now — accept
                                  // with default text
                                  int result = MessageBoxW(
                                      hwnd, message.c_str(), L"Prompt",
                                      MB_OKCANCEL | MB_ICONQUESTION);
                                  if (result == IDOK) {
                                    args->put_ResultText(defaultText.c_str());
                                    args->Accept();
                                  }
                                } else if (
                                    kind ==
                                    COREWEBVIEW2_SCRIPT_DIALOG_KIND_BEFOREUNLOAD) {
                                  args->Accept();
                                }

                                return S_OK;
                              })
                              .Get(),
                          nullptr);

                      state->webview_ready = true;

                      if (!state->pending_url.empty()) {
                        state->webview->Navigate(state->pending_url.c_str());
                        state->pending_url.clear();
                      }
                      if (!state->pending_title.empty()) {
                        SetWindowTextW(hwnd, state->pending_title.c_str());
                        state->pending_title.clear();
                      }

                      return S_OK;
                    })
                    .Get());

            return S_OK;
          })
          .Get());
}

void WebView2Backend::CloseWindow(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    if (state->controller)
      state->controller->Close();
    {
      std::lock_guard<std::mutex> hlock(g_hwnd_mutex);
      g_hwnd_to_laufey_id.erase(state->hwnd);
    }
    DestroyWindow(state->hwnd);
    windows_.erase(window_id);
  }
}

void WebView2Backend::Navigate(uint32_t window_id, const std::string& url) {
  std::wstring wurl = Utf8ToWide(url);
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (!state)
    return;
  if (state->webview_ready && state->webview) {
    state->webview->Navigate(wurl.c_str());
  } else {
    state->pending_url = wurl;
  }
}

void WebView2Backend::SetTitle(uint32_t window_id, const std::string& title) {
  std::wstring wtitle = Utf8ToWide(title);
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (!state)
    return;
  if (state->webview_ready) {
    SetWindowTextW(state->hwnd, wtitle.c_str());
  } else {
    state->pending_title = wtitle;
  }
}

void WebView2Backend::ExecuteJs(uint32_t window_id, const std::string& script,
                                laufey_js_result_fn callback,
                                void* callback_data) {
  std::wstring wscript = Utf8ToWide(script);
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (!state || !state->webview_ready || !state->webview) {
    if (callback)
      callback(nullptr, nullptr, callback_data);
    return;
  }
  if (!callback) {
    state->webview->ExecuteScript(wscript.c_str(), nullptr);
  } else {
    state->webview->ExecuteScript(
        wscript.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [callback, callback_data](HRESULT hr,
                                      LPCWSTR resultJson) -> HRESULT {
              if (FAILED(hr)) {
                auto errVal = laufey::Value::String("ExecuteScript failed");
                laufey_value errLaufey(errVal);
                callback(nullptr, &errLaufey, callback_data);
                return S_OK;
              }
              if (!resultJson) {
                callback(nullptr, nullptr, callback_data);
                return S_OK;
              }
              // WebView2 returns the result as a JSON string
              std::wstring wresult(resultJson);
              std::string result(wresult.begin(), wresult.end());
              auto val = json::ParseJson(result);
              laufey_value laufey(val);
              callback(&laufey, nullptr, callback_data);
              return S_OK;
            })
            .Get());
  }
}

void WebView2Backend::Quit() {
  PostQuitMessage(0);
}

void WebView2Backend::SetWindowSize(uint32_t window_id, int width, int height) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    SetWindowPos(state->hwnd, nullptr, 0, 0, width, height,
                 SWP_NOMOVE | SWP_NOZORDER);
  }
}

void WebView2Backend::GetWindowSize(uint32_t window_id, int* width,
                                    int* height) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    RECT rect;
    if (GetWindowRect(state->hwnd, &rect)) {
      if (width)
        *width = rect.right - rect.left;
      if (height)
        *height = rect.bottom - rect.top;
    }
  }
}

void WebView2Backend::SetWindowPosition(uint32_t window_id, int x, int y) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    SetWindowPos(state->hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
  }
}

void WebView2Backend::GetWindowPosition(uint32_t window_id, int* x, int* y) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    RECT rect;
    if (GetWindowRect(state->hwnd, &rect)) {
      if (x)
        *x = rect.left;
      if (y)
        *y = rect.top;
    }
  }
}

void WebView2Backend::SetResizable(uint32_t window_id, bool resizable) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    LONG style = GetWindowLong(state->hwnd, GWL_STYLE);
    if (resizable) {
      style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    } else {
      style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    }
    SetWindowLong(state->hwnd, GWL_STYLE, style);
  }
}

bool WebView2Backend::IsResizable(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  return state ? (GetWindowLong(state->hwnd, GWL_STYLE) & WS_THICKFRAME) != 0
               : false;
}

void WebView2Backend::SetAlwaysOnTop(uint32_t window_id, bool always_on_top) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    SetWindowPos(state->hwnd, always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0,
                 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
  }
}

bool WebView2Backend::IsAlwaysOnTop(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  return state ? (GetWindowLong(state->hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0
               : false;
}

bool WebView2Backend::IsVisible(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  return state ? IsWindowVisible(state->hwnd) != FALSE : false;
}

void WebView2Backend::Show(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state)
    ShowWindow(state->hwnd, SW_SHOW);
}

void WebView2Backend::Hide(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state)
    ShowWindow(state->hwnd, SW_HIDE);
}

void WebView2Backend::Focus(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    ShowWindow(state->hwnd, SW_SHOW);
    SetForegroundWindow(state->hwnd);
    SetFocus(state->hwnd);
  }
}

void WebView2Backend::PostUiTask(void (*task)(void*), void* data) {
  // Post to the first available window, or use a message-only window
  std::lock_guard<std::mutex> lock(windows_mutex_);
  if (!windows_.empty()) {
    PostMessage(windows_.begin()->second.hwnd, WM_UI_TASK, 0,
                reinterpret_cast<LPARAM>(new UiTaskData{task, data}));
  }
}

void WebView2Backend::InvokeJsCallback(uint32_t window_id, uint64_t callback_id,
                                       laufey::ValuePtr args) {
  std::string argsJson = json::Serialize(args);
  std::wstring wscript =
      Utf8ToWide(BuildInvokeCallbackScript(callback_id, argsJson));
  std::lock_guard<std::mutex> lock(windows_mutex_);
  if (window_id == 0) {
    for (auto& [wid, state] : windows_) {
      if (state.webview_ready && state.webview) {
        state.webview->ExecuteScript(wscript.c_str(), nullptr);
      }
    }
  } else {
    auto* state = GetWindow(window_id);
    if (state && state->webview_ready && state->webview) {
      state->webview->ExecuteScript(wscript.c_str(), nullptr);
    }
  }
}

void WebView2Backend::ReleaseJsCallback(uint32_t window_id,
                                        uint64_t callback_id) {
  std::wstring wscript = Utf8ToWide(BuildReleaseCallbackScript(callback_id));
  std::lock_guard<std::mutex> lock(windows_mutex_);
  if (window_id == 0) {
    for (auto& [wid, state] : windows_) {
      if (state.webview_ready && state.webview) {
        state.webview->ExecuteScript(wscript.c_str(), nullptr);
      }
    }
  } else {
    auto* state = GetWindow(window_id);
    if (state && state->webview_ready && state->webview) {
      state->webview->ExecuteScript(wscript.c_str(), nullptr);
    }
  }
}

void WebView2Backend::RespondToJsCall(uint32_t window_id, uint64_t call_id,
                                      laufey::ValuePtr result,
                                      laufey::ValuePtr error) {
  std::string resultJson = json::Serialize(result);
  std::string errorJson = error ? json::Serialize(error) : "null";
  std::wstring wscript = Utf8ToWide(BuildRespondScript(
      call_id, resultJson, errorJson, static_cast<bool>(error)));
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state && state->webview_ready && state->webview) {
    state->webview->ExecuteScript(wscript.c_str(), nullptr);
  }
}

void WebView2Backend::Run() {
  MSG msg;
  while (GetMessage(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

void WebView2Backend::HandleJsMessage(uint32_t window_id,
                                      const std::wstring& json) {
  std::string jsonStr = WideToUtf8(json);
  laufey::ValuePtr msg = json::ParseJson(jsonStr);
  if (!msg || !msg->IsDict())
    return;

  const auto& dict = msg->GetDict();

  auto callIdIt = dict.find("callId");
  auto methodIt = dict.find("method");
  auto argsIt = dict.find("args");

  if (callIdIt == dict.end() || methodIt == dict.end())
    return;

  uint64_t call_id = 0;
  if (callIdIt->second->IsInt()) {
    call_id = static_cast<uint64_t>(callIdIt->second->GetInt());
  } else if (callIdIt->second->IsDouble()) {
    call_id = static_cast<uint64_t>(callIdIt->second->GetDouble());
  }

  std::string method =
      methodIt->second->IsString() ? methodIt->second->GetString() : "";
  laufey::ValuePtr args =
      (argsIt != dict.end()) ? argsIt->second : laufey::Value::List();

  RuntimeLoader::GetInstance()->OnJsCall(window_id, call_id, method, args);
}

// ============================================================================
// Application Menu
// ============================================================================

void WebView2Backend::SetApplicationMenu(uint32_t window_id,
                                         laufey_value_t* menu_template,
                                         const laufey_backend_api_t* api,
                                         laufey_menu_click_fn on_click,
                                         void* on_click_data) {
  if (!menu_template)
    return;
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state && state->hwnd) {
    win32_menu::SetApplicationMenu(state->hwnd, menu_template, api, on_click,
                                   on_click_data, window_id);
  }
}

// ============================================================================
// Context Menu
// ============================================================================

void WebView2Backend::ShowContextMenu(uint32_t window_id, int x, int y,
                                      laufey_value_t* menu_template,
                                      const laufey_backend_api_t* api,
                                      laufey_menu_click_fn on_click,
                                      void* on_click_data) {
  if (!menu_template)
    return;
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state && state->hwnd) {
    win32_menu::ShowContextMenu(state->hwnd, x, y, menu_template, api, on_click,
                                on_click_data, window_id);
  }
}

// ============================================================================
// DevTools
// ============================================================================

void WebView2Backend::OpenDevTools(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state && state->webview) {
    state->webview->OpenDevToolsWindow();
  }
}

// ============================================================================
// Dialog
// ============================================================================

int WebView2Backend::ShowDialog(uint32_t /*window_id*/, int dialog_type,
                                const std::string& title,
                                const std::string& message,
                                const std::string& default_value,
                                char** out_input_value) {
  return laufey_common::ShowDialogWin(dialog_type, title, message, default_value,
                                   out_input_value);
}

// ============================================================================
// Dock / taskbar (Windows)
// ============================================================================

void WebView2Backend::BounceDock(int type) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  for (auto& [wid, state] : windows_) {
    if (!state.hwnd)
      continue;
    FLASHWINFO fi = {sizeof(FLASHWINFO), state.hwnd, 0, 0, 0};
    if (type == LAUFEY_DOCK_BOUNCE_CRITICAL) {
      fi.dwFlags = FLASHW_ALL | FLASHW_TIMER;
      fi.uCount = 0;
    } else {
      fi.dwFlags = FLASHW_TIMERNOFG;
      fi.uCount = 3;
    }
    FlashWindowEx(&fi);
  }
}

// Badge via title prefix. Saved-titles map lives in
// laufey_common::ApplyTitlePrefixBadge. Win32 titles are UTF-16 natively
// but ApplyTitlePrefixBadge works in UTF-8 — we round-trip through
// Utf8ToWide / WideToUtf8 here.
void WebView2Backend::SetDockBadge(const char* badge_or_null) {
  std::string badge =
      (badge_or_null && *badge_or_null) ? std::string(badge_or_null) : "";
  std::lock_guard<std::mutex> wlock(windows_mutex_);
  for (auto& [wid, state] : windows_) {
    if (!state.hwnd)
      continue;
    wchar_t buf[512];
    int n = GetWindowTextW(state.hwnd, buf, 512);
    std::wstring current_w(buf, n);
    // UTF-16 → UTF-8 for ApplyTitlePrefixBadge.
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, current_w.c_str(), n,
                                       nullptr, 0, nullptr, nullptr);
    std::string current_u8(utf8_len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, current_w.c_str(), n, current_u8.data(),
                        utf8_len, nullptr, nullptr);
    std::string next_u8 =
        laufey_common::ApplyTitlePrefixBadge(wid, current_u8, badge);
    std::wstring next_w = Utf8ToWide(next_u8);
    SetWindowTextW(state.hwnd, next_w.c_str());
  }
}

// ============================================================================
// Tray / status bar (Windows)
// ============================================================================
//
// Thin trampolines over backend-common/src/tray_win.cc.

uint32_t WebView2Backend::CreateTrayIcon() {
  uint32_t tray_id = laufey_common::CreateTrayIconWin();
  laufey_common::FinalizeTrayIconWin(tray_id);
  return tray_id;
}
void WebView2Backend::DestroyTrayIcon(uint32_t tray_id) {
  laufey_common::DestroyTrayIconWin(tray_id);
}
void WebView2Backend::SetTrayIcon(uint32_t tray_id, const void* png_bytes,
                                  size_t len) {
  laufey_common::SetTrayIconWin(tray_id, png_bytes, len);
}
void WebView2Backend::SetTrayIconDark(uint32_t tray_id, const void* png_bytes,
                                      size_t len) {
  laufey_common::SetTrayIconDarkWin(tray_id, png_bytes, len);
}

bool WebView2Backend::GetTrayIconBounds(uint32_t tray_id, int* x, int* y,
                                        int* width, int* height) {
  return laufey_common::GetTrayIconBoundsWin(tray_id, x, y, width, height);
}
void WebView2Backend::SetTrayDoubleClickHandler(uint32_t tray_id,
                                                laufey_tray_click_fn handler,
                                                void* user_data) {
  laufey_common::SetTrayDoubleClickHandlerWin(tray_id, handler, user_data);
}
void WebView2Backend::SetTrayTooltip(uint32_t tray_id,
                                     const char* tooltip_or_null) {
  laufey_common::SetTrayTooltipWin(tray_id, tooltip_or_null);
}
void WebView2Backend::SetTrayMenu(uint32_t tray_id, laufey_value_t* menu_template,
                                  const laufey_backend_api_t* api,
                                  laufey_menu_click_fn on_click,
                                  void* on_click_data) {
  laufey_common::SetTrayMenuWin(tray_id, menu_template, api, on_click,
                             on_click_data);
}
void WebView2Backend::SetTrayClickHandler(uint32_t tray_id,
                                          laufey_tray_click_fn handler,
                                          void* user_data) {
  laufey_common::SetTrayClickHandlerWin(tray_id, handler, user_data);
}
// ============================================================================
// Notifications (WebView2 Windows)
// ============================================================================
//
// Thin trampoline over backend-common/src/notifications_win.cc.

uint32_t WebView2Backend::ShowNotification(laufey_value_t* options,
                                           const laufey_backend_api_t* api,
                                           laufey_notification_event_fn on_event,
                                           void* user_data) {
  laufey_common::NotificationOptions opts =
      laufey_common::ParseNotificationOptions(options, api);
  return laufey_common::ShowNotificationWin(opts, on_event, user_data);
}

void WebView2Backend::CloseNotification(uint32_t notification_id) {
  laufey_common::CloseNotificationWin(notification_id);
}

// ============================================================================
// Factory Function
// ============================================================================

LaufeyBackend* CreateLaufeyBackend() {
  return new WebView2Backend();
}
