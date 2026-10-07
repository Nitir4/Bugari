/* Display-dependent native GUI integration. Only disposable local fixtures;
 * optional saved-session tests will be restricted to Bugari_testing. */
#include "gui/window.h"
#include "gui_test_support.h"
#include "test_fixture.h"
#include "fault_hooks.h"
#include <ghm/history.h>
#include <ghm/remote.h>
#include <stdatomic.h>
#include <glib/gstdio.h>
#ifdef _WIN32
#include "platform/windows_runtime.h"
#endif
typedef struct {
    GtkApplication *app; GtkWidget *window; GtkWidget *second_window;
    Fixture primary, secondary;
    GhmContext *driver;
    git_repository *bare;
    unsigned step; int failed;
    gint64 deadline;
    gint64 guard_wait;
    gboolean retry_opened;
    int64_t job;
    int64_t author_time, committer_time, rescheduled_time;
    unsigned stress;
    char initial_branch[128], previous[GHM_OID_HEX_CAPACITY];
} Workflow;
static int capture(GtkWidget *window, const char *name)
{
    const char *directory = g_getenv("GHM_GUI_TEST_OUTPUT_DIR");
    if (directory == NULL || directory[0] == '\0') directory = g_get_tmp_dir();
    if (window == NULL || g_mkdir_with_parents(directory, 0700) != 0) return -1;
    char *path = g_build_filename(directory, name, NULL);
    int result = gui_capture(window, path);
    g_free(path); return result;
}
static int network_warning(const char *point, GhmError *error, void *payload)
{
    (void)payload;
    if (strcmp(point, "remote.push") != 0) return 0;
    error->code = GHM_ERROR_RATE_LIMIT; error->retry_at = (int64_t)time(NULL) + 60;
    (void)snprintf(error->message, sizeof(error->message), "GitHub rate limit reached. Wait before retrying");
    return -1;
}
static int delayed_write(const char *point, GhmError *error, void *payload)
{
    (void)error; (void)payload;
    if (strcmp(point, "file.write") == 0) g_usleep(300000);
    return 0;
}
static int head_message(const char *path, const char *message, char *oid)
{
    GhmHistory history = {0}; GhmError error = {0};
    int result = ghm_commit_get_history(path, 1, &history, &error) == 0 && history.count == 1 && strcmp(history.items[0].summary, message) == 0;
    if (result && oid != NULL) (void)snprintf(oid, GHM_OID_HEX_CAPACITY, "%s", history.items[0].oid);
    ghm_history_free(&history); return result;
}
static int stage_is(const char *path, int expected)
{
    GhmStatus status = {0}; GhmError error = {0}; int result = 0;
    if (ghm_repo_status(path, &status, &error) == 0)
        for (size_t i = 0; i < status.count; ++i) if (strcmp(status.items[i].path, "a.txt") == 0) result = status.items[i].staged == expected;
    ghm_status_free(&status); return result;
}
static int current_branch(const char *path, const char *branch)
{
    GhmStatus status = {0}; GhmError error = {0};
    int result = ghm_repo_status(path, &status, &error) == 0 && g_strcmp0(status.branch, branch) == 0;
    ghm_status_free(&status); return result;
}
static int latest_job(Workflow *state, const char *message, const char *status)
{
    GhmScheduledJobList jobs = {0}; GhmError error = {0}; int result = 0;
    if (ghm_schedule_list(state->driver, state->primary.path, &jobs, &error) == 0 && jobs.count > 0) {
        result = strcmp(jobs.items[0].message, message) == 0 && strcmp(jobs.items[0].status, status) == 0;
        if (result) state->job = jobs.items[0].id;
    }
    ghm_schedule_list_free(&jobs); return result;
}
static int push_is(Workflow *state, const char *status)
{
    GhmScheduledJobList jobs = {0}; GhmError error = {0}; int result = 0;
    if (ghm_schedule_list(state->driver, state->primary.path, &jobs, &error) == 0)
        for (size_t i = 0; i < jobs.count; ++i)
            if (jobs.items[i].id == state->job) result = strcmp(jobs.items[i].push_status, status) == 0;
    ghm_schedule_list_free(&jobs); return result;
}
static int scheduled_time_is(Workflow *state)
{
    GhmScheduledJobList jobs = {0}; GhmError error = {0}; int result = 0;
    if (ghm_schedule_list(state->driver, state->primary.path, &jobs, &error) == 0)
        for (size_t i = 0; i < jobs.count; ++i)
            if (jobs.items[i].id == state->job) result = jobs.items[i].execute_at == state->rescheduled_time && jobs.items[i].push_after_commit;
    ghm_schedule_list_free(&jobs); return result;
}
static int64_t choose_time(GtkWidget *dialog, unsigned column, int days, int hour)
{
    GtkWidget *calendar = gui_type(dialog, GTK_TYPE_CALENDAR, column);
    GtkWidget *hours = gui_type(dialog, GTK_TYPE_SPIN_BUTTON, column * 2);
    GtkWidget *minutes = gui_type(dialog, GTK_TYPE_SPIN_BUTTON, column * 2 + 1);
    if (calendar == NULL || hours == NULL || minutes == NULL) return 0;
    GDateTime *now = g_date_time_new_now_local(), *date = g_date_time_add_days(now, days);
#if GTK_CHECK_VERSION(4, 20, 0)
    gtk_calendar_set_date(GTK_CALENDAR(calendar), date);
#else
    gtk_calendar_select_day(GTK_CALENDAR(calendar), date);
#endif
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(hours), hour);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(minutes), 17);
    GDateTime *chosen = g_date_time_new_local(g_date_time_get_year(date), g_date_time_get_month(date), g_date_time_get_day_of_month(date), hour, 17, 0);
    int64_t result = g_date_time_to_unix(chosen);
    g_date_time_unref(chosen); g_date_time_unref(date); g_date_time_unref(now); return result;
}
static gint64 test_timeout(void)
{
    const char *setting = g_getenv("GHM_GUI_TEST_TIMEOUT_SECONDS");
    if (setting != NULL) {
        char *end = NULL;
        gint64 seconds = g_ascii_strtoll(setting, &end, 10);
        if (end != setting && *end == '\0' && seconds >= 15 && seconds <= 120)
            return seconds * G_TIME_SPAN_SECOND;
    }
    return 15 * G_TIME_SPAN_SECOND;
}
static void next(Workflow *state)
{ ++state->step; state->deadline = g_get_monotonic_time() + test_timeout(); }
static void success(const char *message)
{ printf("PASS GUI %s\n", message); fflush(stdout); }
#define WAIT(expression) do { if (!(expression)) return G_SOURCE_CONTINUE; } while (0)
#define CHECK(expression) do { if (!(expression)) { fprintf(stderr, "GUI FAIL step %u line %d: %s\n", state->step, __LINE__, #expression); state->failed = 1; goto stop; } } while (0)
static gboolean run(gpointer data)
{
    Workflow *state = data;
    GtkWidget *dialog, *entry, *editor, *button, *widget;
    GhmError error = {0};
    if (g_get_monotonic_time() > state->deadline) { fprintf(stderr, "GUI timeout at step %u\n", state->step); (void)capture(state->window, "ghm-gui-timeout.png"); state->failed = 1; goto stop; }
    switch (state->step) {
    case 0:
        widget = gui_type(state->window, GTK_TYPE_DROP_DOWN, 0); WAIT(widget != NULL);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(widget), 1);
        CHECK(gtk_drop_down_get_selected(GTK_DROP_DOWN(widget)) == 1);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(widget), 0);
        CHECK(gtk_drop_down_get_selected(GTK_DROP_DOWN(widget)) == 0);
        WAIT(gui_click(state->window, "Continue with local repositories")); next(state); break;
    case 1: WAIT(gui_select(state->window, "a.txt", 3)); next(state); break;
    case 2:
        editor = gui_type(state->window, GTK_TYPE_TEXT_VIEW, 0);
        WAIT(editor != NULL && gtk_text_view_get_editable(GTK_TEXT_VIEW(editor)));
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor)), "base\nGUI saved\n", -1);
        CHECK(gui_key(state->window, GDK_KEY_s, GDK_CONTROL_MASK)); next(state); break;
    case 3:
        WAIT(fixture_text_is(state->primary.path, "a.txt", "base\nGUI saved\n"));
        editor = gui_type(state->window, GTK_TYPE_TEXT_VIEW, 0);
        WAIT(editor != NULL && !gtk_text_buffer_get_modified(gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor))));
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor)), "must not save\n", -1);
        CHECK(gui_select(state->window, state->secondary.path, 4)); next(state); break;
    case 4:
        WAIT(gui_find(state->window, "Save or discard file edits before switching repositories", 2) != NULL);
        if (state->guard_wait == 0) {
            CHECK(gui_view(state->window, "Source Control"));
            CHECK(gui_click(state->window, "Refresh Status"));
            CHECK(gui_view(state->window, "Explorer"));
            state->guard_wait = g_get_monotonic_time() + 500000;
            return G_SOURCE_CONTINUE;
        }
        WAIT(g_get_monotonic_time() >= state->guard_wait);
        CHECK(gui_find(state->window, "Save or discard file edits before switching repositories", 2) != NULL);
        CHECK(fixture_text_is(state->primary.path, "a.txt", "base\nGUI saved\n"));
        CHECK(gui_click(state->window, "Discard Edits")); next(state); break;
    case 5:
        editor = gui_type(state->window, GTK_TYPE_TEXT_VIEW, 0);
        WAIT(editor != NULL && !gtk_text_buffer_get_modified(gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor))));
        CHECK(gui_find(state->window, "Save or discard file edits before switching repositories", 2) == NULL);
        CHECK(gui_select(state->window, state->secondary.path, 4)); next(state); break;
    case 6: WAIT(gui_find(state->window, "Secondary", 2) != NULL); CHECK(gui_select(state->window, state->primary.path, 4)); next(state); break;
    case 7: WAIT(gui_find(state->window, "Primary", 2) != NULL); WAIT(gui_click(state->window, "New Folder")); next(state); break;
    case 8:
        dialog = gui_dialog("New Folder"); WAIT(dialog != NULL);
        entry = gui_type(dialog, GTK_TYPE_ENTRY, 0); WAIT(entry != NULL);
        gtk_editable_set_text(GTK_EDITABLE(entry), "ui-folder"); CHECK(gui_click(dialog, "Create")); next(state); break;
    case 9: WAIT(gui_dialog("New Folder") == NULL && gui_find(state->window, "ui-folder", 3) != NULL); CHECK(gui_click(state->window, "New File")); next(state); break;
    case 10:
        dialog = gui_dialog("New File"); WAIT(dialog != NULL);
        entry = gui_type(dialog, GTK_TYPE_ENTRY, 0); WAIT(entry != NULL);
        gtk_editable_set_text(GTK_EDITABLE(entry), "ui-created.txt"); CHECK(gui_click(dialog, "Create")); next(state); break;
    case 11: WAIT(gui_dialog("New File") == NULL && gui_select(state->window, "ui-created.txt", 3)); WAIT(gui_click(state->window, "Rename")); next(state); break;
    case 12:
        dialog = gui_dialog("Rename"); WAIT(dialog != NULL);
        entry = gui_type(dialog, GTK_TYPE_ENTRY, 0); WAIT(entry != NULL);
        gtk_editable_set_text(GTK_EDITABLE(entry), "ui-renamed.txt"); CHECK(gui_click(dialog, "Rename")); next(state); break;
    case 13: WAIT(gui_dialog("Rename") == NULL && gui_select(state->window, "ui-renamed.txt", 3)); WAIT(gui_click(state->window, "Delete")); next(state); break;
    case 14: dialog = gui_dialog("Delete File"); WAIT(dialog != NULL); CHECK(gui_click(dialog, "Delete File")); next(state); break;
    case 15:
        WAIT(gui_dialog("Delete File") == NULL && gui_find(state->window, "ui-renamed.txt", 3) == NULL);
        success("save shortcut, discard, unsaved repository guard, repository switch, create folder/file, rename and confirmed delete");
        CHECK(gui_key(state->window, GDK_KEY_g, GDK_CONTROL_MASK | GDK_SHIFT_MASK)); next(state); break;
    case 16: WAIT(gui_click(state->window, "Stage")); next(state); break;
    case 17: WAIT(stage_is(state->primary.path, 1)); WAIT(gui_click(state->window, "Unstage")); next(state); break;
    case 18: WAIT(stage_is(state->primary.path, 0)); CHECK(gui_click(state->window, "Stage All")); next(state); break;
    case 19: WAIT(stage_is(state->primary.path, 1)); WAIT(gui_click(state->window, "Commit Staged")); next(state); break;
    case 20:
        dialog = gui_dialog("Commit Staged Changes"); WAIT(dialog != NULL);
        button = gui_find(dialog, "Create Commit", 0); WAIT(button != NULL && gtk_widget_get_sensitive(button));
        CHECK(gui_click(dialog, "Create Commit")); CHECK(gui_find(dialog, "Enter a commit message", 2) != NULL);
        entry = gui_type(dialog, GTK_TYPE_ENTRY, 0); CHECK(entry != NULL);
        gtk_editable_set_text(GTK_EDITABLE(entry), "GUI staged commit");
        state->author_time = choose_time(dialog, 0, -2, 10); state->committer_time = choose_time(dialog, 1, -1, 11);
        CHECK(state->author_time > 0 && state->committer_time > state->author_time);
        CHECK(gui_click(dialog, "Create Commit")); next(state); break;
    case 21:
        WAIT(gui_dialog("Commit Staged Changes") == NULL && head_message(state->primary.path, "GUI staged commit", NULL));
        GhmHistory metadata = {0}; CHECK(ghm_commit_get_history(state->primary.path, 1, &metadata, &error) == 0);
        CHECK(metadata.items[0].author_timestamp == state->author_time && metadata.items[0].committer_timestamp == state->committer_time);
        ghm_history_free(&metadata);
        success("stage, unstage, Stage All, commit dialog validation and local staged commit");
        CHECK(gui_view(state->window, "Branches")); next(state); break;
    case 22:
        CHECK(gui_click(state->window, "Refresh Branches"));
        entry = gui_type(state->window, GTK_TYPE_ENTRY, 0); WAIT(entry != NULL);
        gtk_editable_set_text(GTK_EDITABLE(entry), "ui-feature"); CHECK(gui_click(state->window, "Create Branch")); next(state); break;
    case 23: WAIT(gui_branch_action(state->window, "ui-feature", "Checkout")); next(state); break;
    case 24:
        WAIT(current_branch(state->primary.path, "ui-feature"));
        CHECK(fixture_put(state->primary.path, "a.txt", "GUI feature content\n") == 0);
        CHECK(gui_view(state->window, "Source Control")); CHECK(gui_click(state->window, "Refresh Status")); next(state); break;
    case 25: WAIT(gui_find(state->window, "Stage", 0) != NULL); CHECK(gui_click(state->window, "Stage All")); next(state); break;
    case 26: WAIT(stage_is(state->primary.path, 1)); WAIT(gui_click(state->window, "Commit Staged")); next(state); break;
    case 27:
        dialog = gui_dialog("Commit Staged Changes"); WAIT(dialog != NULL);
        button = gui_find(dialog, "Create Commit", 0); WAIT(button != NULL && gtk_widget_get_sensitive(button));
        entry = gui_type(dialog, GTK_TYPE_ENTRY, 0); CHECK(entry != NULL);
        gtk_editable_set_text(GTK_EDITABLE(entry), "GUI feature commit"); CHECK(gui_click(dialog, "Create Commit")); next(state); break;
    case 28: WAIT(gui_dialog("Commit Staged Changes") == NULL && head_message(state->primary.path, "GUI feature commit", state->previous)); CHECK(gui_view(state->window, "Branches")); next(state); break;
    case 29: WAIT(gui_branch_action(state->window, state->initial_branch, "Checkout")); next(state); break;
    case 30: WAIT(current_branch(state->primary.path, state->initial_branch)); WAIT(gui_branch_action(state->window, "ui-feature", "Merge into current")); next(state); break;
    case 31:
        WAIT(fixture_head_is(state->primary.path, state->previous)); success("branch creation, safe checkout and explicit merge controls");
        CHECK(gui_view(state->window, "Commit History")); CHECK(gui_click(state->window, "Refresh History")); next(state); break;
    case 32: WAIT(gui_select(state->window, "GUI feature commit", 2)); WAIT(gui_click(state->window, "Rewrite HEAD…")); next(state); break;
    case 33:
        dialog = gui_dialog("Rewrite History — HEAD Only"); WAIT(dialog != NULL);
        editor = gui_type(dialog, GTK_TYPE_TEXT_VIEW, 0); WAIT(editor != NULL);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor)), "GUI rewritten HEAD", -1);
        CHECK(gui_click(dialog, "Rewrite Local HEAD"));
        CHECK(gui_find(dialog, "Confirm the history change first", 2) != NULL);
        button = gui_type(dialog, GTK_TYPE_CHECK_BUTTON, 0); CHECK(button != NULL);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(button), TRUE);
        CHECK(gui_click(dialog, "Rewrite Local HEAD")); next(state); break;
    case 34:
        WAIT(gui_dialog("Rewrite History — HEAD Only") == NULL && head_message(state->primary.path, "GUI rewritten HEAD", NULL));
        CHECK(!fixture_head_is(state->primary.path, state->previous)); success("history selection and explicitly confirmed HEAD rewrite");
        CHECK(fixture_put(state->primary.path, "a.txt", "GUI scheduled snapshot\n") == 0);
        CHECK(gui_view(state->window, "Source Control")); CHECK(gui_click(state->window, "Schedule Commit")); next(state); break;
    case 35:
        dialog = gui_dialog("Schedule Commit"); WAIT(dialog != NULL);
        entry = gui_type(dialog, GTK_TYPE_ENTRY, 0); WAIT(entry != NULL);
        gtk_editable_set_text(GTK_EDITABLE(entry), "GUI scheduled job"); CHECK(gui_click(dialog, "Schedule Commit")); next(state); break;
    case 36:
        WAIT(gui_dialog("Schedule Commit") == NULL && latest_job(state, "GUI scheduled job", "PENDING"));
        CHECK(gui_view(state->window, "Scheduled Jobs")); CHECK(gui_click(state->window, "Refresh Jobs")); next(state); break;
    case 37: WAIT(gui_click(state->window, "Edit Job")); next(state); break;
    case 38:
        dialog = gui_dialog("Edit Scheduled Job"); WAIT(dialog != NULL);
        entry = gui_type(dialog, GTK_TYPE_ENTRY, 0); WAIT(entry != NULL);
        gtk_editable_set_text(GTK_EDITABLE(entry), "GUI edited job");
        button = gui_type(dialog, GTK_TYPE_CHECK_BUTTON, 0); CHECK(button != NULL); gtk_check_button_set_active(GTK_CHECK_BUTTON(button), TRUE);
        CHECK(gui_click(dialog, "Save Job")); next(state); break;
    case 39: WAIT(gui_dialog("Edit Scheduled Job") == NULL && latest_job(state, "GUI edited job", "PENDING")); WAIT(gui_click(state->window, "Reschedule")); next(state); break;
    case 40:
        dialog = gui_dialog("Reschedule Job"); WAIT(dialog != NULL);
        state->rescheduled_time = choose_time(dialog, 0, 1, 15); CHECK(state->rescheduled_time > 0);
        CHECK(gui_click(dialog, "Save Time")); next(state); break;
    case 41: WAIT(gui_dialog("Reschedule Job") == NULL && scheduled_time_is(state)); WAIT(gui_click(state->window, "Cancel Job")); next(state); break;
    case 42: dialog = gui_dialog("Cancel Scheduled Job"); WAIT(dialog != NULL); CHECK(gui_click(dialog, "Cancel Job")); next(state); break;
    case 43:
        WAIT(gui_dialog("Cancel Scheduled Job") == NULL && latest_job(state, "GUI edited job", "CANCELLED"));
        CHECK(fixture_text_is(state->primary.path, "a.txt", "GUI scheduled snapshot\n"));
        success("frozen schedule, metadata edit, reschedule and confirmed cancellation");
        ghm_test_fault_set_global(network_warning, NULL); CHECK(gui_click(state->window, "Push")); next(state); break;
    case 44:
        WAIT(gui_find(state->window, "GitHub rate limit reached", 2) != NULL);
        WAIT(capture(state->window, "ghm-gui-rate-warning.png") == 0);
        CHECK(gui_click(state->window, "Dismiss warning")); ghm_test_fault_set_global(NULL, NULL);
        CHECK(gui_click(state->window, "Push")); next(state); break;
    case 45: WAIT(gui_find(state->window, "Pushed commit", 2) != NULL); CHECK(gui_click(state->window, "Fetch")); next(state); break;
    case 46:
        WAIT(gui_find(state->window, "Fetched origin", 2) != NULL);
        success("visible persistent rate warning, dismissal, Push and Fetch controls");
        CHECK(gui_view(state->window, "Source Control")); CHECK(gui_click(state->window, "Stage All")); next(state); break;
    case 47: WAIT(stage_is(state->primary.path, 1)); WAIT(gui_click(state->window, "Commit Staged")); next(state); break;
    case 48:
        dialog = gui_dialog("Commit Staged Changes"); WAIT(dialog != NULL);
        entry = gui_type(dialog, GTK_TYPE_ENTRY, 0); WAIT(entry != NULL);
        button = gui_find(dialog, "Create Commit", 0); WAIT(button != NULL && gtk_widget_get_sensitive(button));
        gtk_editable_set_text(GTK_EDITABLE(entry), "GUI clean before pull"); CHECK(gui_click(dialog, "Create Commit")); next(state); break;
    case 49: WAIT(gui_dialog("Commit Staged Changes") == NULL && head_message(state->primary.path, "GUI clean before pull", NULL)); CHECK(gui_click(state->window, "Push")); next(state); break;
    case 50: WAIT(gui_find(state->window, "Pushed commit", 2) != NULL); CHECK(gui_click(state->window, "Pull")); next(state); break;
    case 51:
        WAIT(gui_find(state->window, "Already up to date", 2) != NULL);
        success("independent author/committer calendars and times, Push then Pull");
        CHECK(fixture_put(state->primary.path, "a.txt", "GUI retry snapshot\n") == 0);
        CHECK(ghm_repo_stage_all(state->primary.path, &error) == 0);
        CHECK(gui_click(state->window, "Schedule Commit")); next(state); break;
    case 52:
        dialog = gui_dialog("Schedule Commit"); WAIT(dialog != NULL);
        entry = gui_type(dialog, GTK_TYPE_ENTRY, 0); WAIT(entry != NULL);
        gtk_editable_set_text(GTK_EDITABLE(entry), "GUI retry job");
        button = gui_type(dialog, GTK_TYPE_CHECK_BUTTON, 0); CHECK(button != NULL); gtk_check_button_set_active(GTK_CHECK_BUTTON(button), FALSE);
        button = gui_type(dialog, GTK_TYPE_CHECK_BUTTON, 1); CHECK(button != NULL); gtk_check_button_set_active(GTK_CHECK_BUTTON(button), TRUE);
        CHECK(choose_time(dialog, 0, -1, 10) > 0); CHECK(choose_time(dialog, 1, 1, 11) > 0);
        CHECK(gui_click(dialog, "Schedule Commit")); next(state); break;
    case 53:
        WAIT(gui_dialog("Schedule Commit") == NULL && latest_job(state, "GUI retry job", "PENDING"));
        CHECK(ghm_schedule_run_one_due(state->driver, (int64_t)time(NULL) + 3 * 86400, &error) == 1);
        ghm_test_fault_set(network_warning, NULL);
        CHECK(ghm_schedule_run_one_push(state->driver, &error) == 1 && push_is(state, "PENDING"));
        ghm_test_fault_set(NULL, NULL);
        CHECK(gui_view(state->window, "Explorer")); next(state); break;
    case 54:
        WAIT(gui_find(state->window, "GitHub rate limit reached", 2) != NULL);
        CHECK(sqlite3_exec(state->driver->db, "UPDATE scheduled_jobs SET push_retry_at=0,push_attempts=8 WHERE push_status='PENDING'", NULL, NULL, NULL) == SQLITE_OK);
        ghm_test_fault_set(network_warning, NULL);
        CHECK(ghm_schedule_run_one_push(state->driver, &error) == 1 && push_is(state, "FAILED")); ghm_test_fault_set(NULL, NULL);
        CHECK(gui_view(state->window, "Scheduled Jobs")); next(state); break;
    case 55:
        if (!state->retry_opened) {
            WAIT(gui_click(state->window, "Retry Push"));
            state->retry_opened = TRUE; break;
        }
        dialog = gui_dialog("Retry Scheduled Push"); WAIT(dialog != NULL);
        CHECK(gui_click(dialog, "Retry Push")); next(state); break;
    case 56:
        WAIT(gui_dialog("Retry Scheduled Push") == NULL && push_is(state, "PENDING"));
        CHECK(ghm_schedule_run_one_push(state->driver, &error) == 1 && push_is(state, "COMPLETED"));
        success("staged-only scheduling, date/time and push options, database-monitor warning across views and GUI Retry Push");
        CHECK(gui_view(state->window, "Settings")); next(state); break;
    case 57: WAIT(gui_click(state->window, "Refresh Developer Log")); next(state); break;
    case 58:
        editor = gui_type(state->window, GTK_TYPE_TEXT_VIEW, 0); WAIT(editor != NULL);
        char *log_text = gui_buffer_text(editor); CHECK(log_text != NULL); g_free(log_text);
        for (guint key = GDK_KEY_1; key <= GDK_KEY_6; ++key) CHECK(gui_key(state->window, key, GDK_CONTROL_MASK));
        CHECK(gui_key(state->window, GDK_KEY_b, GDK_CONTROL_MASK)); CHECK(gui_key(state->window, GDK_KEY_b, GDK_CONTROL_MASK));
        CHECK(gui_view(state->window, "Explorer")); next(state); break;
    case 59: WAIT(gui_select(state->window, "a.txt", 3)); next(state); break;
    case 60:
        editor = gui_type(state->window, GTK_TYPE_TEXT_VIEW, 0); WAIT(editor != NULL && gtk_text_view_get_editable(GTK_TEXT_VIEW(editor)));
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor)), "unsaved close guard\n", -1);
        gboolean blocked = FALSE; g_signal_emit_by_name(state->window, "close-request", &blocked); CHECK(blocked);
        CHECK(gui_find(state->window, "Save or discard file edits before closing", 2) != NULL);
        CHECK(gui_click(state->window, "Discard Edits")); next(state); break;
    case 61:
        editor = gui_type(state->window, GTK_TYPE_TEXT_VIEW, 0); WAIT(editor != NULL && !gtk_text_buffer_get_modified(gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor))));
        success("settings log refresh, all six view shortcuts, sidebar shortcut and unsaved close guard");
        gtk_window_set_default_size(GTK_WINDOW(state->window), 900, 600); next(state); break;
    case 62:
        CHECK(gui_select(state->window, state->secondary.path, 4));
        CHECK(gui_select(state->window, state->primary.path, 4));
        CHECK(gui_view(state->window, "Source Control"));
        CHECK(gui_click(state->window, "Refresh Status"));
        CHECK(gui_key(state->window, GDK_KEY_1 + state->stress % 6, GDK_CONTROL_MASK));
        if (++state->stress == 60) { success("60 rapid repository pairs and view/status refreshes, small-window layout"); next(state); }
        break;
    case 63:
        WAIT(capture(state->window, "ghm-gui-stress.png") == 0);
        CHECK(gui_view(state->window, "Explorer")); WAIT(gui_click(state->window, "New File"));
        state->step = 70; next(state); break;
    case 71:
        dialog = gui_dialog("New File"); WAIT(dialog != NULL);
        CHECK(gui_click(dialog, "Create")); CHECK(gui_find(dialog, "Enter a repository-relative path", 2) != NULL);
        entry = gui_type(dialog, GTK_TYPE_ENTRY, 0); CHECK(entry != NULL);
        gtk_editable_set_text(GTK_EDITABLE(entry), "../forbidden.txt"); CHECK(gui_click(dialog, "Create")); next(state); break;
    case 72:
        dialog = gui_dialog("New File"); WAIT(dialog != NULL && gui_find(dialog, "Invalid or protected", 2) != NULL);
        CHECK(gui_click(dialog, "Cancel")); CHECK(gui_view(state->window, "Source Control")); CHECK(gui_click(state->window, "Schedule Commit")); next(state); break;
    case 73:
        dialog = gui_dialog("Schedule Commit"); WAIT(dialog != NULL);
        CHECK(gui_click(dialog, "Schedule Commit")); CHECK(gui_find(dialog, "Enter a commit message", 2) != NULL);
        CHECK(gui_click(dialog, "Cancel")); success("file/schedule validation feedback and cancelled dialogs"); next(state); break;
    case 74:
        CHECK(fixture_put(state->primary.path, "b-binary.bin", "\xff\xfe\x80") == 0);
        char *large = g_malloc0(600000); memset(large, 'a', 599999);
        CHECK(fixture_put(state->primary.path, "b-large.txt", large) == 0); g_free(large);
        char *folder = g_build_filename(state->primary.path, "b-folder", NULL);
        int created = g_mkdir(folder, 0700); g_free(folder); CHECK(created == 0);
        CHECK(fixture_put(state->primary.path, "b-folder/nested.txt", "nested\n") == 0);
        for (unsigned i = 0; i < 5100; ++i) { char name[64]; (void)snprintf(name, sizeof(name), "bulk-%05u.txt", i); CHECK(fixture_put(state->primary.path, name, "stress\n") == 0); }
        CHECK(gui_view(state->window, "Source Control"));
        CHECK(gui_click(state->window, "Refresh Status"));
        CHECK(gui_view(state->window, "Explorer")); state->step = 63; next(state); break;
    case 64:
        WAIT(gui_find(state->window, "first 5,000 entries", 2) != NULL);
        CHECK(gui_select(state->window, "b-binary.bin", 3)); next(state); break;
    case 65:
        editor = gui_type(state->window, GTK_TYPE_TEXT_VIEW, 0); WAIT(editor != NULL && !gtk_text_view_get_editable(GTK_TEXT_VIEW(editor)));
        char *binary_text = gui_buffer_text(editor); int binary_ok = strstr(binary_text, "non-UTF-8") != NULL; g_free(binary_text); WAIT(binary_ok);
        CHECK(gui_select(state->window, "b-large.txt", 3)); next(state); break;
    case 66:
        editor = gui_type(state->window, GTK_TYPE_TEXT_VIEW, 0); WAIT(editor != NULL && !gtk_text_view_get_editable(GTK_TEXT_VIEW(editor)));
        char *large_text = gui_buffer_text(editor); int large_ok = strstr(large_text, "512 KiB") != NULL; g_free(large_text); WAIT(large_ok);
        button = gui_find(state->window, "b-folder", 3); CHECK(button != NULL);
        CHECK(g_strcmp0(g_object_get_data(G_OBJECT(button), "ghm-file-marker"), "•") == 0);
        g_signal_emit_by_name(gtk_widget_get_parent(button), "row-activated", button); next(state); break;
    case 67:
        WAIT(gui_find(state->window, "b-folder/nested.txt", 3) != NULL);
        success("5,100-file explorer limit, binary/large preview protections and folder expansion");
        GhmContext *second_context = NULL; CHECK(ghm_context_open(state->primary.data, &second_context, &error) == 0);
        state->second_window = ghm_window_new(state->app, second_context); gtk_window_present(GTK_WINDOW(state->second_window)); next(state); break;
    case 68: WAIT(gui_click(state->second_window, "Continue with local repositories")); CHECK(gui_select(state->window, "a.txt", 3)); next(state); break;
    case 69:
        editor = gui_type(state->window, GTK_TYPE_TEXT_VIEW, 0); WAIT(editor != NULL && gtk_text_view_get_editable(GTK_TEXT_VIEW(editor)));
        ghm_test_fault_set_global(delayed_write, NULL);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor)), "save completing after close\n", -1);
        CHECK(gui_click(state->window, "Save"));
        CHECK(gui_view(state->second_window, "Source Control")); CHECK(gui_click(state->second_window, "Refresh Status"));
        CHECK(gui_click(state->second_window, "Schedule Commit"));
        CHECK(gui_view(state->window, "Source Control")); CHECK(gui_click(state->window, "Refresh Status"));
        gtk_window_destroy(GTK_WINDOW(state->second_window)); state->second_window = NULL;
        gtk_window_destroy(GTK_WINDOW(state->window)); state->window = NULL; next(state); break;
    case 70:
        if (++state->stress < 100) return G_SOURCE_CONTINUE;
        WAIT(fixture_text_is(state->primary.path, "a.txt", "save completing after close\n"));
        CHECK(gui_dialog("Schedule Commit") == NULL); ghm_test_fault_set_global(NULL, NULL);
        success("two-window destruction during delayed save and large refresh; modal closes with parent and callbacks drain");
        g_application_quit(G_APPLICATION(state->app)); return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
stop:
    (void)capture(state->window, "ghm-gui-failure.png");
    ghm_test_fault_set_global(NULL, NULL);
    if (state->window != NULL) gtk_window_destroy(GTK_WINDOW(state->window));
    g_application_quit(G_APPLICATION(state->app));
    return G_SOURCE_REMOVE;
}
static void activate(GtkApplication *application, gpointer data)
{
    Workflow *state = data;
    GhmError error = {0};
    GhmContext *context = NULL;
    git_remote *remote = NULL; git_config *config = NULL;
    char path[sizeof(state->primary.root) + 64], oid[GHM_OID_HEX_CAPACITY];
    state->app = application;
    g_application_hold(G_APPLICATION(application));
    if (fixture_open(&state->primary, &error) != 0 || fixture_open(&state->secondary, &error) != 0 ||
        ghm_repo_register(state->primary.context, state->secondary.path, &error) != 0 ||
        git_repository_config(&config, state->primary.repo) < 0 ||
        git_config_set_string(config, "user.name", "GUI Test") < 0 ||
        git_config_set_string(config, "user.email", "gui@example.invalid") < 0) goto failed;
    (void)snprintf(state->initial_branch, sizeof(state->initial_branch), "%s", state->primary.branch);
    (void)snprintf(path, sizeof(path), "%s/remote.git", state->primary.root);
    if (git_repository_init(&state->bare, path, 1) < 0 || git_remote_create(&remote, state->primary.repo, "origin", path) < 0 ||
        ghm_repo_push(state->primary.path, NULL, oid, &error) != 0 ||
        sqlite3_exec(state->primary.context->db, "UPDATE repositories SET name=CASE WHEN id=1 THEN 'Primary' ELSE 'Secondary' END", NULL, NULL, NULL) != SQLITE_OK ||
        ghm_context_open(state->primary.data, &state->driver, &error) != 0) goto failed;
    context = state->primary.context; state->primary.context = NULL;
    state->window = ghm_window_new(application, context);
    gtk_window_present(GTK_WINDOW(state->window));
    state->deadline = g_get_monotonic_time() + test_timeout();
    g_timeout_add(50, run, state);
    git_config_free(config); git_remote_free(remote); return;
failed:
    fprintf(stderr, "GUI fixture failed: %s\n", error.message); state->failed = 1;
    git_config_free(config); git_remote_free(remote); g_application_quit(G_APPLICATION(application));
}
int main(void)
{
    Workflow state = {0};
    (void)g_setenv("GHM_GITHUB_CLIENT_ID", "", TRUE);
    g_unsetenv("GTK_THEME");
#ifdef _WIN32
    if (ghm_windows_gui_runtime() != 0) return 1;
#endif
    GtkApplication *application = gtk_application_new("io.github.ghm.WorkflowTest", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(application, "activate", G_CALLBACK(activate), &state);
    int status = g_application_run(G_APPLICATION(application), 0, NULL);
    g_object_unref(application);
    ghm_context_close(state.driver); git_repository_free(state.bare);
    fixture_close(&state.primary); fixture_close(&state.secondary);
    return status == 0 && !state.failed ? 0 : 1;
}
