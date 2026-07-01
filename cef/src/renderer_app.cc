// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "renderer_app.h"

#include "scheme_handler.h"  // LAUFEY_APP_SCHEME

LaufeyRendererApp::LaufeyRendererApp()
    : render_handler_(new LaufeyRenderProcessHandler()) {}

void LaufeyRendererApp::OnRegisterCustomSchemes(
    CefRawPtr<CefSchemeRegistrar> registrar) {
  registrar->AddCustomScheme(
      LAUFEY_APP_SCHEME, CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE |
                             CEF_SCHEME_OPTION_CORS_ENABLED |
                             CEF_SCHEME_OPTION_FETCH_ENABLED);
}
