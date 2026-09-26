#ifndef GHM_GUI_FILE_VIEW_H
#define GHM_GUI_FILE_VIEW_H

#include <gtk/gtk.h>

/* Git and file I/O stay in the core/background workers. */
typedef void (*GhmFilesChanged)(GtkWidget *parent);
GtkWidget *ghm_file_view_new(GtkWidget *parent, GhmFilesChanged changed);
void ghm_file_view_refresh(GtkWidget *view, const char *repository_path);
gboolean ghm_file_view_has_unsaved(GtkWidget *view);

#endif
