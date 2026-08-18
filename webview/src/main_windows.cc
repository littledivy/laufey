// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "laufey_backend_common.h"
#include "runtime_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  std::string runtimePath;

  int argc;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv) {
    for (int i = 1; i < argc; ++i) {
      if (wcscmp(argv[i], L"--runtime") == 0 && i + 1 < argc) {
        runtimePath = laufey_common::WideToUtf8(argv[++i]);
      }
    }
    LocalFree(argv);
  }

  if (runtimePath.empty()) {
    // Read as UTF-16 and convert to UTF-8; the ANSI variant would garble
    // non-ASCII paths in the active codepage.
    std::wstring envPath(MAX_PATH, L'\0');
    DWORD envLen = GetEnvironmentVariableW(L"LAUFEY_RUNTIME_PATH", &envPath[0],
                                           static_cast<DWORD>(envPath.size()));
    if (envLen >= envPath.size()) {
      // Buffer too small; envLen is the required size including the NUL.
      envPath.resize(envLen);
      envLen = GetEnvironmentVariableW(L"LAUFEY_RUNTIME_PATH", &envPath[0],
                                       static_cast<DWORD>(envPath.size()));
    }
    if (envLen > 0 && envLen < envPath.size()) {
      envPath.resize(envLen);
      runtimePath = laufey_common::WideToUtf8(envPath);
    }
  }

  if (runtimePath.empty()) {
    runtimePath = LaufeyFindColocatedRuntime();
  }

  if (runtimePath.empty()) {
    const wchar_t* searchPaths[] = {L".\\runtime.dll",
                                    L".\\target\\debug\\hello.dll",
                                    L".\\target\\release\\hello.dll"};
    for (const wchar_t* path : searchPaths) {
      if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        runtimePath = laufey_common::WideToUtf8(path);
        break;
      }
    }
  }

  if (runtimePath.empty()) {
    MessageBoxW(nullptr,
                L"No runtime library found.\nSet LAUFEY_RUNTIME_PATH or use "
                L"--runtime <path>",
                L"LAUFEY Webview Error", MB_OK | MB_ICONERROR);
    CoUninitialize();
    return 1;
  }

  LaufeyBackend* backend = CreateLaufeyBackend();

  RuntimeLoader* loader = RuntimeLoader::GetInstance();
  loader->SetBackend(backend);

  if (!loader->Load(runtimePath)) {
    // The path is UTF-8; show it through the wide API so non-ASCII
    // characters render correctly in the dialog.
    MessageBoxW(nullptr,
                (L"Failed to load runtime from: " +
                 laufey_common::Utf8ToWide(runtimePath))
                    .c_str(),
                L"LAUFEY Webview Error", MB_OK | MB_ICONERROR);
    delete backend;
    CoUninitialize();
    return 1;
  }

  if (!loader->Start()) {
    MessageBoxW(nullptr, L"Failed to start runtime", L"LAUFEY Webview Error",
                MB_OK | MB_ICONERROR);
    delete backend;
    CoUninitialize();
    return 1;
  }

  backend->Run();

  loader->Shutdown();
  delete backend;

  CoUninitialize();
  return 0;
}
