// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef LAUFEY_APP_H_
#define LAUFEY_APP_H_

#include <list>
#include <map>
#include <queue>
#include <string>

#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_jsdialog_handler.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"

extern std::string g_runtime_path;

// Queue of laufey window IDs waiting for OnAfterCreated to fire.
// Push before CreateBrowserView, pop in OnAfterCreated.
// Both happen on the UI thread so no synchronization needed.
extern std::queue<uint32_t> g_pending_laufey_ids;

class LaufeyWindowDelegate : public CefWindowDelegate {
 public:
  LaufeyWindowDelegate(CefRefPtr<CefBrowserView> browser_view, uint32_t laufey_id,
                    uint32_t flags = 0)
      : browser_view_(browser_view), laufey_id_(laufey_id), flags_(flags) {}

  void OnWindowCreated(CefRefPtr<CefWindow> window) override;
  void OnWindowDestroyed(CefRefPtr<CefWindow> window) override;
  bool CanClose(CefRefPtr<CefWindow> window) override;
  CefSize GetPreferredSize(CefRefPtr<CefView> view) override;

  // Frameless windows (LAUFEY_WINDOW_FLAG_FRAMELESS) drop the title bar and
  // standard window buttons.
  bool IsFrameless(CefRefPtr<CefWindow> window) override;
  // Non-activating panels (LAUFEY_WINDOW_FLAG_NO_ACTIVATE) accept the first
  // click without the app having to activate first, so a tray popover can be
  // interacted with while the previously-focused app keeps focus.
  cef_state_t AcceptsFirstMouse(CefRefPtr<CefWindow> window) override;

 private:
  CefRefPtr<CefBrowserView> browser_view_;
  uint32_t laufey_id_ = 0;
  uint32_t flags_ = 0;
  IMPLEMENT_REFCOUNTING(LaufeyWindowDelegate);
};

class LaufeyHandler : public CefClient,
                   public CefLifeSpanHandler,
                   public CefDisplayHandler,
                   public CefKeyboardHandler,
                   public CefDragHandler,
                   public CefJSDialogHandler {
 public:
  LaufeyHandler();
  ~LaufeyHandler() override;

  static LaufeyHandler* GetInstance();

  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
    return this;
  }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override {
    return this;
  }
  CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override {
    return this;
  }
  CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override {
    return this;
  }
  CefRefPtr<CefDragHandler> GetDragHandler() override {
    return this;
  }

  // Forward the page's `-webkit-app-region: drag` rectangles to the window so
  // those areas drag the OS window (used by the transparent-titlebar layout).
  void OnDraggableRegionsChanged(
      CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
      const std::vector<CefDraggableRegion>& regions) override;

  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  bool DoClose(CefRefPtr<CefBrowser> browser) override;
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

  void OnTitleChange(CefRefPtr<CefBrowser> browser,
                     const CefString& title) override;

  bool OnKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent& event,
                  CefEventHandle os_event) override;

  bool OnJSDialog(CefRefPtr<CefBrowser> browser, const CefString& origin_url,
                  JSDialogType dialog_type, const CefString& message_text,
                  const CefString& default_prompt_text,
                  CefRefPtr<CefJSDialogCallback> callback,
                  bool& suppress_message) override;

  bool OnBeforeUnloadDialog(CefRefPtr<CefBrowser> browser,
                            const CefString& message_text, bool is_reload,
                            CefRefPtr<CefJSDialogCallback> callback) override;

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;

  void CloseAllBrowsers(bool force_close);
  bool IsClosing() const {
    return is_closing_;
  }

 private:
  std::list<CefRefPtr<CefBrowser>> browser_list_;
  bool is_closing_ = false;

  IMPLEMENT_REFCOUNTING(LaufeyHandler);
};

class LaufeyApp : public CefApp, public CefBrowserProcessHandler {
 public:
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }

  void OnBeforeCommandLineProcessing(
      const CefString& process_type,
      CefRefPtr<CefCommandLine> command_line) override {
    command_line->AppendSwitch("use-mock-keychain");
  }

  void OnContextInitialized() override;

#if defined(__APPLE__)
  // Drive CefDoMessageLoopWork from the main run loop (external_message_pump)
  // so the libdispatch main queue keeps draining — tray/status-item creation
  // and other dispatch_async(main_queue) work would otherwise never run under
  // CEF's own message loop.
  void OnScheduleMessagePumpWork(int64_t delay_ms) override;
#endif

 private:
  IMPLEMENT_REFCOUNTING(LaufeyApp);
};

#if defined(__APPLE__)
// Stop the [NSApp run] main loop (replaces CefQuitMessageLoop on macOS).
// Implemented in main_mac.mm.
void LaufeyQuitMainLoopMac();
#endif

#endif
