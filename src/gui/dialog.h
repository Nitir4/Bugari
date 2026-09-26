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
#endif
