// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// gtk_message_dialog-based dialogs. Both backends (CEF, webview) link
// GTK3 on Linux, so we can use the in-process dialog rather than
// shelling out to zenity.

#include <gtk/gtk.h>

#include "laufey_backend_common.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace laufey_common {

int ShowDialogLinux(int dialog_type, const std::string& title,
                    const std::string& message,
                    const std::string& default_value, char** out_input_value) {
  if (out_input_value)
    *out_input_value = nullptr;

  GtkWindow* parent = nullptr;

  if (dialog_type == LAUFEY_DIALOG_ALERT) {
    GtkWidget* dlg = gtk_message_dialog_new(
        parent, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s",
        message.c_str());
    if (!title.empty())
      gtk_window_set_title(GTK_WINDOW(dlg), title.c_str());
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    return 1;
  }
  if (dialog_type == LAUFEY_DIALOG_CONFIRM) {
    GtkWidget* dlg = gtk_message_dialog_new(
        parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
        "%s", message.c_str());
    if (!title.empty())
      gtk_window_set_title(GTK_WINDOW(dlg), title.c_str());
    gint result = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    return (result == GTK_RESPONSE_OK) ? 1 : 0;
  }
  if (dialog_type == LAUFEY_DIALOG_PROMPT) {
    GtkWidget* dlg = gtk_message_dialog_new(
        parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
        "%s", message.c_str());
    if (!title.empty())
      gtk_window_set_title(GTK_WINDOW(dlg), title.c_str());
    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), default_value.c_str());
    gtk_container_add(GTK_CONTAINER(content), entry);
    gtk_widget_show(entry);

    gint result = gtk_dialog_run(GTK_DIALOG(dlg));
    int rc = 0;
    if (result == GTK_RESPONSE_OK) {
      const char* text = gtk_entry_get_text(GTK_ENTRY(entry));
      if (out_input_value && text)
        *out_input_value = strdup(text);
      rc = 1;
    }
    gtk_widget_destroy(dlg);
    return rc;
  }
  return 0;
}

}  // namespace laufey_common
