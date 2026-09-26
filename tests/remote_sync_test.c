#include <ghm/commit.h>
#include <ghm/remote.h>
#include <ghm/scheduler.h>
#include "core/remote.h"

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

static int has_contents(const char *directory, const char *contents)
{
    char path[512], buffer[128];
    FILE *file;
    if (snprintf(path, sizeof(path), "%s/a.txt", directory) >= (int)sizeof(path)) return 0;
    file = fopen(path, "r");
    if (file == NULL) return 0;
    int found = fgets(buffer, sizeof(buffer), file) != NULL && strcmp(buffer, contents) == 0;
    fclose(file);
    return found;
}

int main(void)
{
    char root[] = "/tmp/ghm-sync-test-XXXXXX";
    char source_path[512], peer_path[512], bare_path[512], data_path[512];
    char first_oid[GHM_OID_HEX_CAPACITY] = {0};
    char peer_oid[GHM_OID_HEX_CAPACITY] = {0};
    char local_oid[GHM_OID_HEX_CAPACITY] = {0};
    char pushed_oid[GHM_OID_HEX_CAPACITY] = {0};
    git_repository *source = NULL, *bare = NULL;
    git_remote *origin = NULL;
    git_reference *head = NULL;
    GhmContext *context = NULL;
    GhmPullOutcome outcome = GHM_PULL_UP_TO_DATE;
    GhmError error = {0};
    const GhmCommitSignature signature = {"Sync Test", "sync@example.invalid", 1790000000, 0};
    int64_t job_id = 0;
    int result = 1;
    if (mkdtemp(root) == NULL ||
        setenv("XDG_STATE_HOME", root, 1) != 0 ||
        snprintf(source_path, sizeof(source_path), "%s/source", root) >= (int)sizeof(source_path) ||
        snprintf(peer_path, sizeof(peer_path), "%s/peer", root) >= (int)sizeof(peer_path) ||
        snprintf(bare_path, sizeof(bare_path), "%s/bare.git", root) >= (int)sizeof(bare_path) ||
        snprintf(data_path, sizeof(data_path), "%s/data", root) >= (int)sizeof(data_path) ||
        ghm_context_open(data_path, &context, &error) != 0) goto done;
    if (git_repository_init(&source, source_path, 0) < 0 ||
        git_repository_init(&bare, bare_path, 1) < 0 ||
        write_text(source_path, "first\n") != 0 ||
        ghm_commit_now(source_path, "first", &signature, &signature, first_oid, &error) != 0 ||
        git_remote_create(&origin, source, "origin", bare_path) < 0 ||
        ghm_repo_push(source_path, NULL, pushed_oid, &error) != 0 ||
        ghm_remote_clone_url(bare_path, peer_path, NULL, &error) != 0 ||
        ghm_repo_register(context, source_path, &error) != 0 ||
        write_text(peer_path, "peer second\n") != 0 ||
        ghm_commit_now(peer_path, "peer second", &signature, &signature, peer_oid, &error) != 0 ||
        ghm_repo_push(peer_path, NULL, pushed_oid, &error) != 0 ||
        ghm_repo_fetch(source_path, NULL, &error) != 0 ||
        git_repository_head(&head, source) < 0 ||
        git_oid_streq(git_reference_target(head), first_oid) != 0 ||
        ghm_repo_pull(context, source_path, NULL, &outcome, &error) != 0 ||
        outcome != GHM_PULL_FAST_FORWARDED || !has_contents(source_path, "peer second\n")) goto done;
    git_reference_free(head); head = NULL;
    if (git_repository_head(&head, source) < 0 ||
        git_oid_streq(git_reference_target(head), peer_oid) != 0 ||
        ghm_repo_pull(context, source_path, NULL, &outcome, &error) != 0 ||
        outcome != GHM_PULL_UP_TO_DATE ||
        write_text(source_path, "dirty\n") != 0 ||
        write_text(peer_path, "peer third\n") != 0 ||
        ghm_commit_now(peer_path, "peer third", &signature, &signature, peer_oid, &error) != 0 ||
        ghm_repo_push(peer_path, NULL, pushed_oid, &error) != 0 ||
        ghm_repo_pull(context, source_path, NULL, &outcome, &error) == 0 ||
        !has_contents(source_path, "dirty\n") ||
        write_text(source_path, "peer second\n") != 0 ||
        ghm_repo_pull(context, source_path, NULL, &outcome, &error) != 0 ||
        outcome != GHM_PULL_FAST_FORWARDED ||
        write_text(source_path, "local ahead\n") != 0 ||
        ghm_commit_now(source_path, "local ahead", &signature, &signature, local_oid, &error) != 0 ||
        ghm_repo_pull(context, source_path, NULL, &outcome, &error) != 0 ||
        outcome != GHM_PULL_LOCAL_AHEAD ||
        write_text(peer_path, "peer diverged\n") != 0 ||
        ghm_commit_now(peer_path, "peer diverged", &signature, &signature, peer_oid, &error) != 0 ||
        ghm_repo_push(peer_path, NULL, pushed_oid, &error) != 0 ||
        ghm_repo_pull(context, source_path, NULL, &outcome, &error) == 0) goto done;
    git_reference_free(head); head = NULL;
    if (git_repository_head(&head, source) < 0 ||
        git_oid_streq(git_reference_target(head), local_oid) != 0 ||
        write_text(source_path, "pending edit\n") != 0) goto done;
    /* A pending frozen snapshot also blocks an otherwise safe branch advance. */
    if (ghm_schedule_add(context, source_path, "future", &signature, &signature,
                         (int64_t)time(NULL) + 3600, &job_id, &error) != 0 ||
        ghm_repo_pull(context, source_path, NULL, &outcome, &error) == 0 ||
        strstr(error.message, "Pending scheduled snapshots") == NULL ||
        ghm_schedule_cancel(context, job_id, &error) != 0) goto done;
    result = 0;
done:
    if (result != 0) fprintf(stderr, "Remote sync test failed: %s\n", error.message);
    git_reference_free(head);
    git_remote_free(origin);
    git_repository_free(bare);
    git_repository_free(source);
    ghm_context_close(context);
    if (remove_tree(root) != 0) fprintf(stderr, "Could not remove sync test directory\n");
    return result;
}
