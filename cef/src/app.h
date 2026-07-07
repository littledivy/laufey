// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef LAUFEY_APP_H_
#define LAUFEY_APP_H_

#include <cctype>
#include <cstdlib>
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

// Default CEF log severity for CefSettings.log_severity.
//
// Chromium continuously logs ERROR/WARNING lines that are irrelevant to an
// embedded webview app and not actionable by the app developer: GCM
// registration failures (`registration_request.cc ... PHONE_REGISTRATION_ERROR`
// / `DEPRECATED_ENDPOINT`), on-device model service disconnects
// (`on_device_model/...`), xdg desktop-portal "Request cancelled by user",
// long-running `CompositorAnimationObserver` warnings, "Unable to get gpu
// adapter", etc. Targeted `--disable-*` switches can't cover all of them (the
// portal and compositor messages aren't feature-gated), so raise the log floor
// to FATAL by default to hide the noise — matching what Electron production
// apps do. Set LAUFEY_CEF_LOG_SEVERITY to restore output while debugging:
// verbose | debug | info | warning | error | fatal | disable | default.
inline cef_log_severity_t LaufeyCefLogSeverity() {
  const char* env = getenv("LAUFEY_CEF_LOG_SEVERITY");
  if (!env || !*env) {
    return LOGSEVERITY_FATAL;
  }
  std::string v(env);
  for (char& c : v) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (v == "verbose") return LOGSEVERITY_VERBOSE;
  if (v == "debug") return LOGSEVERITY_DEBUG;
  if (v == "info") return LOGSEVERITY_INFO;
  if (v == "warning") return LOGSEVERITY_WARNING;
  if (v == "error") return LOGSEVERITY_ERROR;
  if (v == "fatal") return LOGSEVERITY_FATAL;
  if (v == "disable") return LOGSEVERITY_DISABLE;
  if (v == "default") return LOGSEVERITY_DEFAULT;
  return LOGSEVERITY_FATAL;
}

// Wayland app_id / X11 WM_CLASS for the app's windows. Read at startup from
// LAUFEY_APP_ID (falling back to LAUFEY_APP_NAME). Empty leaves the
// CEF/Chromium default (the backend binary name). On Wayland the compositor
// keys the taskbar/overview icon off this app_id matching an installed
// `<app_id>.desktop`, so it must equal the desktop file's id for the icon to
// show.
extern std::string g_app_id;

// Open `url` in the user's default OS browser. Implemented per platform in
// main_mac.mm / main_windows.cc / main_linux.cc. Used to honor the external
// link redirect policy (laufey_external_links.h).
void LaufeyOpenExternalURL(const std::string& url);

// Queue of laufey window IDs waiting for OnAfterCreated to fire.
// Push before CreateBrowserView, pop in OnAfterCreated.
// Both happen on the UI thread so no synchronization needed.
extern std::queue<uint32_t> g_pending_laufey_ids;

class LaufeyWindowDelegate : public CefWindowDelegate {
 public:
  LaufeyWindowDelegate(CefRefPtr<CefBrowserView> browser_view,
                       uint32_t laufey_id, uint32_t flags = 0)
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

#if defined(__linux__)
  // Advertise the Wayland app_id / X11 WM_CLASS (from g_app_id) so window
  // managers can attribute the right `.desktop` file — and therefore the right
  // taskbar/overview icon — to our windows. Without this the app_id defaults to
  // the backend binary name and Wayland shows a generic placeholder icon.
  bool GetLinuxWindowProperties(CefRefPtr<CefWindow> window,
                                CefLinuxWindowProperties& properties) override;
#endif

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
  // `target="_blank"` / `window.open()` requests aren't seen by the page's
  // Navigation API listener; cancel the popup and open http(s) destinations in
  // the OS browser instead.
  bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                     int popup_id, const CefString& target_url,
                     const CefString& target_frame_name,
                     WindowOpenDisposition target_disposition,
                     bool user_gesture, const CefPopupFeatures& popupFeatures,
                     CefWindowInfo& windowInfo, CefRefPtr<CefClient>& client,
                     CefBrowserSettings& settings,
                     CefRefPtr<CefDictionaryValue>& extra_info,
                     bool* no_javascript_access) override;
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

  void OnContextInitialized() override;

  // Register the custom "app" scheme (standard, secure, fetch/CORS-enabled) so
  // the in-process scheme handler can serve pages over app:// like an https
  // origin. Called early in every process.
  void OnRegisterCustomSchemes(
      CefRawPtr<CefSchemeRegistrar> registrar) override;

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
