// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Stub permission implementation for Windows and Linux. Both platforms'
// notification systems (Shell_NotifyIcon balloons / notify-send) have no
// permission model, so we report GRANTED synchronously for
// LAUFEY_PERMISSION_NOTIFICATIONS and UNSUPPORTED for anything else.

#include "laufey_backend_common.h"

namespace laufey_common {

void QueryPermissionStub(int kind, laufey_permission_callback_fn cb,
                         void* user_data) {
  if (!cb) return;
  cb(user_data, kind == LAUFEY_PERMISSION_NOTIFICATIONS
                    ? LAUFEY_PERMISSION_STATUS_GRANTED
                    : LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
}

void RequestPermissionStub(int kind, laufey_permission_callback_fn cb,
                           void* user_data) {
  if (!cb) return;
  cb(user_data, kind == LAUFEY_PERMISSION_NOTIFICATIONS
                    ? LAUFEY_PERMISSION_STATUS_GRANTED
                    : LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
}

}  // namespace laufey_common
