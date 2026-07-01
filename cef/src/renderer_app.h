// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef LAUFEY_RENDERER_APP_H_
#define LAUFEY_RENDERER_APP_H_

#include "include/cef_app.h"
#include "include/cef_scheme.h"
#include "render_process_handler.h"

class LaufeyRendererApp : public CefApp {
 public:
  LaufeyRendererApp();

  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
    return render_handler_;
  }

  // Custom schemes must be registered in EVERY process (browser, renderer,
  // and the network service), not just the browser. This CefApp is the one
  // passed to CefExecuteProcess for all sub-processes, so it must mirror
  // LaufeyApp::OnRegisterCustomSchemes — otherwise the network service does
  // not know the scheme is standard and rejects navigations to it
  // (VALIDATION_ERROR_DESERIALIZATION_FAILED on network.mojom.NetworkContext),
  // leaving the page blank.
  void OnRegisterCustomSchemes(
      CefRawPtr<CefSchemeRegistrar> registrar) override;

 private:
  CefRefPtr<LaufeyRenderProcessHandler> render_handler_;

  IMPLEMENT_REFCOUNTING(LaufeyRendererApp);
};

#endif
