#include "gui/dialog.h"
#include "gui/history_view.h"

#include <ghm/history.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    GtkWidget *parent;
    GhmContext *context;
    GhmHistoryRewritten rewritten;
    char *path;
    GtkWidget *list;
    GtkWidget *detail;
    GtkWidget *rewrite_button;
    char head_oid[GHM_OID_HEX_CAPACITY];
    char *head_message;
    int64_t author_timestamp;
    int64_t committer_timestamp;
    unsigned generation;
} HistoryState;

typedef struct {
    char *path;
    unsigned generation;
    GhmHistory history;
    GhmError error;
    gboolean succeeded;
} HistoryTask;

typedef struct {
    GtkWidget *view;
    GtkWidget *message;
    GtkWidget *author_calendar;
    GtkWidget *author_hour;
    GtkWidget *author_minute;
    GtkWidget *committer_calendar;
    GtkWidget *committer_hour;
    GtkWidget *committer_minute;
    GtkWidget *confirmed;
    GtkWidget *feedback;
    GtkWidget *submit;
    gboolean busy;
} RewriteDialog;

typedef struct {
    GhmContext *context;
    char *path;
    char *message;
    char expected_oid[GHM_OID_HEX_CAPACITY];
    char result_oid[GHM_OID_HEX_CAPACITY];
    int64_t author_timestamp;
    int64_t committer_timestamp;
    int author_offset;
    int committer_offset;
    GhmError error;
    gboolean succeeded;
} RewriteTask;

static void history_state_free(gpointer data)
{
    HistoryState *state = data;
    free(state->path);
    free(state->head_message);
    free(state);
}

static void rewrite_task_free(gpointer data)
{
    RewriteTask *request = data;
    free(request->path);
    free(request->message);
    free(request);
}

static void history_task_free(gpointer data)
{
    HistoryTask *request = data;
    free(request->path);
    ghm_history_free(&request->history);
    free(request);
}

static void clear_history(GtkWidget *list)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(list)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(list), child);
}

static void history_selected(GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
    HistoryState *state = user_data;
    const char *details = row != NULL ? g_object_get_data(G_OBJECT(row), "ghm-history-detail") : NULL;
    (void)list;
    gtk_label_set_text(GTK_LABEL(state->detail), details != NULL ? details : "Select a commit to inspect it.");
}

static void history_worker(GTask *task, gpointer source, gpointer task_data,
                           GCancellable *cancellable)
{
    HistoryTask *request = task_data;
    (void)source;
    (void)cancellable;
    request->succeeded = ghm_commit_get_history(request->path, 200,
                                                 &request->history, &request->error) == 0;
    g_task_return_boolean(task, request->succeeded);
}

static char *format_git_date(int64_t timestamp, int offset_minutes)
{
    GDateTime *utc = g_date_time_new_from_unix_utc(timestamp);
    GTimeZone *zone = g_time_zone_new_offset(offset_minutes * 60);
    GDateTime *at_offset = utc != NULL ? g_date_time_to_timezone(utc, zone) : NULL;
    char *formatted = at_offset != NULL ? g_date_time_format(at_offset, "%Y-%m-%d %H:%M %z") : NULL;
    if (at_offset != NULL) g_date_time_unref(at_offset);
    if (utc != NULL) g_date_time_unref(utc);
    g_time_zone_unref(zone);
    return formatted;
}

static void history_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *view = GTK_WIDGET(source);
    HistoryState *state = g_object_get_data(G_OBJECT(view), "ghm-history-state");
    HistoryTask *request = g_task_get_task_data(G_TASK(result));
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (!ghm_view_is_open(view)) return;
    if (request->generation != state->generation || g_strcmp0(request->path, state->path) != 0) return;
    clear_history(state->list);
    if (!request->succeeded) {
        gtk_label_set_text(GTK_LABEL(state->detail), request->error.message);
        return;
    }
    if (request->history.count > 0) {
        const GhmHistoryEntry *head = &request->history.items[0];
        (void)g_strlcpy(state->head_oid, head->oid, sizeof(state->head_oid));
        state->head_message = strdup(head->message);
        state->author_timestamp = head->author_timestamp;
        state->committer_timestamp = head->committer_timestamp;
        gtk_widget_set_sensitive(state->rewrite_button, state->head_message != NULL);
    }
    for (size_t i = 0; i < request->history.count; ++i) {
        const GhmHistoryEntry *entry = &request->history.items[i];
        char *author_text = format_git_date(entry->author_timestamp, entry->author_offset);
        char *committer_text = format_git_date(entry->committer_timestamp, entry->committer_offset);
        char *title = g_strdup_printf("%.10s   %s   %s", entry->oid,
                                       author_text != NULL ? author_text : "?", entry->summary);
        char *details = g_strdup_printf("Commit: %s\nMessage: %s\nAuthor: %s <%s>\n"
                                         "Author date: %s\nCommitter date: %s",
                                         entry->oid, entry->message, entry->author_name,
                                         entry->author_email, author_text != NULL ? author_text : "?",
                                         committer_text != NULL ? committer_text : "?");
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *label = gtk_label_new(title);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        g_object_set_data_full(G_OBJECT(row), "ghm-history-detail", details, g_free);
        gtk_list_box_append(GTK_LIST_BOX(state->list), row);
        g_free(title);
        g_free(author_text);
        g_free(committer_text);
    }
    gtk_label_set_text(GTK_LABEL(state->detail), request->history.count == 0 ?
                       "No commits on this branch yet." : "Select a commit to inspect it.");
}

void ghm_history_view_refresh(GtkWidget *view, const char *repository_path)
{
    HistoryState *state = g_object_get_data(G_OBJECT(view), "ghm-history-state");
    HistoryTask *request;
    GTask *task;
    ++state->generation;
    state->head_oid[0] = '\0';
    free(state->head_message);
    state->head_message = NULL;
    gtk_widget_set_sensitive(state->rewrite_button, FALSE);
    free(state->path);
    state->path = repository_path != NULL ? strdup(repository_path) : NULL;
    clear_history(state->list);
    if (state->path == NULL) {
        gtk_label_set_text(GTK_LABEL(state->detail), "Select a repository to view history.");
        return;
    }
    request = calloc(1, sizeof(*request));
    if (request == NULL) return;
    request->path = strdup(state->path);
    request->generation = state->generation;
    if (request->path == NULL) { history_task_free(request); return; }
    gtk_label_set_text(GTK_LABEL(state->detail), "Loading history…");
    task = g_task_new(view, NULL, history_finished, NULL);
    g_task_set_task_data(task, request, history_task_free);
    g_task_run_in_thread(task, history_worker);
    g_object_unref(task);
}

static void refresh_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *view = GTK_WIDGET(user_data);
    HistoryState *state = g_object_get_data(G_OBJECT(view), "ghm-history-state");
    (void)button;
    if (state->path != NULL) {
        char *path = strdup(state->path);
        if (path != NULL) { ghm_history_view_refresh(view, path); free(path); }
    }
}

static GtkWidget *rewrite_date_column(const char *title, int64_t timestamp,
                                      GtkWidget **calendar, GtkWidget **hour,
                                      GtkWidget **minute)
{
    GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *clock = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GDateTime *date = g_date_time_new_from_unix_local(timestamp);
    *calendar = gtk_calendar_new();
    *hour = gtk_spin_button_new_with_range(0.0, 23.0, 1.0);
    *minute = gtk_spin_button_new_with_range(0.0, 59.0, 1.0);
    if (date != NULL) {
#if GTK_CHECK_VERSION(4, 20, 0)
        gtk_calendar_set_date(GTK_CALENDAR(*calendar), date);
#else
        gtk_calendar_select_day(GTK_CALENDAR(*calendar), date);
#endif
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(*hour), (double)g_date_time_get_hour(date));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(*minute), (double)g_date_time_get_minute(date));
        g_date_time_unref(date);
    }
    gtk_box_append(GTK_BOX(column), gtk_label_new(title));
    gtk_box_append(GTK_BOX(column), *calendar);
    gtk_box_append(GTK_BOX(clock), *hour);
    gtk_box_append(GTK_BOX(clock), gtk_label_new(":"));
    gtk_box_append(GTK_BOX(clock), *minute);
    gtk_box_append(GTK_BOX(column), clock);
    return column;
}

static int rewrite_date_read(GtkWidget *calendar, GtkWidget *hour, GtkWidget *minute,
                             int64_t *timestamp, int *offset)
{
    GDateTime *selected = gtk_calendar_get_date(GTK_CALENDAR(calendar));
    GDateTime *date = selected != NULL ?
        g_date_time_new_local(g_date_time_get_year(selected), g_date_time_get_month(selected),
                              g_date_time_get_day_of_month(selected),
                              gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(hour)),
                              gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(minute)), 0.0) : NULL;
    if (selected != NULL) g_date_time_unref(selected);
    if (date == NULL) return -1;
    *timestamp = g_date_time_to_unix(date);
    *offset = (int)(g_date_time_get_utc_offset(date) / G_TIME_SPAN_MINUTE);
    g_date_time_unref(date);
    return 0;
}

static void rewrite_worker(GTask *task, gpointer source, gpointer task_data,
                           GCancellable *cancellable)
{
    RewriteTask *request = task_data;
    (void)source;
    (void)cancellable;
    request->succeeded = ghm_commit_rewrite_head(request->context, request->path,
        request->expected_oid, request->message, request->author_timestamp,
        request->author_offset, request->committer_timestamp, request->committer_offset,
        1, request->result_oid, &request->error) == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void rewrite_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *dialog = GTK_WIDGET(user_data);
    RewriteDialog *dialog_state = g_object_get_data(G_OBJECT(dialog), "ghm-rewrite-dialog");
    HistoryState *history = g_object_get_data(G_OBJECT(dialog_state->view), "ghm-history-state");
    RewriteTask *request = g_task_get_task_data(G_TASK(result));
    (void)source;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    dialog_state->busy = FALSE;
    if (!gtk_widget_get_realized(GTK_WIDGET(source))) {
        gtk_window_destroy(GTK_WINDOW(dialog));
        g_object_unref(dialog);
        return;
    }
    if (request->succeeded) {
        gtk_window_destroy(GTK_WINDOW(dialog));
        history->rewritten(history->parent);
    } else {
        gtk_label_set_text(GTK_LABEL(dialog_state->feedback), request->error.message);
        gtk_widget_set_sensitive(dialog_state->submit, TRUE);
    }
    g_object_unref(dialog);
}

static void rewrite_submit(GtkButton *button, gpointer user_data)
{
    GtkWidget *dialog = GTK_WIDGET(user_data);
    RewriteDialog *dialog_state = g_object_get_data(G_OBJECT(dialog), "ghm-rewrite-dialog");
    HistoryState *history = g_object_get_data(G_OBJECT(dialog_state->view), "ghm-history-state");
    RewriteTask *request;
    GTask *task;
    GtkTextIter start, end;
    GtkTextBuffer *message_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(dialog_state->message));
    gtk_text_buffer_get_bounds(message_buffer, &start, &end);
    char *message_text = gtk_text_buffer_get_text(message_buffer, &start, &end, FALSE);
    (void)button;
    if (!gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog_state->confirmed))) {
        gtk_label_set_text(GTK_LABEL(dialog_state->feedback), "Confirm the history change first.");
        g_free(message_text);
        return;
    }
    if (message_text[0] == '\0') {
        gtk_label_set_text(GTK_LABEL(dialog_state->feedback), "Enter a commit message.");
        g_free(message_text);
        return;
    }
    request = calloc(1, sizeof(*request));
    if (request == NULL) { g_free(message_text); return; }
    if (rewrite_date_read(dialog_state->author_calendar, dialog_state->author_hour,
                          dialog_state->author_minute, &request->author_timestamp,
                          &request->author_offset) != 0 ||
        rewrite_date_read(dialog_state->committer_calendar, dialog_state->committer_hour,
                          dialog_state->committer_minute, &request->committer_timestamp,
                          &request->committer_offset) != 0) {
        gtk_label_set_text(GTK_LABEL(dialog_state->feedback), "Choose valid local dates and times.");
        free(request);
        g_free(message_text);
        return;
    }
    request->context = history->context;
    request->path = history->path != NULL ? strdup(history->path) : NULL;
    request->message = strdup(message_text);
    g_free(message_text);
    (void)g_strlcpy(request->expected_oid, history->head_oid, sizeof(request->expected_oid));
    if (request->path == NULL || request->message == NULL || request->expected_oid[0] == '\0') {
        rewrite_task_free(request);
        gtk_label_set_text(GTK_LABEL(dialog_state->feedback), "Repository or commit is unavailable.");
        return;
    }
    dialog_state->busy = TRUE;
    gtk_widget_set_sensitive(dialog_state->submit, FALSE);
    gtk_label_set_text(GTK_LABEL(dialog_state->feedback), "Rewriting local HEAD…");
    task = g_task_new(history->parent, NULL, rewrite_finished, g_object_ref(dialog));
    g_task_set_task_data(task, request, rewrite_task_free);
    g_task_run_in_thread(task, rewrite_worker);
    g_object_unref(task);
}

static gboolean rewrite_close_requested(GtkWindow *window, gpointer user_data)
{
    RewriteDialog *state = user_data;
    (void)window;
    return state->busy;
}

static void rewrite_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *view = GTK_WIDGET(user_data);
    HistoryState *history = g_object_get_data(G_OBJECT(view), "ghm-history-state");
    GtkWidget *dialog, *root, *dates, *actions, *cancel;
    RewriteDialog *state;
    (void)button;
    if (history->head_oid[0] == '\0' || history->path == NULL) return;
    dialog = gtk_window_new();
    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    dates = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    cancel = gtk_button_new_with_label("Back");
    state = g_new0(RewriteDialog, 1);
    state->view = view;
    state->message = gtk_text_view_new();
    gtk_widget_set_size_request(state->message, -1, 90);
    state->confirmed = gtk_check_button_new_with_label(
        "I understand this replaces HEAD and changes its commit SHA");
    state->feedback = gtk_label_new("");
    state->submit = gtk_button_new_with_label("Rewrite Local HEAD");
    gtk_window_set_title(GTK_WINDOW(dialog), "Rewrite History — HEAD Only");
    ghm_dialog_attach(dialog, history->parent);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 740, 600);
    gtk_widget_set_margin_start(root, 20);
    gtk_widget_set_margin_end(root, 20);
    gtk_widget_set_margin_top(root, 20);
    gtk_widget_set_margin_bottom(root, 20);
    gtk_box_append(GTK_BOX(root), gtk_label_new("New HEAD commit message"));
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->message)),
                             history->head_message, -1);
    GtkWidget *message_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(message_scroll), state->message);
    gtk_widget_set_size_request(message_scroll, -1, 100);
    gtk_box_append(GTK_BOX(root), message_scroll);
    gtk_box_append(GTK_BOX(dates), rewrite_date_column("Author date (local time)",
        history->author_timestamp, &state->author_calendar,
        &state->author_hour, &state->author_minute));
    gtk_box_append(GTK_BOX(dates), rewrite_date_column("Committer date (local time)",
        history->committer_timestamp, &state->committer_calendar,
        &state->committer_hour, &state->committer_minute));
    gtk_box_append(GTK_BOX(root), dates);
    GtkWidget *warning = gtk_label_new(
        "Advanced: this changes Git history. Only the current HEAD can be amended here.\n"
        "Already pushed? The normal Push action will reject the rewritten branch.\n"
        "This app does not yet offer a lease-safe force push. Commit dates do not change GitHub push time.");
    gtk_label_set_xalign(GTK_LABEL(warning), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(warning), TRUE);
    gtk_box_append(GTK_BOX(root), warning);
    gtk_box_append(GTK_BOX(root), state->confirmed);
    gtk_label_set_xalign(GTK_LABEL(state->feedback), 0.0f);
    gtk_box_append(GTK_BOX(root), state->feedback);
    gtk_widget_add_css_class(state->submit, "destructive-action");
    gtk_box_append(GTK_BOX(actions), cancel);
    gtk_box_append(GTK_BOX(actions), state->submit);
    gtk_box_append(GTK_BOX(root), actions);
    gtk_window_set_child(GTK_WINDOW(dialog), root);
    g_object_set_data_full(G_OBJECT(dialog), "ghm-rewrite-dialog", state, g_free);
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), dialog);
    g_signal_connect(state->submit, "clicked", G_CALLBACK(rewrite_submit), dialog);
    g_signal_connect(dialog, "close-request", G_CALLBACK(rewrite_close_requested), state);
    gtk_window_present(GTK_WINDOW(dialog));
}

GtkWidget *ghm_history_view_new(GtkWidget *parent, GhmContext *context,
                                GhmHistoryRewritten rewritten)
{
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *refresh = gtk_button_new_with_label("Refresh History");
    GtkWidget *rewrite = gtk_button_new_with_label("Rewrite HEAD…");
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *scroll = gtk_scrolled_window_new();
    HistoryState *state = calloc(1, sizeof(*state));
    if (state == NULL) return root;
    state->parent = parent;
    state->context = context;
    state->rewritten = rewritten;
    state->rewrite_button = rewrite;
    state->list = gtk_list_box_new();
    state->detail = gtk_label_new("Select a repository to view history.");
    gtk_label_set_xalign(GTK_LABEL(state->detail), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(state->detail), TRUE);
    gtk_widget_set_halign(refresh, GTK_ALIGN_START);
    gtk_widget_set_sensitive(rewrite, FALSE);
    gtk_box_append(GTK_BOX(actions), refresh);
    gtk_box_append(GTK_BOX(actions), rewrite);
    gtk_box_append(GTK_BOX(root), actions);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), state->list);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(root), scroll);
    gtk_box_append(GTK_BOX(root), state->detail);
    g_object_set_data_full(G_OBJECT(root), "ghm-history-state", state, history_state_free);
    g_signal_connect(state->list, "row-selected", G_CALLBACK(history_selected), state);
    g_signal_connect(refresh, "clicked", G_CALLBACK(refresh_clicked), root);
    g_signal_connect(rewrite, "clicked", G_CALLBACK(rewrite_clicked), root);
    return root;
}
