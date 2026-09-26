#ifndef GHM_TEST_FIXTURE_H
#define GHM_TEST_FIXTURE_H
#include <ghm/branch.h>
#include <ghm/scheduler.h>
#include "storage/database.h"
#include <dirent.h>
#include <git2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
typedef struct {
    char root[64], path[512], data[512], branch[128], base[GHM_OID_HEX_CAPACITY];
    GhmContext *context;
    git_repository *repo;
} Fixture;
static const GhmCommitSignature test_signature = {"Failure Test", "test@example.invalid", 1790000000, 330};
static inline int fixture_remove(const char *path)
{
    struct stat metadata;
    if (lstat(path, &metadata) != 0) return -1;
    if (!S_ISDIR(metadata.st_mode)) return unlink(path);
    (void)chmod(path, 0700);
    DIR *dir = opendir(path);
    if (dir == NULL) return -1;
    struct dirent *entry;
    int result = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[2048];
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child) || fixture_remove(child) != 0)
            result = -1;
    }
    closedir(dir);
    return rmdir(path) == 0 ? result : -1;
}
static inline int fixture_put(const char *root, const char *name, const char *text)
{
    char path[1024];
    if (snprintf(path, sizeof(path), "%s/%s", root, name) >= (int)sizeof(path)) return -1;
    FILE *file = fopen(path, "w");
    if (file == NULL) return -1;
    int result = fputs(text, file);
    return fclose(file) == 0 && result != EOF ? 0 : -1;
}
static inline int fixture_text_is(const char *root, const char *name, const char *text)
{
    char path[1024], buffer[4096];
    if (snprintf(path, sizeof(path), "%s/%s", root, name) >= (int)sizeof(path)) return 0;
    FILE *file = fopen(path, "r");
    if (file == NULL) return 0;
    size_t count = fread(buffer, 1, sizeof(buffer), file);
    int result = count == strlen(text) && memcmp(buffer, text, count) == 0;
    fclose(file);
    return result;
}
static inline int fixture_head_is(const char *path, const char *oid)
{
    git_repository *repo = NULL;
    git_reference *head = NULL;
    int result = git_repository_open(&repo, path) == 0 && git_repository_head(&head, repo) == 0 &&
        git_reference_target(head) != NULL && git_oid_streq(git_reference_target(head), oid) == 0;
    git_reference_free(head); git_repository_free(repo);
    return result;
}
static inline int fixture_open(Fixture *fixture, GhmError *error)
{
    *fixture = (Fixture){.root = "/tmp/ghm-failure-XXXXXX"};
    if (mkdtemp(fixture->root) == NULL || setenv("XDG_STATE_HOME", fixture->root, 1) != 0) return -1;
    (void)snprintf(fixture->path, sizeof(fixture->path), "%s/repo", fixture->root);
    (void)snprintf(fixture->data, sizeof(fixture->data), "%s/data", fixture->root);
    if (ghm_context_open(fixture->data, &fixture->context, error) != 0 ||
        git_repository_init(&fixture->repo, fixture->path, 0) < 0 ||
        fixture_put(fixture->path, "a.txt", "base\n") != 0 ||
        fixture_put(fixture->path, "z.txt", "base z\n") != 0 ||
        ghm_commit_now(fixture->path, "base", &test_signature, &test_signature, fixture->base, error) != 0 ||
        ghm_repo_register(fixture->context, fixture->path, error) != 0) return -1;
    git_reference *head = NULL;
    if (git_repository_head(&head, fixture->repo) < 0) return -1;
    (void)snprintf(fixture->branch, sizeof(fixture->branch), "%s", git_reference_shorthand(head));
    git_reference_free(head);
    return 0;
}
static inline void fixture_close(Fixture *fixture)
{
    git_repository_free(fixture->repo);
    ghm_context_close(fixture->context);
    (void)fixture_remove(fixture->root);
    *fixture = (Fixture){0};
}
#define REQUIRE(expression) do { if (!(expression)) { fprintf(stderr, "FAIL %d: %s: %s\n", __LINE__, #expression, error.message); goto done; } } while (0)
#endif
