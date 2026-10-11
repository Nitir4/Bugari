#include <ghm/scheduler.h>
#include <ghm/remote.h>
#include <ghm/log.h>
#include "core/git.h"
#include "core/lock.h"
#include "core/storage_check.h"
#include "core/fault.h"
#include "core/commit_private.h"
#include "storage/database.h"

#include <git2.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "platform/io.h"

typedef struct {
    int64_t id;
    char *path;
    char *branch;
    char *message;
    char *snapshot_ref;
    char *snapshot_tree_oid;
    char *base_parent_oid;
    char *predecessor_oid;
    char *author_name;
    char *author_email;
    char *committer_name;
    char *committer_email;
    int64_t author_timestamp;
    int64_t committer_timestamp;
    int author_offset;
    int committer_offset;
    int64_t predecessor_id;
    char *predecessor_status;
} DueJob;

static void database_error(GhmContext *context, GhmError *error)
{
    ghm_error_set(error, GHM_ERROR_DATABASE, sqlite3_errmsg(context->db));
}

static int bind_text(sqlite3_stmt *stmt, int index, const char *value)
{
    return sqlite3_bind_text(stmt, index, value, -1, SQLITE_TRANSIENT);
}

static char *column_copy(sqlite3_stmt *stmt, int index)
{
    const unsigned char *value = sqlite3_column_text(stmt, index);
    return value != NULL ? strdup((const char *)value) : NULL;
}


/* Resolve the job before taking its repository lock; the mutating SQL below
 * rechecks the status after acquisition. Never wait on a repository under SQLite. */
static int lock_job_repository(GhmContext *context, int64_t job_id,
                                GhmRepoLock **out, GhmError *error)
{
    sqlite3_stmt *stmt = NULL;
    char *path = NULL;
    int result = -1, step;
    if (context == NULL || job_id <= 0) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Valid context and job ID are required");
        return -1;
    }
    pthread_mutex_lock(&context->database_mutex);
    if (sqlite3_prepare_v2(context->db,
        "SELECT r.path FROM scheduled_jobs j JOIN repositories r ON r.id=j.repository_id WHERE j.id=?1",
        -1, &stmt, NULL) != SQLITE_OK || sqlite3_bind_int64(stmt, 1, job_id) != SQLITE_OK)
        database_error(context, error);
    else if ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        path = column_copy(stmt, 0);
        if (path == NULL) ghm_error_set(error, GHM_ERROR_MEMORY, "Cannot read job repository");
    } else if (step == SQLITE_DONE)
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Scheduled job no longer exists");
    else database_error(context, error);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&context->database_mutex);
    if (path != NULL) result = ghm_repo_lock_acquire(path, out, error);
    free(path);
    return result;
}

static void job_free(DueJob *job)
{
    free(job->path);
    free(job->branch);
    free(job->message);
    free(job->snapshot_ref);
    free(job->snapshot_tree_oid);
    free(job->base_parent_oid);
    free(job->predecessor_oid);
    free(job->author_name);
    free(job->author_email);
    free(job->committer_name);
    free(job->committer_email);
    free(job->predecessor_status);
    memset(job, 0, sizeof(*job));
}

void ghm_schedule_list_free(GhmScheduledJobList *list)
{
    if (list == NULL) return;
    for (size_t i = 0; i < list->count; ++i) {
        free(list->items[i].branch);
        free(list->items[i].message);
        free(list->items[i].status);
        free(list->items[i].error_message);
        free(list->items[i].result_oid);
        free(list->items[i].push_status);
    }
    free(list->items);
    *list = (GhmScheduledJobList){0};
}

int ghm_schedule_list(GhmContext *context, const char *repository_path,
                      GhmScheduledJobList *out, GhmError *error)
{
    static const char sql[] =
        "SELECT j.id,j.branch,j.message,j.status,j.error_message,j.result_oid,j.execute_at,"
        "j.push_after_commit,j.push_status,j.push_retry_at,j.push_attempts "
        "FROM scheduled_jobs j JOIN repositories r ON r.id=j.repository_id "
        "WHERE r.path=?1 ORDER BY j.id DESC LIMIT 100";
    sqlite3_stmt *stmt = NULL;
    int result = -1;
    int step;
    if (context == NULL || repository_path == NULL || out == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Context, repository and output are required");
        return -1;
    }
    *out = (GhmScheduledJobList){0};
#ifdef _WIN32
    /* libgit2 expands Windows short names during registration. Resolve input
     * with the same API before comparing it with the stored worktree root. */
    char *normalized = NULL;
    if (ghm_repo_find(repository_path, &normalized, error) != 0) return -1;
    repository_path = normalized;
#endif
    pthread_mutex_lock(&context->database_mutex);
    if (sqlite3_prepare_v2(context->db, sql, -1, &stmt, NULL) != SQLITE_OK ||
        bind_text(stmt, 1, repository_path) != SQLITE_OK) goto db_fail;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        GhmScheduledJob *grown = realloc(out->items, (out->count + 1) * sizeof(*grown));
        if (grown == NULL) {
            ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
            goto done;
        }
        out->items = grown;
        GhmScheduledJob *item = &out->items[out->count];
        *item = (GhmScheduledJob){0};
        ++out->count;
        item->id = sqlite3_column_int64(stmt, 0);
        item->branch = column_copy(stmt, 1);
        item->message = column_copy(stmt, 2);
        item->status = column_copy(stmt, 3);
        item->error_message = column_copy(stmt, 4);
        item->result_oid = column_copy(stmt, 5);
        item->execute_at = sqlite3_column_int64(stmt, 6);
        item->push_after_commit = sqlite3_column_int(stmt, 7);
        item->push_status = column_copy(stmt, 8);
        item->push_retry_at = sqlite3_column_int64(stmt, 9);
        item->push_attempts = sqlite3_column_int(stmt, 10);
        if (item->branch == NULL || item->message == NULL || item->status == NULL ||
            item->push_status == NULL) {
            ghm_error_set(error, GHM_ERROR_MEMORY, "Cannot read scheduled job");
            goto done;
        }
    }
    if (step != SQLITE_DONE) goto db_fail;
    result = 0;
    goto done;
db_fail:
    database_error(context, error);
done:
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&context->database_mutex);
    if (result != 0) ghm_schedule_list_free(out);
#ifdef _WIN32
    free(normalized);
#endif
    return result;
}

static int valid_signature(const GhmCommitSignature *signature)
{
    return signature != NULL && signature->name != NULL && signature->name[0] != '\0' &&
           signature->email != NULL && signature->email[0] != '\0' &&
           signature->timestamp >= 0 && signature->timestamp <= INT64_MAX &&
           signature->offset_minutes >= -14 * 60 && signature->offset_minutes <= 14 * 60;
}

static int repository_head(const char *path, char **out_branch,
                           char out_oid[GHM_OID_HEX_CAPACITY], GhmError *error)
{
    git_repository *repository = NULL;
    git_reference *head = NULL;
    int result = -1;
    if (git_repository_open(&repository, path) < 0) {
        ghm_error_from_git(error, "Open scheduled repository");
        goto done;
    }
    if (git_repository_is_bare(repository) || git_repository_state(repository) != GIT_REPOSITORY_STATE_NONE ||
        git_repository_head_detached(repository)) {
        ghm_error_set(error, GHM_ERROR_GIT, "A checked-out branch with no ongoing Git operation is required");
        goto done;
    }
    if (git_repository_head(&head, repository) < 0 || git_reference_target(head) == NULL) {
        ghm_error_from_git(error, "Read scheduled branch");
        goto done;
    }
    *out_branch = strdup(git_reference_shorthand(head));
    if (*out_branch == NULL) {
        ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
        goto done;
    }
    (void)git_oid_tostr(out_oid, GHM_OID_HEX_CAPACITY, git_reference_target(head));
    result = 0;
done:
    git_reference_free(head);
    git_repository_free(repository);
    return result;
}

static void make_snapshot_ref(char result[sizeof("refs/ghm/jobs/") + 32])
{
    unsigned char random_bytes[16];
    static const char hex[] = "0123456789abcdef";
    static const char prefix[] = "refs/ghm/jobs/";
    sqlite3_randomness((int)sizeof(random_bytes), random_bytes);
    memcpy(result, prefix, sizeof(prefix) - 1);
    for (size_t i = 0; i < sizeof(random_bytes); ++i) {
        result[sizeof(prefix) - 1 + i * 2] = hex[random_bytes[i] >> 4];
        result[sizeof(prefix) + i * 2] = hex[random_bytes[i] & 15];
    }
    result[sizeof(prefix) - 1 + sizeof(random_bytes) * 2] = '\0';
}

static int ghm_schedule_add_options_unlocked(GhmContext *context, const char *repository_path,
                             const char *message, const GhmCommitSignature *author,
                             const GhmCommitSignature *committer, int64_t execute_at,
                             int stage_all, int push_after_commit,
                             int64_t *out_job_id, GhmError *error)
{
    static const char repository_sql[] = "SELECT id FROM repositories WHERE path=?1";
    static const char previous_sql[] =
        "SELECT id,execute_at,snapshot_tree_oid,base_parent_oid,status FROM scheduled_jobs "
        "WHERE repository_id=?1 AND branch=?2 AND status IN ('PENDING','RUNNING') "
        "ORDER BY (status='RUNNING') DESC,execute_at DESC,id DESC LIMIT 1";
    static const char insert_sql[] =
        "INSERT INTO scheduled_jobs(repository_id,branch,message,author_timestamp,committer_timestamp,"
        "execute_at,snapshot_ref,snapshot_tree_oid,base_parent_oid,predecessor_job_id,"
        "author_name,author_email,author_offset,committer_name,committer_email,committer_offset,"
        "push_after_commit) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17)";
    sqlite3_stmt *stmt = NULL;
    char *branch = NULL;
    char *previous_tree = NULL;
    char *previous_base = NULL;
    char head_oid[GHM_OID_HEX_CAPACITY] = {0};
    char tree_oid[GHM_OID_HEX_CAPACITY] = {0};
    char snapshot_ref[sizeof("refs/ghm/jobs/") + 32];
    int64_t repository_id = 0;
    int64_t predecessor_id = 0;
    int64_t inserted_id = 0;
    int transaction = 0;
    int captured = 0;
    int result = -1;
    int step;
    if (context == NULL || repository_path == NULL || repository_path[0] == '\0' ||
        message == NULL || message[0] == '\0' || !valid_signature(author) ||
        !valid_signature(committer) || execute_at <= 0 || out_job_id == NULL ||
        (push_after_commit != 0 && push_after_commit != 1)) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Repository, message, identities and execution time are required");
        return -1;
    }
    *out_job_id = 0;
    struct stat database_stat;
    char *database_path = sqlite3_db_filename(context->db, "main") != NULL ?
                          strdup(sqlite3_db_filename(context->db, "main")) : NULL;
    uint64_t database_bytes = 0;
    if (database_path != NULL && stat(database_path, &database_stat) == 0 && database_stat.st_size > 0)
        database_bytes = (uint64_t)database_stat.st_size * 2U;
    free(database_path);
    if (ghm_storage_check(context->data_directory, database_bytes, error) != 0 ||
        ghm_repository_storage_check(repository_path, error) != 0) goto done_unlocked;
    if (repository_head(repository_path, &branch, head_oid, error) != 0) goto done_unlocked;
    make_snapshot_ref(snapshot_ref);
    pthread_mutex_lock(&context->database_mutex);
    if (sqlite3_exec(context->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) goto db_fail;
    transaction = 1;
    if (sqlite3_prepare_v2(context->db, repository_sql, -1, &stmt, NULL) != SQLITE_OK ||
        bind_text(stmt, 1, repository_path) != SQLITE_OK) goto db_fail;
    step = sqlite3_step(stmt);
    if (step != SQLITE_ROW) {
        if (step == SQLITE_DONE) ghm_error_set(error, GHM_ERROR_ARGUMENT, "Register the repository before scheduling");
        else database_error(context, error);
        goto done_locked;
    }
    repository_id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt); stmt = NULL;
    if (sqlite3_prepare_v2(context->db, previous_sql, -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 1, repository_id) != SQLITE_OK ||
        bind_text(stmt, 2, branch) != SQLITE_OK) goto db_fail;
    step = sqlite3_step(stmt);
    if (step == SQLITE_ROW) {
        if (strcmp((const char *)sqlite3_column_text(stmt, 4), "RUNNING") == 0) {
            ghm_error_set(error, GHM_ERROR_BUSY, "A running scheduled job needs to finish or be recovered first");
            goto done_locked;
        }
        predecessor_id = sqlite3_column_int64(stmt, 0);
        if (execute_at <= sqlite3_column_int64(stmt, 1)) {
            ghm_error_set(error, GHM_ERROR_ARGUMENT, "Schedule this branch after its existing pending job");
            goto done_locked;
        }
        previous_tree = column_copy(stmt, 2);
        previous_base = column_copy(stmt, 3);
        if (previous_tree == NULL || previous_base == NULL) {
            ghm_error_set(error, GHM_ERROR_MEMORY, "Missing previous snapshot or out of memory");
            goto done_locked;
        }
        if (strcmp(previous_base, head_oid) != 0) {
            ghm_error_set(error, GHM_ERROR_GIT, "Branch changed since the previous job was scheduled");
            goto done_locked;
        }
    } else if (step != SQLITE_DONE) goto db_fail;
    sqlite3_finalize(stmt); stmt = NULL;
    if (ghm_snapshot_capture_mode(repository_path, previous_tree, snapshot_ref,
                                  stage_all, tree_oid, error) != 0) goto done_locked;
    captured = 1;
    if (ghm_fault("scheduler.before_insert", error) != 0) goto done_locked;
    if (sqlite3_prepare_v2(context->db, insert_sql, -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 1, repository_id) != SQLITE_OK ||
        bind_text(stmt, 2, branch) != SQLITE_OK || bind_text(stmt, 3, message) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 4, author->timestamp) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 5, committer->timestamp) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 6, execute_at) != SQLITE_OK ||
        bind_text(stmt, 7, snapshot_ref) != SQLITE_OK || bind_text(stmt, 8, tree_oid) != SQLITE_OK ||
        bind_text(stmt, 9, previous_base != NULL ? previous_base : head_oid) != SQLITE_OK ||
        (predecessor_id != 0 && sqlite3_bind_int64(stmt, 10, predecessor_id) != SQLITE_OK) ||
        bind_text(stmt, 11, author->name) != SQLITE_OK ||
        bind_text(stmt, 12, author->email) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 13, author->offset_minutes) != SQLITE_OK ||
        bind_text(stmt, 14, committer->name) != SQLITE_OK ||
        bind_text(stmt, 15, committer->email) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 16, committer->offset_minutes) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 17, push_after_commit) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_DONE) goto db_fail;
    inserted_id = sqlite3_last_insert_rowid(context->db);
    sqlite3_finalize(stmt); stmt = NULL;
    if (sqlite3_exec(context->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) goto db_fail;
    transaction = 0;
    *out_job_id = inserted_id;
    result = 0;
    goto done_locked;
db_fail:
    database_error(context, error);
done_locked:
    sqlite3_finalize(stmt);
    if (transaction) (void)sqlite3_exec(context->db, "ROLLBACK", NULL, NULL, NULL);
    pthread_mutex_unlock(&context->database_mutex);
    if (result != 0 && captured) {
        GhmError ignored = {0};
        (void)ghm_snapshot_release(repository_path, snapshot_ref, tree_oid, &ignored);
    }
done_unlocked:
    free(branch);
    free(previous_tree);
    free(previous_base);
    return result;
}

int ghm_schedule_add_mode(GhmContext *context, const char *repository_path,
                          const char *message, const GhmCommitSignature *author,
                          const GhmCommitSignature *committer, int64_t execute_at,
                          int stage_all, int64_t *out_job_id, GhmError *error)
{
    return ghm_schedule_add_options(context, repository_path, message, author, committer,
                                    execute_at, stage_all, 0, out_job_id, error);
}

int ghm_schedule_add(GhmContext *context, const char *repository_path,
                     const char *message, const GhmCommitSignature *author,
                     const GhmCommitSignature *committer, int64_t execute_at,
                     int64_t *out_job_id, GhmError *error)
{
    return ghm_schedule_add_mode(context, repository_path, message, author, committer,
                                 execute_at, 1, out_job_id, error);
}

int ghm_schedule_recover(GhmContext *context, int64_t now, GhmError *error)
{
    sqlite3_stmt *select = NULL, *update = NULL;
    int result = -1, step;
    if (context == NULL) { ghm_error_set(error, GHM_ERROR_ARGUMENT, "Context is required"); return -1; }
    pthread_mutex_lock(&context->database_mutex);
    if (sqlite3_exec(context->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) goto done;
    if (sqlite3_prepare_v2(context->db,
        "SELECT j.id,r.path,j.branch,j.result_oid,j.status FROM scheduled_jobs j JOIN repositories r ON r.id=j.repository_id "
        "WHERE j.status='RUNNING' OR j.push_status='RUNNING' ORDER BY j.id",
        -1, &select, NULL) != SQLITE_OK) goto done;
    while ((step = sqlite3_step(select)) == SQLITE_ROW) {
        GhmRepoLock *lock = NULL;
        GhmError lock_error = {0};
        const char *path = (const char *)sqlite3_column_text(select, 1);
        if (ghm_repo_lock_acquire(path, &lock, &lock_error) != 0 && lock_error.code == GHM_ERROR_BUSY) {
            if (error != NULL) *error = lock_error;
            continue; /* A live owner is still committing/pushing; never recover its job. */
        }
        int published = 0, needs_inspection = 0;
        const char *planned = (const char *)sqlite3_column_text(select, 3);
        const char *status = (const char *)sqlite3_column_text(select, 4);
        if (lock != NULL && planned != NULL && strcmp(status, "RUNNING") == 0) {
            git_repository *repo = NULL;
            git_reference *branch = NULL;
            if (git_repository_open(&repo, path) == 0 &&
                git_branch_lookup(&branch, repo, (const char *)sqlite3_column_text(select, 2), GIT_BRANCH_LOCAL) == 0 &&
                git_reference_target(branch) != NULL && git_oid_streq(git_reference_target(branch), planned) == 0) {
                published = 1;
                git_index *index = NULL;
                if (git_repository_index(&index, repo) < 0) needs_inspection = 1;
                else {
                    const char *index_path = git_index_path(index);
                    char *native_lock = index_path != NULL ? malloc(strlen(index_path) + sizeof(".lock")) : NULL;
                    if (native_lock == NULL) needs_inspection = 1;
                    else {
                        (void)snprintf(native_lock, strlen(index_path) + sizeof(".lock"), "%s.lock", index_path);
                        struct stat metadata;
                        needs_inspection = lstat(native_lock, &metadata) == 0;
                        free(native_lock);
                    }
                }
                git_index_free(index);
            }
            git_reference_free(branch); git_repository_free(repo);
        }
        if (sqlite3_prepare_v2(context->db,
            "UPDATE scheduled_jobs SET "
            "status=CASE WHEN status='RUNNING' THEN CASE WHEN ?3=1 THEN 'COMPLETED' ELSE 'FAILED' END ELSE status END,"
            "completed_at=CASE WHEN status='RUNNING' THEN ?2 ELSE completed_at END,"
            "result_oid=CASE WHEN status='RUNNING' AND ?3=0 THEN NULL ELSE result_oid END,"
            "error_message=CASE WHEN status='RUNNING' THEN CASE WHEN ?3=0 THEN "
            "'Worker stopped before completion; inspect native Git locks and branch before rescheduling' "
            "WHEN ?4=1 THEN 'Commit recovered; index.lock remains after interruption. Inspect the index before Retry Push' "
            "ELSE NULL END ELSE error_message END,"
            "push_status=CASE WHEN status='RUNNING' AND ?3=1 AND push_after_commit=1 THEN "
            "CASE WHEN ?4=1 THEN 'FAILED' ELSE 'PENDING' END "
            "WHEN push_status='RUNNING' THEN 'PENDING' ELSE push_status END WHERE id=?1",
            -1, &update, NULL) != SQLITE_OK ||
            sqlite3_bind_int64(update, 1, sqlite3_column_int64(select, 0)) != SQLITE_OK ||
            sqlite3_bind_int64(update, 2, now) != SQLITE_OK || sqlite3_bind_int(update, 3, published) != SQLITE_OK ||
            sqlite3_bind_int(update, 4, needs_inspection) != SQLITE_OK || sqlite3_step(update) != SQLITE_DONE) {
            ghm_repo_lock_release(lock);
            goto done;
        }
        sqlite3_finalize(update); update = NULL;
        ghm_repo_lock_release(lock);
    }
    if (step != SQLITE_DONE ||
        sqlite3_exec(context->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) goto done;
    result = 0;
done:
    if (result != 0) {
        database_error(context, error);
        (void)sqlite3_exec(context->db, "ROLLBACK", NULL, NULL, NULL);
    }
    sqlite3_finalize(update);
    sqlite3_finalize(select);
    pthread_mutex_unlock(&context->database_mutex);
    return result;
}

static int load_due_job(sqlite3_stmt *stmt, DueJob *job)
{
    job->id = sqlite3_column_int64(stmt, 0);
    job->path = column_copy(stmt, 1);
    job->branch = column_copy(stmt, 2);
    job->message = column_copy(stmt, 3);
    job->snapshot_ref = column_copy(stmt, 4);
    job->snapshot_tree_oid = column_copy(stmt, 5);
    job->base_parent_oid = column_copy(stmt, 6);
    job->predecessor_id = sqlite3_column_int64(stmt, 7);
    job->predecessor_status = column_copy(stmt, 8);
    job->predecessor_oid = column_copy(stmt, 9);
    job->author_name = column_copy(stmt, 10);
    job->author_email = column_copy(stmt, 11);
    job->author_timestamp = sqlite3_column_int64(stmt, 12);
    job->author_offset = sqlite3_column_int(stmt, 13);
    job->committer_name = column_copy(stmt, 14);
    job->committer_email = column_copy(stmt, 15);
    job->committer_timestamp = sqlite3_column_int64(stmt, 16);
    job->committer_offset = sqlite3_column_int(stmt, 17);
    return job->path != NULL && job->branch != NULL && job->message != NULL &&
           job->snapshot_ref != NULL && job->snapshot_tree_oid != NULL &&
           job->base_parent_oid != NULL && job->author_name != NULL &&
           job->author_email != NULL && job->committer_name != NULL && job->committer_email != NULL &&
           (job->predecessor_id == 0 || job->predecessor_status != NULL);
}

typedef struct { GhmContext *context; int64_t job_id; } PreparedJob;
static int record_prepared_commit(const char *oid, void *payload, GhmError *error)
{
    PreparedJob *job = payload;
    sqlite3_stmt *stmt = NULL;
    int result = -1;
    pthread_mutex_lock(&job->context->database_mutex);
    if (sqlite3_prepare_v2(job->context->db,
        "UPDATE scheduled_jobs SET result_oid=?2 WHERE id=?1 AND status='RUNNING'",
        -1, &stmt, NULL) == SQLITE_OK && sqlite3_bind_int64(stmt, 1, job->job_id) == SQLITE_OK &&
        bind_text(stmt, 2, oid) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_DONE &&
        sqlite3_changes(job->context->db) == 1) result = 0;
    else database_error(job->context, error);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&job->context->database_mutex);
    return result;
}

int ghm_schedule_run_one_due(GhmContext *context, int64_t now, GhmError *error)
{
    static const char select_sql[] =
        "SELECT j.id,r.path,j.branch,j.message,j.snapshot_ref,j.snapshot_tree_oid,j.base_parent_oid,"
        "j.predecessor_job_id,p.status,p.result_oid,j.author_name,j.author_email,j.author_timestamp,"
        "j.author_offset,j.committer_name,j.committer_email,j.committer_timestamp,j.committer_offset,"
        "j.push_after_commit "
        "FROM scheduled_jobs j JOIN repositories r ON r.id=j.repository_id "
        "LEFT JOIN scheduled_jobs p ON p.id=j.predecessor_job_id "
        "WHERE j.status='PENDING' AND j.execute_at<=?1 ORDER BY j.execute_at,j.id";
    static const char finish_sql[] =
        "UPDATE scheduled_jobs SET status=?2,completed_at=?3,result_oid=?4,error_message=?5,"
        "push_status=CASE WHEN ?6=1 AND ?2='COMPLETED' THEN 'PENDING' ELSE 'NONE' END "
        "WHERE id=?1 AND status='RUNNING'";
    DueJob job = {0};
    GhmRepoLock *operation_lock = NULL;
    sqlite3_stmt *stmt = NULL;
    char created_oid[GHM_OID_HEX_CAPACITY] = {0};
    GhmError job_error = {0};
    GhmCommitSignature author, committer;
    int transaction = 0;
    int claimed = 0;
    int committed = 0;
    int push_after_commit = 0;
    int step;
    int result = -1;
    if (context == NULL) { ghm_error_set(error, GHM_ERROR_ARGUMENT, "Context is required"); return -1; }
    pthread_mutex_lock(&context->database_mutex);
    if (sqlite3_exec(context->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) goto db_fail;
    transaction = 1;
    if (sqlite3_prepare_v2(context->db, select_sql, -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 1, now) != SQLITE_OK) goto db_fail;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (!load_due_job(stmt, &job)) {
            ghm_error_set(error, GHM_ERROR_MEMORY, "Cannot load scheduled job");
            goto done_locked;
        }
        /* This try-lock never waits while holding SQLite. Busy jobs remain
         * PENDING; continue so an unrelated repository can still execute. */
        if (ghm_repo_lock_acquire(job.path, &operation_lock, &job_error) == 0 ||
            job_error.code != GHM_ERROR_BUSY) break;
        if (error != NULL) *error = job_error;
        job_free(&job);
        job_error = (GhmError){0};
    }
    if (step == SQLITE_DONE) { result = 0; goto done_locked; }
    if (step != SQLITE_ROW) goto db_fail;
    /* A scheduled push is initiated only after its local commit is recorded. */
    push_after_commit = sqlite3_column_int(stmt, 18);
    sqlite3_finalize(stmt); stmt = NULL;
    if (sqlite3_prepare_v2(context->db,
            "UPDATE scheduled_jobs SET status='RUNNING' WHERE id=?1 AND status='PENDING'",
            -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 1, job.id) != SQLITE_OK || sqlite3_step(stmt) != SQLITE_DONE ||
        sqlite3_changes(context->db) != 1) goto db_fail;
    sqlite3_finalize(stmt); stmt = NULL;
    if (sqlite3_exec(context->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) goto db_fail;
    transaction = 0;
    claimed = 1;
    goto done_locked;
db_fail:
    database_error(context, error);
done_locked:
    sqlite3_finalize(stmt); stmt = NULL;
    if (transaction) (void)sqlite3_exec(context->db, "ROLLBACK", NULL, NULL, NULL);
    pthread_mutex_unlock(&context->database_mutex);
    if (!claimed) { ghm_repo_lock_release(operation_lock); job_free(&job); return result; }

    author = (GhmCommitSignature){job.author_name, job.author_email, job.author_timestamp, job.author_offset};
    committer = (GhmCommitSignature){job.committer_name, job.committer_email,
                                     job.committer_timestamp, job.committer_offset};
    if (job.predecessor_id != 0 &&
        (strcmp(job.predecessor_status, "COMPLETED") != 0 || job.predecessor_oid == NULL)) {
        ghm_error_set(&job_error, GHM_ERROR_GIT, "Earlier scheduled commit did not complete; review and reschedule");
    } else if (job_error.code == GHM_OK) {
        const char *parent = job.predecessor_id != 0 ? job.predecessor_oid : job.base_parent_oid;
        PreparedJob prepared = {context, job.id};
        committed = ghm_commit_snapshot_prepared(job.path, job.snapshot_ref, job.branch, parent,
            job.message, &author, &committer, record_prepared_commit, &prepared, created_oid, &job_error) == 0;
        /* A published commit remains completed even if later index sync failed.
         * Preserve the warning and require inspection before any automatic push. */
        if (!committed && created_oid[0] != '\0') { committed = 1; push_after_commit = 0; }
    }
    if (ghm_fault("scheduler.before_finish", error) != 0) { result = -1; goto finish_done; }
    pthread_mutex_lock(&context->database_mutex);
    if (sqlite3_prepare_v2(context->db, finish_sql, -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 1, job.id) != SQLITE_OK ||
        bind_text(stmt, 2, committed ? "COMPLETED" : "FAILED") != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, (int64_t)time(NULL)) != SQLITE_OK ||
        (created_oid[0] != '\0' && bind_text(stmt, 4, created_oid) != SQLITE_OK) ||
        (job_error.code != GHM_OK && bind_text(stmt, 5, job_error.message) != SQLITE_OK) ||
        sqlite3_bind_int(stmt, 6, push_after_commit) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_DONE || sqlite3_changes(context->db) != 1) {
        database_error(context, error);
        result = -1;
    } else result = 1;
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&context->database_mutex);
    if (committed && result == 1) {
        GhmError ignored = {0};
        (void)ghm_snapshot_release(job.path, job.snapshot_ref, job.snapshot_tree_oid, &ignored);
    }
    if (result == 1) {
        char detail[256];
        (void)snprintf(detail, sizeof(detail), "job=%lld branch=%.80s oid=%.64s %.80s",
                       (long long)job.id, job.branch, created_oid,
                       committed ? "" : job_error.message);
        ghm_log_event(committed ? GHM_LOG_INFO : GHM_LOG_ERROR, "scheduler",
                      committed ? "commit_completed" : "commit_failed", detail);
    }
finish_done:
    job_free(&job);
    ghm_repo_lock_release(operation_lock);
    return result;
}

int ghm_schedule_run_one_push(GhmContext *context, GhmError *error)
{
    static const char select_sql[] =
        "SELECT j.id,r.path,j.branch,j.result_oid,j.push_attempts FROM scheduled_jobs j "
        "JOIN repositories r ON r.id=j.repository_id "
        "WHERE j.status='COMPLETED' AND j.push_status='PENDING' AND j.push_retry_at<=unixepoch() ORDER BY j.id";
    static const char finish_sql[] =
        "UPDATE scheduled_jobs SET push_status=?2,error_message=?3,push_retry_at=?4,push_attempts=?5,push_error_code=?6 "
        "WHERE id=?1 AND push_status='RUNNING'";
    sqlite3_stmt *stmt = NULL;
    int64_t job_id = 0;
    char *path = NULL, *branch = NULL, *oid = NULL, *client_id = NULL;
    GhmError push_error = {0};
    GhmRepoLock *operation_lock = NULL;
    int transaction = 0, claimed = 0, pushed = 0, result = -1, step;
    int attempts = 0;
    if (context == NULL) { ghm_error_set(error, GHM_ERROR_ARGUMENT, "Context is required"); return -1; }
    pthread_mutex_lock(&context->database_mutex);
    if (sqlite3_exec(context->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) goto db_fail;
    transaction = 1;
    if (sqlite3_prepare_v2(context->db, select_sql, -1, &stmt, NULL) != SQLITE_OK) goto db_fail;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        job_id = sqlite3_column_int64(stmt, 0);
        path = column_copy(stmt, 1);
        branch = column_copy(stmt, 2);
        oid = column_copy(stmt, 3);
        attempts = sqlite3_column_int(stmt, 4);
        if (path == NULL || branch == NULL || oid == NULL) {
            ghm_error_set(error, GHM_ERROR_MEMORY, "Cannot load scheduled push");
            goto done_locked;
        }
        if (ghm_repo_lock_acquire(path, &operation_lock, &push_error) == 0 ||
            push_error.code != GHM_ERROR_BUSY) break;
        if (error != NULL) *error = push_error;
        free(path); path = NULL;
        free(branch); branch = NULL;
        free(oid); oid = NULL;
        push_error = (GhmError){0};
    }
    if (step == SQLITE_DONE) { result = 0; goto done_locked; }
    if (step != SQLITE_ROW) goto db_fail;
    sqlite3_finalize(stmt); stmt = NULL;
    if (sqlite3_prepare_v2(context->db,
            "UPDATE scheduled_jobs SET push_status='RUNNING' WHERE id=?1 AND push_status='PENDING'",
            -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 1, job_id) != SQLITE_OK || sqlite3_step(stmt) != SQLITE_DONE ||
        sqlite3_changes(context->db) != 1) goto db_fail;
    sqlite3_finalize(stmt); stmt = NULL;
    if (sqlite3_exec(context->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) goto db_fail;
    transaction = 0;
    claimed = 1;
    goto done_locked;
db_fail:
    database_error(context, error);
done_locked:
    sqlite3_finalize(stmt); stmt = NULL;
    if (transaction) (void)sqlite3_exec(context->db, "ROLLBACK", NULL, NULL, NULL);
    pthread_mutex_unlock(&context->database_mutex);
    if (!claimed) goto done;
    if (push_error.code == GHM_OK &&
        ghm_setting_get(context, "github_client_id", &client_id, &push_error) == 0)
        pushed = ghm_repo_push_exact(path, client_id, branch, oid, &push_error) == 0;
    int retry = !pushed && (push_error.code == GHM_ERROR_NETWORK ||
        (push_error.code == GHM_ERROR_RATE_LIMIT && attempts < 8));
    int64_t retry_at = 0;
    char notice[256];
    if (retry) {
        int delay = push_error.code == GHM_ERROR_RATE_LIMIT ? 60 : 15;
        for (int i = 0; i < attempts && delay < 300; ++i) delay *= 2;
        if (delay > 300) delay = 300;
        retry_at = (int64_t)time(NULL) + delay;
        if (push_error.retry_at > retry_at) retry_at = push_error.retry_at;
        (void)snprintf(notice, sizeof(notice), "%s; push queued for automatic retry at %lld",
            push_error.code == GHM_ERROR_RATE_LIMIT ? "GitHub rate limit reached" : "Waiting for network/server availability",
            (long long)retry_at);
    } else (void)snprintf(notice, sizeof(notice), "%s",
        !pushed && push_error.code == GHM_ERROR_RATE_LIMIT ?
        "Rate limit persisted after repeated attempts. Automatic retry paused; use Retry Push after the limit clears" : push_error.message);
    pthread_mutex_lock(&context->database_mutex);
    if (sqlite3_prepare_v2(context->db, finish_sql, -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 1, job_id) != SQLITE_OK ||
        bind_text(stmt, 2, pushed ? "COMPLETED" : retry ? "PENDING" : "FAILED") != SQLITE_OK ||
        (!pushed && bind_text(stmt, 3, notice) != SQLITE_OK) ||
        sqlite3_bind_int64(stmt, 4, retry_at) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 5, attempts < INT_MAX ? attempts + 1 : INT_MAX) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 6, pushed ? (int)GHM_OK : (int)push_error.code) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_DONE || sqlite3_changes(context->db) != 1) {
        database_error(context, error);
        result = -1;
    } else result = 1;
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&context->database_mutex);
    if (result == 1) {
        char detail[256];
        (void)snprintf(detail, sizeof(detail), "job=%lld oid=%.64s %.150s",
                       (long long)job_id, oid, pushed ? "" : notice);
        ghm_log_event(pushed ? GHM_LOG_INFO : retry ? GHM_LOG_WARNING : GHM_LOG_ERROR, "scheduler",
                      pushed ? "push_completed" : retry ? "push_waiting" : "push_failed", detail);
    }
done:
    ghm_repo_lock_release(operation_lock);
    free(path);
    free(branch);
    free(oid);
    free(client_id);
    return result;
}

static int pending_job_edit(GhmContext *context, int64_t job_id, int64_t execute_at,
                            int cancel, GhmError *error)
{
    static const char inspect_sql[] =
        "SELECT r.path,j.snapshot_ref,j.snapshot_tree_oid,"
        "(SELECT COUNT(*) FROM scheduled_jobs d WHERE d.predecessor_job_id=j.id "
        "AND d.status IN ('PENDING','RUNNING')) ,"
        "(SELECT p.execute_at FROM scheduled_jobs p WHERE p.id=j.predecessor_job_id) "
        "FROM scheduled_jobs j JOIN repositories r ON r.id=j.repository_id "
        "WHERE j.id=?1 AND j.status='PENDING'";
    sqlite3_stmt *stmt = NULL;
    char *path = NULL, *snapshot_ref = NULL, *tree_oid = NULL;
    int transaction = 0, result = -1, step;
    if (context == NULL || job_id <= 0 || (!cancel && execute_at <= (int64_t)time(NULL))) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "A pending job and future execution time are required");
        return -1;
    }
    pthread_mutex_lock(&context->database_mutex);
    if (sqlite3_exec(context->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) goto db_fail;
    transaction = 1;
    if (sqlite3_prepare_v2(context->db, inspect_sql, -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 1, job_id) != SQLITE_OK) goto db_fail;
    step = sqlite3_step(stmt);
    if (step == SQLITE_DONE) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Job is not pending or no longer exists");
        goto done_locked;
    }
    if (step != SQLITE_ROW) goto db_fail;
    if (sqlite3_column_int(stmt, 3) != 0) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT,
                      "Change or cancel later jobs on this branch first");
        goto done_locked;
    }
    if (!cancel && sqlite3_column_type(stmt, 4) != SQLITE_NULL &&
        execute_at <= sqlite3_column_int64(stmt, 4)) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "New time must follow the preceding job");
        goto done_locked;
    }
    if (cancel) {
        path = column_copy(stmt, 0);
        snapshot_ref = column_copy(stmt, 1);
        tree_oid = column_copy(stmt, 2);
        if (path == NULL || snapshot_ref == NULL || tree_oid == NULL) {
            ghm_error_set(error, GHM_ERROR_MEMORY, "Cannot read scheduled snapshot");
            goto done_locked;
        }
    }
    sqlite3_finalize(stmt); stmt = NULL;
    const char *update_sql = cancel ?
        "UPDATE scheduled_jobs SET status='CANCELLED',completed_at=?2 WHERE id=?1 AND status='PENDING'" :
        "UPDATE scheduled_jobs SET execute_at=?2 WHERE id=?1 AND status='PENDING'";
    if (sqlite3_prepare_v2(context->db, update_sql, -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 1, job_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, cancel ? (int64_t)time(NULL) : execute_at) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_DONE || sqlite3_changes(context->db) != 1) goto db_fail;
    sqlite3_finalize(stmt); stmt = NULL;
    if (sqlite3_exec(context->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) goto db_fail;
    transaction = 0;
    result = 0;
    goto done_locked;
db_fail:
    database_error(context, error);
done_locked:
    sqlite3_finalize(stmt);
    if (transaction) (void)sqlite3_exec(context->db, "ROLLBACK", NULL, NULL, NULL);
    pthread_mutex_unlock(&context->database_mutex);
    if (result == 0 && cancel) {
        GhmError ignored = {0};
        (void)ghm_snapshot_release(path, snapshot_ref, tree_oid, &ignored);
    }
    if (result == 0) {
        char detail[96];
        (void)snprintf(detail, sizeof(detail), "job=%lld", (long long)job_id);
        ghm_log_event(GHM_LOG_INFO, "scheduler", cancel ? "job_cancelled" : "job_rescheduled", detail);
    }
    free(path);
    free(snapshot_ref);
    free(tree_oid);
    return result;
}

static int ghm_schedule_cancel_unlocked(GhmContext *context, int64_t job_id, GhmError *error)
{
    return pending_job_edit(context, job_id, 0, 1, error);
}

static int ghm_schedule_reschedule_unlocked(GhmContext *context, int64_t job_id,
                            int64_t execute_at, GhmError *error)
{
    return pending_job_edit(context, job_id, execute_at, 0, error);
}

static int ghm_schedule_edit_unlocked(GhmContext *context, int64_t job_id,
                      const char *message, int push_after_commit, GhmError *error)
{
    static const char sql[] =
        "UPDATE scheduled_jobs SET message=?2,push_after_commit=?3 "
        "WHERE id=?1 AND status='PENDING'";
    sqlite3_stmt *stmt = NULL;
    int result = -1;
    if (context == NULL || job_id <= 0 || message == NULL || message[0] == '\0' ||
        (push_after_commit != 0 && push_after_commit != 1)) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "A pending job, message and push choice are required");
        return -1;
    }
    pthread_mutex_lock(&context->database_mutex);
    if (sqlite3_prepare_v2(context->db, sql, -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 1, job_id) != SQLITE_OK ||
        bind_text(stmt, 2, message) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 3, push_after_commit) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_DONE) database_error(context, error);
    else if (sqlite3_changes(context->db) != 1)
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Job is not pending or no longer exists");
    else result = 0;
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&context->database_mutex);
    if (result == 0) {
        char detail[96];
        (void)snprintf(detail, sizeof(detail), "job=%lld", (long long)job_id);
        ghm_log_event(GHM_LOG_INFO, "scheduler", "job_edited", detail);
    }
    return result;
}

static int ghm_schedule_retry_push_unlocked(GhmContext *context, int64_t job_id, GhmError *error)
{
    static const char sql[] =
        "UPDATE scheduled_jobs SET push_status='PENDING',error_message=NULL,push_retry_at=0,push_attempts=0,push_error_code=0 "
        "WHERE id=?1 AND status='COMPLETED' AND push_after_commit=1 AND push_status='FAILED' "
        "AND result_oid IS NOT NULL";
    sqlite3_stmt *stmt = NULL;
    int result = -1;
    if (context == NULL || job_id <= 0) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Job ID is required");
        return -1;
    }
    pthread_mutex_lock(&context->database_mutex);
    if (sqlite3_prepare_v2(context->db, sql, -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 1, job_id) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_DONE) database_error(context, error);
    else if (sqlite3_changes(context->db) != 1)
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Only a failed push of a completed job can be retried");
    else result = 0;
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&context->database_mutex);
    if (result == 0) {
        char detail[96];
        (void)snprintf(detail, sizeof(detail), "job=%lld", (long long)job_id);
        ghm_log_event(GHM_LOG_INFO, "scheduler", "push_requeued", detail);
    }
    return result;
}

int ghm_schedule_next_due(GhmContext *context, int64_t *out_execute_at, GhmError *error)
{
    sqlite3_stmt *stmt = NULL;
    int result = -1;
    if (context == NULL || out_execute_at == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Context and output are required");
        return -1;
    }
    *out_execute_at = -1;
    pthread_mutex_lock(&context->database_mutex);
    if (sqlite3_prepare_v2(context->db,
            "SELECT MIN(due) FROM (SELECT execute_at AS due FROM scheduled_jobs WHERE status='PENDING' "
            "UNION ALL SELECT CASE WHEN push_retry_at=0 THEN unixepoch() ELSE push_retry_at END AS due "
            "FROM scheduled_jobs WHERE status='COMPLETED' AND push_status='PENDING')",
            -1, &stmt, NULL) != SQLITE_OK || sqlite3_step(stmt) != SQLITE_ROW)
        database_error(context, error);
    else {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) *out_execute_at = sqlite3_column_int64(stmt, 0);
        result = 0;
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&context->database_mutex);
    return result;
}

int ghm_schedule_network_changed(GhmContext *context, GhmError *error)
{
    sqlite3_stmt *stmt = NULL;
    int result = -1;
    if (context == NULL) { ghm_error_set(error, GHM_ERROR_ARGUMENT, "Context is required"); return -1; }
    pthread_mutex_lock(&context->database_mutex);
    if (sqlite3_prepare_v2(context->db,
        "UPDATE scheduled_jobs SET push_retry_at=0 WHERE status='COMPLETED' AND push_status='PENDING' AND push_error_code=?1 AND push_retry_at>0",
        -1, &stmt, NULL) == SQLITE_OK && sqlite3_bind_int(stmt, 1, GHM_ERROR_NETWORK) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_DONE) result = 0;
    else database_error(context, error);
    sqlite3_finalize(stmt); pthread_mutex_unlock(&context->database_mutex);
    return result;
}

int ghm_schedule_add_options(GhmContext *context, const char *repository_path,
                             const char *message, const GhmCommitSignature *author,
                             const GhmCommitSignature *committer, int64_t execute_at,
                             int stage_all, int push_after_commit,
                             int64_t *out_job_id, GhmError *error)
{
    GhmRepoLock *lock = NULL;
#ifdef _WIN32
    char *normalized = NULL;
    if (ghm_repo_find(repository_path, &normalized, error) != 0) return -1;
    repository_path = normalized;
#endif
    int result = -1;
    if (ghm_repo_lock_acquire(repository_path, &lock, error) == 0)
        result = ghm_schedule_add_options_unlocked(context, repository_path, message, author, committer, execute_at, stage_all, push_after_commit, out_job_id, error);
    ghm_repo_lock_release(lock);
#ifdef _WIN32
    free(normalized);
#endif
    return result;
}

int ghm_schedule_cancel(GhmContext *context, int64_t job_id, GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (lock_job_repository(context, job_id, &lock, error) != 0) return -1;
    int result = ghm_schedule_cancel_unlocked(context, job_id, error);
    ghm_repo_lock_release(lock);
    return result;
}

int ghm_schedule_reschedule(GhmContext *context, int64_t job_id,
                            int64_t execute_at, GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (lock_job_repository(context, job_id, &lock, error) != 0) return -1;
    int result = ghm_schedule_reschedule_unlocked(context, job_id, execute_at, error);
    ghm_repo_lock_release(lock);
    return result;
}

int ghm_schedule_edit(GhmContext *context, int64_t job_id,
                      const char *message, int push_after_commit, GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (lock_job_repository(context, job_id, &lock, error) != 0) return -1;
    int result = ghm_schedule_edit_unlocked(context, job_id, message, push_after_commit, error);
    ghm_repo_lock_release(lock);
    return result;
}

int ghm_schedule_retry_push(GhmContext *context, int64_t job_id, GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (lock_job_repository(context, job_id, &lock, error) != 0) return -1;
    int result = ghm_schedule_retry_push_unlocked(context, job_id, error);
    ghm_repo_lock_release(lock);
    return result;
}
