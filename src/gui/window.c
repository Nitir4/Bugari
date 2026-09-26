#include "gui/dialog.h"
#include "gui/window.h"
#include "gui/github_repos_view.h"
#include "gui/commit_view.h"
#include "gui/branch_view.h"
#include "gui/file_view.h"
#include "gui/history_view.h"
#include "gui/login_view.h"
#include "gui/scheduler_view.h"
#include "gui/settings_view.h"

#include <ghm/scheduler.h>
#include <ghm/remote.h>
#include <ghm/diff.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    GhmContext *context;
    GtkWidget *repository_list;
    GtkWidget *changes_list;
    GtkWidget *diff_view;
    GtkWidget *diff_title;
    GtkWidget *file_view;
    GtkWidget *history_view;
    GtkWidget *branch_view;
    GtkWidget *jobs_list;
    GtkWidget *repository_title;
    GtkWidget *branch_label;
    GtkWidget *message_label;
    GtkWidget *warning_box;
    GtkWidget *warning_label;
    GFileMonitor *job_monitor;
    GCancellable *folder_cancellable;
    guint job_refresh_source;
    gboolean closed;
    gboolean restoring_repository;
    GtkWidget *main_stack;
    GtkWidget *view_stack;
    GtkWidget *sidebar;
    GtkWidget *view_title;
    GtkWidget *status_branch;
    GtkWidget *status_changes;
    GtkWidget *activity_buttons[6];
    GtkWidget *add_button;
    GtkWidget *account_button;
    GtkWidget *github_button;
    GtkWidget *schedule_button;
    GtkWidget *commit_button;
    GtkWidget *stage_all_button;
    GtkWidget *push_button;
    GtkWidget *fetch_button;
    GtkWidget *pull_button;
    char *selected_path;
    char *pending_notice;
    char *editor_notice;
    unsigned long status_generation;
    unsigned long diff_generation;
    unsigned long jobs_generation;
} WindowState;

typedef struct {
    char *path;
    GhmStatus status;
    GhmError error;
    gboolean succeeded;
    unsigned long generation;
} StatusTask;

typedef struct {
    char *repository_path;
    char *relative_path;
    char *staged_patch;
    char *unstaged_patch;
    int staged;
    int unstaged;
    unsigned long generation;
    GhmError error;
    gboolean succeeded;
} DiffTask;

typedef struct {
    char *repository_path;
    char *relative_path;
    int action;
    GhmError error;
    gboolean succeeded;
} StageTask;

typedef struct {
    GhmError error;
    gboolean succeeded;
} DiscoveryTask;

typedef struct {
    char *path;
    GhmError error;
    gboolean succeeded;
} RegisterTask;

typedef struct {
    char *path;
    GhmScheduledJobList jobs;
    unsigned long generation;
    GhmError error;
    gboolean succeeded;
} JobsTask;

typedef struct {
    char *path;
    char *client_id;
    char oid[GHM_OID_HEX_CAPACITY];
    GhmError error;
    gboolean succeeded;
} PushTask;

typedef struct {
    GhmContext *context;
    char *path;
    char *client_id;
    gboolean pull;
    gboolean succeeded;
    GhmPullOutcome outcome;
    GhmError error;
} SyncTask;

typedef struct {
    GhmContext *context;
    int64_t job_id;
    int64_t execute_at;
    int action; /* 0=reschedule, 1=cancel, 2=edit, 3=retry push */
    char *message;
    char *client_id;
    int push_after_commit;
    gboolean succeeded;
    GhmError error;
} JobActionTask;

typedef struct {
    GtkWidget *window;
    GtkWidget *calendar;
    GtkWidget *hour;
    GtkWidget *minute;
    GtkWidget *feedback;
    GtkWidget *message_entry;
    GtkWidget *push_check;
    int64_t job_id;
    int action;
} JobDialogState;
static void job_dialog_free(gpointer data)
{
    JobDialogState *state = data;
    g_object_unref(state->window);
    g_free(state);
}

static void stage_clicked(GtkButton *button, gpointer user_data);
static void refresh_jobs(GtkWidget *window);
static void refresh_status(GtkWidget *window);
static void job_action_dialog_show(GtkWidget *window, int64_t job_id,
                                   int64_t execute_at, int action,
                                   const char *message, int push_after_commit);

static const char *const view_names[] = {
    "files", "changes", "history", "branches", "jobs", "settings"
};

static const char *const view_titles[] = {
    "Explorer", "Source Control", "Commit History", "Branches",
    "Scheduled Jobs", "Settings"
};

static void view_selected(GtkToggleButton *button, gpointer user_data)
{
    WindowState *state = g_object_get_data(G_OBJECT(user_data), "ghm-state");
    if (!gtk_toggle_button_get_active(button)) return;
    for (size_t i = 0; i < G_N_ELEMENTS(view_names); ++i) {
        if (GTK_WIDGET(button) != state->activity_buttons[i]) continue;
        gtk_stack_set_visible_child_name(GTK_STACK(state->view_stack), view_names[i]);
        gtk_label_set_text(GTK_LABEL(state->view_title), view_titles[i]);
        break;
    }
}

static GtkWidget *activity_button(const char *icon_name, const char *label)
{
    GtkWidget *button = gtk_toggle_button_new();
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 22);
    gtk_button_set_child(GTK_BUTTON(button), icon);
    gtk_widget_set_tooltip_text(button, label);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   label, -1);
    return button;
}

static gboolean workspace_key_pressed(GtkEventControllerKey *controller,
                                       guint keyval, guint keycode,
                                       GdkModifierType modifiers, gpointer user_data)
{
    WindowState *state = g_object_get_data(G_OBJECT(user_data), "ghm-state");
    (void)controller;
    (void)keycode;
    if ((modifiers & GDK_CONTROL_MASK) == 0 ||
        g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(state->main_stack)), "workspace") != 0)
        return FALSE;
    if (keyval >= GDK_KEY_1 && keyval <= GDK_KEY_6) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->activity_buttons[keyval - GDK_KEY_1]), TRUE);
        return TRUE;
    }
    if (keyval == GDK_KEY_b || keyval == GDK_KEY_B) {
        gtk_widget_set_visible(state->sidebar, !gtk_widget_get_visible(state->sidebar));
        return TRUE;
    }
    if ((modifiers & GDK_SHIFT_MASK) != 0 && (keyval == GDK_KEY_g || keyval == GDK_KEY_G)) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->activity_buttons[1]), TRUE);
        return TRUE;
    }
    return FALSE;
}

static void install_workspace_css(GtkWidget *window)
{
    GdkDisplay *display = gtk_widget_get_display(window);
    if (g_object_get_data(G_OBJECT(display), "ghm-workspace-css") != NULL) return;
    static const char css[] =
        ".ghm-warning { background: #513b12; color: #ffdd88; padding: 8px; border-radius: 4px; }"
        ".ghm-workspace { background: #1e1e1e; color: #d4d4d4; }"
        ".ghm-activity { background: #252526; border-right: 1px solid #343434; }"
        ".ghm-activity button { background: transparent; border: 0; border-radius: 0;"
        " min-width: 48px; min-height: 48px; color: #9d9d9d; box-shadow: none; }"
        ".ghm-activity button:hover { background: #303030; color: #f3f3f3; }"
        ".ghm-activity button:checked { background: #303030; color: #ffffff;"
        " border-left: 3px solid #3794ff; }"
        ".ghm-sidebar { background: #252526; border-right: 1px solid #343434; }"
        ".ghm-sidebar list, .ghm-sidebar row { background: transparent; }"
        ".ghm-sidebar row:selected { background: #37373d; color: #ffffff; }"
        ".ghm-editor { background: #1e1e1e; }"
        ".ghm-editor list, .ghm-editor row { background: #252526; color: #d4d4d4; }"
        ".ghm-editor row:selected { background: #37373d; color: #ffffff; }"
        ".ghm-code, .ghm-code text { background: #1e1e1e; color: #d4d4d4; }"
        ".ghm-code { font-family: monospace; font-size: 13px; }"
        ".ghm-file-tree { background: #252526; }"
        ".ghm-editor button { min-height: 26px; }"
        ".ghm-editor .dim-label { color: #a0a0a0; opacity: 1; }"
        ".ghm-editor-toolbar { background: #252526; border-bottom: 1px solid #343434; }"
        ".ghm-editor-toolbar button { min-height: 26px; padding: 2px 8px; }"
        ".ghm-view-heading { font-size: 11px; font-weight: 600; color: #bfbfbf; }"
        ".ghm-status { background: #007acc; color: #ffffff; min-height: 25px; }"
        ".ghm-status label { color: #ffffff; font-size: 11px; }"
        ".ghm-status-error { background: #a1260d; }";
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_set_data_full(G_OBJECT(display), "ghm-workspace-css", provider, g_object_unref);
}

static void job_action_task_free(gpointer data)
{
    JobActionTask *request = data;
    free(request->message);
    free(request->client_id);
    free(request);
}

static void job_control_clicked(GtkButton *button, gpointer user_data)
{
    int64_t *id = g_object_get_data(G_OBJECT(button), "ghm-job-id");
    int64_t *due = g_object_get_data(G_OBJECT(button), "ghm-job-due");
    int action = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "ghm-job-action"));
    const char *message = g_object_get_data(G_OBJECT(button), "ghm-job-message");
    int push_after_commit = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "ghm-job-push"));
    if (id != NULL && due != NULL)
        job_action_dialog_show(GTK_WIDGET(user_data), *id, *due, action,
                               message, push_after_commit);
}

static void window_state_free(gpointer data)
{
    WindowState *state = data;
    g_clear_object(&state->folder_cancellable);
    if (state->job_monitor != NULL) {
        g_file_monitor_cancel(state->job_monitor);
        g_object_unref(state->job_monitor);
    }
    ghm_context_close(state->context);
    free(state->selected_path);
    g_free(state->pending_notice);
    g_free(state->editor_notice);
    free(state);
}

static void status_task_free(gpointer data)
{
    StatusTask *task = data;
    ghm_status_free(&task->status);
    free(task->path);
    free(task);
}

static void diff_task_free(gpointer data)
{
    DiffTask *request = data;
    free(request->repository_path);
    free(request->relative_path);
    free(request->staged_patch);
    free(request->unstaged_patch);
    free(request);
}

static void diff_set_text(WindowState *state, const char *text)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->diff_view));
    gtk_text_buffer_set_text(buffer, text, -1);
    GtkTextIter line;
    gtk_text_buffer_get_start_iter(buffer, &line);
    do {
        gunichar first = gtk_text_iter_get_char(&line);
        const char *tag = first == '+' ? "diff-added" : first == '-' ? "diff-deleted" :
                          first == '@' ? "diff-hunk" : NULL;
        if (tag != NULL) {
            GtkTextIter end = line;
            gtk_text_iter_forward_to_line_end(&end);
            gtk_text_buffer_apply_tag_by_name(buffer, tag, &line, &end);
        }
    } while (gtk_text_iter_forward_line(&line));
}

static void diff_worker(GTask *task, gpointer source, gpointer task_data,
                        GCancellable *cancellable)
{
    DiffTask *request = task_data;
    (void)source;
    (void)cancellable;
    request->succeeded = (!request->staged ||
        ghm_repo_diff_file(request->repository_path, request->relative_path, 1,
                           &request->staged_patch, &request->error) == 0) &&
        (!request->unstaged ||
         ghm_repo_diff_file(request->repository_path, request->relative_path, 0,
                            &request->unstaged_patch, &request->error) == 0);
    g_task_return_boolean(task, request->succeeded);
}

static void diff_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(source);
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    DiffTask *request = g_task_get_task_data(G_TASK(result));
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (state->closed) return;
    if (request->generation != state->diff_generation ||
        g_strcmp0(request->repository_path, state->selected_path) != 0) return;
    if (!request->succeeded) {
        diff_set_text(state, request->error.message);
        return;
    }
    char *combined = g_strdup_printf("%s%s%s%s%s",
        request->staged ? "STAGED\n" : "",
        request->staged_patch != NULL && request->staged_patch[0] != '\0' ? request->staged_patch :
            request->staged ? "No staged text diff (possibly binary).\n" : "",
        request->staged && request->unstaged ? "\n" : "",
        request->unstaged ? "WORKING TREE\n" : "",
        request->unstaged_patch != NULL && request->unstaged_patch[0] != '\0' ? request->unstaged_patch :
            request->unstaged ? "No working-tree text diff (possibly binary).\n" : "");
    char *valid = g_utf8_make_valid(combined, -1);
    diff_set_text(state, valid);
    g_free(valid);
    g_free(combined);
}

static void changes_selected(GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(user_data);
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    GtkWidget *child = row != NULL ? gtk_list_box_row_get_child(row) : NULL;
    const char *path = child != NULL ? g_object_get_data(G_OBJECT(child), "ghm-diff-path") : NULL;
    DiffTask *request;
    GTask *task;
    (void)list;
    ++state->diff_generation;
    if (path == NULL || state->selected_path == NULL) {
        diff_set_text(state, "Select a changed file to preview its diff.");
        return;
    }
    request = calloc(1, sizeof(*request));
    if (request == NULL) return;
    request->repository_path = strdup(state->selected_path);
    request->relative_path = strdup(path);
    request->staged = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(child), "ghm-diff-staged"));
    request->unstaged = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(child), "ghm-diff-unstaged"));
    request->generation = state->diff_generation;
    if (request->repository_path == NULL || request->relative_path == NULL) {
        diff_task_free(request);
        diff_set_text(state, "Out of memory.");
        return;
    }
    diff_set_text(state, "Loading diff…");
    gtk_label_set_text(GTK_LABEL(state->diff_title), path);
    task = g_task_new(window, NULL, diff_finished, NULL);
    g_task_set_task_data(task, request, diff_task_free);
    g_task_run_in_thread(task, diff_worker);
    g_object_unref(task);
}

static void jobs_task_free(gpointer data)
{
    JobsTask *request = data;
    free(request->path);
    ghm_schedule_list_free(&request->jobs);
    free(request);
}

static void push_task_free(gpointer data)
{
    PushTask *request = data;
    free(request->path);
    free(request->client_id);
    free(request);
}

static void sync_task_free(gpointer data)
{
    SyncTask *request = data;
    free(request->path);
    free(request->client_id);
    free(request);
}

static void stage_task_free(gpointer data)
{
    StageTask *request = data;
    free(request->repository_path);
    free(request->relative_path);
    free(request);
}

static void clear_list(GtkWidget *list)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(list)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(list), child);
}

static void set_message(WindowState *state, const char *message)
{
    if (state->editor_notice != NULL) {
        if (ghm_file_view_has_unsaved(state->file_view)) {
            gtk_label_set_text(GTK_LABEL(state->message_label), state->editor_notice);
            return;
        }
        g_clear_pointer(&state->editor_notice, g_free);
    }
    gtk_label_set_text(GTK_LABEL(state->message_label), message);
}

static void set_editor_guard(WindowState *state, const char *message)
{
    g_free(state->editor_notice);
    state->editor_notice = g_strdup(message);
    set_message(state, message);
}

static void set_warning(WindowState *state, const char *message)
{
    gtk_label_set_text(GTK_LABEL(state->warning_label), message);
    gtk_widget_set_visible(state->warning_box, TRUE);
}

static void dismiss_warning(GtkButton *button, gpointer data)
{
    WindowState *state = data;
    (void)button;
    gtk_widget_set_visible(state->warning_box, FALSE);
}

static void status_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
    StatusTask *request = task_data;
    (void)source;
    (void)cancellable;
    request->succeeded = ghm_repo_status(request->path, &request->status, &request->error) == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void status_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(source);
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    StatusTask *request = g_task_get_task_data(G_TASK(result));
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (state->closed) return;
    if (request == NULL) return;
    if (g_strcmp0(state->selected_path, request->path) != 0 ||
        state->status_generation != request->generation) return;
    clear_list(state->changes_list);
    if (!request->succeeded) {
        gtk_label_set_text(GTK_LABEL(state->branch_label), "Branch unavailable");
        set_message(state, request->error.message);
        return;
    }
    const char *branch_text = request->status.detached ? "detached HEAD" :
        request->status.branch != NULL ? request->status.branch : "unborn branch";
    gtk_label_set_text(GTK_LABEL(state->branch_label), branch_text);
    gtk_label_set_text(GTK_LABEL(state->status_branch), branch_text);
    size_t staged_count = 0;
    for (size_t i = 0; i < request->status.count; ++i)
        if (request->status.items[i].staged) ++staged_count;
    char *counts = g_strdup_printf("%zu changed  ·  %zu staged", request->status.count, staged_count);
    gtk_label_set_text(GTK_LABEL(state->status_changes), counts);
    g_free(counts);
    gtk_widget_set_sensitive(state->commit_button, staged_count > 0);
    if (request->status.count == 0) {
        GtkWidget *empty = gtk_label_new("Working tree clean.\nOpen Explorer to browse your files.");
        gtk_label_set_xalign(GTK_LABEL(empty), 0.0f);
        gtk_label_set_wrap(GTK_LABEL(empty), TRUE);
        gtk_list_box_append(GTK_LIST_BOX(state->changes_list), empty);
    }
    for (size_t i = 0; i < request->status.count; ++i) {
        const GhmStatusEntry *entry = &request->status.items[i];
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget *file_line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *kind = gtk_label_new(ghm_status_kind_label(entry->kind));
        GtkWidget *path = gtk_label_new(entry->path);
        GtkWidget *stage = entry->unstaged ? gtk_button_new_with_label("Stage") : NULL;
        GtkWidget *unstage = entry->staged ? gtk_button_new_with_label("Unstage") : NULL;
        GtkWidget *state_label = gtk_label_new(entry->staged && entry->unstaged ?
                                               "Staged + edited" : entry->staged ? "Staged" : "Unstaged");
        gtk_widget_add_css_class(kind, "status-kind");
        gtk_widget_add_css_class(state_label, "dim-label");
        gtk_widget_set_margin_start(row, 8);
        gtk_widget_set_margin_end(row, 8);
        gtk_widget_set_margin_top(row, 6);
        gtk_widget_set_margin_bottom(row, 6);
        gtk_widget_set_size_request(kind, 24, -1);
        gtk_label_set_xalign(GTK_LABEL(path), 0.0f);
        gtk_widget_set_hexpand(path, TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(path), PANGO_ELLIPSIZE_MIDDLE);
        gtk_box_append(GTK_BOX(file_line), kind);
        gtk_box_append(GTK_BOX(file_line), path);
        gtk_box_append(GTK_BOX(row), file_line);
        gtk_label_set_xalign(GTK_LABEL(state_label), 0.0f);
        gtk_widget_set_hexpand(state_label, TRUE);
        gtk_box_append(GTK_BOX(controls), state_label);
        gtk_box_append(GTK_BOX(row), controls);
        g_object_set_data_full(G_OBJECT(row), "ghm-diff-path", g_strdup(entry->path), g_free);
        g_object_set_data(G_OBJECT(row), "ghm-diff-staged", GINT_TO_POINTER(entry->staged));
        g_object_set_data(G_OBJECT(row), "ghm-diff-unstaged", GINT_TO_POINTER(entry->unstaged));
        if (stage != NULL) {
            g_object_set_data_full(G_OBJECT(stage), "ghm-stage-path", g_strdup(entry->path), g_free);
            g_object_set_data(G_OBJECT(stage), "ghm-stage-action", GINT_TO_POINTER(0));
            g_signal_connect(stage, "clicked", G_CALLBACK(stage_clicked), window);
            gtk_box_append(GTK_BOX(controls), stage);
        }
        if (unstage != NULL) {
            g_object_set_data_full(G_OBJECT(unstage), "ghm-stage-path", g_strdup(entry->path), g_free);
            g_object_set_data(G_OBJECT(unstage), "ghm-stage-action", GINT_TO_POINTER(1));
            g_signal_connect(unstage, "clicked", G_CALLBACK(stage_clicked), window);
            gtk_box_append(GTK_BOX(controls), unstage);
        }
        gtk_list_box_append(GTK_LIST_BOX(state->changes_list), row);
    }
    if (request->status.count == 0) set_message(state, "Working tree clean");
    else {
        char *message = g_strdup_printf("%zu changed file%s", request->status.count,
                                        request->status.count == 1 ? "" : "s");
        set_message(state, message);
        g_free(message);
    }
    if (state->pending_notice != NULL) {
        set_message(state, state->pending_notice);
        g_free(state->pending_notice);
        state->pending_notice = NULL;
    }
}

static void refresh_status(GtkWidget *window)
{
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    StatusTask *request;
    GTask *task;
    if (state->selected_path == NULL) return;
    request = calloc(1, sizeof(*request));
    if (request == NULL) { set_message(state, "Out of memory"); return; }
    request->path = strdup(state->selected_path);
    if (request->path == NULL) { free(request); set_message(state, "Out of memory"); return; }
    set_message(state, "Reading working tree…");
    request->generation = ++state->status_generation;
    ++state->diff_generation;
    diff_set_text(state, "Select a changed file to preview its diff.");
    ghm_file_view_refresh(state->file_view, state->selected_path);
    task = g_task_new(window, NULL, status_finished, NULL);
    g_task_set_task_data(task, request, status_task_free);
    g_task_run_in_thread(task, status_worker);
    g_object_unref(task);
}

static void stage_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
    StageTask *request = task_data;
    (void)source;
    (void)cancellable;
    if (request->action == 2)
        request->succeeded = ghm_repo_stage_all(request->repository_path, &request->error) == 0;
    else if (request->action == 1)
        request->succeeded = ghm_repo_unstage_path(request->repository_path,
                                                    request->relative_path, &request->error) == 0;
    else request->succeeded = ghm_repo_stage_path(request->repository_path,
                                                   request->relative_path, &request->error) == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void stage_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(source);
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    StageTask *request = g_task_get_task_data(G_TASK(result));
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (state->closed) return;
    if (g_strcmp0(state->selected_path, request->repository_path) != 0) return;
    if (!request->succeeded) set_message(state, request->error.message);
    else refresh_status(window);
}

static void stage_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *window = user_data;
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    const char *relative = g_object_get_data(G_OBJECT(button), "ghm-stage-path");
    StageTask *request;
    GTask *task;
    if (state->selected_path == NULL) return;
    if (ghm_file_view_has_unsaved(state->file_view)) {
        set_editor_guard(state, "Save or discard file edits before staging");
        return;
    }
    request = calloc(1, sizeof(*request));
    if (request == NULL) { set_message(state, "Out of memory"); return; }
    request->repository_path = strdup(state->selected_path);
    request->relative_path = relative != NULL ? strdup(relative) : NULL;
    request->action = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "ghm-stage-action"));
    if (request->repository_path == NULL || (relative != NULL && request->relative_path == NULL)) {
        stage_task_free(request);
        set_message(state, "Out of memory");
        return;
    }
    set_message(state, request->action == 1 ? "Unstaging file…" : "Staging files…");
    task = g_task_new(window, NULL, stage_finished, NULL);
    g_task_set_task_data(task, request, stage_task_free);
    g_task_run_in_thread(task, stage_worker);
    g_object_unref(task);
}

static void jobs_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
    GtkWidget *window = GTK_WIDGET(source);
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    JobsTask *request = task_data;
    (void)cancellable;
    request->succeeded = ghm_schedule_list(state->context, request->path,
                                           &request->jobs, &request->error) == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void jobs_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(source);
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    JobsTask *request = g_task_get_task_data(G_TASK(result));
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (state->closed) return;
    if (g_strcmp0(state->selected_path, request->path) != 0 || request->generation != state->jobs_generation) return;
    clear_list(state->jobs_list);
    if (!request->succeeded) { set_message(state, request->error.message); return; }
    for (size_t i = 0; i < request->jobs.count; ++i) {
        const GhmScheduledJob *job = &request->jobs.items[i];
        GDateTime *date = g_date_time_new_from_unix_local(job->execute_at);
        char *formatted = date != NULL ? g_date_time_format(date, "%Y-%m-%d %H:%M") : NULL;
        char *heading = g_strdup_printf("#%" G_GINT64_FORMAT "  %s  %s  %s — %s",
                                        job->id, job->status, formatted != NULL ? formatted : "?",
                                        job->branch, job->message);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        GtkWidget *label = gtk_label_new(heading);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_box_append(GTK_BOX(row), label);
        if (job->push_after_commit) {
            char *push_label_text = g_strdup_printf("Push: %s", job->push_status);
            GtkWidget *push_label = gtk_label_new(push_label_text);
            gtk_label_set_xalign(GTK_LABEL(push_label), 0.0f);
            gtk_box_append(GTK_BOX(row), push_label);
            g_free(push_label_text);
            if (g_strcmp0(job->push_status, "PENDING") == 0 && job->push_retry_at > 0) {
                GDateTime *retry_date = g_date_time_new_from_unix_local(job->push_retry_at);
                char *retry_text = retry_date != NULL ? g_date_time_format(retry_date, "%Y-%m-%d %H:%M:%S %Z") : NULL;
                char *text = g_strdup_printf("Automatic retry: %s (attempts: %d)", retry_text != NULL ? retry_text : "when available", job->push_attempts);
                gtk_box_append(GTK_BOX(row), gtk_label_new(text));
                g_free(text); g_free(retry_text);
                if (retry_date != NULL) g_date_time_unref(retry_date);
            }
        }
        if (job->error_message != NULL && job->error_message[0] != '\0') {
            GtkWidget *error_label = gtk_label_new(job->error_message);
            gtk_label_set_xalign(GTK_LABEL(error_label), 0.0f);
            gtk_label_set_wrap(GTK_LABEL(error_label), TRUE);
            gtk_widget_add_css_class(error_label, "error");
            gtk_box_append(GTK_BOX(row), error_label);
            if (g_strcmp0(job->push_status, "PENDING") == 0 || strstr(job->error_message, "Rate limit") != NULL ||
                strstr(job->error_message, "rate limit") != NULL || strstr(job->error_message, "recovered") != NULL)
                set_warning(state, job->error_message);
        }
        if (g_strcmp0(job->status, "PENDING") == 0) {
            GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            for (int action = 0; action < 3; ++action) {
                GtkWidget *button = gtk_button_new_with_label(
                    action == 0 ? "Reschedule" : action == 1 ? "Cancel Job" : "Edit Job");
                int64_t *id = g_new(int64_t, 1);
                int64_t *due = g_new(int64_t, 1);
                *id = job->id;
                *due = job->execute_at;
                g_object_set_data_full(G_OBJECT(button), "ghm-job-id", id, g_free);
                g_object_set_data_full(G_OBJECT(button), "ghm-job-due", due, g_free);
                g_object_set_data(G_OBJECT(button), "ghm-job-action", GINT_TO_POINTER(action));
                g_object_set_data_full(G_OBJECT(button), "ghm-job-message",
                                       g_strdup(job->message), g_free);
                g_object_set_data(G_OBJECT(button), "ghm-job-push",
                                  GINT_TO_POINTER(job->push_after_commit));
                g_signal_connect(button, "clicked", G_CALLBACK(job_control_clicked), window);
                gtk_box_append(GTK_BOX(controls), button);
            }
            gtk_box_append(GTK_BOX(row), controls);
        }
        if (g_strcmp0(job->status, "COMPLETED") == 0 &&
            g_strcmp0(job->push_status, "FAILED") == 0) {
            GtkWidget *retry = gtk_button_new_with_label("Retry Push");
            int64_t *id = g_new(int64_t, 1);
            int64_t *due = g_new(int64_t, 1);
            *id = job->id;
            *due = job->execute_at;
            g_object_set_data_full(G_OBJECT(retry), "ghm-job-id", id, g_free);
            g_object_set_data_full(G_OBJECT(retry), "ghm-job-due", due, g_free);
            g_object_set_data(G_OBJECT(retry), "ghm-job-action", GINT_TO_POINTER(3));
            g_signal_connect(retry, "clicked", G_CALLBACK(job_control_clicked), window);
            gtk_box_append(GTK_BOX(row), retry);
        }
        gtk_list_box_append(GTK_LIST_BOX(state->jobs_list), row);
        g_free(heading);
        g_free(formatted);
        if (date != NULL) g_date_time_unref(date);
    }
}

static void job_action_worker(GTask *task, gpointer source, gpointer task_data,
                              GCancellable *cancellable)
{
    JobActionTask *request = task_data;
    (void)source;
    (void)cancellable;
    int operation = -1;
    if (request->action == 0)
        operation = ghm_schedule_reschedule(request->context, request->job_id,
                                            request->execute_at, &request->error);
    else if (request->action == 1)
        operation = ghm_schedule_cancel(request->context, request->job_id, &request->error);
    else if (request->action == 2) {
        if (!request->push_after_commit || request->client_id == NULL ||
            ghm_setting_set(request->context, "github_client_id", request->client_id,
                            &request->error) == 0)
            operation = ghm_schedule_edit(request->context, request->job_id,
                                          request->message, request->push_after_commit,
                                          &request->error);
    } else if (request->action == 3)
        operation = ghm_schedule_retry_push(request->context, request->job_id, &request->error);
    request->succeeded = operation == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void job_action_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(source);
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    JobActionTask *request = g_task_get_task_data(G_TASK(result));
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (state->closed) return;
    if (request->succeeded) {
        char *message = g_strdup_printf("Job #%" G_GINT64_FORMAT " %s",
                                        request->job_id,
                                        request->action == 0 ? "rescheduled" :
                                        request->action == 1 ? "cancelled" :
                                        request->action == 2 ? "edited" : "push queued again");
        set_message(state, message);
        g_free(message);
    } else set_message(state, request->error.message);
    refresh_jobs(window);
}

static void job_dialog_submit(GtkButton *button, gpointer user_data)
{
    GtkWidget *dialog = GTK_WIDGET(user_data);
    JobDialogState *state = g_object_get_data(G_OBJECT(dialog), "ghm-job-dialog");
    JobActionTask *request = g_new0(JobActionTask, 1);
    GTask *task;
    (void)button;
    request->context = ((WindowState *)g_object_get_data(G_OBJECT(state->window), "ghm-state"))->context;
    request->job_id = state->job_id;
    request->action = state->action;
    if (state->action == 0) {
        GDateTime *selected = gtk_calendar_get_date(GTK_CALENDAR(state->calendar));
        GDateTime *date_time = selected != NULL ?
            g_date_time_new_local(g_date_time_get_year(selected), g_date_time_get_month(selected),
                                  g_date_time_get_day_of_month(selected),
                                  gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(state->hour)),
                                  gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(state->minute)), 0.0) : NULL;
        if (selected != NULL) g_date_time_unref(selected);
        if (date_time == NULL || g_date_time_to_unix(date_time) <= (gint64)time(NULL)) {
            gtk_label_set_text(GTK_LABEL(state->feedback), "Choose a future local date and time.");
            if (date_time != NULL) g_date_time_unref(date_time);
            job_action_task_free(request);
            return;
        }
        request->execute_at = g_date_time_to_unix(date_time);
        g_date_time_unref(date_time);
    }
    if (state->action == 2) {
        const char *message = gtk_editable_get_text(GTK_EDITABLE(state->message_entry));
        const char *id = ghm_login_view_client_id(state->window);
        if (message[0] == '\0') {
            gtk_label_set_text(GTK_LABEL(state->feedback), "Enter a commit message.");
            job_action_task_free(request);
            return;
        }
        request->message = strdup(message);
        request->client_id = id != NULL ? strdup(id) : NULL;
        request->push_after_commit = gtk_check_button_get_active(GTK_CHECK_BUTTON(state->push_check));
        if (request->message == NULL || (id != NULL && request->client_id == NULL)) {
            gtk_label_set_text(GTK_LABEL(state->feedback), "Out of memory.");
            job_action_task_free(request);
            return;
        }
    }
    task = g_task_new(state->window, NULL, job_action_finished, NULL);
    g_task_set_task_data(task, request, job_action_task_free);
    g_task_run_in_thread(task, job_action_worker);
    g_object_unref(task);
    gtk_window_destroy(GTK_WINDOW(dialog));
}

static void job_action_dialog_show(GtkWidget *window, int64_t job_id,
                                   int64_t execute_at, int action,
                                   const char *message, int push_after_commit)
{
    GtkWidget *dialog = gtk_window_new();
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *back = gtk_button_new_with_label("Back");
    GtkWidget *submit = gtk_button_new_with_label(
        action == 0 ? "Save Time" : action == 1 ? "Cancel Job" :
        action == 2 ? "Save Job" : "Retry Push");
    JobDialogState *state = g_new0(JobDialogState, 1);
    state->window = g_object_ref(window);
    state->job_id = job_id;
    state->action = action;
    state->feedback = gtk_label_new("");
    gtk_window_set_title(GTK_WINDOW(dialog),
                         action == 0 ? "Reschedule Job" : action == 1 ? "Cancel Scheduled Job" :
                         action == 2 ? "Edit Scheduled Job" : "Retry Scheduled Push");
    ghm_dialog_attach(dialog, window);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_widget_set_margin_start(root, 20);
    gtk_widget_set_margin_end(root, 20);
    gtk_widget_set_margin_top(root, 20);
    gtk_widget_set_margin_bottom(root, 20);
    if (action == 1) {
        gtk_box_append(GTK_BOX(root), gtk_label_new(
            "Cancel this pending commit? Its frozen snapshot will be discarded.\n"
            "Later dependent jobs must be cancelled first."));
        gtk_widget_add_css_class(submit, "destructive-action");
    } else if (action == 0) {
        GDateTime *date = g_date_time_new_from_unix_local(execute_at);
        state->calendar = gtk_calendar_new();
        state->hour = gtk_spin_button_new_with_range(0.0, 23.0, 1.0);
        state->minute = gtk_spin_button_new_with_range(0.0, 59.0, 1.0);
        if (date != NULL) {
#if GTK_CHECK_VERSION(4, 20, 0)
            gtk_calendar_set_date(GTK_CALENDAR(state->calendar), date);
#else
            gtk_calendar_select_day(GTK_CALENDAR(state->calendar), date);
#endif
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->hour), (double)g_date_time_get_hour(date));
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->minute), (double)g_date_time_get_minute(date));
            g_date_time_unref(date);
        }
        GtkWidget *clock = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_append(GTK_BOX(root), gtk_label_new("New local execution time"));
        gtk_box_append(GTK_BOX(root), state->calendar);
        gtk_box_append(GTK_BOX(clock), state->hour);
        gtk_box_append(GTK_BOX(clock), gtk_label_new(":"));
        gtk_box_append(GTK_BOX(clock), state->minute);
        gtk_box_append(GTK_BOX(root), clock);
    } else if (action == 2) {
        state->message_entry = gtk_entry_new();
        state->push_check = gtk_check_button_new_with_label("Push this commit after it is created");
        gtk_editable_set_text(GTK_EDITABLE(state->message_entry), message != NULL ? message : "");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(state->push_check), push_after_commit != 0);
        gtk_box_append(GTK_BOX(root), gtk_label_new("Commit message (frozen files do not change)"));
        gtk_box_append(GTK_BOX(root), state->message_entry);
        gtk_box_append(GTK_BOX(root), state->push_check);
    } else {
        GtkWidget *warning = gtk_label_new(
            "Retry the non-force push of this exact commit?\n"
            "If the local branch moved, the push will fail safely.");
        gtk_label_set_wrap(GTK_LABEL(warning), TRUE);
        gtk_box_append(GTK_BOX(root), warning);
    }
    gtk_box_append(GTK_BOX(root), state->feedback);
    gtk_box_append(GTK_BOX(actions), back);
    gtk_box_append(GTK_BOX(actions), submit);
    gtk_box_append(GTK_BOX(root), actions);
    gtk_window_set_child(GTK_WINDOW(dialog), root);
    g_object_set_data_full(G_OBJECT(dialog), "ghm-job-dialog", state, job_dialog_free);
    g_signal_connect_swapped(back, "clicked", G_CALLBACK(gtk_window_destroy), dialog);
    g_signal_connect(submit, "clicked", G_CALLBACK(job_dialog_submit), dialog);
    gtk_window_present(GTK_WINDOW(dialog));
}

static void refresh_jobs(GtkWidget *window)
{
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    JobsTask *request;
    GTask *task;
    if (state->selected_path == NULL) return;
    request = calloc(1, sizeof(*request));
    if (request == NULL) { set_message(state, "Out of memory"); return; }
    request->path = strdup(state->selected_path);
    if (request->path == NULL) { free(request); set_message(state, "Out of memory"); return; }
    request->generation = ++state->jobs_generation;
    task = g_task_new(window, NULL, jobs_finished, NULL);
    g_task_set_task_data(task, request, jobs_task_free);
    g_task_run_in_thread(task, jobs_worker);
    g_object_unref(task);
}

static void repository_selected(GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
    GtkWidget *window = user_data;
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    const char *path;
    (void)list;
    if (row == NULL) return;
    if (state->restoring_repository) return;
    path = g_object_get_data(G_OBJECT(row), "ghm-path");
    if (ghm_file_view_has_unsaved(state->file_view) &&
        g_strcmp0(state->selected_path, path) != 0) {
        for (GtkWidget *candidate = gtk_widget_get_first_child(state->repository_list);
             candidate != NULL; candidate = gtk_widget_get_next_sibling(candidate)) {
            if (g_strcmp0(g_object_get_data(G_OBJECT(candidate), "ghm-path"),
                          state->selected_path) == 0) {
                state->restoring_repository = TRUE;
                gtk_list_box_select_row(GTK_LIST_BOX(state->repository_list),
                                        GTK_LIST_BOX_ROW(candidate));
                state->restoring_repository = FALSE;
                break;
            }
        }
        set_editor_guard(state, "Save or discard file edits before switching repositories");
        return;
    }
    free(state->selected_path);
    state->selected_path = strdup(path);
    gtk_widget_set_sensitive(state->commit_button, FALSE);
    gtk_widget_set_sensitive(state->stage_all_button, state->selected_path != NULL);
    gtk_widget_set_sensitive(state->schedule_button, state->selected_path != NULL);
    gtk_widget_set_sensitive(state->push_button, state->selected_path != NULL);
    gtk_widget_set_sensitive(state->fetch_button, state->selected_path != NULL);
    gtk_widget_set_sensitive(state->pull_button, state->selected_path != NULL);
    gtk_label_set_text(GTK_LABEL(state->repository_title),
                       gtk_label_get_text(GTK_LABEL(gtk_list_box_row_get_child(row))));
    clear_list(state->changes_list);
    clear_list(state->jobs_list);
    refresh_status(window);
    refresh_jobs(window);
    ghm_history_view_refresh(state->history_view, state->selected_path);
    ghm_branch_view_refresh(state->branch_view, state->selected_path);
}

static void refresh_repositories(GtkWidget *window)
{
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    GhmRepositoryList repositories = {0};
    GhmError error = {0};
    GtkListBoxRow *select_row = NULL;
    if (ghm_repo_list(state->context, &repositories, &error) != 0) {
        set_message(state, error.message);
        return;
    }
    clear_list(state->repository_list);
    for (size_t i = 0; i < repositories.count; ++i) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *label = gtk_label_new(repositories.items[i].name);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_set_margin_start(label, 12);
        gtk_widget_set_margin_end(label, 12);
        gtk_widget_set_margin_top(label, 8);
        gtk_widget_set_margin_bottom(label, 8);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        g_object_set_data_full(G_OBJECT(row), "ghm-path", g_strdup(repositories.items[i].path), g_free);
        gtk_list_box_append(GTK_LIST_BOX(state->repository_list), row);
        if (state->selected_path != NULL && strcmp(state->selected_path, repositories.items[i].path) == 0)
            select_row = GTK_LIST_BOX_ROW(row);
        else if (select_row == NULL && i == 0) select_row = GTK_LIST_BOX_ROW(row);
    }
    if (select_row != NULL) gtk_list_box_select_row(GTK_LIST_BOX(state->repository_list), select_row);
    else {
        gtk_widget_set_sensitive(state->schedule_button, FALSE);
        gtk_widget_set_sensitive(state->commit_button, FALSE);
        gtk_widget_set_sensitive(state->stage_all_button, FALSE);
        gtk_widget_set_sensitive(state->push_button, FALSE);
        gtk_widget_set_sensitive(state->fetch_button, FALSE);
        gtk_widget_set_sensitive(state->pull_button, FALSE);
        gtk_label_set_text(GTK_LABEL(state->repository_title), "No repository selected");
        gtk_label_set_text(GTK_LABEL(state->branch_label), "Add a local repository to begin");
        set_message(state, "Ready");
    }
    ghm_repo_list_free(&repositories);
}

static void discovery_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
    GtkWidget *window = GTK_WIDGET(source);
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    DiscoveryTask *request = task_data;
    (void)cancellable;
    request->succeeded = ghm_repo_discover(state->context, &request->error) == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void discovery_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(source);
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    DiscoveryTask *request = user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (state->closed) return;
    if (request->succeeded) refresh_repositories(window);
    else set_message(state, request->error.message);
}

static void register_task_free(gpointer data)
{
    RegisterTask *request = data;
    g_free(request->path);
    g_free(request);
}

static void register_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
    GtkWidget *window = GTK_WIDGET(source);
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    RegisterTask *request = task_data;
    (void)cancellable;
    request->succeeded = ghm_repo_register(state->context, request->path, &request->error) == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void register_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(source);
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    RegisterTask *request = user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (state->closed) return;
    if (request->succeeded) refresh_repositories(window);
    else set_message(state, request->error.message);
}

static void folder_chosen(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    GtkWidget *window = user_data;
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    GError *gtk_error = NULL;
    GFile *file = gtk_file_dialog_select_folder_finish(dialog, result, &gtk_error);
    g_clear_object(&state->folder_cancellable);
    if (!state->closed) gtk_widget_set_sensitive(state->add_button, TRUE);
    if (file != NULL && !state->closed) {
        char *path = g_file_get_path(file);
        if (path != NULL) {
            RegisterTask *request = g_new0(RegisterTask, 1);
            GTask *task = g_task_new(window, NULL, register_finished, request);
            request->path = path;
            g_task_set_task_data(task, request, register_task_free);
            g_task_run_in_thread(task, register_worker);
            g_object_unref(task);
            set_message(state, "Adding repository…");
        } else set_message(state, "Select a local folder");
    }
    g_clear_object(&file);
    g_clear_error(&gtk_error);
    g_object_unref(window);
}

static void add_local_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *window = user_data;
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    GtkFileDialog *dialog = gtk_file_dialog_new();
    (void)button;
    if (state->folder_cancellable != NULL) { g_object_unref(dialog); return; }
    if (ghm_file_view_has_unsaved(state->file_view)) {
        set_editor_guard(state, "Save or discard file edits first");
        g_object_unref(dialog);
        return;
    }
    gtk_file_dialog_set_title(dialog, "Add Local Repository");
    state->folder_cancellable = g_cancellable_new();
    gtk_widget_set_sensitive(state->add_button, FALSE);
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(window), state->folder_cancellable, folder_chosen, g_object_ref(window));
    g_object_unref(dialog);
}

static void refresh_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    refresh_status(GTK_WIDGET(user_data));
}

static void job_added(GtkWidget *window, int64_t job_id)
{
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    char *message = g_strdup_printf("Scheduled job #%" G_GINT64_FORMAT " — snapshot frozen", job_id);
    set_message(state, message);
    g_free(message);
    refresh_status(window);
    refresh_jobs(window);
}

static void refresh_jobs_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    refresh_jobs(GTK_WIDGET(user_data));
}

static void schedule_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *window = user_data;
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    (void)button;
    if (ghm_file_view_has_unsaved(state->file_view)) {
        set_editor_guard(state, "Save or discard file edits before scheduling");
        return;
    }
    if (state->selected_path != NULL)
        ghm_scheduler_dialog_show(window, state->context, state->selected_path,
                                  ghm_login_view_client_id(window), job_added);
}

static void commit_created(GtkWidget *window, const char *oid)
{
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    g_free(state->pending_notice);
    state->pending_notice = g_strdup_printf("Commit created: %.12s", oid);
    if (state->pending_notice != NULL) set_message(state, state->pending_notice);
    refresh_status(window);
    refresh_jobs(window);
    ghm_history_view_refresh(state->history_view, state->selected_path);
}

static void commit_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *window = user_data;
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    (void)button;
    if (ghm_file_view_has_unsaved(state->file_view)) {
        set_editor_guard(state, "Save or discard file edits before committing");
        return;
    }
    if (state->selected_path != NULL)
        ghm_commit_dialog_show(window, state->selected_path, commit_created);
}

static void push_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
    PushTask *request = task_data;
    (void)source;
    (void)cancellable;
    request->succeeded = ghm_repo_push(request->path, request->client_id,
                                       request->oid, &request->error) == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void push_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(source);
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    PushTask *request = g_task_get_task_data(G_TASK(result));
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (state->closed) return;
    if (g_strcmp0(state->selected_path, request->path) != 0) return;
    gtk_widget_set_sensitive(state->push_button, state->selected_path != NULL);
    if (request->succeeded) {
        char *message = g_strdup_printf("Pushed commit %.12s to origin", request->oid);
        set_message(state, message);
        g_free(message);
    } else {
        set_message(state, request->error.message);
        set_warning(state, request->error.message);
    }
}

static void push_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *window = user_data;
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    const char *client_id = ghm_login_view_client_id(window);
    PushTask *request;
    GTask *task;
    (void)button;
    if (state->selected_path == NULL) return;
    request = calloc(1, sizeof(*request));
    if (request == NULL) { set_message(state, "Out of memory"); return; }
    request->path = strdup(state->selected_path);
    request->client_id = client_id != NULL ? strdup(client_id) : NULL;
    if (request->path == NULL || (client_id != NULL && request->client_id == NULL)) {
        push_task_free(request);
        set_message(state, "Out of memory");
        return;
    }
    gtk_widget_set_sensitive(state->push_button, FALSE);
    set_message(state, "Pushing current branch…");
    task = g_task_new(window, NULL, push_finished, NULL);
    g_task_set_task_data(task, request, push_task_free);
    g_task_run_in_thread(task, push_worker);
    g_object_unref(task);
}

static void sync_worker(GTask *task, gpointer source, gpointer task_data,
                        GCancellable *cancellable)
{
    SyncTask *request = task_data;
    (void)source;
    (void)cancellable;
    request->succeeded = (request->pull ?
        ghm_repo_pull(request->context, request->path, request->client_id,
                      &request->outcome, &request->error) :
        ghm_repo_fetch(request->path, request->client_id, &request->error)) == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void sync_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(source);
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    SyncTask *request = g_task_get_task_data(G_TASK(result));
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (state->closed) return;
    gtk_widget_set_sensitive(state->fetch_button, state->selected_path != NULL);
    gtk_widget_set_sensitive(state->pull_button, state->selected_path != NULL);
    if (g_strcmp0(state->selected_path, request->path) != 0) return;
    if (!request->succeeded) set_message(state, request->error.message);
    else if (!request->pull) set_message(state, "Fetched origin; local files unchanged");
    else if (request->outcome == GHM_PULL_FAST_FORWARDED) {
        set_message(state, "Pulled and fast-forwarded the current branch");
        refresh_status(window);
        ghm_file_view_refresh(state->file_view, state->selected_path);
        ghm_history_view_refresh(state->history_view, state->selected_path);
        ghm_branch_view_refresh(state->branch_view, state->selected_path);
    } else if (request->outcome == GHM_PULL_LOCAL_AHEAD)
        set_message(state, "Local branch is ahead of origin; nothing to pull");
    else set_message(state, "Already up to date");
}

static void sync_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(user_data);
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    const char *client_id = ghm_login_view_client_id(window);
    SyncTask *request;
    GTask *task;
    if (state->selected_path == NULL) return;
    if (ghm_file_view_has_unsaved(state->file_view)) {
        set_editor_guard(state, "Save or discard editor changes before syncing");
        return;
    }
    request = calloc(1, sizeof(*request));
    if (request == NULL) { set_message(state, "Out of memory"); return; }
    request->context = state->context;
    request->pull = button == GTK_BUTTON(state->pull_button);
    request->path = strdup(state->selected_path);
    request->client_id = client_id != NULL ? strdup(client_id) : NULL;
    if (request->path == NULL || (client_id != NULL && request->client_id == NULL)) {
        sync_task_free(request);
        set_message(state, "Out of memory");
        return;
    }
    gtk_widget_set_sensitive(state->fetch_button, FALSE);
    gtk_widget_set_sensitive(state->pull_button, FALSE);
    set_message(state, request->pull ? "Fetching and checking fast-forward…" : "Fetching origin…");
    task = g_task_new(window, NULL, sync_finished, NULL);
    g_task_set_task_data(task, request, sync_task_free);
    g_task_run_in_thread(task, sync_worker);
    g_object_unref(task);
}

static void github_repository_opened(GtkWidget *window, const char *path)
{
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    char *selected = strdup(path);
    if (selected == NULL) { set_message(state, "Out of memory"); return; }
    free(state->selected_path);
    state->selected_path = selected;
    refresh_repositories(window);
    set_message(state, "Repository ready locally");
}

static void github_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *window = user_data;
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    const char *client_id = ghm_login_view_client_id(window);
    (void)button;
    if (ghm_file_view_has_unsaved(state->file_view)) {
        set_editor_guard(state, "Save or discard file edits first");
        return;
    }
    if (client_id == NULL) { set_message(state, "Sign in to GitHub first"); return; }
    ghm_github_repos_dialog_show(window, state->context, client_id, github_repository_opened);
}

static void login_ready(GtkWidget *window, const char *login)
{
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    char *label = login != NULL ? g_strdup_printf("@%s", login) : g_strdup("GitHub Account");
    gtk_button_set_label(GTK_BUTTON(state->account_button), label);
    g_free(label);
    gtk_widget_set_visible(state->add_button, TRUE);
    gtk_widget_set_visible(state->github_button, login != NULL);
    gtk_stack_set_visible_child_name(GTK_STACK(state->main_stack), "workspace");
    if (login == NULL) set_message(state, "Local repositories available; GitHub is not connected");
    else set_message(state, "Connected to GitHub");
}

static void account_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(user_data);
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    (void)button;
    if (ghm_file_view_has_unsaved(state->file_view)) {
        set_editor_guard(state, "Save or discard file edits first");
        return;
    }
    gtk_widget_set_visible(state->add_button, FALSE);
    gtk_widget_set_visible(state->github_button, FALSE);
    gtk_stack_set_visible_child_name(GTK_STACK(state->main_stack), "login");
}

static gboolean window_close_requested(GtkWindow *window, gpointer user_data)
{
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    (void)user_data;
    if (!ghm_file_view_has_unsaved(state->file_view)) return FALSE;
    set_editor_guard(state, "Save or discard file edits before closing the window");
    return TRUE;
}

static void window_unrealized(GtkWidget *window, gpointer data)
{
    WindowState *state = data;
    (void)window;
    state->closed = TRUE;
    if (state->folder_cancellable != NULL) g_cancellable_cancel(state->folder_cancellable);
    if (state->job_monitor != NULL) g_file_monitor_cancel(state->job_monitor);
    if (state->job_refresh_source != 0) {
        guint source = state->job_refresh_source;
        state->job_refresh_source = 0;
        g_source_remove(source);
    }
}

static gboolean refresh_changed_jobs(gpointer data)
{
    GtkWidget *window = data;
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    state->job_refresh_source = 0;
    if (!state->closed) refresh_jobs(window);
    return G_SOURCE_REMOVE;
}

static void jobs_database_changed(GFileMonitor *monitor, GFile *file, GFile *other,
                                   GFileMonitorEvent event, gpointer data)
{
    GtkWidget *window = data;
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    (void)monitor; (void)other; (void)event;
    char *name = g_file_get_basename(file);
    int database = name != NULL && g_str_has_prefix(name, "ghm.db");
    g_free(name);
    if (!database || state->closed || state->job_refresh_source != 0) return;
    state->job_refresh_source = g_timeout_add_full(G_PRIORITY_DEFAULT, 150,
        refresh_changed_jobs, g_object_ref(window), g_object_unref);
}

static gboolean branch_can_switch(GtkWidget *window)
{
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    if (ghm_file_view_has_unsaved(state->file_view)) {
        set_editor_guard(state, "Save or discard editor changes before switching branches");
        return FALSE;
    }
    return TRUE;
}

static void branch_changed(GtkWidget *window)
{
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    refresh_status(window);
    ghm_history_view_refresh(state->history_view, state->selected_path);
}

static void history_rewritten(GtkWidget *window)
{
    WindowState *state = g_object_get_data(G_OBJECT(window), "ghm-state");
    refresh_status(window);
    refresh_jobs(window);
    ghm_history_view_refresh(state->history_view, state->selected_path);
}

GtkWidget *ghm_window_new(GtkApplication *application, GhmContext *context)
{
    GtkSettings *settings = gtk_settings_get_default();
    if (settings != NULL) {
        g_object_set(settings, "gtk-theme-name", "Adwaita",
                     "gtk-application-prefer-dark-theme", TRUE, NULL);
    }
    GtkWidget *window = gtk_application_window_new(application);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *workspace = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *stack = gtk_stack_new();
    GtkWidget *header = gtk_header_bar_new();
    GtkWidget *workbench = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *activity = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *body = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *main_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *editor_toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *sidebar_scroll = gtk_scrolled_window_new();
    GtkWidget *changes_scroll = gtk_scrolled_window_new();
    GtkWidget *diff_scroll = gtk_scrolled_window_new();
    GtkWidget *view_stack = gtk_stack_new();
    GtkWidget *changes_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *changes_split = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *diff_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *jobs_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *sync_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *jobs_scroll = gtk_scrolled_window_new();
    GtkWidget *jobs_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *add_button = gtk_button_new_with_label("Add Local");
    GtkWidget *account_button = gtk_button_new_with_label("GitHub Account");
    GtkWidget *github_button = gtk_button_new_with_label("GitHub Repositories");
    GtkWidget *refresh_button = gtk_button_new_with_label("Refresh Status");
    GtkWidget *stage_all_button = gtk_button_new_with_label("Stage All");
    GtkWidget *schedule_button = gtk_button_new_with_label("Schedule Commit");
    GtkWidget *commit_button = gtk_button_new_with_label("Commit Staged");
    GtkWidget *push_button = gtk_button_new_with_label("Push");
    GtkWidget *fetch_button = gtk_button_new_with_label("Fetch");
    GtkWidget *pull_button = gtk_button_new_with_label("Pull");
    GtkWidget *refresh_jobs_button = gtk_button_new_with_label("Refresh Jobs");
    WindowState *state = calloc(1, sizeof(*state));
    GTask *discovery;
    DiscoveryTask *discovery_request;
    if (state == NULL) abort();
    state->context = context;
    state->repository_list = gtk_list_box_new();
    state->changes_list = gtk_list_box_new();
    state->diff_view = gtk_text_view_new();
    gtk_widget_add_css_class(state->diff_view, "ghm-code");
    GtkTextBuffer *diff_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->diff_view));
    gtk_text_buffer_create_tag(diff_buffer, "diff-added", "foreground", "#8ec07c", NULL);
    gtk_text_buffer_create_tag(diff_buffer, "diff-deleted", "foreground", "#f48771", NULL);
    gtk_text_buffer_create_tag(diff_buffer, "diff-hunk", "foreground", "#75beff", NULL);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(state->diff_view), 12);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(state->diff_view), 8);
    state->diff_title = gtk_label_new("Select a file to inspect its diff");
    state->file_view = ghm_file_view_new(window, refresh_status);
    state->history_view = ghm_history_view_new(window, context, history_rewritten);
    state->branch_view = ghm_branch_view_new(window, context, branch_can_switch, branch_changed);
    state->jobs_list = gtk_list_box_new();
    state->repository_title = gtk_label_new("No repository selected");
    state->branch_label = gtk_label_new("Add a local repository to begin");
    state->message_label = gtk_label_new("Ready");
    state->warning_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    state->warning_label = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(state->warning_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(state->warning_label), 0.0f);
    gtk_widget_set_hexpand(state->warning_label, TRUE);
    gtk_widget_add_css_class(state->warning_box, "ghm-warning");
    gtk_widget_set_margin_start(state->warning_box, 12);
    gtk_widget_set_margin_end(state->warning_box, 12);
    GtkWidget *dismiss = gtk_button_new_with_label("Dismiss warning");
    gtk_box_append(GTK_BOX(state->warning_box), state->warning_label);
    gtk_box_append(GTK_BOX(state->warning_box), dismiss);
    g_signal_connect(dismiss, "clicked", G_CALLBACK(dismiss_warning), state);
    gtk_widget_set_visible(state->warning_box, FALSE);
    state->main_stack = stack;
    state->view_stack = view_stack;
    state->sidebar = sidebar;
    state->view_title = gtk_label_new("Explorer");
    state->status_branch = gtk_label_new("No branch");
    state->status_changes = gtk_label_new("0 changes");
    state->add_button = add_button;
    state->account_button = account_button;
    state->github_button = github_button;
    state->schedule_button = schedule_button;
    state->commit_button = commit_button;
    state->stage_all_button = stage_all_button;
    state->push_button = push_button;
    state->fetch_button = fetch_button;
    state->pull_button = pull_button;
    gtk_widget_set_sensitive(schedule_button, FALSE);
    gtk_widget_set_sensitive(commit_button, FALSE);
    gtk_widget_set_sensitive(stage_all_button, FALSE);
    gtk_widget_set_sensitive(push_button, FALSE);
    gtk_widget_set_sensitive(fetch_button, FALSE);
    gtk_widget_set_sensitive(pull_button, FALSE);
    g_object_set_data_full(G_OBJECT(window), "ghm-state", state, window_state_free);

    gtk_window_set_title(GTK_WINDOW(window), "GitHub Commit Manager");
    gtk_window_set_default_size(GTK_WINDOW(window), 1240, 760);
    install_workspace_css(window);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), gtk_label_new("GitHub Commit Manager"));
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), add_button);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), github_button);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), account_button);
    gtk_widget_set_visible(add_button, FALSE);
    gtk_widget_set_visible(github_button, FALSE);
    gtk_window_set_titlebar(GTK_WINDOW(window), header);
    gtk_widget_add_css_class(workspace, "ghm-workspace");
    gtk_widget_add_css_class(activity, "ghm-activity");
    gtk_widget_add_css_class(sidebar, "ghm-sidebar");
    gtk_widget_add_css_class(main_area, "ghm-editor");
    gtk_widget_add_css_class(editor_toolbar, "ghm-editor-toolbar");
    gtk_widget_set_size_request(activity, 52, -1);
    gtk_widget_set_size_request(sidebar, 180, -1);
    gtk_widget_set_margin_start(sidebar, 10);
    gtk_widget_set_margin_end(sidebar, 10);
    gtk_widget_set_margin_top(sidebar, 14);
    GtkWidget *activity_spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    const char *const activity_icons[] = {
        "folder-symbolic", "view-list-symbolic", "document-open-recent-symbolic",
        "network-workgroup-symbolic", "appointment-soon-symbolic",
        "preferences-system-symbolic"
    };
    for (size_t i = 0; i < G_N_ELEMENTS(view_names); ++i) {
        state->activity_buttons[i] = activity_button(activity_icons[i], view_titles[i]);
        if (i > 0)
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(state->activity_buttons[i]),
                                        GTK_TOGGLE_BUTTON(state->activity_buttons[0]));
        g_signal_connect(state->activity_buttons[i], "toggled", G_CALLBACK(view_selected), window);
        if (i == G_N_ELEMENTS(view_names) - 1) {
            gtk_widget_set_vexpand(activity_spacer, TRUE);
            gtk_box_append(GTK_BOX(activity), activity_spacer);
        }
        gtk_box_append(GTK_BOX(activity), state->activity_buttons[i]);
    }
    gtk_box_append(GTK_BOX(workbench), activity);
    GtkWidget *sidebar_title = gtk_label_new("EXPLORER");
    gtk_label_set_xalign(GTK_LABEL(sidebar_title), 0.0f);
    gtk_widget_add_css_class(sidebar_title, "ghm-view-heading");
    gtk_box_append(GTK_BOX(sidebar), sidebar_title);
    GtkWidget *sidebar_hint = gtk_label_new("REPOSITORIES");
    gtk_label_set_xalign(GTK_LABEL(sidebar_hint), 0.0f);
    gtk_widget_add_css_class(sidebar_hint, "dim-label");
    gtk_box_append(GTK_BOX(sidebar), sidebar_hint);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar_scroll), state->repository_list);
    gtk_widget_set_vexpand(sidebar_scroll, TRUE);
    gtk_box_append(GTK_BOX(sidebar), sidebar_scroll);
    gtk_paned_set_start_child(GTK_PANED(body), sidebar);
    gtk_paned_set_resize_start_child(GTK_PANED(body), FALSE);
    gtk_paned_set_position(GTK_PANED(body), 250);
    gtk_widget_set_hexpand(body, TRUE);
    gtk_box_append(GTK_BOX(workbench), body);
    gtk_widget_set_margin_start(editor_toolbar, 14);
    gtk_widget_set_margin_end(editor_toolbar, 14);
    gtk_widget_set_margin_top(editor_toolbar, 6);
    gtk_widget_set_margin_bottom(editor_toolbar, 6);
    gtk_widget_add_css_class(state->view_title, "ghm-view-heading");
    gtk_label_set_xalign(GTK_LABEL(state->view_title), 0.0f);
    gtk_box_append(GTK_BOX(editor_toolbar), state->view_title);
    gtk_box_append(GTK_BOX(editor_toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_label_set_xalign(GTK_LABEL(state->repository_title), 0.0f);
    gtk_widget_set_hexpand(state->repository_title, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(state->repository_title), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(editor_toolbar), state->repository_title);
    gtk_label_set_xalign(GTK_LABEL(state->branch_label), 0.0f);
    gtk_widget_add_css_class(state->branch_label, "dim-label");
    gtk_box_append(GTK_BOX(editor_toolbar), state->branch_label);
    gtk_box_append(GTK_BOX(main_area), editor_toolbar);
    gtk_box_append(GTK_BOX(main_area), state->warning_box);
    GtkWidget *working_label = gtk_label_new("Working Tree");
    gtk_label_set_xalign(GTK_LABEL(working_label), 0.0f);
    gtk_widget_add_css_class(working_label, "heading");
    gtk_box_append(GTK_BOX(changes_page), working_label);
    gtk_box_append(GTK_BOX(actions), refresh_button);
    gtk_box_append(GTK_BOX(actions), stage_all_button);
    gtk_box_append(GTK_BOX(actions), commit_button);
    gtk_box_append(GTK_BOX(actions), schedule_button);
    gtk_widget_add_css_class(commit_button, "suggested-action");
    gtk_box_append(GTK_BOX(changes_page), actions);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(changes_scroll), state->changes_list);
    gtk_widget_set_vexpand(changes_scroll, TRUE);
    gtk_paned_set_start_child(GTK_PANED(changes_split), changes_scroll);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(state->diff_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(state->diff_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(state->diff_view), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(diff_scroll), state->diff_view);
    gtk_widget_set_vexpand(diff_scroll, TRUE);
    gtk_label_set_xalign(GTK_LABEL(state->diff_title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(state->diff_title), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(state->diff_title, "ghm-view-heading");
    gtk_box_append(GTK_BOX(diff_panel), state->diff_title);
    gtk_box_append(GTK_BOX(diff_panel), diff_scroll);
    gtk_paned_set_end_child(GTK_PANED(changes_split), diff_panel);
    gtk_paned_set_position(GTK_PANED(changes_split), 350);
    gtk_widget_set_vexpand(changes_split, TRUE);
    gtk_box_append(GTK_BOX(changes_page), changes_split);
    diff_set_text(state, "Select a changed file to preview its diff.");
    gtk_box_append(GTK_BOX(sync_actions), fetch_button);
    gtk_box_append(GTK_BOX(sync_actions), pull_button);
    gtk_box_append(GTK_BOX(sync_actions), push_button);
    gtk_box_append(GTK_BOX(editor_toolbar), sync_actions);
    gtk_stack_add_named(GTK_STACK(view_stack), state->file_view, "files");
    gtk_stack_add_named(GTK_STACK(view_stack), changes_page, "changes");
    gtk_stack_add_named(GTK_STACK(view_stack), state->history_view, "history");
    gtk_stack_add_named(GTK_STACK(view_stack), state->branch_view, "branches");
    GtkWidget *jobs_label = gtk_label_new("Scheduled commits and pushes continue in the background after this window closes.");
    gtk_label_set_xalign(GTK_LABEL(jobs_label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(jobs_label), TRUE);
    gtk_widget_add_css_class(jobs_label, "dim-label");
    gtk_widget_set_hexpand(jobs_label, TRUE);
    gtk_box_append(GTK_BOX(jobs_header), jobs_label);
    gtk_box_append(GTK_BOX(jobs_header), refresh_jobs_button);
    gtk_box_append(GTK_BOX(jobs_page), jobs_header);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(jobs_scroll), state->jobs_list);
    gtk_widget_set_vexpand(jobs_scroll, TRUE);
    gtk_box_append(GTK_BOX(jobs_page), jobs_scroll);
    gtk_stack_add_named(GTK_STACK(view_stack), jobs_page, "jobs");
    gtk_stack_add_named(GTK_STACK(view_stack), ghm_settings_view_new(), "settings");
    gtk_stack_set_hhomogeneous(GTK_STACK(view_stack), FALSE);
    gtk_stack_set_vhomogeneous(GTK_STACK(view_stack), FALSE);
    gtk_widget_set_margin_start(view_stack, 10);
    gtk_widget_set_margin_end(view_stack, 10);
    gtk_widget_set_margin_top(view_stack, 10);
    gtk_widget_set_margin_bottom(view_stack, 10);
    gtk_widget_set_vexpand(view_stack, TRUE);
    gtk_box_append(GTK_BOX(main_area), view_stack);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->activity_buttons[0]), TRUE);
    gtk_paned_set_end_child(GTK_PANED(body), main_area);
    gtk_widget_set_vexpand(workbench, TRUE);
    gtk_box_append(GTK_BOX(workspace), workbench);

    gtk_widget_add_css_class(footer, "ghm-status");
    gtk_widget_set_margin_start(state->status_branch, 12);
    gtk_box_append(GTK_BOX(footer), state->status_branch);
    gtk_box_append(GTK_BOX(footer), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(footer), state->status_changes);
    gtk_box_append(GTK_BOX(footer), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_label_set_xalign(GTK_LABEL(state->message_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(state->message_label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(state->message_label, TRUE);
    gtk_box_append(GTK_BOX(footer), state->message_label);
    GtkWidget *native_label = gtk_label_new("C17  ·  GTK4");
    gtk_widget_set_margin_end(native_label, 12);
    gtk_box_append(GTK_BOX(footer), native_label);
    gtk_box_append(GTK_BOX(workspace), footer);
    gtk_stack_add_named(GTK_STACK(stack), ghm_login_view_new(window, context, login_ready), "login");
    gtk_stack_add_named(GTK_STACK(stack), workspace, "workspace");
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "login");
    gtk_widget_set_vexpand(stack, TRUE);
    gtk_box_append(GTK_BOX(root), stack);
    gtk_window_set_child(GTK_WINDOW(window), root);

    g_signal_connect(state->repository_list, "row-selected", G_CALLBACK(repository_selected), window);
    g_signal_connect(state->changes_list, "row-selected", G_CALLBACK(changes_selected), window);
    g_signal_connect(add_button, "clicked", G_CALLBACK(add_local_clicked), window);
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(refresh_clicked), window);
    g_object_set_data(G_OBJECT(stage_all_button), "ghm-stage-action", GINT_TO_POINTER(2));
    g_signal_connect(stage_all_button, "clicked", G_CALLBACK(stage_clicked), window);
    g_signal_connect(schedule_button, "clicked", G_CALLBACK(schedule_clicked), window);
    g_signal_connect(commit_button, "clicked", G_CALLBACK(commit_clicked), window);
    g_signal_connect(push_button, "clicked", G_CALLBACK(push_clicked), window);
    g_signal_connect(fetch_button, "clicked", G_CALLBACK(sync_clicked), window);
    g_signal_connect(pull_button, "clicked", G_CALLBACK(sync_clicked), window);
    g_signal_connect(refresh_jobs_button, "clicked", G_CALLBACK(refresh_jobs_clicked), window);
    g_signal_connect(account_button, "clicked", G_CALLBACK(account_clicked), window);
    g_signal_connect(github_button, "clicked", G_CALLBACK(github_clicked), window);
    g_signal_connect(window, "close-request", G_CALLBACK(window_close_requested), NULL);
    g_signal_connect(window, "unrealize", G_CALLBACK(window_unrealized), state);
    GFile *job_directory = g_file_new_for_path(ghm_context_data_directory(context));
    state->job_monitor = g_file_monitor_directory(job_directory, G_FILE_MONITOR_NONE, NULL, NULL);
    g_object_unref(job_directory);
    if (state->job_monitor != NULL)
        g_signal_connect(state->job_monitor, "changed", G_CALLBACK(jobs_database_changed), window);
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_name(keys, "ghm-workspace-shortcuts");
    g_signal_connect(keys, "key-pressed", G_CALLBACK(workspace_key_pressed), window);
    gtk_widget_add_controller(window, keys);
    refresh_repositories(window);
    discovery_request = g_new0(DiscoveryTask, 1);
    discovery = g_task_new(window, NULL, discovery_finished, discovery_request);
    g_task_set_task_data(discovery, discovery_request, g_free);
    g_task_run_in_thread(discovery, discovery_worker);
    g_object_unref(discovery);
    ghm_login_view_check_saved(window);
    return window;
}
