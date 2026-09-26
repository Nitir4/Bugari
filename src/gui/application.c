#include "gui/window.h"

#include <ghm/ghm.h>
#include <gtk/gtk.h>

static void activate(GtkApplication *application, gpointer user_data)
{
    GhmContext *context = NULL;
    GhmError error = {0};
    GtkWidget *window;
    (void)user_data;
    GList *windows = gtk_application_get_windows(application);
    if (windows != NULL) {
        gtk_window_present(GTK_WINDOW(windows->data));
        return;
    }
    if (ghm_context_open(NULL, &context, &error) != 0) {
        g_printerr("Cannot start GitHub Commit Manager: %s\n", error.message);
        g_application_quit(G_APPLICATION(application));
        return;
    }
    window = ghm_window_new(application, context);
    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv)
{
    /* The application owns its dark theme, independently of a theme override. */
    g_unsetenv("GTK_THEME");
    GtkApplication *application = gtk_application_new("io.github.ghm.CommitManager", G_APPLICATION_DEFAULT_FLAGS);
    int result;
    g_signal_connect(application, "activate", G_CALLBACK(activate), NULL);
    result = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return result;
}
