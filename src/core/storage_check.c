#include "core/storage_check.h"
#include "core/fault.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/statvfs.h>
#endif
#include <unistd.h>
#include "platform/io.h"

int ghm_storage_check(const char *directory, uint64_t bytes, GhmError *error)
{
#ifdef _WIN32
    if (ghm_fault("storage.check", error) != 0) return -1;
    WCHAR *path = (WCHAR *)g_utf8_to_utf16(directory, -1, NULL, NULL, NULL);
    ULARGE_INTEGER available;
    int ok = path != NULL && GetDiskFreeSpaceExW(path, &available, NULL, NULL);
    g_free(path);
    uint64_t needed = bytes > UINT64_MAX - 1048576U ? UINT64_MAX : bytes + 1048576U;
    if (!ok || access(directory, W_OK) != 0) {
        ghm_error_set(error, GHM_ERROR_IO, "Cannot check destination permissions or free disk space"); return -1;
    }
    if (available.QuadPart < needed) {
        ghm_error_set(error, GHM_ERROR_IO, "Not enough free disk space; nothing was started"); return -1;
    }
    return 0;
#else
    struct statvfs space;
    if (ghm_fault("storage.check", error) != 0) return -1;
    if (access(directory, W_OK | X_OK) != 0) {
        ghm_error_set(error, GHM_ERROR_IO, "Permission denied: destination directory is not writable; nothing was started");
        return -1;
    }
    if (statvfs(directory, &space) != 0 || space.f_frsize == 0) {
        ghm_error_set(error, GHM_ERROR_IO, "Cannot check available disk space; nothing was started");
        return -1;
    }
    uint64_t needed = bytes > UINT64_MAX - 1048576U ? UINT64_MAX : bytes + 1048576U;
    if ((uint64_t)space.f_bavail < needed / space.f_frsize + (needed % space.f_frsize != 0)) {
        ghm_error_set(error, GHM_ERROR_IO, "Not enough free disk space; free space before committing or scheduling");
        return -1;
    }
    return 0;
#endif
}

int ghm_storage_check_parent(const char *path, uint64_t bytes, GhmError *error)
{
    char *parent = strdup(path);
    if (parent == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); return -1; }
    char *slash = strrchr(parent, '/');
    if (slash != NULL) { if (slash == parent) slash[1] = '\0'; else *slash = '\0'; }
    int result = ghm_storage_check(slash != NULL ? parent : ".", bytes, error);
    free(parent);
    return result;
}

int ghm_storage_check_descriptor(int directory, uint64_t bytes, GhmError *error)
{
#ifdef _WIN32
    HANDLE handle = (HANDLE)_get_osfhandle(directory);
    DWORD length = GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED);
    WCHAR *path = length > 0 ? malloc(((size_t)length + 1) * sizeof(*path)) : NULL;
    if (path == NULL || GetFinalPathNameByHandleW(handle, path, length + 1, FILE_NAME_NORMALIZED) == 0) {
        free(path); ghm_error_set(error, GHM_ERROR_IO, "Cannot inspect save destination"); return -1;
    }
    char *utf8 = g_utf16_to_utf8((const gunichar2 *)path, -1, NULL, NULL, NULL);
    free(path);
    int result = utf8 != NULL ? ghm_storage_check(utf8, bytes, error) : -1;
    g_free(utf8); return result;
#else
    struct statvfs space;
    if (ghm_fault("storage.check", error) != 0) return -1;
    if (faccessat(directory, ".", W_OK | X_OK, 0) != 0 || fstatvfs(directory, &space) != 0 || space.f_frsize == 0) {
        ghm_error_set(error, GHM_ERROR_IO, "Destination is not writable or disk space cannot be checked; save was not started"); return -1;
    }
    uint64_t needed = bytes > UINT64_MAX - 1048576U ? UINT64_MAX : bytes + 1048576U;
    if ((uint64_t)space.f_bavail < needed / space.f_frsize + (needed % space.f_frsize != 0)) {
        ghm_error_set(error, GHM_ERROR_IO, "Not enough free disk space; save was not started"); return -1;
    }
    return 0;
#endif
}

int ghm_repository_storage_check(const char *path, GhmError *error)
{
    git_repository *repo = NULL;
    GhmStatus status = {0};
    uint64_t bytes = 0;
    int result = -1;
    if (git_repository_open(&repo, path) < 0) { ghm_error_from_git(error, "Check repository storage"); return -1; }
    if (ghm_repo_status(path, &status, error) != 0) goto done;
    for (size_t i = 0; i < status.count; ++i) {
        const char *root = git_repository_workdir(repo);
        size_t length = strlen(root) + strlen(status.items[i].path) + 1;
        char *file = malloc(length);
        if (file == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); goto done; }
        (void)snprintf(file, length, "%s%s", root, status.items[i].path);
        struct stat metadata;
        if (lstat(file, &metadata) == 0 && S_ISREG(metadata.st_mode) && metadata.st_size > 0) {
            uint64_t size = (uint64_t)metadata.st_size;
            bytes = size > UINT64_MAX - bytes ? UINT64_MAX : bytes + size;
        }
        free(file);
    }
    result = ghm_storage_check(git_repository_commondir(repo), bytes, error);
done:
    ghm_status_free(&status);
    git_repository_free(repo);
    return result;
}

static int check_checkout_path(const char *root, const char *relative, GhmError *error)
{
    size_t root_length = strlen(root);
    char *path = malloc(root_length + strlen(relative) + 1);
    if (path == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); return -1; }
    (void)snprintf(path, root_length + strlen(relative) + 1, "%s%s", root, relative);
    struct stat metadata;
    int result = -1;
    for (char *cursor = path + root_length; ; ++cursor) {
        if (*cursor != '/' && *cursor != '\0') continue;
        char saved = *cursor;
        *cursor = '\0';
        if (lstat(path, &metadata) != 0) {
            if (errno == ENOENT) { result = 0; break; }
            ghm_error_set(error, GHM_ERROR_IO, "Cannot inspect checkout destination; nothing was started"); break;
        }
        if (saved == '/' && !S_ISDIR(metadata.st_mode)) {
            ghm_error_set(error, GHM_ERROR_IO, "Checkout parent is not a directory; nothing was started"); break;
        }
        if (!S_ISLNK(metadata.st_mode) && access(path, W_OK | (saved == '/' ? X_OK : 0)) != 0) {
            ghm_error_set(error, GHM_ERROR_IO, "Permission denied in checkout destination; nothing was started"); break;
        }
        *cursor = saved;
        if (saved == '\0') { result = 0; break; }
    }
    free(path);
    return result;
}

int ghm_checkout_storage_check(git_repository *repo, git_commit *original,
                                git_commit *target, GhmError *error)
{
    git_tree *before = NULL, *after = NULL;
    git_diff *diff = NULL;
    uint64_t needed = 0;
    int result = -1;
    const char *root = git_repository_workdir(repo);
    if (git_commit_tree(&before, original) < 0 || git_commit_tree(&after, target) < 0 ||
        git_diff_tree_to_tree(&diff, repo, before, after, NULL) < 0) {
        ghm_error_from_git(error, "Inspect checkout changes"); goto done;
    }
    if (ghm_storage_check(root, 0, error) != 0) goto done;
    for (size_t i = 0; i < git_diff_num_deltas(diff); ++i) {
        const git_diff_delta *delta = git_diff_get_delta(diff, i);
        const git_diff_file *files[] = {&delta->old_file, &delta->new_file};
        for (size_t j = 0; j < 2; ++j) {
            const git_diff_file *file = files[j];
            if (file->path != NULL && check_checkout_path(root, file->path, error) != 0) goto done;
            if (!git_oid_is_zero(&file->id) && file->mode != GIT_FILEMODE_COMMIT) {
                git_blob *blob = NULL;
                if (git_blob_lookup(&blob, repo, &file->id) < 0) { ghm_error_from_git(error, "Inspect checkout size"); goto done; }
                uint64_t size = git_blob_rawsize(blob);
                needed = size > UINT64_MAX - needed ? UINT64_MAX : needed + size;
                git_blob_free(blob);
            }
        }
    }
    result = ghm_storage_check(root, needed, error);
done:
    git_diff_free(diff); git_tree_free(before); git_tree_free(after);
    return result;
}
