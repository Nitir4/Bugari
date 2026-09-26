#include <ghm/commit.h>
#include <ghm/ghm.h>

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

static int write_text(const char *root, const char *name, const char *value)
{
    char filename[512];
    FILE *file;
    int written;
    if (snprintf(filename, sizeof(filename), "%s/%s", root, name) >= (int)sizeof(filename)) return -1;
    file = fopen(filename, "w");
    if (file == NULL) return -1;
    written = fputs(value, file);
    return fclose(file) == 0 && written != EOF ? 0 : -1;
}

int main(void)
{
    char root[] = "/tmp/ghm-commit-now-test-XXXXXX";
    char first_oid[GHM_OID_HEX_CAPACITY] = {0};
    char second_oid[GHM_OID_HEX_CAPACITY] = {0};
    char ignored[GHM_OID_HEX_CAPACITY] = {0};
    git_repository *repository = NULL;
    git_commit *first = NULL;
    git_commit *second = NULL;
    git_oid oid;
    GhmStatus status = {0};
    GhmError error = {0};
    const GhmCommitSignature author = {"Author", "author@example.invalid", 1790000000, 330};
    const GhmCommitSignature committer = {"Committer", "committer@example.invalid", 1790003600, 330};
    int result = 1;
    if (mkdtemp(root) == NULL || git_libgit2_init() < 0 ||
        git_repository_init(&repository, root, 0) < 0 ||
        write_text(root, "a.txt", "first\n") != 0 ||
        ghm_commit_now(root, "initial", &author, &committer, first_oid, &error) != 0 ||
        git_oid_fromstr(&oid, first_oid) < 0 || git_commit_lookup(&first, repository, &oid) < 0 ||
        git_commit_parentcount(first) != 0 ||
        git_commit_author(first)->when.time != author.timestamp ||
        git_commit_committer(first)->when.time != committer.timestamp ||
        strcmp(git_commit_message(first), "initial") != 0 ||
        write_text(root, "a.txt", "second\n") != 0 ||
        write_text(root, "b.txt", "new\n") != 0 ||
        ghm_commit_now(root, "second", &author, &committer, second_oid, &error) != 0 ||
        git_oid_fromstr(&oid, second_oid) < 0 || git_commit_lookup(&second, repository, &oid) < 0 ||
        git_commit_parentcount(second) != 1 ||
        git_oid_streq(git_commit_parent_id(second, 0), first_oid) != 0 ||
        ghm_repo_status(root, &status, &error) != 0 || status.count != 0 ||
        ghm_commit_now(root, "empty", &author, &committer, ignored, &error) == 0 ||
        ignored[0] != '\0') goto done;
    result = 0;
done:
    if (result != 0) fprintf(stderr, "Immediate commit test failed: %s\n", error.message);
    ghm_status_free(&status);
    git_commit_free(second);
    git_commit_free(first);
    git_repository_free(repository);
    git_libgit2_shutdown();
    if (remove_tree(root) != 0) fprintf(stderr, "Could not remove commit test directory\n");
    return result;
}
