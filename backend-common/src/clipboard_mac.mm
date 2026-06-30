// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// NSPasteboard-backed clipboard. Pasteboard access is forwarded to the main
// thread (via dispatch_sync when called elsewhere), matching the convention
// used by the AppKit dialog/menu code in this directory.

#include "laufey_backend_common.h"

#import <Cocoa/Cocoa.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace laufey_common {

char* ClipboardReadTextMac() {
  __block char* result = nullptr;
  void (^body)(void) = ^{
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    NSString* str = [pb stringForType:NSPasteboardTypeString];
    if (str) {
      const char* utf8 = [str UTF8String];
      if (utf8)
        result = strdup(utf8);
    }
  };
  if ([NSThread isMainThread]) {
    body();
  } else {
    dispatch_sync(dispatch_get_main_queue(), body);
  }
  return result;
}

void ClipboardWriteTextMac(const std::string& text) {
  NSString* str = [NSString stringWithUTF8String:text.c_str()];
  if (!str)
    str = @"";
  void (^body)(void) = ^{
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:str forType:NSPasteboardTypeString];
  };
  if ([NSThread isMainThread]) {
    body();
  } else {
    dispatch_sync(dispatch_get_main_queue(), body);
  }
}

}  // namespace laufey_common
