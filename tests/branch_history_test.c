#include <ghm/branch.h>
#include <ghm/commit.h>
#include <ghm/history.h>
#include <ghm/scheduler.h>

#include <dirent.h>
#include <git2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

static int write_text(const char *directory, const char *contents)
{
    char path[512];
    FILE *file;
    int result;
    if (snprintf(path, sizeof(path), "%s/a.txt", directory) >= (int)sizeof(path)) return -1;
    file = fopen(path, "w");
    if (file == NULL) return -1;
    result = fputs(contents, file);
    return fclose(file) == 0 && result != EOF ? 0 : -1;
}

int main(void)
{
    char root[] = "/tmp/ghm-branch-history-test-XXXXXX";
    char repo_path[512], data_path[512];
    char first_oid[GHM_OID_HEX_CAPACITY] = {0};
    char feature_oid[GHM_OID_HEX_CAPACITY] = {0};
    char rewritten_oid[GHM_OID_HEX_CAPACITY] = {0};
    char *initial_branch = NULL;
    git_repository *repository = NULL;
    git_reference *head = NULL;
    GhmContext *context = NULL;
    GhmHistory history = {0};
    GhmBranchList branches = {0};
    GhmError error = {0};
    const GhmCommitSignature signature = {"Branch Test", "branch@example.invalid", 1790000000, 330};
    int64_t job_id = 0;
    int result = 1, found_feature = 0;
    if (mkdtemp(root) == NULL ||
        setenv("XDG_STATE_HOME", root, 1) != 0 ||
        snprintf(repo_path, sizeof(repo_path), "%s/repo", root) >= (int)sizeof(repo_path) ||
        snprintf(data_path, sizeof(data_path), "%s/data", root) >= (int)sizeof(data_path) ||
        ghm_context_open(data_path, &context, &error) != 0 ||
        git_repository_init(&repository, repo_path, 0) < 0 ||
        ghm_repo_register(context, repo_path, &error) != 0 ||
        write_text(repo_path, "initial\n") != 0 ||
        ghm_commit_now(repo_path, "initial", &signature, &signature, first_oid, &error) != 0 ||
        git_repository_head(&head, repository) < 0) goto done;
    initial_branch = strdup(git_reference_shorthand(head));
    if (initial_branch == NULL ||
        ghm_branch_create(repo_path, "feature", &error) != 0 ||
        ghm_branch_list(repo_path, &branches, &error) != 0) goto done;
    for (size_t i = 0; i < branches.count; ++i)
        if (strcmp(branches.items[i].name, "feature") == 0) found_feature = 1;
    if (!found_feature || ghm_branch_checkout(context, repo_path, "feature", &error) != 0 ||
        write_text(repo_path, "feature\n") != 0 ||
        ghm_commit_now(repo_path, "feature commit\n\nbody", &signature, &signature, feature_oid, &error) != 0 ||
        ghm_commit_get_history(repo_path, 20, &history, &error) != 0 ||
        history.count != 2 || strcmp(history.items[0].oid, feature_oid) != 0 ||
        strcmp(history.items[0].summary, "feature commit") != 0 ||
        strcmp(history.items[0].message, "feature commit\n\nbody") != 0 ||
        history.items[0].author_timestamp != signature.timestamp ||
        history.items[0].committer_offset != signature.offset_minutes ||
        ghm_branch_checkout(context, repo_path, initial_branch, &error) != 0) goto done;
    ghm_history_free(&history);
    if (ghm_commit_get_history(repo_path, 20, &history, &error) != 0 ||
        history.count != 1 || strcmp(history.items[0].oid, first_oid) != 0 ||
        write_text(repo_path, "pending change\n") != 0 ||
        ghm_schedule_add(context, repo_path, "pending", &signature, &signature,
                         (int64_t)time(NULL) + 3600, &job_id, &error) != 0 ||
        ghm_branch_checkout(context, repo_path, "feature", &error) == 0 ||
        strstr(error.message, "pending scheduled jobs") == NULL ||
        ghm_commit_rewrite_head(context, repo_path, first_oid, "reworded",
                                1790000001, 330, 1790000002, 330,
                                1, rewritten_oid, &error) == 0 ||
        ghm_schedule_cancel(context, job_id, &error) != 0 ||
        write_text(repo_path, "initial\n") != 0 ||
        ghm_commit_rewrite_head(context, repo_path, first_oid, "reworded",
                                1790000001, 330, 1790000002, 330,
                                0, rewritten_oid, &error) == 0 ||
        ghm_commit_rewrite_head(context, repo_path, first_oid, "reworded",
                                1790000001, 330, 1790000002, 330,
                                1, rewritten_oid, &error) != 0 ||
        strcmp(rewritten_oid, first_oid) == 0) goto done;
    ghm_history_free(&history);
    if (ghm_commit_get_history(repo_path, 20, &history, &error) != 0 ||
        history.count != 1 || strcmp(history.items[0].oid, rewritten_oid) != 0 ||
        strcmp(history.items[0].summary, "reworded") != 0 ||
        history.items[0].author_timestamp != 1790000001 ||
        history.items[0].committer_timestamp != 1790000002 ||
        ghm_commit_rewrite_head(context, repo_path, first_oid, "stale",
                                1790000001, 330, 1790000002, 330,
                                1, first_oid, &error) == 0) goto done;
    result = 0;
done:
    if (result != 0) fprintf(stderr, "Branch/history test failed: %s\n", error.message);
    ghm_history_free(&history);
    ghm_branch_list_free(&branches);
    free(initial_branch);
    git_reference_free(head);
    git_repository_free(repository);
    ghm_context_close(context);
    if (remove_tree(root) != 0) fprintf(stderr, "Could not remove branch test directory\n");
    return result;
}
