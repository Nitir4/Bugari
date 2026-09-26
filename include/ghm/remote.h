#ifndef GHM_REMOTE_H
#define GHM_REMOTE_H

#include <ghm/github_repos.h>
#include <ghm/commit.h>

/* Clone a selected GitHub repository into the managed workspace or reopen its
 * existing matching clone. Caller frees out_path with free(). */
int ghm_repo_clone_github(GhmContext *context, const char *client_id,
                          const GhmGitHubRepo *repo, char **out_path, GhmError *error);

/* Push the checked-out branch to origin without force. GitHub HTTPS remotes
 * use the saved OAuth session; local filesystem remotes need no credential. */
int ghm_repo_push(const char *repository_path, const char *client_id,
                  char out_oid[GHM_OID_HEX_CAPACITY], GhmError *error);

/* Push an immutable commit to its named branch, without force. Refuse if the
 * checked-out local branch no longer points to that commit. */
int ghm_repo_push_exact(const char *repository_path, const char *client_id,
                        const char *branch, const char *commit_oid, GhmError *error);

typedef enum {
    GHM_PULL_UP_TO_DATE,
    GHM_PULL_FAST_FORWARDED,
    GHM_PULL_LOCAL_AHEAD
} GhmPullOutcome;

/* Fetch origin without changing local branches or worktree files. */
int ghm_repo_fetch(const char *repository_path, const char *client_id, GhmError *error);

/* Fetch then safely fast-forward the checked-out branch. Divergence and dirty
 * worktrees require manual resolution; pending/running scheduled commits
 * prevent advancing, even when opening through a repository path alias. */
int ghm_repo_pull(GhmContext *context, const char *repository_path,
                  const char *client_id, GhmPullOutcome *outcome, GhmError *error);

#endif
