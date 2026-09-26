#include "test_fixture.h"
#include "core/remote.h"
#include <ghm/github.h>
#include <signal.h>
#include <sys/wait.h>
static int isolated_network(void)
{
    char current[128];
    const char *initial = getenv("GHM_TEST_INITIAL_NETNS");
    ssize_t a = readlink("/proc/self/ns/net", current, sizeof(current) - 1);
    if (a <= 0 || initial == NULL) return -1;
    current[a] = '\0';
    return strcmp(current, initial) != 0; /* Never change host networking. */
}
static int network_event(void)
{
    if (isolated_network() != 1) return -1;
    pid_t child = fork();
    if (child == 0) { execl("/usr/bin/ip", "ip", "address", "add", "127.0.0.2/8", "dev", "lo", (char *)NULL); _exit(127); }
    int status;
    return child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}
static pid_t start_worker(const char *executable, const char *root)
{
    pid_t process = fork();
    if (process == 0) {
        if (setenv("XDG_DATA_HOME", root, 1) != 0 || setenv("XDG_STATE_HOME", root, 1) != 0) _exit(127);
        execl(executable, executable, (char *)NULL); _exit(127);
    }
    return process;
}
static void stop_worker(pid_t *process)
{
    if (*process > 0) { int status; (void)kill(*process, SIGCONT); (void)kill(*process, SIGTERM); (void)waitpid(*process, &status, 0); *process = -1; }
}
static int push_completed(Fixture *fixture, int64_t id)
{
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        sqlite3_stmt *stmt = NULL;
        int result = sqlite3_prepare_v2(fixture->context->db,
            "SELECT status,push_status FROM scheduled_jobs WHERE id=?1", -1, &stmt, NULL) == SQLITE_OK &&
            sqlite3_bind_int64(stmt, 1, id) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW &&
            strcmp((const char *)sqlite3_column_text(stmt, 0), "COMPLETED") == 0 &&
            strcmp((const char *)sqlite3_column_text(stmt, 1), "COMPLETED") == 0;
        sqlite3_finalize(stmt);
        if (result) return 1;
        struct timespec pause = {.tv_nsec = 100000000L}; (void)nanosleep(&pause, NULL);
    }
    return 0;
}
static int64_t retry_time(Fixture *fixture, int64_t id)
{
    sqlite3_stmt *stmt = NULL; int64_t result = -1;
    if (sqlite3_prepare_v2(fixture->context->db, "SELECT push_retry_at FROM scheduled_jobs WHERE id=?1", -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_bind_int64(stmt, 1, id) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) result = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt); return result;
}
int main(int argc, char **argv)
{
    Fixture fixture = {0}; GhmError error = {0}; int result = 1;
    git_repository *bare = NULL; git_remote *remote = NULL; pid_t worker = -1;
    char path[600], sql[400]; int64_t job, deadline = (int64_t)time(NULL) + 3600;
    REQUIRE(argc == 2 && fixture_open(&fixture, &error) == 0);
    if (getenv("GHM_TEST_ROUTE_EVENT") != NULL) {
        REQUIRE(isolated_network() == 1);
        char *login = NULL;
        REQUIRE(ghm_github_current_user("isolated-network-test-no-real-token", &login, &error) != 0 &&
            error.code == GHM_ERROR_NETWORK && login == NULL);
        (void)snprintf(path, sizeof(path), "%s/disconnected-clone", fixture.root);
        REQUIRE(ghm_remote_clone_url("https://github.com/octocat/Hello-World.git", path, NULL, &error) != 0 &&
            error.code == GHM_ERROR_NETWORK);
        puts("PASS real libcurl and libgit2 HTTPS requests report network failure in isolated offline namespace");
    }
    ghm_context_close(fixture.context); fixture.context = NULL;
    (void)snprintf(fixture.data, sizeof(fixture.data), "%s/ghm", fixture.root);
    REQUIRE(ghm_context_open(fixture.data, &fixture.context, &error) == 0 && ghm_repo_register(fixture.context, fixture.path, &error) == 0);
    (void)snprintf(path, sizeof(path), "%s/bare.git", fixture.root);
    REQUIRE(git_repository_init(&bare, path, 1) == 0 && git_remote_create(&remote, fixture.repo, "origin", path) == 0);
    REQUIRE(fixture_put(fixture.path, "a.txt", "overdue\n") == 0);
    REQUIRE(ghm_schedule_add_options(fixture.context, fixture.path, "missed deadline", &test_signature, &test_signature,
        (int64_t)time(NULL) - 3600, 1, 1, &job, &error) == 0);
    worker = start_worker(argv[1], fixture.root); REQUIRE(worker > 0 && push_completed(&fixture, job)); stop_worker(&worker);
    puts("PASS independent worker starts after missed deadline and creates/pushes exactly the frozen commit");
    REQUIRE(fixture_put(fixture.path, "a.txt", "offline retry on restart\n") == 0);
    REQUIRE(ghm_schedule_add_options(fixture.context, fixture.path, "offline restart", &test_signature, &test_signature,
        (int64_t)time(NULL), 1, 1, &job, &error) == 0 && ghm_schedule_run_one_due(fixture.context, (int64_t)time(NULL), &error) == 1);
    (void)snprintf(sql, sizeof(sql), "UPDATE scheduled_jobs SET push_retry_at=%lld,push_error_code=%d,push_attempts=1 WHERE id=%lld",
        (long long)deadline, GHM_ERROR_NETWORK, (long long)job);
    REQUIRE(sqlite3_exec(fixture.context->db, sql, NULL, NULL, NULL) == SQLITE_OK);
    worker = start_worker(argv[1], fixture.root); REQUIRE(worker > 0 && push_completed(&fixture, job)); stop_worker(&worker);
    puts("PASS independent worker restart immediately retries persisted offline push");
    REQUIRE(fixture_put(fixture.path, "a.txt", "rate cooldown\n") == 0);
    REQUIRE(ghm_schedule_add_options(fixture.context, fixture.path, "rate restart", &test_signature, &test_signature,
        (int64_t)time(NULL), 1, 1, &job, &error) == 0 && ghm_schedule_run_one_due(fixture.context, (int64_t)time(NULL), &error) == 1);
    (void)snprintf(sql, sizeof(sql), "UPDATE scheduled_jobs SET push_retry_at=%lld,push_error_code=%d,push_attempts=1 WHERE id=%lld",
        (long long)deadline, GHM_ERROR_RATE_LIMIT, (long long)job);
    REQUIRE(sqlite3_exec(fixture.context->db, sql, NULL, NULL, NULL) == SQLITE_OK);
    worker = start_worker(argv[1], fixture.root); REQUIRE(worker > 0);
    struct timespec pause = {.tv_nsec = 300000000L}; (void)nanosleep(&pause, NULL);
    REQUIRE(retry_time(&fixture, job) == deadline);
    (void)snprintf(sql, sizeof(sql), "UPDATE scheduled_jobs SET push_retry_at=unixepoch()+1 WHERE id=%lld", (long long)job);
    REQUIRE(sqlite3_exec(fixture.context->db, sql, NULL, NULL, NULL) == SQLITE_OK && push_completed(&fixture, job));
    stop_worker(&worker); puts("PASS worker preserves rate cooldown across restart and timer automatically retries when deadline expires");
    if (getenv("GHM_TEST_ROUTE_EVENT") != NULL) {
        worker = start_worker(argv[1], fixture.root); REQUIRE(worker > 0);
        (void)nanosleep(&pause, NULL);
        REQUIRE(kill(worker, SIGSTOP) == 0);
        int stopped; REQUIRE(waitpid(worker, &stopped, WUNTRACED) == worker && WIFSTOPPED(stopped));
        REQUIRE(fixture_put(fixture.path, "a.txt", "route event retry\n") == 0);
        REQUIRE(ghm_schedule_add_options(fixture.context, fixture.path, "route event", &test_signature, &test_signature,
            (int64_t)time(NULL) + 3600, 1, 1, &job, &error) == 0);
        REQUIRE(ghm_schedule_run_one_due(fixture.context, (int64_t)time(NULL) + 3601, &error) == 1);
        /* Keep the worker from racing this driver between commit completion
         * and inserting the simulated disconnected push state. */
        (void)snprintf(sql, sizeof(sql), "UPDATE scheduled_jobs SET push_status='PENDING',push_retry_at=%lld,push_error_code=%d,push_attempts=1 WHERE id=%lld",
            (long long)deadline, GHM_ERROR_NETWORK, (long long)job);
        REQUIRE(sqlite3_exec(fixture.context->db, sql, NULL, NULL, NULL) == SQLITE_OK);
        REQUIRE(kill(worker, SIGCONT) == 0);
        (void)nanosleep(&pause, NULL);
        REQUIRE(retry_time(&fixture, job) == deadline);
        REQUIRE(network_event() == 0);
        REQUIRE(push_completed(&fixture, job));
        stop_worker(&worker); puts("PASS real Linux netlink address event wakes worker and retries offline push before fallback deadline");
    }
    result = 0;
done:
    stop_worker(&worker); git_remote_free(remote); git_repository_free(bare); fixture_close(&fixture); return result;
}
