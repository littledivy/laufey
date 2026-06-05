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

 private:
  CefRefPtr<LaufeyRenderProcessHandler> render_handler_;

  IMPLEMENT_REFCOUNTING(LaufeyRendererApp);
};

#endif
