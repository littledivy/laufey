// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// NSAlert-based dialogs. `runModal` itself spins NSRunLoop, pumping AppKit
// events for other windows while the dialog is up. Runs on main directly
// when the caller is already there (the typical case for the Deno
// runtime); otherwise dispatch_sync forwards.

#include "laufey_backend_common.h"

#import <Cocoa/Cocoa.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace laufey_common {

int ShowDialogMac(int dialog_type, const std::string& title,
                  const std::string& message,
                  const std::string& default_value, char** out_input_value) {
  if (out_input_value)
    *out_input_value = nullptr;
  NSString* nsTitle = [NSString stringWithUTF8String:title.c_str()];
  NSString* nsMessage = [NSString stringWithUTF8String:message.c_str()];
  NSString* nsDefault = [NSString stringWithUTF8String:default_value.c_str()];

  __block bool confirmed = false;
  __block char* input_strdup = nullptr;
  void (^body)(void) = ^{
    NSAlert* alert = [[NSAlert alloc] init];
    [alert setMessageText:nsTitle];
    [alert setInformativeText:nsMessage];

    NSTextField* inputField = nil;
    if (dialog_type == LAUFEY_DIALOG_ALERT) {
      [alert addButtonWithTitle:@"OK"];
    } else if (dialog_type == LAUFEY_DIALOG_CONFIRM) {
      [alert addButtonWithTitle:@"OK"];
      [alert addButtonWithTitle:@"Cancel"];
    } else if (dialog_type == LAUFEY_DIALOG_PROMPT) {
      [alert addButtonWithTitle:@"OK"];
      [alert addButtonWithTitle:@"Cancel"];
      inputField =
          [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
      [inputField setStringValue:nsDefault];
      [alert setAccessoryView:inputField];
      [alert layout];
      [[alert window] makeFirstResponder:inputField];
    }

    NSModalResponse response = [alert runModal];
    confirmed = (response == NSAlertFirstButtonReturn);
    if (dialog_type == LAUFEY_DIALOG_PROMPT && confirmed && inputField &&
        out_input_value) {
      const char* text = [[inputField stringValue] UTF8String];
      if (text)
        input_strdup = strdup(text);
    }
  };

  if ([NSThread isMainThread]) {
    body();
  } else {
    dispatch_sync(dispatch_get_main_queue(), body);
  }

  if (out_input_value && input_strdup)
    *out_input_value = input_strdup;
  return confirmed ? 1 : 0;
}

}  // namespace laufey_common
