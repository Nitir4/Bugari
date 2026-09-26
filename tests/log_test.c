#include <ghm/log.h>

#include <dirent.h>
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

int main(void)
{
    char root[] = "/tmp/ghm-log-test-XXXXXX";
    char *content = NULL;
    GhmError error = {0};
    int result = 1;
    if (mkdtemp(root) == NULL || setenv("XDG_STATE_HOME", root, 1) != 0 ||
        ghm_log_tail(4096, &content, &error) != 0 || content == NULL || content[0] != '\0') goto done;
    free(content); content = NULL;
    ghm_log_event(GHM_LOG_INFO, "test", "created", "detail with \"quotes\"");
    if (ghm_log_tail(4096, &content, &error) != 0 || content == NULL ||
        strstr(content, "\"level\":\"INFO\"") == NULL ||
        strstr(content, "\"component\":\"test\"") == NULL ||
        strstr(content, "detail with \\\"quotes\\\"") == NULL) goto done;
    result = 0;
done:
    if (result != 0) fprintf(stderr, "Log test failed: %s\n", error.message);
    free(content);
    if (remove_tree(root) != 0) fprintf(stderr, "Could not remove log test directory\n");
    return result;
}
