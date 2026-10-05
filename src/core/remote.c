#include <ghm/remote.h>
#include "core/remote.h"
#include "core/git.h"
#include "core/lock.h"
#include "core/fault.h"

static int push_branch(const char *repository_path, const char *client_id,
                       const char *expected_branch, const char *expected_oid,
                       char out_oid[GHM_OID_HEX_CAPACITY], GhmError *error);
#include "storage/database.h"

#include <errno.h>
#include <git2.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <ctype.h>
#include <time.h>
#include "platform/io.h"
#include <glib.h>

void ghm_remote_error_classify(const char *message, int error_class, GhmError *error)
{
    char lower[512];
    (void)snprintf(lower, sizeof(lower), "%s", message != NULL ? message : "unknown transport error");
    for (size_t i = 0; lower[i] != '\0'; ++i) lower[i] = (char)tolower((unsigned char)lower[i]);
    if (strstr(lower, "429") != NULL || strstr(lower, "rate limit") != NULL ||
        strstr(lower, "rate-limit") != NULL || strstr(lower, "too many requests") != NULL ||
        strstr(lower, "abuse detection") != NULL) {
        ghm_error_set(error, GHM_ERROR_RATE_LIMIT,
            "GitHub rate limit reached. Wait before retrying; scheduled pushes will retry with backoff");
        if (error != NULL) error->retry_at = (int64_t)time(NULL) + 60;
    } else if (strstr(lower, "401") != NULL || strstr(lower, "403") != NULL ||
        strstr(lower, "authentication") != NULL || strstr(lower, "permission denied") != NULL ||
        strstr(lower, "access denied") != NULL || strstr(lower, "not authorized") != NULL) {
        ghm_error_set(error, GHM_ERROR_AUTH, "GitHub denied access. Check your login and repository permissions before retrying");
    } else if (strstr(lower, "resolve path") != NULL || strstr(lower, "no such file or directory") != NULL ||
        strstr(lower, "unsupported url protocol") != NULL) {
        ghm_error_set(error, GHM_ERROR_GIT, message != NULL ? message : "Local remote path is unavailable");
    } else if (error_class == GIT_ERROR_NET || error_class == GIT_ERROR_HTTP ||
        strstr(lower, "could not resolve") != NULL || strstr(lower, "failed to connect") != NULL ||
        strstr(lower, "connection refused") != NULL || strstr(lower, "network is unreachable") != NULL ||
        strstr(lower, "timed out") != NULL) {
        /* A missing private repo (404) and malformed requests are not offline. */
        if (strstr(lower, "404") != NULL || strstr(lower, "400") != NULL)
            ghm_error_set(error, GHM_ERROR_AUTH, "Repository is unavailable. Check its URL and your access permissions");
        else ghm_error_set(error, GHM_ERROR_NETWORK,
            "Network unavailable or server temporarily unreachable. Scheduled pushes will retry automatically");
    } else ghm_error_set(error, GHM_ERROR_GIT, message != NULL ? message : "Unknown transport error");
}

static void remote_error(GhmError *error)
{
    const git_error *detail = git_error_last();
    ghm_remote_error_classify(detail != NULL ? detail->message : NULL,
        detail != NULL ? detail->klass : GIT_ERROR_NONE, error);
}

typedef struct {
    const char *token;
    unsigned attempts;
    char rejection[256];
} GitHubCredentials;

static int github_credentials(git_credential **out, const char *url,
                              const char *username_from_url, unsigned int allowed_types,
                              void *payload)
{
    GitHubCredentials *credentials = payload;
    (void)username_from_url;
    if (credentials->token == NULL || url == NULL ||
        strncmp(url, "https://github.com/", sizeof("https://github.com/") - 1) != 0)
        return GIT_PASSTHROUGH;
    if (++credentials->attempts > 3) return GIT_EAUTH;
    if ((allowed_types & GIT_CREDENTIAL_USERPASS_PLAINTEXT) != 0U)
        return git_credential_userpass_plaintext_new(out, "x-access-token", credentials->token);
    if ((allowed_types & GIT_CREDENTIAL_USERNAME) != 0U)
        return git_credential_username_new(out, "x-access-token");
    return GIT_PASSTHROUGH;
}

int ghm_remote_clone_url(const char *url, const char *destination,
                         const char *github_token, GhmError *error)
{
    git_clone_options options = {0};
    git_repository *repository = NULL;
    GitHubCredentials credentials = {.token = github_token};
    int result = -1;
    if (url == NULL || url[0] == '\0' || destination == NULL || destination[0] == '\0') {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Clone URL and destination are required");
        return -1;
    }
    if (git_clone_options_init(&options, GIT_CLONE_OPTIONS_VERSION) < 0) {
        ghm_error_from_git(error, "Initialize clone options");
        return -1;
    }
    options.fetch_opts.callbacks.credentials = github_credentials;
    options.fetch_opts.callbacks.payload = &credentials;
    if (git_clone(&repository, url, destination, &options) < 0) {
        remote_error(error);
        goto done;
    }
    result = 0;
done:
    git_repository_free(repository);
    return result;
}

static int valid_segment(const char *begin, size_t length)
{
    if (length == 0 || (length == 1 && begin[0] == '.') ||
        (length == 2 && begin[0] == '.' && begin[1] == '.')) return 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)begin[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.')) return 0;
    }
    return 1;
}

static int valid_full_name(const char *name)
{
    const char *slash;
    if (name == NULL) return 0;
    slash = strchr(name, '/');
    return slash != NULL && strchr(slash + 1, '/') == NULL &&
           valid_segment(name, (size_t)(slash - name)) &&
           valid_segment(slash + 1, strlen(slash + 1));
}

static int matching_existing_clone(const char *path, const char *url, GhmError *error)
{
    git_repository *repository = NULL;
    git_remote *origin = NULL;
    int result = -1;
    if (git_repository_open(&repository, path) < 0 ||
        git_repository_workdir(repository) == NULL ||
        strncmp(git_repository_workdir(repository), path, strlen(path)) != 0 ||
        git_repository_workdir(repository)[strlen(path)] != '/' ||
        git_repository_workdir(repository)[strlen(path) + 1] != '\0' ||
        git_remote_lookup(&origin, repository, "origin") < 0 ||
        git_remote_url(origin) == NULL || strcmp(git_remote_url(origin), url) != 0) {
        ghm_error_set(error, GHM_ERROR_GIT,
                      "The managed path already exists but is not this GitHub repository");
        goto done;
    }
    result = 0;
done:
    git_remote_free(origin);
    git_repository_free(repository);
    return result;
}

static int clone_github_unlocked(GhmContext *context, const char *client_id,
                          const GhmGitHubRepo *repo, char **out_path, GhmError *error)
{
    const char *root;
    const char *slash;
    char *existing = NULL;
    char *token = NULL;
    char *repos_dir = NULL;
    char *owner_dir = NULL;
    char *destination = NULL;
    char *url = NULL;
    git_repository *check = NULL;
    struct stat metadata;
    int result = -1;
    if (context == NULL || repo == NULL || repo->github_id <= 0 ||
        !valid_full_name(repo->full_name) || out_path == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Valid GitHub repository metadata is required");
        return -1;
    }
    *out_path = NULL;
    root = ghm_context_data_directory(context);
    slash = strchr(repo->full_name, '/');
    repos_dir = malloc(strlen(root) + sizeof("/repos"));
    owner_dir = malloc(strlen(root) + sizeof("/repos/") + (size_t)(slash - repo->full_name));
    destination = malloc(strlen(root) + sizeof("/repos/") + strlen(repo->full_name));
    url = malloc(sizeof("https://github.com/") + strlen(repo->full_name) + sizeof(".git"));
    if (repos_dir == NULL || owner_dir == NULL || destination == NULL || url == NULL) {
        ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
        goto done;
    }
    (void)snprintf(repos_dir, strlen(root) + sizeof("/repos"), "%s/repos", root);
    (void)snprintf(owner_dir, strlen(root) + sizeof("/repos/") + (size_t)(slash - repo->full_name),
                   "%s/repos/%.*s", root, (int)(slash - repo->full_name), repo->full_name);
    (void)snprintf(destination, strlen(root) + sizeof("/repos/") + strlen(repo->full_name),
                   "%s/repos/%s", root, repo->full_name);
    (void)snprintf(url, sizeof("https://github.com/") + strlen(repo->full_name) + sizeof(".git"),
                   "https://github.com/%s.git", repo->full_name);
    if (ghm_database_find_github(context, repo->github_id, &existing, error) != 0) goto done;
    if (existing != NULL) {
        if (matching_existing_clone(existing, url, error) != 0) goto done;
        *out_path = existing;
        existing = NULL;
        result = 0;
        goto done;
    }
    if ((mkdir(repos_dir, 0700) != 0 && errno != EEXIST) ||
        (mkdir(owner_dir, 0700) != 0 && errno != EEXIST)) {
        ghm_error_set(error, GHM_ERROR_IO, "Cannot create managed repository directory");
        goto done;
    }
    if (stat(destination, &metadata) == 0) {
        if (matching_existing_clone(destination, url, error) != 0) goto done;
    } else if (errno == ENOENT) {
        if (ghm_credentials_access_token(client_id, &token, error) != 0 ||
            ghm_remote_clone_url(url, destination, token, error) != 0) goto done;
    } else {
        ghm_error_set(error, GHM_ERROR_IO, "Cannot inspect managed repository path");
        goto done;
    }
    if (git_repository_open(&check, destination) < 0) {
        ghm_error_from_git(error, "Open cloned repository");
        goto done;
    }
    git_repository_free(check); check = NULL;
    if (ghm_database_attach_github(context, repo->full_name, destination,
                                   repo->github_id, url, error) != 0) goto done;
    *out_path = destination;
    destination = NULL;
    result = 0;
done:
    git_repository_free(check);
    ghm_access_token_free(token);
    free(existing);
    free(repos_dir);
    free(owner_dir);
    free(destination);
    free(url);
    return result;
}

static int push_update_reference(const char *reference, const char *status, void *payload)
{
    GitHubCredentials *credentials = payload;
    (void)reference;
    if (status != NULL && status[0] != '\0' && credentials->rejection[0] == '\0')
        (void)snprintf(credentials->rejection, sizeof(credentials->rejection),
                       "Remote rejected push: %.220s", status);
    return 0;
}

static int push_branch_unlocked(const char *repository_path, const char *client_id,
                       const char *expected_branch, const char *expected_oid,
                       char out_oid[GHM_OID_HEX_CAPACITY], GhmError *error)
{
    git_repository *repository = NULL;
    git_reference *head = NULL;
    git_remote *remote = NULL;
    git_reference *temporary = NULL;
    git_transaction *head_lock = NULL;
    git_push_options options = {0};
    git_strarray refspecs = {0};
    GitHubCredentials credentials = {0};
    char *token = NULL;
    char *refspec = NULL;
    char temporary_name[sizeof("refs/ghm/push/") + 32] = {0};
    git_oid expected = {0};
    const char *remote_url;
    int result = -1;
    if (repository_path == NULL || repository_path[0] == '\0' || out_oid == NULL ||
        (expected_oid != NULL && (expected_branch == NULL ||
         git_oid_fromstr(&expected, expected_oid) < 0))) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Repository and output are required");
        return -1;
    }
    out_oid[0] = '\0';
    if (git_repository_open(&repository, repository_path) < 0) {
        ghm_error_from_git(error, "Open repository");
        goto done;
    }
    if (git_repository_state(repository) != GIT_REPOSITORY_STATE_NONE ||
        git_repository_head_detached(repository) > 0 ||
        git_repository_head(&head, repository) < 0 || git_reference_target(head) == NULL) {
        ghm_error_set(error, GHM_ERROR_GIT, "A checked-out branch with a commit is required to push");
        goto done;
    }
    const char *branch = git_reference_shorthand(head);
    if (expected_oid != NULL &&
        (strcmp(branch, expected_branch) != 0 ||
         git_oid_cmp(git_reference_target(head), &expected) != 0)) {
        ghm_error_set(error, GHM_ERROR_GIT,
                      "Branch moved after the scheduled commit; refusing to push other changes");
        goto done;
    }
    if (ghm_head_transaction(&head_lock, repository, head, error) != 0) goto done;
    if (git_remote_lookup(&remote, repository, "origin") < 0) {
        ghm_error_set(error, GHM_ERROR_GIT, "No origin remote is configured");
        goto done;
    }
    remote_url = git_remote_pushurl(remote);
    if (remote_url == NULL) remote_url = git_remote_url(remote);
    if (remote_url == NULL) {
        ghm_error_set(error, GHM_ERROR_GIT, "Origin has no push URL");
        goto done;
    }
    if (strncmp(remote_url, "https://github.com/", sizeof("https://github.com/") - 1) == 0) {
        if (client_id == NULL || client_id[0] == '\0') {
            ghm_error_set(error, GHM_ERROR_AUTH, "Sign in to GitHub before pushing");
            goto done;
        }
        if (ghm_credentials_access_token(client_id, &token, error) != 0) goto done;
        credentials.token = token;
    } else if (!g_path_is_absolute(remote_url)) {
        ghm_error_set(error, GHM_ERROR_GIT,
                      "Push supports GitHub HTTPS or local remotes; change the origin URL to GitHub HTTPS");
        goto done;
    }
    const char *source_ref = git_reference_name(head);
    {
        unsigned char random_bytes[16];
        static const char hex[] = "0123456789abcdef";
        static const char prefix[] = "refs/ghm/push/";
        sqlite3_randomness((int)sizeof(random_bytes), random_bytes);
        memcpy(temporary_name, prefix, sizeof(prefix) - 1);
        for (size_t i = 0; i < sizeof(random_bytes); ++i) {
            temporary_name[sizeof(prefix) - 1 + i * 2] = hex[random_bytes[i] >> 4];
            temporary_name[sizeof(prefix) + i * 2] = hex[random_bytes[i] & 15];
        }
        if (git_reference_create(&temporary, repository, temporary_name,
                                 git_reference_target(head), 0, NULL) < 0) {
            ghm_error_from_git(error, "Prepare scheduled push");
            goto done;
        }
        source_ref = temporary_name;
    }
    size_t refspec_length = strlen(source_ref) + strlen(branch) + sizeof(":refs/heads/");
    refspec = malloc(refspec_length);
    if (refspec == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); goto done; }
    (void)snprintf(refspec, refspec_length, "%s:refs/heads/%s", source_ref, branch);
    refspecs.strings = &refspec;
    refspecs.count = 1;
    if (git_push_options_init(&options, GIT_PUSH_OPTIONS_VERSION) < 0) {
        ghm_error_from_git(error, "Initialize push options");
        goto done;
    }
    options.callbacks.credentials = github_credentials;
    options.callbacks.push_update_reference = push_update_reference;
    options.callbacks.payload = &credentials;
    if (ghm_fault("remote.push", error) != 0) goto done;
    if (git_remote_push(remote, &refspecs, &options) < 0) {
        remote_error(error);
        goto done;
    }
    if (credentials.rejection[0] != '\0') {
        ghm_remote_error_classify(credentials.rejection, GIT_ERROR_NONE, error);
        goto done;
    }
    (void)git_oid_tostr(out_oid, GHM_OID_HEX_CAPACITY, git_reference_target(head));
    result = 0;
done:
    git_transaction_free(head_lock);
    if (temporary != NULL) {
        GhmError saved = error != NULL ? *error : (GhmError){0};
        if (git_reference_delete(temporary) < 0 && result == 0) {
            ghm_error_from_git(error, "Clean up scheduled push reference");
            result = -1;
        } else if (result != 0 && error != NULL) *error = saved;
    }
    git_reference_free(temporary);
    free(refspec);
    ghm_access_token_free(token);
    git_remote_free(remote);
    git_reference_free(head);
    git_repository_free(repository);
    return result;
}

int ghm_repo_push(const char *repository_path, const char *client_id,
                  char out_oid[GHM_OID_HEX_CAPACITY], GhmError *error)
{
    return push_branch(repository_path, client_id, NULL, NULL, out_oid, error);
}

int ghm_repo_push_exact(const char *repository_path, const char *client_id,
                        const char *branch, const char *commit_oid, GhmError *error)
{
    char pushed_oid[GHM_OID_HEX_CAPACITY] = {0};
    return push_branch(repository_path, client_id, branch, commit_oid, pushed_oid, error);
}

static int ghm_repo_fetch_unlocked(const char *repository_path, const char *client_id, GhmError *error)
{
    git_repository *repository = NULL;
    git_remote *remote = NULL;
    git_fetch_options options = {0};
    GitHubCredentials credentials = {0};
    char *token = NULL;
    const char *url;
    int result = -1;
    if (repository_path == NULL || repository_path[0] == '\0') {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Repository path is required");
        return -1;
    }
    if (git_repository_open(&repository, repository_path) < 0) {
        ghm_error_from_git(error, "Open repository");
        goto done;
    }
    if (git_remote_lookup(&remote, repository, "origin") < 0) {
        ghm_error_set(error, GHM_ERROR_GIT, "No origin remote is configured");
        goto done;
    }
    url = git_remote_url(remote);
    if (url == NULL) {
        ghm_error_set(error, GHM_ERROR_GIT, "Origin has no fetch URL");
        goto done;
    }
    if (strncmp(url, "https://github.com/", sizeof("https://github.com/") - 1) == 0) {
        if (client_id == NULL || client_id[0] == '\0') {
            ghm_error_set(error, GHM_ERROR_AUTH, "Sign in to GitHub before fetching");
            goto done;
        }
        if (ghm_credentials_access_token(client_id, &token, error) != 0) goto done;
        credentials.token = token;
    } else if (!g_path_is_absolute(url)) {
        ghm_error_set(error, GHM_ERROR_GIT,
                      "Fetch supports GitHub HTTPS or local remotes; change origin to GitHub HTTPS");
        goto done;
    }
    if (git_fetch_options_init(&options, GIT_FETCH_OPTIONS_VERSION) < 0) {
        ghm_error_from_git(error, "Initialize fetch options");
        goto done;
    }
    options.callbacks.credentials = github_credentials;
    options.callbacks.payload = &credentials;
    if (git_remote_fetch(remote, NULL, &options, NULL) < 0) {
        remote_error(error);
        goto done;
    }
    result = 0;
done:
    ghm_access_token_free(token);
    git_remote_free(remote);
    git_repository_free(repository);
    return result;
}

static int ghm_repo_pull_unlocked(GhmContext *context, const char *repository_path,
                  const char *client_id, GhmPullOutcome *outcome, GhmError *error)
{
    git_repository *repository = NULL;
    git_reference *head = NULL, *current_head = NULL;
    git_commit *remote_commit = NULL;
    git_status_list *status = NULL;
    git_checkout_options checkout = {0};
    git_oid remote_oid, original_oid;
    git_reference *updated = NULL;
    char *remote_ref = NULL;
    const char *branch;
    int pending = 0;
    int graph_result;
    int result = -1;
    if (context == NULL || repository_path == NULL || repository_path[0] == '\0' ||
        outcome == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Context, repository and pull result are required");
        return -1;
    }
    if (git_repository_open(&repository, repository_path) < 0) {
        ghm_error_from_git(error, "Open repository");
        goto done;
    }
    if (git_repository_is_bare(repository) || git_repository_state(repository) != GIT_REPOSITORY_STATE_NONE ||
        git_repository_head_detached(repository) != 0 ||
        git_repository_head(&head, repository) < 0 || git_reference_target(head) == NULL) {
        ghm_error_set(error, GHM_ERROR_GIT, "Pull requires a checked-out branch with no ongoing Git operation");
        goto done;
    }
    branch = git_reference_shorthand(head);
    original_oid = *git_reference_target(head);
    if (ghm_database_branch_has_active_jobs(context, git_repository_workdir(repository),
                                           branch, &pending, error) != 0) goto done;
    if (pending) {
        ghm_error_set(error, GHM_ERROR_GIT,
                      "Pending scheduled snapshots or a running scheduled commit depend on this branch; finish them before pulling");
        goto done;
    }
    if (ghm_repo_fetch(repository_path, client_id, error) != 0) goto done;
    size_t ref_length = strlen(branch) + sizeof("refs/remotes/origin/");
    remote_ref = malloc(ref_length);
    if (remote_ref == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); goto done; }
    (void)snprintf(remote_ref, ref_length, "refs/remotes/origin/%s", branch);
    if (git_reference_name_to_id(&remote_oid, repository, remote_ref) < 0) {
        ghm_error_set(error, GHM_ERROR_GIT, "Origin does not have this branch");
        goto done;
    }
    if (git_repository_head(&current_head, repository) < 0 ||
        git_reference_target(current_head) == NULL ||
        strcmp(git_reference_name(current_head), git_reference_name(head)) != 0 ||
        git_oid_cmp(git_reference_target(current_head), &original_oid) != 0) {
        ghm_error_set(error, GHM_ERROR_GIT, "Branch changed during fetch; pull was stopped");
        goto done;
    }
    if (git_oid_cmp(&remote_oid, &original_oid) == 0) {
        *outcome = GHM_PULL_UP_TO_DATE;
        result = 0;
        goto done;
    }
    graph_result = git_graph_descendant_of(repository, &original_oid, &remote_oid);
    if (graph_result < 0) { ghm_error_from_git(error, "Inspect Git history"); goto done; }
    if (graph_result > 0) {
        *outcome = GHM_PULL_LOCAL_AHEAD;
        result = 0;
        goto done;
    }
    graph_result = git_graph_descendant_of(repository, &remote_oid, &original_oid);
    if (graph_result < 0) { ghm_error_from_git(error, "Inspect Git history"); goto done; }
    if (graph_result == 0) {
        ghm_error_set(error, GHM_ERROR_GIT,
                      "Local and origin branches diverged; pull cannot fast-forward. Resolve the divergence explicitly");
        goto done;
    }
    git_status_options status_options = {0};
    if (git_status_options_init(&status_options, GIT_STATUS_OPTIONS_VERSION) < 0) {
        ghm_error_from_git(error, "Initialize status options");
        goto done;
    }
    status_options.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    status_options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
    if (git_status_list_new(&status, repository, &status_options) < 0) {
        ghm_error_from_git(error, "Inspect working tree");
        goto done;
    }
    if (git_status_list_entrycount(status) != 0) {
        ghm_error_set(error, GHM_ERROR_GIT, "Save, commit or discard local changes before pulling");
        goto done;
    }
    if (git_commit_lookup(&remote_commit, repository, &remote_oid) < 0) {
        ghm_error_from_git(error, "Read fetched commit");
        goto done;
    }
    if (git_checkout_options_init(&checkout, GIT_CHECKOUT_OPTIONS_VERSION) < 0) {
        ghm_error_from_git(error, "Initialize checkout options");
        goto done;
    }
    if (ghm_checkout_transaction(repository, current_head, NULL, remote_commit, 0,
                                  "Fast-forward origin", error) != 0) goto done;
    *outcome = GHM_PULL_FAST_FORWARDED;
    result = 0;
done:
    free(remote_ref);
    git_status_list_free(status);
    git_reference_free(updated);
    git_commit_free(remote_commit);
    git_reference_free(current_head);
    git_reference_free(head);
    git_repository_free(repository);
    return result;
}

static int push_branch(const char *repository_path, const char *client_id,
                       const char *expected_branch, const char *expected_oid,
                       char out_oid[GHM_OID_HEX_CAPACITY], GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (ghm_repo_lock_acquire(repository_path, &lock, error) != 0) return -1;
    int result = push_branch_unlocked(repository_path, client_id, expected_branch, expected_oid, out_oid, error);
    ghm_repo_lock_release(lock);
    return result;
}

int ghm_repo_fetch(const char *repository_path, const char *client_id, GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (ghm_repo_lock_acquire(repository_path, &lock, error) != 0) return -1;
    int result = ghm_repo_fetch_unlocked(repository_path, client_id, error);
    ghm_repo_lock_release(lock);
    return result;
}

int ghm_repo_pull(GhmContext *context, const char *repository_path,
                  const char *client_id, GhmPullOutcome *outcome, GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (ghm_repo_lock_acquire(repository_path, &lock, error) != 0) return -1;
    int result = ghm_repo_pull_unlocked(context, repository_path, client_id, outcome, error);
    ghm_repo_lock_release(lock);
    return result;
}

int ghm_repo_clone_github(GhmContext *context, const char *client_id,
                          const GhmGitHubRepo *repo, char **out_path, GhmError *error)
{
    GhmRepoLock *lock = NULL;
    if (ghm_directory_lock_acquire(ghm_context_data_directory(context), "clone.lock", &lock, error) != 0)
        return -1;
    int result = clone_github_unlocked(context, client_id, repo, out_path, error);
    ghm_repo_lock_release(lock);
    return result;
}
