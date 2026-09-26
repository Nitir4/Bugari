#include "test_fixture.h"
#include "core/lock.h"
#include "fault_hooks.h"
#include <signal.h>
#include <sys/wait.h>
static int terminate_commit(const char *point, GhmError *error, void *payload)
{
    (void)error; (void)payload;
    if (strcmp(point, "commit.before_publish") == 0) (void)kill(getpid(), SIGKILL);
    return 0;
}
int main(void)
{
    Fixture fixture = {0};
    GhmError error = {0};
    git_worktree *worktree = NULL;
    git_repository *linked_repo = NULL;
    GhmRepoLock *lock = NULL, *nested = NULL;
    char linked[512], oid[GHM_OID_HEX_CAPACITY];
    int result = 1;
    REQUIRE(fixture_open(&fixture, &error) == 0);
    (void)snprintf(linked, sizeof(linked), "%s/linked", fixture.root);
    REQUIRE(git_worktree_add(&worktree, fixture.repo, "linked", linked, NULL) == 0);
    REQUIRE(git_repository_open(&linked_repo, linked) == 0);
    REQUIRE(strcmp(git_repository_commondir(fixture.repo), git_repository_commondir(linked_repo)) == 0);
    REQUIRE(ghm_repo_register(fixture.context, linked, &error) == 0);
    REQUIRE(ghm_repo_lock_acquire(fixture.path, &lock, &error) == 0);
    REQUIRE(ghm_repo_lock_acquire(linked, &nested, &error) == 0 && nested == lock);
    ghm_repo_lock_release(nested); nested = NULL;
    pid_t child = fork();
    if (child == 0) {
        GhmRepoLock *other = NULL;
        int refused = ghm_repo_lock_acquire(linked, &other, &error) != 0 && error.code == GHM_ERROR_BUSY;
        ghm_repo_lock_release(other);
        _exit(refused ? 0 : 1);
    }
    int status;
    REQUIRE(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    ghm_repo_lock_release(lock); lock = NULL;
    REQUIRE(ghm_branch_checkout(fixture.context, fixture.path, "linked", &error) != 0);
    REQUIRE(strstr(error.message, "another linked worktree") != NULL);
    REQUIRE(fixture_head_is(fixture.path, fixture.base));
    REQUIRE(fixture_put(linked, "a.txt", "linked staged\n") == 0);
    REQUIRE(ghm_repo_stage_path(linked, "a.txt", &error) == 0);
    REQUIRE(ghm_commit_staged(linked, "linked commit", &test_signature, &test_signature, oid, &error) == 0);
    REQUIRE(fixture_head_is(fixture.path, fixture.base) && fixture_text_is(fixture.path, "a.txt", "base\n"));
    REQUIRE(fixture_put(linked, "a.txt", "linked scheduled\n") == 0);
    int64_t job;
    REQUIRE(ghm_schedule_add(fixture.context, linked, "linked snapshot", &test_signature, &test_signature,
        (int64_t)time(NULL) + 60, &job, &error) == 0);
    REQUIRE(ghm_branch_create(linked, "other", &error) == 0);
    REQUIRE(ghm_branch_checkout(fixture.context, linked, "other", &error) != 0 && strstr(error.message, "scheduled") != NULL);
    REQUIRE(ghm_schedule_run_one_due(fixture.context, (int64_t)time(NULL) + 120, &error) == 1);
    REQUIRE(fixture_head_is(fixture.path, fixture.base) && fixture_text_is(fixture.path, "a.txt", "base\n"));
    puts("PASS linked worktree lock, branch occupancy, separate index and scheduled snapshot");
    REQUIRE(fixture_put(linked, "a.txt", "terminated linked commit\n") == 0);
    child = fork();
    if (child == 0) {
        ghm_test_fault_set(terminate_commit, NULL);
        (void)ghm_commit_now(linked, "interrupted linked", &test_signature, &test_signature, oid, &error);
        _exit(1);
    }
    REQUIRE(child > 0 && waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    REQUIRE(ghm_branch_create(fixture.path, "must-wait", &error) != 0 && error.code == GHM_ERROR_BUSY);
    REQUIRE(strstr(error.message, "linked worktree") != NULL);
    REQUIRE(ghm_repo_lock_acquire(linked, &lock, &error) == 0);
    ghm_repo_lock_release(lock); lock = NULL;
    REQUIRE(ghm_branch_create(fixture.path, "recovered", &error) == 0);
    REQUIRE(fixture_head_is(fixture.path, fixture.base) && fixture_text_is(linked, "a.txt", "terminated linked commit\n"));
    puts("PASS other worktrees refuse shared mutation until terminated worktree recovers");
    result = 0;
done:
    ghm_repo_lock_release(nested); ghm_repo_lock_release(lock);
    git_repository_free(linked_repo); git_worktree_free(worktree);
    fixture_close(&fixture);
    return result;
}
