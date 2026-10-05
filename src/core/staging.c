#include <ghm/ghm.h>
#include "core/git.h"
#include "core/lock.h"
#include "core/index_lock.h"

#include <git2.h>
#include <string.h>

static int valid_relative_path(const char *path)
{
    const char *part;
    if (path == NULL || path[0] == '\0' || path[0] == '/') return 0;
#ifdef _WIN32
    if (strpbrk(path, "\\:") != NULL) return 0;
#endif
    part = path;
    for (const char *cursor = path;; ++cursor) {
        if (*cursor == '/' || *cursor == '\0') {
            size_t length = (size_t)(cursor - part);
            if (length == 0 || (length == 1 && part[0] == '.') ||
                (length == 2 && part[0] == '.' && part[1] == '.')) return 0;
            if (*cursor == '\0') break;
            part = cursor + 1;
        }
    }
    return 1;
}

static int open_index(git_repository **repository, git_index **index,
                      GhmIndexLock *lock, const char *path, GhmError *error)
{
    if (path == NULL || path[0] == '\0') {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Repository path is required");
        return -1;
    }
    if (git_repository_open(repository, path) < 0) {
        ghm_error_from_git(error, "Open repository");
        return -1;
    }
    if (git_repository_is_bare(*repository)) {
        ghm_error_set(error, GHM_ERROR_GIT, "A working tree is required");
        return -1;
    }
    if (ghm_index_lock_acquire(lock, *repository, error) != 0) return -1;
    if (git_repository_index(index, *repository) < 0 || git_index_read(*index, 1) < 0) {
        ghm_error_from_git(error, "Read Git index");
        return -1;
    }
    if (git_index_has_conflicts(*index)) {
        ghm_error_set(error, GHM_ERROR_GIT, "Resolve index conflicts before staging");
        return -1;
    }
    return 0;
}

/* git_status_file intentionally omits rename detection. Resolve the pair so
 * staging one renamed row adds its new path and removes only its old path. */
static int stage_rename(git_repository *repository, git_index *index,
                        const char *relative_path, GhmError *error)
{
    git_status_options options = {0};
    git_status_list *list = NULL;
    int result = 0;
    if (git_status_options_init(&options, GIT_STATUS_OPTIONS_VERSION) < 0) {
        ghm_error_from_git(error, "Initialize rename status options");
        return -1;
    }
    options.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
                    GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS |
                    GIT_STATUS_OPT_RENAMES_INDEX_TO_WORKDIR;
    if (git_status_list_new(&list, repository, &options) < 0) {
        ghm_error_from_git(error, "Inspect renamed files");
        return -1;
    }
    for (size_t i = 0; i < git_status_list_entrycount(list); ++i) {
        const git_status_entry *entry = git_status_byindex(list, i);
        const git_diff_delta *delta = entry != NULL ? entry->index_to_workdir : NULL;
        if (entry == NULL || (entry->status & GIT_STATUS_WT_RENAMED) == 0U ||
            delta == NULL || delta->old_file.path == NULL || delta->new_file.path == NULL)
            continue;
        if (strcmp(delta->old_file.path, relative_path) != 0 &&
            strcmp(delta->new_file.path, relative_path) != 0) continue;
        if (git_index_remove_bypath(index, delta->old_file.path) < 0 ||
            git_index_add_bypath(index, delta->new_file.path) < 0) {
            ghm_error_from_git(error, "Stage renamed file");
            result = -1;
        } else result = 1;
        break;
    }
    git_status_list_free(list);
    return result;
}

static int reset_index_path(git_index *index, git_object *head, const char *path)
{
    git_tree *tree = NULL;
    git_tree_entry *entry = NULL;
    if (git_commit_tree(&tree, (git_commit *)head) < 0) return -1;
    int result = git_tree_entry_bypath(&entry, tree, path);
    if (result == GIT_ENOTFOUND) {
        result = git_index_remove_bypath(index, path);
        if (result == GIT_ENOTFOUND) result = 0;
    } else if (result == 0) {
        git_index_entry restored = {0};
        restored.path = path;
        restored.mode = (uint32_t)git_tree_entry_filemode(entry);
        restored.id = *git_tree_entry_id(entry);
        result = git_index_add(index, &restored);
    }
    git_tree_entry_free(entry);
    git_tree_free(tree);
    return result;
}

static int unstage_rename(git_repository *repository, git_index *index, git_object *head,
                          const char *relative_path, GhmError *error)
{
    git_status_options options = {0};
    git_status_list *list = NULL;
    int result = 0;
    if (git_status_options_init(&options, GIT_STATUS_OPTIONS_VERSION) < 0) {
        ghm_error_from_git(error, "Initialize rename status options");
        return -1;
    }
    options.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    options.flags = GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX;
    if (git_status_list_new(&list, repository, &options) < 0) {
        ghm_error_from_git(error, "Inspect staged rename");
        return -1;
    }
    for (size_t i = 0; i < git_status_list_entrycount(list); ++i) {
        const git_status_entry *entry = git_status_byindex(list, i);
        const git_diff_delta *delta = entry != NULL ? entry->head_to_index : NULL;
        if (entry == NULL || (entry->status & GIT_STATUS_INDEX_RENAMED) == 0U ||
            delta == NULL || delta->old_file.path == NULL || delta->new_file.path == NULL)
            continue;
        if (strcmp(delta->old_file.path, relative_path) != 0 &&
            strcmp(delta->new_file.path, relative_path) != 0) continue;
        if (reset_index_path(index, head, delta->old_file.path) < 0 ||
            reset_index_path(index, head, delta->new_file.path) < 0) {
            ghm_error_from_git(error, "Unstage renamed file");
            result = -1;
        } else result = 1;
        break;
    }
    git_status_list_free(list);
    return result;
}

static int ghm_repo_stage_path_unlocked(const char *repository_path, const char *relative_path,
                        GhmError *error)
{
    git_repository *repository = NULL;
    git_index *index = NULL;
    GhmIndexLock index_lock = {.descriptor = -1};
    unsigned int status = 0;
    int result = -1;
    if (!valid_relative_path(relative_path)) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Choose a repository-relative file path");
        return -1;
    }
    if (open_index(&repository, &index, &index_lock, repository_path, error) != 0) goto done;
    int status_result = git_status_file(&status, repository, relative_path);
    if (status_result < 0 && status_result != GIT_ENOTFOUND) {
        ghm_error_from_git(error, "Inspect file status");
        goto done;
    }
    if (status_result == GIT_ENOTFOUND ||
        (status & (GIT_STATUS_WT_NEW | GIT_STATUS_WT_DELETED)) != 0U) {
        int renamed = stage_rename(repository, index, relative_path, error);
        if (renamed < 0) goto done;
        if (renamed > 0) goto write_index;
    }
    if (status_result == GIT_ENOTFOUND) {
        ghm_error_set(error, GHM_ERROR_GIT, "File is not present in the working tree or index");
        goto done;
    }
    if ((status & GIT_STATUS_WT_DELETED) != 0U) {
        if (git_index_remove_bypath(index, relative_path) < 0) {
            ghm_error_from_git(error, "Stage file deletion");
            goto done;
        }
    } else if (git_index_add_bypath(index, relative_path) < 0) {
        ghm_error_from_git(error, "Stage file");
        goto done;
    }
write_index:
    result = ghm_index_lock_write(&index_lock, index, error);
done:
    ghm_index_lock_release(&index_lock);
    git_index_free(index);
    git_repository_free(repository);
    return result;
}

static int ghm_repo_unstage_path_unlocked(const char *repository_path, const char *relative_path,
                          GhmError *error)
{
    git_repository *repository = NULL;
    git_index *index = NULL;
    GhmIndexLock index_lock = {.descriptor = -1};
    git_object *head = NULL;
    int result = -1;
    int head_result;
    if (!valid_relative_path(relative_path)) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Choose a repository-relative file path");
        return -1;
    }
    if (open_index(&repository, &index, &index_lock, repository_path, error) != 0) goto done;
    head_result = git_revparse_single(&head, repository, "HEAD^{commit}");
    if (head_result == GIT_ENOTFOUND || head_result == GIT_EUNBORNBRANCH) {
        if (git_index_remove_bypath(index, relative_path) < 0) {
            ghm_error_from_git(error, "Unstage file");
            goto done;
        }
    } else if (head_result < 0) {
        ghm_error_from_git(error, "Read HEAD");
        goto done;
    } else {
        int rename_result = unstage_rename(repository, index, head, relative_path, error);
        if (rename_result < 0) goto done;
        if (rename_result == 0 && reset_index_path(index, head, relative_path) < 0) {
            ghm_error_from_git(error, "Unstage file");
            goto done;
        }
    }
    result = ghm_index_lock_write(&index_lock, index, error);
done:
    ghm_index_lock_release(&index_lock);
    git_object_free(head);
    git_index_free(index);
    git_repository_free(repository);
    return result;
}

static int ghm_repo_stage_all_unlocked(const char *repository_path, GhmError *error)
{
    git_repository *repository = NULL;
    git_index *index = NULL;
    GhmIndexLock index_lock = {.descriptor = -1};
    int result = -1;
    if (open_index(&repository, &index, &index_lock, repository_path, error) != 0) goto done;
    if (git_index_update_all(index, NULL, NULL, NULL) < 0 ||
        git_index_add_all(index, NULL, 0U, NULL, NULL) < 0) {
        ghm_error_from_git(error, "Stage all changes");
        goto done;
    }
    result = ghm_index_lock_write(&index_lock, index, error);
done:
    ghm_index_lock_release(&index_lock);
    git_index_free(index);
    git_repository_free(repository);
    return result;
}

int ghm_repo_stage_path(const char *repository_path, const char *relative_path,
                        GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (ghm_repo_lock_acquire(repository_path, &lock, error) != 0) return -1;
    int result = ghm_repo_stage_path_unlocked(repository_path, relative_path, error);
    ghm_repo_lock_release(lock);
    return result;
}

int ghm_repo_unstage_path(const char *repository_path, const char *relative_path,
                          GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (ghm_repo_lock_acquire(repository_path, &lock, error) != 0) return -1;
    int result = ghm_repo_unstage_path_unlocked(repository_path, relative_path, error);
    ghm_repo_lock_release(lock);
    return result;
}

int ghm_repo_stage_all(const char *repository_path, GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (ghm_repo_lock_acquire(repository_path, &lock, error) != 0) return -1;
    int result = ghm_repo_stage_all_unlocked(repository_path, error);
    ghm_repo_lock_release(lock);
    return result;
}
