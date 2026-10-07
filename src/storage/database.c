#include "storage/database.h"
#include "storage/migrations.h"
#include "core/git.h"

#include <errno.h>
#include <curl/curl.h>
#include <git2.h>
#ifdef _WIN32
#include <glib/gstdio.h>
#endif
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "platform/io.h"

#ifndef _WIN32
static int make_directory(const char *path, GhmError *error)
{
    if (mkdir(path, 0700) == 0 || errno == EEXIST) return 0;
    if (error != NULL) {
        error->code = GHM_ERROR_IO;
        (void)snprintf(error->message, sizeof(error->message), "Cannot create directory: %s", strerror(errno));
    }
    return -1;
}
#endif

static char *default_data_directory(void)
{
#ifdef _WIN32
    const char *xdg = g_getenv("XDG_DATA_HOME");
    char *path = g_build_filename(xdg != NULL && g_path_is_absolute(xdg) ? xdg :
        g_get_user_data_dir(), "ghm", NULL);
    for (char *p = path; p != NULL && *p; ++p) if (*p == '\\') *p = '/';
    char *result = path != NULL ? strdup(path) : NULL;
    g_free(path); return result;
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    const char *base = xdg != NULL && xdg[0] == '/' ? xdg : NULL;
    const char *suffix = base != NULL ? "/ghm" : "/.local/share/ghm";
    size_t length;
    char *result;
    if (base == NULL) base = home;
    if (base == NULL || base[0] == '\0') return NULL;
    length = strlen(base) + strlen(suffix) + 1;
    result = malloc(length);
    if (result != NULL) (void)snprintf(result, length, "%s%s", base, suffix);
    return result;
#endif
}

static int ensure_data_directory(const char *path, GhmError *error)
{
#ifdef _WIN32
    if (g_mkdir_with_parents(path, 0700) == 0) return 0;
    ghm_error_set(error, GHM_ERROR_IO, "Cannot create Windows application data directory");
    return -1;
#else
    char *copy = strdup(path);
    if (copy == NULL) {
        ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
        return -1;
    }
    for (char *part = copy + 1; *part != '\0'; ++part) {
        if (*part == '/') {
            *part = '\0';
            if (make_directory(copy, error) != 0) { free(copy); return -1; }
            *part = '/';
        }
    }
    int result = make_directory(copy, error);
    free(copy);
    return result;
#endif
}

int ghm_context_open(const char *data_directory, GhmContext **out, GhmError *error)
{
    GhmContext *context;
    char *db_path = NULL;
    size_t path_length;
    if (out == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Output context is required");
        return -1;
    }
    *out = NULL;
    context = calloc(1, sizeof(*context));
    if (context == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); return -1; }
    context->data_directory = data_directory != NULL ? strdup(data_directory) : default_data_directory();
    if (context->data_directory == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Cannot determine application data directory");
        goto fail;
    }
    if (ensure_data_directory(context->data_directory, error) != 0) goto fail;
    path_length = strlen(context->data_directory) + sizeof("/ghm.db");
    db_path = malloc(path_length);
    if (db_path == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); goto fail; }
    (void)snprintf(db_path, path_length, "%s/ghm.db", context->data_directory);
    if (git_libgit2_init() < 0) { ghm_error_from_git(error, "Initialize libgit2"); goto fail; }
#ifdef _WIN32
    if (g_getenv("GHM_CA_BUNDLE") == NULL) {
        char *bundle = ghm_windows_ca_bundle(context->data_directory);
        if (bundle != NULL) { g_setenv("GHM_CA_BUNDLE", bundle, FALSE); g_free(bundle); }
    }
#endif
    const char *ca_bundle = getenv("GHM_CA_BUNDLE");
    if (ca_bundle != NULL && ca_bundle[0] != '\0' &&
        git_libgit2_opts(GIT_OPT_SET_SSL_CERT_LOCATIONS, ca_bundle, NULL) < 0) {
        ghm_error_from_git(error, "Load configured certificate trust store");
        git_libgit2_shutdown();
        goto fail;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        ghm_error_set(error, GHM_ERROR_NETWORK, "Cannot initialize libcurl");
        git_libgit2_shutdown();
        goto fail;
    }
    if (pthread_mutex_init(&context->database_mutex, NULL) != 0) {
        ghm_error_set(error, GHM_ERROR_IO, "Cannot initialize database lock");
        curl_global_cleanup();
        git_libgit2_shutdown();
        goto fail;
    }
    if (sqlite3_open_v2(db_path, &context->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        ghm_error_set(error, GHM_ERROR_DATABASE, sqlite3_errmsg(context->db));
        goto fail_initialized;
    }
    (void)sqlite3_busy_timeout(context->db, 5000);
    if (sqlite3_exec(context->db, "PRAGMA foreign_keys = ON", NULL, NULL, NULL) != SQLITE_OK ||
        ghm_database_migrate(context->db, error) != 0) {
        if (error != NULL && error->code == GHM_OK)
            ghm_error_set(error, GHM_ERROR_DATABASE, sqlite3_errmsg(context->db));
        goto fail_initialized;
    }
    free(db_path);
    *out = context;
    if (error != NULL) { error->code = GHM_OK; error->message[0] = '\0'; }
    return 0;

fail_initialized:
    sqlite3_close(context->db);
    pthread_mutex_destroy(&context->database_mutex);
    curl_global_cleanup();
    git_libgit2_shutdown();
fail:
    free(db_path);
    free(context->data_directory);
    free(context);
    return -1;
}

void ghm_context_close(GhmContext *context)
{
    if (context == NULL) return;
    sqlite3_close(context->db);
    pthread_mutex_destroy(&context->database_mutex);
    curl_global_cleanup();
    git_libgit2_shutdown();
    free(context->data_directory);
    free(context);
}

const char *ghm_context_data_directory(const GhmContext *context)
{
    return context != NULL ? context->data_directory : NULL;
}

int ghm_setting_get(GhmContext *context, const char *key, char **out, GhmError *error)
{
    sqlite3_stmt *statement = NULL;
    int step;
    int result = -1;
    if (context == NULL || key == NULL || out == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Setting key and output are required");
        return -1;
    }
    *out = NULL;
    pthread_mutex_lock(&context->database_mutex);
    if (sqlite3_prepare_v2(context->db, "SELECT value FROM application_settings WHERE key=?1",
                           -1, &statement, NULL) != SQLITE_OK ||
        sqlite3_bind_text(statement, 1, key, -1, SQLITE_TRANSIENT) != SQLITE_OK) goto error_out;
    step = sqlite3_step(statement);
    if (step == SQLITE_ROW) {
        *out = strdup((const char *)sqlite3_column_text(statement, 0));
        if (*out == NULL) {
            ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
            goto done;
        }
    } else if (step != SQLITE_DONE) goto error_out;
    result = 0;
    goto done;
error_out:
    ghm_error_set(error, GHM_ERROR_DATABASE, sqlite3_errmsg(context->db));
done:
    sqlite3_finalize(statement);
    pthread_mutex_unlock(&context->database_mutex);
    return result;
}

int ghm_setting_set(GhmContext *context, const char *key, const char *value, GhmError *error)
{
    static const char sql[] = "INSERT INTO application_settings(key,value) VALUES(?1,?2) "
                              "ON CONFLICT(key) DO UPDATE SET value=excluded.value";
    sqlite3_stmt *statement = NULL;
    int result = -1;
    if (context == NULL || key == NULL || value == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Setting key and value are required");
        return -1;
    }
    pthread_mutex_lock(&context->database_mutex);
    if (sqlite3_prepare_v2(context->db, sql, -1, &statement, NULL) == SQLITE_OK &&
        sqlite3_bind_text(statement, 1, key, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 2, value, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_DONE) result = 0;
    else ghm_error_set(error, GHM_ERROR_DATABASE, sqlite3_errmsg(context->db));
    sqlite3_finalize(statement);
    pthread_mutex_unlock(&context->database_mutex);
    return result;
}

int ghm_database_register(GhmContext *context, const char *name, const char *path, GhmError *error)
{
    static const char sql[] = "INSERT INTO repositories(name,path) VALUES(?1,?2) "
                              "ON CONFLICT(path) DO UPDATE SET name=excluded.name";
    sqlite3_stmt *statement = NULL;
    int result = -1;
    pthread_mutex_lock(&context->database_mutex);
    if (sqlite3_prepare_v2(context->db, sql, -1, &statement, NULL) == SQLITE_OK &&
        sqlite3_bind_text(statement, 1, name, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 2, path, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_DONE) {
        result = 0;
    } else {
        ghm_error_set(error, GHM_ERROR_DATABASE, sqlite3_errmsg(context->db));
    }
    sqlite3_finalize(statement);
    pthread_mutex_unlock(&context->database_mutex);
    return result;
}

int ghm_database_branch_has_active_jobs(GhmContext *context, const char *workdir,
                                        const char *branch, int *out_active, GhmError *error)
{
    static const char sql[] =
        "SELECT DISTINCT r.path FROM scheduled_jobs j JOIN repositories r "
        "ON r.id=j.repository_id WHERE j.branch=?1 AND j.status IN ('PENDING','RUNNING')";
    sqlite3_stmt *stmt = NULL;
    struct stat worktree;
    int step, result = -1;
    if (context == NULL || workdir == NULL || branch == NULL || out_active == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Worktree, branch and job output are required");
        return -1;
    }
    *out_active = 0;
    if (stat(workdir, &worktree) != 0 || !S_ISDIR(worktree.st_mode)) {
        ghm_error_set(error, GHM_ERROR_IO, "Cannot identify worktree for scheduled job safety check");
        return -1;
    }
    pthread_mutex_lock(&context->database_mutex);
    if (sqlite3_prepare_v2(context->db, sql, -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 1, branch, -1, SQLITE_TRANSIENT) != SQLITE_OK) goto db_fail;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(stmt, 0);
        struct stat registered;
        /* An unavailable registered path cannot designate this open worktree.
         * Match existing aliases without rewriting stored paths or job refs. */
        if (path != NULL && stat(path, &registered) == 0 &&
            registered.st_dev == worktree.st_dev && registered.st_ino == worktree.st_ino) {
            *out_active = 1;
            result = 0;
            goto done;
        }
    }
    if (step != SQLITE_DONE) goto db_fail;
    result = 0;
    goto done;
db_fail:
    ghm_error_set(error, GHM_ERROR_DATABASE, sqlite3_errmsg(context->db));
done:
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&context->database_mutex);
    return result;
}

int ghm_database_find_github(GhmContext *context, int64_t github_id,
                             char **out_path, GhmError *error)
{
    sqlite3_stmt *statement = NULL;
    int result = -1;
    int step;
    if (context == NULL || out_path == NULL || github_id <= 0) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "GitHub repository ID and output are required");
        return -1;
    }
    *out_path = NULL;
    pthread_mutex_lock(&context->database_mutex);
    if (sqlite3_prepare_v2(context->db, "SELECT path FROM repositories WHERE github_id=?1",
                           -1, &statement, NULL) != SQLITE_OK ||
        sqlite3_bind_int64(statement, 1, github_id) != SQLITE_OK) goto db_fail;
    step = sqlite3_step(statement);
    if (step == SQLITE_ROW) {
        *out_path = strdup((const char *)sqlite3_column_text(statement, 0));
        if (*out_path == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); goto done; }
    } else if (step != SQLITE_DONE) goto db_fail;
    result = 0;
    goto done;
db_fail:
    ghm_error_set(error, GHM_ERROR_DATABASE, sqlite3_errmsg(context->db));
done:
    sqlite3_finalize(statement);
    pthread_mutex_unlock(&context->database_mutex);
    return result;
}

int ghm_database_attach_github(GhmContext *context, const char *name, const char *path,
                               int64_t github_id, const char *remote_url, GhmError *error)
{
    static const char sql[] =
        "INSERT INTO repositories(name,path,github_id,remote_url) VALUES(?1,?2,?3,?4) "
        "ON CONFLICT(path) DO UPDATE SET name=excluded.name,github_id=excluded.github_id,"
        "remote_url=excluded.remote_url";
    sqlite3_stmt *statement = NULL;
    int result = -1;
    if (context == NULL || name == NULL || path == NULL || github_id <= 0 || remote_url == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "GitHub repository metadata is required");
        return -1;
    }
    pthread_mutex_lock(&context->database_mutex);
    if (sqlite3_prepare_v2(context->db, sql, -1, &statement, NULL) == SQLITE_OK &&
        sqlite3_bind_text(statement, 1, name, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 2, path, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_int64(statement, 3, github_id) == SQLITE_OK &&
        sqlite3_bind_text(statement, 4, remote_url, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_DONE) result = 0;
    else ghm_error_set(error, GHM_ERROR_DATABASE, sqlite3_errmsg(context->db));
    sqlite3_finalize(statement);
    pthread_mutex_unlock(&context->database_mutex);
    return result;
}

int ghm_repo_list(GhmContext *context, GhmRepositoryList *out, GhmError *error)
{
    static const char sql[] = "SELECT id,name,path FROM repositories ORDER BY name COLLATE NOCASE";
    sqlite3_stmt *statement = NULL;
    size_t capacity = 0;
    int step;
    if (context == NULL || out == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Context and output list are required");
        return -1;
    }
    *out = (GhmRepositoryList){0};
    pthread_mutex_lock(&context->database_mutex);
    if (sqlite3_prepare_v2(context->db, sql, -1, &statement, NULL) != SQLITE_OK) goto database_error;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
        GhmRepository *item;
        if (out->count == capacity) {
            size_t next = capacity == 0 ? 8 : capacity * 2;
            GhmRepository *grown = realloc(out->items, next * sizeof(*grown));
            if (grown == NULL) goto memory_error;
            out->items = grown;
            capacity = next;
        }
        item = &out->items[out->count];
        item->id = sqlite3_column_int64(statement, 0);
        item->name = strdup((const char *)sqlite3_column_text(statement, 1));
        item->path = strdup((const char *)sqlite3_column_text(statement, 2));
        if (item->name == NULL || item->path == NULL) {
            free(item->name); free(item->path);
            goto memory_error;
        }
        ++out->count;
    }
    if (step != SQLITE_DONE) goto database_error;
    sqlite3_finalize(statement);
    pthread_mutex_unlock(&context->database_mutex);
    return 0;

memory_error:
    ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
    goto fail;
database_error:
    ghm_error_set(error, GHM_ERROR_DATABASE, sqlite3_errmsg(context->db));
fail:
    sqlite3_finalize(statement);
    pthread_mutex_unlock(&context->database_mutex);
    ghm_repo_list_free(out);
    return -1;
}

void ghm_repo_list_free(GhmRepositoryList *list)
{
    if (list == NULL) return;
    for (size_t i = 0; i < list->count; ++i) {
        free(list->items[i].name);
        free(list->items[i].path);
    }
    free(list->items);
    *list = (GhmRepositoryList){0};
}
