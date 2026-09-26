#ifndef GHM_GUI_LOGIN_VIEW_H
#define GHM_GUI_LOGIN_VIEW_H

#include <ghm/ghm.h>
#include <gtk/gtk.h>

typedef void (*GhmLoginReady)(GtkWidget *window, const char *login);

GtkWidget *ghm_login_view_new(GtkWidget *window, GhmContext *context, GhmLoginReady ready);
void ghm_login_view_check_saved(GtkWidget *window);
/* Borrowed public OAuth Client ID for the authenticated account, or NULL. */
const char *ghm_login_view_client_id(GtkWidget *window);

#endif
