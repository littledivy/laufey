// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include <windows.h>
#include <shellapi.h>

#include <iostream>
#include <string>
#include <cstdlib>
#include <cstring>

#include "include/base/cef_callback.h"
#include "include/cef_app.h"
#include "include/cef_sandbox_win.h"
#include "include/cef_task.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#include "app.h"
#include "laufey_backend_common.h"
#include "renderer_app.h"
#include "runtime_loader.h"

void LaufeyOpenExternalURL(const std::string& url) {
  std::wstring wurl = laufey_common::Utf8ToWide(url);
  if (wurl.empty())
    return;
  ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL);
}

// Windows mouse/input monitor for CEF Views windows.
// CEF Views creates its own HWND; we hook into it after window creation.

static HHOOK g_mouse_hook = nullptr;

static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode >= 0) {
    MOUSEHOOKSTRUCT* mhs = reinterpret_cast<MOUSEHOOKSTRUCT*>(lParam);
    RuntimeLoader* loader = RuntimeLoader::GetInstance();

    // Find the laufey window_id from the top-level HWND
    HWND topLevel = mhs->hwnd ? GetAncestor(mhs->hwnd, GA_ROOT) : nullptr;
    uint32_t window_id =
        topLevel ? loader->GetLaufeyIdForNativeHandle((void*)topLevel) : 0;

    POINT pt = mhs->pt;
    if (mhs->hwnd) {
      ScreenToClient(mhs->hwnd, &pt);
    }
    double x = static_cast<double>(pt.x);
    double y = static_cast<double>(pt.y);

    uint32_t modifiers = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000)
      modifiers |= LAUFEY_MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000)
      modifiers |= LAUFEY_MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000)
      modifiers |= LAUFEY_MOD_ALT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000)
      modifiers |= LAUFEY_MOD_META;

    switch (wParam) {
      case WM_LBUTTONDOWN:
        loader->DispatchMouseClickEvent(window_id, LAUFEY_MOUSE_PRESSED,
                                        LAUFEY_MOUSE_BUTTON_LEFT, x, y,
                                        modifiers, 1);
        break;
      case WM_LBUTTONUP:
        loader->DispatchMouseClickEvent(window_id, LAUFEY_MOUSE_RELEASED,
                                        LAUFEY_MOUSE_BUTTON_LEFT, x, y,
                                        modifiers, 1);
        break;
      case WM_RBUTTONDOWN:
        loader->DispatchMouseClickEvent(window_id, LAUFEY_MOUSE_PRESSED,
                                        LAUFEY_MOUSE_BUTTON_RIGHT, x, y,
                                        modifiers, 1);
        break;
      case WM_RBUTTONUP:
        loader->DispatchMouseClickEvent(window_id, LAUFEY_MOUSE_RELEASED,
                                        LAUFEY_MOUSE_BUTTON_RIGHT, x, y,
                                        modifiers, 1);
        break;
      case WM_MBUTTONDOWN:
        loader->DispatchMouseClickEvent(window_id, LAUFEY_MOUSE_PRESSED,
                                        LAUFEY_MOUSE_BUTTON_MIDDLE, x, y,
                                        modifiers, 1);
        break;
      case WM_MBUTTONUP:
        loader->DispatchMouseClickEvent(window_id, LAUFEY_MOUSE_RELEASED,
                                        LAUFEY_MOUSE_BUTTON_MIDDLE, x, y,
                                        modifiers, 1);
        break;
      case WM_MOUSEMOVE:
        loader->DispatchMouseMoveEvent(window_id, x, y, modifiers);
        break;
      case WM_MOUSEWHEEL: {
        // In WH_MOUSE hook, wheel data is in MOUSEHOOKSTRUCTEX::mouseData
        MOUSEHOOKSTRUCTEX* mhsx = reinterpret_cast<MOUSEHOOKSTRUCTEX*>(lParam);
        short delta = HIWORD(mhsx->mouseData);
        double delta_y = static_cast<double>(delta) / WHEEL_DELTA;
        loader->DispatchWheelEvent(window_id, 0.0, delta_y, x, y, modifiers,
                                   LAUFEY_WHEEL_DELTA_LINE);
        break;
      }
    }
  }
  return CallNextHookEx(g_mouse_hook, nCode, wParam, lParam);
}

void InstallNativeMouseMonitor() {
  if (g_mouse_hook)
    return;
  g_mouse_hook =
      SetWindowsHookExW(WH_MOUSE, MouseProc, nullptr, GetCurrentThreadId());
}

void RemoveNativeMouseMonitor() {
  if (g_mouse_hook) {
    UnhookWindowsHookEx(g_mouse_hook);
    g_mouse_hook = nullptr;
  }
}

// --- Headless / forked worker support ---

static int run_headless(const std::string& runtimePath) {
  RuntimeLoader* loader = RuntimeLoader::GetInstance();

  if (runtimePath.empty()) {
    std::cerr << "No runtime library found for headless worker." << std::endl;
    return 1;
  }

  if (!loader->Load(runtimePath)) {
    std::cerr << "Failed to load runtime for headless worker." << std::endl;
    return 1;
  }

  if (!loader->Start()) {
    std::cerr << "Failed to start headless worker runtime." << std::endl;
    return 1;
  }

  loader->Shutdown();
  return 0;
}

static bool is_forked_worker() {
  // Windows equivalent of checking NODE_CHANNEL_FD / NEXT_PRIVATE_WORKER
  char buf[2];
  return GetEnvironmentVariableA("NODE_CHANNEL_FD", buf, sizeof(buf)) > 0 ||
         GetEnvironmentVariableA("NEXT_PRIVATE_WORKER", buf, sizeof(buf)) > 0;
}

static bool is_cli_worker_command(int argc, LPWSTR* argv) {
  if (argc < 3 || wcscmp(argv[1], L"run") != 0) {
    return false;
  }
  for (int i = 2; i < argc; ++i) {
    if (argv[i][0] == L'-') {
      continue;
    }
    return true;
  }
  return false;
}

// Combined app that handles both browser and renderer processes (single-exe
// model)
class LaufeyCombinedApp : public CefApp, public CefBrowserProcessHandler {
 public:
  LaufeyCombinedApp() : renderer_app_(new LaufeyRendererApp()) {}

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }

  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
    return renderer_app_->GetRenderProcessHandler();
  }

  void OnBeforeCommandLineProcessing(
      const CefString& process_type,
      CefRefPtr<CefCommandLine> command_line) override {
    // Silence Chromium's background networking. The GCM (Google Cloud
    // Messaging) client tries to register on startup and logs noisy
    // `registration_request.cc ... PHONE_REGISTRATION_ERROR` /
    // `DEPRECATED_ENDPOINT` errors that have nothing to do with the app. A
    // webview-embedding desktop app doesn't use GCM, the component updater,
    // safebrowsing auto-update, etc., so disable the lot (matches what
    // Electron/Puppeteer do). Only the browser process needs the switch; CEF
    // propagates it to subprocesses.
    if (process_type.empty()) {
      command_line->AppendSwitch("disable-background-networking");
    }
  }

  void OnContextInitialized() override {
    CEF_REQUIRE_UI_THREAD();

    // Keep the handler alive for the lifetime of the app.
    // Backend_CreateWindow uses LaufeyHandler::GetInstance() from the runtime
    // thread, so the handler must outlive this function scope.
    static CefRefPtr<LaufeyHandler> handler(new LaufeyHandler());

    if (!g_runtime_path.empty()) {
      if (!RuntimeLoader::GetInstance()->Load(g_runtime_path)) {
        // WIN32 subsystem: stderr is usually detached, so also show a
        // dialog (matches the webview binary's load-failure behavior).
        std::cerr << "Failed to load runtime from: " << g_runtime_path
                  << std::endl;
        MessageBoxW(nullptr,
                    (L"Failed to load runtime from: " +
                     laufey_common::Utf8ToWide(g_runtime_path))
                        .c_str(),
                    L"LAUFEY Error", MB_OK | MB_ICONERROR);
        CefQuitMessageLoop();
        return;
      }
      // Defer Start() to the next message loop iteration.
      // OnContextInitialized runs during CefInitialize(), before
      // CefRunMessageLoop() has started.
      CefPostTask(TID_UI, base::BindOnce(
                              []() { RuntimeLoader::GetInstance()->Start(); }));
    } else {
      // No runtime: create a default window for demo
      uint32_t laufey_id = RuntimeLoader::GetInstance()->AllocateWindowId();
      g_pending_laufey_ids.push(laufey_id);
      CefBrowserSettings browser_settings;
      CefRefPtr<CefBrowserView> browser_view =
          CefBrowserView::CreateBrowserView(handler, "https://example.com",
                                            browser_settings, nullptr, nullptr,
                                            nullptr);
      CefWindow::CreateTopLevelWindow(
          new LaufeyWindowDelegate(browser_view, laufey_id));
    }
  }

 private:
  CefRefPtr<LaufeyRendererApp> renderer_app_;
  IMPLEMENT_REFCOUNTING(LaufeyCombinedApp);
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
  CefMainArgs main_args(hInstance);

  // Single-exe model: check if we are a subprocess first
  CefRefPtr<LaufeyCombinedApp> app(new LaufeyCombinedApp());
  int exit_code = CefExecuteProcess(main_args, app, nullptr);
  if (exit_code >= 0) {
    return exit_code;
  }

  // Parse --runtime argument
  int argc;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv) {
    for (int i = 1; i < argc; ++i) {
      if (wcscmp(argv[i], L"--runtime") == 0 && i + 1 < argc) {
        g_runtime_path = laufey_common::WideToUtf8(argv[++i]);
      } else if (wcsncmp(argv[i], L"--runtime=", 10) == 0) {
        g_runtime_path = laufey_common::WideToUtf8(argv[i] + 10);
      }
    }
  }

  if (g_runtime_path.empty()) {
    // Read as UTF-16 and convert to UTF-8; the ANSI variant would garble
    // non-ASCII paths in the active codepage.
    std::wstring envPath(MAX_PATH, L'\0');
    DWORD envLen = GetEnvironmentVariableW(L"LAUFEY_RUNTIME_PATH", &envPath[0],
                                           static_cast<DWORD>(envPath.size()));
    if (envLen >= envPath.size()) {
      // Buffer too small; envLen is the required size including the NUL.
      envPath.resize(envLen);
      envLen = GetEnvironmentVariableW(L"LAUFEY_RUNTIME_PATH", &envPath[0],
                                       static_cast<DWORD>(envPath.size()));
    }
    if (envLen > 0 && envLen < envPath.size()) {
      envPath.resize(envLen);
      g_runtime_path = laufey_common::WideToUtf8(envPath);
    }
  }

  if (g_runtime_path.empty()) {
    g_runtime_path = LaufeyFindColocatedRuntime();
  }

  // Check for headless / forked worker mode (skip CEF entirely)
  if (is_forked_worker() || (argv && is_cli_worker_command(argc, argv))) {
    if (argv)
      LocalFree(argv);
    return run_headless(g_runtime_path);
  }
  if (argv)
    LocalFree(argv);

  CefSettings settings;
  settings.no_sandbox = true;
  settings.log_severity = LaufeyCefLogSeverity();

  // Set cache path. CefString decodes std::string as UTF-8, so read the
  // temp dir wide and convert; GetTempPathA would hand over active-codepage
  // bytes that garble non-ASCII profile names. On failure leave the cache
  // path unset (CEF then runs with its in-memory default) rather than
  // pointing it at garbage.
  wchar_t tempPath[MAX_PATH + 2];
  DWORD tempLen = GetTempPathW(MAX_PATH + 2, tempPath);
  if (tempLen > 0 && tempLen < MAX_PATH + 2) {
    std::string cache_path = laufey_common::WideToUtf8(tempPath) +
                             "laufey_cef_" +
                             std::to_string(GetCurrentProcessId());
    CefString(&settings.root_cache_path) = cache_path;
  }

  wchar_t port_buf[16];
  DWORD port_len = GetEnvironmentVariableW(L"LAUFEY_REMOTE_DEBUGGING_PORT",
                                           port_buf, 16);
  // On a value of 16+ chars the API returns the required size and leaves the
  // buffer untouched, so the upper bound is load-bearing.
  if (port_len > 0 && port_len < 16) {
    int port = _wtoi(port_buf);
    if (port > 0 && port < 65536) {
      settings.remote_debugging_port = port;
    }
  }

  if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
    return 1;
  }

  CefRunMessageLoop();

  RuntimeLoader::GetInstance()->Shutdown();

  CefShutdown();

  return 0;
}
