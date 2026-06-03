// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "app.h"
#include "runtime_loader.h"
#include "wef_backend_common.h"

#include <iostream>

#ifdef __linux__
#include <gtk/gtk.h>
#endif

#ifdef __APPLE__
// Defined in runtime_loader_mac.mm
struct NativeDialogResult {
  bool confirmed;
  std::string text;
};
NativeDialogResult ShowNativeJSDialog_Mac(int type, const std::string& message,
                                          const std::string& default_text);
#endif

#include "include/base/cef_callback.h"
#include "include/cef_browser.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

std::string g_runtime_path;
std::queue<uint32_t> g_pending_wef_ids;

namespace {
WefHandler* g_handler = nullptr;
}

// WefWindowDelegate implementation

void WefWindowDelegate::OnWindowCreated(CefRefPtr<CefWindow> window) {
  window->AddChildView(browser_view_);

  // Register native window for event routing. Non-activating panels
  // (WEF_WINDOW_FLAG_NO_ACTIVATE) are reconfigured before the first Show()
  // so they float without stealing focus from the foreground app.
  bool no_activate = (flags_ & WEF_WINDOW_FLAG_NO_ACTIVATE) != 0;
  CefWindowHandle handle = window->GetWindowHandle();
  if (handle && wef_id_ > 0) {
#if defined(__APPLE__)
    if (no_activate) {
      ConfigureNSWindowAsPanelForCefHandle(handle);
    }
    RegisterNSWindowForCefHandle(handle, wef_id_);
#elif defined(_WIN32)
    if (no_activate) {
      ConfigureWin32WindowAsPanel((void*)handle);
    }
    RuntimeLoader::GetInstance()->RegisterNativeHandle((void*)(uintptr_t)handle,
                                                       wef_id_);
#elif defined(__linux__)
    if (no_activate) {
      ConfigureLinuxWindowAsPanel(handle);
    }
    RuntimeLoader::GetInstance()->RegisterNativeHandle((void*)(uintptr_t)handle,
                                                       wef_id_);
    MonitorLinuxWindowEvents(handle);
#endif
  }

  window->Show();
  InstallNativeMouseMonitor();
}

bool WefWindowDelegate::IsFrameless(CefRefPtr<CefWindow> window) {
  return (flags_ & WEF_WINDOW_FLAG_FRAMELESS) != 0;
}

cef_state_t WefWindowDelegate::AcceptsFirstMouse(CefRefPtr<CefWindow> window) {
  return (flags_ & WEF_WINDOW_FLAG_NO_ACTIVATE) ? STATE_ENABLED : STATE_DEFAULT;
}

void WefWindowDelegate::OnWindowDestroyed(CefRefPtr<CefWindow> window) {
  // Unregister native window
  CefWindowHandle handle = window->GetWindowHandle();
  if (handle) {
#ifdef __APPLE__
    UnregisterNSWindowForCefHandle(handle);
#else
    RuntimeLoader::GetInstance()->UnregisterNativeHandle(
        (void*)(uintptr_t)handle);
#endif
  }
  if (wef_id_ > 0) {
    RuntimeLoader::GetInstance()->UnregisterBrowser(wef_id_);
  }
  RemoveNativeMouseMonitor();
  browser_view_ = nullptr;
}

bool WefWindowDelegate::CanClose(CefRefPtr<CefWindow> window) {
  CefRefPtr<CefBrowser> browser = browser_view_->GetBrowser();
  return browser ? browser->GetHost()->TryCloseBrowser() : true;
}

CefSize WefWindowDelegate::GetPreferredSize(CefRefPtr<CefView> view) {
  return CefSize(800, 600);
}

WefHandler::WefHandler() {
  g_handler = this;
}

WefHandler::~WefHandler() {
  g_handler = nullptr;
}

WefHandler* WefHandler::GetInstance() {
  return g_handler;
}

void WefHandler::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  browser_list_.push_back(browser);

  auto* loader = RuntimeLoader::GetInstance();
  if (!g_pending_wef_ids.empty()) {
    uint32_t wef_id = g_pending_wef_ids.front();
    g_pending_wef_ids.pop();
    loader->RegisterBrowser(wef_id, browser);
  }
}

bool WefHandler::DoClose(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  if (browser_list_.size() == 1) {
    is_closing_ = true;
  }
  return false;
}

void WefHandler::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();

  auto* loader = RuntimeLoader::GetInstance();
  uint32_t wid = loader->GetWefIdForBrowser(browser);
  if (wid > 0) {
    loader->DispatchCloseRequestedEvent(wid);
  }

  for (auto it = browser_list_.begin(); it != browser_list_.end(); ++it) {
    if ((*it)->IsSame(browser)) {
      browser_list_.erase(it);
      break;
    }
  }
  if (browser_list_.empty()) {
    CefQuitMessageLoop();
  }
}

void WefHandler::OnTitleChange(CefRefPtr<CefBrowser> browser,
                               const CefString& title) {
  CEF_REQUIRE_UI_THREAD();
  if (auto browser_view = CefBrowserView::GetForBrowser(browser)) {
    if (auto window = browser_view->GetWindow()) {
      window->SetTitle(title);
    }
  }
}

// Keyboard mapping lives in backend-common (wef_common::VkToKey / VkToCode).
// CEF normalizes every platform's key events to Windows VK codes, so the
// same table works here.

bool WefHandler::OnKeyEvent(CefRefPtr<CefBrowser> browser,
                            const CefKeyEvent& event, CefEventHandle os_event) {
  int state;
  if (event.type == KEYEVENT_RAWKEYDOWN || event.type == KEYEVENT_KEYDOWN) {
    state = WEF_KEY_PRESSED;
  } else if (event.type == KEYEVENT_KEYUP) {
    state = WEF_KEY_RELEASED;
  } else {
    return false;
  }

  uint32_t modifiers = 0;
  if (event.modifiers & EVENTFLAG_SHIFT_DOWN)
    modifiers |= WEF_MOD_SHIFT;
  if (event.modifiers & EVENTFLAG_CONTROL_DOWN)
    modifiers |= WEF_MOD_CONTROL;
  if (event.modifiers & EVENTFLAG_ALT_DOWN)
    modifiers |= WEF_MOD_ALT;
  if (event.modifiers & EVENTFLAG_COMMAND_DOWN)
    modifiers |= WEF_MOD_META;

  std::string key = wef_common::VkToKey(event.windows_key_code, event.character,
                                        false, false);
  std::string code = wef_common::VkToCode(event.windows_key_code, false, 0);

  uint32_t wid = RuntimeLoader::GetInstance()->GetWefIdForBrowser(browser);
  RuntimeLoader::GetInstance()->DispatchKeyboardEvent(
      wid, state, key.c_str(), code.c_str(), modifiers, false);

  return false;  // Don't consume the event — let CEF handle it too
}

bool WefHandler::OnJSDialog(CefRefPtr<CefBrowser> browser,
                            const CefString& origin_url,
                            JSDialogType dialog_type,
                            const CefString& message_text,
                            const CefString& default_prompt_text,
                            CefRefPtr<CefJSDialogCallback> callback,
                            bool& suppress_message) {
  CEF_REQUIRE_UI_THREAD();
  std::string msg = message_text.ToString();

#ifdef _WIN32
  // Get the native window handle from CEF Views
  HWND hwnd = nullptr;
  if (auto bv = CefBrowserView::GetForBrowser(browser)) {
    if (auto win = bv->GetWindow()) {
      hwnd = win->GetWindowHandle();
    }
  }

  if (dialog_type == JSDialogType::JSDIALOGTYPE_ALERT) {
    std::wstring wmsg(msg.begin(), msg.end());
    MessageBoxW(hwnd, wmsg.c_str(), L"Alert", MB_OK | MB_ICONINFORMATION);
    callback->Continue(true, "");
    return true;
  }
  if (dialog_type == JSDialogType::JSDIALOGTYPE_CONFIRM) {
    std::wstring wmsg(msg.begin(), msg.end());
    int result = MessageBoxW(hwnd, wmsg.c_str(), L"Confirm",
                             MB_OKCANCEL | MB_ICONQUESTION);
    callback->Continue(result == IDOK, "");
    return true;
  }
  if (dialog_type == JSDialogType::JSDIALOGTYPE_PROMPT) {
    std::wstring wmsg(msg.begin(), msg.end());
    int result = MessageBoxW(hwnd, wmsg.c_str(), L"Prompt",
                             MB_OKCANCEL | MB_ICONQUESTION);
    callback->Continue(result == IDOK, default_prompt_text);
    return true;
  }
#elif defined(__APPLE__)
  // macOS: use native NSAlert via helper in runtime_loader_mac.mm
  if (dialog_type == JSDialogType::JSDIALOGTYPE_ALERT) {
    ShowNativeJSDialog_Mac(0, msg, "");
    callback->Continue(true, "");
    return true;
  }
  if (dialog_type == JSDialogType::JSDIALOGTYPE_CONFIRM) {
    auto result = ShowNativeJSDialog_Mac(1, msg, "");
    callback->Continue(result.confirmed, "");
    return true;
  }
  if (dialog_type == JSDialogType::JSDIALOGTYPE_PROMPT) {
    auto result =
        ShowNativeJSDialog_Mac(2, msg, default_prompt_text.ToString());
    callback->Continue(result.confirmed, result.text);
    return true;
  }
#elif defined(__linux__)
  // Linux: use GTK dialogs
  if (dialog_type == JSDialogType::JSDIALOGTYPE_ALERT) {
    GtkWidget* dlg =
        gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                               GTK_BUTTONS_OK, "%s", msg.c_str());
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    callback->Continue(true, "");
    return true;
  }
  if (dialog_type == JSDialogType::JSDIALOGTYPE_CONFIRM) {
    GtkWidget* dlg =
        gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                               GTK_BUTTONS_OK_CANCEL, "%s", msg.c_str());
    gint result = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    callback->Continue(result == GTK_RESPONSE_OK, "");
    return true;
  }
  if (dialog_type == JSDialogType::JSDIALOGTYPE_PROMPT) {
    std::string defaultText = default_prompt_text.ToString();
    GtkWidget* dlg =
        gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                               GTK_BUTTONS_OK_CANCEL, "%s", msg.c_str());
    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), defaultText.c_str());
    gtk_container_add(GTK_CONTAINER(content), entry);
    gtk_widget_show(entry);
    gint result = gtk_dialog_run(GTK_DIALOG(dlg));
    std::string resultText =
        (result == GTK_RESPONSE_OK) ? gtk_entry_get_text(GTK_ENTRY(entry)) : "";
    gtk_widget_destroy(dlg);
    callback->Continue(result == GTK_RESPONSE_OK, resultText);
    return true;
  }
#endif

  return false;
}

bool WefHandler::OnBeforeUnloadDialog(CefRefPtr<CefBrowser> browser,
                                      const CefString& message_text,
                                      bool is_reload,
                                      CefRefPtr<CefJSDialogCallback> callback) {
  callback->Continue(true, "");
  return true;
}

void WefHandler::CloseAllBrowsers(bool force_close) {
  if (!CefCurrentlyOn(TID_UI)) {
    CefPostTask(TID_UI, base::BindOnce(&WefHandler::CloseAllBrowsers, this,
                                       force_close));
    return;
  }
  for (const auto& browser : browser_list_) {
    browser->GetHost()->CloseBrowser(force_close);
  }
}

bool WefHandler::OnProcessMessageReceived(
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
    CefProcessId source_process, CefRefPtr<CefProcessMessage> message) {
  CEF_REQUIRE_UI_THREAD();

  const std::string& name = message->GetName().ToString();

  if (name == "wef_call") {
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    uint64_t call_id = static_cast<uint64_t>(args->GetInt(0));
    std::string method_path = args->GetString(1).ToString();
    CefRefPtr<CefListValue> callArgs = args->GetList(2);

    uint32_t wid = RuntimeLoader::GetInstance()->GetWefIdForBrowser(browser);
    RuntimeLoader::GetInstance()->OnJsCall(wid, call_id, method_path, callArgs);
    return true;
  }

  if (name == "wef_eval_result") {
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    uint64_t eval_id = static_cast<uint64_t>(args->GetInt(0));
    CefRefPtr<CefValue> result = args->GetValue(1);
    std::string error = args->GetString(2).ToString();
    RuntimeLoader::GetInstance()->HandleEvalResult(eval_id, result, error);
    return true;
  }

  return false;
}

void WefApp::OnContextInitialized() {
  CEF_REQUIRE_UI_THREAD();

  // Create the handler and keep it alive for the lifetime of the app.
  // Backend_CreateWindow uses WefHandler::GetInstance() from the runtime
  // thread, so the handler must outlive this function scope.
  static CefRefPtr<WefHandler> handler(new WefHandler());

  if (!g_runtime_path.empty()) {
    if (!RuntimeLoader::GetInstance()->Load(g_runtime_path)) {
      std::cerr << "Failed to load runtime, exiting" << std::endl;
      CefQuitMessageLoop();
      return;
    }
    // Defer Start() to the next message loop iteration. OnContextInitialized
    // runs during CefInitialize(), before CefRunMessageLoop() has started.
    // The runtime thread's Backend_CreateWindow posts CefPostTasks to the UI
    // thread and blocks until they complete — this deadlocks if the message
    // loop isn't running yet.
    CefPostTask(TID_UI, base::BindOnce(
                            []() { RuntimeLoader::GetInstance()->Start(); }));
  } else {
    // No runtime: create a default window for demo
    uint32_t wef_id = RuntimeLoader::GetInstance()->AllocateWindowId();
    g_pending_wef_ids.push(wef_id);
    CefBrowserSettings browser_settings;
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::CreateBrowserView(
        handler, "https://example.com", browser_settings, nullptr, nullptr,
        nullptr);
    CefWindow::CreateTopLevelWindow(
        new WefWindowDelegate(browser_view, wef_id));
  }
}
