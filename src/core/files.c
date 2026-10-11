#include <ghm/ghm.h>
#include "core/git.h"

#include <dirent.h>
#include <errno.h>
#include <git2.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "platform/io.h"

#define GHM_FILE_LIMIT 5000U

typedef struct {
    GhmFileList *list;
    size_t capacity;
    const char *root;
    GhmError *error;
} FileWalker;

static char *join_path(const char *first, const char *second)
{
    size_t first_length = strlen(first);
    size_t second_length = strlen(second);
    if (first_length > SIZE_MAX - second_length - 1U) return NULL;
    char *joined = malloc(first_length + second_length + 1U);
    if (joined == NULL) return NULL;
    memcpy(joined, first, first_length);
    memcpy(joined + first_length, second, second_length + 1U);
    return joined;
}

void ghm_file_list_free(GhmFileList *list)
{
    if (list == NULL) return;
    for (size_t i = 0; i < list->count; ++i) free(list->items[i].path);
    free(list->items);
    *list = (GhmFileList){0};
}

static int append_file(FileWalker *walker, const char *relative, int is_directory)
{
    char *copy;
    if (walker->list->count >= GHM_FILE_LIMIT) {
        walker->list->truncated = 1;
        return 0;
    }
    copy = strdup(relative);
    if (copy == NULL) {
        ghm_error_set(walker->error, GHM_ERROR_MEMORY, "Out of memory listing files");
        return -1;
    }
    if (walker->list->count == walker->capacity) {
        size_t next = walker->capacity == 0 ? 64U : walker->capacity * 2U;
        GhmFileEntry *grown = realloc(walker->list->items, next * sizeof(*grown));
        if (grown == NULL) {
            ghm_error_set(walker->error, GHM_ERROR_MEMORY, "Out of memory listing files");
            free(copy);
            return -1;
        }
        walker->list->items = grown;
        walker->capacity = next;
    }
    GhmFileEntry *entry = &walker->list->items[walker->list->count];
    entry->path = copy;
    entry->is_directory = is_directory;
    ++walker->list->count;
    return 0;
}

static int walk_directory(FileWalker *walker, const char *relative)
{
    struct dirent **names = NULL;
    char *absolute = NULL;
    int count;
    int result = -1;
    absolute = join_path(walker->root, relative);
    if (absolute == NULL) {
        ghm_error_set(walker->error, GHM_ERROR_MEMORY, "Out of memory listing files");
        return -1;
    }
    count = scandir(absolute, &names, NULL, alphasort);
    free(absolute);
    if (count < 0) {
        if (errno == EACCES) return 0;
        ghm_error_set(walker->error, GHM_ERROR_IO, "Cannot list repository directory");
        return -1;
    }
    result = 0;
    for (int i = 0; i < count; ++i) {
        char *child = NULL;
        char *full = NULL;
        struct stat metadata;
        if (strcmp(names[i]->d_name, ".") == 0 ||
            strcmp(names[i]->d_name, "..") == 0 ||
            strcmp(names[i]->d_name, ".git") == 0) continue;
#ifdef _WIN32
        if (g_ascii_strcasecmp(names[i]->d_name, ".git") == 0) continue;
#endif
        child = join_path(relative, names[i]->d_name);
        if (child != NULL) full = join_path(walker->root, child);
        if (child == NULL || full == NULL) {
            ghm_error_set(walker->error, GHM_ERROR_MEMORY, "Out of memory listing files");
            result = -1;
            goto entry_done;
        }
        if (lstat(full, &metadata) == 0) {
            int is_directory = S_ISDIR(metadata.st_mode) ? 1 : 0;
            if (append_file(walker, child, is_directory) != 0) result = -1;
            else if (is_directory && !walker->list->truncated) {
                char *nested = join_path(child, "/");
                if (nested == NULL) {
                    ghm_error_set(walker->error, GHM_ERROR_MEMORY, "Out of memory listing files");
                    result = -1;
                } else result = walk_directory(walker, nested);
                free(nested);
            }
        } else if (errno != ENOENT) {
            ghm_error_set(walker->error, GHM_ERROR_IO, "Cannot inspect repository file");
            result = -1;
        }
entry_done:
        free(full);
        free(child);
        if (result != 0 || walker->list->truncated) break;
    }
    for (int i = 0; i < count; ++i) free(names[i]);
    free(names);
    return result;
}

int ghm_repo_files(const char *path, GhmFileList *out, GhmError *error)
{
    git_repository *repository = NULL;
    FileWalker walker;
    const char *workdir;
    int result;
    if (path == NULL || path[0] == '\0' || out == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Repository and output are required");
        return -1;
    }
    *out = (GhmFileList){0};
    if (git_repository_open(&repository, path) < 0) {
        ghm_error_from_git(error, "Open repository");
        return -1;
    }
    workdir = git_repository_workdir(repository);
    if (workdir == NULL) {
        ghm_error_set(error, GHM_ERROR_GIT, "Bare repositories have no files to browse");
        git_repository_free(repository);
        return -1;
    }
    walker = (FileWalker){.list = out, .root = workdir, .error = error};
    result = walk_directory(&walker, "");
    git_repository_free(repository);
    if (result != 0) ghm_file_list_free(out);
    return result;
}
