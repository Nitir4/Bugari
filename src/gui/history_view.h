#ifndef GHM_GUI_HISTORY_VIEW_H
#define GHM_GUI_HISTORY_VIEW_H

#include <gtk/gtk.h>
#include <ghm/ghm.h>

typedef void (*GhmHistoryRewritten)(GtkWidget *parent);

GtkWidget *ghm_history_view_new(GtkWidget *parent, GhmContext *context,
                                GhmHistoryRewritten rewritten);
void ghm_history_view_refresh(GtkWidget *view, const char *repository_path);

#endif
