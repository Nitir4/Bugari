#include "platform/windows_runtime.h"
#include <windows.h>
#include <glib.h>
#include <glib/gwin32.h>
#include <glib/gstdio.h>

int ghm_windows_gui_runtime(void)
{
    char *root = g_win32_get_package_installation_directory_of_module(NULL);
    char *schemas = g_build_filename(root, "share", "glib-2.0", "schemas", NULL);
    char *fonts = g_build_filename(root, "etc", "fonts", "fonts.conf", NULL);
    char *loaders = g_build_filename(root, "lib", "gdk-pixbuf-2.0", "2.10.0", "loaders", NULL);
    char *query = g_build_filename(root, "bin", "gdk-pixbuf-query-loaders.exe", NULL);
    char *data = g_build_filename(g_get_user_data_dir(), "ghm", NULL);
    char *cache = g_build_filename(data, "loaders.cache", NULL);
    int result = 0;
    /* Development runs can use the toolchain's resources. Installed packages
     * supply these paths, so no MSYS2 installation is required. */
    if (g_file_test(schemas, G_FILE_TEST_IS_DIR)) g_setenv("GSETTINGS_SCHEMA_DIR", schemas, TRUE);
    if (g_file_test(fonts, G_FILE_TEST_IS_REGULAR)) g_setenv("FONTCONFIG_FILE", fonts, TRUE);
    if (g_file_test(query, G_FILE_TEST_IS_REGULAR) && g_file_test(loaders, G_FILE_TEST_IS_DIR)) {
        char *output = NULL, *messages = NULL; int status = -1;
        char *arguments[] = {query, NULL};
        GError *error = NULL;
        g_setenv("GDK_PIXBUF_MODULEDIR", loaders, TRUE);
        if (!g_spawn_sync(NULL, arguments, NULL, G_SPAWN_DEFAULT, NULL, NULL,
            &output, &messages, &status, &error) || !g_spawn_check_wait_status(status, &error) ||
            g_mkdir_with_parents(data, 0700) != 0 || !g_file_set_contents(cache, output, -1, &error)) {
            g_printerr("Cannot initialize bundled image loaders: %s\n", error != NULL ? error->message : "I/O error");
            result = -1;
        } else g_setenv("GDK_PIXBUF_MODULE_FILE", cache, TRUE);
        g_clear_error(&error); g_free(output); g_free(messages);
    }
    g_free(root); g_free(schemas); g_free(fonts); g_free(loaders); g_free(query);
    g_free(data); g_free(cache); return result;
}
