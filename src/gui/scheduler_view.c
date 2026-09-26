#include "gui/dialog.h"
#include "gui/scheduler_view.h"

#include <ghm/scheduler.h>

#include <stdlib.h>
#include <string.h>

typedef struct {
    GtkWidget *parent;
    GhmContext *context;
    char *repository_path;
    GtkWidget *message;
    GtkWidget *commit_calendar;
    GtkWidget *commit_hour;
    GtkWidget *commit_minute;
    GtkWidget *execute_calendar;
    GtkWidget *execute_hour;
    GtkWidget *execute_minute;
    GtkWidget *stage_all;
    GtkWidget *push_after_commit;
    GtkWidget *feedback;
    GtkWidget *submit;
    GtkWidget *cancel;
    gboolean busy;
    char *client_id;
    GhmScheduleAdded added;
} ScheduleState;

typedef struct {
    GhmContext *context;
    char *repository_path;
    char *message;
    int64_t commit_timestamp;
    int64_t execute_at;
    int offset_minutes;
    int stage_all;
    int push_after_commit;
    char *client_id;
    int64_t job_id;
    GhmError error;
} ScheduleTask;

static void schedule_state_free(gpointer data)
{
    ScheduleState *state = data;
    g_object_unref(state->parent);
    free(state->repository_path);
    free(state->client_id);
    free(state);
}

static void schedule_task_free(gpointer data)
{
    ScheduleTask *request = data;
    free(request->repository_path);
    free(request->message);
    free(request->client_id);
    free(request);
}

static int read_date_time(GtkWidget *calendar, GtkWidget *hour, GtkWidget *minute,
                          int64_t *out_timestamp, int *out_offset)
{
    GDateTime *selected = gtk_calendar_get_date(GTK_CALENDAR(calendar));
    GDateTime *date_time;
    if (selected == NULL) return -1;
    date_time = g_date_time_new_local(g_date_time_get_year(selected),
                                      g_date_time_get_month(selected),
                                      g_date_time_get_day_of_month(selected),
                                      gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(hour)),
                                      gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(minute)), 0.0);
    g_date_time_unref(selected);
    if (date_time == NULL) return -1;
    *out_timestamp = g_date_time_to_unix(date_time);
    *out_offset = (int)(g_date_time_get_utc_offset(date_time) / G_TIME_SPAN_MINUTE);
    g_date_time_unref(date_time);
    return 0;
}

static void schedule_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
    ScheduleTask *request = task_data;
    char *name = NULL;
    char *email = NULL;
    (void)source;
    (void)cancellable;
    if (ghm_commit_default_identity(request->repository_path, &name, &email, &request->error) == 0) {
        GhmCommitSignature signature = {name, email, request->commit_timestamp, request->offset_minutes};
        if (!request->push_after_commit || request->client_id == NULL ||
            ghm_setting_set(request->context, "github_client_id", request->client_id,
                            &request->error) == 0)
            (void)ghm_schedule_add_options(request->context, request->repository_path,
                                           request->message, &signature, &signature,
                                           request->execute_at, request->stage_all,
                                           request->push_after_commit,
                                           &request->job_id, &request->error);
    }
    free(name);
    free(email);
    g_task_return_boolean(task, request->job_id > 0);
}

static void schedule_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *dialog = GTK_WIDGET(source);
    ScheduleState *state = g_object_get_data(G_OBJECT(dialog), "ghm-schedule-state");
    ScheduleTask *request = g_task_get_task_data(G_TASK(result));
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (!gtk_widget_get_realized(state->parent)) { gtk_window_destroy(GTK_WINDOW(dialog)); return; }
    if (request->job_id > 0) {
        state->busy = FALSE;
        state->added(state->parent, request->job_id);
        gtk_window_destroy(GTK_WINDOW(dialog));
    } else {
        state->busy = FALSE;
        gtk_label_set_text(GTK_LABEL(state->feedback), request->error.message);
        gtk_widget_set_sensitive(state->submit, TRUE);
        gtk_widget_set_sensitive(state->cancel, TRUE);
    }
}

static void submit_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *dialog = user_data;
    ScheduleState *state = g_object_get_data(G_OBJECT(dialog), "ghm-schedule-state");
    ScheduleTask *request;
    GTask *task;
    int execute_offset;
    (void)button;
    if (gtk_editable_get_text(GTK_EDITABLE(state->message))[0] == '\0') {
        gtk_label_set_text(GTK_LABEL(state->feedback), "Enter a commit message.");
        return;
    }
    request = calloc(1, sizeof(*request));
    if (request == NULL) {
        gtk_label_set_text(GTK_LABEL(state->feedback), "Out of memory.");
        return;
    }
    if (read_date_time(state->commit_calendar, state->commit_hour, state->commit_minute,
                       &request->commit_timestamp, &request->offset_minutes) != 0 ||
        read_date_time(state->execute_calendar, state->execute_hour, state->execute_minute,
                       &request->execute_at, &execute_offset) != 0) {
        gtk_label_set_text(GTK_LABEL(state->feedback), "Choose a valid local date and time.");
        free(request);
        return;
    }
    (void)execute_offset;
    GDateTime *now = g_date_time_new_now_local();
    int64_t current_time = g_date_time_to_unix(now);
    g_date_time_unref(now);
    if (request->execute_at <= current_time) {
        gtk_label_set_text(GTK_LABEL(state->feedback), "Execution time must be in the future.");
        free(request);
        return;
    }
    request->context = state->context;
    request->stage_all = gtk_check_button_get_active(GTK_CHECK_BUTTON(state->stage_all));
    request->push_after_commit = gtk_check_button_get_active(GTK_CHECK_BUTTON(state->push_after_commit));
    request->repository_path = strdup(state->repository_path);
    request->message = strdup(gtk_editable_get_text(GTK_EDITABLE(state->message)));
    request->client_id = state->client_id != NULL ? strdup(state->client_id) : NULL;
    if (request->repository_path == NULL || request->message == NULL ||
        (state->client_id != NULL && request->client_id == NULL)) {
        schedule_task_free(request);
        gtk_label_set_text(GTK_LABEL(state->feedback), "Out of memory.");
        return;
    }
    gtk_widget_set_sensitive(state->submit, FALSE);
    gtk_widget_set_sensitive(state->cancel, FALSE);
    state->busy = TRUE;
    gtk_label_set_text(GTK_LABEL(state->feedback), "Freezing the current files…");
    task = g_task_new(dialog, NULL, schedule_finished, NULL);
    g_task_set_task_data(task, request, schedule_task_free);
    g_task_run_in_thread(task, schedule_worker);
    g_object_unref(task);
}

static GtkWidget *date_time_column(const char *title, GDateTime *initial,
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
    gtk_calendar_set_date(GTK_CALENDAR(*calendar), initial);
#else
    gtk_calendar_select_day(GTK_CALENDAR(*calendar), initial);
#endif
    *hour = gtk_spin_button_new_with_range(0.0, 23.0, 1.0);
    *minute = gtk_spin_button_new_with_range(0.0, 59.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(*hour), (double)g_date_time_get_hour(initial));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(*minute), (double)g_date_time_get_minute(initial));
    gtk_box_append(GTK_BOX(clock), gtk_label_new("Time"));
    gtk_box_append(GTK_BOX(clock), *hour);
    gtk_box_append(GTK_BOX(clock), gtk_label_new(":"));
    gtk_box_append(GTK_BOX(clock), *minute);
    gtk_box_append(GTK_BOX(column), heading);
    gtk_box_append(GTK_BOX(column), *calendar);
    gtk_box_append(GTK_BOX(column), clock);
    return column;
}

static void cancel_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    gtk_window_destroy(GTK_WINDOW(user_data));
}

static gboolean close_requested(GtkWindow *window, gpointer user_data)
{
    ScheduleState *state = user_data;
    (void)window;
    return state->busy;
}

void ghm_scheduler_dialog_show(GtkWidget *parent, GhmContext *context,
                               const char *repository_path, const char *client_id,
                               GhmScheduleAdded added)
{
    GtkWidget *dialog = gtk_window_new();
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    GtkWidget *dates = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    GDateTime *now = g_date_time_new_now_local();
    GDateTime *soon = g_date_time_add_minutes(now, 5);
    ScheduleState *state = calloc(1, sizeof(*state));
    if (state == NULL) { gtk_window_destroy(GTK_WINDOW(dialog)); g_date_time_unref(soon); g_date_time_unref(now); return; }
    state->parent = g_object_ref(parent);
    state->context = context;
    state->repository_path = strdup(repository_path);
    state->client_id = client_id != NULL ? strdup(client_id) : NULL;
    state->added = added;
    if (state->repository_path == NULL || (client_id != NULL && state->client_id == NULL)) {
        schedule_state_free(state);
        gtk_window_destroy(GTK_WINDOW(dialog));
        g_date_time_unref(soon); g_date_time_unref(now);
        return;
    }
    state->message = gtk_entry_new();
    state->feedback = gtk_label_new("The selected files are frozen when you schedule; no staging happens at execution.");
    state->stage_all = gtk_check_button_new_with_label("Capture all current changes now (private snapshot)");
    state->push_after_commit = gtk_check_button_new_with_label("Push this commit to origin after it is created");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(state->stage_all), TRUE);
    state->submit = gtk_button_new_with_label("Schedule Commit");
    state->cancel = cancel;
    gtk_widget_add_css_class(state->submit, "suggested-action");
    gtk_window_set_title(GTK_WINDOW(dialog), "Schedule Commit");
    ghm_dialog_attach(dialog, parent);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 740, 560);
    gtk_widget_set_margin_start(root, 20);
    gtk_widget_set_margin_end(root, 20);
    gtk_widget_set_margin_top(root, 20);
    gtk_widget_set_margin_bottom(root, 20);
    gtk_box_append(GTK_BOX(root), gtk_label_new("Commit message"));
    gtk_box_append(GTK_BOX(root), state->message);
    gtk_box_append(GTK_BOX(dates), date_time_column("Commit metadata date and time", now,
                                                   &state->commit_calendar, &state->commit_hour,
                                                   &state->commit_minute));
    gtk_box_append(GTK_BOX(dates), date_time_column("Run the commit at", soon,
                                                   &state->execute_calendar, &state->execute_hour,
                                                   &state->execute_minute));
    gtk_box_append(GTK_BOX(root), dates);
    gtk_box_append(GTK_BOX(root), state->stage_all);
    gtk_box_append(GTK_BOX(root), state->push_after_commit);
    GtkWidget *note = gtk_label_new("The commit date is Git metadata. It does not change GitHub's push time.\n"
                                    "Uncheck to capture only files manually staged in Changes. Later edits cannot enter this job.\n"
                                    "Jobs run while ghm-worker is active. A scheduled push is non-force and targets only this commit.\n"
                                    "GitHub pushes require a valid saved login when the job runs.");
    gtk_label_set_xalign(GTK_LABEL(note), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(note), TRUE);
    gtk_box_append(GTK_BOX(root), note);
    gtk_label_set_xalign(GTK_LABEL(state->feedback), 0.0f);
    gtk_box_append(GTK_BOX(root), state->feedback);
    gtk_widget_set_hexpand(cancel, TRUE);
    gtk_widget_set_halign(cancel, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(actions), cancel);
    gtk_box_append(GTK_BOX(actions), state->submit);
    gtk_box_append(GTK_BOX(root), actions);
    gtk_window_set_child(GTK_WINDOW(dialog), root);
    g_object_set_data_full(G_OBJECT(dialog), "ghm-schedule-state", state, schedule_state_free);
    g_signal_connect(cancel, "clicked", G_CALLBACK(cancel_clicked), dialog);
    g_signal_connect(state->submit, "clicked", G_CALLBACK(submit_clicked), dialog);
    g_signal_connect(dialog, "close-request", G_CALLBACK(close_requested), state);
    gtk_window_present(GTK_WINDOW(dialog));
    g_date_time_unref(soon);
    g_date_time_unref(now);
}
