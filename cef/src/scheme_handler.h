// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef LAUFEY_SCHEME_HANDLER_H_
#define LAUFEY_SCHEME_HANDLER_H_

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "include/cef_resource_handler.h"
#include "include/cef_scheme.h"

// The custom standard scheme laufey registers for in-process app serving
// (e.g. "app://..."). Declared as a standard, secure, fetch/CORS-enabled
// scheme so pages served over it behave like normal https origins.
#define LAUFEY_APP_SCHEME "app"

// Registers LAUFEY_APP_SCHEME with the given registrar. A custom standard
// scheme must be declared identically in EVERY process — the browser
// (LaufeyApp) and all sub-processes including the network service
// (LaufeyRendererApp, handed to CefExecuteProcess) — otherwise the network
// service does not know the scheme is standard and rejects navigations to it
// (VALIDATION_ERROR_DESERIALIZATION_FAILED on network.mojom.NetworkContext),
// leaving the page blank. Both OnRegisterCustomSchemes overrides call this
// single definition so the flags cannot drift apart.
inline void RegisterLaufeyCustomSchemes(
    CefRawPtr<CefSchemeRegistrar> registrar) {
  registrar->AddCustomScheme(
      LAUFEY_APP_SCHEME, CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE |
                             CEF_SCHEME_OPTION_CORS_ENABLED |
                             CEF_SCHEME_OPTION_FETCH_ENABLED);
}

// A CefResourceHandler that bridges a single webview request to the laufey
// runtime's registered scheme handler. The handler object IS the opaque
// `laufey_scheme_exchange_t` passed across the C ABI: the runtime pulls the
// request body and streams the response back through the scheme_* vtable
// functions, which call the methods below.
//
// Threading: Open/GetResponseHeaders/Read/Cancel run on a CEF IO thread; the
// Begin/WriteResponse/FinishResponse/ReadRequestBody methods are called by the
// runtime from its own thread. All shared state is guarded by `mutex_`, and
// CEF's callbacks are resumed via Continue() (safe from any thread).
class LaufeySchemeHandler : public CefResourceHandler {
 public:
  explicit LaufeySchemeHandler(uint32_t window_id) : window_id_(window_id) {}

  // CefResourceHandler:
  bool Open(CefRefPtr<CefRequest> request, bool& handle_request,
            CefRefPtr<CefCallback> callback) override;
  void GetResponseHeaders(CefRefPtr<CefResponse> response,
                          int64_t& response_length,
                          CefString& redirectUrl) override;
  bool Read(void* data_out, int bytes_to_read, int& bytes_read,
            CefRefPtr<CefResourceReadCallback> callback) override;
  void Cancel() override;

  // Called by the runtime (via the scheme_* vtable functions), off the IO
  // thread. Return values mirror the C ABI contract.
  intptr_t ReadRequestBody(uint8_t* buf, size_t cap);
  void Begin(int status, const char* headers, size_t headers_len);
  intptr_t WriteResponse(const uint8_t* buf, size_t len);
  void FinishResponse();

 private:
  uint32_t window_id_;

  // Request state (immutable after Open).
  std::string method_;
  std::string url_;
  std::vector<uint8_t> request_body_;
  size_t request_body_cursor_ = 0;

  // Response state, guarded by mutex_.
  std::mutex mutex_;
  int status_ = 200;
  std::vector<std::pair<std::string, std::string>> response_headers_;
  std::deque<uint8_t> response_body_;
  bool began_ = false;
  bool finished_ = false;
  bool cancelled_ = false;

  // Deferred CEF continuations. When a Read arrives with no body buffered, the
  // caller's output buffer (`pending_data_` / `pending_cap_`, valid until the
  // callback fires) and `read_callback_` are stashed and satisfied once
  // WriteResponse/FinishResponse provides data.
  CefRefPtr<CefCallback> open_callback_;
  CefRefPtr<CefResourceReadCallback> read_callback_;
  void* pending_data_ = nullptr;
  int pending_cap_ = 0;

  IMPLEMENT_REFCOUNTING(LaufeySchemeHandler);
};

// Factory installed via CefRegisterSchemeHandlerFactory. Stateless: each
// Create() builds a LaufeySchemeHandler and dispatches the request to the
// runtime's registered handler through RuntimeLoader.
class LaufeySchemeHandlerFactory : public CefSchemeHandlerFactory {
 public:
  CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefFrame> frame,
                                       const CefString& scheme_name,
                                       CefRefPtr<CefRequest> request) override;

 private:
  IMPLEMENT_REFCOUNTING(LaufeySchemeHandlerFactory);
};

#endif  // LAUFEY_SCHEME_HANDLER_H_
