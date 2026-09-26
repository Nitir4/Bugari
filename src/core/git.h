#ifndef GHM_CORE_GIT_H
#define GHM_CORE_GIT_H

#include <ghm/ghm.h>
#include <git2.h>

void ghm_error_set(GhmError *error, GhmErrorCode code, const char *message);
void ghm_error_from_git(GhmError *error, const char *operation);
/* Hold native HEAD/branch locks and verify the expected branch before writes. */
int ghm_head_transaction(git_transaction **out, git_repository *repository,
                          const git_reference *expected, GhmError *error);
int ghm_checkout_transaction(git_repository *repository, const git_reference *head,
                              const git_reference *source, git_commit *target,
                              int switch_branch, const char *message, GhmError *error);

#endif
