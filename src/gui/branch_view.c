#include "gui/branch_view.h"
#include "gui/dialog.h"

#include <ghm/branch.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    GtkWidget *parent;
    GhmContext *context;
    GhmBranchCanSwitch can_switch;
    GhmBranchChanged changed;
    GtkWidget *list;
    GtkWidget *name_entry;
    GtkWidget *message;
    char *path;
    unsigned generation;
} BranchState;

typedef struct {
    char *path;
    GhmBranchList branches;
    GhmError error;
    unsigned generation;
    gboolean succeeded;
} BranchListTask;

typedef struct {
    GhmContext *context;
    char *path;
    char *name;
    GhmError error;
    gboolean create;
    gboolean merge;
    GhmMergeOutcome outcome;
    char oid[GHM_OID_HEX_CAPACITY];
    gboolean succeeded;
} BranchActionTask;

static void branch_state_free(gpointer data)
{
    BranchState *state = data;
    free(state->path);
    free(state);
}

static void branch_list_task_free(gpointer data)
{
    BranchListTask *request = data;
    free(request->path);
    ghm_branch_list_free(&request->branches);
    free(request);
}

static void branch_action_task_free(gpointer data)
{
    BranchActionTask *request = data;
    free(request->path);
    free(request->name);
    free(request);
}

static void clear_branches(GtkWidget *list)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(list)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(list), child);
}

static void action_worker(GTask *task, gpointer source, gpointer task_data,
                          GCancellable *cancellable)
{
    BranchActionTask *request = task_data;
    (void)source;
    (void)cancellable;
    request->succeeded = (request->merge ?
        ghm_branch_merge(request->context, request->path, request->name,
                         &request->outcome, request->oid, &request->error) :
        request->create ? ghm_branch_create(request->path, request->name, &request->error) :
        ghm_branch_checkout(request->context, request->path,
                            request->name, &request->error)) == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void action_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *view = GTK_WIDGET(user_data);
    BranchState *state = g_object_get_data(G_OBJECT(view), "ghm-branch-state");
    BranchActionTask *request = g_task_get_task_data(G_TASK(result));
    (void)source;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (!gtk_widget_get_realized(GTK_WIDGET(source))) { g_object_unref(view); return; }
    if (g_strcmp0(state->path, request->path) == 0) {
        if (request->succeeded) {
            gtk_label_set_text(GTK_LABEL(state->message), request->merge ?
                               request->outcome == GHM_MERGE_COMMITTED ? "Merge commit created." :
                               request->outcome == GHM_MERGE_FAST_FORWARDED ? "Branch fast-forwarded." :
                               "Already up to date." : request->create ?
                               "Branch created." : "Branch checked out.");
            if (!request->create) state->changed(state->parent);
            ghm_branch_view_refresh(view, state->path);
        } else gtk_label_set_text(GTK_LABEL(state->message), request->error.message);
    }
    g_object_unref(view);
}

static void start_action(GtkWidget *view, const char *name, gboolean create, gboolean merge)
{
    BranchState *state = g_object_get_data(G_OBJECT(view), "ghm-branch-state");
    BranchActionTask *request;
    GTask *task;
    if (state->path == NULL || name == NULL || name[0] == '\0') {
        gtk_label_set_text(GTK_LABEL(state->message), "Enter or select a branch name.");
        return;
    }
    if (!create && !state->can_switch(state->parent)) return;
    request = calloc(1, sizeof(*request));
    if (request == NULL) return;
    request->context = state->context;
    request->path = strdup(state->path);
    request->name = strdup(name);
    request->create = create;
    request->merge = merge;
    if (request->path == NULL || request->name == NULL) {
        branch_action_task_free(request);
        gtk_label_set_text(GTK_LABEL(state->message), "Out of memory.");
        return;
    }
    gtk_label_set_text(GTK_LABEL(state->message), merge ? "Merging local branch…" :
                       create ? "Creating branch…" : "Switching branch…");
    task = g_task_new(state->parent, NULL, action_finished, g_object_ref(view));
    g_task_set_task_data(task, request, branch_action_task_free);
    g_task_run_in_thread(task, action_worker);
    g_object_unref(task);
}

static void create_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *view = GTK_WIDGET(user_data);
    BranchState *state = g_object_get_data(G_OBJECT(view), "ghm-branch-state");
    (void)button;
    start_action(view, gtk_editable_get_text(GTK_EDITABLE(state->name_entry)), TRUE, FALSE);
}

static void checkout_clicked(GtkButton *button, gpointer user_data)
{
    const char *name = g_object_get_data(G_OBJECT(button), "ghm-branch-name");
    start_action(GTK_WIDGET(user_data), name, FALSE, FALSE);
}

static void merge_clicked(GtkButton *button, gpointer user_data)
{
    const char *name = g_object_get_data(G_OBJECT(button), "ghm-branch-name");
    start_action(GTK_WIDGET(user_data), name, FALSE, TRUE);
}

static void list_worker(GTask *task, gpointer source, gpointer task_data,
                        GCancellable *cancellable)
{
    BranchListTask *request = task_data;
    (void)source;
    (void)cancellable;
    request->succeeded = ghm_branch_list(request->path, &request->branches, &request->error) == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void list_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *view = GTK_WIDGET(source);
    BranchState *state = g_object_get_data(G_OBJECT(view), "ghm-branch-state");
    BranchListTask *request = g_task_get_task_data(G_TASK(result));
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (!ghm_view_is_open(view)) return;
    if (request->generation != state->generation || g_strcmp0(state->path, request->path) != 0) return;
    clear_branches(state->list);
    if (!request->succeeded) {
        gtk_label_set_text(GTK_LABEL(state->message), request->error.message);
        return;
    }
    for (size_t i = 0; i < request->branches.count; ++i) {
        const GhmBranch *branch = &request->branches.items[i];
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        char *text = g_strdup_printf("%s%s", branch->is_current ? "● " : "  ", branch->name);
        GtkWidget *label = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_box_append(GTK_BOX(row), label);
        if (!branch->is_remote && !branch->is_current) {
            GtkWidget *button = gtk_button_new_with_label("Checkout");
            GtkWidget *merge = gtk_button_new_with_label("Merge into current");
            g_object_set_data_full(G_OBJECT(button), "ghm-branch-name",
                                   g_strdup(branch->name), g_free);
            g_object_set_data_full(G_OBJECT(merge), "ghm-branch-name",
                                   g_strdup(branch->name), g_free);
            g_signal_connect(button, "clicked", G_CALLBACK(checkout_clicked), view);
            g_signal_connect(merge, "clicked", G_CALLBACK(merge_clicked), view);
            gtk_box_append(GTK_BOX(row), button);
            gtk_box_append(GTK_BOX(row), merge);
        }
        gtk_list_box_append(GTK_LIST_BOX(state->list), row);
        g_free(text);
    }
    if (request->branches.count == 0)
        gtk_label_set_text(GTK_LABEL(state->message), "No branches yet. Create a commit first.");
    else gtk_label_set_text(GTK_LABEL(state->message), "");
}

void ghm_branch_view_refresh(GtkWidget *view, const char *repository_path)
{
    BranchState *state = g_object_get_data(G_OBJECT(view), "ghm-branch-state");
    BranchListTask *request;
    GTask *task;
    char *new_path = repository_path != NULL ? strdup(repository_path) : NULL;
    ++state->generation;
    free(state->path);
    state->path = new_path;
    clear_branches(state->list);
    if (state->path == NULL) {
        gtk_label_set_text(GTK_LABEL(state->message), "Select a repository to view branches.");
        return;
    }
    request = calloc(1, sizeof(*request));
    if (request == NULL) return;
    request->path = strdup(state->path);
    request->generation = state->generation;
    if (request->path == NULL) { branch_list_task_free(request); return; }
    gtk_label_set_text(GTK_LABEL(state->message), "Loading branches…");
    task = g_task_new(view, NULL, list_finished, NULL);
    g_task_set_task_data(task, request, branch_list_task_free);
    g_task_run_in_thread(task, list_worker);
    g_object_unref(task);
}

static void refresh_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *view = GTK_WIDGET(user_data);
    BranchState *state = g_object_get_data(G_OBJECT(view), "ghm-branch-state");
    (void)button;
    ghm_branch_view_refresh(view, state->path);
}

GtkWidget *ghm_branch_view_new(GtkWidget *parent, GhmContext *context,
                               GhmBranchCanSwitch can_switch, GhmBranchChanged changed)
{
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *refresh = gtk_button_new_with_label("Refresh Branches");
    GtkWidget *create = gtk_button_new_with_label("Create Branch");
    GtkWidget *scroll = gtk_scrolled_window_new();
    BranchState *state = g_new0(BranchState, 1);
    state->parent = parent;
    state->context = context;
    state->can_switch = can_switch;
    state->changed = changed;
    state->list = gtk_list_box_new();
    state->name_entry = gtk_entry_new();
    state->message = gtk_label_new("Select a repository to view branches.");
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->name_entry), "New branch name");
    gtk_widget_set_hexpand(state->name_entry, TRUE);
    gtk_label_set_xalign(GTK_LABEL(state->message), 0.0f);
    gtk_box_append(GTK_BOX(controls), state->name_entry);
    gtk_box_append(GTK_BOX(controls), create);
    gtk_box_append(GTK_BOX(controls), refresh);
    gtk_box_append(GTK_BOX(root), controls);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), state->list);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(root), scroll);
    gtk_box_append(GTK_BOX(root), state->message);
    g_object_set_data_full(G_OBJECT(root), "ghm-branch-state", state, branch_state_free);
    g_signal_connect(create, "clicked", G_CALLBACK(create_clicked), root);
    g_signal_connect(refresh, "clicked", G_CALLBACK(refresh_clicked), root);
    return root;
}
