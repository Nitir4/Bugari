#include <ghm/commit.h>
#include "core/git.h"
#include "core/lock.h"
#include "core/storage_check.h"
#include "core/fault.h"
#include "core/commit_private.h"
#include "core/mutation.h"
#include "core/index_lock.h"

static int commit_with_mode(const char *repository_path, const char *message,
                            const GhmCommitSignature *author, const GhmCommitSignature *committer,
                            int stage_all, char out_oid[GHM_OID_HEX_CAPACITY], GhmError *error);

#include <git2.h>
#include <git2/sys/repository.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int ghm_commit_default_identity(const char *repository_path, char **out_name,
                                char **out_email, GhmError *error)
{
    git_repository *repository = NULL;
    git_signature *signature = NULL;
    int result = -1;
    if (repository_path == NULL || out_name == NULL || out_email == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Repository and identity outputs are required");
        return -1;
    }
    *out_name = NULL;
    *out_email = NULL;
    if (git_repository_open(&repository, repository_path) < 0) {
        ghm_error_from_git(error, "Open repository");
        goto done;
    }
    if (git_signature_default(&signature, repository) < 0) {
        ghm_error_set(error, GHM_ERROR_GIT, "Set Git user.name and user.email for this repository before scheduling");
        goto done;
    }
    *out_name = strdup(signature->name);
    *out_email = strdup(signature->email);
    if (*out_name == NULL || *out_email == NULL) {
        free(*out_name); *out_name = NULL;
        free(*out_email); *out_email = NULL;
        ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
        goto done;
    }
    result = 0;
done:
    git_signature_free(signature);
    git_repository_free(repository);
    return result;
}

static int valid_snapshot_ref(const char *name)
{
    static const char prefix[] = "refs/ghm/jobs/";
    int valid = 0;
    return name != NULL && strncmp(name, prefix, sizeof(prefix) - 1) == 0 &&
           name[sizeof(prefix) - 1] != '\0' &&
           git_reference_name_is_valid(&valid, name) == 0 && valid;
}

static int open_worktree(git_repository **out, const char *path, GhmError *error)
{
    if (git_repository_open(out, path) < 0) {
        ghm_error_from_git(error, "Open repository");
        return -1;
    }
    if (git_repository_is_bare(*out)) {
        ghm_error_set(error, GHM_ERROR_GIT, "A working tree is required");
        return -1;
    }
    if (git_repository_state(*out) != GIT_REPOSITORY_STATE_NONE) {
        ghm_error_set(error, GHM_ERROR_GIT, "Finish the current Git operation before scheduling a commit");
        return -1;
    }
    return 0;
}

static int load_base_tree(git_tree **out, git_repository *repository,
                          const char *base_tree_oid, GhmError *error)
{
    git_object *head = NULL;
    git_oid oid;
    int result = -1;
    if (base_tree_oid != NULL) {
        if (git_oid_fromstr(&oid, base_tree_oid) < 0) {
            ghm_error_set(error, GHM_ERROR_ARGUMENT, "Invalid base tree ID");
            return -1;
        }
        if (git_tree_lookup(out, repository, &oid) < 0)
            ghm_error_from_git(error, "Load base snapshot tree");
        else result = 0;
        return result;
    }
    if (git_revparse_single(&head, repository, "HEAD^{commit}") < 0) {
        ghm_error_set(error, GHM_ERROR_GIT, "An initial commit is required before scheduling");
        return -1;
    }
    if (git_commit_tree(out, (git_commit *)head) < 0) ghm_error_from_git(error, "Load HEAD tree");
    else result = 0;
    git_object_free(head);
    return result;
}

static int check_prior_only_deletions(git_repository *repository, git_index *real_index,
                                      git_tree *head_tree, git_tree *base_tree,
                                      GhmError *error)
{
    git_diff *prior = NULL;
    int result = -1;
    if (git_diff_tree_to_tree(&prior, repository, head_tree, base_tree, NULL) < 0) {
        ghm_error_from_git(error, "Inspect earlier scheduled snapshot");
        return -1;
    }
    for (size_t i = 0; i < git_diff_num_deltas(prior); ++i) {
        const git_diff_delta *delta = git_diff_get_delta(prior, i);
        if (delta == NULL || delta->status != GIT_DELTA_ADDED ||
            delta->new_file.path == NULL ||
            git_index_get_bypath(real_index, delta->new_file.path, 0) != NULL)
            continue;
        const char *workdir = git_repository_workdir(repository);
        size_t root_length = strlen(workdir);
        size_t path_length = strlen(delta->new_file.path);
        if (root_length > SIZE_MAX - path_length - 1U) {
            ghm_error_set(error, GHM_ERROR_MEMORY, "Scheduled path is too long");
            goto done;
        }
        char *absolute = malloc(root_length + path_length + 1U);
        struct stat metadata;
        int stat_result;
        int saved_errno;
        if (absolute == NULL) {
            ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory inspecting scheduled files");
            goto done;
        }
        memcpy(absolute, workdir, root_length);
        memcpy(absolute + root_length, delta->new_file.path, path_length + 1U);
        stat_result = lstat(absolute, &metadata);
        saved_errno = errno;
        free(absolute);
        if (stat_result == 0) continue;
        if (saved_errno == ENOENT || saved_errno == ENOTDIR) {
            ghm_error_set(error, GHM_ERROR_GIT,
                          "A file from the previous job was removed; use Capture all current changes for this job");
            goto done;
        }
        ghm_error_set(error, GHM_ERROR_IO, "Cannot inspect a file from the previous scheduled job");
        goto done;
    }
    result = 0;
done:
    git_diff_free(prior);
    return result;
}

static int apply_staged_paths(git_repository *repository, git_index *real_index,
                              git_index *memory_index, git_tree *base_tree,
                              GhmError *error)
{
    git_tree *head_tree = NULL;
    git_diff *diff = NULL;
    int result = -1;
    if (load_base_tree(&head_tree, repository, NULL, error) != 0) return -1;
    if (check_prior_only_deletions(repository, real_index, head_tree, base_tree, error) != 0)
        goto done;
    if (git_diff_tree_to_index(&diff, repository, head_tree, real_index, NULL) < 0) {
        ghm_error_from_git(error, "Compare staged files with HEAD");
        goto done;
    }
    for (size_t i = 0; i < git_diff_num_deltas(diff); ++i) {
        const git_diff_delta *delta = git_diff_get_delta(diff, i);
        if (delta == NULL) continue;
        if (delta->status == GIT_DELTA_DELETED || delta->status == GIT_DELTA_RENAMED) {
            if (git_index_remove_bypath(memory_index, delta->old_file.path) < 0) {
                ghm_error_from_git(error, "Capture staged deletion");
                goto done;
            }
        }
        if (delta->status != GIT_DELTA_DELETED) {
            const git_index_entry *entry = git_index_get_bypath(real_index, delta->new_file.path, 0);
            if (entry == NULL || git_index_add(memory_index, entry) < 0) {
                ghm_error_from_git(error, "Capture staged file");
                goto done;
            }
        }
    }
    result = 0;
done:
    git_diff_free(diff);
    git_tree_free(head_tree);
    return result;
}

static int ghm_snapshot_capture_mode_unlocked(const char *repository_path, const char *base_tree_oid,
                              const char *snapshot_ref, int stage_all,
                              char out_tree_oid[GHM_OID_HEX_CAPACITY], GhmError *error)
{
    git_repository *repository = NULL;
    git_index *real_index = NULL;
    git_index *memory_index = NULL;
    git_tree *base_tree = NULL;
    git_reference *saved_ref = NULL;
    git_oid tree_oid;
    int result = -1;
    if (repository_path == NULL || repository_path[0] == '\0' ||
        !valid_snapshot_ref(snapshot_ref) || out_tree_oid == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Repository, job ref and output are required");
        return -1;
    }
    out_tree_oid[0] = '\0';
    if (open_worktree(&repository, repository_path, error) != 0) goto done;
    if (git_repository_index(&real_index, repository) < 0) {
        ghm_error_from_git(error, "Read repository index");
        goto done;
    }
    if (git_index_has_conflicts(real_index)) {
        ghm_error_set(error, GHM_ERROR_GIT, "Resolve index conflicts before scheduling");
        goto done;
    }
    if (load_base_tree(&base_tree, repository, base_tree_oid, error) != 0) goto done;
    if (git_index_new(&memory_index) < 0 ||
        git_repository_set_index(repository, memory_index) < 0 ||
        git_index_read_tree(memory_index, base_tree) < 0) {
        ghm_error_from_git(error, "Prepare scheduled snapshot");
        goto done;
    }
    if (stage_all) {
        if (git_index_update_all(memory_index, NULL, NULL, NULL) < 0 ||
            git_index_add_all(memory_index, NULL, 0U, NULL, NULL) < 0) {
            ghm_error_from_git(error, "Capture working tree changes");
            goto done;
        }
    } else if (apply_staged_paths(repository, real_index, memory_index, base_tree, error) != 0)
        goto done;
    if (git_index_write_tree_to(&tree_oid, memory_index, repository) < 0) {
        ghm_error_from_git(error, "Write scheduled snapshot tree");
        goto done;
    }
    if (git_oid_equal(&tree_oid, git_tree_id(base_tree))) {
        ghm_error_set(error, GHM_ERROR_GIT, "No changes to capture for this job");
        goto done;
    }
    if (git_reference_create(&saved_ref, repository, snapshot_ref, &tree_oid, 0,
                             "ghm scheduled snapshot") < 0) {
        ghm_error_from_git(error, "Protect scheduled snapshot");
        goto done;
    }
    (void)git_oid_tostr(out_tree_oid, GHM_OID_HEX_CAPACITY, &tree_oid);
    result = 0;
done:
    git_reference_free(saved_ref);
    git_tree_free(base_tree);
    git_index_free(memory_index);
    git_index_free(real_index);
    git_repository_free(repository);
    return result;
}

int ghm_snapshot_capture(const char *repository_path, const char *base_tree_oid,
                         const char *snapshot_ref, char out_tree_oid[GHM_OID_HEX_CAPACITY],
                         GhmError *error)
{
    return ghm_snapshot_capture_mode(repository_path, base_tree_oid, snapshot_ref, 1,
                                     out_tree_oid, error);
}

static int make_signature(git_signature **out, const GhmCommitSignature *input,
                          GhmError *error)
{
    if (input == NULL || input->name == NULL || input->name[0] == '\0' ||
        input->email == NULL || input->email[0] == '\0' ||
        input->offset_minutes < -14 * 60 || input->offset_minutes > 14 * 60 ||
        input->timestamp < 0 || input->timestamp > LONG_MAX) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Invalid commit identity or timestamp");
        return -1;
    }
    if (git_signature_new(out, input->name, input->email,
                          (git_time_t)input->timestamp, input->offset_minutes) < 0) {
        ghm_error_from_git(error, "Create commit signature");
        return -1;
    }
    return 0;
}

static int commit_with_mode_unlocked(const char *repository_path, const char *message,
                            const GhmCommitSignature *author, const GhmCommitSignature *committer,
                            int stage_all, char out_oid[GHM_OID_HEX_CAPACITY], GhmError *error)
{
    git_repository *repository = NULL;
    git_reference *head = NULL;
    git_commit *parent = NULL;
    git_tree *parent_tree = NULL;
    git_tree *tree = NULL;
    git_index *real_index = NULL;
    git_index *memory_index = NULL;
    git_signature *author_signature = NULL;
    git_signature *committer_signature = NULL;
    git_oid tree_oid;
    git_oid commit_oid;
    git_transaction *transaction = NULL;
    GhmIndexLock index_lock = {.descriptor = -1};
    int result = -1;
    int head_result;
    int recorded = 0, published = 0;
    git_commit *created = NULL;
    if (repository_path == NULL || repository_path[0] == '\0' ||
        message == NULL || message[0] == '\0' || out_oid == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Repository, commit message and output are required");
        return -1;
    }
    out_oid[0] = '\0';
    if (ghm_repository_storage_check(repository_path, error) != 0) goto done;
    if (open_worktree(&repository, repository_path, error) != 0) goto done;
    if (git_repository_head_detached(repository) > 0) {
        ghm_error_set(error, GHM_ERROR_GIT, "Check out a branch before committing");
        goto done;
    }
    head_result = git_repository_head(&head, repository);
    if (head_result == 0) {
        if (git_reference_target(head) == NULL ||
            git_commit_lookup(&parent, repository, git_reference_target(head)) < 0 ||
            git_commit_tree(&parent_tree, parent) < 0) {
            ghm_error_from_git(error, "Read current commit");
            goto done;
        }
    } else if (head_result == GIT_EUNBORNBRANCH) {
        if (git_reference_lookup(&head, repository, "HEAD") < 0) {
            ghm_error_from_git(error, "Read unborn branch");
            goto done;
        }
    } else {
        ghm_error_from_git(error, "Read branch HEAD");
        goto done;
    }
    if (ghm_head_transaction(&transaction, repository, head, error) != 0) goto done;
    if (ghm_index_lock_acquire(&index_lock, repository, error) != 0) goto done;
    if (git_repository_index(&real_index, repository) < 0 || git_index_read(real_index, 1) < 0) {
        ghm_error_from_git(error, "Read repository index");
        goto done;
    }
    if (git_index_has_conflicts(real_index)) {
        ghm_error_set(error, GHM_ERROR_GIT, "Resolve index conflicts before committing");
        goto done;
    }
    if (stage_all) {
        if (git_index_new(&memory_index) < 0 ||
            git_repository_set_index(repository, memory_index) < 0 ||
            (parent_tree != NULL && git_index_read_tree(memory_index, parent_tree) < 0) ||
            git_index_update_all(memory_index, NULL, NULL, NULL) < 0 ||
            git_index_add_all(memory_index, NULL, 0U, NULL, NULL) < 0 ||
            git_index_write_tree_to(&tree_oid, memory_index, repository) < 0) {
            ghm_error_from_git(error, "Stage working tree");
            goto done;
        }
    } else if (git_index_write_tree(&tree_oid, real_index) < 0) {
        ghm_error_from_git(error, "Write staged tree");
        goto done;
    }
    if (git_tree_lookup(&tree, repository, &tree_oid) < 0) {
        ghm_error_from_git(error, "Load staged tree");
        goto done;
    }
    if ((parent != NULL && git_oid_equal(&tree_oid, git_commit_tree_id(parent))) ||
        (parent == NULL && git_index_entrycount(stage_all ? memory_index : real_index) == 0)) {
        ghm_error_set(error, GHM_ERROR_GIT,
                      stage_all ? "No working-tree changes to commit" : "No staged changes to commit");
        goto done;
    }
    if (make_signature(&author_signature, author, error) != 0 ||
        make_signature(&committer_signature, committer, error) != 0) goto done;
    const git_commit *parents[] = {parent};
    if (git_commit_create(&commit_oid, repository, NULL, author_signature,
                          committer_signature, NULL, message, tree,
                          parent != NULL ? 1U : 0U, parent != NULL ? parents : NULL) < 0) {
        ghm_error_from_git(error, "Create commit");
        goto done;
    }
    const char *branch_ref = git_reference_type(head) == GIT_REFERENCE_SYMBOLIC ?
                             git_reference_symbolic_target(head) : git_reference_name(head);
    if (git_commit_lookup(&created, repository, &commit_oid) < 0 ||
        ghm_mutation_begin(repository, head, created, NULL, 0, 0, stage_all, real_index, error) != 0) goto done;
    recorded = 1;
    if (ghm_fault("commit.before_publish", error) != 0) goto done;
    if (git_transaction_set_target(transaction, branch_ref, &commit_oid,
                                    committer_signature, "Create local commit") < 0 ||
        git_transaction_commit(transaction) < 0) {
        ghm_error_from_git(error, "Advance committed branch");
        goto done;
    }
    (void)git_oid_tostr(out_oid, GHM_OID_HEX_CAPACITY, &commit_oid);
    published = 1;
    if (ghm_fault("commit.after_publish", error) != 0) goto done;
    if (stage_all && (git_index_read_tree(real_index, tree) < 0 ||
                     ghm_index_lock_write(&index_lock, real_index, error) != 0)) {
        ghm_error_set(error, GHM_ERROR_GIT,
                      "Commit created, but the index could not be synchronized; inspect before retrying");
        goto done;
    }
    result = 0;
done:
    if (recorded) ghm_mutation_end(repository, published && result != 0);
    git_commit_free(created);
    ghm_index_lock_release(&index_lock);
    git_transaction_free(transaction);
    git_signature_free(committer_signature);
    git_signature_free(author_signature);
    git_index_free(memory_index);
    git_index_free(real_index);
    git_tree_free(tree);
    git_tree_free(parent_tree);
    git_commit_free(parent);
    git_reference_free(head);
    git_repository_free(repository);
    return result;
}

int ghm_commit_now(const char *repository_path, const char *message,
                   const GhmCommitSignature *author, const GhmCommitSignature *committer,
                   char out_oid[GHM_OID_HEX_CAPACITY], GhmError *error)
{
    return commit_with_mode(repository_path, message, author, committer, 1, out_oid, error);
}

int ghm_commit_staged(const char *repository_path, const char *message,
                      const GhmCommitSignature *author, const GhmCommitSignature *committer,
                      char out_oid[GHM_OID_HEX_CAPACITY], GhmError *error)
{
    return commit_with_mode(repository_path, message, author, committer, 0, out_oid, error);
}

static int ghm_commit_from_snapshot_unlocked(const char *repository_path, const char *snapshot_ref,
                             const char *expected_branch, const char *expected_parent_oid,
                             const char *message,
                             const GhmCommitSignature *author,
                             const GhmCommitSignature *committer,
                             GhmCommitPrepared prepared, void *payload,
                             char out_commit_oid[GHM_OID_HEX_CAPACITY], GhmError *error)
{
    git_repository *repository = NULL;
    git_reference *head = NULL;
    git_reference *snapshot = NULL;
    git_tree *tree = NULL;
    git_commit *parent = NULL;
    git_index *index = NULL;
    git_signature *author_signature = NULL;
    git_signature *committer_signature = NULL;
    git_oid expected_oid;
    git_oid created_oid;
    git_oid staged_tree_oid;
    git_transaction *transaction = NULL;
    GhmIndexLock index_lock = {.descriptor = -1};
    int result = -1;
    int recorded = 0, published = 0;
    git_commit *created = NULL;
    if (repository_path == NULL || repository_path[0] == '\0' ||
        !valid_snapshot_ref(snapshot_ref) || expected_branch == NULL ||
        expected_branch[0] == '\0' || expected_parent_oid == NULL ||
        message == NULL || message[0] == '\0' || out_commit_oid == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Repository, snapshot, parent and message are required");
        return -1;
    }
    out_commit_oid[0] = '\0';
    if (git_oid_fromstr(&expected_oid, expected_parent_oid) < 0) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Invalid expected parent ID");
        return -1;
    }
    if (open_worktree(&repository, repository_path, error) != 0) goto done;
    if (git_repository_head_detached(repository)) {
        ghm_error_set(error, GHM_ERROR_GIT, "Cannot execute a scheduled commit on detached HEAD");
        goto done;
    }
    if (git_repository_head(&head, repository) < 0) {
        ghm_error_from_git(error, "Read branch HEAD");
        goto done;
    }
    if (strcmp(git_reference_shorthand(head), expected_branch) != 0) {
        ghm_error_set(error, GHM_ERROR_GIT, "Scheduled branch is not checked out");
        goto done;
    }
    if (git_oid_cmp(git_reference_target(head), &expected_oid) != 0) {
        ghm_error_set(error, GHM_ERROR_GIT, "Branch changed since this job was scheduled; review it before committing");
        goto done;
    }
    if (ghm_head_transaction(&transaction, repository, head, error) != 0) goto done;
    if (git_reference_lookup(&snapshot, repository, snapshot_ref) < 0 ||
        git_reference_target(snapshot) == NULL ||
        git_tree_lookup(&tree, repository, git_reference_target(snapshot)) < 0) {
        ghm_error_from_git(error, "Load scheduled snapshot");
        goto done;
    }
    if (git_commit_lookup(&parent, repository, &expected_oid) < 0) {
        ghm_error_from_git(error, "Load expected parent commit");
        goto done;
    }
    if (git_oid_equal(git_tree_id(tree), git_commit_tree_id(parent))) {
        ghm_error_set(error, GHM_ERROR_GIT, "Scheduled snapshot has no changes against its parent");
        goto done;
    }
    if (ghm_index_lock_acquire(&index_lock, repository, error) != 0) goto done;
    if (git_repository_index(&index, repository) < 0 || git_index_read(index, 1) < 0 ||
        git_index_write_tree(&staged_tree_oid, index) < 0) {
        ghm_error_from_git(error, "Inspect index or resolve its conflicts");
        goto done;
    }
    if (make_signature(&author_signature, author, error) != 0 ||
        make_signature(&committer_signature, committer, error) != 0) goto done;
    const git_commit *parents[] = {parent};
    if (git_commit_create(&created_oid, repository, NULL, author_signature,
                          committer_signature, NULL, message, tree, 1, parents) < 0) {
        ghm_error_from_git(error, "Create scheduled commit");
        goto done;
    }
    char planned[GHM_OID_HEX_CAPACITY];
    (void)git_oid_tostr(planned, sizeof(planned), &created_oid);
    if (prepared != NULL && prepared(planned, payload, error) != 0) goto done;
    if (git_commit_lookup(&created, repository, &created_oid) < 0 ||
        ghm_mutation_begin(repository, head, created, NULL, 0, 0,
            git_oid_equal(&staged_tree_oid, git_commit_tree_id(parent)), index, error) != 0) goto done;
    recorded = 1;
    if (ghm_fault("commit.before_publish", error) != 0) goto done;
    if (git_transaction_set_target(transaction, git_reference_name(head), &created_oid,
                                    committer_signature, "Create scheduled commit") < 0 ||
        git_transaction_commit(transaction) < 0) {
        ghm_error_from_git(error, "Advance scheduled branch");
        goto done;
    }
    (void)git_oid_tostr(out_commit_oid, GHM_OID_HEX_CAPACITY, &created_oid);
    published = 1;
    if (ghm_fault("commit.after_publish", error) != 0) goto done;
    /* Only advance a clean index. A separately staged later edit belongs to
     * the user's next job or unscheduled work and must be left untouched. */
    if (git_oid_equal(&staged_tree_oid, git_commit_tree_id(parent)) &&
        (git_index_read_tree(index, tree) < 0 || ghm_index_lock_write(&index_lock, index, error) != 0)) {
        ghm_error_set(error, GHM_ERROR_GIT,
                      "Commit was created, but the index could not be synchronized; review before retrying");
        goto done;
    }
    result = 0;
done:
    if (recorded) ghm_mutation_end(repository, published && result != 0);
    git_commit_free(created);
    ghm_index_lock_release(&index_lock);
    git_transaction_free(transaction);
    git_index_free(index);
    git_signature_free(committer_signature);
    git_signature_free(author_signature);
    git_commit_free(parent);
    git_tree_free(tree);
    git_reference_free(snapshot);
    git_reference_free(head);
    git_repository_free(repository);
    return result;
}

static int ghm_snapshot_release_unlocked(const char *repository_path, const char *snapshot_ref,
                         const char *expected_tree_oid, GhmError *error)
{
    git_repository *repository = NULL;
    git_reference *snapshot = NULL;
    git_oid expected_oid;
    int result = -1;
    if (repository_path == NULL || repository_path[0] == '\0' ||
        !valid_snapshot_ref(snapshot_ref) || expected_tree_oid == NULL ||
        git_oid_fromstr(&expected_oid, expected_tree_oid) < 0) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Valid repository, job ref and tree ID are required");
        return -1;
    }
    if (git_repository_open(&repository, repository_path) < 0) {
        ghm_error_from_git(error, "Open repository");
        goto done;
    }
    if (git_reference_lookup(&snapshot, repository, snapshot_ref) < 0) {
        ghm_error_from_git(error, "Find scheduled snapshot");
        goto done;
    }
    if (git_reference_target(snapshot) == NULL ||
        git_oid_cmp(git_reference_target(snapshot), &expected_oid) != 0) {
        ghm_error_set(error, GHM_ERROR_GIT, "Scheduled snapshot changed; refusing to remove it");
        goto done;
    }
    if (git_reference_delete(snapshot) < 0) {
        ghm_error_from_git(error, "Release scheduled snapshot");
        goto done;
    }
    result = 0;
done:
    git_reference_free(snapshot);
    git_repository_free(repository);
    return result;
}

int ghm_snapshot_capture_mode(const char *repository_path, const char *base_tree_oid,
                              const char *snapshot_ref, int stage_all,
                              char out_tree_oid[GHM_OID_HEX_CAPACITY], GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (ghm_repo_lock_acquire(repository_path, &lock, error) != 0) return -1;
    int result = ghm_snapshot_capture_mode_unlocked(repository_path, base_tree_oid, snapshot_ref, stage_all, out_tree_oid, error);
    ghm_repo_lock_release(lock);
    return result;
}

int ghm_commit_from_snapshot(const char *repository_path, const char *snapshot_ref,
                             const char *expected_branch, const char *expected_parent_oid,
                             const char *message,
                             const GhmCommitSignature *author,
                             const GhmCommitSignature *committer,
                             char out_commit_oid[GHM_OID_HEX_CAPACITY], GhmError *error)
{
    return ghm_commit_snapshot_prepared(repository_path, snapshot_ref, expected_branch,
        expected_parent_oid, message, author, committer, NULL, NULL, out_commit_oid, error);
}

int ghm_commit_snapshot_prepared(const char *repository_path, const char *snapshot_ref,
                             const char *expected_branch, const char *expected_parent_oid,
                             const char *message, const GhmCommitSignature *author,
                             const GhmCommitSignature *committer,
                             GhmCommitPrepared prepared, void *payload,
                             char out_commit_oid[GHM_OID_HEX_CAPACITY], GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (ghm_repo_lock_acquire(repository_path, &lock, error) != 0) return -1;
    int result = ghm_commit_from_snapshot_unlocked(repository_path, snapshot_ref, expected_branch, expected_parent_oid, message, author, committer, prepared, payload, out_commit_oid, error);
    ghm_repo_lock_release(lock);
    return result;
}

int ghm_snapshot_release(const char *repository_path, const char *snapshot_ref,
                         const char *expected_tree_oid, GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (ghm_repo_lock_acquire(repository_path, &lock, error) != 0) return -1;
    int result = ghm_snapshot_release_unlocked(repository_path, snapshot_ref, expected_tree_oid, error);
    ghm_repo_lock_release(lock);
    return result;
}

static int commit_with_mode(const char *repository_path, const char *message,
                            const GhmCommitSignature *author, const GhmCommitSignature *committer,
                            int stage_all, char out_oid[GHM_OID_HEX_CAPACITY], GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (ghm_repo_lock_acquire(repository_path, &lock, error) != 0) return -1;
    int result = commit_with_mode_unlocked(repository_path, message, author, committer, stage_all, out_oid, error);
    ghm_repo_lock_release(lock);
    return result;
}
