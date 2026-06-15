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
    char envPath[MAX_PATH];
    if (GetEnvironmentVariableA("LAUFEY_RUNTIME_PATH", envPath, MAX_PATH) > 0) {
      runtimePath = envPath;
    }
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
    MessageBoxA(nullptr,
                ("Failed to load runtime from: " + runtimePath).c_str(),
                "LAUFEY Webview Error", MB_OK | MB_ICONERROR);
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
