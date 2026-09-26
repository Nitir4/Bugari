#include <ghm/branch.h>
#include <ghm/remote.h>
#include <ghm/scheduler.h>
#include "storage/database.h"

#include <dirent.h>
#include <git2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int remove_tree(const char *path)
{
    struct stat metadata;
    if (lstat(path, &metadata) != 0) return -1;
    if (!S_ISDIR(metadata.st_mode)) return unlink(path);
    DIR *directory = opendir(path);
    struct dirent *entry;
    if (directory == NULL) return -1;
    while ((entry = readdir(directory)) != NULL) {
        char child[1024];
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child) ||
            remove_tree(child) != 0) { closedir(directory); return -1; }
    }
    closedir(directory);
    return rmdir(path);
}

static int write_text(const char *directory, const char *contents)
{
    char path[1024];
    if (snprintf(path, sizeof(path), "%s/a.txt", directory) >= (int)sizeof(path)) return -1;
    FILE *file = fopen(path, "w");
    if (file == NULL) return -1;
    int wrote = fputs(contents, file);
    return fclose(file) == 0 && wrote != EOF ? 0 : -1;
}

static int unchanged(const char *path, const char *branch, const char *oid, GhmError *error)
{
    GhmStatus status = {0};
    git_repository *repository = NULL;
    git_reference *head = NULL;
    int result = -1;
    if (ghm_repo_status(path, &status, error) != 0 || status.count != 0 ||
        strcmp(status.branch, branch) != 0 || git_repository_open(&repository, path) < 0 ||
        git_repository_head(&head, repository) < 0 ||
        git_reference_target(head) == NULL || git_oid_streq(git_reference_target(head), oid) != 0)
        goto done;
    result = 0;
done:
    git_reference_free(head);
    git_repository_free(repository);
    ghm_status_free(&status);
    return result;
}

static int blocked_operations(GhmContext *context, const char *alias, const char *root,
                               const char *branch, const char *oid, GhmError *error)
{
    char output[GHM_OID_HEX_CAPACITY];
    GhmMergeOutcome merge;
    GhmPullOutcome pull;
    for (int operation = 0; operation < 4; ++operation) {
        *error = (GhmError){0};
        int result;
        switch (operation) {
        case 0: result = ghm_branch_checkout(context, alias, "feature", error); break;
        case 1: result = ghm_branch_merge(context, alias, "feature", &merge, output, error); break;
        case 2: result = ghm_repo_pull(context, alias, NULL, &pull, error); break;
        default:
            result = ghm_commit_rewrite_head(context, alias, oid, "reworded",
                1790000001, 0, 1790000002, 0, 1, output, error);
            break;
        }
        if (result == 0 || error->code != GHM_ERROR_GIT ||
            strstr(error->message, "scheduled") == NULL ||
            unchanged(root, branch, oid, error) != 0) {
            fprintf(stderr, "Guard failed for operation %d via %s: %s\n",
                    operation, alias, error->message);
            return -1;
        }
    }
    return 0;
}

/* Only the disposable test database is changed to simulate a worker claiming
 * its job. No worker timing or user database is involved. */
static int job_status(GhmContext *context, int64_t id, const char *status)
{
    sqlite3_stmt *stmt = NULL;
    int result = -1;
    if (sqlite3_prepare_v2(context->db, "UPDATE scheduled_jobs SET status=?1 WHERE id=?2",
                           -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 1, status, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_int64(stmt, 2, id) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_DONE &&
        sqlite3_changes(context->db) == 1) result = 0;
    sqlite3_finalize(stmt);
    return result;
}

static int stored_path(GhmContext *context, const char *path)
{
    sqlite3_stmt *stmt = NULL;
    int result = -1;
    if (sqlite3_prepare_v2(context->db, "UPDATE repositories SET path=?1",
                           -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(context->db) == 1) result = 0;
    sqlite3_finalize(stmt);
    return result;
}

static int snapshot_unchanged(GhmContext *context, git_repository *repository,
                               int64_t job_id, const char *status, const char *snapshot_ref,
                               const char *tree_oid)
{
    sqlite3_stmt *stmt = NULL;
    git_oid target;
    int result = -1;
    if (sqlite3_prepare_v2(context->db,
        "SELECT status,snapshot_ref,snapshot_tree_oid FROM scheduled_jobs WHERE id=?1",
        -1, &stmt, NULL) != SQLITE_OK || sqlite3_bind_int64(stmt, 1, job_id) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_ROW) goto done;
    const char *actual_status = (const char *)sqlite3_column_text(stmt, 0);
    const char *actual_ref = (const char *)sqlite3_column_text(stmt, 1);
    const char *actual_tree = (const char *)sqlite3_column_text(stmt, 2);
    if (actual_status == NULL || actual_ref == NULL || actual_tree == NULL ||
        strcmp(actual_status, status) != 0 || strcmp(actual_ref, snapshot_ref) != 0 ||
        strcmp(actual_tree, tree_oid) != 0 ||
        git_reference_name_to_id(&target, repository, snapshot_ref) < 0 ||
        git_oid_streq(&target, tree_oid) != 0) goto done;
    result = 0;
done:
    sqlite3_finalize(stmt);
    return result;
}

int main(void)
{
    char root[] = "/tmp/ghm-job-guard-test-XXXXXX";
    char repo_path[1024], data_path[1024], bare_path[1024], alias_path[1024];
    char dot_path[1024], git_path[1024], slash_path[1024];
    char first[GHM_OID_HEX_CAPACITY], feature[GHM_OID_HEX_CAPACITY], output[GHM_OID_HEX_CAPACITY];
    git_repository *repository = NULL, *bare = NULL;
    git_reference *head = NULL, *remote_head = NULL;
    git_object *base_object = NULL;
    git_remote *origin = NULL;
    char *branch = NULL, *branch_ref = NULL;
    char *snapshot_ref = NULL, *tree_oid = NULL;
    sqlite3_stmt *snapshot = NULL;
    int cwd = -1, result = 1;
    int64_t job_id = 0;
    GhmContext *context = NULL;
    GhmError error = {0};
    GhmPullOutcome pull;
    const GhmCommitSignature signature = {"Guard Test", "guard@example.invalid", 1790000000, 0};
    if (mkdtemp(root) == NULL || setenv("XDG_STATE_HOME", root, 1) != 0 ||
        snprintf(repo_path, sizeof(repo_path), "%s/repo", root) >= (int)sizeof(repo_path) ||
        snprintf(data_path, sizeof(data_path), "%s/data", root) >= (int)sizeof(data_path) ||
        snprintf(bare_path, sizeof(bare_path), "%s/bare.git", root) >= (int)sizeof(bare_path) ||
        snprintf(alias_path, sizeof(alias_path), "%s/link", root) >= (int)sizeof(alias_path) ||
        snprintf(dot_path, sizeof(dot_path), "%s/.", repo_path) >= (int)sizeof(dot_path) ||
        snprintf(git_path, sizeof(git_path), "%s/.git", repo_path) >= (int)sizeof(git_path) ||
        snprintf(slash_path, sizeof(slash_path), "%s/", repo_path) >= (int)sizeof(slash_path) ||
        ghm_context_open(data_path, &context, &error) != 0 ||
        git_repository_init(&repository, repo_path, 0) < 0 ||
        git_repository_init(&bare, bare_path, 1) < 0 ||
        ghm_repo_register(context, repo_path, &error) != 0 ||
        symlink(repo_path, alias_path) != 0 || write_text(repo_path, "base\n") != 0 ||
        ghm_commit_now(repo_path, "base", &signature, &signature, first, &error) != 0 ||
        git_repository_head(&head, repository) < 0) goto done;
    branch = strdup(git_reference_shorthand(head));
    branch_ref = strdup(git_reference_name(head));
    if (branch == NULL || branch_ref == NULL ||
        git_remote_create(&origin, repository, "origin", bare_path) < 0 ||
        ghm_branch_create(repo_path, "feature", &error) != 0 ||
        ghm_branch_checkout(context, repo_path, "feature", &error) != 0 ||
        write_text(repo_path, "feature\n") != 0 ||
        ghm_commit_now(repo_path, "feature", &signature, &signature, feature, &error) != 0 ||
        ghm_repo_push(repo_path, NULL, output, &error) != 0) goto done;
    git_oid feature_oid;
    if (git_oid_fromstr(&feature_oid, feature) < 0 ||
        git_reference_create(&remote_head, bare, branch_ref, &feature_oid, 0, NULL) < 0 ||
        ghm_branch_checkout(context, repo_path, branch, &error) != 0 ||
        write_text(repo_path, "frozen scheduled edit\n") != 0 ||
        ghm_schedule_add(context, repo_path, "future", &signature, &signature,
                         (int64_t)time(NULL) + 3600, &job_id, &error) != 0 ||
        write_text(repo_path, "base\n") != 0) goto done;
    if (sqlite3_prepare_v2(context->db,
        "SELECT snapshot_ref,snapshot_tree_oid FROM scheduled_jobs WHERE id=?1",
        -1, &snapshot, NULL) != SQLITE_OK || sqlite3_bind_int64(snapshot, 1, job_id) != SQLITE_OK ||
        sqlite3_step(snapshot) != SQLITE_ROW) goto done;
    snapshot_ref = strdup((const char *)sqlite3_column_text(snapshot, 0));
    tree_oid = strdup((const char *)sqlite3_column_text(snapshot, 1));
    sqlite3_finalize(snapshot); snapshot = NULL;
    if (snapshot_ref == NULL || tree_oid == NULL) goto done;
    /* Save cwd using an open directory so no path-size assumptions are needed. */
    DIR *cwd_directory = opendir(".");
    if (cwd_directory == NULL) goto done;
    cwd = dup(dirfd(cwd_directory));
    closedir(cwd_directory);
    if (cwd < 0 || chdir(repo_path) != 0) goto done;
    const char *aliases[] = {repo_path, dot_path, slash_path, git_path, alias_path, "."};
    const char *statuses[] = {"PENDING", "RUNNING"};
    const char *stored_paths[] = {repo_path, alias_path};
    for (size_t p = 0; p < sizeof(stored_paths) / sizeof(*stored_paths); ++p) {
        if (stored_path(context, stored_paths[p]) != 0) goto done;
        for (size_t s = 0; s < sizeof(statuses) / sizeof(*statuses); ++s) {
            if (job_status(context, job_id, statuses[s]) != 0) goto done;
            for (size_t a = 0; a < sizeof(aliases) / sizeof(*aliases); ++a)
                if (blocked_operations(context, aliases[a], repo_path, branch, first, &error) != 0)
                    goto done;
            if (snapshot_unchanged(context, repository, job_id, statuses[s], snapshot_ref, tree_oid) != 0)
                goto done;
        }
    }
    /* Jobs on another branch or another repository must not block this pull.
     * Reset only this disposable fixture between the successful operations. */
    if (git_revparse_single(&base_object, repository, first) < 0 ||
        stored_path(context, repo_path) != 0 || job_status(context, job_id, "PENDING") != 0 ||
        sqlite3_exec(context->db, "UPDATE scheduled_jobs SET branch='other-branch'",
                     NULL, NULL, NULL) != SQLITE_OK ||
        ghm_repo_pull(context, ".", NULL, &pull, &error) != 0 || pull != GHM_PULL_FAST_FORWARDED ||
        git_reset(repository, base_object, GIT_RESET_HARD, NULL) < 0) goto done;
    /* Restore the branch using bound text, then designate the other repository. */
    if (sqlite3_prepare_v2(context->db, "UPDATE scheduled_jobs SET branch=?1",
                           -1, &snapshot, NULL) != SQLITE_OK ||
        sqlite3_bind_text(snapshot, 1, branch, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_step(snapshot) != SQLITE_DONE) goto done;
    sqlite3_finalize(snapshot); snapshot = NULL;
    if (stored_path(context, bare_path) != 0 ||
        ghm_repo_pull(context, ".", NULL, &pull, &error) != 0 || pull != GHM_PULL_FAST_FORWARDED ||
        git_reset(repository, base_object, GIT_RESET_HARD, NULL) < 0) goto done;
    if (stored_path(context, repo_path) != 0 || job_status(context, job_id, "PENDING") != 0 ||
        ghm_schedule_cancel(context, job_id, &error) != 0 ||
        ghm_branch_checkout(context, ".", "feature", &error) != 0 ||
        ghm_branch_checkout(context, ".", branch, &error) != 0 ||
        ghm_repo_pull(context, ".", NULL, &pull, &error) != 0 || pull != GHM_PULL_FAST_FORWARDED ||
        ghm_commit_rewrite_head(context, ".", feature, "reworded",
            1790000001, 0, 1790000002, 0, 1, output, &error) != 0) goto done;
    result = 0;
done:
    if (result != 0) fprintf(stderr, "Job guard test failed: %s\n", error.message);
    if (cwd >= 0) {
        if (fchdir(cwd) != 0) result = 1;
        close(cwd);
    }
    free(branch);
    free(branch_ref);
    free(snapshot_ref);
    free(tree_oid);
    sqlite3_finalize(snapshot);
    git_object_free(base_object);
    git_reference_free(remote_head);
    git_reference_free(head);
    git_remote_free(origin);
    git_repository_free(bare);
    git_repository_free(repository);
    ghm_context_close(context);
    if (remove_tree(root) != 0) { fprintf(stderr, "Could not remove guard test directory\n"); result = 1; }
    return result;
}
