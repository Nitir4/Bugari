#include <ghm/commit.h>
#include <ghm/diff.h>

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

static int write_text(const char *root, const char *name, const char *contents)
{
    char path[512];
    FILE *file;
    int result;
    if (snprintf(path, sizeof(path), "%s/%s", root, name) >= (int)sizeof(path)) return -1;
    file = fopen(path, "w");
    if (file == NULL) return -1;
    result = fputs(contents, file);
    return fclose(file) == 0 && result != EOF ? 0 : -1;
}

int main(void)
{
    char root[] = "/tmp/ghm-diff-test-XXXXXX";
    char repo_path[512];
    char oid[GHM_OID_HEX_CAPACITY] = {0};
    char *patch = NULL;
    git_repository *repository = NULL;
    GhmError error = {0};
    const GhmCommitSignature signature = {"Diff Test", "diff@example.invalid", 1790000000, 0};
    int result = 1;
    if (mkdtemp(root) == NULL ||
        snprintf(repo_path, sizeof(repo_path), "%s/repo", root) >= (int)sizeof(repo_path) ||
        git_libgit2_init() < 0 || git_repository_init(&repository, repo_path, 0) < 0 ||
        write_text(repo_path, "a.txt", "first\n") != 0 ||
        ghm_commit_now(repo_path, "first", &signature, &signature, oid, &error) != 0 ||
        write_text(repo_path, "a.txt", "second\n") != 0 ||
        ghm_repo_diff_file(repo_path, "a.txt", 0, &patch, &error) != 0 ||
        strstr(patch, "+second") == NULL || strstr(patch, "-first") == NULL) goto done;
    free(patch); patch = NULL;
    if (ghm_repo_stage_path(repo_path, "a.txt", &error) != 0 ||
        ghm_repo_diff_file(repo_path, "a.txt", 1, &patch, &error) != 0 ||
        strstr(patch, "+second") == NULL ||
        write_text(repo_path, "new.txt", "untracked body\n") != 0) goto done;
    free(patch); patch = NULL;
    if (ghm_repo_diff_file(repo_path, "new.txt", 0, &patch, &error) != 0 ||
        strstr(patch, "+untracked body") == NULL) goto done;
    result = 0;
done:
    if (result != 0) fprintf(stderr, "Diff test failed: %s\n", error.message);
    free(patch);
    git_repository_free(repository);
    git_libgit2_shutdown();
    if (remove_tree(root) != 0) fprintf(stderr, "Could not remove diff test directory\n");
    return result;
}
