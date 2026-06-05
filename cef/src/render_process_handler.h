// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef LAUFEY_RENDER_PROCESS_HANDLER_H_
#define LAUFEY_RENDER_PROCESS_HANDLER_H_

#include <map>
#include <vector>
#include <string>
#include <atomic>

#include "include/cef_render_process_handler.h"
#include "include/cef_v8.h"

struct StoredCallback {
  CefRefPtr<CefV8Value> func;
  CefRefPtr<CefV8Context> context;
};

class LaufeyPathObject : public CefV8Interceptor, public CefV8Handler {
 public:
  explicit LaufeyPathObject(std::vector<std::string> path,
                         CefRefPtr<CefFrame> frame);

  bool Get(const CefString& name, const CefRefPtr<CefV8Value> object,
           CefRefPtr<CefV8Value>& retval, CefString& exception) override;

  bool Set(const CefString& name, const CefRefPtr<CefV8Value> object,
           const CefRefPtr<CefV8Value> value, CefString& exception) override;

  bool Get(int index, const CefRefPtr<CefV8Value> object,
           CefRefPtr<CefV8Value>& retval, CefString& exception) override;

  bool Set(int index, const CefRefPtr<CefV8Value> object,
           const CefRefPtr<CefV8Value> value, CefString& exception) override;

  bool Execute(const CefString& name, CefRefPtr<CefV8Value> object,
               const CefV8ValueList& arguments, CefRefPtr<CefV8Value>& retval,
               CefString& exception) override;

 private:
  std::vector<std::string> path_;
  CefRefPtr<CefFrame> frame_;

  IMPLEMENT_REFCOUNTING(LaufeyPathObject);
};

class PromiseResolver : public CefBaseRefCounted {
 public:
  explicit PromiseResolver(uint64_t call_id, CefRefPtr<CefV8Value> promise,
                           CefRefPtr<CefV8Context> context);

  void Resolve(CefRefPtr<CefV8Value> value);
  void Reject(const std::string& error);

 private:
  CefRefPtr<CefV8Value> promise_;
  CefRefPtr<CefV8Context> context_;

  IMPLEMENT_REFCOUNTING(PromiseResolver);
};

class LaufeyRenderProcessHandler : public CefRenderProcessHandler {
 public:
  LaufeyRenderProcessHandler();

  void OnBrowserCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefDictionaryValue> extra_info) override;

  void OnContextCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override;

  void OnContextReleased(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override;

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;

  void StorePendingCall(uint64_t call_id, CefRefPtr<PromiseResolver> resolver);

  uint64_t StoreCallback(CefRefPtr<CefV8Value> func,
                         CefRefPtr<CefV8Context> context);

  uint64_t GetNextCallId();

  CefRefPtr<CefValue> V8ValueToCefValue(CefRefPtr<CefV8Value> v8val);

  CefRefPtr<CefV8Value> CefValueToV8Value(CefRefPtr<CefValue> value,
                                          CefRefPtr<CefV8Context> context);

 private:
  std::map<int, std::string> browser_namespaces_;
  std::atomic<uint64_t> next_call_id_{1};
  std::atomic<uint64_t> next_callback_id_{1};
  std::map<uint64_t, CefRefPtr<PromiseResolver>> pending_calls_;
  std::map<uint64_t, StoredCallback> stored_callbacks_;

  IMPLEMENT_REFCOUNTING(LaufeyRenderProcessHandler);
};

extern CefRefPtr<LaufeyRenderProcessHandler> g_render_handler;

#endif
