#include <ghm/commit.h>
#include "core/git.h"
#include "core/lock.h"
#include "core/index_lock.h"
#include "storage/database.h"

#include <git2.h>
#include <limits.h>
#include <string.h>

static int ghm_commit_rewrite_head_unlocked(GhmContext *context, const char *repository_path,
                            const char *expected_oid, const char *message,
                            int64_t author_timestamp, int author_offset,
                            int64_t committer_timestamp, int committer_offset,
                            int confirm, char out_oid[GHM_OID_HEX_CAPACITY], GhmError *error)
{
    git_repository *repository = NULL;
    git_reference *head = NULL;
    git_commit *commit = NULL;
    git_signature *author = NULL, *committer = NULL;
    git_status_list *status = NULL;
    git_status_options options = {0};
    git_buf signature_field = GIT_BUF_INIT;
    git_oid old_oid, new_oid;
    git_transaction *transaction = NULL;
    GhmIndexLock index_lock = {.descriptor = -1};
    int pending = 0, signed_status, result = -1;
    if (context == NULL || repository_path == NULL || repository_path[0] == '\0' ||
        expected_oid == NULL || git_oid_fromstr(&old_oid, expected_oid) < 0 ||
        message == NULL || message[0] == '\0' || out_oid == NULL ||
        author_timestamp < 0 || author_timestamp > LONG_MAX ||
        committer_timestamp < 0 || committer_timestamp > LONG_MAX ||
        author_offset < -14 * 60 || author_offset > 14 * 60 ||
        committer_offset < -14 * 60 || committer_offset > 14 * 60) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Valid repository, HEAD ID, message and dates are required");
        return -1;
    }
    out_oid[0] = '\0';
    if (!confirm) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "History rewrite requires explicit confirmation");
        return -1;
    }
    if (git_repository_open(&repository, repository_path) < 0) {
        ghm_error_from_git(error, "Open repository");
        goto done;
    }
    if (git_repository_state(repository) != GIT_REPOSITORY_STATE_NONE ||
        git_repository_head_detached(repository) != 0 ||
        git_repository_head(&head, repository) < 0 || git_reference_target(head) == NULL) {
        ghm_error_set(error, GHM_ERROR_GIT, "Rewrite requires a checked-out branch and no ongoing Git operation");
        goto done;
    }
    if (git_oid_cmp(git_reference_target(head), &old_oid) != 0) {
        ghm_error_set(error, GHM_ERROR_GIT, "HEAD changed since this rewrite was prepared");
        goto done;
    }
    if (ghm_database_branch_has_active_jobs(context, git_repository_workdir(repository),
                                           git_reference_shorthand(head), &pending, error) != 0)
        goto done;
    if (pending) {
        ghm_error_set(error, GHM_ERROR_GIT,
                      "Finish or cancel pending scheduled jobs, or wait for running jobs, before rewriting HEAD");
        goto done;
    }
    if (ghm_head_transaction(&transaction, repository, head, error) != 0 ||
        ghm_index_lock_acquire(&index_lock, repository, error) != 0) goto done;
    if (git_status_options_init(&options, GIT_STATUS_OPTIONS_VERSION) < 0) {
        ghm_error_from_git(error, "Initialize status inspection");
        goto done;
    }
    options.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
    if (git_status_list_new(&status, repository, &options) < 0) {
        ghm_error_from_git(error, "Inspect working tree");
        goto done;
    }
    if (git_status_list_entrycount(status) != 0) {
        ghm_error_set(error, GHM_ERROR_GIT, "Commit or discard local changes before rewriting HEAD");
        goto done;
    }
    if (git_commit_lookup(&commit, repository, &old_oid) < 0) {
        ghm_error_from_git(error, "Read HEAD commit");
        goto done;
    }
    signed_status = git_commit_header_field(&signature_field, commit, "gpgsig");
    if (signed_status == 0) {
        ghm_error_set(error, GHM_ERROR_GIT, "Signed commits cannot be rewritten by this app");
        goto done;
    }
    if (signed_status != GIT_ENOTFOUND) {
        ghm_error_from_git(error, "Inspect commit signature");
        goto done;
    }
    const git_signature *old_author = git_commit_author(commit);
    const git_signature *old_committer = git_commit_committer(commit);
    if (git_signature_new(&author, old_author->name, old_author->email,
                          (git_time_t)author_timestamp, author_offset) < 0 ||
        git_signature_new(&committer, old_committer->name, old_committer->email,
                          (git_time_t)committer_timestamp, committer_offset) < 0) {
        ghm_error_from_git(error, "Prepare replacement commit dates");
        goto done;
    }
    if (git_commit_amend(&new_oid, commit, NULL, author, committer,
                         NULL, message, NULL) < 0) {
        ghm_error_from_git(error, "Rewrite HEAD commit");
        goto done;
    }
    if (git_transaction_set_target(transaction, git_reference_name(head), &new_oid,
                                    committer, "Rewrite local HEAD") < 0 ||
        git_transaction_commit(transaction) < 0) {
        ghm_error_from_git(error, "Update rewritten branch");
        goto done;
    }
    (void)git_oid_tostr(out_oid, GHM_OID_HEX_CAPACITY, &new_oid);
    result = 0;
done:
    ghm_index_lock_release(&index_lock);
    git_transaction_free(transaction);
    git_buf_dispose(&signature_field);
    git_status_list_free(status);
    git_signature_free(author);
    git_signature_free(committer);
    git_commit_free(commit);
    git_reference_free(head);
    git_repository_free(repository);
    return result;
}

int ghm_commit_rewrite_head(GhmContext *context, const char *repository_path,
                            const char *expected_oid, const char *message,
                            int64_t author_timestamp, int author_offset,
                            int64_t committer_timestamp, int committer_offset,
                            int confirm, char out_oid[GHM_OID_HEX_CAPACITY], GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (ghm_repo_lock_acquire(repository_path, &lock, error) != 0) return -1;
    int result = ghm_commit_rewrite_head_unlocked(context, repository_path, expected_oid, message, author_timestamp, author_offset, committer_timestamp, committer_offset, confirm, out_oid, error);
    ghm_repo_lock_release(lock);
    return result;
}
