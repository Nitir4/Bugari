#include <ghm/branch.h>
#include "core/git.h"
#include "core/lock.h"
#include "storage/database.h"

#include <git2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ghm_branch_list_free(GhmBranchList *list)
{
    if (list == NULL) return;
    for (size_t i = 0; i < list->count; ++i) free(list->items[i].name);
    free(list->items);
    *list = (GhmBranchList){0};
}

int ghm_branch_list(const char *repository_path, GhmBranchList *out, GhmError *error)
{
    git_repository *repository = NULL;
    git_branch_iterator *iterator = NULL;
    git_reference *branch = NULL;
    git_branch_t kind = GIT_BRANCH_LOCAL;
    int result = -1, step;
    if (repository_path == NULL || repository_path[0] == '\0' || out == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Repository and branch output are required");
        return -1;
    }
    *out = (GhmBranchList){0};
    if (git_repository_open(&repository, repository_path) < 0 ||
        git_branch_iterator_new(&iterator, repository, GIT_BRANCH_ALL) < 0) {
        ghm_error_from_git(error, "List branches");
        goto done;
    }
    while ((step = git_branch_next(&branch, &kind, iterator)) == 0) {
        const char *name = NULL;
        GhmBranch *grown;
        GhmBranch *entry;
        if (git_branch_name(&name, branch) < 0 || name == NULL) {
            ghm_error_from_git(error, "Read branch name");
            goto done;
        }
        grown = realloc(out->items, (out->count + 1) * sizeof(*grown));
        if (grown == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); goto done; }
        out->items = grown;
        entry = &out->items[out->count++];
        *entry = (GhmBranch){0};
        entry->is_current = git_branch_is_head(branch) == 1;
        entry->is_remote = kind == GIT_BRANCH_REMOTE;
        entry->name = strdup(name);
        if (entry->name == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); goto done; }
        git_reference_free(branch); branch = NULL;
    }
    if (step != GIT_ITEROVER) { ghm_error_from_git(error, "Walk branches"); goto done; }
    result = 0;
done:
    git_reference_free(branch);
    git_branch_iterator_free(iterator);
    git_repository_free(repository);
    if (result != 0) ghm_branch_list_free(out);
    return result;
}

static int ghm_branch_create_unlocked(const char *repository_path, const char *name, GhmError *error)
{
    git_repository *repository = NULL;
    git_reference *created = NULL;
    git_object *head = NULL;
    int result = -1;
    if (repository_path == NULL || repository_path[0] == '\0' ||
        name == NULL || name[0] == '\0') {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Repository and branch name are required");
        return -1;
    }
    if (git_repository_open(&repository, repository_path) < 0 ||
        git_revparse_single(&head, repository, "HEAD") < 0) {
        ghm_error_from_git(error, "Read branch starting commit");
        goto done;
    }
    if (git_object_type(head) != GIT_OBJECT_COMMIT ||
        git_branch_create(&created, repository, name, (git_commit *)head, 0) < 0) {
        ghm_error_from_git(error, "Create branch");
        goto done;
    }
    result = 0;
done:
    git_reference_free(created);
    git_object_free(head);
    git_repository_free(repository);
    return result;
}

static int ghm_branch_checkout_unlocked(GhmContext *context, const char *repository_path,
                        const char *name, GhmError *error)
{
    git_repository *repository = NULL;
    git_reference *current = NULL, *target = NULL;
    git_commit *commit = NULL;
    git_status_list *status = NULL;
    git_status_options status_options = {0};
    git_checkout_options checkout = {0};
    int pending = 0, result = -1;
    if (context == NULL || repository_path == NULL || repository_path[0] == '\0' ||
        name == NULL || name[0] == '\0') {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Context, repository and branch name are required");
        return -1;
    }
    if (git_repository_open(&repository, repository_path) < 0) {
        ghm_error_from_git(error, "Open repository");
        goto done;
    }
    if (git_repository_state(repository) != GIT_REPOSITORY_STATE_NONE ||
        git_repository_head_detached(repository) != 0 ||
        git_repository_head(&current, repository) < 0 || git_reference_target(current) == NULL) {
        ghm_error_set(error, GHM_ERROR_GIT, "Checkout requires a current branch and no ongoing Git operation");
        goto done;
    }
    if (strcmp(git_reference_shorthand(current), name) == 0) { result = 0; goto done; }
    if (ghm_database_branch_has_active_jobs(context, git_repository_workdir(repository),
                                           git_reference_shorthand(current), &pending, error) != 0) goto done;
    if (pending) {
        ghm_error_set(error, GHM_ERROR_GIT,
                      "Finish or cancel pending scheduled jobs, or wait for running jobs, before switching");
        goto done;
    }
    if (git_branch_lookup(&target, repository, name, GIT_BRANCH_LOCAL) < 0 ||
        git_reference_target(target) == NULL) {
        ghm_error_set(error, GHM_ERROR_GIT, "Local branch was not found");
        goto done;
    }
    if (git_branch_is_checked_out(target) == 1) {
        ghm_error_set(error, GHM_ERROR_GIT, "Branch is already checked out in another linked worktree");
        goto done;
    }
    if (git_status_options_init(&status_options, GIT_STATUS_OPTIONS_VERSION) < 0 ||
        git_checkout_options_init(&checkout, GIT_CHECKOUT_OPTIONS_VERSION) < 0) {
        ghm_error_from_git(error, "Initialize checkout");
        goto done;
    }
    status_options.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    status_options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
    if (git_status_list_new(&status, repository, &status_options) < 0) {
        ghm_error_from_git(error, "Inspect working tree");
        goto done;
    }
    if (git_status_list_entrycount(status) != 0) {
        ghm_error_set(error, GHM_ERROR_GIT, "Save, commit or discard local changes before switching branches");
        goto done;
    }
    if (git_commit_lookup(&commit, repository, git_reference_target(target)) < 0) {
        ghm_error_from_git(error, "Read target branch");
        goto done;
    }
    if (ghm_checkout_transaction(repository, current, target, commit, 1,
                                  "Checkout local branch", error) != 0) goto done;
    result = 0;
done:
    git_status_list_free(status);
    git_commit_free(commit);
    git_reference_free(target);
    git_reference_free(current);
    git_repository_free(repository);
    return result;
}

static int ghm_branch_merge_unlocked(GhmContext *context, const char *repository_path,
                     const char *source_branch, GhmMergeOutcome *outcome,
                     char out_oid[GHM_OID_HEX_CAPACITY], GhmError *error)
{
    git_repository *repository = NULL;
    git_reference *head = NULL, *source = NULL, *current = NULL, *updated = NULL;
    git_commit *ours = NULL, *theirs = NULL, *merged = NULL;
    git_index *merge_index = NULL;
    git_tree *tree = NULL;
    git_signature *signature = NULL;
    git_status_list *status = NULL;
    git_status_options status_options = {0};
    git_checkout_options checkout = {0};
    git_oid initial_oid, target_oid, new_oid, tree_oid;
    char *message = NULL;
    int pending = 0, graph, result = -1;
    if (context == NULL || repository_path == NULL || repository_path[0] == '\0' ||
        source_branch == NULL || source_branch[0] == '\0' || outcome == NULL || out_oid == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Repository, source branch and outputs are required");
        return -1;
    }
    out_oid[0] = '\0';
    if (git_repository_open(&repository, repository_path) < 0) {
        ghm_error_from_git(error, "Open repository");
        goto done;
    }
    if (git_repository_is_bare(repository) ||
        git_repository_state(repository) != GIT_REPOSITORY_STATE_NONE ||
        git_repository_head_detached(repository) != 0 ||
        git_repository_head(&head, repository) < 0 || git_reference_target(head) == NULL) {
        ghm_error_set(error, GHM_ERROR_GIT, "Merge requires a checked-out branch and no ongoing Git operation");
        goto done;
    }
    if (git_branch_lookup(&source, repository, source_branch, GIT_BRANCH_LOCAL) < 0 ||
        git_reference_target(source) == NULL) {
        ghm_error_set(error, GHM_ERROR_GIT, "Source local branch was not found");
        goto done;
    }
    initial_oid = *git_reference_target(head);
    target_oid = *git_reference_target(source);
    if (ghm_database_branch_has_active_jobs(context, git_repository_workdir(repository),
                                           git_reference_shorthand(head), &pending, error) != 0) goto done;
    if (pending) {
        ghm_error_set(error, GHM_ERROR_GIT,
                      "Finish or cancel pending scheduled jobs, or wait for running jobs, before merging");
        goto done;
    }
    if (git_status_options_init(&status_options, GIT_STATUS_OPTIONS_VERSION) < 0 ||
        git_checkout_options_init(&checkout, GIT_CHECKOUT_OPTIONS_VERSION) < 0) {
        ghm_error_from_git(error, "Initialize merge inspection");
        goto done;
    }
    status_options.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    status_options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
    if (git_status_list_new(&status, repository, &status_options) < 0) {
        ghm_error_from_git(error, "Inspect working tree");
        goto done;
    }
    if (git_status_list_entrycount(status) != 0) {
        ghm_error_set(error, GHM_ERROR_GIT, "Commit or remove local changes before merging");
        goto done;
    }
    graph = git_graph_descendant_of(repository, &initial_oid, &target_oid);
    if (graph < 0) { ghm_error_from_git(error, "Inspect branch history"); goto done; }
    if (git_oid_cmp(&initial_oid, &target_oid) == 0 || graph > 0) {
        *outcome = GHM_MERGE_UP_TO_DATE;
        (void)git_oid_tostr(out_oid, GHM_OID_HEX_CAPACITY, &initial_oid);
        result = 0;
        goto done;
    }
    graph = git_graph_descendant_of(repository, &target_oid, &initial_oid);
    if (graph < 0) { ghm_error_from_git(error, "Inspect branch history"); goto done; }
    if (git_commit_lookup(&theirs, repository, &target_oid) < 0 ||
        git_commit_lookup(&ours, repository, &initial_oid) < 0) {
        ghm_error_from_git(error, "Read branch commits");
        goto done;
    }
    if (graph > 0) {
        new_oid = target_oid;
        merged = theirs;
        *outcome = GHM_MERGE_FAST_FORWARDED;
    } else {
        if (git_merge_commits(&merge_index, repository, ours, theirs, NULL) < 0) {
            ghm_error_from_git(error, "Prepare branch merge");
            goto done;
        }
        if (git_index_has_conflicts(merge_index)) {
            git_index_conflict_iterator *iterator = NULL;
            const git_index_entry *ancestor = NULL, *our_entry = NULL, *their_entry = NULL;
            const char *path = NULL;
            if (git_index_conflict_iterator_new(&iterator, merge_index) == 0 &&
                git_index_conflict_next(&ancestor, &our_entry, &their_entry, iterator) == 0) {
                const git_index_entry *entry = our_entry != NULL ? our_entry :
                                               their_entry != NULL ? their_entry : ancestor;
                if (entry != NULL) path = entry->path;
            }
            (void)snprintf(error->message, sizeof(error->message),
                           "Merge conflicts%s%s; no files changed. Resolve manually before retrying",
                           path != NULL ? " in " : "", path != NULL ? path : "");
            error->code = GHM_ERROR_GIT;
            git_index_conflict_iterator_free(iterator);
            goto done;
        }
        if (git_index_write_tree_to(&tree_oid, merge_index, repository) < 0 ||
            git_tree_lookup(&tree, repository, &tree_oid) < 0) {
            ghm_error_from_git(error, "Write merged tree");
            goto done;
        }
        if (git_signature_default(&signature, repository) < 0) {
            ghm_error_set(error, GHM_ERROR_GIT, "Set Git user.name and user.email before merging");
            goto done;
        }
        message = malloc(strlen(source_branch) + strlen(git_reference_shorthand(head)) + 32U);
        if (message == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); goto done; }
        (void)snprintf(message, strlen(source_branch) + strlen(git_reference_shorthand(head)) + 32U,
                       "Merge branch '%s' into %s", source_branch,
                       git_reference_shorthand(head));
        const git_commit *parents[] = {ours, theirs};
        if (git_commit_create(&new_oid, repository, NULL, signature, signature,
                              NULL, message, tree, 2, parents) < 0 ||
            git_commit_lookup(&merged, repository, &new_oid) < 0) {
            ghm_error_from_git(error, "Create merge commit");
            goto done;
        }
        *outcome = GHM_MERGE_COMMITTED;
    }
    if (git_repository_head(&current, repository) < 0 ||
        git_reference_target(current) == NULL ||
        strcmp(git_reference_name(current), git_reference_name(head)) != 0 ||
        git_oid_cmp(git_reference_target(current), &initial_oid) != 0 ||
        git_reference_target(source) == NULL ||
        git_reference_name_to_id(&tree_oid, repository, git_reference_name(source)) < 0 ||
        git_oid_cmp(&tree_oid, &target_oid) != 0) {
        ghm_error_set(error, GHM_ERROR_GIT, "A branch moved while preparing the merge; nothing was checked out");
        goto done;
    }
    if (ghm_checkout_transaction(repository, current, source, merged, 0,
                                  "Merge local branch", error) != 0) goto done;
    (void)git_oid_tostr(out_oid, GHM_OID_HEX_CAPACITY, &new_oid);
    result = 0;
done:
    free(message);
    git_status_list_free(status);
    git_signature_free(signature);
    git_tree_free(tree);
    git_index_free(merge_index);
    if (merged != theirs) git_commit_free(merged);
    git_commit_free(theirs);
    git_commit_free(ours);
    git_reference_free(updated);
    git_reference_free(current);
    git_reference_free(source);
    git_reference_free(head);
    git_repository_free(repository);
    return result;
}

int ghm_branch_create(const char *repository_path, const char *name, GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (ghm_repo_lock_acquire(repository_path, &lock, error) != 0) return -1;
    int result = ghm_branch_create_unlocked(repository_path, name, error);
    ghm_repo_lock_release(lock);
    return result;
}

int ghm_branch_checkout(GhmContext *context, const char *repository_path,
                        const char *name, GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (ghm_repo_lock_acquire(repository_path, &lock, error) != 0) return -1;
    int result = ghm_branch_checkout_unlocked(context, repository_path, name, error);
    ghm_repo_lock_release(lock);
    return result;
}

int ghm_branch_merge(GhmContext *context, const char *repository_path,
                     const char *source_branch, GhmMergeOutcome *outcome,
                     char out_oid[GHM_OID_HEX_CAPACITY], GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (ghm_repo_lock_acquire(repository_path, &lock, error) != 0) return -1;
    int result = ghm_branch_merge_unlocked(context, repository_path, source_branch, outcome, out_oid, error);
    ghm_repo_lock_release(lock);
    return result;
}
