#include "gui/dialog.h"
#include "gui/file_view.h"

#include <ghm/file_ops.h>
#include <ghm/ghm.h>

#include <string.h>

typedef struct {
    GtkWidget *files;
    GtkWidget *preview;
    GtkWidget *preview_title;
    GtkWidget *hint;
    GtkWidget *save_button;
    GtkWidget *discard_button;
    GtkWidget *new_file_button;
    GtkWidget *new_directory_button;
    GtkWidget *rename_button;
    GtkWidget *delete_button;
    GtkWidget *parent;
    GhmFilesChanged changed;
    char *repository_path;
    char *preview_path;
    char *selected_path;
    GtkListBoxRow *selected_row;
    int selected_is_directory;
    GhmFileVersion preview_version;
    gboolean preview_editable;
    gboolean busy;
    unsigned int active_operations;
    unsigned long generation;
    unsigned long preview_generation;
    GHashTable *expanded;
} FileViewState;

typedef struct {
    char *repository_path;
    GhmFileList files;
    GhmStatus status;
    GhmError error;
    gboolean succeeded;
    unsigned long generation;
} FilesTask;

typedef struct {
    char *repository_path;
    char *relative_path;
    char *contents;
    char *message;
    size_t length;
    GhmFileVersion version;
    unsigned long generation;
    gboolean succeeded;
} PreviewTask;

typedef struct {
    char *repository_path;
    char *relative_path;
    char *contents;
    size_t length;
    GhmFileVersion expected;
    GhmFileVersion saved;
    GhmError error;
    gboolean succeeded;
} SaveTask;

typedef enum {
    FILE_OP_CREATE,
    FILE_OP_DIRECTORY,
    FILE_OP_RENAME,
    FILE_OP_DELETE
} FileOperation;

typedef struct {
    GtkWidget *view;
    GtkWidget *entry;
    GtkWidget *feedback;
    GtkWidget *submit;
    GtkWidget *cancel;
    char *repository_path;
    char *old_path;
    FileOperation operation;
    gboolean busy;
    gboolean active_counted;
} OperationDialog;

typedef struct {
    char *repository_path;
    char *old_path;
    char *new_path;
    FileOperation operation;
    GhmError error;
    gboolean succeeded;
} OperationTask;

static void selected(GtkListBox *list, GtkListBoxRow *row, gpointer user_data);

static void state_free(gpointer data)
{
    FileViewState *state = data;
    g_free(state->repository_path);
    g_free(state->preview_path);
    g_free(state->selected_path);
    g_hash_table_unref(state->expanded);
    g_free(state);
}

static void files_task_free(gpointer data)
{
    FilesTask *task = data;
    g_free(task->repository_path);
    ghm_file_list_free(&task->files);
    ghm_status_free(&task->status);
    g_free(task);
}

static void preview_task_free(gpointer data)
{
    PreviewTask *task = data;
    g_free(task->repository_path);
    g_free(task->relative_path);
    free(task->contents);
    g_free(task->message);
    g_free(task);
}

static void save_task_free(gpointer data)
{
    SaveTask *task = data;
    g_free(task->repository_path);
    g_free(task->relative_path);
    g_free(task->contents);
    g_free(task);
}

static void operation_dialog_free(gpointer data)
{
    OperationDialog *state = data;
    FileViewState *view_state = g_object_get_data(G_OBJECT(state->view), "ghm-file-view-state");
    if (state->active_counted && view_state->active_operations > 0U)
        --view_state->active_operations;
    g_object_unref(state->view);
    g_free(state->repository_path);
    g_free(state->old_path);
    g_free(state);
}

static void operation_task_free(gpointer data)
{
    OperationTask *request = data;
    g_free(request->repository_path);
    g_free(request->old_path);
    g_free(request->new_path);
    g_free(request);
}

static void clear_list(GtkWidget *list)
{
    GtkWidget *row;
    while ((row = gtk_widget_get_first_child(list)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(list), row);
}

static gboolean row_is_visible(FileViewState *state, const char *path)
{
    char *prefix = g_strdup(path);
    char *slash;
    gboolean visible = TRUE;
    if (prefix == NULL) return FALSE;
    for (slash = strchr(prefix, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        if (!g_hash_table_contains(state->expanded, prefix)) visible = FALSE;
        *slash = '/';
        if (!visible) break;
    }
    g_free(prefix);
    return visible;
}

static void update_rows(FileViewState *state)
{
    for (GtkWidget *row = gtk_widget_get_first_child(state->files); row != NULL;
         row = gtk_widget_get_next_sibling(row)) {
        const char *path = g_object_get_data(G_OBJECT(row), "ghm-file-path");
        gboolean is_directory = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "ghm-is-directory"));
        gtk_widget_set_visible(row, path != NULL && row_is_visible(state, path));
        if (is_directory) {
            GtkWidget *label = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
            const char *name = strrchr(path, '/');
            const char *marker = g_object_get_data(G_OBJECT(row), "ghm-file-marker");
            char *text = g_strdup_printf("%s  %s%s%s",
                g_hash_table_contains(state->expanded, path) ? "▾" : "▸",
                name != NULL ? name + 1 : path,
                marker != NULL && marker[0] != '\0' ? "  " : "",
                marker != NULL ? marker : "");
            gtk_label_set_text(GTK_LABEL(label), text);
            g_free(text);
        }
    }
}

static void status_markers(const GhmStatus *status, GHashTable *files, GHashTable *directories)
{
    for (size_t i = 0; i < status->count; ++i) {
        g_hash_table_insert(files, g_strdup(status->items[i].path),
                            (gpointer)ghm_status_kind_label(status->items[i].kind));
        char *parent = g_strdup(status->items[i].path);
        char *slash;
        while ((slash = strrchr(parent, '/')) != NULL) {
            *slash = '\0';
            g_hash_table_add(directories, g_strdup(parent));
        }
        g_free(parent);
    }
}

static void files_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
    FilesTask *request = task_data;
    (void)source;
    (void)cancellable;
    request->succeeded = ghm_repo_files(request->repository_path, &request->files,
                                        &request->error) == 0 &&
                         ghm_repo_status(request->repository_path, &request->status,
                                         &request->error) == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void files_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *view = GTK_WIDGET(source);
    FileViewState *state = g_object_get_data(G_OBJECT(view), "ghm-file-view-state");
    FilesTask *request = g_task_get_task_data(G_TASK(result));
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (!ghm_view_is_open(view)) return;
    if (g_strcmp0(state->repository_path, request->repository_path) != 0 ||
        request->generation != state->generation) return;
    state->selected_row = NULL;
    clear_list(state->files);
    if (!request->succeeded) {
        gtk_label_set_text(GTK_LABEL(state->hint), request->error.message);
        return;
    }
    GHashTable *markers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *directories = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    status_markers(&request->status, markers, directories);
    for (size_t i = 0; i < request->files.count; ++i) {
        const GhmFileEntry *entry = &request->files.items[i];
        const char *name = strrchr(entry->path, '/');
        const char *marker = entry->is_directory ?
            (g_hash_table_contains(directories, entry->path) ? "•" : "") :
            g_hash_table_lookup(markers, entry->path);
        if (marker == NULL) marker = "";
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *label = gtk_label_new(NULL);
        size_t depth = 0;
        char *text;
        for (const char *cursor = entry->path; *cursor != '\0'; ++cursor)
            if (*cursor == '/') ++depth;
        text = g_strdup_printf("%s%s%s%s", entry->is_directory ? "▸  " : "    ",
                               name != NULL ? name + 1 : entry->path,
                               marker[0] != '\0' ? "  " : "", marker);
        gtk_label_set_text(GTK_LABEL(label), text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_widget_set_margin_start(label, 8 + (int)depth * 16);
        gtk_widget_set_margin_top(label, 3);
        gtk_widget_set_margin_bottom(label, 3);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        g_object_set_data_full(G_OBJECT(row), "ghm-file-path", g_strdup(entry->path), g_free);
        g_object_set_data_full(G_OBJECT(row), "ghm-file-marker", g_strdup(marker), g_free);
        g_object_set_data(G_OBJECT(row), "ghm-is-directory", GINT_TO_POINTER(entry->is_directory));
        gtk_list_box_append(GTK_LIST_BOX(state->files), row);
        g_free(text);
    }
    g_hash_table_unref(markers);
    g_hash_table_unref(directories);
    update_rows(state);
    if (state->selected_path != NULL) {
        for (GtkWidget *row = gtk_widget_get_first_child(state->files); row != NULL;
             row = gtk_widget_get_next_sibling(row)) {
            if (g_strcmp0(state->selected_path,
                          g_object_get_data(G_OBJECT(row), "ghm-file-path")) == 0) {
                state->selected_row = GTK_LIST_BOX_ROW(row);
                gtk_list_box_select_row(GTK_LIST_BOX(state->files), state->selected_row);
                break;
            }
        }
    }
    if (request->files.truncated)
        gtk_label_set_text(GTK_LABEL(state->hint), "Large repository: showing the first 5,000 entries. Changes still show all Git status paths.");
    else gtk_label_set_text(GTK_LABEL(state->hint), "Select a file to preview; double-click folders to expand.");
}

static void preview_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
    PreviewTask *request = task_data;
    GhmError error = {0};
    (void)source;
    (void)cancellable;
    request->succeeded = ghm_file_read_text(request->repository_path, request->relative_path,
                                             &request->contents, &request->length,
                                             &request->version, &error) == 0;
    if (!request->succeeded) request->message = g_strdup(error.message);
    else if (!g_utf8_validate(request->contents, (gssize)request->length, NULL)) {
        free(request->contents);
        request->contents = NULL;
        request->message = g_strdup("Binary or non-UTF-8 file; preview unavailable.");
        request->succeeded = FALSE;
    }
    g_task_return_boolean(task, request->succeeded);
}

static void preview_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *view = GTK_WIDGET(source);
    FileViewState *state = g_object_get_data(G_OBJECT(view), "ghm-file-view-state");
    PreviewTask *request = g_task_get_task_data(G_TASK(result));
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->preview));
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    if (!ghm_view_is_open(view) ||
        request->generation != state->preview_generation ||
        gtk_text_buffer_get_modified(buffer) ||
        g_strcmp0(state->repository_path, request->repository_path) != 0 ||
        g_strcmp0(state->preview_path, request->relative_path) != 0) return;
    gtk_label_set_text(GTK_LABEL(state->preview_title), request->relative_path);
    gtk_text_buffer_set_text(buffer, request->contents != NULL ? request->contents :
                            request->message != NULL ? request->message : "Preview unavailable", -1);
    gtk_text_buffer_set_modified(buffer, FALSE);
    state->preview_editable = request->succeeded;
    if (request->succeeded) state->preview_version = request->version;
    gtk_text_view_set_editable(GTK_TEXT_VIEW(state->preview), request->succeeded);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(state->preview), request->succeeded);
    gtk_widget_set_sensitive(state->save_button, FALSE);
}

static void modified_changed(GtkTextBuffer *buffer, gpointer user_data)
{
    FileViewState *state = g_object_get_data(G_OBJECT(user_data), "ghm-file-view-state");
    gboolean modified = gtk_text_buffer_get_modified(buffer);
    if (modified) ++state->preview_generation;
    if (state->preview_path != NULL) {
        char *title = g_strdup_printf("%s%s", modified ? "● " : "", state->preview_path);
        gtk_label_set_text(GTK_LABEL(state->preview_title), title);
        g_free(title);
    }
    gtk_widget_set_sensitive(state->save_button, modified && state->preview_editable && !state->busy);
    gtk_widget_set_sensitive(state->discard_button, modified && !state->busy);
    gtk_widget_set_sensitive(state->new_file_button, !modified && !state->busy);
    gtk_widget_set_sensitive(state->new_directory_button, !modified && !state->busy);
    gtk_widget_set_sensitive(state->rename_button, !modified && !state->busy && state->selected_path != NULL);
    gtk_widget_set_sensitive(state->delete_button, !modified && !state->busy &&
                             state->selected_path != NULL && !state->selected_is_directory);
}

static void save_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
    SaveTask *request = task_data;
    (void)source;
    (void)cancellable;
    request->succeeded = ghm_file_save_text(request->repository_path, request->relative_path,
                                             request->contents, request->length,
                                             &request->expected, &request->saved,
                                             &request->error) == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void save_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *view = GTK_WIDGET(source);
    FileViewState *state = g_object_get_data(G_OBJECT(view), "ghm-file-view-state");
    SaveTask *request = g_task_get_task_data(G_TASK(result));
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->preview));
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    state->busy = FALSE;
    if (!ghm_view_is_open(view)) return;
    if (g_strcmp0(state->repository_path, request->repository_path) != 0 ||
        g_strcmp0(state->preview_path, request->relative_path) != 0) return;
    gtk_text_view_set_editable(GTK_TEXT_VIEW(state->preview), TRUE);
    if (request->succeeded) {
        state->preview_version = request->saved;
        gtk_text_buffer_set_modified(buffer, FALSE);
        gtk_label_set_text(GTK_LABEL(state->hint), "Saved locally. Stage the file in Changes when ready.");
        state->changed(state->parent);
    } else {
        gtk_label_set_text(GTK_LABEL(state->hint), request->error.message);
        gtk_widget_set_sensitive(state->save_button, TRUE);
    }
}

static void save_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *view = user_data;
    FileViewState *state = g_object_get_data(G_OBJECT(view), "ghm-file-view-state");
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->preview));
    GtkTextIter start, end;
    SaveTask *request;
    GTask *task;
    (void)button;
    if (state->repository_path == NULL || state->preview_path == NULL ||
        !state->preview_editable || state->busy || !gtk_text_buffer_get_modified(buffer)) return;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    request = g_new0(SaveTask, 1);
    request->repository_path = g_strdup(state->repository_path);
    request->relative_path = g_strdup(state->preview_path);
    request->contents = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    request->length = request->contents != NULL ? strlen(request->contents) : 0;
    request->expected = state->preview_version;
    if (request->repository_path == NULL || request->relative_path == NULL ||
        request->contents == NULL) {
        save_task_free(request);
        gtk_label_set_text(GTK_LABEL(state->hint), "Out of memory while saving.");
        return;
    }
    state->busy = TRUE;
    gtk_text_view_set_editable(GTK_TEXT_VIEW(state->preview), FALSE);
    gtk_widget_set_sensitive(state->save_button, FALSE);
    gtk_label_set_text(GTK_LABEL(state->hint), "Saving file…");
    task = g_task_new(view, NULL, save_finished, NULL);
    g_task_set_task_data(task, request, save_task_free);
    g_task_run_in_thread(task, save_worker);
    g_object_unref(task);
}

static gboolean editor_key_pressed(GtkEventControllerKey *controller, guint keyval,
                                    guint keycode, GdkModifierType modifiers,
                                    gpointer user_data)
{
    (void)controller;
    (void)keycode;
    if ((modifiers & GDK_CONTROL_MASK) != 0 && (keyval == GDK_KEY_s || keyval == GDK_KEY_S)) {
        save_clicked(NULL, user_data);
        return TRUE;
    }
    return FALSE;
}

static void discard_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *view = user_data;
    FileViewState *state = g_object_get_data(G_OBJECT(view), "ghm-file-view-state");
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->preview));
    (void)button;
    if (state->busy || state->selected_row == NULL) return;
    gtk_text_buffer_set_modified(buffer, FALSE);
    selected(GTK_LIST_BOX(state->files), state->selected_row, view);
    state->changed(state->parent);
}

static void operation_worker(GTask *task, gpointer source, gpointer task_data,
                             GCancellable *cancellable)
{
    OperationTask *request = task_data;
    int result = -1;
    (void)source;
    (void)cancellable;
    switch (request->operation) {
    case FILE_OP_CREATE:
        result = ghm_file_create(request->repository_path, request->new_path, &request->error);
        break;
    case FILE_OP_DIRECTORY:
        result = ghm_directory_create(request->repository_path, request->new_path, &request->error);
        break;
    case FILE_OP_RENAME:
        result = ghm_file_rename(request->repository_path, request->old_path,
                                 request->new_path, &request->error);
        break;
    case FILE_OP_DELETE:
        result = ghm_file_delete(request->repository_path, request->old_path, &request->error);
        break;
    }
    request->succeeded = result == 0;
    g_task_return_boolean(task, request->succeeded);
}

static void operation_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *dialog = GTK_WIDGET(source);
    OperationDialog *operation = g_object_get_data(G_OBJECT(dialog), "ghm-file-operation");
    OperationTask *request = g_task_get_task_data(G_TASK(result));
    FileViewState *state = g_object_get_data(G_OBJECT(operation->view), "ghm-file-view-state");
    (void)user_data;
    (void)g_task_propagate_boolean(G_TASK(result), NULL);
    operation->busy = FALSE;
    if (!ghm_view_is_open(operation->view)) {
        gtk_window_destroy(GTK_WINDOW(dialog));
        return;
    }
    if (!request->succeeded) {
        gtk_label_set_text(GTK_LABEL(operation->feedback), request->error.message);
        gtk_widget_set_sensitive(operation->submit, TRUE);
        gtk_widget_set_sensitive(operation->cancel, TRUE);
        return;
    }
    if (request->operation == FILE_OP_DELETE || request->operation == FILE_OP_RENAME) {
        size_t old_length = strlen(request->old_path);
        if (g_strcmp0(state->preview_path, request->old_path) == 0 ||
            (request->operation == FILE_OP_RENAME && state->preview_path != NULL &&
             strncmp(state->preview_path, request->old_path, old_length) == 0 &&
             state->preview_path[old_length] == '/')) {
            g_clear_pointer(&state->preview_path, g_free);
            state->preview_editable = FALSE;
            GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->preview));
            gtk_text_buffer_set_text(buffer, "", -1);
            gtk_text_buffer_set_modified(buffer, FALSE);
            gtk_text_view_set_editable(GTK_TEXT_VIEW(state->preview), FALSE);
            gtk_label_set_text(GTK_LABEL(state->preview_title), "Select a file");
        }
        g_clear_pointer(&state->selected_path, g_free);
        state->selected_row = NULL;
    }
    if (operation->active_counted && state->active_operations > 0U) {
        --state->active_operations;
        operation->active_counted = FALSE;
    }
    gtk_window_destroy(GTK_WINDOW(dialog));
    state->changed(state->parent);
}

static void operation_submit(GtkButton *button, gpointer user_data)
{
    GtkWidget *dialog = user_data;
    OperationDialog *operation = g_object_get_data(G_OBJECT(dialog), "ghm-file-operation");
    OperationTask *request;
    GTask *task;
    (void)button;
    if (operation->busy) return;
    if (operation->operation != FILE_OP_DELETE &&
        gtk_editable_get_text(GTK_EDITABLE(operation->entry))[0] == '\0') {
        gtk_label_set_text(GTK_LABEL(operation->feedback), "Enter a repository-relative path.");
        return;
    }
    request = g_new0(OperationTask, 1);
    request->repository_path = g_strdup(operation->repository_path);
    request->old_path = g_strdup(operation->old_path);
    request->new_path = operation->operation == FILE_OP_DELETE ? NULL :
                        g_strdup(gtk_editable_get_text(GTK_EDITABLE(operation->entry)));
    request->operation = operation->operation;
    if (request->repository_path == NULL ||
        (operation->old_path != NULL && request->old_path == NULL) ||
        (operation->operation != FILE_OP_DELETE && request->new_path == NULL)) {
        operation_task_free(request);
        gtk_label_set_text(GTK_LABEL(operation->feedback), "Out of memory.");
        return;
    }
    operation->busy = TRUE;
    gtk_widget_set_sensitive(operation->submit, FALSE);
    gtk_widget_set_sensitive(operation->cancel, FALSE);
    gtk_label_set_text(GTK_LABEL(operation->feedback), "Applying file change…");
    task = g_task_new(dialog, NULL, operation_finished, NULL);
    g_task_set_task_data(task, request, operation_task_free);
    g_task_run_in_thread(task, operation_worker);
    g_object_unref(task);
}

static gboolean operation_close_requested(GtkWindow *dialog, gpointer user_data)
{
    OperationDialog *operation = user_data;
    (void)dialog;
    return operation->busy;
}

static void operation_cancel(GtkButton *button, gpointer user_data)
{
    (void)button;
    gtk_window_destroy(GTK_WINDOW(user_data));
}

static void operation_clicked(GtkButton *button, gpointer user_data)
{
    GtkWidget *view = user_data;
    FileViewState *state = g_object_get_data(G_OBJECT(view), "ghm-file-view-state");
    FileOperation kind = (FileOperation)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "ghm-operation"));
    GtkWidget *dialog;
    GtkWidget *root;
    GtkWidget *actions;
    GtkWidget *cancel;
    OperationDialog *operation;
    const char *title;
    char *prefill = NULL;
    if (state->repository_path == NULL || state->busy ||
        gtk_text_buffer_get_modified(gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->preview)))) return;
    if ((kind == FILE_OP_RENAME || kind == FILE_OP_DELETE) && state->selected_path == NULL) return;
    if (kind == FILE_OP_DELETE && state->selected_is_directory) return;
    title = kind == FILE_OP_CREATE ? "New File" : kind == FILE_OP_DIRECTORY ? "New Folder" :
            kind == FILE_OP_RENAME ? "Rename" : "Delete File";
    dialog = gtk_window_new();
    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    cancel = gtk_button_new_with_label("Cancel");
    operation = g_new0(OperationDialog, 1);
    operation->view = g_object_ref(view);
    ++state->active_operations;
    operation->active_counted = TRUE;
    operation->repository_path = g_strdup(state->repository_path);
    operation->old_path = kind == FILE_OP_RENAME || kind == FILE_OP_DELETE ?
                          g_strdup(state->selected_path) : NULL;
    operation->operation = kind;
    operation->entry = gtk_entry_new();
    operation->feedback = gtk_label_new("");
    operation->submit = gtk_button_new_with_label(kind == FILE_OP_DELETE ? "Delete File" :
        kind == FILE_OP_RENAME ? "Rename" : "Create");
    operation->cancel = cancel;
    gtk_widget_add_css_class(operation->submit,
                             kind == FILE_OP_DELETE ? "destructive-action" : "suggested-action");
    gtk_window_set_title(GTK_WINDOW(dialog), title);
    ghm_dialog_attach(dialog, state->parent);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 520, 170);
    gtk_widget_set_margin_start(root, 18);
    gtk_widget_set_margin_end(root, 18);
    gtk_widget_set_margin_top(root, 18);
    gtk_widget_set_margin_bottom(root, 18);
    if (kind == FILE_OP_DELETE) {
        char *question = g_strdup_printf("Delete %s from the local working tree? This cannot be undone here.",
                                         state->selected_path);
        GtkWidget *label = gtk_label_new(question);
        gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_box_append(GTK_BOX(root), label);
        g_free(question);
    } else {
        if (kind == FILE_OP_RENAME) prefill = g_strdup(state->selected_path);
        else if (state->selected_path != NULL) {
            if (state->selected_is_directory)
                prefill = g_strdup_printf("%s/", state->selected_path);
            else {
                const char *slash = strrchr(state->selected_path, '/');
                if (slash != NULL) prefill = g_strndup(state->selected_path,
                                                       (gsize)(slash - state->selected_path + 1));
            }
        }
        gtk_box_append(GTK_BOX(root), gtk_label_new("Repository-relative path"));
        gtk_editable_set_text(GTK_EDITABLE(operation->entry), prefill != NULL ? prefill : "");
        gtk_box_append(GTK_BOX(root), operation->entry);
        g_free(prefill);
    }
    gtk_label_set_xalign(GTK_LABEL(operation->feedback), 0.0f);
    gtk_box_append(GTK_BOX(root), operation->feedback);
    gtk_widget_set_hexpand(cancel, TRUE);
    gtk_widget_set_halign(cancel, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(actions), cancel);
    gtk_box_append(GTK_BOX(actions), operation->submit);
    gtk_box_append(GTK_BOX(root), actions);
    gtk_window_set_child(GTK_WINDOW(dialog), root);
    g_object_set_data_full(G_OBJECT(dialog), "ghm-file-operation", operation, operation_dialog_free);
    g_signal_connect(dialog, "close-request", G_CALLBACK(operation_close_requested), operation);
    g_signal_connect(cancel, "clicked", G_CALLBACK(operation_cancel), dialog);
    g_signal_connect(operation->submit, "clicked", G_CALLBACK(operation_submit), dialog);
    gtk_window_present(GTK_WINDOW(dialog));
}

static void selected(GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
    GtkWidget *view = user_data;
    FileViewState *state = g_object_get_data(G_OBJECT(view), "ghm-file-view-state");
    const char *path;
    PreviewTask *request;
    GTask *task;
    if (row == NULL) return;
    path = g_object_get_data(G_OBJECT(row), "ghm-file-path");
    gboolean modified = gtk_text_buffer_get_modified(
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->preview)));
    if (state->busy || (modified && g_strcmp0(state->preview_path, path) != 0)) {
        gtk_label_set_text(GTK_LABEL(state->hint), "Save or discard your edits before switching files.");
        if (state->selected_row != NULL && state->selected_row != row)
            gtk_list_box_select_row(list, state->selected_row);
        return;
    }
    if (modified) { state->selected_row = row; return; }
    state->selected_row = row;
    g_free(state->selected_path);
    state->selected_path = g_strdup(path);
    state->selected_is_directory = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "ghm-is-directory"));
    modified_changed(gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->preview)), view);
    if (state->selected_is_directory) return;
    g_free(state->preview_path);
    state->preview_path = g_strdup(path);
    state->preview_editable = FALSE;
    gtk_text_view_set_editable(GTK_TEXT_VIEW(state->preview), FALSE);
    request = g_new0(PreviewTask, 1);
    request->generation = ++state->preview_generation;
    request->repository_path = g_strdup(state->repository_path);
    request->relative_path = g_strdup(path);
    if (request->repository_path == NULL || request->relative_path == NULL) {
        preview_task_free(request);
        return;
    }
    gtk_label_set_text(GTK_LABEL(state->preview_title), "Loading preview…");
    task = g_task_new(view, NULL, preview_finished, NULL);
    g_task_set_task_data(task, request, preview_task_free);
    g_task_run_in_thread(task, preview_worker);
    g_object_unref(task);
}

static void activated(GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
    FileViewState *state = g_object_get_data(G_OBJECT(user_data), "ghm-file-view-state");
    const char *path = g_object_get_data(G_OBJECT(row), "ghm-file-path");
    (void)list;
    if (!GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "ghm-is-directory"))) return;
    if (g_hash_table_contains(state->expanded, path)) g_hash_table_remove(state->expanded, path);
    else g_hash_table_add(state->expanded, g_strdup(path));
    update_rows(state);
}

GtkWidget *ghm_file_view_new(GtkWidget *parent, GhmFilesChanged changed)
{
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *files_scroll = gtk_scrolled_window_new();
    GtkWidget *preview_scroll = gtk_scrolled_window_new();
    GtkWidget *explorer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *editor = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *editor_tabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    FileViewState *state = g_new0(FileViewState, 1);
    state->parent = parent;
    state->changed = changed;
    state->files = gtk_list_box_new();
    state->preview = gtk_text_view_new();
    state->preview_title = gtk_label_new("Select a file");
    state->hint = gtk_label_new("Select a repository to browse files.");
    state->new_file_button = gtk_button_new_with_label("New File");
    state->new_directory_button = gtk_button_new_with_label("New Folder");
    state->rename_button = gtk_button_new_with_label("Rename");
    state->delete_button = gtk_button_new_with_label("Delete");
    state->save_button = gtk_button_new_with_label("Save");
    state->discard_button = gtk_button_new_with_label("Discard Edits");
    state->expanded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gtk_widget_set_halign(state->hint, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(state->preview_title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(state->preview_title), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(state->preview_title, TRUE);
    gtk_widget_add_css_class(state->hint, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(state->hint), TRUE);
    gtk_widget_add_css_class(state->preview, "ghm-code");
    gtk_widget_add_css_class(explorer, "ghm-file-tree");
    gtk_widget_add_css_class(editor_tabs, "ghm-editor-toolbar");
    gtk_widget_set_sensitive(state->new_file_button, FALSE);
    gtk_widget_set_sensitive(state->new_directory_button, FALSE);
    gtk_widget_set_sensitive(state->rename_button, FALSE);
    gtk_widget_set_sensitive(state->delete_button, FALSE);
    gtk_widget_set_sensitive(state->save_button, FALSE);
    gtk_widget_set_sensitive(state->discard_button, FALSE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(state->preview), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(state->preview), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(state->preview), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(state->preview), GTK_WRAP_NONE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(state->preview), 14);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(state->preview), 14);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(state->preview), 10);
    gtk_widget_set_vexpand(files_scroll, TRUE);
    gtk_widget_set_vexpand(preview_scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(files_scroll), state->files);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(preview_scroll), state->preview);
    GtkWidget *files_heading = gtk_label_new("FILES");
    gtk_label_set_xalign(GTK_LABEL(files_heading), 0.0f);
    gtk_widget_set_margin_start(files_heading, 8);
    gtk_widget_add_css_class(files_heading, "ghm-view-heading");
    gtk_box_append(GTK_BOX(explorer), files_heading);
    gtk_box_append(GTK_BOX(explorer), files_scroll);
    gtk_widget_set_margin_start(editor_tabs, 12);
    gtk_widget_set_margin_end(editor_tabs, 6);
    gtk_widget_set_margin_top(editor_tabs, 4);
    gtk_widget_set_margin_bottom(editor_tabs, 4);
    gtk_box_append(GTK_BOX(editor_tabs), state->preview_title);
    gtk_box_append(GTK_BOX(editor_tabs), state->save_button);
    gtk_box_append(GTK_BOX(editor_tabs), state->discard_button);
    gtk_widget_add_css_class(state->save_button, "suggested-action");
    gtk_widget_set_tooltip_text(state->save_button, "Save locally (Ctrl+S)");
    gtk_box_append(GTK_BOX(editor), editor_tabs);
    gtk_box_append(GTK_BOX(editor), preview_scroll);
    gtk_paned_set_start_child(GTK_PANED(paned), explorer);
    gtk_paned_set_end_child(GTK_PANED(paned), editor);
    gtk_paned_set_position(GTK_PANED(paned), 240);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_append(GTK_BOX(toolbar), state->new_file_button);
    gtk_box_append(GTK_BOX(toolbar), state->new_directory_button);
    gtk_box_append(GTK_BOX(toolbar), state->rename_button);
    gtk_box_append(GTK_BOX(toolbar), state->delete_button);
    gtk_box_append(GTK_BOX(root), toolbar);
    gtk_box_append(GTK_BOX(root), paned);
    gtk_box_append(GTK_BOX(root), state->hint);
    g_object_set_data_full(G_OBJECT(root), "ghm-file-view-state", state, state_free);
    g_signal_connect(state->files, "row-selected", G_CALLBACK(selected), root);
    g_signal_connect(state->files, "row-activated", G_CALLBACK(activated), root);
    g_signal_connect(gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->preview)),
                     "modified-changed", G_CALLBACK(modified_changed), root);
    g_signal_connect(state->save_button, "clicked", G_CALLBACK(save_clicked), root);
    g_signal_connect(state->discard_button, "clicked", G_CALLBACK(discard_clicked), root);
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_name(keys, "ghm-editor-shortcuts");
    g_signal_connect(keys, "key-pressed", G_CALLBACK(editor_key_pressed), root);
    gtk_widget_add_controller(root, keys);
    GtkWidget *operation_buttons[] = {state->new_file_button, state->new_directory_button,
                                      state->rename_button, state->delete_button};
    for (int i = 0; i < 4; ++i) {
        g_object_set_data(G_OBJECT(operation_buttons[i]), "ghm-operation", GINT_TO_POINTER(i));
        g_signal_connect(operation_buttons[i], "clicked", G_CALLBACK(operation_clicked), root);
    }
    return root;
}

void ghm_file_view_refresh(GtkWidget *view, const char *repository_path)
{
    FileViewState *state = g_object_get_data(G_OBJECT(view), "ghm-file-view-state");
    FilesTask *request;
    GTask *task;
    if (repository_path == NULL) return;
    if (ghm_file_view_has_unsaved(view)) {
        gtk_label_set_text(GTK_LABEL(state->hint), "Save or discard edits before refreshing files.");
        return;
    }
    if (g_strcmp0(state->repository_path, repository_path) != 0) {
        g_free(state->repository_path);
        state->repository_path = g_strdup(repository_path);
        g_hash_table_remove_all(state->expanded);
        g_clear_pointer(&state->selected_path, g_free);
        g_clear_pointer(&state->preview_path, g_free);
        state->selected_row = NULL;
        state->preview_editable = FALSE;
        clear_list(state->files);
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->preview));
        gtk_text_buffer_set_text(buffer, "", -1);
        gtk_text_buffer_set_modified(buffer, FALSE);
        gtk_label_set_text(GTK_LABEL(state->preview_title), "Select a file");
        gtk_text_view_set_editable(GTK_TEXT_VIEW(state->preview), FALSE);
    }
    request = g_new0(FilesTask, 1);
    request->repository_path = g_strdup(repository_path);
    if (request->repository_path == NULL) { files_task_free(request); return; }
    gtk_label_set_text(GTK_LABEL(state->hint), "Reading repository files…");
    request->generation = ++state->generation;
    modified_changed(gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->preview)), view);
    task = g_task_new(view, NULL, files_finished, NULL);
    g_task_set_task_data(task, request, files_task_free);
    g_task_run_in_thread(task, files_worker);
    g_object_unref(task);
}

gboolean ghm_file_view_has_unsaved(GtkWidget *view)
{
    FileViewState *state = g_object_get_data(G_OBJECT(view), "ghm-file-view-state");
    return state->busy ||
           state->active_operations > 0U ||
           gtk_text_buffer_get_modified(gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->preview)));
}
