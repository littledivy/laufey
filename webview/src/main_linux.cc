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

  // Application identity for the window manager. The embedder (e.g. deno
  // desktop) passes LAUFEY_APP_ID (the reverse-DNS identifier it also uses for
  // the `.desktop` file) and LAUFEY_APP_NAME (the display name). GTK derives a
  // Wayland xdg_toplevel app_id from g_get_prgname() and an X11 WM_CLASS from
  // the program class, so set both to the app id — otherwise they default to
  // this backend binary's name and the compositor shows a generic placeholder
  // icon instead of the one from the matching `<app_id>.desktop`.
  std::string appId;
  if (const char* env = getenv("LAUFEY_APP_ID")) {
    if (*env) {
      appId = env;
    }
  }
  if (appId.empty()) {
    if (const char* env = getenv("LAUFEY_APP_NAME")) {
      if (*env) {
        appId = env;
      }
    }
  }
  if (!appId.empty()) {
    g_set_prgname(appId.c_str());
    gdk_set_program_class(appId.c_str());
  }

  // Default icon for all windows. Wayland ignores client-set window icons (it
  // relies on the app_id → `.desktop` match above), but X11 honors this, so a
  // dev run under X11 shows the configured icon directly.
  if (const char* iconPath = getenv("LAUFEY_APP_ICON")) {
    if (*iconPath) {
      GError* error = nullptr;
      if (!gtk_window_set_default_icon_from_file(iconPath, &error)) {
        if (error) {
          std::cerr << "Failed to load app icon '" << iconPath
                    << "': " << error->message << std::endl;
          g_error_free(error);
        }
      }
    }
  }

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
    runtimePath = LaufeyFindColocatedRuntime();
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
