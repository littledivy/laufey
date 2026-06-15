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
#include "laufey_backend_common.h"

@interface LaufeyApplication : NSApplication <CefAppProtocol> {
 @private
  BOOL handlingSendEvent_;
}
@end

@implementation LaufeyApplication
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
  LaufeyHandler* handler = LaufeyHandler::GetInstance();
  if (handler && !handler->IsClosing()) {
    handler->CloseAllBrowsers(false);
  }
}
@end

// Dock menu + reopen handler storage lives in backend-common
// (laufey_common::{Get,Set}DockMenuMac, {Set,Fire}DockReopenHandlerMac).

@interface LaufeyAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation LaufeyAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:
    (NSApplication*)sender {
  return NSTerminateNow;
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication*)app {
  return NO;
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

// ── External message pump ────────────────────────────────────────────────
// CEF's own CefRunMessageLoop() does not service libdispatch's main queue, so
// dispatch_async(dispatch_get_main_queue(), …) work — NSStatusItem (tray)
// creation, notifications, etc. — never runs. Instead we enable
// settings.external_message_pump and pump CefDoMessageLoopWork() from the main
// NSRunLoop driven by [NSApp run], which DOES drain the main queue.
@interface LaufeyPumpTarget : NSObject
- (void)start;
- (void)tick;
@end

@implementation LaufeyPumpTarget {
  NSTimer* _timer;
  BOOL _working;
}
- (void)tick {
  if (_working)
    return;  // CefDoMessageLoopWork is not reentrant
  _working = YES;
  CefDoMessageLoopWork();
  _working = NO;
}
- (void)start {
  // Steady pump: external_message_pump's OnScheduleMessagePumpWork alone
  // doesn't reliably re-fire for cross-thread CefPostTasks, so drive CEF on a
  // fixed cadence. Common modes so it keeps ticking while a menu/tray popup is
  // tracking the run loop.
  _timer = [NSTimer timerWithTimeInterval:(1.0 / 60.0)
                                   target:self
                                 selector:@selector(tick)
                                 userInfo:nil
                                  repeats:YES];
  [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
}
@end

static LaufeyPumpTarget* g_pump = nil;

void LaufeyApp::OnScheduleMessagePumpWork(int64_t delay_ms) {
  // Nudge an immediate pump for snappier response; the steady timer guarantees
  // progress regardless.
  dispatch_async(dispatch_get_main_queue(), ^{
    if (g_pump)
      [g_pump tick];
  });
}

void LaufeyQuitMainLoopMac() {
  dispatch_async(dispatch_get_main_queue(), ^{
    [NSApp stop:nil];
    // [NSApp run] only returns after it dequeues one more event; nudge it.
    NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSZeroPoint
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0];
    [NSApp postEvent:event atStart:YES];
  });
}

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
    if ([item action] == @selector(copy:))
      return true;
    if ([item submenu] && MenuTreeHasCopyAction([item submenu]))
      return true;
  }
  return false;
}

// Force an Edit submenu into the menubar if the embedder didn't include one.
// Without copy:/paste: items somewhere in the menu, Cmd+C/V silently no-op.
void EnsureEditMenu(NSMenu* menubar) {
  if (MenuTreeHasCopyAction(menubar))
    return;
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
    if (const char* app_name = getenv("LAUFEY_APP_NAME")) {
      if (*app_name) {
        [[NSProcessInfo processInfo]
            setProcessName:[NSString stringWithUTF8String:app_name]];
      }
    }

    [LaufeyApplication sharedApplication];
    CHECK([NSApp isKindOfClass:[LaufeyApplication class]]);

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
    if (const char* icon_path = getenv("LAUFEY_APP_ICON")) {
      NSString* path = [NSString stringWithUTF8String:icon_path];
      NSImage* icon = [[NSImage alloc] initWithContentsOfFile:path];
      if (icon) {
        [NSApp setApplicationIconImage:icon];
      }
    }

    CefSettings settings;
    settings.no_sandbox = true;
    // Run [NSApp run] as the message loop (see LaufeyPumpTarget) so the main
    // libdispatch queue is serviced — required for tray/status items.
    settings.external_message_pump = true;

    std::string cache_path = std::string(NSTemporaryDirectory().UTF8String) +
                             "laufey_cef_" + std::to_string(getpid());
    CefString(&settings.root_cache_path) = cache_path;

    if (const char* port_env = getenv("LAUFEY_REMOTE_DEBUGGING_PORT")) {
      int port = atoi(port_env);
      if (port > 0 && port < 65536) {
        settings.remote_debugging_port = port;
      }
    }

    CefRefPtr<LaufeyApp> app(new LaufeyApp);

    // Must exist before CefInitialize so the first OnScheduleMessagePumpWork
    // (which kicks OnContextInitialized → runtime start) isn't dropped.
    g_pump = [[LaufeyPumpTarget alloc] init];

    if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
      return CefGetExitCode();
    }

    InstallDefaultEditMenu();

    // NSApp.delegate is a weak property — keep a strong reference here so
    // the delegate isn't released immediately under ARC.
    static LaufeyAppDelegate* delegate = [[LaufeyAppDelegate alloc] init];
    NSApp.delegate = delegate;

    [NSApp activateIgnoringOtherApps:YES];

    // Drive CEF from the main NSRunLoop (external_message_pump). Unlike
    // CefRunMessageLoop(), [NSApp run] also drains the libdispatch main queue.
    [g_pump start];  // begin the steady pump so the runtime starts
    [NSApp run];

    RuntimeLoader::GetInstance()->Shutdown();

    CefShutdown();
  }

  return 0;
}
