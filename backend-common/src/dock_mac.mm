// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// App-scoped macOS dock primitives. All run on the main queue because
// NSApp / NSDockTile interactions must happen on main.

#include "laufey_backend_common.h"

#import <AppKit/AppKit.h>

namespace laufey_common {

void SetDockBadgeMac(const char* badge_or_null) {
  NSString* ns = (badge_or_null && *badge_or_null)
                     ? [NSString stringWithUTF8String:badge_or_null]
                     : nil;
  dispatch_async(dispatch_get_main_queue(), ^{
    NSDockTile* tile = [NSApp dockTile];
    [tile setBadgeLabel:ns];
    [tile display];
  });
}

void BounceDockMac(int type) {
  NSRequestUserAttentionType t = (type == LAUFEY_DOCK_BOUNCE_CRITICAL)
                                     ? NSCriticalRequest
                                     : NSInformationalRequest;
  dispatch_async(dispatch_get_main_queue(), ^{
    [NSApp requestUserAttention:t];
  });
}

void SetDockVisibleMac(bool visible) {
  NSApplicationActivationPolicy policy =
      visible ? NSApplicationActivationPolicyRegular
              : NSApplicationActivationPolicyAccessory;
  dispatch_async(dispatch_get_main_queue(), ^{
    [NSApp setActivationPolicy:policy];
  });
}

// --- Dock menu + reopen handler storage (read by each backend's AppDelegate)

namespace {
NSMenu* g_dock_menu = nil;
laufey_dock_reopen_fn g_dock_reopen_fn = nullptr;
void* g_dock_reopen_data = nullptr;
}  // namespace

void SetDockMenuMac(NSMenu* menu) {
  g_dock_menu = menu;
}

NSMenu* GetDockMenuMac() {
  return g_dock_menu;
}

void SetDockReopenHandlerMac(laufey_dock_reopen_fn handler, void* user_data) {
  g_dock_reopen_fn = handler;
  g_dock_reopen_data = user_data;
}

void FireDockReopenMac(bool has_visible_windows) {
  if (g_dock_reopen_fn) {
    g_dock_reopen_fn(g_dock_reopen_data, has_visible_windows);
  }
}

}  // namespace laufey_common
