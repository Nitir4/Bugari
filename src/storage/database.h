#ifndef GHM_STORAGE_DATABASE_H
#define GHM_STORAGE_DATABASE_H

#include <ghm/ghm.h>
#include <pthread.h>
#include <sqlite3.h>

struct GhmContext {
    sqlite3 *db;
    pthread_mutex_t database_mutex;
    char *data_directory;
};

int ghm_database_register(GhmContext *context, const char *name, const char *path, GhmError *error);
int ghm_database_find_github(GhmContext *context, int64_t github_id,
                             char **out_path, GhmError *error);
int ghm_database_attach_github(GhmContext *context, const char *name, const char *path,
                               int64_t github_id, const char *remote_url, GhmError *error);
/* Match the opened worktree by filesystem identity, including registered path
 * aliases. Both PENDING and RUNNING commit jobs block branch mutations. This
 * is a preflight check, not a cross-process repository operation lock. */
int ghm_database_branch_has_active_jobs(GhmContext *context, const char *workdir,
                                        const char *branch, int *out_active, GhmError *error);

#endif
