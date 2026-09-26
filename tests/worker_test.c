#include <ghm/ghm.h>
#include <ghm/scheduler.h>

#include <dirent.h>
#include <git2.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
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
    char name[512];
    FILE *file;
    int result;
    if (snprintf(name, sizeof(name), "%s/a.txt", directory) >= (int)sizeof(name)) return -1;
    file = fopen(name, "w");
    if (file == NULL) return -1;
    result = fputs(contents, file);
    return fclose(file) == 0 && result != EOF ? 0 : -1;
}

static int initial_commit(git_repository *repository, const char *path)
{
    git_index *index = NULL;
    git_tree *tree = NULL;
    git_signature *signature = NULL;
    git_oid tree_oid, commit_oid;
    int result = -1;
    if (write_text(path, "base\n") != 0 ||
        git_repository_index(&index, repository) < 0 ||
        git_index_add_bypath(index, "a.txt") < 0 || git_index_write(index) < 0 ||
        git_index_write_tree(&tree_oid, index) < 0 ||
        git_tree_lookup(&tree, repository, &tree_oid) < 0 ||
        git_signature_new(&signature, "Worker Test", "test@example.invalid", 1700000000, 0) < 0 ||
        git_commit_create(&commit_oid, repository, "HEAD", signature, signature,
                          NULL, "initial", tree, 0, NULL) < 0) goto done;
    result = 0;
done:
    git_signature_free(signature);
    git_tree_free(tree);
    git_index_free(index);
    return result;
}

static int completed(sqlite3 *db, int64_t job_id, char oid[GHM_OID_HEX_CAPACITY])
{
    sqlite3_stmt *stmt = NULL;
    int result = 0;
    if (sqlite3_prepare_v2(db, "SELECT status,result_oid FROM scheduled_jobs WHERE id=?1",
                           -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 1, job_id) != SQLITE_OK || sqlite3_step(stmt) != SQLITE_ROW) goto done;
    const char *status = (const char *)sqlite3_column_text(stmt, 0);
    const char *result_oid = (const char *)sqlite3_column_text(stmt, 1);
    if (strcmp(status, "FAILED") == 0) result = -1;
    if (strcmp(status, "COMPLETED") == 0 && result_oid != NULL) {
        (void)snprintf(oid, GHM_OID_HEX_CAPACITY, "%s", result_oid);
        result = 1;
    }
done:
    sqlite3_finalize(stmt);
    return result;
}

int main(int argc, char **argv)
{
    char root[] = "/tmp/ghm-worker-test-XXXXXX";
    char xdg_path[512], data_path[512], repo_path[512], db_file[512];
    char result_oid[GHM_OID_HEX_CAPACITY] = {0};
    git_repository *repository = NULL;
    git_reference *head = NULL;
    sqlite3 *db = NULL;
    GhmContext *context = NULL;
    GhmError error = {0};
    int64_t job_id = 0;
    pid_t child = -1;
    int result = 1;
    int child_status;
    int own_git = 0;
    const GhmCommitSignature signature = {"Worker Test", "test@example.invalid", 1790000000, 330};
    if (argc != 2 || mkdtemp(root) == NULL ||
        setenv("XDG_STATE_HOME", root, 1) != 0 ||
        snprintf(xdg_path, sizeof(xdg_path), "%s/xdg", root) >= (int)sizeof(xdg_path) ||
        snprintf(data_path, sizeof(data_path), "%s/ghm", xdg_path) >= (int)sizeof(data_path) ||
        snprintf(repo_path, sizeof(repo_path), "%s/repo", root) >= (int)sizeof(repo_path) ||
        snprintf(db_file, sizeof(db_file), "%s/ghm.db", data_path) >= (int)sizeof(db_file) ||
        ghm_context_open(data_path, &context, &error) != 0 ||
        git_repository_init(&repository, repo_path, 0) < 0 ||
        initial_commit(repository, repo_path) != 0 ||
        ghm_repo_register(context, repo_path, &error) != 0 ||
        write_text(repo_path, "worker result\n") != 0 ||
        ghm_schedule_add(context, repo_path, "worker commit", &signature, &signature,
                         (int64_t)time(NULL) + 2, &job_id, &error) != 0) goto done;
    git_repository_free(repository); repository = NULL;
    ghm_context_close(context); context = NULL;
    child = fork();
    if (child < 0) goto done;
    if (child == 0) {
        if (setenv("XDG_DATA_HOME", xdg_path, 1) != 0) _exit(127);
        execl(argv[1], argv[1], (char *)NULL);
        _exit(127);
    }
    if (sqlite3_open_v2(db_file, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) goto done;
    for (int attempt = 0; attempt < 100; ++attempt) {
        int state = completed(db, job_id, result_oid);
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000L};
        if (state < 0) goto done;
        if (state == 1) break;
        if (waitpid(child, &child_status, WNOHANG) == child) {
            child = -1;
            goto done;
        }
        (void)nanosleep(&delay, NULL);
    }
    if (result_oid[0] == '\0' || git_libgit2_init() < 0) goto done;
    own_git = 1;
    if (git_repository_open(&repository, repo_path) < 0 ||
        git_repository_head(&head, repository) < 0 ||
        git_oid_streq(git_reference_target(head), result_oid) != 0) goto done;
    result = 0;
done:
    if (result != 0) fprintf(stderr, "Worker test failed: %s\n", error.message);
    if (child > 0) {
        (void)kill(child, SIGTERM);
        (void)waitpid(child, &child_status, 0);
    }
    git_reference_free(head);
    git_repository_free(repository);
    if (own_git) git_libgit2_shutdown();
    if (db != NULL) sqlite3_close(db);
    ghm_context_close(context);
    if (remove_tree(root) != 0) fprintf(stderr, "Could not remove worker test directory\n");
    return result;
}
