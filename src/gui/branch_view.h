#ifndef GHM_GUI_BRANCH_VIEW_H
#define GHM_GUI_BRANCH_VIEW_H

#include <ghm/ghm.h>
#include <gtk/gtk.h>

typedef gboolean (*GhmBranchCanSwitch)(GtkWidget *parent);
typedef void (*GhmBranchChanged)(GtkWidget *parent);

GtkWidget *ghm_branch_view_new(GtkWidget *parent, GhmContext *context,
                               GhmBranchCanSwitch can_switch, GhmBranchChanged changed);
void ghm_branch_view_refresh(GtkWidget *view, const char *repository_path);

#endif
