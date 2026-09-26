#ifndef GHM_GITHUB_REPOS_H
#define GHM_GITHUB_REPOS_H

#include <ghm/github.h>

typedef struct {
    int64_t github_id;
    char *full_name;
    char *clone_url;
    char *default_branch;
    char *description;
    char *html_url;
    int64_t stargazers_count;
    int64_t forks_count;
    int is_private;
    int is_archived;
} GhmGitHubRepo;

typedef struct {
    GhmGitHubRepo *items;
    size_t count;
} GhmGitHubRepoList;

/* Lists all repositories visible to the signed-in account. Network call. */
int ghm_github_repos_list(const char *client_id, GhmGitHubRepoList *out, GhmError *error);
void ghm_github_repos_free(GhmGitHubRepoList *list);

typedef struct {
    char *name;
    char *head_oid;
    int is_protected;
} GhmGitHubBranch;

typedef struct {
    GhmGitHubBranch *items;
    size_t count;
} GhmGitHubBranchList;

/* Reads GitHub's remote branch metadata; does not fetch or alter local Git. */
int ghm_github_branches_list(const char *client_id, const char *full_name,
                             GhmGitHubBranchList *out, GhmError *error);
void ghm_github_branches_free(GhmGitHubBranchList *list);

#endif
