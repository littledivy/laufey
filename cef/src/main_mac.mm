// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#import <Cocoa/Cocoa.h>

#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>

#include "include/cef_application_mac.h"
#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_library_loader.h"
#include "app.h"
#include "runtime_loader.h"
#include "wef_backend_common.h"

@interface WefApplication : NSApplication <CefAppProtocol> {
 @private
  BOOL handlingSendEvent_;
}
@end

@implementation WefApplication
- (BOOL)isHandlingSendEvent {
  return handlingSendEvent_;
}

- (void)setHandlingSendEvent:(BOOL)handlingSendEvent {
  handlingSendEvent_ = handlingSendEvent;
}

- (void)sendEvent:(NSEvent*)event {
  // Swallow Cmd+Q so Chromium's "Hold ⌘Q to Quit" panel never fires and no
  // default termination happens. Embedders receive the key event through the
  // normal keyboard pipeline and decide what (if anything) to do.
  if (event.type == NSEventTypeKeyDown &&
      (event.modifierFlags & NSEventModifierFlagCommand) &&
      [event.charactersIgnoringModifiers isEqualToString:@"q"]) {
    return;
  }
  CefScopedSendingEvent sendingEventScoper;
  [super sendEvent:event];
}

- (void)terminate:(id)sender {
  WefHandler* handler = WefHandler::GetInstance();
  if (handler && !handler->IsClosing()) {
    handler->CloseAllBrowsers(false);
  }
}
@end

// Dock menu + reopen handler storage lives in backend-common
// (wef_common::{Get,Set}DockMenuMac, {Set,Fire}DockReopenHandlerMac).

@interface WefAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation WefAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:
    (NSApplication*)sender {
  return NSTerminateNow;
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication*)app {
  return NO;
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender
                    hasVisibleWindows:(BOOL)hasVisibleWindows {
  wef_common::FireDockReopenMac(hasVisibleWindows ? true : false);
  // Always swallow the default "show last hidden window" behavior — the
  // embedder's callback decides what to do.
  return NO;
}

- (NSMenu*)applicationDockMenu:(NSApplication*)sender {
  return wef_common::GetDockMenuMac();
}
@end

// Cmd+C/V/X/A on macOS dispatch through the main menu's performKeyEquivalent:
// — Cocoa matches the keystroke against menu items, then sends their action
// (cut:/copy:/paste:/selectAll:) up the responder chain to Chromium's content
// view, which forwards it to the page. Without these items in the main menu,
// the standard editing shortcuts have nowhere to land. Embedder-provided
// menus get this Edit submenu force-appended in
// runtime_loader_mac.mm so the shortcuts keep working.
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
    if ([item action] == @selector(copy:)) return true;
    if ([item submenu] && MenuTreeHasCopyAction([item submenu])) return true;
  }
  return false;
}

// Force an Edit submenu into the menubar if the embedder didn't include one.
// Without copy:/paste: items somewhere in the menu, Cmd+C/V silently no-op.
void EnsureEditMenu(NSMenu* menubar) {
  if (MenuTreeHasCopyAction(menubar)) return;
  NSMenuItem* editItem = [[NSMenuItem alloc] init];
  [editItem setSubmenu:BuildDefaultEditSubmenu()];
  [menubar addItem:editItem];
}

static void InstallDefaultEditMenu() {
  NSMenu* mainMenu = [[NSMenu alloc] init];

  // Empty placeholder so AppKit fills slot 0 with the bold app name.
  NSMenuItem* appItem = [[NSMenuItem alloc] init];
  [appItem setSubmenu:[[NSMenu alloc] init]];
  [mainMenu addItem:appItem];

  EnsureEditMenu(mainMenu);
  [NSApp setMainMenu:mainMenu];
}

static int run_headless(const char* runtimePath) {
  RuntimeLoader* loader = RuntimeLoader::GetInstance();

  std::string path;
  if (runtimePath) {
    path = runtimePath;
  } else {
    const char* envPath = getenv("WEF_RUNTIME_PATH");
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
      g_runtime_path = argv[i + 1];
      runtimePathArg = [NSString stringWithUTF8String:argv[i + 1]];
      break;
    } else if (strncmp(argv[i], "--runtime=", 10) == 0) {
      g_runtime_path = argv[i] + 10;
      runtimePathArg = [NSString stringWithUTF8String:argv[i] + 10];
      break;
    }
  }

  if (is_forked_worker() || is_cli_worker_command(argc, argv)) {
    return run_headless(runtimePathArg ? [runtimePathArg UTF8String] : nullptr);
  }

  CefScopedLibraryLoader library_loader;
  if (!library_loader.LoadInMain()) {
    return 1;
  }

  CefMainArgs main_args(argc, argv);

  @autoreleasepool {
    // Allow the host to override the user-visible app name (menu-bar app
    // menu, Dock, Cmd-Tab) at launch, e.g. the project name during
    // `deno desktop --hmr`. AppKit derives the app menu title from the
    // process name for an exec'd, non-LaunchServices-registered bundle like
    // ours, so set it before the menu is built. Do this before
    // +sharedApplication so the first read already sees the new name.
    if (const char* app_name = getenv("WEF_APP_NAME")) {
      if (*app_name) {
        [[NSProcessInfo processInfo]
            setProcessName:[NSString stringWithUTF8String:app_name]];
      }
    }

    [WefApplication sharedApplication];
    CHECK([NSApp isKindOfClass:[WefApplication class]]);

    // The bundle's CFBundleExecutable is a sh launcher that exec's this
    // binary, so LaunchServices doesn't auto-register us as a foreground
    // app. Call this before CefInitialize so CEF's startup sees a
    // properly-registered NSApp — otherwise NSDockTile.setBadgeLabel: is
    // silently ignored.
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    // Allow the host to override the dock icon at launch (e.g. a project's
    // favicon during `deno desktop --hmr`). Setting it programmatically
    // bypasses LaunchServices' icon cache and the bundle's CFBundleIconFile,
    // both of which we can't rely on for the shared dev bundle.
    if (const char* icon_path = getenv("WEF_APP_ICON")) {
      NSString* path = [NSString stringWithUTF8String:icon_path];
      NSImage* icon = [[NSImage alloc] initWithContentsOfFile:path];
      if (icon) {
        [NSApp setApplicationIconImage:icon];
      }
    }

    CefSettings settings;
    settings.no_sandbox = true;

    std::string cache_path = std::string(NSTemporaryDirectory().UTF8String) +
                             "wef_cef_" + std::to_string(getpid());
    CefString(&settings.root_cache_path) = cache_path;

    if (const char* port_env = getenv("WEF_REMOTE_DEBUGGING_PORT")) {
      int port = atoi(port_env);
      if (port > 0 && port < 65536) {
        settings.remote_debugging_port = port;
      }
    }

    CefRefPtr<WefApp> app(new WefApp);

    if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
      return CefGetExitCode();
    }

    InstallDefaultEditMenu();

    // NSApp.delegate is a weak property — keep a strong reference here so
    // the delegate isn't released immediately under ARC.
    static WefAppDelegate* delegate = [[WefAppDelegate alloc] init];
    NSApp.delegate = delegate;

    [NSApp activateIgnoringOtherApps:YES];

    CefRunMessageLoop();

    RuntimeLoader::GetInstance()->Shutdown();

    CefShutdown();
  }

  return 0;
}
