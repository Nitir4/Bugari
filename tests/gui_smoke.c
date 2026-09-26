/* Manual display-dependent smoke test. Default fixtures are disposable.
 * Optional --repository/--data-directory accept only the authorized /tmp live
 * test workspace. No user credentials/configuration/database. Never CTest. */
#include "gui/window.h"
#include <ghm/commit.h>
#include <ghm/history.h>
#include <dirent.h>
#include <git2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    GtkApplication *application;
    GtkWidget *window;
    char root[64];
    const char *prefix;
    const char *repository;
    const char *data_directory;
    unsigned extra_view;
    unsigned action_step;
    int views_only;
    int failed;
} SmokeState;

static int remove_tree(const char *path)
{
    DIR *directory = opendir(path);
    struct dirent *entry;
    if (directory == NULL) return unlink(path);
    while ((entry = readdir(directory)) != NULL) {
        char child[512];
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child) ||
            remove_tree(child) != 0) { closedir(directory); return -1; }
    }
    closedir(directory);
    return rmdir(path);
}

static int write_text(const char *root, const char *name, const char *text)
{
    char path[512];
    if (snprintf(path, sizeof(path), "%s/%s", root, name) >= (int)sizeof(path)) return -1;
    FILE *file = fopen(path, "w");
    if (file == NULL) return -1;
    int written = fputs(text, file);
    return fclose(file) == 0 && written != EOF ? 0 : -1;
}

static GtkWidget *find_widget(GtkWidget *widget, const char *needle, int mode)
{
    if (mode == 4 && GTK_IS_TEXT_VIEW(widget) && gtk_text_view_get_editable(GTK_TEXT_VIEW(widget))) return widget;
    if (mode == 5 && GTK_IS_ENTRY(widget)) return widget;
    const char *text = NULL;
    if (mode == 0 && GTK_IS_BUTTON(widget)) text = gtk_button_get_label(GTK_BUTTON(widget));
    if (mode == 1) text = gtk_widget_get_tooltip_text(widget);
    if (mode == 2 && GTK_IS_LABEL(widget)) text = gtk_label_get_text(GTK_LABEL(widget));
    if (mode == 3) text = g_object_get_data(G_OBJECT(widget), "ghm-diff-path");
    if (text != NULL && strstr(text, needle) != NULL) return widget;
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_widget(child, needle, mode);
        if (found != NULL) return found;
    }
    return NULL;
}

static int capture(SmokeState *state, const char *suffix)
{
    GdkPaintable *paintable = gtk_widget_paintable_new(state->window);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot),
        (double)gtk_widget_get_width(state->window), (double)gtk_widget_get_height(state->window));
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    g_object_unref(paintable);
    if (node == NULL) { fprintf(stderr, "Empty GTK capture: %s (%dx%d)\n", suffix,
        gtk_widget_get_width(state->window), gtk_widget_get_height(state->window)); return -1; }
    GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(state->window));
    GdkTexture *texture = gsk_renderer_render_texture(renderer, node, NULL);
    gsk_render_node_unref(node);
    if (texture == NULL) return -1;
    char *path = g_strdup_printf("%s-%s.png", state->prefix, suffix);
    gboolean saved = gdk_texture_save_to_png(texture, path);
    if (saved) printf("Rendered %s\n", path);
    g_free(path);
    g_object_unref(texture);
    return saved ? 0 : -1;
}

static void select_row_for(GtkWidget *widget)
{
    GtkWidget *row = widget != NULL ? gtk_widget_get_ancestor(widget, GTK_TYPE_LIST_BOX_ROW) : NULL;
    if (row != NULL)
        gtk_list_box_select_row(GTK_LIST_BOX(gtk_widget_get_parent(row)), GTK_LIST_BOX_ROW(row));
}

static gboolean open_file(gpointer data)
{
    SmokeState *state = data;
    GtkWidget *label = find_widget(state->window, "README.md", 2);
    if (label == NULL) state->failed = 1;
    else select_row_for(label);
    return G_SOURCE_REMOVE;
}

static gboolean show_changes(gpointer data)
{
    SmokeState *state = data;
    if (capture(state, "files") != 0) state->failed = 1;
    GtkWidget *button = find_widget(state->window, "Source Control", 1);
    if (button == NULL) state->failed = 1;
    else gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), TRUE);
    select_row_for(find_widget(state->window, "README.md", 3));
    return G_SOURCE_REMOVE;
}

static gboolean save_editor(gpointer data)
{
    SmokeState *state = data;
    GtkWidget *editor = find_widget(state->window, "", 4);
    GtkWidget *save = find_widget(state->window, "Save", 0);
    if (editor == NULL || save == NULL) state->failed = 1;
    else {
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor));
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(buffer, &end);
        gtk_text_buffer_insert(buffer, &end, "\nGUI smoke save validation.\n", -1);
        if (!gtk_widget_get_sensitive(save)) state->failed = 1;
        else g_signal_emit_by_name(save, "clicked");
    }
    return G_SOURCE_REMOVE;
}

static gboolean discard_editor(gpointer data)
{
    SmokeState *state = data;
    GtkWidget *editor = find_widget(state->window, "", 4);
    GtkWidget *discard = find_widget(state->window, "Discard Edits", 0);
    if (editor == NULL || discard == NULL) state->failed = 1;
    else {
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor));
        if (gtk_text_buffer_get_modified(buffer)) state->failed = 1;
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(buffer, &end);
        gtk_text_buffer_insert(buffer, &end, "Discard this unsaved smoke text.\n", -1);
        g_signal_emit_by_name(discard, "clicked");
    }
    return G_SOURCE_REMOVE;
}

static GtkWidget *dialog_named(const char *title)
{
    GListModel *windows = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(windows); ++i) {
        GtkWidget *window = g_list_model_get_item(windows, i);
        if (g_strcmp0(gtk_window_get_title(GTK_WINDOW(window)), title) == 0) return window;
        g_object_unref(window);
    }
    return NULL;
}

static void click(SmokeState *state, GtkWidget *root, const char *label)
{
    GtkWidget *button = root != NULL ? find_widget(root, label, 0) : NULL;
    if (button == NULL || !gtk_widget_get_sensitive(button)) state->failed = 1;
    else g_signal_emit_by_name(button, "clicked");
}

static void switch_view(SmokeState *state, const char *name)
{
    GtkWidget *button = find_widget(state->window, name, 1);
    if (button == NULL) state->failed = 1;
    else gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), TRUE);
}

static gboolean exercise_dialogs(gpointer data)
{
    SmokeState *state = data;
    int previously_failed = state->failed;
    GtkWidget *dialog = NULL, *entry = NULL;
    switch (state->action_step++) {
    case 0: switch_view(state, "Source Control"); click(state, state->window, "Stage All"); break;
    case 1: click(state, state->window, "Commit Staged"); break;
    case 2:
        dialog = dialog_named("Commit Staged Changes");
        entry = dialog != NULL ? find_widget(dialog, "", 5) : NULL;
        if (entry == NULL) state->failed = 1;
        else {
            gtk_editable_set_text(GTK_EDITABLE(entry), "GHM validation: desktop commit dialog");
            click(state, dialog, "Create Commit");
        }
        break;
    case 3:
        dialog = dialog_named("Commit Staged Changes");
        if (dialog != NULL) state->failed = 1;
        GhmHistory history = {0};
        GhmError error = {0};
        if (ghm_commit_get_history(state->repository, 1, &history, &error) != 0 || history.count != 1 ||
            strcmp(history.items[0].summary, "GHM validation: desktop commit dialog") != 0) state->failed = 1;
        ghm_history_free(&history);
        char fixture[128];
        (void)snprintf(fixture, sizeof(fixture), "/* Desktop scheduling validation %lld. */\n", (long long)g_get_real_time());
        if (write_text(state->repository, "main.c", fixture) != 0) state->failed = 1;
        click(state, state->window, "Schedule Commit");
        break;
    case 4:
        dialog = dialog_named("Schedule Commit");
        entry = dialog != NULL ? find_widget(dialog, "", 5) : NULL;
        if (entry == NULL) state->failed = 1;
        else {
            gtk_editable_set_text(GTK_EDITABLE(entry), "GHM validation: desktop schedule then cancel");
            click(state, dialog, "Schedule Commit");
        }
        break;
    case 5:
        dialog = dialog_named("Schedule Commit");
        if (dialog != NULL) state->failed = 1;
        switch_view(state, "Scheduled Jobs"); click(state, state->window, "Refresh Jobs"); break;
    case 6: click(state, state->window, "Cancel Job"); break;
    case 7:
        dialog = dialog_named("Cancel Scheduled Job");
        click(state, dialog, "Cancel Job"); break;
    case 8:
        dialog = dialog_named("Cancel Scheduled Job");
        if (dialog != NULL || find_widget(state->window, "desktop schedule then cancel", 2) == NULL ||
            find_widget(state->window, "PENDING", 2) != NULL) state->failed = 1;
        if (capture(state, "cancelled-job") != 0) state->failed = 1;
        gtk_window_destroy(GTK_WINDOW(state->window));
        g_application_quit(G_APPLICATION(state->application));
        if (dialog != NULL) g_object_unref(dialog);
        return G_SOURCE_REMOVE;
    }
    if (dialog != NULL) g_object_unref(dialog);
    if (state->failed && !previously_failed) fprintf(stderr, "Desktop action failed at step %u\n", state->action_step - 1);
    g_timeout_add(1000, exercise_dialogs, state);
    return G_SOURCE_REMOVE;
}

static gboolean finish(gpointer data)
{
    SmokeState *state = data;
    static const char *views[] = {"Commit History", "Branches", "Scheduled Jobs", "Settings"};
    static const char *suffixes[] = {"history", "branches", "jobs", "settings"};
    if (capture(state, state->extra_view == 0 ? "changes" : suffixes[state->extra_view - 1]) != 0) state->failed = 1;
    if (state->extra_view < G_N_ELEMENTS(views)) {
        GtkWidget *button = find_widget(state->window, views[state->extra_view], 1);
        if (button == NULL) state->failed = 1;
        else gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), TRUE);
        ++state->extra_view;
        if (state->extra_view == 4) {
            GtkWidget *refresh = find_widget(state->window, "Refresh Developer Log", 0);
            if (refresh == NULL) state->failed = 1;
            else g_signal_emit_by_name(refresh, "clicked");
        }
        g_timeout_add(1000, finish, state);
        return G_SOURCE_REMOVE;
    }
    if (state->repository != NULL && !state->views_only) {
        g_timeout_add(1000, exercise_dialogs, state);
        return G_SOURCE_REMOVE;
    }
    gtk_window_destroy(GTK_WINDOW(state->window));
    g_application_quit(G_APPLICATION(state->application));
    return G_SOURCE_REMOVE;
}

static void activate(GtkApplication *application, gpointer data)
{
    SmokeState *state = data;
    GhmContext *context = NULL;
    GhmError error = {0};
    git_repository *repository = NULL;
    char repo_path[1024], data_path[1024], oid[GHM_OID_HEX_CAPACITY];
    const GhmCommitSignature signature = {"Demo Developer", "demo@example.invalid", 1790000000, 330};
    if (state->repository != NULL) (void)snprintf(repo_path, sizeof(repo_path), "%s", state->repository);
    else (void)snprintf(repo_path, sizeof(repo_path), "%s/native-workspace", state->root);
    if (state->data_directory != NULL) (void)snprintf(data_path, sizeof(data_path), "%s", state->data_directory);
    else (void)snprintf(data_path, sizeof(data_path), "%s/data", state->root);
    if (ghm_context_open(data_path, &context, &error) != 0 ||
        (state->repository == NULL && (git_repository_init(&repository, repo_path, 0) < 0 ||
        write_text(repo_path, "README.md", "# Native Workspace\n\nA fast local Git workspace built in C.\n") != 0 ||
        ghm_commit_now(repo_path, "Initial workspace", &signature, &signature, oid, &error) != 0 ||
        write_text(repo_path, "README.md", "# Native Workspace\n\nA fast local Git workspace built in C.\n\n## Workbench\n\n- Explore and edit local files\n- Review staged and unstaged changes\n- Freeze snapshots for scheduled commits\n") != 0 ||
        write_text(repo_path, "main.c", "#include <stdio.h>\n\nint main(void)\n{\n    puts(\"Hello from a native desktop app\");\n    return 0;\n}\n") != 0)) ||
        ghm_repo_register(context, repo_path, &error) != 0) {
        fprintf(stderr, "UI smoke setup failed: %s\n", error.message);
        git_repository_free(repository);
        ghm_context_close(context);
        state->failed = 1;
        g_application_quit(G_APPLICATION(application));
        return;
    }
    git_repository_free(repository);
    state->window = ghm_window_new(application, context);
    gtk_window_present(GTK_WINDOW(state->window));
    GtkWidget *local = find_widget(state->window, "Continue with local", 0);
    if (local == NULL) state->failed = 1;
    else g_signal_emit_by_name(local, "clicked");
    g_timeout_add(600, open_file, state);
    if (!state->views_only) {
        g_timeout_add(1100, save_editor, state);
        g_timeout_add(1800, discard_editor, state);
    }
    g_timeout_add(2700, show_changes, state);
    g_timeout_add(3700, finish, state);
}

int main(int argc, char **argv)
{
    SmokeState state = {.root = "/tmp/ghm-gui-smoke-XXXXXX",
                        .prefix = argc > 1 ? argv[1] : "/tmp/ghm-ui"};
    if ((argc == 6 || (argc == 7 && strcmp(argv[6], "--views-only") == 0)) &&
        strcmp(argv[1], "--repository") == 0 && strcmp(argv[3], "--data-directory") == 0) {
        const char *allowed = "/tmp/ghm-live-bugari-";
        if (strncmp(argv[2], allowed, strlen(allowed)) != 0 || strncmp(argv[4], allowed, strlen(allowed)) != 0)
            return EXIT_FAILURE;
        state.repository = argv[2]; state.data_directory = argv[4]; state.prefix = argv[5];
        state.views_only = argc == 7;
    } else if (argc > 2) return EXIT_FAILURE;
    if (mkdtemp(state.root) == NULL || setenv("XDG_STATE_HOME", state.root, 1) != 0 ||
        setenv("XDG_CONFIG_HOME", state.root, 1) != 0 || setenv("GHM_GITHUB_CLIENT_ID", "", 1) != 0 ||
        setenv("GTK_THEME", "Adwaita:dark", 1) != 0)
        return EXIT_FAILURE;
    state.application = gtk_application_new("io.github.ghm.UiSmoke", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(state.application, "activate", G_CALLBACK(activate), &state);
    int result = g_application_run(G_APPLICATION(state.application), 1, argv);
    g_object_unref(state.application);
    if (remove_tree(state.root) != 0) state.failed = 1;
    return result == 0 && !state.failed ? EXIT_SUCCESS : EXIT_FAILURE;
}
