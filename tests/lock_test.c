#include <ghm/branch.h>
#include <ghm/file_ops.h>
#include <ghm/remote.h>
#include <ghm/scheduler.h>
#include "core/lock.h"
#include "storage/database.h"

#include <dirent.h>
#include <fcntl.h>
#include <git2.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

static int write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    if (file == NULL) return -1;
    int wrote = fputs(text, file);
    return fclose(file) == 0 && wrote != EOF ? 0 : -1;
}

typedef struct { const char *path; int passed; } ThreadCheck;
static void *thread_try_lock(void *data)
{
    ThreadCheck *check = data;
    GhmRepoLock *lock = NULL;
    GhmError error = {0};
    check->passed = ghm_repo_lock_acquire(check->path, &lock, &error) != 0 && error.code == GHM_ERROR_BUSY;
    ghm_repo_lock_release(lock);
    return NULL;
}

static int unchanged(const char *path, const char *oid)
{
    GhmError error = {0};
    GhmStatus status = {0};
    git_repository *repository = NULL;
    git_reference *head = NULL;
    int result = ghm_repo_status(path, &status, &error) == 0 && status.count == 0 &&
        git_repository_open(&repository, path) == 0 && git_repository_head(&head, repository) == 0 &&
        git_oid_streq(git_reference_target(head), oid) == 0;
    ghm_status_free(&status);
    git_reference_free(head);
    git_repository_free(repository);
    return result;
}

static int competing_process(const char *data, const char *path, const char *oid, int64_t job_id)
{
    GhmContext *context = NULL;
    GhmError error = {0};
    char output[GHM_OID_HEX_CAPACITY];
    GhmMergeOutcome merge;
    GhmPullOutcome pull;
    GhmFileVersion version = {0};
    const GhmCommitSignature signature = {"Lock Test", "lock@example.invalid", 1790000000, 0};
    if (ghm_context_open(data, &context, &error) != 0) return 1;
#define BUSY(call) do { error = (GhmError){0}; if ((call) == 0 || error.code != GHM_ERROR_BUSY) { fprintf(stderr, "%s did not refuse contention\n", #call); goto failed; } } while (0)
    BUSY(ghm_repo_stage_all(path, &error));
    BUSY(ghm_repo_stage_path(path, "a.txt", &error));
    BUSY(ghm_repo_unstage_path(path, "a.txt", &error));
    BUSY(ghm_commit_now(path, "race", &signature, &signature, output, &error));
    BUSY(ghm_commit_staged(path, "race", &signature, &signature, output, &error));
    BUSY(ghm_commit_rewrite_head(context, path, oid, "race", 1790000001, 0, 1790000001, 0, 1, output, &error));
    BUSY(ghm_branch_create(path, "race", &error));
    BUSY(ghm_branch_checkout(context, path, "feature", &error));
    BUSY(ghm_branch_merge(context, path, "feature", &merge, output, &error));
    BUSY(ghm_repo_fetch(path, NULL, &error));
    BUSY(ghm_repo_pull(context, path, NULL, &pull, &error));
    BUSY(ghm_repo_push(path, NULL, output, &error));
    BUSY(ghm_file_create(path, "race.txt", &error));
    BUSY(ghm_directory_create(path, "race", &error));
    BUSY(ghm_file_save_text(path, "a.txt", "race", 4, &version, &version, &error));
    BUSY(ghm_file_delete(path, "a.txt", &error));
    BUSY(ghm_file_rename(path, "a.txt", "race.txt", &error));
    int64_t id;
    BUSY(ghm_schedule_add(context, path, "race", &signature, &signature, 1790000001, &id, &error));
    BUSY(ghm_schedule_cancel(context, job_id, &error));
    BUSY(ghm_schedule_reschedule(context, job_id, (int64_t)time(NULL) + 100, &error));
    BUSY(ghm_schedule_edit(context, job_id, "race", 0, &error));
    error = (GhmError){0};
    if (ghm_schedule_run_one_due(context, (int64_t)time(NULL) + 7200, &error) != 0 ||
        error.code != GHM_ERROR_BUSY) goto failed;
    if (sqlite3_exec(context->db, "UPDATE scheduled_jobs SET status='RUNNING'", NULL, NULL, NULL) != SQLITE_OK ||
        ghm_schedule_recover(context, (int64_t)time(NULL), &error) != 0) goto failed;
    GhmScheduledJobList jobs = {0};
    GhmRepositoryList repositories = {0};
    if (ghm_repo_list(context, &repositories, &error) != 0 || repositories.count != 1) {
        ghm_repo_list_free(&repositories);
        goto failed;
    }
    int listed = ghm_schedule_list(context, repositories.items[0].path, &jobs, &error);
    ghm_repo_list_free(&repositories);
    if (listed != 0) goto failed;
    int preserved = jobs.count == 1 && strcmp(jobs.items[0].status, "RUNNING") == 0;
    ghm_schedule_list_free(&jobs);
    if (!preserved || sqlite3_exec(context->db, "UPDATE scheduled_jobs SET status='PENDING'", NULL, NULL, NULL) != SQLITE_OK)
        goto failed;
    ghm_context_close(context);
    return 0;
failed:
    fprintf(stderr, "Competing process: %s\n", error.message);
    ghm_context_close(context);
    return 1;
#undef BUSY
}

int main(void)
{
    char root[] = "/tmp/ghm-lock-test-XXXXXX";
    char path[1024], data[1024], file[1024], alias[1024], lock_path[1024];
    char base[GHM_OID_HEX_CAPACITY], feature[GHM_OID_HEX_CAPACITY], output[GHM_OID_HEX_CAPACITY];
    GhmContext *context = NULL;
    GhmRepoLock *lock = NULL, *nested = NULL;
    GhmError error = {0};
    git_repository *repository = NULL;
    git_reference *head = NULL;
    char *branch = NULL;
    int64_t job_id;
    int result = 1, child_status;
    const GhmCommitSignature signature = {"Lock Test", "lock@example.invalid", 1790000000, 0};
    if (mkdtemp(root) == NULL || setenv("XDG_STATE_HOME", root, 1) != 0 ||
        snprintf(path, sizeof(path), "%s/repo", root) >= (int)sizeof(path) ||
        snprintf(data, sizeof(data), "%s/data", root) >= (int)sizeof(data) ||
        snprintf(file, sizeof(file), "%s/a.txt", path) >= (int)sizeof(file) ||
        snprintf(alias, sizeof(alias), "%s/alias", root) >= (int)sizeof(alias) ||
        ghm_context_open(data, &context, &error) != 0 || git_repository_init(&repository, path, 0) < 0 ||
        ghm_repo_register(context, path, &error) != 0 || write_text(file, "base\n") != 0 ||
        ghm_commit_now(path, "base", &signature, &signature, base, &error) != 0 ||
        git_repository_head(&head, repository) < 0) goto done;
    branch = strdup(git_reference_shorthand(head));
    if (branch == NULL || ghm_branch_create(path, "feature", &error) != 0 ||
        ghm_branch_checkout(context, path, "feature", &error) != 0 || write_text(file, "feature\n") != 0 ||
        ghm_commit_now(path, "feature", &signature, &signature, feature, &error) != 0 ||
        ghm_branch_checkout(context, path, branch, &error) != 0 || write_text(file, "snapshot\n") != 0 ||
        ghm_schedule_add(context, path, "snapshot", &signature, &signature,
                         (int64_t)time(NULL) + 3600, &job_id, &error) != 0 ||
        write_text(file, "base\n") != 0 || symlink(path, alias) != 0 ||
        ghm_repo_lock_acquire(path, &lock, &error) != 0 ||
        ghm_repo_lock_acquire(alias, &nested, &error) != 0 || nested != lock) goto done;
    ghm_repo_lock_release(nested); nested = NULL;
    pthread_t thread;
    ThreadCheck check = {alias, 0};
    if (pthread_create(&thread, NULL, thread_try_lock, &check) != 0) goto done;
    if (pthread_join(thread, NULL) != 0 || !check.passed) goto done;
    pid_t child = fork();
    if (child == 0) _exit(competing_process(data, alias, base, job_id));
    if (child < 0 || waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0 || !unchanged(path, base)) goto done;
    ghm_repo_lock_release(lock); lock = NULL;
    if (ghm_schedule_cancel(context, job_id, &error) != 0) goto done;
    /* A later pending job must not hide an abandoned RUNNING predecessor. */
    int64_t first_job, later_job, rejected_job;
    int64_t future = (int64_t)time(NULL) + 3600;
    if (write_text(file, "first snapshot\n") != 0 ||
        ghm_schedule_add(context, path, "first", &signature, &signature, future, &first_job, &error) != 0 ||
        write_text(file, "later snapshot\n") != 0 ||
        ghm_schedule_add(context, path, "later", &signature, &signature, future + 60, &later_job, &error) != 0)
        goto done;
    char update[128];
    (void)snprintf(update, sizeof(update), "UPDATE scheduled_jobs SET status='RUNNING' WHERE id=%lld", (long long)first_job);
    if (sqlite3_exec(context->db, update, NULL, NULL, NULL) != SQLITE_OK ||
        ghm_schedule_add(context, path, "must refuse", &signature, &signature, future + 120, &rejected_job, &error) == 0 ||
        strstr(error.message, "running") == NULL) goto done;
    (void)snprintf(update, sizeof(update), "UPDATE scheduled_jobs SET status='PENDING' WHERE id=%lld", (long long)first_job);
    if (sqlite3_exec(context->db, update, NULL, NULL, NULL) != SQLITE_OK ||
        ghm_schedule_cancel(context, later_job, &error) != 0 ||
        ghm_schedule_cancel(context, first_job, &error) != 0 || write_text(file, "base\n") != 0)
        goto done;
    /* Native Git lock contention must be detected before checkout changes files. */
    const char *native_names[] = {"HEAD.lock", "index.lock"};
    for (size_t i = 0; i < sizeof(native_names) / sizeof(*native_names); ++i) {
        if (snprintf(lock_path, sizeof(lock_path), "%s%s", git_repository_path(repository), native_names[i]) >= (int)sizeof(lock_path))
            goto done;
        int fd = open(lock_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) goto done;
        close(fd);
        GhmMergeOutcome outcome;
        int refused = ghm_branch_checkout(context, path, "feature", &error) != 0 && unchanged(path, base) &&
            ghm_branch_merge(context, path, "feature", &outcome, output, &error) != 0 && unchanged(path, base);
        if (unlink(lock_path) != 0 || !refused) goto done;
    }
    /* Process exit without an explicit release must not leave a stale lock. */
    child = fork();
    if (child == 0) { GhmRepoLock *abandoned = NULL; _exit(ghm_repo_lock_acquire(path, &abandoned, &error) != 0); }
    if (child < 0 || waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0 || ghm_repo_lock_acquire(path, &lock, &error) != 0) goto done;
    ghm_repo_lock_release(lock); lock = NULL;
    if (ghm_branch_checkout(context, path, "feature", &error) != 0 ||
        ghm_commit_rewrite_head(context, path, feature, "rewritten", 1790000001, 0,
                                1790000001, 0, 1, output, &error) != 0) goto done;
    result = 0;
done:
    if (result != 0) fprintf(stderr, "Lock test failed: %s\n", error.message);
    ghm_repo_lock_release(nested);
    ghm_repo_lock_release(lock);
    free(branch);
    git_reference_free(head);
    git_repository_free(repository);
    ghm_context_close(context);
    if (remove_tree(root) != 0) { fprintf(stderr, "Could not remove lock test directory\n"); result = 1; }
    return result;
}
