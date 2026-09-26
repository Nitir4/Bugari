#include "test_fixture.h"
#include "fault_hooks.h"
#include "core/remote.h"
#include "github/github_api.h"
#include <ghm/remote.h>
typedef struct { GhmErrorCode code; int64_t retry_at; } NetworkFault;
static int disconnected(const char *point, GhmError *error, void *payload)
{
    NetworkFault *fault = payload;
    if (strcmp(point, "remote.push") != 0) return 0;
    error->code = fault->code; error->retry_at = fault->retry_at;
    (void)snprintf(error->message, sizeof(error->message), "%s", fault->code == GHM_ERROR_RATE_LIMIT ? "GitHub rate limit reached" : "Network unavailable");
    return -1;
}
static int job_is(Fixture *fixture, int64_t id, const char *push, int64_t *retry_at, int *attempts)
{
    GhmScheduledJobList jobs = {0}; GhmError error = {0}; int result = 0;
    if (ghm_schedule_list(fixture->context, fixture->path, &jobs, &error) != 0) return 0;
    for (size_t i = 0; i < jobs.count; ++i) if (jobs.items[i].id == id) {
        result = strcmp(jobs.items[i].status, "COMPLETED") == 0 && strcmp(jobs.items[i].push_status, push) == 0;
        if (retry_at != NULL) *retry_at = jobs.items[i].push_retry_at;
        if (attempts != NULL) *attempts = jobs.items[i].push_attempts;
    }
    ghm_schedule_list_free(&jobs); return result;
}
int main(void)
{
    Fixture fixture = {0}; GhmError error = {0}; int result = 1;
    git_repository *bare = NULL; git_remote *remote = NULL;
    char path[512]; int64_t id, retry_at, next_due; int attempts;
    REQUIRE(fixture_open(&fixture, &error) == 0);
    (void)snprintf(path, sizeof(path), "%s/remote.git", fixture.root);
    REQUIRE(git_repository_init(&bare, path, 1) == 0 && git_remote_create(&remote, fixture.repo, "origin", path) == 0);
    REQUIRE(fixture_put(fixture.path, "a.txt", "offline snapshot\n") == 0);
    REQUIRE(ghm_schedule_add_options(fixture.context, fixture.path, "offline", &test_signature, &test_signature,
        (int64_t)time(NULL) - 300, 1, 1, &id, &error) == 0);
    /* Overdue local execution does not require a network connection. */
    REQUIRE(ghm_schedule_run_one_due(fixture.context, (int64_t)time(NULL), &error) == 1);
    NetworkFault fault = {GHM_ERROR_NETWORK, 0}; ghm_test_fault_set(disconnected, &fault);
    REQUIRE(ghm_schedule_run_one_push(fixture.context, &error) == 1 && job_is(&fixture, id, "PENDING", &retry_at, &attempts));
    REQUIRE(retry_at > (int64_t)time(NULL) && attempts == 1);
    REQUIRE(ghm_schedule_run_one_push(fixture.context, &error) == 0);
    REQUIRE(ghm_schedule_next_due(fixture.context, &next_due, &error) == 0 && next_due == retry_at);
    ghm_context_close(fixture.context); fixture.context = NULL;
    REQUIRE(ghm_context_open(fixture.data, &fixture.context, &error) == 0 && job_is(&fixture, id, "PENDING", &next_due, NULL) && next_due == retry_at);
    REQUIRE(ghm_schedule_network_changed(fixture.context, &error) == 0);
    ghm_test_fault_set(NULL, NULL);
    REQUIRE(ghm_schedule_run_one_push(fixture.context, &error) == 1 && job_is(&fixture, id, "COMPLETED", NULL, NULL));
    REQUIRE(fixture_text_is(fixture.path, "a.txt", "offline snapshot\n") && !fixture_head_is(fixture.path, fixture.base));
    puts("PASS overdue local commit, persisted offline retry and immediate eligibility on connectivity change");
    REQUIRE(fixture_put(fixture.path, "a.txt", "rate limited snapshot\n") == 0);
    REQUIRE(ghm_schedule_add_options(fixture.context, fixture.path, "rate limited", &test_signature, &test_signature,
        (int64_t)time(NULL) - 1, 1, 1, &id, &error) == 0 && ghm_schedule_run_one_due(fixture.context, (int64_t)time(NULL), &error) == 1);
    fault = (NetworkFault){GHM_ERROR_RATE_LIMIT, (int64_t)time(NULL) + 3600}; ghm_test_fault_set(disconnected, &fault);
    REQUIRE(ghm_schedule_run_one_push(fixture.context, &error) == 1 && job_is(&fixture, id, "PENDING", &retry_at, &attempts));
    REQUIRE(retry_at == fault.retry_at && attempts == 1);
    REQUIRE(ghm_schedule_network_changed(fixture.context, &error) == 0 && job_is(&fixture, id, "PENDING", &retry_at, NULL) && retry_at == fault.retry_at);
    REQUIRE(ghm_schedule_run_one_push(fixture.context, &error) == 0);
    REQUIRE(sqlite3_exec(fixture.context->db, "UPDATE scheduled_jobs SET push_retry_at=0,push_attempts=8 WHERE push_status='PENDING'", NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(ghm_schedule_run_one_push(fixture.context, &error) == 1 && job_is(&fixture, id, "FAILED", NULL, NULL));
    ghm_test_fault_set(NULL, NULL);
    REQUIRE(ghm_schedule_retry_push(fixture.context, id, &error) == 0 && ghm_schedule_run_one_push(fixture.context, &error) == 1);
    puts("PASS rate-limit deadline, connectivity cannot bypass cooldown, bounded attempts and manual retry");
    (void)snprintf(path, sizeof(path), "%s/missing.git", fixture.root);
    char pushed_oid[GHM_OID_HEX_CAPACITY];
    REQUIRE(git_remote_set_pushurl(fixture.repo, "origin", path) == 0);
    REQUIRE(ghm_repo_push(fixture.path, NULL, pushed_oid, &error) != 0);
    REQUIRE(error.code == GHM_ERROR_GIT);
    REQUIRE(git_remote_set_pushurl(fixture.repo, "origin", NULL) == 0);
    puts("PASS real missing local remote is permanent failure rather than offline retry");
    ghm_remote_error_classify("unexpected HTTP status code: 429", GIT_ERROR_HTTP, &error);
    REQUIRE(error.code == GHM_ERROR_RATE_LIMIT && error.retry_at > (int64_t)time(NULL));
    ghm_remote_error_classify("unexpected HTTP status code: 403", GIT_ERROR_HTTP, &error);
    REQUIRE(error.code == GHM_ERROR_AUTH);
    ghm_remote_error_classify("failed to connect", GIT_ERROR_NET, &error);
    REQUIRE(error.code == GHM_ERROR_NETWORK);
    ghm_remote_error_classify("could not resolve path '/tmp/missing': No such file or directory", GIT_ERROR_OS, &error);
    REQUIRE(error.code == GHM_ERROR_GIT);
    ghm_remote_error_classify("non-fastforward update", GIT_ERROR_REFERENCE, &error);
    REQUIRE(error.code == GHM_ERROR_GIT);
    GhmHttpResponse response = {0};
    const char *headers[] = {"HTTP/2 403\r\n", "X-RateLimit-Remaining: 0\r\n", "X-RateLimit-Reset: 1900000000\r\n", "Retry-After: 120\r\n"};
    for (size_t i = 0; i < 4; ++i) ghm_http_read_header(&response, headers[i], strlen(headers[i]));
    REQUIRE(response.rate_remaining_zero && response.rate_reset_at == 1900000000 && response.retry_at >= (int64_t)time(NULL) + 119);
    ghm_http_read_header(&response, "HTTP/2 200\r\n", 12);
    REQUIRE(!response.rate_remaining_zero && response.retry_at == 0 && response.rate_reset_at == 0);
    puts("PASS HTTP rate headers and separate transient/permission/non-fast-forward classification");
    result = 0;
done:
    ghm_test_fault_set(NULL, NULL); git_remote_free(remote); git_repository_free(bare); fixture_close(&fixture); return result;
}
