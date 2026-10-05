// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "renderer_app.h"

#include "custom_schemes.h"

LaufeyRendererApp::LaufeyRendererApp()
    : render_handler_(new LaufeyRenderProcessHandler()) {}

void LaufeyRendererApp::OnRegisterCustomSchemes(
    CefRawPtr<CefSchemeRegistrar> registrar) {
  laufey_schemes::RegisterAll(registrar);
}
