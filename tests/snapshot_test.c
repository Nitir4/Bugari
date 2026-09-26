#include <ghm/commit.h>
#include <ghm/ghm.h>

#include <dirent.h>
#include <git2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_text(const char *directory, const char *name, const char *contents)
{
    char path[512];
    FILE *file;
    int result;
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >= (int)sizeof(path)) return -1;
    file = fopen(path, "w");
    if (file == NULL) return -1;
    result = fputs(contents, file);
    if (fclose(file) != 0) return -1;
    return result == EOF ? -1 : 0;
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

static int expect(int condition, const char *message)
{
    if (condition) return 0;
    fprintf(stderr, "FAIL: %s\n", message);
    return -1;
}

static int expect_blob(git_repository *repository, const git_tree *tree,
                       const char *path, const char *expected)
{
    git_tree_entry *entry = NULL;
    git_blob *blob = NULL;
    int result = -1;
    if (git_tree_entry_bypath(&entry, tree, path) < 0 ||
        git_blob_lookup(&blob, repository, git_tree_entry_id(entry)) < 0) goto done;
    size_t size = strlen(expected);
    result = expect(git_blob_rawsize(blob) == (git_object_size_t)size &&
                    memcmp(git_blob_rawcontent(blob), expected, size) == 0,
                    "snapshot has the expected file content");
done:
    git_blob_free(blob);
    git_tree_entry_free(entry);
    return result;
}

static int make_initial_commit(git_repository *repository, const char *directory,
                               git_oid *out_commit, git_oid *out_tree)
{
    git_index *index = NULL;
    git_tree *tree = NULL;
    git_signature *signature = NULL;
    int result = -1;
    if (write_text(directory, "a.txt", "base\n") != 0 ||
        git_repository_index(&index, repository) < 0 ||
        git_index_add_bypath(index, "a.txt") < 0 || git_index_write(index) < 0 ||
        git_index_write_tree(out_tree, index) < 0 ||
        git_tree_lookup(&tree, repository, out_tree) < 0 ||
        git_signature_new(&signature, "Test Author", "test@example.invalid", 1700000000, 0) < 0 ||
        git_commit_create(out_commit, repository, "HEAD", signature, signature,
                          NULL, "initial", tree, 0, NULL) < 0) goto done;
    result = 0;
done:
    git_signature_free(signature);
    git_tree_free(tree);
    git_index_free(index);
    return result;
}

int main(void)
{
    char directory[] = "/tmp/ghm-snapshot-test-XXXXXX";
    char tree1[GHM_OID_HEX_CAPACITY] = {0};
    char tree2[GHM_OID_HEX_CAPACITY] = {0};
    char base_hex[GHM_OID_HEX_CAPACITY] = {0};
    char commit1[GHM_OID_HEX_CAPACITY] = {0};
    char commit2[GHM_OID_HEX_CAPACITY] = {0};
    char ignored[GHM_OID_HEX_CAPACITY] = {0};
    git_repository *repository = NULL;
    git_index *index = NULL;
    git_commit *commit = NULL;
    git_tree *tree = NULL;
    git_tree *base_tree = NULL;
    git_reference *head = NULL;
    git_oid base_oid;
    git_oid base_tree_oid;
    git_oid index_tree_oid;
    GhmStatus status = {0};
    GhmError error = {0};
    const char *branch = NULL;
    const GhmCommitSignature author = {"Test Author", "test@example.invalid", 1790000000, 330};
    const GhmCommitSignature committer = {"Test Author", "test@example.invalid", 1790000000, 330};
    int result = 1;

    if (mkdtemp(directory) == NULL || git_libgit2_init() < 0) return 1;
    if (git_repository_init(&repository, directory, 0) < 0 ||
        make_initial_commit(repository, directory, &base_oid, &base_tree_oid) != 0) goto done;
    if (git_repository_head(&head, repository) < 0) goto done;
    branch = git_reference_shorthand(head);
    (void)git_oid_tostr(base_hex, sizeof(base_hex), &base_oid);

    if (write_text(directory, "a.txt", "first\n") != 0 ||
        ghm_snapshot_capture(directory, NULL, "refs/ghm/jobs/one", tree1, &error) != 0) goto done;
    if (write_text(directory, "a.txt", "second\n") != 0 ||
        write_text(directory, "b.txt", "new\n") != 0 ||
        ghm_snapshot_capture(directory, tree1, "refs/ghm/jobs/two", tree2, &error) != 0) goto done;

    if (git_repository_index(&index, repository) < 0 ||
        git_index_write_tree(&index_tree_oid, index) < 0 ||
        expect(git_oid_equal(&index_tree_oid, &base_tree_oid),
               "capturing snapshots leaves the normal index unchanged") != 0) goto done;
    if (git_index_add_bypath(index, "a.txt") < 0 ||
        git_index_add_bypath(index, "b.txt") < 0 || git_index_write(index) < 0 ||
        git_index_write_tree(&index_tree_oid, index) < 0) goto done;
    if (ghm_commit_from_snapshot(directory, "refs/ghm/jobs/one", branch, base_hex,
                                 "first commit", &author, &committer, commit1, &error) != 0) goto done;
    git_oid after_first_index_oid;
    if (git_index_read(index, 1) < 0 || git_index_write_tree(&after_first_index_oid, index) < 0 ||
        expect(git_oid_equal(&after_first_index_oid, &index_tree_oid),
               "later staged edits are preserved after the earlier frozen commit") != 0) goto done;
    git_oid oid;
    if (git_oid_fromstr(&oid, commit1) < 0 || git_commit_lookup(&commit, repository, &oid) < 0 ||
        git_commit_tree(&tree, commit) < 0 ||
        expect(strcmp(git_commit_message(commit), "first commit") == 0 &&
               git_commit_author(commit)->when.time == author.timestamp &&
               git_commit_committer(commit)->when.offset == committer.offset_minutes,
               "first commit records message and chosen timestamps") != 0 ||
        expect_blob(repository, tree, "a.txt", "first\n") != 0 ||
        expect(git_tree_entry_byname(tree, "b.txt") == NULL,
               "first snapshot excludes later file") != 0) goto done;
    git_tree_free(tree); tree = NULL;
    git_commit_free(commit); commit = NULL;

    if (ghm_commit_from_snapshot(directory, "refs/ghm/jobs/two", branch, base_hex,
                                 "should fail", &author, &committer, ignored, &error) == 0 ||
        expect(ignored[0] == '\0', "branch change prevents outdated commit") != 0) goto done;
    if (ghm_commit_from_snapshot(directory, "refs/ghm/jobs/two", branch, commit1,
                                 "second commit", &author, &committer, commit2, &error) != 0) goto done;
    if (git_oid_fromstr(&oid, commit2) < 0 || git_commit_lookup(&commit, repository, &oid) < 0 ||
        git_commit_tree(&tree, commit) < 0 ||
        expect(strcmp(git_commit_message(commit), "second commit") == 0 &&
               git_commit_parentcount(commit) == 1 &&
               git_oid_streq(git_commit_parent_id(commit, 0), commit1) == 0,
               "second commit follows the first") != 0 ||
        expect_blob(repository, tree, "a.txt", "second\n") != 0 ||
        expect_blob(repository, tree, "b.txt", "new\n") != 0 ||
        ghm_repo_status(directory, &status, &error) != 0 ||
        expect(status.count == 0, "working tree is clean after both commits") != 0) goto done;
    if (ghm_snapshot_release(directory, "refs/ghm/jobs/one", tree2, &error) == 0 ||
        ghm_snapshot_release(directory, "refs/ghm/jobs/one", tree1, &error) != 0 ||
        ghm_snapshot_release(directory, "refs/ghm/jobs/two", tree2, &error) != 0) goto done;
    result = 0;
done:
    if (result != 0) fprintf(stderr, "Snapshot test error: %s\n", error.message);
    ghm_status_free(&status);
    git_tree_free(tree);
    git_tree_free(base_tree);
    git_reference_free(head);
    git_commit_free(commit);
    git_index_free(index);
    git_repository_free(repository);
    git_libgit2_shutdown();
    if (remove_tree(directory) != 0) fprintf(stderr, "Could not remove test repository\n");
    return result;
}
