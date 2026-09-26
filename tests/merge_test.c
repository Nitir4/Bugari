#include <ghm/branch.h>
#include <ghm/commit.h>

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

static int write_file(const char *root, const char *name, const char *text)
{
    char path[512];
    if (snprintf(path, sizeof(path), "%s/%s", root, name) >= (int)sizeof(path)) return -1;
    FILE *file = fopen(path, "w");
    if (file == NULL) return -1;
    int wrote = fputs(text, file);
    return fclose(file) == 0 && wrote != EOF ? 0 : -1;
}

int main(void)
{
    char root[] = "/tmp/ghm-merge-test-XXXXXX";
    char repo_path[512], data_path[512], oid[GHM_OID_HEX_CAPACITY] = {0};
    char before[GHM_OID_HEX_CAPACITY] = {0};
    git_repository *repository = NULL;
    git_config *config = NULL;
    git_reference *head = NULL;
    char *main_branch = NULL;
    GhmContext *context = NULL;
    GhmError error = {0};
    GhmMergeOutcome outcome = GHM_MERGE_UP_TO_DATE;
    GhmStatus status = {0};
    const GhmCommitSignature signature = {"Merge Test", "merge@example.invalid", 1790000000, 0};
    int result = 1;
    if (mkdtemp(root) == NULL || setenv("XDG_STATE_HOME", root, 1) != 0 ||
        snprintf(repo_path, sizeof(repo_path), "%s/repo", root) >= (int)sizeof(repo_path) ||
        snprintf(data_path, sizeof(data_path), "%s/data", root) >= (int)sizeof(data_path) ||
        ghm_context_open(data_path, &context, &error) != 0 ||
        git_repository_init(&repository, repo_path, 0) < 0 ||
        git_repository_config(&config, repository) < 0 ||
        git_config_set_string(config, "user.name", signature.name) < 0 ||
        git_config_set_string(config, "user.email", signature.email) < 0 ||
        ghm_repo_register(context, repo_path, &error) != 0 ||
        write_file(repo_path, "base.txt", "base\n") != 0 ||
        ghm_commit_now(repo_path, "base", &signature, &signature, oid, &error) != 0 ||
        git_repository_head(&head, repository) < 0) goto done;
    main_branch = strdup(git_reference_shorthand(head));
    if (main_branch == NULL || ghm_branch_create(repo_path, "fast-forward", &error) != 0 ||
        ghm_branch_checkout(context, repo_path, "fast-forward", &error) != 0 ||
        write_file(repo_path, "fast-forward.txt", "ahead\n") != 0 ||
        ghm_commit_now(repo_path, "ahead", &signature, &signature, oid, &error) != 0 ||
        ghm_branch_checkout(context, repo_path, main_branch, &error) != 0 ||
        ghm_branch_merge(context, repo_path, "fast-forward", &outcome, oid, &error) != 0 ||
        outcome != GHM_MERGE_FAST_FORWARDED ||
        ghm_branch_merge(context, repo_path, "fast-forward", &outcome, oid, &error) != 0 ||
        outcome != GHM_MERGE_UP_TO_DATE ||
        ghm_branch_create(repo_path, "feature", &error) != 0 ||
        ghm_branch_checkout(context, repo_path, "feature", &error) != 0 ||
        write_file(repo_path, "feature.txt", "feature\n") != 0 ||
        ghm_commit_now(repo_path, "feature", &signature, &signature, oid, &error) != 0 ||
        ghm_branch_checkout(context, repo_path, main_branch, &error) != 0 ||
        write_file(repo_path, "main.txt", "main\n") != 0 ||
        ghm_commit_now(repo_path, "main", &signature, &signature, oid, &error) != 0 ||
        ghm_branch_merge(context, repo_path, "feature", &outcome, oid, &error) != 0 ||
        outcome != GHM_MERGE_COMMITTED ||
        ghm_repo_status(repo_path, &status, &error) != 0 || status.count != 0) goto done;
    ghm_status_free(&status);
    git_reference_free(head); head = NULL;
    if (git_repository_head(&head, repository) < 0 ||
        git_reference_target(head) == NULL ||
        git_oid_tostr(before, sizeof(before), git_reference_target(head)) == NULL ||
        ghm_branch_create(repo_path, "conflict", &error) != 0 ||
        ghm_branch_checkout(context, repo_path, "conflict", &error) != 0 ||
        write_file(repo_path, "base.txt", "theirs\n") != 0 ||
        ghm_commit_now(repo_path, "theirs", &signature, &signature, oid, &error) != 0 ||
        ghm_branch_checkout(context, repo_path, main_branch, &error) != 0 ||
        write_file(repo_path, "base.txt", "ours\n") != 0 ||
        ghm_commit_now(repo_path, "ours", &signature, &signature, before, &error) != 0 ||
        ghm_branch_merge(context, repo_path, "conflict", &outcome, oid, &error) == 0 ||
        strstr(error.message, "base.txt") == NULL ||
        ghm_repo_status(repo_path, &status, &error) != 0 || status.count != 0) goto done;
    git_reference_free(head); head = NULL;
    if (git_repository_head(&head, repository) < 0 ||
        git_reference_target(head) == NULL ||
        strcmp(git_oid_tostr(oid, sizeof(oid), git_reference_target(head)), before) != 0)
        goto done;
    result = 0;
done:
    if (result != 0) fprintf(stderr, "Merge test failed: %s\n", error.message);
    ghm_status_free(&status);
    free(main_branch);
    git_reference_free(head);
    git_config_free(config);
    git_repository_free(repository);
    ghm_context_close(context);
    if (remove_tree(root) != 0) fprintf(stderr, "Could not remove merge test directory\n");
    return result;
}
