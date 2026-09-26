#include <ghm/ghm.h>
#include <ghm/scheduler.h>
#include <ghm/remote.h>

#include <dirent.h>
#include <git2.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int remove_tree(const char *path)
{
    DIR *directory = opendir(path);
    struct dirent *entry;
    if (directory == NULL) return unlink(path);
    while ((entry = readdir(directory)) != NULL) {
        char child[512];
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child) ||
            remove_tree(child) != 0) { closedir(directory); return -1; }
    }
    closedir(directory);
    return rmdir(path);
}

static int write_text(const char *directory, const char *name, const char *contents)
{
    char path[512];
    FILE *file;
    int result;
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >= (int)sizeof(path)) return -1;
    file = fopen(path, "w");
    if (file == NULL) return -1;
    result = fputs(contents, file);
    return fclose(file) == 0 && result != EOF ? 0 : -1;
}

static int check(int condition, const char *message)
{
    if (condition) return 0;
    fprintf(stderr, "FAIL: %s\n", message);
    return -1;
}

static int make_initial_commit(git_repository *repository, const char *path)
{
    git_index *index = NULL;
    git_tree *tree = NULL;
    git_signature *signature = NULL;
    git_oid tree_oid, commit_oid;
    int result = -1;
    if (write_text(path, "a.txt", "base\n") != 0 ||
        git_repository_index(&index, repository) < 0 ||
        git_index_add_bypath(index, "a.txt") < 0 || git_index_write(index) < 0 ||
        git_index_write_tree(&tree_oid, index) < 0 ||
        git_tree_lookup(&tree, repository, &tree_oid) < 0 ||
        git_signature_new(&signature, "Test", "test@example.invalid", 1700000000, 0) < 0 ||
        git_commit_create(&commit_oid, repository, "HEAD", signature, signature,
                          NULL, "initial", tree, 0, NULL) < 0) goto done;
    result = 0;
done:
    git_signature_free(signature);
    git_tree_free(tree);
    git_index_free(index);
    return result;
}

static int inspect_job(sqlite3 *db, int64_t id, const char *status,
                       char *out_oid, size_t out_capacity)
{
    sqlite3_stmt *stmt = NULL;
    int result = -1;
    if (sqlite3_prepare_v2(db, "SELECT status,result_oid FROM scheduled_jobs WHERE id=?1",
                           -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 1, id) != SQLITE_OK || sqlite3_step(stmt) != SQLITE_ROW) goto done;
    const char *actual = (const char *)sqlite3_column_text(stmt, 0);
    const char *oid = (const char *)sqlite3_column_text(stmt, 1);
    if (strcmp(actual, status) != 0) goto done;
    if (oid != NULL && out_oid != NULL) (void)snprintf(out_oid, out_capacity, "%s", oid);
    result = 0;
done:
    sqlite3_finalize(stmt);
    return result;
}

static int inspect_commit(git_repository *repository, const char *oid_text,
                          const char *message, const char *a_contents, int has_b)
{
    git_oid oid;
    git_commit *commit = NULL;
    git_tree *tree = NULL;
    git_tree_entry *entry = NULL;
    git_blob *blob = NULL;
    int result = -1;
    if (git_oid_fromstr(&oid, oid_text) < 0 || git_commit_lookup(&commit, repository, &oid) < 0 ||
        git_commit_tree(&tree, commit) < 0 ||
        git_tree_entry_bypath(&entry, tree, "a.txt") < 0 ||
        git_blob_lookup(&blob, repository, git_tree_entry_id(entry)) < 0) goto done;
    result = check(strcmp(git_commit_message(commit), message) == 0 &&
                   git_commit_author(commit)->when.time == 1790000000 &&
                   git_commit_committer(commit)->when.offset == 330 &&
                   git_blob_rawsize(blob) == (git_object_size_t)strlen(a_contents) &&
                   memcmp(git_blob_rawcontent(blob), a_contents, strlen(a_contents)) == 0 &&
                   (git_tree_entry_byname(tree, "b.txt") != NULL) == has_b,
                   "commit uses frozen contents, message and chosen timestamps");
done:
    git_blob_free(blob);
    git_tree_entry_free(entry);
    git_tree_free(tree);
    git_commit_free(commit);
    return result;
}

int main(void)
{
    char root[] = "/tmp/ghm-scheduler-test-XXXXXX";
    char repo_path[512], db_path[512];
    char first_oid[GHM_OID_HEX_CAPACITY] = {0};
    char second_oid[GHM_OID_HEX_CAPACITY] = {0};
    char third_oid[GHM_OID_HEX_CAPACITY] = {0};
    char manual_oid[GHM_OID_HEX_CAPACITY] = {0};
    char pushed_oid[GHM_OID_HEX_CAPACITY] = {0};
    char bare_path[512];
    git_repository *repository = NULL;
    git_repository *bare = NULL;
    git_remote *origin = NULL;
    git_reference *head = NULL, *remote_head = NULL;
    sqlite3 *db = NULL;
    GhmContext *context = NULL;
    GhmError error = {0};
    GhmScheduledJobList jobs = {0};
    int64_t first_id = 0, second_id = 0, third_id = 0;
    int64_t fourth_id = 0, fifth_id = 0, next_due = 0;
    const GhmCommitSignature signature = {"Test", "test@example.invalid", 1790000000, 330};
    int result = 1;
    if (mkdtemp(root) == NULL ||
        setenv("XDG_STATE_HOME", root, 1) != 0 ||
        snprintf(repo_path, sizeof(repo_path), "%s/repo", root) >= (int)sizeof(repo_path) ||
        snprintf(bare_path, sizeof(bare_path), "%s/remote.git", root) >= (int)sizeof(bare_path) ||
        snprintf(db_path, sizeof(db_path), "%s/data", root) >= (int)sizeof(db_path) ||
        ghm_context_open(db_path, &context, &error) != 0) goto done;
    if (git_repository_init(&repository, repo_path, 0) < 0 ||
        make_initial_commit(repository, repo_path) != 0 ||
        ghm_repo_register(context, repo_path, &error) != 0 ||
        write_text(repo_path, "a.txt", "first\n") != 0 ||
        ghm_schedule_add(context, repo_path, "first job", &signature, &signature,
                         1900000000, &first_id, &error) != 0 ||
        write_text(repo_path, "a.txt", "second\n") != 0 ||
        write_text(repo_path, "b.txt", "later\n") != 0 ||
        ghm_schedule_add(context, repo_path, "second job", &signature, &signature,
                         1900000100, &second_id, &error) != 0 ||
        check(first_id > 0 && second_id > first_id, "two distinct persistent jobs") != 0 ||
        ghm_schedule_next_due(context, &next_due, &error) != 0 ||
        check(next_due == 1900000000, "earliest job is selected") != 0 ||
        check(ghm_schedule_run_one_due(context, 1899999999, &error) == 0,
              "job does not run early") != 0 ||
        check(ghm_schedule_run_one_due(context, 1900000000, &error) == 1,
              "first job executes when due") != 0 ||
        check(ghm_schedule_run_one_due(context, 1900000000, &error) == 0,
              "second job waits for its own time") != 0 ||
        ghm_schedule_next_due(context, &next_due, &error) != 0 ||
        check(next_due == 1900000100, "next due time advances") != 0 ||
        ghm_schedule_run_one_due(context, 1900000100, &error) != 1 ||
        ghm_schedule_list(context, repo_path, &jobs, &error) != 0 ||
        check(jobs.count == 2 && strcmp(jobs.items[0].status, "COMPLETED") == 0 &&
              strcmp(jobs.items[1].status, "COMPLETED") == 0,
              "completed jobs remain visible in the persistent list") != 0) goto done;
    ghm_schedule_list_free(&jobs);
    char file_path[512];
    if (snprintf(file_path, sizeof(file_path), "%s/ghm.db", db_path) >= (int)sizeof(file_path) ||
        sqlite3_open_v2(file_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
        inspect_job(db, first_id, "COMPLETED", first_oid, sizeof(first_oid)) != 0 ||
        inspect_job(db, second_id, "COMPLETED", second_oid, sizeof(second_oid)) != 0 ||
        inspect_commit(repository, first_oid, "first job", "first\n", 0) != 0 ||
        inspect_commit(repository, second_oid, "second job", "second\n", 1) != 0) goto done;
    if (git_repository_init(&bare, bare_path, 1) < 0 ||
        git_remote_create(&origin, repository, "origin", bare_path) < 0 ||
        ghm_repo_push(repo_path, NULL, pushed_oid, &error) != 0 ||
        strcmp(pushed_oid, second_oid) != 0 ||
        write_text(repo_path, "a.txt", "third\n") != 0 ||
        ghm_schedule_add_options(context, repo_path, "third push", &signature, &signature,
                                 1900000200, 1, 1, &third_id, &error) != 0 ||
        ghm_schedule_run_one_due(context, 1900000200, &error) != 1 ||
        ghm_schedule_run_one_push(context, &error) != 1 ||
        inspect_job(db, third_id, "COMPLETED", third_oid, sizeof(third_oid)) != 0 ||
        git_repository_head(&head, repository) < 0 ||
        git_reference_lookup(&remote_head, bare, git_reference_name(head)) < 0 ||
        check(git_oid_streq(git_reference_target(remote_head), third_oid) == 0,
              "scheduled push sends exactly its frozen commit") != 0) goto done;
    git_reference_free(remote_head); remote_head = NULL;
    if (write_text(repo_path, "a.txt", "fourth\n") != 0 ||
        ghm_schedule_add_options(context, repo_path, "fourth push", &signature, &signature,
                                 1900000300, 1, 1, &fourth_id, &error) != 0 ||
        write_text(repo_path, "a.txt", "fifth\n") != 0 ||
        ghm_schedule_add(context, repo_path, "fifth", &signature, &signature,
                         1900000400, &fifth_id, &error) != 0 ||
        ghm_schedule_edit(context, fourth_id, "fourth edited", 1, &error) != 0 ||
        ghm_schedule_edit(context, fifth_id, "", 0, &error) == 0 ||
        check(ghm_schedule_cancel(context, fourth_id, &error) != 0,
              "cannot cancel a job with a pending dependent snapshot") != 0 ||
        check(ghm_schedule_reschedule(context, fourth_id, 1900000500, &error) != 0,
              "cannot reorder a job with pending dependents") != 0 ||
        ghm_schedule_cancel(context, fifth_id, &error) != 0 ||
        inspect_job(db, fifth_id, "CANCELLED", NULL, 0) != 0 ||
        ghm_schedule_reschedule(context, fourth_id, 1900000500, &error) != 0 ||
        ghm_schedule_next_due(context, &next_due, &error) != 0 ||
        check(next_due == 1900000500, "reschedule updates the worker deadline") != 0 ||
        ghm_schedule_run_one_due(context, 1900000500, &error) != 1 ||
        write_text(repo_path, "a.txt", "manual after scheduled\n") != 0 ||
        ghm_commit_now(repo_path, "manual", &signature, &signature, manual_oid, &error) != 0 ||
        ghm_schedule_run_one_push(context, &error) != 1 ||
        ghm_schedule_retry_push(context, fourth_id, &error) != 0 ||
        ghm_schedule_run_one_push(context, &error) != 1 ||
        git_reference_lookup(&remote_head, bare, git_reference_name(head)) < 0 ||
        check(git_oid_streq(git_reference_target(remote_head), third_oid) == 0,
              "a later local commit is never included in a scheduled push") != 0) goto done;
    ghm_schedule_list_free(&jobs);
    if (ghm_schedule_list(context, repo_path, &jobs, &error) != 0 ||
        check(jobs.count == 5 && strcmp(jobs.items[0].status, "CANCELLED") == 0 &&
              strcmp(jobs.items[1].push_status, "FAILED") == 0 &&
              strcmp(jobs.items[1].message, "fourth edited") == 0 &&
              strcmp(jobs.items[2].push_status, "COMPLETED") == 0,
              "push outcome and cancellation are persisted separately from commit status") != 0) goto done;
    result = 0;
done:
    if (result != 0) fprintf(stderr, "Scheduler test error: %s\n", error.message);
    ghm_schedule_list_free(&jobs);
    if (db != NULL) sqlite3_close(db);
    git_reference_free(remote_head);
    git_reference_free(head);
    git_remote_free(origin);
    git_repository_free(bare);
    git_repository_free(repository);
    ghm_context_close(context);
    if (remove_tree(root) != 0) fprintf(stderr, "Could not remove scheduler test directory\n");
    return result;
}
