// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// NSMenu construction from a laufey_value_t menu template. Each non-role
// menu item carries its own click handler+data in an attached
// LaufeyCommonMenuItem object (via NSMenuItem.representedObject), so the
// same builder serves the application menu, context menu, dock menu,
// and tray menus without per-context global state.

#include "laufey_backend_common.h"

#import <AppKit/AppKit.h>

#include <cctype>
#include <string>
#include <vector>

@interface LaufeyCommonMenuItem : NSObject
@property(nonatomic, copy) NSString* itemId;
@property(nonatomic, assign) laufey_menu_click_fn clickFn;
@property(nonatomic, assign) void* clickData;
@property(nonatomic, assign) uint32_t windowId;
@end

@implementation LaufeyCommonMenuItem
@end

@interface LaufeyCommonMenuTarget : NSObject
+ (instancetype)shared;
- (void)menuItemClicked:(id)sender;
@end

@implementation LaufeyCommonMenuTarget
+ (instancetype)shared {
  static LaufeyCommonMenuTarget* instance = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    instance = [[LaufeyCommonMenuTarget alloc] init];
  });
  return instance;
}

- (void)menuItemClicked:(id)sender {
  NSMenuItem* item = (NSMenuItem*)sender;
  id rep = [item representedObject];
  if (![rep isKindOfClass:[LaufeyCommonMenuItem class]]) return;
  LaufeyCommonMenuItem* w = (LaufeyCommonMenuItem*)rep;
  if (!w.clickFn || !w.itemId) return;
  w.clickFn(w.clickData, w.windowId, [w.itemId UTF8String]);
}
@end

namespace laufey_common {

namespace {

void ParseAccelerator(const std::string& accel, NSString** outKey,
                      NSEventModifierFlags* outMask) {
  *outKey = @"";
  *outMask = 0;

  std::string lower = accel;
  for (auto& c : lower) c = static_cast<char>(tolower(c));

  std::vector<std::string> parts;
  std::string remaining = lower;
  size_t pos;
  while ((pos = remaining.find('+')) != std::string::npos) {
    parts.push_back(remaining.substr(0, pos));
    remaining = remaining.substr(pos + 1);
  }
  if (!remaining.empty()) parts.push_back(remaining);

  for (const auto& part : parts) {
    if (part == "cmd" || part == "command" || part == "cmdorctrl" ||
        part == "commandorcontrol") {
      *outMask |= NSEventModifierFlagCommand;
    } else if (part == "shift") {
      *outMask |= NSEventModifierFlagShift;
    } else if (part == "alt" || part == "option") {
      *outMask |= NSEventModifierFlagOption;
    } else if (part == "ctrl" || part == "control") {
      *outMask |= NSEventModifierFlagControl;
    } else {
      *outKey = [NSString stringWithUTF8String:part.c_str()];
    }
  }
}

NSMenuItem* CreateRoleMenuItem(const std::string& role) {
  NSString* title = @"";
  SEL action = nil;
  NSString* keyEquiv = @"";
  NSEventModifierFlags mask = NSEventModifierFlagCommand;

  if (role == "quit") {
    title = @"Quit"; action = @selector(terminate:); keyEquiv = @"q";
  } else if (role == "copy") {
    title = @"Copy"; action = @selector(copy:); keyEquiv = @"c";
  } else if (role == "paste") {
    title = @"Paste"; action = @selector(paste:); keyEquiv = @"v";
  } else if (role == "cut") {
    title = @"Cut"; action = @selector(cut:); keyEquiv = @"x";
  } else if (role == "selectall" || role == "selectAll") {
    title = @"Select All"; action = @selector(selectAll:); keyEquiv = @"a";
  } else if (role == "undo") {
    title = @"Undo"; action = @selector(undo:); keyEquiv = @"z";
  } else if (role == "redo") {
    title = @"Redo"; action = @selector(redo:); keyEquiv = @"Z";
    mask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
  } else if (role == "minimize") {
    title = @"Minimize"; action = @selector(performMiniaturize:); keyEquiv = @"m";
  } else if (role == "zoom") {
    title = @"Zoom"; action = @selector(performZoom:);
  } else if (role == "close") {
    title = @"Close"; action = @selector(performClose:); keyEquiv = @"w";
  } else if (role == "about") {
    title = @"About"; action = @selector(orderFrontStandardAboutPanel:);
  } else if (role == "hide") {
    title = @"Hide"; action = @selector(hide:); keyEquiv = @"h";
  } else if (role == "hideothers" || role == "hideOthers") {
    title = @"Hide Others"; action = @selector(hideOtherApplications:);
    keyEquiv = @"h";
    mask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
  } else if (role == "unhide") {
    title = @"Show All"; action = @selector(unhideAllApplications:);
  } else if (role == "front") {
    title = @"Bring All to Front"; action = @selector(arrangeInFront:);
  } else if (role == "togglefullscreen" || role == "toggleFullScreen") {
    title = @"Toggle Full Screen"; action = @selector(toggleFullScreen:);
    keyEquiv = @"f";
    mask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
  } else {
    return nil;
  }

  NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                action:action
                                         keyEquivalent:keyEquiv];
  [item setKeyEquivalentModifierMask:mask];
  return item;
}

std::string DictString(const laufey_backend_api_t* api, laufey_value_t* dict,
                       const char* key) {
  laufey_value_t* v = api->value_dict_get(dict, key);
  if (!v || !api->value_is_string(v)) return std::string();
  size_t len = 0;
  char* s = api->value_get_string(v, &len);
  if (!s) return std::string();
  std::string out(s, len);
  api->value_free_string(s);
  return out;
}

}  // namespace

NSMenu* BuildNSMenuFromValue(laufey_value_t* val, const laufey_backend_api_t* api,
                             laufey_menu_click_fn on_click, void* on_click_data,
                             uint32_t window_id) {
  if (!val || !api->value_is_list(val)) return nil;
  NSMenu* menu = [[NSMenu alloc] init];
  [menu setAutoenablesItems:NO];
  size_t count = api->value_list_size(val);
  for (size_t i = 0; i < count; ++i) {
    laufey_value_t* itemVal = api->value_list_get(val, i);
    if (!itemVal || !api->value_is_dict(itemVal)) continue;

    std::string typeStr = DictString(api, itemVal, "type");
    if (typeStr == "separator") {
      [menu addItem:[NSMenuItem separatorItem]];
      continue;
    }

    std::string roleStr = DictString(api, itemVal, "role");
    if (!roleStr.empty()) {
      NSMenuItem* roleItem = CreateRoleMenuItem(roleStr);
      if (roleItem) [menu addItem:roleItem];
      continue;
    }

    std::string labelStr = DictString(api, itemVal, "label");
    if (labelStr.empty()) continue;
    NSString* label = [NSString stringWithUTF8String:labelStr.c_str()];

    laufey_value_t* submenuVal = api->value_dict_get(itemVal, "submenu");
    if (submenuVal && api->value_is_list(submenuVal)) {
      NSMenuItem* submenuItem = [[NSMenuItem alloc] init];
      [submenuItem setTitle:label];
      NSMenu* submenu = BuildNSMenuFromValue(submenuVal, api, on_click,
                                              on_click_data, window_id);
      [submenu setTitle:label];
      [submenuItem setSubmenu:submenu];
      [menu addItem:submenuItem];
      continue;
    }

    NSString* keyEquiv = @"";
    NSEventModifierFlags modMask = NSEventModifierFlagCommand;
    std::string accelStr = DictString(api, itemVal, "accelerator");
    if (!accelStr.empty()) {
      ParseAccelerator(accelStr, &keyEquiv, &modMask);
    }

    NSMenuItem* nsItem =
        [[NSMenuItem alloc] initWithTitle:label
                                   action:@selector(menuItemClicked:)
                            keyEquivalent:keyEquiv];
    [nsItem setKeyEquivalentModifierMask:modMask];
    [nsItem setTarget:[LaufeyCommonMenuTarget shared]];

    std::string idStr = DictString(api, itemVal, "id");
    if (!idStr.empty()) {
      LaufeyCommonMenuItem* wrap = [[LaufeyCommonMenuItem alloc] init];
      wrap.itemId = [NSString stringWithUTF8String:idStr.c_str()];
      wrap.clickFn = on_click;
      wrap.clickData = on_click_data;
      wrap.windowId = window_id;
      [nsItem setRepresentedObject:wrap];
    }

    laufey_value_t* enabledVal = api->value_dict_get(itemVal, "enabled");
    if (enabledVal && api->value_is_bool(enabledVal)) {
      [nsItem setEnabled:api->value_get_bool(enabledVal)];
    } else {
      [nsItem setEnabled:YES];
    }
    [menu addItem:nsItem];
  }
  return menu;
}

}  // namespace laufey_common
