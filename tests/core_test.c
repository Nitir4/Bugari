#include <ghm/ghm.h>

#include <dirent.h>
#include <git2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int remove_tree(const char *path)
{
    DIR *directory = opendir(path);
    if (directory == NULL) return unlink(path);
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        char *child;
        size_t length;
        int result;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        length = strlen(path) + strlen(entry->d_name) + 2;
        child = malloc(length);
        if (child == NULL) { closedir(directory); return -1; }
        (void)snprintf(child, length, "%s/%s", path, entry->d_name);
        result = remove_tree(child);
        free(child);
        if (result != 0) { closedir(directory); return -1; }
    }
    closedir(directory);
    return rmdir(path);
}

static int check(int expression, const char *message)
{
    if (expression) return 0;
    fprintf(stderr, "FAIL: %s\n", message);
    return -1;
}

int main(void)
{
    char temporary[] = "/tmp/ghm-test-XXXXXX";
    char repo_path[sizeof(temporary) + 12];
    char discovered_path[sizeof(temporary) + 24];
    char file_path[sizeof(temporary) + 24];
    GhmContext *context = NULL;
    git_repository *git_repo = NULL;
    GhmRepositoryList list = {0};
    GhmStatus status = {0};
    git_index *index = NULL;
    GhmError error = {0};
    char *setting = NULL;
    FILE *file;
    int result = 1;
    if (mkdtemp(temporary) == NULL) return 1;
    (void)snprintf(repo_path, sizeof(repo_path), "%s/local-repo", temporary);
    (void)snprintf(discovered_path, sizeof(discovered_path), "%s/repos/discovered", temporary);
    (void)snprintf(file_path, sizeof(file_path), "%s/README.md", repo_path);
    if (ghm_context_open(temporary, &context, &error) != 0) goto done;
    if (ghm_setting_set(context, "github_client_id", "test_client", &error) != 0 ||
        ghm_setting_get(context, "github_client_id", &setting, &error) != 0) goto done;
    if (check(setting != NULL && strcmp(setting, "test_client") == 0, "persistent setting") != 0) goto done;
    free(setting);
    setting = NULL;
    if (git_repository_init(&git_repo, repo_path, 0) != 0) goto done;
    if (ghm_repo_register(context, repo_path, &error) != 0) goto done;
    if (ghm_repo_list(context, &list, &error) != 0) goto done;
    if (check(list.count == 1 && strcmp(list.items[0].name, "local-repo") == 0,
              "repository is persisted") != 0) goto done;
    ghm_repo_list_free(&list);
    file = fopen(file_path, "w");
    if (file == NULL) goto done;
    int write_result = fputs("# Test\n", file);
    int close_result = fclose(file);
    if (write_result == EOF || close_result != 0) goto done;
    if (ghm_repo_status(repo_path, &status, &error) != 0) goto done;
    if (check(status.count == 1 && status.items[0].kind == GHM_STATUS_UNTRACKED &&
              strcmp(status.items[0].path, "README.md") == 0, "untracked file status") != 0) goto done;
    ghm_status_free(&status);
    if (git_repository_index(&index, git_repo) != 0 ||
        git_index_add_bypath(index, "README.md") != 0 || git_index_write(index) != 0) goto done;
    if (ghm_repo_status(repo_path, &status, &error) != 0) goto done;
    if (check(status.count == 1 && status.items[0].kind == GHM_STATUS_ADDED,
              "staged file status") != 0) goto done;
    ghm_status_free(&status);
    git_index_free(index);
    index = NULL;
    git_repository_free(git_repo);
    git_repo = NULL;
    ghm_context_close(context);
    context = NULL;
    if (ghm_context_open(temporary, &context, &error) != 0 ||
        ghm_repo_list(context, &list, &error) != 0) goto done;
    if (check(list.count == 1, "repository persists across restart") != 0) goto done;
    if (ghm_setting_get(context, "github_client_id", &setting, &error) != 0) goto done;
    if (check(setting != NULL && strcmp(setting, "test_client") == 0,
              "setting persists across restart") != 0) goto done;
    free(setting);
    setting = NULL;
    ghm_repo_list_free(&list);
    if (ghm_repo_discover(context, &error) != 0 ||
        git_repository_init(&git_repo, discovered_path, 0) != 0 ||
        ghm_repo_discover(context, &error) != 0 ||
        ghm_repo_list(context, &list, &error) != 0) goto done;
    if (check(list.count == 2, "repository discovery") != 0) goto done;
    result = 0;
done:
    if (result != 0 && error.message[0] != '\0') fprintf(stderr, "%s\n", error.message);
    ghm_status_free(&status);
    ghm_repo_list_free(&list);
    free(setting);
    git_index_free(index);
    git_repository_free(git_repo);
    ghm_context_close(context);
    if (remove_tree(temporary) != 0) fprintf(stderr, "Could not remove test directory\n");
    return result;
}
