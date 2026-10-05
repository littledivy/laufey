// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef LAUFEY_RENDERER_APP_H_
#define LAUFEY_RENDERER_APP_H_

#include "include/cef_app.h"
#include "render_process_handler.h"

class LaufeyRendererApp : public CefApp {
 public:
  LaufeyRendererApp();

  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
    return render_handler_;
  }

  // Custom schemes must be registered in every process: mirror the browser
  // process's set ("app" + --laufey-custom-schemes) in the renderer so
  // `location.origin`, secure-context and storage checks agree.
  void OnRegisterCustomSchemes(
      CefRawPtr<CefSchemeRegistrar> registrar) override;

 private:
  CefRefPtr<LaufeyRenderProcessHandler> render_handler_;

  IMPLEMENT_REFCOUNTING(LaufeyRendererApp);
};

#endif
