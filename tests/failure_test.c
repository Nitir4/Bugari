#include "test_fixture.h"
#include "fault_hooks.h"
#include "core/lock.h"
#include "core/mutation.h"
#include <signal.h>
#include <sys/wait.h>

typedef struct { const char *point; const char *second; int terminate; } Fault;
static int inject(const char *point, GhmError *error, void *payload)
{
    Fault *fault = payload;
    if (strcmp(point, fault->point) != 0 && (fault->second == NULL || strcmp(point, fault->second) != 0)) return 0;
    if (fault->terminate) { (void)kill(getpid(), SIGKILL); _exit(99); }
    if (error != NULL) {
        error->code = GHM_ERROR_IO;
        (void)snprintf(error->message, sizeof(error->message), "%s",
            strcmp(point, "storage.check") == 0 ? "Not enough free disk space; scheduling was refused" : "Injected write failure");
    }
    return -1;
}
static int snapshot_refs(git_repository *repo)
{
    git_reference_iterator *iterator = NULL;
    int count = 0;
    const char *name;
    if (git_reference_iterator_glob_new(&iterator, repo, "refs/ghm/jobs/*") < 0) return -1;
    while (git_reference_next_name(&name, iterator) == 0) ++count;
    git_reference_iterator_free(iterator);
    return count;
}
static int job_status(Fixture *fixture, int64_t id, const char *status)
{
    sqlite3_stmt *stmt = NULL;
    int result = sqlite3_prepare_v2(fixture->context->db, "SELECT status FROM scheduled_jobs WHERE id=?1", -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_bind_int64(stmt, 1, id) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW &&
        strcmp((const char *)sqlite3_column_text(stmt, 0), status) == 0;
    sqlite3_finalize(stmt);
    return result;
}
static int clean(const char *path)
{
    GhmStatus status = {0}; GhmError error = {0};
    int result = ghm_repo_status(path, &status, &error) == 0 && status.count == 0;
    ghm_status_free(&status);
    return result;
}
static int feature(Fixture *fixture, GhmError *error)
{
    char oid[GHM_OID_HEX_CAPACITY];
    return ghm_branch_create(fixture->path, "feature", error) == 0 &&
        ghm_branch_checkout(fixture->context, fixture->path, "feature", error) == 0 &&
        fixture_put(fixture->path, "a.txt", "feature\n") == 0 &&
        fixture_put(fixture->path, "z.txt", "feature z\n") == 0 &&
        ghm_commit_now(fixture->path, "feature", &test_signature, &test_signature, oid, error) == 0 &&
        ghm_branch_checkout(fixture->context, fixture->path, fixture->branch, error) == 0 ? 0 : -1;
}
static int permission_and_sqlite(void)
{
    Fixture fixture = {0}; GhmError error = {0};
    int result = 1; int64_t job = 0;
    char path[1024], oid[GHM_OID_HEX_CAPACITY];
    REQUIRE(fixture_open(&fixture, &error) == 0 && feature(&fixture, &error) == 0);
    (void)snprintf(path, sizeof(path), "%s/z.txt", fixture.path);
    REQUIRE(chmod(path, 0400) == 0);
    REQUIRE(ghm_branch_checkout(fixture.context, fixture.path, "feature", &error) != 0 && error.code == GHM_ERROR_IO);
    REQUIRE(fixture_head_is(fixture.path, fixture.base) && fixture_text_is(fixture.path, "a.txt", "base\n") &&
        fixture_text_is(fixture.path, "z.txt", "base z\n") && clean(fixture.path));
    REQUIRE(chmod(path, 0600) == 0);
    REQUIRE(fixture_put(fixture.path, "a.txt", "snapshot\n") == 0);
    REQUIRE(chmod(fixture.data, 0500) == 0);
    REQUIRE(ghm_schedule_add(fixture.context, fixture.path, "denied", &test_signature, &test_signature,
        (int64_t)time(NULL) + 60, &job, &error) != 0 && error.code == GHM_ERROR_IO && job == 0);
    REQUIRE(chmod(fixture.data, 0700) == 0 && snapshot_refs(fixture.repo) == 0);
    Fault fault = {.point = "storage.check"}; ghm_test_fault_set(inject, &fault);
    REQUIRE(ghm_schedule_add(fixture.context, fixture.path, "full disk", &test_signature, &test_signature,
        (int64_t)time(NULL) + 60, &job, &error) != 0 && job == 0 && strstr(error.message, "disk space") != NULL);
    ghm_test_fault_set(NULL, NULL);
    REQUIRE(snapshot_refs(fixture.repo) == 0 && fixture_head_is(fixture.path, fixture.base));
    REQUIRE(sqlite3_exec(fixture.context->db, "PRAGMA query_only=ON", NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(ghm_schedule_add(fixture.context, fixture.path, "read-only database", &test_signature, &test_signature,
        (int64_t)time(NULL) + 60, &job, &error) != 0 && error.code == GHM_ERROR_DATABASE && job == 0);
    REQUIRE(sqlite3_exec(fixture.context->db, "PRAGMA query_only=OFF; PRAGMA max_page_count=1", NULL, NULL, NULL) == SQLITE_OK);
    char *large_message = malloc(2U * 1024U * 1024U);
    REQUIRE(large_message != NULL);
    memset(large_message, 'X', 2U * 1024U * 1024U - 1); large_message[2U * 1024U * 1024U - 1] = '\0';
    int full_result = ghm_schedule_add(fixture.context, fixture.path, large_message, &test_signature, &test_signature,
        (int64_t)time(NULL) + 60, &job, &error);
    free(large_message);
    REQUIRE(full_result != 0 && error.code == GHM_ERROR_DATABASE && strstr(error.message, "full") != NULL && job == 0);
    REQUIRE(snapshot_refs(fixture.repo) == 0 && fixture_head_is(fixture.path, fixture.base));
    REQUIRE(sqlite3_exec(fixture.context->db, "PRAGMA max_page_count=2147483646", NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_exec(fixture.context->db, "CREATE TRIGGER deny_insert BEFORE INSERT ON scheduled_jobs BEGIN SELECT RAISE(FAIL,'test insertion failure'); END", NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(ghm_schedule_add(fixture.context, fixture.path, "SQL failure", &test_signature, &test_signature,
        (int64_t)time(NULL) + 60, &job, &error) != 0 && error.code == GHM_ERROR_DATABASE && job == 0);
    REQUIRE(snapshot_refs(fixture.repo) == 0 && fixture_text_is(fixture.path, "a.txt", "snapshot\n"));
    REQUIRE(sqlite3_exec(fixture.context->db, "DROP TRIGGER deny_insert", NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(ghm_schedule_add(fixture.context, fixture.path, "SQL recover", &test_signature, &test_signature,
        (int64_t)time(NULL) + 60, &job, &error) == 0);
    REQUIRE(sqlite3_exec(fixture.context->db, "CREATE TRIGGER deny_claim BEFORE UPDATE OF status ON scheduled_jobs WHEN NEW.status='RUNNING' BEGIN SELECT RAISE(FAIL,'test claim failure'); END", NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(ghm_schedule_run_one_due(fixture.context, (int64_t)time(NULL) + 120, &error) == -1 && job_status(&fixture, job, "PENDING"));
    REQUIRE(fixture_head_is(fixture.path, fixture.base));
    REQUIRE(sqlite3_exec(fixture.context->db, "DROP TRIGGER deny_claim; CREATE TRIGGER deny_finish BEFORE UPDATE OF status ON scheduled_jobs WHEN NEW.status='COMPLETED' BEGIN SELECT RAISE(FAIL,'test finish failure'); END", NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(ghm_schedule_run_one_due(fixture.context, (int64_t)time(NULL) + 120, &error) == -1 && job_status(&fixture, job, "RUNNING"));
    REQUIRE(!fixture_head_is(fixture.path, fixture.base));
    git_reference *head = NULL;
    REQUIRE(git_repository_head(&head, fixture.repo) == 0);
    (void)git_oid_tostr(oid, sizeof(oid), git_reference_target(head)); git_reference_free(head);
    REQUIRE(sqlite3_exec(fixture.context->db, "DROP TRIGGER deny_finish", NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(ghm_schedule_recover(fixture.context, (int64_t)time(NULL), &error) == 0 && job_status(&fixture, job, "COMPLETED"));
    REQUIRE(ghm_schedule_run_one_due(fixture.context, (int64_t)time(NULL) + 120, &error) == 0 && fixture_head_is(fixture.path, oid));
    puts("PASS denied permissions, full-disk refusal, SQLite insertion/claim/completion failure and no duplicate recovery");
    result = 0;
done:
    ghm_test_fault_set(NULL, NULL); fixture_close(&fixture); return result;
}
static int checkout_failures(void)
{
    Fixture fixture = {0}; GhmError error = {0}; int result = 1;
    REQUIRE(fixture_open(&fixture, &error) == 0 && feature(&fixture, &error) == 0);
    const char *points[] = {"checkout.after_files", "index.write", "checkout.publish_ref"};
    for (size_t i = 0; i < sizeof(points) / sizeof(*points); ++i) {
        Fault fault = {.point = points[i]}; ghm_test_fault_set(inject, &fault);
        REQUIRE(ghm_branch_checkout(fixture.context, fixture.path, "feature", &error) != 0);
        ghm_test_fault_set(NULL, NULL);
        REQUIRE(fixture_head_is(fixture.path, fixture.base) && clean(fixture.path) &&
            fixture_text_is(fixture.path, "a.txt", "base\n") && !ghm_mutation_pending(fixture.repo));
    }
    Fault fault = {.point = "checkout.after_files", .second = "checkout.rollback"};
    ghm_test_fault_set(inject, &fault);
    REQUIRE(ghm_branch_checkout(fixture.context, fixture.path, "feature", &error) != 0 && strstr(error.message, "rollback failed") != NULL);
    REQUIRE(ghm_mutation_pending(fixture.repo));
    ghm_test_fault_set(NULL, NULL);
    GhmRepoLock *lock = NULL;
    REQUIRE(ghm_repo_lock_acquire(fixture.path, &lock, &error) == 0);
    ghm_repo_lock_release(lock);
    REQUIRE(!ghm_mutation_pending(fixture.repo) && fixture_head_is(fixture.path, fixture.base) && clean(fixture.path));
    puts("PASS checkout/index/ref write rollback and retained recovery record after rollback failure");
    result = 0;
done:
    ghm_test_fault_set(NULL, NULL); fixture_close(&fixture); return result;
}
static int killed_mutation(const char *point, int schedule, int checkout)
{
    Fixture fixture = {0}; GhmError error = {0}; int result = 1; int64_t job = 0;
    REQUIRE(fixture_open(&fixture, &error) == 0);
    if (checkout) REQUIRE(feature(&fixture, &error) == 0);
    else REQUIRE(fixture_put(fixture.path, "a.txt", "killed commit\n") == 0);
    if (schedule) REQUIRE(ghm_schedule_add(fixture.context, fixture.path, "killed job", &test_signature, &test_signature,
        (int64_t)time(NULL) + 60, &job, &error) == 0);
    pid_t child = fork();
    if (child == 0) {
        Fault fault = {.point = point, .terminate = 1}; ghm_test_fault_set(inject, &fault);
        char oid[GHM_OID_HEX_CAPACITY];
        if (checkout) (void)ghm_branch_checkout(fixture.context, fixture.path, "feature", &error);
        else if (schedule) {
            GhmContext *context = NULL;
            if (ghm_context_open(fixture.data, &context, &error) != 0) _exit(98);
            (void)ghm_schedule_run_one_due(context, (int64_t)time(NULL) + 120, &error);
        } else (void)ghm_commit_now(fixture.path, "killed immediate", &test_signature, &test_signature, oid, &error);
        _exit(97);
    }
    int status;
    REQUIRE(child > 0 && waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    GhmRepoLock *lock = NULL;
    REQUIRE(ghm_repo_lock_acquire(fixture.path, &lock, &error) == 0);
    ghm_repo_lock_release(lock);
    REQUIRE(!ghm_mutation_pending(fixture.repo));
    if (checkout) REQUIRE(clean(fixture.path) && fixture_head_is(fixture.path, fixture.base));
    else if (strcmp(point, "commit.before_publish") == 0) REQUIRE(fixture_head_is(fixture.path, fixture.base));
    else REQUIRE(!fixture_head_is(fixture.path, fixture.base) && clean(fixture.path));
    if (schedule) {
        REQUIRE(ghm_schedule_recover(fixture.context, (int64_t)time(NULL), &error) == 0);
        REQUIRE(job_status(&fixture, job, strcmp(point, "commit.before_publish") == 0 ? "FAILED" : "COMPLETED"));
        REQUIRE(ghm_schedule_run_one_due(fixture.context, (int64_t)time(NULL) + 120, &error) == 0);
    }
    printf("PASS SIGKILL %s (%s) and safe recovery\n", point, checkout ? "checkout" : schedule ? "scheduled" : "immediate");
    result = 0;
done:
    fixture_close(&fixture); return result;
}
int main(void)
{
    if (permission_and_sqlite() != 0 || checkout_failures() != 0 || killed_mutation("checkout.after_files", 0, 1) != 0) return 1;
    const char *points[] = {"commit.before_publish", "commit.after_publish"};
    for (size_t i = 0; i < 2; ++i) for (int scheduled = 0; scheduled <= 1; ++scheduled)
        if (killed_mutation(points[i], scheduled, 0) != 0) return 1;
    return killed_mutation("scheduler.before_finish", 1, 0);
}
