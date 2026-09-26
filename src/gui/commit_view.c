#include "gui/dialog.h"
#include "gui/commit_view.h"

#include <ghm/commit.h>

#include <stdlib.h>
#include <string.h>

typedef struct {
    GtkWidget *parent;
    char *path;
    GtkWidget *message;
    GtkWidget *author_calendar;
    GtkWidget *author_hour;
    GtkWidget *author_minute;
    GtkWidget *committer_calendar;
    GtkWidget *committer_hour;
    GtkWidget *committer_minute;
    GtkWidget *feedback;
    GtkWidget *staged_list;
    GtkWidget *submit;
    GtkWidget *cancel;
    gboolean busy;
    GhmCommitCreated created;
} CommitState;

typedef struct {
    char *path;
    char *message;
    int64_t author_timestamp;
    int64_t committer_timestamp;
    int author_offset;
    int committer_offset;
    char oid[GHM_OID_HEX_CAPACITY];
    GhmError error;
    gboolean succeeded;
} CommitTask;

typedef struct {
    char *path;
    GhmStatus status;
    GhmError error;
    gboolean succeeded;
} StagedTask;

static void state_free(gpointer data)
{
    CommitState *state = data;
    g_object_unref(state->parent);
    free(state->path);
    free(state);
}

static void task_free(gpointer data)
{
    CommitTask *request = data;
    free(request->path);
    free(request->message);
    free(request);
}

static void staged_task_free(gpointer data)
{
    StagedTask *request = data;
    free(request->path);
    ghm_status_free(&request->status);
    free(request);
}

static void staged_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
    StagedTask *request = task_data;
    (void)source;
    (void)cancellable;
    request->succeeded = ghm_repo_status(request->path, &request->status, &request->error) == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void staged_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *dialog = GTK_WIDGET(source);
    CommitState *state = g_object_get_data(G_OBJECT(dialog), "ghm-commit-state");
    StagedTask *request = g_task_get_task_data(G_TASK(result));
    size_t count = 0;
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (!gtk_widget_get_realized(state->parent)) return;
    if (!request->succeeded) {
        gtk_label_set_text(GTK_LABEL(state->feedback), request->error.message);
        return;
    }
    GtkWidget *old_row;
    while ((old_row = gtk_widget_get_first_child(state->staged_list)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(state->staged_list), old_row);
    for (size_t i = 0; i < request->status.count; ++i) {
        const GhmStatusEntry *entry = &request->status.items[i];
        if (!entry->staged) continue;
        char *text = g_strdup_printf("%s  %s", ghm_status_kind_label(entry->kind), entry->path);
        GtkWidget *label = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_list_box_append(GTK_LIST_BOX(state->staged_list), label);
        g_free(text);
        ++count;
    }
    if (count == 0) {
        gtk_list_box_append(GTK_LIST_BOX(state->staged_list),
                            gtk_label_new("No staged files. Stage them in Changes first."));
        gtk_label_set_text(GTK_LABEL(state->feedback), "Nothing staged to commit.");
    } else gtk_widget_set_sensitive(state->submit, TRUE);
}

static int read_date_time(GtkWidget *calendar, GtkWidget *hour, GtkWidget *minute,
                          int64_t *timestamp, int *offset)
{
    GDateTime *date = gtk_calendar_get_date(GTK_CALENDAR(calendar));
    GDateTime *chosen;
    if (date == NULL) return -1;
    chosen = g_date_time_new_local(g_date_time_get_year(date), g_date_time_get_month(date),
                                    g_date_time_get_day_of_month(date),
                                    gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(hour)),
                                    gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(minute)), 0.0);
    g_date_time_unref(date);
    if (chosen == NULL) return -1;
    *timestamp = g_date_time_to_unix(chosen);
    *offset = (int)(g_date_time_get_utc_offset(chosen) / G_TIME_SPAN_MINUTE);
    g_date_time_unref(chosen);
    return 0;
}

static GtkWidget *time_column(const char *title, GDateTime *now,
                               GtkWidget **calendar, GtkWidget **hour,
                               GtkWidget **minute)
{
    GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *clock = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *heading = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
    gtk_widget_add_css_class(heading, "heading");
    *calendar = gtk_calendar_new();
#if GTK_CHECK_VERSION(4, 20, 0)
    gtk_calendar_set_date(GTK_CALENDAR(*calendar), now);
#else
    gtk_calendar_select_day(GTK_CALENDAR(*calendar), now);
#endif
    *hour = gtk_spin_button_new_with_range(0.0, 23.0, 1.0);
    *minute = gtk_spin_button_new_with_range(0.0, 59.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(*hour), (double)g_date_time_get_hour(now));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(*minute), (double)g_date_time_get_minute(now));
    gtk_box_append(GTK_BOX(clock), gtk_label_new("Time"));
    gtk_box_append(GTK_BOX(clock), *hour);
    gtk_box_append(GTK_BOX(clock), gtk_label_new(":"));
    gtk_box_append(GTK_BOX(clock), *minute);
    gtk_box_append(GTK_BOX(column), heading);
    gtk_box_append(GTK_BOX(column), *calendar);
    gtk_box_append(GTK_BOX(column), clock);
    return column;
}

static void commit_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
    CommitTask *request = task_data;
    char *name = NULL;
    char *email = NULL;
    (void)source;
    (void)cancellable;
    if (ghm_commit_default_identity(request->path, &name, &email, &request->error) == 0) {
        GhmCommitSignature author = {name, email, request->author_timestamp, request->author_offset};
        GhmCommitSignature committer = {name, email, request->committer_timestamp,
                                        request->committer_offset};
        request->succeeded = ghm_commit_staged(request->path, request->message,
                                               &author, &committer, request->oid,
                                               &request->error) == 0;
    }
    free(name);
    free(email);
    g_task_return_boolean(task, request->succeeded);
}

static void commit_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *dialog = GTK_WIDGET(source);
    CommitState *state = g_object_get_data(G_OBJECT(dialog), "ghm-commit-state");
    CommitTask *request = g_task_get_task_data(G_TASK(result));
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    state->busy = FALSE;
    if (!gtk_widget_get_realized(state->parent)) { gtk_window_destroy(GTK_WINDOW(dialog)); return; }
    if (request->succeeded) {
        state->created(state->parent, request->oid);
        gtk_window_destroy(GTK_WINDOW(dialog));
    } else {
        char *message = request->oid[0] != '\0' ?
            g_strdup_printf("%s\nCreated SHA: %s", request->error.message, request->oid) :
            g_strdup(request->error.message);
        gtk_label_set_text(GTK_LABEL(state->feedback), message);
        g_free(message);
        gtk_widget_set_sensitive(state->submit, TRUE);
        gtk_widget_set_sensitive(state->cancel, TRUE);
    }
}

static void submit_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *dialog = user_data;
    CommitState *state = g_object_get_data(G_OBJECT(dialog), "ghm-commit-state");
    CommitTask *request;
    GTask *task;
    (void)button;
    if (gtk_editable_get_text(GTK_EDITABLE(state->message))[0] == '\0') {
        gtk_label_set_text(GTK_LABEL(state->feedback), "Enter a commit message.");
        return;
    }
    request = calloc(1, sizeof(*request));
    if (request == NULL) return;
    if (read_date_time(state->author_calendar, state->author_hour, state->author_minute,
                       &request->author_timestamp, &request->author_offset) != 0 ||
        read_date_time(state->committer_calendar, state->committer_hour, state->committer_minute,
                       &request->committer_timestamp, &request->committer_offset) != 0) {
        gtk_label_set_text(GTK_LABEL(state->feedback), "Choose valid local dates and times.");
        free(request);
        return;
    }
    request->path = strdup(state->path);
    request->message = strdup(gtk_editable_get_text(GTK_EDITABLE(state->message)));
    if (request->path == NULL || request->message == NULL) {
        task_free(request);
        gtk_label_set_text(GTK_LABEL(state->feedback), "Out of memory.");
        return;
    }
    state->busy = TRUE;
    gtk_widget_set_sensitive(state->submit, FALSE);
    gtk_widget_set_sensitive(state->cancel, FALSE);
    gtk_label_set_text(GTK_LABEL(state->feedback), "Creating local commit…");
    task = g_task_new(dialog, NULL, commit_finished, NULL);
    g_task_set_task_data(task, request, task_free);
    g_task_run_in_thread(task, commit_worker);
    g_object_unref(task);
}

static gboolean close_requested(GtkWindow *dialog, gpointer user_data)
{
    CommitState *state = user_data;
    (void)dialog;
    return state->busy;
}

static void cancel_clicked(GtkButton *button, gpointer user_data)
{
    CommitState *state = g_object_get_data(G_OBJECT(user_data), "ghm-commit-state");
    (void)button;
    if (!state->busy) gtk_window_destroy(GTK_WINDOW(user_data));
}

void ghm_commit_dialog_show(GtkWidget *parent, const char *repository_path,
                            GhmCommitCreated created)
{
    GtkWidget *dialog = gtk_window_new();
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    GtkWidget *dates = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *staged_scroll = gtk_scrolled_window_new();
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    GDateTime *now = g_date_time_new_now_local();
    CommitState *state = calloc(1, sizeof(*state));
    if (state == NULL) { gtk_window_destroy(GTK_WINDOW(dialog)); g_date_time_unref(now); return; }
    state->parent = g_object_ref(parent);
    state->path = strdup(repository_path);
    if (state->path == NULL) {
        state_free(state);
        gtk_window_destroy(GTK_WINDOW(dialog));
        g_date_time_unref(now);
        return;
    }
    state->created = created;
    state->message = gtk_entry_new();
    state->staged_list = gtk_list_box_new();
    state->feedback = gtk_label_new("Only staged files will be committed. Use Stage in the Changes tab first.");
    state->submit = gtk_button_new_with_label("Create Commit");
    gtk_widget_set_sensitive(state->submit, FALSE);
    state->cancel = cancel;
    gtk_widget_add_css_class(state->submit, "suggested-action");
    gtk_window_set_title(GTK_WINDOW(dialog), "Commit Staged Changes");
    ghm_dialog_attach(dialog, parent);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 740, 650);
    gtk_widget_set_margin_start(root, 20);
    gtk_widget_set_margin_end(root, 20);
    gtk_widget_set_margin_top(root, 20);
    gtk_widget_set_margin_bottom(root, 20);
    gtk_box_append(GTK_BOX(root), gtk_label_new("Commit message"));
    gtk_box_append(GTK_BOX(root), state->message);
    GtkWidget *staged_heading = gtk_label_new("Staged files included in this commit");
    gtk_label_set_xalign(GTK_LABEL(staged_heading), 0.0f);
    gtk_box_append(GTK_BOX(root), staged_heading);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(staged_scroll), state->staged_list);
    gtk_widget_set_size_request(staged_scroll, -1, 95);
    gtk_list_box_append(GTK_LIST_BOX(state->staged_list), gtk_label_new("Loading staged files…"));
    gtk_box_append(GTK_BOX(root), staged_scroll);
    gtk_box_append(GTK_BOX(dates), time_column("Author date and time", now,
                                              &state->author_calendar, &state->author_hour,
                                              &state->author_minute));
    gtk_box_append(GTK_BOX(dates), time_column("Committer date and time", now,
                                              &state->committer_calendar, &state->committer_hour,
                                              &state->committer_minute));
    gtk_box_append(GTK_BOX(root), dates);
    GtkWidget *note = gtk_label_new("These dates are stored in Git metadata, not GitHub push time.\n"
                                    "Create Commit runs now, even if a future metadata date is selected.\n"
                                    "For later execution, use Schedule Commit in the Changes tab.\n"
                                    "Committing now advances the branch; any pending scheduled jobs on it may fail.");
    gtk_label_set_xalign(GTK_LABEL(note), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(note), TRUE);
    gtk_box_append(GTK_BOX(root), note);
    gtk_label_set_xalign(GTK_LABEL(state->feedback), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(state->feedback), TRUE);
    gtk_box_append(GTK_BOX(root), state->feedback);
    gtk_widget_set_hexpand(cancel, TRUE);
    gtk_widget_set_halign(cancel, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(actions), cancel);
    gtk_box_append(GTK_BOX(actions), state->submit);
    gtk_box_append(GTK_BOX(root), actions);
    gtk_window_set_child(GTK_WINDOW(dialog), root);
    g_object_set_data_full(G_OBJECT(dialog), "ghm-commit-state", state, state_free);
    g_signal_connect(dialog, "close-request", G_CALLBACK(close_requested), state);
    g_signal_connect(cancel, "clicked", G_CALLBACK(cancel_clicked), dialog);
    g_signal_connect(state->submit, "clicked", G_CALLBACK(submit_clicked), dialog);
    StagedTask *request = calloc(1, sizeof(*request));
    if (request != NULL) {
        request->path = strdup(state->path);
        if (request->path != NULL) {
            GTask *task = g_task_new(dialog, NULL, staged_finished, NULL);
            g_task_set_task_data(task, request, staged_task_free);
            g_task_run_in_thread(task, staged_worker);
            g_object_unref(task);
        } else staged_task_free(request);
    }
    gtk_window_present(GTK_WINDOW(dialog));
    g_date_time_unref(now);
}
