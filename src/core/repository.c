#include <ghm/ghm.h>
#include "core/git.h"
#include "storage/database.h"

#include <dirent.h>
#include <git2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "platform/io.h"

int ghm_repo_find(const char *start, char **out_path, GhmError *error)
{
    git_repository *repository = NULL;
    const char *workdir;
    size_t length;
    if (start == NULL || start[0] == '\0' || out_path == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Starting path and output are required");
        return -1;
    }
    *out_path = NULL;
    if (git_repository_open_ext(&repository, start, 0, NULL) < 0) {
        ghm_error_from_git(error, "Find local repository");
        return -1;
    }
    workdir = git_repository_workdir(repository);
    if (workdir == NULL) {
        ghm_error_set(error, GHM_ERROR_GIT, "A working-tree repository is required");
        git_repository_free(repository);
        return -1;
    }
    length = strlen(workdir);
    while (length > 1 && workdir[length - 1] == '/') --length;
    *out_path = strndup(workdir, length);
    git_repository_free(repository);
    if (*out_path == NULL) {
        ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
        return -1;
    }
    return 0;
}

int ghm_repo_register(GhmContext *context, const char *path, GhmError *error)
{
    git_repository *repository = NULL;
    const char *name;
    int result;
    if (context == NULL || path == NULL || path[0] == '\0') {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Repository path is required");
        return -1;
    }
    if (git_repository_open(&repository, path) < 0) {
        ghm_error_from_git(error, "Open repository");
        return -1;
    }
    /* Store the actual worktree root; selecting a nested folder is harmless. */
    const char *workdir = git_repository_workdir(repository);
    const char *repo_path = workdir != NULL ? workdir : git_repository_path(repository);
    size_t length = strlen(repo_path);
    while (length > 1 && repo_path[length - 1] == '/') --length;
    char *normalized = strndup(repo_path, length);
    if (normalized == NULL) {
        ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
        git_repository_free(repository);
        return -1;
    }
    name = strrchr(normalized, '/');
    name = name != NULL ? name + 1 : normalized;
    result = ghm_database_register(context, name, normalized, error);
    free(normalized);
    git_repository_free(repository);
    return result;
}

int ghm_repo_discover(GhmContext *context, GhmError *error)
{
    char *root;
    DIR *directory;
    struct dirent *entry;
    size_t root_length;
    if (context == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Context is required");
        return -1;
    }
    root_length = strlen(context->data_directory) + sizeof("/repos");
    root = malloc(root_length);
    if (root == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); return -1; }
    (void)snprintf(root, root_length, "%s/repos", context->data_directory);
    if (mkdir(root, 0700) != 0) {
        struct stat stat_result;
        if (stat(root, &stat_result) != 0 || !S_ISDIR(stat_result.st_mode)) {
            ghm_error_set(error, GHM_ERROR_IO, "Cannot create repository directory");
            free(root);
            return -1;
        }
    }
    directory = opendir(root);
    if (directory == NULL) {
        ghm_error_set(error, GHM_ERROR_IO, "Cannot scan repository directory");
        free(root);
        return -1;
    }
    while ((entry = readdir(directory)) != NULL) {
        char *candidate;
        size_t candidate_length;
        struct stat stat_result;
        if (entry->d_name[0] == '.') continue;
        candidate_length = strlen(root) + strlen(entry->d_name) + 2;
        candidate = malloc(candidate_length);
        if (candidate == NULL) {
            ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
            closedir(directory); free(root); return -1;
        }
        (void)snprintf(candidate, candidate_length, "%s/%s", root, entry->d_name);
        if (stat(candidate, &stat_result) == 0 && S_ISDIR(stat_result.st_mode)) {
            GhmError ignored = {0};
            if (ghm_repo_register(context, candidate, &ignored) != 0 &&
                (ignored.code == GHM_ERROR_DATABASE || ignored.code == GHM_ERROR_MEMORY)) {
                if (error != NULL) *error = ignored;
                free(candidate);
                closedir(directory);
                free(root);
                return -1;
            }
        }
        free(candidate);
    }
    closedir(directory);
    free(root);
    return 0;
}

typedef struct {
    GhmStatus *status;
    size_t capacity;
    int out_of_memory;
} StatusCollector;

static int collect_status(const char *path, unsigned int flags, void *payload)
{
    StatusCollector *collector = payload;
    GhmStatusKind kind;
    GhmStatusEntry *grown;
    if ((flags & GIT_STATUS_CONFLICTED) != 0U) kind = GHM_STATUS_CONFLICTED;
    else if ((flags & (GIT_STATUS_INDEX_RENAMED | GIT_STATUS_WT_RENAMED)) != 0U) kind = GHM_STATUS_RENAMED;
    else if ((flags & (GIT_STATUS_INDEX_DELETED | GIT_STATUS_WT_DELETED)) != 0U) kind = GHM_STATUS_DELETED;
    else if ((flags & GIT_STATUS_WT_NEW) != 0U) kind = GHM_STATUS_UNTRACKED;
    else if ((flags & GIT_STATUS_INDEX_NEW) != 0U) kind = GHM_STATUS_ADDED;
    else kind = GHM_STATUS_MODIFIED;
    if (collector->status->count == collector->capacity) {
        size_t next = collector->capacity == 0 ? 16 : collector->capacity * 2;
        grown = realloc(collector->status->items, next * sizeof(*grown));
        if (grown == NULL) { collector->out_of_memory = 1; return -1; }
        collector->status->items = grown;
        collector->capacity = next;
    }
    GhmStatusEntry *item = &collector->status->items[collector->status->count];
    item->path = strdup(path);
    if (item->path == NULL) { collector->out_of_memory = 1; return -1; }
    item->kind = kind;
    item->staged = (flags & (GIT_STATUS_INDEX_NEW | GIT_STATUS_INDEX_MODIFIED |
                             GIT_STATUS_INDEX_DELETED | GIT_STATUS_INDEX_RENAMED |
                             GIT_STATUS_INDEX_TYPECHANGE)) != 0U;
    item->unstaged = (flags & (GIT_STATUS_WT_NEW | GIT_STATUS_WT_MODIFIED |
                               GIT_STATUS_WT_DELETED | GIT_STATUS_WT_RENAMED |
                               GIT_STATUS_WT_TYPECHANGE)) != 0U;
    ++collector->status->count;
    return 0;
}

int ghm_repo_status(const char *path, GhmStatus *out, GhmError *error)
{
    git_repository *repository = NULL;
    git_reference *head = NULL;
    git_status_options options = {0};
    StatusCollector collector;
    int head_result;
    if (path == NULL || out == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Repository path and output are required");
        return -1;
    }
    *out = (GhmStatus){0};
    if (git_repository_open(&repository, path) < 0) {
        ghm_error_from_git(error, "Open repository");
        return -1;
    }
    if (git_repository_is_bare(repository)) {
        ghm_error_set(error, GHM_ERROR_GIT, "Bare repositories have no working tree");
        git_repository_free(repository);
        return -1;
    }
    head_result = git_repository_head(&head, repository);
    if (head_result == 0) {
        out->detached = git_repository_head_detached(repository);
        const char *branch = out->detached ? "Detached HEAD" : git_reference_shorthand(head);
        out->branch = strdup(branch);
    } else if (head_result == GIT_EUNBORNBRANCH) {
        out->branch = strdup("Unborn branch");
    } else {
        ghm_error_from_git(error, "Read repository HEAD");
        git_repository_free(repository);
        return -1;
    }
    if (out->branch == NULL) {
        ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
        git_reference_free(head); git_repository_free(repository);
        return -1;
    }
    if (git_status_options_init(&options, GIT_STATUS_OPTIONS_VERSION) < 0) {
        ghm_error_from_git(error, "Initialize status options");
        ghm_status_free(out);
        git_reference_free(head); git_repository_free(repository);
        return -1;
    }
    options.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS |
                    GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX | GIT_STATUS_OPT_RENAMES_INDEX_TO_WORKDIR;
    collector = (StatusCollector){.status = out};
    if (git_status_foreach_ext(repository, &options, collect_status, &collector) < 0) {
        if (collector.out_of_memory) ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
        else ghm_error_from_git(error, "Read working tree status");
        ghm_status_free(out);
        git_reference_free(head); git_repository_free(repository);
        return -1;
    }
    git_reference_free(head);
    git_repository_free(repository);
    return 0;
}

void ghm_status_free(GhmStatus *status)
{
    if (status == NULL) return;
    for (size_t i = 0; i < status->count; ++i) free(status->items[i].path);
    free(status->items);
    free(status->branch);
    *status = (GhmStatus){0};
}

const char *ghm_status_kind_label(GhmStatusKind kind)
{
    switch (kind) {
    case GHM_STATUS_MODIFIED: return "M";
    case GHM_STATUS_ADDED: return "A";
    case GHM_STATUS_DELETED: return "D";
    case GHM_STATUS_RENAMED: return "R";
    case GHM_STATUS_UNTRACKED: return "?";
    case GHM_STATUS_CONFLICTED: return "!";
    }
    return "?";
}
