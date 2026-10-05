// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Custom URL schemes for the CEF backend.
//
// CEF only treats a scheme as a real origin (standard, secure, CORS- and
// fetch-enabled) if it is added in CefApp::OnRegisterCustomSchemes — which
// runs during CefInitialize / CefExecuteProcess in EVERY process, long before
// the runtime library is loaded and can call register_scheme_handler. So the
// runtime's registrations cannot feed this hook; the embedder must instead
// declare the schemes when it launches the CEF host:
//
//   laufey --laufey-custom-schemes=myapp,other --runtime ...
//   LAUFEY_CUSTOM_SCHEMES=myapp,other laufey --runtime ...
//
// Both spellings are read in every process (the switch is forwarded to child
// processes in OnBeforeChildProcessLaunch; the environment is inherited), and
// "app" is always declared. Registering a scheme at runtime that was not
// declared still installs a handler factory for it, but Chromium then treats
// the scheme as non-standard (opaque origin, insecure context); the runtime
// loader logs a warning in that case.

#ifndef LAUFEY_CUSTOM_SCHEMES_H_
#define LAUFEY_CUSTOM_SCHEMES_H_

#include <string>
#include <vector>

#include "include/cef_command_line.h"
#include "include/cef_scheme.h"

namespace laufey_schemes {

// Command-line switch (without leading dashes) and environment variable that
// list the embedder's custom schemes, comma-separated.
extern const char kSwitch[];
extern const char kEnv[];

// The schemes declared for this process: "app" first, then the valid,
// normalized, deduplicated names from --laufey-custom-schemes and
// LAUFEY_CUSTOM_SCHEMES (in that order). Invalid entries are logged once and
// skipped. Safe to call in any process once the CEF command line exists
// (i.e. inside OnRegisterCustomSchemes and later).
std::vector<std::string> Declared();

// Whether `scheme` (any case) is one of Declared().
bool IsDeclared(const std::string& scheme);

// Add every Declared() scheme to `registrar` as standard + secure +
// CORS-enabled + fetch-enabled, so pages served over it behave like an https
// origin. Call from every CefApp::OnRegisterCustomSchemes override.
void RegisterAll(CefRawPtr<CefSchemeRegistrar> registrar);

// Forward this process's declared schemes to a child process's command line
// (CefBrowserProcessHandler::OnBeforeChildProcessLaunch) so renderers and
// utility processes register the same set.
void ForwardToChild(CefRefPtr<CefCommandLine> command_line);

}  // namespace laufey_schemes

#endif  // LAUFEY_CUSTOM_SCHEMES_H_
