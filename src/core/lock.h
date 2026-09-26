#ifndef GHM_CORE_LOCK_H
#define GHM_CORE_LOCK_H

#include <ghm/ghm.h>

typedef struct GhmRepoLock GhmRepoLock;

/* Nonblocking, thread-recursive lock in the common Git directory. Cooperating
 * GUI/CLI/workers share it across data directories and worktree/path aliases.
 * Busy returns GHM_ERROR_BUSY. Never remove the persistent lock file.
 * Release on the acquiring thread. Forked children discard inherited handles. */
int ghm_repo_lock_acquire(const char *path, GhmRepoLock **out, GhmError *error);
int ghm_directory_lock_acquire(const char *directory, const char *name,
                                GhmRepoLock **out, GhmError *error);
void ghm_repo_lock_release(GhmRepoLock *lock);

#endif
