#ifndef GHM_GUI_COMMIT_MESSAGE_H
#define GHM_GUI_COMMIT_MESSAGE_H

#include <gtk/gtk.h>
#include <string.h>

/* Keep message composition consistent for immediate and scheduled commits. */
static inline GtkWidget *ghm_commit_message_editor_new(const char *title,
                                                       const char *initial,
                                                       GtkWidget **editor)
{
    GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *heading = gtk_label_new(title);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *hint = gtk_label_new("Write a short summary, then a blank line and an optional description.");
    *editor = gtk_text_view_new();
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
    gtk_widget_add_css_class(heading, "heading");
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(*editor), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(*editor), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(*editor), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(*editor), 12);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(*editor), 12);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(*editor), 12);
    gtk_text_view_set_pixels_below_lines(GTK_TEXT_VIEW(*editor), 3);
    gtk_accessible_update_property(GTK_ACCESSIBLE(*editor),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(*editor)),
                             initial != NULL ? initial : "", -1);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 140);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), *editor);
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_box_append(GTK_BOX(section), heading);
    gtk_box_append(GTK_BOX(section), scroll);
    gtk_box_append(GTK_BOX(section), hint);
    return section;
}

static inline gboolean ghm_commit_message_buffer_has_text(GtkTextBuffer *buffer)
{
    GtkTextIter iter, end;
    gtk_text_buffer_get_bounds(buffer, &iter, &end);
    while (gtk_text_iter_compare(&iter, &end) < 0) {
        if (!g_unichar_isspace(gtk_text_iter_get_char(&iter))) return TRUE;
        gtk_text_iter_forward_char(&iter);
    }
    return FALSE;
}

static inline gboolean ghm_commit_message_has_text(GtkWidget *editor)
{
    return ghm_commit_message_buffer_has_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor)));
}

static inline void ghm_commit_message_changed(GtkTextBuffer *buffer, GtkLabel *feedback)
{
    if (strcmp(gtk_label_get_text(feedback), "Enter a commit message.") == 0 &&
        ghm_commit_message_buffer_has_text(buffer)) gtk_label_set_text(feedback, "");
}

static inline void ghm_commit_message_bind_feedback(GtkWidget *editor, GtkWidget *feedback)
{
    g_signal_connect_object(gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor)), "changed",
                             G_CALLBACK(ghm_commit_message_changed), feedback, 0);
}

/* Tasks own malloc-allocated strings. Preserve newlines and whitespace verbatim. */
static inline char *ghm_commit_message_dup(GtkWidget *editor)
{
    GtkTextIter start, end;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor));
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    char *copy = strdup(text);
    g_free(text);
    return copy;
}

#endif
