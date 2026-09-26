#include <ghm/commit.h>
#include <ghm/remote.h>
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

static int write_text(const char *root, const char *contents)
{
    char filename[512];
    FILE *file;
    int written;
    if (snprintf(filename, sizeof(filename), "%s/file.txt", root) >= (int)sizeof(filename)) return -1;
    file = fopen(filename, "w");
    if (file == NULL) return -1;
    written = fputs(contents, file);
    return fclose(file) == 0 && written != EOF ? 0 : -1;
}

int main(void)
{
    char root[] = "/tmp/ghm-remote-push-test-XXXXXX";
    char source_path[512], bare_path[512], competitor_path[512];
    char first_oid[GHM_OID_HEX_CAPACITY] = {0};
    char second_oid[GHM_OID_HEX_CAPACITY] = {0};
    char competitor_oid[GHM_OID_HEX_CAPACITY] = {0};
    char rejected_oid[GHM_OID_HEX_CAPACITY] = {0};
    char pushed_oid[GHM_OID_HEX_CAPACITY] = {0};
    git_repository *source = NULL, *bare = NULL, *competitor = NULL;
    git_remote *origin = NULL;
    git_reference *head = NULL, *bare_head = NULL;
    GhmError error = {0};
    const GhmCommitSignature signature = {"Push Test", "push@example.invalid", 1790000000, 0};
    int result = 1;
    int initialized = 0;
    if (mkdtemp(root) == NULL ||
        snprintf(source_path, sizeof(source_path), "%s/source", root) >= (int)sizeof(source_path) ||
        snprintf(bare_path, sizeof(bare_path), "%s/bare.git", root) >= (int)sizeof(bare_path) ||
        snprintf(competitor_path, sizeof(competitor_path), "%s/competitor", root) >= (int)sizeof(competitor_path) ||
        git_libgit2_init() < 0) goto done;
    initialized = 1;
    if (git_repository_init(&source, source_path, 0) < 0 ||
        git_repository_init(&bare, bare_path, 1) < 0 ||
        write_text(source_path, "first\n") != 0 ||
        ghm_commit_now(source_path, "first", &signature, &signature, first_oid, &error) != 0 ||
        git_remote_create(&origin, source, "origin", bare_path) < 0 ||
        ghm_repo_push(source_path, NULL, pushed_oid, &error) != 0 ||
        strcmp(pushed_oid, first_oid) != 0 ||
        git_repository_head(&head, source) < 0 ||
        git_reference_lookup(&bare_head, bare, git_reference_name(head)) < 0 ||
        git_oid_streq(git_reference_target(bare_head), first_oid) != 0) goto done;
    git_reference_free(bare_head); bare_head = NULL;
    if (write_text(source_path, "second\n") != 0 ||
        ghm_commit_now(source_path, "second", &signature, &signature, second_oid, &error) != 0 ||
        ghm_repo_push(source_path, NULL, pushed_oid, &error) != 0 ||
        strcmp(pushed_oid, second_oid) != 0 ||
        ghm_remote_clone_url(bare_path, competitor_path, NULL, &error) != 0 ||
        git_repository_open(&competitor, competitor_path) < 0 ||
        write_text(competitor_path, "competitor\n") != 0 ||
        ghm_commit_now(competitor_path, "competitor", &signature, &signature,
                       competitor_oid, &error) != 0 ||
        ghm_repo_push(competitor_path, NULL, pushed_oid, &error) != 0 ||
        write_text(source_path, "diverged\n") != 0 ||
        ghm_commit_now(source_path, "diverged", &signature, &signature,
                       rejected_oid, &error) != 0) goto done;
    rejected_oid[0] = '\0';
    if (ghm_repo_push(source_path, NULL, rejected_oid, &error) == 0 ||
        rejected_oid[0] != '\0' ||
        git_reference_lookup(&bare_head, bare, git_reference_name(head)) < 0 ||
        git_oid_streq(git_reference_target(bare_head), competitor_oid) != 0) goto done;
    result = 0;
done:
    if (result != 0) fprintf(stderr, "Remote push test failed: %s\n", error.message);
    git_reference_free(bare_head);
    git_reference_free(head);
    git_remote_free(origin);
    git_repository_free(competitor);
    git_repository_free(bare);
    git_repository_free(source);
    if (initialized) git_libgit2_shutdown();
    if (remove_tree(root) != 0) fprintf(stderr, "Could not remove remote test directory\n");
    return result;
}
