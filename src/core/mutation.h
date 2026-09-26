#ifndef GHM_CORE_MUTATION_H
#define GHM_CORE_MUTATION_H
#include "core/git.h"
#include <ghm/commit.h>
int ghm_mutation_begin(git_repository *repo, const git_reference *head,
    git_commit *target, const git_reference *source, int switch_branch,
    int checkout_files, int sync_index, git_index *index, GhmError *error);
void ghm_mutation_end(git_repository *repo, int keep_record);
/* Called only while owning the exclusive GHM operation lock. */
int ghm_mutation_recover(git_repository *repo, GhmError *error);
int ghm_mutation_other_worktrees(git_repository *repo, GhmError *error);
int ghm_mutation_pending(git_repository *repo);
#endif
