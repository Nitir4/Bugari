#include "gui/login_view.h"
#include "core/git.h"

#include <ghm/github.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef GHM_GITHUB_CLIENT_ID
#define GHM_GITHUB_CLIENT_ID ""
#endif

typedef struct {
    GhmLoginReady ready;
    GtkWidget *scope_dropdown;
    GtkWidget *start_button;
    GtkWidget *code_area;
    GtkWidget *code_label;
    GtkWidget *status_label;
    GtkWidget *signout_button;
    GCancellable *poll_cancellable;
    guint timer_id;
    guint generation;
    gint64 expires_at;
    char *app_client_id;
    char *client_id;
    char *credential_client_id;
    char *login_name;
    gboolean authenticated;
    GhmDeviceCode device;
} LoginState;

typedef struct {
    guint generation;
    char *client_id;
    char *scope;
    GhmDeviceCode device;
    GhmError error;
    gboolean succeeded;
} BeginTask;

typedef struct {
    guint generation;
    char *client_id;
    GhmDeviceCode device;
    GhmOAuthToken token;
    GhmAuthPollStatus status;
    unsigned next_interval;
    char *login;
    GhmError error;
    gboolean succeeded;
} PollTask;

typedef struct {
    char *client_id;
    GhmError error;
    gboolean succeeded;
} SignoutTask;

typedef struct {
    guint generation;
    char *client_id;
    GhmOAuthToken token;
    char *login;
    GhmError error;
    gboolean found;
    gboolean succeeded;
} LoadTask;

static LoginState *login_state(GtkWidget *window)
{
    return g_object_get_data(G_OBJECT(window), "ghm-login-state");
}

const char *ghm_login_view_client_id(GtkWidget *window)
{
    LoginState *state = login_state(window);
    return state != NULL && state->authenticated ? state->credential_client_id : NULL;
}

static void login_state_free(gpointer data)
{
    LoginState *state = data;
    if (state->timer_id != 0) g_source_remove(state->timer_id);
    g_clear_object(&state->poll_cancellable);
    ghm_device_code_clear(&state->device);
    g_free(state->app_client_id);
    g_free(state->client_id);
    g_free(state->credential_client_id);
    g_free(state->login_name);
    g_free(state);
}

static void begin_task_free(gpointer data)
{
    BeginTask *task = data;
    g_free(task->client_id);
    g_free(task->scope);
    ghm_device_code_clear(&task->device);
    g_free(task);
}

static void poll_task_free(gpointer data)
{
    PollTask *task = data;
    g_free(task->client_id);
    ghm_device_code_clear(&task->device);
    ghm_oauth_token_clear(&task->token);
    free(task->login);
    g_free(task);
}

static void load_task_free(gpointer data)
{
    LoadTask *task = data;
    g_free(task->client_id);
    ghm_oauth_token_clear(&task->token);
    free(task->login);
    g_free(task);
}

static void signout_task_free(gpointer data)
{
    SignoutTask *task = data;
    g_free(task->client_id);
    g_free(task);
}

static void mark_authenticated(LoginState *state, const char *client_id, const char *login)
{
    g_free(state->login_name);
    state->login_name = g_strdup(login != NULL ? login : "GitHub");
    g_free(state->credential_client_id);
    state->credential_client_id = g_strdup(client_id);
    state->authenticated = TRUE;
    gtk_widget_set_visible(state->signout_button, TRUE);
}

static void show_message(LoginState *state, const char *message)
{
    gtk_label_set_text(GTK_LABEL(state->status_label), message);
}

static void stop_polling(LoginState *state)
{
    ++state->generation;
    if (state->timer_id != 0) {
        g_source_remove(state->timer_id);
        state->timer_id = 0;
    }
    if (state->poll_cancellable != NULL) g_cancellable_cancel(state->poll_cancellable);
    g_clear_object(&state->poll_cancellable);
    ghm_device_code_clear(&state->device);
    g_clear_pointer(&state->client_id, g_free);
    gtk_widget_set_visible(state->code_area, FALSE);
    gtk_widget_set_sensitive(state->start_button,
                             state->app_client_id != NULL && state->app_client_id[0] != '\0');
}

static gboolean poll_timeout(gpointer user_data);

static void schedule_poll(GtkWidget *window, unsigned seconds)
{
    LoginState *state = login_state(window);
    guint delay = seconds < 5 ? 5 : seconds;
    if (delay > 3600) delay = 3600;
    state->timer_id = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, delay,
                                                  poll_timeout, g_object_ref(window), g_object_unref);
}

static void poll_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
    PollTask *request = task_data;
    (void)source;
    (void)cancellable;
    request->succeeded = ghm_github_auth_poll(request->client_id, &request->device,
                                                &request->status, &request->token,
                                                &request->next_interval, &request->error) == 0;
    if (request->succeeded && request->status == GHM_AUTH_SUCCESS &&
        !g_cancellable_is_cancelled(cancellable)) {
        request->succeeded = ghm_credentials_store(request->client_id, &request->token,
                                                    &request->error) == 0;
        if (request->succeeded) {
            GhmError profile_error = {0};
            if (ghm_github_current_user(request->token.access_token, &request->login,
                                        &profile_error) != 0 && profile_error.code == GHM_ERROR_AUTH) {
                GhmError ignored = {0};
                (void)ghm_credentials_clear(request->client_id, &ignored);
                request->error = profile_error;
                request->succeeded = FALSE;
            }
        }
    }
    g_task_return_boolean(task, request->succeeded);
}

static void poll_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(source);
    LoginState *state = login_state(window);
    PollTask *request = g_task_get_task_data(G_TASK(result));
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (!gtk_widget_get_realized(window) || request->generation != state->generation) return;
    g_clear_object(&state->poll_cancellable);
    if (!request->succeeded) {
        show_message(state, request->error.message);
        stop_polling(state);
        return;
    }
    if (request->status == GHM_AUTH_SUCCESS) {
        show_message(state, request->login != NULL ? "Signed in to GitHub" :
                     "Signed in; GitHub profile is temporarily unavailable");
        stop_polling(state);
        mark_authenticated(state, request->client_id, request->login);
        state->ready(window, state->login_name);
        return;
    }
    if (request->status == GHM_AUTH_EXPIRED || request->status == GHM_AUTH_DENIED) {
        show_message(state, request->status == GHM_AUTH_EXPIRED ?
                     "Code expired. Start sign-in again." : "GitHub sign-in was denied.");
        stop_polling(state);
        return;
    }
    if (g_get_monotonic_time() >= state->expires_at) {
        show_message(state, "Code expired. Start sign-in again.");
        stop_polling(state);
        return;
    }
    state->device.interval = request->next_interval;
    show_message(state, "Waiting for approval in your browser…");
    schedule_poll(window, request->next_interval);
}

static gboolean poll_timeout(gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(user_data);
    LoginState *state = login_state(window);
    PollTask *request;
    GTask *task;
    state->timer_id = 0;
    if (g_get_monotonic_time() >= state->expires_at) {
        show_message(state, "Code expired. Start sign-in again.");
        stop_polling(state);
        return G_SOURCE_REMOVE;
    }
    request = g_new0(PollTask, 1);
    request->generation = state->generation;
    request->client_id = g_strdup(state->client_id);
    request->device.device_code = strdup(state->device.device_code);
    request->device.interval = state->device.interval;
    state->poll_cancellable = g_cancellable_new();
    task = g_task_new(window, state->poll_cancellable, poll_finished, NULL);
    g_task_set_task_data(task, request, poll_task_free);
    g_task_run_in_thread(task, poll_worker);
    g_object_unref(task);
    return G_SOURCE_REMOVE;
}

static void begin_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
    BeginTask *request = task_data;
    (void)source;
    (void)cancellable;
    request->succeeded = ghm_github_auth_begin(request->client_id, request->scope,
                                               &request->device, &request->error) == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void begin_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(source);
    LoginState *state = login_state(window);
    BeginTask *request = g_task_get_task_data(G_TASK(result));
    GError *browser_error = NULL;
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (!gtk_widget_get_realized(window) || request->generation != state->generation) return;
    if (!request->succeeded) {
        show_message(state, request->error.message);
        gtk_widget_set_sensitive(state->start_button, TRUE);
        return;
    }
    state->client_id = g_strdup(request->client_id);
    state->device = request->device;
    request->device = (GhmDeviceCode){0};
    state->expires_at = g_get_monotonic_time() + (gint64)state->device.expires_in * G_USEC_PER_SEC;
    gtk_label_set_text(GTK_LABEL(state->code_label), state->device.user_code);
    gtk_widget_set_visible(state->code_area, TRUE);
    show_message(state, "Enter this code in the browser to authorize GitHub access.");
    if (!g_app_info_launch_default_for_uri(state->device.verification_uri, NULL, &browser_error)) {
        show_message(state, "Open github.com/login/device in your browser and enter the code.");
        g_clear_error(&browser_error);
    }
    schedule_poll(window, state->device.interval);
}

static void login_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(user_data);
    LoginState *state = login_state(window);
    const char *client_id = state->app_client_id;
    BeginTask *request;
    GTask *task;
    (void)button;
    if (client_id == NULL || client_id[0] == '\0') {
        show_message(state, "This build has no GitHub OAuth app configured. Ask the app developer to add its public Client ID.");
        return;
    }
    stop_polling(state);
    request = g_new0(BeginTask, 1);
    request->generation = state->generation;
    request->client_id = g_strdup(client_id);
    request->scope = g_strdup(gtk_drop_down_get_selected(GTK_DROP_DOWN(state->scope_dropdown)) == 0 ?
                              "public_repo read:user" : "repo read:user");
    gtk_widget_set_sensitive(state->start_button, FALSE);
    show_message(state, "Contacting GitHub…");
    task = g_task_new(window, NULL, begin_finished, NULL);
    g_task_set_task_data(task, request, begin_task_free);
    g_task_run_in_thread(task, begin_worker);
    g_object_unref(task);
}

static void cancel_clicked(GtkButton *button, gpointer user_data)
{
    LoginState *state = login_state(GTK_WIDGET(user_data));
    (void)button;
    stop_polling(state);
    show_message(state, "Sign-in cancelled.");
}

static void browser_clicked(GtkButton *button, gpointer user_data)
{
    LoginState *state = login_state(GTK_WIDGET(user_data));
    GError *error = NULL;
    (void)button;
    if (state->device.verification_uri != NULL)
        (void)g_app_info_launch_default_for_uri(state->device.verification_uri, NULL, &error);
    if (error != NULL) {
        show_message(state, "Open github.com/login/device in your browser.");
        g_clear_error(&error);
    }
}

static void copy_clicked(GtkButton *button, gpointer user_data)
{
    LoginState *state = login_state(GTK_WIDGET(user_data));
    (void)button;
    if (state->device.user_code != NULL) {
        GdkClipboard *clipboard = gdk_display_get_clipboard(gtk_widget_get_display(state->code_area));
        gdk_clipboard_set_text(clipboard, state->device.user_code);
        show_message(state, "Code copied. Paste it into GitHub's device sign-in page.");
    }
}

static void local_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(user_data);
    LoginState *state = login_state(window);
    (void)button;
    stop_polling(state);
    state->ready(window, state->authenticated ? state->login_name : NULL);
}

static void signout_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
    SignoutTask *request = task_data;
    (void)source;
    (void)cancellable;
    request->succeeded = ghm_credentials_clear(request->client_id, &request->error) == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void signout_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(source);
    LoginState *state = login_state(window);
    SignoutTask *request = g_task_get_task_data(G_TASK(result));
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (!gtk_widget_get_realized(window)) return;
    if (!request->succeeded) {
        show_message(state, request->error.message);
        gtk_widget_set_sensitive(state->signout_button, TRUE);
        return;
    }
    state->authenticated = FALSE;
    g_clear_pointer(&state->credential_client_id, g_free);
    g_clear_pointer(&state->login_name, g_free);
    gtk_widget_set_visible(state->signout_button, FALSE);
    show_message(state, "Signed out of GitHub.");
    state->ready(window, NULL);
}

static void signout_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(user_data);
    LoginState *state = login_state(window);
    const char *client_id = state->credential_client_id;
    SignoutTask *request;
    GTask *task;
    (void)button;
    if (client_id == NULL || client_id[0] == '\0') return;
    stop_polling(state);
    request = g_new0(SignoutTask, 1);
    request->client_id = g_strdup(client_id);
    gtk_widget_set_sensitive(state->signout_button, FALSE);
    show_message(state, "Signing out…");
    task = g_task_new(window, NULL, signout_finished, NULL);
    g_task_set_task_data(task, request, signout_task_free);
    g_task_run_in_thread(task, signout_worker);
    g_object_unref(task);
}

static void load_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
    LoadTask *request = task_data;
    GhmOAuthToken renewed = {0};
    time_t now = time(NULL);
    (void)source;
    (void)cancellable;
    request->succeeded = ghm_credentials_load(request->client_id, &request->token, &request->error) == 0;
    if (!request->succeeded || request->token.access_token == NULL) {
        g_task_return_boolean(task, request->succeeded);
        return;
    }
    request->found = TRUE;
    if (request->token.expires_at > 0 && request->token.expires_at <= (int64_t)now + 60) {
        if (request->token.refresh_token == NULL ||
            (request->token.refresh_expires_at > 0 && request->token.refresh_expires_at <= (int64_t)now)) {
            ghm_error_set(&request->error, GHM_ERROR_AUTH, "GitHub session expired; sign in again");
            request->succeeded = FALSE;
        } else {
            request->succeeded = ghm_github_auth_refresh(request->client_id,
                             request->token.refresh_token, &renewed, &request->error) == 0;
            if (request->succeeded) {
                request->succeeded = ghm_credentials_store(request->client_id, &renewed,
                                                            &request->error) == 0;
                if (request->succeeded) {
                    ghm_oauth_token_clear(&request->token);
                    request->token = renewed;
                    renewed = (GhmOAuthToken){0};
                }
            }
        }
    }
    ghm_oauth_token_clear(&renewed);
    if (request->succeeded)
        request->succeeded = ghm_github_current_user(request->token.access_token,
                                                      &request->login, &request->error) == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void load_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(source);
    LoginState *state = login_state(window);
    LoadTask *request = g_task_get_task_data(G_TASK(result));
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (!gtk_widget_get_realized(window) || request->generation != state->generation) return;
    if (request->succeeded && request->found) {
        show_message(state, "Signed in to GitHub");
        mark_authenticated(state, request->client_id, request->login);
        state->ready(window, state->login_name);
    } else if (!request->succeeded) show_message(state, request->error.message);
    else show_message(state, "Sign in to connect your GitHub account.");
}

void ghm_login_view_check_saved(GtkWidget *window)
{
    LoginState *state = login_state(window);
    const char *client_id = state->app_client_id;
    LoadTask *request;
    GTask *task;
    if (client_id == NULL || client_id[0] == '\0') return;
    request = g_new0(LoadTask, 1);
    request->generation = state->generation;
    request->client_id = g_strdup(client_id);
    show_message(state, "Checking saved GitHub login…");
    task = g_task_new(window, NULL, load_finished, NULL);
    g_task_set_task_data(task, request, load_task_free);
    g_task_run_in_thread(task, load_worker);
    g_object_unref(task);
}

GtkWidget *ghm_login_view_new(GtkWidget *window, GhmContext *context, GhmLoginReady ready)
{
    LoginState *state = g_new0(LoginState, 1);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    GtkWidget *heading = gtk_label_new("GitHub Commit Manager");
    GtkWidget *description = gtk_label_new(
        "Sign in securely on GitHub in your system browser. "
        "This app never asks for your GitHub password.");
    GtkWidget *scope_label = gtk_label_new("Repository access");
    GtkWidget *code_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *browser_button = gtk_button_new_with_label("Open GitHub");
    GtkWidget *copy_button = gtk_button_new_with_label("Copy Code");
    GtkWidget *cancel_button = gtk_button_new_with_label("Cancel");
    GtkWidget *local_button = gtk_button_new_with_label("Continue with local repositories");
    char *saved = NULL;
    GhmError setting_error = {0};
    state->ready = ready;
    state->scope_dropdown = gtk_drop_down_new_from_strings((const char *[]) {
        "Public repositories only", "Public and private repositories", NULL
    });
    state->start_button = gtk_button_new_with_label("Login with GitHub");
    state->code_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    state->code_label = gtk_label_new("");
    state->status_label = gtk_label_new("Sign in to connect your GitHub account.");
    state->signout_button = gtk_button_new_with_label("Sign out of GitHub");
    g_object_set_data_full(G_OBJECT(window), "ghm-login-state", state, login_state_free);
    (void)ghm_setting_get(context, "github_client_id", &saved, &setting_error);
    const char *environment_id = g_getenv("GHM_GITHUB_CLIENT_ID");
    const char *configured_id = environment_id != NULL && environment_id[0] != '\0' ?
                                environment_id : GHM_GITHUB_CLIENT_ID;
    if (configured_id[0] == '\0') configured_id = saved;
    state->app_client_id = g_strdup(configured_id);
    free(saved);
    gtk_widget_set_halign(outer, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(outer, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(outer, 420, -1);
    gtk_label_set_wrap(GTK_LABEL(description), TRUE);
    gtk_label_set_wrap(GTK_LABEL(state->status_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(scope_label), 0.0f);
    gtk_widget_add_css_class(heading, "title-1");
    gtk_box_append(GTK_BOX(outer), heading);
    gtk_box_append(GTK_BOX(outer), description);
    gtk_box_append(GTK_BOX(outer), scope_label);
    gtk_box_append(GTK_BOX(outer), state->scope_dropdown);
    gtk_box_append(GTK_BOX(outer), state->start_button);
    if (state->app_client_id == NULL || state->app_client_id[0] == '\0') {
        gtk_widget_set_sensitive(state->start_button, FALSE);
        show_message(state, "GitHub login is unavailable: this build needs a public OAuth app Client ID configured by its developer.");
    }
    gtk_widget_add_css_class(state->code_label, "title-1");
    gtk_box_append(GTK_BOX(state->code_area), gtk_label_new("Your one-time code"));
    gtk_box_append(GTK_BOX(state->code_area), state->code_label);
    gtk_box_append(GTK_BOX(code_buttons), browser_button);
    gtk_box_append(GTK_BOX(code_buttons), copy_button);
    gtk_box_append(GTK_BOX(code_buttons), cancel_button);
    gtk_box_append(GTK_BOX(state->code_area), code_buttons);
    gtk_widget_set_visible(state->code_area, FALSE);
    gtk_box_append(GTK_BOX(outer), state->code_area);
    gtk_box_append(GTK_BOX(outer), state->status_label);
    gtk_widget_set_visible(state->signout_button, FALSE);
    gtk_box_append(GTK_BOX(outer), state->signout_button);
    gtk_box_append(GTK_BOX(outer), local_button);
    g_signal_connect(state->start_button, "clicked", G_CALLBACK(login_clicked), window);
    g_signal_connect(browser_button, "clicked", G_CALLBACK(browser_clicked), window);
    g_signal_connect(copy_button, "clicked", G_CALLBACK(copy_clicked), window);
    g_signal_connect(cancel_button, "clicked", G_CALLBACK(cancel_clicked), window);
    g_signal_connect(local_button, "clicked", G_CALLBACK(local_clicked), window);
    g_signal_connect(state->signout_button, "clicked", G_CALLBACK(signout_clicked), window);
    return outer;
}
