#ifndef GHM_GUI_GITHUB_REPOS_VIEW_H
#define GHM_GUI_GITHUB_REPOS_VIEW_H

#include <ghm/ghm.h>
#include <gtk/gtk.h>

typedef void (*GhmRepositoryOpened)(GtkWidget *parent, const char *path);

void ghm_github_repos_dialog_show(GtkWidget *parent, GhmContext *context,
                                  const char *client_id, GhmRepositoryOpened opened);

#endif
