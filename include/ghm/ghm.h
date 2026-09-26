#ifndef GHM_GHM_H
#define GHM_GHM_H

#include <stddef.h>
#include <stdint.h>

typedef struct GhmContext GhmContext;

typedef enum {
    GHM_OK = 0,
    GHM_ERROR_ARGUMENT,
    GHM_ERROR_MEMORY,
    GHM_ERROR_IO,
    GHM_ERROR_DATABASE,
    GHM_ERROR_GIT,
    GHM_ERROR_NETWORK,
    GHM_ERROR_AUTH,
    GHM_ERROR_SECRET,
    GHM_ERROR_BUSY,
    GHM_ERROR_RATE_LIMIT
} GhmErrorCode;

typedef struct {
    GhmErrorCode code;
    char message[256];
    int64_t retry_at; /* Unix time; 0 when the transport supplied no delay. */
} GhmError;

typedef struct {
    int64_t id;
    char *name;
    char *path;
} GhmRepository;

typedef struct {
    GhmRepository *items;
    size_t count;
} GhmRepositoryList;

typedef enum {
    GHM_STATUS_MODIFIED,
    GHM_STATUS_ADDED,
    GHM_STATUS_DELETED,
    GHM_STATUS_RENAMED,
    GHM_STATUS_UNTRACKED,
    GHM_STATUS_CONFLICTED
} GhmStatusKind;

typedef struct {
    char *path;
    GhmStatusKind kind;
    int staged;
    int unstaged;
} GhmStatusEntry;

typedef struct {
    GhmStatusEntry *items;
    size_t count;
    char *branch;
    int detached;
} GhmStatus;

/* The context owns one SQLite connection. Its database methods are mutex-guarded. */
int ghm_context_open(const char *data_directory, GhmContext **out, GhmError *error);
void ghm_context_close(GhmContext *context);
const char *ghm_context_data_directory(const GhmContext *context);
int ghm_setting_get(GhmContext *context, const char *key, char **out, GhmError *error);
int ghm_setting_set(GhmContext *context, const char *key, const char *value, GhmError *error);

int ghm_repo_discover(GhmContext *context, GhmError *error);
int ghm_repo_register(GhmContext *context, const char *path, GhmError *error);
/* Find the worktree root at or above start. Caller frees out_path. */
int ghm_repo_find(const char *start, char **out_path, GhmError *error);
int ghm_repo_list(GhmContext *context, GhmRepositoryList *out, GhmError *error);
void ghm_repo_list_free(GhmRepositoryList *list);
int ghm_repo_status(const char *path, GhmStatus *out, GhmError *error);
void ghm_status_free(GhmStatus *status);
const char *ghm_status_kind_label(GhmStatusKind kind);

typedef struct {
    char *path;
    int is_directory;
} GhmFileEntry;

typedef struct {
    GhmFileEntry *items;
    size_t count;
    int truncated;
} GhmFileList;

/* Non-ignored and ignored working-tree files are listed; .git is excluded.
 * The list is capped so a large repository cannot freeze the GUI. */
int ghm_repo_files(const char *path, GhmFileList *out, GhmError *error);
void ghm_file_list_free(GhmFileList *list);

/* Stage or unstage exactly one repository-relative path in the real Git index. */
int ghm_repo_stage_path(const char *repository_path, const char *relative_path,
                        GhmError *error);
int ghm_repo_unstage_path(const char *repository_path, const char *relative_path,
                          GhmError *error);
int ghm_repo_stage_all(const char *repository_path, GhmError *error);

#endif
