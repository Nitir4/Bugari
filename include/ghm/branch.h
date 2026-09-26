#ifndef GHM_BRANCH_H
#define GHM_BRANCH_H

#include <ghm/commit.h>

typedef struct {
    char *name;
    int is_current;
    int is_remote;
} GhmBranch;

typedef struct {
    GhmBranch *items;
    size_t count;
} GhmBranchList;

int ghm_branch_list(const char *repository_path, GhmBranchList *out, GhmError *error);
void ghm_branch_list_free(GhmBranchList *list);
int ghm_branch_create(const char *repository_path, const char *name, GhmError *error);
/* Checkout is safe-only and refuses to abandon pending/running scheduled jobs,
 * including when the repository is opened through another path alias. */
int ghm_branch_checkout(GhmContext *context, const char *repository_path,
                        const char *name, GhmError *error);

typedef enum {
    GHM_MERGE_UP_TO_DATE,
    GHM_MERGE_FAST_FORWARDED,
    GHM_MERGE_COMMITTED
} GhmMergeOutcome;

/* Merge a local branch into the checked-out branch. A dirty worktree, pending/running
 * scheduled snapshots, or conflicts stop the operation without touching the
 * worktree or refs. Conflicts are reported for manual resolution elsewhere. */
int ghm_branch_merge(GhmContext *context, const char *repository_path,
                     const char *source_branch, GhmMergeOutcome *outcome,
                     char out_oid[GHM_OID_HEX_CAPACITY], GhmError *error);

#endif
