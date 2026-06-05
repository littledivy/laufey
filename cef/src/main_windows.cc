// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include <windows.h>
#include <shellapi.h>

#include <iostream>
#include <string>
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
#include "renderer_app.h"
#include "runtime_loader.h"

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
                                        LAUFEY_MOUSE_BUTTON_LEFT, x, y, modifiers,
                                        1);
        break;
      case WM_LBUTTONUP:
        loader->DispatchMouseClickEvent(window_id, LAUFEY_MOUSE_RELEASED,
                                        LAUFEY_MOUSE_BUTTON_LEFT, x, y, modifiers,
                                        1);
        break;
      case WM_RBUTTONDOWN:
        loader->DispatchMouseClickEvent(window_id, LAUFEY_MOUSE_PRESSED,
                                        LAUFEY_MOUSE_BUTTON_RIGHT, x, y, modifiers,
                                        1);
        break;
      case WM_RBUTTONUP:
        loader->DispatchMouseClickEvent(window_id, LAUFEY_MOUSE_RELEASED,
                                        LAUFEY_MOUSE_BUTTON_RIGHT, x, y, modifiers,
                                        1);
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
    // No macOS-specific flags needed on Windows
  }

  void OnContextInitialized() override {
    CEF_REQUIRE_UI_THREAD();

    // Keep the handler alive for the lifetime of the app.
    // Backend_CreateWindow uses LaufeyHandler::GetInstance() from the runtime
    // thread, so the handler must outlive this function scope.
    static CefRefPtr<LaufeyHandler> handler(new LaufeyHandler());

    if (!g_runtime_path.empty()) {
      if (!RuntimeLoader::GetInstance()->Load(g_runtime_path)) {
        std::cerr << "Failed to load runtime, exiting" << std::endl;
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
        ++i;
        int size = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0,
                                       nullptr, nullptr);
        g_runtime_path.resize(size - 1);
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, &g_runtime_path[0], size,
                            nullptr, nullptr);
      }
    }
  }

  if (g_runtime_path.empty()) {
    char envPath[MAX_PATH];
    if (GetEnvironmentVariableA("LAUFEY_RUNTIME_PATH", envPath, MAX_PATH) > 0) {
      g_runtime_path = envPath;
    }
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

  // Set cache path
  char tempPath[MAX_PATH];
  GetTempPathA(MAX_PATH, tempPath);
  std::string cache_path = std::string(tempPath) + "laufey_cef_" +
                           std::to_string(GetCurrentProcessId());
  CefString(&settings.root_cache_path) = cache_path;

  char port_buf[16];
  if (GetEnvironmentVariableA("LAUFEY_REMOTE_DEBUGGING_PORT", port_buf,
                              sizeof(port_buf)) > 0) {
    int port = atoi(port_buf);
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
