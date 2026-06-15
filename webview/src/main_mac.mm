// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#import <Cocoa/Cocoa.h>

#include "runtime_loader.h"
#include "laufey_backend_common.h"

#include <iostream>
#include <string>

// Cmd+C/V/X/A on macOS dispatch through the main menu's performKeyEquivalent:
// — Cocoa matches the keystroke against menu items, then sends their action
// (cut:/copy:/paste:/selectAll:) up the responder chain to the WKWebView,
// which forwards it to the page. Without these items in the main menu, the
// standard editing shortcuts have nowhere to land.
NSMenu* BuildDefaultEditSubmenu() {
  NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Edit"];
  [edit addItem:[[NSMenuItem alloc] initWithTitle:@"Undo"
                                           action:@selector(undo:)
                                    keyEquivalent:@"z"]];
  NSMenuItem* redo = [[NSMenuItem alloc] initWithTitle:@"Redo"
                                                action:@selector(redo:)
                                         keyEquivalent:@"Z"];
  [redo setKeyEquivalentModifierMask:(NSEventModifierFlagCommand |
                                      NSEventModifierFlagShift)];
  [edit addItem:redo];
  [edit addItem:[NSMenuItem separatorItem]];
  [edit addItem:[[NSMenuItem alloc] initWithTitle:@"Cut"
                                           action:@selector(cut:)
                                    keyEquivalent:@"x"]];
  [edit addItem:[[NSMenuItem alloc] initWithTitle:@"Copy"
                                           action:@selector(copy:)
                                    keyEquivalent:@"c"]];
  [edit addItem:[[NSMenuItem alloc] initWithTitle:@"Paste"
                                           action:@selector(paste:)
                                    keyEquivalent:@"v"]];
  [edit addItem:[NSMenuItem separatorItem]];
  [edit addItem:[[NSMenuItem alloc] initWithTitle:@"Select All"
                                           action:@selector(selectAll:)
                                    keyEquivalent:@"a"]];
  return edit;
}

static bool MenuTreeHasCopyAction(NSMenu* menu) {
  for (NSMenuItem* item in [menu itemArray]) {
    if ([item action] == @selector(copy:))
      return true;
    if ([item submenu] && MenuTreeHasCopyAction([item submenu]))
      return true;
  }
  return false;
}

// Force an Edit submenu into the menubar if the embedder didn't include one.
void EnsureEditMenu(NSMenu* menubar) {
  if (MenuTreeHasCopyAction(menubar))
    return;
  NSMenuItem* editItem = [[NSMenuItem alloc] init];
  [editItem setSubmenu:BuildDefaultEditSubmenu()];
  [menubar addItem:editItem];
}

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, assign) LaufeyBackend* backend;
@property(nonatomic, copy) NSString* runtimePath;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
  self.backend = CreateLaufeyBackend();

  RuntimeLoader* loader = RuntimeLoader::GetInstance();
  loader->SetBackend(self.backend);

  std::string runtimePath;
  if (self.runtimePath) {
    runtimePath = [self.runtimePath UTF8String];
  } else {
    NSBundle* bundle = [NSBundle mainBundle];
    NSString* bundlePath = [bundle bundlePath];

    NSArray* searchPaths = @[
      [bundlePath stringByAppendingPathComponent:
                      @"Contents/Frameworks/libruntime.dylib"],
      [bundlePath
          stringByAppendingPathComponent:@"Contents/MacOS/libruntime.dylib"],
      @"./libruntime.dylib", @"./target/debug/libhello.dylib",
      @"./target/release/libhello.dylib"
    ];

    for (NSString* path in searchPaths) {
      if ([[NSFileManager defaultManager] fileExistsAtPath:path]) {
        runtimePath = [path UTF8String];
        break;
      }
    }

    const char* envPath = getenv("LAUFEY_RUNTIME_PATH");
    if (envPath) {
      runtimePath = envPath;
    }
  }

  if (runtimePath.empty()) {
    std::cerr << "No runtime library found. Set LAUFEY_RUNTIME_PATH or place "
                 "libruntime.dylib in the app bundle."
              << std::endl;
    [NSApp terminate:nil];
    return;
  }

  if (!loader->Load(runtimePath)) {
    std::cerr << "Failed to load runtime from: " << runtimePath << std::endl;
    [NSApp terminate:nil];
    return;
  }

  if (!loader->Start()) {
    std::cerr << "Failed to start runtime" << std::endl;
    [NSApp terminate:nil];
    return;
  }
}

- (void)applicationWillTerminate:(NSNotification*)notification {
  RuntimeLoader::GetInstance()->Shutdown();
  delete self.backend;
  self.backend = nullptr;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  return YES;
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender
                    hasVisibleWindows:(BOOL)hasVisibleWindows {
  laufey_common::FireDockReopenMac(hasVisibleWindows ? true : false);
  // Always swallow the default "show last hidden window" behavior — the
  // embedder's callback decides what to do.
  return NO;
}

- (NSMenu*)applicationDockMenu:(NSApplication*)sender {
  return laufey_common::GetDockMenuMac();
}

@end

// Run the runtime headless (no window) for forked worker processes.
// Framework dev servers (e.g. Next.js Turbopack) fork child processes
// via child_process.fork(), which re-executes this binary. We detect
// these workers and run the Deno runtime without creating a window.
static int run_headless(const char* runtimePath) {
  RuntimeLoader* loader = RuntimeLoader::GetInstance();

  // Create a minimal backend with no visible window
  loader->SetBackend(nullptr);

  std::string path;
  if (runtimePath) {
    path = runtimePath;
  } else {
    const char* envPath = getenv("LAUFEY_RUNTIME_PATH");
    if (envPath) {
      path = envPath;
    }
  }

  if (path.empty()) {
    std::cerr << "No runtime library found for headless worker." << std::endl;
    return 1;
  }

  if (!loader->Load(path)) {
    std::cerr << "Failed to load runtime for headless worker." << std::endl;
    return 1;
  }

  if (!loader->Start()) {
    std::cerr << "Failed to start headless worker runtime." << std::endl;
    return 1;
  }

  // Wait for the runtime thread to finish
  loader->Shutdown();
  return 0;
}

static bool is_cli_worker_command(int argc, char* argv[]) {
  if (argc < 3 || strcmp(argv[1], "run") != 0) {
    return false;
  }

  for (int i = 2; i < argc; ++i) {
    if (argv[i][0] == '-') {
      continue;
    }
    return true;
  }

  return false;
}

static bool is_forked_worker() {
  return getenv("NODE_CHANNEL_FD") != nullptr ||
         getenv("NEXT_PRIVATE_WORKER") != nullptr;
}

int main(int argc, char* argv[]) {
  NSString* runtimePathArg = nil;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--runtime") == 0 && i + 1 < argc) {
      runtimePathArg = [NSString stringWithUTF8String:argv[++i]];
    }
  }

  // Forked worker processes should not create a window.
  if (is_forked_worker() || is_cli_worker_command(argc, argv)) {
    return run_headless(runtimePathArg ? [runtimePathArg UTF8String] : nullptr);
  }

  @autoreleasepool {
    // Allow the host to override the user-visible app name (menu-bar app
    // menu, Dock, Cmd-Tab) at launch, e.g. the project name during
    // `deno desktop --hmr`. AppKit derives the app menu title from the
    // process name for an exec'd bundle like ours, so set it before the
    // menu is built and before +sharedApplication.
    if (const char* app_name = getenv("LAUFEY_APP_NAME")) {
      if (*app_name) {
        [[NSProcessInfo processInfo]
            setProcessName:[NSString stringWithUTF8String:app_name]];
      }
    }

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    // Allow the host to override the dock icon at launch (e.g. a project's
    // favicon during `deno desktop --hmr`). Setting it programmatically
    // bypasses LaunchServices' icon cache and the bundle's CFBundleIconFile.
    if (const char* icon_path = getenv("LAUFEY_APP_ICON")) {
      NSImage* icon = [[NSImage alloc]
          initWithContentsOfFile:[NSString stringWithUTF8String:icon_path]];
      if (icon) {
        [NSApp setApplicationIconImage:icon];
      }
    }

    AppDelegate* delegate = [[AppDelegate alloc] init];
    delegate.runtimePath = runtimePathArg;

    [NSApp setDelegate:delegate];

    NSMenu* menubar = [[NSMenu alloc] init];
    NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
    [menubar addItem:appMenuItem];

    NSMenu* appMenu = [[NSMenu alloc] init];
    NSMenuItem* quitItem =
        [[NSMenuItem alloc] initWithTitle:@"Quit"
                                   action:@selector(terminate:)
                            keyEquivalent:@"q"];
    [appMenu addItem:quitItem];
    [appMenuItem setSubmenu:appMenu];

    EnsureEditMenu(menubar);
    [NSApp setMainMenu:menubar];

    [NSApp activateIgnoringOtherApps:YES];
    [NSApp run];
  }

  return 0;
}
