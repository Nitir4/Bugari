#define _GNU_SOURCE
#include <ghm/file_ops.h>
#include "core/git.h"
#include "core/lock.h"
#include "core/storage_check.h"
#include "core/fault.h"

#include <errno.h>
#include <fcntl.h>
#include <git2.h>
#include <inttypes.h>
#ifndef _WIN32
#include <linux/fs.h>
#endif
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "platform/io.h"

#define GHM_TEXT_LIMIT (512U * 1024U)

static void file_error(GhmError *error, const char *operation)
{
    char message[256];
    (void)snprintf(message, sizeof(message), "%s: %s", operation, strerror(errno));
    ghm_error_set(error, GHM_ERROR_IO, message);
}

static int valid_segment(const char *segment)
{
#ifdef _WIN32
    size_t length = strlen(segment);
    if (length == 0 || segment[length - 1] == '.' || segment[length - 1] == ' ' ||
        strpbrk(segment, "\\:<>\"|?*") != NULL || g_ascii_strcasecmp(segment, ".git") == 0 ||
        (length >= 5 && g_ascii_strncasecmp(segment, "git~", 4) == 0)) return 0;
    for (const unsigned char *p = (const unsigned char *)segment; *p; ++p)
        if (*p < 32) return 0;
    const char *dot = strchr(segment, '.');
    size_t stem = dot != NULL ? (size_t)(dot - segment) : length;
    if ((stem == 3 && (g_ascii_strncasecmp(segment, "CON", 3) == 0 ||
        g_ascii_strncasecmp(segment, "PRN", 3) == 0 ||
        g_ascii_strncasecmp(segment, "AUX", 3) == 0 || g_ascii_strncasecmp(segment, "NUL", 3) == 0)) ||
        (stem == 4 && (g_ascii_strncasecmp(segment, "COM", 3) == 0 ||
        g_ascii_strncasecmp(segment, "LPT", 3) == 0) && segment[3] >= '1' && segment[3] <= '9')) return 0;
#endif
    return segment[0] != '\0' && strcmp(segment, ".") != 0 &&
           strcmp(segment, "..") != 0 && strcmp(segment, ".git") != 0;
}

static int open_parent(const char *repository_path, const char *relative_path,
                       int *out_directory, char **out_name, GhmError *error)
{
    git_repository *repository = NULL;
    int directory = -1;
    char *copy = NULL;
    char *segment;
    int result = -1;
    if (repository_path == NULL || repository_path[0] == '\0' ||
        relative_path == NULL || relative_path[0] == '\0' || relative_path[0] == '/' ||
        out_directory == NULL || out_name == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "A repository-relative path is required");
        return -1;
    }
    *out_directory = -1;
    *out_name = NULL;
    if (git_repository_open(&repository, repository_path) < 0) {
        ghm_error_from_git(error, "Open repository");
        goto done;
    }
    const char *workdir = git_repository_workdir(repository);
    if (workdir == NULL) {
        ghm_error_set(error, GHM_ERROR_GIT, "A working tree is required");
        goto done;
    }
    directory = open(workdir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) { file_error(error, "Open working tree"); goto done; }
    copy = strdup(relative_path);
    if (copy == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); goto done; }
    segment = copy;
    for (;;) {
        char *slash = strchr(segment, '/');
        if (slash != NULL) *slash = '\0';
        if (!valid_segment(segment)) {
            ghm_error_set(error, GHM_ERROR_ARGUMENT, "Invalid or protected repository path");
            goto done;
        }
        if (slash == NULL) {
            *out_name = strdup(segment);
            if (*out_name == NULL) ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
            else result = 0;
            break;
        }
        int next = openat(directory, segment, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) { file_error(error, "Open parent directory"); goto done; }
        close(directory);
        directory = next;
        segment = slash + 1;
    }
done:
    free(copy);
    git_repository_free(repository);
    if (result == 0) *out_directory = directory;
    else { if (directory >= 0) close(directory); free(*out_name); *out_name = NULL; }
    return result;
}

static void get_version(const struct stat *metadata, GhmFileVersion *version)
{
    version->size = (int64_t)metadata->st_size;
    version->mtime_seconds = (int64_t)metadata->st_mtim.tv_sec;
    version->mtime_nanoseconds = metadata->st_mtim.tv_nsec;
}

static int same_version(const struct stat *metadata, const GhmFileVersion *expected)
{
    return expected != NULL && (int64_t)metadata->st_size == expected->size &&
           (int64_t)metadata->st_mtim.tv_sec == expected->mtime_seconds &&
           metadata->st_mtim.tv_nsec == expected->mtime_nanoseconds;
}

int ghm_file_read_text(const char *repository_path, const char *relative_path,
                       char **out_text, size_t *out_length, GhmFileVersion *out_version,
                       GhmError *error)
{
    int directory = -1;
    int file = -1;
    char *name = NULL;
    char *text = NULL;
    struct stat before, after;
    size_t used = 0;
    int result = -1;
    if (out_text == NULL || out_length == NULL || out_version == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Text, length and version outputs are required");
        return -1;
    }
    *out_text = NULL;
    *out_length = 0;
    if (open_parent(repository_path, relative_path, &directory, &name, error) != 0) goto done;
    file = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (file < 0) { file_error(error, "Open file"); goto done; }
    if (fstat(file, &before) < 0) { file_error(error, "Inspect file"); goto done; }
    if (!S_ISREG(before.st_mode)) {
        ghm_error_set(error, GHM_ERROR_IO, "Only regular files can be edited");
        goto done;
    }
    if (before.st_size < 0 || before.st_size > (off_t)GHM_TEXT_LIMIT) {
        ghm_error_set(error, GHM_ERROR_IO, "File exceeds the 512 KiB editor limit");
        goto done;
    }
    size_t expected_size = (size_t)before.st_size;
    text = malloc(expected_size + 1U);
    if (text == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); goto done; }
    while (used < expected_size) {
        ssize_t count = read(file, text + used, expected_size - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { file_error(error, "Read file"); goto done; }
        if (count == 0) break;
        used += (size_t)count;
    }
    char extra;
    ssize_t extra_count;
    do extra_count = read(file, &extra, 1); while (extra_count < 0 && errno == EINTR);
    if (extra_count > 0) {
        ghm_error_set(error, GHM_ERROR_IO, "File changed while reading; reopen it");
        goto done;
    }
    if (extra_count < 0) { file_error(error, "Read file"); goto done; }
    if (fstat(file, &after) < 0) { file_error(error, "Inspect file after read"); goto done; }
    GhmFileVersion version;
    get_version(&before, &version);
    if (!same_version(&after, &version) || used != (size_t)before.st_size) {
        ghm_error_set(error, GHM_ERROR_IO, "File changed while reading; reopen it");
        goto done;
    }
    text[used] = '\0';
    *out_text = text;
    *out_length = used;
    *out_version = version;
    text = NULL;
    result = 0;
done:
    free(text);
    free(name);
    if (file >= 0) close(file);
    if (directory >= 0) close(directory);
    return result;
}

static int ghm_file_create_unlocked(const char *repository_path, const char *relative_path, GhmError *error)
{
    int directory = -1;
    int file = -1;
    char *name = NULL;
    int result = -1;
    if (open_parent(repository_path, relative_path, &directory, &name, error) != 0) goto done;
    file = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (file < 0) file_error(error, "Create file");
    else result = 0;
done:
    if (file >= 0) close(file);
    if (directory >= 0) close(directory);
    free(name);
    return result;
}

static int ghm_directory_create_unlocked(const char *repository_path, const char *relative_path, GhmError *error)
{
    int directory = -1;
    char *name = NULL;
    int result = -1;
    if (open_parent(repository_path, relative_path, &directory, &name, error) != 0) goto done;
    if (mkdirat(directory, name, 0755) < 0) file_error(error, "Create directory");
    else result = 0;
done:
    if (directory >= 0) close(directory);
    free(name);
    return result;
}

static int ghm_file_save_text_unlocked(const char *repository_path, const char *relative_path,
                       const char *text, size_t length, const GhmFileVersion *expected,
                       GhmFileVersion *out_version, GhmError *error)
{
    int directory = -1;
    int temporary = -1;
    char *name = NULL;
    char temporary_name[64] = {0};
    int temporary_owned = 0;
    struct stat metadata;
    int result = -1;
    if (text == NULL || expected == NULL || out_version == NULL || length > GHM_TEXT_LIMIT) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Valid text, file version and output are required");
        return -1;
    }
    if (open_parent(repository_path, relative_path, &directory, &name, error) != 0) goto done;
    if (fstatat(directory, name, &metadata, AT_SYMLINK_NOFOLLOW) < 0) {
        file_error(error, "Inspect file before save"); goto done;
    }
    if (!S_ISREG(metadata.st_mode) || !same_version(&metadata, expected)) {
        ghm_error_set(error, GHM_ERROR_IO, "File changed since it was opened; reopen before saving");
        goto done;
    }
    if (faccessat(directory, name, W_OK, 0) != 0) { file_error(error, "Permission denied before save"); goto done; }
    if (ghm_storage_check_descriptor(directory, length, error) != 0) goto done;
    for (int attempt = 0; attempt < 16; ++attempt) {
        uint64_t random_value;
        sqlite3_randomness((int)sizeof(random_value), &random_value);
        (void)snprintf(temporary_name, sizeof(temporary_name), ".ghm-save-%016" PRIx64,
                       random_value);
        temporary = openat(directory, temporary_name,
                           O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (temporary >= 0) { temporary_owned = 1; break; }
        if (errno != EEXIST) { file_error(error, "Create temporary save file"); goto done; }
    }
    if (temporary < 0) {
        ghm_error_set(error, GHM_ERROR_IO, "Could not allocate a temporary save file");
        goto done;
    }
    size_t written = 0;
    if (ghm_fault("file.write", error) != 0) goto done;
    while (written < length) {
        ssize_t count = write(temporary, text + written, length - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { file_error(error, "Write file"); goto done; }
        written += (size_t)count;
    }
    if (fchmod(temporary, metadata.st_mode & 0777U) < 0 || fsync(temporary) < 0) {
        file_error(error, "Finish file save"); goto done;
    }
    struct stat current;
    if (ghm_fault("file.before_publish", error) != 0) goto done;
    if (fstatat(directory, name, &current, AT_SYMLINK_NOFOLLOW) < 0 ||
        !S_ISREG(current.st_mode) || !same_version(&current, expected)) {
        ghm_error_set(error, GHM_ERROR_IO, "File changed during save; reopen before retrying");
        goto done;
    }
    if (renameat(directory, temporary_name, directory, name) < 0) {
        file_error(error, "Replace saved file"); goto done;
    }
    temporary_owned = 0;
    temporary_name[0] = '\0';
    if (fstatat(directory, name, &current, AT_SYMLINK_NOFOLLOW) < 0) {
        file_error(error, "Inspect saved file"); goto done;
    }
    get_version(&current, out_version);
    result = 0;
done:
    if (temporary >= 0) close(temporary);
    if (temporary_owned && directory >= 0)
        (void)unlinkat(directory, temporary_name, 0);
    if (directory >= 0) close(directory);
    free(name);
    return result;
}

static int ghm_file_delete_unlocked(const char *repository_path, const char *relative_path, GhmError *error)
{
    int directory = -1;
    char *name = NULL;
    struct stat metadata;
    int result = -1;
    if (open_parent(repository_path, relative_path, &directory, &name, error) != 0) goto done;
    if (fstatat(directory, name, &metadata, AT_SYMLINK_NOFOLLOW) < 0) {
        file_error(error, "Inspect file before deletion"); goto done;
    }
    if (S_ISDIR(metadata.st_mode)) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Delete files individually; directory deletion is not supported");
        goto done;
    }
    if (unlinkat(directory, name, 0) < 0) file_error(error, "Delete file");
    else result = 0;
done:
    if (directory >= 0) close(directory);
    free(name);
    return result;
}

static int ghm_file_rename_unlocked(const char *repository_path, const char *old_relative_path,
                     const char *new_relative_path, GhmError *error)
{
    int old_directory = -1;
    int new_directory = -1;
    char *old_name = NULL;
    char *new_name = NULL;
    int result = -1;
    if (open_parent(repository_path, old_relative_path, &old_directory, &old_name, error) != 0 ||
        open_parent(repository_path, new_relative_path, &new_directory, &new_name, error) != 0)
        goto done;
    if (renameat2(old_directory, old_name, new_directory, new_name, RENAME_NOREPLACE) < 0)
        file_error(error, "Rename file");
    else result = 0;
done:
    if (old_directory >= 0) close(old_directory);
    if (new_directory >= 0) close(new_directory);
    free(old_name);
    free(new_name);
    return result;
}

int ghm_file_create(const char *repository_path, const char *relative_path, GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (ghm_repo_lock_acquire(repository_path, &lock, error) != 0) return -1;
    int result = ghm_file_create_unlocked(repository_path, relative_path, error);
    ghm_repo_lock_release(lock);
    return result;
}

int ghm_directory_create(const char *repository_path, const char *relative_path, GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (ghm_repo_lock_acquire(repository_path, &lock, error) != 0) return -1;
    int result = ghm_directory_create_unlocked(repository_path, relative_path, error);
    ghm_repo_lock_release(lock);
    return result;
}

int ghm_file_save_text(const char *repository_path, const char *relative_path,
                       const char *text, size_t length, const GhmFileVersion *expected,
                       GhmFileVersion *out_version, GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (ghm_repo_lock_acquire(repository_path, &lock, error) != 0) return -1;
    int result = ghm_file_save_text_unlocked(repository_path, relative_path, text, length, expected, out_version, error);
    ghm_repo_lock_release(lock);
    return result;
}

int ghm_file_delete(const char *repository_path, const char *relative_path, GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (ghm_repo_lock_acquire(repository_path, &lock, error) != 0) return -1;
    int result = ghm_file_delete_unlocked(repository_path, relative_path, error);
    ghm_repo_lock_release(lock);
    return result;
}

int ghm_file_rename(const char *repository_path, const char *old_relative_path,
                     const char *new_relative_path, GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (ghm_repo_lock_acquire(repository_path, &lock, error) != 0) return -1;
    int result = ghm_file_rename_unlocked(repository_path, old_relative_path, new_relative_path, error);
    ghm_repo_lock_release(lock);
    return result;
}
