#ifndef GHM_GUI_TEST_SUPPORT_H
#define GHM_GUI_TEST_SUPPORT_H
#include <gtk/gtk.h>
#include <string.h>
static inline GtkWidget *gui_find(GtkWidget *root, const char *text, int kind)
{
    const char *value = NULL;
    if (kind == 0 && GTK_IS_BUTTON(root)) value = gtk_button_get_label(GTK_BUTTON(root));
    if (kind == 1) value = gtk_widget_get_tooltip_text(root);
    if (kind == 2 && GTK_IS_LABEL(root)) value = gtk_label_get_text(GTK_LABEL(root));
    if (kind == 3) value = g_object_get_data(G_OBJECT(root), "ghm-file-path");
    if (kind == 4) value = g_object_get_data(G_OBJECT(root), "ghm-path");
    if (value != NULL && (kind == 0 || kind == 3 || kind == 4 ? strcmp(value, text) == 0 : strstr(value, text) != NULL) &&
        gtk_widget_get_mapped(root)) return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child != NULL; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = gui_find(child, text, kind);
        if (found != NULL) return found;
    }
    return NULL;
}
static inline GtkWidget *gui_type_at(GtkWidget *root, GType type, unsigned *index)
{
    if (G_TYPE_CHECK_INSTANCE_TYPE(root, type) && gtk_widget_get_mapped(root)) {
        if (*index == 0) return root;
        --*index;
    }
    for (GtkWidget *child = gtk_widget_get_first_child(root); child != NULL; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = gui_type_at(child, type, index);
        if (found != NULL) return found;
    }
    return NULL;
}
static inline GtkWidget *gui_type(GtkWidget *root, GType type, unsigned index)
{ return gui_type_at(root, type, &index); }
static inline GtkWidget *gui_dialog(const char *title)
{
    GListModel *windows = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(windows); ++i) {
        GtkWidget *window = g_list_model_get_item(windows, i);
        int matches = g_strcmp0(gtk_window_get_title(GTK_WINDOW(window)), title) == 0 && gtk_widget_get_mapped(window);
        g_object_unref(window);
        if (matches) return window; /* GTK owns each presented top-level. */
    }
    return NULL;
}
static inline int gui_click(GtkWidget *root, const char *text)
{
    GtkWidget *button = root != NULL ? gui_find(root, text, 0) : NULL;
    if (button == NULL || !gtk_widget_get_sensitive(button)) return 0;
    g_signal_emit_by_name(button, "clicked"); return 1;
}
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
static inline int gui_native_response(GtkWidget *dialog, int response)
{
    GtkWidget *button = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), response);
    if (button == NULL || !gtk_widget_get_sensitive(button) || !gtk_widget_get_mapped(button)) return 0;
    g_signal_emit_by_name(button, "clicked"); return 1;
}
G_GNUC_END_IGNORE_DEPRECATIONS
static inline int gui_view(GtkWidget *root, const char *text)
{
    GtkWidget *button = gui_find(root, text, 1);
    if (button == NULL) return 0;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), TRUE); return 1;
}
static inline int gui_select(GtkWidget *root, const char *text, int kind)
{
    GtkWidget *widget = gui_find(root, text, kind);
    GtkWidget *row = widget != NULL ? (GTK_IS_LIST_BOX_ROW(widget) ? widget : gtk_widget_get_ancestor(widget, GTK_TYPE_LIST_BOX_ROW)) : NULL;
    if (row == NULL) return 0;
    gtk_list_box_select_row(GTK_LIST_BOX(gtk_widget_get_parent(row)), GTK_LIST_BOX_ROW(row)); return 1;
}
static inline int gui_branch_action(GtkWidget *root, const char *branch, const char *action)
{
    if (GTK_IS_BUTTON(root)) {
        const char *name = g_object_get_data(G_OBJECT(root), "ghm-branch-name");
        const char *label = gtk_button_get_label(GTK_BUTTON(root));
        if (g_strcmp0(name, branch) == 0 && g_strcmp0(label, action) == 0 && gtk_widget_get_mapped(root)) {
            g_signal_emit_by_name(root, "clicked"); return 1;
        }
    }
    for (GtkWidget *child = gtk_widget_get_first_child(root); child != NULL; child = gtk_widget_get_next_sibling(child))
        if (gui_branch_action(child, branch, action)) return 1;
    return 0;
}
static inline int gui_key(GtkWidget *root, guint key, GdkModifierType modifiers)
{
    if (!gtk_widget_get_mapped(root)) return 0;
    GListModel *controllers = gtk_widget_observe_controllers(root);
    gboolean handled = FALSE;
    for (guint i = 0; i < g_list_model_get_n_items(controllers) && !handled; ++i) {
        GObject *controller = g_list_model_get_item(controllers, i);
        if (GTK_IS_EVENT_CONTROLLER_KEY(controller) &&
            g_str_has_prefix(gtk_event_controller_get_name(GTK_EVENT_CONTROLLER(controller)) != NULL ?
                gtk_event_controller_get_name(GTK_EVENT_CONTROLLER(controller)) : "", "ghm-"))
            g_signal_emit_by_name(controller, "key-pressed", key, 0U, modifiers, &handled);
        g_object_unref(controller);
    }
    g_object_unref(controllers);
    if (handled) return 1;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child != NULL; child = gtk_widget_get_next_sibling(child))
        if (gui_key(child, key, modifiers)) return 1;
    return 0;
}
static inline char *gui_buffer_text(GtkWidget *editor)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    return gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
}
static inline int gui_capture(GtkWidget *window, const char *path)
{
    GdkPaintable *paintable = gtk_widget_paintable_new(window);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), (double)gtk_widget_get_width(window), (double)gtk_widget_get_height(window));
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    g_object_unref(paintable);
    if (node == NULL) return -1;
    GdkTexture *texture = gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(window)), node, NULL);
    gsk_render_node_unref(node);
    if (texture == NULL) return -1;
    int result = gdk_texture_save_to_png(texture, path) ? 0 : -1;
    g_object_unref(texture); return result;
}
#endif
