// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// iOS app entry for laufey. Unlike the desktop backends (which are an
// executable that dlopen()s the runtime dylib), iOS forbids loading arbitrary
// dylibs, so the runtime is *statically linked* into the app binary and its
// entry points are called directly. UIApplicationMain owns the run loop; the
// runtime runs on RuntimeLoader's worker thread and creates its UIWindow via
// the backend (hopping to the main thread).

#import <UIKit/UIKit.h>

#include "runtime_loader.h"

// Provided by the statically-linked laufey runtime (capi `main!` macro).
extern "C" int laufey_runtime_init(const laufey_backend_api_t* api);
extern "C" int laufey_runtime_start(void);
extern "C" void laufey_runtime_shutdown(void);

@interface LaufeyAppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation LaufeyAppDelegate
- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
  LaufeyBackend* backend = CreateLaufeyBackend();

  RuntimeLoader* loader = RuntimeLoader::GetInstance();
  loader->SetBackend(backend);
  loader->LoadStatic(laufey_runtime_init, laufey_runtime_start,
                     laufey_runtime_shutdown);
  // Spawns the runtime thread; the runtime calls create_window, which the
  // backend builds (and shows) on the main thread.
  loader->Start();
  return YES;
}

- (void)applicationWillTerminate:(UIApplication*)application {
  RuntimeLoader::GetInstance()->Shutdown();
}
@end

// Entry point. Exposed as a plain function so it can be driven either by the
// weak `main` below (C/C++-linked app, e.g. examples/ios_hello) or by a Rust
// `main` (the cargo-linked fused build that also embeds deno_core).
extern "C" int laufey_ios_main(void) {
  @autoreleasepool {
    return UIApplicationMain(0, NULL, nil,
                             NSStringFromClass([LaufeyAppDelegate class]));
  }
}

__attribute__((weak)) int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;
  return laufey_ios_main();
}
