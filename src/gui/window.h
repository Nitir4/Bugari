#ifndef GHM_GUI_WINDOW_H
#define GHM_GUI_WINDOW_H

#include <ghm/ghm.h>
#include <gtk/gtk.h>

/* Takes ownership of context. */
GtkWidget *ghm_window_new(GtkApplication *application, GhmContext *context);

#endif
