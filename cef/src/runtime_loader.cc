// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "runtime_loader.h"
#include "app.h"
#include "laufey_backend_common.h"

#ifndef _WIN32
#include <dlfcn.h>
#else
#include <windows.h>
#endif

#include <iostream>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <mutex>
#include <condition_variable>

#include "include/base/cef_callback.h"
#include "include/cef_app.h"
#include "include/cef_task.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

RuntimeLoader* RuntimeLoader::instance_ = nullptr;

#ifdef _WIN32
void ConfigureWin32WindowAsPanel(void* hwnd_ptr) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  if (!hwnd)
    return;
  // WS_EX_NOACTIVATE: showing the window doesn't steal focus / foreground
  // from the user's active app. WS_EX_TOOLWINDOW: keep it out of the taskbar
  // and Alt-Tab, matching a menu-bar / tray popover.
  LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
  ex |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
  SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex);
}
#endif

// Helper to run a callback synchronously on the CEF UI thread.
// If already on the UI thread, runs immediately.
template <typename F>
static void cef_invoke_sync(F&& fn) {
  if (CefCurrentlyOn(TID_UI)) {
    fn();
    return;
  }
  std::mutex mtx;
  std::condition_variable cv;
  bool done = false;
  CefPostTask(TID_UI, base::BindOnce(
                          [](F* fn, std::mutex* mtx,
                             std::condition_variable* cv, bool* done) {
                            (*fn)();
                            {
                              std::lock_guard<std::mutex> lock(*mtx);
                              *done = true;
                            }
                            cv->notify_one();
                          },
                          &fn, &mtx, &cv, &done));
  std::unique_lock<std::mutex> lock(mtx);
  cv.wait(lock, [&done] { return done; });
}

// --- Backend API functions (cross-platform, using CEF Views) ---

static void Backend_Navigate(void* data, uint32_t window_id, const char* url) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser && url) {
    std::string url_str(url);
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, std::string u) {
                              b->GetMainFrame()->LoadURL(u);
                            },
                            browser, url_str));
  }
}

static void Backend_SetTitle(void* data, uint32_t window_id,
                             const char* title) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser && title) {
    std::string title_str(title);
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, std::string t) {
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
                              if (browser_view) {
                                auto window = browser_view->GetWindow();
                                if (window) {
                                  window->SetTitle(t);
                                }
                              }
                            },
                            browser, title_str));
  }
}

static void Backend_ExecuteJs(void* data, uint32_t window_id,
                              const char* script, laufey_js_result_fn callback,
                              void* callback_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (!browser || !script) {
    if (callback)
      callback(nullptr, nullptr, callback_data);
    return;
  }

  if (!callback) {
    // Fire-and-forget: use the simple path
    std::string script_str(script);
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, std::string s) {
                              b->GetMainFrame()->ExecuteJavaScript(s, "", 0);
                            },
                            browser, script_str));
    return;
  }

  // With callback: send IPC to renderer for eval with result
  uint64_t eval_id = loader->StoreEvalCallback(callback, callback_data);
  std::string script_str(script);
  CefPostTask(TID_UI,
              base::BindOnce(
                  [](CefRefPtr<CefBrowser> b, uint64_t id, std::string s) {
                    CefRefPtr<CefProcessMessage> msg =
                        CefProcessMessage::Create("laufey_eval");
                    CefRefPtr<CefListValue> args = msg->GetArgumentList();
                    args->SetDouble(0, static_cast<double>(id));
                    args->SetString(1, s);
                    b->GetMainFrame()->SendProcessMessage(PID_RENDERER, msg);
                  },
                  browser, eval_id, script_str));
}

static void Backend_Quit(void* data) {
  CefPostTask(TID_UI, base::BindOnce([]() { CefQuitMessageLoop(); }));
}

static void Backend_SetWindowSize(void* data, uint32_t window_id, int width,
                                  int height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, int w, int h) {
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
                              if (browser_view) {
                                auto window = browser_view->GetWindow();
                                if (window) {
                                  window->SetSize(CefSize(w, h));
                                }
                              }
                            },
                            browser, width, height));
  }
}

static void Backend_GetWindowSize(void* data, uint32_t window_id, int* width,
                                  int* height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  int w = 0, h = 0;
  if (browser) {
    cef_invoke_sync([&] {
      auto browser_view = CefBrowserView::GetForBrowser(browser);
      if (browser_view) {
        auto window = browser_view->GetWindow();
        if (window) {
          CefSize size = window->GetSize();
          w = size.width;
          h = size.height;
        }
      }
    });
  }
  if (width)
    *width = w;
  if (height)
    *height = h;
}

static void Backend_SetWindowPosition(void* data, uint32_t window_id, int x,
                                      int y) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, int px, int py) {
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
                              if (browser_view) {
                                auto window = browser_view->GetWindow();
                                if (window) {
                                  window->SetPosition(CefPoint(px, py));
                                }
                              }
                            },
                            browser, x, y));
  }
}

static void Backend_GetWindowPosition(void* data, uint32_t window_id, int* x,
                                      int* y) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  int px = 0, py = 0;
  if (browser) {
    cef_invoke_sync([&] {
      auto browser_view = CefBrowserView::GetForBrowser(browser);
      if (browser_view) {
        auto window = browser_view->GetWindow();
        if (window) {
          CefPoint pos = window->GetPosition();
          px = pos.x;
          py = pos.y;
        }
      }
    });
  }
  if (x)
    *x = px;
  if (y)
    *y = py;
}

static void Backend_SetResizable(void* data, uint32_t window_id,
                                 bool resizable) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, bool r) {
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
                              if (!browser_view)
                                return;
                              auto window = browser_view->GetWindow();
                              if (!window)
                                return;
#ifdef _WIN32
                              HWND hwnd = window->GetWindowHandle();
                              LONG style = GetWindowLong(hwnd, GWL_STYLE);
                              if (r) {
                                style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
                              } else {
                                style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
                              }
                              SetWindowLong(hwnd, GWL_STYLE, style);
#elif defined(__APPLE__)
                              SetNSWindowResizable(window->GetWindowHandle(),
                                                   r);
#elif defined(__linux__)
                              SetLinuxWindowResizable(window->GetWindowHandle(),
                                                      r);
#endif
                            },
                            browser, resizable));
  }
}

static bool Backend_IsResizable(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  bool result = true;
  if (browser) {
    cef_invoke_sync([&] {
      auto browser_view = CefBrowserView::GetForBrowser(browser);
      if (!browser_view)
        return;
      auto window = browser_view->GetWindow();
      if (!window)
        return;
#ifdef _WIN32
      HWND hwnd = window->GetWindowHandle();
      LONG style = GetWindowLong(hwnd, GWL_STYLE);
      result = (style & WS_THICKFRAME) != 0;
#elif defined(__APPLE__)
      result = IsNSWindowResizable(window->GetWindowHandle());
#elif defined(__linux__)
      result = IsLinuxWindowResizable(window->GetWindowHandle());
#endif
    });
  }
  return result;
}

static void Backend_SetAlwaysOnTop(void* data, uint32_t window_id,
                                   bool always_on_top) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, bool on_top) {
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
                              if (browser_view) {
                                auto window = browser_view->GetWindow();
                                if (window) {
                                  window->SetAlwaysOnTop(on_top);
                                }
                              }
                            },
                            browser, always_on_top));
  }
}

static bool Backend_IsAlwaysOnTop(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  bool result = false;
  if (browser) {
    cef_invoke_sync([&] {
      auto browser_view = CefBrowserView::GetForBrowser(browser);
      if (browser_view) {
        auto window = browser_view->GetWindow();
        if (window) {
          result = window->IsAlwaysOnTop();
        }
      }
    });
  }
  return result;
}

static bool Backend_IsVisible(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  bool result = false;
  if (browser) {
    cef_invoke_sync([&] {
      auto browser_view = CefBrowserView::GetForBrowser(browser);
      if (browser_view) {
        auto window = browser_view->GetWindow();
        if (window) {
          result = window->IsVisible();
        }
      }
    });
  }
  return result;
}

static void Backend_Show(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b) {
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
                              if (browser_view) {
                                auto window = browser_view->GetWindow();
                                if (window) {
                                  window->Show();
                                }
                              }
                            },
                            browser));
  }
}

static void Backend_Hide(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b) {
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
                              if (browser_view) {
                                auto window = browser_view->GetWindow();
                                if (window) {
                                  window->Hide();
                                }
                              }
                            },
                            browser));
  }
}

static void Backend_Focus(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b) {
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
                              if (browser_view) {
                                auto window = browser_view->GetWindow();
                                if (window) {
                                  window->Show();
                                  window->Activate();
                                }
                              }
                            },
                            browser));
  }
}

static void Backend_PostUiTask(void* data, void (*task)(void*),
                               void* task_data) {
  if (task) {
    CefPostTask(TID_UI, base::BindOnce([](void (*t)(void*), void* d) { t(d); },
                                       task, task_data));
  }
}

// --- CefValue <-> laufey::Value conversion (IPC boundary only) ---
//
// Values cross the renderer<->browser process boundary as CefValue trees, but
// the shared marshalling layer operates on laufey::Value. Convert once on the way
// in (incoming JS args / eval results) and once on the way out (responses,
// callback args). A JS function is encoded by the renderer as a dictionary
// {"__callback__": "<id>"}; decode it to laufey::Value::Callback so that
// value_is_callback works, matching the webview backend.

static laufey::ValuePtr CefValueToLaufey(CefRefPtr<CefValue> v) {
  if (!v)
    return laufey::Value::Null();
  switch (v->GetType()) {
    case VTYPE_BOOL:
      return laufey::Value::Bool(v->GetBool());
    case VTYPE_INT:
      return laufey::Value::Int(v->GetInt());
    case VTYPE_DOUBLE:
      return laufey::Value::Double(v->GetDouble());
    case VTYPE_STRING:
      return laufey::Value::String(v->GetString().ToString());
    case VTYPE_BINARY: {
      CefRefPtr<CefBinaryValue> bin = v->GetBinary();
      std::vector<uint8_t> buf(bin->GetSize());
      if (!buf.empty())
        bin->GetData(buf.data(), buf.size(), 0);
      return laufey::Value::Binary(buf.data(), buf.size());
    }
    case VTYPE_LIST: {
      CefRefPtr<CefListValue> list = v->GetList();
      auto out = laufey::Value::List();
      for (size_t i = 0; i < list->GetSize(); ++i) {
        out->GetList().push_back(CefValueToLaufey(list->GetValue(i)));
      }
      return out;
    }
    case VTYPE_DICTIONARY: {
      CefRefPtr<CefDictionaryValue> dict = v->GetDictionary();
      if (dict->HasKey("__callback__")) {
        // Renderer-supplied. CEF builds with -fno-exceptions, so parse without
        // std::stoull (which throws); strtoull returns 0 on a malformed id.
        std::string id = dict->GetString("__callback__").ToString();
        uint64_t cb_id = std::strtoull(id.c_str(), nullptr, 10);
        return laufey::Value::Callback(cb_id);
      }
      auto out = laufey::Value::Dict();
      CefDictionaryValue::KeyList keys;
      dict->GetKeys(keys);
      for (const auto& key : keys) {
        out->GetDict()[key.ToString()] = CefValueToLaufey(dict->GetValue(key));
      }
      return out;
    }
    case VTYPE_NULL:
    case VTYPE_INVALID:
    default:
      return laufey::Value::Null();
  }
}

static CefRefPtr<CefValue> LaufeyToCefValue(const laufey::ValuePtr& v) {
  CefRefPtr<CefValue> out = CefValue::Create();
  if (!v) {
    out->SetNull();
    return out;
  }
  switch (v->type) {
    case laufey::ValueType::Bool:
      out->SetBool(v->GetBool());
      break;
    case laufey::ValueType::Int:
      out->SetInt(v->GetInt());
      break;
    case laufey::ValueType::Double:
      out->SetDouble(v->GetDouble());
      break;
    case laufey::ValueType::String:
      out->SetString(v->GetString());
      break;
    case laufey::ValueType::Binary: {
      const auto& bin = v->GetBinary();
      out->SetBinary(CefBinaryValue::Create(bin.data.data(), bin.data.size()));
      break;
    }
    case laufey::ValueType::List: {
      CefRefPtr<CefListValue> list = CefListValue::Create();
      const auto& items = v->GetList();
      for (size_t i = 0; i < items.size(); ++i) {
        list->SetValue(i, LaufeyToCefValue(items[i]));
      }
      out->SetList(list);
      break;
    }
    case laufey::ValueType::Dict: {
      CefRefPtr<CefDictionaryValue> dict = CefDictionaryValue::Create();
      for (const auto& pair : v->GetDict()) {
        dict->SetValue(pair.first, LaufeyToCefValue(pair.second));
      }
      out->SetDictionary(dict);
      break;
    }
    case laufey::ValueType::Callback: {
      CefRefPtr<CefDictionaryValue> dict = CefDictionaryValue::Create();
      dict->SetString("__callback__", std::to_string(v->GetCallbackId()));
      out->SetDictionary(dict);
      break;
    }
    case laufey::ValueType::Null:
    default:
      out->SetNull();
      break;
  }
  return out;
}

// Build a CefListValue from a laufey list value (empty list otherwise).
static CefRefPtr<CefListValue> LaufeyListToCef(const laufey::ValuePtr& v) {
  CefRefPtr<CefListValue> list = CefListValue::Create();
  if (v && v->IsList()) {
    const auto& items = v->GetList();
    for (size_t i = 0; i < items.size(); ++i) {
      list->SetValue(i, LaufeyToCefValue(items[i]));
    }
  }
  return list;
}

// --- JS call/callback handling ---

static void Backend_SetJsCallHandler(void* data, laufey_js_call_fn handler,
                                     void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetJsCallHandler(handler, user_data);
}

static void Backend_JsCallRespond(void* data, uint64_t call_id,
                                  laufey_value_t* result, laufey_value_t* error) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  uint32_t window_id = loader->ConsumeCallWindow(call_id);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (!browser)
    return;

  CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("laufey_response");
  CefRefPtr<CefListValue> args = msg->GetArgumentList();
  // IDs are 64-bit; carry as double (exact to 2^53) since CefValue has no
  // int64.
  args->SetDouble(0, static_cast<double>(call_id));

  if (result && result->value) {
    args->SetValue(1, LaufeyToCefValue(result->value));
  } else {
    CefRefPtr<CefValue> null_val = CefValue::Create();
    null_val->SetNull();
    args->SetValue(1, null_val);
  }

  if (error && error->value) {
    args->SetValue(2, LaufeyToCefValue(error->value));
  } else {
    CefRefPtr<CefValue> null_val = CefValue::Create();
    null_val->SetNull();
    args->SetValue(2, null_val);
  }

  CefPostTask(TID_UI,
              base::BindOnce(
                  [](CefRefPtr<CefBrowser> b, CefRefPtr<CefProcessMessage> m) {
                    b->GetMainFrame()->SendProcessMessage(PID_RENDERER, m);
                  },
                  browser, msg));
}

static void Backend_InvokeJsCallback(void* data, uint64_t callback_id,
                                     laufey_value_t* args) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);

  CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("laufey_callback");
  CefRefPtr<CefListValue> msgArgs = msg->GetArgumentList();
  msgArgs->SetDouble(0, static_cast<double>(callback_id));

  msgArgs->SetList(1, LaufeyListToCef(args ? args->value : nullptr));

  loader->ForEachBrowser([&msg](CefRefPtr<CefBrowser> browser) {
    CefPostTask(
        TID_UI,
        base::BindOnce(
            [](CefRefPtr<CefBrowser> b, CefRefPtr<CefProcessMessage> m) {
              b->GetMainFrame()->SendProcessMessage(PID_RENDERER, m);
            },
            browser, msg));
  });
}

static void Backend_SetKeyboardEventHandler(void* data,
                                            laufey_keyboard_event_fn handler,
                                            void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetKeyboardEventHandler(handler, user_data);
}

static void Backend_SetMouseClickHandler(void* data, laufey_mouse_click_fn handler,
                                         void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetMouseClickHandler(handler, user_data);
}

static void Backend_SetMouseMoveHandler(void* data, laufey_mouse_move_fn handler,
                                        void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetMouseMoveHandler(handler, user_data);
}

static void Backend_SetWheelHandler(void* data, laufey_wheel_fn handler,
                                    void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetWheelHandler(handler, user_data);
}

static void Backend_SetCursorEnterLeaveHandler(
    void* data, laufey_cursor_enter_leave_fn handler, void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetCursorEnterLeaveHandler(handler, user_data);
}

static void Backend_SetFocusedHandler(void* data, laufey_focused_fn handler,
                                      void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetFocusedHandler(handler, user_data);
}

static void Backend_SetResizeHandler(void* data, laufey_resize_fn handler,
                                     void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetResizeHandler(handler, user_data);
}

static void Backend_SetMoveHandler(void* data, laufey_move_fn handler,
                                   void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetMoveHandler(handler, user_data);
}

static void Backend_ReleaseJsCallback(void* data, uint64_t callback_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);

  CefRefPtr<CefProcessMessage> msg =
      CefProcessMessage::Create("laufey_release_callback");
  CefRefPtr<CefListValue> msgArgs = msg->GetArgumentList();
  msgArgs->SetDouble(0, static_cast<double>(callback_id));

  loader->ForEachBrowser([&msg](CefRefPtr<CefBrowser> browser) {
    CefPostTask(
        TID_UI,
        base::BindOnce(
            [](CefRefPtr<CefBrowser> b, CefRefPtr<CefProcessMessage> m) {
              b->GetMainFrame()->SendProcessMessage(PID_RENDERER, m);
            },
            browser, msg));
  });
}

// --- Platform-specific menu (stub on Windows, implemented in runtime_loader.mm
// on macOS) ---

#if defined(_WIN32)
#include <win32_menu.h>

static void Backend_SetApplicationMenu(void* data, uint32_t window_id,
                                       laufey_value_t* menu_template,
                                       laufey_menu_click_fn on_click,
                                       void* on_click_data) {
  if (!menu_template)
    return;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const laufey_backend_api_t* api = &loader->GetBackendApi();

  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(
        TID_UI,
        base::BindOnce(
            [](CefRefPtr<CefBrowser> b, uint32_t wid, laufey_value_t* tmpl,
               const laufey_backend_api_t* a, laufey_menu_click_fn fn, void* d) {
              HWND hwnd = b->GetHost()->GetWindowHandle();
              if (hwnd) {
                win32_menu::SetApplicationMenu(hwnd, tmpl, a, fn, d, wid);
              }
            },
            browser, window_id, menu_template, api, on_click, on_click_data));
  }
}

static void Backend_ShowContextMenu(void* data, uint32_t window_id, int x,
                                    int y, laufey_value_t* menu_template,
                                    laufey_menu_click_fn on_click,
                                    void* on_click_data) {
  if (!menu_template)
    return;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const laufey_backend_api_t* api = &loader->GetBackendApi();

  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI,
                base::BindOnce(
                    [](CefRefPtr<CefBrowser> b, uint32_t wid, int cx, int cy,
                       laufey_value_t* tmpl, const laufey_backend_api_t* a,
                       laufey_menu_click_fn fn, void* d) {
                      HWND hwnd = b->GetHost()->GetWindowHandle();
                      if (hwnd) {
                        win32_menu::ShowContextMenu(hwnd, cx, cy, tmpl, a, fn,
                                                    d, wid);
                      }
                    },
                    browser, window_id, x, y, menu_template, api, on_click,
                    on_click_data));
  }
}
#elif defined(__APPLE__)
// Defined in runtime_loader_mac.mm
extern void Backend_SetApplicationMenu_Mac(void* data, uint32_t window_id,
                                           laufey_value_t* menu_template,
                                           laufey_menu_click_fn on_click,
                                           void* on_click_data);
extern void Backend_ShowContextMenu_Mac(void* data, uint32_t window_id, int x,
                                        int y, laufey_value_t* menu_template,
                                        laufey_menu_click_fn on_click,
                                        void* on_click_data);
extern void Backend_SetDockBadge_Mac(void* data, const char* badge_or_null);
extern void Backend_BounceDock_Mac(void* data, int type);
extern void Backend_SetDockMenu_Mac(void* data, laufey_value_t* menu_template,
                                    laufey_menu_click_fn on_click,
                                    void* on_click_data);
extern void Backend_SetDockVisible_Mac(void* data, bool visible);
extern void Backend_SetDockReopenHandler_Mac(void* data,
                                             laufey_dock_reopen_fn handler,
                                             void* user_data);

extern uint32_t Backend_CreateTrayIcon_Mac(void* data);
extern void Backend_DestroyTrayIcon_Mac(void* data, uint32_t tray_id);
extern void Backend_SetTrayIcon_Mac(void* data, uint32_t tray_id,
                                    const void* png_bytes, size_t len);
extern void Backend_SetTrayTooltip_Mac(void* data, uint32_t tray_id,
                                       const char* tooltip_or_null);
extern void Backend_SetTrayMenu_Mac(void* data, uint32_t tray_id,
                                    laufey_value_t* menu_template,
                                    laufey_menu_click_fn on_click,
                                    void* on_click_data);
extern void Backend_SetTrayClickHandler_Mac(void* data, uint32_t tray_id,
                                            laufey_tray_click_fn handler,
                                            void* user_data);
extern void Backend_SetTrayDoubleClickHandler_Mac(void* data, uint32_t tray_id,
                                                  laufey_tray_click_fn handler,
                                                  void* user_data);
extern void Backend_SetTrayIconDark_Mac(void* data, uint32_t tray_id,
                                        const void* png_bytes, size_t len);
extern bool Backend_GetTrayIconBounds_Mac(void* data, uint32_t tray_id, int* x,
                                          int* y, int* width, int* height);
extern uint32_t Backend_ShowNotification_Mac(void* data, laufey_value_t* options,
                                             laufey_notification_event_fn on_event,
                                             void* user_data);
extern void Backend_CloseNotification_Mac(void* data, uint32_t notification_id);
extern void Backend_QueryPermission_Mac(void* data, int kind,
                                        laufey_permission_callback_fn cb,
                                        void* user_data);
extern void Backend_RequestPermission_Mac(void* data, int kind,
                                          laufey_permission_callback_fn cb,
                                          void* user_data);
#elif defined(__linux__)
// Defined in runtime_loader_linux.cc
extern void Backend_ShowContextMenu_Linux(void* data, uint32_t window_id, int x,
                                          int y, laufey_value_t* menu_template,
                                          laufey_menu_click_fn on_click,
                                          void* on_click_data);
extern uint32_t Backend_CreateTrayIcon_Linux(void* data);
extern void Backend_DestroyTrayIcon_Linux(void* data, uint32_t tray_id);
extern void Backend_SetTrayIcon_Linux(void* data, uint32_t tray_id,
                                      const void* png_bytes, size_t len);
extern void Backend_SetTrayTooltip_Linux(void* data, uint32_t tray_id,
                                         const char* tooltip_or_null);
extern void Backend_SetTrayMenu_Linux(void* data, uint32_t tray_id,
                                      laufey_value_t* menu_template,
                                      laufey_menu_click_fn on_click,
                                      void* on_click_data);
extern void Backend_SetTrayClickHandler_Linux(void* data, uint32_t tray_id,
                                              laufey_tray_click_fn handler,
                                              void* user_data);
extern void Backend_SetTrayDoubleClickHandler_Linux(void* data,
                                                    uint32_t tray_id,
                                                    laufey_tray_click_fn handler,
                                                    void* user_data);
extern void Backend_SetTrayIconDark_Linux(void* data, uint32_t tray_id,
                                          const void* png_bytes, size_t len);
extern "C" uint32_t Backend_ShowNotification_Linux(
    void* data, laufey_value_t* options, laufey_notification_event_fn on_event,
    void* user_data);
extern "C" void Backend_CloseNotification_Linux(void* data,
                                                uint32_t notification_id);
#endif

// --- Permissions / runtime authorization ---
//
// macOS routes to UNUserNotificationCenter (see runtime_loader_mac.mm).
// Windows + Linux permission stubs live in backend-common
// (laufey_common::QueryPermissionStub / RequestPermissionStub).
#if !defined(__APPLE__)
static void Backend_QueryPermission_Stub(void* /*data*/, int kind,
                                         laufey_permission_callback_fn cb,
                                         void* user_data) {
  laufey_common::QueryPermissionStub(kind, cb, user_data);
}

static void Backend_RequestPermission_Stub(void* /*data*/, int kind,
                                           laufey_permission_callback_fn cb,
                                           void* user_data) {
  laufey_common::RequestPermissionStub(kind, cb, user_data);
}
#endif

// --- Dock / taskbar (Windows + Linux) ---
//
// The dock is a macOS concept; on Windows the analog is the taskbar button
// (per-window), and on Linux it's the WM urgency hint. Bounce maps cleanly:
//   - Windows: FlashWindowEx on every LAUFEY window's HWND.
//   - Linux:   X11 UrgencyHint on every LAUFEY window's X11 Window.
// Badge is implemented as a `"(N) " prefix on each window's title — the
// convention used by Slack/Discord/Telegram. If user code updates the title
// while a badge is active, the badge falls off (best-effort v1; proper
// Windows overlay icons and Linux libunity are future work). Menu, visible,
// and reopen have no clean analog.

#if !defined(__APPLE__)
#include <map>
#include <mutex>
#include <string>

#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"

// Badge is implemented as a title prefix. Per window we remember the
// "original" title at the moment a badge is applied, so clearing can
// restore it. If user code updates the window title while a badge is
// active, the badge is visually lost — that's a best-effort v1
// limitation; calling set_dock_badge again re-applies it on top of the
// (now-stale) saved original.

static void Backend_SetDockBadge_TitlePrefix(void* data,
                                             const char* badge_or_null) {
  std::string badge =
      (badge_or_null && *badge_or_null) ? std::string(badge_or_null) : "";
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);

  loader->ForEachBrowserWithId([&badge](uint32_t wid,
                                        CefRefPtr<CefBrowser> browser) {
    CefPostTask(TID_UI,
                base::BindOnce(
                    [](uint32_t wid, CefRefPtr<CefBrowser> b, std::string bg) {
                      auto bv = CefBrowserView::GetForBrowser(b);
                      if (!bv)
                        return;
                      auto win = bv->GetWindow();
                      if (!win)
                        return;
                      std::string current = win->GetTitle().ToString();
                      std::string next =
                          laufey_common::ApplyTitlePrefixBadge(wid, current, bg);
                      win->SetTitle(next);
                    },
                    wid, browser, badge));
  });
}
#endif  // !__APPLE__

#if defined(_WIN32)
#include <shellapi.h>
#include <windows.h>
#include <windowsx.h>
#include <wincodec.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "windowscodecs.lib")

static void Backend_BounceDock_Win(void* data, int type) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->ForEachBrowser([type](CefRefPtr<CefBrowser> browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, int t) {
                              HWND hwnd = b->GetHost()->GetWindowHandle();
                              if (!hwnd)
                                return;
                              FLASHWINFO fi = {sizeof(FLASHWINFO), hwnd, 0, 0,
                                               0};
                              if (t == LAUFEY_DOCK_BOUNCE_CRITICAL) {
                                fi.dwFlags = FLASHW_ALL | FLASHW_TIMER;
                                fi.uCount = 0;
                              } else {
                                fi.dwFlags = FLASHW_TIMERNOFG;
                                fi.uCount = 3;
                              }
                              FlashWindowEx(&fi);
                            },
                            browser, type));
  });
}

// --- Tray (Windows) ---
//
// Shell_NotifyIcon + a hidden message-only window that receives
// WM_TRAYICON (one per process). PNG → HICON via WIC.

// --- Tray (Windows) ---
//
// Thin trampolines over backend-common/src/tray_win.cc. CefPostTask
// marshals to the UI thread since the common impl is synchronous.

uint32_t Backend_CreateTrayIcon_Win(void* /*data*/) {
  // Allocate the id synchronously; do the Shell_NotifyIcon setup on the
  // UI thread so the message-only window is owned by the thread that
  // pumps messages for it.
  uint32_t tray_id = laufey_common::CreateTrayIconWin();
  CefPostTask(
      TID_UI,
      base::BindOnce([](uint32_t tid) { laufey_common::FinalizeTrayIconWin(tid); },
                     tray_id));
  return tray_id;
}

void Backend_DestroyTrayIcon_Win(void* /*data*/, uint32_t tray_id) {
  CefPostTask(
      TID_UI,
      base::BindOnce([](uint32_t tid) { laufey_common::DestroyTrayIconWin(tid); },
                     tray_id));
}

void Backend_SetTrayIcon_Win(void* /*data*/, uint32_t tray_id,
                             const void* png_bytes, size_t len) {
  if (!png_bytes || len == 0)
    return;
  std::vector<BYTE> copy((const BYTE*)png_bytes, (const BYTE*)png_bytes + len);
  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t tid, std::vector<BYTE> b) {
                            laufey_common::SetTrayIconWin(tid, b.data(), b.size());
                          },
                          tray_id, std::move(copy)));
}

void Backend_SetTrayIconDark_Win(void* /*data*/, uint32_t tray_id,
                                 const void* png_bytes, size_t len) {
  std::vector<BYTE> copy;
  if (png_bytes && len > 0) {
    copy.assign((const BYTE*)png_bytes, (const BYTE*)png_bytes + len);
  }
  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t tid, std::vector<BYTE> b) {
                            laufey_common::SetTrayIconDarkWin(
                                tid, b.empty() ? nullptr : b.data(), b.size());
                          },
                          tray_id, std::move(copy)));
}

bool Backend_GetTrayIconBounds_Win(void* /*data*/, uint32_t tray_id, int* x,
                                   int* y, int* width, int* height) {
  // Shell_NotifyIconGetRect touches the shell/message window owned by the UI
  // thread, so query synchronously there.
  bool ok = false;
  cef_invoke_sync([&] {
    ok = laufey_common::GetTrayIconBoundsWin(tray_id, x, y, width, height);
  });
  return ok;
}

void Backend_SetTrayDoubleClickHandler_Win(void* /*data*/, uint32_t tray_id,
                                           laufey_tray_click_fn handler,
                                           void* user_data) {
  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t tid, laufey_tray_click_fn h, void* d) {
                            laufey_common::SetTrayDoubleClickHandlerWin(tid, h, d);
                          },
                          tray_id, handler, user_data));
}

void Backend_SetTrayTooltip_Win(void* /*data*/, uint32_t tray_id,
                                const char* tooltip_or_null) {
  std::string tip = tooltip_or_null ? tooltip_or_null : std::string();
  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t tid, std::string t) {
                            laufey_common::SetTrayTooltipWin(
                                tid, t.empty() ? nullptr : t.c_str());
                          },
                          tray_id, std::move(tip)));
}

void Backend_SetTrayMenu_Win(void* data, uint32_t tray_id,
                             laufey_value_t* menu_template,
                             laufey_menu_click_fn on_click, void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const laufey_backend_api_t* api = &loader->GetBackendApi();
  CefPostTask(
      TID_UI,
      base::BindOnce(
          [](uint32_t tid, laufey_value_t* tmpl, const laufey_backend_api_t* a,
             laufey_menu_click_fn cb, void* cb_data) {
            laufey_common::SetTrayMenuWin(tid, tmpl, a, cb, cb_data);
          },
          tray_id, menu_template, api, on_click, on_click_data));
}

void Backend_SetTrayClickHandler_Win(void* /*data*/, uint32_t tray_id,
                                     laufey_tray_click_fn handler,
                                     void* user_data) {
  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t tid, laufey_tray_click_fn h, void* d) {
                            laufey_common::SetTrayClickHandlerWin(tid, h, d);
                          },
                          tray_id, handler, user_data));
}

// --- Notifications (Windows) ---
//
// Thin trampolines over backend-common/src/notifications_win.cc.

static uint32_t Backend_ShowNotification_Win(void* data, laufey_value_t* options,
                                             laufey_notification_event_fn on_event,
                                             void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  laufey_common::NotificationOptions opts =
      laufey_common::ParseNotificationOptions(options, &loader->GetBackendApi());
  return laufey_common::ShowNotificationWin(opts, on_event, user_data);
}

static void Backend_CloseNotification_Win(void* /*data*/,
                                          uint32_t notification_id) {
  laufey_common::CloseNotificationWin(notification_id);
}

#elif defined(__linux__)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <gdk/gdk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

static void Backend_BounceDock_Linux(void* data, int /*type*/) {
  // X11 urgency hint is binary — there's no informational vs critical. Set
  // it on every LAUFEY window; WMs will surface this (taskbar flash, workspace
  // indicator, etc.).
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->ForEachBrowser([](CefRefPtr<CefBrowser> browser) {
    CefPostTask(TID_UI,
                base::BindOnce(
                    [](CefRefPtr<CefBrowser> b) {
#ifdef GDK_WINDOWING_X11
                      GdkDisplay* gdk_display = gdk_display_get_default();
                      if (!gdk_display || !GDK_IS_X11_DISPLAY(gdk_display))
                        return;
                      Display* display = GDK_DISPLAY_XDISPLAY(gdk_display);
                      ::Window win = (::Window)b->GetHost()->GetWindowHandle();
                      if (!win)
                        return;
                      XWMHints* hints = XGetWMHints(display, win);
                      if (!hints)
                        hints = XAllocWMHints();
                      if (hints) {
                        hints->flags |= XUrgencyHint;
                        XSetWMHints(display, win, hints);
                        XFree(hints);
                        XFlush(display);
                      }
#else
                      (void)b;
#endif
                    },
                    browser));
  });
}
#endif

static void Backend_OpenDevTools(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b) {
                              CefWindowInfo windowInfo;
#if defined(_WIN32)
                              windowInfo.SetAsPopup(nullptr, "DevTools");
#endif
                              b->GetHost()->ShowDevTools(windowInfo, nullptr,
                                                         CefBrowserSettings(),
                                                         CefPoint());
                            },
                            browser));
  }
}

static void Backend_SetJsNamespace(void* data, const char* name) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (name) {
    loader->SetJsNamespace(name);
  }
}

// --- InitializeBackendApi ---

static uint32_t Backend_CreateWindowImpl(void* data, uint32_t flags) {
  auto* loader = RuntimeLoader::GetInstance();
  uint32_t window_id = loader->AllocateWindowId();

  CefPostTask(TID_UI,
              base::BindOnce(
                  [](uint32_t wid, uint32_t window_flags) {
                    auto* handler = LaufeyHandler::GetInstance();
                    if (!handler)
                      return;

                    // Push laufey_id before creating the browser so
                    // OnAfterCreated can pop it. Both run on the UI
                    // thread so no race.
                    g_pending_laufey_ids.push(wid);

                    CefBrowserSettings browser_settings;
                    CefRefPtr<CefDictionaryValue> extra_info =
                        CefDictionaryValue::Create();
                    extra_info->SetString(
                        "laufey_js_namespace",
                        RuntimeLoader::GetInstance()->GetJsNamespace());
                    CefRefPtr<CefBrowserView> browser_view =
                        CefBrowserView::CreateBrowserView(
                            handler, "about:blank", browser_settings,
                            extra_info, nullptr, nullptr);
                    CefWindow::CreateTopLevelWindow(
                        new LaufeyWindowDelegate(browser_view, wid, window_flags));
                  },
                  window_id, flags));

  // Block until the browser is registered by OnAfterCreated, so that
  // subsequent calls (navigate, set_title, etc.) can find it.
  loader->WaitForBrowser(window_id);

  return window_id;
}

static uint32_t Backend_CreateWindow(void* data) {
  return Backend_CreateWindowImpl(data, 0);
}

static uint32_t Backend_CreateWindowEx(void* data, uint32_t flags) {
  return Backend_CreateWindowImpl(data, flags);
}

static void Backend_CloseWindow(void* data, uint32_t window_id) {
  auto* loader = RuntimeLoader::GetInstance();
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b) {
                              b->GetHost()->CloseBrowser(true);
                            },
                            browser));
  }
}

static void Backend_SetCloseRequestedHandler(void* data,
                                             laufey_close_requested_fn handler,
                                             void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetCloseRequestedHandler(handler, user_data);
}

static int Backend_ShowDialog(void* /*data*/, uint32_t /*window_id*/,
                              int dialog_type, const char* title,
                              const char* message, const char* default_value,
                              char** out_input_value) {
  std::string title_str = title ? title : "";
  std::string message_str = message ? message : "";
  std::string default_str = default_value ? default_value : "";
#ifdef __APPLE__
  return laufey_common::ShowDialogMac(dialog_type, title_str, message_str,
                                   default_str, out_input_value);
#elif defined(__linux__)
  return laufey_common::ShowDialogLinux(dialog_type, title_str, message_str,
                                     default_str, out_input_value);
#elif defined(_WIN32)
  return laufey_common::ShowDialogWin(dialog_type, title_str, message_str,
                                   default_str, out_input_value);
#else
  (void)out_input_value;
  return 0;
#endif
}

static void Backend_StringFree(void* /*data*/, char* s) {
  if (s)
    free(s);
}

void RuntimeLoader::InitializeBackendApi() {
  memset(&backend_api_, 0, sizeof(backend_api_));
  backend_api_.version = LAUFEY_API_VERSION;
  backend_api_.backend_data = this;

  backend_api_.create_window = Backend_CreateWindow;
  backend_api_.create_window_ex = Backend_CreateWindowEx;
  backend_api_.close_window = Backend_CloseWindow;

  backend_api_.navigate = Backend_Navigate;
  backend_api_.set_title = Backend_SetTitle;
  backend_api_.execute_js = Backend_ExecuteJs;
  backend_api_.quit = Backend_Quit;
  backend_api_.set_window_size = Backend_SetWindowSize;
  backend_api_.get_window_size = Backend_GetWindowSize;
  backend_api_.set_window_position = Backend_SetWindowPosition;
  backend_api_.get_window_position = Backend_GetWindowPosition;
  backend_api_.set_resizable = Backend_SetResizable;
  backend_api_.is_resizable = Backend_IsResizable;
  backend_api_.set_always_on_top = Backend_SetAlwaysOnTop;
  backend_api_.is_always_on_top = Backend_IsAlwaysOnTop;
  backend_api_.is_visible = Backend_IsVisible;
  backend_api_.show = Backend_Show;
  backend_api_.hide = Backend_Hide;
  backend_api_.focus = Backend_Focus;
  backend_api_.post_ui_task = Backend_PostUiTask;

  laufey_register_value_api(&backend_api_);

  backend_api_.set_js_call_handler = Backend_SetJsCallHandler;
  backend_api_.js_call_respond = Backend_JsCallRespond;

  backend_api_.invoke_js_callback = Backend_InvokeJsCallback;
  backend_api_.release_js_callback = Backend_ReleaseJsCallback;

  backend_api_.get_window_handle = [](void*, uint32_t) -> void* {
    return nullptr;
  };
  backend_api_.get_display_handle = [](void*, uint32_t) -> void* {
    return nullptr;
  };
#if defined(_WIN32)
  backend_api_.get_window_handle_type = [](void*, uint32_t) -> int {
    return LAUFEY_WINDOW_HANDLE_WIN32;
  };
#elif defined(__APPLE__)
  backend_api_.get_window_handle_type = [](void*, uint32_t) -> int {
    return LAUFEY_WINDOW_HANDLE_APPKIT;
  };
#else
  backend_api_.get_window_handle_type = [](void*, uint32_t) -> int {
    return LAUFEY_WINDOW_HANDLE_X11;
  };
#endif

  backend_api_.set_keyboard_event_handler = Backend_SetKeyboardEventHandler;
  backend_api_.set_mouse_click_handler = Backend_SetMouseClickHandler;
  backend_api_.set_mouse_move_handler = Backend_SetMouseMoveHandler;
  backend_api_.set_wheel_handler = Backend_SetWheelHandler;
  backend_api_.set_cursor_enter_leave_handler =
      Backend_SetCursorEnterLeaveHandler;
  backend_api_.set_focused_handler = Backend_SetFocusedHandler;
  backend_api_.set_resize_handler = Backend_SetResizeHandler;
  backend_api_.set_move_handler = Backend_SetMoveHandler;
  backend_api_.set_close_requested_handler = Backend_SetCloseRequestedHandler;

  backend_api_.poll_js_calls = [](void* data) {
    RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
    loader->PollPendingJsCalls();
  };

  backend_api_.set_js_call_notify = [](void* data, void (*notify_fn)(void*),
                                       void* notify_data) {
    RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
    loader->SetJsCallNotify(notify_fn, notify_data);
  };

#if defined(_WIN32)
  backend_api_.set_application_menu = Backend_SetApplicationMenu;
  backend_api_.show_context_menu = Backend_ShowContextMenu;
#elif defined(__APPLE__)
  backend_api_.set_application_menu = Backend_SetApplicationMenu_Mac;
  backend_api_.show_context_menu = Backend_ShowContextMenu_Mac;
#else
  // Linux: in-window menu bar (set_application_menu) requires packing a
  // GtkMenuBar above the browser, which means the top-level window must be
  // a GtkWindow we own. CEF Views creates the native window itself, so an
  // embedded menubar isn't reachable without reparenting CEF into a GTK
  // host — and that path breaks on XWayland (cross-client X11 child
  // windows aren't supported in Wayland-native ways). Context menus and
  // tray menus still work because GtkMenu popups don't need a GtkWindow.
  backend_api_.set_application_menu = [](void*, uint32_t, laufey_value_t*,
                                         laufey_menu_click_fn, void*) {};
  backend_api_.show_context_menu = Backend_ShowContextMenu_Linux;
#endif

  backend_api_.open_devtools = Backend_OpenDevTools;
  backend_api_.set_js_namespace = Backend_SetJsNamespace;
  backend_api_.show_dialog = Backend_ShowDialog;
  backend_api_.string_free = Backend_StringFree;

  // --- Dock / taskbar ---
#if defined(__APPLE__)
  backend_api_.set_dock_badge = Backend_SetDockBadge_Mac;
  backend_api_.bounce_dock = Backend_BounceDock_Mac;
  backend_api_.set_dock_menu = Backend_SetDockMenu_Mac;
  backend_api_.set_dock_visible = Backend_SetDockVisible_Mac;
  backend_api_.set_dock_reopen_handler = Backend_SetDockReopenHandler_Mac;
#elif defined(_WIN32)
  backend_api_.bounce_dock = Backend_BounceDock_Win;
  backend_api_.set_dock_badge = Backend_SetDockBadge_TitlePrefix;
  // Menu/visible/reopen have no Windows analog — left nullptr; the
  // runtime-side Rust wrapper silently no-ops when the pointer is missing.
#elif defined(__linux__)
  backend_api_.bounce_dock = Backend_BounceDock_Linux;
  backend_api_.set_dock_badge = Backend_SetDockBadge_TitlePrefix;
  // Menu/visible/reopen: left nullptr (no clean Linux analog).
#endif

  // --- Tray / status bar ---
#if defined(__APPLE__)
  backend_api_.create_tray_icon = Backend_CreateTrayIcon_Mac;
  backend_api_.destroy_tray_icon = Backend_DestroyTrayIcon_Mac;
  backend_api_.set_tray_icon = Backend_SetTrayIcon_Mac;
  backend_api_.set_tray_tooltip = Backend_SetTrayTooltip_Mac;
  backend_api_.set_tray_menu = Backend_SetTrayMenu_Mac;
  backend_api_.set_tray_click_handler = Backend_SetTrayClickHandler_Mac;
  backend_api_.set_tray_double_click_handler =
      Backend_SetTrayDoubleClickHandler_Mac;
  backend_api_.set_tray_icon_dark = Backend_SetTrayIconDark_Mac;
  backend_api_.get_tray_icon_bounds = Backend_GetTrayIconBounds_Mac;
#elif defined(_WIN32)
  backend_api_.create_tray_icon = Backend_CreateTrayIcon_Win;
  backend_api_.destroy_tray_icon = Backend_DestroyTrayIcon_Win;
  backend_api_.set_tray_icon = Backend_SetTrayIcon_Win;
  backend_api_.set_tray_tooltip = Backend_SetTrayTooltip_Win;
  backend_api_.set_tray_menu = Backend_SetTrayMenu_Win;
  backend_api_.set_tray_click_handler = Backend_SetTrayClickHandler_Win;
  backend_api_.set_tray_double_click_handler =
      Backend_SetTrayDoubleClickHandler_Win;
  backend_api_.set_tray_icon_dark = Backend_SetTrayIconDark_Win;
  backend_api_.get_tray_icon_bounds = Backend_GetTrayIconBounds_Win;
#elif defined(__linux__)
  backend_api_.create_tray_icon = Backend_CreateTrayIcon_Linux;
  backend_api_.destroy_tray_icon = Backend_DestroyTrayIcon_Linux;
  backend_api_.set_tray_icon = Backend_SetTrayIcon_Linux;
  backend_api_.set_tray_tooltip = Backend_SetTrayTooltip_Linux;
  backend_api_.set_tray_menu = Backend_SetTrayMenu_Linux;
  backend_api_.set_tray_click_handler = Backend_SetTrayClickHandler_Linux;
  backend_api_.set_tray_double_click_handler =
      Backend_SetTrayDoubleClickHandler_Linux;
  backend_api_.set_tray_icon_dark = Backend_SetTrayIconDark_Linux;
  // No get_tray_icon_bounds on Linux: the AppIndicator / StatusNotifier
  // protocol does not expose the icon's screen position, so it stays NULL
  // and Tray.getBounds() reports null.
#endif

  // --- Notifications ---
#if defined(__APPLE__)
  backend_api_.show_notification = Backend_ShowNotification_Mac;
  backend_api_.close_notification = Backend_CloseNotification_Mac;
#elif defined(_WIN32)
  backend_api_.show_notification = Backend_ShowNotification_Win;
  backend_api_.close_notification = Backend_CloseNotification_Win;
#elif defined(__linux__)
  backend_api_.show_notification = Backend_ShowNotification_Linux;
  backend_api_.close_notification = Backend_CloseNotification_Linux;
#endif

  // --- Permissions ---
#if defined(__APPLE__)
  backend_api_.query_permission = Backend_QueryPermission_Mac;
  backend_api_.request_permission = Backend_RequestPermission_Mac;
#else
  backend_api_.query_permission = Backend_QueryPermission_Stub;
  backend_api_.request_permission = Backend_RequestPermission_Stub;
#endif
}

// --- RuntimeLoader lifecycle ---

RuntimeLoader::RuntimeLoader() {
  instance_ = this;
  InitializeBackendApi();
}

RuntimeLoader::~RuntimeLoader() {
  Shutdown();
  if (library_handle_) {
#ifndef _WIN32
    dlclose(library_handle_);
#else
    FreeLibrary(static_cast<HMODULE>(library_handle_));
#endif
  }
  instance_ = nullptr;
}

RuntimeLoader* RuntimeLoader::GetInstance() {
  if (!instance_) {
    instance_ = new RuntimeLoader();
  }
  return instance_;
}

bool RuntimeLoader::Load(const std::string& path) {
#ifndef _WIN32
  library_handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library_handle_) {
    std::cerr << "Failed to load runtime: " << dlerror() << std::endl;
    return false;
  }

  init_fn_ = reinterpret_cast<laufey_runtime_init_fn>(
      dlsym(library_handle_, LAUFEY_RUNTIME_INIT_SYMBOL));
  if (!init_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_INIT_SYMBOL << ": "
              << dlerror() << std::endl;
    return false;
  }

  start_fn_ = reinterpret_cast<laufey_runtime_start_fn>(
      dlsym(library_handle_, LAUFEY_RUNTIME_START_SYMBOL));
  if (!start_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_START_SYMBOL << ": "
              << dlerror() << std::endl;
    return false;
  }

  shutdown_fn_ = reinterpret_cast<laufey_runtime_shutdown_fn>(
      dlsym(library_handle_, LAUFEY_RUNTIME_SHUTDOWN_SYMBOL));
  if (!shutdown_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_SHUTDOWN_SYMBOL << ": "
              << dlerror() << std::endl;
    return false;
  }
#else
  library_handle_ = LoadLibraryA(path.c_str());
  if (!library_handle_) {
    std::cerr << "Failed to load runtime: error " << GetLastError()
              << std::endl;
    return false;
  }

  init_fn_ = reinterpret_cast<laufey_runtime_init_fn>(GetProcAddress(
      static_cast<HMODULE>(library_handle_), LAUFEY_RUNTIME_INIT_SYMBOL));
  if (!init_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_INIT_SYMBOL << std::endl;
    return false;
  }

  start_fn_ = reinterpret_cast<laufey_runtime_start_fn>(GetProcAddress(
      static_cast<HMODULE>(library_handle_), LAUFEY_RUNTIME_START_SYMBOL));
  if (!start_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_START_SYMBOL << std::endl;
    return false;
  }

  shutdown_fn_ = reinterpret_cast<laufey_runtime_shutdown_fn>(GetProcAddress(
      static_cast<HMODULE>(library_handle_), LAUFEY_RUNTIME_SHUTDOWN_SYMBOL));
  if (!shutdown_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_SHUTDOWN_SYMBOL << std::endl;
    return false;
  }
#endif

  std::cout << "Runtime loaded successfully from: " << path << std::endl;
  return true;
}

bool RuntimeLoader::Start() {
  if (running_) {
    return true;
  }

  if (!init_fn_ || !start_fn_) {
    std::cerr << "Runtime not loaded" << std::endl;
    return false;
  }

  int result = init_fn_(&backend_api_);
  if (result != 0) {
    std::cerr << "Runtime init failed with code: " << result << std::endl;
    return false;
  }

  running_ = true;
  runtime_thread_ = std::thread(&RuntimeLoader::RuntimeThread, this);

  std::cout << "Runtime started" << std::endl;
  return true;
}

void RuntimeLoader::RuntimeThread() {
  int result = start_fn_();
  if (result != 0) {
    std::cerr << "Runtime start returned error: " << result << std::endl;
  }
  running_ = false;
}

void RuntimeLoader::Shutdown() {
  if (shutdown_fn_) {
    shutdown_fn_();
  }

  if (runtime_thread_.joinable()) {
    runtime_thread_.join();
  }
}

void RuntimeLoader::HandleEvalResult(uint64_t eval_id,
                                     CefRefPtr<CefValue> result,
                                     const std::string& error) {
  PendingEval eval;
  {
    std::lock_guard<std::mutex> lock(eval_mutex_);
    auto it = pending_evals_.find(eval_id);
    if (it == pending_evals_.end())
      return;
    eval = it->second;
    pending_evals_.erase(it);
  }

  if (!error.empty()) {
    laufey_value errLaufey(laufey::Value::String(error));
    eval.callback(nullptr, &errLaufey, eval.callback_data);
  } else if (result && result->GetType() != VTYPE_NULL) {
    laufey_value resultLaufey(CefValueToLaufey(result));
    eval.callback(&resultLaufey, nullptr, eval.callback_data);
  } else {
    eval.callback(nullptr, nullptr, eval.callback_data);
  }
}

void RuntimeLoader::OnJsCall(uint32_t window_id, uint64_t call_id,
                             const std::string& method_path,
                             CefRefPtr<CefListValue> args) {
  // The CefListValue passed in is owned by the CefProcessMessage and
  // becomes invalid once OnProcessMessageReceived returns. Copy it so the
  // queued entry survives until PollPendingJsCalls runs.
  CefRefPtr<CefListValue> owned_args =
      args ? args->Copy() : CefListValue::Create();
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_js_calls_.push({window_id, call_id, method_path, owned_args});
  }
  StoreCallWindow(call_id, window_id);

  std::lock_guard<std::mutex> lock(notify_mutex_);
  if (js_call_notify_fn_) {
    js_call_notify_fn_(js_call_notify_data_);
  }
}

void RuntimeLoader::PollPendingJsCalls() {
  std::vector<PendingJsCall> calls;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    while (!pending_js_calls_.empty()) {
      calls.push_back(std::move(pending_js_calls_.front()));
      pending_js_calls_.pop();
    }
  }

  if (calls.empty())
    return;

  laufey_js_call_fn handler;
  void* user_data;
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handler = js_call_handler_;
    user_data = js_call_user_data_;
  }

  for (auto& call : calls) {
    if (handler) {
      CefRefPtr<CefValue> argsValue = CefValue::Create();
      argsValue->SetList(call.args);
      laufey_value_t* argsWrapper = new laufey_value(CefValueToLaufey(argsValue));
      handler(user_data, call.window_id, call.call_id, call.method_path.c_str(),
              argsWrapper);
    } else {
      laufey_value_t errWrapper(
          laufey::Value::String("No JS call handler registered"));
      Backend_JsCallRespond(this, call.call_id, nullptr, &errWrapper);
    }
  }
}
