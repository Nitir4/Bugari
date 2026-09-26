#ifndef GHM_CORE_STORAGE_CHECK_H
#define GHM_CORE_STORAGE_CHECK_H
#include "core/git.h"
/* Estimates are conservative preflights; all actual writes still check errors. */
int ghm_storage_check(const char *directory, uint64_t bytes, GhmError *error);
int ghm_storage_check_parent(const char *path, uint64_t bytes, GhmError *error);
int ghm_storage_check_descriptor(int directory, uint64_t bytes, GhmError *error);
int ghm_repository_storage_check(const char *path, GhmError *error);
int ghm_checkout_storage_check(git_repository *repo, git_commit *original,
                                git_commit *target, GhmError *error);
#endif
