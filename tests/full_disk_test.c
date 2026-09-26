/* Manual test inside an 8 MiB tmpfs mounted in a private user/mount namespace.
 * Hooks only choose when to fill it; production writes encounter real ENOSPC. */
#include "test_fixture.h"
#include "fault_hooks.h"
#include "core/lock.h"
#include "core/mutation.h"
#include <ghm/file_ops.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/statvfs.h>
typedef struct { const char *point; char filler[512]; int filled; } Fill;
static int fill_disk(Fill *fill)
{
    int file = open(fill->filler, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
    if (file < 0) return -1;
    char bytes[65536] = {0}; ssize_t count;
    do { count = write(file, bytes, sizeof(bytes)); } while (count > 0);
    int full = count < 0 && errno == ENOSPC;
    close(file); fill->filled = full; return full ? 0 : -1;
}
static int fill_at(const char *point, GhmError *error, void *payload)
{
    Fill *fill = payload; (void)error;
    if (strcmp(point, fill->point) == 0 && !fill->filled) return fill_disk(fill);
    return 0;
}
static int no_jobs(Fixture *fixture)
{
    GhmScheduledJobList jobs = {0}; GhmError error = {0};
    int result = ghm_schedule_list(fixture->context, fixture->path, &jobs, &error) == 0 && jobs.count == 0;
    ghm_schedule_list_free(&jobs); return result;
}
int main(void)
{
    Fixture fixture = {0}; GhmError error = {0}; int result = 1;
    char *text = NULL; size_t length; GhmFileVersion version, saved;
    Fill fill = {0}; int64_t job = 0; char oid[GHM_OID_HEX_CAPACITY]; GhmRepoLock *lock = NULL;
    struct statvfs space;
    /* Never fill the host filesystem: require a deliberately tiny mount. */
    REQUIRE(statvfs("/tmp", &space) == 0 && space.f_blocks * space.f_frsize <= 16U * 1024U * 1024U);
    REQUIRE(fixture_open(&fixture, &error) == 0);
    (void)snprintf(fill.filler, sizeof(fill.filler), "%s/filler", fixture.root);
    REQUIRE(ghm_file_read_text(fixture.path, "a.txt", &text, &length, &version, &error) == 0); free(text); text = NULL;
    REQUIRE(fill_disk(&fill) == 0);
    REQUIRE(ghm_schedule_add(fixture.context, fixture.path, "full", &test_signature, &test_signature,
        (int64_t)time(NULL) + 60, &job, &error) != 0 && job == 0 && strstr(error.message, "disk space") != NULL);
    REQUIRE(ghm_commit_now(fixture.path, "full", &test_signature, &test_signature, oid, &error) != 0);
    REQUIRE(ghm_file_save_text(fixture.path, "a.txt", "replacement\n", 12, &version, &saved, &error) != 0);
    REQUIRE(fixture_head_is(fixture.path, fixture.base) && fixture_text_is(fixture.path, "a.txt", "base\n") && no_jobs(&fixture));
    REQUIRE(unlink(fill.filler) == 0); fill.filled = 0;
    puts("PASS actual full-filesystem preflight refuses schedule/commit/save without changing HEAD, index, files or jobs");
    fill.point = "file.write"; ghm_test_fault_set(fill_at, &fill);
    REQUIRE(ghm_file_save_text(fixture.path, "a.txt", "replacement\n", 12, &version, &saved, &error) != 0 && fill.filled && error.code == GHM_ERROR_IO);
    ghm_test_fault_set(NULL, NULL);
    REQUIRE(fixture_text_is(fixture.path, "a.txt", "base\n") && unlink(fill.filler) == 0); fill.filled = 0;
    puts("PASS real ENOSPC during atomic editor write preserves the original file");
    REQUIRE(fixture_put(fixture.path, "a.txt", "commit before index full\n") == 0);
    fill.point = "index.write"; ghm_test_fault_set(fill_at, &fill);
    REQUIRE(ghm_commit_now(fixture.path, "published with full index", &test_signature, &test_signature, oid, &error) != 0 && fill.filled && oid[0] != '\0');
    ghm_test_fault_set(NULL, NULL);
    REQUIRE(ghm_mutation_pending(fixture.repo) && unlink(fill.filler) == 0); fill.filled = 0;
    REQUIRE(ghm_repo_lock_acquire(fixture.path, &lock, &error) == 0); ghm_repo_lock_release(lock); lock = NULL;
    REQUIRE(fixture_head_is(fixture.path, oid) && !ghm_mutation_pending(fixture.repo));
    GhmStatus status = {0}; REQUIRE(ghm_repo_status(fixture.path, &status, &error) == 0 && status.count == 0); ghm_status_free(&status);
    puts("PASS real ENOSPC after commit publication retains recovery record and finishes index without another commit");
    REQUIRE(fixture_put(fixture.path, "a.txt", "snapshot under disk pressure\n") == 0);
    fill.point = "scheduler.before_insert"; ghm_test_fault_set(fill_at, &fill);
    REQUIRE(ghm_schedule_add(fixture.context, fixture.path, "SQL full", &test_signature, &test_signature,
        (int64_t)time(NULL) + 60, &job, &error) != 0 && fill.filled && job == 0 && error.code == GHM_ERROR_DATABASE);
    ghm_test_fault_set(NULL, NULL); REQUIRE(unlink(fill.filler) == 0 && no_jobs(&fixture));
    git_reference_iterator *iterator = NULL; const char *name;
    REQUIRE(git_reference_iterator_glob_new(&iterator, fixture.repo, "refs/ghm/jobs/*") == 0);
    int remaining = git_reference_next_name(&name, iterator); git_reference_iterator_free(iterator); REQUIRE(remaining == GIT_ITEROVER);
    puts("PASS real ENOSPC during SQLite job insertion rolls back and releases the frozen snapshot ref");
    result = 0;
done:
    ghm_test_fault_set(NULL, NULL); free(text); ghm_repo_lock_release(lock); fixture_close(&fixture); return result;
}
