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

  // This CefApp is the one passed to CefExecuteProcess for all sub-processes
  // (renderer, GPU, network service), which must declare custom standard
  // schemes just like the browser process; see RegisterLaufeyCustomSchemes in
  // scheme_handler.h.
  void OnRegisterCustomSchemes(
      CefRawPtr<CefSchemeRegistrar> registrar) override;

 private:
  CefRefPtr<LaufeyRenderProcessHandler> render_handler_;

  IMPLEMENT_REFCOUNTING(LaufeyRendererApp);
};

#endif
