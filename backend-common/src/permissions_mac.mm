// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// UNUserNotificationCenter authorization for LAUFEY_PERMISSION_NOTIFICATIONS.
// UN requires the process to run inside a bundled .app with a
// CFBundleIdentifier; without one `getNotificationSettings:` returns
// garbage and `requestAuthorization:` fails immediately. We detect that
// case and report UNSUPPORTED so the embedder can branch on it instead
// of seeing a phantom DENIED.

#include "laufey_backend_common.h"

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

namespace laufey_common {

namespace {

int MapUNStatus(UNAuthorizationStatus s) {
  switch (s) {
    case UNAuthorizationStatusNotDetermined:
      return LAUFEY_PERMISSION_STATUS_PROMPT;
    case UNAuthorizationStatusDenied:
      return LAUFEY_PERMISSION_STATUS_DENIED;
    case UNAuthorizationStatusAuthorized:
    case UNAuthorizationStatusProvisional:
      return LAUFEY_PERMISSION_STATUS_GRANTED;
    default:
      return LAUFEY_PERMISSION_STATUS_UNSUPPORTED;
  }
}

bool MacProcessIsBundled() {
  NSBundle* mb = [NSBundle mainBundle];
  if (!mb) return false;
  if (![mb bundleIdentifier]) return false;
  // Reject the synthetic bundle `cargo run` etc. produces for a bare
  // exe (path doesn't end in .app).
  NSString* path = [mb bundlePath];
  return path && [path hasSuffix:@".app"];
}

void FirePermissionOnMain(laufey_permission_callback_fn cb, void* ud,
                          int status) {
  if (!cb) return;
  dispatch_async(dispatch_get_main_queue(), ^{
    cb(ud, status);
  });
}

}  // namespace

void QueryPermissionMac(int kind, laufey_permission_callback_fn cb,
                        void* user_data) {
  if (kind != LAUFEY_PERMISSION_NOTIFICATIONS) {
    FirePermissionOnMain(cb, user_data, LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
    return;
  }
  if (!MacProcessIsBundled()) {
    FirePermissionOnMain(cb, user_data, LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
    return;
  }
  UNUserNotificationCenter* center =
      [UNUserNotificationCenter currentNotificationCenter];
  [center getNotificationSettingsWithCompletionHandler:^(
              UNNotificationSettings* settings) {
    int status = MapUNStatus(settings.authorizationStatus);
    FirePermissionOnMain(cb, user_data, status);
  }];
}

void RequestPermissionMac(int kind, laufey_permission_callback_fn cb,
                          void* user_data) {
  if (kind != LAUFEY_PERMISSION_NOTIFICATIONS) {
    FirePermissionOnMain(cb, user_data, LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
    return;
  }
  if (!MacProcessIsBundled()) {
    FirePermissionOnMain(cb, user_data, LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
    return;
  }
  UNUserNotificationCenter* center =
      [UNUserNotificationCenter currentNotificationCenter];
  UNAuthorizationOptions opts = UNAuthorizationOptionAlert |
                                UNAuthorizationOptionSound |
                                UNAuthorizationOptionBadge;
  [center requestAuthorizationWithOptions:opts
                       completionHandler:^(BOOL granted, NSError* error) {
                         (void)error;
                         // After the user picks, fetch the real status —
                         // `granted` is BOOL but the cached state can be
                         // PROVISIONAL or EPHEMERAL which we still want
                         // mapped through MapUNStatus.
                         [center getNotificationSettingsWithCompletionHandler:^(
                                     UNNotificationSettings* settings) {
                           int status;
                           if (granted) {
                             status =
                                 MapUNStatus(settings.authorizationStatus);
                           } else {
                             // The OS rejected; map by current settings
                             // (NotDetermined collapses to DENIED here
                             // because the request was rejected).
                             status = (settings.authorizationStatus ==
                                       UNAuthorizationStatusNotDetermined)
                                          ? LAUFEY_PERMISSION_STATUS_DENIED
                                          : MapUNStatus(
                                                settings.authorizationStatus);
                           }
                           FirePermissionOnMain(cb, user_data, status);
                         }];
                       }];
}

}  // namespace laufey_common
