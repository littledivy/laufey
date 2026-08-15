// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

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
        ++i;
        int size = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0,
                                       nullptr, nullptr);
        runtimePath.resize(size - 1);
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, &runtimePath[0], size,
                            nullptr, nullptr);
      }
    }
    LocalFree(argv);
  }

  if (runtimePath.empty()) {
    // Read as UTF-16 and convert to UTF-8; the ANSI variant would garble
    // non-ASCII paths in the active codepage.
    wchar_t envPath[MAX_PATH];
    DWORD envLen =
        GetEnvironmentVariableW(L"LAUFEY_RUNTIME_PATH", envPath, MAX_PATH);
    if (envLen > 0 && envLen < MAX_PATH) {
      int size = WideCharToMultiByte(CP_UTF8, 0, envPath, -1, nullptr, 0,
                                     nullptr, nullptr);
      runtimePath.resize(size - 1);
      WideCharToMultiByte(CP_UTF8, 0, envPath, -1, &runtimePath[0], size,
                          nullptr, nullptr);
    }
  }

  if (runtimePath.empty()) {
    runtimePath = LaufeyFindColocatedRuntime();
  }

  if (runtimePath.empty()) {
    const char* searchPaths[] = {".\\runtime.dll",
                                 ".\\target\\debug\\hello.dll",
                                 ".\\target\\release\\hello.dll"};
    for (const char* path : searchPaths) {
      if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        runtimePath = path;
        break;
      }
    }
  }

  if (runtimePath.empty()) {
    MessageBoxA(nullptr,
                "No runtime library found.\nSet LAUFEY_RUNTIME_PATH or use "
                "--runtime <path>",
                "LAUFEY Webview Error", MB_OK | MB_ICONERROR);
    CoUninitialize();
    return 1;
  }

  LaufeyBackend* backend = CreateLaufeyBackend();

  RuntimeLoader* loader = RuntimeLoader::GetInstance();
  loader->SetBackend(backend);

  if (!loader->Load(runtimePath)) {
    // The path is UTF-8; show it through the wide API so non-ASCII
    // characters render correctly in the dialog.
    int wlen =
        MultiByteToWideChar(CP_UTF8, 0, runtimePath.c_str(), -1, nullptr, 0);
    std::wstring wpath(wlen > 0 ? wlen - 1 : 0, L'\0');
    if (wlen > 0) {
      MultiByteToWideChar(CP_UTF8, 0, runtimePath.c_str(), -1, &wpath[0], wlen);
    }
    MessageBoxW(nullptr, (L"Failed to load runtime from: " + wpath).c_str(),
                L"LAUFEY Webview Error", MB_OK | MB_ICONERROR);
    delete backend;
    CoUninitialize();
    return 1;
  }

  if (!loader->Start()) {
    MessageBoxA(nullptr, "Failed to start runtime", "LAUFEY Webview Error",
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
