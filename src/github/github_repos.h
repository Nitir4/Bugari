#ifndef GHM_GITHUB_REPOS_PRIVATE_H
#define GHM_GITHUB_REPOS_PRIVATE_H

#include <ghm/github_repos.h>

/* Parses one page and appends it to out; used by offline parser tests. */
int ghm_github_repos_parse_page(const char *json, GhmGitHubRepoList *out,
                                size_t *page_count, GhmError *error);
int ghm_github_branches_parse_page(const char *json, GhmGitHubBranchList *out,
                                   size_t *page_count, GhmError *error);

#endif
