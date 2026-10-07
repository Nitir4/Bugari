/* Runs on both native platforms. All repositories, jobs and processes are
 * disposable; no GitHub account or installed worker is used. */
#include <ghm/branch.h>
#include <ghm/file_ops.h>
#include <ghm/scheduler.h>
#include <ghm/remote.h>
#include "core/lock.h"
#include "core/mutation.h"
#include "storage/database.h"
#include "fault_hooks.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <glib/gwin32.h>
#endif

#define CHECK(expression) do { if (!(expression)) { fprintf(stderr, "FAIL line %d: %s: %s\n", \
    __LINE__, #expression, error.message); goto done; } } while (0)
static const GhmCommitSignature signature = {"Portable Test", "portable@example.invalid", 4102444800LL, 330};

static int put(const char *root, const char *name, const char *text)
{
    char *path = g_build_filename(root, name, NULL);
    int ok = g_file_set_contents(path, text, -1, NULL);
    g_free(path); return ok;
}
static int remove_tree(const char *root)
{
    GDir *directory = g_dir_open(root, 0, NULL);
    if (directory == NULL) return g_remove(root);
    const char *name; int result = 0;
    while ((name = g_dir_read_name(directory)) != NULL) {
        char *path = g_build_filename(root, name, NULL);
        /* Test fixtures never contain directory links. */
        if (g_file_test(path, G_FILE_TEST_IS_DIR)) result |= remove_tree(path);
        else { g_chmod(path, 0600); result |= g_remove(path); }
        g_free(path);
    }
    g_dir_close(directory); return g_rmdir(root) | result;
}
static int blob_is(git_repository *repo, const char *name, const char *expected)
{
    git_reference *head = NULL; git_commit *commit = NULL; git_tree *tree = NULL;
    git_tree_entry *entry = NULL; git_blob *blob = NULL;
    int ok = git_repository_head(&head, repo) == 0 &&
        git_commit_lookup(&commit, repo, git_reference_target(head)) == 0 &&
        git_commit_tree(&tree, commit) == 0 && git_tree_entry_bypath(&entry, tree, name) == 0 &&
        git_blob_lookup(&blob, repo, git_tree_entry_id(entry)) == 0 &&
        git_blob_rawsize(blob) == strlen(expected) && memcmp(git_blob_rawcontent(blob), expected, strlen(expected)) == 0;
    git_blob_free(blob); git_tree_entry_free(entry); git_tree_free(tree);
    git_commit_free(commit); git_reference_free(head); return ok;
}
static int completed(GhmContext *context, const char *path, int64_t id)
{
    GhmScheduledJobList jobs = {0}; GhmError error = {0}; int ok = 0;
    if (ghm_schedule_list(context, path, &jobs, &error) != 0) return -1;
    for (size_t i = 0; i < jobs.count; ++i) if (jobs.items[i].id == id) {
        if (strcmp(jobs.items[i].status, "COMPLETED") == 0) ok = 1;
        else if (strcmp(jobs.items[i].status, "FAILED") == 0) ok = -1;
    }
    ghm_schedule_list_free(&jobs); return ok;
}
static int pushed(GhmContext *context, const char *path, int64_t id)
{
    GhmScheduledJobList jobs = {0}; GhmError error = {0}; int ok = 0;
    if (ghm_schedule_list(context, path, &jobs, &error) != 0) return -1;
    for (size_t i = 0; i < jobs.count; ++i) if (jobs.items[i].id == id)
        ok = strcmp(jobs.items[i].push_status, "COMPLETED") == 0;
    ghm_schedule_list_free(&jobs); return ok;
}
static int crash(const char *point, GhmError *error, void *payload)
{
    (void)error; (void)payload;
    if (strcmp(point, "commit.after_publish") == 0) _Exit(99);
    return 0;
}
static GSubprocess *spawn(const char *exe, const char *mode, const char *path)
{
    GError *error = NULL;
    GSubprocess *child = mode != NULL ? g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error, exe, mode, path, NULL) :
        g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error, exe, NULL);
    if (error != NULL) fprintf(stderr, "Cannot spawn test process: %s\n", error->message);
    g_clear_error(&error); return child;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    g_auto(GStrv) utf8_argv = g_win32_get_command_line();
    argc = (int)g_strv_length(utf8_argv); argv = utf8_argv;
#endif
    GhmError error = {0}; GhmRepoLock *lock = NULL;
    if (argc == 3 && strcmp(argv[1], "--lock-probe") == 0) {
        git_libgit2_init();
        int ok = ghm_repo_lock_acquire(argv[2], &lock, &error) != 0 && error.code == GHM_ERROR_BUSY;
        if (!ok) fprintf(stderr, "Lock probe failed: %d %s\n", error.code, error.message);
        ghm_repo_lock_release(lock); git_libgit2_shutdown(); return ok ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "--crash-commit") == 0) {
        git_libgit2_init(); char oid[GHM_OID_HEX_CAPACITY];
        ghm_test_fault_set(crash, NULL);
        (void)ghm_commit_now(argv[2], "crash publication", &signature, &signature, oid, &error);
        return 1;
    }
    if (argc != 2) return 1;
    int result = 1;
    char *root = g_dir_make_tmp("ghm-portable-XXXXXX", NULL);
    char *path = root != NULL ? g_build_filename(root, "repo space \xce\xa9", NULL) : NULL;
    char *alias = path != NULL ? g_build_filename(path, ".git", NULL) : NULL;
    char *bare_path = root != NULL ? g_build_filename(root, "remote.git", NULL) : NULL;
#ifdef _WIN32
    for (char *p = path; p != NULL && *p; ++p) if (*p == '\\') *p = '/';
#endif
    char *text = NULL; size_t length = 0;
    GhmFileVersion version, saved;
    GhmContext *context = NULL;
    git_repository *repo = NULL, *bare = NULL;
    git_config *config = NULL; git_index *index = NULL;
    GSubprocess *worker = NULL, *child = NULL;
    int64_t id = 0, next_id = 0;
    char oid[GHM_OID_HEX_CAPACITY]; git_oid before, after;
    CHECK(root != NULL && path != NULL && alias != NULL && bare_path != NULL);
    CHECK(g_setenv("XDG_DATA_HOME", root, TRUE) && g_setenv("XDG_STATE_HOME", root, TRUE));
    CHECK(ghm_context_open(NULL, &context, &error) == 0);
    CHECK(git_repository_init(&repo, path, 0) == 0 && git_repository_config(&config, repo) == 0);
    CHECK(git_config_set_bool(config, "core.autocrlf", 0) == 0);
    CHECK(put(path, "a.txt", "base\n") && put(path, "b.txt", "base b\n"));
    CHECK(ghm_commit_now(path, "initial", &signature, &signature, oid, &error) == 0);
    CHECK(ghm_repo_register(context, path, &error) == 0);
    CHECK(put(path, "a.txt", "staged\n") && ghm_repo_stage_path(path, "a.txt", &error) == 0);
    CHECK(ghm_repo_unstage_path(path, "a.txt", &error) == 0 && ghm_repo_stage_path(path, "a.txt", &error) == 0);
    CHECK(put(path, "a.txt", "unstaged later\n"));
    CHECK(ghm_commit_staged(path, "staged only", &signature, &signature, oid, &error) == 0);
    CHECK(blob_is(repo, "a.txt", "staged\n"));
    CHECK(ghm_file_read_text(path, "a.txt", &text, &length, &version, &error) == 0);
    free(text); text = NULL;
    CHECK(ghm_file_save_text(path, "a.txt", "saved\n", 6, &version, &saved, &error) == 0);
    CHECK(put(path, "a.txt", "changed by another editor\n"));
    CHECK(ghm_file_save_text(path, "a.txt", "stale\n", 6, &saved, &version, &error) != 0);
    CHECK(ghm_directory_create(path, "folder", &error) == 0);
    CHECK(ghm_file_create(path, "folder/\xce\xa9.txt", &error) == 0);
    CHECK(ghm_file_rename(path, "folder/\xce\xa9.txt", "folder/renamed.txt", &error) == 0);
    CHECK(ghm_file_create(path, "folder/existing.txt", &error) == 0);
    CHECK(ghm_file_rename(path, "folder/renamed.txt", "folder/existing.txt", &error) != 0);
    CHECK(ghm_file_delete(path, "folder/renamed.txt", &error) == 0);
    CHECK(ghm_file_create(path, "../escape", &error) != 0 && ghm_file_create(path, ".git/escape", &error) != 0);
#ifdef _WIN32
    CHECK(ghm_file_create(path, ".GIT/escape", &error) != 0 && ghm_file_create(path, "a.txt:stream", &error) != 0);
    CHECK(ghm_file_create(path, "CON.txt", &error) != 0 && ghm_file_create(path, "trailing.", &error) != 0);
#endif
    CHECK(ghm_repo_lock_acquire(path, &lock, &error) == 0);
    child = spawn(argv[0], "--lock-probe", alias);
    CHECK(child != NULL && g_subprocess_wait_check(child, NULL, NULL));
    g_clear_object(&child); ghm_repo_lock_release(lock); lock = NULL;
    CHECK(put(path, "a.txt", "crash content\n"));
    child = spawn(argv[0], "--crash-commit", path);
    CHECK(child != NULL && g_subprocess_wait(child, NULL, NULL) && g_subprocess_get_exit_status(child) == 99);
    g_clear_object(&child);
    CHECK(ghm_mutation_pending(repo));
    CHECK(ghm_repo_lock_acquire(path, &lock, &error) == 0);
    ghm_repo_lock_release(lock); lock = NULL;
    CHECK(!ghm_mutation_pending(repo) && blob_is(repo, "a.txt", "crash content\n"));
    CHECK(ghm_branch_create(path, "other", &error) == 0);
    CHECK(put(path, "a.txt", "frozen staged\n") && ghm_repo_stage_path(path, "a.txt", &error) == 0);
    CHECK(put(path, "b.txt", "not staged\n"));
    CHECK(git_repository_index(&index, repo) == 0 && git_index_write_tree(&before, index) == 0);
    int64_t due = (int64_t)time(NULL) + 60;
    CHECK(ghm_schedule_add_mode(context, path, "frozen staged", &signature, &signature, due, 0, &id, &error) == 0);
#ifdef _WIN32
    CHECK(completed(context, alias, id) == 0);
    GhmScheduledJobList alias_jobs = {0};
    CHECK(ghm_schedule_list(context, alias, &alias_jobs, &error) == 0);
    int alias_matches = alias_jobs.count == 1 && alias_jobs.items[0].id == id;
    ghm_schedule_list_free(&alias_jobs);
    CHECK(alias_matches);
#endif
    CHECK(git_index_read(index, 1) == 0 && git_index_write_tree(&after, index) == 0 && git_oid_equal(&before, &after));
    CHECK(ghm_branch_checkout(context, alias, "other", &error) != 0);
    CHECK(put(path, "a.txt", "frozen second\n"));
    CHECK(ghm_schedule_add(context, path, "chained all", &signature, &signature, due + 1, &next_id, &error) == 0);
    CHECK(ghm_schedule_cancel(context, id, &error) != 0);
    CHECK(put(path, "a.txt", "later edit must stay out\n"));
    CHECK(ghm_schedule_run_one_due(context, due, &error) == 1 && completed(context, path, id) == 1);
    CHECK(blob_is(repo, "a.txt", "frozen staged\n") && blob_is(repo, "b.txt", "base b\n"));
    CHECK(ghm_schedule_run_one_due(context, due + 1, &error) == 1 && completed(context, path, next_id) == 1);
    CHECK(blob_is(repo, "a.txt", "frozen second\n") && blob_is(repo, "b.txt", "not staged\n"));
    CHECK(ghm_schedule_run_one_due(context, due + 2, &error) == 0);
    CHECK(git_repository_init(&bare, bare_path, 1) == 0);
    git_remote *remote = NULL;
    CHECK(git_remote_create(&remote, repo, "origin", bare_path) == 0);
    git_remote_free(remote);
    CHECK(put(path, "a.txt", "worker frozen\n"));
    CHECK(ghm_schedule_add_options(context, path, "background", &signature, &signature,
        (int64_t)time(NULL) + 2, 1, 1, &id, &error) == 0);
    CHECK(put(path, "a.txt", "after worker scheduling\n"));
    worker = spawn(argv[1], NULL, NULL);
    CHECK(worker != NULL);
    for (int attempt = 0; attempt < 150 && completed(context, path, id) == 0; ++attempt) g_usleep(100000);
    CHECK(completed(context, path, id) == 1 && blob_is(repo, "a.txt", "worker frozen\n"));
    for (int attempt = 0; attempt < 100 && pushed(context, path, id) == 0; ++attempt) g_usleep(100000);
    CHECK(pushed(context, path, id) == 1);
    git_reference *local_head = NULL, *remote_head = NULL;
    CHECK(git_repository_head(&local_head, repo) == 0);
    CHECK(git_reference_lookup(&remote_head, bare, git_reference_name(local_head)) == 0 &&
        git_oid_equal(git_reference_target(local_head), git_reference_target(remote_head)));
    git_reference_free(local_head); git_reference_free(remote_head);
    /* SQLite finish failure after commit publication must recover that exact
     * commit rather than creating another one. */
    g_subprocess_force_exit(worker); CHECK(g_subprocess_wait(worker, NULL, NULL)); g_clear_object(&worker);
    CHECK(put(path, "a.txt", "finish recovery\n"));
    CHECK(ghm_schedule_add(context, path, "recover once", &signature, &signature, due + 120, &id, &error) == 0);
    CHECK(sqlite3_exec(context->db, "CREATE TRIGGER deny_finish BEFORE UPDATE OF status ON scheduled_jobs WHEN NEW.status='COMPLETED' BEGIN SELECT RAISE(FAIL,'test finish'); END", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(ghm_schedule_run_one_due(context, due + 120, &error) == -1);
    CHECK(sqlite3_exec(context->db, "DROP TRIGGER deny_finish", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(ghm_schedule_recover(context, due + 121, &error) == 0 && completed(context, path, id) == 1);
    CHECK(ghm_schedule_run_one_due(context, due + 122, &error) == 0);
    puts("PASS portable staging, safe editor, Unicode paths, process locks, interrupted publication, frozen/chained jobs, worker and finish recovery");
    result = 0;
done:
    if (worker != NULL) { g_subprocess_force_exit(worker); g_subprocess_wait(worker, NULL, NULL); g_object_unref(worker); }
    if (child != NULL) { g_subprocess_force_exit(child); g_subprocess_wait(child, NULL, NULL); g_object_unref(child); }
    ghm_repo_lock_release(lock); git_index_free(index); git_config_free(config);
    git_repository_free(bare); git_repository_free(repo); ghm_context_close(context); free(text);
    if (root != NULL) remove_tree(root);
    g_free(alias); g_free(path); g_free(bare_path); g_free(root); return result;
}
