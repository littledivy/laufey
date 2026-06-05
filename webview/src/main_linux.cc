// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "runtime_loader.h"

#include <gtk/gtk.h>

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
  gtk_init(&argc, &argv);

  std::string runtimePath;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--runtime") == 0 && i + 1 < argc) {
      runtimePath = argv[++i];
    }
  }

  if (runtimePath.empty()) {
    const char* envPath = getenv("LAUFEY_RUNTIME_PATH");
    if (envPath) {
      runtimePath = envPath;
    }
  }

  if (runtimePath.empty()) {
    const char* searchPaths[] = {
        "./libruntime.so", "./target/debug/libhello.so",
        "./target/release/libhello.so", "/usr/lib/laufey/libruntime.so",
        "/usr/local/lib/laufey/libruntime.so"};
    for (const char* path : searchPaths) {
      if (access(path, F_OK) == 0) {
        runtimePath = path;
        break;
      }
    }
  }

  if (runtimePath.empty()) {
    std::cerr << "No runtime library found. Set LAUFEY_RUNTIME_PATH or use "
                 "--runtime <path>"
              << std::endl;
    return 1;
  }

  LaufeyBackend* backend = CreateLaufeyBackend();

  RuntimeLoader* loader = RuntimeLoader::GetInstance();
  loader->SetBackend(backend);

  if (!loader->Load(runtimePath)) {
    std::cerr << "Failed to load runtime from: " << runtimePath << std::endl;
    delete backend;
    return 1;
  }

  if (!loader->Start()) {
    std::cerr << "Failed to start runtime" << std::endl;
    delete backend;
    return 1;
  }

  backend->Run();

  loader->Shutdown();
  delete backend;

  return 0;
}
