#include "gui/settings_view.h"
#include "gui/dialog.h"

#include <ghm/log.h>
#include <stdlib.h>

typedef struct {
    GtkWidget *log_view;
    GtkWidget *message;
} SettingsState;

typedef struct {
    char *text;
    GhmError error;
    gboolean succeeded;
} LogTask;

static void log_task_free(gpointer data)
{
    LogTask *request = data;
    free(request->text);
    free(request);
}

static void log_worker(GTask *task, gpointer source, gpointer task_data,
                       GCancellable *cancellable)
{
    LogTask *request = task_data;
    (void)source;
    (void)cancellable;
    request->succeeded = ghm_log_tail(64 * 1024, &request->text, &request->error) == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void log_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *view = GTK_WIDGET(source);
    SettingsState *state = g_object_get_data(G_OBJECT(view), "ghm-settings-state");
    LogTask *request = g_task_get_task_data(G_TASK(result));
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (!ghm_view_is_open(view)) return;
    if (!request->succeeded) {
        gtk_label_set_text(GTK_LABEL(state->message), request->error.message);
        return;
    }
    char *valid = g_utf8_make_valid(request->text != NULL ? request->text : "", -1);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->log_view));
    gtk_text_buffer_set_text(buffer, valid, -1);
    gtk_label_set_text(GTK_LABEL(state->message), valid[0] == '\0' ? "No log events yet." : "Latest log events");
    g_free(valid);
}

static void refresh_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *view = GTK_WIDGET(user_data);
    SettingsState *state = g_object_get_data(G_OBJECT(view), "ghm-settings-state");
    LogTask *request = calloc(1, sizeof(*request));
    GTask *task;
    (void)button;
    if (request == NULL) return;
    gtk_label_set_text(GTK_LABEL(state->message), "Reading application log…");
    task = g_task_new(view, NULL, log_finished, NULL);
    g_task_set_task_data(task, request, log_task_free);
    g_task_run_in_thread(task, log_worker);
    g_object_unref(task);
}

GtkWidget *ghm_settings_view_new(void)
{
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *refresh = gtk_button_new_with_label("Refresh Developer Log");
    GtkWidget *scroll = gtk_scrolled_window_new();
    SettingsState *state = g_new0(SettingsState, 1);
    state->log_view = gtk_text_view_new();
#ifdef _WIN32
    state->message = gtk_label_new("Application log: your Windows application data folder, ghm/state/ghm.log");
#else
    state->message = gtk_label_new("Application log: ~/.local/state/ghm/ghm.log");
#endif
    gtk_label_set_xalign(GTK_LABEL(state->message), 0.0f);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(state->log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(state->log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(state->log_view), TRUE);
    gtk_widget_set_halign(refresh, GTK_ALIGN_START);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), state->log_view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(root), gtk_label_new(
        "Scheduled jobs run in the separate user service while it is active.\n"
        "Commit dates are Git metadata, not GitHub push times."));
    gtk_box_append(GTK_BOX(root), refresh);
    gtk_box_append(GTK_BOX(root), state->message);
    gtk_box_append(GTK_BOX(root), scroll);
    g_object_set_data_full(G_OBJECT(root), "ghm-settings-state", state, g_free);
    g_signal_connect(refresh, "clicked", G_CALLBACK(refresh_clicked), root);
    return root;
}
