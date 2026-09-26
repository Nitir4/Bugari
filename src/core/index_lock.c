#include "core/index_lock.h"
#include "core/storage_check.h"
#include "core/fault.h"

#include <errno.h>
#include <fcntl.h>
#include <git2/sys/index.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int ghm_index_lock_acquire(GhmIndexLock *lock, git_repository *repository, GhmError *error)
{
    git_index *index = NULL;
    *lock = (GhmIndexLock){.descriptor = -1};
    if (git_repository_index(&index, repository) < 0 || git_index_path(index) == NULL) {
        ghm_error_from_git(error, "Find index for native lock");
        git_index_free(index);
        return -1;
    }
    const char *path = git_index_path(index);
    if (ghm_storage_check_parent(path, 0, error) != 0) { git_index_free(index); return -1; }
    lock->path = strdup(path);
    lock->lock_path = malloc(strlen(path) + sizeof(".lock"));
    if (lock->path != NULL && lock->lock_path != NULL) {
        (void)snprintf(lock->lock_path, strlen(path) + sizeof(".lock"), "%s.lock", path);
        lock->descriptor = open(lock->lock_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
        if (lock->descriptor < 0)
            ghm_error_set(error, errno == EEXIST ? GHM_ERROR_BUSY : GHM_ERROR_IO,
                          "Cannot reserve Git index; another client may be using index.lock");
    } else ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
    git_index_free(index);
    if (lock->descriptor < 0) {
        free(lock->path); free(lock->lock_path);
        *lock = (GhmIndexLock){.descriptor = -1};
        return -1;
    }
    return 0;
}

int ghm_index_lock_write(GhmIndexLock *lock, git_index *index, GhmError *error)
{
    if (ghm_fault("index.write", error) != 0) return -1;
    char *directory = NULL, *path = NULL, *temporary_lock = NULL;
    git_index *serialized = NULL;
    int input = -1, result = -1;
    size_t length = strlen(lock->path) + sizeof(".ghm-XXXXXX");
    directory = malloc(length);
    if (directory == NULL) goto memory_failed;
    (void)snprintf(directory, length, "%s.ghm-XXXXXX", lock->path);
    if (mkdtemp(directory) == NULL) goto io_failed;
    path = calloc(strlen(directory) + sizeof("/index"), 1);
    temporary_lock = calloc(strlen(directory) + sizeof("/index.lock"), 1);
    if (path == NULL || temporary_lock == NULL) goto memory_failed;
    (void)snprintf(path, strlen(directory) + sizeof("/index"), "%s/index", directory);
    (void)snprintf(temporary_lock, strlen(directory) + sizeof("/index.lock"), "%s/index.lock", directory);
    /* Serialize with libgit2 in a private directory, then publish through the
     * native index.lock we have held since before reading the real index. */
    if (git_index_open(&serialized, path) < 0 ||
        git_index_set_caps(serialized, git_index_caps(index)) < 0 ||
        git_index_set_version(serialized, git_index_version(index)) < 0) goto git_failed;
    for (size_t i = 0; i < git_index_entrycount(index); ++i)
        if (git_index_add(serialized, git_index_get_byindex(index, i)) < 0) goto git_failed;
    for (size_t i = 0; i < git_index_reuc_entrycount(index); ++i) {
        const git_index_reuc_entry *entry = git_index_reuc_get_byindex(index, i);
        if (git_index_reuc_add(serialized, entry->path,
            (int)entry->mode[0], &entry->oid[0], (int)entry->mode[1], &entry->oid[1],
            (int)entry->mode[2], &entry->oid[2]) < 0) goto git_failed;
    }
    if (git_index_write(serialized) < 0) goto git_failed;
    input = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (input < 0 || lseek(lock->descriptor, 0, SEEK_SET) < 0 || ftruncate(lock->descriptor, 0) != 0) goto io_failed;
    char bytes[8192];
    for (;;) {
        ssize_t received = read(input, bytes, sizeof(bytes));
        if (received == 0) break;
        if (received < 0) { if (errno == EINTR) continue; goto io_failed; }
        ssize_t offset = 0;
        while (offset < received) {
            ssize_t written = write(lock->descriptor, bytes + offset, (size_t)(received - offset));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) goto io_failed;
            offset += written;
        }
    }
    if (fsync(lock->descriptor) != 0 || rename(lock->lock_path, lock->path) != 0) goto io_failed;
    close(lock->descriptor); lock->descriptor = -1;
    result = 0;
    goto done;
git_failed:
    ghm_error_from_git(error, "Serialize reserved Git index");
    goto done;
memory_failed:
    ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
    goto done;
io_failed:
    ghm_error_set(error, GHM_ERROR_IO, "Cannot publish reserved Git index");
done:
    if (input >= 0) close(input);
    git_index_free(serialized);
    if (path != NULL) (void)unlink(path);
    if (temporary_lock != NULL) (void)unlink(temporary_lock);
    if (directory != NULL) (void)rmdir(directory);
    free(directory); free(path); free(temporary_lock);
    return result;
}

void ghm_index_lock_release(GhmIndexLock *lock)
{
    if (lock->descriptor >= 0) {
        close(lock->descriptor);
        (void)unlink(lock->lock_path);
    }
    free(lock->path); free(lock->lock_path);
    *lock = (GhmIndexLock){.descriptor = -1};
}
