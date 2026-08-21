// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "renderer_app.h"

#include "scheme_handler.h"  // RegisterLaufeyCustomSchemes

LaufeyRendererApp::LaufeyRendererApp()
    : render_handler_(new LaufeyRenderProcessHandler()) {}

void LaufeyRendererApp::OnRegisterCustomSchemes(
    CefRawPtr<CefSchemeRegistrar> registrar) {
  RegisterLaufeyCustomSchemes(registrar);
}
