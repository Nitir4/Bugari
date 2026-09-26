#include <ghm/commit.h>
#include <ghm/ghm.h>

#include <dirent.h>
#include <git2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int expect(int condition, const char *message)
{
    if (condition) return 0;
    fprintf(stderr, "FAIL: %s\n", message);
    return -1;
}

static int write_file(const char *root, const char *name, const char *value)
{
    char path[512];
    FILE *file;
    int wrote;
    if (snprintf(path, sizeof(path), "%s/%s", root, name) >= (int)sizeof(path)) return -1;
    file = fopen(path, "w");
    if (file == NULL) return -1;
    wrote = fputs(value, file);
    if (fclose(file) != 0) return -1;
    return wrote == EOF ? -1 : 0;
}

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

static const GhmStatusEntry *find_status(const GhmStatus *status, const char *path)
{
    for (size_t i = 0; i < status->count; ++i)
        if (strcmp(status->items[i].path, path) == 0) return &status->items[i];
    return NULL;
}

static int tree_has(git_repository *repo, const char *hex_oid, const char *path)
{
    git_oid oid;
    git_tree *tree = NULL;
    git_tree_entry *entry = NULL;
    int found = 0;
    if (git_oid_fromstr(&oid, hex_oid) < 0 || git_tree_lookup(&tree, repo, &oid) < 0) return -1;
    found = git_tree_entry_bypath(&entry, tree, path) == 0;
    git_tree_entry_free(entry);
    git_tree_free(tree);
    return found;
}

static int tree_blob_equals(git_repository *repo, const char *hex_oid,
                            const char *path, const char *expected)
{
    git_oid oid;
    git_tree *tree = NULL;
    git_tree_entry *entry = NULL;
    git_blob *blob = NULL;
    int same = 0;
    if (git_oid_fromstr(&oid, hex_oid) < 0 ||
        git_tree_lookup(&tree, repo, &oid) < 0 ||
        git_tree_entry_bypath(&entry, tree, path) < 0 ||
        git_blob_lookup(&blob, repo, git_tree_entry_id(entry)) < 0) goto done;
    same = git_blob_rawsize(blob) == (git_object_size_t)strlen(expected) &&
           memcmp(git_blob_rawcontent(blob), expected, strlen(expected)) == 0;
done:
    git_blob_free(blob);
    git_tree_entry_free(entry);
    git_tree_free(tree);
    return same;
}

int main(void)
{
    char root[] = "/tmp/ghm-staging-test-XXXXXX";
    char initial_oid[GHM_OID_HEX_CAPACITY] = {0};
    char commit_oid[GHM_OID_HEX_CAPACITY] = {0};
    char tree1[GHM_OID_HEX_CAPACITY] = {0};
    char tree2[GHM_OID_HEX_CAPACITY] = {0};
    char unsafe_tree[GHM_OID_HEX_CAPACITY] = {0};
    char path[512];
    char renamed_path[512];
    git_repository *repo = NULL;
    GhmStatus status = {0};
    GhmFileList files = {0};
    GhmError error = {0};
    const GhmCommitSignature identity = {"Test", "test@example.invalid", 1790000000, 330};
    int result = 1;
    if (mkdtemp(root) == NULL || git_libgit2_init() < 0) return 1;
    if (git_repository_init(&repo, root, 0) < 0 ||
        write_file(root, "a.txt", "base\n") != 0 ||
        ghm_commit_now(root, "initial", &identity, &identity, initial_oid, &error) != 0) goto done;

    if (snprintf(path, sizeof(path), "%s/a.txt", root) >= (int)sizeof(path) ||
        snprintf(renamed_path, sizeof(renamed_path), "%s/renamed.txt", root) >= (int)sizeof(renamed_path) ||
        rename(path, renamed_path) != 0 || ghm_repo_status(root, &status, &error) != 0) goto done;
    const GhmStatusEntry *renamed = NULL;
    for (size_t i = 0; i < status.count; ++i)
        if (status.items[i].kind == GHM_STATUS_RENAMED) renamed = &status.items[i];
    if (expect(renamed != NULL, "rename is recognized") != 0 ||
        ghm_repo_stage_path(root, renamed->path, &error) != 0) goto done;
    ghm_status_free(&status);
    if (ghm_repo_status(root, &status, &error) != 0) goto done;
    for (size_t i = 0; i < status.count; ++i)
        if (expect(!status.items[i].unstaged, "manual staging captures both sides of rename") != 0)
            goto done;
    ghm_status_free(&status);
    if (ghm_repo_unstage_path(root, "a.txt", &error) != 0 ||
        ghm_repo_status(root, &status, &error) != 0) goto done;
    for (size_t i = 0; i < status.count; ++i)
        if (expect(!status.items[i].staged, "unstaging a rename resets both paths") != 0)
            goto done;
    ghm_status_free(&status);
    if (rename(renamed_path, path) != 0) goto done;

    if (write_file(root, "a.txt", "first\n") != 0 ||
        write_file(root, "b.txt", "second\n") != 0 ||
        ghm_commit_staged(root, "should fail", &identity, &identity, commit_oid, &error) == 0 ||
        ghm_repo_stage_path(root, "a.txt", &error) != 0 ||
        ghm_repo_status(root, &status, &error) != 0) goto done;
    const GhmStatusEntry *a = find_status(&status, "a.txt");
    const GhmStatusEntry *b = find_status(&status, "b.txt");
    if (expect(a != NULL && a->staged && !a->unstaged, "a.txt is staged") != 0 ||
        expect(b != NULL && !b->staged && b->unstaged, "b.txt remains untracked") != 0) goto done;
    ghm_status_free(&status);
    if (ghm_commit_staged(root, "first only", &identity, &identity, commit_oid, &error) != 0 ||
        ghm_repo_status(root, &status, &error) != 0) goto done;
    if (expect(find_status(&status, "a.txt") == NULL &&
               find_status(&status, "b.txt") != NULL,
               "staged commit excludes later untracked file") != 0) goto done;
    ghm_status_free(&status);

    if (ghm_repo_stage_path(root, "b.txt", &error) != 0 ||
        ghm_repo_unstage_path(root, "b.txt", &error) != 0 ||
        ghm_repo_status(root, &status, &error) != 0) goto done;
    b = find_status(&status, "b.txt");
    if (expect(b != NULL && !b->staged && b->unstaged, "unstage restores untracked state") != 0) goto done;
    ghm_status_free(&status);
    if (ghm_repo_stage_all(root, &error) != 0 ||
        ghm_snapshot_capture_mode(root, NULL, "refs/ghm/jobs/staged-one", 0,
                                  tree1, &error) != 0) goto done;
    if (write_file(root, "b.txt", "edited after first job\n") != 0 ||
        write_file(root, "c.txt", "third\n") != 0 ||
        ghm_repo_stage_path(root, "c.txt", &error) != 0 ||
        ghm_snapshot_capture_mode(root, tree1, "refs/ghm/jobs/staged-two", 0,
                                  tree2, &error) != 0) goto done;
    if (expect(tree_has(repo, tree1, "b.txt") == 1 && tree_has(repo, tree1, "c.txt") == 0,
               "first staged snapshot excludes later file") != 0 ||
        expect(tree_has(repo, tree2, "b.txt") == 1 && tree_has(repo, tree2, "c.txt") == 1,
               "second staged snapshot builds on first") != 0 ||
        expect(tree_blob_equals(repo, tree2, "b.txt", "second\n"),
               "unstaged later edit does not enter the second job") != 0) goto done;
    if (ghm_repo_unstage_path(root, "b.txt", &error) != 0 ||
        snprintf(path, sizeof(path), "%s/b.txt", root) >= (int)sizeof(path) ||
        unlink(path) != 0 ||
        ghm_snapshot_capture_mode(root, tree1, "refs/ghm/jobs/unsafe", 0,
                                  unsafe_tree, &error) == 0 ||
        expect(unsafe_tree[0] == '\0',
               "staged-only mode rejects a deletion it cannot represent") != 0) goto done;

    if (snprintf(path, sizeof(path), "%s/docs", root) >= (int)sizeof(path) ||
        mkdir(path, 0700) != 0 || write_file(path, "note.txt", "hello\n") != 0 ||
        ghm_repo_files(root, &files, &error) != 0) goto done;
    int saw_directory = 0;
    int saw_file = 0;
    for (size_t i = 0; i < files.count; ++i) {
        if (strcmp(files.items[i].path, "docs") == 0 && files.items[i].is_directory)
            saw_directory = 1;
        if (strcmp(files.items[i].path, "docs/note.txt") == 0 && !files.items[i].is_directory)
            saw_file = 1;
        if (strstr(files.items[i].path, ".git") != NULL) goto done;
    }
    if (expect(saw_directory && saw_file, "file explorer lists nested files") != 0) goto done;
    ghm_file_list_free(&files);

    if (snprintf(path, sizeof(path), "%s/a.txt", root) >= (int)sizeof(path) ||
        unlink(path) != 0 || ghm_repo_stage_path(root, "a.txt", &error) != 0 ||
        ghm_repo_status(root, &status, &error) != 0) goto done;
    a = find_status(&status, "a.txt");
    if (expect(a != NULL && a->staged && a->kind == GHM_STATUS_DELETED,
               "manual staging includes file deletion") != 0) goto done;
    result = 0;
done:
    if (result != 0 && error.message[0] != '\0') fprintf(stderr, "%s\n", error.message);
    ghm_status_free(&status);
    ghm_file_list_free(&files);
    if (tree1[0] != '\0') {
        GhmError ignored = {0};
        (void)ghm_snapshot_release(root, "refs/ghm/jobs/staged-one", tree1, &ignored);
    }
    if (tree2[0] != '\0') {
        GhmError ignored = {0};
        (void)ghm_snapshot_release(root, "refs/ghm/jobs/staged-two", tree2, &ignored);
    }
    git_repository_free(repo);
    git_libgit2_shutdown();
    if (remove_tree(root) != 0) fprintf(stderr, "Could not remove test repository\n");
    return result;
}
