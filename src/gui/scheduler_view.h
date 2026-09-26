#ifndef GHM_GUI_SCHEDULER_VIEW_H
#define GHM_GUI_SCHEDULER_VIEW_H

#include <ghm/ghm.h>
#include <gtk/gtk.h>

typedef void (*GhmScheduleAdded)(GtkWidget *parent, int64_t job_id);

void ghm_scheduler_dialog_show(GtkWidget *parent, GhmContext *context,
                               const char *repository_path, const char *client_id,
                               GhmScheduleAdded added);

#endif
