// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// GTK3 clipboard (the CLIPBOARD selection). Both backends (CEF, webview)
// already link and initialize GTK on Linux, so we use the in-process
// clipboard rather than shelling out to xclip / wl-clipboard.

#include <gtk/gtk.h>

#include "laufey_backend_common.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace laufey_common {

char* ClipboardReadTextLinux() {
  GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
  if (!clipboard)
    return nullptr;
  gchar* text = gtk_clipboard_wait_for_text(clipboard);
  if (!text)
    return nullptr;
  // Re-own through malloc/strdup so the caller frees with free() (matching the
  // other ClipboardReadText* implementations) rather than g_free.
  char* result = strdup(text);
  g_free(text);
  return result;
}

void ClipboardWriteTextLinux(const std::string& text) {
  GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
  if (!clipboard)
    return;
  gtk_clipboard_set_text(clipboard, text.c_str(),
                         static_cast<gint>(text.size()));
  // Persist the contents so they survive this process exiting, if a clipboard
  // manager is running to take ownership.
  gtk_clipboard_store(clipboard);
}

}  // namespace laufey_common
