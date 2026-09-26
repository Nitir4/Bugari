#ifndef GHM_CORE_INDEX_LOCK_H
#define GHM_CORE_INDEX_LOCK_H

#include "core/git.h"

typedef struct {
    char *path;
    char *lock_path;
    int descriptor;
} GhmIndexLock;

int ghm_index_lock_acquire(GhmIndexLock *lock, git_repository *repository, GhmError *error);
int ghm_index_lock_write(GhmIndexLock *lock, git_index *index, GhmError *error);
void ghm_index_lock_release(GhmIndexLock *lock);

#endif
