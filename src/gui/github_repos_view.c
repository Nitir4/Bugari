#include "gui/dialog.h"
#include "gui/github_repos_view.h"

#include <ghm/github_repos.h>
#include <ghm/remote.h>

#include <stdlib.h>
#include <string.h>

typedef struct {
    GtkWidget *parent;
    GhmContext *context;
    char *client_id;
    GhmGitHubRepoList repos;
    GtkWidget *list;
    GtkWidget *status;
    GtkWidget *search;
    GtkWidget *open_button;
    GtkWidget *branch_button;
    gboolean busy;
    GhmRepositoryOpened opened;
} ReposState;

typedef struct {
    char *client_id;
    GhmGitHubRepoList repos;
    GhmError error;
    gboolean succeeded;
} ListTask;

typedef struct {
    GhmContext *context;
    char *client_id;
    GhmGitHubRepo repo;
    char *path;
    GhmError error;
    gboolean succeeded;
} CloneTask;

typedef struct {
    char *client_id;
    char *full_name;
    GhmGitHubBranchList branches;
    GhmError error;
    gboolean succeeded;
} BranchTask;

static void state_free(gpointer data)
{
    ReposState *state = data;
    g_object_unref(state->parent);
    free(state->client_id);
    ghm_github_repos_free(&state->repos);
    free(state);
}

static void list_task_free(gpointer data)
{
    ListTask *request = data;
    free(request->client_id);
    ghm_github_repos_free(&request->repos);
    free(request);
}

static void clone_task_free(gpointer data)
{
    CloneTask *request = data;
    free(request->client_id);
    free(request->repo.full_name);
    free(request->repo.clone_url);
    free(request->repo.default_branch);
    free(request->repo.description);
    free(request->repo.html_url);
    free(request->path);
    free(request);
}

static void branch_task_free(gpointer data)
{
    BranchTask *request = data;
    free(request->client_id);
    free(request->full_name);
    ghm_github_branches_free(&request->branches);
    free(request);
}

static void branch_worker(GTask *task, gpointer source, gpointer task_data,
                          GCancellable *cancellable)
{
    BranchTask *request = task_data;
    (void)source;
    (void)cancellable;
    request->succeeded = ghm_github_branches_list(request->client_id,
        request->full_name, &request->branches, &request->error) == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void branch_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *dialog = GTK_WIDGET(source);
    GtkWidget *status = g_object_get_data(G_OBJECT(dialog), "ghm-remote-branches-status");
    GtkWidget *list = g_object_get_data(G_OBJECT(dialog), "ghm-remote-branches-list");
    BranchTask *request = g_task_get_task_data(G_TASK(result));
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    g_object_set_data(G_OBJECT(dialog), "ghm-remote-branches-busy", NULL);
    if (!request->succeeded) {
        gtk_label_set_text(GTK_LABEL(status), request->error.message);
        return;
    }
    char *summary = g_strdup_printf("%zu branches on GitHub", request->branches.count);
    gtk_label_set_text(GTK_LABEL(status), summary);
    g_free(summary);
    for (size_t i = 0; i < request->branches.count; ++i) {
        const GhmGitHubBranch *branch = &request->branches.items[i];
        char *label_text = g_strdup_printf("%s%s  ·  %.12s",
            branch->name, branch->is_protected ? "  🔒" : "", branch->head_oid);
        GtkWidget *label = gtk_label_new(label_text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_set_margin_start(label, 10);
        gtk_widget_set_margin_top(label, 6);
        gtk_widget_set_margin_bottom(label, 6);
        gtk_list_box_append(GTK_LIST_BOX(list), label);
        g_free(label_text);
    }
}

static gboolean branch_close_requested(GtkWindow *window, gpointer user_data)
{
    (void)user_data;
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(window), "ghm-remote-branches-busy")) != 0;
}

static void branches_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *parent = GTK_WIDGET(user_data);
    ReposState *state = g_object_get_data(G_OBJECT(parent), "ghm-github-repos-state");
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(state->list));
    (void)button;
    if (row == NULL || state->busy) return;
    size_t index = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(row), "ghm-repo-index")) - 1;
    if (index >= state->repos.count) return;
    BranchTask *request = calloc(1, sizeof(*request));
    if (request == NULL) return;
    request->client_id = strdup(state->client_id);
    request->full_name = strdup(state->repos.items[index].full_name);
    if (request->client_id == NULL || request->full_name == NULL) {
        branch_task_free(request);
        return;
    }
    GtkWidget *dialog = gtk_window_new();
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *status = gtk_label_new("Loading GitHub branches…");
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *list = gtk_list_box_new();
    char *title = g_strdup_printf("GitHub Branches · %s", request->full_name);
    gtk_window_set_title(GTK_WINDOW(dialog), title);
    g_free(title);
    ghm_dialog_attach(dialog, parent);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 520, 460);
    gtk_widget_set_margin_start(root, 16);
    gtk_widget_set_margin_end(root, 16);
    gtk_widget_set_margin_top(root, 16);
    gtk_widget_set_margin_bottom(root, 16);
    gtk_label_set_xalign(GTK_LABEL(status), 0.0f);
    gtk_box_append(GTK_BOX(root), status);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(root), scroll);
    gtk_window_set_child(GTK_WINDOW(dialog), root);
    g_object_set_data(G_OBJECT(dialog), "ghm-remote-branches-status", status);
    g_object_set_data(G_OBJECT(dialog), "ghm-remote-branches-list", list);
    g_object_set_data(G_OBJECT(dialog), "ghm-remote-branches-busy", GINT_TO_POINTER(1));
    g_signal_connect(dialog, "close-request", G_CALLBACK(branch_close_requested), NULL);
    gtk_window_present(GTK_WINDOW(dialog));
    GTask *task = g_task_new(dialog, NULL, branch_finished, NULL);
    g_task_set_task_data(task, request, branch_task_free);
    g_task_run_in_thread(task, branch_worker);
    g_object_unref(task);
}

static gboolean repo_visible(GtkListBoxRow *row, gpointer user_data)
{
    ReposState *state = user_data;
    const char *query = gtk_editable_get_text(GTK_EDITABLE(state->search));
    size_t index = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(row), "ghm-repo-index"));
    if (query[0] == '\0' || index == 0 || index > state->repos.count) return TRUE;
    const GhmGitHubRepo *repo = &state->repos.items[index - 1];
    char *needle = g_utf8_casefold(query, -1);
    char *name = g_utf8_casefold(repo->full_name, -1);
    char *description = g_utf8_casefold(repo->description, -1);
    gboolean matches = strstr(name, needle) != NULL || strstr(description, needle) != NULL;
    g_free(needle);
    g_free(name);
    g_free(description);
    return matches;
}

static void search_changed(GtkEditable *editable, gpointer user_data)
{
    ReposState *state = user_data;
    (void)editable;
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(state->list));
    GtkListBoxRow *selected = gtk_list_box_get_selected_row(GTK_LIST_BOX(state->list));
    if (selected == NULL || !gtk_widget_get_visible(GTK_WIDGET(selected))) {
        gtk_list_box_unselect_all(GTK_LIST_BOX(state->list));
        GtkWidget *row = gtk_widget_get_first_child(state->list);
        while (row != NULL && !gtk_widget_get_visible(row))
            row = gtk_widget_get_next_sibling(row);
        if (row != NULL) gtk_list_box_select_row(GTK_LIST_BOX(state->list), GTK_LIST_BOX_ROW(row));
        gtk_widget_set_sensitive(state->open_button, row != NULL && !state->busy);
        gtk_widget_set_sensitive(state->branch_button, row != NULL && !state->busy);
    }
}

static void list_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
    ListTask *request = task_data;
    (void)source;
    (void)cancellable;
    request->succeeded = ghm_github_repos_list(request->client_id,
                                               &request->repos, &request->error) == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void list_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *dialog = GTK_WIDGET(source);
    ReposState *state = g_object_get_data(G_OBJECT(dialog), "ghm-github-repos-state");
    ListTask *request = g_task_get_task_data(G_TASK(result));
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (!gtk_widget_get_realized(state->parent)) return;
    state->busy = FALSE;
    if (!request->succeeded) {
        gtk_label_set_text(GTK_LABEL(state->status), request->error.message);
        return;
    }
    state->repos = request->repos;
    request->repos = (GhmGitHubRepoList){0};
    for (size_t i = 0; i < state->repos.count; ++i) {
        const GhmGitHubRepo *repo = &state->repos.items[i];
        char *text = g_strdup_printf("%s%s%s", repo->full_name,
                                     repo->is_private ? "  ·  Private" : "",
                                     repo->is_archived ? "  ·  Archived" : "");
        char *details = g_strdup_printf("%s%s%s  ·  ★ %" G_GINT64_FORMAT "  ·  Forks %" G_GINT64_FORMAT
                                        "  ·  Default %s", repo->description,
                                        repo->description[0] != '\0' ? "  ·" : "",
                                        repo->description[0] != '\0' ? "  " : "",
                                        repo->stargazers_count, repo->forks_count,
                                        repo->default_branch);
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        GtkWidget *title = gtk_label_new(text);
        GtkWidget *subtitle = gtk_label_new(details);
        gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
        gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(subtitle, "dim-label");
        gtk_widget_set_margin_start(content, 10);
        gtk_widget_set_margin_end(content, 10);
        gtk_widget_set_margin_top(content, 7);
        gtk_widget_set_margin_bottom(content, 7);
        gtk_box_append(GTK_BOX(content), title);
        gtk_box_append(GTK_BOX(content), subtitle);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), content);
        g_object_set_data(G_OBJECT(row), "ghm-repo-index", GSIZE_TO_POINTER(i + 1));
        gtk_list_box_append(GTK_LIST_BOX(state->list), row);
        g_free(text);
        g_free(details);
    }
    search_changed(GTK_EDITABLE(state->search), state);
    char *summary = g_strdup_printf("%zu repositories available", state->repos.count);
    gtk_label_set_text(GTK_LABEL(state->status), summary);
    g_free(summary);
}

static void clone_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
    CloneTask *request = task_data;
    (void)source;
    (void)cancellable;
    request->succeeded = ghm_repo_clone_github(request->context, request->client_id,
                                                &request->repo, &request->path,
                                                &request->error) == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void clone_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *dialog = GTK_WIDGET(source);
    ReposState *state = g_object_get_data(G_OBJECT(dialog), "ghm-github-repos-state");
    CloneTask *request = g_task_get_task_data(G_TASK(result));
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    state->busy = FALSE;
    if (!gtk_widget_get_realized(state->parent)) { gtk_window_destroy(GTK_WINDOW(dialog)); return; }
    if (request->succeeded) {
        state->opened(state->parent, request->path);
        gtk_window_destroy(GTK_WINDOW(dialog));
    } else {
        gtk_label_set_text(GTK_LABEL(state->status), request->error.message);
        gtk_widget_set_sensitive(state->open_button, TRUE);
        gtk_widget_set_sensitive(state->branch_button, TRUE);
        gtk_widget_set_sensitive(state->search, TRUE);
    }
}

static void open_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *dialog = user_data;
    ReposState *state = g_object_get_data(G_OBJECT(dialog), "ghm-github-repos-state");
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(state->list));
    CloneTask *request;
    GTask *task;
    (void)button;
    if (row == NULL || state->busy) return;
    size_t index = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(row), "ghm-repo-index")) - 1;
    if (index >= state->repos.count) return;
    const GhmGitHubRepo *repo = &state->repos.items[index];
    request = calloc(1, sizeof(*request));
    if (request == NULL) return;
    request->context = state->context;
    request->client_id = strdup(state->client_id);
    request->repo.github_id = repo->github_id;
    request->repo.full_name = strdup(repo->full_name);
    request->repo.clone_url = strdup(repo->clone_url);
    request->repo.default_branch = strdup(repo->default_branch);
    request->repo.description = strdup(repo->description);
    request->repo.html_url = strdup(repo->html_url);
    request->repo.stargazers_count = repo->stargazers_count;
    request->repo.forks_count = repo->forks_count;
    request->repo.is_private = repo->is_private;
    request->repo.is_archived = repo->is_archived;
    if (request->client_id == NULL || request->repo.full_name == NULL ||
        request->repo.clone_url == NULL || request->repo.default_branch == NULL ||
        request->repo.description == NULL || request->repo.html_url == NULL) {
        clone_task_free(request);
        gtk_label_set_text(GTK_LABEL(state->status), "Out of memory");
        return;
    }
    state->busy = TRUE;
    gtk_widget_set_sensitive(state->open_button, FALSE);
    gtk_widget_set_sensitive(state->branch_button, FALSE);
    gtk_widget_set_sensitive(state->search, FALSE);
    gtk_label_set_text(GTK_LABEL(state->status), "Opening or cloning repository…");
    task = g_task_new(dialog, NULL, clone_finished, NULL);
    g_task_set_task_data(task, request, clone_task_free);
    g_task_run_in_thread(task, clone_worker);
    g_object_unref(task);
}

static gboolean close_requested(GtkWindow *dialog, gpointer user_data)
{
    ReposState *state = user_data;
    (void)dialog;
    return state->busy;
}

static void cancel_clicked(GtkButton *button, gpointer user_data)
{
    ReposState *state = g_object_get_data(G_OBJECT(user_data), "ghm-github-repos-state");
    (void)button;
    if (!state->busy) gtk_window_destroy(GTK_WINDOW(user_data));
}

void ghm_github_repos_dialog_show(GtkWidget *parent, GhmContext *context,
                                  const char *client_id, GhmRepositoryOpened opened)
{
    GtkWidget *dialog = gtk_window_new();
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *cancel = gtk_button_new_with_label("Close");
    ReposState *state = calloc(1, sizeof(*state));
    ListTask *request;
    GTask *task;
    if (state == NULL) { gtk_window_destroy(GTK_WINDOW(dialog)); return; }
    state->parent = g_object_ref(parent);
    state->context = context;
    state->client_id = strdup(client_id);
    state->opened = opened;
    state->list = gtk_list_box_new();
    state->search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(state->search), "Search repositories");
    gtk_list_box_set_filter_func(GTK_LIST_BOX(state->list), repo_visible, state, NULL);
    g_signal_connect(state->search, "changed", G_CALLBACK(search_changed), state);
    state->status = gtk_label_new("Loading your GitHub repositories…");
    state->open_button = gtk_button_new_with_label("Clone / Open");
    state->branch_button = gtk_button_new_with_label("GitHub Branches");
    gtk_widget_set_sensitive(state->open_button, FALSE);
    gtk_widget_set_sensitive(state->branch_button, FALSE);
    gtk_window_set_title(GTK_WINDOW(dialog), "GitHub Repositories");
    ghm_dialog_attach(dialog, parent);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 560, 550);
    gtk_widget_set_margin_start(root, 16);
    gtk_widget_set_margin_end(root, 16);
    gtk_widget_set_margin_top(root, 16);
    gtk_widget_set_margin_bottom(root, 16);
    gtk_label_set_xalign(GTK_LABEL(state->status), 0.0f);
    gtk_box_append(GTK_BOX(root), state->status);
    gtk_box_append(GTK_BOX(root), state->search);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), state->list);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(root), scroll);
    gtk_widget_set_hexpand(cancel, TRUE);
    gtk_widget_set_halign(cancel, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(actions), cancel);
    gtk_box_append(GTK_BOX(actions), state->branch_button);
    gtk_box_append(GTK_BOX(actions), state->open_button);
    gtk_box_append(GTK_BOX(root), actions);
    gtk_window_set_child(GTK_WINDOW(dialog), root);
    g_object_set_data_full(G_OBJECT(dialog), "ghm-github-repos-state", state, state_free);
    g_signal_connect(dialog, "close-request", G_CALLBACK(close_requested), state);
    g_signal_connect(cancel, "clicked", G_CALLBACK(cancel_clicked), dialog);
    g_signal_connect(state->open_button, "clicked", G_CALLBACK(open_clicked), dialog);
    g_signal_connect(state->branch_button, "clicked", G_CALLBACK(branches_clicked), dialog);
    gtk_window_present(GTK_WINDOW(dialog));
    if (state->client_id == NULL) {
        gtk_label_set_text(GTK_LABEL(state->status), "GitHub login is unavailable");
        return;
    }
    state->busy = TRUE;
    request = calloc(1, sizeof(*request));
    if (request == NULL) {
        state->busy = FALSE;
        gtk_label_set_text(GTK_LABEL(state->status), "Out of memory");
        return;
    }
    request->client_id = strdup(state->client_id);
    if (request->client_id == NULL) {
        list_task_free(request);
        state->busy = FALSE;
        gtk_label_set_text(GTK_LABEL(state->status), "Out of memory");
        return;
    }
    task = g_task_new(dialog, NULL, list_finished, NULL);
    g_task_set_task_data(task, request, list_task_free);
    g_task_run_in_thread(task, list_worker);
    g_object_unref(task);
}
