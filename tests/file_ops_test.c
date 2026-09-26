#include <ghm/file_ops.h>

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
            remove_tree(child) != 0) {
            closedir(directory);
            return -1;
        }
    }
    closedir(directory);
    return rmdir(path);
}

static int expect(int condition, const char *message)
{
    if (condition) return 0;
    fprintf(stderr, "FAIL: %s\n", message);
    return -1;
}

int main(void)
{
    char root[] = "/tmp/ghm-file-ops-test-XXXXXX";
    char link_path[512];
    git_repository *repository = NULL;
    GhmFileVersion version = {0}, newer = {0};
    GhmError error = {0};
    char *text = NULL;
    size_t length = 0;
    int result = 1;
    if (mkdtemp(root) == NULL || git_libgit2_init() < 0) return 1;
    if (git_repository_init(&repository, root, 0) < 0 ||
        ghm_file_create(root, "note.txt", &error) != 0 ||
        ghm_file_read_text(root, "note.txt", &text, &length, &version, &error) != 0 ||
        expect(length == 0 && text[0] == '\0', "new file is empty") != 0) goto done;
    free(text); text = NULL;
    if (ghm_file_save_text(root, "note.txt", "hello\n", 6, &version, &newer, &error) != 0 ||
        ghm_file_save_text(root, "note.txt", "stale", 5, &version, &version, &error) == 0 ||
        ghm_file_read_text(root, "note.txt", &text, &length, &version, &error) != 0 ||
        expect(length == 6 && strcmp(text, "hello\n") == 0, "save preserves content and rejects stale version") != 0)
        goto done;
    free(text); text = NULL;
    if (ghm_directory_create(root, "docs", &error) != 0 ||
        ghm_file_create(root, "docs/guide.txt", &error) != 0 ||
        ghm_file_rename(root, "docs/guide.txt", "docs/renamed.txt", &error) != 0 ||
        ghm_file_rename(root, "docs/renamed.txt", "note.txt", &error) == 0 ||
        ghm_file_delete(root, "docs/renamed.txt", &error) != 0 ||
        ghm_file_delete(root, "docs", &error) == 0 ||
        ghm_file_create(root, "../outside.txt", &error) == 0 ||
        ghm_file_create(root, ".git/config", &error) == 0) goto done;
    if (snprintf(link_path, sizeof(link_path), "%s/link", root) >= (int)sizeof(link_path) ||
        symlink("/tmp", link_path) != 0 ||
        ghm_file_create(root, "link/ghm-unsafe-test", &error) == 0) goto done;
    result = 0;
done:
    if (result != 0 && error.message[0] != '\0') fprintf(stderr, "%s\n", error.message);
    free(text);
    git_repository_free(repository);
    git_libgit2_shutdown();
    if (remove_tree(root) != 0) fprintf(stderr, "Could not remove test repository\n");
    return result;
}
