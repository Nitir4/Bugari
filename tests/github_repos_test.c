#include <ghm/ghm.h>
#include <ghm/github_repos.h>
#include "github/github_repos.h"
#include "core/remote.h"

#include <dirent.h>
#include <git2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int remove_tree(const char *path)
{
    DIR *directory = opendir(path);
    struct dirent *entry;
    if (directory == NULL) return unlink(path);
    while ((entry = readdir(directory)) != NULL) {
        char child[512];
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child) ||
            remove_tree(child) != 0) { closedir(directory); return -1; }
    }
    closedir(directory);
    return rmdir(path);
}

static int initial_commit(git_repository *repository, const char *root)
{
    char filename[512];
    FILE *file = NULL;
    git_index *index = NULL;
    git_tree *tree = NULL;
    git_signature *signature = NULL;
    git_oid tree_oid, commit_oid;
    int result = -1;
    if (snprintf(filename, sizeof(filename), "%s/README.md", root) >= (int)sizeof(filename)) return -1;
    file = fopen(filename, "w");
    if (file == NULL) goto done;
    int written = fputs("# clone test\n", file);
    int closed = fclose(file);
    file = NULL;
    if (written == EOF || closed != 0) goto done;
    if (git_repository_index(&index, repository) < 0 ||
        git_index_add_bypath(index, "README.md") < 0 || git_index_write(index) < 0 ||
        git_index_write_tree(&tree_oid, index) < 0 ||
        git_tree_lookup(&tree, repository, &tree_oid) < 0 ||
        git_signature_new(&signature, "Test", "test@example.invalid", 1700000000, 0) < 0 ||
        git_commit_create(&commit_oid, repository, "HEAD", signature, signature,
                          NULL, "initial", tree, 0, NULL) < 0) goto done;
    result = 0;
done:
    if (file != NULL) fclose(file);
    git_signature_free(signature);
    git_tree_free(tree);
    git_index_free(index);
    return result;
}

int main(void)
{
    char root[] = "/tmp/ghm-github-repos-test-XXXXXX";
    char source_path[512], clone_path[512];
    GhmGitHubRepoList repos = {0};
    GhmGitHubBranchList branches = {0};
    GhmError error = {0};
    size_t page_count = 0;
    git_repository *source = NULL;
    git_repository *clone = NULL;
    git_remote *origin = NULL;
    int git_initialized = 0;
    int result = 1;
    static const char page[] =
        "[{\"id\":42,\"full_name\":\"owner/alpha\",\"clone_url\":"
        "\"https://github.com/owner/alpha.git\",\"default_branch\":\"main\","
        "\"private\":false,\"description\":\"Alpha project\","
        "\"stargazers_count\":7,\"forks_count\":2,\"archived\":true},{\"id\":43,\"full_name\":\"owner/beta\","
        "\"clone_url\":\"https://github.com/owner/beta.git\","
        "\"default_branch\":\"trunk\",\"private\":true}]";
    static const char branch_page[] =
        "[{\"name\":\"main\",\"protected\":true,\"commit\":{"
        "\"sha\":\"0123456789abcdef0123456789abcdef01234567\"}},{"
        "\"name\":\"feature/ui\",\"protected\":false,\"commit\":{"
        "\"sha\":\"abcdef0123456789abcdef0123456789abcdef01\"}}]";
    if (mkdtemp(root) == NULL ||
        snprintf(source_path, sizeof(source_path), "%s/source", root) >= (int)sizeof(source_path) ||
        snprintf(clone_path, sizeof(clone_path), "%s/clone", root) >= (int)sizeof(clone_path) ||
        ghm_github_repos_parse_page(page, &repos, &page_count, &error) != 0 ||
        page_count != 2 || repos.count != 2 || repos.items[0].github_id != 42 ||
        strcmp(repos.items[1].full_name, "owner/beta") != 0 || !repos.items[1].is_private ||
        strcmp(repos.items[0].description, "Alpha project") != 0 ||
        repos.items[0].stargazers_count != 7 || repos.items[0].forks_count != 2 ||
        !repos.items[0].is_archived ||
        ghm_github_branches_parse_page(branch_page, &branches, &page_count, &error) != 0 ||
        page_count != 2 || branches.count != 2 || !branches.items[0].is_protected ||
        strcmp(branches.items[1].name, "feature/ui") != 0 ||
        ghm_github_branches_parse_page("{\"message\":\"error\"}",
                                       &branches, &page_count, &error) == 0 ||
        ghm_github_repos_parse_page("{\"message\":\"error\"}", &repos, &page_count, &error) == 0) goto done;
    /* libgit2 is initialized directly so this test never opens user state. */
    if (git_libgit2_init() < 0) goto done;
    git_initialized = 1;
    if (git_repository_init(&source, source_path, 0) < 0 ||
        initial_commit(source, source_path) != 0 ||
        ghm_remote_clone_url(source_path, clone_path, NULL, &error) != 0 ||
        git_repository_open(&clone, clone_path) < 0 ||
        git_remote_lookup(&origin, clone, "origin") < 0 ||
        strcmp(git_remote_url(origin), source_path) != 0) goto done;
    result = 0;
done:
    if (result != 0) fprintf(stderr, "GitHub repo test failed: %s\n", error.message);
    git_remote_free(origin);
    git_repository_free(clone);
    git_repository_free(source);
    if (git_initialized) git_libgit2_shutdown();
    ghm_github_repos_free(&repos);
    ghm_github_branches_free(&branches);
    if (remove_tree(root) != 0) fprintf(stderr, "Could not remove GitHub repo test directory\n");
    return result;
}
