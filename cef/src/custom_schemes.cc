// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "custom_schemes.h"

#include <cstdlib>
#include <iostream>

#include "laufey_scheme_registry.h"

namespace laufey_schemes {

const char kSwitch[] = "laufey-custom-schemes";
const char kEnv[] = "LAUFEY_CUSTOM_SCHEMES";

namespace {

std::vector<std::string> ComputeDeclared() {
  std::vector<std::string> lists;
  CefRefPtr<CefCommandLine> command_line =
      CefCommandLine::GetGlobalCommandLine();
  if (command_line && command_line->HasSwitch(kSwitch)) {
    lists.push_back(command_line->GetSwitchValue(kSwitch).ToString());
  }
  if (const char* env = std::getenv(kEnv)) {
    lists.push_back(env);
  }
  std::vector<std::string> rejected;
  std::vector<std::string> declared =
      laufey_common::MergeSchemeLists(lists, &rejected);
  for (const std::string& bad : rejected) {
    std::cerr << "laufey: ignoring invalid URL scheme name \"" << bad
              << "\" in --" << kSwitch << " / " << kEnv << std::endl;
  }
  return declared;
}

}  // namespace

std::vector<std::string> Declared() {
  // Computed once per process: the command line and environment are fixed by
  // the time OnRegisterCustomSchemes runs, and the invalid-name warnings
  // should not repeat on every call.
  static const std::vector<std::string> declared = ComputeDeclared();
  return declared;
}

bool IsDeclared(const std::string& scheme) {
  std::string normalized = laufey_common::NormalizeSchemeName(scheme);
  for (const std::string& declared : Declared()) {
    if (declared == normalized) {
      return true;
    }
  }
  return false;
}

void RegisterAll(CefRawPtr<CefSchemeRegistrar> registrar) {
  for (const std::string& scheme : Declared()) {
    registrar->AddCustomScheme(scheme, CEF_SCHEME_OPTION_STANDARD |
                                           CEF_SCHEME_OPTION_SECURE |
                                           CEF_SCHEME_OPTION_CORS_ENABLED |
                                           CEF_SCHEME_OPTION_FETCH_ENABLED);
  }
}

void ForwardToChild(CefRefPtr<CefCommandLine> command_line) {
  if (!command_line) {
    return;
  }
  // The child's command line may already carry the switch (CEF copies the
  // browser's own switches to it); merge rather than skip, so schemes this
  // process declared from the environment reach it too.
  std::string existing;
  if (command_line->HasSwitch(kSwitch)) {
    existing = command_line->GetSwitchValue(kSwitch).ToString();
  }
  std::string merged =
      laufey_common::MergeForwardedSchemes(existing, Declared());
  if (merged == existing) {
    return;
  }
  if (command_line->HasSwitch(kSwitch)) {
    command_line->RemoveSwitch(kSwitch);
  }
  if (!merged.empty()) {
    command_line->AppendSwitchWithValue(kSwitch, merged);
  }
}

}  // namespace laufey_schemes
