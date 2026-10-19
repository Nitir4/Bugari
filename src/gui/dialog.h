#ifndef GHM_GUI_DIALOG_H
#define GHM_GUI_DIALOG_H
#include <gtk/gtk.h>
static inline gboolean ghm_view_is_open(GtkWidget *view)
{
    GtkRoot *root = gtk_widget_get_root(view);
    return root != NULL && gtk_widget_get_realized(GTK_WIDGET(root));
}
static inline void ghm_dialog_attach(GtkWidget *dialog, GtkWidget *parent)
{
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(parent));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    /* Background tasks and dialog state retain parent objects. Close on
     * unrealize as well, rather than waiting for the parent's final unref. */
    g_signal_connect_object(parent, "unrealize", G_CALLBACK(gtk_window_destroy),
                            dialog, G_CONNECT_SWAPPED);
}
/* Scroll longer forms while keeping their action buttons visible. */
static inline void ghm_dialog_set_scrollable_content(GtkWidget *dialog,
                                                     GtkWidget *content,
                                                     GtkWidget *feedback,
                                                     GtkWidget *actions)
{
    GtkWidget *layout = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content);
    gtk_widget_set_margin_start(footer, 20);
    gtk_widget_set_margin_end(footer, 20);
    gtk_widget_set_margin_top(footer, 12);
    gtk_widget_set_margin_bottom(footer, 16);
    gtk_label_set_xalign(GTK_LABEL(feedback), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(feedback), TRUE);
    gtk_box_append(GTK_BOX(footer), feedback);
    gtk_box_append(GTK_BOX(footer), actions);
    gtk_box_append(GTK_BOX(layout), scroll);
    gtk_box_append(GTK_BOX(layout), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(layout), footer);
    gtk_window_set_child(GTK_WINDOW(dialog), layout);
}
#endif
