#include "core/lock.h"
#include "core/git.h"
#include "core/mutation.h"

#include <errno.h>
#include <fcntl.h>
#include <git2.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

struct GhmRepoLock {
    int descriptor;
    dev_t device;
    ino_t inode;
    unsigned depth;
    struct GhmRepoLock *next;
};

static _Thread_local GhmRepoLock *held_locks;
static _Thread_local pid_t held_process;

int ghm_directory_lock_acquire(const char *directory, const char *name,
                                GhmRepoLock **out, GhmError *error)
{
    int parent = -1, descriptor = -1;
    struct stat metadata;
    if (out == NULL || directory == NULL || name == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Lock directory and output are required");
        return -1;
    }
    *out = NULL;
    if (held_process != getpid()) {
        while (held_locks != NULL) {
            GhmRepoLock *inherited = held_locks;
            held_locks = inherited->next;
            close(inherited->descriptor);
            free(inherited);
        }
        held_process = getpid();
    }
    parent = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent >= 0)
        descriptor = openat(parent, name, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (parent >= 0) close(parent);
    if (descriptor < 0 || fstat(descriptor, &metadata) != 0 ||
        !S_ISREG(metadata.st_mode) || metadata.st_nlink != 1) {
        if (descriptor >= 0) close(descriptor);
        ghm_error_set(error, GHM_ERROR_IO, "Cannot open a safe repository operation lock");
        return -1;
    }
    for (GhmRepoLock *held = held_locks; held != NULL; held = held->next) {
        if (held->device == metadata.st_dev && held->inode == metadata.st_ino) {
            close(descriptor);
            ++held->depth;
            *out = held;
            return 0;
        }
    }
    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        int busy = errno == EWOULDBLOCK || errno == EAGAIN;
        close(descriptor);
        ghm_error_set(error, busy ? GHM_ERROR_BUSY : GHM_ERROR_IO,
                      busy ? "Repository is busy with another GHM operation; retry when it finishes" :
                             "Cannot acquire repository operation lock");
        return -1;
    }
    GhmRepoLock *lock = calloc(1, sizeof(*lock));
    if (lock == NULL) {
        close(descriptor);
        ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
        return -1;
    }
    *lock = (GhmRepoLock){descriptor, metadata.st_dev, metadata.st_ino, 1, held_locks};
    held_locks = lock;
    *out = lock;
    return 0;
}

int ghm_repo_lock_acquire(const char *path, GhmRepoLock **out, GhmError *error)
{
    git_repository *repository = NULL;
    if (out == NULL || path == NULL || path[0] == '\0') {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Repository and lock output are required");
        return -1;
    }
    *out = NULL;
    if (git_repository_open(&repository, path) < 0) {
        ghm_error_from_git(error, "Open repository for operation lock");
        return -1;
    }
    int result = ghm_directory_lock_acquire(git_repository_commondir(repository),
                                            "ghm-operation.lock", out, error);
    if (result == 0 && (*out)->depth == 1 &&
        (ghm_mutation_recover(repository, error) != 0 || ghm_mutation_other_worktrees(repository, error) != 0)) {
        ghm_repo_lock_release(*out);
        *out = NULL;
        result = -1;
    }
    git_repository_free(repository);
    return result;
}

void ghm_repo_lock_release(GhmRepoLock *lock)
{
    if (lock == NULL || --lock->depth != 0) return;
    GhmRepoLock **cursor = &held_locks;
    while (*cursor != NULL && *cursor != lock) cursor = &(*cursor)->next;
    if (*cursor == lock) *cursor = lock->next;
    /* close releases the kernel lock, including after a crash. The path stays
     * in place so another process can never acquire a different lock inode. */
    close(lock->descriptor);
    free(lock);
}
