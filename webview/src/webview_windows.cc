// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "runtime_loader.h"
#include "wef_json.h"
#include <win32_menu.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl.h>

// windows.h defines CreateWindow as a macro which conflicts with
// WefBackend::CreateWindow
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

std::string VirtualKeyToKey(WPARAM vk, LPARAM lParam) {
  switch (vk) {
    case VK_BACK:
      return "Backspace";
    case VK_TAB:
      return "Tab";
    case VK_RETURN:
      return "Enter";
    case VK_SHIFT:
      return "Shift";
    case VK_CONTROL:
      return "Control";
    case VK_MENU:
      return "Alt";
    case VK_PAUSE:
      return "Pause";
    case VK_CAPITAL:
      return "CapsLock";
    case VK_ESCAPE:
      return "Escape";
    case VK_SPACE:
      return " ";
    case VK_PRIOR:
      return "PageUp";
    case VK_NEXT:
      return "PageDown";
    case VK_END:
      return "End";
    case VK_HOME:
      return "Home";
    case VK_LEFT:
      return "ArrowLeft";
    case VK_UP:
      return "ArrowUp";
    case VK_RIGHT:
      return "ArrowRight";
    case VK_DOWN:
      return "ArrowDown";
    case VK_INSERT:
      return "Insert";
    case VK_DELETE:
      return "Delete";
    case VK_LWIN:
    case VK_RWIN:
      return "Meta";
    case VK_F1:
      return "F1";
    case VK_F2:
      return "F2";
    case VK_F3:
      return "F3";
    case VK_F4:
      return "F4";
    case VK_F5:
      return "F5";
    case VK_F6:
      return "F6";
    case VK_F7:
      return "F7";
    case VK_F8:
      return "F8";
    case VK_F9:
      return "F9";
    case VK_F10:
      return "F10";
    case VK_F11:
      return "F11";
    case VK_F12:
      return "F12";
    case VK_NUMLOCK:
      return "NumLock";
    case VK_SCROLL:
      return "ScrollLock";
    default:
      if (vk >= 'A' && vk <= 'Z') {
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool caps = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
        char c = static_cast<char>(vk);
        if (!(shift ^ caps))
          c += 32;  // lowercase
        return std::string(1, c);
      }
      if (vk >= '0' && vk <= '9') {
        return std::string(1, static_cast<char>(vk));
      }
      return "Unidentified";
  }
}

std::string VirtualKeyToCode(WPARAM vk, LPARAM lParam) {
  bool isExtended = (lParam & (1 << 24)) != 0;
  switch (vk) {
    case VK_BACK:
      return "Backspace";
    case VK_TAB:
      return "Tab";
    case VK_RETURN:
      return isExtended ? "NumpadEnter" : "Enter";
    case VK_SHIFT:
      return (MapVirtualKey((lParam >> 16) & 0xFF, MAPVK_VSC_TO_VK_EX) ==
              VK_RSHIFT)
                 ? "ShiftRight"
                 : "ShiftLeft";
    case VK_CONTROL:
      return isExtended ? "ControlRight" : "ControlLeft";
    case VK_MENU:
      return isExtended ? "AltRight" : "AltLeft";
    case VK_PAUSE:
      return "Pause";
    case VK_CAPITAL:
      return "CapsLock";
    case VK_ESCAPE:
      return "Escape";
    case VK_SPACE:
      return "Space";
    case VK_PRIOR:
      return "PageUp";
    case VK_NEXT:
      return "PageDown";
    case VK_END:
      return "End";
    case VK_HOME:
      return "Home";
    case VK_LEFT:
      return "ArrowLeft";
    case VK_UP:
      return "ArrowUp";
    case VK_RIGHT:
      return "ArrowRight";
    case VK_DOWN:
      return "ArrowDown";
    case VK_INSERT:
      return "Insert";
    case VK_DELETE:
      return "Delete";
    case VK_LWIN:
      return "MetaLeft";
    case VK_RWIN:
      return "MetaRight";
    case VK_F1:
      return "F1";
    case VK_F2:
      return "F2";
    case VK_F3:
      return "F3";
    case VK_F4:
      return "F4";
    case VK_F5:
      return "F5";
    case VK_F6:
      return "F6";
    case VK_F7:
      return "F7";
    case VK_F8:
      return "F8";
    case VK_F9:
      return "F9";
    case VK_F10:
      return "F10";
    case VK_F11:
      return "F11";
    case VK_F12:
      return "F12";
    case VK_NUMLOCK:
      return "NumLock";
    case VK_SCROLL:
      return "ScrollLock";
    case VK_OEM_1:
      return "Semicolon";
    case VK_OEM_PLUS:
      return "Equal";
    case VK_OEM_COMMA:
      return "Comma";
    case VK_OEM_MINUS:
      return "Minus";
    case VK_OEM_PERIOD:
      return "Period";
    case VK_OEM_2:
      return "Slash";
    case VK_OEM_3:
      return "Backquote";
    case VK_OEM_4:
      return "BracketLeft";
    case VK_OEM_5:
      return "Backslash";
    case VK_OEM_6:
      return "BracketRight";
    case VK_OEM_7:
      return "Quote";
    default:
      if (vk >= 'A' && vk <= 'Z') {
        return "Key" + std::string(1, static_cast<char>(vk));
      }
      if (vk >= '0' && vk <= '9') {
        return "Digit" + std::string(1, static_cast<char>(vk));
      }
      return "Unidentified";
  }
}

uint32_t GetWefModifiers() {
  uint32_t modifiers = 0;
  if (GetKeyState(VK_SHIFT) & 0x8000)
    modifiers |= WEF_MOD_SHIFT;
  if (GetKeyState(VK_CONTROL) & 0x8000)
    modifiers |= WEF_MOD_CONTROL;
  if (GetKeyState(VK_MENU) & 0x8000)
    modifiers |= WEF_MOD_ALT;
  if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000)
    modifiers |= WEF_MOD_META;
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

// HWND → wef_id mapping
static std::map<HWND, uint32_t> g_hwnd_to_wef_id;
static std::mutex g_hwnd_mutex;

static uint32_t WefIdForHwnd(HWND hwnd) {
  if (!hwnd)
    return 0;
  std::lock_guard<std::mutex> lock(g_hwnd_mutex);
  auto it = g_hwnd_to_wef_id.find(hwnd);
  return it != g_hwnd_to_wef_id.end() ? it->second : 0;
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

static std::string BuildInitScript(const std::string& ns,
                                   const std::string& postMessage) {
  return R"JS(
(function() {
  const pendingCalls = new Map();
  let nextCallId = 1;

  function createWefProxy(path = []) {
    return new Proxy(function() {}, {
      get(target, prop) {
        if (prop === 'then' || prop === 'catch' || prop === 'finally' ||
            prop === 'constructor' || prop === Symbol.toStringTag) {
          return undefined;
        }
        return createWefProxy([...path, prop]);
      },
      apply(target, thisArg, args) {
        return new Promise((resolve, reject) => {
          const callId = nextCallId++;
          pendingCalls.set(callId, { resolve, reject });

          const processedArgs = args.map(arg => {
            if (typeof arg === 'function') {
              const cbId = nextCallId++;
              window.__wefCallbacks = window.__wefCallbacks || {};
              window.__wefCallbacks[cbId] = arg;
              return { __callback__: String(cbId) };
            }
            if (arg instanceof ArrayBuffer) {
              const bytes = new Uint8Array(arg);
              let binary = '';
              bytes.forEach(b => binary += String.fromCharCode(b));
              return { __binary__: btoa(binary) };
            }
            if (arg instanceof Uint8Array) {
              let binary = '';
              arg.forEach(b => binary += String.fromCharCode(b));
              return { __binary__: btoa(binary) };
            }
            return arg;
          });

          )JS" +
         postMessage + R"JS(
        });
      }
    });
  }

  window[")JS" +
         ns + R"JS("] = createWefProxy();

  window.__wefRespond = function(callId, result, error) {
    const pending = pendingCalls.get(callId);
    if (pending) {
      pendingCalls.delete(callId);
      if (error) {
        pending.reject(new Error(error));
      } else {
        function convertBinary(obj) {
          if (obj && typeof obj === 'object') {
            if (obj.__binary__) {
              const binary = atob(obj.__binary__);
              const bytes = new Uint8Array(binary.length);
              for (let i = 0; i < binary.length; i++) {
                bytes[i] = binary.charCodeAt(i);
              }
              return bytes.buffer;
            }
            if (Array.isArray(obj)) {
              return obj.map(convertBinary);
            }
            const result = {};
            for (const key in obj) {
              result[key] = convertBinary(obj[key]);
            }
            return result;
          }
          return obj;
        }
        pending.resolve(convertBinary(result));
      }
    }
  };

  window.__wefInvokeCallback = function(callbackId, args) {
    const cb = window.__wefCallbacks && window.__wefCallbacks[callbackId];
    if (cb) {
      cb.apply(null, args);
    }
  };

  window.__wefReleaseCallback = function(callbackId) {
    if (window.__wefCallbacks) {
      delete window.__wefCallbacks[callbackId];
    }
  };

})();
)JS";
}

class WebView2Backend;
static WebView2Backend* g_win_backend = nullptr;

class WebView2Backend : public WefBackend {
 public:
  WebView2Backend();
  ~WebView2Backend() override;

  void CreateWindow(uint32_t window_id, int width, int height) override;
  void CloseWindow(uint32_t window_id) override;

  void Navigate(uint32_t window_id, const std::string& url) override;
  void SetTitle(uint32_t window_id, const std::string& title) override;
  void ExecuteJs(uint32_t window_id, const std::string& script,
                 wef_js_result_fn callback, void* callback_data) override;
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
                        wef::ValuePtr args) override;
  void ReleaseJsCallback(uint32_t window_id, uint64_t callback_id) override;
  void RespondToJsCall(uint32_t window_id, uint64_t call_id,
                       wef::ValuePtr result, wef::ValuePtr error) override;

  void Run() override;

  void SetApplicationMenu(uint32_t window_id, wef_value_t* menu_template,
                          const wef_backend_api_t* api,
                          wef_menu_click_fn on_click,
                          void* on_click_data) override;

  void ShowContextMenu(uint32_t window_id, int x, int y,
                       wef_value_t* menu_template, const wef_backend_api_t* api,
                       wef_menu_click_fn on_click,
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
  void SetTrayMenu(uint32_t tray_id, wef_value_t* menu_template,
                   const wef_backend_api_t* api, wef_menu_click_fn on_click,
                   void* on_click_data) override;
  void SetTrayClickHandler(uint32_t tray_id, wef_tray_click_fn handler,
                           void* user_data) override;
  void SetTrayDoubleClickHandler(uint32_t tray_id, wef_tray_click_fn handler,
                                 void* user_data) override;
  void SetTrayIconDark(uint32_t tray_id, const void* png_bytes,
                       size_t len) override;

  uint32_t ShowNotification(wef_value_t* options,
                            const wef_backend_api_t* api,
                            wef_notification_event_fn on_event,
                            void* user_data) override;
  void CloseNotification(uint32_t notification_id) override;

  // Shell_NotifyIcon balloons have no permission model — always granted.
  void QueryPermission(int kind, wef_permission_callback_fn cb,
                       void* user_data) override {
    if (cb)
      cb(user_data, kind == WEF_PERMISSION_NOTIFICATIONS
                        ? WEF_PERMISSION_STATUS_GRANTED
                        : WEF_PERMISSION_STATUS_UNSUPPORTED);
  }
  void RequestPermission(int kind, wef_permission_callback_fn cb,
                         void* user_data) override {
    QueryPermission(kind, cb, user_data);
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
  uint32_t wid = WefIdForHwnd(hwnd);

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
                      ? WEF_MOUSE_PRESSED
                      : WEF_MOUSE_RELEASED;
      int button;
      switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
          button = WEF_MOUSE_BUTTON_LEFT;
          break;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
          button = WEF_MOUSE_BUTTON_RIGHT;
          break;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
          button = WEF_MOUSE_BUTTON_MIDDLE;
          break;
        default:
          button = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1)
                       ? WEF_MOUSE_BUTTON_BACK
                       : WEF_MOUSE_BUTTON_FORWARD;
          break;
      }
      double x = static_cast<double>(GET_X_LPARAM(lParam));
      double y = static_cast<double>(GET_Y_LPARAM(lParam));
      uint32_t modifiers = keyboard::GetWefModifiers();
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
      uint32_t modifiers = keyboard::GetWefModifiers();
      bool repeat = (lParam & (1 << 30)) != 0;
      RuntimeLoader::GetInstance()->DispatchKeyboardEvent(
          wid, WEF_KEY_PRESSED, key.c_str(), code.c_str(), modifiers, repeat);
      break;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
      if (wid == 0)
        break;
      std::string key = keyboard::VirtualKeyToKey(wParam, lParam);
      std::string code = keyboard::VirtualKeyToCode(wParam, lParam);
      uint32_t modifiers = keyboard::GetWefModifiers();
      RuntimeLoader::GetInstance()->DispatchKeyboardEvent(
          wid, WEF_KEY_RELEASED, key.c_str(), code.c_str(), modifiers, false);
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
        g_hwnd_to_wef_id.erase(hwnd);
        if (g_hwnd_to_wef_id.empty()) {
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
  wc.lpszClassName = L"WefWebView2";
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
      g_hwnd_to_wef_id.erase(state.hwnd);
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
  HWND hwnd = CreateWindowExW(
      0, L"WefWebView2", L"", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
      width, height, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);

  {
    std::lock_guard<std::mutex> lock(g_hwnd_mutex);
    g_hwnd_to_wef_id[hwnd] = window_id;
  }

  WinWindowState state;
  state.window_id = window_id;
  state.hwnd = hwnd;

  {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    windows_[window_id] = state;
  }

  InitializeWebViewForWindow(window_id, hwnd);

  ShowWindow(hwnd, SW_SHOW);
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
      g_hwnd_to_wef_id.erase(state->hwnd);
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
                                wef_js_result_fn callback,
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
                auto errVal = wef::Value::String("ExecuteScript failed");
                wef_value errWef(errVal);
                callback(nullptr, &errWef, callback_data);
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
              wef_value wef(val);
              callback(&wef, nullptr, callback_data);
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
                                       wef::ValuePtr args) {
  std::string argsJson = json::Serialize(args);
  std::string script = "window.__wefInvokeCallback(" +
                       std::to_string(callback_id) + ", " + argsJson + ");";
  std::wstring wscript = Utf8ToWide(script);
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
  std::string script =
      "window.__wefReleaseCallback(" + std::to_string(callback_id) + ");";
  std::wstring wscript = Utf8ToWide(script);
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
                                      wef::ValuePtr result,
                                      wef::ValuePtr error) {
  std::string resultJson = json::Serialize(result);
  std::string script;
  if (error) {
    std::string errorJson = json::Serialize(error);
    script = "window.__wefRespond(" + std::to_string(call_id) + ", null, " +
             errorJson + ");";
  } else {
    script = "window.__wefRespond(" + std::to_string(call_id) + ", " +
             resultJson + ", null);";
  }
  std::wstring wscript = Utf8ToWide(script);
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
  wef::ValuePtr msg = json::ParseJson(jsonStr);
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
  wef::ValuePtr args =
      (argsIt != dict.end()) ? argsIt->second : wef::Value::List();

  RuntimeLoader::GetInstance()->OnJsCall(window_id, call_id, method, args);
}

// ============================================================================
// Application Menu
// ============================================================================

void WebView2Backend::SetApplicationMenu(uint32_t window_id,
                                         wef_value_t* menu_template,
                                         const wef_backend_api_t* api,
                                         wef_menu_click_fn on_click,
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
                                      wef_value_t* menu_template,
                                      const wef_backend_api_t* api,
                                      wef_menu_click_fn on_click,
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

int WebView2Backend::ShowDialog(uint32_t window_id, int dialog_type,
                                const std::string& title,
                                const std::string& message,
                                const std::string& default_value,
                                char** out_input_value) {
  if (out_input_value)
    *out_input_value = nullptr;
  // Convert strings to wide strings for Win32 API
  auto toWide = [](const std::string& s) -> std::wstring {
    if (s.empty())
      return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring ws(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
  };

  std::wstring wTitle = toWide(title);
  std::wstring wMessage = toWide(message);

  HWND hwnd = nullptr;
  auto* win = GetWindow(window_id);
  if (win)
    hwnd = win->hwnd;

  if (dialog_type == WEF_DIALOG_ALERT) {
    MessageBoxW(hwnd, wMessage.c_str(), wTitle.c_str(),
                MB_OK | MB_ICONINFORMATION);
    return 1;
  } else if (dialog_type == WEF_DIALOG_CONFIRM) {
    int ret = MessageBoxW(hwnd, wMessage.c_str(), wTitle.c_str(),
                          MB_OKCANCEL | MB_ICONQUESTION);
    return (ret == IDOK) ? 1 : 0;
  } else if (dialog_type == WEF_DIALOG_PROMPT) {
    // Windows does not have a built-in prompt dialog, so use PowerShell
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
    // Trim trailing whitespace
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

// ============================================================================
// Dock / taskbar (Windows)
// ============================================================================

void WebView2Backend::BounceDock(int type) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  for (auto& [wid, state] : windows_) {
    if (!state.hwnd)
      continue;
    FLASHWINFO fi = {sizeof(FLASHWINFO), state.hwnd, 0, 0, 0};
    if (type == WEF_DOCK_BOUNCE_CRITICAL) {
      fi.dwFlags = FLASHW_ALL | FLASHW_TIMER;
      fi.uCount = 0;
    } else {
      fi.dwFlags = FLASHW_TIMERNOFG;
      fi.uCount = 3;
    }
    FlashWindowEx(&fi);
  }
}

// Badge via title prefix. See CEF backend for the same best-effort model.
static std::mutex g_wv_badge_mutex;
static std::map<uint32_t, std::wstring> g_wv_saved_titles;

void WebView2Backend::SetDockBadge(const char* badge_or_null) {
  std::string badge =
      (badge_or_null && *badge_or_null) ? std::string(badge_or_null) : "";
  std::lock_guard<std::mutex> wlock(windows_mutex_);
  std::lock_guard<std::mutex> block(g_wv_badge_mutex);
  for (auto& [wid, state] : windows_) {
    if (!state.hwnd)
      continue;
    if (!badge.empty()) {
      if (g_wv_saved_titles.find(wid) == g_wv_saved_titles.end()) {
        wchar_t buf[512];
        int n = GetWindowTextW(state.hwnd, buf, 512);
        g_wv_saved_titles[wid] = std::wstring(buf, n);
      }
      std::wstring prefixed =
          L"(" + Utf8ToWide(badge) + L") " + g_wv_saved_titles[wid];
      SetWindowTextW(state.hwnd, prefixed.c_str());
    } else {
      auto it = g_wv_saved_titles.find(wid);
      if (it != g_wv_saved_titles.end()) {
        SetWindowTextW(state.hwnd, it->second.c_str());
        g_wv_saved_titles.erase(it);
      }
    }
  }
}

// ============================================================================
// Tray / status bar (Windows) — Shell_NotifyIcon + WIC PNG decode
// ============================================================================

#define WM_WV_TRAYICON (WM_APP + 2)
#define WM_WV_NOTIFICATION (WM_APP + 3)

namespace {
struct WvWinNotifEntry {
  UINT uid;
  HICON hicon;
  std::string tag;
  wef_notification_event_fn on_event;
  void* user_data;
};
std::mutex& WvNotifMutexWin() {
  static std::mutex m;
  return m;
}
std::map<uint32_t, WvWinNotifEntry>& WvNotifMapWin() {
  static std::map<uint32_t, WvWinNotifEntry> m;
  return m;
}
std::map<UINT, uint32_t>& WvNotifUidToId() {
  static std::map<UINT, uint32_t> m;
  return m;
}
std::atomic<uint32_t> g_wv_next_notif_id_win{1};
std::atomic<UINT> g_wv_next_notif_uid{0x4000};

struct WvWinTrayEntry {
  UINT uid;
  HICON hicon_light;
  HICON hicon_dark;
  HMENU hmenu;
  std::map<UINT, std::string> cmd_to_id;
  wef_menu_click_fn menu_click_fn;
  void* menu_click_data;
  wef_tray_click_fn click_fn;
  void* click_data;
  wef_tray_click_fn dblclick_fn;
  void* dblclick_data;
};
std::mutex& WvWinTrayMutex() {
  static std::mutex m;
  return m;
}
std::map<uint32_t, WvWinTrayEntry>& WvWinTrayMap() {
  static std::map<uint32_t, WvWinTrayEntry> m;
  return m;
}
std::atomic<uint32_t> g_wv_next_tray_id_win{1};
std::atomic<UINT> g_wv_next_cmd_id{5000};
HWND g_wv_tray_hwnd = nullptr;

bool WvWinIsDarkMode() {
  DWORD data = 1, size = sizeof(data), kind = 0;
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\"
                    L"Personalize",
                    0, KEY_READ, &key) != 0)
    return false;
  LONG rc = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &kind,
                             (LPBYTE)&data, &size);
  RegCloseKey(key);
  return (rc == 0 && kind == REG_DWORD && data == 0);
}

void WvApplyActiveIcon(uint32_t tray_id) {
  if (!g_wv_tray_hwnd)
    return;
  std::lock_guard<std::mutex> lock(WvWinTrayMutex());
  auto it = WvWinTrayMap().find(tray_id);
  if (it == WvWinTrayMap().end())
    return;
  HICON chosen = (WvWinIsDarkMode() && it->second.hicon_dark)
                     ? it->second.hicon_dark
                     : it->second.hicon_light;
  if (!chosen)
    return;
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g_wv_tray_hwnd;
  nid.uID = tray_id;
  nid.uFlags = NIF_ICON;
  nid.hIcon = chosen;
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void WvReapplyAllIcons() {
  std::vector<uint32_t> ids;
  {
    std::lock_guard<std::mutex> lock(WvWinTrayMutex());
    for (auto& [tid, e] : WvWinTrayMap())
      ids.push_back(tid);
  }
  for (uint32_t id : ids)
    WvApplyActiveIcon(id);
}

LRESULT CALLBACK WvTrayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_SETTINGCHANGE) {
    if (lp && wcscmp((LPCWSTR)lp, L"ImmersiveColorSet") == 0) {
      WvReapplyAllIcons();
    }
    return 0;
  }
  if (msg == WM_WV_NOTIFICATION) {
    UINT uid = (UINT)wp;
    UINT event = LOWORD(lp);
    uint32_t nid = 0;
    {
      std::lock_guard<std::mutex> lock(WvNotifMutexWin());
      auto it = WvNotifUidToId().find(uid);
      if (it != WvNotifUidToId().end())
        nid = it->second;
    }
    if (!nid)
      return 0;
    int reason = -1;
    if (event == NIN_BALLOONSHOW)
      reason = WEF_NOTIFICATION_SHOWN;
    else if (event == NIN_BALLOONUSERCLICK)
      reason = WEF_NOTIFICATION_CLICKED;
    else if (event == NIN_BALLOONHIDE || event == NIN_BALLOONTIMEOUT)
      reason = WEF_NOTIFICATION_CLOSED;
    if (reason < 0)
      return 0;
    wef_notification_event_fn fn = nullptr;
    void* user_data = nullptr;
    bool is_terminal =
        (reason == WEF_NOTIFICATION_CLOSED ||
         reason == WEF_NOTIFICATION_CLICKED);
    {
      std::lock_guard<std::mutex> lock(WvNotifMutexWin());
      auto it = WvNotifMapWin().find(nid);
      if (it != WvNotifMapWin().end()) {
        fn = it->second.on_event;
        user_data = it->second.user_data;
      }
    }
    if (fn)
      fn(user_data, nid, reason, nullptr);
    if (is_terminal) {
      std::lock_guard<std::mutex> lock(WvNotifMutexWin());
      auto it = WvNotifMapWin().find(nid);
      if (it != WvNotifMapWin().end()) {
        NOTIFYICONDATAW del = {};
        del.cbSize = sizeof(del);
        del.hWnd = hwnd;
        del.uID = it->second.uid;
        Shell_NotifyIconW(NIM_DELETE, &del);
        if (it->second.hicon)
          DestroyIcon(it->second.hicon);
        WvNotifUidToId().erase(it->second.uid);
        WvNotifMapWin().erase(it);
      }
    }
    return 0;
  }
  if (msg == WM_WV_TRAYICON) {
    uint32_t tray_id = (uint32_t)wp;
    UINT event = LOWORD(lp);
    if (event == WM_LBUTTONDBLCLK) {
      wef_tray_click_fn fn = nullptr;
      void* data = nullptr;
      {
        std::lock_guard<std::mutex> lock(WvWinTrayMutex());
        auto it = WvWinTrayMap().find(tray_id);
        if (it != WvWinTrayMap().end()) {
          fn = it->second.dblclick_fn;
          data = it->second.dblclick_data;
        }
      }
      if (fn)
        fn(data, tray_id);
      return 0;
    }
    if (event == WM_LBUTTONUP) {
      wef_tray_click_fn fn = nullptr;
      void* data = nullptr;
      {
        std::lock_guard<std::mutex> lock(WvWinTrayMutex());
        auto it = WvWinTrayMap().find(tray_id);
        if (it != WvWinTrayMap().end()) {
          fn = it->second.click_fn;
          data = it->second.click_data;
        }
      }
      if (fn)
        fn(data, tray_id);
    } else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
      HMENU menu = nullptr;
      {
        std::lock_guard<std::mutex> lock(WvWinTrayMutex());
        auto it = WvWinTrayMap().find(tray_id);
        if (it != WvWinTrayMap().end())
          menu = it->second.hmenu;
      }
      if (menu) {
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd);
        UINT cmd =
            TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                           pt.x, pt.y, 0, hwnd, nullptr);
        if (cmd) {
          wef_menu_click_fn fn = nullptr;
          void* data = nullptr;
          std::string item_id;
          {
            std::lock_guard<std::mutex> lock(WvWinTrayMutex());
            auto it = WvWinTrayMap().find(tray_id);
            if (it != WvWinTrayMap().end()) {
              auto cit = it->second.cmd_to_id.find(cmd);
              if (cit != it->second.cmd_to_id.end())
                item_id = cit->second;
              fn = it->second.menu_click_fn;
              data = it->second.menu_click_data;
            }
          }
          if (fn && !item_id.empty())
            fn(data, tray_id, item_id.c_str());
        }
      }
    }
    return 0;
  }
  return DefWindowProc(hwnd, msg, wp, lp);
}

HWND EnsureWvTrayWindow() {
  if (g_wv_tray_hwnd)
    return g_wv_tray_hwnd;
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WvTrayWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"WefWvTrayMessageWindow";
  RegisterClassExW(&wc);
  g_wv_tray_hwnd =
      CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                      nullptr, wc.hInstance, nullptr);
  return g_wv_tray_hwnd;
}

HICON WvDecodePngToHicon(const void* bytes, size_t len, int desired) {
  if (!bytes || len == 0)
    return nullptr;
  IWICImagingFactory* factory = nullptr;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
    return nullptr;
  }
  IWICStream* stream = nullptr;
  factory->CreateStream(&stream);
  stream->InitializeFromMemory((BYTE*)bytes, (DWORD)len);
  IWICBitmapDecoder* decoder = nullptr;
  factory->CreateDecoderFromStream(stream, nullptr,
                                   WICDecodeMetadataCacheOnLoad, &decoder);
  IWICBitmapFrameDecode* frame = nullptr;
  if (decoder)
    decoder->GetFrame(0, &frame);
  IWICFormatConverter* conv = nullptr;
  factory->CreateFormatConverter(&conv);
  if (frame && conv)
    conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                     WICBitmapDitherTypeNone, nullptr, 0.0,
                     WICBitmapPaletteTypeCustom);
  IWICBitmapScaler* scaler = nullptr;
  factory->CreateBitmapScaler(&scaler);
  UINT w = desired, h = desired;
  if (conv)
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
    if (ii.hbmColor && bits)
      memcpy(bits, pixels.data(), pixels.size());
    ii.hbmMask = CreateBitmap(w, h, 1, 1, nullptr);
    if (ii.hbmColor && ii.hbmMask)
      hicon = CreateIconIndirect(&ii);
    if (ii.hbmColor)
      DeleteObject(ii.hbmColor);
    if (ii.hbmMask)
      DeleteObject(ii.hbmMask);
  }
  if (scaler)
    scaler->Release();
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
  return hicon;
}

HMENU WvBuildMenuFromValue(wef_value_t* val, const wef_backend_api_t* api,
                           std::map<UINT, std::string>& cmd_to_id) {
  if (!val || !api->value_is_list(val))
    return nullptr;
  HMENU menu = CreatePopupMenu();
  size_t count = api->value_list_size(val);
  for (size_t i = 0; i < count; ++i) {
    wef_value_t* itemVal = api->value_list_get(val, i);
    if (!itemVal || !api->value_is_dict(itemVal))
      continue;
    wef_value_t* typeVal = api->value_dict_get(itemVal, "type");
    if (typeVal && api->value_is_string(typeVal)) {
      size_t len = 0;
      char* s = api->value_get_string(typeVal, &len);
      if (s && std::string(s) == "separator") {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        api->value_free_string(s);
        continue;
      }
      if (s)
        api->value_free_string(s);
    }
    wef_value_t* labelVal = api->value_dict_get(itemVal, "label");
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
    wef_value_t* submenuVal = api->value_dict_get(itemVal, "submenu");
    if (submenuVal && api->value_is_list(submenuVal)) {
      HMENU sub = WvBuildMenuFromValue(submenuVal, api, cmd_to_id);
      AppendMenuW(menu, MF_POPUP | MF_STRING, (UINT_PTR)sub, wlabel.c_str());
      continue;
    }
    wef_value_t* idVal = api->value_dict_get(itemVal, "id");
    std::string item_id;
    if (idVal && api->value_is_string(idVal)) {
      size_t len = 0;
      char* s = api->value_get_string(idVal, &len);
      if (s) {
        item_id = std::string(s, len);
        api->value_free_string(s);
      }
    }
    UINT cmd = g_wv_next_cmd_id.fetch_add(1, std::memory_order_relaxed);
    UINT flags = MF_STRING;
    wef_value_t* enabledVal = api->value_dict_get(itemVal, "enabled");
    if (enabledVal && api->value_is_bool(enabledVal) &&
        !api->value_get_bool(enabledVal))
      flags |= MF_GRAYED;
    AppendMenuW(menu, flags, cmd, wlabel.c_str());
    if (!item_id.empty())
      cmd_to_id[cmd] = item_id;
  }
  return menu;
}
}  // namespace

uint32_t WebView2Backend::CreateTrayIcon() {
  uint32_t tray_id =
      g_wv_next_tray_id_win.fetch_add(1, std::memory_order_relaxed);
  HWND hwnd = EnsureWvTrayWindow();
  if (!hwnd)
    return 0;
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd;
  nid.uID = tray_id;
  nid.uFlags = NIF_MESSAGE;
  nid.uCallbackMessage = WM_WV_TRAYICON;
  Shell_NotifyIconW(NIM_ADD, &nid);
  WvWinTrayEntry entry = {};
  entry.uid = tray_id;
  std::lock_guard<std::mutex> lock(WvWinTrayMutex());
  WvWinTrayMap()[tray_id] = std::move(entry);
  return tray_id;
}

void WebView2Backend::DestroyTrayIcon(uint32_t tray_id) {
  if (!g_wv_tray_hwnd)
    return;
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g_wv_tray_hwnd;
  nid.uID = tray_id;
  Shell_NotifyIconW(NIM_DELETE, &nid);
  std::lock_guard<std::mutex> lock(WvWinTrayMutex());
  auto it = WvWinTrayMap().find(tray_id);
  if (it != WvWinTrayMap().end()) {
    if (it->second.hicon_light)
      DestroyIcon(it->second.hicon_light);
    if (it->second.hicon_dark)
      DestroyIcon(it->second.hicon_dark);
    if (it->second.hmenu)
      DestroyMenu(it->second.hmenu);
    WvWinTrayMap().erase(it);
  }
}

void WebView2Backend::SetTrayIcon(uint32_t tray_id, const void* png_bytes,
                                  size_t len) {
  if (!g_wv_tray_hwnd)
    return;
  HICON hicon =
      WvDecodePngToHicon(png_bytes, len, GetSystemMetrics(SM_CXSMICON));
  if (!hicon)
    return;
  {
    std::lock_guard<std::mutex> lock(WvWinTrayMutex());
    auto it = WvWinTrayMap().find(tray_id);
    if (it == WvWinTrayMap().end()) {
      DestroyIcon(hicon);
      return;
    }
    if (it->second.hicon_light)
      DestroyIcon(it->second.hicon_light);
    it->second.hicon_light = hicon;
  }
  WvApplyActiveIcon(tray_id);
}

void WebView2Backend::SetTrayIconDark(uint32_t tray_id, const void* png_bytes,
                                      size_t len) {
  if (!g_wv_tray_hwnd)
    return;
  if (!png_bytes || len == 0) {
    {
      std::lock_guard<std::mutex> lock(WvWinTrayMutex());
      auto it = WvWinTrayMap().find(tray_id);
      if (it != WvWinTrayMap().end() && it->second.hicon_dark) {
        DestroyIcon(it->second.hicon_dark);
        it->second.hicon_dark = nullptr;
      }
    }
    WvApplyActiveIcon(tray_id);
    return;
  }
  HICON hicon =
      WvDecodePngToHicon(png_bytes, len, GetSystemMetrics(SM_CXSMICON));
  if (!hicon)
    return;
  {
    std::lock_guard<std::mutex> lock(WvWinTrayMutex());
    auto it = WvWinTrayMap().find(tray_id);
    if (it == WvWinTrayMap().end()) {
      DestroyIcon(hicon);
      return;
    }
    if (it->second.hicon_dark)
      DestroyIcon(it->second.hicon_dark);
    it->second.hicon_dark = hicon;
  }
  WvApplyActiveIcon(tray_id);
}

void WebView2Backend::SetTrayDoubleClickHandler(uint32_t tray_id,
                                                wef_tray_click_fn handler,
                                                void* user_data) {
  std::lock_guard<std::mutex> lock(WvWinTrayMutex());
  auto it = WvWinTrayMap().find(tray_id);
  if (it != WvWinTrayMap().end()) {
    it->second.dblclick_fn = handler;
    it->second.dblclick_data = user_data;
  }
}

void WebView2Backend::SetTrayTooltip(uint32_t tray_id,
                                     const char* tooltip_or_null) {
  if (!g_wv_tray_hwnd)
    return;
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g_wv_tray_hwnd;
  nid.uID = tray_id;
  nid.uFlags = NIF_TIP;
  if (tooltip_or_null && *tooltip_or_null) {
    int n = MultiByteToWideChar(CP_UTF8, 0, tooltip_or_null, -1, nullptr, 0);
    std::wstring tip(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0)
      MultiByteToWideChar(CP_UTF8, 0, tooltip_or_null, -1, tip.data(), n);
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
  }
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void WebView2Backend::SetTrayMenu(uint32_t tray_id, wef_value_t* menu_template,
                                  const wef_backend_api_t* api,
                                  wef_menu_click_fn on_click,
                                  void* on_click_data) {
  std::map<UINT, std::string> cmd_to_id;
  HMENU menu = menu_template
                   ? WvBuildMenuFromValue(menu_template, api, cmd_to_id)
                   : nullptr;
  if (menu_template)
    api->value_free(menu_template);
  std::lock_guard<std::mutex> lock(WvWinTrayMutex());
  auto it = WvWinTrayMap().find(tray_id);
  if (it == WvWinTrayMap().end()) {
    if (menu)
      DestroyMenu(menu);
    return;
  }
  if (it->second.hmenu)
    DestroyMenu(it->second.hmenu);
  it->second.hmenu = menu;
  it->second.cmd_to_id = std::move(cmd_to_id);
  it->second.menu_click_fn = on_click;
  it->second.menu_click_data = on_click_data;
}

void WebView2Backend::SetTrayClickHandler(uint32_t tray_id,
                                          wef_tray_click_fn handler,
                                          void* user_data) {
  std::lock_guard<std::mutex> lock(WvWinTrayMutex());
  auto it = WvWinTrayMap().find(tray_id);
  if (it != WvWinTrayMap().end()) {
    it->second.click_fn = handler;
    it->second.click_data = user_data;
  }
}

// ============================================================================
// Notifications (WebView2 Windows)
// ============================================================================
//
// Same Shell_NotifyIcon balloon approach as the CEF Windows backend.
// Click → CLICKED; dismiss/timeout → CLOSED. Action buttons (NIIF balloons
// don't have them) are ignored.

static HICON WvLoadDefaultAppIcon() {
  HICON h = (HICON)LoadImageW(GetModuleHandleW(nullptr), IDI_APPLICATION,
                              IMAGE_ICON, 0, 0, LR_SHARED | LR_DEFAULTSIZE);
  return h;
}

uint32_t WebView2Backend::ShowNotification(
    wef_value_t* options, const wef_backend_api_t* api,
    wef_notification_event_fn on_event, void* user_data) {
  if (!options)
    return 0;
  if (!api->value_is_dict(options)) {
    api->value_free(options);
    return 0;
  }

  auto get_string = [&](const char* key) -> std::string {
    wef_value_t* v = api->value_dict_get(options, key);
    if (!v || !api->value_is_string(v))
      return std::string();
    size_t len = 0;
    char* s = api->value_get_string(v, &len);
    if (!s)
      return std::string();
    std::string out(s, len);
    api->value_free_string(s);
    return out;
  };
  auto get_bool = [&](const char* key, bool dfl) -> bool {
    wef_value_t* v = api->value_dict_get(options, key);
    if (!v || !api->value_is_bool(v))
      return dfl;
    return api->value_get_bool(v);
  };
  auto get_binary = [&](const char* key) -> std::vector<BYTE> {
    wef_value_t* v = api->value_dict_get(options, key);
    if (!v || !api->value_is_binary(v))
      return {};
    size_t len = 0;
    const void* ptr = api->value_get_binary(v, &len);
    if (!ptr || len == 0)
      return {};
    return std::vector<BYTE>((const BYTE*)ptr, (const BYTE*)ptr + len);
  };

  std::string title = get_string("title");
  std::string body = get_string("body");
  std::string tag = get_string("tag");
  bool silent = get_bool("silent", false);
  std::vector<BYTE> icon_png = get_binary("icon");

  api->value_free(options);

  // Tag → replace existing notification with the same tag.
  if (!tag.empty()) {
    std::vector<uint32_t> to_drop;
    {
      std::lock_guard<std::mutex> lock(WvNotifMutexWin());
      for (auto& [id, e] : WvNotifMapWin()) {
        if (e.tag == tag)
          to_drop.push_back(id);
      }
    }
    for (uint32_t old : to_drop) {
      std::lock_guard<std::mutex> lock(WvNotifMutexWin());
      auto it = WvNotifMapWin().find(old);
      if (it == WvNotifMapWin().end())
        continue;
      HWND hwnd = g_wv_tray_hwnd;
      if (hwnd) {
        NOTIFYICONDATAW del = {};
        del.cbSize = sizeof(del);
        del.hWnd = hwnd;
        del.uID = it->second.uid;
        Shell_NotifyIconW(NIM_DELETE, &del);
      }
      if (it->second.hicon)
        DestroyIcon(it->second.hicon);
      WvNotifUidToId().erase(it->second.uid);
      WvNotifMapWin().erase(it);
    }
  }

  uint32_t nid =
      g_wv_next_notif_id_win.fetch_add(1, std::memory_order_relaxed);
  UINT uid = g_wv_next_notif_uid.fetch_add(1, std::memory_order_relaxed);

  std::wstring wtitle, wbody;
  if (!title.empty()) {
    int n = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
    wtitle.resize(n > 0 ? n - 1 : 0);
    if (n > 0)
      MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, wtitle.data(), n);
  }
  if (!body.empty()) {
    int n = MultiByteToWideChar(CP_UTF8, 0, body.c_str(), -1, nullptr, 0);
    wbody.resize(n > 0 ? n - 1 : 0);
    if (n > 0)
      MultiByteToWideChar(CP_UTF8, 0, body.c_str(), -1, wbody.data(), n);
  }

  HWND hwnd = EnsureWvTrayWindow();
  if (!hwnd)
    return 0;

  HICON hicon = nullptr;
  if (!icon_png.empty()) {
    hicon =
        WvDecodePngToHicon(icon_png.data(), icon_png.size(),
                           GetSystemMetrics(SM_CXICON));
  }
  if (!hicon)
    hicon = WvLoadDefaultAppIcon();

  NOTIFYICONDATAW nd = {};
  nd.cbSize = sizeof(nd);
  nd.hWnd = hwnd;
  nd.uID = uid;
  nd.uFlags = NIF_MESSAGE | NIF_ICON | NIF_INFO;
  nd.uCallbackMessage = WM_WV_NOTIFICATION;
  nd.hIcon = hicon;
  wcsncpy_s(nd.szInfoTitle, wtitle.c_str(), _TRUNCATE);
  wcsncpy_s(nd.szInfo, wbody.c_str(), _TRUNCATE);
  nd.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
  if (silent)
    nd.dwInfoFlags |= NIIF_NOSOUND;

  if (!Shell_NotifyIconW(NIM_ADD, &nd)) {
    if (hicon)
      DestroyIcon(hicon);
    return 0;
  }

  std::lock_guard<std::mutex> lock(WvNotifMutexWin());
  WvWinNotifEntry e = {};
  e.uid = uid;
  e.hicon = hicon;
  e.tag = tag;
  e.on_event = on_event;
  e.user_data = user_data;
  WvNotifMapWin()[nid] = e;
  WvNotifUidToId()[uid] = nid;
  return nid;
}

void WebView2Backend::CloseNotification(uint32_t notification_id) {
  HWND hwnd = g_wv_tray_hwnd;
  wef_notification_event_fn fn = nullptr;
  void* ud = nullptr;
  {
    std::lock_guard<std::mutex> lock(WvNotifMutexWin());
    auto it = WvNotifMapWin().find(notification_id);
    if (it == WvNotifMapWin().end())
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
    WvNotifUidToId().erase(it->second.uid);
    fn = it->second.on_event;
    ud = it->second.user_data;
    WvNotifMapWin().erase(it);
  }
  if (fn)
    fn(ud, notification_id, WEF_NOTIFICATION_CLOSED, nullptr);
}

// ============================================================================
// Factory Function
// ============================================================================

WefBackend* CreateWefBackend() {
  return new WebView2Backend();
}
