#include <ghm/log.h>
#include "core/git.h"

#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "platform/io.h"

static char *log_path(int make_directory)
{
    const char *state_home = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    char *directory, *path;
#ifdef _WIN32
    directory = g_build_filename(state_home != NULL && g_path_is_absolute(state_home) ?
        state_home : g_get_user_data_dir(), "ghm", "state", NULL);
    (void)home;
#else
    if (state_home != NULL && state_home[0] == '/')
        directory = g_build_filename(state_home, "ghm", NULL);
    else if (home != NULL && home[0] == '/')
        directory = g_build_filename(home, ".local", "state", "ghm", NULL);
    else return NULL;
#endif
    if (make_directory && g_mkdir_with_parents(directory, 0700) != 0) {
        g_free(directory);
        return NULL;
    }
    path = g_build_filename(directory, "ghm.log", NULL);
    g_free(directory);
    return path;
}

void ghm_log_event(GhmLogLevel level, const char *component,
                   const char *event, const char *detail)
{
    static const char *const levels[] = {"DEBUG", "INFO", "WARNING", "ERROR"};
    JsonBuilder *builder;
    JsonGenerator *generator;
    JsonNode *root;
    GDateTime *now;
    char *timestamp, *serialized, *path, *line;
    size_t length;
    int descriptor;
    if (level < GHM_LOG_DEBUG || level > GHM_LOG_ERROR) return;
    path = log_path(1);
    if (path == NULL) return;
    now = g_date_time_new_now_utc();
    timestamp = g_date_time_format_iso8601(now);
    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "time");
    json_builder_add_string_value(builder, timestamp);
    json_builder_set_member_name(builder, "level");
    json_builder_add_string_value(builder, levels[level]);
    json_builder_set_member_name(builder, "component");
    json_builder_add_string_value(builder, component != NULL ? component : "app");
    json_builder_set_member_name(builder, "event");
    json_builder_add_string_value(builder, event != NULL ? event : "event");
    json_builder_set_member_name(builder, "detail");
    json_builder_add_string_value(builder, detail != NULL ? detail : "");
    json_builder_end_object(builder);
    root = json_builder_get_root(builder);
    generator = json_generator_new();
    json_generator_set_root(generator, root);
    serialized = json_generator_to_data(generator, &length);
    line = g_malloc(length + 2);
    memcpy(line, serialized, length);
    line[length] = '\n';
    line[length + 1] = '\0';
    descriptor = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor >= 0) {
        struct stat info;
        if (fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode) && length < 16384) {
            (void)write(descriptor, line, length + 1);
        }
        close(descriptor);
    }
    g_free(serialized);
    g_free(line);
    g_object_unref(generator);
    json_node_unref(root);
    g_object_unref(builder);
    g_free(timestamp);
    g_date_time_unref(now);
    g_free(path);
}

int ghm_log_tail(size_t max_bytes, char **out_text, GhmError *error)
{
    char *path;
    FILE *file;
    long size, start;
    size_t read_count;
    if (out_text == NULL || max_bytes == 0 || max_bytes > 1024 * 1024) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Log output and a limit up to 1 MiB are required");
        return -1;
    }
    *out_text = NULL;
    path = log_path(0);
    if (path == NULL) { ghm_error_set(error, GHM_ERROR_IO, "Log location is unavailable"); return -1; }
    file = fopen(path, "r");
    g_free(path);
    if (file == NULL) {
        if (errno == ENOENT) {
            *out_text = strdup("");
            if (*out_text == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); return -1; }
            return 0;
        }
        ghm_error_set(error, GHM_ERROR_IO, "Cannot read application log");
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0) {
        ghm_error_set(error, GHM_ERROR_IO, "Cannot inspect application log");
        fclose(file);
        return -1;
    }
    start = size > (long)max_bytes ? size - (long)max_bytes : 0;
    if (fseek(file, start, SEEK_SET) != 0) {
        ghm_error_set(error, GHM_ERROR_IO, "Cannot seek application log");
        fclose(file);
        return -1;
    }
    *out_text = malloc((size_t)(size - start) + 1);
    if (*out_text == NULL) {
        ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
        fclose(file);
        return -1;
    }
    read_count = fread(*out_text, 1, (size_t)(size - start), file);
    (*out_text)[read_count] = '\0';
    if (ferror(file)) {
        free(*out_text);
        *out_text = NULL;
        ghm_error_set(error, GHM_ERROR_IO, "Cannot read application log");
        fclose(file);
        return -1;
    }
    fclose(file);
    return 0;
}
