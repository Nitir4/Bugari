#ifndef GHM_GUI_COMMIT_VIEW_H
#define GHM_GUI_COMMIT_VIEW_H

#include <gtk/gtk.h>

typedef void (*GhmCommitCreated)(GtkWidget *parent, const char *oid);

void ghm_commit_dialog_show(GtkWidget *parent, const char *repository_path,
                            GhmCommitCreated created);

#endif
