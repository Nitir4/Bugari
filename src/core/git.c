#include "core/git.h"
#include "core/index_lock.h"
#include "core/storage_check.h"
#include "core/fault.h"
#include "core/mutation.h"

#include <git2.h>
#include <stdio.h>
#include <string.h>

void ghm_error_set(GhmError *error, GhmErrorCode code, const char *message)
{
    if (error == NULL) return;
    error->code = code;
    error->retry_at = 0;
    (void)snprintf(error->message, sizeof(error->message), "%s", message);
}

void ghm_error_from_git(GhmError *error, const char *operation)
{
    const git_error *detail = git_error_last();
    if (error == NULL) return;
    error->code = GHM_ERROR_GIT;
    error->retry_at = 0;
    (void)snprintf(error->message, sizeof(error->message), "%s: %s", operation,
                   detail != NULL && detail->message != NULL ? detail->message : "unknown Git error");
}

int ghm_head_transaction(git_transaction **out, git_repository *repository,
                          const git_reference *expected, GhmError *error)
{
    git_reference *current = NULL;
    int unborn = git_reference_type(expected) == GIT_REFERENCE_SYMBOLIC;
    const char *branch = unborn ? git_reference_symbolic_target(expected) : git_reference_name(expected);
    *out = NULL;
    if (git_transaction_new(out, repository) < 0 ||
        git_transaction_lock_ref(*out, "HEAD") < 0 ||
        git_transaction_lock_ref(*out, branch) < 0) {
        ghm_error_from_git(error, "Lock HEAD and branch before mutation");
        goto failed;
    }
    if (unborn) {
        if (git_reference_lookup(&current, repository, "HEAD") < 0 ||
            git_reference_type(current) != GIT_REFERENCE_SYMBOLIC ||
            strcmp(git_reference_symbolic_target(current), branch) != 0 ||
            git_repository_head_unborn(repository) != 1) {
            ghm_error_set(error, GHM_ERROR_GIT, "Unborn branch changed before native locks were acquired");
            goto failed;
        }
    } else if (git_repository_head(&current, repository) < 0 ||
        git_repository_head_detached(repository) != 0 ||
        strcmp(git_reference_name(current), git_reference_name(expected)) != 0 ||
        git_reference_target(current) == NULL ||
        !git_oid_equal(git_reference_target(current), git_reference_target(expected))) {
        ghm_error_set(error, GHM_ERROR_GIT, "Branch changed before native locks were acquired");
        goto failed;
    }
    git_reference_free(current);
    return 0;
failed:
    git_reference_free(current);
    git_transaction_free(*out);
    *out = NULL;
    return -1;
}

int ghm_checkout_transaction(git_repository *repository, const git_reference *head,
                              const git_reference *source, git_commit *target,
                              int switch_branch, const char *message, GhmError *error)
{
    git_transaction *transaction = NULL;
    git_commit *original = NULL;
    git_tree *baseline = NULL;
    git_checkout_options checkout = {0};
    git_oid source_oid;
    GhmIndexLock index_lock = {.descriptor = -1};
    git_index *index = NULL;
    int result = -1;
    int recorded = 0, keep_record = 0;
    if (git_checkout_options_init(&checkout, GIT_CHECKOUT_OPTIONS_VERSION) < 0) {
        ghm_error_from_git(error, "Initialize checkout options");
        goto done;
    }
    if (ghm_head_transaction(&transaction, repository, head, error) != 0) goto done;
    if (ghm_index_lock_acquire(&index_lock, repository, error) != 0 ||
        git_repository_index(&index, repository) < 0 || git_index_read(index, 1) < 0) goto done;
    if (source != NULL && strcmp(git_reference_name(source), git_reference_name(head)) != 0) {
        if (git_transaction_lock_ref(transaction, git_reference_name(source)) < 0) {
            ghm_error_from_git(error, "Lock source branch before checkout");
            goto done;
        }
        if (git_reference_name_to_id(&source_oid, repository, git_reference_name(source)) < 0 ||
            !git_oid_equal(&source_oid, git_reference_target(source))) {
            ghm_error_set(error, GHM_ERROR_GIT, "Source branch changed before checkout");
            goto done;
        }
    }
    if (git_commit_lookup(&original, repository, git_reference_target(head)) < 0 ||
        git_commit_tree(&baseline, target) < 0 ||
        (switch_branch ? git_transaction_set_symbolic_target(transaction, "HEAD",
            git_reference_name(source), NULL, message) :
          git_transaction_set_target(transaction, git_reference_name(head),
            git_commit_id(target), NULL, message)) < 0) {
        ghm_error_from_git(error, "Prepare checkout and reference update");
        goto done;
    }
    if (ghm_checkout_storage_check(repository, original, target, error) != 0) goto done;
    if (ghm_mutation_begin(repository, head, target, source, switch_branch, 1, 1, index, error) != 0) goto done;
    recorded = 1;
    checkout.checkout_strategy = GIT_CHECKOUT_SAFE | GIT_CHECKOUT_DONT_WRITE_INDEX;
    int checkout_result = git_checkout_tree(repository, (git_object *)target, &checkout);
    if (checkout_result < 0) {
        ghm_error_from_git(error, "Update working tree");
    }
    if (checkout_result == 0 && ghm_fault("checkout.after_files", error) != 0) checkout_result = -1;
    int index_result = checkout_result == 0 ? ghm_index_lock_write(&index_lock, index, error) : -1;
    int publish_fault = index_result == 0 ? ghm_fault("checkout.publish_ref", error) : 0;
    if (index_result != 0 || publish_fault != 0 || git_transaction_commit(transaction) < 0) {
        GhmError saved = {0};
        if ((index_result != 0 || publish_fault != 0) && error != NULL) saved = *error;
        else ghm_error_from_git(&saved, "Update reference after checkout");
        /* SAFE against the just-checked-out baseline preserves external edits.
         * A failed rollback is explicit; never force checkout to hide it. */
        checkout.baseline = baseline;
        checkout.checkout_strategy = GIT_CHECKOUT_SAFE;
        ghm_index_lock_release(&index_lock);
        if (ghm_fault("checkout.rollback", error) != 0 || git_checkout_tree(repository, (git_object *)original, &checkout) < 0) {
            keep_record = 1;
            ghm_error_set(error, GHM_ERROR_GIT,
                          "Reference update and safe rollback failed; inspect worktree/index before retrying");
        } else if (error != NULL) *error = saved;
        goto done;
    }
    result = 0;
done:
    if (recorded) ghm_mutation_end(repository, keep_record);
    git_index_free(index);
    ghm_index_lock_release(&index_lock);
    git_tree_free(baseline);
    git_commit_free(original);
    git_transaction_free(transaction);
    return result;
}
